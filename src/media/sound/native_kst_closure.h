#ifndef AI_ARRANGER_MEDIA_SOUND_NATIVE_KST_CLOSURE_H
#define AI_ARRANGER_MEDIA_SOUND_NATIVE_KST_CLOSURE_H

// M1 Read-Only Upstream-to-Downstream Native-KST Dependency Closure & Reproducible Manifest:
//   Style (.stg) / KeyboardSet (.ksc) / Pad (.pdc)
//     -> Sound (SOUND/USERnn.PCG, SOUND/USERDKnn.PCG)
//     -> Oscillator -> Multisample (MULTISMP/RAM.KMP) -> Zone -> Sample -> PCM (PCM/*.PCM)
//
// Boundaries (STRICTLY READ-ONLY):
//  * Read-only: never writes, modifies, or deletes any KST/SET file; no OC31 encoder; no Native Sound KST export.
//  * Delete safety: the unified Graph NEVER declares scanned/complete; deleteCheck() NEVER returns Unreferenced.
//  * Epistemic honesty:
//      - VERIFIED    : Root-tree container (.stg/.ksc/.pdc), strict SMF 1.0 CC0/CC32/PC extraction,
//                      User Sound/DK bank addressing (MSB 121/120, LSB 64..75 -> USER01..12 / USERDK01..12),
//                      KORF/OC31/OC32 container & codec, raw Pa5X RAM.KMP (0x11080018 v0x0103) -> Zone -> Sample -> PCM.
//      - INFERRED    : Factory/GM2 ROM classification (MSB 121/120, LSB 0..63; ROM bank table not hardware-verified),
//                      Sound -> Oscillator -> High/Low Multisample layout (fitLayout), stored PCM byte proxy.
//      - UNSUPPORTED : OC31 Pa700/Pa4X RAM.KMP (0x11050038), User Drum Kit Sound -> DrumSample body edges,
//                      SongBook (.SBD), Global (SETUP.GBL), malformed SMF leaves (e.g. CC11 >= 0x80),
//                      and unknown-origin MIDI bank addresses (MSB outside {120,121}, LSB > 75, or missing CC0/CC32).
//  * Determinism: every input, node ID, edge, manifest collection, and JSON output is canonically sorted;
//    repeated runs and differing directory/input orderings produce byte-identical manifests.
//  * Memory & FD safety: folder reads enforce O_NOFOLLOW | O_NONBLOCK + S_ISREG + trailing-slash stripping +
//    per-file/total read budgets + entry caps, and stream Root-tree files one at a time.

#include "media/panative/kst_closure.h"
#include "media/panative/root_tree.h"
#include "media/sound/native_refs.h"
#include "media/sound/resource_graph.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ai_arranger::media::graph {

enum class ResourceOrigin { Factory, User, Ram, Unknown };
inline const char* resourceOriginName(ResourceOrigin o) {
    switch (o) {
        case ResourceOrigin::Factory: return "Factory";
        case ResourceOrigin::User:    return "User";
        case ResourceOrigin::Ram:     return "RAM";
        case ResourceOrigin::Unknown: return "Unknown";
    }
    return "Unknown";
}

enum class EpistemicStatus { Verified, Inferred, Unsupported };
inline const char* epistemicStatusName(EpistemicStatus s) {
    switch (s) {
        case EpistemicStatus::Verified:    return "VERIFIED";
        case EpistemicStatus::Inferred:    return "INFERRED";
        case EpistemicStatus::Unsupported: return "UNSUPPORTED";
    }
    return "UNSUPPORTED";
}

enum class MissingRefReason { None, SoundBankAbsent, SoundSlotEmpty };
inline const char* missingRefReasonName(MissingRefReason r) {
    switch (r) {
        case MissingRefReason::None:            return "None";
        case MissingRefReason::SoundBankAbsent: return "SoundBankAbsent";
        case MissingRefReason::SoundSlotEmpty:  return "SoundSlotEmpty";
    }
    return "None";
}

struct MidiSoundAddress {
    int msb = -1;       // CC0 (0..127), -1 if unset before Program Change
    int lsb = -1;       // CC32 (0..127), -1 if unset before Program Change
    int program = -1;   // Program Change (0..127)
    auto operator<=>(const MidiSoundAddress&) const = default;
};

struct MidiAddressClassification {
    ResourceOrigin origin = ResourceOrigin::Unknown;
    EpistemicStatus epistemic = EpistemicStatus::Unsupported;
    bool bankMissing = false;          // CC0 or CC32 unset before PC
    bool isDrumKit = false;            // true for MSB 120 (User or Factory Drum Kit)
    std::string userBank;              // "USER01".."USER12" or "USERDK01".."USERDK12" when origin == User
    std::optional<ResourceId> target;  // pa5x:<userBank>/Sound/<program> when origin == User
};

// Evidence-based MIDI Bank Select + Program Change classifier (KST_DEPENDENCY_EVIDENCE.md D3-D6).
// Never guesses: anything outside verified User (121/120, LSB 64..75) or Factory/GM2 (121/120, LSB 0..63)
// is classified strictly as ResourceOrigin::Unknown / EpistemicStatus::Unsupported.
inline MidiAddressClassification classifyMidiAddress(const MidiSoundAddress& addr) {
    MidiAddressClassification c;
    if (addr.msb < 0 || addr.lsb < 0 || addr.program < 0 || addr.msb > 127 || addr.lsb > 127 || addr.program > 127) {
        c.origin = ResourceOrigin::Unknown;
        c.epistemic = EpistemicStatus::Unsupported;
        c.bankMissing = (addr.msb < 0 || addr.lsb < 0);
        return c;
    }
    if ((addr.msb == 121 || addr.msb == 120) && addr.lsb >= 64 && addr.lsb <= 75) {
        c.origin = ResourceOrigin::User;
        c.epistemic = EpistemicStatus::Verified;
        c.isDrumKit = (addr.msb == 120);
        char bankBuf[16];
        std::snprintf(bankBuf, sizeof bankBuf, c.isDrumKit ? "USERDK%02d" : "USER%02d", addr.lsb - 63);
        c.userBank = bankBuf;
        c.target = ResourceId{false, c.userBank, Kind::Sound, static_cast<uint32_t>(addr.program)};
        return c;
    }
    if ((addr.msb == 121 || addr.msb == 120) && addr.lsb >= 0 && addr.lsb < 64) {
        c.origin = ResourceOrigin::Factory;
        c.epistemic = EpistemicStatus::Inferred;
        c.isDrumKit = (addr.msb == 120);
        return c;
    }
    // Non-120/121 MSB (e.g., 0, 8, 99, 100, 104, 111, 127) or LSB > 75 (e.g., 121/127): unknown/unverified.
    c.origin = ResourceOrigin::Unknown;
    c.epistemic = EpistemicStatus::Unsupported;
    return c;
}

struct UpstreamRootBlob {
    std::string relPath;          // canonical KST-relative path, e.g. "STYLE/Bank1/MyStyle.stg"
    std::vector<uint8_t> bytes;   // raw Root-tree container bytes
};

struct NativeKstClosureInput {
    NativeKstInput downstream;
    std::vector<UpstreamRootBlob> upstreamRoots;
    bool songBookPresent = false;
    size_t metadirJsonFiles = 0;
};

struct NativeKstClosureOptions {
    NativeKstOptions downstream;
    // Separate upstream read budget so multi-hundred-MB .STG/.KSC/.PDC trees stream cleanly while keeping
    // downstream KORF/PCM/KMP/SOUND capped independently by downstream.maxTotalBytes (default 256 MiB).
    size_t maxUpstreamTotalBytes = sizeof(size_t) >= 8 ? (size_t(2) << 30) : (size_t(512) << 20);
    size_t maxUpstreamFileBytes = panative::kMaxFileBytes;
    size_t maxUpstreamFiles = 100000;
    size_t maxDirEntries = 1000000;
    size_t maxLeafOriginsPerRef = 16;   // bounds per-(via,address) leaf origin list in manifest; exact total kept in occurrences
    std::function<bool()> keepGoing = {}; // UX Gate #4 cooperative cancellation (checked every <= 256 roots/entries)
};

struct MalformedTreeEntry {
    std::string relPath;
    std::string reason;
    auto operator<=>(const MalformedTreeEntry&) const = default;
};

struct MalformedMidiLeafEntry {
    std::string rootRelPath;
    std::string leafPath;
    std::string reason;
    auto operator<=>(const MalformedMidiLeafEntry&) const = default;
};

struct UnresolvedBankSelectEntry {
    std::string rootRelPath;
    std::string leafPath;
    int channel = 0;
    int msb = -1;
    int lsb = -1;
    int program = -1;
    auto operator<=>(const UnresolvedBankSelectEntry&) const = default;
};

struct UnknownOriginAddressEntry {
    MidiSoundAddress address;
    size_t occurrences = 0;
    size_t rootCount = 0;
    auto operator<=>(const UnknownOriginAddressEntry&) const = default;
};

struct RootSoundRef {
    Via via = Via::StyleTrack;
    MidiSoundAddress address;
    ResourceOrigin origin = ResourceOrigin::Unknown;
    EpistemicStatus epistemic = EpistemicStatus::Unsupported;
    bool isDrumKit = false;
    std::optional<ResourceId> targetId;   // set for ResourceOrigin::User
    bool resolved = false;                // true iff origin == User and occupied slot exists in SOUND/*.PCG
    MissingRefReason missingReason = MissingRefReason::None;
    size_t occurrences = 0;               // total Program Change events for this (via, address) in this root
    std::vector<std::string> leafOrigins; // sorted unique tree-internal leaf paths (capped by maxLeafOriginsPerRef)
    size_t totalDistinctLeafOrigins = 0;
};

struct RootClosureEntry {
    ResourceId id;
    Kind kind = Kind::Style;
    ResourceOrigin origin = ResourceOrigin::User;
    EpistemicStatus epistemic = EpistemicStatus::Verified;
    std::string relPath;                  // e.g. "STYLE/BankA/Demo.stg"
    std::string locationCategory;         // "STYLE", "KEYBOARDSET", "PAD", "SETTINGS", or other top dir
    std::string bankFolder;               // bank folder name ("" for SETTINGS/*.ksc or depth-2)
    std::string displayName;              // filename stem
    bool parseOk = false;
    std::string parseError;
    size_t midiLeafCount = 0;
    size_t malformedMidiLeafCount = 0;
    size_t rawSelectCount = 0;
    std::vector<RootSoundRef> soundRefs;              // sorted by (via, msb, lsb, program)
    std::vector<ResourceId> userSoundIds;             // sorted unique resolved User Sound / DK IDs
    std::vector<ResourceId> missingUserSoundIds;      // sorted unique dangling User Sound / DK IDs
    std::vector<ResourceId> multisampleIds;           // sorted unique reachable RAM Multisample IDs
    std::vector<ResourceId> sampleIds;                // sorted unique reachable RAM Sample IDs
    std::vector<std::string> pcmFiles;                // sorted unique reachable PCM/*.pcm relative paths
    uint64_t storedPcmBytesProxy = 0;                 // sum of SampleInfo::bytes() across reachable samples (INFERRED proxy)
    bool hasUnanalysedUserSound = false;
    bool hasMissingDependency = false;
    bool hasUnknownOriginRef = false;
    bool hasUnresolvedBankRef = false;
};

struct UserSoundManifestEntry {
    ResourceId id;
    std::string bank;
    uint32_t slot = 0;
    uint32_t kindChunkId = 0;
    std::string name;
    std::string sourceFile;
    ResourceOrigin origin = ResourceOrigin::User;
    bool isDrumKit = false;
    bool decoded = false;
    bool analysed = false;
    std::string gap;                                  // "" when analysed, else gapName() or "KmpUnavailable"
    EpistemicStatus downstreamEpistemic = EpistemicStatus::Unsupported;
    std::vector<ResourceId> referencingRootIds;       // sorted unique upstream roots referencing this Sound
    size_t totalRefOccurrences = 0;
    size_t multisampleCount = 0;
    size_t sampleCount = 0;
    size_t pcmFileCount = 0;
    bool orphanFromScannedRoots = false;              // true when referencingRootIds.empty() (NOT provably deletable)
};

struct SharedResourceEntry {
    std::string id;                                   // ResourceId::str() or PCM relative file path
    std::string kind;                                 // "Sound", "Multisample", "Sample", "PcmFile"
    ResourceOrigin origin = ResourceOrigin::Ram;
    size_t referencingRootCount = 0;
    size_t referencingSoundCount = 0;                 // for Multisample/Sample/PcmFile
    std::vector<ResourceId> referencingRootIds;       // sorted unique upstream roots (populated for User Sounds & PCM files)
};

struct MissingUserSoundEntry {
    ResourceId targetId;
    std::string bank;
    uint32_t slot = 0;
    bool isDrumKit = false;
    MissingRefReason reason = MissingRefReason::SoundBankAbsent;
    size_t occurrences = 0;
    std::vector<ResourceId> referencingRootIds;       // sorted unique upstream roots
};

struct ManifestCoverageFlags {
    bool readOnly = true;
    bool deleteProofAvailable = false;                // ALWAYS false for Native-KST
    bool upstreamScanned = false;
    bool downstreamKmpPresent = false;
    bool downstreamKmpParsed = false;
    std::string kmpError;
    bool songBookPresent = false;
    bool songBookScanned = false;                     // ALWAYS false (OC31 SBD unsupported)
    bool drumKitBodyEdgesSupported = false;           // ALWAYS false (USERDK -> DrumSample unsupported)
    bool coverageComplete = false;
    size_t bytesReadDownstream = 0;
    size_t bytesReadUpstream = 0;
    size_t metadirJsonFiles = 0;
};

struct ManifestSummary {
    size_t styleCount = 0;
    size_t keyboardSetCount = 0;
    size_t padCount = 0;
    size_t settingsKeyboardSetCount = 0;
    size_t totalUpstreamRoots = 0;
    size_t parsedUpstreamRoots = 0;
    size_t malformedUpstreamRoots = 0;
    size_t midiLeavesTotal = 0;
    size_t malformedMidiLeaves = 0;
    size_t rawSelectEvents = 0;
    size_t userSelectsResolved = 0;
    size_t userSelectsFileAbsent = 0;
    size_t userSelectsSlotEmpty = 0;
    size_t factorySelects = 0;
    size_t unknownOriginSelects = 0;
    size_t unresolvedBankSelects = 0;
    size_t soundContainers = 0;
    size_t soundSlotsOccupied = 0;
    size_t soundsAnalysed = 0;
    size_t soundsUnanalysed = 0;
    size_t unresolvedSoundSlotsInAnalysed = 0;
    size_t unresolvedSoundSlotsInUnanalysed = 0;
    size_t multisamplesTotal = 0;
    size_t multisamplesReachableFromRoots = 0;
    size_t multisamplesReachableFromAnalysedSounds = 0;
    size_t drumSamplesTotal = 0;
    size_t duplicateDrumIdGroups = 0;
    size_t samplesTotal = 0;
    size_t samplesReachableFromRoots = 0;
    size_t samplesReachableFromAnalysedSounds = 0;
    size_t pcmContainers = 0;
    size_t pcmSamplesTotal = 0;
    size_t pcmSamplesWithoutId = 0;
    size_t pcmFilesTotal = 0;
    size_t pcmFilesReachableFromRoots = 0;
    size_t missingUserSoundTargets = 0;
    size_t missingPcmSamples = 0;
    size_t danglingGraphEdges = 0;
    size_t sharedUserSoundsAcrossRoots = 0;
    size_t sharedMultisamplesAcrossRoots = 0;
    size_t sharedMultisamplesAcrossSounds = 0;
    size_t sharedSamplesAcrossRoots = 0;
    size_t sharedSamplesAcrossSounds = 0;
    size_t sharedPcmFilesAcrossRoots = 0;
    size_t orphanUserSounds = 0;
    size_t orphanMultisamplesFromRoots = 0;
    size_t orphanMultisamplesFromAnalysedSounds = 0;
    size_t orphanDrumSamples = 0;
    size_t orphanSamplesFromRoots = 0;
    size_t orphanPcmEntries = 0;
    size_t orphanPcmFilesFromRoots = 0;
};

struct NativeKstDependencyManifest {
    std::string schema = "aiarranger-native-kst-closure-manifest/1";
    ManifestCoverageFlags coverage;
    ManifestSummary summary;
    std::vector<RootClosureEntry> roots;                  // sorted by relPath
    std::vector<UserSoundManifestEntry> userSounds;       // sorted by ResourceId
    std::vector<SharedResourceEntry> sharedUserSounds;    // sorted by id
    std::vector<SharedResourceEntry> sharedMultisamples;  // sorted by id
    std::vector<SharedResourceEntry> sharedSamples;       // sorted by id
    std::vector<SharedResourceEntry> sharedPcmFiles;      // sorted by id
    std::vector<MissingUserSoundEntry> missingUserSounds; // sorted by targetId
    std::vector<MissingPcmSample> missingPcmSamples;      // sorted by sampleIndex
    std::vector<ResourceId> orphanUserSounds;             // sorted by ResourceId (NOT provably deletable)
    std::vector<ResourceId> orphanMultisamples;           // sorted by ResourceId (NOT provably deletable)
    std::vector<ResourceId> orphanSamples;                // sorted by ResourceId (NOT provably deletable)
    std::vector<std::string> orphanPcmFiles;              // sorted by file path (NOT provably deletable)
    std::vector<PcmEntry> orphanPcmEntries;               // sorted by (file, hexId) (NOT provably deletable)
    std::vector<MalformedTreeEntry> malformedTrees;       // sorted
    std::vector<MalformedMidiLeafEntry> malformedMidiLeaves;         // sorted
    std::vector<UnresolvedBankSelectEntry> unresolvedBankSelects;    // sorted
    std::vector<UnknownOriginAddressEntry> unknownOriginAddresses;   // sorted
    std::vector<Unanalysed> unanalysedSounds;             // sorted by sound ResourceId
    std::vector<UnresolvedSlot> unresolvedSoundSlots;     // sorted
    std::vector<DuplicateKmpId> duplicateDrumIds;         // sorted
    std::vector<std::string> skippedPaths;                // sorted unique
    std::vector<std::string> containerErrors;             // sorted unique
    std::vector<std::string> diagnostics;                 // deterministic order
};

struct ManifestJsonOptions {
    bool includePerRootSampleIds = false;   // if true, serialize full sampleIds array per root (can be large on 4GB KSTs)
    bool includeSampleLists = false;        // if true, serialize full sharedSamples & orphanSamples ID arrays
};

struct NativeKstClosureResult {
    bool ok = false;                        // true when root opened/inputs accepted and unified Graph was built
    bool cancelled = false;                 // true when cooperative opts.keepGoing returned false (partial state dropped)
    std::optional<Graph> graph;             // unified read-only Graph; deleteCheck() NEVER returns Unreferenced
    NativeKstResult downstream;
    NativeKstDependencyManifest manifest;

    bool upstreamOk() const {
        return manifest.malformedTrees.empty() && manifest.malformedMidiLeaves.empty() &&
               manifest.containerErrors.empty();
    }
    bool downstreamOk() const { return downstream.ok && downstream.report.ok(); }
    bool coverageComplete() const { return manifest.coverage.coverageComplete; }
};

namespace closure_detail {

inline std::string normalizeRelPath(std::string p) {
    for (char& c : p) if (c == '\\') c = '/';
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    return p;
}

inline std::string extensionUpper(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return native_detail::upperAscii(path.substr(dot));
}

inline bool kindFromUpstreamExt(const std::string& path, Kind& outKind) {
    const std::string ext = extensionUpper(path);
    if (ext == ".STG") { outKind = Kind::Style; return true; }
    if (ext == ".KSC") { outKind = Kind::KeyboardSet; return true; }
    if (ext == ".PDC") { outKind = Kind::Pad; return true; }
    return false;
}

inline void splitRelPathMeta(const std::string& rel, std::string& locationCategory,
                             std::string& bankFolder, std::string& displayName) {
    locationCategory.clear();
    bankFolder.clear();
    displayName.clear();
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
    if (!parts.empty()) locationCategory = native_detail::upperAscii(parts[0]);
    if (parts.size() >= 3) {
        bankFolder = parts[1];
        for (size_t i = 2; i + 1 < parts.size(); ++i) bankFolder += "/" + parts[i];
    }
    const std::string leaf = parts.empty() ? rel : parts.back();
    const size_t dot = leaf.rfind('.');
    displayName = (dot != std::string::npos && dot > 0) ? leaf.substr(0, dot) : leaf;
}

inline bool isDrumKitBankName(const std::string& bankUpper) {
    return bankUpper.rfind("USERDK", 0) == 0;
}

// Determines Graph::Via for a MIDI leaf inside a Style, KeyboardSet, or Pad Root tree.
inline Via classifyLeafVia(Kind rootKind, const std::string& leafPath) {
    if (rootKind == Kind::Pad) return Via::PadTrack;
    if (rootKind == Kind::KeyboardSet) return Via::KeyboardSetPart;
    // Kind::Style (.STG): inspect path segments (case-insensitive)
    size_t pos = 0;
    bool hasEmbeddedKbd = false;
    bool hasStyleTrack = false;
    while (pos < leafPath.size()) {
        const size_t slash = leafPath.find('/', pos);
        const std::string seg = leafPath.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        const std::string up = native_detail::upperAscii(seg);
        if (native_detail::endsWithUpper(up, ".KSC") || native_detail::endsWithUpper(up, ".KSA") ||
            up.rfind("KBDSET", 0) == 0) {
            hasEmbeddedKbd = true;
        } else if (native_detail::endsWithUpper(up, ".PDC") || native_detail::endsWithUpper(up, ".STC") ||
                   up.rfind("PADSET", 0) == 0) {
            hasStyleTrack = true;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    if (hasEmbeddedKbd) return Via::KeyboardSetPart;
    if (hasStyleTrack) return Via::StyleTrack;
    return Via::ProgramChange;
}

struct CollectedMidiLeaf {
    std::string leafPath;
    const panative::roottree::Node* node = nullptr;
};

inline void collectMidiLeavesWithPaths(const panative::roottree::Node& n, const std::string& prefix,
                                       std::vector<CollectedMidiLeaf>& out) {
    for (const auto& c : n.children) {
        const std::string full = prefix.empty() ? c.name : (prefix + "/" + c.name);
        if (c.kind == panative::roottree::Node::Leaf) {
            if (native_detail::endsWithUpper(c.name, ".MID")) out.push_back({full, &c});
        } else {
            collectMidiLeavesWithPaths(c, full, out);
        }
    }
}

struct ExtractedRoot {
    std::string relPath;
    Kind kind = Kind::Style;
    bool parseOk = false;
    std::string parseError;
    size_t midiLeafCount = 0;
    size_t rawSelectCount = 0;
    std::vector<MalformedMidiLeafEntry> malformedLeaves;
    std::vector<UnresolvedBankSelectEntry> unresolvedBanks;
    struct RefAgg {
        size_t occurrences = 0;
        std::set<std::string> leafOrigins;
    };
    // Key: (Via, MidiSoundAddress) -> deduplicated count + unique leaf paths
    std::map<std::pair<Via, MidiSoundAddress>, RefAgg> refs;
};

inline ExtractedRoot extractUpstreamRootBytes(const std::string& relPath, Kind kind,
                                              const uint8_t* data, size_t size) {
    ExtractedRoot er;
    er.relPath = relPath;
    er.kind = kind;
    const panative::roottree::Tree tree = panative::roottree::parse(data, size);
    if (!tree.ok) {
        er.parseOk = false;
        er.parseError = tree.error;
        return er;
    }
    er.parseOk = true;
    std::vector<CollectedMidiLeaf> mids;
    collectMidiLeavesWithPaths(tree.root, "", mids);
    er.midiLeafCount = mids.size();

    for (const CollectedMidiLeaf& m : mids) {
        std::vector<panative::closure::Select> sels;
        std::string serr;
        if (!panative::closure::leafSelects(*m.node, sels, serr)) {
            er.malformedLeaves.push_back({relPath, m.leafPath, serr});
            continue;
        }
        const Via via = classifyLeafVia(kind, m.leafPath);
        for (const auto& s : sels) {
            ++er.rawSelectCount;
            const MidiSoundAddress addr{s.msb, s.lsb, s.program};
            auto& agg = er.refs[{via, addr}];
            ++agg.occurrences;
            agg.leafOrigins.insert(m.leafPath);
            if (s.msb < 0 || s.lsb < 0) {
                er.unresolvedBanks.push_back({relPath, m.leafPath, s.channel, s.msb, s.lsb, s.program});
            }
        }
    }
    return er;
}

// UTF-8-safe canonical JSON string escaper: preserves valid UTF-8 sequences, escapes control chars,
// quotes, backslashes, and any invalid/lone non-ASCII (e.g. Latin-1 Pa5X name) byte as \u00XX.
inline std::string manifestJsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') { out += "\\\""; ++i; }
        else if (c == '\\') { out += "\\\\"; ++i; }
        else if (c == '\b') { out += "\\b"; ++i; }
        else if (c == '\f') { out += "\\f"; ++i; }
        else if (c == '\n') { out += "\\n"; ++i; }
        else if (c == '\r') { out += "\\r"; ++i; }
        else if (c == '\t') { out += "\\t"; ++i; }
        else if (c < 0x20 || c == 0x7F) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", static_cast<unsigned>(c));
            out += u;
            ++i;
        } else if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
        } else {
            // Check valid UTF-8 multibyte sequence; if invalid (e.g. Latin-1), escape byte as \u00XX.
            size_t len = 0;
            uint32_t cp = 0;
            if ((c & 0xE0u) == 0xC0u && i + 1 < n) {
                const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
                if ((c1 & 0xC0u) == 0x80u) { cp = ((c & 0x1Fu) << 6) | (c1 & 0x3Fu); if (cp >= 0x80u) len = 2; }
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
                out.append(s, i, len);
                i += len;
            } else {
                char u[8];
                std::snprintf(u, sizeof u, "\\u%04x", static_cast<unsigned>(c));
                out += u;
                ++i;
            }
        }
    }
    return out;
}

inline std::string hexU32(uint32_t v) {
    char b[12];
    std::snprintf(b, sizeof b, "0x%08x", v);
    return b;
}

inline constexpr size_t kClosureCancelStride = 64; // strictly <= 256 per UX Gate #4

inline bool checkKeepGoing(const std::function<bool()>& keepGoing, size_t& tick) {
    if (!keepGoing) return true;
    const size_t cur = tick++;
    if (cur == 0 || (cur % kClosureCancelStride) == 0) return keepGoing();
    return true;
}

inline NativeKstClosureResult makeCancelledResult() {
    NativeKstClosureResult r;
    r.ok = false;
    r.cancelled = true;
    r.manifest.coverage.readOnly = true;
    r.manifest.coverage.deleteProofAvailable = false;
    r.manifest.coverage.coverageComplete = false;
    return r;
}

inline NativeKstClosureResult finalizeClosure(NativeKstResult downstream,
                                              std::vector<ExtractedRoot> extractedRoots,
                                              bool songBookPresent,
                                              size_t bytesReadUpstream,
                                              size_t metadirJsonFiles,
                                              std::vector<std::string> extraSkipped,
                                              std::vector<std::string> extraErrors,
                                              std::vector<std::string> extraDiags,
                                              const NativeKstClosureOptions& opts) {
    size_t cancelTick = 0;
    if (!checkKeepGoing(opts.keepGoing, cancelTick)) return makeCancelledResult();
    NativeKstClosureResult out;
    out.downstream = std::move(downstream);

    // 1. Canonical sort of upstream roots by relPath before assigning positional ResourceIds
    std::sort(extractedRoots.begin(), extractedRoots.end(),
              [](const ExtractedRoot& a, const ExtractedRoot& b) { return a.relPath < b.relPath; });

    // 2. Build/extend unified read-only Graph (NEVER calling declareScanned / declarePackageComplete!)
    Graph g;
    if (out.downstream.ok && out.downstream.graph.has_value()) {
        g = *out.downstream.graph;
    } else {
        // Downstream KMP absent or unsupported (e.g. OC31 KMP): still populate occupied Sound nodes from soundCatalog
        // so upstream Style/KSC/Pad -> Sound closure resolves accurately!
        for (const SoundSlotMeta& sm : out.downstream.report.soundCatalog) {
            if (!ResourceId::validScope(sm.bank)) continue;
            const ResourceId sid{false, sm.bank, Kind::Sound, sm.number};
            if (!g.find(sid)) {
                std::string gerr;
                (void)g.addNode({sid, sm.name, std::nullopt}, &gerr);
            }
        }
    }

    // Build lookup sets for occupied Sound banks & slots
    std::set<std::string> presentSoundBanks;
    std::map<ResourceId, SoundSlotMeta> soundMetaById;
    for (const SoundSlotMeta& sm : out.downstream.report.soundCatalog) {
        presentSoundBanks.insert(sm.bank);
        soundMetaById.emplace(ResourceId{false, sm.bank, Kind::Sound, sm.number}, sm);
    }

    // Build lookup for unanalysed Sounds from downstream report
    std::map<ResourceId, std::string> unanalysedGapById;
    for (const Unanalysed& u : out.downstream.report.refs.unanalysed) {
        unanalysedGapById[u.sound] = gapName(u.why);
    }

    // 3. Precompute downstream closure once per Sound (Sound -> Oscillator -> Multisample -> Zone -> Sample -> PCM)
    struct SoundDownstreamInfo {
        bool analysed = false;
        std::vector<uint32_t> multisampleIndices;
        std::vector<uint32_t> sampleIndices;
        std::vector<std::string> pcmFiles;
        uint64_t storedPcmBytesProxy = 0;
    };
    std::map<ResourceId, SoundDownstreamInfo> soundDownstream;
    std::map<uint32_t, std::set<ResourceId>> msAnalysedSoundUsers;
    std::map<uint32_t, std::set<ResourceId>> sampleAnalysedSoundUsers;

    // Collect all PCM files present in KMP/PCM inventory (for total & orphan PCM file tracking)
    std::set<std::string> allPcmFiles;
    for (const auto& sr : out.downstream.kmpIndex.samples) {
        if (sr.info && !sr.info->file.empty()) allPcmFiles.insert(sr.info->file);
    }
    for (const auto& pe : out.downstream.report.kmp.unmatchedPcm) {
        if (!pe.file.empty()) allPcmFiles.insert(pe.file);
    }

    if (out.downstream.ok && out.downstream.graph.has_value()) {
        for (const auto& [sid, sm] : soundMetaById) {
            if (!checkKeepGoing(opts.keepGoing, cancelTick)) return makeCancelledResult();
            SoundDownstreamInfo sdi;
            sdi.analysed = (unanalysedGapById.find(sid) == unanalysedGapById.end()) && sm.decoded;
            std::set<uint32_t> msSet, smpSet;
            std::set<std::string> pcmSet;
            const auto cl = g.closure({sid});
            for (const ResourceId& dep : cl) {
                if (dep.kind == Kind::Multisample) {
                    msSet.insert(dep.number);
                    if (sdi.analysed) msAnalysedSoundUsers[dep.number].insert(sid);
                } else if (dep.kind == Kind::Sample) {
                    smpSet.insert(dep.number);
                    if (sdi.analysed) sampleAnalysedSoundUsers[dep.number].insert(sid);
                    if (const Node* sn = g.find(dep); sn && sn->sample) {
                        sdi.storedPcmBytesProxy += sn->sample->bytes();
                        if (!sn->sample->file.empty()) pcmSet.insert(sn->sample->file);
                    }
                }
            }
            sdi.multisampleIndices.assign(msSet.begin(), msSet.end());
            sdi.sampleIndices.assign(smpSet.begin(), smpSet.end());
            sdi.pcmFiles.assign(pcmSet.begin(), pcmSet.end());
            soundDownstream.emplace(sid, std::move(sdi));
        }
    }

    // 4. Add upstream Style/KeyboardSet/Pad nodes & User Sound edges into Graph, and assemble per-root closure entries
    NativeKstDependencyManifest& M = out.manifest;
    M.coverage.readOnly = true;
    M.coverage.deleteProofAvailable = false;
    M.coverage.upstreamScanned = true;
    M.coverage.downstreamKmpPresent = out.downstream.report.kmpPresent;
    M.coverage.downstreamKmpParsed = out.downstream.report.kmpParsed;
    M.coverage.kmpError = out.downstream.report.kmpError;
    M.coverage.songBookPresent = songBookPresent;
    M.coverage.songBookScanned = false;
    M.coverage.drumKitBodyEdgesSupported = false;
    M.coverage.bytesReadDownstream = out.downstream.report.bytesRead;
    M.coverage.bytesReadUpstream = bytesReadUpstream;
    M.coverage.metadirJsonFiles = metadirJsonFiles;

    uint32_t nextStyle = 0, nextKsc = 0, nextPad = 0;
    std::map<ResourceId, std::set<ResourceId>> soundRootUsers;
    std::map<ResourceId, size_t> soundRefOccurrences;
    std::map<uint32_t, std::set<ResourceId>> msRootUsers;
    std::map<uint32_t, std::set<ResourceId>> sampleRootUsers;
    std::map<std::string, std::set<ResourceId>> pcmFileRootUsers;
    std::map<std::string, std::set<ResourceId>> pcmFileSoundUsers;
    for (const auto& [sid, sdi] : soundDownstream) {
        if (!sdi.analysed) continue;
        for (const std::string& pf : sdi.pcmFiles) pcmFileSoundUsers[pf].insert(sid);
    }

    struct MissingAgg {
        MissingRefReason reason = MissingRefReason::SoundBankAbsent;
        bool isDrumKit = false;
        size_t occurrences = 0;
        std::set<ResourceId> roots;
    };
    std::map<ResourceId, MissingAgg> missingSoundMap;

    struct UnknownAddrAgg {
        size_t occurrences = 0;
        std::set<ResourceId> roots;
    };
    std::map<MidiSoundAddress, UnknownAddrAgg> unknownAddrMap;

    auto failGraph = [&](const std::string& msg) -> NativeKstClosureResult {
        extraErrors.push_back(msg);
        std::set<std::string> errSet(out.downstream.report.containerErrors.begin(), out.downstream.report.containerErrors.end());
        errSet.insert(extraErrors.begin(), extraErrors.end());
        M.containerErrors.assign(errSet.begin(), errSet.end());
        std::set<std::string> diagSet(out.downstream.report.diagnostics.begin(), out.downstream.report.diagnostics.end());
        diagSet.insert(extraDiags.begin(), extraDiags.end());
        diagSet.insert(msg);
        M.diagnostics.assign(diagSet.begin(), diagSet.end());
        M.coverage.coverageComplete = false;
        out.ok = false;
        return out;
    };

    M.roots.reserve(extractedRoots.size());
    for (const ExtractedRoot& er : extractedRoots) {
        if (!checkKeepGoing(opts.keepGoing, cancelTick)) return makeCancelledResult();
        uint32_t idx = 0;
        if (er.kind == Kind::Style) { idx = nextStyle++; ++M.summary.styleCount; }
        else if (er.kind == Kind::KeyboardSet) { idx = nextKsc++; ++M.summary.keyboardSetCount; }
        else { idx = nextPad++; ++M.summary.padCount; }

        const ResourceId rootId{false, "KST", er.kind, idx};
        std::string gerr;
        if (!g.addNode({rootId, er.relPath, std::nullopt}, &gerr)) {
            return failGraph("graph addNode failed for " + er.relPath + ": " + gerr);
        }

        RootClosureEntry entry;
        entry.id = rootId;
        entry.kind = er.kind;
        entry.origin = ResourceOrigin::User;
        entry.epistemic = EpistemicStatus::Verified;
        entry.relPath = er.relPath;
        splitRelPathMeta(er.relPath, entry.locationCategory, entry.bankFolder, entry.displayName);
        if (er.kind == Kind::KeyboardSet && entry.locationCategory == "SETTINGS")
            ++M.summary.settingsKeyboardSetCount;
        entry.parseOk = er.parseOk;
        entry.parseError = er.parseError;
        entry.midiLeafCount = er.midiLeafCount;
        entry.malformedMidiLeafCount = er.malformedLeaves.size();
        entry.rawSelectCount = er.rawSelectCount;

        ++M.summary.totalUpstreamRoots;
        if (er.parseOk) ++M.summary.parsedUpstreamRoots;
        else {
            ++M.summary.malformedUpstreamRoots;
            M.malformedTrees.push_back({er.relPath, er.parseError});
        }
        M.summary.midiLeavesTotal += er.midiLeafCount;
        M.summary.malformedMidiLeaves += er.malformedLeaves.size();
        M.summary.rawSelectEvents += er.rawSelectCount;

        for (const auto& ml : er.malformedLeaves) M.malformedMidiLeaves.push_back(ml);
        for (const auto& ub : er.unresolvedBanks) {
            M.unresolvedBankSelects.push_back(ub);
            ++M.summary.unresolvedBankSelects;
            entry.hasUnresolvedBankRef = true;
        }

        // Deduplicated Graph edges per (via, targetSoundId)
        std::map<std::pair<Via, ResourceId>, std::pair<size_t, std::string>> graphEdgeSummary;
        std::set<ResourceId> resolvedSoundsSet, missingSoundsSet;

        for (const auto& [key, agg] : er.refs) {
            const Via via = key.first;
            const MidiSoundAddress& addr = key.second;
            const MidiAddressClassification cls = classifyMidiAddress(addr);

            RootSoundRef rref;
            rref.via = via;
            rref.address = addr;
            rref.origin = cls.origin;
            rref.epistemic = cls.epistemic;
            rref.isDrumKit = cls.isDrumKit;
            rref.targetId = cls.target;
            rref.occurrences = agg.occurrences;
            rref.totalDistinctLeafOrigins = agg.leafOrigins.size();
            size_t kept = 0;
            for (const std::string& lp : agg.leafOrigins) {
                if (kept >= opts.maxLeafOriginsPerRef) break;
                rref.leafOrigins.push_back(lp);
                ++kept;
            }

            if (cls.origin == ResourceOrigin::User && cls.target.has_value()) {
                const ResourceId& tid = *cls.target;
                if (soundMetaById.count(tid)) {
                    rref.resolved = true;
                    rref.missingReason = MissingRefReason::None;
                    M.summary.userSelectsResolved += agg.occurrences;
                    resolvedSoundsSet.insert(tid);
                    soundRootUsers[tid].insert(rootId);
                    soundRefOccurrences[tid] += agg.occurrences;
                } else {
                    rref.resolved = false;
                    const bool bankPresent = presentSoundBanks.count(cls.userBank) > 0;
                    rref.missingReason = bankPresent ? MissingRefReason::SoundSlotEmpty : MissingRefReason::SoundBankAbsent;
                    if (bankPresent) M.summary.userSelectsSlotEmpty += agg.occurrences;
                    else M.summary.userSelectsFileAbsent += agg.occurrences;
                    missingSoundsSet.insert(tid);
                    entry.hasMissingDependency = true;
                    auto& ma = missingSoundMap[tid];
                    ma.reason = rref.missingReason;
                    ma.isDrumKit = cls.isDrumKit;
                    ma.occurrences += agg.occurrences;
                    ma.roots.insert(rootId);
                }
                auto& es = graphEdgeSummary[{via, tid}];
                es.first += agg.occurrences;
                if (es.second.empty()) {
                    es.second = std::to_string(addr.msb) + "/" + std::to_string(addr.lsb) + "/" + std::to_string(addr.program);
                }
            } else if (cls.origin == ResourceOrigin::Factory) {
                M.summary.factorySelects += agg.occurrences;
            } else {
                if (!cls.bankMissing) {
                    M.summary.unknownOriginSelects += agg.occurrences;
                    entry.hasUnknownOriginRef = true;
                    auto& ua = unknownAddrMap[addr];
                    ua.occurrences += agg.occurrences;
                    ua.roots.insert(rootId);
                }
            }
            entry.soundRefs.push_back(std::move(rref));
        }

        // Insert deduplicated User Sound edges into unified Graph
        for (const auto& [ekey, es] : graphEdgeSummary) {
            const std::string detail = es.second + " (x" + std::to_string(es.first) + ")";
            if (!g.addRef({rootId, ekey.second, ekey.first, detail, Source::Package, Confidence::Confirmed, "kst-smf"}, &gerr)) {
                return failGraph("graph addRef failed for " + er.relPath + " -> " + ekey.second.str() + ": " + gerr);
            }
        }

        // Union precomputed downstream closures for all resolved User Sounds referenced by this root
        entry.userSoundIds.assign(resolvedSoundsSet.begin(), resolvedSoundsSet.end());
        entry.missingUserSoundIds.assign(missingSoundsSet.begin(), missingSoundsSet.end());

        std::set<uint32_t> rootMs, rootSamples;
        std::set<std::string> rootPcmFiles;
        for (const ResourceId& sid : entry.userSoundIds) {
            const auto sdiIt = soundDownstream.find(sid);
            if (sdiIt == soundDownstream.end() || !sdiIt->second.analysed) {
                entry.hasUnanalysedUserSound = true;
                continue;
            }
            const SoundDownstreamInfo& sdi = sdiIt->second;
            for (uint32_t mi : sdi.multisampleIndices) {
                rootMs.insert(mi);
                msRootUsers[mi].insert(rootId);
            }
            for (uint32_t si : sdi.sampleIndices) {
                rootSamples.insert(si);
                sampleRootUsers[si].insert(rootId);
            }
            for (const std::string& pf : sdi.pcmFiles) {
                rootPcmFiles.insert(pf);
                pcmFileRootUsers[pf].insert(rootId);
            }
        }
        entry.multisampleIds.reserve(rootMs.size());
        for (uint32_t mi : rootMs) entry.multisampleIds.push_back({false, "KST", Kind::Multisample, mi});
        entry.sampleIds.reserve(rootSamples.size());
        uint64_t rootBytesProxy = 0;
        for (uint32_t si : rootSamples) {
            const ResourceId smpId{false, "KST", Kind::Sample, si};
            entry.sampleIds.push_back(smpId);
            if (const Node* sn = g.find(smpId); sn && sn->sample) rootBytesProxy += sn->sample->bytes();
        }
        entry.pcmFiles.assign(rootPcmFiles.begin(), rootPcmFiles.end());
        entry.storedPcmBytesProxy = rootBytesProxy;

        M.roots.push_back(std::move(entry));
    }

    // 5. Populate User Sound manifest entries & orphan User Sounds
    M.summary.soundContainers = out.downstream.report.soundContainers;
    M.summary.soundSlotsOccupied = soundMetaById.size();
    M.summary.soundsAnalysed = out.downstream.report.refs.analysed;
    M.summary.soundsUnanalysed = soundMetaById.size() - std::min(soundMetaById.size(), out.downstream.report.refs.analysed);
    for (const UnresolvedSlot& u : out.downstream.report.refs.unresolved) {
        if (u.inUnanalysedSound) ++M.summary.unresolvedSoundSlotsInUnanalysed;
        else ++M.summary.unresolvedSoundSlotsInAnalysed;
    }

    M.userSounds.reserve(soundMetaById.size());
    for (const auto& [sid, sm] : soundMetaById) {
        if (!checkKeepGoing(opts.keepGoing, cancelTick)) return makeCancelledResult();
        UserSoundManifestEntry use;
        use.id = sid;
        use.bank = sm.bank;
        use.slot = sm.number;
        use.kindChunkId = sm.kind;
        use.name = sm.name;
        use.sourceFile = sm.source;
        use.origin = ResourceOrigin::User;
        use.isDrumKit = isDrumKitBankName(sm.bank);
        use.decoded = sm.decoded;
        if (!out.downstream.ok) {
            use.analysed = false;
            use.gap = sm.decoded ? "KmpUnavailable" : "DecodeFailed";
            use.downstreamEpistemic = EpistemicStatus::Unsupported;
        } else if (const auto git = unanalysedGapById.find(sid); git != unanalysedGapById.end()) {
            use.analysed = false;
            use.gap = git->second;
            use.downstreamEpistemic = (git->second == "NoKnownReference") ? EpistemicStatus::Inferred : EpistemicStatus::Unsupported;
        } else {
            use.analysed = true;
            use.downstreamEpistemic = EpistemicStatus::Inferred;   // oscillator layout is Inferred
        }
        if (const auto sdiIt = soundDownstream.find(sid); sdiIt != soundDownstream.end()) {
            use.multisampleCount = sdiIt->second.multisampleIndices.size();
            use.sampleCount = sdiIt->second.sampleIndices.size();
            use.pcmFileCount = sdiIt->second.pcmFiles.size();
        }
        if (const auto rit = soundRootUsers.find(sid); rit != soundRootUsers.end()) {
            use.referencingRootIds.assign(rit->second.begin(), rit->second.end());
            use.totalRefOccurrences = soundRefOccurrences[sid];
            use.orphanFromScannedRoots = false;
            if (rit->second.size() > 1) {
                M.sharedUserSounds.push_back({sid.str(), "Sound", ResourceOrigin::User,
                                              rit->second.size(), 1, use.referencingRootIds});
            }
        } else {
            use.orphanFromScannedRoots = true;
            M.orphanUserSounds.push_back(sid);
        }
        M.userSounds.push_back(std::move(use));
    }

    // 6. Downstream RAM Multisample / DrumSample / Sample / PCM sharing & orphan accounting
    M.summary.multisamplesTotal = out.downstream.report.kmp.multisamplesTotal;
    M.summary.drumSamplesTotal = out.downstream.report.kmp.drumsTotal;
    M.summary.duplicateDrumIdGroups = out.downstream.report.kmp.duplicateDrumIds.size();
    M.summary.samplesTotal = out.downstream.report.kmp.samplesTotal;
    M.summary.pcmContainers = out.downstream.report.pcmContainers;
    M.summary.pcmSamplesTotal = out.downstream.report.pcmSamples;
    M.summary.pcmSamplesWithoutId = out.downstream.report.pcmSamplesWithoutId;
    M.summary.pcmFilesTotal = allPcmFiles.size();

    if (out.downstream.ok) {
        for (uint32_t mi = 0; mi < out.downstream.kmpIndex.multisamples.size(); ++mi) {
            const ResourceId mid{false, "KST", Kind::Multisample, mi};
            const auto rit = msRootUsers.find(mi);
            const auto sit = msAnalysedSoundUsers.find(mi);
            const size_t rc = rit == msRootUsers.end() ? 0 : rit->second.size();
            const size_t sc = sit == msAnalysedSoundUsers.end() ? 0 : sit->second.size();
            if (rc > 0) ++M.summary.multisamplesReachableFromRoots;
            else M.orphanMultisamples.push_back(mid);
            if (sc > 0) ++M.summary.multisamplesReachableFromAnalysedSounds;
            else ++M.summary.orphanMultisamplesFromAnalysedSounds;
            if (sc > 1) ++M.summary.sharedMultisamplesAcrossSounds;
            if (rc > 1) {
                M.sharedMultisamples.push_back({mid.str(), "Multisample", ResourceOrigin::Ram, rc, sc, {}});
            }
        }
        M.summary.orphanDrumSamples = out.downstream.kmpIndex.drums.size();   // DK Sound -> DrumSample edges UNSUPPORTED

        for (uint32_t si = 0; si < out.downstream.kmpIndex.samples.size(); ++si) {
            const ResourceId sid{false, "KST", Kind::Sample, si};
            const auto rit = sampleRootUsers.find(si);
            const auto sit = sampleAnalysedSoundUsers.find(si);
            const size_t rc = rit == sampleRootUsers.end() ? 0 : rit->second.size();
            const size_t sc = sit == sampleAnalysedSoundUsers.end() ? 0 : sit->second.size();
            if (rc > 0) ++M.summary.samplesReachableFromRoots;
            else M.orphanSamples.push_back(sid);
            if (sc > 0) ++M.summary.samplesReachableFromAnalysedSounds;
            if (sc > 1) ++M.summary.sharedSamplesAcrossSounds;
            if (rc > 1) {
                M.sharedSamples.push_back({sid.str(), "Sample", ResourceOrigin::Ram, rc, sc, {}});
            }
        }

        for (const std::string& pf : allPcmFiles) {
            const auto rit = pcmFileRootUsers.find(pf);
            const auto sit = pcmFileSoundUsers.find(pf);
            const size_t rc = rit == pcmFileRootUsers.end() ? 0 : rit->second.size();
            const size_t sc = sit == pcmFileSoundUsers.end() ? 0 : sit->second.size();
            if (rc > 0) ++M.summary.pcmFilesReachableFromRoots;
            else M.orphanPcmFiles.push_back(pf);
            if (rc > 1) {
                M.sharedPcmFiles.push_back({pf, "PcmFile", ResourceOrigin::Ram, rc, sc,
                                            std::vector<ResourceId>(rit->second.begin(), rit->second.end())});
            }
        }
    }

    // 7. Missing & anomaly tables (canonically sorted)
    for (const auto& [tid, ma] : missingSoundMap) {
        M.missingUserSounds.push_back({tid, tid.scope, tid.number, ma.isDrumKit, ma.reason, ma.occurrences,
                                       std::vector<ResourceId>(ma.roots.begin(), ma.roots.end())});
    }
    M.missingPcmSamples = out.downstream.report.kmp.missingPcm;
    std::sort(M.missingPcmSamples.begin(), M.missingPcmSamples.end(),
              [](const MissingPcmSample& a, const MissingPcmSample& b) { return a.sampleIndex < b.sampleIndex; });

    M.orphanPcmEntries = out.downstream.report.kmp.unmatchedPcm;
    std::sort(M.orphanPcmEntries.begin(), M.orphanPcmEntries.end(),
              [](const PcmEntry& a, const PcmEntry& b) {
                  return a.file != b.file ? a.file < b.file : a.id < b.id;
              });

    for (const auto& [addr, ua] : unknownAddrMap) {
        M.unknownOriginAddresses.push_back({addr, ua.occurrences, ua.roots.size()});
    }

    M.unanalysedSounds = out.downstream.report.refs.unanalysed;
    std::sort(M.unanalysedSounds.begin(), M.unanalysedSounds.end(),
              [](const Unanalysed& a, const Unanalysed& b) {
                  return a.sound != b.sound ? a.sound < b.sound : static_cast<int>(a.why) < static_cast<int>(b.why);
              });

    M.unresolvedSoundSlots = out.downstream.report.refs.unresolved;
    std::sort(M.unresolvedSoundSlots.begin(), M.unresolvedSoundSlots.end(),
              [](const UnresolvedSlot& a, const UnresolvedSlot& b) {
                  return std::tie(a.sound, a.record, a.slot, a.wrongKind, a.inUnanalysedSound) <
                         std::tie(b.sound, b.record, b.slot, b.wrongKind, b.inUnanalysedSound);
              });

    M.duplicateDrumIds = out.downstream.report.kmp.duplicateDrumIds;
    std::sort(M.duplicateDrumIds.begin(), M.duplicateDrumIds.end(),
              [](const DuplicateKmpId& a, const DuplicateKmpId& b) { return a.id < b.id; });

    std::sort(M.malformedTrees.begin(), M.malformedTrees.end());
    std::sort(M.malformedMidiLeaves.begin(), M.malformedMidiLeaves.end());
    std::sort(M.unresolvedBankSelects.begin(), M.unresolvedBankSelects.end());

    // Deduplicate & sort skippedPaths and containerErrors deterministically
    std::set<std::string> skippedSet(out.downstream.report.skippedPaths.begin(), out.downstream.report.skippedPaths.end());
    skippedSet.insert(extraSkipped.begin(), extraSkipped.end());
    M.skippedPaths.assign(skippedSet.begin(), skippedSet.end());

    std::set<std::string> errSet(out.downstream.report.containerErrors.begin(), out.downstream.report.containerErrors.end());
    errSet.insert(extraErrors.begin(), extraErrors.end());
    M.containerErrors.assign(errSet.begin(), errSet.end());

    // Consolidate deterministic diagnostics
    std::set<std::string> diagSet(out.downstream.report.diagnostics.begin(), out.downstream.report.diagnostics.end());
    diagSet.insert(extraDiags.begin(), extraDiags.end());
    for (const auto& mt : M.malformedTrees)
        diagSet.insert("malformed root tree: " + mt.relPath + ": " + mt.reason);
    for (const auto& ml : M.malformedMidiLeaves)
        diagSet.insert("malformed midi leaf: " + ml.rootRelPath + ":" + ml.leafPath + ": " + ml.reason);
    for (const auto& ub : M.unresolvedBankSelects)
        diagSet.insert("unresolved bank select: " + ub.rootRelPath + ":" + ub.leafPath + " ch " +
                       std::to_string(ub.channel) + " pc " + std::to_string(ub.program));
    for (const auto& ms : M.missingUserSounds)
        diagSet.insert("missing user sound: " + ms.targetId.str() + " (" + missingRefReasonName(ms.reason) +
                       ", " + std::to_string(ms.occurrences) + "x across " + std::to_string(ms.referencingRootIds.size()) + " root(s))");
    if (songBookPresent)
        diagSet.insert("unsupported: SONGBOOK/*.SBD is OC31-encoded and not scanned (delete safety unprovable)");
    M.diagnostics.assign(diagSet.begin(), diagSet.end());

    // Final summary counts
    M.summary.missingUserSoundTargets = M.missingUserSounds.size();
    M.summary.missingPcmSamples = M.missingPcmSamples.size();
    M.summary.danglingGraphEdges = g.dangling().size();
    M.summary.sharedUserSoundsAcrossRoots = M.sharedUserSounds.size();
    M.summary.sharedMultisamplesAcrossRoots = M.sharedMultisamples.size();
    M.summary.sharedSamplesAcrossRoots = M.sharedSamples.size();
    M.summary.sharedPcmFilesAcrossRoots = M.sharedPcmFiles.size();
    M.summary.orphanUserSounds = M.orphanUserSounds.size();
    M.summary.orphanMultisamplesFromRoots = M.orphanMultisamples.size();
    M.summary.orphanSamplesFromRoots = M.orphanSamples.size();
    M.summary.orphanPcmEntries = M.orphanPcmEntries.size();
    M.summary.orphanPcmFilesFromRoots = M.orphanPcmFiles.size();

    // Honest coverage completeness: true ONLY when every upstream & downstream resource is cleanly parsed,
    // no SongBook/unknown-origin/unresolved-bank/dangling/unanalysed/duplicate-drum/skipped/error gap exists.
    M.coverage.coverageComplete =
        out.downstream.ok && out.downstream.report.coverageComplete() &&
        !songBookPresent &&
        M.malformedTrees.empty() && M.malformedMidiLeaves.empty() &&
        M.missingUserSounds.empty() && M.unresolvedBankSelects.empty() &&
        M.unknownOriginAddresses.empty() && M.skippedPaths.empty() &&
        M.containerErrors.empty();

    out.graph = std::move(g);
    out.ok = true;
    return out;
}

}  // namespace closure_detail

// ── In-memory read-only entry point ──────────────────────────────────────────────────────────────────────────────────
// Sorts all downstream and upstream inputs by canonical label/path so output is 100% deterministic regardless of input order.
inline NativeKstClosureResult resolveNativeKstClosure(const NativeKstClosureInput& in,
                                                      const NativeKstClosureOptions& opts = {}) {
    size_t cancelTick = 0;
    if (!closure_detail::checkKeepGoing(opts.keepGoing, cancelTick)) return closure_detail::makeCancelledResult();
    // Canonicalize downstream input order for deterministic results regardless of caller vector order
    NativeKstInput sortedDownstream = in.downstream;
    std::sort(sortedDownstream.pcmContainers.begin(), sortedDownstream.pcmContainers.end(),
              [](const RawNamedBlob& a, const RawNamedBlob& b) { return a.label < b.label; });
    std::sort(sortedDownstream.soundContainers.begin(), sortedDownstream.soundContainers.end(),
              [](const RawNamedBlob& a, const RawNamedBlob& b) {
                  return a.label != b.label ? a.label < b.label : a.bank < b.bank;
              });

    NativeKstResult downstream = resolveNativeKst(sortedDownstream, opts.downstream);

    std::vector<size_t> order(in.upstreamRoots.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return closure_detail::normalizeRelPath(in.upstreamRoots[a].relPath) <
               closure_detail::normalizeRelPath(in.upstreamRoots[b].relPath);
    });

    std::vector<closure_detail::ExtractedRoot> extracted;
    std::vector<std::string> extraErrors, extraSkipped, extraDiags;
    std::set<std::string> seenRelPaths;
    size_t bytesUpstream = 0;

    for (size_t idx : order) {
        if (!closure_detail::checkKeepGoing(opts.keepGoing, cancelTick)) return closure_detail::makeCancelledResult();
        const UpstreamRootBlob& blob = in.upstreamRoots[idx];
        const std::string rel = closure_detail::normalizeRelPath(blob.relPath);
        Kind kind = Kind::Style;
        if (!closure_detail::kindFromUpstreamExt(rel, kind)) {
            extraSkipped.push_back(rel);
            extraDiags.push_back("skipped non-root upstream blob: " + rel);
            continue;
        }
        if (!seenRelPaths.insert(rel).second) {
            extraErrors.push_back("duplicate upstream root path: " + rel);
            continue;
        }
        if (extracted.size() >= opts.maxUpstreamFiles) {
            extraErrors.push_back("upstream root count exceeds maximum limit (" + std::to_string(opts.maxUpstreamFiles) + ")");
            break;
        }
        if (blob.bytes.size() > opts.maxUpstreamFileBytes) {
            closure_detail::ExtractedRoot bad;
            bad.relPath = rel;
            bad.kind = kind;
            bad.parseOk = false;
            bad.parseError = "file larger than the reader's limit";
            extracted.push_back(std::move(bad));
            continue;
        }
        if (blob.bytes.size() > opts.maxUpstreamTotalBytes - bytesUpstream) {
            extraErrors.push_back(rel + ": total upstream read size exceeds the reader's limit");
            break;
        }
        bytesUpstream += blob.bytes.size();
        extracted.push_back(closure_detail::extractUpstreamRootBytes(rel, kind, blob.bytes.data(), blob.bytes.size()));
    }

    return closure_detail::finalizeClosure(std::move(downstream), std::move(extracted), in.songBookPresent,
                                           bytesUpstream, in.metadirJsonFiles, std::move(extraSkipped),
                                           std::move(extraErrors), std::move(extraDiags), opts);
}

// ── On-disk read-only streaming folder entry point ───────────────────────────────────────────────────────────────────
// Walks STYLE/, KEYBOARDSET/, PAD/, SETTINGS/ with O_NOFOLLOW | O_NONBLOCK + S_ISREG + entry/byte caps,
// streaming one Root-tree file at a time (constant memory even on 500+ MB real KSTs), and fuses with
// resolveNativeKstFolder() (MULTISMP/, PCM/, SOUND/) into one unified Graph + deterministic Manifest.
inline NativeKstClosureResult resolveNativeKstClosureFolder(const std::filesystem::path& kstDir,
                                                            const NativeKstClosureOptions& opts = {}) {
    size_t cancelTick = 0;
    if (!closure_detail::checkKeepGoing(opts.keepGoing, cancelTick)) return closure_detail::makeCancelledResult();
    NativeKstClosureResult earlyFail;
    const std::string rawRootPath = kstDir.string();
    const std::string rootPath = native_detail::stripTrailingSlashes(kstDir);
    const native_detail::ScopedFd rootFd(::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (rootFd.fd < 0) {
        const std::string msg = "cannot open KST folder <kst-root> (must be a real directory, symlinks are not followed)";
        earlyFail.manifest.containerErrors.push_back(msg);
        earlyFail.manifest.diagnostics.push_back(msg);
        return earlyFail;
    }

    size_t remainingEntries = opts.maxDirEntries;
    std::vector<std::string> topEntries;
    std::string lerr;
    if (!native_detail::listDirEntriesAt(rootFd.fd, topEntries, remainingEntries, lerr)) {
        const std::string msg = "cannot list KST folder <kst-root>: " + lerr;
        earlyFail.manifest.containerErrors.push_back(msg);
        earlyFail.manifest.diagnostics.push_back(msg);
        return earlyFail;
    }
    remainingEntries -= topEntries.size();

    // Run downstream resolution (MULTISMP/, PCM/, SOUND/) with its own strict O_NOFOLLOW + pread() metadata reader
    NativeKstOptions downOpts = opts.downstream;
    if (opts.maxDirEntries < downOpts.maxDirEntries) downOpts.maxDirEntries = opts.maxDirEntries;
    NativeKstResult downstream = resolveNativeKstFolder(kstDir, downOpts);
    if (!closure_detail::checkKeepGoing(opts.keepGoing, cancelTick)) return closure_detail::makeCancelledResult();

    // Ensure downstream error/diagnostic strings never leak the caller's local filesystem path into manifests.
    auto scrubLocalRoot = [&](std::vector<std::string>& vec) {
        auto replaceAll = [](std::string& s, const std::string& needle) {
            if (needle.empty()) return;
            size_t pos = 0;
            while ((pos = s.find(needle, pos)) != std::string::npos) {
                s.replace(pos, needle.size(), "<kst-root>");
                pos += 10;
            }
        };
        for (std::string& s : vec) {
            replaceAll(s, rawRootPath);
            if (rootPath != rawRootPath) replaceAll(s, rootPath);
        }
    };
    scrubLocalRoot(downstream.report.containerErrors);
    scrubLocalRoot(downstream.report.diagnostics);

    std::vector<closure_detail::ExtractedRoot> extracted;
    std::vector<std::string> extraSkipped, extraErrors, extraDiags;
    bool songBookPresent = false;
    size_t remainingUpstreamBudget = opts.maxUpstreamTotalBytes;
    size_t bytesReadUpstream = 0;
    size_t metadirJsonFiles = 0;
    bool hitFileCountCap = false;
    bool hitByteBudgetCap = false;

    auto streamRootFile = [&](int parentFd, const std::string& leafName, const std::string& rel, Kind kind) {
        if (hitFileCountCap || hitByteBudgetCap) return;
        if (extracted.size() >= opts.maxUpstreamFiles) {
            hitFileCountCap = true;
            extraErrors.push_back("upstream root count exceeds maximum limit (" + std::to_string(opts.maxUpstreamFiles) + ")");
            return;
        }
        std::vector<uint8_t> bytes;
        std::string rerr;
        if (!native_detail::readRegularAt(parentFd, leafName.c_str(), opts.maxUpstreamFileBytes,
                                          remainingUpstreamBudget, bytes, rerr)) {
            if (rerr.find("total read size exceeds") != std::string::npos) {
                hitByteBudgetCap = true;
                extraErrors.push_back(rel + ": " + rerr);
            } else {
                closure_detail::ExtractedRoot bad;
                bad.relPath = rel;
                bad.kind = kind;
                bad.parseOk = false;
                bad.parseError = rerr;
                extracted.push_back(std::move(bad));
            }
            return;
        }
        remainingUpstreamBudget -= bytes.size();
        bytesReadUpstream += bytes.size();
        // Parse and extract immediately; raw bytes and Tree are freed at scope exit!
        extracted.push_back(closure_detail::extractUpstreamRootBytes(rel, kind, bytes.data(), bytes.size()));
    };

    for (const std::string& top : topEntries) {
        if (!closure_detail::checkKeepGoing(opts.keepGoing, cancelTick)) return closure_detail::makeCancelledResult();
        struct stat st{};
        if (::fstatat(rootFd.fd, top.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            extraErrors.push_back("cannot stat entry " + top);
            continue;
        }
        const std::string topUpper = native_detail::upperAscii(top);
        const bool isBankedRootDir = (topUpper == "STYLE" || topUpper == "KEYBOARDSET" || topUpper == "PAD");
        const bool isSettingsDir = (topUpper == "SETTINGS");
        if (topUpper == "SONGBOOK" && S_ISDIR(st.st_mode)) {
            songBookPresent = true;
            continue;
        }
        if (topUpper == "GLOBAL") {
            extraSkipped.push_back(top);
            if (S_ISDIR(st.st_mode)) {
                extraDiags.push_back("unsupported: " + top + "/ is not scanned (coverage incomplete)");
            } else {
                extraErrors.push_back("refusing non-directory/symlink at " + top);
            }
            continue;
        }
        if (!isBankedRootDir && !isSettingsDir) continue;

        if (!S_ISDIR(st.st_mode)) {
            extraSkipped.push_back(top);
            extraErrors.push_back("refusing non-directory/symlink at " + top);
            continue;
        }

        const native_detail::ScopedFd subFd(::openat(rootFd.fd, top.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (subFd.fd < 0) {
            extraErrors.push_back("cannot open folder " + top + " (symlinks are not followed)");
            continue;
        }
        std::vector<std::string> level1;
        std::string serr;
        if (!native_detail::listDirEntriesAt(subFd.fd, level1, remainingEntries, serr)) {
            extraErrors.push_back("cannot list folder " + top + ": " + serr);
            continue;
        }
        remainingEntries -= level1.size();

        for (const std::string& e1 : level1) {
            if (!closure_detail::checkKeepGoing(opts.keepGoing, cancelTick)) return closure_detail::makeCancelledResult();
            const std::string rel1 = top + "/" + e1;
            struct stat st1{};
            if (::fstatat(subFd.fd, e1.c_str(), &st1, AT_SYMLINK_NOFOLLOW) != 0) {
                extraErrors.push_back("cannot stat " + rel1);
                continue;
            }
            if (S_ISREG(st1.st_mode)) {
                if (native_detail::upperAscii(e1) == "METADIR.JSON") {
                    ++metadirJsonFiles;
                    continue;
                }
                Kind k = Kind::Style;
                if (closure_detail::kindFromUpstreamExt(e1, k)) {
                    streamRootFile(subFd.fd, e1, rel1, k);
                } else {
                    extraSkipped.push_back(rel1);
                    if (isSettingsDir && native_detail::endsWithUpper(e1, ".GBL")) {
                        extraDiags.push_back("unsupported unparsed settings file: " + rel1);
                    } else {
                        extraDiags.push_back("skipped unexpected regular file: " + rel1);
                    }
                }
                continue;
            }
            if (!S_ISDIR(st1.st_mode)) {
                extraSkipped.push_back(rel1);
                extraErrors.push_back("refusing non-regular file or symlink: " + rel1);
                continue;
            }
            if (isSettingsDir) {
                extraSkipped.push_back(rel1);
                extraDiags.push_back("skipped unexpected nested directory: " + rel1);
                continue;
            }

            // Bank folder inside STYLE/, KEYBOARDSET/, or PAD/
            const native_detail::ScopedFd bankFd(::openat(subFd.fd, e1.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
            if (bankFd.fd < 0) {
                extraErrors.push_back("cannot open bank folder " + rel1 + " (symlinks are not followed)");
                continue;
            }
            std::vector<std::string> level2;
            std::string berr;
            if (!native_detail::listDirEntriesAt(bankFd.fd, level2, remainingEntries, berr)) {
                extraErrors.push_back("cannot list bank folder " + rel1 + ": " + berr);
                continue;
            }
            remainingEntries -= level2.size();

            for (const std::string& e2 : level2) {
                if (!closure_detail::checkKeepGoing(opts.keepGoing, cancelTick)) return closure_detail::makeCancelledResult();
                const std::string rel2 = rel1 + "/" + e2;
                struct stat st2{};
                if (::fstatat(bankFd.fd, e2.c_str(), &st2, AT_SYMLINK_NOFOLLOW) != 0) {
                    extraErrors.push_back("cannot stat " + rel2);
                    continue;
                }
                if (!S_ISREG(st2.st_mode)) {
                    extraSkipped.push_back(rel2);
                    if (S_ISDIR(st2.st_mode)) {
                        extraDiags.push_back("skipped unexpected nested directory: " + rel2);
                    } else {
                        extraErrors.push_back("refusing non-regular file or symlink: " + rel2);
                    }
                    continue;
                }
                if (native_detail::upperAscii(e2) == "METADIR.JSON") {
                    ++metadirJsonFiles;
                    continue;
                }
                Kind k = Kind::Style;
                if (closure_detail::kindFromUpstreamExt(e2, k)) {
                    streamRootFile(bankFd.fd, e2, rel2, k);
                } else {
                    extraSkipped.push_back(rel2);
                    extraDiags.push_back("skipped unexpected regular file: " + rel2);
                }
            }
        }
    }

    return closure_detail::finalizeClosure(std::move(downstream), std::move(extracted), songBookPresent,
                                           bytesReadUpstream, metadirJsonFiles, std::move(extraSkipped),
                                           std::move(extraErrors), std::move(extraDiags), opts);
}

// ── Canonical Deterministic Manifest JSON Serializer ─────────────────────────────────────────────────────────────────
// Produces byte-identical JSON across repeated runs and differing input/directory orderings.
// Never emits timestamps, pointer addresses, or local absolute paths.
inline std::string serializeManifestJson(const NativeKstDependencyManifest& m,
                                         const ManifestJsonOptions& jopts = {}) {
    auto q = [](const std::string& s) { return "\"" + closure_detail::manifestJsonEscape(s) + "\""; };
    auto bstr = [](bool b) -> const char* { return b ? "true" : "false"; };
    auto writeStrVec = [&](std::string& o, const std::vector<std::string>& v) {
        o += "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ", "; o += q(v[i]); }
        o += "]";
    };
    auto writeIdVec = [&](std::string& o, const std::vector<ResourceId>& v) {
        o += "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ", "; o += q(v[i].str()); }
        o += "]";
    };

    std::string o;
    o.reserve(4096 + m.roots.size() * 256 + m.userSounds.size() * 128);
    o += "{\n";
    o += "  \"schema\": " + q(m.schema) + ",\n";
    o += "  \"readOnly\": " + std::string(bstr(m.coverage.readOnly)) + ",\n";
    o += "  \"deleteProofAvailable\": " + std::string(bstr(m.coverage.deleteProofAvailable)) + ",\n";

    // Epistemic policy map
    o += "  \"epistemicPolicy\": {\n";
    o += "    \"rootTreeContainer\": \"VERIFIED\",\n";
    o += "    \"smfSoundSelect\": \"VERIFIED\",\n";
    o += "    \"userSoundBankAddress\": \"VERIFIED\",\n";
    o += "    \"factorySoundBankAddress\": \"INFERRED\",\n";
    o += "    \"unknownOriginBankAddress\": \"UNSUPPORTED\",\n";
    o += "    \"korfAndOc31\": \"VERIFIED\",\n";
    o += "    \"pa5xRamKmp\": \"VERIFIED\",\n";
    o += "    \"oc31LegacyKmp\": \"UNSUPPORTED\",\n";
    o += "    \"soundOscillatorLayout\": \"INFERRED\",\n";
    o += "    \"storedPcmBytesProxy\": \"INFERRED\",\n";
    o += "    \"drumKitSoundBody\": \"UNSUPPORTED\",\n";
    o += "    \"songBookAndGlobal\": \"UNSUPPORTED\"\n";
    o += "  },\n";

    // Coverage honesty flags
    o += "  \"coverage\": {\n";
    o += "    \"upstreamScanned\": " + std::string(bstr(m.coverage.upstreamScanned)) + ",\n";
    o += "    \"downstreamKmpPresent\": " + std::string(bstr(m.coverage.downstreamKmpPresent)) + ",\n";
    o += "    \"downstreamKmpParsed\": " + std::string(bstr(m.coverage.downstreamKmpParsed)) + ",\n";
    o += "    \"kmpError\": " + q(m.coverage.kmpError) + ",\n";
    o += "    \"songBookPresent\": " + std::string(bstr(m.coverage.songBookPresent)) + ",\n";
    o += "    \"songBookScanned\": " + std::string(bstr(m.coverage.songBookScanned)) + ",\n";
    o += "    \"drumKitBodyEdgesSupported\": " + std::string(bstr(m.coverage.drumKitBodyEdgesSupported)) + ",\n";
    o += "    \"coverageComplete\": " + std::string(bstr(m.coverage.coverageComplete)) + ",\n";
    o += "    \"bytesReadDownstream\": " + std::to_string(m.coverage.bytesReadDownstream) + ",\n";
    o += "    \"bytesReadUpstream\": " + std::to_string(m.coverage.bytesReadUpstream) + ",\n";
    o += "    \"metadirJsonFiles\": " + std::to_string(m.coverage.metadirJsonFiles) + "\n";
    o += "  },\n";

    // Summary counts
    const auto& s = m.summary;
    o += "  \"summary\": {\n";
    o += "    \"styles\": " + std::to_string(s.styleCount) + ",\n";
    o += "    \"keyboardSets\": " + std::to_string(s.keyboardSetCount) + ",\n";
    o += "    \"settingsKeyboardSets\": " + std::to_string(s.settingsKeyboardSetCount) + ",\n";
    o += "    \"pads\": " + std::to_string(s.padCount) + ",\n";
    o += "    \"totalUpstreamRoots\": " + std::to_string(s.totalUpstreamRoots) + ",\n";
    o += "    \"parsedUpstreamRoots\": " + std::to_string(s.parsedUpstreamRoots) + ",\n";
    o += "    \"malformedUpstreamRoots\": " + std::to_string(s.malformedUpstreamRoots) + ",\n";
    o += "    \"midiLeavesTotal\": " + std::to_string(s.midiLeavesTotal) + ",\n";
    o += "    \"malformedMidiLeaves\": " + std::to_string(s.malformedMidiLeaves) + ",\n";
    o += "    \"rawSelectEvents\": " + std::to_string(s.rawSelectEvents) + ",\n";
    o += "    \"userSelectsResolved\": " + std::to_string(s.userSelectsResolved) + ",\n";
    o += "    \"userSelectsFileAbsent\": " + std::to_string(s.userSelectsFileAbsent) + ",\n";
    o += "    \"userSelectsSlotEmpty\": " + std::to_string(s.userSelectsSlotEmpty) + ",\n";
    o += "    \"factorySelects\": " + std::to_string(s.factorySelects) + ",\n";
    o += "    \"unknownOriginSelects\": " + std::to_string(s.unknownOriginSelects) + ",\n";
    o += "    \"unresolvedBankSelects\": " + std::to_string(s.unresolvedBankSelects) + ",\n";
    o += "    \"soundContainers\": " + std::to_string(s.soundContainers) + ",\n";
    o += "    \"soundSlotsOccupied\": " + std::to_string(s.soundSlotsOccupied) + ",\n";
    o += "    \"soundsAnalysed\": " + std::to_string(s.soundsAnalysed) + ",\n";
    o += "    \"soundsUnanalysed\": " + std::to_string(s.soundsUnanalysed) + ",\n";
    o += "    \"unresolvedSoundSlotsInAnalysed\": " + std::to_string(s.unresolvedSoundSlotsInAnalysed) + ",\n";
    o += "    \"unresolvedSoundSlotsInUnanalysed\": " + std::to_string(s.unresolvedSoundSlotsInUnanalysed) + ",\n";
    o += "    \"multisamplesTotal\": " + std::to_string(s.multisamplesTotal) + ",\n";
    o += "    \"multisamplesReachableFromRoots\": " + std::to_string(s.multisamplesReachableFromRoots) + ",\n";
    o += "    \"multisamplesReachableFromAnalysedSounds\": " + std::to_string(s.multisamplesReachableFromAnalysedSounds) + ",\n";
    o += "    \"drumSamplesTotal\": " + std::to_string(s.drumSamplesTotal) + ",\n";
    o += "    \"duplicateDrumIdGroups\": " + std::to_string(s.duplicateDrumIdGroups) + ",\n";
    o += "    \"samplesTotal\": " + std::to_string(s.samplesTotal) + ",\n";
    o += "    \"samplesReachableFromRoots\": " + std::to_string(s.samplesReachableFromRoots) + ",\n";
    o += "    \"samplesReachableFromAnalysedSounds\": " + std::to_string(s.samplesReachableFromAnalysedSounds) + ",\n";
    o += "    \"pcmContainers\": " + std::to_string(s.pcmContainers) + ",\n";
    o += "    \"pcmSamplesTotal\": " + std::to_string(s.pcmSamplesTotal) + ",\n";
    o += "    \"pcmSamplesWithoutId\": " + std::to_string(s.pcmSamplesWithoutId) + ",\n";
    o += "    \"pcmFilesTotal\": " + std::to_string(s.pcmFilesTotal) + ",\n";
    o += "    \"pcmFilesReachableFromRoots\": " + std::to_string(s.pcmFilesReachableFromRoots) + ",\n";
    o += "    \"missingUserSoundTargets\": " + std::to_string(s.missingUserSoundTargets) + ",\n";
    o += "    \"missingPcmSamples\": " + std::to_string(s.missingPcmSamples) + ",\n";
    o += "    \"danglingGraphEdges\": " + std::to_string(s.danglingGraphEdges) + ",\n";
    o += "    \"sharedUserSoundsAcrossRoots\": " + std::to_string(s.sharedUserSoundsAcrossRoots) + ",\n";
    o += "    \"sharedMultisamplesAcrossRoots\": " + std::to_string(s.sharedMultisamplesAcrossRoots) + ",\n";
    o += "    \"sharedMultisamplesAcrossSounds\": " + std::to_string(s.sharedMultisamplesAcrossSounds) + ",\n";
    o += "    \"sharedSamplesAcrossRoots\": " + std::to_string(s.sharedSamplesAcrossRoots) + ",\n";
    o += "    \"sharedSamplesAcrossSounds\": " + std::to_string(s.sharedSamplesAcrossSounds) + ",\n";
    o += "    \"sharedPcmFilesAcrossRoots\": " + std::to_string(s.sharedPcmFilesAcrossRoots) + ",\n";
    o += "    \"orphanUserSounds\": " + std::to_string(s.orphanUserSounds) + ",\n";
    o += "    \"orphanMultisamplesFromRoots\": " + std::to_string(s.orphanMultisamplesFromRoots) + ",\n";
    o += "    \"orphanMultisamplesFromAnalysedSounds\": " + std::to_string(s.orphanMultisamplesFromAnalysedSounds) + ",\n";
    o += "    \"orphanDrumSamples\": " + std::to_string(s.orphanDrumSamples) + ",\n";
    o += "    \"orphanSamplesFromRoots\": " + std::to_string(s.orphanSamplesFromRoots) + ",\n";
    o += "    \"orphanPcmEntries\": " + std::to_string(s.orphanPcmEntries) + ",\n";
    o += "    \"orphanPcmFilesFromRoots\": " + std::to_string(s.orphanPcmFilesFromRoots) + "\n";
    o += "  },\n";

    // Per-root closures
    o += "  \"roots\": [";
    for (size_t i = 0; i < m.roots.size(); ++i) {
        const auto& r = m.roots[i];
        o += i ? ",\n    {\n" : "\n    {\n";
        o += "      \"id\": " + q(r.id.str()) + ",\n";
        o += "      \"kind\": " + q(kindName(r.kind)) + ",\n";
        o += "      \"origin\": " + q(resourceOriginName(r.origin)) + ",\n";
        o += "      \"epistemic\": " + q(epistemicStatusName(r.epistemic)) + ",\n";
        o += "      \"relPath\": " + q(r.relPath) + ",\n";
        o += "      \"locationCategory\": " + q(r.locationCategory) + ",\n";
        o += "      \"bankFolder\": " + q(r.bankFolder) + ",\n";
        o += "      \"displayName\": " + q(r.displayName) + ",\n";
        o += "      \"parseOk\": " + std::string(bstr(r.parseOk)) + ",\n";
        if (!r.parseOk) o += "      \"parseError\": " + q(r.parseError) + ",\n";
        o += "      \"midiLeafCount\": " + std::to_string(r.midiLeafCount) + ",\n";
        o += "      \"malformedMidiLeafCount\": " + std::to_string(r.malformedMidiLeafCount) + ",\n";
        o += "      \"rawSelectCount\": " + std::to_string(r.rawSelectCount) + ",\n";
        o += "      \"soundRefs\": [";
        for (size_t j = 0; j < r.soundRefs.size(); ++j) {
            const auto& sr = r.soundRefs[j];
            o += j ? ",\n        {" : "\n        {";
            o += "\"via\": " + q(viaName(sr.via)) +
                 ", \"msb\": " + std::to_string(sr.address.msb) +
                 ", \"lsb\": " + std::to_string(sr.address.lsb) +
                 ", \"program\": " + std::to_string(sr.address.program) +
                 ", \"origin\": " + q(resourceOriginName(sr.origin)) +
                 ", \"epistemic\": " + q(epistemicStatusName(sr.epistemic)) +
                 ", \"isDrumKit\": " + std::string(bstr(sr.isDrumKit)) +
                 ", \"targetId\": " + (sr.targetId ? q(sr.targetId->str()) : std::string("null")) +
                 ", \"resolved\": " + std::string(bstr(sr.resolved)) +
                 ", \"missingReason\": " + q(missingRefReasonName(sr.missingReason)) +
                 ", \"occurrences\": " + std::to_string(sr.occurrences) +
                 ", \"totalDistinctLeafOrigins\": " + std::to_string(sr.totalDistinctLeafOrigins) +
                 ", \"leafOrigins\": ";
            writeStrVec(o, sr.leafOrigins);
            o += "}";
        }
        o += r.soundRefs.empty() ? "],\n" : "\n      ],\n";
        o += "      \"closure\": {\n";
        o += "        \"userSoundIds\": "; writeIdVec(o, r.userSoundIds); o += ",\n";
        o += "        \"missingUserSoundIds\": "; writeIdVec(o, r.missingUserSoundIds); o += ",\n";
        o += "        \"multisampleIds\": "; writeIdVec(o, r.multisampleIds); o += ",\n";
        o += "        \"sampleCount\": " + std::to_string(r.sampleIds.size()) + ",\n";
        if (jopts.includePerRootSampleIds) {
            o += "        \"sampleIds\": "; writeIdVec(o, r.sampleIds); o += ",\n";
        }
        o += "        \"pcmFiles\": "; writeStrVec(o, r.pcmFiles); o += ",\n";
        o += "        \"storedPcmBytesProxy\": " + std::to_string(r.storedPcmBytesProxy) + ",\n";
        o += "        \"hasUnanalysedUserSound\": " + std::string(bstr(r.hasUnanalysedUserSound)) + ",\n";
        o += "        \"hasMissingDependency\": " + std::string(bstr(r.hasMissingDependency)) + ",\n";
        o += "        \"hasUnknownOriginRef\": " + std::string(bstr(r.hasUnknownOriginRef)) + ",\n";
        o += "        \"hasUnresolvedBankRef\": " + std::string(bstr(r.hasUnresolvedBankRef)) + "\n";
        o += "      }\n";
        o += "    }";
    }
    o += m.roots.empty() ? "],\n" : "\n  ],\n";

    // Occupied User Sounds
    o += "  \"userSounds\": [";
    for (size_t i = 0; i < m.userSounds.size(); ++i) {
        const auto& u = m.userSounds[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"id\": " + q(u.id.str()) +
             ", \"bank\": " + q(u.bank) +
             ", \"slot\": " + std::to_string(u.slot) +
             ", \"kindChunkId\": " + q(closure_detail::hexU32(u.kindChunkId)) +
             ", \"name\": " + q(u.name) +
             ", \"sourceFile\": " + q(u.sourceFile) +
             ", \"origin\": " + q(resourceOriginName(u.origin)) +
             ", \"isDrumKit\": " + std::string(bstr(u.isDrumKit)) +
             ", \"decoded\": " + std::string(bstr(u.decoded)) +
             ", \"analysed\": " + std::string(bstr(u.analysed)) +
             ", \"gap\": " + q(u.gap) +
             ", \"downstreamEpistemic\": " + q(epistemicStatusName(u.downstreamEpistemic)) +
             ", \"referencingRootCount\": " + std::to_string(u.referencingRootIds.size()) +
             ", \"totalRefOccurrences\": " + std::to_string(u.totalRefOccurrences) +
             ", \"multisampleCount\": " + std::to_string(u.multisampleCount) +
             ", \"sampleCount\": " + std::to_string(u.sampleCount) +
             ", \"pcmFileCount\": " + std::to_string(u.pcmFileCount) +
             ", \"orphanFromScannedRoots\": " + std::string(bstr(u.orphanFromScannedRoots)) + "}";
    }
    o += m.userSounds.empty() ? "],\n" : "\n  ],\n";

    // Shared resources across multiple roots
    auto writeSharedVec = [&](const char* key, const std::vector<SharedResourceEntry>& vec, bool withRoots, bool trailingComma) {
        o += "    " + q(key) + ": [";
        for (size_t i = 0; i < vec.size(); ++i) {
            const auto& sh = vec[i];
            o += i ? ",\n      {" : "\n      {";
            o += "\"id\": " + q(sh.id) +
                 ", \"kind\": " + q(sh.kind) +
                 ", \"origin\": " + q(resourceOriginName(sh.origin)) +
                 ", \"referencingRootCount\": " + std::to_string(sh.referencingRootCount) +
                 ", \"referencingSoundCount\": " + std::to_string(sh.referencingSoundCount);
            if (withRoots) { o += ", \"referencingRootIds\": "; writeIdVec(o, sh.referencingRootIds); }
            o += "}";
        }
        o += vec.empty() ? "]" : "\n    ]";
        if (trailingComma) o += ",";
        o += "\n";
    };
    o += "  \"shared\": {\n";
    writeSharedVec("userSounds", m.sharedUserSounds, true, true);
    writeSharedVec("multisamples", m.sharedMultisamples, false, true);
    writeSharedVec("pcmFiles", m.sharedPcmFiles, true, true);
    o += "    \"sharedSampleCount\": " + std::to_string(m.sharedSamples.size());
    if (jopts.includeSampleLists) {
        o += ",\n";
        writeSharedVec("samples", m.sharedSamples, false, false);
    } else {
        o += "\n";
    }
    o += "  },\n";

    // Missing references
    o += "  \"missing\": {\n";
    o += "    \"userSounds\": [";
    for (size_t i = 0; i < m.missingUserSounds.size(); ++i) {
        const auto& ms = m.missingUserSounds[i];
        o += i ? ",\n      {" : "\n      {";
        o += "\"targetId\": " + q(ms.targetId.str()) +
             ", \"bank\": " + q(ms.bank) +
             ", \"slot\": " + std::to_string(ms.slot) +
             ", \"isDrumKit\": " + std::string(bstr(ms.isDrumKit)) +
             ", \"reason\": " + q(missingRefReasonName(ms.reason)) +
             ", \"occurrences\": " + std::to_string(ms.occurrences) +
             ", \"referencingRootIds\": ";
        writeIdVec(o, ms.referencingRootIds);
        o += "}";
    }
    o += m.missingUserSounds.empty() ? "],\n" : "\n    ],\n";
    o += "    \"pcmSamples\": [";
    for (size_t i = 0; i < m.missingPcmSamples.size(); ++i) {
        const auto& mp = m.missingPcmSamples[i];
        o += i ? ",\n      {" : "\n      {";
        o += "\"sampleIndex\": " + std::to_string(mp.sampleIndex) +
             ", \"idHex\": " + q(native_detail::hexId(mp.id)) +
             ", \"name\": " + q(mp.name) +
             ", \"reason\": " + q(mp.reason) + "}";
    }
    o += m.missingPcmSamples.empty() ? "]\n" : "\n    ]\n";
    o += "  },\n";

    // Orphans (explicitly NOT provably deletable)
    o += "  \"orphans\": {\n";
    o += "    \"notProvablyDeletable\": true,\n";
    o += "    \"userSounds\": "; writeIdVec(o, m.orphanUserSounds); o += ",\n";
    o += "    \"multisamples\": "; writeIdVec(o, m.orphanMultisamples); o += ",\n";
    o += "    \"pcmFiles\": "; writeStrVec(o, m.orphanPcmFiles); o += ",\n";
    o += "    \"unmatchedPcmEntries\": [";
    for (size_t i = 0; i < m.orphanPcmEntries.size(); ++i) {
        const auto& pe = m.orphanPcmEntries[i];
        o += i ? ",\n      {" : "\n      {";
        o += "\"idHex\": " + q(native_detail::hexId(pe.id)) +
             ", \"name\": " + q(pe.name) +
             ", \"file\": " + q(pe.file) +
             ", \"payloadBytes\": " + std::to_string(pe.payloadBytes) + "}";
    }
    o += m.orphanPcmEntries.empty() ? "],\n" : "\n    ],\n";
    o += "    \"orphanDrumSampleCount\": " + std::to_string(m.summary.orphanDrumSamples) + ",\n";
    o += "    \"orphanSampleCount\": " + std::to_string(m.orphanSamples.size());
    if (jopts.includeSampleLists) {
        o += ",\n    \"samples\": "; writeIdVec(o, m.orphanSamples); o += "\n";
    } else {
        o += "\n";
    }
    o += "  },\n";

    // Anomalies & unresolved entries
    o += "  \"anomalies\": {\n";
    o += "    \"malformedTrees\": [";
    for (size_t i = 0; i < m.malformedTrees.size(); ++i) {
        o += i ? ",\n      {" : "\n      {";
        o += "\"relPath\": " + q(m.malformedTrees[i].relPath) + ", \"reason\": " + q(m.malformedTrees[i].reason) + "}";
    }
    o += m.malformedTrees.empty() ? "],\n" : "\n    ],\n";

    o += "    \"malformedMidiLeaves\": [";
    for (size_t i = 0; i < m.malformedMidiLeaves.size(); ++i) {
        o += i ? ",\n      {" : "\n      {";
        o += "\"rootRelPath\": " + q(m.malformedMidiLeaves[i].rootRelPath) +
             ", \"leafPath\": " + q(m.malformedMidiLeaves[i].leafPath) +
             ", \"reason\": " + q(m.malformedMidiLeaves[i].reason) + "}";
    }
    o += m.malformedMidiLeaves.empty() ? "],\n" : "\n    ],\n";

    o += "    \"unresolvedBankSelects\": [";
    for (size_t i = 0; i < m.unresolvedBankSelects.size(); ++i) {
        const auto& ub = m.unresolvedBankSelects[i];
        o += i ? ",\n      {" : "\n      {";
        o += "\"rootRelPath\": " + q(ub.rootRelPath) +
             ", \"leafPath\": " + q(ub.leafPath) +
             ", \"channel\": " + std::to_string(ub.channel) +
             ", \"msb\": " + std::to_string(ub.msb) +
             ", \"lsb\": " + std::to_string(ub.lsb) +
             ", \"program\": " + std::to_string(ub.program) + "}";
    }
    o += m.unresolvedBankSelects.empty() ? "],\n" : "\n    ],\n";

    o += "    \"unknownOriginAddresses\": [";
    for (size_t i = 0; i < m.unknownOriginAddresses.size(); ++i) {
        const auto& ua = m.unknownOriginAddresses[i];
        o += i ? ",\n      {" : "\n      {";
        o += "\"msb\": " + std::to_string(ua.address.msb) +
             ", \"lsb\": " + std::to_string(ua.address.lsb) +
             ", \"program\": " + std::to_string(ua.address.program) +
             ", \"occurrences\": " + std::to_string(ua.occurrences) +
             ", \"rootCount\": " + std::to_string(ua.rootCount) + "}";
    }
    o += m.unknownOriginAddresses.empty() ? "],\n" : "\n    ],\n";

    o += "    \"unresolvedSoundSlotsInAnalysed\": [";
    {
        size_t emitted = 0;
        for (const auto& u : m.unresolvedSoundSlots) {
            if (u.inUnanalysedSound) continue;
            o += emitted ? ",\n      {" : "\n      {";
            o += "\"sound\": " + q(u.sound.str()) +
                 ", \"record\": " + std::to_string(u.record) +
                 ", \"slot\": " + std::to_string(u.slot) +
                 ", \"wrongKind\": " + std::string(bstr(u.wrongKind)) + "}";
            ++emitted;
        }
        o += emitted == 0 ? "],\n" : "\n    ],\n";
    }
    o += "    \"unresolvedSoundSlotsInUnanalysedCount\": " + std::to_string(m.summary.unresolvedSoundSlotsInUnanalysed) + ",\n";

    o += "    \"duplicateDrumIds\": [";
    for (size_t i = 0; i < m.duplicateDrumIds.size(); ++i) {
        const auto& d = m.duplicateDrumIds[i];
        o += i ? ",\n      {" : "\n      {";
        o += "\"idHex\": " + q(native_detail::hexId(d.id)) + ", \"indices\": [";
        for (size_t j = 0; j < d.indices.size(); ++j) { if (j) o += ", "; o += std::to_string(d.indices[j]); }
        o += "]}";
    }
    o += m.duplicateDrumIds.empty() ? "],\n" : "\n    ],\n";

    o += "    \"skippedPaths\": "; writeStrVec(o, m.skippedPaths); o += ",\n";
    o += "    \"containerErrors\": "; writeStrVec(o, m.containerErrors); o += "\n";
    o += "  }\n";
    o += "}\n";
    return o;
}

}  // namespace ai_arranger::media::graph

#endif  // AI_ARRANGER_MEDIA_SOUND_NATIVE_KST_CLOSURE_H
