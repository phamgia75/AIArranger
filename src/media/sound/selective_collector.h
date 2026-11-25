#ifndef AI_ARRANGER_MEDIA_SOUND_SELECTIVE_COLLECTOR_H
#define AI_ARRANGER_MEDIA_SOUND_SELECTIVE_COLLECTOR_H

// Slice M2b: Automatic Selective Dependency Collector, 3-Tier Deduplicator, Honest Memory Projection,
// and Reproducible Path-Free Dry-Run Export Plan & Preview Serializer.
//
// Design & Safety Invariants (M2_SELECTIVE_STYLE_EXPORT_DESIGN.md §2, §3, §4, §8 Slice M2b):
//  * Strictly READ-ONLY: pure in-memory planning & dry-run preview; zero disk writes or mutations.
//  * HOLD GATES INTACT: zero OC31 encoder, zero Native Sound KST export, zero apps/macos/ wiring,
//    `hardwareVerified = false` ("STRUCTURALLY VALID — NOT VERIFIED ON A PA5X").
//  * Zero silent Sound loss: NEVER drops a dependency or silently substitutes a Factory sound; fails closed
//    with typed blocking reasons (`executable == false`) on missing/unwritable/unanalysed/malformed dependencies.
//  * 3-Tier Deduplication (NEVER deduplicates by filename or Sound name alone!):
//      - Tier 1 (Logical Graph Identity): canonical ResourceId + 8-byte KMP Id8 across all selected roots.
//      - Tier 2 (Provisional Package & WAV Content Hash): packageId/PACKAGE.sha256 (appliedInExport=true)
//        and byte-identical WAVs across distinct packages by contentHash+bytes (appliedInExport=false, advisory).
//      - Tier 3 (Native KST Content Equivalence — 3-Way Classification): Identical (exact SHA-256 + Analysed +
//        0 unresolved analysed slots + full downstream MS/Zone/Sample Id8 & PCM size match) vs Different vs
//        InsufficientEvidence (NEVER merges distinct Pa5X Id8 identities!).
//  * Honest Memory Projection (§4.4): strictly separates "uncompressed_pcm_bytes" from
//    "native_kst_stored_pcm_proxy_bytes" (mandatory "unverified-proxy" verdict & caveat string).
//  * Reproducible & Path-Free: deterministic planFingerprint (SHA-256) and canonical JSON/Markdown previews
//    with zero local filesystem paths, byte-identical across repeated runs and shuffled selection orders.

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
inline constexpr const char* kStructuralOnlyBanner = "STRUCTURALLY VALID — NOT VERIFIED ON A PA5X";
inline constexpr const char* kNativeProxyCaveat =
    "Stored PCM size (compressed 0180 + uncompressed 0110/0210 mix; decoded Pa5X RAM use unverified)";
inline constexpr const char* kProvisionalUncompressedCaveat =
    "Uncompressed PCM software estimate (hardware validation pending; per-sample overhead and Preload buffer not modelled)";

enum class CollectorExportMode {
    AdvisoryPlan,        // Read-only dry-run plan & preview (zero native KST bytes written)
    ImportKitBundle,     // Path A: Pa5X Import Kit (provisional Sound/DrumKit/KMP packages + marker SMF)
    RootTreeSubKst,      // Path A CLI-only (L1): .stg/.ksc/.pdc only; blocks on any unwritable User Sound
    NativeSoundKst_HOLD  // Strictly refused fail-closed until OC31/KMP writer + S6 HOLD gates lift
};

inline const char* collectorExportModeName(CollectorExportMode m) {
    switch (m) {
        case CollectorExportMode::AdvisoryPlan:        return "advisory-plan";
        case CollectorExportMode::ImportKitBundle:     return "import-kit";
        case CollectorExportMode::RootTreeSubKst:      return "root-tree-sub-kst";
        case CollectorExportMode::NativeSoundKst_HOLD: return "native-sound-kst-hold";
    }
    return "advisory-plan";
}

struct CollectorSelectionTarget {
    std::string relPath;       // Source-relative path (canonicalized to forward slashes)
    std::string targetBank;    // Optional destination bank override
    std::string targetName;    // Optional destination leaf name override
    auto operator<=>(const CollectorSelectionTarget&) const = default;
};

struct CollectorRequest {
    std::vector<CollectorSelectionTarget> targets; // 1..N selected roots (order-independent)
    CollectorExportMode mode = CollectorExportMode::AdvisoryPlan;
    bool includeEmbeddedKbdSets = true;            // Include Style embedded KBD Sets
    bool includeEmbeddedPads = true;               // Include Style embedded Pads
    bool requireCompleteSampleClosure = true;      // Fail-closed on unanalysed USERDK / OC31 KMP / missing PCM
    size_t cancelCheckStride = kDefaultCancelCheckStride; // Clamped to [1, 256]
};

enum class BlockingCode {
    EMPTY_SELECTION,
    SOURCE_UNAVAILABLE,
    SELECTED_ROOT_NOT_FOUND,
    MALFORMED_ROOT_TREE,
    MALFORMED_MIDI_LEAF,
    UNRESOLVED_MIDI_BANK_SELECT,
    UNKNOWN_ORIGIN_MIDI_ADDRESS,
    MISSING_USER_SOUND_BANK,
    MISSING_USER_SOUND_SLOT,
    MISSING_PCM_FOR_KMP_SAMPLE,
    MISSING_PACKAGE_WAV_OR_TX,
    DANGLING_GRAPH_EDGE,
    OC31_KMP_UNSUPPORTED,
    KMP_UNAVAILABLE,
    SOUND_DECODE_FAILED,
    DRUMKIT_BODY_UNPARSED,
    UNANALYSED_USER_SOUND,
    NATIVE_SOUND_WRITE_HOLD
};

inline const char* blockingCodeName(BlockingCode c) {
    switch (c) {
        case BlockingCode::EMPTY_SELECTION:             return "EMPTY_SELECTION";
        case BlockingCode::SOURCE_UNAVAILABLE:          return "SOURCE_UNAVAILABLE";
        case BlockingCode::SELECTED_ROOT_NOT_FOUND:     return "SELECTED_ROOT_NOT_FOUND";
        case BlockingCode::MALFORMED_ROOT_TREE:         return "MALFORMED_ROOT_TREE";
        case BlockingCode::MALFORMED_MIDI_LEAF:         return "MALFORMED_MIDI_LEAF";
        case BlockingCode::UNRESOLVED_MIDI_BANK_SELECT: return "UNRESOLVED_MIDI_BANK_SELECT";
        case BlockingCode::UNKNOWN_ORIGIN_MIDI_ADDRESS: return "UNKNOWN_ORIGIN_MIDI_ADDRESS";
        case BlockingCode::MISSING_USER_SOUND_BANK:     return "MISSING_USER_SOUND_BANK";
        case BlockingCode::MISSING_USER_SOUND_SLOT:     return "MISSING_USER_SOUND_SLOT";
        case BlockingCode::MISSING_PCM_FOR_KMP_SAMPLE:  return "MISSING_PCM_FOR_KMP_SAMPLE";
        case BlockingCode::MISSING_PACKAGE_WAV_OR_TX:   return "MISSING_PACKAGE_WAV_OR_TX";
        case BlockingCode::DANGLING_GRAPH_EDGE:         return "DANGLING_GRAPH_EDGE";
        case BlockingCode::OC31_KMP_UNSUPPORTED:        return "OC31_KMP_UNSUPPORTED";
        case BlockingCode::KMP_UNAVAILABLE:             return "KMP_UNAVAILABLE";
        case BlockingCode::SOUND_DECODE_FAILED:         return "SOUND_DECODE_FAILED";
        case BlockingCode::DRUMKIT_BODY_UNPARSED:       return "DRUMKIT_BODY_UNPARSED";
        case BlockingCode::UNANALYSED_USER_SOUND:       return "UNANALYSED_USER_SOUND";
        case BlockingCode::NATIVE_SOUND_WRITE_HOLD:     return "NATIVE_SOUND_WRITE_HOLD";
    }
    return "UNKNOWN_BLOCKER";
}

struct BlockingReason {
    BlockingCode code = BlockingCode::SOURCE_UNAVAILABLE;
    std::string subject;                   // ResourceId::str() or source-relative path (never local path)
    std::string summary;
    std::string remediation;
    std::vector<std::string> affectedRoots; // sorted unique relative paths
    auto operator<=>(const BlockingReason&) const = default;
};

struct CollectorNotice {
    std::string severity;                  // "warning" | "info"
    std::string code;                      // e.g. "FACTORY_ROM_SOUND_REF", "UNRESOLVED_SOUND_SLOT_ANALYSED", ...
    std::string evidence;                  // "Confirmed" | "Inferred" | "Unsupported"
    std::string subject;
    std::string summary;
    std::vector<std::string> affectedRoots;
    auto operator<=>(const CollectorNotice&) const = default;
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
};

// Strict 3-Way Classification for Tier 3 Native KST Content Equivalence.
// NEVER merges distinct Pa5X Id8 identities by content; NEVER deduplicates by filename or Sound name alone!
enum class NativeEquivalenceVerdict {
    Identical,            // Exact decoded body SHA-256 + both Analysed + 0 unresolved analysed slots + identical downstream MS/Zone/Sample Id8 & PCM sizes
    Different,            // Proven different kind, body SHA-256, or downstream MS/Zone/Sample Id8/size
    InsufficientEvidence  // Unanalysed (e.g. USERDK / NoKnownReference), missing SHA-256, unresolved slot, or KMP unavailable — NEVER merged!
};

inline const char* nativeEquivalenceVerdictName(NativeEquivalenceVerdict v) {
    switch (v) {
        case NativeEquivalenceVerdict::Identical:            return "Identical";
        case NativeEquivalenceVerdict::Different:            return "Different";
        case NativeEquivalenceVerdict::InsufficientEvidence: return "InsufficientEvidence";
    }
    return "InsufficientEvidence";
}

struct NativeEquivalenceEntry {
    std::string scope;                     // "source-vs-target-sound" | "intra-source-sound" | "source-vs-target-root"
    std::string sourceSubject;             // ResourceId::str() or relPath
    std::string targetSubject;             // ResourceId::str() or relPath
    NativeEquivalenceVerdict verdict = NativeEquivalenceVerdict::InsufficientEvidence;
    std::string reason;
    bool mergesPa5xId8 = false;            // INVARIANT: ALWAYS false (obeys resource_graph.h)
    auto operator<=>(const NativeEquivalenceEntry&) const = default;
};

struct MemoryProjection {
    std::string unit;                      // "uncompressed_pcm_bytes" | "native_kst_stored_pcm_proxy_bytes"
    bool hardwareVerified = false;         // ALWAYS false (S6 HOLD)
    bool sampleBytesAreNativeStoredProxy = false;
    uint64_t rawReferencedSampleBytes = 0;
    uint64_t uniqueLogicalSampleBytes = 0;
    uint64_t logicalDedupSavedBytes = 0;   // == rawReferencedSampleBytes - uniqueLogicalSampleBytes
    uint64_t identicalContentExtraBytes = 0;
    uint64_t uniqueSamplesCount = 0;
    uint64_t unknownSizeSamplesCount = 0;
    uint64_t targetBaselineSampleBytes = 0;
    uint64_t projectedCombinedBytes = 0;
    uint64_t limitDecimal4GB = graph::kPa5xUserSampleBytesDecimal;
    uint64_t limitBinary4GiB = graph::kPa5xUserSampleBytesBinary;
    uint64_t limitCompressed8GB = graph::kPa5xCompressedEquivalentDecimal;
    std::string fitVerdict;                // "fits" | "ambiguous" | "needs-compression" | "exceeds" | "unverified-proxy"
    std::string caveat;
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
    size_t sampleCount = 0;
    uint64_t uniqueSampleBytes = 0;
    bool satisfiedByTarget = false;
    std::string targetMatchSlot;           // populated when proven Identical in target
};

struct DryRunExportPlan {
    std::string schema = kDryRunPlanSchema;
    std::string banner = kStructuralOnlyBanner;
    bool hardwareVerified = false;         // ALWAYS false
    bool ok = false;                       // true = analysis finished cleanly (still check executable!)
    bool cancelled = false;                // true = cooperatively cancelled; partial state dropped
    bool executable = false;               // true iff ok && !cancelled && blockingReasons.empty() && mode != NativeSoundKst_HOLD
    bool coverageComplete = false;         // false whenever any selected Sound/KMP/DK is unanalysed/incomplete
    bool deleteProofAvailable = false;     // ALWAYS false for Native-KST

    AdapterSourceKind sourceKind = AdapterSourceKind::NativeKst;
    CollectorExportMode mode = CollectorExportMode::AdvisoryPlan;
    bool includeEmbeddedKbdSets = true;
    bool includeEmbeddedPads = true;
    bool requireCompleteSampleClosure = true;

    std::string planFingerprint;           // 64-hex SHA-256 over canonical selection, closure, dedup, source hashes
    std::string sourceManifestSha256;
    std::string targetManifestSha256;

    std::vector<CollectorSelectionTarget> selectedTargets;      // canonical sorted unique
    std::vector<graph::ResourceId> closureNodes;                // sorted unique forward closure
    std::vector<CollectedSoundSummary> collectedSounds;         // sorted by ResourceId
    std::vector<std::string> uniqueProvisionalPackages;         // sorted unique packageIds
    std::vector<graph::ResourceId> uniqueMultisamples;          // sorted unique
    std::vector<graph::ResourceId> uniqueSamples;               // sorted unique
    std::vector<std::string> uniquePcmFiles;                    // sorted unique KST-relative PCM paths

    size_t factorySoundSelectCount = 0;
    size_t userSoundSelectCount = 0;
    size_t unresolvedSlotsInAnalysedCount = 0;
    size_t unresolvedSlotsInUnanalysedCount = 0;

    std::vector<DedupGroup> dedupGroups;                        // canonically sorted across Tier 1, 2, 3
    std::vector<NativeEquivalenceEntry> nativeEquivalences;     // 3-way Tier 3 classification records
    std::vector<BlockingReason> blockingReasons;                // fail-closed blockers
    std::vector<CollectorNotice> notices;                       // warnings & info

    MemoryProjection memoryProjection;
    std::map<std::string, std::string> sourceFileSha256;        // relative path -> SHA-256 (zero local paths!)
};

namespace collector_detail {

struct DownstreamZoneSig {
    graph::Id8 sampleId8{};
    bool hasPcm = false;
    uint64_t payloadBytes = 0;
    uint32_t sampleRate = 0;
    auto operator<=>(const DownstreamZoneSig&) const = default;
};

struct DownstreamMsSig {
    graph::Id8 msId8{};
    std::vector<DownstreamZoneSig> zones;
    auto operator<=>(const DownstreamMsSig&) const = default;
};

inline bool buildSoundDownstreamSignature(
    const M1AdapterView& view,
    const SoundClosureRecord& sc,
    std::vector<DownstreamMsSig>& outSig,
    std::string& incompleteReason) {
    outSig.clear();
    incompleteReason.clear();
    for (const graph::ResourceId& msId : sc.multisampleIds) {
        const auto mit = view.multisamples.find(msId);
        if (mit == view.multisamples.end()) {
            incompleteReason = "referenced Multisample absent from KMP index";
            return false;
        }
        const MultisampleRecord& ms = mit->second;
        DownstreamMsSig sig;
        sig.msId8 = ms.id8;
        sig.zones.reserve(ms.zoneSampleIndices.size());
        for (uint32_t si : ms.zoneSampleIndices) {
            if (si == graph::kNoZoneSample) {
                sig.zones.push_back({graph::Id8{}, true, 0, 0});
                continue;
            }
            const graph::ResourceId smpId{false, "KST", graph::Kind::Sample, si};
            const auto sit = view.samples.find(smpId);
            if (sit == view.samples.end() || !sit->second.hasPcm) {
                incompleteReason = "referenced Sample missing PCM metadata";
                return false;
            }
            sig.zones.push_back({sit->second.id8, true, sit->second.payloadBytes, sit->second.sampleRate});
        }
        outSig.push_back(std::move(sig));
    }
    std::sort(outSig.begin(), outSig.end());
    return true;
}

inline std::string computePlanFingerprint(const DryRunExportPlan& p) {
    Sha256 h;
    adapter_detail::hashField(h, p.schema);
    adapter_detail::hashField(h, adapterSourceKindName(p.sourceKind));
    adapter_detail::hashField(h, collectorExportModeName(p.mode));
    adapter_detail::hashField(h, p.includeEmbeddedKbdSets ? "1" : "0");
    adapter_detail::hashField(h, p.includeEmbeddedPads ? "1" : "0");
    adapter_detail::hashField(h, p.requireCompleteSampleClosure ? "1" : "0");
    adapter_detail::hashField(h, p.sourceManifestSha256);
    adapter_detail::hashField(h, p.targetManifestSha256);

    for (const auto& t : p.selectedTargets) {
        adapter_detail::hashField(h, "sel");
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
        adapter_detail::hashField(h, nativeEquivalenceVerdictName(ne.verdict));
    }
    for (const auto& b : p.blockingReasons) {
        adapter_detail::hashField(h, "block");
        adapter_detail::hashField(h, blockingCodeName(b.code));
        adapter_detail::hashField(h, b.subject);
    }
    for (const auto& [rel, sha] : p.sourceFileSha256) {
        adapter_detail::hashField(h, "file");
        adapter_detail::hashField(h, rel);
        adapter_detail::hashField(h, sha);
    }
    adapter_detail::hashField(h, "mem");
    adapter_detail::hashField(h, p.memoryProjection.unit);
    adapter_detail::hashField(h, p.memoryProjection.fitVerdict);
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.rawReferencedSampleBytes));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.uniqueLogicalSampleBytes));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.logicalDedupSavedBytes));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.identicalContentExtraBytes));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.uniqueSamplesCount));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.unknownSizeSamplesCount));
    adapter_detail::hashField(h, std::to_string(p.memoryProjection.targetBaselineSampleBytes));
    return h.hex();
}

}  // namespace collector_detail

// Tier 3 Native KST Sound Equivalence Classifier (strict 3-way: Identical | Different | InsufficientEvidence).
// Never matches by Sound name alone; never merges distinct Pa5X Id8 identities!
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
        out.verdict = NativeEquivalenceVerdict::InsufficientEvidence;
        out.reason = "Sound absent in source or target view";
        return out;
    }
    const SoundClosureRecord& s = sit->second;
    const SoundClosureRecord& d = dit->second;

    // Definite structural/content mismatches prove Different even if names match!
    if (s.isDrumKit != d.isDrumKit) {
        out.verdict = NativeEquivalenceVerdict::Different;
        out.reason = "melodic Sound vs Drum Kit mismatch";
        return out;
    }
    if (s.kindChunkId != d.kindChunkId) {
        out.verdict = NativeEquivalenceVerdict::Different;
        out.reason = "Sound chunk kind differs";
        return out;
    }
    if (!s.decodedBodySha256.empty() && !d.decodedBodySha256.empty() && s.decodedBodySha256 != d.decodedBodySha256) {
        out.verdict = NativeEquivalenceVerdict::Different;
        out.reason = "decoded Sound body SHA-256 differs (same-name Sounds are never merged)";
        return out;
    }

    // Check evidence sufficiency before proving Identical
    if (!srcView.coverage.kmpParsed || !dstView.coverage.kmpParsed) {
        out.verdict = NativeEquivalenceVerdict::InsufficientEvidence;
        out.reason = "downstream RAM.KMP unparsed in source or target";
        return out;
    }
    if (!s.decoded || !d.decoded) {
        out.verdict = NativeEquivalenceVerdict::InsufficientEvidence;
        out.reason = "one or both Sounds failed OC31/payload decode";
        return out;
    }
    if (s.decodedBodySha256.empty() || d.decodedBodySha256.empty()) {
        out.verdict = NativeEquivalenceVerdict::InsufficientEvidence;
        out.reason = "decoded Sound body SHA-256 unavailable";
        return out;
    }
    if (!s.analysed || !d.analysed) {
        out.verdict = NativeEquivalenceVerdict::InsufficientEvidence;
        out.reason = "unanalysed Sound (" + (s.analysed ? d.unanalysedGap : s.unanalysedGap) + "); never merged";
        return out;
    }
    if (!s.unresolvedSlotsInAnalysed.empty() || !d.unresolvedSlotsInAnalysed.empty()) {
        out.verdict = NativeEquivalenceVerdict::InsufficientEvidence;
        out.reason = "unresolved slot(s) present in analysed Sound";
        return out;
    }

    // Both are Analysed with identical kind & body SHA-256; verify exact downstream MS/Zone/Sample Id8 & PCM sizes
    std::vector<collector_detail::DownstreamMsSig> sSig, dSig;
    std::string incomp;
    if (!collector_detail::buildSoundDownstreamSignature(srcView, s, sSig, incomp) ||
        !collector_detail::buildSoundDownstreamSignature(dstView, d, dSig, incomp)) {
        out.verdict = NativeEquivalenceVerdict::InsufficientEvidence;
        out.reason = incomp;
        return out;
    }
    if (sSig != dSig) {
        out.verdict = NativeEquivalenceVerdict::Different;
        out.reason = "downstream KMP Multisample/Zone/Sample Id8 or PCM payload size differs";
        return out;
    }

    out.verdict = NativeEquivalenceVerdict::Identical;
    out.reason = "proven equivalent: matching kind, decoded body SHA-256, analysed status, and full downstream Id8/PCM sizes";
    return out;
}

// Main M2b Read-Only Selective Collector, 3-Tier Deduplicator & Dry-Run Planner.
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
        return c;
    };
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

    DryRunExportPlan plan;
    plan.hardwareVerified = false;
    plan.deleteProofAvailable = false;
    plan.sourceKind = source.sourceKind;
    plan.mode = request.mode;
    plan.includeEmbeddedKbdSets = request.includeEmbeddedKbdSets;
    plan.includeEmbeddedPads = request.includeEmbeddedPads;
    plan.requireCompleteSampleClosure = request.requireCompleteSampleClosure;
    plan.sourceManifestSha256 = source.manifestSha256;
    if (optionalTarget) plan.targetManifestSha256 = optionalTarget->manifestSha256;

    // Helper maps for deduplicating blocking reasons and notices deterministically
    struct BlockerAgg {
        std::string summary;
        std::string remediation;
        std::set<std::string> roots;
    };
    std::map<std::pair<BlockingCode, std::string>, BlockerAgg> blockerMap;
    auto addBlocker = [&](BlockingCode code, const std::string& subject, const std::string& summary,
                          const std::string& remediation, const std::string& rootRel = "") {
        auto& agg = blockerMap[{code, subject}];
        if (agg.summary.empty()) { agg.summary = summary; agg.remediation = remediation; }
        if (!rootRel.empty()) agg.roots.insert(rootRel);
    };

    struct NoticeAgg {
        std::string severity;
        std::string evidence;
        std::string summary;
        std::set<std::string> roots;
    };
    std::map<std::tuple<std::string, std::string, std::string>, NoticeAgg> noticeMap; // (severity, code, subject)
    auto addNotice = [&](const std::string& severity, const std::string& code, const std::string& evidence,
                         const std::string& subject, const std::string& summary, const std::string& rootRel = "") {
        auto& agg = noticeMap[{severity, code, subject}];
        if (agg.summary.empty()) { agg.severity = severity; agg.evidence = evidence; agg.summary = summary; }
        if (!rootRel.empty()) agg.roots.insert(rootRel);
    };

    if (source.cancelled) return makeCancelled();
    if (!source.ok) {
        addBlocker(BlockingCode::SOURCE_UNAVAILABLE, "<source>",
                   "Source manifest view is unavailable or failed to load.",
                   "Inspect source container errors before planning a selective export.");
    }

    // 1. Canonicalize, sort, and deduplicate selection targets (order-independent!)
    for (const CollectorSelectionTarget& raw : request.targets) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        CollectorSelectionTarget norm = raw;
        norm.relPath = graph::closure_detail::normalizeRelPath(norm.relPath);
        plan.selectedTargets.push_back(std::move(norm));
    }
    std::sort(plan.selectedTargets.begin(), plan.selectedTargets.end());
    plan.selectedTargets.erase(std::unique(plan.selectedTargets.begin(), plan.selectedTargets.end()),
                               plan.selectedTargets.end());

    if (plan.selectedTargets.empty()) {
        addBlocker(BlockingCode::EMPTY_SELECTION, "<selection>",
                   "No Styles, Keyboard Sets, or Pads were selected.",
                   "Select at least one Style, Keyboard Set, or Pad to collect dependencies.");
    }

    if (request.mode == CollectorExportMode::NativeSoundKst_HOLD) {
        addBlocker(BlockingCode::NATIVE_SOUND_WRITE_HOLD, "<export-mode>",
                   "Native Sound KST export (.PCG/.KMP/.PCM writing) is on HOLD.",
                   "Use AdvisoryPlan (Pa5X single-Sound load, MAN5 p.736) or ImportKitBundle.");
    }

    // Closure & Tier-1/Tier-2/Tier-3 accumulators
    std::set<graph::ResourceId> closureNodeSet;
    std::map<graph::ResourceId, std::set<std::string>> soundRefRoots;
    std::map<graph::ResourceId, std::set<std::string>> msRefRoots;
    std::map<graph::ResourceId, std::set<graph::ResourceId>> msRefSounds;
    std::map<graph::ResourceId, std::set<std::string>> sampleRefRoots;
    std::map<graph::ResourceId, size_t> sampleRawRefCount;     // exact count of (root, sound/pkg) expansions reaching sample
    std::map<graph::ResourceId, std::set<std::string>> unknownSampleRoots;
    std::set<std::string> pcmFileSet;
    std::map<std::string, std::set<std::string>> provPkgRefRoots; // packageId -> referencing selected root relPaths
    std::map<std::string, size_t> provPkgRefCount;

    bool selectedHasUnanalysedOrGap = false;
    uint64_t rawReferencedSampleBytes = 0;

    // 2. Pass 1 & 2: Expand every selected root in canonical sorted order
    for (const CollectorSelectionTarget& target : plan.selectedTargets) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const RootItemRecord* root = source.findRoot(target.relPath);
        if (!root) {
            addBlocker(BlockingCode::SELECTED_ROOT_NOT_FOUND, target.relPath,
                       "Selected item '" + target.relPath + "' was not found in the source manifest.",
                       "Verify the source-relative path and refresh the source manifest.", target.relPath);
            selectedHasUnanalysedOrGap = true;
            continue;
        }

        closureNodeSet.insert(root->id);
        if (!root->contentSha256.empty()) plan.sourceFileSha256[root->relPath] = root->contentSha256;

        if (root->malformedMidiLeafCount > 0) {
            addBlocker(BlockingCode::MALFORMED_MIDI_LEAF, root->relPath,
                       "Root contains " + std::to_string(root->malformedMidiLeafCount) +
                           " malformed SMF 1.0 leaf/leaves (e.g. high-bit data byte >= 0x80).",
                       "Inspect or fix the malformed MIDI leaf; fail-closed to avoid silent corruption.", root->relPath);
            selectedHasUnanalysedOrGap = true;
        }
        if (!root->parseOk) {
            if (root->malformedMidiLeafCount == 0) {
                addBlocker(BlockingCode::MALFORMED_ROOT_TREE, root->relPath,
                           "Root container failed structural parsing: " + root->parseError,
                           "Inspect or repair the source file before exporting.", root->relPath);
            }
            selectedHasUnanalysedOrGap = true;
            continue;
        }

        // Per-root unique User Sound & Provisional Package sets (prevents duplicate counting within one root)
        std::set<graph::ResourceId> rootUserSounds;
        std::set<std::string> rootProvPkgs(root->directPackageIds.begin(), root->directPackageIds.end());

        for (const SoundSelectionRecord& sel : root->soundSelections) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

            // Apply explicit embedded KBD Set / Pad inclusion flags (never dropping main tracks!)
            if (root->kind == RootItemKind::Style) {
                const bool include =
                    sel.fromStyleMainTrack ||
                    (request.includeEmbeddedKbdSets && sel.fromEmbeddedKbdSet) ||
                    (request.includeEmbeddedPads && sel.fromEmbeddedPad);
                if (!include) continue;
            }

            // Provisional package binding (Path A Import Kit / Provisional Workspace)
            if (!sel.provisionalPackageId.empty()) {
                rootProvPkgs.insert(sel.provisionalPackageId);
                ++plan.userSoundSelectCount;
                continue;
            }

            switch (sel.boundary) {
                case SoundBoundaryKind::FactoryRom: {
                    ++plan.factorySoundSelectCount;
                    addNotice("info", "FACTORY_ROM_SOUND_REF", "Inferred",
                              std::to_string(sel.address.msb) + "/" + std::to_string(sel.address.lsb) + "/" +
                                  std::to_string(sel.address.program),
                              "External Pa5X Factory ROM Sound reference (no User Sound/Sample copied).",
                              root->relPath);
                    break;
                }
                case SoundBoundaryKind::UnknownOrUnanalysed: {
                    selectedHasUnanalysedOrGap = true;
                    if (sel.bankMissing) {
                        addBlocker(BlockingCode::UNRESOLVED_MIDI_BANK_SELECT, root->relPath,
                                   "Program Change " + std::to_string(sel.address.program) +
                                       " has unset CC0/CC32 bank select; target cannot be classified.",
                                   "Add explicit CC0/CC32 Bank Select before Program Change or supply a Sound Map.",
                                   root->relPath);
                    } else {
                        const std::string addrStr = std::to_string(sel.address.msb) + "/" +
                                                    std::to_string(sel.address.lsb) + "/" +
                                                    std::to_string(sel.address.program);
                        if (request.mode != CollectorExportMode::AdvisoryPlan || request.requireCompleteSampleClosure) {
                            addBlocker(BlockingCode::UNKNOWN_ORIGIN_MIDI_ADDRESS, addrStr,
                                       "MIDI Bank/PC " + addrStr + " lies outside verified Pa5X User and Factory ranges.",
                                       "Verify this address on Pa5X or remap explicitly; never silently coerced to Factory.",
                                       root->relPath);
                        } else {
                            addNotice("warning", "UNKNOWN_ORIGIN_MIDI_ADDRESS", "Unsupported", addrStr,
                                      "MIDI Bank/PC " + addrStr + " lies outside verified Pa5X User/Factory ranges.",
                                      root->relPath);
                        }
                    }
                    break;
                }
                case SoundBoundaryKind::AmbiguousOrOutOfSpec: {
                    ++plan.userSoundSelectCount;
                    const std::string subj = sel.targetSoundId ? sel.targetSoundId->str() : sel.userBank;
                    if (sel.resolved && sel.targetSoundId.has_value()) {
                        addNotice("warning", "AMBIGUOUS_OUT_OF_SPEC_DRUMKIT_BANK", "Unsupported", subj,
                                  "Address MSB 120 LSB " + std::to_string(sel.address.lsb) +
                                      " is outside documented USERDK01..06 range but matched an existing bank file.",
                                  root->relPath);
                        rootUserSounds.insert(*sel.targetSoundId);
                    } else {
                        selectedHasUnanalysedOrGap = true;
                        addBlocker(BlockingCode::MISSING_USER_SOUND_BANK, subj,
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
                        addBlocker(bankAbsent ? BlockingCode::MISSING_USER_SOUND_BANK : BlockingCode::MISSING_USER_SOUND_SLOT,
                                   sel.targetSoundId->str(),
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
                addBlocker(BlockingCode::SOUND_DECODE_FAILED, sid.str(),
                           "Sound " + sid.str() + " failed OC31/payload decoding.",
                           "Source Sound item is corrupt or unsupported; cannot trace dependencies.",
                           root->relPath);
                continue;
            }

            if (!source.coverage.kmpParsed) {
                selectedHasUnanalysedOrGap = true;
                if (source.coverage.kmpIsOc31) {
                    if (request.requireCompleteSampleClosure || request.mode != CollectorExportMode::AdvisoryPlan) {
                        addBlocker(BlockingCode::OC31_KMP_UNSUPPORTED, "MULTISMP/RAM.KMP",
                                   "Legacy OC31-compressed RAM.KMP (Pa700/Pa4X); Pa5X raw RAM.KMP is required for sample closure.",
                                   "Use an authentic Pa5X raw RAM.KMP or inspect Sound inventory only.",
                                   root->relPath);
                    } else {
                        addNotice("warning", "OC31_KMP_UNSUPPORTED", "Unsupported", "MULTISMP/RAM.KMP",
                                  "Legacy OC31-compressed RAM.KMP; downstream sample closure unavailable.",
                                  root->relPath);
                    }
                } else {
                    if (request.requireCompleteSampleClosure || request.mode != CollectorExportMode::AdvisoryPlan) {
                        addBlocker(BlockingCode::KMP_UNAVAILABLE, "MULTISMP/RAM.KMP",
                                   "RAM.KMP is absent or unparsed: " + source.coverage.kmpError,
                                   "Ensure MULTISMP/RAM.KMP is present and valid.",
                                   root->relPath);
                    }
                }
                continue;
            }

            // Record explicit UnresolvedSlot separation for this Sound
            for (const auto& u : sc.unresolvedSlotsInAnalysed) {
                addNotice("warning", "UNRESOLVED_SOUND_SLOT_ANALYSED", "Inferred",
                          u.sound.str() + ":rec" + std::to_string(u.record) + ":slot" + std::to_string(u.slot),
                          "Non-empty, non-RAM 8-byte ID in analysed Sound (possible ROM multisample or dangling ID).",
                          root->relPath);
            }
            for (const auto& u : sc.unresolvedSlotsInUnanalysed) {
                addNotice("info", "UNRESOLVED_SOUND_SLOT_SPECULATIVE", "Unsupported",
                          u.sound.str() + ":rec" + std::to_string(u.record) + ":slot" + std::to_string(u.slot),
                          "Speculative slot in unanalysed Sound (layout may not apply).",
                          root->relPath);
            }

            if (!sc.analysed) {
                selectedHasUnanalysedOrGap = true;
                if (sc.isDrumKit) {
                    if (request.requireCompleteSampleClosure || request.mode != CollectorExportMode::AdvisoryPlan) {
                        addBlocker(BlockingCode::DRUMKIT_BODY_UNPARSED, sid.str(),
                                   "User Drum Kit " + sid.str() + " internal DrumSample edges are not yet parsed (" + sc.unanalysedGap + ").",
                                   "Sample closure for USERDK is incomplete; use Pa5X single-Sound load or TXPERF Import Kit.",
                                   root->relPath);
                    } else {
                        addNotice("warning", "DRUMKIT_BODY_UNPARSED", "Unsupported", sid.str(),
                                  "User Drum Kit body edges unparsed; sample closure incomplete.",
                                  root->relPath);
                    }
                } else if (sc.unanalysedGap == "NoKnownReference") {
                    addNotice("info", "SOUND_NO_RAM_REF_INFERRED_ROM", "Inferred", sid.str(),
                              "Melodic User Sound fits layout with 0 RAM IDs (consistent with ROM-only multisamples; INFERRED).",
                              root->relPath);
                } else {
                    if (request.requireCompleteSampleClosure || request.mode != CollectorExportMode::AdvisoryPlan) {
                        addBlocker(BlockingCode::UNANALYSED_USER_SOUND, sid.str(),
                                   "User Sound " + sid.str() + " could not be analysed (" + sc.unanalysedGap + ").",
                                   "Cannot prove complete Multisample/Sample dependencies offline.",
                                   root->relPath);
                    } else {
                        addNotice("warning", "UNANALYSED_USER_SOUND", "Unsupported", sid.str(),
                                  "User Sound unanalysed (" + sc.unanalysedGap + ").",
                                  root->relPath);
                    }
                }
                continue;
            }

            // Trace analysed downstream closure: Sound -> Oscillator -> Multisample -> Zone -> Sample -> PCM
            for (const auto& oscId : sc.oscillatorIds) closureNodeSet.insert(oscId);
            for (const auto& msId : sc.multisampleIds) {
                closureNodeSet.insert(msId);
                msRefRoots[msId].insert(root->relPath);
                msRefSounds[msId].insert(sid);
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
                    addBlocker(BlockingCode::MISSING_PCM_FOR_KMP_SAMPLE, smpId.str(),
                               "Reachable KMP Sample " + smpId.str() + " has no matching PCM entry.",
                               "Restore missing PCM/*.pcm container before exporting.",
                               root->relPath);
                } else {
                    rawReferencedSampleBytes += spIt->second.payloadBytes;
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
                addBlocker(BlockingCode::MISSING_PACKAGE_WAV_OR_TX, pkgId,
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
                addBlocker(BlockingCode::DANGLING_GRAPH_EDGE, pkg.packageId,
                           "Provisional package '" + pkg.packageId + "' contains dangling graph references.",
                           "Regenerate package cleanly before export.",
                           root->relPath);
            }
            for (const graph::ResourceId& smpId : pkg.sampleIds) {
                sampleRefRoots[smpId].insert(root->relPath);
                ++sampleRawRefCount[smpId];
                const auto spIt = source.samples.find(smpId);
                if (spIt != source.samples.end() && spIt->second.hasPcm) {
                    rawReferencedSampleBytes += spIt->second.payloadBytes;
                } else {
                    unknownSampleRoots[smpId].insert(root->relPath);
                    selectedHasUnanalysedOrGap = true;
                }
            }
        }
    }

    // Record KMP file SHA-256 in sourceFileSha256 if present in source view
    for (const auto& [rel, sha] : source.sourceFileSha256) {
        if (graph::native_detail::endsWithUpper(rel, ".KMP")) plan.sourceFileSha256[rel] = sha;
    }

    // Check duplicate drum IDs in source KMP -> coverageComplete cannot be true
    if (source.isNativeKst() && !source.duplicateDrumIds.empty()) {
        selectedHasUnanalysedOrGap = true;
        addNotice("warning", "DUPLICATE_KMP_DRUM_ID", "Confirmed", "MULTISMP/RAM.KMP",
                  std::to_string(source.duplicateDrumIds.size()) +
                      " duplicate 8-byte DrumSample ID group(s) in RAM.KMP (kept positional; delete safety unprovable).");
    }

    // 3. Tier 3 Native KST Equivalence (source vs optionalTarget AND intra-source same-name/same-hash check)
    std::map<graph::ResourceId, std::string> targetMatchedSoundSlot;
    if (source.isNativeKst()) {
        // 3a. Compare selected roots against optionalTarget by case-folded relative path
        if (optionalTarget && optionalTarget->isNativeKst()) {
            std::map<std::string, const RootItemRecord*> targetRootsFolded;
            for (const auto& tr : optionalTarget->roots) {
                targetRootsFolded[graph::native_detail::upperAscii(tr.relPath)] = &tr;
            }
            for (const auto& sel : plan.selectedTargets) {
                if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
                const RootItemRecord* sr = source.findRoot(sel.relPath);
                if (!sr) continue;
                const auto tit = targetRootsFolded.find(graph::native_detail::upperAscii(sr->relPath));
                if (tit == targetRootsFolded.end()) continue;
                const RootItemRecord* tr = tit->second;
                NativeEquivalenceEntry eq;
                eq.scope = "source-vs-target-root";
                eq.sourceSubject = sr->relPath;
                eq.targetSubject = tr->relPath;
                eq.mergesPa5xId8 = false;
                if (sr->contentSha256.empty() || tr->contentSha256.empty()) {
                    eq.verdict = NativeEquivalenceVerdict::InsufficientEvidence;
                    eq.reason = "root file SHA-256 unavailable in source or target";
                } else if (sr->contentSha256 == tr->contentSha256) {
                    eq.verdict = NativeEquivalenceVerdict::Identical;
                    eq.reason = "root file SHA-256 identical in target (advisory no-op)";
                    DedupGroup dg;
                    dg.tier = DedupTier::Tier3NativeEquivalence;
                    dg.resourceKind = "RootFile";
                    dg.keyType = "root-file-sha256";
                    dg.keyValue = sr->contentSha256;
                    dg.canonicalMember = tr->relPath;
                    dg.members = {sr->relPath, tr->relPath};
                    std::sort(dg.members.begin(), dg.members.end());
                    dg.occurrenceCount = 2;
                    dg.savedBytes = 0;
                    dg.appliedInExport = false;
                    plan.dedupGroups.push_back(std::move(dg));
                } else {
                    eq.verdict = NativeEquivalenceVerdict::Different;
                    eq.reason = "root file SHA-256 differs from target at same case-folded path";
                }
                plan.nativeEquivalences.push_back(std::move(eq));
            }

            // Index target Sounds by decodedBodySha256 and by upper-case name (for non-equivalence check)
            std::map<std::string, std::vector<graph::ResourceId>> targetByBodySha;
            std::map<std::string, std::vector<graph::ResourceId>> targetByName;
            for (const auto& [tid, tsc] : optionalTarget->sounds) {
                if (!tsc.decodedBodySha256.empty()) targetByBodySha[tsc.decodedBodySha256].push_back(tid);
                if (!tsc.name.empty()) targetByName[graph::native_detail::upperAscii(tsc.name)].push_back(tid);
            }

            for (const auto& [sid, roots] : soundRefRoots) {
                if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
                const auto scIt = source.sounds.find(sid);
                if (scIt == source.sounds.end()) continue;
                const SoundClosureRecord& sc = scIt->second;

                std::set<graph::ResourceId> candidates;
                if (optionalTarget->sounds.count(sid)) candidates.insert(sid);
                if (!sc.decodedBodySha256.empty()) {
                    if (const auto bit = targetByBodySha.find(sc.decodedBodySha256); bit != targetByBodySha.end()) {
                        candidates.insert(bit->second.begin(), bit->second.end());
                    }
                }
                if (!sc.name.empty()) {
                    if (const auto nit = targetByName.find(graph::native_detail::upperAscii(sc.name)); nit != targetByName.end()) {
                        candidates.insert(nit->second.begin(), nit->second.end());
                    }
                }

                for (const graph::ResourceId& candId : candidates) {
                    NativeEquivalenceEntry eq = classifyNativeSoundEquivalence(source, sid, *optionalTarget, candId, "source-vs-target-sound");
                    if (eq.verdict == NativeEquivalenceVerdict::Identical && !targetMatchedSoundSlot.count(sid)) {
                        targetMatchedSoundSlot[sid] = candId.str();
                        DedupGroup dg;
                        dg.tier = DedupTier::Tier3NativeEquivalence;
                        dg.resourceKind = "Sound";
                        dg.keyType = "native-sound-equivalence";
                        dg.keyValue = sc.decodedBodySha256;
                        dg.canonicalMember = candId.str();
                        dg.members = {sid.str(), candId.str()};
                        std::sort(dg.members.begin(), dg.members.end());
                        dg.occurrenceCount = 2;
                        dg.savedBytes = sc.storedPcmBytesProxy;
                        dg.appliedInExport = false; // advisory; RootTreeSubKst reuses target without rewriting KMP/PCG
                        plan.dedupGroups.push_back(std::move(dg));
                    }
                    plan.nativeEquivalences.push_back(std::move(eq));
                }
            }
        }

        // 3b. Intra-source Sound equivalence check across collected Sounds sharing name OR decodedBodySha256
        //     (strictly 3-way: Identical vs Different vs InsufficientEvidence; NEVER merges Pa5X identities!)
        std::vector<graph::ResourceId> collectedSoundIds;
        collectedSoundIds.reserve(soundRefRoots.size());
        for (const auto& [sid, _] : soundRefRoots) collectedSoundIds.push_back(sid);
        std::map<std::string, std::vector<graph::ResourceId>> srcByNameOrSha;
        for (const auto& sid : collectedSoundIds) {
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
                    if (eq.verdict == NativeEquivalenceVerdict::Identical) {
                        const auto& sc = source.sounds.at(pairKey.first);
                        DedupGroup dg;
                        dg.tier = DedupTier::Tier3NativeEquivalence;
                        dg.resourceKind = "Sound";
                        dg.keyType = "native-sound-equivalence";
                        dg.keyValue = sc.decodedBodySha256;
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

        // Enforce NATIVE_SOUND_WRITE_HOLD when export mode would require writing a Native User Sound not satisfied by target
        if (request.mode == CollectorExportMode::RootTreeSubKst || request.mode == CollectorExportMode::ImportKitBundle) {
            for (const auto& [sid, roots] : soundRefRoots) {
                if (!targetMatchedSoundSlot.count(sid)) {
                    for (const std::string& r : roots) {
                        addBlocker(BlockingCode::NATIVE_SOUND_WRITE_HOLD, sid.str(),
                                   "Selected item requires Native User Sound " + sid.str() +
                                       " which is not proven equivalent in Target KST; native .PCG/.KMP/.PCM writing is on HOLD.",
                                   "Use AdvisoryPlan + Pa5X single-Sound load (MAN5 p.736), remap to a Factory/Target Sound, or replace via Import Kit.",
                                   r);
                    }
                }
            }
        }
    }

    // 4. Build CollectedSoundSummary + Tier 1 Logical Identity Dedup Groups (Sounds, Multisamples, Samples)
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
        css.sampleCount = sc.sampleIds.size();
        css.uniqueSampleBytes = sc.storedPcmBytesProxy;
        if (const auto tmit = targetMatchedSoundSlot.find(sid); tmit != targetMatchedSoundSlot.end()) {
            css.satisfiedByTarget = true;
            css.targetMatchSlot = tmit->second;
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
            dg.savedBytes = (roots.size() - 1) * sc.storedPcmBytesProxy;
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

    uint64_t uniqueLogicalSampleBytes = 0;
    std::map<std::string, std::vector<graph::ResourceId>> provisionalWavByContentHash;
    for (const auto& [smpId, rawCount] : sampleRawRefCount) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        plan.uniqueSamples.push_back(smpId);
        const auto spIt = source.samples.find(smpId);
        uint64_t sbytes = 0;
        if (spIt != source.samples.end() && spIt->second.hasPcm) {
            sbytes = spIt->second.payloadBytes;
            uniqueLogicalSampleBytes += sbytes;
            // Tier 2b candidate: provisional WAV audio fingerprint (contentHash + bytes) across distinct packages
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
            dg.savedBytes = (rawCount - 1) * sbytes;
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
            dg.savedBytes = (rc - 1) * pkg.totalUncompressedBytes;
            dg.appliedInExport = true;
            plan.dedupGroups.push_back(std::move(dg));
        }
    }

    uint64_t identicalContentExtraBytes = 0;
    for (const auto& [hkey, ids] : provisionalWavByContentHash) {
        if (ids.size() <= 1) continue;
        std::vector<graph::ResourceId> sortedIds = ids;
        std::sort(sortedIds.begin(), sortedIds.end());
        const uint64_t oneBytes = source.samples.at(sortedIds.front()).payloadBytes;
        const uint64_t extra = (sortedIds.size() - 1) * oneBytes;
        identicalContentExtraBytes += extra;

        DedupGroup dg;
        dg.tier = DedupTier::Tier2ProvisionalContent;
        dg.resourceKind = "Sample";
        dg.keyType = "audio-fnv1a64+bytes";
        dg.keyValue = hkey;
        dg.canonicalMember = sortedIds.front().str();
        for (const auto& id : sortedIds) dg.members.push_back(id.str());
        dg.occurrenceCount = sortedIds.size();
        dg.savedBytes = extra;
        dg.appliedInExport = false; // advisory only: never mutates signed TXPROG/SF2 packages!
        plan.dedupGroups.push_back(std::move(dg));
    }

    // 6. Honest Memory & Sample RAM Projection (§4.4)
    MemoryProjection& mp = plan.memoryProjection;
    mp.hardwareVerified = false;
    mp.rawReferencedSampleBytes = rawReferencedSampleBytes;
    mp.uniqueLogicalSampleBytes = uniqueLogicalSampleBytes;
    mp.logicalDedupSavedBytes = rawReferencedSampleBytes >= uniqueLogicalSampleBytes
                                    ? (rawReferencedSampleBytes - uniqueLogicalSampleBytes)
                                    : 0;
    mp.identicalContentExtraBytes = identicalContentExtraBytes;
    mp.uniqueSamplesCount = plan.uniqueSamples.size();
    mp.unknownSizeSamplesCount = unknownSampleRoots.size();

    uint64_t targetBaselineBytes = 0;
    if (optionalTarget && optionalTarget->ok) {
        for (const auto& [_, sr] : optionalTarget->samples) {
            if (sr.hasPcm) targetBaselineBytes += sr.payloadBytes;
        }
    }
    mp.targetBaselineSampleBytes = targetBaselineBytes;
    mp.projectedCombinedBytes = targetBaselineBytes + uniqueLogicalSampleBytes;

    const bool anyNativeProxy = source.isNativeKst() || (optionalTarget && optionalTarget->isNativeKst());
    mp.sampleBytesAreNativeStoredProxy = anyNativeProxy;
    if (anyNativeProxy) {
        mp.unit = "native_kst_stored_pcm_proxy_bytes";
        mp.fitVerdict = "unverified-proxy";
        mp.caveat = kNativeProxyCaveat;
    } else {
        mp.unit = "uncompressed_pcm_bytes";
        if (mp.projectedCombinedBytes <= mp.limitDecimal4GB) mp.fitVerdict = "fits";
        else if (mp.projectedCombinedBytes <= mp.limitBinary4GiB) mp.fitVerdict = "ambiguous";
        else if (mp.projectedCombinedBytes <= mp.limitCompressed8GB) mp.fitVerdict = "needs-compression";
        else mp.fitVerdict = "exceeds";
        mp.caveat = kProvisionalUncompressedCaveat;
    }

    // 7. Finalize sorted tables, fail-closed executable gate, and deterministic planFingerprint
    plan.closureNodes.assign(closureNodeSet.begin(), closureNodeSet.end());
    plan.uniquePcmFiles.assign(pcmFileSet.begin(), pcmFileSet.end());

    for (const auto& [key, agg] : blockerMap) {
        plan.blockingReasons.push_back({
            key.first, key.second, agg.summary, agg.remediation,
            std::vector<std::string>(agg.roots.begin(), agg.roots.end())
        });
    }
    for (const auto& [key, agg] : noticeMap) {
        plan.notices.push_back({
            std::get<0>(key), std::get<1>(key), agg.evidence, std::get<2>(key), agg.summary,
            std::vector<std::string>(agg.roots.begin(), agg.roots.end())
        });
    }
    std::sort(plan.dedupGroups.begin(), plan.dedupGroups.end());
    std::sort(plan.nativeEquivalences.begin(), plan.nativeEquivalences.end());
    std::sort(plan.blockingReasons.begin(), plan.blockingReasons.end());
    std::sort(plan.notices.begin(), plan.notices.end());

    plan.ok = source.ok;
    plan.coverageComplete =
        source.ok && !selectedHasUnanalysedOrGap &&
        (source.isProvisional() ? source.coverage.coverageComplete
                                : (source.coverage.kmpParsed && !source.coverage.songBookPresent));
    plan.executable = plan.ok && !plan.cancelled && plan.blockingReasons.empty() &&
                      request.mode != CollectorExportMode::NativeSoundKst_HOLD;
    plan.planFingerprint = collector_detail::computePlanFingerprint(plan);
    return plan;
}

// ── Reproducible, Path-Free JSON Dry-Run Preview Serializer ──────────────────────────────────────────────────────────
// Guaranteed RFC 8259 valid JSON (UTF-8-safe escaping), zero local paths, byte-identical across repeated runs
// and shuffled selection orders.
inline std::string serializeDryRunPlanJson(const DryRunExportPlan& p) {
    auto q = [](const std::string& s) { return "\"" + graph::closure_detail::manifestJsonEscape(s) + "\""; };
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
    o.reserve(4096 + p.dedupGroups.size() * 192 + p.blockingReasons.size() * 192);
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
    o += "  \"mode\": " + q(collectorExportModeName(p.mode)) + ",\n";
    o += "  \"includeEmbeddedKbdSets\": " + std::string(bstr(p.includeEmbeddedKbdSets)) + ",\n";
    o += "  \"includeEmbeddedPads\": " + std::string(bstr(p.includeEmbeddedPads)) + ",\n";
    o += "  \"requireCompleteSampleClosure\": " + std::string(bstr(p.requireCompleteSampleClosure)) + ",\n";
    o += "  \"planFingerprint\": " + q(p.planFingerprint) + ",\n";
    o += "  \"sourceManifestSha256\": " + q(p.sourceManifestSha256) + ",\n";
    o += "  \"targetManifestSha256\": " + q(p.targetManifestSha256) + ",\n";

    o += "  \"selection\": [";
    for (size_t i = 0; i < p.selectedTargets.size(); ++i) {
        const auto& t = p.selectedTargets[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"relPath\": " + q(t.relPath) +
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
    o += "    \"uniqueSamples\": " + std::to_string(p.uniqueSamples.size()) + ",\n";
    o += "    \"uniquePcmFiles\": " + std::to_string(p.uniquePcmFiles.size()) + ",\n";
    o += "    \"unresolvedSlotsInAnalysed\": " + std::to_string(p.unresolvedSlotsInAnalysedCount) + ",\n";
    o += "    \"unresolvedSlotsInUnanalysed\": " + std::to_string(p.unresolvedSlotsInUnanalysedCount) + ",\n";
    o += "    \"blockingIssues\": " + std::to_string(p.blockingReasons.size()) + ",\n";
    o += "    \"notices\": " + std::to_string(p.notices.size()) + "\n";
    o += "  },\n";

    const auto& mp = p.memoryProjection;
    o += "  \"memoryProjection\": {\n";
    o += "    \"unit\": " + q(mp.unit) + ",\n";
    o += "    \"hardwareVerified\": " + std::string(bstr(mp.hardwareVerified)) + ",\n";
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
    o += "      \"decimal4GB\": " + std::to_string(mp.limitDecimal4GB) + ",\n";
    o += "      \"binary4GiB\": " + std::to_string(mp.limitBinary4GiB) + ",\n";
    o += "      \"compressedEquivalent8GB\": " + std::to_string(mp.limitCompressed8GB) + "\n";
    o += "    },\n";
    o += "    \"fitVerdict\": " + q(mp.fitVerdict) + ",\n";
    o += "    \"caveat\": " + q(mp.caveat) + "\n";
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
             ", \"sampleCount\": " + std::to_string(cs.sampleCount) +
             ", \"uniqueSampleBytes\": " + std::to_string(cs.uniqueSampleBytes) +
             ", \"satisfiedByTarget\": " + std::string(bstr(cs.satisfiedByTarget)) +
             ", \"targetMatchSlot\": " + q(cs.targetMatchSlot) + "}";
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
             ", \"verdict\": " + q(nativeEquivalenceVerdictName(ne.verdict)) +
             ", \"reason\": " + q(ne.reason) +
             ", \"mergesPa5xId8\": " + std::string(bstr(ne.mergesPa5xId8)) + "}";
    }
    o += p.nativeEquivalences.empty() ? "],\n" : "\n  ],\n";

    o += "  \"blockingReasons\": [";
    for (size_t i = 0; i < p.blockingReasons.size(); ++i) {
        const auto& b = p.blockingReasons[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"code\": " + q(blockingCodeName(b.code)) +
             ", \"subject\": " + q(b.subject) +
             ", \"summary\": " + q(b.summary) +
             ", \"remediation\": " + q(b.remediation) +
             ", \"affectedRoots\": ";
        writeStrVec(o, b.affectedRoots);
        o += "}";
    }
    o += p.blockingReasons.empty() ? "],\n" : "\n  ],\n";

    o += "  \"notices\": [";
    for (size_t i = 0; i < p.notices.size(); ++i) {
        const auto& n = p.notices[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"severity\": " + q(n.severity) +
             ", \"code\": " + q(n.code) +
             ", \"evidence\": " + q(n.evidence) +
             ", \"subject\": " + q(n.subject) +
             ", \"summary\": " + q(n.summary) +
             ", \"affectedRoots\": ";
        writeStrVec(o, n.affectedRoots);
        o += "}";
    }
    o += p.notices.empty() ? "],\n" : "\n  ],\n";

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

// ── Reproducible, Path-Free Markdown Dry-Run Preview Serializer ──────────────────────────────────────────────────────
inline std::string serializeDryRunPlanMarkdown(const DryRunExportPlan& p) {
    std::string o;
    o.reserve(2048 + p.blockingReasons.size() * 128 + p.dedupGroups.size() * 96);
    o += "# Selective Export Dry-Run Plan Preview\n\n";
    o += "> **" + p.banner + "** (`hardwareVerified: false`, `deleteProofAvailable: false`)\n\n";
    if (!p.coverageComplete) {
        o += "> **Coverage Notice:** Partial coverage — delete safety cannot be proven.\n\n";
    }
    o += "- **Schema:** `" + p.schema + "`\n";
    o += "- **Source Kind:** `" + std::string(adapterSourceKindName(p.sourceKind)) + "`\n";
    o += "- **Export Mode:** `" + std::string(collectorExportModeName(p.mode)) + "`\n";
    o += "- **Executable:** `" + std::string(p.executable ? "true" : "false") + "`\n";
    o += "- **Plan Fingerprint (SHA-256):** `" + p.planFingerprint + "`\n";
    o += "- **Selected Items:** " + std::to_string(p.selectedTargets.size()) + "\n";
    o += "- **Closure Summary:** " + std::to_string(p.collectedSounds.size()) + " User Sound(s), " +
         std::to_string(p.uniqueProvisionalPackages.size()) + " Provisional Package(s), " +
         std::to_string(p.uniqueMultisamples.size()) + " Multisample(s), " +
         std::to_string(p.uniqueSamples.size()) + " Sample(s), " +
         std::to_string(p.uniquePcmFiles.size()) + " PCM file(s), " +
         std::to_string(p.factorySoundSelectCount) + " Factory ROM select(s)\n";
    o += "- **Unresolved Sound Slots:** " + std::to_string(p.unresolvedSlotsInAnalysedCount) +
         " in analysed Sound(s), " + std::to_string(p.unresolvedSlotsInUnanalysedCount) +
         " speculative in unanalysed Sound(s)\n\n";

    const auto& mp = p.memoryProjection;
    o += "## Memory & Sample RAM Projection\n\n";
    o += "- **Unit:** `" + mp.unit + "`\n";
    o += "- **Fit Verdict:** `" + mp.fitVerdict + "`\n";
    o += "- **Pre-Dedup Referenced Sample Bytes:** " + std::to_string(mp.rawReferencedSampleBytes) + "\n";
    o += "- **Unique Logical Sample Bytes:** " + std::to_string(mp.uniqueLogicalSampleBytes) + "\n";
    o += "- **Tier-1 Logical Dedup Saved Bytes:** " + std::to_string(mp.logicalDedupSavedBytes) + "\n";
    o += "- **Tier-2 Identical-Content Extra Bytes (Advisory):** " + std::to_string(mp.identicalContentExtraBytes) + "\n";
    o += "- **Unique Samples / Unknown-Size Samples:** " + std::to_string(mp.uniqueSamplesCount) + " / " +
         std::to_string(mp.unknownSizeSamplesCount) + "\n";
    o += "- **Target Baseline / Projected Combined Bytes:** " + std::to_string(mp.targetBaselineSampleBytes) +
         " / " + std::to_string(mp.projectedCombinedBytes) + "\n";
    o += "- **Caveat:** " + mp.caveat + "\n\n";

    o += "## Blocking Issues (" + std::to_string(p.blockingReasons.size()) + ")\n\n";
    if (p.blockingReasons.empty()) {
        o += "_None._\n\n";
    } else {
        for (const auto& b : p.blockingReasons) {
            o += "- **`" + std::string(blockingCodeName(b.code)) + "`** (`" + b.subject + "`): " + b.summary +
                 " — _Remediation:_ " + b.remediation + "\n";
        }
        o += "\n";
    }

    o += "## Deduplication Groups (" + std::to_string(p.dedupGroups.size()) + ")\n\n";
    if (p.dedupGroups.empty()) {
        o += "_None._\n\n";
    } else {
        for (const auto& dg : p.dedupGroups) {
            o += "- `" + std::string(dedupTierName(dg.tier)) + "` | `" + dg.resourceKind + "` | `" + dg.keyType +
                 "` | canonical `" + dg.canonicalMember + "` | refs=" + std::to_string(dg.occurrenceCount) +
                 " | savedBytes=" + std::to_string(dg.savedBytes) + " | applied=" +
                 (dg.appliedInExport ? "true" : "false") + "\n";
        }
        o += "\n";
    }

    if (!p.nativeEquivalences.empty()) {
        o += "## Tier-3 Native KST Equivalence (" + std::to_string(p.nativeEquivalences.size()) + ")\n\n";
        for (const auto& ne : p.nativeEquivalences) {
            o += "- `" + ne.scope + "` | `" + ne.sourceSubject + "` vs `" + ne.targetSubject + "` -> **`" +
                 std::string(nativeEquivalenceVerdictName(ne.verdict)) + "`** (" + ne.reason + ")\n";
        }
        o += "\n";
    }

    return o;
}

}  // namespace ai_arranger::media::selective

#endif  // AI_ARRANGER_MEDIA_SOUND_SELECTIVE_COLLECTOR_H
