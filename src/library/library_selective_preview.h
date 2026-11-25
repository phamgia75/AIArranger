#ifndef AI_ARRANGER_LIBRARY_LIBRARY_SELECTIVE_PREVIEW_H
#define AI_ARRANGER_LIBRARY_LIBRARY_SELECTIVE_PREVIEW_H

// Slice M2d: Zero-Breaking-Change Backend Enrichment for Library Manager Plans & Where-Used.
//
// Wraps existing `library::evaluate()`, `library::planJson()`, and `library::whereUsed()` WITHOUT modifying
// `library_plan.h`, `library_plan_store.h` (`AIARRANGER-LIBRARY-PLAN 1`), `library_where_used.h`, or `apps/macos/`.
//
// Safety & Epistemic Invariants:
//  * Strictly READ-ONLY: zero disk writes, zero mutations to source or target libraries.
//  * `deleteProofAvailable` is ALWAYS `false` for Native-KST.
//  * `whereUsedWithManifest()` ALWAYS preserves the mandatory honest footer:
//      "SongBook Entries and unanalysed Drum Kits are not fully scanned; NOT proven unused — deleting may break references."
//  * Cooperative `keepGoing` cancellation checked every <= 256 steps/items.

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "library/library_model.h"
#include "library/library_plan.h"
#include "library/library_where_used.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "media/sound/native_kst_closure.h"
#include "media/sound/selective_diagnostics.h"
#include "media/sound/slot_conflicts.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ai_arranger::library {

inline constexpr const char* kWhereUsedHonestFooter =
    "SongBook Entries and unanalysed Drum Kits are not fully scanned; NOT proven unused — deleting may break references.";

struct SelectiveDependencyContext {
    const media::graph::NativeKstClosureResult* closureA = nullptr;
    const media::graph::NativeKstClosureResult* closureB = nullptr;
    const media::selective::TargetOccupancyMap* occupancyA = nullptr;
    const media::selective::TargetOccupancyMap* occupancyB = nullptr;
    media::selective::ConflictPolicy policy = media::selective::ConflictPolicy::BlockWithReport;
    bool strictBlockingConflicts = false;
};

namespace preview_detail {

inline constexpr size_t kPreviewCancelStride = 64;  // <= 256 per UX Gate #4

inline bool previewKeepGoing(const std::function<bool()>& keepGoing, size_t& tick) {
    if (!keepGoing) return true;
    const size_t cur = tick++;
    if (cur == 0 || (cur % kPreviewCancelStride) == 0) return keepGoing();
    return true;
}

inline bool isGenericOpaqueSoundWarning(const std::string& w) {
    return w.find("payload not decoded: which Sounds it uses is UNKNOWN") != std::string::npos;
}

inline const media::graph::RootClosureEntry* findRootByRelPath(
    const media::graph::NativeKstClosureResult& closure,
    const std::string& relPath) {
    const std::string norm = media::graph::closure_detail::normalizeRelPath(relPath);
    const std::string lowerNorm = media::selective::conflict_detail::asciiLower(norm);
    for (const auto& r : closure.manifest.roots) {
        if (r.relPath == norm) return &r;
    }
    for (const auto& r : closure.manifest.roots) {
        if (media::selective::conflict_detail::asciiLower(r.relPath) == lowerNorm) return &r;
    }
    return nullptr;
}

inline std::map<media::graph::ResourceId, const media::graph::UserSoundManifestEntry*> indexUserSounds(
    const media::graph::NativeKstClosureResult& closure) {
    std::map<media::graph::ResourceId, const media::graph::UserSoundManifestEntry*> m;
    for (const auto& u : closure.manifest.userSounds) m.emplace(u.id, &u);
    return m;
}

inline bool isKstRootObjectItem(const Item& it) {
    return it.type == "Style" || it.type == "Keyboard Set" || it.type == "Pad";
}

}  // namespace preview_detail

// Enriches `library::evaluate()` with M1/M2 Native-KST dependency closure, TargetOccupancyMap slot conflicts,
// USERDK/OC31 UNSUPPORTED_FORMAT notices, and Factory/dedup counts. When `deps == nullptr`, matches
// `library::evaluate()` identically (zero breaking changes).
inline Evaluation evaluateWithDependencies(
    const Library& A,
    const Library& B,
    const std::vector<Step>& plan,
    const SelectiveDependencyContext* deps,
    const std::function<bool()>& keepGoing = {}) {
    Evaluation ev = evaluate(A, B, plan, keepGoing);
    if (!deps) return ev;

    size_t tick = 0;
    if (!preview_detail::previewKeepGoing(keepGoing, tick)) return ev;

    std::map<media::graph::ResourceId, const media::graph::UserSoundManifestEntry*> soundsA, soundsB;
    if (deps->closureA && deps->closureA->ok) soundsA = preview_detail::indexUserSounds(*deps->closureA);
    if (deps->closureB && deps->closureB->ok) soundsB = preview_detail::indexUserSounds(*deps->closureB);

    // Track Sounds already referenced across earlier steps in the plan for cross-step dedup reporting
    std::set<media::graph::ResourceId> seenPlanSoundsA, seenPlanSoundsB;

    for (size_t i = 0; i < plan.size() && i < ev.steps.size(); ++i) {
        if (!preview_detail::previewKeepGoing(keepGoing, tick)) break;
        const Step& st = plan[i];
        if (st.op != Op::Copy && st.op != Op::Move) continue;

        const bool fromA = (st.side == 'A');
        const Library& srcLib = fromA ? A : B;
        if (srcLib.format != Format::Pa5xKst || st.item >= srcLib.items.size()) continue;
        const Item& it = srcLib.items[st.item];
        if (!preview_detail::isKstRootObjectItem(it)) continue;

        const media::graph::NativeKstClosureResult* srcClosure = fromA ? deps->closureA : deps->closureB;
        if (!srcClosure || !srcClosure->ok) continue;

        const media::selective::TargetOccupancyMap* dstOccupancy = fromA ? deps->occupancyB : deps->occupancyA;
        const auto& srcSoundIndex = fromA ? soundsA : soundsB;
        auto& seenPlanSounds = fromA ? seenPlanSoundsA : seenPlanSoundsB;

        const media::graph::RootClosureEntry* root = preview_detail::findRootByRelPath(*srcClosure, it.file);
        if (!root) continue;

        StepCheck& sc = ev.steps[i];
        // Remove the generic opaque warning ONLY now that we have real M1/M2 closure data for this root
        sc.warnings.erase(
            std::remove_if(sc.warnings.begin(), sc.warnings.end(), preview_detail::isGenericOpaqueSoundWarning),
            sc.warnings.end());

        auto addWarn = [&](std::string msg) { sc.warnings.push_back(std::move(msg)); };
        auto addBlockingOrWarn = [&](std::string msg) {
            if (deps->strictBlockingConflicts) sc.conflicts.push_back(std::move(msg));
            else sc.warnings.push_back(std::move(msg));
        };

        // Count Factory ROM, resolved User Sounds, missing User Sounds, USERDK, dedup hits
        size_t factoryRefs = 0;
        size_t unknownOriginRefs = 0;
        size_t dedupSharedWithEarlierStep = 0;
        std::vector<std::string> unanalysedDrumKits;

        for (const auto& sr : root->soundRefs) {
            if (!preview_detail::previewKeepGoing(keepGoing, tick)) break;
            if (sr.origin == media::graph::ResourceOrigin::Factory) ++factoryRefs;
            else if (sr.origin == media::graph::ResourceOrigin::Unknown) ++unknownOriginRefs;
        }

        for (const auto& sid : root->userSoundIds) {
            if (!seenPlanSounds.insert(sid).second) ++dedupSharedWithEarlierStep;
            if (const auto uit = srcSoundIndex.find(sid); uit != srcSoundIndex.end()) {
                if (uit->second->isDrumKit) unanalysedDrumKits.push_back(sid.str());
            }
        }

        // Real M2 closure summary line
        {
            std::ostringstream oss;
            oss << "M2 dependency closure (" << media::selective::kHardwareUnverifiedBanner << "): "
                << root->userSoundIds.size() << " User Sound(s), "
                << factoryRefs << " Factory ROM ref(s) (external), "
                << root->multisampleIds.size() << " Multisample(s), "
                << root->sampleIds.size() << " Sample(s) (stored PCM proxy " << root->storedPcmBytesProxy
                << " bytes; " << media::selective::kStoredPcmProxyCaveat << ")";
            if (dedupSharedWithEarlierStep > 0) {
                oss << "; " << dedupSharedWithEarlierStep << " User Sound(s) deduplicated with earlier plan step(s)";
            }
            addWarn(oss.str());
        }

        // OC31 KMP degraded source notice (UX Gate #2)
        const bool srcKmpOc31 = !srcClosure->manifest.coverage.downstreamKmpParsed &&
                                media::selective::conflict_detail::isOc31KmpError(srcClosure->manifest.coverage.kmpError);
        if (srcKmpOc31) {
            addBlockingOrWarn("UNSUPPORTED_FORMAT [OC31_KMP_UNSUPPORTED]: " +
                              std::string(media::selective::kOc31KmpDegradedNotice));
        }

        // Unanalysed USERDK warning (UX Gate #1 / R1)
        if (!unanalysedDrumKits.empty()) {
            std::string list;
            for (size_t k = 0; k < unanalysedDrumKits.size(); ++k) {
                if (k) list += ", ";
                list += unanalysedDrumKits[k];
            }
            addWarn("UNSUPPORTED_FORMAT [DRUMKIT_BODY_UNPARSED]: references " +
                    std::to_string(unanalysedDrumKits.size()) + " unanalysed User Drum Kit(s) (" + list +
                    "); sample closure incomplete (deleteProofAvailable: false).");
        }

        // Missing source User Sounds (VERIFIED_MISSING)
        for (const auto& mid : root->missingUserSoundIds) {
            addBlockingOrWarn("VERIFIED_MISSING [MISSING_USER_SOUND_SLOT]: referenced User Sound " + mid.str() +
                              " is missing in source KST.");
        }

        // Malformed MIDI leaves / unresolved bank selects / unknown origin addresses
        if (root->malformedMidiLeafCount > 0) {
            addBlockingOrWarn("AMBIGUOUS_OR_DUPLICATE [MALFORMED_MIDI_LEAF]: " +
                              std::to_string(root->malformedMidiLeafCount) + " malformed MIDI leaf/leaves.");
        }
        if (root->hasUnresolvedBankRef) {
            addWarn("AMBIGUOUS_OR_DUPLICATE [UNRESOLVED_MIDI_BANK_SELECT]: Program Change with missing CC0/CC32.");
        }
        if (unknownOriginRefs > 0) {
            addWarn("AMBIGUOUS_OR_DUPLICATE: " + std::to_string(unknownOriginRefs) +
                    " MIDI Bank/PC reference(s) outside verified Pa5X User and Factory ranges.");
        }

        // TargetOccupancyMap slot conflict checks (when target occupancy map is provided)
        if (dstOccupancy) {
            if (!dstOccupancy->valid || !dstOccupancy->fullyInspectable) {
                const std::string why = dstOccupancy->uninspectableReason.empty()
                                            ? std::string("target KST could not be fully inspected (OC31 KMP or corrupt PCG)")
                                            : dstOccupancy->uninspectableReason;
                addBlockingOrWarn("UNSUPPORTED_FORMAT [TARGET_UNINSPECTABLE]: " + why);
            } else {
                for (const auto& sid : root->userSoundIds) {
                    if (!preview_detail::previewKeepGoing(keepGoing, tick)) break;
                    const auto addr = media::selective::SlotAddress::fromBankAndSlot(sid.scope, sid.number);
                    if (!addr.has_value()) continue;
                    const media::selective::SlotState state = dstOccupancy->slotState(*addr);
                    if (state == media::selective::SlotState::Occupied) {
                        const auto* dstEntry = dstOccupancy->findOccupied(*addr);
                        const std::string dstName = dstEntry ? media::panative::printable(dstEntry->displayName) : "?";
                        if (deps->policy == media::selective::ConflictPolicy::RemapToFreeSlotInNewLib) {
                            addWarn("Sound slot conflict at " + addr->shortLabel() + " (occupied in target by \"" +
                                    dstName + "\"): remap to free slot required (policy: remap).");
                        } else {
                            addBlockingOrWarn("AMBIGUOUS_OR_DUPLICATE [SOUND_SLOT_OCCUPIED_DIFFERENT]: target slot " +
                                              addr->shortLabel() + " is occupied by \"" + dstName +
                                              "\" (never overwritten; policy: " +
                                              std::string(media::selective::conflictPolicyName(deps->policy)) + ").");
                        }
                    } else if (state == media::selective::SlotState::BankFileAbsent) {
                        addWarn("Target bank file SOUND/" + addr->bankName() +
                                ".PCG is absent; slot " + addr->shortLabel() + " requires single-Sound load (MAN5 p.736).");
                    }
                }
            }
        }
    }

    // Recompute exact aggregate conflict & warning totals after enrichment
    ev.conflicts = 0;
    ev.warnings = 0;
    for (const auto& sc : ev.steps) {
        ev.conflicts += sc.conflicts.size();
        ev.warnings += sc.warnings.size();
    }
    return ev;
}

// Emits `library::planJson()` preserving `"format": "aiarranger-library-plan", "version": 1` intact,
// and appends an additive `"selectiveDependencies"` summary block when `deps != nullptr`.
inline std::string planJsonWithDependencies(
    const Library& A,
    const Library& B,
    const std::vector<Step>& plan,
    const Evaluation& ev,
    const SelectiveDependencyContext* deps = nullptr) {
    std::string base = planJson(A, B, plan, ev);
    if (!deps) return base;

    const size_t closePos = base.rfind("\n}\n");
    if (closePos == std::string::npos) return base;
    base.erase(closePos);

    auto q = [](const std::string& s) { return "\"" + media::selective::diag_detail::safeJsonEscape(s) + "\""; };
    auto bstr = [](bool b) -> const char* { return b ? "true" : "false"; };
    auto emitSide = [&](const char* key,
                        const media::graph::NativeKstClosureResult* cl,
                        const media::selective::TargetOccupancyMap* occ) {
        std::string o = "    \"" + std::string(key) + "\": {";
        o += "\"closureAttached\": " + std::string(bstr(cl != nullptr && cl->ok));
        o += ", \"occupancyAttached\": " + std::string(bstr(occ != nullptr && occ->valid));
        o += ", \"coverageComplete\": " + std::string(bstr(cl != nullptr && cl->ok && cl->coverageComplete()));
        o += ", \"deleteProofAvailable\": false";
        o += ", \"kmpParsed\": " + std::string(bstr(cl != nullptr && cl->manifest.coverage.downstreamKmpParsed));
        o += ", \"kmpIsOc31\": " + std::string(bstr(cl != nullptr && !cl->manifest.coverage.downstreamKmpParsed &&
                                                    media::selective::conflict_detail::isOc31KmpError(cl->manifest.coverage.kmpError)));
        o += ", \"targetLibraryKey\": " + q(occ ? occ->targetLibraryKey : "");
        o += "}";
        return o;
    };

    std::string ext = ",\n  \"selectiveDependencies\": {\n";
    ext += "    \"hardwareVerified\": false,\n";
    ext += "    \"deleteProofAvailable\": false,\n";
    ext += "    \"label\": " + q(media::selective::kHardwareUnverifiedBanner) + ",\n";
    ext += "    \"conflictPolicy\": " + q(media::selective::conflictPolicyName(deps->policy)) + ",\n";
    ext += "    \"strictBlockingConflicts\": " + std::string(bstr(deps->strictBlockingConflicts)) + ",\n";
    ext += "    \"storedPcmProxyCaveat\": " + q(media::selective::kStoredPcmProxyCaveat) + ",\n";
    ext += emitSide("A", deps->closureA, deps->occupancyA) + ",\n";
    ext += emitSide("B", deps->closureB, deps->occupancyB) + "\n";
    ext += "  }\n}\n";
    base += ext;
    return base;
}

// ── Manifest-Backed Where-Used (Backend Only) ────────────────────────────────────────────────────────────────────────

struct WhereUsedManifestEdge {
    std::string fromId;
    std::string fromLabel;
    std::string via;              // StyleTrack | KeyboardSetPart | PadTrack | OscillatorOf | OscHigh | OscLow | Zone | DrumSampleOf | ProgramChange
    std::string toId;
    std::string confidence;       // "Confirmed" | "Inferred"
    std::string detail;

    auto operator<=>(const WhereUsedManifestEdge&) const = default;
};

struct WhereUsedManifest {
    std::string verdict;
    std::vector<std::string> lines;
    std::vector<WhereUsedManifestEdge> verifiedEdges;
    bool usedManifest = false;
    bool coverageComplete = false;
    bool deleteProofAvailable = false;   // INVARIANT: ALWAYS false!
    std::string footer = kWhereUsedHonestFooter;
};

inline WhereUsedManifest whereUsedWithManifest(
    const Library& L,
    const Item& it,
    const media::graph::NativeKstClosureResult* closure,
    const Library* other = nullptr) {
    WhereUsedManifest out;
    out.deleteProofAvailable = false;
    out.footer = kWhereUsedHonestFooter;

    if (!closure || !closure->ok || !closure->graph.has_value() || L.format != Format::Pa5xKst) {
        const WhereUsed fallback = whereUsed(L, it, other);
        out.verdict = fallback.verdict;
        out.lines = fallback.lines;
        out.usedManifest = false;
        out.coverageComplete = false;
        return out;
    }

    out.usedManifest = true;
    out.coverageComplete = closure->coverageComplete();
    const media::graph::Graph& g = *closure->graph;

    // Collect graph ResourceIds corresponding to this Library Item
    std::vector<media::graph::ResourceId> targets;
    const std::string normFile = media::graph::closure_detail::normalizeRelPath(it.file);
    const std::string upperFile = media::graph::native_detail::upperAscii(normFile);

    if (it.type == "Sound") {
        // Match SOUND/USERnn.PCG or SOUND/USERDKnn.PCG (single slot if it.slot >= 0, else all occupied slots in bank)
        const std::string bank = media::graph::native_detail::bankFromLabel(normFile);
        for (const auto& u : closure->manifest.userSounds) {
            if (media::graph::native_detail::upperAscii(u.bank) != media::graph::native_detail::upperAscii(bank)) continue;
            if (it.slot >= 0 && static_cast<uint32_t>(it.slot) != u.slot) continue;
            targets.push_back(u.id);
        }
    } else if (it.type == "Sample") {
        // Match all RAM Samples residing in this PCM/*.PCM container
        for (const auto& [nid, node] : g.nodes()) {
            if (nid.kind != media::graph::Kind::Sample || !node.sample.has_value()) continue;
            if (media::graph::native_detail::upperAscii(
                    media::graph::closure_detail::normalizeRelPath(node.sample->file)) == upperFile) {
                targets.push_back(nid);
            }
        }
    } else if (it.type == "Multisample") {
        // Match all Multisamples and DrumSamples from MULTISMP/RAM.KMP
        for (const auto& [nid, node] : g.nodes()) {
            if (nid.kind == media::graph::Kind::Multisample || nid.kind == media::graph::Kind::DrumSample) {
                targets.push_back(nid);
            }
        }
    } else if (preview_detail::isKstRootObjectItem(it)) {
        if (const auto* r = preview_detail::findRootByRelPath(*closure, it.file)) {
            targets.push_back(r->id);
        }
    }

    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

    // Gather incoming verified M1 edges to every matched ResourceId
    std::set<WhereUsedManifestEdge> edgeSet;
    for (const auto& tid : targets) {
        const auto incoming = g.whereUsed(tid);
        for (const auto& e : incoming) {
            const auto* fn = g.find(e.from);
            WhereUsedManifestEdge me;
            me.fromId = e.from.str();
            me.fromLabel = fn ? media::panative::printable(fn->name) : e.from.str();
            me.via = media::graph::viaName(e.via);
            me.toId = e.to.str();
            me.confidence = (e.confidence == media::graph::Confidence::Confirmed) ? "Confirmed" : "Inferred";
            me.detail = media::panative::printable(e.detail);
            edgeSet.insert(std::move(me));
        }
    }
    out.verifiedEdges.assign(edgeSet.begin(), edgeSet.end());

    if (!out.verifiedEdges.empty()) {
        out.verdict = "IN USE (M1 manifest): " + std::to_string(out.verifiedEdges.size()) +
                      " edge(s) across " + std::to_string(targets.size()) + " matched resource node(s)";
        for (const auto& e : out.verifiedEdges) {
            out.lines.push_back(e.fromId + " (" + e.fromLabel + ") -" + e.via + "-> " + e.toId +
                                " [" + e.confidence + (e.detail.empty() ? "" : ", " + e.detail) + "]");
        }
    } else if (!targets.empty()) {
        out.verdict = "no references found in scanned Styles/Keyboard Sets/Pads/Sounds (UNPROVABLE — partial coverage)";
        out.lines.push_back("0 incoming edges in M1 manifest across " + std::to_string(targets.size()) +
                            " matched resource node(s); unparsed SongBook/Drum Kits/external libraries are not scanned.");
    } else {
        out.verdict = "no matching resource node in M1 manifest (UNPROVABLE)";
        out.lines.push_back("Item '" + media::panative::printable(it.file) +
                            "' had no matched resource nodes in the attached M1 manifest.");
    }

    if (other != nullptr) {
        out.lines.push_back("Other open library (" + media::panative::printable(other->name) +
                            ") was not scanned by this single-library M1 manifest.");
    }
    if (!out.coverageComplete) {
        out.lines.push_back(std::string("Coverage notice: ") + media::selective::kPartialCoverageNotice);
    }
    return out;
}

inline std::string whereUsedWithManifestText(
    const Library& L,
    const Item& it,
    const media::graph::NativeKstClosureResult* closure,
    const Library* other = nullptr) {
    const WhereUsedManifest w = whereUsedWithManifest(L, it, closure, other);
    std::string o = "Where used: " + it.type + " \"" + media::panative::printable(trimmed(it.name)) + "\" (" +
                    media::panative::printable(it.bank) +
                    (it.slot >= 0 ? " slot " + std::to_string(it.slot + 1) : "") + ")\n";
    o += "  Verdict: " + w.verdict + "\n";
    o += "  Delete proof available: false\n";
    for (const auto& l : w.lines) o += "  " + l + "\n";
    o += "  " + w.footer + "\n";
    return o;
}

}  // namespace ai_arranger::library

#endif  // AI_ARRANGER_LIBRARY_LIBRARY_SELECTIVE_PREVIEW_H
