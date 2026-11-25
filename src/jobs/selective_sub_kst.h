#ifndef AI_ARRANGER_JOBS_SELECTIVE_SUB_KST_H
#define AI_ARRANGER_JOBS_SELECTIVE_SUB_KST_H

// Slice M2e-2: Experimental Root-Tree Sub-KST Selective Exporter (`ExportMode::RootTreeSubKst`).
//
// Portable C++20, header-only, zero JUCE/GUI dependency.
//
// Reuses M1 (`native_kst_closure.h`), M2a (`selective_diagnostics.h`, `slot_conflicts.h`),
// M2b (`m1_manifest_adapter.h`, `selective_collector.h`), M2d (`selective_plan.h`),
// M2e-1 (`smf_remap.h`), W4/W5 (`kst_closure.h`, `kst_assemble.h`), and E6/E8/E9
// (`export_job.h`, `library_validate.h`, `library_apply.h`) as single sources of truth.
//
// Absolute HOLD Gates & Safety Invariants:
//  * ZERO Native Sound KST export, ZERO OC31 encoder, ZERO `.PCG`/`.KMP`/`.PCM`/`.SBD`/`.SBL`/`.GBL` writes.
//  * Output `.KST` folder contains ONLY selected Root-tree files under `STYLE/<bank>/*.stg`,
//    `KEYBOARDSET/<bank>/*.ksc`, `PAD/<bank>/*.pdc`, or optional `SETTINGS/*.ksc` — NEVER
//    `PCM/`, `SOUND/`, `MULTISMP/`, or `SONGBOOK/` (MAN5 p.736 RAM-wipe prevention + HOLD gate).
//  * Strict sound eligibility gate (fail-closed): every MIDI Sound selection across every selected
//    object (including embedded KBD Sets and embedded Pads) MUST be strictly:
//      1. Factory ROM, OR
//      2. Exact same-slot Target-matched User Sound (`EquivalenceVerdict::Identical` in targetKst), OR
//      3. Approved remapped Target-matched User Sound at another slot in targetKst
//         (`EquivalenceVerdict::Identical` in targetKst, rewritten in-memory via `smf_remap::remapRootTree()`).
//  * Any non-present User Sound, unanalysed USERDK, `NoKnownReference` / `InsufficientEvidence`,
//    dangling Sound, malformed MIDI leaf (`>= 0x80` or non-lowercase `.mid`), unresolved bank select,
//    OC31-KMP `TargetUninspectable` target, or occupied/R-18 path collision -> REFUSED fail-closed.
//  * Descriptor-pinned staging via `jobs::runExport()` (`ExportSpec::writeAt`, `O_NOFOLLOW`, `O_EXCL`),
//    D18 overlap guard on BOTH `sourceKst` and optional `targetKst`, TOCTOU `planFingerprint` +
//    per-file SHA-256 + `targetLibraryKey` + `library::recheckPinned()` pre-publish verification.
//  * Always stamped `STRUCTURALLY VALID — NOT VERIFIED ON A PA5X`, `hardwareVerified: false`,
//    `deleteProofAvailable: false`, `L2 = NotRun`, `L3 = NotRun`.

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "jobs/export_job.h"
#include "library/library_apply.h"
#include "library/library_validate.h"
#include "media/panative/kst_assemble.h"
#include "media/panative/kst_closure.h"
#include "media/panative/root_tree.h"
#include "media/panative/smf_remap.h"
#include "media/sound/selective_plan.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ai_arranger::jobs::subkst {

inline constexpr const char* kSubKstExportSchema = "aiarranger-selective-sub-kst-export/1";

struct SubKstExportRequest {
    std::filesystem::path sourceKst;
    std::optional<std::filesystem::path> targetKst;
    std::vector<media::selective::SelectionTarget> targets;
    media::selective::ExportMode mode = media::selective::ExportMode::RootTreeSubKst;
    media::selective::ConflictPolicy policy = media::selective::ConflictPolicy::BlockWithReport;
    // Optional explicit cross-slot Sound remaps (source Sound ResourceId -> target Sound ResourceId).
    // If empty under ReuseIdenticalTargetSlot / RemapToFreeSlotInNewLib (and !requireExplicitRemapApproval),
    // proven-Identical cross-slot target matches are automatically approved.
    // If non-empty (or requireExplicitRemapApproval == true), ONLY listed mappings are approved.
    std::map<media::graph::ResourceId, media::graph::ResourceId> approvedSoundRemaps;
    bool requireExplicitRemapApproval = false;
    bool moveLeftoverAside = false;
    std::string expectFingerprint;       // Optional TOCTOU review-to-execute fingerprint pin (64-hex)
    std::string expectTargetLibraryKey;  // Optional TOCTOU target library key pin
    media::selective::KeepGoing keepGoing = {};
    size_t cancelCheckStride = media::selective::kDefaultCancelCheckStride;
};

struct PlannedRootObject {
    media::selective::SelectedKind kind = media::selective::SelectedKind::Style;
    std::string sourceRelPath;          // Canonical source-relative path (e.g. "STYLE/Pop/Demo.stg")
    std::string destRelPath;            // Output KST-relative path (after targetBank/targetName override)
    std::string sourceSha256;           // Expected SHA-256 of source file at review time
    bool renamedOrRebanked = false;
    bool alreadyPresentInTarget = false;
    bool requiresSmfRemap = false;

    auto operator<=>(const PlannedRootObject&) const = default;
};

struct SubKstSoundRemapEntry {
    media::graph::ResourceId sourceSoundId;
    media::graph::ResourceId targetSoundId;
    media::selective::SlotAddress sourceAddress;
    media::selective::SlotAddress targetAddress;
    media::panative::smf_remap::MidiTriple srcTriple;
    media::panative::smf_remap::MidiTriple dstTriple;

    auto operator<=>(const SubKstSoundRemapEntry&) const = default;
};

struct SubKstExportPlan {
    bool ok = false;                     // true = analysis succeeded (still check executable() before executing!)
    bool cancelled = false;
    bool missingSelectionTarget = false; // true when >= 1 requested --select path is absent in source KST
    std::string error;                   // path-free error message when !ok
    std::string planFingerprint;         // 64-hex SHA-256 binding selection + source SHAs + targetLibraryKey + remaps
    std::string targetLibraryKey;        // "" if no targetKst

    media::selective::NativeSelectivePlanResult planResult;
    std::vector<PlannedRootObject> plannedObjects;
    std::vector<SubKstSoundRemapEntry> soundRemaps;
    std::map<media::panative::smf_remap::MidiTriple,
             media::panative::smf_remap::MidiTriple> midiRemapRules;
    // Exact keys matching panative::closure::Result::dangling ("USER01.PCG slot 5", etc.)
    // for every User Sound proven Identical in pinned targetKst (same-slot or remapped target slot).
    std::set<std::string> allowedTargetMatchedUserSlots;
    std::map<std::string, std::string> expectedSourceFileSha256;
    std::map<std::string, std::string> expectedTargetSoundBankSha256; // Referenced target SOUND/*.PCG SHA-256 (TOCTOU)

    bool anySoundRemapped = false;
    bool anyObjectRenamedOrRebanked = false;

    bool executable() const {
        return ok && !cancelled && !missingSelectionTarget &&
               planResult.executable() &&
               planResult.report.mode == media::selective::ExportMode::RootTreeSubKst &&
               !plannedObjects.empty();
    }
};

struct ExportedFileSummary {
    std::string sourceRelPath;
    std::string destRelPath;
    std::string sha256;
    uint64_t bytes = 0;
    bool remapped = false;
    size_t remappedEvents = 0;
};

struct SubKstExecutionSummary {
    bool published = false;
    std::string outputLeafName;  // Scrubbed leaf only (e.g. "Subset.KST") — zero local paths
    std::vector<ExportedFileSummary> exportedFiles;
    size_t factoryRefs = 0;
    size_t targetMatchedUserRefs = 0;
    size_t validatorWarnings = 0;
    std::vector<std::string> warnings;
};

namespace detail {

inline bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (unsigned char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

inline bool isForbiddenTopDirUpper(const std::string& topUpper) {
    return topUpper == "PCM" || topUpper == "SOUND" || topUpper == "MULTISMP" || topUpper == "SONGBOOK";
}

inline bool isAllowedRootTopDirUpper(const std::string& topUpper) {
    return topUpper == "STYLE" || topUpper == "KEYBOARDSET" || topUpper == "PAD" || topUpper == "SETTINGS";
}

inline bool isForbiddenExtensionUpper(const std::string& pathUpper) {
    using media::graph::native_detail::endsWithUpper;
    return endsWithUpper(pathUpper, ".PCG") || endsWithUpper(pathUpper, ".KMP") ||
           endsWithUpper(pathUpper, ".PCM") || endsWithUpper(pathUpper, ".SBD") ||
           endsWithUpper(pathUpper, ".SBL") || endsWithUpper(pathUpper, ".GBL");
}

inline std::vector<std::string> splitComponents(const std::string& rel) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t slash = rel.find('/', start);
        if (slash == std::string::npos) {
            parts.push_back(rel.substr(start));
            break;
        }
        parts.push_back(rel.substr(start, slash - start));
        start = slash + 1;
    }
    return parts;
}

// Strict validation for RootTreeSubKst output relative paths:
//  - SelectedKind::Style       -> STYLE/<bank>/<name>.stg
//  - SelectedKind::KeyboardSet -> KEYBOARDSET/<bank>/<name>.ksc or SETTINGS/<name>.ksc
//  - SelectedKind::Pad         -> PAD/<bank>/<name>.pdc
// Refuses PCM/, SOUND/, MULTISMP/, SONGBOOK/, cross-type redirects, unsafe FAT/KST names, wrong extensions, or depth mismatches.
inline std::string validateSubKstDestRelPath(
    const std::string& rel,
    std::optional<media::selective::SelectedKind> expectedKind = std::nullopt) {
    if (!library::detail::plainRel(rel)) {
        return "destination path is not a plain relative path: " + rel;
    }
    const std::vector<std::string> comps = splitComponents(rel);
    if (comps.size() < 2) {
        return "destination path must reside inside STYLE/, KEYBOARDSET/, PAD/, or SETTINGS/: " + rel;
    }
    const std::string topUpper = media::graph::native_detail::upperAscii(comps[0]);
    if (isForbiddenTopDirUpper(topUpper)) {
        return "HOLD VIOLATION: directory " + topUpper + "/ is strictly forbidden in RootTreeSubKst output";
    }
    if (!isAllowedRootTopDirUpper(topUpper)) {
        return "unsupported top-level directory for RootTreeSubKst: " + comps[0];
    }
    if (expectedKind.has_value()) {
        using media::selective::SelectedKind;
        const bool kindMatchesTop =
            (*expectedKind == SelectedKind::Style && topUpper == "STYLE") ||
            (*expectedKind == SelectedKind::KeyboardSet && (topUpper == "KEYBOARDSET" || topUpper == "SETTINGS")) ||
            (*expectedKind == SelectedKind::Pad && topUpper == "PAD");
        if (!kindMatchesTop) {
            return "cross-type target override refused: " +
                   std::string(media::selective::selectedKindName(*expectedKind)) +
                   " cannot be exported into " + topUpper + "/ (" + rel + ")";
        }
    }
    for (size_t i = 1; i < comps.size(); ++i) {
        if (!media::panative::assemble::safeName(comps[i])) {
            return "unsafe path component '" + comps[i] + "' in destination path " + rel;
        }
    }
    const std::string& leaf = comps.back();
    const std::string leafUpper = media::graph::native_detail::upperAscii(leaf);
    if (isForbiddenExtensionUpper(leafUpper)) {
        return "HOLD VIOLATION: forbidden native container extension in " + rel;
    }
    auto hasExactLowerExt = [&](const char* ext4) {
        return leaf.size() > 4 && leaf.compare(leaf.size() - 4, 4, ext4) == 0;
    };
    if (topUpper == "STYLE") {
        if (comps.size() != 3 || !hasExactLowerExt(".stg")) {
            return "STYLE objects must use STYLE/<bank>/<name>.stg (lower-case .stg): " + rel;
        }
    } else if (topUpper == "KEYBOARDSET") {
        if (comps.size() != 3 || !hasExactLowerExt(".ksc")) {
            return "KEYBOARDSET objects must use KEYBOARDSET/<bank>/<name>.ksc (lower-case .ksc): " + rel;
        }
    } else if (topUpper == "PAD") {
        if (comps.size() != 3 || !hasExactLowerExt(".pdc")) {
            return "PAD objects must use PAD/<bank>/<name>.pdc (lower-case .pdc): " + rel;
        }
    } else if (topUpper == "SETTINGS") {
        if (comps.size() != 2 || !hasExactLowerExt(".ksc")) {
            return "SETTINGS objects must use SETTINGS/<name>.ksc (lower-case .ksc): " + rel;
        }
    }
    return "";
}

// Formats a SlotAddress into the exact key string used by `panative::closure::Result::dangling`
// (see `src/media/panative/kst_closure.h:179-184`: "USER%02d.PCG slot %u").
inline std::string closureDanglingKeyForSlot(const media::selective::SlotAddress& addr) {
    char buf[48];
    std::snprintf(buf, sizeof(buf),
                  addr.isDrumKit ? "USERDK%02u.PCG slot %u" : "USER%02u.PCG slot %u",
                  static_cast<unsigned>(addr.bank),
                  static_cast<unsigned>(addr.slot));
    return std::string(buf);
}

inline std::string sha256Bytes(const uint8_t* data, size_t size) {
    media::Sha256 h;
    if (size > 0) h.update(data, size);
    return h.hex();
}

inline media::panative::smf_remap::MidiTriple slotToMidiTriple(const media::selective::SlotAddress& addr) {
    return media::panative::smf_remap::MidiTriple{addr.msb(), addr.lsb(), addr.pc()};
}

// Reads a regular file strictly through a pinned directory fd (O_NOFOLLOW per component), bounded by maxBytes.
inline bool readRelBytesPinned(int rootFd,
                               const std::string& rel,
                               size_t maxBytes,
                               std::vector<uint8_t>& outBytes,
                               std::string& outSha256,
                               std::string& errOut) {
    outBytes.clear();
    outSha256.clear();
    errOut.clear();
    if (!library::detail::plainRel(rel)) {
        errOut = "invalid relative path: " + rel;
        return false;
    }
    const auto [dir, leaf] = media::detail::splitRel(rel);
    const media::detail::Fd dfd(library::detail::openDirIn(rootFd, dir, false));
    if (dfd.v < 0) {
        errOut = "cannot open parent folder for " + rel + " (missing or symlink)";
        return false;
    }
    const media::detail::Fd ffd(::openat(dfd.v, leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (ffd.v < 0) {
        errOut = "cannot open " + rel + " read-only (missing or symlink)";
        return false;
    }
    struct stat st{};
    if (::fstat(ffd.v, &st) != 0 || !S_ISREG(st.st_mode)) {
        errOut = rel + " is not a regular file";
        return false;
    }
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) > maxBytes) {
        errOut = rel + " exceeds size limit";
        return false;
    }
    const size_t expectedSize = static_cast<size_t>(st.st_size);
    outBytes.reserve(expectedSize);
    media::Sha256 hasher;
    uint8_t buf[65536];
    for (;;) {
        const ssize_t n = ::read(ffd.v, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            errOut = "read error on " + rel;
            outBytes.clear();
            return false;
        }
        if (n == 0) break;
        if (outBytes.size() + static_cast<size_t>(n) > maxBytes) {
            errOut = rel + " grew beyond size limit while reading";
            outBytes.clear();
            return false;
        }
        hasher.update(buf, static_cast<size_t>(n));
        outBytes.insert(outBytes.end(), buf, buf + n);
    }
    outSha256 = hasher.hex();
    return true;
}

inline std::string hashRelFilePinned(int rootFd, const std::string& rel, std::string& errOut) {
    errOut.clear();
    if (!library::detail::plainRel(rel)) {
        errOut = "invalid relative path: " + rel;
        return {};
    }
    const auto [dir, leaf] = media::detail::splitRel(rel);
    const media::detail::Fd dfd(library::detail::openDirIn(rootFd, dir, false));
    if (dfd.v < 0) {
        errOut = "cannot open parent folder for " + rel;
        return {};
    }
    std::FILE* in = media::detail::openRegular(dfd.v, leaf.c_str());
    if (!in) {
        errOut = "cannot open regular file " + rel + " (missing or symlink)";
        return {};
    }
    bool cancelled = false;
    const std::string h = media::detail::sha256Stream(in, nullptr, {}, &cancelled, &errOut, rel);
    std::fclose(in);
    return h;
}

// Collects non-MIDI leaf SHA-256s (XMLs, etc.) keyed by leaf path inside a Root tree, plus MIDI leaf selects.
inline bool collectRootTreeDigest(const media::panative::roottree::Node& node,
                                  const std::string& prefix,
                                  std::map<std::string, std::string>& nonMidiSha,
                                  std::vector<std::pair<std::string, std::vector<media::panative::closure::Select>>>& midiSelects,
                                  std::string& errOut) {
    for (const auto& child : node.children) {
        const std::string path = prefix.empty() ? child.name : (prefix + "/" + child.name);
        if (child.kind == media::panative::roottree::Node::Leaf) {
            if (media::panative::smf_remap::detail::isMidiLeafName(child.name)) {
                std::vector<media::panative::closure::Select> sel;
                std::string selErr;
                if (!media::panative::closure::leafSelects(child, sel, selErr)) {
                    errOut = path + ": " + selErr;
                    return false;
                }
                midiSelects.emplace_back(path, std::move(sel));
            } else {
                nonMidiSha[path] = sha256Bytes(
                    reinterpret_cast<const uint8_t*>(child.payload.data()),
                    child.payload.size());
            }
        } else {
            if (!collectRootTreeDigest(child, path, nonMidiSha, midiSelects, errOut)) return false;
        }
    }
    return true;
}

inline bool midiLeafVectorsEqual(
    const std::vector<std::pair<std::string, std::vector<media::panative::closure::Select>>>& a,
    const std::vector<std::pair<std::string, std::vector<media::panative::closure::Select>>>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first) return false;
        if (!media::panative::smf_remap::detail::selectsEqual(a[i].second, b[i].second)) return false;
    }
    return true;
}

inline std::string computeSubKstFingerprint(const SubKstExportPlan& plan) {
    media::Sha256 h;
    media::selective::adapter_detail::hashField(h, "aiarranger-selective-sub-kst-plan/1");
    media::selective::adapter_detail::hashField(h, plan.planResult.planFingerprint);
    media::selective::adapter_detail::hashField(h, plan.targetLibraryKey);
    for (const auto& obj : plan.plannedObjects) {
        media::selective::adapter_detail::hashField(h, "obj");
        media::selective::adapter_detail::hashField(h, media::selective::selectedKindName(obj.kind));
        media::selective::adapter_detail::hashField(h, obj.sourceRelPath);
        media::selective::adapter_detail::hashField(h, obj.destRelPath);
        media::selective::adapter_detail::hashField(h, obj.sourceSha256);
    }
    for (const auto& rm : plan.soundRemaps) {
        media::selective::adapter_detail::hashField(h, "remap");
        media::selective::adapter_detail::hashField(h, rm.sourceSoundId.str());
        media::selective::adapter_detail::hashField(h, rm.targetSoundId.str());
    }
    for (const auto& key : plan.allowedTargetMatchedUserSlots) {
        media::selective::adapter_detail::hashField(h, "allowed-slot");
        media::selective::adapter_detail::hashField(h, key);
    }
    for (const auto& [rel, sha] : plan.expectedTargetSoundBankSha256) {
        media::selective::adapter_detail::hashField(h, "target-pcg");
        media::selective::adapter_detail::hashField(h, rel);
        media::selective::adapter_detail::hashField(h, sha);
    }
    return h.hex();
}

inline std::string firstBlockingSummary(const media::selective::SelectiveExportReport& rep) {
    for (const auto& d : rep.diagnostics) {
        if (d.severity == media::selective::DiagnosticSeverity::Blocking) {
            return "[" + std::string(media::selective::diagnosticCategoryName(d.category)) + "] " +
                   d.code + ": " + d.summary;
        }
    }
    for (const auto& c : rep.conflicts) {
        if (c.blocking) {
            return std::string(media::selective::slotConflictKindName(c.kind)) + " (" + c.address + "): " + c.detail;
        }
    }
    return "export is blocked by one or more diagnostics or conflicts";
}

}  // namespace detail

// Parses a compact slot spec ("USER01/5", "USERDK02/12", or canonical "pa5x:USER01/Sound/5") into a ResourceId.
inline bool parseSlotOrResourceId(const std::string& raw, media::graph::ResourceId& outId) {
    if (media::graph::ResourceId::parse(raw, outId)) {
        return !outId.provisional && (outId.kind == media::graph::Kind::Sound || outId.kind == media::graph::Kind::DrumKit);
    }
    const size_t slash = raw.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= raw.size()) return false;
    const std::string bank = media::graph::native_detail::upperAscii(raw.substr(0, slash));
    const std::string slotStr = raw.substr(slash + 1);
    for (unsigned char c : slotStr) if (!std::isdigit(c)) return false;
    unsigned long slotVal = 0;
    try {
        slotVal = std::stoul(slotStr);
    } catch (...) {
        return false;
    }
    if (slotVal > 127ul) return false;
    const auto addr = media::selective::SlotAddress::fromBankAndSlot(bank, static_cast<uint32_t>(slotVal));
    if (!addr.has_value() || !addr->isInSpec()) return false;
    outId = addr->resourceId();
    return true;
}

// Plans and validates a RootTreeSubKst selective export strictly READ-ONLY.
// Zero disk mutations; reuses M1 + M2a + M2b + M2d single sources of truth.
inline SubKstExportPlan planSubKstExport(const SubKstExportRequest& req) {
    using namespace media::selective;
    const size_t stride = adapter_detail::clampStride(req.cancelCheckStride);
    size_t tick = 0;

    auto makeCancelled = [&]() {
        SubKstExportPlan c;
        c.ok = false;
        c.cancelled = true;
        c.error = "selective sub-KST export planning cancelled";
        c.planResult.ok = false;
        c.planResult.cancelled = true;
        c.planResult.error = c.error;
        c.planResult.report.mode = req.mode;
        c.planResult.report.conflictPolicy = req.policy;
        c.planResult.report.coverage.deleteProofAvailable = false;
        c.planResult.report.memoryProjection.sampleBytesAreNativeStoredProxy = true;
        return c;
    };

    if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();

    SubKstExportPlan out;
    out.planResult.report.mode = req.mode;
    out.planResult.report.conflictPolicy = req.policy;
    out.planResult.report.coverage.deleteProofAvailable = false;
    out.planResult.report.memoryProjection.sampleBytesAreNativeStoredProxy = true;

    // 1. Absolute HOLD gate: refuse NativeSoundKst_HOLD or non-RootTreeSubKst modes fail-closed!
    if (req.mode == ExportMode::NativeSoundKst_HOLD) {
        out.ok = true;
        out.planResult.ok = true;
        out.planResult.report.mode = ExportMode::NativeSoundKst_HOLD;
        out.planResult.report.diagnostics.push_back({
            DiagnosticCategory::UNSUPPORTED_FORMAT,
            diag_code::kNativeSoundWriteHold,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Unsupported,
            "<export-mode>",
            "",
            "Native Sound KST export (.PCG/.KMP/.PCM writing) is strictly on HOLD; refusing fail-closed.",
            "Use AdvisoryPlan + Pa5X single-Sound load (MAN5 p.736) or RootTreeSubKst with Factory/Target-matched Sounds only.",
            {}
        });
        out.planResult.report = canonicalizeReport(std::move(out.planResult.report));
        out.planFingerprint = detail::computeSubKstFingerprint(out);
        out.planResult.planFingerprint = out.planFingerprint;
        out.planResult.report.planFingerprint = out.planFingerprint;
        return out;
    }
    if (req.mode != ExportMode::RootTreeSubKst) {
        out.ok = false;
        out.error = "selective sub-KST exporter requires ExportMode::RootTreeSubKst";
        return out;
    }

    // 2. Read-only O_NOFOLLOW directory checks on sourceKst and optional targetKst
    std::string dirErr;
    if (!plan_detail::isRealDirectoryNoFollow(req.sourceKst, dirErr)) {
        out.ok = false;
        out.error = "cannot open source KST <source>: " + dirErr;
        return out;
    }
    if (req.targetKst.has_value() && !plan_detail::isRealDirectoryNoFollow(*req.targetKst, dirErr)) {
        out.ok = false;
        out.error = "cannot open target KST <target>: " + dirErr;
        return out;
    }

    // 3. Load source M1 view & optional target occupancy map ONCE (no double disk scan!)
    media::graph::NativeKstClosureOptions closureOpts;
    closureOpts.keepGoing = req.keepGoing;
    const media::graph::NativeKstClosureResult m1 =
        media::graph::resolveNativeKstClosureFolder(req.sourceKst, closureOpts);
    if (m1.cancelled) return makeCancelled();
    if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();

    NativeEquivalenceExtra extra;
    if (m1.ok) {
        if (!extractNativeEquivalenceExtraFolder(req.sourceKst, extra, req.keepGoing, stride,
                                                 closureOpts.downstream.maxTotalBytes,
                                                 closureOpts.maxDirEntries,
                                                 closureOpts.downstream.soundMode)) {
            return makeCancelled();
        }
        std::vector<std::string> selectedRels;
        selectedRels.reserve(req.targets.size());
        for (const auto& t : req.targets) selectedRels.push_back(conflict_detail::normalizeRelPath(t.relPath));
        if (!hashNativeRelFilesFolder(req.sourceKst, selectedRels, extra.fileSha256, req.keepGoing, stride)) {
            return makeCancelled();
        }
    }

    M1AdapterView srcView = adaptFromM1Closure(m1, extra, req.keepGoing, stride);
    if (srcView.cancelled) return makeCancelled();

    std::optional<TargetOccupancyMap> targetMap;
    uint64_t targetBaselineBytes = 0;
    if (req.targetKst.has_value()) {
        TargetInspectOptions tOpts;
        tOpts.keepGoing = req.keepGoing;
        TargetOccupancyMap tm = inspectTargetKstFolder(*req.targetKst, tOpts);
        if (tm.cancelled) return makeCancelled();
        if (tm.valid) {
            if (!plan_detail::sumTargetPcmBaselineBytesFolder(*req.targetKst, targetBaselineBytes, req.keepGoing, stride)) {
                return makeCancelled();
            }
        }
        targetMap = std::move(tm);
    }

    // 4. Always inspect embedded KBD Sets & Pads + require complete sample closure for RootTreeSubKst
    NativeSelectivePlanRequest pReq;
    pReq.sourceKst = req.sourceKst;
    pReq.targetKst = req.targetKst;
    pReq.targets = req.targets;
    pReq.mode = ExportMode::RootTreeSubKst;
    pReq.policy = req.policy;
    pReq.includeEmbeddedKbdSets = true;
    pReq.includeEmbeddedPads = true;
    pReq.requireCompleteSampleClosure = true;
    pReq.approvedSoundRemaps = req.approvedSoundRemaps;
    pReq.keepGoing = req.keepGoing;
    pReq.cancelCheckStride = stride;

    NativeSelectivePlanResult planRes = planNativeSelectiveExportFromViews(
        srcView, pReq, targetMap.has_value() ? &(*targetMap) : nullptr, targetBaselineBytes);
    if (planRes.cancelled) return makeCancelled();

    // 5. Under ReuseIdenticalTargetSlot / RemapToFreeSlotInNewLib (when no explicit remap filter is set
    //    and !requireExplicitRemapApproval), automatically approve M2a/M2b-proven Identical cross-slot
    //    target matches (zero redundant disk I/O — re-evaluates in-memory views!).
    const bool policyAllowsCrossSlotReuse =
        (req.policy == ConflictPolicy::ReuseIdenticalTargetSlot ||
         req.policy == ConflictPolicy::RemapToFreeSlotInNewLib);
    if (planRes.ok && !planRes.missingSelectionTarget &&
        targetMap.has_value() && targetMap->valid && targetMap->fullyInspectable &&
        policyAllowsCrossSlotReuse && !req.requireExplicitRemapApproval && req.approvedSoundRemaps.empty()) {
        std::map<media::graph::ResourceId, media::graph::ResourceId> autoApproved;
        for (const CollectedSoundSummary& cs : planRes.dryRunPlan.collectedSounds) {
            if (cs.requiresRemap && !cs.satisfiedByTarget && !cs.matchedTargetSlot.empty() &&
                cs.analysed && !cs.isDrumKit) {
                media::graph::ResourceId dstId;
                if (media::graph::ResourceId::parse(cs.matchedTargetSlot, dstId)) {
                    autoApproved[cs.id] = dstId;
                }
            }
        }
        if (!autoApproved.empty()) {
            pReq.approvedSoundRemaps = std::move(autoApproved);
            planRes = planNativeSelectiveExportFromViews(
                srcView, pReq, &(*targetMap), targetBaselineBytes);
            if (planRes.cancelled) return makeCancelled();
        }
    }

    // Track any extra diagnostics added after planNativeSelectiveExportFromViews() so both
    // report and dryRunPlan stay 100% canonicalized and synchronized on executable().
    bool addedDiag = false;
    auto pushExtraDiag = [&](Diagnostic d) {
        addedDiag = true;
        planRes.report.diagnostics.push_back(d);
        planRes.dryRunPlan.diagnostics.push_back(std::move(d));
    };

    // Also fail closed if user explicitly passed an approved remap entry targeting a slot that is NOT
    // proven Identical in targetKst (or when targetKst is missing/uninspectable).
    if (planRes.ok && !req.approvedSoundRemaps.empty()) {
        for (const auto& [srcId, dstId] : req.approvedSoundRemaps) {
            bool matchedAndSatisfied = false;
            bool referenced = false;
            for (const CollectedSoundSummary& cs : planRes.dryRunPlan.collectedSounds) {
                if (cs.id == srcId) {
                    referenced = true;
                    if (cs.satisfiedByTarget && cs.matchedTargetSlot == dstId.str()) {
                        matchedAndSatisfied = true;
                    }
                    break;
                }
            }
            if (referenced && !matchedAndSatisfied) {
                const bool alreadyBlocked = planRes.report.blockingDiagnosticCount() > 0;
                if (!alreadyBlocked) {
                    pushExtraDiag({
                        DiagnosticCategory::UNSUPPORTED_FORMAT,
                        diag_code::kNativeSoundWriteHold,
                        DiagnosticSeverity::Blocking,
                        EvidenceLevel::Unsupported,
                        srcId.str(),
                        dstId.str(),
                        "Approved remap target " + dstId.str() + " is not proven equivalent (Identical) to " +
                            srcId.str() + " in Target KST.",
                        "Target slot must hold a proven-identical analysed Sound; otherwise use AdvisoryPlan + MAN5 p.736 single-Sound load.",
                        {}
                    });
                }
            }
        }
    }

    out.missingSelectionTarget = planRes.missingSelectionTarget;
    out.targetLibraryKey = planRes.report.targetLibraryKey;

    if (!planRes.ok) {
        out.ok = false;
        out.error = planRes.error;
        out.planResult = std::move(planRes);
        return out;
    }

    // 6. Strict Belt-and-Suspenders Sound Eligibility & Allowed Target-Matched Slot Extraction

    for (const CollectedSoundSummary& cs : planRes.dryRunPlan.collectedSounds) {
        if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();
        if (!cs.satisfiedByTarget || cs.matchedTargetSlot.empty() || !cs.analysed || cs.isDrumKit) {
            // Ensure blocking diagnostic is always present (never silently drop or allow unanalysed/USERDK/missing Sound)
            if (planRes.report.blockingDiagnosticCount() == 0) {
                pushExtraDiag({
                    DiagnosticCategory::UNSUPPORTED_FORMAT,
                    diag_code::kNativeSoundWriteHold,
                    DiagnosticSeverity::Blocking,
                    EvidenceLevel::Unsupported,
                    cs.id.str(),
                    "",
                    "User Sound " + cs.id.str() + " is not satisfied by a proven-identical analysed Sound in Target KST; native Sound export is on HOLD.",
                    "Load this Sound individually on Pa5X via Media -> Load (MAN5 p.736) or remap to an existing Factory/Target Sound.",
                    cs.referencingRoots
                });
            }
            continue;
        }

        const auto srcAddr = SlotAddress::fromBankAndSlot(cs.id.scope, cs.id.number);
        media::graph::ResourceId matchedId;
        const bool parsedMatched = media::graph::ResourceId::parse(cs.matchedTargetSlot, matchedId);
        const auto dstAddr = parsedMatched ? SlotAddress::fromBankAndSlot(matchedId.scope, matchedId.number) : std::nullopt;
        if (!srcAddr.has_value() || !dstAddr.has_value() ||
            !srcAddr->isInSpec() || !dstAddr->isInSpec() ||
            srcAddr->isDrumKit || dstAddr->isDrumKit) {
            pushExtraDiag({
                DiagnosticCategory::UNSUPPORTED_FORMAT,
                diag_code::kNativeSoundWriteHold,
                DiagnosticSeverity::Blocking,
                EvidenceLevel::Unsupported,
                cs.id.str(),
                cs.matchedTargetSlot,
                "Target-matched Sound slot address is invalid or outside melodic USER01..12 spec.",
                "Verify source and target Sound slots before exporting.",
                cs.referencingRoots
            });
            continue;
        }

        // Verify target occupancy map still proves Identical at dstAddr!
        if (!targetMap.has_value() || !targetMap->valid || !targetMap->fullyInspectable ||
            !targetMap->findOccupied(*dstAddr)) {
            pushExtraDiag({
                DiagnosticCategory::UNSUPPORTED_FORMAT,
                diag_code::kTargetUninspectable,
                DiagnosticSeverity::Blocking,
                EvidenceLevel::Unsupported,
                cs.id.str(),
                dstAddr->shortLabel(),
                "Target KST does not provide a verifiable Sound at " + dstAddr->shortLabel() + ".",
                "Supply a fully inspectable Target KST containing the proven-identical Sound.",
                cs.referencingRoots
            });
            continue;
        }

        out.allowedTargetMatchedUserSlots.insert(detail::closureDanglingKeyForSlot(*dstAddr));

        if (*srcAddr != *dstAddr) {
            SubKstSoundRemapEntry rm;
            rm.sourceSoundId = cs.id;
            rm.targetSoundId = matchedId;
            rm.sourceAddress = *srcAddr;
            rm.targetAddress = *dstAddr;
            rm.srcTriple = detail::slotToMidiTriple(*srcAddr);
            rm.dstTriple = detail::slotToMidiTriple(*dstAddr);
            out.midiRemapRules[rm.srcTriple] = rm.dstTriple;
            out.soundRemaps.push_back(std::move(rm));
            out.anySoundRemapped = true;
        }
    }

    std::sort(out.soundRemaps.begin(), out.soundRemaps.end());

    // Pin exact raw SHA-256 of every referenced target SOUND/*.PCG bank file (catches even name/dir-level TOCTOU edits)
    if (req.targetKst.has_value() && !out.allowedTargetMatchedUserSlots.empty()) {
        std::set<std::string> wantedPcgUpper;
        for (const std::string& slotKey : out.allowedTargetMatchedUserSlots) {
            const size_t sp = slotKey.find(' ');
            if (sp != std::string::npos) {
                wantedPcgUpper.insert(media::graph::native_detail::upperAscii(slotKey.substr(0, sp)));
            }
        }
        const std::string tgtClean = media::graph::native_detail::stripTrailingSlashes(*req.targetKst);
        const media::detail::Fd tgtRootFd(
            ::open(tgtClean.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (tgtRootFd.v >= 0) {
            std::vector<std::string> topEntries;
            std::string lerr;
            if (media::graph::native_detail::listDirEntriesAt(tgtRootFd.v, topEntries, 1000000, lerr)) {
                for (const std::string& top : topEntries) {
                    if (media::graph::native_detail::upperAscii(top) != "SOUND") continue;
                    const media::detail::Fd sndFd(
                        ::openat(tgtRootFd.v, top.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
                    if (sndFd.v < 0) continue;
                    std::vector<std::string> leaves;
                    if (!media::graph::native_detail::listDirEntriesAt(sndFd.v, leaves, 1000000, lerr)) continue;
                    for (const std::string& leaf : leaves) {
                        if (!wantedPcgUpper.count(media::graph::native_detail::upperAscii(leaf))) continue;
                        const std::string rel = top + "/" + leaf;
                        std::string herr;
                        const std::string sha = detail::hashRelFilePinned(tgtRootFd.v, rel, herr);
                        if (!sha.empty()) out.expectedTargetSoundBankSha256[rel] = sha;
                    }
                }
            }
        }
        if (out.expectedTargetSoundBankSha256.size() < wantedPcgUpper.size()) {
            pushExtraDiag({
                DiagnosticCategory::UNSUPPORTED_FORMAT,
                diag_code::kTargetUninspectable,
                DiagnosticSeverity::Blocking,
                EvidenceLevel::Unsupported,
                "<target>",
                "SOUND/*.PCG",
                "Failed to pin SHA-256 of every referenced target SOUND/*.PCG bank file (expected " +
                    std::to_string(wantedPcgUpper.size()) + ", pinned " +
                    std::to_string(out.expectedTargetSoundBankSha256.size()) + "); refusing fail-closed.",
                "Verify target KST SOUND/*.PCG files are readable regular files (no symlinks) and retry.",
                {}
            });
        }
    }

    // 7. Build deterministic PlannedRootObjects & validate every destination path strictly
    std::map<std::string, const ObjectPathResolutionEntry*> objResBySrc;
    for (const auto& ores : planRes.objectResolutions) {
        objResBySrc[ores.sourceRelPath] = &ores;
    }

    std::set<std::string> seenDestFolded;
    for (const SelectionTarget& sel : planRes.dryRunPlan.selectedTargets) {
        if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();
        const RootItemRecord* r = srcView.findRoot(sel.relPath);
        if (!r) continue;

        PlannedRootObject obj;
        obj.kind = sel.kind;
        obj.sourceRelPath = r->relPath;
        obj.sourceSha256 = r->contentSha256;

        if (const auto oit = objResBySrc.find(r->relPath); oit != objResBySrc.end()) {
            obj.destRelPath = oit->second->targetRelPath;
            obj.alreadyPresentInTarget = (oit->second->action == "already-present-noop");
        } else {
            std::string perr;
            const auto eff = conflict_detail::resolveTargetPathOverride(r->relPath, sel.targetBank, sel.targetName, &perr);
            obj.destRelPath = eff.value_or(r->relPath);
        }

        obj.renamedOrRebanked = (obj.destRelPath != obj.sourceRelPath);
        if (obj.renamedOrRebanked) out.anyObjectRenamedOrRebanked = true;
        obj.requiresSmfRemap = !out.midiRemapRules.empty();

        // Enforce strict Root-tree destination path & kind-matching invariant (zero PCM/SOUND/MULTISMP/SONGBOOK, zero cross-type redirect)
        if (!obj.destRelPath.empty()) {
            const std::string pathErr = detail::validateSubKstDestRelPath(obj.destRelPath, obj.kind);
            if (!pathErr.empty()) {
                pushExtraDiag({
                    DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                    diag_code::kStyleOrObjectPathCollision,
                    DiagnosticSeverity::Blocking,
                    EvidenceLevel::Verified,
                    obj.sourceRelPath,
                    obj.destRelPath,
                    pathErr,
                    "Select only STYLE/<bank>/*.stg, KEYBOARDSET/<bank>/*.ksc, PAD/<bank>/*.pdc, or SETTINGS/*.ksc with safe names.",
                    {obj.sourceRelPath}
                });
            }
            const std::string folded = conflict_detail::asciiLower(obj.destRelPath);
            if (!seenDestFolded.insert(folded).second) {
                pushExtraDiag({
                    DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                    diag_code::kFatCaseCollisionR18,
                    DiagnosticSeverity::Blocking,
                    EvidenceLevel::Verified,
                    obj.destRelPath,
                    folded,
                    "Duplicate output path under ASCII case-folding (FAT32/exFAT R-18): " + obj.destRelPath,
                    "Provide distinct targetBank or targetName overrides.",
                    {obj.sourceRelPath}
                });
            }
        }

        if (obj.sourceSha256.size() != 64) {
            pushExtraDiag({
                DiagnosticCategory::VERIFIED_MISSING,
                collector_diag_code::kSourceUnavailable,
                DiagnosticSeverity::Blocking,
                EvidenceLevel::Verified,
                obj.sourceRelPath,
                "",
                "Missing SHA-256 digest for selected source root file " + obj.sourceRelPath + "; refusing TOCTOU-unsafe export.",
                "Verify the source file is a readable regular file and retry.",
                {obj.sourceRelPath}
            });
        } else {
            out.expectedSourceFileSha256[obj.sourceRelPath] = obj.sourceSha256;
        }

        out.plannedObjects.push_back(std::move(obj));
    }

    std::sort(out.plannedObjects.begin(), out.plannedObjects.end(),
              [](const PlannedRootObject& a, const PlannedRootObject& b) {
                  return std::tie(a.destRelPath, a.sourceRelPath) < std::tie(b.destRelPath, b.sourceRelPath);
              });

    // 8. Mandatory SONGBOOK_UNREWRITTEN warning whenever ANY Sound is remapped OR object renamed/rebanked!
    if (out.anySoundRemapped || out.anyObjectRenamedOrRebanked) {
        const bool hasSongBookWarn = std::any_of(
            planRes.report.diagnostics.begin(), planRes.report.diagnostics.end(),
            [](const Diagnostic& d) { return d.code == diag_code::kSongBookUnrewritten; });
        if (!hasSongBookWarn) {
            pushExtraDiag({
                DiagnosticCategory::UNSUPPORTED_FORMAT,
                diag_code::kSongBookUnrewritten,
                DiagnosticSeverity::Warning,
                EvidenceLevel::Unsupported,
                "SONGBOOK/SONGDB.SBD",
                "",
                kSongBookUnrewrittenWarning,
                "After loading remapped Sounds or renamed Styles on the Pa5X, update affected SongBook entries on the instrument.",
                {}
            });
        }
    }

    // 9. Re-canonicalize report & dryRunPlan if any diagnostics were added
    if (addedDiag) {
        canonicalizeDiagnostics(planRes.dryRunPlan.diagnostics);
        planRes.dryRunPlan.executable =
            planRes.dryRunPlan.ok && !planRes.dryRunPlan.cancelled &&
            planRes.dryRunPlan.blockingDiagnosticCount() == 0 &&
            planRes.dryRunPlan.blockingConflictCount() == 0 &&
            planRes.dryRunPlan.mode != ExportMode::NativeSoundKst_HOLD;
        planRes.dryRunPlan.planFingerprint = collector_detail::computePlanFingerprint(planRes.dryRunPlan);
        planRes.planFingerprint = planRes.dryRunPlan.planFingerprint;
        planRes.report.planFingerprint = planRes.planFingerprint;
        planRes.report = canonicalizeReport(std::move(planRes.report));
    }

    out.ok = true;
    out.planResult = std::move(planRes);
    out.planFingerprint = detail::computeSubKstFingerprint(out);
    out.planResult.planFingerprint = out.planFingerprint;
    out.planResult.report.planFingerprint = out.planFingerprint;
    return out;
}

// Constructs the descriptor-pinned E6/E9 ExportSpec for executing a RootTreeSubKst selective export.
// Modeled directly on `jobs::setToKst()`: uses `ExportSpec::writeAt`, `O_NOFOLLOW`, `O_EXCL`,
// D18 overlap protection on both `sourceKst` and optional `targetKst`, target-aware L1 staged
// verification, and `beforePublish` source/target TOCTOU + `library::recheckPinned()`.
inline ExportSpec selectiveSubKstExportSpec(
    const SubKstExportRequest& req,
    std::filesystem::path dest,
    std::shared_ptr<SubKstExportPlan> outPlan = nullptr,
    std::shared_ptr<SubKstExecutionSummary> outSummary = nullptr,
    std::function<void(const char*)> testHook = {}) {
    namespace fs = std::filesystem;
    using namespace media::selective;

    struct State {
        SubKstExportPlan plan;
        library::detail::FileIds written;
        std::shared_ptr<library::Pinned> pinned = std::make_shared<library::Pinned>();
        std::map<std::string, std::string> plannedOutputSha;
        std::map<std::string, std::map<std::string, std::string>> expectedNonMidiLeafSha;
        std::map<std::string, std::vector<std::pair<std::string, std::vector<media::panative::closure::Select>>>> expectedPostSelects;
        std::vector<ExportedFileSummary> exportedFiles;
        std::string destLeaf;
    };

    const auto st = std::make_shared<State>();
    ExportSpec s;
    s.type = "Selective Root-Tree Sub-KST (EXPERIMENTAL, NOT VERIFIED ON A PA5X)";
    s.source = req.targetKst.has_value() ? "<source> -> <target>" : "<source>";
    s.sources.push_back(req.sourceKst);
    if (req.targetKst.has_value()) s.sources.push_back(*req.targetKst);
    s.dest = std::move(dest);
    s.moveLeftoverAside = req.moveLeftoverAside;

    std::error_code absEc;
    const fs::path destAbs = media::folderPath(fs::absolute(s.dest, absEc));
    st->destLeaf = destAbs.filename().string();

    s.validate = [st, req, destAbs, outPlan, testHook]() -> std::string {
        if (testHook) testHook("validateStart");

        // Destination checks first (zero wasted source reads on invalid destination / dangling symlink / D18 overlap).
        {
            std::error_code symEc;
            const fs::file_status destStat = fs::symlink_status(destAbs, symEc);
            if (symEc && destStat.type() != fs::file_type::not_found) {
                return "refusing: cannot inspect destination path: " + symEc.message();
            }
            if (fs::is_symlink(destStat)) {
                return "refusing: destination path is a symlink (symlinks are never followed or replaced)";
            }
        }
        if (const auto why = media::outputFolderRefusal(destAbs);
            !why.empty() && why != media::incompleteRefusal(destAbs)) {
            return why;
        }
        const std::string leaf = destAbs.filename().string();
        if (leaf.size() <= 4 || leaf.compare(leaf.size() - 4, 4, ".KST") != 0 ||
            !media::panative::assemble::safeName(leaf.substr(0, leaf.size() - 4))) {
            return "refusing: output folder must be named NAME.KST with a safe NAME: " + leaf;
        }
        if (jobs::detail::overlaps(destAbs, req.sourceKst)) {
            return "refusing: output folder overlaps source KST (D18)";
        }
        if (req.targetKst.has_value() && jobs::detail::overlaps(destAbs, *req.targetKst)) {
            return "refusing: output folder overlaps target KST (D18)";
        }
        if (!req.expectFingerprint.empty() && !detail::isHex64(req.expectFingerprint)) {
            return "refusing: --expect-fingerprint must be a 64-character hex SHA-256";
        }

        // Re-plan strictly read-only (O_NOFOLLOW) and check TOCTOU fingerprint & targetLibraryKey.
        st->plan = planSubKstExport(req);
        if (outPlan) *outPlan = st->plan;
        if (st->plan.cancelled) return "refusing: selective sub-KST planning cancelled";
        if (!st->plan.ok) return "refusing: " + st->plan.error;
        if (st->plan.missingSelectionTarget) {
            return "refusing: one or more selected paths were not found in source KST";
        }
        if (!req.expectFingerprint.empty() && st->plan.planFingerprint != req.expectFingerprint) {
            return "refusing: sources or target changed since review (planFingerprint mismatch); review again";
        }
        if (!req.expectTargetLibraryKey.empty() && st->plan.targetLibraryKey != req.expectTargetLibraryKey) {
            return "refusing: target KST changed since review (targetLibraryKey mismatch); review again";
        }
        if (!st->plan.executable()) {
            return "refusing: " + detail::firstBlockingSummary(st->plan.planResult.report);
        }
        return "";
    };

    s.dependencies = [st](std::vector<std::string>& notes, uint64_t& sampleBytes) {
        sampleBytes = 0;  // Zero PCM/Sound/Multisample bytes written in RootTreeSubKst
        notes.push_back(std::string(kHardwareUnverifiedBanner));
        notes.push_back("Root-tree sub-KST ONLY: ZERO PCM/, SOUND/, MULTISMP/, or SONGBOOK/ directories written (HOLD + MAN5 p.736).");
        notes.push_back("Plan fingerprint: " + st->plan.planFingerprint);
        notes.push_back("Selected objects: " + std::to_string(st->plan.plannedObjects.size()) +
                        ", Factory ROM selects: " + std::to_string(st->plan.planResult.dryRunPlan.factorySoundSelectCount) +
                        ", Target-matched User Sounds: " + std::to_string(st->plan.allowedTargetMatchedUserSlots.size()) +
                        ", Sound remaps: " + std::to_string(st->plan.soundRemaps.size()));
        if (!st->plan.allowedTargetMatchedUserSlots.empty()) {
            notes.push_back("Target-matched User Sounds verified against pinned Target KST (targetLibraryKey=" +
                            st->plan.targetLibraryKey + "). " + std::string(kStoredPcmProxyCaveat));
        }
        if (st->plan.anySoundRemapped || st->plan.anyObjectRenamedOrRebanked) {
            notes.push_back(std::string(diag_code::kSongBookUnrewritten) + ": " + std::string(kSongBookUnrewrittenWarning));
        }
        return std::vector<std::string>{};
    };

    s.preview = [st]() {
        std::vector<OutFile> r;
        r.reserve(st->plan.plannedObjects.size());
        for (const auto& obj : st->plan.plannedObjects) {
            r.push_back({obj.destRelPath, 0, "", !obj.requiresSmfRemap});
        }
        return r;
    };

    s.writeAt = [st, req, srcs = s.sources, testHook](
                    int outFd, const fs::path&, const std::function<bool(double)>& progress) -> WriteResult {
        if (testHook) testHook("beforeWriteAt");
        const std::string real = media::detail::fdPath(outFd);
        if (real.empty()) {
            return {"refusing: cannot confirm real path of pinned staging folder; nothing published", ""};
        }
        for (const auto& src : srcs) {
            if (jobs::detail::overlaps(real, src)) {
                return {"refusing: staged sub-KST overlaps source or target KST (D18); nothing published", ""};
            }
        }

        const std::string srcClean = media::graph::native_detail::stripTrailingSlashes(req.sourceKst);
        const media::detail::Fd srcRootFd(
            ::open(srcClean.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (srcRootFd.v < 0) {
            return {"refusing: cannot open source KST read-only (missing or symlink)", ""};
        }

        st->written.clear();
        st->plannedOutputSha.clear();
        st->expectedNonMidiLeafSha.clear();
        st->expectedPostSelects.clear();
        st->exportedFiles.clear();

        const size_t total = std::max<size_t>(1, st->plan.plannedObjects.size());
        size_t done = 0;
        size_t totalRemappedEvents = 0;

        for (const auto& obj : st->plan.plannedObjects) {
            if (const std::string perr = detail::validateSubKstDestRelPath(obj.destRelPath, obj.kind); !perr.empty()) {
                return {"refusing: " + perr, ""};
            }

            // Read source Root-tree strictly through pinned source fd + streaming SHA-256 verification
            std::vector<uint8_t> rawBytes;
            std::string gotSrcSha;
            std::string readErr;
            if (!detail::readRelBytesPinned(srcRootFd.v, obj.sourceRelPath,
                                            media::panative::kMaxFileBytes,
                                            rawBytes, gotSrcSha, readErr)) {
                return {"refusing: " + readErr, ""};
            }
            if (gotSrcSha != obj.sourceSha256) {
                return {"refusing: source file " + obj.sourceRelPath +
                        " changed since review (SHA-256 mismatch); review again", ""};
            }

            // Parse source Root-tree to capture original non-MIDI leaf SHAs (XML byte preservation proof)
            const auto origParsed = media::panative::roottree::parse(rawBytes.data(), rawBytes.size());
            if (!origParsed.ok) {
                return {"refusing: source Root-tree failed parsing (" + obj.sourceRelPath + "): " + origParsed.error, ""};
            }
            std::map<std::string, std::string> origNonMidiSha;
            std::vector<std::pair<std::string, std::vector<media::panative::closure::Select>>> origMidiSel;
            std::string origDigestErr;
            if (!detail::collectRootTreeDigest(origParsed.root, "", origNonMidiSha, origMidiSel, origDigestErr)) {
                return {"refusing: malformed source Root-tree MIDI leaf (" + obj.sourceRelPath + "): " + origDigestErr, ""};
            }

            // Run M2e-1 smf_remap::remapRootTree() in memory:
            //  - When midiRemapRules is empty: validates every MIDI leaf strictly and returns exact untouched bytes!
            //  - When midiRemapRules is non-empty: rewrites approved (MSB, LSB, PC) triples with round-trip verification!
            media::panative::smf_remap::RemapOptions remapOpts;
            remapOpts.strictUnresolved = true;
            remapOpts.verifyRoundTrip = true;
            const auto remapRes = media::panative::smf_remap::remapRootTree(
                rawBytes.data(), rawBytes.size(), st->plan.midiRemapRules, remapOpts);
            if (!remapRes.ok) {
                return {"refusing: SMF remap/validation failed on " + obj.sourceRelPath +
                        (remapRes.failedLeafPath.empty() ? "" : (" (" + remapRes.failedLeafPath + ")")) +
                        ": " + remapRes.error, ""};
            }

            // Parse output bytes to verify content type & record expected post-remap selects
            const auto outParsed = media::panative::roottree::parse(remapRes.bytes.data(), remapRes.bytes.size());
            if (!outParsed.ok) {
                return {"refusing: output Root-tree failed re-parse (" + obj.destRelPath + "): " + outParsed.error, ""};
            }
            const std::string topUpper = media::graph::native_detail::upperAscii(
                obj.destRelPath.substr(0, obj.destRelPath.find('/')));
            if (topUpper == "STYLE" || topUpper == "KEYBOARDSET" || topUpper == "PAD") {
                if (const std::string why = media::panative::assemble::contentRefusal(topUpper, outParsed.root);
                    !why.empty()) {
                    return {"refusing: " + obj.destRelPath + ": " + why, ""};
                }
            }
            std::map<std::string, std::string> outNonMidiSha;
            std::vector<std::pair<std::string, std::vector<media::panative::closure::Select>>> outMidiSel;
            std::string outDigestErr;
            if (!detail::collectRootTreeDigest(outParsed.root, "", outNonMidiSha, outMidiSel, outDigestErr)) {
                return {"refusing: output Root-tree MIDI verification failed (" + obj.destRelPath + "): " + outDigestErr, ""};
            }
            if (outNonMidiSha != origNonMidiSha) {
                return {"refusing: non-MIDI (XML) leaf integrity mismatch during in-memory remap of " + obj.destRelPath, ""};
            }

            const std::string outSha = detail::sha256Bytes(remapRes.bytes.data(), remapRes.bytes.size());

            // Write via descriptor-pinned openDirIn(outFd) + openat(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC) + fsync + fstat
            const auto [destDir, destLeaf] = media::detail::splitRel(obj.destRelPath);
            const media::detail::Fd dfd(library::detail::openDirIn(outFd, destDir, true));
            const media::detail::Fd ffd(
                dfd.v < 0 ? -1
                          : ::openat(dfd.v, destLeaf.c_str(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
            if (ffd.v < 0) {
                return {"cannot create " + obj.destRelPath + " (it may already exist; links are never followed)", ""};
            }
            for (size_t w = 0; w < remapRes.bytes.size();) {
                const ssize_t k = ::write(ffd.v, remapRes.bytes.data() + w, remapRes.bytes.size() - w);
                if (k < 0) {
                    if (errno == EINTR) continue;
                    return {"cannot write " + obj.destRelPath + " (disk full?)", ""};
                }
                if (k == 0) return {"short write on " + obj.destRelPath, ""};
                w += static_cast<size_t>(k);
            }
            struct stat fst{};
            if (::fsync(ffd.v) != 0 || ::fstat(ffd.v, &fst) != 0) {
                return {"cannot fsync/fstat " + obj.destRelPath, ""};
            }

            st->written[obj.destRelPath] = {fst.st_dev, fst.st_ino};
            st->plannedOutputSha[obj.destRelPath] = outSha;
            st->expectedNonMidiLeafSha[obj.destRelPath] = std::move(origNonMidiSha);
            st->expectedPostSelects[obj.destRelPath] = std::move(outMidiSel);
            st->exportedFiles.push_back({
                obj.sourceRelPath,
                obj.destRelPath,
                outSha,
                static_cast<uint64_t>(remapRes.bytes.size()),
                remapRes.modified,
                remapRes.remappedEvents
            });
            totalRemappedEvents += remapRes.remappedEvents;

            if (!progress(double(++done) / double(total))) return {"cancelled", ""};
        }

        const std::string msg =
            "Staged " + std::to_string(st->exportedFiles.size()) + " Root-tree object(s) (" +
            std::to_string(totalRemappedEvents) + " MIDI Bank/PC event(s) remapped; ZERO PCM/SOUND/MULTISMP/SONGBOOK written). " +
            std::string(kHardwareUnverifiedBanner);
        return {"", msg};
    };

    s.verify = [st, outSummary, testHook](const fs::path& out) {
        if (testHook) testHook("beforeVerify");
        Verification v;
        v.l2 = {Check::NotRun, "not verified on a Pa5X"};
        v.l3 = {Check::NotRun, "not tested on a Pa5X"};
        const auto failed = [&](const std::string& why) {
            v.l1 = {Check::Failed, why};
            return std::pair<Verification, std::string>{v, why};
        };

        // 1. Pin staged folder by descriptor & assert exact device/inode + SHA-256 parity with in-memory bytes
        st->pinned = std::make_shared<library::Pinned>();
        library::detail::pin(out, *st->pinned, false);
        if (!st->pinned->error.empty()) return failed(st->pinned->error);
        if (st->pinned->ids != st->written) {
            return failed("staged files do not match written descriptors (device/inode differ, or file missing/extra)");
        }

        size_t regularCount = 0;
        for (const auto& [rel, sha] : st->pinned->tree) {
            const bool isDir = (!rel.empty() && rel.back() == '/');
            const std::string trimmed = isDir ? rel.substr(0, rel.size() - 1) : rel;
            const std::string top = trimmed.substr(0, trimmed.find('/'));
            const std::string topUpper = media::graph::native_detail::upperAscii(top);

            // 2. Explicit HOLD gate & MAN5 p.736 RAM-wipe prevention assertion!
            if (detail::isForbiddenTopDirUpper(topUpper)) {
                return failed("HOLD VIOLATION: forbidden directory " + topUpper + "/ found in staged output");
            }
            if (!detail::isAllowedRootTopDirUpper(topUpper)) {
                return failed("unexpected top-level directory in staged output: " + top);
            }
            if (!isDir) {
                ++regularCount;
                const std::string relUpper = media::graph::native_detail::upperAscii(rel);
                if (detail::isForbiddenExtensionUpper(relUpper)) {
                    return failed("HOLD VIOLATION: forbidden native container file in staged output: " + rel);
                }
                const auto pit = st->plannedOutputSha.find(rel);
                if (pit == st->plannedOutputSha.end() || pit->second != sha) {
                    return failed("staged file SHA-256 mismatch at " + rel);
                }
            }
        }
        if (regularCount != st->plannedOutputSha.size()) {
            return failed("staged regular file count mismatch (expected " +
                          std::to_string(st->plannedOutputSha.size()) + ", found " +
                          std::to_string(regularCount) + ")");
        }

        // 3. Structural KST validator (0 errors required)
        const auto val = library::validate(out);
        const size_t valErrors = val.count(library::Severity::Error);
        const size_t valWarnings = val.count(library::Severity::Warn);
        if (!val.ok) return failed("structural validator could not finish: " + val.error);
        if (valErrors > 0) {
            return failed("structural validator reported " + std::to_string(valErrors) +
                          " error(s)\n" + library::validationText(val));
        }

        // 4. Target-aware closure verification (avoids the standalone stagedRefusal() trap!)
        size_t factoryRefs = 0;
        size_t targetMatchedUserRefs = 0;
        if (st->plan.allowedTargetMatchedUserSlots.empty()) {
            // Factory-only sub-KST: call W5 assemble::stagedRefusal() verbatim!
            media::panative::assemble::Result asmRes;
            if (const auto why = media::panative::assemble::stagedRefusal(out, asmRes); !why.empty()) {
                return failed(why);
            }
            factoryRefs = asmRes.factoryRefs;
            targetMatchedUserRefs = asmRes.userRefs;
        } else {
            // Target-matched User Sounds present: SOUND/ is intentionally omitted (HOLD + MAN5 p.736),
            // so every dangling ref MUST belong to planned.allowedTargetMatchedUserSlots!
            const auto c = media::panative::closure::check(out);
            if (!c.ok || !c.problems.empty()) {
                return failed("closure could not read every staged object: " +
                              (c.problems.empty() ? c.error : c.problems.front()));
            }
            if (c.unresolved > 0) {
                return failed("closure: " + std::to_string(c.unresolved) +
                              " unresolved sound selection(s) (Program Change without CC0/CC32)");
            }
            if (c.resolved > 0) {
                return failed("HOLD VIOLATION: staged output resolved " + std::to_string(c.resolved) +
                              " local User Sound(s), implying SOUND/ exists");
            }
            for (const auto& [danglingKey, count] : c.dangling) {
                if (!st->plan.allowedTargetMatchedUserSlots.count(danglingKey)) {
                    return failed("unexpected dangling User Sound reference not proven in Target KST: " +
                                  danglingKey + " (" + std::to_string(count) + " ref(s))");
                }
                targetMatchedUserRefs += count;
            }
            factoryRefs = c.factory;
        }

        // 5. Independent re-parse through pinned staging fd: verify post-remap MIDI selects 1:1 & XML preservation
        for (const auto& obj : st->plan.plannedObjects) {
            std::vector<uint8_t> stagedBytes;
            std::string stagedSha;
            std::string rerr;
            if (!detail::readRelBytesPinned(st->pinned->dir, obj.destRelPath,
                                            media::panative::kMaxFileBytes,
                                            stagedBytes, stagedSha, rerr)) {
                return failed("re-parse read failed on " + obj.destRelPath + ": " + rerr);
            }
            if (stagedSha != st->plannedOutputSha.at(obj.destRelPath)) {
                return failed("re-parse SHA-256 mismatch on " + obj.destRelPath);
            }
            const auto reparsed = media::panative::roottree::parse(stagedBytes.data(), stagedBytes.size());
            if (!reparsed.ok) {
                return failed("staged Root-tree failed re-parse (" + obj.destRelPath + "): " + reparsed.error);
            }
            std::string roundTripBytes;
            std::string rtErr;
            if (!media::panative::roottree::write(reparsed.root, roundTripBytes, rtErr) ||
                roundTripBytes.size() != stagedBytes.size() ||
                std::memcmp(roundTripBytes.data(), stagedBytes.data(), stagedBytes.size()) != 0) {
                return failed("staged Root-tree failed byte-exact round-trip (" + obj.destRelPath + ")");
            }
            std::map<std::string, std::string> gotNonMidiSha;
            std::vector<std::pair<std::string, std::vector<media::panative::closure::Select>>> gotMidiSel;
            std::string dErr;
            if (!detail::collectRootTreeDigest(reparsed.root, "", gotNonMidiSha, gotMidiSel, dErr)) {
                return failed("staged MIDI leaf check failed (" + obj.destRelPath + "): " + dErr);
            }
            if (gotNonMidiSha != st->expectedNonMidiLeafSha.at(obj.destRelPath)) {
                return failed("staged non-MIDI (XML) leaf bytes differ from source (" + obj.destRelPath + ")");
            }
            if (!detail::midiLeafVectorsEqual(gotMidiSel, st->expectedPostSelects.at(obj.destRelPath))) {
                return failed("staged post-remap MIDI Sound selects do not match expected plan 1:1 (" +
                              obj.destRelPath + ")");
            }
        }

        if (outSummary) {
            outSummary->outputLeafName = st->destLeaf;
            outSummary->exportedFiles = st->exportedFiles;
            outSummary->factoryRefs = factoryRefs;
            outSummary->targetMatchedUserRefs = targetMatchedUserRefs;
            outSummary->validatorWarnings = valWarnings;
            outSummary->warnings.clear();
            if (st->plan.anySoundRemapped || st->plan.anyObjectRenamedOrRebanked) {
                outSummary->warnings.push_back(
                    std::string(diag_code::kSongBookUnrewritten) + ": " + std::string(kSongBookUnrewrittenWarning));
            }
        }

        const std::string l1Detail =
            std::to_string(st->exportedFiles.size()) + " Root-tree file(s) pinned, re-parsed & verified; " +
            "ZERO PCM/SOUND/MULTISMP/SONGBOOK dirs; validator 0 errors (" + std::to_string(valWarnings) +
            " warning(s)); closure OK (factoryRefs=" + std::to_string(factoryRefs) +
            ", targetMatchedUserRefs=" + std::to_string(targetMatchedUserRefs) + ", unresolved=0)";
        v.l1 = {Check::Passed, l1Detail};
        return std::pair<Verification, std::string>{
            v, std::string(kHardwareUnverifiedBanner) + ". " + l1Detail + "."};
    };

    s.verifiedHashes = [st]() {
        std::map<std::string, std::string> filesOnly;
        if (!st->pinned) return filesOnly;
        for (const auto& [rel, sha] : st->pinned->tree) {
            if (!rel.empty() && rel.back() != '/') filesOnly[rel] = sha;
        }
        return filesOnly;
    };

    s.beforePublish = [st, req, testHook](const fs::path& out) -> std::string {
        if (testHook) testHook("beforePublish");

        // (1) Re-verify every selected source root file SHA-256 unchanged via pinned source fd (O_NOFOLLOW)
        const std::string srcClean = media::graph::native_detail::stripTrailingSlashes(req.sourceKst);
        const media::detail::Fd srcRootFd(
            ::open(srcClean.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (srcRootFd.v < 0) {
            return "source KST became unreadable or is a symlink before publish";
        }
        for (const auto& [srcRel, expectedSha] : st->plan.expectedSourceFileSha256) {
            std::string herr;
            const std::string nowSha = detail::hashRelFilePinned(srcRootFd.v, srcRel, herr);
            if (nowSha.empty()) {
                return "cannot re-hash source file " + srcRel + " before publish: " + herr;
            }
            if (nowSha != expectedSha) {
                return "source file " + srcRel + " changed after review/staging (SHA-256 mismatch)";
            }
        }

        // (2) If targetKst was provided, re-inspect targetLibraryKey unchanged before publish!
        if (req.targetKst.has_value()) {
            std::string dirErr;
            if (!plan_detail::isRealDirectoryNoFollow(*req.targetKst, dirErr)) {
                return "target KST became unreadable or symlink before publish: " + dirErr;
            }
            TargetInspectOptions tOpts;
            tOpts.keepGoing = req.keepGoing;
            const TargetOccupancyMap nowTarget = inspectTargetKstFolder(*req.targetKst, tOpts);
            if (nowTarget.cancelled) return "target KST re-verification cancelled before publish";
            if (!nowTarget.valid || !nowTarget.fullyInspectable) {
                return "target KST is no longer fully inspectable before publish";
            }
            if (nowTarget.targetLibraryKey != st->plan.targetLibraryKey) {
                return "target KST changed after review/staging (targetLibraryKey mismatch)";
            }
            if (!st->plan.expectedTargetSoundBankSha256.empty()) {
                const std::string tgtClean = media::graph::native_detail::stripTrailingSlashes(*req.targetKst);
                const media::detail::Fd tgtRootFd(
                    ::open(tgtClean.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
                if (tgtRootFd.v < 0) {
                    return "target KST became unreadable or symlink before publish";
                }
                for (const auto& [pcgRel, expectedSha] : st->plan.expectedTargetSoundBankSha256) {
                    std::string herr;
                    const std::string nowSha = detail::hashRelFilePinned(tgtRootFd.v, pcgRel, herr);
                    if (nowSha.empty() || nowSha != expectedSha) {
                        return "target Sound bank " + pcgRel + " changed after review/staging (SHA-256 mismatch)";
                    }
                }
            }
        }

        // (3) LAST: re-verify every staged file's descriptor identity & SHA-256 right before renameAtNoReplace.
        // Do NOT set outSummary->published = true here: publication succeeds only after renameAtNoReplace reaches Phase::Done!
        const std::string recheckErr = library::recheckPinned(*st->pinned, out);
        if (!recheckErr.empty()) return recheckErr;
        return "";
    };

    s.finished = [st]() {
        st->expectedNonMidiLeafSha.clear();
        st->expectedPostSelects.clear();
    };

    s.conversion = [st]() {
        std::vector<std::pair<std::string, std::string>> c;
        for (const auto& obj : st->plan.plannedObjects) {
            c.emplace_back(
                "EXACT",
                obj.sourceRelPath + " -> " + obj.destRelPath +
                    (st->plan.midiRemapRules.empty() ? " (byte-identical Root tree)" : " (SMF Bank/PC remapped, XML preserved)"));
        }
        return c;
    };

    return s;
}

// Deterministic, path-free JSON serializer for a published RootTreeSubKst export.
inline std::string serializeSubKstExportJson(const SubKstExportPlan& plan,
                                             const SubKstExecutionSummary& exec,
                                             const JobState& job) {
    using namespace media::selective;
    auto q = [](const std::string& s) { return "\"" + diag_detail::safeJsonEscape(s) + "\""; };
    auto bstr = [](bool b) -> const char* { return b ? "true" : "false"; };
    auto levelName = [](Check c) -> const char* {
        return c == Check::Passed ? "passed" : c == Check::Failed ? "failed" : "not-run";
    };

    std::string o;
    o.reserve(4096 + exec.exportedFiles.size() * 256 + plan.soundRemaps.size() * 192);
    o += "{\n";
    o += "  \"schema\": " + q(kSubKstExportSchema) + ",\n";
    o += "  \"label\": " + q(kHardwareUnverifiedBanner) + ",\n";
    o += "  \"hardwareVerified\": false,\n";
    o += "  \"deleteProofAvailable\": false,\n";
    o += "  \"published\": " + std::string(bstr(job.phase == Phase::Done)) + ",\n";
    o += "  \"mode\": " + q(exportModeName(plan.planResult.report.mode)) + ",\n";
    o += "  \"conflictPolicy\": " + q(conflictPolicyName(plan.planResult.report.conflictPolicy)) + ",\n";
    o += "  \"planFingerprint\": " + q(plan.planFingerprint) + ",\n";
    o += "  \"targetLibraryKey\": " + q(plan.targetLibraryKey) + ",\n";
    o += "  \"outputName\": " + q(exec.outputLeafName) + ",\n";
    o += "  \"verification\": {\n";
    o += "    \"L1\": " + q(levelName(job.level.l1.state)) + ",\n";
    o += "    \"L2\": " + q(levelName(job.level.l2.state)) + ",\n";
    o += "    \"L3\": " + q(levelName(job.level.l3.state)) + "\n";
    o += "  },\n";
    o += "  \"summary\": {\n";
    o += "    \"exportedFiles\": " + std::to_string(exec.exportedFiles.size()) + ",\n";
    o += "    \"factoryRefs\": " + std::to_string(exec.factoryRefs) + ",\n";
    o += "    \"targetMatchedUserRefs\": " + std::to_string(exec.targetMatchedUserRefs) + ",\n";
    o += "    \"soundRemaps\": " + std::to_string(plan.soundRemaps.size()) + ",\n";
    o += "    \"validatorWarnings\": " + std::to_string(exec.validatorWarnings) + ",\n";
    o += "    \"forbiddenNativeDirsPresent\": false\n";
    o += "  },\n";

    o += "  \"exportedFiles\": [";
    for (size_t i = 0; i < exec.exportedFiles.size(); ++i) {
        const auto& f = exec.exportedFiles[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"sourceRelPath\": " + q(f.sourceRelPath) +
             ", \"destRelPath\": " + q(f.destRelPath) +
             ", \"bytes\": " + std::to_string(f.bytes) +
             ", \"sha256\": " + q(f.sha256) +
             ", \"remapped\": " + std::string(bstr(f.remapped)) +
             ", \"remappedEvents\": " + std::to_string(f.remappedEvents) + "}";
    }
    o += exec.exportedFiles.empty() ? "],\n" : "\n  ],\n";

    o += "  \"soundRemaps\": [";
    for (size_t i = 0; i < plan.soundRemaps.size(); ++i) {
        const auto& rm = plan.soundRemaps[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"sourceSoundId\": " + q(rm.sourceSoundId.str()) +
             ", \"targetSoundId\": " + q(rm.targetSoundId.str()) +
             ", \"sourceSlot\": " + q(rm.sourceAddress.shortLabel()) +
             ", \"targetSlot\": " + q(rm.targetAddress.shortLabel()) +
             ", \"srcMidi\": [" + std::to_string(rm.srcTriple.msb) + ", " +
                 std::to_string(rm.srcTriple.lsb) + ", " + std::to_string(rm.srcTriple.pc) + "]" +
             ", \"dstMidi\": [" + std::to_string(rm.dstTriple.msb) + ", " +
                 std::to_string(rm.dstTriple.lsb) + ", " + std::to_string(rm.dstTriple.pc) + "]}";
    }
    o += plan.soundRemaps.empty() ? "],\n" : "\n  ],\n";

    o += "  \"allowedTargetMatchedUserSlots\": [";
    {
        size_t i = 0;
        for (const auto& s : plan.allowedTargetMatchedUserSlots) {
            if (i++) o += ", ";
            o += q(s);
        }
    }
    o += "],\n";

    o += "  \"warnings\": [";
    for (size_t i = 0; i < exec.warnings.size(); ++i) {
        if (i) o += ", ";
        o += q(exec.warnings[i]);
    }
    o += "],\n";

    o += "  \"diagnostics\": [";
    for (size_t i = 0; i < plan.planResult.report.diagnostics.size(); ++i) {
        const auto& d = plan.planResult.report.diagnostics[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"category\": " + q(diagnosticCategoryName(d.category)) +
             ", \"code\": " + q(d.code) +
             ", \"severity\": " + q(diagnosticSeverityName(d.severity)) +
             ", \"subject\": " + q(d.subject) +
             ", \"summary\": " + q(d.summary) + "}";
    }
    o += plan.planResult.report.diagnostics.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

// Deterministic, path-free Markdown summary for a published RootTreeSubKst export.
inline std::string serializeSubKstExportMarkdown(const SubKstExportPlan& plan,
                                                 const SubKstExecutionSummary& exec,
                                                 const JobState& job) {
    using namespace media::selective;
    using collector_detail::mdCodeSpan;
    std::string o;
    o += "# Selective Root-Tree Sub-KST Export\n\n";
    o += "> **" + std::string(kHardwareUnverifiedBanner) + "** (`hardwareVerified: false`, `deleteProofAvailable: false`)\n\n";
    o += "- **Published:** `" + std::string(job.phase == Phase::Done ? "true" : "false") + "`\n";
    o += "- **Output Folder:** " + mdCodeSpan(exec.outputLeafName) + "\n";
    o += "- **Mode:** " + mdCodeSpan(exportModeName(plan.planResult.report.mode)) +
         " | **Policy:** " + mdCodeSpan(conflictPolicyName(plan.planResult.report.conflictPolicy)) + "\n";
    o += "- **Plan Fingerprint (SHA-256):** " + mdCodeSpan(plan.planFingerprint) + "\n";
    o += "- **Verification:** L1=" + std::string(job.level.l1.state == Check::Passed ? "PASSED" : "FAILED") +
         ", L2=NOT RUN, L3=NOT RUN\n";
    o += "- **Exported Files:** " + std::to_string(exec.exportedFiles.size()) +
         " (ZERO `PCM/`, `SOUND/`, `MULTISMP/`, `SONGBOOK/` directories)\n";
    o += "- **Sound References:** " + std::to_string(exec.factoryRefs) + " Factory ROM, " +
         std::to_string(exec.targetMatchedUserRefs) + " Target-matched User Sound(s)\n\n";

    if (!plan.soundRemaps.empty()) {
        o += "## Sound Remaps Applied (SMF Bank/PC)\n\n";
        for (const auto& rm : plan.soundRemaps) {
            o += "- " + mdCodeSpan(rm.sourceAddress.shortLabel()) + " -> " +
                 mdCodeSpan(rm.targetAddress.shortLabel()) + "\n";
        }
        o += "\n";
    }

    if (!exec.warnings.empty()) {
        o += "## Warnings\n\n";
        for (const auto& w : exec.warnings) {
            o += "- " + media::panative::printable(w) + "\n";
        }
        o += "\n";
    }

    o += "## Exported Root-Tree Files\n\n";
    for (const auto& f : exec.exportedFiles) {
        o += "- " + mdCodeSpan(f.destRelPath) + " (" + mdCodeSpan(f.sha256) + ", " +
             std::to_string(f.bytes) + " bytes" +
             (f.remapped ? (", " + std::to_string(f.remappedEvents) + " MIDI event(s) remapped") : ", verbatim") +
             ")\n";
    }
    return o;
}

}  // namespace ai_arranger::jobs::subkst

#endif  // AI_ARRANGER_JOBS_SELECTIVE_SUB_KST_H
