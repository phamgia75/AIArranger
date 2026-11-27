#ifndef AI_ARRANGER_MEDIA_SOUND_SELECTIVE_COLLECTOR_H
#define AI_ARRANGER_MEDIA_SOUND_SELECTIVE_COLLECTOR_H

// Slice M2b: Automatic Selective Dependency Collector, 3-Tier Deduplicator, Honest Memory Projection,
// and Reproducible Path-Free Dry-Run Export Plan & Preview Serializer — unified cleanly on M2a.
//
// Single Source of Truth with M2a (`selective_diagnostics.h` + `slot_conflicts.h`):
//  * Reuses M2a's ExportMode, ConflictPolicy, SelectedKind, SelectionTarget, Diagnostic (4-category taxonomy +
//    diag_code::*), SlotConflict, CoverageSummary, MemoryProjectionReport, DeduplicationGroup,
//    SelectiveExportReport, EquivalenceVerdict, compareSoundEquivalence(), TargetOccupancyMap,
//    CollisionMatrixInput/Result, evaluateCollisionMatrix(), safeJsonEscape(), and canonicalPathFreeLabel().
//  * Strictly READ-ONLY: pure in-memory planning & dry-run preview; zero disk writes or mutations.
//  * HOLD GATES INTACT: zero OC31 encoder, zero Native Sound KST export, zero apps/macos/ wiring,
//    `hardwareVerified = false` ("STRUCTURALLY VALID — NOT VERIFIED ON A PA5X"), deleteCheck() never Unreferenced.
//  * Zero silent Sound loss: cross-slot Identical matches NEVER lift NATIVE_SOUND_WRITE_HOLD for RootTreeSubKst
//    without an explicit approved remap override (SMF_REMAP_REQUIRED); exact same-slot matches always win.
//  * Overflow-safe 64-bit byte math (satAdd/satMul), fail-closed failed/cancelled optionalTarget handling,
//    Markdown sanitization (printable + code-span backtick escaping), and <= 256-item cooperative cancellation.

#include "media/panative/panative_reader.h"
#include "media/sha256.h"
#include "media/sound/m1_manifest_adapter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ai_arranger::media::selective {

inline constexpr const char* kDryRunPlanSchema = "aiarranger-selective-dry-run-plan/1";
inline constexpr const char* kProvisionalUncompressedCaveat =
    "Uncompressed PCM software estimate (hardware validation pending; per-sample overhead and Preload buffer not modelled)";

// Collector-specific diagnostic codes extending M2a's canonical diag_code::* catalog
namespace collector_diag_code {
inline constexpr const char* kEmptySelection           = "EMPTY_SELECTION";
inline constexpr const char* kSourceUnavailable        = "SOURCE_UNAVAILABLE";
inline constexpr const char* kSelectedRootNotFound     = "SELECTED_ROOT_NOT_FOUND";
inline constexpr const char* kMalformedRootTree        = "MALFORMED_ROOT_TREE";
inline constexpr const char* kUnknownOriginMidiAddress = "UNKNOWN_ORIGIN_MIDI_ADDRESS";
inline constexpr const char* kKmpUnavailable           = "KMP_UNAVAILABLE";
inline constexpr const char* kSoundDecodeFailed        = "SOUND_DECODE_FAILED";
inline constexpr const char* kUnanalysedUserSound      = "UNANALYSED_USER_SOUND";
inline constexpr const char* kSmfRemapRequired         = "SMF_REMAP_REQUIRED";
}  // namespace collector_diag_code

struct CollectorRequest {
    std::vector<SelectionTarget> targets;          // M2a canonical SelectionTarget (order-independent)
    ExportMode mode = ExportMode::AdvisoryPlan;    // M2a canonical ExportMode
    ConflictPolicy conflictPolicy = ConflictPolicy::BlockWithReport; // M2a canonical ConflictPolicy
    bool includeEmbeddedKbdSets = true;            // Include Style embedded KBD Sets
    bool includeEmbeddedPads = true;               // Include Style embedded Pads
    bool requireCompleteSampleClosure = true;      // Fail-closed on unanalysed USERDK / OC31 KMP / missing PCM
    // Explicit approved cross-slot Sound remaps (source Sound ResourceId -> proven-identical Target Sound ResourceId).
    // Cross-slot Identical matches NEVER lift NATIVE_SOUND_WRITE_HOLD without an explicit entry here!
    std::map<graph::ResourceId, graph::ResourceId> approvedSoundRemaps;
    size_t cancelCheckStride = kDefaultCancelCheckStride; // Clamped to [1, 256]
};

enum class DedupTier {
    Tier1LogicalIdentity = 1,
    Tier2ProvisionalContent = 2,
    Tier3NativeEquivalence = 3
};

inline const char* dedupTierName(DedupTier t) {
    switch (t) {
        case DedupTier::Tier1LogicalIdentity:    return "tier1-logical-identity";
        case DedupTier::Tier2ProvisionalContent: return "tier2-provisional-content";
        case DedupTier::Tier3NativeEquivalence:  return "tier3-native-equivalence";
    }
    return "unknown";
}

struct DedupGroup {
    DedupTier tier = DedupTier::Tier1LogicalIdentity;
    std::string resourceKind;              // "Sound", "Multisample", "DrumSample", "Sample", "ProvisionalPackage", "RootFile"
    std::string keyType;                   // "resource-id" | "resource-id+kmp-id8" | "provisional-package-id" | "audio-fnv1a64+bytes" | "native-sound-equivalence" | "root-file-sha256"
    std::string keyValue;                  // canonical ResourceId/Id8/hash key (never name-only!)
    std::string canonicalMember;           // deterministic lexicographic representative
    std::vector<std::string> members;      // sorted referring roots or distinct resource IDs
    size_t occurrenceCount = 0;            // sharing/reference count
    uint64_t savedBytes = 0;               // logical or projected savings
    bool appliedInExport = false;          // true for Tier 1 & Tier 2a package dedup; false for advisory WAV/Native KST
    auto operator<=>(const DedupGroup&) const = default;

    DeduplicationGroup toM2aGroup() const {
        return {keyType, keyValue, canonicalMember, members, savedBytes, appliedInExport};
    }
};

struct NativeEquivalenceEntry {
    std::string scope;                     // "source-vs-target-sound" | "intra-source-sound" | "source-vs-target-root"
    std::string sourceSubject;             // ResourceId::str() or relPath
    std::string targetSubject;             // ResourceId::str() or relPath
    EquivalenceVerdict verdict = EquivalenceVerdict::InsufficientEvidence; // M2a canonical 3-way verdict
    std::string reason;
    bool mergesPa5xId8 = false;            // INVARIANT: ALWAYS false (obeys resource_graph.h)
    auto operator<=>(const NativeEquivalenceEntry&) const = default;
};

struct CollectedSoundSummary {
    graph::ResourceId id;
    std::string name;
    bool isDrumKit = false;
    bool analysed = false;
    std::string unanalysedGap;
    EdgeConfidence soundToMsConfidence = EdgeConfidence::Unsupported;
    EdgeConfidence msToSampleConfidence = EdgeConfidence::Unsupported;
    size_t referencingRootCount = 0;
    std::vector<std::string> referencingRoots;
    size_t multisampleCount = 0;
    size_t drumSampleCount = 0;
    size_t sampleCount = 0;
    uint64_t uniqueSampleBytes = 0;
    bool satisfiedByTarget = false;        // true ONLY on exact same-slot Identical OR explicitly approved cross-slot remap
    bool requiresRemap = false;            // true when proven Identical target Sound sits at a different slot (!= id)
    std::string matchedTargetSlot;         // populated when proven Identical in target (exact slot always preferred)
};

struct DryRunExportPlan {
    std::string schema = kDryRunPlanSchema;
    std::string banner = kHardwareUnverifiedBanner; // M2a canonical unverified banner
    bool hardwareVerified = false;                  // ALWAYS false
    bool ok = false;                                // true iff source (and target if given) loaded cleanly
    bool cancelled = false;                         // true = cooperatively cancelled; partial state dropped
    bool executable = false;                        // true iff ok && !cancelled && 0 blocking diags/conflicts && mode != HOLD
    bool coverageComplete = false;                  // false whenever any selected Sound/KMP/DK is unanalysed/incomplete
    bool deleteProofAvailable = false;              // ALWAYS false for Native-KST

    AdapterSourceKind sourceKind = AdapterSourceKind::NativeKst;
    ExportMode mode = ExportMode::AdvisoryPlan;
    ConflictPolicy conflictPolicy = ConflictPolicy::BlockWithReport;
    bool includeEmbeddedKbdSets = true;
    bool includeEmbeddedPads = true;
    bool requireCompleteSampleClosure = true;

    std::string planFingerprint;                    // 64-hex SHA-256 over canonical selection, closure, dedup, conflicts, hashes
    std::string sourceManifestSha256;
    std::string targetLibraryKey;                   // M2a TargetOccupancyMap::targetLibraryKey ("" if no target)
    bool targetPresent = false;

    std::vector<SelectionTarget> selectedTargets;   // canonical sorted unique (M2a SelectionTarget)
    std::vector<graph::ResourceId> closureNodes;    // sorted unique forward closure
    std::vector<CollectedSoundSummary> collectedSounds; // sorted by ResourceId
    std::vector<std::string> uniqueProvisionalPackages; // sorted unique packageIds
    std::vector<graph::ResourceId> uniqueMultisamples;  // sorted unique
    std::vector<graph::ResourceId> uniqueDrumSamples;   // sorted unique
    std::vector<graph::ResourceId> uniqueSamples;       // sorted unique
    std::vector<std::string> uniquePcmFiles;            // sorted unique KST-relative PCM paths

    size_t factorySoundSelectCount = 0;
    size_t userSoundSelectCount = 0;
    size_t unresolvedSlotsInAnalysedCount = 0;
    size_t unresolvedSlotsInUnanalysedCount = 0;

    CoverageSummary coverage;                       // M2a canonical UX Gate #1 coverage summary
    MemoryProjectionReport memoryProjection;        // M2a canonical §4.4 memory projection
    std::string memoryCaveat;                       // non-empty caveat string (stored-PCM proxy or software estimate)

    std::vector<DedupGroup> dedupGroups;            // canonically sorted across Tier 1, 2, 3
    std::vector<NativeEquivalenceEntry> nativeEquivalences; // 3-way Tier-3 classification records
    std::vector<SlotConflict> conflicts;            // M2a canonical SlotConflicts from evaluateCollisionMatrix()
    std::vector<Diagnostic> diagnostics;            // M2a canonical 4-category Diagnostics
    std::optional<CollisionMatrixResult> collisionMatrix; // full M2a CollisionMatrixResult when target supplied
    std::map<std::string, std::string> sourceFileSha256;  // relative path -> SHA-256 (zero local paths!)

    size_t blockingDiagnosticCount() const {
        return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& d) { return d.severity == DiagnosticSeverity::Blocking; }));
    }
    size_t blockingConflictCount() const {
        return static_cast<size_t>(std::count_if(conflicts.begin(), conflicts.end(),
            [](const SlotConflict& c) { return c.blocking; }));
    }
    bool hasBlockingCode(const std::string& code) const {
        for (const auto& d : diagnostics) {
            if (d.severity == DiagnosticSeverity::Blocking && d.code == code) return true;
        }
        return false;
    }
    bool hasDiagnosticCode(const std::string& code, std::optional<DiagnosticSeverity> sev = std::nullopt) const {
        for (const auto& d : diagnostics) {
            if (d.code == code && (!sev.has_value() || d.severity == *sev)) return true;
        }
        return false;
    }
};

namespace collector_detail {

inline std::string computePlanFingerprint(const DryRunExportPlan& p) {
    Sha256 h;
    adapter_detail::hashField(h, p.schema);
    adapter_detail::hashField(h, adapterSourceKindName(p.sourceKind));
    adapter_detail::hashField(h, exportModeName(p.mode));
    adapter_detail::hashField(h, conflictPolicyName(p.conflictPolicy));
    adapter_detail::hashField(h, p.includeEmbeddedKbdSets ? "1" : "0");
    adapter_detail::hashField(h, p.includeEmbeddedPads ? "1" : "0");
    adapter_detail::hashField(h, p.requireCompleteSampleClosure ? "1" : "0");
    adapter_detail::hashField(h, p.sourceManifestSha256);
    adapter_detail::hashField(h, p.targetLibraryKey);

    for (const auto& t : p.selectedTargets) {
        adapter_detail::hashField(h, "sel");
        adapter_detail::hashField(h, selectedKindName(t.kind));
        adapter_detail::hashField(h, t.relPath);
        adapter_detail::hashField(h, t.targetBank);
        adapter_detail::hashField(h, t.targetName);
    }
    for (const auto& id : p.closureNodes) {
        adapter_detail::hashField(h, "node");
        adapter_detail::hashField(h, id.str());
    }
    for (const auto& pkg : p.uniqueProvisionalPackages) {
        adapter_detail::hashField(h, "pkg");
        adapter_detail::hashField(h, pkg);
    }
    for (const auto& cs : p.collectedSounds) {
        adapter_detail::hashField(h, "cs");
        adapter_detail::hashField(h, cs.id.str());
        adapter_detail::hashField(h, cs.satisfiedByTarget ? "1" : "0");
        adapter_detail::hashField(h, cs.requiresRemap ? "1" : "0");
        adapter_detail::hashField(h, cs.matchedTargetSlot);
    }
    for (const auto& dg : p.dedupGroups) {
        adapter_detail::hashField(h, "dedup");
        adapter_detail::hashField(h, dedupTierName(dg.tier));
        adapter_detail::hashField(h, dg.resourceKind);
        adapter_detail::hashField(h, dg.keyType);
        adapter_detail::hashField(h, dg.keyValue);
        adapter_detail::hashField(h, dg.canonicalMember);
        adapter_detail::hashField(h, std::to_string(dg.occurrenceCount));
        adapter_detail::hashField(h, std::to_string(dg.savedBytes));
        adapter_detail::hashField(h, dg.appliedInExport ? "1" : "0");
        for (const auto& m : dg.members) adapter_detail::hashField(h, m);
    }
    for (const auto& ne : p.nativeEquivalences) {
        adapter_detail::hashField(h, "equiv");
        adapter_detail::hashField(h, ne.scope);
        adapter_detail::hashField(h, ne.sourceSubject);
        adapter_detail::hashField(h, ne.targetSubject);
        adapter_detail::hashField(h, equivalenceVerdictName(ne.verdict));
    }
    for (const auto& c : p.conflicts) {
        adapter_detail::hashField(h, "conf");
        adapter_detail::hashField(h, slotConflictKindName(c.kind));
        adapter_detail::hashField(h, c.address);
        adapter_detail::hashField(h, c.resolution);
        adapter_detail::hashField(h, c.blocking ? "1" : "0");
    }
    for (const auto& d : p.diagnostics) {
        adapter_detail::hashField(h, "diag");
        adapter_detail::hashField(h, diagnosticCategoryName(d.category));
        adapter_detail::hashField(h, d.code);
        adapter_detail::hashField(h, diagnosticSeverityName(d.severity));
        adapter_detail::hashField(h, d.subject);
    }
    for (const auto& [rel, sha] : p.sourceFileSha256) {
        adapter_detail::hashField(h, "file");
        adapter_detail::hashField(h, rel);
        adapter_detail::hashField(h, sha);
    }
    adapter_detail::hashField(h, "mem");
    adapter_detail::hashField(h, p.memoryProjection.unitName());
    adapter_detail::hashField(h, p.memoryProjection.fitVerdictName());
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.rawReferencedSampleBytes));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.uniqueLogicalSampleBytes));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.logicalDedupSavedBytes));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.identicalContentExtraBytes));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.uniqueSamplesCount));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.unknownSizeSamplesCount));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.targetBaselineSampleBytes));
    return h.hex();
}

// Escapes control bytes, newlines, backslashes (via panative::printable) and Markdown syntax characters
// so untrusted resource names, paths, or error strings cannot inject Markdown headings/lists/links/code.
inline std::string mdSanitizeText(const std::string& s) {
    const std::string p = panative::printable(s);
    std::string out;
    out.reserve(p.size() + 8);
    for (char c : p) {
        switch (c) {
            case '`': case '*': case '_': case '[': case ']':
            case '<': case '>': case '|': case '#':
                out += '\\';
                out += c;
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

// Inline Markdown code span: sanitizes control chars/newlines via printable() and replaces backticks
// with \x60 so untrusted input can never close or break out of the surrounding single-backtick span.
inline std::string mdCodeSpan(const std::string& s) {
    const std::string p = panative::printable(s);
    std::string out;
    out.reserve(p.size() + 8);
    out += '`';
    for (char c : p) {
        if (c == '`') out += "\\x60";
        else out += c;
    }
    out += '`';
    return out;
}

}  // namespace collector_detail

// Tier-3 Native KST Sound Equivalence Classifier delegating directly to M2a's single-source-of-truth
// `compareSoundEquivalence()` (`slot_conflicts.h`). Never matches by Sound name or filename!
inline NativeEquivalenceEntry classifyNativeSoundEquivalence(
    const M1AdapterView& srcView,
    const graph::ResourceId& srcSoundId,
    const M1AdapterView& dstView,
    const graph::ResourceId& dstSoundId,
    const std::string& scope = "source-vs-target-sound") {
    NativeEquivalenceEntry out;
    out.scope = scope;
    out.sourceSubject = srcSoundId.str();
    out.targetSubject = dstSoundId.str();
    out.mergesPa5xId8 = false;

    const auto sit = srcView.sounds.find(srcSoundId);
    const auto dit = dstView.sounds.find(dstSoundId);
    if (sit == srcView.sounds.end() || dit == dstView.sounds.end()) {
        out.verdict = EquivalenceVerdict::InsufficientEvidence;
        out.reason = "Sound absent in source or target view";
        return out;
    }
    const EquivalenceResult eq = compareSoundEquivalence(sit->second.equivalence, dit->second.equivalence);
    out.verdict = eq.verdict;
    out.reason = eq.reason;
    return out;
}

// Core M2b Read-Only Selective Collector, 3-Tier Deduplicator, M2a Collision Matrix Integrator & Dry-Run Planner.
// Accepts a pre-built M2a `TargetOccupancyMap` (plus optional target baseline bytes & target-is-native flag).
inline DryRunExportPlan collectSelectiveExportWithTargetMap(
    const M1AdapterView& source,
    const CollectorRequest& request,
    const TargetOccupancyMap* optionalTargetMap = nullptr,
    uint64_t optionalTargetBaselineBytes = 0,
    bool optionalTargetIsNativeKst = false,
    const KeepGoing& keepGoing = {}) {
    const size_t stride = adapter_detail::clampStride(request.cancelCheckStride);
    size_t tick = 0;
    auto makeCancelled = [&]() {
        DryRunExportPlan c;
        c.ok = false;
        c.cancelled = true;
        c.executable = false;
        c.hardwareVerified = false;
        c.deleteProofAvailable = false;
        c.sourceKind = source.sourceKind;
        c.mode = request.mode;
        c.conflictPolicy = request.conflictPolicy;
        return c;
    };
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

    if (source.cancelled) return makeCancelled();
    if (optionalTargetMap && optionalTargetMap->cancelled) return makeCancelled();

    DryRunExportPlan plan;
    plan.hardwareVerified = false;
    plan.deleteProofAvailable = false;
    plan.sourceKind = source.sourceKind;
    plan.mode = request.mode;
    plan.conflictPolicy = request.conflictPolicy;
    plan.includeEmbeddedKbdSets = request.includeEmbeddedKbdSets;
    plan.includeEmbeddedPads = request.includeEmbeddedPads;
    plan.requireCompleteSampleClosure = request.requireCompleteSampleClosure;
    plan.sourceManifestSha256 = source.manifestSha256;
    plan.coverage = source.coverage.toM2aCoverageSummary();
    if (optionalTargetMap) {
        plan.targetPresent = true;
        plan.targetLibraryKey = optionalTargetMap->targetLibraryKey;
    }

    // Aggregated M2a Diagnostics map (deterministic deduplication + merged affectedRoots)
    struct DiagAgg {
        DiagnosticCategory category = DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE;
        DiagnosticSeverity severity = DiagnosticSeverity::Warning;
        EvidenceLevel evidence = EvidenceLevel::Inferred;
        std::string referencedTarget;
        std::string summary;
        std::string remediation;
        std::set<std::string> roots;
    };
    using DiagKey = std::tuple<DiagnosticSeverity, DiagnosticCategory, std::string, std::string>; // (sev, cat, code, subject)
    std::map<DiagKey, DiagAgg> diagMap;
    auto addDiag = [&](DiagnosticCategory cat, const std::string& code, DiagnosticSeverity sev, EvidenceLevel ev,
                       const std::string& subject, const std::string& refTarget,
                       const std::string& summary, const std::string& remediation, const std::string& rootRel = "") {
        auto& agg = diagMap[{sev, cat, code, subject}];
        if (agg.summary.empty()) {
            agg.category = cat;
            agg.severity = sev;
            agg.evidence = ev;
            agg.referencedTarget = refTarget;
            agg.summary = summary;
            agg.remediation = remediation;
        }
        if (!rootRel.empty()) agg.roots.insert(rootRel);
    };

    if (!source.ok) {
        addDiag(DiagnosticCategory::VERIFIED_MISSING, collector_diag_code::kSourceUnavailable,
                DiagnosticSeverity::Blocking, EvidenceLevel::Verified, "<source>", "",
                "Source manifest view is unavailable or failed to load.",
                "Inspect source container errors before planning a selective export.");
    }

    // P2-1: Fail closed on invalid/uninspectable optionalTargetMap (never silent 0-byte baseline!)
    const bool targetValid = (!optionalTargetMap || optionalTargetMap->valid);
    if (optionalTargetMap && !optionalTargetMap->valid) {
        const std::string reason = optionalTargetMap->uninspectableReason.empty()
                                       ? std::string("Target manifest view failed to load or is uninspectable; refusing silent 0-byte baseline.")
                                       : optionalTargetMap->uninspectableReason;
        addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, diag_code::kTargetUninspectable,
                DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported, "<target>", "",
                reason, "Inspect target container errors before planning against this target.");
        plan.conflicts.push_back({
            SlotConflict::Kind::TargetUninspectable, "<target>", "", "", "BLOCKED", true, reason
        });
    }

    // 1. Canonicalize, sort, and deduplicate selection targets (order-independent!)
    for (const SelectionTarget& raw : request.targets) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        SelectionTarget norm = raw;
        norm.relPath = conflict_detail::normalizeRelPath(norm.relPath);
        if (const RootItemRecord* r = source.findRoot(norm.relPath)) {
            norm.kind = toSelectedKind(r->kind);
        }
        plan.selectedTargets.push_back(std::move(norm));
    }
    std::sort(plan.selectedTargets.begin(), plan.selectedTargets.end());
    plan.selectedTargets.erase(std::unique(plan.selectedTargets.begin(), plan.selectedTargets.end()),
                               plan.selectedTargets.end());

    if (plan.selectedTargets.empty()) {
        addDiag(DiagnosticCategory::VERIFIED_MISSING, collector_diag_code::kEmptySelection,
                DiagnosticSeverity::Blocking, EvidenceLevel::Verified, "<selection>", "",
                "No Styles, Keyboard Sets, or Pads were selected.",
                "Select at least one Style, Keyboard Set, or Pad to collect dependencies.");
    }

    if (request.mode == ExportMode::NativeSoundKst_HOLD) {
        addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, diag_code::kNativeSoundWriteHold,
                DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported, "<export-mode>", "",
                "Native Sound KST export (.PCG/.KMP/.PCM writing) is on HOLD.",
                "Use AdvisoryPlan (Pa5X single-Sound load, MAN5 p.736) or ImportKitBundle.");
    }

    // Closure & Tier-1/Tier-2/Tier-3 accumulators
    std::set<graph::ResourceId> closureNodeSet;
    std::map<graph::ResourceId, std::set<std::string>> soundRefRoots;
    std::map<graph::ResourceId, std::set<std::string>> msRefRoots;
    std::map<graph::ResourceId, std::set<graph::ResourceId>> msRefSounds;
    std::map<graph::ResourceId, std::set<std::string>> dsRefRoots;
    std::map<graph::ResourceId, std::set<graph::ResourceId>> dsRefSounds;
    std::map<graph::ResourceId, std::set<std::string>> sampleRefRoots;
    std::map<graph::ResourceId, size_t> sampleRawRefCount;
    std::map<graph::ResourceId, std::set<std::string>> unknownSampleRoots;
    std::set<std::string> pcmFileSet;
    std::map<std::string, std::set<std::string>> provPkgRefRoots;
    std::map<std::string, size_t> provPkgRefCount;

    bool selectedHasUnanalysedOrGap = false;
    uint64_t rawReferencedSampleBytes = 0;

    // 2. Pass 1 & 2: Expand every selected root in canonical sorted order
    for (const SelectionTarget& target : plan.selectedTargets) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const RootItemRecord* root = source.findRoot(target.relPath);
        if (!root) {
            addDiag(DiagnosticCategory::VERIFIED_MISSING, collector_diag_code::kSelectedRootNotFound,
                    DiagnosticSeverity::Blocking, EvidenceLevel::Verified, target.relPath, "",
                    "Selected item '" + target.relPath + "' was not found in the source manifest.",
                    "Verify the source-relative path and refresh the source manifest.", target.relPath);
            selectedHasUnanalysedOrGap = true;
            continue;
        }

        closureNodeSet.insert(root->id);
        if (!root->contentSha256.empty()) plan.sourceFileSha256[root->relPath] = root->contentSha256;

        if (root->malformedMidiLeafCount > 0) {
            addDiag(DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE, diag_code::kMalformedMidiLeaf,
                    DiagnosticSeverity::Blocking, EvidenceLevel::Verified, root->relPath, "",
                    "Root contains " + std::to_string(root->malformedMidiLeafCount) +
                        " malformed SMF 1.0 leaf/leaves (e.g. high-bit data byte >= 0x80).",
                    "Inspect or fix the malformed MIDI leaf; fail-closed to avoid silent corruption.", root->relPath);
            selectedHasUnanalysedOrGap = true;
        }
        if (!root->parseOk) {
            if (root->malformedMidiLeafCount == 0) {
                addDiag(DiagnosticCategory::VERIFIED_MISSING, collector_diag_code::kMalformedRootTree,
                        DiagnosticSeverity::Blocking, EvidenceLevel::Verified, root->relPath, "",
                        "Root container failed structural parsing: " + root->parseError,
                        "Inspect or repair the source file before exporting.", root->relPath);
            }
            selectedHasUnanalysedOrGap = true;
            continue;
        }

        std::set<graph::ResourceId> rootUserSounds;
        std::set<std::string> rootProvPkgs(root->directPackageIds.begin(), root->directPackageIds.end());

        for (const SoundSelectionRecord& sel : root->soundSelections) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

            if (root->kind == RootItemKind::Style) {
                const bool include =
                    sel.fromStyleMainTrack ||
                    (request.includeEmbeddedKbdSets && sel.fromEmbeddedKbdSet) ||
                    (request.includeEmbeddedPads && sel.fromEmbeddedPad);
                if (!include) continue;
            }

            if (!sel.provisionalPackageId.empty()) {
                rootProvPkgs.insert(sel.provisionalPackageId);
                ++plan.userSoundSelectCount;
                continue;
            }

            switch (sel.boundary) {
                case SoundBoundaryKind::FactoryRom: {
                    ++plan.factorySoundSelectCount;
                    addDiag(DiagnosticCategory::FACTORY_ASSUMED_EXTERNAL, diag_code::kFactoryRomSoundRef,
                            DiagnosticSeverity::Info, EvidenceLevel::Inferred,
                            std::to_string(sel.address.msb) + "/" + std::to_string(sel.address.lsb) + "/" +
                                std::to_string(sel.address.program),
                            "",
                            "External Pa5X Factory ROM Sound reference (no User Sound/Sample copied).",
                            "Assumes target Pa5X OS carries the same Factory ROM bank.",
                            root->relPath);
                    break;
                }
                case SoundBoundaryKind::UnknownOrUnanalysed: {
                    selectedHasUnanalysedOrGap = true;
                    if (sel.bankMissing) {
                        addDiag(DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE, diag_code::kUnresolvedMidiBankSelect,
                                DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported, root->relPath, "",
                                "Program Change " + std::to_string(sel.address.program) +
                                    " has unset CC0/CC32 bank select; target cannot be classified.",
                                "Add explicit CC0/CC32 Bank Select before Program Change or supply a Sound Map.",
                                root->relPath);
                    } else {
                        const std::string addrStr = std::to_string(sel.address.msb) + "/" +
                                                    std::to_string(sel.address.lsb) + "/" +
                                                    std::to_string(sel.address.program);
                        const DiagnosticSeverity sev =
                            (request.mode != ExportMode::AdvisoryPlan || request.requireCompleteSampleClosure)
                                ? DiagnosticSeverity::Blocking
                                : DiagnosticSeverity::Warning;
                        addDiag(DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                                collector_diag_code::kUnknownOriginMidiAddress, sev, EvidenceLevel::Unsupported,
                                addrStr, "",
                                "MIDI Bank/PC " + addrStr + " lies outside verified Pa5X User and Factory ranges.",
                                "Verify this address on Pa5X or remap explicitly; never silently coerced to Factory.",
                                root->relPath);
                    }
                    break;
                }
                case SoundBoundaryKind::AmbiguousOrOutOfSpec: {
                    ++plan.userSoundSelectCount;
                    const std::string subj = sel.targetSoundId ? sel.targetSoundId->str() : sel.userBank;
                    if (sel.resolved && sel.targetSoundId.has_value()) {
                        addDiag(DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE, diag_code::kOutOfSpecDrumkitBank,
                                DiagnosticSeverity::Warning, EvidenceLevel::Unsupported, subj, sel.userBank,
                                "Address MSB 120 LSB " + std::to_string(sel.address.lsb) +
                                    " is outside documented USERDK01..06 range but matched an existing bank file.",
                                "Verify target Drum Kit bank layout or remap track explicitly to USERDK01..USERDK06.",
                                root->relPath);
                        rootUserSounds.insert(*sel.targetSoundId);
                    } else {
                        selectedHasUnanalysedOrGap = true;
                        addDiag(DiagnosticCategory::VERIFIED_MISSING, diag_code::kMissingUserSoundBank,
                                DiagnosticSeverity::Blocking, EvidenceLevel::Verified, subj, sel.userBank + ".PCG",
                                "Out-of-spec User Drum Kit bank " + sel.userBank +
                                    " (MSB 120 LSB " + std::to_string(sel.address.lsb) + ") is absent.",
                                "Supply missing Drum Kit bank or remap track; never silently mapped to Factory.",
                                root->relPath);
                    }
                    break;
                }
                case SoundBoundaryKind::NativeUserSound:
                case SoundBoundaryKind::NativeUserDrumKit: {
                    ++plan.userSoundSelectCount;
                    if (!sel.targetSoundId.has_value()) break;
                    if (!sel.resolved) {
                        selectedHasUnanalysedOrGap = true;
                        const bool bankAbsent = (sel.missingReason == graph::MissingRefReason::SoundBankAbsent);
                        addDiag(DiagnosticCategory::VERIFIED_MISSING,
                                bankAbsent ? diag_code::kMissingUserSoundBank : diag_code::kMissingUserSoundSlot,
                                DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
                                sel.targetSoundId->str(), sel.userBank,
                                bankAbsent ? ("Referenced User Sound bank " + sel.userBank + " is absent in source.")
                                           : ("Referenced User Sound slot " + sel.targetSoundId->str() + " is empty in source."),
                                "Restore missing Sound bank/slot or remap track explicitly before export.",
                                root->relPath);
                    } else {
                        rootUserSounds.insert(*sel.targetSoundId);
                    }
                    break;
                }
            }
        }

        // Expand resolved Native User Sounds for this root
        for (const graph::ResourceId& sid : rootUserSounds) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
            closureNodeSet.insert(sid);
            soundRefRoots[sid].insert(root->relPath);

            const auto scIt = source.sounds.find(sid);
            if (scIt == source.sounds.end()) continue;
            const SoundClosureRecord& sc = scIt->second;

            if (!sc.sourceFile.empty()) {
                if (const auto fit = source.sourceFileSha256.find(sc.sourceFile); fit != source.sourceFileSha256.end()) {
                    plan.sourceFileSha256[sc.sourceFile] = fit->second;
                }
            }

            if (!sc.decoded) {
                selectedHasUnanalysedOrGap = true;
                addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, collector_diag_code::kSoundDecodeFailed,
                        DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported, sid.str(), sc.sourceFile,
                        "Sound " + sid.str() + " failed OC31/payload decoding.",
                        "Source Sound item is corrupt or unsupported; cannot trace dependencies.",
                        root->relPath);
                continue;
            }

            if (!source.coverage.kmpParsed) {
                selectedHasUnanalysedOrGap = true;
                const DiagnosticSeverity sev =
                    (request.requireCompleteSampleClosure || request.mode != ExportMode::AdvisoryPlan)
                        ? DiagnosticSeverity::Blocking
                        : DiagnosticSeverity::Warning;
                if (source.coverage.kmpIsOc31) {
                    addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, diag_code::kOc31KmpUnsupported,
                            sev, EvidenceLevel::Unsupported, "MULTISMP/RAM.KMP", "0x11050038",
                            kOc31KmpDegradedNotice,
                            "Use an authentic Pa5X raw RAM.KMP (0x11080018 v0x0103) or inspect Sound inventory only.",
                            root->relPath);
                } else {
                    addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, collector_diag_code::kKmpUnavailable,
                            sev, EvidenceLevel::Unsupported, "MULTISMP/RAM.KMP", "",
                            "RAM.KMP is absent or unparsed: " + source.coverage.kmpError,
                            "Ensure MULTISMP/RAM.KMP is present and valid.",
                            root->relPath);
                }
                continue;
            }

            // Explicit separation of UnresolvedSlot::inUnanalysedSound == false vs true (UX Gate #1)
            for (const auto& u : sc.unresolvedSlotsInAnalysed) {
                addDiag(DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE, diag_code::kUnresolvedSoundSlotAnalysed,
                        DiagnosticSeverity::Warning, EvidenceLevel::Inferred,
                        u.sound.str() + ":rec" + std::to_string(u.record) + ":slot" + std::to_string(u.slot), "",
                        "Non-empty, non-RAM 8-byte ID in analysed Sound (possible ROM multisample or dangling ID).",
                        "Verify audibly on Pa5X; Sound is excluded from automatic equivalence reuse.",
                        root->relPath);
            }
            for (const auto& u : sc.unresolvedSlotsInUnanalysed) {
                addDiag(DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE, diag_code::kUnresolvedSoundSlotSpeculative,
                        DiagnosticSeverity::Info, EvidenceLevel::Unsupported,
                        u.sound.str() + ":rec" + std::to_string(u.record) + ":slot" + std::to_string(u.slot), "",
                        "Speculative slot in unanalysed Sound (layout may not apply).",
                        "Hidden from blocking counters; inspect in expert details only.",
                        root->relPath);
            }

            if (!sc.analysed) {
                selectedHasUnanalysedOrGap = true;
                if (sc.isDrumKit) {
                    const DiagnosticSeverity sev =
                        (request.requireCompleteSampleClosure || request.mode != ExportMode::AdvisoryPlan)
                            ? DiagnosticSeverity::Blocking
                            : DiagnosticSeverity::Warning;
                    addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, diag_code::kDrumkitBodyUnparsed,
                            sev, EvidenceLevel::Unsupported, sid.str(), "",
                            "User Drum Kit " + sid.str() + " internal DrumSample edges are not yet parsed (" + sc.unanalysedGap + ").",
                            "Sample closure for USERDK is incomplete; use Pa5X single-Sound load or TXPERF Import Kit.",
                            root->relPath);
                } else if (sc.unanalysedGap == "NoKnownReference") {
                    addDiag(DiagnosticCategory::FACTORY_ASSUMED_EXTERNAL, diag_code::kSoundNoRamRefInferredRom,
                            DiagnosticSeverity::Info, EvidenceLevel::Inferred, sid.str(), "",
                            "Melodic User Sound fits layout with 0 RAM IDs (consistent with ROM-only multisamples; INFERRED).",
                            "Not hardware-verified; verify on Pa5X.",
                            root->relPath);
                } else {
                    const DiagnosticSeverity sev =
                        (request.requireCompleteSampleClosure || request.mode != ExportMode::AdvisoryPlan)
                            ? DiagnosticSeverity::Blocking
                            : DiagnosticSeverity::Warning;
                    addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, collector_diag_code::kUnanalysedUserSound,
                            sev, EvidenceLevel::Unsupported, sid.str(), "",
                            "User Sound " + sid.str() + " could not be analysed (" + sc.unanalysedGap + ").",
                            "Cannot prove complete Multisample/Sample dependencies offline.",
                            root->relPath);
                }
                continue;
            }

            // Trace analysed downstream closure: Sound -> Oscillator -> Multisample -> Zone -> Sample -> PCM
            // and USERDK -> DrumSample -> Sample -> PCM
            for (const auto& oscId : sc.oscillatorIds) closureNodeSet.insert(oscId);
            for (const auto& msId : sc.multisampleIds) {
                closureNodeSet.insert(msId);
                msRefRoots[msId].insert(root->relPath);
                msRefSounds[msId].insert(sid);
            }
            for (const auto& dsId : sc.drumSampleIds) {
                closureNodeSet.insert(dsId);
                dsRefRoots[dsId].insert(root->relPath);
                dsRefSounds[dsId].insert(sid);
            }
            for (const auto& smpId : sc.sampleIds) {
                if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
                closureNodeSet.insert(smpId);
                sampleRefRoots[smpId].insert(root->relPath);
                ++sampleRawRefCount[smpId];
                const auto spIt = source.samples.find(smpId);
                if (spIt == source.samples.end() || !spIt->second.hasPcm) {
                    unknownSampleRoots[smpId].insert(root->relPath);
                    selectedHasUnanalysedOrGap = true;
                    addDiag(DiagnosticCategory::VERIFIED_MISSING, diag_code::kMissingPcmForKmpSample,
                            DiagnosticSeverity::Blocking, EvidenceLevel::Verified, smpId.str(), "",
                            "Reachable KMP Sample " + smpId.str() + " has no matching PCM entry.",
                            "Restore missing PCM/*.pcm container before exporting.",
                            root->relPath);
                } else {
                    rawReferencedSampleBytes = adapter_detail::satAdd(rawReferencedSampleBytes, spIt->second.payloadBytes);
                    if (!spIt->second.pcmRelPath.empty()) pcmFileSet.insert(spIt->second.pcmRelPath);
                }
            }
        }

        // Expand provisional packages referenced by this root (Path A Provisional Workspace)
        for (const std::string& pkgId : rootProvPkgs) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
            const auto pit = source.packagesById.find(pkgId);
            if (pit == source.packagesById.end() || !pit->second.valid) {
                selectedHasUnanalysedOrGap = true;
                addDiag(DiagnosticCategory::VERIFIED_MISSING, diag_code::kMissingPackageWavOrTx,
                        DiagnosticSeverity::Blocking, EvidenceLevel::Verified, pkgId, "",
                        "Referenced provisional package '" + pkgId + "' is missing or failed validation" +
                            (pit != source.packagesById.end() && !pit->second.error.empty() ? (": " + pit->second.error) : "."),
                        "Regenerate or supply the missing Sound/DrumKit package in Sound Studio.",
                        root->relPath);
                continue;
            }
            const ProvisionalPackageRecord& pkg = pit->second;
            provPkgRefRoots[pkg.packageId].insert(root->relPath);
            ++provPkgRefCount[pkg.packageId];
            if (!pkg.packageSha256.empty()) {
                plan.sourceFileSha256[pkg.relDir.empty() ? pkg.packageId : (pkg.relDir + "/RESOURCES.tsv")] = pkg.packageSha256;
            }
            for (const auto& [nid, node] : pkg.graph.nodes()) closureNodeSet.insert(nid);
            if (!pkg.graph.dangling().empty()) {
                selectedHasUnanalysedOrGap = true;
                addDiag(DiagnosticCategory::VERIFIED_MISSING, diag_code::kDanglingGraphEdge,
                        DiagnosticSeverity::Blocking, EvidenceLevel::Verified, pkg.packageId, "",
                        "Provisional package '" + pkg.packageId + "' contains dangling graph references.",
                        "Regenerate package cleanly before export.",
                        root->relPath);
            }
            for (const graph::ResourceId& smpId : pkg.sampleIds) {
                if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
                sampleRefRoots[smpId].insert(root->relPath);
                ++sampleRawRefCount[smpId];
                const auto spIt = source.samples.find(smpId);
                if (spIt != source.samples.end() && spIt->second.hasPcm) {
                    rawReferencedSampleBytes = adapter_detail::satAdd(rawReferencedSampleBytes, spIt->second.payloadBytes);
                } else {
                    unknownSampleRoots[smpId].insert(root->relPath);
                    selectedHasUnanalysedOrGap = true;
                }
            }
        }
    }

    for (const auto& [rel, sha] : source.sourceFileSha256) {
        if (graph::native_detail::endsWithUpper(rel, ".KMP")) plan.sourceFileSha256[rel] = sha;
    }

    if (source.isNativeKst() && source.coverage.kmpIsOc31) {
        selectedHasUnanalysedOrGap = true;
        const DiagnosticSeverity sev =
            (request.requireCompleteSampleClosure || request.mode != ExportMode::AdvisoryPlan)
                ? DiagnosticSeverity::Blocking
                : DiagnosticSeverity::Warning;
        addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, diag_code::kOc31KmpUnsupported,
                sev, EvidenceLevel::Unsupported, "MULTISMP/RAM.KMP", "0x11050038",
                kOc31KmpDegradedNotice,
                "Use an authentic Pa5X raw RAM.KMP (0x11080018 v0x0103) or inspect Sound inventory only.");
    }

    if (source.isNativeKst() && !source.duplicateDrumIds.empty()) {
        selectedHasUnanalysedOrGap = true;
        addDiag(DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE, diag_code::kDuplicateKmpDrumId,
                DiagnosticSeverity::Warning, EvidenceLevel::Verified, "MULTISMP/RAM.KMP", "",
                std::to_string(source.duplicateDrumIds.size()) +
                    " duplicate 8-byte DrumSample ID group(s) in RAM.KMP (kept positional; delete safety unprovable).",
                "Nodes kept positional; coverageComplete is false; never deduce deletion safety.");
    }

    // 3. M2a Collision Matrix + Tier-3 Native KST Equivalence (single source of truth via M2a!)
    struct TargetSoundMatch {
        graph::ResourceId matchedTargetId;
        bool requiresRemap = false;
        bool satisfied = false; // true ONLY on exact same-slot match OR explicit approvedSoundRemaps override!
    };
    std::map<graph::ResourceId, TargetSoundMatch> targetMatchedSounds;

    if (optionalTargetMap && optionalTargetMap->valid) {
        CollisionMatrixInput cmIn;
        cmIn.policy = request.conflictPolicy;
        cmIn.mode = request.mode;

        // Build M2a ObjectPathCollisionRequests for every selected root
        for (const SelectionTarget& sel : plan.selectedTargets) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
            const RootItemRecord* r = source.findRoot(sel.relPath);
            if (!r) continue;
            ObjectPathCollisionRequest oreq;
            oreq.kind = sel.kind;
            oreq.sourceRelPath = r->relPath;
            oreq.targetBank = sel.targetBank;
            oreq.targetName = sel.targetName;
            oreq.fileSha256 = r->contentSha256;
            oreq.affectedRoots = {r->relPath};
            cmIn.objectRequests.push_back(std::move(oreq));
        }

        // Build M2a SoundCollisionRequests for every collected Native User Sound
        for (const auto& [sid, roots] : soundRefRoots) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
            const auto scIt = source.sounds.find(sid);
            if (scIt == source.sounds.end()) continue;
            const SoundClosureRecord& sc = scIt->second;
            const auto addr = sc.slotAddress.has_value() ? sc.slotAddress : SlotAddress::fromBankAndSlot(sc.bank, sc.slot);
            if (!addr.has_value()) continue;
            SoundCollisionRequest sreq;
            sreq.requestId = sid.str();
            sreq.sourceAddress = *addr;
            sreq.sourceLabel = sc.name;
            sreq.equivalence = sc.equivalence;
            sreq.expectExistingInTarget = false;
            sreq.affectedRoots.assign(roots.begin(), roots.end());
            cmIn.soundRequests.push_back(std::move(sreq));
        }

        // Run M2a's canonical Collision Matrix!
        CollisionMatrixResult cmRes = evaluateCollisionMatrix(*optionalTargetMap, std::move(cmIn));

        // Carry forward M2a's conflicts and diagnostics
        plan.conflicts.insert(plan.conflicts.end(), cmRes.conflicts.begin(), cmRes.conflicts.end());
        for (const auto& d : cmRes.diagnostics) {
            addDiag(d.category, d.code, d.severity, d.evidence, d.subject, d.referencedTarget,
                    d.summary, d.remediation);
            if (!d.affectedRoots.empty()) {
                auto& agg = diagMap[{d.severity, d.category, d.code, d.subject}];
                agg.roots.insert(d.affectedRoots.begin(), d.affectedRoots.end());
            }
        }

        // Record Tier-3 source-vs-target root equivalence from M2a's objectResolutions
        for (const auto& ores : cmRes.objectResolutions) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
            if (ores.action == "already-present-noop") {
                const RootItemRecord* sr = source.findRoot(ores.sourceRelPath);
                NativeEquivalenceEntry eq;
                eq.scope = "source-vs-target-root";
                eq.sourceSubject = ores.sourceRelPath;
                eq.targetSubject = ores.targetRelPath;
                eq.verdict = EquivalenceVerdict::Identical;
                eq.reason = "root file SHA-256 identical in target (M2a already-present-noop)";
                plan.nativeEquivalences.push_back(std::move(eq));
                if (sr && !sr->contentSha256.empty()) {
                    DedupGroup dg;
                    dg.tier = DedupTier::Tier3NativeEquivalence;
                    dg.resourceKind = "RootFile";
                    dg.keyType = "root-file-sha256";
                    dg.keyValue = sr->contentSha256;
                    dg.canonicalMember = ores.targetRelPath;
                    dg.members = {ores.sourceRelPath, ores.targetRelPath};
                    std::sort(dg.members.begin(), dg.members.end());
                    dg.occurrenceCount = 2;
                    dg.savedBytes = 0;
                    dg.appliedInExport = false;
                    plan.dedupGroups.push_back(std::move(dg));
                }
            } else if (ores.action == "blocked-collision") {
                plan.nativeEquivalences.push_back({
                    "source-vs-target-root", ores.sourceRelPath, ores.targetRelPath,
                    EquivalenceVerdict::Different, ores.detail, false
                });
            }
        }

        // Record Tier-3 source-vs-target Sound equivalence via M2a's TargetOccupancyMap + compareSoundEquivalence()
        // P1 invariant: ALWAYS check exact same-slot `sid` FIRST so exact same-slot strictly wins over cross-slot!
        if (source.isNativeKst() && optionalTargetMap->fullyInspectable) {
            for (const auto& [sid, roots] : soundRefRoots) {
                if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
                const auto scIt = source.sounds.find(sid);
                if (scIt == source.sounds.end()) continue;
                const SoundClosureRecord& sc = scIt->second;
                const auto srcAddr = sc.slotAddress.has_value() ? sc.slotAddress : SlotAddress::fromBankAndSlot(sc.bank, sc.slot);
                if (!srcAddr.has_value()) continue;

                std::optional<graph::ResourceId> exactMatch;
                std::optional<graph::ResourceId> approvedCrossMatch;
                std::optional<graph::ResourceId> firstCrossMatch;

                const auto approvedIt = request.approvedSoundRemaps.find(sid);

                // Step A: Exact same-slot check FIRST!
                if (const OccupiedSlotEntry* sameOcc = optionalTargetMap->findOccupied(*srcAddr)) {
                    const EquivalenceResult eq = compareSoundEquivalence(sc.equivalence, sameOcc->equivalence);
                    plan.nativeEquivalences.push_back({
                        "source-vs-target-sound", sid.str(), sameOcc->address.resourceId().str(),
                        eq.verdict, eq.reason, false
                    });
                    if (eq.verdict == EquivalenceVerdict::Identical) {
                        exactMatch = sid;
                    }
                }

                // Step B: Cross-slot candidates via M2a's canonical equivalenceIndex (plus explicit approved remap target)
                std::set<SlotAddress> crossAddrs;
                if (sc.equivalence.provablyComparable() && !sc.equivalence.equivalenceDigest.empty()) {
                    if (const auto eit = optionalTargetMap->equivalenceIndex.find(sc.equivalence.equivalenceDigest);
                        eit != optionalTargetMap->equivalenceIndex.end()) {
                        for (const SlotAddress& a : eit->second) {
                            if (a != *srcAddr && a.isDrumKit == srcAddr->isDrumKit) crossAddrs.insert(a);
                        }
                    }
                }
                if (approvedIt != request.approvedSoundRemaps.end()) {
                    if (const auto appAddr = SlotAddress::fromBankAndSlot(approvedIt->second.scope, approvedIt->second.number);
                        appAddr.has_value() && *appAddr != *srcAddr && optionalTargetMap->findOccupied(*appAddr)) {
                        crossAddrs.insert(*appAddr);
                    }
                }

                for (const SlotAddress& candAddr : crossAddrs) {
                    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
                    const OccupiedSlotEntry* occ = optionalTargetMap->findOccupied(candAddr);
                    if (!occ) continue;
                    const EquivalenceResult eq = compareSoundEquivalence(sc.equivalence, occ->equivalence);
                    const graph::ResourceId candId = candAddr.resourceId();
                    plan.nativeEquivalences.push_back({
                        "source-vs-target-sound", sid.str(), candId.str(), eq.verdict, eq.reason, false
                    });
                    if (eq.verdict == EquivalenceVerdict::Identical) {
                        if (approvedIt != request.approvedSoundRemaps.end() && approvedIt->second == candId) {
                            approvedCrossMatch = candId;
                        }
                        if (!firstCrossMatch.has_value()) firstCrossMatch = candId;
                    }
                }

                std::optional<graph::ResourceId> chosenMatch;
                bool requiresRemap = false;
                bool satisfied = false;
                if (exactMatch.has_value()) {
                    chosenMatch = *exactMatch;
                    requiresRemap = false;
                    satisfied = true;
                } else if (approvedCrossMatch.has_value()) {
                    chosenMatch = *approvedCrossMatch;
                    requiresRemap = true;
                    satisfied = true; // explicit approvedSoundRemaps override!
                } else if (firstCrossMatch.has_value()) {
                    chosenMatch = *firstCrossMatch;
                    requiresRemap = true;
                    satisfied = false; // cross-slot NEVER lifts HOLD without explicit remap!
                }

                if (chosenMatch.has_value()) {
                    targetMatchedSounds[sid] = {*chosenMatch, requiresRemap, satisfied};
                    DedupGroup dg;
                    dg.tier = DedupTier::Tier3NativeEquivalence;
                    dg.resourceKind = "Sound";
                    dg.keyType = "native-sound-equivalence";
                    dg.keyValue = sc.equivalence.equivalenceDigest;
                    dg.canonicalMember = chosenMatch->str();
                    dg.members = {sid.str(), chosenMatch->str()};
                    std::sort(dg.members.begin(), dg.members.end());
                    dg.occurrenceCount = 2;
                    dg.savedBytes = sc.storedPcmBytesProxy;
                    dg.appliedInExport = false;
                    plan.dedupGroups.push_back(std::move(dg));
                }
            }
        }

        plan.collisionMatrix = std::move(cmRes);
    }

    // 3b. Intra-source Sound equivalence check across collected Sounds sharing name OR decodedBodySha256
    //     using M2a's single-source-of-truth `compareSoundEquivalence()` (NEVER merges Pa5X identities!).
    if (source.isNativeKst()) {
        std::vector<graph::ResourceId> collectedSoundIds;
        collectedSoundIds.reserve(soundRefRoots.size());
        for (const auto& [sid, _] : soundRefRoots) collectedSoundIds.push_back(sid);
        std::map<std::string, std::vector<graph::ResourceId>> srcByNameOrSha;
        for (const auto& sid : collectedSoundIds) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
            const auto it = source.sounds.find(sid);
            if (it == source.sounds.end()) continue;
            if (!it->second.name.empty())
                srcByNameOrSha["name:" + graph::native_detail::upperAscii(it->second.name)].push_back(sid);
            if (!it->second.decodedBodySha256.empty())
                srcByNameOrSha["sha:" + it->second.decodedBodySha256].push_back(sid);
        }
        std::set<std::pair<graph::ResourceId, graph::ResourceId>> comparedPairs;
        for (const auto& [_, ids] : srcByNameOrSha) {
            if (ids.size() < 2) continue;
            for (size_t i = 0; i < ids.size(); ++i) {
                for (size_t j = i + 1; j < ids.size(); ++j) {
                    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
                    const auto pairKey = ids[i] < ids[j] ? std::make_pair(ids[i], ids[j]) : std::make_pair(ids[j], ids[i]);
                    if (!comparedPairs.insert(pairKey).second) continue;
                    NativeEquivalenceEntry eq = classifyNativeSoundEquivalence(
                        source, pairKey.first, source, pairKey.second, "intra-source-sound");
                    if (eq.verdict == EquivalenceVerdict::Identical) {
                        const auto& sc = source.sounds.at(pairKey.first);
                        DedupGroup dg;
                        dg.tier = DedupTier::Tier3NativeEquivalence;
                        dg.resourceKind = "Sound";
                        dg.keyType = "native-sound-equivalence";
                        dg.keyValue = sc.equivalence.equivalenceDigest;
                        dg.canonicalMember = pairKey.first.str();
                        dg.members = {pairKey.first.str(), pairKey.second.str()};
                        dg.occurrenceCount = 2;
                        dg.savedBytes = 0; // advisory only; distinct Pa5X slots/Id8s are NEVER merged
                        dg.appliedInExport = false;
                        plan.dedupGroups.push_back(std::move(dg));
                    }
                    plan.nativeEquivalences.push_back(std::move(eq));
                }
            }
        }

        // Enforce NATIVE_SOUND_WRITE_HOLD and P1 SMF_REMAP_REQUIRED for write modes:
        // Only exact same-slot Identical target matches (or explicitly approved remaps) satisfy target for melodic Sounds!
        // Track A write-HOLD invariant: USERDK Drum Kits remain gated by NATIVE_SOUND_WRITE_HOLD in write modes
        // (matching selective_sub_kst.h:625,698,722 — read-only USERDK analysis never lifts native write HOLD).
        if (request.mode == ExportMode::RootTreeSubKst || request.mode == ExportMode::ImportKitBundle) {
            for (const auto& [sid, roots] : soundRefRoots) {
                const auto scIt = source.sounds.find(sid);
                const bool isDk = (scIt != source.sounds.end() && scIt->second.isDrumKit) ||
                                  graph::closure_detail::isDrumKitBankName(sid.scope);
                if (isDk) {
                    for (const std::string& r : roots) {
                        addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, diag_code::kNativeSoundWriteHold,
                                DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported, sid.str(), "",
                                "Selected item requires Native User Drum Kit " + sid.str() +
                                    "; USERDK write/target-slot execution remains on HOLD (read-only USERDK analysis only).",
                                "Use AdvisoryPlan + Pa5X single-Sound load (MAN5 p.736) or replace via Import Kit.",
                                r);
                    }
                    continue;
                }
                const auto tmit = targetMatchedSounds.find(sid);
                if (tmit != targetMatchedSounds.end() && tmit->second.satisfied) continue;
                if (tmit != targetMatchedSounds.end() && tmit->second.requiresRemap) {
                    for (const std::string& r : roots) {
                        addDiag(DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE, collector_diag_code::kSmfRemapRequired,
                                DiagnosticSeverity::Blocking, EvidenceLevel::Verified, sid.str(),
                                tmit->second.matchedTargetId.str(),
                                "Selected item references " + sid.str() +
                                    " which matches identical Target Sound at different slot " +
                                    tmit->second.matchedTargetId.str() + "; SMF Bank/PC remap is required before RootTreeSubKst export.",
                                "Approve an explicit SMF Bank/PC remap to " + tmit->second.matchedTargetId.str() +
                                    " or use AdvisoryPlan.",
                                r);
                    }
                } else {
                    for (const std::string& r : roots) {
                        addDiag(DiagnosticCategory::UNSUPPORTED_FORMAT, diag_code::kNativeSoundWriteHold,
                                DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported, sid.str(), "",
                                "Selected item requires Native User Sound " + sid.str() +
                                    " which is not proven equivalent at the same slot in Target KST; native .PCG/.KMP/.PCM writing is on HOLD.",
                                "Use AdvisoryPlan + Pa5X single-Sound load (MAN5 p.736), remap to a Factory/Target Sound, or replace via Import Kit.",
                                r);
                    }
                }
            }
        }
    }

    // 4. Build CollectedSoundSummary + Tier-1 Logical Identity Dedup Groups (Sounds, Multisamples, DrumSamples, Samples)
    for (const auto& [sid, roots] : soundRefRoots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const auto scIt = source.sounds.find(sid);
        if (scIt == source.sounds.end()) continue;
        const SoundClosureRecord& sc = scIt->second;

        CollectedSoundSummary css;
        css.id = sid;
        css.name = sc.name;
        css.isDrumKit = sc.isDrumKit;
        css.analysed = sc.analysed;
        css.unanalysedGap = sc.unanalysedGap;
        css.soundToMsConfidence = sc.soundToMsConfidence;
        css.msToSampleConfidence = sc.msToSampleConfidence;
        css.referencingRootCount = roots.size();
        css.referencingRoots.assign(roots.begin(), roots.end());
        css.multisampleCount = sc.multisampleIds.size();
        css.drumSampleCount = sc.drumSampleIds.size();
        css.sampleCount = sc.sampleIds.size();
        css.uniqueSampleBytes = sc.storedPcmBytesProxy;
        if (const auto tmit = targetMatchedSounds.find(sid); tmit != targetMatchedSounds.end()) {
            css.satisfiedByTarget = tmit->second.satisfied;
            css.requiresRemap = tmit->second.requiresRemap;
            css.matchedTargetSlot = tmit->second.matchedTargetId.str();
        }
        plan.unresolvedSlotsInAnalysedCount += sc.unresolvedSlotsInAnalysed.size();
        plan.unresolvedSlotsInUnanalysedCount += sc.unresolvedSlotsInUnanalysed.size();
        plan.collectedSounds.push_back(std::move(css));

        if (roots.size() > 1) {
            DedupGroup dg;
            dg.tier = DedupTier::Tier1LogicalIdentity;
            dg.resourceKind = "Sound";
            dg.keyType = "resource-id";
            dg.keyValue = sid.str();
            dg.canonicalMember = sid.str();
            dg.members.assign(roots.begin(), roots.end());
            dg.occurrenceCount = roots.size();
            dg.savedBytes = adapter_detail::satMul(static_cast<uint64_t>(roots.size() - 1), sc.storedPcmBytesProxy);
            dg.appliedInExport = true;
            plan.dedupGroups.push_back(std::move(dg));
        }
    }

    for (const auto& [msId, roots] : msRefRoots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        plan.uniqueMultisamples.push_back(msId);
        const auto& sounds = msRefSounds[msId];
        const size_t shareCount = std::max(roots.size(), sounds.size());
        if (shareCount > 1) {
            DedupGroup dg;
            dg.tier = DedupTier::Tier1LogicalIdentity;
            dg.resourceKind = "Multisample";
            const auto mit = source.multisamples.find(msId);
            if (mit != source.multisamples.end()) {
                dg.keyType = "resource-id+kmp-id8";
                dg.keyValue = msId.str() + "|id8:" + graph::native_detail::hexId(mit->second.id8);
            } else {
                dg.keyType = "resource-id";
                dg.keyValue = msId.str();
            }
            dg.canonicalMember = msId.str();
            dg.members.assign(roots.begin(), roots.end());
            dg.occurrenceCount = shareCount;
            dg.savedBytes = 0; // Sample-level Tier 1 carries exact byte accounting without double-counting
            dg.appliedInExport = true;
            plan.dedupGroups.push_back(std::move(dg));
        }
    }

    for (const auto& [dsId, roots] : dsRefRoots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        plan.uniqueDrumSamples.push_back(dsId);
        const auto& sounds = dsRefSounds[dsId];
        const size_t shareCount = std::max(roots.size(), sounds.size());
        if (shareCount > 1) {
            DedupGroup dg;
            dg.tier = DedupTier::Tier1LogicalIdentity;
            dg.resourceKind = "DrumSample";
            const auto dit = source.drumSamples.find(dsId);
            if (dit != source.drumSamples.end()) {
                dg.keyType = "resource-id+kmp-id8";
                dg.keyValue = dsId.str() + "|id8:" + graph::native_detail::hexId(dit->second.id8);
            } else {
                dg.keyType = "resource-id";
                dg.keyValue = dsId.str();
            }
            dg.canonicalMember = dsId.str();
            dg.members.assign(roots.begin(), roots.end());
            dg.occurrenceCount = shareCount;
            dg.savedBytes = 0; // Sample-level Tier 1 carries exact byte accounting without double-counting
            dg.appliedInExport = true;
            plan.dedupGroups.push_back(std::move(dg));
        }
    }

    uint64_t uniqueLogicalSampleBytes = 0;
    std::map<std::string, std::vector<graph::ResourceId>> provisionalWavByContentHash;
    for (const auto& [smpId, rawCount] : sampleRawRefCount) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        plan.uniqueSamples.push_back(smpId);
        const auto spIt = source.samples.find(smpId);
        uint64_t sbytes = 0;
        if (spIt != source.samples.end() && spIt->second.hasPcm) {
            sbytes = spIt->second.payloadBytes;
            uniqueLogicalSampleBytes = adapter_detail::satAdd(uniqueLogicalSampleBytes, sbytes);
            if (smpId.provisional && spIt->second.info.has_value() && !spIt->second.info->contentHash.empty()) {
                const std::string hkey = spIt->second.info->contentHash + "|" + std::to_string(sbytes);
                provisionalWavByContentHash[hkey].push_back(smpId);
            }
        }
        if (rawCount > 1) {
            DedupGroup dg;
            dg.tier = DedupTier::Tier1LogicalIdentity;
            dg.resourceKind = "Sample";
            if (!smpId.provisional && spIt != source.samples.end()) {
                dg.keyType = "resource-id+kmp-id8";
                dg.keyValue = smpId.str() + "|id8:" + graph::native_detail::hexId(spIt->second.id8);
            } else {
                dg.keyType = "resource-id";
                dg.keyValue = smpId.str();
            }
            dg.canonicalMember = smpId.str();
            const auto& roots = sampleRefRoots[smpId];
            dg.members.assign(roots.begin(), roots.end());
            dg.occurrenceCount = rawCount;
            dg.savedBytes = adapter_detail::satMul(static_cast<uint64_t>(rawCount - 1), sbytes);
            dg.appliedInExport = true;
            plan.dedupGroups.push_back(std::move(dg));
        }
    }

    // 5. Tier 2a (Provisional Package Dedup, appliedInExport = true) & Tier 2b (WAV contentHash, advisory)
    for (const auto& [pkgId, roots] : provPkgRefRoots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        plan.uniqueProvisionalPackages.push_back(pkgId);
        const size_t rc = provPkgRefCount[pkgId];
        if (rc > 1) {
            const auto& pkg = source.packagesById.at(pkgId);
            DedupGroup dg;
            dg.tier = DedupTier::Tier2ProvisionalContent;
            dg.resourceKind = "ProvisionalPackage";
            dg.keyType = "provisional-package-id";
            dg.keyValue = pkgId + "|sha256:" + pkg.packageSha256;
            dg.canonicalMember = pkgId;
            dg.members.assign(roots.begin(), roots.end());
            dg.occurrenceCount = rc;
            dg.savedBytes = adapter_detail::satMul(static_cast<uint64_t>(rc - 1), pkg.totalUncompressedBytes);
            dg.appliedInExport = true;
            plan.dedupGroups.push_back(std::move(dg));
        }
    }

    uint64_t identicalContentExtraBytes = 0;
    for (const auto& [hkey, ids] : provisionalWavByContentHash) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        if (ids.size() <= 1) continue;
        std::vector<graph::ResourceId> sortedIds = ids;
        std::sort(sortedIds.begin(), sortedIds.end());
        const uint64_t oneBytes = source.samples.at(sortedIds.front()).payloadBytes;
        const uint64_t extra = adapter_detail::satMul(static_cast<uint64_t>(sortedIds.size() - 1), oneBytes);
        identicalContentExtraBytes = adapter_detail::satAdd(identicalContentExtraBytes, extra);

        DedupGroup dg;
        dg.tier = DedupTier::Tier2ProvisionalContent;
        dg.resourceKind = "Sample";
        dg.keyType = "audio-fnv1a64+bytes";
        dg.keyValue = hkey;
        dg.canonicalMember = sortedIds.front().str();
        for (const auto& id : sortedIds) dg.members.push_back(id.str());
        dg.occurrenceCount = sortedIds.size();
        dg.savedBytes = extra;
        dg.appliedInExport = false;
        plan.dedupGroups.push_back(std::move(dg));
    }

    // 6. Honest Memory & Sample RAM Projection (M2a MemoryProjectionReport + P2-3 saturating math)
    MemoryProjectionReport& mp = plan.memoryProjection;
    mp.rawReferencedSampleBytes = rawReferencedSampleBytes;
    mp.uniqueLogicalSampleBytes = uniqueLogicalSampleBytes;
    mp.logicalDedupSavedBytes = rawReferencedSampleBytes >= uniqueLogicalSampleBytes
                                    ? (rawReferencedSampleBytes - uniqueLogicalSampleBytes)
                                    : 0;
    mp.identicalContentExtraBytes = identicalContentExtraBytes;
    mp.uniqueSamplesCount = plan.uniqueSamples.size();
    mp.unknownSizeSamplesCount = unknownSampleRoots.size();
    mp.targetBaselineSampleBytes = targetValid ? optionalTargetBaselineBytes : 0;
    mp.projectedCombinedBytes = adapter_detail::satAdd(mp.targetBaselineSampleBytes, uniqueLogicalSampleBytes);

    const bool anyNativeProxy = source.isNativeKst() || optionalTargetIsNativeKst;
    mp.sampleBytesAreNativeStoredProxy = anyNativeProxy;
    if (mp.projectedCombinedBytes <= graph::kPa5xUserSampleBytesDecimal) mp.projectedFit = graph::Fit::Fits;
    else if (mp.projectedCombinedBytes <= graph::kPa5xUserSampleBytesBinary) mp.projectedFit = graph::Fit::Ambiguous;
    else if (mp.projectedCombinedBytes <= graph::kPa5xCompressedEquivalentDecimal) mp.projectedFit = graph::Fit::NeedsCompression;
    else mp.projectedFit = graph::Fit::Exceeds;
    plan.memoryCaveat = anyNativeProxy ? std::string(kStoredPcmProxyCaveat) : std::string(kProvisionalUncompressedCaveat);

    // 7. Finalize canonical sorted tables, fail-closed executable gate, and deterministic planFingerprint
    plan.closureNodes.assign(closureNodeSet.begin(), closureNodeSet.end());
    plan.uniquePcmFiles.assign(pcmFileSet.begin(), pcmFileSet.end());

    for (const auto& [key, agg] : diagMap) {
        plan.diagnostics.push_back({
            agg.category,
            std::get<2>(key),
            agg.severity,
            agg.evidence,
            std::get<3>(key),
            agg.referencedTarget,
            agg.summary,
            agg.remediation,
            std::vector<std::string>(agg.roots.begin(), agg.roots.end())
        });
    }
    canonicalizeDiagnostics(plan.diagnostics);
    canonicalizeConflicts(plan.conflicts);
    std::sort(plan.dedupGroups.begin(), plan.dedupGroups.end());
    plan.dedupGroups.erase(std::unique(plan.dedupGroups.begin(), plan.dedupGroups.end()), plan.dedupGroups.end());
    std::sort(plan.nativeEquivalences.begin(), plan.nativeEquivalences.end());
    plan.nativeEquivalences.erase(std::unique(plan.nativeEquivalences.begin(), plan.nativeEquivalences.end()),
                                  plan.nativeEquivalences.end());

    plan.ok = source.ok && targetValid;
    plan.coverageComplete =
        plan.ok && !selectedHasUnanalysedOrGap &&
        (source.isProvisional() ? source.coverage.coverageComplete
                                : (source.coverage.kmpParsed && !source.coverage.songBookPresent)) &&
        (!optionalTargetMap || optionalTargetMap->fullyInspectable);
    plan.coverage.coverageComplete = plan.coverageComplete;
    plan.coverage.deleteProofAvailable = false;

    plan.executable = plan.ok && !plan.cancelled &&
                      plan.blockingDiagnosticCount() == 0 &&
                      plan.blockingConflictCount() == 0 &&
                      request.mode != ExportMode::NativeSoundKst_HOLD;
    plan.planFingerprint = collector_detail::computePlanFingerprint(plan);
    return plan;
}

// Convenience overload accepting an `M1AdapterView*` target (builds M2a TargetOccupancyMap + saturating baseline).
inline DryRunExportPlan collectSelectiveExport(
    const M1AdapterView& source,
    const CollectorRequest& request,
    const M1AdapterView* optionalTarget = nullptr,
    const KeepGoing& keepGoing = {}) {
    const size_t stride = adapter_detail::clampStride(request.cancelCheckStride);
    size_t tick = 0;
    auto makeCancelled = [&]() {
        DryRunExportPlan c;
        c.ok = false;
        c.cancelled = true;
        c.executable = false;
        c.hardwareVerified = false;
        c.deleteProofAvailable = false;
        c.sourceKind = source.sourceKind;
        c.mode = request.mode;
        c.conflictPolicy = request.conflictPolicy;
        return c;
    };
    if (source.cancelled || (optionalTarget && optionalTarget->cancelled)) return makeCancelled();
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

    if (!optionalTarget) {
        return collectSelectiveExportWithTargetMap(source, request, nullptr, 0, false, keepGoing);
    }

    const TargetOccupancyMap targetMap = buildTargetOccupancyFromAdapter(*optionalTarget, keepGoing, stride);
    if (targetMap.cancelled) return makeCancelled();

    uint64_t targetBaselineBytes = 0;
    if (optionalTarget->ok) {
        for (const auto& [_, sr] : optionalTarget->samples) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
            if (sr.hasPcm) targetBaselineBytes = adapter_detail::satAdd(targetBaselineBytes, sr.payloadBytes);
        }
    }
    return collectSelectiveExportWithTargetMap(
        source, request, &targetMap, targetBaselineBytes, optionalTarget->isNativeKst(), keepGoing);
}

// ── Bridge to M2a's Canonical SelectiveExportReport (`aiarranger-selective-export-report/1`) ─────────────────────────
inline SelectiveExportReport toSelectiveExportReport(
    const DryRunExportPlan& p,
    const std::string& sourceLabel = "<source>",
    const std::string& targetLabel = "<target>") {
    SelectiveExportReport r;
    r.mode = p.mode;
    r.conflictPolicy = p.conflictPolicy;
    r.planFingerprint = p.planFingerprint;
    r.sourceKind = adapterSourceKindName(p.sourceKind);
    r.sourceLabel = diag_detail::canonicalPathFreeLabel(sourceLabel, "<source>");
    r.m1ManifestSha256 = p.sourceManifestSha256;
    r.targetPresent = p.targetPresent;
    r.targetLabel = p.targetPresent ? diag_detail::canonicalPathFreeLabel(targetLabel, "<target>") : "";
    r.targetLibraryKey = p.targetLibraryKey;
    r.selection = p.selectedTargets;
    r.soundsReferenced = p.factorySoundSelectCount + p.userSoundSelectCount;
    r.factorySounds = p.factorySoundSelectCount;
    r.userSounds = p.collectedSounds.size();
    r.multisamples = p.uniqueMultisamples.size();
    r.samples = p.uniqueSamples.size();
    r.coverage = p.coverage;
    r.memoryProjection = p.memoryProjection;
    r.deduplication.reserve(p.dedupGroups.size());
    for (const auto& dg : p.dedupGroups) r.deduplication.push_back(dg.toM2aGroup());
    r.conflicts = p.conflicts;
    r.diagnostics = p.diagnostics;
    return canonicalizeReport(std::move(r));
}

// ── Reproducible, Path-Free JSON Dry-Run Preview Serializer (reusing M2a diag_detail::safeJsonEscape) ────────────────
inline std::string serializeDryRunPlanJson(const DryRunExportPlan& p) {
    auto q = [](const std::string& s) { return "\"" + diag_detail::safeJsonEscape(s) + "\""; };
    auto bstr = [](bool b) -> const char* { return b ? "true" : "false"; };
    auto writeStrVec = [&](std::string& o, const std::vector<std::string>& v) {
        o += "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ", "; o += q(v[i]); }
        o += "]";
    };
    auto writeIdVec = [&](std::string& o, const std::vector<graph::ResourceId>& v) {
        o += "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ", "; o += q(v[i].str()); }
        o += "]";
    };

    std::string o;
    o.reserve(4096 + p.dedupGroups.size() * 192 + p.diagnostics.size() * 224 + p.conflicts.size() * 192);
    o += "{\n";
    o += "  \"schema\": " + q(p.schema) + ",\n";
    o += "  \"label\": " + q(p.banner) + ",\n";
    o += "  \"hardwareVerified\": " + std::string(bstr(p.hardwareVerified)) + ",\n";
    o += "  \"deleteProofAvailable\": " + std::string(bstr(p.deleteProofAvailable)) + ",\n";
    o += "  \"ok\": " + std::string(bstr(p.ok)) + ",\n";
    o += "  \"cancelled\": " + std::string(bstr(p.cancelled)) + ",\n";
    o += "  \"executable\": " + std::string(bstr(p.executable)) + ",\n";
    o += "  \"coverageComplete\": " + std::string(bstr(p.coverageComplete)) + ",\n";
    o += "  \"sourceKind\": " + q(adapterSourceKindName(p.sourceKind)) + ",\n";
    o += "  \"mode\": " + q(exportModeName(p.mode)) + ",\n";
    o += "  \"conflictPolicy\": " + q(conflictPolicyName(p.conflictPolicy)) + ",\n";
    o += "  \"includeEmbeddedKbdSets\": " + std::string(bstr(p.includeEmbeddedKbdSets)) + ",\n";
    o += "  \"includeEmbeddedPads\": " + std::string(bstr(p.includeEmbeddedPads)) + ",\n";
    o += "  \"requireCompleteSampleClosure\": " + std::string(bstr(p.requireCompleteSampleClosure)) + ",\n";
    o += "  \"planFingerprint\": " + q(p.planFingerprint) + ",\n";
    o += "  \"sourceManifestSha256\": " + q(p.sourceManifestSha256) + ",\n";
    o += "  \"targetLibraryKey\": " + q(p.targetLibraryKey) + ",\n";

    o += "  \"selection\": [";
    for (size_t i = 0; i < p.selectedTargets.size(); ++i) {
        const auto& t = p.selectedTargets[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"kind\": " + q(selectedKindName(t.kind)) +
             ", \"relPath\": " + q(t.relPath) +
             ", \"targetBank\": " + q(t.targetBank) +
             ", \"targetName\": " + q(t.targetName) + "}";
    }
    o += p.selectedTargets.empty() ? "],\n" : "\n  ],\n";

    o += "  \"summary\": {\n";
    o += "    \"selectedItems\": " + std::to_string(p.selectedTargets.size()) + ",\n";
    o += "    \"closureNodes\": " + std::to_string(p.closureNodes.size()) + ",\n";
    o += "    \"factorySoundSelects\": " + std::to_string(p.factorySoundSelectCount) + ",\n";
    o += "    \"userSoundSelects\": " + std::to_string(p.userSoundSelectCount) + ",\n";
    o += "    \"uniqueUserSounds\": " + std::to_string(p.collectedSounds.size()) + ",\n";
    o += "    \"uniqueProvisionalPackages\": " + std::to_string(p.uniqueProvisionalPackages.size()) + ",\n";
    o += "    \"uniqueMultisamples\": " + std::to_string(p.uniqueMultisamples.size()) + ",\n";
    o += "    \"uniqueDrumSamples\": " + std::to_string(p.uniqueDrumSamples.size()) + ",\n";
    o += "    \"uniqueSamples\": " + std::to_string(p.uniqueSamples.size()) + ",\n";
    o += "    \"uniquePcmFiles\": " + std::to_string(p.uniquePcmFiles.size()) + ",\n";
    o += "    \"unresolvedSlotsInAnalysed\": " + std::to_string(p.unresolvedSlotsInAnalysedCount) + ",\n";
    o += "    \"unresolvedSlotsInUnanalysed\": " + std::to_string(p.unresolvedSlotsInUnanalysedCount) + ",\n";
    o += "    \"blockingDiagnostics\": " + std::to_string(p.blockingDiagnosticCount()) + ",\n";
    o += "    \"blockingConflicts\": " + std::to_string(p.blockingConflictCount()) + ",\n";
    o += "    \"totalDiagnostics\": " + std::to_string(p.diagnostics.size()) + "\n";
    o += "  },\n";

    const auto& mp = p.memoryProjection;
    o += "  \"memoryProjection\": {\n";
    o += "    \"unit\": " + q(mp.unitName()) + ",\n";
    o += "    \"hardwareVerified\": false,\n";
    o += "    \"sampleBytesAreNativeStoredProxy\": " + std::string(bstr(mp.sampleBytesAreNativeStoredProxy)) + ",\n";
    o += "    \"rawReferencedSampleBytes\": " + std::to_string(mp.rawReferencedSampleBytes) + ",\n";
    o += "    \"uniqueLogicalSampleBytes\": " + std::to_string(mp.uniqueLogicalSampleBytes) + ",\n";
    o += "    \"logicalDedupSavedBytes\": " + std::to_string(mp.logicalDedupSavedBytes) + ",\n";
    o += "    \"identicalContentExtraBytes\": " + std::to_string(mp.identicalContentExtraBytes) + ",\n";
    o += "    \"uniqueSamplesCount\": " + std::to_string(mp.uniqueSamplesCount) + ",\n";
    o += "    \"unknownSizeSamplesCount\": " + std::to_string(mp.unknownSizeSamplesCount) + ",\n";
    o += "    \"targetBaselineSampleBytes\": " + std::to_string(mp.targetBaselineSampleBytes) + ",\n";
    o += "    \"projectedCombinedBytes\": " + std::to_string(mp.projectedCombinedBytes) + ",\n";
    o += "    \"pa5xLimits\": {\n";
    o += "      \"decimal4GB\": " + std::to_string(graph::kPa5xUserSampleBytesDecimal) + ",\n";
    o += "      \"binary4GiB\": " + std::to_string(graph::kPa5xUserSampleBytesBinary) + ",\n";
    o += "      \"compressedEquivalent8GB\": " + std::to_string(graph::kPa5xCompressedEquivalentDecimal) + "\n";
    o += "    },\n";
    o += "    \"fitVerdict\": " + q(mp.fitVerdictName()) + ",\n";
    o += "    \"caveat\": " + q(p.memoryCaveat) + "\n";
    o += "  },\n";

    o += "  \"collectedSounds\": [";
    for (size_t i = 0; i < p.collectedSounds.size(); ++i) {
        const auto& cs = p.collectedSounds[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"id\": " + q(cs.id.str()) +
             ", \"name\": " + q(panative::printable(cs.name)) +
             ", \"isDrumKit\": " + std::string(bstr(cs.isDrumKit)) +
             ", \"analysed\": " + std::string(bstr(cs.analysed)) +
             ", \"unanalysedGap\": " + q(cs.unanalysedGap) +
             ", \"soundToMsConfidence\": " + q(edgeConfidenceName(cs.soundToMsConfidence)) +
             ", \"msToSampleConfidence\": " + q(edgeConfidenceName(cs.msToSampleConfidence)) +
             ", \"referencingRootCount\": " + std::to_string(cs.referencingRootCount) +
             ", \"multisampleCount\": " + std::to_string(cs.multisampleCount) +
             ", \"drumSampleCount\": " + std::to_string(cs.drumSampleCount) +
             ", \"sampleCount\": " + std::to_string(cs.sampleCount) +
             ", \"uniqueSampleBytes\": " + std::to_string(cs.uniqueSampleBytes) +
             ", \"satisfiedByTarget\": " + std::string(bstr(cs.satisfiedByTarget)) +
             ", \"requiresRemap\": " + std::string(bstr(cs.requiresRemap)) +
             ", \"matchedTargetSlot\": " + q(cs.matchedTargetSlot) + "}";
    }
    o += p.collectedSounds.empty() ? "],\n" : "\n  ],\n";

    o += "  \"uniqueProvisionalPackages\": "; writeStrVec(o, p.uniqueProvisionalPackages); o += ",\n";
    o += "  \"uniquePcmFiles\": "; writeStrVec(o, p.uniquePcmFiles); o += ",\n";
    o += "  \"closureNodes\": "; writeIdVec(o, p.closureNodes); o += ",\n";

    o += "  \"deduplication\": [";
    for (size_t i = 0; i < p.dedupGroups.size(); ++i) {
        const auto& dg = p.dedupGroups[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"tier\": " + q(dedupTierName(dg.tier)) +
             ", \"resourceKind\": " + q(dg.resourceKind) +
             ", \"keyType\": " + q(dg.keyType) +
             ", \"keyValue\": " + q(dg.keyValue) +
             ", \"canonicalMember\": " + q(dg.canonicalMember) +
             ", \"occurrenceCount\": " + std::to_string(dg.occurrenceCount) +
             ", \"savedBytes\": " + std::to_string(dg.savedBytes) +
             ", \"appliedInExport\": " + std::string(bstr(dg.appliedInExport)) +
             ", \"members\": ";
        writeStrVec(o, dg.members);
        o += "}";
    }
    o += p.dedupGroups.empty() ? "],\n" : "\n  ],\n";

    o += "  \"nativeEquivalence\": [";
    for (size_t i = 0; i < p.nativeEquivalences.size(); ++i) {
        const auto& ne = p.nativeEquivalences[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"scope\": " + q(ne.scope) +
             ", \"sourceSubject\": " + q(ne.sourceSubject) +
             ", \"targetSubject\": " + q(ne.targetSubject) +
             ", \"verdict\": " + q(equivalenceVerdictName(ne.verdict)) +
             ", \"reason\": " + q(ne.reason) +
             ", \"mergesPa5xId8\": " + std::string(bstr(ne.mergesPa5xId8)) + "}";
    }
    o += p.nativeEquivalences.empty() ? "],\n" : "\n  ],\n";

    o += "  \"conflicts\": [";
    for (size_t i = 0; i < p.conflicts.size(); ++i) {
        const auto& c = p.conflicts[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"kind\": " + q(slotConflictKindName(c.kind)) +
             ", \"address\": " + q(c.address) +
             ", \"sourceLabel\": " + q(c.sourceLabel) +
             ", \"targetLabel\": " + q(c.targetLabel) +
             ", \"resolution\": " + q(c.resolution) +
             ", \"blocking\": " + std::string(bstr(c.blocking)) +
             ", \"detail\": " + q(c.detail) + "}";
    }
    o += p.conflicts.empty() ? "],\n" : "\n  ],\n";

    o += "  \"diagnostics\": [";
    for (size_t i = 0; i < p.diagnostics.size(); ++i) {
        const auto& d = p.diagnostics[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"category\": " + q(diagnosticCategoryName(d.category)) +
             ", \"code\": " + q(d.code) +
             ", \"severity\": " + q(diagnosticSeverityName(d.severity)) +
             ", \"evidence\": " + q(evidenceLevelName(d.evidence)) +
             ", \"subject\": " + q(d.subject) +
             ", \"referencedTarget\": " + q(d.referencedTarget) +
             ", \"summary\": " + q(d.summary) +
             ", \"remediation\": " + q(d.remediation) +
             ", \"affectedRoots\": ";
        writeStrVec(o, d.affectedRoots);
        o += "}";
    }
    o += p.diagnostics.empty() ? "],\n" : "\n  ],\n";

    o += "  \"sourceFileSha256\": {";
    bool firstFile = true;
    for (const auto& [rel, sha] : p.sourceFileSha256) {
        o += std::string(firstFile ? "\n    " : ",\n    ") + q(rel) + ": " + q(sha);
        firstFile = false;
    }
    o += firstFile ? "}\n" : "\n  }\n";
    o += "}\n";
    return o;
}

// ── Reproducible, Path-Free, Injection-Safe Markdown Dry-Run Preview Serializer ──────────────────────────────────────
inline std::string serializeDryRunPlanMarkdown(const DryRunExportPlan& p) {
    using collector_detail::mdCodeSpan;
    using collector_detail::mdSanitizeText;
    std::string o;
    o.reserve(2048 + p.diagnostics.size() * 144 + p.conflicts.size() * 128 + p.dedupGroups.size() * 96);
    o += "# Selective Export Dry-Run Plan Preview\n\n";
    o += "> **" + (p.banner == kHardwareUnverifiedBanner ? p.banner : mdSanitizeText(p.banner)) +
         "** (`hardwareVerified: false`, `deleteProofAvailable: false`)\n\n";
    if (!p.coverageComplete) {
        o += "> **Coverage Notice:** " + std::string(kPartialCoverageNotice) + "\n\n";
    }
    o += "- **Schema:** " + mdCodeSpan(p.schema) + "\n";
    o += "- **Source Kind:** " + mdCodeSpan(adapterSourceKindName(p.sourceKind)) + "\n";
    o += "- **Export Mode:** " + mdCodeSpan(exportModeName(p.mode)) +
         " | **Conflict Policy:** " + mdCodeSpan(conflictPolicyName(p.conflictPolicy)) + "\n";
    o += "- **Executable:** `" + std::string(p.executable ? "true" : "false") + "`\n";
    o += "- **Plan Fingerprint (SHA-256):** " + mdCodeSpan(p.planFingerprint) + "\n";
    o += "- **Selected Items:** " + std::to_string(p.selectedTargets.size()) + "\n";
    o += "- **Closure Summary:** " + std::to_string(p.collectedSounds.size()) + " User Sound(s), " +
         std::to_string(p.uniqueProvisionalPackages.size()) + " Provisional Package(s), " +
         std::to_string(p.uniqueMultisamples.size()) + " Multisample(s), " +
         std::to_string(p.uniqueDrumSamples.size()) + " DrumSample(s), " +
         std::to_string(p.uniqueSamples.size()) + " Sample(s), " +
         std::to_string(p.uniquePcmFiles.size()) + " PCM file(s), " +
         std::to_string(p.factorySoundSelectCount) + " Factory ROM select(s)\n";
    o += "- **Unresolved Sound Slots:** " + std::to_string(p.unresolvedSlotsInAnalysedCount) +
         " in analysed Sound(s), " + std::to_string(p.unresolvedSlotsInUnanalysedCount) +
         " speculative in unanalysed Sound(s)\n\n";

    const auto& mp = p.memoryProjection;
    o += "## Memory & Sample RAM Projection\n\n";
    o += "- **Unit:** " + mdCodeSpan(mp.unitName()) + "\n";
    o += "- **Fit Verdict:** " + mdCodeSpan(mp.fitVerdictName()) + "\n";
    o += "- **Pre-Dedup Referenced Sample Bytes:** " + std::to_string(mp.rawReferencedSampleBytes) + "\n";
    o += "- **Unique Logical Sample Bytes:** " + std::to_string(mp.uniqueLogicalSampleBytes) + "\n";
    o += "- **Tier-1 Logical Dedup Saved Bytes:** " + std::to_string(mp.logicalDedupSavedBytes) + "\n";
    o += "- **Tier-2 Identical-Content Extra Bytes (Advisory):** " + std::to_string(mp.identicalContentExtraBytes) + "\n";
    o += "- **Unique Samples / Unknown-Size Samples:** " + std::to_string(mp.uniqueSamplesCount) + " / " +
         std::to_string(mp.unknownSizeSamplesCount) + "\n";
    o += "- **Target Baseline / Projected Combined Bytes:** " + std::to_string(mp.targetBaselineSampleBytes) +
         " / " + std::to_string(mp.projectedCombinedBytes) + "\n";
    o += "- **Caveat:** " + mdSanitizeText(p.memoryCaveat) + "\n\n";

    o += "## Blocking Issues (" + std::to_string(p.blockingDiagnosticCount()) + " diagnostics, " +
         std::to_string(p.blockingConflictCount()) + " conflicts)\n\n";
    size_t emittedBlock = 0;
    for (const auto& d : p.diagnostics) {
        if (d.severity != DiagnosticSeverity::Blocking) continue;
        ++emittedBlock;
        o += "- **[" + std::string(diagnosticCategoryName(d.category)) + "] " + mdCodeSpan(d.code) + "** (" +
             mdCodeSpan(d.subject) + "): " + mdSanitizeText(d.summary) + " — _Remediation:_ " +
             mdSanitizeText(d.remediation) + "\n";
    }
    if (emittedBlock == 0) o += "_None._\n";
    o += "\n";

    if (!p.conflicts.empty()) {
        o += "## Target Slot & Path Conflicts (" + std::to_string(p.conflicts.size()) + ")\n\n";
        for (const auto& c : p.conflicts) {
            o += "- " + mdCodeSpan(slotConflictKindName(c.kind)) + " | " + mdCodeSpan(c.address) +
                 " | resolution=" + mdCodeSpan(c.resolution) + " | blocking=" +
                 (c.blocking ? "true" : "false") + " (" + mdSanitizeText(c.detail) + ")\n";
        }
        o += "\n";
    }

    o += "## Deduplication Groups (" + std::to_string(p.dedupGroups.size()) + ")\n\n";
    if (p.dedupGroups.empty()) {
        o += "_None._\n\n";
    } else {
        for (const auto& dg : p.dedupGroups) {
            o += "- " + mdCodeSpan(dedupTierName(dg.tier)) + " | " + mdCodeSpan(dg.resourceKind) + " | " +
                 mdCodeSpan(dg.keyType) + " | canonical " + mdCodeSpan(dg.canonicalMember) +
                 " | refs=" + std::to_string(dg.occurrenceCount) +
                 " | savedBytes=" + std::to_string(dg.savedBytes) + " | applied=" +
                 (dg.appliedInExport ? "true" : "false") + "\n";
        }
        o += "\n";
    }

    if (!p.nativeEquivalences.empty()) {
        o += "## Tier-3 Native KST Equivalence (" + std::to_string(p.nativeEquivalences.size()) + ")\n\n";
        for (const auto& ne : p.nativeEquivalences) {
            o += "- " + mdCodeSpan(ne.scope) + " | " + mdCodeSpan(ne.sourceSubject) + " vs " +
                 mdCodeSpan(ne.targetSubject) + " -> **" + mdCodeSpan(equivalenceVerdictName(ne.verdict)) +
                 "** (" + mdSanitizeText(ne.reason) + ")\n";
        }
        o += "\n";
    }

    return o;
}

}  // namespace ai_arranger::media::selective

#endif  // AI_ARRANGER_MEDIA_SOUND_SELECTIVE_COLLECTOR_H
