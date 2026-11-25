#ifndef AI_ARRANGER_MEDIA_SOUND_M1_MANIFEST_ADAPTER_H
#define AI_ARRANGER_MEDIA_SOUND_M1_MANIFEST_ADAPTER_H

// Slice M2b: Thin, Read-Only Adapter over M1's Native-KST Dependency Closure Manifest
// (`native_kst_closure.h` + `native_refs.h`) and Provisional Package Graphs (`package_graph.h`).
//
// Design & Safety Invariants (M2_SELECTIVE_STYLE_EXPORT_DESIGN.md §2, §3, §8 Slice M2b):
//  * Strictly READ-ONLY: zero disk writes, zero mutations to source/target libraries, zero OC31 encoder.
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
    std::vector<graph::ResourceId> oscillatorIds;     // sorted unique
    std::vector<graph::ResourceId> multisampleIds;    // sorted unique
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
    uint64_t payloadBytes = 0;
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
};

struct M1AdapterView {
    bool ok = false;
    bool cancelled = false;
    AdapterSourceKind sourceKind = AdapterSourceKind::NativeKst;
    AdapterCoverageSummary coverage;

    std::vector<RootItemRecord> roots;                              // sorted by relPath
    std::map<std::string, size_t> rootIndexByRelPath;               // relPath -> index in `roots`
    std::map<graph::ResourceId, SoundClosureRecord> sounds;         // Native KST occupied Sound catalog
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
};

struct M1AdapterOptions {
    graph::NativeKstClosureOptions closureOptions;
    bool computeSoundBodyHashes = true;        // hash decoded SOUND/*.PCG bodies for Tier 3 exact Sound equivalence
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

inline std::string sha256Bytes(const void* data, size_t n) {
    Sha256 h;
    if (n > 0) h.update(data, n);
    return h.hex();
}

inline std::string sha256Str(const std::string& s) {
    return sha256Bytes(s.data(), s.size());
}

inline void hashField(Sha256& h, const std::string& s) {
    const uint64_t len = static_cast<uint64_t>(s.size());
    uint8_t le[8];
    for (int i = 0; i < 8; ++i) le[i] = static_cast<uint8_t>((len >> (8 * i)) & 0xFFu);
    h.update(le, sizeof le);
    if (!s.empty()) h.update(s.data(), s.size());
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

inline bool isOc31KmpError(const std::string& err) {
    return err.find("OC31") != std::string::npos || err.find("OC32") != std::string::npos;
}

inline std::string computeViewDigest(const M1AdapterView& v) {
    Sha256 h;
    hashField(h, "aiarranger-m1-adapter-view/1");
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

// Strict boundary classification helper (M2 Design §3.2):
//   - MSB 121, LSB 64..75 -> NativeUserSound (USER01..USER12, Verified)
//   - MSB 120, LSB 64..69 -> NativeUserDrumKit (USERDK01..USERDK06, Verified)
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
    if (addr.msb == 121 && addr.lsb >= 64 && addr.lsb <= 75) {
        rec.boundary = SoundBoundaryKind::NativeUserSound;
        rec.confidence = EdgeConfidence::Confirmed;
        rec.isDrumKit = false;
        char buf[16];
        std::snprintf(buf, sizeof buf, "USER%02d", addr.lsb - 63);
        rec.userBank = buf;
        rec.targetSoundId = graph::ResourceId{false, rec.userBank, graph::Kind::Sound, static_cast<uint32_t>(addr.program)};
        return rec;
    }
    if (addr.msb == 120 && addr.lsb >= 64 && addr.lsb <= 69) {
        rec.boundary = SoundBoundaryKind::NativeUserDrumKit;
        rec.confidence = EdgeConfidence::Confirmed;
        rec.isDrumKit = true;
        char buf[16];
        std::snprintf(buf, sizeof buf, "USERDK%02d", addr.lsb - 63);
        rec.userBank = buf;
        rec.targetSoundId = graph::ResourceId{false, rec.userBank, graph::Kind::Sound, static_cast<uint32_t>(addr.program)};
        return rec;
    }
    if (addr.msb == 120 && addr.lsb >= 70 && addr.lsb <= 75) {
        rec.boundary = SoundBoundaryKind::AmbiguousOrOutOfSpec;
        rec.confidence = EdgeConfidence::Unsupported;
        rec.isDrumKit = true;
        char buf[16];
        std::snprintf(buf, sizeof buf, "USERDK%02d", addr.lsb - 63);
        rec.userBank = buf;
        rec.targetSoundId = graph::ResourceId{false, rec.userBank, graph::Kind::Sound, static_cast<uint32_t>(addr.program)};
        return rec;
    }
    if ((addr.msb == 121 || addr.msb == 120) && addr.lsb >= 0 && addr.lsb < 64) {
        rec.boundary = SoundBoundaryKind::FactoryRom;
        rec.confidence = EdgeConfidence::Inferred;
        rec.isDrumKit = (addr.msb == 120);
        return rec;
    }
    rec.boundary = SoundBoundaryKind::UnknownOrUnanalysed;
    rec.confidence = EdgeConfidence::Unsupported;
    return rec;
}

// Computes decoded Sound body SHA-256 hashes from in-memory NativeKstInput (strictly read-only).
// Returns false on cooperative cancellation (clearing out maps cleanly).
inline bool computeSoundBodyHashesInMemory(
    const graph::NativeKstInput& in,
    std::map<graph::ResourceId, std::string>& outSoundBodySha256,
    std::map<std::string, std::string>& outFileSha256,
    const KeepGoing& keepGoing = {},
    size_t cancelCheckStride = kDefaultCancelCheckStride,
    graph::SoundPayloadMode soundMode = graph::SoundPayloadMode::AutoDetectOc31) {
    outSoundBodySha256.clear();
    outFileSha256.clear();
    const size_t stride = adapter_detail::clampStride(cancelCheckStride);
    size_t tick = 0;
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return false;

    if (in.kmpContainer.has_value()) {
        const std::string rel = graph::closure_detail::normalizeRelPath(in.kmpContainer->label);
        outFileSha256[rel] = adapter_detail::sha256Bytes(in.kmpContainer->bytes.data(), in.kmpContainer->bytes.size());
    }
    for (const auto& blob : in.soundContainers) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) {
            outSoundBodySha256.clear();
            outFileSha256.clear();
            return false;
        }
        const std::string rel = graph::closure_detail::normalizeRelPath(blob.label);
        outFileSha256[rel] = adapter_detail::sha256Bytes(blob.bytes.data(), blob.bytes.size());
        const std::string bank = blob.bank.empty() ? graph::native_detail::bankFromLabel(blob.label) : blob.bank;
        const graph::SoundExtractResult ex = graph::extractSoundsFromPcgBytes(
            blob.bytes.data(), blob.bytes.size(), bank, blob.label, soundMode);
        if (!ex.containerOk) continue;
        for (const graph::DecodedSound& s : ex.sounds) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) {
                outSoundBodySha256.clear();
                outFileSha256.clear();
                return false;
            }
            const graph::ResourceId sid = graph::soundId(s);
            outSoundBodySha256.emplace(sid, adapter_detail::sha256Bytes(s.body.data(), s.body.size()));
        }
    }
    return true;
}

// Computes decoded Sound body SHA-256 hashes from a read-only KST folder (O_NOFOLLOW | O_NONBLOCK + S_ISREG).
// Never modifies disk; returns false on cooperative cancellation (clearing output maps cleanly).
inline bool computeSoundBodyHashesFolder(
    const std::filesystem::path& kstDir,
    std::map<graph::ResourceId, std::string>& outSoundBodySha256,
    std::map<std::string, std::string>& outFileSha256,
    const KeepGoing& keepGoing = {},
    size_t cancelCheckStride = kDefaultCancelCheckStride,
    size_t maxTotalBytes = 256u << 20,
    size_t maxDirEntries = 1000000,
    graph::SoundPayloadMode soundMode = graph::SoundPayloadMode::AutoDetectOc31) {
    outSoundBodySha256.clear();
    outFileSha256.clear();
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
    for (const std::string& top : topEntries) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) {
            outSoundBodySha256.clear();
            outFileSha256.clear();
            return false;
        }
        const std::string topUpper = graph::native_detail::upperAscii(top);
        if (topUpper != "SOUND" && topUpper != "MULTISMP") continue;
        struct stat st{};
        if (::fstatat(rootFd.fd, top.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) continue;
        const graph::native_detail::ScopedFd subFd(
            ::openat(rootFd.fd, top.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (subFd.fd < 0) continue;
        std::vector<std::string> leaves;
        if (!graph::native_detail::listDirEntriesAt(subFd.fd, leaves, maxDirEntries, err)) continue;
        for (const std::string& leaf : leaves) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) {
                outSoundBodySha256.clear();
                outFileSha256.clear();
                return false;
            }
            const bool isPcg = (topUpper == "SOUND") && graph::native_detail::endsWithUpper(leaf, ".PCG");
            const bool isKmp = (topUpper == "MULTISMP") && graph::native_detail::endsWithUpper(leaf, ".KMP");
            if (!isPcg && !isKmp) continue;
            std::vector<uint8_t> bytes;
            std::string rerr;
            if (!graph::native_detail::readRegularAt(subFd.fd, leaf.c_str(), panative::kMaxFileBytes,
                                                     remainingBudget, bytes, rerr)) {
                continue;
            }
            remainingBudget -= bytes.size();
            const std::string rel = top + "/" + leaf;
            outFileSha256[rel] = adapter_detail::sha256Bytes(bytes.data(), bytes.size());
            if (isPcg) {
                const std::string bank = graph::native_detail::bankFromLabel(rel);
                const graph::SoundExtractResult ex = graph::extractSoundsFromPcgBytes(
                    bytes.data(), bytes.size(), bank, rel, soundMode);
                if (!ex.containerOk) continue;
                for (const graph::DecodedSound& s : ex.sounds) {
                    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) {
                        outSoundBodySha256.clear();
                        outFileSha256.clear();
                        return false;
                    }
                    const graph::ResourceId sid = graph::soundId(s);
                    outSoundBodySha256.emplace(sid, adapter_detail::sha256Bytes(s.body.data(), s.body.size()));
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
        sorted.push_back(graph::closure_detail::normalizeRelPath(p));
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
        // Walk path components with O_NOFOLLOW | O_DIRECTORY
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
            outFileSha256[rel] = adapter_detail::sha256Bytes(bytes.data(), bytes.size());
        }
    }
    return true;
}

// Adapts an already-built M1 `NativeKstClosureResult` into a stable, read-only `M1AdapterView`.
// Cooperative cancellation (`keepGoing`) is checked every <= 256 items; on cancel, all partial state is dropped.
inline M1AdapterView adaptFromM1Closure(
    const graph::NativeKstClosureResult& m1,
    const std::map<graph::ResourceId, std::string>& optionalSoundBodySha256 = {},
    const std::map<std::string, std::string>& optionalFileSha256 = {},
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
    view.sourceFileSha256 = optionalFileSha256;

    const graph::NativeKstDependencyManifest& M = m1.manifest;
    view.coverage.readOnly = true;
    view.coverage.deleteProofAvailable = false; // ALWAYS false for Native-KST
    view.coverage.upstreamScanned = M.coverage.upstreamScanned;
    view.coverage.kmpPresent = M.coverage.downstreamKmpPresent;
    view.coverage.kmpParsed = M.coverage.downstreamKmpParsed;
    view.coverage.kmpIsOc31 = !M.coverage.downstreamKmpParsed && adapter_detail::isOc31KmpError(M.coverage.kmpError);
    view.coverage.kmpError = M.coverage.kmpError;
    view.coverage.songBookPresent = M.coverage.songBookPresent;
    view.coverage.drumKitBodyEdgesSupported = false;
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

    view.missingPcmSamples = M.missingPcmSamples;
    view.duplicateDrumIds = M.duplicateDrumIds;
    view.malformedTrees = M.malformedTrees;
    view.malformedMidiLeaves = M.malformedMidiLeaves;
    view.containerErrors = M.containerErrors;
    view.skippedPaths = M.skippedPaths;

    if (!m1.ok || !m1.graph.has_value()) {
        view.ok = false;
        view.manifestSha256 = adapter_detail::computeViewDigest(view);
        return view;
    }

    // Preserve unified read-only Graph (never declaring scanned/complete!)
    view.graph = *m1.graph;
    const graph::Graph& g = *view.graph;

    // 1. Index KMP Samples, Multisamples, DrumSamples with exact Id8 and stored-proxy flag
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
            rec.payloadBytes = sr.info->bytes();
            rec.sampleRate = sr.info->rate;
        } else {
            rec.hasPcm = false;
        }
        view.samples.emplace(rec.id, std::move(rec));
    }

    for (uint32_t i = 0; i < kmp.multisamples.size(); ++i) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const auto& mr = kmp.multisamples[i];
        MultisampleRecord rec;
        rec.id = graph::ResourceId{false, "KST", graph::Kind::Multisample, i};
        rec.index = i;
        rec.id8 = mr.id;
        rec.name = mr.name;
        rec.zoneSampleIndices = mr.zoneSamples;
        rec.confidence = EdgeConfidence::Confirmed;
        std::set<graph::ResourceId> sids;
        for (uint32_t si : mr.zoneSamples) {
            if (si != graph::kNoZoneSample) sids.insert(graph::ResourceId{false, "KST", graph::Kind::Sample, si});
        }
        rec.sampleIds.assign(sids.begin(), sids.end());
        view.multisamples.emplace(rec.id, std::move(rec));
    }

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
        if (dr.leftSample != graph::kNoZoneSample)
            sids.insert(graph::ResourceId{false, "KST", graph::Kind::Sample, dr.leftSample});
        if (dr.rightSample != graph::kNoZoneSample)
            sids.insert(graph::ResourceId{false, "KST", graph::Kind::Sample, dr.rightSample});
        rec.sampleIds.assign(sids.begin(), sids.end());
        view.drumSamples.emplace(rec.id, std::move(rec));
    }

    // Group UnresolvedSlots per Sound with explicit separation of inUnanalysedSound == false vs true
    std::map<graph::ResourceId, std::vector<graph::UnresolvedSlot>> unresAnalysedBySound;
    std::map<graph::ResourceId, std::vector<graph::UnresolvedSlot>> unresSpeculativeBySound;
    for (const graph::UnresolvedSlot& u : M.unresolvedSoundSlots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        if (u.inUnanalysedSound) unresSpeculativeBySound[u.sound].push_back(u);
        else unresAnalysedBySound[u.sound].push_back(u);
    }

    // 2. Index User Sounds & downstream closure with confidence provenance
    for (const graph::UserSoundManifestEntry& use : M.userSounds) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        view.presentSoundBanks.insert(use.bank);

        SoundClosureRecord sc;
        sc.id = use.id;
        sc.bank = use.bank;
        sc.slot = use.slot;
        sc.kindChunkId = use.kindChunkId;
        sc.name = use.name;
        sc.sourceFile = use.sourceFile;
        sc.isDrumKit = use.isDrumKit;
        sc.decoded = use.decoded;
        sc.analysed = use.analysed;
        sc.unanalysedGap = use.gap;
        sc.soundToMsConfidence = use.analysed ? EdgeConfidence::Inferred : EdgeConfidence::Unsupported;
        sc.msToSampleConfidence = M.coverage.downstreamKmpParsed ? EdgeConfidence::Confirmed : EdgeConfidence::Unsupported;

        if (!use.analysed) {
            if (use.isDrumKit) ++view.coverage.unanalysedDrumKits;
            else if (use.gap == "NoKnownReference") ++view.coverage.unanalysedNoKnownRef;
            else ++view.coverage.unanalysedOther;
        }
        if (const auto bit = optionalSoundBodySha256.find(use.id); bit != optionalSoundBodySha256.end()) {
            sc.decodedBodySha256 = bit->second;
        }
        if (const auto ait = unresAnalysedBySound.find(use.id); ait != unresAnalysedBySound.end()) {
            sc.unresolvedSlotsInAnalysed = ait->second;
        }
        if (const auto sit = unresSpeculativeBySound.find(use.id); sit != unresSpeculativeBySound.end()) {
            sc.unresolvedSlotsInUnanalysed = sit->second;
        }

        if (use.analysed && M.coverage.downstreamKmpParsed) {
            const auto cl = g.closure({use.id});
            std::set<graph::ResourceId> oscs, mss, smps;
            std::set<std::string> pcms;
            uint64_t bytesProxy = 0;
            for (const graph::ResourceId& dep : cl) {
                if (dep.kind == graph::Kind::Oscillator) oscs.insert(dep);
                else if (dep.kind == graph::Kind::Multisample) mss.insert(dep);
                else if (dep.kind == graph::Kind::Sample) {
                    smps.insert(dep);
                    if (const auto spIt = view.samples.find(dep); spIt != view.samples.end() && spIt->second.hasPcm) {
                        bytesProxy = adapter_detail::satAdd(bytesProxy, spIt->second.payloadBytes);
                        if (!spIt->second.pcmRelPath.empty()) pcms.insert(spIt->second.pcmRelPath);
                    }
                }
            }
            sc.oscillatorIds.assign(oscs.begin(), oscs.end());
            sc.multisampleIds.assign(mss.begin(), mss.end());
            sc.sampleIds.assign(smps.begin(), smps.end());
            sc.pcmFiles.assign(pcms.begin(), pcms.end());
            sc.storedPcmBytesProxy = bytesProxy;
        }
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

        rec.relPath = graph::closure_detail::normalizeRelPath(r.relPath);
        rec.locationCategory = r.locationCategory;
        rec.bankFolder = r.bankFolder;
        rec.displayName = r.displayName;
        if (const auto fit = optionalFileSha256.find(rec.relPath); fit != optionalFileSha256.end()) {
            rec.contentSha256 = fit->second;
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

            // Resolve against source sound catalog (strictly preserving AmbiguousOrOutOfSpec boundary!)
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

            // Classify embedded KBD Set / Pad / Main Track provenance inside Styles
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
                    // Fail-safe: if leafOrigins was capped (>16 distinct leaves) and we only saw PadSet leaves,
                    // conservatively mark fromStyleMainTrack = true so we never accidentally drop a dependency.
                    if (sr.totalDistinctLeafOrigins > sr.leafOrigins.size() && !anyMain) anyMain = true;
                    if (!anyPad && !sel.fromEmbeddedKbdSet && !anyMain) anyMain = true;
                    sel.fromEmbeddedPad = anyPad;
                    sel.fromStyleMainTrack = anyMain;
                }
            } else if (rec.kind == RootItemKind::KeyboardSet) {
                sel.fromStyleMainTrack = true;
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

// End-to-end in-memory Native-KST adaptation: runs M1 closure + computes decoded Sound body & root SHA-256 hashes.
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

    std::map<graph::ResourceId, std::string> bodyHashes;
    std::map<std::string, std::string> fileHashes;
    if (opts.computeSoundBodyHashes) {
        if (!computeSoundBodyHashesInMemory(in.downstream, bodyHashes, fileHashes, keepGoing, stride,
                                            opts.closureOptions.downstream.soundMode)) {
            return makeCancelled();
        }
    }
    for (const auto& rb : in.upstreamRoots) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const std::string rel = graph::closure_detail::normalizeRelPath(rb.relPath);
        fileHashes[rel] = adapter_detail::sha256Bytes(rb.bytes.data(), rb.bytes.size());
    }
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
    const graph::NativeKstClosureResult m1 = graph::resolveNativeKstClosure(in, opts.closureOptions);
    return adaptFromM1Closure(m1, bodyHashes, fileHashes, keepGoing, stride);
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

    std::map<graph::ResourceId, std::string> bodyHashes;
    std::map<std::string, std::string> fileHashes;
    if (m1.ok && opts.computeSoundBodyHashes) {
        if (!computeSoundBodyHashesFolder(kstDir, bodyHashes, fileHashes, keepGoing, stride,
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
        if (!hashNativeRelFilesFolder(kstDir, rels, fileHashes, keepGoing, stride)) {
            return makeCancelled();
        }
    }
    return adaptFromM1Closure(m1, bodyHashes, fileHashes, keepGoing, stride);
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
        return graph::closure_detail::normalizeRelPath(in.packages[a].relDir) <
               graph::closure_detail::normalizeRelPath(in.packages[b].relDir);
    });

    graph::Graph mergedGraph;
    bool allPackagesValid = true;
    for (size_t idx : pkgOrder) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const ProvisionalPackageInput& pi = in.packages[idx];
        const std::string relDir = graph::closure_detail::normalizeRelPath(pi.relDir);

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

            // Merge into workspace graph if not already present (identical packageId merges cleanly!)
            std::string merr;
            if (!mergedGraph.merge(rec.graph, &merr)) {
                rec.valid = false;
                rec.error = "package graph merge conflict: " + merr;
                allPackagesValid = false;
            }
        }

        if (!relDir.empty()) view.packageIdByRelDir[relDir] = rec.packageId;
        // Keep canonical first entry per packageId (if two dirs have identical packageId & sha256, deduplicated!)
        const auto existing = view.packagesById.find(rec.packageId);
        if (existing == view.packagesById.end()) {
            view.packagesById.emplace(rec.packageId, std::move(rec));
        } else if (existing->second.packageSha256 != rec.packageSha256) {
            // Same packageId with differing SHA-256 is an integrity conflict!
            existing->second.valid = false;
            existing->second.error = "conflicting content for packageId " + rec.packageId;
            allPackagesValid = false;
        }
    }

    // 2. Canonical sort of provisional roots by relPath
    std::vector<size_t> rootOrder(in.roots.size());
    std::iota(rootOrder.begin(), rootOrder.end(), 0);
    std::sort(rootOrder.begin(), rootOrder.end(), [&](size_t a, size_t b) {
        return graph::closure_detail::normalizeRelPath(in.roots[a].relPath) <
               graph::closure_detail::normalizeRelPath(in.roots[b].relPath);
    });

    uint32_t nextIdx = 0;
    bool allRootsParsed = true;
    for (size_t idx : rootOrder) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return makeCancelled();
        const ProvisionalRootInput& ri = in.roots[idx];
        RootItemRecord rec;
        rec.relPath = graph::closure_detail::normalizeRelPath(ri.relPath);
        rec.kind = ri.kind;
        rec.id = graph::ResourceId{true, "workspace", graph::Kind::Style, nextIdx++};
        graph::closure_detail::splitRelPathMeta(rec.relPath, rec.locationCategory, rec.bankFolder, rec.displayName);
        if (rec.locationCategory.empty()) rec.locationCategory = "PROVISIONAL";

        if (!ri.contentSha256.empty()) {
            rec.contentSha256 = ri.contentSha256;
        } else if (!ri.smfBytes.empty()) {
            rec.contentSha256 = adapter_detail::sha256Bytes(ri.smfBytes.data(), ri.smfBytes.size());
        } else {
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
