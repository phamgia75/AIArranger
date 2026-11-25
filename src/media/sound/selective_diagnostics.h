#ifndef AI_ARRANGER_MEDIA_SOUND_SELECTIVE_DIAGNOSTICS_H
#define AI_ARRANGER_MEDIA_SOUND_SELECTIVE_DIAGNOSTICS_H

// M2a Structured Missing-Resource Diagnostics & Deterministic Report Serializers (READ-ONLY).
//
// Implements the 4-category diagnostic taxonomy, canonical code catalog, coverage honesty summary,
// and deterministic JSON (`aiarranger-selective-export-report/1`) + Markdown report formatters.
//
// Invariants:
//  * Portable C++20 header-only, zero JUCE/GUI dependency, zero disk mutations.
//  * Mandatory `STRUCTURALLY VALID — NOT VERIFIED ON A PA5X` banner and `"hardwareVerified": false`.
//  * Honest `coverageComplete` disclosure and `deleteProofAvailable: false` (never proves delete safety).
//  * Path-free output: local filesystem paths are scrubbed to canonical labels; zero timestamps or pointers.
//  * JSON strings escape via `media::graph::jsonEscape()` + UTF-8-safe non-ASCII/DEL byte escaping;
//    Markdown labels sanitize via `panative::printable()`.

#include "media/panative/panative_reader.h"
#include "media/sound/resource_graph.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ai_arranger::media::selective {

inline constexpr const char* kSelectiveReportSchema = "aiarranger-selective-export-report/1";
inline constexpr const char* kHardwareUnverifiedBanner = "STRUCTURALLY VALID — NOT VERIFIED ON A PA5X";
inline constexpr const char* kStoredPcmProxyCaveat =
    "Stored PCM size (compressed 0180 + uncompressed 0110/0210 mix; decoded Pa5X RAM use unverified)";
inline constexpr const char* kOc31KmpDegradedNotice =
    "Legacy OC31-compressed RAM.KMP (converted from Pa700/Pa4X); Pa5X raw RAM.KMP is required for Multisample/Sample dependency resolution.";
inline constexpr const char* kPartialCoverageNotice =
    "Partial coverage — delete safety cannot be proven.";
inline constexpr const char* kSongBookUnrewrittenWarning =
    "SongBook entries are not decoded or remapped; SongBook references to remapped Sounds or renamed Styles must be updated on the Pa5X.";
inline constexpr const char* kSmfPcCaveatK9 =
    "Whether the Pa5X retains Style SMF Program Changes on Import SMF is UNPROVEN until hardware verification; verify each track in Style Edit.";

// ── Shared Enums ──────────────────────────────────────────────────────────────────────────────────────────────────────

enum class DiagnosticCategory {
    VERIFIED_MISSING,          // Proven absent resource or broken structural link
    UNSUPPORTED_FORMAT,        // Known proprietary/legacy/HOLD format boundary (not guessed)
    FACTORY_ASSUMED_EXTERNAL,  // ROM/Factory reference external to User library (info/inferred)
    AMBIGUOUS_OR_DUPLICATE     // Ambiguous metadata, duplicate IDs, unresolved bank, path/slot collisions
};

inline const char* diagnosticCategoryName(DiagnosticCategory c) {
    switch (c) {
        case DiagnosticCategory::VERIFIED_MISSING:         return "VERIFIED_MISSING";
        case DiagnosticCategory::UNSUPPORTED_FORMAT:       return "UNSUPPORTED_FORMAT";
        case DiagnosticCategory::FACTORY_ASSUMED_EXTERNAL: return "FACTORY_ASSUMED_EXTERNAL";
        case DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE:   return "AMBIGUOUS_OR_DUPLICATE";
    }
    return "AMBIGUOUS_OR_DUPLICATE";
}

enum class DiagnosticSeverity { Blocking, Warning, Info };

inline const char* diagnosticSeverityName(DiagnosticSeverity s) {
    switch (s) {
        case DiagnosticSeverity::Blocking: return "blocking";
        case DiagnosticSeverity::Warning:  return "warning";
        case DiagnosticSeverity::Info:     return "info";
    }
    return "warning";
}

enum class EvidenceLevel { Verified, Inferred, Unsupported };

inline const char* evidenceLevelName(EvidenceLevel e) {
    switch (e) {
        case EvidenceLevel::Verified:    return "verified";
        case EvidenceLevel::Inferred:    return "inferred";
        case EvidenceLevel::Unsupported: return "unsupported";
    }
    return "unsupported";
}

enum class ExportMode {
    AdvisoryPlan,          // Always allowed: read-only plan + diagnostics + closure manifest (no KST written)
    ImportKitBundle,       // Path A: Pa5X Import Kit (marker SMF + TXPROG/TXPERF/SF2/KMP + KIT.md)
    RootTreeSubKst,        // Path A CLI-only (L1, NOT HW-verified): .stg/.ksc/.pdc only, 0 unwritable User Sounds
    NativeSoundKst_HOLD    // Strictly refused fail-closed until OC31/KMP writer + hardware HOLD gates lift
};

inline const char* exportModeName(ExportMode m) {
    switch (m) {
        case ExportMode::AdvisoryPlan:        return "advisory-plan";
        case ExportMode::ImportKitBundle:     return "import-kit";
        case ExportMode::RootTreeSubKst:      return "root-tree-sub-kst";
        case ExportMode::NativeSoundKst_HOLD: return "native-sound-kst-hold";
    }
    return "advisory-plan";
}

enum class ConflictPolicy {
    BlockWithReport,           // Default (fail-closed): any slot or path collision blocks execution
    ReuseIdenticalTargetSlot,  // Safe remap: if target holds proven-identical Sound at another slot, rewrite upstream MIDI
    RemapToFreeSlotInNewLib    // Allocate free slot in NEW output library & rewrite upstream MIDI Bank/PC
};

inline const char* conflictPolicyName(ConflictPolicy p) {
    switch (p) {
        case ConflictPolicy::BlockWithReport:          return "block-with-report";
        case ConflictPolicy::ReuseIdenticalTargetSlot: return "reuse-identical-target-slot";
        case ConflictPolicy::RemapToFreeSlotInNewLib:  return "remap-to-free-slot-in-new-lib";
    }
    return "block-with-report";
}

enum class SelectedKind { Style, KeyboardSet, Pad, SoundPackage, DrumKitPackage, KmpFolder };

inline const char* selectedKindName(SelectedKind k) {
    switch (k) {
        case SelectedKind::Style:          return "Style";
        case SelectedKind::KeyboardSet:    return "KeyboardSet";
        case SelectedKind::Pad:            return "Pad";
        case SelectedKind::SoundPackage:   return "SoundPackage";
        case SelectedKind::DrumKitPackage: return "DrumKitPackage";
        case SelectedKind::KmpFolder:      return "KmpFolder";
    }
    return "Style";
}

struct SelectionTarget {
    SelectedKind kind = SelectedKind::Style;
    std::string relPath;     // Source-relative path (e.g. "STYLE/BankA/Demo.stg")
    std::string targetBank;  // Optional destination bank override ("" = preserve source bank)
    std::string targetName;  // Optional destination leaf name override ("" = preserve source name)

    auto operator<=>(const SelectionTarget&) const = default;
};

// ── Canonical Diagnostic Code Catalog ─────────────────────────────────────────────────────────────────────────────────

namespace diag_code {

// VERIFIED_MISSING
inline constexpr const char* kMissingUserSoundSlot    = "MISSING_USER_SOUND_SLOT";
inline constexpr const char* kMissingUserSoundBank    = "MISSING_USER_SOUND_BANK";
inline constexpr const char* kMissingPcmForKmpSample  = "MISSING_PCM_FOR_KMP_SAMPLE";
inline constexpr const char* kMissingPackageWavOrTx   = "MISSING_PACKAGE_WAV_OR_TX";
inline constexpr const char* kDanglingGraphEdge       = "DANGLING_GRAPH_EDGE";

// UNSUPPORTED_FORMAT
inline constexpr const char* kOc31KmpUnsupported          = "OC31_KMP_UNSUPPORTED";
inline constexpr const char* kDrumkitBodyUnparsed         = "DRUMKIT_BODY_UNPARSED";
inline constexpr const char* kLegacySoundKindUnsupported  = "LEGACY_SOUND_KIND_UNSUPPORTED";
inline constexpr const char* kNativeSoundWriteHold        = "NATIVE_SOUND_WRITE_HOLD";
inline constexpr const char* kPcmCompressed0180Estimate   = "PCM_COMPRESSED_0180_ESTIMATE";
inline constexpr const char* kTargetUninspectable         = "TARGET_UNINSPECTABLE";
inline constexpr const char* kSongBookUnrewritten         = "SONGBOOK_UNREWRITTEN";

// FACTORY_ASSUMED_EXTERNAL
inline constexpr const char* kFactoryRomSoundRef          = "FACTORY_ROM_SOUND_REF";
inline constexpr const char* kSoundNoRamRefInferredRom    = "SOUND_NO_RAM_REF_INFERRED_ROM";

// AMBIGUOUS_OR_DUPLICATE
inline constexpr const char* kDuplicateKmpDrumId               = "DUPLICATE_KMP_DRUM_ID";
inline constexpr const char* kUnresolvedSoundSlotAnalysed      = "UNRESOLVED_SOUND_SLOT_ANALYSED";
inline constexpr const char* kUnresolvedSoundSlotSpeculative   = "UNRESOLVED_SOUND_SLOT_SPECULATIVE";
inline constexpr const char* kUnresolvedMidiBankSelect         = "UNRESOLVED_MIDI_BANK_SELECT";
inline constexpr const char* kMalformedMidiLeaf                = "MALFORMED_MIDI_LEAF";
inline constexpr const char* kOrphanPcmEntry                   = "ORPHAN_PCM_ENTRY";
inline constexpr const char* kFatCaseCollisionR18              = "FAT_CASE_COLLISION_R18";
inline constexpr const char* kBankGridExhausted                = "BANK_GRID_EXHAUSTED";
inline constexpr const char* kOutOfSpecDrumkitBank             = "OUT_OF_SPEC_DRUMKIT_BANK";
inline constexpr const char* kSoundSlotOccupiedDifferent       = "SOUND_SLOT_OCCUPIED_DIFFERENT";
inline constexpr const char* kSoundSlotEquivalenceInsufficient = "SOUND_SLOT_EQUIVALENCE_INSUFFICIENT";
inline constexpr const char* kStyleOrObjectPathCollision       = "STYLE_OR_OBJECT_PATH_COLLISION";

}  // namespace diag_code

struct DiagnosticCodeSpec {
    const char* code;
    DiagnosticCategory category;
    DiagnosticSeverity defaultSeverity;
    EvidenceLevel defaultEvidence;
    const char* defaultRemediation;
};

inline const std::vector<DiagnosticCodeSpec>& diagnosticCatalog() {
    static const std::vector<DiagnosticCodeSpec> kCatalog = {
        // VERIFIED_MISSING
        {diag_code::kMissingUserSoundSlot, DiagnosticCategory::VERIFIED_MISSING,
         DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
         "Remap track to an occupied User/Factory Sound or supply missing Sound via Import Kit."},
        {diag_code::kMissingUserSoundBank, DiagnosticCategory::VERIFIED_MISSING,
         DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
         "Supply the missing SOUND/*.PCG bank file from backup or remap track to an existing Sound/Drum Kit."},
        {diag_code::kMissingPcmForKmpSample, DiagnosticCategory::VERIFIED_MISSING,
         DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
         "Restore missing PCM/*.pcm container or re-author Sound from clean WAV sources."},
        {diag_code::kMissingPackageWavOrTx, DiagnosticCategory::VERIFIED_MISSING,
         DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
         "Regenerate the Sound/DrumKit package before building the Import Kit."},
        {diag_code::kDanglingGraphEdge, DiagnosticCategory::VERIFIED_MISSING,
         DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
         "Inspect the broken dependency link; cannot export a self-contained closure until resolved."},

        // UNSUPPORTED_FORMAT
        {diag_code::kOc31KmpUnsupported, DiagnosticCategory::UNSUPPORTED_FORMAT,
         DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported,
         "Requires authentic Pa5X raw RAM.KMP (0x11080018 v0x0103) for sample-level closure; inspect Sound bank list only or carry whole KORF banks unmodified."},
        {diag_code::kDrumkitBodyUnparsed, DiagnosticCategory::UNSUPPORTED_FORMAT,
         DiagnosticSeverity::Warning, EvidenceLevel::Unsupported,
         "Drum Kit -> DrumSample internal edges are not parsed; sample closure is incomplete. Use Pa5X single-Sound load (MAN5 p.736) or TXPERF Import Kit."},
        {diag_code::kLegacySoundKindUnsupported, DiagnosticCategory::UNSUPPORTED_FORMAT,
         DiagnosticSeverity::Warning, EvidenceLevel::Unsupported,
         "Re-save Sound on Pa5X or remap track; cannot trace multisample dependencies offline."},
        {diag_code::kNativeSoundWriteHold, DiagnosticCategory::UNSUPPORTED_FORMAT,
         DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported,
         "Native .PCG/.KMP/.PCM writing is on HOLD. Use Advisory Transfer Plan (Pa5X single-Sound load, MAN5 p.736) or Import Kit path."},
        {diag_code::kPcmCompressed0180Estimate, DiagnosticCategory::UNSUPPORTED_FORMAT,
         DiagnosticSeverity::Info, EvidenceLevel::Inferred,
         "Memory figure is stored PCM proxy size, not verified Pa5X uncompressed RAM. Verify free memory on Pa5X Memory Info (MAN5 p.1174)."},
        {diag_code::kTargetUninspectable, DiagnosticCategory::UNSUPPORTED_FORMAT,
         DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported,
         "Target KST could not be fully inspected (OC31 RAM.KMP, corrupt container, or unreadable path); refusing fail-closed without overwriting."},
        {diag_code::kSongBookUnrewritten, DiagnosticCategory::UNSUPPORTED_FORMAT,
         DiagnosticSeverity::Warning, EvidenceLevel::Unsupported,
         kSongBookUnrewrittenWarning},

        // FACTORY_ASSUMED_EXTERNAL
        {diag_code::kFactoryRomSoundRef, DiagnosticCategory::FACTORY_ASSUMED_EXTERNAL,
         DiagnosticSeverity::Info, EvidenceLevel::Inferred,
         "External Pa5X Factory ROM Sound; no files copied. Assumes target Pa5X OS carries the same Factory ROM bank."},
        {diag_code::kSoundNoRamRefInferredRom, DiagnosticCategory::FACTORY_ASSUMED_EXTERNAL,
         DiagnosticSeverity::Info, EvidenceLevel::Inferred,
         "Consistent with a User Sound built purely on Factory/ROM multisamples (INFERRED, not hardware-verified)."},

        // AMBIGUOUS_OR_DUPLICATE
        {diag_code::kDuplicateKmpDrumId, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Warning, EvidenceLevel::Verified,
         "Nodes kept positional (DrumSample/i); coverageComplete is false; do not deduce deletion safety."},
        {diag_code::kUnresolvedSoundSlotAnalysed, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Warning, EvidenceLevel::Inferred,
         "Could be a Factory/ROM multisample ID or a dangling deleted-sample ID; verify audibly on Pa5X."},
        {diag_code::kUnresolvedSoundSlotSpeculative, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Info, EvidenceLevel::Unsupported,
         "Layout may not apply to this unanalysed Sound; hidden from blocking counters, shown in expert details only."},
        {diag_code::kUnresolvedMidiBankSelect, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Warning, EvidenceLevel::Unsupported,
         "Target bank cannot be classified as Factory vs User; fix MIDI track or supply an explicit Sound Map."},
        {diag_code::kMalformedMidiLeaf, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Warning, EvidenceLevel::Verified,
         "File cannot be safely remapped by the SMF rewriter; inspect or exclude affected leaf."},
        {diag_code::kOrphanPcmEntry, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Info, EvidenceLevel::Verified,
         "Not referenced by any Multisample or DrumSample in RAM.KMP; not included in selective closure."},
        {diag_code::kFatCaseCollisionR18, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
         "Rename one target item or bank before export to satisfy FAT32/exFAT case-insensitivity (rule R-18)."},
        {diag_code::kBankGridExhausted, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
         "All target User Sound (1,536) or User Drum Kit (768) slots are occupied or reserved; free target slots or narrow selection."},
        {diag_code::kOutOfSpecDrumkitBank, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Warning, EvidenceLevel::Unsupported,
         "MIDI address uses MSB 120 LSB 70..75 (outside documented 6 User Drum Kit banks USERDK01..USERDK06); verify target bank or remap."},
        {diag_code::kSoundSlotOccupiedDifferent, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
         "Target slot holds a different Sound (never overwritten). Switch policy to RemapToFreeSlotInNewLib or remap manually."},
        {diag_code::kSoundSlotEquivalenceInsufficient, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported,
         "Sound equivalence could not be proven (unanalysed Sound, USERDK, unresolved slots, or incomplete KMP); never auto-reused."},
        {diag_code::kStyleOrObjectPathCollision, DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
         DiagnosticSeverity::Blocking, EvidenceLevel::Verified,
         "Target already contains a different file at this path (never overwritten). Provide an explicit targetBank or targetName override."}
    };
    return kCatalog;
}

inline const DiagnosticCodeSpec* findDiagnosticCodeSpec(const std::string& code) {
    for (const auto& spec : diagnosticCatalog()) {
        if (code == spec.code) return &spec;
    }
    return nullptr;
}

// ── Core Report Data Structures ───────────────────────────────────────────────────────────────────────────────────────

struct Diagnostic {
    DiagnosticCategory category = DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE;
    std::string code;
    DiagnosticSeverity severity = DiagnosticSeverity::Warning;
    EvidenceLevel evidence = EvidenceLevel::Inferred;
    std::string subject;                    // ResourceId or source-relative path (path-free)
    std::string referencedTarget;           // Missing/conflicting target (if any)
    std::string summary;                    // Concise human-readable explanation
    std::string remediation;                // Concrete next step for the user
    std::vector<std::string> affectedRoots; // Sorted unique impacted Styles/KSCs/Pads

    auto operator<=>(const Diagnostic&) const = default;
};

struct CoverageSummary {
    size_t analysedSounds = 0;
    size_t unanalysedDrumKits = 0;
    size_t unanalysedNoKnownRef = 0;
    size_t unanalysedUnknownKind = 0;
    size_t decodeFailures = 0;
    size_t duplicateDrumIds = 0;
    size_t unresolvedInAnalysedSounds = 0;
    size_t unresolvedInUnanalysedSounds = 0;
    bool coverageComplete = false;
    bool deleteProofAvailable = false;      // ALWAYS false for Native-KST
};

struct DeduplicationGroup {
    std::string keyType;                    // "resource-id" | "sample-id8" | "audio-fnv1a64" | "file-sha256"
    std::string keyValue;                   // Hex hash or canonical ResourceId string
    std::string canonicalMember;            // Chosen representative (deterministic lexicographic first)
    std::vector<std::string> duplicates;    // Sorted members sharing this key
    uint64_t savedBytes = 0;
    bool appliedInExport = false;

    auto operator<=>(const DeduplicationGroup&) const = default;
};

struct SlotConflict {
    enum class Kind {
        SoundSlotOccupiedDifferent,    // Target USERnn/USERDKnn slot holds a different or unprovable Sound
        SoundSlotIdenticalElsewhere,   // Target holds proven-identical Sound at another slot (remap candidate)
        TargetBankAbsent,              // e.g. USERDK05.PCG absent in target KST
        StyleOrObjectPathCollision,    // Same TYPE/BANK/name (or case-folded FAT R-18) in target or batch
        TargetUninspectable,           // Target uses OC31 KMP or unreadable/corrupt PCG (fail-closed)
        BankGridExhausted              // All 1,536 User Sound or 768 User Drum Kit slots full
    };

    Kind kind = Kind::SoundSlotOccupiedDifferent;
    std::string address;               // e.g. "USER03/12 (MSB 121 LSB 66 PC 12)" or "STYLE/Pop/Ballad.stg"
    std::string sourceLabel;
    std::string targetLabel;
    std::string resolution;            // "BLOCKED" | "REUSE_EXISTING" | "REMAP_TO USER04/0" | "RENAME_REQUIRED" | "ALREADY_PRESENT_NOOP"
    bool blocking = true;
    std::string detail;                // Concise path-free explanation

    auto operator<=>(const SlotConflict&) const = default;
};

inline const char* slotConflictKindName(SlotConflict::Kind k) {
    switch (k) {
        case SlotConflict::Kind::SoundSlotOccupiedDifferent:  return "SoundSlotOccupiedDifferent";
        case SlotConflict::Kind::SoundSlotIdenticalElsewhere: return "SoundSlotIdenticalElsewhere";
        case SlotConflict::Kind::TargetBankAbsent:            return "TargetBankAbsent";
        case SlotConflict::Kind::StyleOrObjectPathCollision:  return "StyleOrObjectPathCollision";
        case SlotConflict::Kind::TargetUninspectable:         return "TargetUninspectable";
        case SlotConflict::Kind::BankGridExhausted:           return "BankGridExhausted";
    }
    return "SoundSlotOccupiedDifferent";
}

struct MemoryProjectionReport {
    bool sampleBytesAreNativeStoredProxy = false;
    uint64_t rawReferencedSampleBytes = 0;
    uint64_t uniqueLogicalSampleBytes = 0;
    uint64_t logicalDedupSavedBytes = 0;
    uint64_t identicalContentExtraBytes = 0;
    uint64_t uniqueSamplesCount = 0;
    uint64_t unknownSizeSamplesCount = 0;
    uint64_t targetBaselineSampleBytes = 0;
    uint64_t projectedCombinedBytes = 0;
    graph::Fit projectedFit = graph::Fit::Fits;

    const char* unitName() const {
        return sampleBytesAreNativeStoredProxy ? "native_kst_stored_pcm_proxy_bytes" : "uncompressed_pcm_bytes";
    }
    const char* fitVerdictName() const {
        if (sampleBytesAreNativeStoredProxy) return "unverified-proxy";
        switch (projectedFit) {
            case graph::Fit::Fits:             return "fits";
            case graph::Fit::Ambiguous:        return "ambiguous";
            case graph::Fit::NeedsCompression: return "needs-compression";
            case graph::Fit::Exceeds:          return "exceeds";
        }
        return "exceeds";
    }
    const char* caveatText() const {
        return sampleBytesAreNativeStoredProxy ? kStoredPcmProxyCaveat : "";
    }
};

struct SelectiveExportReport {
    ExportMode mode = ExportMode::AdvisoryPlan;
    ConflictPolicy conflictPolicy = ConflictPolicy::BlockWithReport;
    std::string planFingerprint;
    std::string sourceKind = "native-kst";   // "native-kst" | "provisional-workspace" | "pa-set-read-only"
    std::string sourceLabel = "<source>";    // Canonical path-free label (absolute paths scrubbed)
    std::string m1ManifestSha256;

    bool targetPresent = false;
    std::string targetLabel;                 // Canonical path-free label (absolute paths scrubbed)
    std::string targetLibraryKey;

    std::vector<SelectionTarget> selection;

    size_t soundsReferenced = 0;
    size_t factorySounds = 0;
    size_t userSounds = 0;
    size_t multisamples = 0;
    size_t samples = 0;

    CoverageSummary coverage;
    MemoryProjectionReport memoryProjection;
    std::vector<DeduplicationGroup> deduplication;
    std::vector<SlotConflict> conflicts;
    std::vector<Diagnostic> diagnostics;

    size_t blockingDiagnosticCount() const {
        return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& d) { return d.severity == DiagnosticSeverity::Blocking; }));
    }
    size_t warningDiagnosticCount() const {
        return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& d) { return d.severity == DiagnosticSeverity::Warning; }));
    }
    size_t infoDiagnosticCount() const {
        return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& d) { return d.severity == DiagnosticSeverity::Info; }));
    }
    size_t blockingConflictCount() const {
        return static_cast<size_t>(std::count_if(conflicts.begin(), conflicts.end(),
            [](const SlotConflict& c) { return c.blocking; }));
    }
    size_t blockingIssueCount() const {
        return blockingDiagnosticCount();
    }
    bool executable() const {
        return mode != ExportMode::NativeSoundKst_HOLD &&
               blockingDiagnosticCount() == 0 &&
               blockingConflictCount() == 0;
    }
};

// ── Sanitization, Normalization & Serialization Helpers ───────────────────────────────────────────────────────────────

namespace diag_detail {

// Detects whether a string looks like a host-local absolute filesystem path (Unix / Windows / UNC).
inline bool looksLikeAbsolutePath(const std::string& s) {
    if (s.empty()) return false;
    if (s[0] == '/' || s[0] == '\\') return true;
    if (s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' &&
        (s[2] == '/' || s[2] == '\\')) {
        return true;
    }
    return false;
}

// Guarantees report labels never leak host-local absolute paths.
inline std::string canonicalPathFreeLabel(const std::string& label, const char* fallback) {
    if (label.empty() || looksLikeAbsolutePath(label)) return fallback;
    if (label.find("/Users/") != std::string::npos ||
        label.find("/home/") != std::string::npos ||
        label.find("/tmp/") != std::string::npos ||
        label.find("/var/") != std::string::npos ||
        label.find("/Volumes/") != std::string::npos ||
        label.find(":\\") != std::string::npos) {
        return fallback;
    }
    return label;
}

// Escapes a string for strict RFC 8259 UTF-8 JSON using `media::graph::jsonEscape()` for quotes,
// backslashes, and control bytes (< 0x20), plus escapes 0x7F and any lone/invalid UTF-8 bytes
// (such as raw Latin-1 bytes in KORF item names) as \u00XX while preserving valid UTF-8 sequences.
inline std::string safeJsonEscape(const std::string& s) {
    // First validate UTF-8 and replace lone/invalid high bytes & 0x7F with safe \u00XX placeholders
    // while delegating ASCII control/quote/backslash escaping directly to media::graph::jsonEscape().
    std::string out;
    out.reserve(s.size() + 16);
    std::string cleanChunk;
    cleanChunk.reserve(s.size());

    auto flushClean = [&]() {
        if (!cleanChunk.empty()) {
            out += graph::jsonEscape(cleanChunk);
            cleanChunk.clear();
        }
    };

    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x7F) {
            flushClean();
            out += "\\u007f";
            ++i;
            continue;
        }
        if (c < 0x80) {
            cleanChunk += static_cast<char>(c);
            ++i;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0u) == 0xC0u && i + 1 < n) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c1 & 0xC0u) == 0x80u) {
                cp = ((c & 0x1Fu) << 6) | (c1 & 0x3Fu);
                if (cp >= 0x80u) len = 2;
            }
        } else if ((c & 0xF0u) == 0xE0u && i + 2 < n) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u) {
                cp = ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
                if (cp >= 0x800u && !(cp >= 0xD800u && cp <= 0xDFFFu)) len = 3;
            }
        } else if ((c & 0xF8u) == 0xF0u && i + 3 < n) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            const unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
            if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u && (c3 & 0xC0u) == 0x80u) {
                cp = ((c & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
                if (cp >= 0x10000u && cp <= 0x10FFFFu) len = 4;
            }
        }
        if (len > 0) {
            cleanChunk.append(s, i, len);
            i += len;
        } else {
            flushClean();
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", static_cast<unsigned>(c));
            out += u;
            ++i;
        }
    }
    flushClean();
    return out;
}

// Markdown table cell sanitizer built on panative::printable() (escapes control/non-ASCII/backslash
// and escapes '|' so table structure cannot be broken by untrusted KORF names).
inline std::string mdCell(const std::string& s) {
    const std::string p = panative::printable(s);
    std::string out;
    out.reserve(p.size());
    for (char c : p) {
        if (c == '|') out += "\\|";
        else out += c;
    }
    return out;
}

}  // namespace diag_detail

// Canonically sorts and deduplicates Diagnostics in deterministic severity-first order.
inline void canonicalizeDiagnostics(std::vector<Diagnostic>& diags) {
    for (Diagnostic& d : diags) {
        std::sort(d.affectedRoots.begin(), d.affectedRoots.end());
        d.affectedRoots.erase(std::unique(d.affectedRoots.begin(), d.affectedRoots.end()), d.affectedRoots.end());
    }
    std::sort(diags.begin(), diags.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return std::tie(a.severity, a.category, a.code, a.subject, a.referencedTarget,
                        a.summary, a.remediation, a.affectedRoots) <
               std::tie(b.severity, b.category, b.code, b.subject, b.referencedTarget,
                        b.summary, b.remediation, b.affectedRoots);
    });
    diags.erase(std::unique(diags.begin(), diags.end()), diags.end());
}

// Canonically sorts and deduplicates SlotConflicts.
inline void canonicalizeConflicts(std::vector<SlotConflict>& conflicts) {
    std::sort(conflicts.begin(), conflicts.end(), [](const SlotConflict& a, const SlotConflict& b) {
        const bool aNonBlock = !a.blocking;
        const bool bNonBlock = !b.blocking;
        return std::tie(aNonBlock, a.kind, a.address, a.sourceLabel, a.targetLabel, a.resolution, a.detail) <
               std::tie(bNonBlock, b.kind, b.address, b.sourceLabel, b.targetLabel, b.resolution, b.detail);
    });
    conflicts.erase(std::unique(conflicts.begin(), conflicts.end()), conflicts.end());
}

// Canonically sorts and deduplicates DeduplicationGroups.
inline void canonicalizeDeduplication(std::vector<DeduplicationGroup>& groups) {
    for (auto& g : groups) {
        std::sort(g.duplicates.begin(), g.duplicates.end());
        g.duplicates.erase(std::unique(g.duplicates.begin(), g.duplicates.end()), g.duplicates.end());
    }
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
}

// Returns a canonically sorted copy of `report` with path-free labels and honest deleteProofAvailable=false.
inline SelectiveExportReport canonicalizeReport(SelectiveExportReport r) {
    r.sourceLabel = diag_detail::canonicalPathFreeLabel(r.sourceLabel, "<source>");
    if (r.targetPresent) {
        r.targetLabel = diag_detail::canonicalPathFreeLabel(r.targetLabel, "<target>");
    } else {
        r.targetLabel.clear();
    }
    r.coverage.deleteProofAvailable = false;  // invariant: never claims delete proof
    std::sort(r.selection.begin(), r.selection.end());
    r.selection.erase(std::unique(r.selection.begin(), r.selection.end()), r.selection.end());
    canonicalizeDeduplication(r.deduplication);
    canonicalizeConflicts(r.conflicts);
    canonicalizeDiagnostics(r.diagnostics);
    return r;
}

// ── Deterministic JSON Serializer (`aiarranger-selective-export-report/1`) ────────────────────────────────────────────
// Byte-identical across repeated runs and shuffled input orderings. Zero local paths, zero timestamps.
inline std::string serializeSelectiveReportJson(const SelectiveExportReport& rawReport) {
    const SelectiveExportReport r = canonicalizeReport(rawReport);
    auto q = [](const std::string& s) { return "\"" + diag_detail::safeJsonEscape(s) + "\""; };
    auto bstr = [](bool b) -> const char* { return b ? "true" : "false"; };
    auto writeStrVec = [&](std::string& o, const std::vector<std::string>& v) {
        o += "[";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) o += ", ";
            o += q(v[i]);
        }
        o += "]";
    };

    std::string o;
    o.reserve(4096 + r.diagnostics.size() * 256 + r.conflicts.size() * 192);
    o += "{\n";
    o += "  \"schema\": " + q(kSelectiveReportSchema) + ",\n";
    o += "  \"label\": " + q(kHardwareUnverifiedBanner) + ",\n";
    o += "  \"hardwareVerified\": false,\n";
    o += "  \"mode\": " + q(exportModeName(r.mode)) + ",\n";
    o += "  \"conflictPolicy\": " + q(conflictPolicyName(r.conflictPolicy)) + ",\n";
    o += "  \"planFingerprint\": " + q(r.planFingerprint) + ",\n";

    // Source
    o += "  \"source\": {\n";
    o += "    \"kind\": " + q(r.sourceKind) + ",\n";
    o += "    \"label\": " + q(r.sourceLabel) + ",\n";
    o += "    \"m1ManifestSha256\": " + q(r.m1ManifestSha256) + ",\n";
    o += "    \"coverageComplete\": " + std::string(bstr(r.coverage.coverageComplete)) + ",\n";
    o += "    \"deleteProofAvailable\": false\n";
    o += "  },\n";

    // Target
    o += "  \"target\": {\n";
    o += "    \"present\": " + std::string(bstr(r.targetPresent)) + ",\n";
    o += "    \"label\": " + q(r.targetLabel) + ",\n";
    o += "    \"libraryKey\": " + q(r.targetLibraryKey) + "\n";
    o += "  },\n";

    // Coverage honesty breakdown (UX Gate #1)
    o += "  \"coverage\": {\n";
    o += "    \"coverageComplete\": " + std::string(bstr(r.coverage.coverageComplete)) + ",\n";
    o += "    \"deleteProofAvailable\": false,\n";
    o += "    \"analysedSounds\": " + std::to_string(r.coverage.analysedSounds) + ",\n";
    o += "    \"unanalysedDrumKits\": " + std::to_string(r.coverage.unanalysedDrumKits) + ",\n";
    o += "    \"unanalysedNoKnownRef\": " + std::to_string(r.coverage.unanalysedNoKnownRef) + ",\n";
    o += "    \"unanalysedUnknownKind\": " + std::to_string(r.coverage.unanalysedUnknownKind) + ",\n";
    o += "    \"decodeFailures\": " + std::to_string(r.coverage.decodeFailures) + ",\n";
    o += "    \"duplicateDrumIds\": " + std::to_string(r.coverage.duplicateDrumIds) + ",\n";
    o += "    \"unresolvedInAnalysedSounds\": " + std::to_string(r.coverage.unresolvedInAnalysedSounds) + ",\n";
    o += "    \"unresolvedInUnanalysedSounds\": " + std::to_string(r.coverage.unresolvedInUnanalysedSounds) + ",\n";
    o += "    \"notice\": " + q(r.coverage.coverageComplete ? "" : kPartialCoverageNotice) + "\n";
    o += "  },\n";

    // Selection
    o += "  \"selection\": [";
    for (size_t i = 0; i < r.selection.size(); ++i) {
        const auto& s = r.selection[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"kind\": " + q(selectedKindName(s.kind)) +
             ", \"relPath\": " + q(s.relPath) +
             ", \"targetBank\": " + q(s.targetBank) +
             ", \"targetName\": " + q(s.targetName) + "}";
    }
    o += r.selection.empty() ? "],\n" : "\n  ],\n";

    // Summary
    o += "  \"summary\": {\n";
    o += "    \"selectedItems\": " + std::to_string(r.selection.size()) + ",\n";
    o += "    \"soundsReferenced\": " + std::to_string(r.soundsReferenced) + ",\n";
    o += "    \"factorySounds\": " + std::to_string(r.factorySounds) + ",\n";
    o += "    \"userSounds\": " + std::to_string(r.userSounds) + ",\n";
    o += "    \"multisamples\": " + std::to_string(r.multisamples) + ",\n";
    o += "    \"samples\": " + std::to_string(r.samples) + ",\n";
    o += "    \"blockingIssues\": " + std::to_string(r.blockingDiagnosticCount()) + ",\n";
    o += "    \"blockingConflicts\": " + std::to_string(r.blockingConflictCount()) + ",\n";
    o += "    \"warnings\": " + std::to_string(r.warningDiagnosticCount()) + ",\n";
    o += "    \"infos\": " + std::to_string(r.infoDiagnosticCount()) + ",\n";
    o += "    \"executable\": " + std::string(bstr(r.executable())) + "\n";
    o += "  },\n";

    // Memory projection (§4.4, UX Gate #3)
    const auto& mp = r.memoryProjection;
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
    o += "    \"caveat\": " + q(mp.caveatText()) + "\n";
    o += "  },\n";

    // Deduplication
    o += "  \"deduplication\": [";
    for (size_t i = 0; i < r.deduplication.size(); ++i) {
        const auto& g = r.deduplication[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"keyType\": " + q(g.keyType) +
             ", \"keyValue\": " + q(g.keyValue) +
             ", \"canonicalMember\": " + q(g.canonicalMember) +
             ", \"duplicates\": ";
        writeStrVec(o, g.duplicates);
        o += ", \"savedBytes\": " + std::to_string(g.savedBytes) +
             ", \"appliedInExport\": " + std::string(bstr(g.appliedInExport)) + "}";
    }
    o += r.deduplication.empty() ? "],\n" : "\n  ],\n";

    // Conflicts
    o += "  \"conflicts\": [";
    for (size_t i = 0; i < r.conflicts.size(); ++i) {
        const auto& c = r.conflicts[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"kind\": " + q(slotConflictKindName(c.kind)) +
             ", \"address\": " + q(c.address) +
             ", \"sourceLabel\": " + q(c.sourceLabel) +
             ", \"targetLabel\": " + q(c.targetLabel) +
             ", \"resolution\": " + q(c.resolution) +
             ", \"blocking\": " + std::string(bstr(c.blocking)) +
             ", \"detail\": " + q(c.detail) + "}";
    }
    o += r.conflicts.empty() ? "],\n" : "\n  ],\n";

    // Diagnostics
    o += "  \"diagnostics\": [";
    for (size_t i = 0; i < r.diagnostics.size(); ++i) {
        const auto& d = r.diagnostics[i];
        o += i ? ",\n    {\n" : "\n    {\n";
        o += "      \"category\": " + q(diagnosticCategoryName(d.category)) + ",\n";
        o += "      \"code\": " + q(d.code) + ",\n";
        o += "      \"severity\": " + q(diagnosticSeverityName(d.severity)) + ",\n";
        o += "      \"evidence\": " + q(evidenceLevelName(d.evidence)) + ",\n";
        o += "      \"subject\": " + q(d.subject) + ",\n";
        o += "      \"referencedTarget\": " + q(d.referencedTarget) + ",\n";
        o += "      \"summary\": " + q(d.summary) + ",\n";
        o += "      \"remediation\": " + q(d.remediation) + ",\n";
        o += "      \"affectedRoots\": ";
        writeStrVec(o, d.affectedRoots);
        o += "\n    }";
    }
    o += r.diagnostics.empty() ? "]\n" : "\n  ]\n";
    o += "}\n";
    return o;
}

// ── Deterministic Markdown Serializer ─────────────────────────────────────────────────────────────────────────────────
// Sanitizes untrusted labels via `panative::printable()`. Always includes the unverified banner and coverage notice.
inline std::string serializeSelectiveReportMarkdown(const SelectiveExportReport& rawReport) {
    const SelectiveExportReport r = canonicalizeReport(rawReport);
    std::string o;
    o.reserve(4096 + r.diagnostics.size() * 256 + r.conflicts.size() * 192);

    o += "# Selective Export Report\n\n";
    o += "> **Status:** `" + std::string(kHardwareUnverifiedBanner) + "` (`hardwareVerified: false`)\n";
    o += "> **Schema:** `" + std::string(kSelectiveReportSchema) + "` | **Mode:** `" +
         std::string(exportModeName(r.mode)) + "` | **Policy:** `" +
         std::string(conflictPolicyName(r.conflictPolicy)) + "` | **Executable:** `" +
         std::string(r.executable() ? "true" : "false") + "`\n\n";

    // Coverage honesty banner (UX Gate #1)
    o += "## Coverage & Honesty Notice\n\n";
    o += "- **Coverage Complete:** `" + std::string(r.coverage.coverageComplete ? "true" : "false") + "`\n";
    o += "- **Delete Proof Available:** `false` (Native-KST delete safety is never concluded)\n";
    if (!r.coverage.coverageComplete) {
        o += "- **Notice:** " + std::string(kPartialCoverageNotice) + "\n";
    }
    o += "- **Sounds:** analysed=" + std::to_string(r.coverage.analysedSounds) +
         ", unanalysedDrumKits=" + std::to_string(r.coverage.unanalysedDrumKits) +
         ", unanalysedNoKnownRef=" + std::to_string(r.coverage.unanalysedNoKnownRef) +
         ", unanalysedUnknownKind=" + std::to_string(r.coverage.unanalysedUnknownKind) +
         ", decodeFailures=" + std::to_string(r.coverage.decodeFailures) + "\n";
    o += "- **Unresolved Sound Slots:** analysed=" + std::to_string(r.coverage.unresolvedInAnalysedSounds) +
         ", unanalysed(speculative)=" + std::to_string(r.coverage.unresolvedInUnanalysedSounds) +
         ", duplicateDrumIds=" + std::to_string(r.coverage.duplicateDrumIds) + "\n\n";

    // Summary
    o += "## Summary\n\n";
    o += "- **Source:** `" + panative::printable(r.sourceKind) + "` (`" + panative::printable(r.sourceLabel) + "`)\n";
    if (r.targetPresent) {
        o += "- **Target:** `" + panative::printable(r.targetLabel) + "` (`libraryKey: " +
             panative::printable(r.targetLibraryKey) + "`)\n";
    } else {
        o += "- **Target:** none\n";
    }
    if (!r.planFingerprint.empty()) {
        o += "- **Plan Fingerprint:** `" + panative::printable(r.planFingerprint) + "`\n";
    }
    o += "- **Selected Items:** " + std::to_string(r.selection.size()) +
         " | **Sounds:** " + std::to_string(r.soundsReferenced) +
         " (Factory " + std::to_string(r.factorySounds) + ", User " + std::to_string(r.userSounds) + ")" +
         " | **Multisamples:** " + std::to_string(r.multisamples) +
         " | **Samples:** " + std::to_string(r.samples) + "\n";
    o += "- **Findings:** Blocking=" + std::to_string(r.blockingDiagnosticCount()) +
         ", Warnings=" + std::to_string(r.warningDiagnosticCount()) +
         ", Info=" + std::to_string(r.infoDiagnosticCount()) +
         ", BlockingConflicts=" + std::to_string(r.blockingConflictCount()) + "\n\n";

    // Diagnostics grouped by BLOCKING -> WARNINGS -> INFO
    auto emitDiagSection = [&](const char* heading, DiagnosticSeverity sev) {
        o += "### " + std::string(heading) + "\n\n";
        size_t count = 0;
        for (const auto& d : r.diagnostics) {
            if (d.severity != sev) continue;
            ++count;
            o += "- **`[" + std::string(diagnosticCategoryName(d.category)) + "] " +
                 panative::printable(d.code) + "`** (`evidence: " +
                 std::string(evidenceLevelName(d.evidence)) + "`)\n";
            if (!d.subject.empty()) {
                o += "  - Subject: `" + panative::printable(d.subject) + "`";
                if (!d.referencedTarget.empty()) {
                    o += " -> `" + panative::printable(d.referencedTarget) + "`";
                }
                o += "\n";
            }
            o += "  - Summary: " + panative::printable(d.summary) + "\n";
            if (!d.remediation.empty()) {
                o += "  - Remediation: " + panative::printable(d.remediation) + "\n";
            }
            if (!d.affectedRoots.empty()) {
                o += "  - Affected Roots: ";
                for (size_t i = 0; i < d.affectedRoots.size(); ++i) {
                    if (i) o += ", ";
                    o += "`" + panative::printable(d.affectedRoots[i]) + "`";
                }
                o += "\n";
            }
        }
        if (count == 0) o += "_None._\n";
        o += "\n";
    };

    o += "## Diagnostics\n\n";
    emitDiagSection("BLOCKING", DiagnosticSeverity::Blocking);
    emitDiagSection("WARNINGS", DiagnosticSeverity::Warning);
    emitDiagSection("INFO", DiagnosticSeverity::Info);

    // Conflict Matrix
    o += "## Conflict Matrix\n\n";
    if (r.conflicts.empty()) {
        o += "_No conflicts detected._\n\n";
    } else {
        o += "| Blocking | Kind | Address | Source | Target | Resolution | Detail |\n";
        o += "|---|---|---|---|---|---|---|\n";
        for (const auto& c : r.conflicts) {
            o += "| " + std::string(c.blocking ? "YES" : "no") +
                 " | `" + std::string(slotConflictKindName(c.kind)) + "`" +
                 " | `" + diag_detail::mdCell(c.address) + "`" +
                 " | " + diag_detail::mdCell(c.sourceLabel) +
                 " | " + diag_detail::mdCell(c.targetLabel) +
                 " | `" + diag_detail::mdCell(c.resolution) + "`" +
                 " | " + diag_detail::mdCell(c.detail) + " |\n";
        }
        o += "\n";
    }

    // Deduplication Table
    o += "## Deduplication Savings\n\n";
    if (r.deduplication.empty()) {
        o += "_No duplicate resource groups reported._\n\n";
    } else {
        o += "| Key Type | Key | Canonical Member | Duplicates | Saved Bytes | Applied |\n";
        o += "|---|---|---|---|---|---|\n";
        for (const auto& g : r.deduplication) {
            o += "| `" + diag_detail::mdCell(g.keyType) + "`" +
                 " | `" + diag_detail::mdCell(g.keyValue) + "`" +
                 " | `" + diag_detail::mdCell(g.canonicalMember) + "`" +
                 " | " + std::to_string(g.duplicates.size()) +
                 " | " + std::to_string(g.savedBytes) +
                 " | " + std::string(g.appliedInExport ? "yes" : "advisory-only") + " |\n";
        }
        o += "\n";
    }

    // Memory Projection
    const auto& mp = r.memoryProjection;
    o += "## Memory Projection\n\n";
    o += "- **Unit:** `" + std::string(mp.unitName()) + "` (`hardwareVerified: false`)\n";
    o += "- **Fit Verdict:** `" + std::string(mp.fitVerdictName()) + "`\n";
    o += "- **Raw Referenced Bytes:** " + std::to_string(mp.rawReferencedSampleBytes) +
         " | **Unique Logical Bytes:** " + std::to_string(mp.uniqueLogicalSampleBytes) +
         " | **Logical Dedup Saved:** " + std::to_string(mp.logicalDedupSavedBytes) + "\n";
    o += "- **Target Baseline Bytes:** " + std::to_string(mp.targetBaselineSampleBytes) +
         " | **Projected Combined Bytes:** " + std::to_string(mp.projectedCombinedBytes) + "\n";
    if (mp.sampleBytesAreNativeStoredProxy) {
        o += "- **Caveat:** " + std::string(kStoredPcmProxyCaveat) + "\n";
    }
    o += "\n";

    return o;
}

}  // namespace ai_arranger::media::selective

#endif  // AI_ARRANGER_MEDIA_SOUND_SELECTIVE_DIAGNOSTICS_H
