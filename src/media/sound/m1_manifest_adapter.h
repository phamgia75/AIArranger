#ifndef AI_ARRANGER_MEDIA_SOUND_M1_MANIFEST_ADAPTER_H
#define AI_ARRANGER_MEDIA_SOUND_M1_MANIFEST_ADAPTER_H

// Slice M2b: Thin, Read-Only Adapter over M1's Native-KST Dependency Closure Manifest
// (`native_kst_closure.h` + `native_refs.h`), Provisional Package Graphs (`package_graph.h`),
// and M2a's Canonical Types (`selective_diagnostics.h` + `slot_conflicts.h`).
//
// Design & Safety Invariants (M2_SELECTIVE_STYLE_EXPORT_DESIGN.md §2, §3, §8 Slice M2b):
//  * Strictly READ-ONLY: zero disk writes, zero mutations to source/target libraries, zero OC31 encoder.
//  * Single Source of Truth with M2a: reuses SlotAddress, CoverageSummary, PartialNativeInspect,
//    SoundEquivalenceProfile, MultisampleFingerprint, ZoneFingerprint, TargetOccupancyMap, and
//    finalizeSoundEquivalenceProfile() directly — zero duplicated target/equivalence definitions.
//  * Delete safety: Native-KST Graph NEVER declares scanned/complete; deleteCheck() NEVER returns Unreferenced.
//  * Strict boundary classification: NativeUserSound, NativeUserDrumKit, AmbiguousOrOutOfSpec, FactoryRom,
//    UnknownOrUnanalysed — NEVER silently mapping or substituting missing/ambiguous Sounds!
//  * Explicit separation of UnresolvedSlot::inUnanalysedSound == false (analysed) vs true (speculative).
//  * Cooperative cancellation (`KeepGoing` callback checked every <= 256 items), dropping partial state cleanly.
//  * Path-free output: NEVER stores or emits local filesystem paths; only canonical relative paths.

#include "media/panative/kst_closure.h"
#include "media/sha256.h"
#include "media/sound/native_kst_closure.h"
#include "media/sound/native_refs.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "media/sound/package_graph.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "media/sound/resource_graph.h"
#include "media/sound/selective_diagnostics.h"
#include "media/sound/slot_conflicts.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ai_arranger::media::selective {

using KeepGoing = std::function<bool()>;

constexpr size_t kMaxCancelCheckStride = 256;
constexpr size_t kDefaultCancelCheckStride = 64;

enum class AdapterSourceKind {
    NativeKst,
    ProvisionalWorkspace
};

inline const char* adapterSourceKindName(AdapterSourceKind k) {
    switch (k) {
        case AdapterSourceKind::NativeKst:            return "native-kst";
        case AdapterSourceKind::ProvisionalWorkspace: return "provisional-workspace";
    }
    return "unknown";
}

enum class RootItemKind {
    Style,               // Native KST .stg
    KeyboardSet,         // Native KST .ksc
    Pad,                 // Native KST .pdc
    ProvisionalMidi,     // Provisional marker-SMF .mid
    ProvisionalUasf,     // Provisional .uasf Style
    ProvisionalSoundPkg, // Standalone provisional Sound package
    ProvisionalDrumPkg   // Standalone provisional DrumKit package
};

inline const char* rootItemKindName(RootItemKind k) {
    switch (k) {
        case RootItemKind::Style:               return "Style";
        case RootItemKind::KeyboardSet:         return "KeyboardSet";
        case RootItemKind::Pad:                 return "Pad";
        case RootItemKind::ProvisionalMidi:     return "ProvisionalMidi";
        case RootItemKind::ProvisionalUasf:     return "ProvisionalUasf";
        case RootItemKind::ProvisionalSoundPkg: return "ProvisionalSoundPkg";
        case RootItemKind::ProvisionalDrumPkg:  return "ProvisionalDrumPkg";
    }
    return "Style";
}

inline SelectedKind toSelectedKind(RootItemKind k) {
    switch (k) {
        case RootItemKind::Style:
        case RootItemKind::ProvisionalMidi:
        case RootItemKind::ProvisionalUasf:     return SelectedKind::Style;
        case RootItemKind::KeyboardSet:         return SelectedKind::KeyboardSet;
        case RootItemKind::Pad:                 return SelectedKind::Pad;
        case RootItemKind::ProvisionalSoundPkg: return SelectedKind::SoundPackage;
        case RootItemKind::ProvisionalDrumPkg:  return SelectedKind::DrumKitPackage;
    }
    return SelectedKind::Style;
}

// Strict boundary classification for every MIDI / Sound selection (M2 Design §3.2).
// Never silently substitutes or coerces missing/ambiguous Sounds into FactoryRom.
enum class SoundBoundaryKind {
    NativeUserSound,      // MSB 121, LSB 64..75 -> USER01..USER12
    NativeUserDrumKit,    // MSB 120, LSB 64..69 -> USERDK01..USERDK06
    AmbiguousOrOutOfSpec, // MSB 120, LSB 70..75 -> outside documented USERDK01..06
    FactoryRom,           // MSB 121/120, LSB 0..63 (external Pa5X Factory ROM; Inferred)
    UnknownOrUnanalysed   // Missing CC0/CC32, MSB outside {120,121}, LSB > 75, or invalid byte
};

inline const char* soundBoundaryKindName(SoundBoundaryKind b) {
    switch (b) {
        case SoundBoundaryKind::NativeUserSound:      return "NativeUserSound";
        case SoundBoundaryKind::NativeUserDrumKit:    return "NativeUserDrumKit";
        case SoundBoundaryKind::AmbiguousOrOutOfSpec: return "AmbiguousOrOutOfSpec";
        case SoundBoundaryKind::FactoryRom:           return "FactoryRom";
        case SoundBoundaryKind::UnknownOrUnanalysed:  return "UnknownOrUnanalysed";
    }
    return "UnknownOrUnanalysed";
}

enum class EdgeConfidence {
    Confirmed,
    Inferred,
    Unsupported
};

inline const char* edgeConfidenceName(EdgeConfidence c) {
    switch (c) {
        case EdgeConfidence::Confirmed:   return "Confirmed";
        case EdgeConfidence::Inferred:    return "Inferred";
        case EdgeConfidence::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

struct SoundSelectionRecord {
    graph::Via via = graph::Via::StyleTrack;
    graph::MidiSoundAddress address;
    SoundBoundaryKind boundary = SoundBoundaryKind::UnknownOrUnanalysed;
    EdgeConfidence confidence = EdgeConfidence::Unsupported;
    bool bankMissing = false;                         // true when CC0 or CC32 unset before PC
    bool isDrumKit = false;
    std::optional<SlotAddress> slotAddress;           // M2a canonical SlotAddress when User or out-of-spec DK
    std::string userBank;                             // e.g. "USER01" or "USERDK01" (when applicable)
    std::optional<graph::ResourceId> targetSoundId;   // pa5x:<bank>/Sound/<pc> when user/out-of-spec
    std::string provisionalPackageId;                 // non-empty when bound to a provisional package
    bool resolved = false;                            // true iff target exists in source catalog/packages
    graph::MissingRefReason missingReason = graph::MissingRefReason::None;
    size_t occurrences = 0;
    bool fromStyleMainTrack = false;                  // direct Style element track / ProgramChange
    bool fromEmbeddedKbdSet = false;                  // Style's embedded KbdSet1-4.scn / *.ksc / *.ksa
    bool fromEmbeddedPad = false;                     // Style's embedded PadSet1.scn / *.pdc
    std::vector<std::string> leafOrigins;             // canonical tree-internal leaf paths (path-free)
    size_t totalDistinctLeafOrigins = 0;
};

struct RootItemRecord {
    graph::ResourceId id;
    RootItemKind kind = RootItemKind::Style;
    std::string relPath;                              // canonical normalized forward-slash relative path
    std::string locationCategory;                     // "STYLE", "KEYBOARDSET", "PAD", "SETTINGS", "PROVISIONAL"
    std::string bankFolder;
    std::string displayName;
    std::string contentSha256;                        // 64-hex SHA-256 of root file bytes ("" if not hashed)
    uint64_t sizeBytes = 0;
    bool parseOk = true;
    std::string parseError;
    size_t midiLeafCount = 0;
    size_t malformedMidiLeafCount = 0;
    size_t rawSelectCount = 0;
    bool hasUnresolvedBankRef = false;
    bool hasUnknownOriginRef = false;
    std::vector<SoundSelectionRecord> soundSelections;
    std::vector<std::string> directPackageIds;        // provisional package IDs referenced directly
};

struct SoundClosureRecord {
    graph::ResourceId id;
    std::optional<SlotAddress> slotAddress;           // M2a canonical SlotAddress
    std::string bank;
    uint32_t slot = 0;
    uint32_t kindChunkId = 0;
    std::string name;
    std::string sourceFile;                           // KST-relative path e.g. "SOUND/USER01.PCG"
    bool isDrumKit = false;
    bool decoded = false;
    bool analysed = false;
    std::string unanalysedGap;                        // "" when analysed; else gapName() / "KmpUnavailable"
    EdgeConfidence soundToMsConfidence = EdgeConfidence::Unsupported;
    EdgeConfidence msToSampleConfidence = EdgeConfidence::Unsupported;
    std::string decodedBodySha256;                    // 64-hex SHA-256 of decoded Sound body ("" if unavailable)
    // M2a canonical Sound equivalence profile (exact body SHA-256 + MS/Zone topKey/Sample Id8 + PCM sizes)
    SoundEquivalenceProfile equivalence;
    std::vector<graph::ResourceId> oscillatorIds;     // sorted unique
    std::vector<graph::ResourceId> multisampleIds;    // sorted unique
    std::vector<graph::ResourceId> drumSampleIds;     // sorted unique
    std::vector<graph::ResourceId> sampleIds;         // sorted unique
    std::vector<std::string> pcmFiles;                // sorted unique KST-relative PCM paths
    uint64_t storedPcmBytesProxy = 0;
    // Explicit separation of unresolved slots (Gate UX-1 / M2 Design §6.1)
    std::vector<graph::UnresolvedSlot> unresolvedSlotsInAnalysed;
    std::vector<graph::UnresolvedSlot> unresolvedSlotsInUnanalysed;
};

struct MultisampleRecord {
    graph::ResourceId id;
    uint32_t index = 0;
    graph::Id8 id8{};
    std::string name;
    std::vector<uint32_t> zoneSampleIndices;          // exact zone table (kNoZoneSample preserved)
    std::vector<uint8_t> zoneTopKeys;                 // exact zone topKey from raw Pa5xKmp (empty if unavailable)
    std::vector<graph::ResourceId> sampleIds;         // sorted unique reachable Sample ResourceIds
    EdgeConfidence confidence = EdgeConfidence::Confirmed;
};

struct DrumSampleRecord {
    graph::ResourceId id;
    uint32_t index = 0;
    graph::Id8 id8{};
    std::string name;
    uint32_t leftSample = graph::kNoZoneSample;
    uint32_t rightSample = graph::kNoZoneSample;
    std::vector<graph::ResourceId> sampleIds;         // sorted unique reachable Sample ResourceIds
    EdgeConfidence confidence = EdgeConfidence::Confirmed;
};

struct SampleRecord {
    graph::ResourceId id;
    uint32_t index = 0;
    graph::Id8 id8{};
    std::string name;
    std::string pcmRelPath;                           // KST-relative PCM file or package-relative WAV
    uint64_t payloadBytes = 0;                        // exact PCM item payload byte count (or WAV uncompressed bytes)
    uint32_t sampleRate = 0;
    bool isStoredSizeProxy = false;                   // true for Native KST PCMs; false for provisional WAVs
    bool hasPcm = false;                              // false when dangling / missing PCM
    std::optional<graph::SampleInfo> info;
};

struct ProvisionalPackageRecord {
    std::string packageId;                            // canonical packageId (e.g. "SynthLead#a1b2c3d4e5f6")
    std::string packageSha256;                        // 64-hex SHA-256 of PACKAGE.sha256 or canonical manifest
    std::string relDir;                               // workspace-relative dir (never absolute local path)
    graph::Kind topKind = graph::Kind::Sound;         // Kind::Sound or Kind::DrumKit
    std::string displayName;
    bool valid = false;
    std::string error;
    graph::Graph graph;
    std::vector<graph::ResourceId> sampleIds;         // sorted unique provisional Sample IDs
    uint64_t totalUncompressedBytes = 0;
};

// Reuses M2a's canonical CoverageSummary & PartialNativeInspect directly while exposing M1 scan metadata.
struct AdapterCoverageSummary {
    bool readOnly = true;
    bool deleteProofAvailable = false;                // ALWAYS false for Native-KST
    bool upstreamScanned = false;
    bool kmpPresent = false;
    bool kmpParsed = false;
    bool kmpIsOc31 = false;
    std::string kmpError;
    bool songBookPresent = false;
    bool drumKitBodyEdgesSupported = false;
    bool coverageComplete = false;
    size_t soundsOccupied = 0;
    size_t soundsAnalysed = 0;
    size_t soundsUnanalysed = 0;
    size_t unanalysedDrumKits = 0;
    size_t unanalysedNoKnownRef = 0;
    size_t unanalysedOther = 0;
    size_t decodeFailures = 0;
    size_t unresolvedSlotsInAnalysed = 0;
    size_t unresolvedSlotsInUnanalysed = 0;
    size_t duplicateDrumIdGroups = 0;
    size_t missingPcmSamples = 0;
    size_t bytesReadDownstream = 0;
    size_t bytesReadUpstream = 0;

    CoverageSummary toM2aCoverageSummary() const {
        CoverageSummary cs;
        cs.analysedSounds = soundsAnalysed;
        cs.unanalysedDrumKits = unanalysedDrumKits;
        cs.unanalysedNoKnownRef = unanalysedNoKnownRef;
        cs.unanalysedUnknownKind = unanalysedOther;
        cs.decodeFailures = decodeFailures;
        cs.duplicateDrumIds = duplicateDrumIdGroups;
        cs.unresolvedInAnalysedSounds = unresolvedSlotsInAnalysed;
        cs.unresolvedInUnanalysedSounds = unresolvedSlotsInUnanalysed;
        cs.coverageComplete = coverageComplete;
        cs.deleteProofAvailable = false;
        return cs;
    }
};

struct M1AdapterView {
    bool ok = false;
    bool cancelled = false;
    AdapterSourceKind sourceKind = AdapterSourceKind::NativeKst;
    AdapterCoverageSummary coverage;
    PartialNativeInspect partialInspect;                            // M2a UX Gate #2 partial inspection payload

    std::vector<RootItemRecord> roots;                              // sorted by relPath
    std::map<std::string, size_t> rootIndexByRelPath;               // relPath -> index in `roots`
    std::map<graph::ResourceId, SoundClosureRecord> sounds;         // Native KST occupied Sound catalog
    std::map<graph::ResourceId, graph::DrumKitKeyMap> drumKeyMaps;  // parsed read-only USERDK key maps (including ROM-only kits)
    std::set<std::string> presentSoundBanks;                        // e.g. "USER01", "USERDK01"
    std::map<graph::ResourceId, MultisampleRecord> multisamples;
    std::map<graph::ResourceId, DrumSampleRecord> drumSamples;
    std::map<graph::ResourceId, SampleRecord> samples;

    // Provisional packages (when sourceKind == ProvisionalWorkspace)
    std::map<std::string, ProvisionalPackageRecord> packagesById;   // canonical packageId -> record
    std::map<std::string, std::string> packageIdByRelDir;           // relDir -> packageId

    // Unified read-only Graph (for Native-KST: deleteCheck() NEVER returns Unreferenced)
    std::optional<graph::Graph> graph;

    // Anomaly tables mirrored cleanly from M1
    std::vector<graph::MissingPcmSample> missingPcmSamples;
    std::vector<graph::DuplicateKmpId> duplicateDrumIds;
    std::vector<graph::MalformedTreeEntry> malformedTrees;
    std::vector<graph::MalformedMidiLeafEntry> malformedMidiLeaves;
    std::vector<std::string> containerErrors;
    std::vector<std::string> skippedPaths;

    // Relative-path SHA-256 map for files hashed during adaptation (TOCTOU & fingerprint input; zero local paths)
    std::map<std::string, std::string> sourceFileSha256;
    std::string manifestSha256;                                     // deterministic 64-hex digest of this view

    bool isNativeKst() const { return sourceKind == AdapterSourceKind::NativeKst; }
    bool isProvisional() const { return sourceKind == AdapterSourceKind::ProvisionalWorkspace; }
    const RootItemRecord* findRoot(const std::string& relPath) const {
        const auto it = rootIndexByRelPath.find(relPath);
        return it == rootIndexByRelPath.end() ? nullptr : &roots[it->second];
    }
    const graph::DrumKitKeyMap* findDrumKitKeyMap(const graph::ResourceId& soundId) const {
        const auto it = drumKeyMaps.find(soundId);
        return it == drumKeyMaps.end() ? nullptr : &it->second;
    }
};

struct M1AdapterOptions {
    graph::NativeKstClosureOptions closureOptions;
    bool computeSoundBodyHashes = true;        // hash decoded SOUND/*.PCG bodies + zone topKeys for M2a equivalence
    bool computeAllRootFileHashes = false;     // hash every .stg/.ksc/.pdc on disk (in-memory blobs are always hashed)
    size_t cancelCheckStride = kDefaultCancelCheckStride; // clamped to [1, 256]
};

// In-memory Provisional Workspace inputs (for synthetic tests and callers with already-loaded manifests/SMFs)
struct ProvisionalPackageInput {
    std::string relDir;                        // workspace-relative path e.g. "SOUNDS/SynthLead"
    std::string resourcesTsv;                  // RESOURCES.tsv text (parsed via sound::parseManifest)
    std::string packageSha256;                 // optional explicit PACKAGE.sha256 digest (auto-hashed if empty)
    std::optional<graph::Graph> prebuiltGraph; // optional alternative to resourcesTsv
    std::string prebuiltPackageId;
    graph::Kind prebuiltTopKind = graph::Kind::Sound;
    std::string prebuiltDisplayName;
};

struct ProvisionalRootInput {
    std::string relPath;                       // e.g. "STYLES/DemoBallad.mid" or "STYLES/DemoBallad.uasf"
    RootItemKind kind = RootItemKind::ProvisionalMidi;
    std::vector<uint8_t> smfBytes;             // optional: parsed strictly via panative::closure::selects()
    std::vector<graph::MidiSoundAddress> explicitSelections; // optional (for .uasf or explicit SoundMap)
    std::map<graph::MidiSoundAddress, std::string> addressToPackageId;
    std::vector<std::string> directPackageIds; // package IDs referenced directly by this root
    std::string contentSha256;                 // optional override; computed from smfBytes/metadata if empty
};

struct ProvisionalWorkspaceInput {
    std::vector<ProvisionalPackageInput> packages;
    std::vector<ProvisionalRootInput> roots;
};

// Optional extra metadata extracted from raw KMP/PCG/PCM bytes to build byte-exact M2a SoundEquivalenceProfiles
struct NativeEquivalenceExtra {
    std::map<graph::ResourceId, std::string> soundBodySha256;
    std::map<std::string, std::string> fileSha256;
    std::map<std::string, uint64_t> fileSizes;
    std::vector<std::vector<uint8_t>> msZoneTopKeys;      // parallel to kmpIndex.multisamples
    std::map<graph::Id8, uint32_t> exactPcmPayloadBytes;  // exact KORF item payloadBytes (handles odd-byte PCM too)
    std::set<std::string> extraPresentSoundBanks;
};

namespace adapter_detail {

inline size_t clampStride(size_t s) {
    if (s == 0) return 1;
    if (s > kMaxCancelCheckStride) return kMaxCancelCheckStride;
    return s;
}

inline uint64_t satAdd(uint64_t a, uint64_t b) {
    return (std::numeric_limits<uint64_t>::max() - a < b)
               ? std::numeric_limits<uint64_t>::max()
               : (a + b);
}

inline uint64_t satMul(uint64_t a, uint64_t b) {
    if (a == 0 || b == 0) return 0;
    return (std::numeric_limits<uint64_t>::max() / a < b)
               ? std::numeric_limits<uint64_t>::max()
               : (a * b);
}

inline bool tickCancel(const KeepGoing& keepGoing, size_t& counter, size_t stride) {
    if (!keepGoing) return true;
    const size_t cur = counter++;
    if (cur == 0 || (cur % stride) == 0) return keepGoing();
    return true;
}

inline std::string sha256Str(const std::string& s) {
    return conflict_detail::sha256Bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

inline void hashField(Sha256& h, const std::string& s) {
    conflict_detail::hashStr(h, s);
}

inline bool leafHasPadSetSegment(const std::string& leafPath) {
    size_t pos = 0;
    while (pos < leafPath.size()) {
        const size_t slash = leafPath.find('/', pos);
        const std::string seg = leafPath.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        const std::string up = graph::native_detail::upperAscii(seg);
        if (up.rfind("PADSET", 0) == 0) return true;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return false;
}

inline bool leafHasKbdSetSegment(const std::string& leafPath) {
    size_t pos = 0;
    while (pos < leafPath.size()) {
        const size_t slash = leafPath.find('/', pos);
        const std::string seg = leafPath.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        const std::string up = graph::native_detail::upperAscii(seg);
        if (graph::native_detail::endsWithUpper(up, ".KSC") ||
            graph::native_detail::endsWithUpper(up, ".KSA") ||
            up.rfind("KBDSET", 0) == 0) {
            return true;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return false;
}

inline void extractKmpZoneTopKeys(const uint8_t* data, size_t size, std::vector<std::vector<uint8_t>>& out) {
    out.clear();
    panative::Pa5xKmp rawKmp;
    std::string err;
    if (!graph::parseKmpContainerBytes(data, size, rawKmp, &err)) return;
    out.reserve(rawKmp.multisamples.size());
    for (const auto& m : rawKmp.multisamples) {
        std::vector<uint8_t> tks;
        if (m.firstZone <= rawKmp.zones.size() && m.zoneCount <= rawKmp.zones.size() - m.firstZone) {
            tks.reserve(m.zoneCount);
            for (size_t z = 0; z < m.zoneCount; ++z) {
                tks.push_back(rawKmp.zones[static_cast<size_t>(m.firstZone) + z].topKey);
            }
        }
        out.push_back(std::move(tks));
    }
}

inline std::string computeViewDigest(const M1AdapterView& v) {
    Sha256 h;
    hashField(h, "aiarranger-m1-adapter-view/2");
    hashField(h, adapterSourceKindName(v.sourceKind));
    hashField(h, v.coverage.kmpPresent ? "1" : "0");
    hashField(h, v.coverage.kmpParsed ? "1" : "0");
    hashField(h, v.coverage.kmpError);
    hashField(h, v.coverage.coverageComplete ? "1" : "0");
    for (const auto& r : v.roots) {
        hashField(h, "root");
        hashField(h, r.relPath);
        hashField(h, rootItemKindName(r.kind));
        hashField(h, r.contentSha256);
        hashField(h, r.parseOk ? "1" : "0");
        hashField(h, std::to_string(r.soundSelections.size()));
        for (const auto& sel : r.soundSelections) {
            hashField(h, std::to_string(sel.address.msb) + "/" + std::to_string(sel.address.lsb) + "/" +
                         std::to_string(sel.address.program) + "|" + soundBoundaryKindName(sel.boundary) + "|" +
                         (sel.resolved ? "1" : "0") + "|" + sel.provisionalPackageId);
        }
    }
    for (const auto& [sid, sc] : v.sounds) {
        hashField(h, "sound");
        hashField(h, sid.str());
        hashField(h, std::to_string(sc.kindChunkId));
        hashField(h, sc.decoded ? "1" : "0");
        hashField(h, sc.analysed ? "1" : "0");
        hashField(h, sc.unanalysedGap);
        hashField(h, sc.decodedBodySha256);
        hashField(h, sc.equivalence.equivalenceDigest);
        hashField(h, std::to_string(sc.storedPcmBytesProxy));
    }
    for (const auto& [pid, pkg] : v.packagesById) {
        hashField(h, "pkg");
        hashField(h, pid);
        hashField(h, pkg.packageSha256);
        hashField(h, pkg.valid ? "1" : "0");
        hashField(h, std::to_string(pkg.totalUncompressedBytes));
    }
    for (const auto& [rel, sha] : v.sourceFileSha256) {
        hashField(h, "file");
        hashField(h, rel);
        hashField(h, sha);
    }
    return h.hex();
}

}  // namespace adapter_detail

// Strict boundary classification helper built directly on M2a's canonical `SlotAddress::fromMidi()` (M2 Design §3.2):
//   - MSB 121, LSB 64..75 -> NativeUserSound (USER01..USER12, Confirmed)
//   - MSB 120, LSB 64..69 -> NativeUserDrumKit (USERDK01..USERDK06, Confirmed)
//   - MSB 120, LSB 70..75 -> AmbiguousOrOutOfSpec (USERDK07..USERDK12 outside documented 6 User DK banks)
//   - MSB 121/120, LSB 0..63 -> FactoryRom (Inferred)
//   - Anything else (missing CC0/CC32, MSB outside {120,121}, LSB > 75) -> UnknownOrUnanalysed (Unsupported)
inline SoundSelectionRecord classifySoundSelection(const graph::MidiSoundAddress& addr) {
    SoundSelectionRecord rec;
    rec.address = addr;
    if (addr.msb < 0 || addr.lsb < 0 || addr.program < 0 ||
        addr.msb > 127 || addr.lsb > 127 || addr.program > 127) {
        rec.boundary = SoundBoundaryKind::UnknownOrUnanalysed;
        rec.confidence = EdgeConfidence::Unsupported;
        rec.bankMissing = (addr.msb < 0 || addr.lsb < 0);
        return rec;
    }
    if (const auto slot = SlotAddress::fromMidi(addr.msb, addr.lsb, addr.program); slot.has_value()) {
        rec.slotAddress = *slot;
        rec.isDrumKit = slot->isDrumKit;
        rec.userBank = slot->bankName();
        rec.targetSoundId = slot->resourceId();
        if (slot->isOutOfSpecDrumKitBank()) {
            rec.boundary = SoundBoundaryKind::AmbiguousOrOutOfSpec;
            rec.confidence = EdgeConfidence::Unsupported;
        } else if (slot->isDrumKit) {
            rec.boundary = SoundBoundaryKind::NativeUserDrumKit;
            rec.confidence = EdgeConfidence::Confirmed;
        } else {
            rec.boundary = SoundBoundaryKind::NativeUserSound;
            rec.confidence = EdgeConfidence::Confirmed;
        }
        return rec;
    }
    if ((addr.msb == kUserSoundMsb || addr.msb == kUserDrumKitMsb) && addr.lsb >= 0 && addr.lsb < kUserBankLsbBase) {
        rec.boundary = SoundBoundaryKind::FactoryRom;
        rec.confidence = EdgeConfidence::Inferred;
        rec.isDrumKit = (addr.msb == kUserDrumKitMsb);
        return rec;
    }
    rec.boundary = SoundBoundaryKind::UnknownOrUnanalysed;
    rec.confidence = EdgeConfidence::Unsupported;
    return rec;
}

// Extracts decoded Sound body SHA-256, KMP zone topKeys, and exact PCM payload sizes from in-memory NativeKstInput.
inline bool extractNativeEquivalenceExtraInMemory(
    const graph::NativeKstInput& in,
    NativeEquivalenceExtra& out,
    const KeepGoing& keepGoing = {},
    size_t cancelCheckStride = kDefaultCancelCheckStride,
    graph::SoundPayloadMode soundMode = graph::SoundPayloadMode::AutoDetectOc31) {
    out = NativeEquivalenceExtra{};
    const size_t stride = adapter_detail::clampStride(cancelCheckStride);
    size_t tick = 0;
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return false;

    if (in.kmpContainer.has_value()) {
        const std::string rel = conflict_detail::normalizeRelPath(in.kmpContainer->label);
        out.fileSha256[rel] = conflict_detail::sha256Bytes(in.kmpContainer->bytes.data(), in.kmpContainer->bytes.size());
        out.fileSizes[rel] = in.kmpContainer->bytes.size();
        adapter_detail::extractKmpZoneTopKeys(in.kmpContainer->bytes.data(), in.kmpContainer->bytes.size(), out.msZoneTopKeys);
    }
    if (in.extraPcm.has_value()) {
        for (const auto& [id, pe] : in.extraPcm->byId) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) { out = NativeEquivalenceExtra{}; return false; }
            out.exactPcmPayloadBytes[id] = pe.payloadBytes;
        }
    }
    for (const auto& blob : in.pcmContainers) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) { out = NativeEquivalenceExtra{}; return false; }
        graph::PcmInventory tmp;
        std::string err;
        if (graph::addPcmContainerBytes(blob.bytes.data(), blob.bytes.size(), blob.label, tmp, &err)) {
            for (const auto& [id, pe] : tmp.byId) out.exactPcmPayloadBytes[id] = pe.payloadBytes;
        }
    }
    for (const auto& blob : in.soundContainers) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) { out = NativeEquivalenceExtra{}; return false; }
        const std::string rel = conflict_detail::normalizeRelPath(blob.label);
        out.fileSha256[rel] = conflict_detail::sha256Bytes(blob.bytes.data(), blob.bytes.size());
        out.fileSizes[rel] = blob.bytes.size();
        const std::string bank = blob.bank.empty() ? graph::native_detail::bankFromLabel(blob.label) : blob.bank;
        out.extraPresentSoundBanks.insert(graph::native_detail::upperAscii(bank));
        const graph::SoundExtractResult ex = graph::extractSoundsFromPcgBytes(
            blob.bytes.data(), blob.bytes.size(), bank, blob.label, soundMode);
        if (!ex.containerOk) continue;
        for (const graph::DecodedSound& s : ex.sounds) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) { out = NativeEquivalenceExtra{}; return false; }
            const graph::ResourceId sid = graph::soundId(s);
            out.soundBodySha256.emplace(sid, conflict_detail::sha256Bytes(s.body.data(), s.body.size()));
        }
    }
    return true;
}

// Extracts decoded Sound body SHA-256, KMP zone topKeys, and exact PCM metadata sizes from a read-only KST folder
// using O_NOFOLLOW | O_NONBLOCK + S_ISREG (zero disk mutations, zero symlinks followed).
inline bool extractNativeEquivalenceExtraFolder(
    const std::filesystem::path& kstDir,
    NativeEquivalenceExtra& out,
    const KeepGoing& keepGoing = {},
    size_t cancelCheckStride = kDefaultCancelCheckStride,
    size_t maxTotalBytes = 256u << 20,
    size_t maxDirEntries = 1000000,
    graph::SoundPayloadMode soundMode = graph::SoundPayloadMode::AutoDetectOc31) {
    out = NativeEquivalenceExtra{};
    const size_t stride = adapter_detail::clampStride(cancelCheckStride);
    size_t tick = 0;
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return false;

    const std::string rootPath = graph::native_detail::stripTrailingSlashes(kstDir);
    const graph::native_detail::ScopedFd rootFd(
        ::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (rootFd.fd < 0) return true;

    std::vector<std::string> topEntries;
    std::string err;
    if (!graph::native_detail::listDirEntriesAt(rootFd.fd, topEntries, maxDirEntries, err)) return true;

    size_t remainingBudget = maxTotalBytes;
    size_t pcmBytesRead = 0;
    for (const std::string& top : topEntries) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) { out = NativeEquivalenceExtra{}; return false; }
        const std::string topUpper = graph::native_detail::upperAscii(top);
        if (topUpper != "SOUND" && topUpper != "MULTISMP" && topUpper != "PCM") continue;
        struct stat st{};
        if (::fstatat(rootFd.fd, top.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) continue;
        const graph::native_detail::ScopedFd subFd(
            ::openat(rootFd.fd, top.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (subFd.fd < 0) continue;
        std::vector<std::string> leaves;
        if (!graph::native_detail::listDirEntriesAt(subFd.fd, leaves, maxDirEntries, err)) continue;
        for (const std::string& leaf : leaves) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) { out = NativeEquivalenceExtra{}; return false; }
            const bool isPcg = (topUpper == "SOUND") && graph::native_detail::endsWithUpper(leaf, ".PCG");
            const bool isKmp = (topUpper == "MULTISMP") && graph::native_detail::endsWithUpper(leaf, ".KMP");
            const bool isPcm = (topUpper == "PCM") && graph::native_detail::endsWithUpper(leaf, ".PCM");
            if (!isPcg && !isKmp && !isPcm) continue;

            const std::string rel = top + "/" + leaf;
            if (isPcm) {
                graph::PcmInventory tmp;
                std::string perr;
                if (graph::addPcmContainerFdAt(subFd.fd, leaf.c_str(), rel, panative::kMaxFileBytes,
                                               remainingBudget, pcmBytesRead, tmp, &perr)) {
                    for (const auto& [id, pe] : tmp.byId) out.exactPcmPayloadBytes[id] = pe.payloadBytes;
                }
                continue;
            }
            std::vector<uint8_t> bytes;
            std::string rerr;
            if (!graph::native_detail::readRegularAt(subFd.fd, leaf.c_str(), panative::kMaxFileBytes,
                                                     remainingBudget, bytes, rerr)) {
                continue;
            }
            remainingBudget -= bytes.size();
            out.fileSha256[rel] = conflict_detail::sha256Bytes(bytes.data(), bytes.size());
            out.fileSizes[rel] = bytes.size();
            if (isKmp) {
                adapter_detail::extractKmpZoneTopKeys(bytes.data(), bytes.size(), out.msZoneTopKeys);
            } else if (isPcg) {
                const std::string bank = graph::native_detail::bankFromLabel(rel);
                out.extraPresentSoundBanks.insert(graph::native_detail::upperAscii(bank));
                const graph::SoundExtractResult ex = graph::extractSoundsFromPcgBytes(
                    bytes.data(), bytes.size(), bank, rel, soundMode);
                if (!ex.containerOk) continue;
                for (const graph::DecodedSound& s : ex.sounds) {
                    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) { out = NativeEquivalenceExtra{}; return false; }
                    const graph::ResourceId sid = graph::soundId(s);
                    out.soundBodySha256.emplace(sid, conflict_detail::sha256Bytes(s.body.data(), s.body.size()));
                }
            }
        }
    }
    return true;
}

// Hashes a specific set of KST-relative regular files (e.g. selected .stg/.ksc/.pdc) strictly read-only
// using O_NOFOLLOW | O_NONBLOCK + S_ISREG, component by component. Never writes or follows symlinks.
inline bool hashNativeRelFilesFolder(
    const std::filesystem::path& kstDir,
    const std::vector<std::string>& relPaths,
    std::map<std::string, std::string>& outFileSha256,
    const KeepGoing& keepGoing = {},
    size_t cancelCheckStride = kDefaultCancelCheckStride,
    size_t maxFileBytes = panative::kMaxFileBytes,
    size_t maxTotalBytes = size_t(512) << 20) {
    const size_t stride = adapter_detail::clampStride(cancelCheckStride);
    size_t tick = 0;
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return false;

    const std::string rootPath = graph::native_detail::stripTrailingSlashes(kstDir);
    const graph::native_detail::ScopedFd rootFd(
        ::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (rootFd.fd < 0) return true;

    std::vector<std::string> sorted;
    sorted.reserve(relPaths.size());
    for (const std::string& p : relPaths) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) {
            outFileSha256.clear();
            return false;
        }
        sorted.push_back(conflict_detail::normalizeRelPath(p));
    }
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    size_t remainingBudget = maxTotalBytes;
    for (const std::string& rel : sorted) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) {
            outFileSha256.clear();
            return false;
        }
        if (rel.empty() || rel.find("..") != std::string::npos) continue;
        std::vector<std::string> parts;
        size_t pos = 0;
        bool badPart = false;
        while (pos <= rel.size()) {
            const size_t slash = rel.find('/', pos);
            const std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            if (part.empty() || part == "." || part == "..") { badPart = true; break; }
            parts.push_back(part);
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        if (badPart || parts.empty()) continue;

        int curFd = ::fcntl(rootFd.fd, F_DUPFD_CLOEXEC, 0);
        if (curFd < 0) continue;
        graph::native_detail::ScopedFd cur(curFd);
        bool dirOk = true;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            const int nextFd = ::openat(cur.fd, parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (nextFd < 0) { dirOk = false; break; }
            ::close(cur.release());
            cur.fd = nextFd;
        }
        if (!dirOk) continue;
        std::vector<uint8_t> bytes;
        std::string rerr;
        if (graph::native_detail::readRegularAt(cur.fd, parts.back().c_str(), maxFileBytes, remainingBudget, bytes, rerr)) {
            remainingBudget -= bytes.size();
            outFileSha256[rel] = conflict_detail::sha256Bytes(bytes.data(), bytes.size());
        }
    }
    return true;
}

// Adapts an already-built M1 `NativeKstClosureResult` into a stable, read-only `M1AdapterView`
// populated with M2a's canonical `SlotAddress`, `SoundEquivalenceProfile`, `CoverageSummary`, and `PartialNativeInspect`.
inline M1AdapterView adaptFromM1Closure(
    const graph::NativeKstClosureResult& m1,
    const NativeEquivalenceExtra& extra = {},
    const KeepGoing& keepGoing = {},
    size_t cancelCheckStride = kDefaultCancelCheckStride) {
    const size_t stride = adapter_detail::clampStride(cancelCheckStride);
    size_t tick = 0;
    auto makeCancelled = []() {
        M1AdapterView c;
        c.ok = false;
        c.cancelled = true;
        return c;
    };
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

    M1AdapterView view;
    view.sourceKind = AdapterSourceKind::NativeKst;
    view.sourceFileSha256 = extra.fileSha256;
    view.presentSoundBanks = extra.extraPresentSoundBanks;

    const graph::NativeKstDependencyManifest& M = m1.manifest;
    view.coverage.readOnly = true;
    view.coverage.deleteProofAvailable = false; // ALWAYS false for Native-KST
    view.coverage.upstreamScanned = M.coverage.upstreamScanned;
    view.coverage.kmpPresent = M.coverage.downstreamKmpPresent;
    view.coverage.kmpParsed = M.coverage.downstreamKmpParsed;
    view.coverage.kmpIsOc31 = !M.coverage.downstreamKmpParsed && conflict_detail::isOc31KmpError(M.coverage.kmpError);
    view.coverage.kmpError = M.coverage.kmpError;
    view.coverage.songBookPresent = M.coverage.songBookPresent;
    view.coverage.drumKitBodyEdgesSupported = M.coverage.drumKitBodyEdgesSupported;
    view.coverage.coverageComplete = M.coverage.coverageComplete;
    view.coverage.bytesReadDownstream = M.coverage.bytesReadDownstream;
    view.coverage.bytesReadUpstream = M.coverage.bytesReadUpstream;
    view.coverage.soundsOccupied = M.summary.soundSlotsOccupied;
    view.coverage.soundsAnalysed = M.summary.soundsAnalysed;
    view.coverage.soundsUnanalysed = M.summary.soundsUnanalysed;
    view.coverage.decodeFailures = m1.downstream.report.decodeFailures.size();
    view.coverage.unresolvedSlotsInAnalysed = M.summary.unresolvedSoundSlotsInAnalysed;
    view.coverage.unresolvedSlotsInUnanalysed = M.summary.unresolvedSoundSlotsInUnanalysed;
    view.coverage.duplicateDrumIdGroups = M.duplicateDrumIds.size();
    view.coverage.missingPcmSamples = M.missingPcmSamples.size();

    // Populate M2a UX Gate #2 PartialNativeInspect
    view.partialInspect.kmpPresent = M.coverage.downstreamKmpPresent;
    view.partialInspect.kmpParsed = M.coverage.downstreamKmpParsed;
    view.partialInspect.kmpIsOc31 = view.coverage.kmpIsOc31;
    view.partialInspect.kmpError = M.coverage.kmpError;
    view.partialInspect.soundContainerCount = M.summary.soundContainers;
    view.partialInspect.pcmContainerCount = M.summary.pcmContainers;
    view.partialInspect.pcmSampleCount = M.summary.pcmSamplesTotal;
    view.partialInspect.soundInventory = m1.downstream.report.soundCatalog;
    if (view.partialInspect.kmpIsOc31) {
        view.partialInspect.isPartial = true;
        view.partialInspect.notice = kOc31KmpDegradedNotice;
    }

    view.missingPcmSamples = M.missingPcmSamples;
    view.duplicateDrumIds = M.duplicateDrumIds;
    view.malformedTrees = M.malformedTrees;
    view.malformedMidiLeaves = M.malformedMidiLeaves;
    view.containerErrors = M.containerErrors;
    view.skippedPaths = M.skippedPaths;

    if (!m1.ok || !m1.graph.has_value()) {
        view.ok = false;
        view.partialInspect.isPartial = true;
        view.manifestSha256 = adapter_detail::computeViewDigest(view);
        return view;
    }

    // Preserve unified read-only Graph (never declaring scanned/complete!) and parsed USERDK key maps
    view.graph = *m1.graph;
    view.drumKeyMaps = m1.downstream.report.drumKeyMaps;
    const graph::Graph& g = *view.graph;

    // 1. Index KMP Samples, Multisamples, DrumSamples with exact Id8, zone topKeys, and stored-proxy flag
    const graph::KmpIndex& kmp = m1.downstream.kmpIndex;
    for (uint32_t i = 0; i < kmp.samples.size(); ++i) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const auto& sr = kmp.samples[i];
        SampleRecord rec;
        rec.id = graph::ResourceId{false, "KST", graph::Kind::Sample, i};
        rec.index = i;
        rec.id8 = sr.id;
        rec.name = sr.name;
        rec.isStoredSizeProxy = true;
        if (sr.info.has_value()) {
            rec.hasPcm = true;
            rec.info = sr.info;
            rec.pcmRelPath = sr.info->file;
            if (const auto pit = extra.exactPcmPayloadBytes.find(sr.id); pit != extra.exactPcmPayloadBytes.end() && pit->second > 0) {
                rec.payloadBytes = pit->second;
            } else {
                rec.payloadBytes = sr.info->bytes();
            }
            rec.sampleRate = sr.info->rate;
        } else {
            rec.hasPcm = false;
        }
        view.samples.emplace(rec.id, std::move(rec));
    }

    const bool hasExactZoneTopKeys = (extra.msZoneTopKeys.size() == kmp.multisamples.size());
    std::vector<MultisampleFingerprint> msFingerprints;
    msFingerprints.reserve(kmp.multisamples.size());

    for (uint32_t i = 0; i < kmp.multisamples.size(); ++i) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const auto& mr = kmp.multisamples[i];
        MultisampleRecord rec;
        rec.id = graph::ResourceId{false, "KST", graph::Kind::Multisample, i};
        rec.index = i;
        rec.id8 = mr.id;
        rec.name = mr.name;
        rec.zoneSampleIndices = mr.zoneSamples;
        if (hasExactZoneTopKeys && extra.msZoneTopKeys[i].size() == mr.zoneSamples.size()) {
            rec.zoneTopKeys = extra.msZoneTopKeys[i];
        }
        rec.confidence = EdgeConfidence::Confirmed;

        MultisampleFingerprint fp;
        fp.multisampleId = mr.id;
        fp.zones.reserve(mr.zoneSamples.size());
        std::set<graph::ResourceId> sids;
        for (size_t z = 0; z < mr.zoneSamples.size(); ++z) {
            const uint32_t si = mr.zoneSamples[z];
            ZoneFingerprint zf;
            zf.topKey = (z < rec.zoneTopKeys.size()) ? rec.zoneTopKeys[z] : 0;
            if (si != graph::kNoZoneSample) {
                const graph::ResourceId smpId{false, "KST", graph::Kind::Sample, si};
                sids.insert(smpId);
                zf.hasSample = true;
                if (const auto spIt = view.samples.find(smpId); spIt != view.samples.end()) {
                    zf.sampleId = spIt->second.id8;
                    if (spIt->second.hasPcm && spIt->second.payloadBytes > 0) {
                        zf.pcmPresent = true;
                        zf.pcmPayloadBytes = spIt->second.payloadBytes > 0xFFFFFFFFull
                                                 ? 0xFFFFFFFFu
                                                 : static_cast<uint32_t>(spIt->second.payloadBytes);
                    }
                }
            }
            fp.zones.push_back(zf);
        }
        rec.sampleIds.assign(sids.begin(), sids.end());
        view.multisamples.emplace(rec.id, std::move(rec));
        msFingerprints.push_back(std::move(fp));
    }

    std::vector<DrumSampleFingerprint> drumFingerprints;
    drumFingerprints.reserve(kmp.drums.size());
    std::set<graph::Id8> duplicateDrumIdSet;
    for (const auto& dup : M.duplicateDrumIds) duplicateDrumIdSet.insert(dup.id);

    for (uint32_t i = 0; i < kmp.drums.size(); ++i) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const auto& dr = kmp.drums[i];
        DrumSampleRecord rec;
        rec.id = graph::ResourceId{false, "KST", graph::Kind::DrumSample, i};
        rec.index = i;
        rec.id8 = dr.id;
        rec.name = dr.name;
        rec.leftSample = dr.leftSample;
        rec.rightSample = dr.rightSample;
        rec.confidence = EdgeConfidence::Confirmed;
        std::set<graph::ResourceId> sids;
        DrumSampleFingerprint df;
        df.drumSampleId = dr.id;
        if (dr.leftSample != graph::kNoZoneSample) {
            const graph::ResourceId lId{false, "KST", graph::Kind::Sample, dr.leftSample};
            sids.insert(lId);
            if (const auto spIt = view.samples.find(lId); spIt != view.samples.end()) {
                df.leftSampleId = spIt->second.id8;
                if (spIt->second.hasPcm && spIt->second.payloadBytes > 0) {
                    df.leftPcmPresent = true;
                    df.leftPcmPayloadBytes = spIt->second.payloadBytes > 0xFFFFFFFFull
                                                 ? 0xFFFFFFFFu
                                                 : static_cast<uint32_t>(spIt->second.payloadBytes);
                }
            }
        }
        if (dr.rightSample != graph::kNoZoneSample) {
            const graph::ResourceId rId{false, "KST", graph::Kind::Sample, dr.rightSample};
            sids.insert(rId);
            df.hasRight = true;
            if (const auto spIt = view.samples.find(rId); spIt != view.samples.end()) {
                df.rightSampleId = spIt->second.id8;
                if (spIt->second.hasPcm && spIt->second.payloadBytes > 0) {
                    df.rightPcmPresent = true;
                    df.rightPcmPayloadBytes = spIt->second.payloadBytes > 0xFFFFFFFFull
                                                  ? 0xFFFFFFFFu
                                                  : static_cast<uint32_t>(spIt->second.payloadBytes);
                }
            }
        }
        rec.sampleIds.assign(sids.begin(), sids.end());
        view.drumSamples.emplace(rec.id, std::move(rec));
        drumFingerprints.push_back(df);
    }

    // Group UnresolvedSlots per Sound with explicit separation of inUnanalysedSound == false vs true
    std::map<graph::ResourceId, std::vector<graph::UnresolvedSlot>> unresAnalysedBySound;
    std::map<graph::ResourceId, std::vector<graph::UnresolvedSlot>> unresSpeculativeBySound;
    for (const graph::UnresolvedSlot& u : M.unresolvedSoundSlots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        if (u.inUnanalysedSound) unresSpeculativeBySound[u.sound].push_back(u);
        else unresAnalysedBySound[u.sound].push_back(u);
    }

    // 2. Index User Sounds & build byte-exact M2a SoundEquivalenceProfiles
    for (const graph::UserSoundManifestEntry& use : M.userSounds) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        view.presentSoundBanks.insert(use.bank);

        SoundClosureRecord sc;
        sc.id = use.id;
        sc.slotAddress = SlotAddress::fromBankAndSlot(use.bank, use.slot);
        sc.bank = use.bank;
        sc.slot = use.slot;
        sc.kindChunkId = use.kindChunkId;
        sc.name = use.name;
        sc.sourceFile = use.sourceFile;
        sc.isDrumKit = use.isDrumKit;
        sc.decoded = use.decoded;
        sc.analysed = use.analysed;
        sc.unanalysedGap = (!sc.analysed && use.isDrumKit && use.gap.empty()) ? std::string("DrumKitBodyUnparsed") : use.gap;
        sc.soundToMsConfidence = sc.analysed ? EdgeConfidence::Inferred : EdgeConfidence::Unsupported;
        sc.msToSampleConfidence = M.coverage.downstreamKmpParsed ? EdgeConfidence::Confirmed : EdgeConfidence::Unsupported;

        if (use.isDrumKit) ++view.partialInspect.occupiedUserDrumKits;
        else ++view.partialInspect.occupiedUserSounds;

        if (!sc.analysed) {
            if (use.isDrumKit) ++view.coverage.unanalysedDrumKits;
            else if (sc.unanalysedGap == "NoKnownReference") ++view.coverage.unanalysedNoKnownRef;
            else ++view.coverage.unanalysedOther;
        }
        if (const auto bit = extra.soundBodySha256.find(use.id); bit != extra.soundBodySha256.end()) {
            sc.decodedBodySha256 = bit->second;
        }
        if (const auto ait = unresAnalysedBySound.find(use.id); ait != unresAnalysedBySound.end()) {
            sc.unresolvedSlotsInAnalysed = ait->second;
        }
        if (const auto sit = unresSpeculativeBySound.find(use.id); sit != unresSpeculativeBySound.end()) {
            sc.unresolvedSlotsInUnanalysed = sit->second;
        }

        // Populate M2a SoundEquivalenceProfile directly (matching slot_conflicts.h byte-for-byte!)
        SoundEquivalenceProfile& eq = sc.equivalence;
        eq.kindChunkId = use.kindChunkId;
        eq.bodySha256 = sc.decodedBodySha256;
        eq.decoded = use.decoded;
        eq.isDrumKit = use.isDrumKit;
        if (use.isDrumKit) {
            if (const auto kmit = view.drumKeyMaps.find(use.id); kmit != view.drumKeyMaps.end()) {
                eq.hasNonRamBank2Layers = kmit->second.hasBank2Layers();
            }
        }
        eq.analysed = sc.analysed;
        eq.unanalysedReason = sc.unanalysedGap;
        eq.unresolvedSlotCount = sc.unresolvedSlotsInAnalysed.size() + sc.unresolvedSlotsInUnanalysed.size();
        // KMP equivalence requires parsed KMP + exact zone topKeys for melodic Sounds (if adapted without raw KMP, stay InsufficientEvidence)
        eq.kmpComplete = use.isDrumKit
                             ? M.coverage.downstreamKmpParsed
                             : (M.coverage.downstreamKmpParsed && (kmp.multisamples.empty() || hasExactZoneTopKeys));

        if (sc.analysed && M.coverage.downstreamKmpParsed) {
            const auto cl = g.closure({use.id});
            std::set<graph::ResourceId> oscs, mss, dss, smps;
            std::set<std::string> pcms;
            uint64_t bytesProxy = 0;
            for (const graph::ResourceId& dep : cl) {
                if (dep.kind == graph::Kind::Oscillator) oscs.insert(dep);
                else if (dep.kind == graph::Kind::Multisample) {
                    mss.insert(dep);
                    if (!use.isDrumKit && dep.number < msFingerprints.size()) {
                        eq.referencedMultisamples.push_back(msFingerprints[dep.number]);
                    }
                } else if (dep.kind == graph::Kind::DrumSample) {
                    dss.insert(dep);
                    if (use.isDrumKit && dep.number < drumFingerprints.size()) {
                        const auto& df = drumFingerprints[dep.number];
                        if (duplicateDrumIdSet.count(df.drumSampleId) > 0) {
                            eq.hasDuplicateDrumSampleId = true;
                        }
                        eq.referencedDrumSamples.push_back(df);
                    }
                } else if (dep.kind == graph::Kind::Sample) {
                    smps.insert(dep);
                    if (const auto spIt = view.samples.find(dep); spIt != view.samples.end() && spIt->second.hasPcm) {
                        bytesProxy = adapter_detail::satAdd(bytesProxy, spIt->second.payloadBytes);
                        if (!spIt->second.pcmRelPath.empty()) pcms.insert(spIt->second.pcmRelPath);
                    }
                }
            }
            sc.oscillatorIds.assign(oscs.begin(), oscs.end());
            sc.multisampleIds.assign(mss.begin(), mss.end());
            sc.drumSampleIds.assign(dss.begin(), dss.end());
            sc.sampleIds.assign(smps.begin(), smps.end());
            sc.pcmFiles.assign(pcms.begin(), pcms.end());
            sc.storedPcmBytesProxy = bytesProxy;
        }
        finalizeSoundEquivalenceProfile(eq);
        view.sounds.emplace(sc.id, std::move(sc));
    }

    // 3. Index Canonical Root Items (.stg, .ksc, .pdc) with strict SoundBoundaryKind & embedded KBD/Pad provenance
    view.roots.reserve(M.roots.size());
    for (const graph::RootClosureEntry& r : M.roots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        RootItemRecord rec;
        rec.id = r.id;
        if (r.kind == graph::Kind::Style) rec.kind = RootItemKind::Style;
        else if (r.kind == graph::Kind::KeyboardSet) rec.kind = RootItemKind::KeyboardSet;
        else rec.kind = RootItemKind::Pad;

        rec.relPath = conflict_detail::normalizeRelPath(r.relPath);
        rec.locationCategory = r.locationCategory;
        rec.bankFolder = r.bankFolder;
        rec.displayName = r.displayName;
        if (const auto fit = extra.fileSha256.find(rec.relPath); fit != extra.fileSha256.end()) {
            rec.contentSha256 = fit->second;
        }
        if (const auto szit = extra.fileSizes.find(rec.relPath); szit != extra.fileSizes.end()) {
            rec.sizeBytes = szit->second;
        }
        rec.parseOk = r.parseOk;
        rec.parseError = r.parseError;
        rec.midiLeafCount = r.midiLeafCount;
        rec.malformedMidiLeafCount = r.malformedMidiLeafCount;
        rec.rawSelectCount = r.rawSelectCount;
        rec.hasUnresolvedBankRef = r.hasUnresolvedBankRef;
        rec.hasUnknownOriginRef = r.hasUnknownOriginRef;

        rec.soundSelections.reserve(r.soundRefs.size());
        for (const graph::RootSoundRef& sr : r.soundRefs) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
            SoundSelectionRecord sel = classifySoundSelection(sr.address);
            sel.via = sr.via;
            sel.occurrences = sr.occurrences;
            sel.leafOrigins = sr.leafOrigins;
            sel.totalDistinctLeafOrigins = sr.totalDistinctLeafOrigins;

            if (sel.targetSoundId.has_value()) {
                if (view.sounds.count(*sel.targetSoundId) > 0) {
                    sel.resolved = true;
                    sel.missingReason = graph::MissingRefReason::None;
                } else {
                    sel.resolved = false;
                    sel.missingReason = view.presentSoundBanks.count(sel.userBank) > 0
                                            ? graph::MissingRefReason::SoundSlotEmpty
                                            : graph::MissingRefReason::SoundBankAbsent;
                }
            }

            if (rec.kind == RootItemKind::Style) {
                if (sr.via == graph::Via::KeyboardSetPart) {
                    sel.fromEmbeddedKbdSet = true;
                } else {
                    bool anyPad = false;
                    bool anyMain = false;
                    for (const std::string& lp : sr.leafOrigins) {
                        if (adapter_detail::leafHasPadSetSegment(lp)) anyPad = true;
                        else if (adapter_detail::leafHasKbdSetSegment(lp)) sel.fromEmbeddedKbdSet = true;
                        else anyMain = true;
                    }
                    if (sr.totalDistinctLeafOrigins > sr.leafOrigins.size() && !anyMain) anyMain = true;
                    if (!anyPad && !sel.fromEmbeddedKbdSet && !anyMain) anyMain = true;
                    sel.fromEmbeddedPad = anyPad;
                    sel.fromStyleMainTrack = anyMain;
                }
            } else {
                sel.fromStyleMainTrack = true;
            }

            rec.soundSelections.push_back(std::move(sel));
        }
        view.roots.push_back(std::move(rec));
    }

    std::sort(view.roots.begin(), view.roots.end(),
              [](const RootItemRecord& a, const RootItemRecord& b) { return a.relPath < b.relPath; });
    for (size_t i = 0; i < view.roots.size(); ++i) view.rootIndexByRelPath[view.roots[i].relPath] = i;

    view.ok = true;
    view.manifestSha256 = adapter_detail::computeViewDigest(view);
    return view;
}

// End-to-end in-memory Native-KST adaptation: runs M1 closure + builds M2a SoundEquivalenceProfiles.
inline M1AdapterView adaptNativeKstInput(
    const graph::NativeKstClosureInput& in,
    const M1AdapterOptions& opts = {},
    const KeepGoing& keepGoing = {}) {
    const size_t stride = adapter_detail::clampStride(opts.cancelCheckStride);
    size_t tick = 0;
    auto makeCancelled = []() {
        M1AdapterView c;
        c.ok = false;
        c.cancelled = true;
        return c;
    };
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

    NativeEquivalenceExtra extra;
    if (opts.computeSoundBodyHashes) {
        if (!extractNativeEquivalenceExtraInMemory(in.downstream, extra, keepGoing, stride,
                                                   opts.closureOptions.downstream.soundMode)) {
            return makeCancelled();
        }
    }
    for (const auto& rb : in.upstreamRoots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const std::string rel = conflict_detail::normalizeRelPath(rb.relPath);
        extra.fileSha256[rel] = conflict_detail::sha256Bytes(rb.bytes.data(), rb.bytes.size());
        extra.fileSizes[rel] = rb.bytes.size();
    }
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
    const graph::NativeKstClosureResult m1 = graph::resolveNativeKstClosure(in, opts.closureOptions);
    return adaptFromM1Closure(m1, extra, keepGoing, stride);
}

// End-to-end read-only on-disk Native-KST folder adaptation. Never mutates disk or emits local paths.
inline M1AdapterView adaptNativeKstFolder(
    const std::filesystem::path& kstDir,
    const M1AdapterOptions& opts = {},
    const KeepGoing& keepGoing = {}) {
    const size_t stride = adapter_detail::clampStride(opts.cancelCheckStride);
    size_t tick = 0;
    auto makeCancelled = []() {
        M1AdapterView c;
        c.ok = false;
        c.cancelled = true;
        return c;
    };
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

    const graph::NativeKstClosureResult m1 = graph::resolveNativeKstClosureFolder(kstDir, opts.closureOptions);
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

    NativeEquivalenceExtra extra;
    if (m1.ok && opts.computeSoundBodyHashes) {
        if (!extractNativeEquivalenceExtraFolder(kstDir, extra, keepGoing, stride,
                                                 opts.closureOptions.downstream.maxTotalBytes,
                                                 opts.closureOptions.maxDirEntries,
                                                 opts.closureOptions.downstream.soundMode)) {
            return makeCancelled();
        }
    }
    if (m1.ok && opts.computeAllRootFileHashes) {
        std::vector<std::string> rels;
        rels.reserve(m1.manifest.roots.size());
        for (const auto& r : m1.manifest.roots) rels.push_back(r.relPath);
        if (!hashNativeRelFilesFolder(kstDir, rels, extra.fileSha256, keepGoing, stride)) {
            return makeCancelled();
        }
    }
    return adaptFromM1Closure(m1, extra, keepGoing, stride);
}

// Converts an adapted Native-KST `M1AdapterView` directly into M2a's canonical `TargetOccupancyMap`
// so M2a's `evaluateCollisionMatrix()` and M2b share one single source of truth for target slots,
// FAT32 R-18 object paths, Sound equivalence digests, and fail-closed TargetUninspectable checks.
inline TargetOccupancyMap buildTargetOccupancyFromAdapter(
    const M1AdapterView& view,
    const KeepGoing& keepGoing = {},
    size_t cancelCheckStride = kDefaultCancelCheckStride) {
    const size_t stride = adapter_detail::clampStride(cancelCheckStride);
    size_t tick = 0;
    auto makeCancelled = []() {
        TargetOccupancyMap c;
        c.valid = false;
        c.fullyInspectable = false;
        c.cancelled = true;
        c.uninspectableReason = "target occupancy conversion cancelled";
        c.coverage.deleteProofAvailable = false;
        return c;
    };
    if (view.cancelled || !adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

    TargetOccupancyMap map;
    map.valid = view.ok;
    map.partialInspect = view.partialInspect;
    map.coverage = view.coverage.toM2aCoverageSummary();
    map.bytesReadDownstream = view.coverage.bytesReadDownstream;
    map.bytesReadObjects = view.coverage.bytesReadUpstream;

    if (!view.ok) {
        map.fullyInspectable = false;
        map.partialInspect.isPartial = true;
        map.uninspectableReason = !view.containerErrors.empty()
                                      ? view.containerErrors.front()
                                      : "Target manifest view failed to load or is uninspectable.";
        map.partialInspect.notice = map.uninspectableReason;
        map.targetDiagnostics.push_back({
            DiagnosticCategory::UNSUPPORTED_FORMAT,
            diag_code::kTargetUninspectable,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Unsupported,
            "<target>",
            "",
            map.uninspectableReason,
            "Target KST could not be inspected; refusing fail-closed without silent 0-byte baseline.",
            {}
        });
        map.rebuildIndexesAndKey();
        return map;
    }

    // Mark present bank files
    for (const std::string& bank : view.presentSoundBanks) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        if (const auto probe = SlotAddress::fromBankAndSlot(bank, 0); probe.has_value()) {
            if (!probe->isDrumKit && probe->bank >= 1 && probe->bank <= kUserSoundBankCount) {
                map.userSoundBankPresent[probe->bank - 1] = true;
            } else if (probe->isDrumKit && probe->bank >= 1 && probe->bank <= kUserDrumKitBankCount) {
                map.userDrumKitBankPresent[probe->bank - 1] = true;
            } else if (probe->isOutOfSpecDrumKitBank()) {
                map.extraOutOfSpecDrumKitBanksPresent.insert(probe->bankName());
            }
        }
    }

    // Populate occupied Sound slots with M2a's canonical SoundEquivalenceProfile & mirror parsed USERDK key maps
    for (const auto& [rid, km] : view.drumKeyMaps) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        if (const auto addrOpt = SlotAddress::fromBankAndSlot(rid.scope, rid.number); addrOpt.has_value()) {
            map.drumKeyMaps.emplace(*addrOpt, km);
        }
    }
    for (const auto& [sid, sc] : view.sounds) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const auto addr = sc.slotAddress.has_value() ? sc.slotAddress : SlotAddress::fromBankAndSlot(sc.bank, sc.slot);
        if (!addr.has_value()) continue;
        OccupiedSlotEntry occ;
        occ.address = *addr;
        occ.displayName = sc.name;
        occ.sourceContainer = sc.sourceFile;
        occ.equivalence = sc.equivalence;
        map.addOccupiedSlot(std::move(occ));
    }

    // Register target Style / KeyboardSet / Pad object paths for FAT32 R-18 & collision detection
    for (const auto& r : view.roots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        map.addObjectPath(r.relPath, r.contentSha256, r.sizeBytes);
    }

    // Detect pre-existing FAT32 R-18 file/dir collisions in target
    for (const auto& [folded, vec] : map.objectPathsByFolded) {
        if (vec.size() > 1) {
            map.targetDiagnostics.push_back({
                DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                diag_code::kFatCaseCollisionR18,
                DiagnosticSeverity::Blocking,
                EvidenceLevel::Verified,
                folded,
                vec[0].relPath + " vs " + vec[1].relPath,
                "Target library contains multiple paths colliding under ASCII case folding (FAT32/exFAT R-18).",
                "Rename one colliding path in the target library before exporting.",
                {}
            });
        }
    }

    // Fail-closed TargetUninspectable gate (mirrors slot_conflicts.h §5.2 & UX Gate #2)
    const bool hasAnySounds = !view.sounds.empty();
    const bool kmpFailed = view.coverage.kmpPresent ? !view.coverage.kmpParsed : hasAnySounds;
    if (view.coverage.kmpIsOc31) {
        map.partialInspect.isPartial = true;
        map.partialInspect.notice = kOc31KmpDegradedNotice;
        map.targetDiagnostics.push_back({
            DiagnosticCategory::UNSUPPORTED_FORMAT,
            diag_code::kOc31KmpUnsupported,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Unsupported,
            "MULTISMP/RAM.KMP",
            "0x11050038",
            kOc31KmpDegradedNotice,
            "Requires authentic Pa5X raw RAM.KMP (0x11080018 v0x0103) for Multisample/Sample equivalence & closure.",
            {}
        });
    }
    if (!view.containerErrors.empty() || view.coverage.decodeFailures > 0 || kmpFailed) {
        map.fullyInspectable = false;
        map.partialInspect.isPartial = true;
        if (view.coverage.kmpIsOc31) {
            map.uninspectableReason = kOc31KmpDegradedNotice;
        } else if (!view.containerErrors.empty()) {
            map.uninspectableReason = "Target KST container error: " + view.containerErrors.front();
        } else if (view.coverage.decodeFailures > 0) {
            map.uninspectableReason = "Target KST contains " + std::to_string(view.coverage.decodeFailures) + " undecodable Sound item(s).";
        } else {
            map.uninspectableReason = "Target KST RAM.KMP unavailable or invalid: " + view.coverage.kmpError;
        }
        map.targetDiagnostics.push_back({
            DiagnosticCategory::UNSUPPORTED_FORMAT,
            diag_code::kTargetUninspectable,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Unsupported,
            "<target>",
            "",
            map.uninspectableReason,
            "Cannot safely verify target Sound equivalence or slot collisions; failing closed.",
            {}
        });
    } else {
        map.fullyInspectable = true;
        map.partialInspect.isPartial = false;
    }

    map.rebuildIndexesAndKey();
    return map;
}

// Adapts an in-memory Provisional Workspace (RESOURCES.tsv Sound/DrumKit packages + .mid/.uasf Styles)
// into the same stable read-only `M1AdapterView`.
inline M1AdapterView adaptProvisionalWorkspace(
    const ProvisionalWorkspaceInput& in,
    const KeepGoing& keepGoing = {},
    size_t cancelCheckStride = kDefaultCancelCheckStride) {
    const size_t stride = adapter_detail::clampStride(cancelCheckStride);
    size_t tick = 0;
    auto makeCancelled = []() {
        M1AdapterView c;
        c.ok = false;
        c.cancelled = true;
        return c;
    };
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();

    M1AdapterView view;
    view.sourceKind = AdapterSourceKind::ProvisionalWorkspace;
    view.coverage.readOnly = true;
    view.coverage.deleteProofAvailable = false;
    view.coverage.upstreamScanned = true;
    view.coverage.kmpPresent = false;
    view.coverage.kmpParsed = false;

    // 1. Canonical sort of package inputs by relDir
    std::vector<size_t> pkgOrder(in.packages.size());
    std::iota(pkgOrder.begin(), pkgOrder.end(), 0);
    std::sort(pkgOrder.begin(), pkgOrder.end(), [&](size_t a, size_t b) {
        return conflict_detail::normalizeRelPath(in.packages[a].relDir) <
               conflict_detail::normalizeRelPath(in.packages[b].relDir);
    });

    graph::Graph mergedGraph;
    bool allPackagesValid = true;
    for (size_t idx : pkgOrder) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const ProvisionalPackageInput& pi = in.packages[idx];
        const std::string relDir = conflict_detail::normalizeRelPath(pi.relDir);

        ProvisionalPackageRecord rec;
        rec.relDir = relDir;
        if (pi.prebuiltGraph.has_value()) {
            rec.graph = *pi.prebuiltGraph;
            rec.packageId = pi.prebuiltPackageId;
            rec.topKind = pi.prebuiltTopKind;
            rec.displayName = pi.prebuiltDisplayName;
            rec.valid = !rec.packageId.empty();
            if (!rec.valid) rec.error = "missing prebuilt packageId";
            rec.packageSha256 = pi.packageSha256.empty()
                                    ? adapter_detail::sha256Str(sound::writeManifest(rec.graph, rec.packageId, rec.topKind))
                                    : pi.packageSha256;
        } else {
            std::string pkgId, perr;
            if (!sound::parseManifest(pi.resourcesTsv, rec.graph, pkgId, &perr)) {
                rec.valid = false;
                rec.error = perr;
                rec.packageId = pi.prebuiltPackageId.empty() ? ("invalid:" + relDir) : pi.prebuiltPackageId;
                allPackagesValid = false;
            } else {
                rec.valid = true;
                rec.packageId = pkgId;
                if (const auto* sn = rec.graph.find({true, pkgId, graph::Kind::Sound, 0})) {
                    rec.topKind = graph::Kind::Sound;
                    rec.displayName = sn->name;
                } else if (const auto* dn = rec.graph.find({true, pkgId, graph::Kind::DrumKit, 0})) {
                    rec.topKind = graph::Kind::DrumKit;
                    rec.displayName = dn->name;
                }
                rec.packageSha256 = pi.packageSha256.empty()
                                        ? adapter_detail::sha256Str(pi.resourcesTsv)
                                        : pi.packageSha256;
            }
        }

        if (rec.valid) {
            std::set<graph::ResourceId> sids;
            uint64_t totalBytes = 0;
            uint32_t sampleIdx = 0;
            for (const auto& [nid, node] : rec.graph.nodes()) {
                if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
                if (!node.sample) continue;
                sids.insert(nid);
                totalBytes = adapter_detail::satAdd(totalBytes, node.sample->bytes());
                SampleRecord sr;
                sr.id = nid;
                sr.index = sampleIdx++;
                sr.name = node.name;
                sr.pcmRelPath = relDir.empty() ? node.sample->file : (relDir + "/" + node.sample->file);
                sr.payloadBytes = node.sample->bytes();
                sr.sampleRate = node.sample->rate;
                sr.isStoredSizeProxy = false;
                sr.hasPcm = true;
                sr.info = node.sample;
                view.samples.emplace(nid, std::move(sr));
            }
            rec.sampleIds.assign(sids.begin(), sids.end());
            rec.totalUncompressedBytes = totalBytes;
            view.sourceFileSha256[relDir.empty() ? rec.packageId : (relDir + "/RESOURCES.tsv")] = rec.packageSha256;

            std::string merr;
            if (!mergedGraph.merge(rec.graph, &merr)) {
                rec.valid = false;
                rec.error = "package graph merge conflict: " + merr;
                allPackagesValid = false;
            }
        }

        if (!relDir.empty()) view.packageIdByRelDir[relDir] = rec.packageId;
        const auto existing = view.packagesById.find(rec.packageId);
        if (existing == view.packagesById.end()) {
            view.packagesById.emplace(rec.packageId, std::move(rec));
        } else if (existing->second.packageSha256 != rec.packageSha256) {
            existing->second.valid = false;
            existing->second.error = "conflicting content for packageId " + rec.packageId;
            allPackagesValid = false;
        }
    }

    // 2. Canonical sort of provisional roots by relPath
    std::vector<size_t> rootOrder(in.roots.size());
    std::iota(rootOrder.begin(), rootOrder.end(), 0);
    std::sort(rootOrder.begin(), rootOrder.end(), [&](size_t a, size_t b) {
        return conflict_detail::normalizeRelPath(in.roots[a].relPath) <
               conflict_detail::normalizeRelPath(in.roots[b].relPath);
    });

    uint32_t nextIdx = 0;
    bool allRootsParsed = true;
    for (size_t idx : rootOrder) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const ProvisionalRootInput& ri = in.roots[idx];
        RootItemRecord rec;
        rec.relPath = conflict_detail::normalizeRelPath(ri.relPath);
        rec.kind = ri.kind;
        rec.id = graph::ResourceId{true, "workspace", graph::Kind::Style, nextIdx++};
        graph::closure_detail::splitRelPathMeta(rec.relPath, rec.locationCategory, rec.bankFolder, rec.displayName);
        if (rec.locationCategory.empty()) rec.locationCategory = "PROVISIONAL";

        if (!ri.contentSha256.empty()) {
            rec.contentSha256 = ri.contentSha256;
        } else if (!ri.smfBytes.empty()) {
            rec.contentSha256 = conflict_detail::sha256Bytes(ri.smfBytes.data(), ri.smfBytes.size());
            rec.sizeBytes = ri.smfBytes.size();
        } else {
            // P3-2: canonical sort before hashing so no-SMF provisional root hash is strictly order-independent!
            Sha256 h;
            adapter_detail::hashField(h, rec.relPath);
            std::vector<graph::MidiSoundAddress> sortedExplicit = ri.explicitSelections;
            std::sort(sortedExplicit.begin(), sortedExplicit.end());
            for (const auto& a : sortedExplicit) {
                adapter_detail::hashField(h, std::to_string(a.msb) + "/" + std::to_string(a.lsb) + "/" + std::to_string(a.program));
            }
            for (const auto& [addr, pid] : ri.addressToPackageId) {
                adapter_detail::hashField(h, "map:" + std::to_string(addr.msb) + "/" + std::to_string(addr.lsb) + "/" +
                                             std::to_string(addr.program) + "=" + pid);
            }
            std::set<std::string> sortedDirectPkgs(ri.directPackageIds.begin(), ri.directPackageIds.end());
            for (const auto& pid : sortedDirectPkgs) adapter_detail::hashField(h, pid);
            rec.contentSha256 = h.hex();
        }
        view.sourceFileSha256[rec.relPath] = rec.contentSha256;

        std::map<graph::MidiSoundAddress, size_t> addrCounts;
        if (!ri.smfBytes.empty()) {
            rec.midiLeafCount = 1;
            std::vector<panative::closure::Select> sels;
            std::string serr;
            if (!panative::closure::selects(ri.smfBytes.data(), ri.smfBytes.size(), sels, serr)) {
                rec.parseOk = false;
                rec.parseError = serr;
                rec.malformedMidiLeafCount = 1;
                view.malformedMidiLeaves.push_back({rec.relPath, "Main.mid", serr});
                allRootsParsed = false;
            } else {
                rec.parseOk = true;
                rec.rawSelectCount = sels.size();
                for (const auto& s : sels) {
                    const graph::MidiSoundAddress a{s.msb, s.lsb, s.program};
                    ++addrCounts[a];
                    if (s.msb < 0 || s.lsb < 0) rec.hasUnresolvedBankRef = true;
                }
            }
        }
        for (const auto& a : ri.explicitSelections) {
            ++addrCounts[a];
            ++rec.rawSelectCount;
            if (a.msb < 0 || a.lsb < 0) rec.hasUnresolvedBankRef = true;
        }

        for (const auto& [addr, count] : addrCounts) {
            SoundSelectionRecord sel = classifySoundSelection(addr);
            sel.via = graph::Via::StyleTrack;
            sel.occurrences = count;
            sel.fromStyleMainTrack = true;
            if (const auto pit = ri.addressToPackageId.find(addr); pit != ri.addressToPackageId.end()) {
                sel.provisionalPackageId = pit->second;
                const auto pkgIt = view.packagesById.find(pit->second);
                sel.resolved = (pkgIt != view.packagesById.end() && pkgIt->second.valid);
                if (!sel.resolved) sel.missingReason = graph::MissingRefReason::SoundSlotEmpty;
            } else if (sel.boundary == SoundBoundaryKind::UnknownOrUnanalysed && !sel.bankMissing) {
                rec.hasUnknownOriginRef = true;
            }
            rec.soundSelections.push_back(std::move(sel));
        }

        std::set<std::string> dirPkgs(ri.directPackageIds.begin(), ri.directPackageIds.end());
        rec.directPackageIds.assign(dirPkgs.begin(), dirPkgs.end());
        view.roots.push_back(std::move(rec));
    }

    for (size_t i = 0; i < view.roots.size(); ++i) view.rootIndexByRelPath[view.roots[i].relPath] = i;
    view.graph = std::move(mergedGraph);
    view.coverage.coverageComplete = allPackagesValid && allRootsParsed && view.malformedMidiLeaves.empty();
    view.ok = true;
    view.manifestSha256 = adapter_detail::computeViewDigest(view);
    return view;
}

}  // namespace ai_arranger::media::selective

#endif  // AI_ARRANGER_MEDIA_SOUND_M1_MANIFEST_ADAPTER_H
