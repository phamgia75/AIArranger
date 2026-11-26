#ifndef AI_ARRANGER_MEDIA_SOUND_NATIVE_REFS_H
#define AI_ARRANGER_MEDIA_SOUND_NATIVE_REFS_H

// Read-only adapter & integration: Pa-native KORF containers + OC31/OC32 Sound payloads (media/panative/oc31.h) +
// Pa5X raw RAM.KMP (media/panative/pa5x_kmp.h) -> the existing Resource Graph (media/sound/resource_graph.h).
// Research basis: owner research SOUND_STRUCTURE_FINDINGS.md and KMP_MULTISAMPLE_ANALYSIS.md (one owner's Pa5X backup;
// OFFLINE RESEARCH VALIDATED there only).
//
// Boundaries (STRICTLY READ-ONLY):
//  * Read-only: never writes, modifies, or deletes any KST/SET file; no OC31 encoder; no Native Sound KST export.
//  * Both low-level (caller supplies already-decoded Sound bodies + KmpIndex) and end-to-end read-only integration
//    (decodeOc31() + parsePa5xKmp() + PCM inventory -> Sound -> Oscillator -> Multisample -> Zone -> Sample -> PCM,
//    plus DrumSample -> L/R Sample -> PCM) are supported.
//  * No layout is built in. A RecordLayout comes from fitLayout() over the caller's own data, or from the caller; a kind
//    without a layout, or an item whose size does not fit it, is reported UNANALYSED, never guessed.
//  * What is a fact: a slot holds the 8-byte ID of a KMP object (Confirmed only where the KMP index supplies the chain).
//    What is not: that a record is an oscillator and slot 0/1 is High/Low. Those edges are Confidence::Inferred.
//  * Unreferenced is never concluded. The graph is not declared complete or scanned, so deleteCheck() can never say
//    Unreferenced for a resource built here; this file only lists dependencies, shared use and gaps.
//  * Native KST reading stays unavailable in the GUI product: nothing here is wired into apps/macos/.

#include "media/panative/oc31.h"
#include "media/panative/pa5x_kmp.h"
#include "media/panative/panative_reader.h"
#include "media/sound/resource_graph.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ai_arranger::media::graph {

using Id8 = std::array<uint8_t, 8>;
constexpr uint32_t kNoZoneSample = 0xFFFFFFFFu;
static_assert(panative::kNoIndex == kNoZoneSample, "Pa5xKmp kNoIndex must match kNoZoneSample");

struct KmpIndex {                       // filled from parsePa5xKmp() via kmpIndexFromPa5x(), or directly by caller
    struct SampleRow { Id8 id{}; std::string name; std::optional<SampleInfo> info; };   // no info -> the Sample node is absent (dangling)
    struct Multi { Id8 id{}; std::string name; std::vector<uint32_t> zoneSamples; };    // sample-table index per zone, or kNoZoneSample
    struct Drum {
        Id8 id{};
        std::string name;
        uint32_t leftSample = kNoZoneSample;
        uint32_t rightSample = kNoZoneSample;
        // INFERRED advisory metadata (+113 in RAM.KMP drum record: 0=B..7=X, 0xFF=None); NEVER used for edges/equivalence.
        uint8_t drumFamilyByte = 0xFF;
    };
    std::vector<SampleRow> samples;
    std::vector<Multi> multisamples;
    std::set<Id8> drumIds;              // drum-sample IDs: known objects, never treated as multisample IDs
    std::vector<Drum> drums;            // optional KMP drum records (L/R sample indices -> DrumSample -> Sample edges)
};

struct DecodedSound {
    std::string bank;                   // e.g. "USER03" or "USERDK01": scope of the Sound
    uint32_t number = 0;                // position inside the bank
    uint32_t kind = 0;                  // chunk id, e.g. 0x10050038
    std::string name;
    std::vector<uint8_t> body;          // decoded bytes
};

struct RecordLayout { uint32_t base = 0, record = 0, pairOffset = 0; };   // size = base + n * record; slots at +0 and +pairOffset
using LayoutTable = std::map<uint32_t, RecordLayout>;

// Verified 3-section variable-layer USERDK Drum Kit layout:
//   [Header (headerBytes)] + [128-Key Table (128 * keyStride)] + [Layer Array (L * layerStride)]
// Every kit is validated against exact header mode-flag bit 7, 128-key cumEnd running sum, key[127] bit-15
// hasId8 sentinel, 8-byte monotonic velocity top thresholds (<= 127), and exact byte-length closure.
struct DrumKitLayout {
    uint32_t headerBytes = 0;       // header bytes before the 128-key table
    uint32_t keyStride = 0;         // bytes per key record (18, 20, or 22)
    uint32_t layerCountOffset = 0;  // offset of u8 layerCount within key record (0 or 2)
    uint32_t layerStride = 0;       // bytes per layer record (20, 30, 32, or 33 when hasId8; 22 when !hasId8)
    uint32_t modeFlagOffset = 0;    // offset of mode flag byte in header (bit 7 == 1 for DrumKit)
    uint32_t velOffset = 0;         // offset of 8-byte velocity top-threshold array in key record (0 => keyStride - 10)
    bool hasId8 = true;             // true iff key[127].cumEnd bit 15 (0x8000) is set and layer ends with 8-byte ID8
};
using DrumKitLayoutTable = std::map<uint32_t, DrumKitLayout>;

// Four-state per-key occupancy classification for parsed USERDK Drum Kits (Track A Next Spec §2.1).
// Prevents silent fallback routing onto unassigned ROM(index==0) placeholder keys.
enum class DrumKeyOccupancy : uint8_t {
    EmptyOrUnknown,          // Unparsed, dangling-only, or structurally invalid key
    RomIndexZeroPlaceholder, // INFERRED: lc==1 && sourceBank==0 && tableIndex==0 (default unassigned placeholder)
    RomOrBank2Populated,     // INFERRED: >=1 valid layer with (sourceBank==0 && tableIndex!=0) or sourceBank==2
    RamPopulated             // VERIFIED: >=1 resolved RAM.KMP DrumSample layer (sourceBank==1)
};

inline const char* drumKeyOccupancyName(DrumKeyOccupancy o) {
    switch (o) {
        case DrumKeyOccupancy::EmptyOrUnknown:          return "EmptyOrUnknown";
        case DrumKeyOccupancy::RomIndexZeroPlaceholder: return "RomIndexZeroPlaceholder";
        case DrumKeyOccupancy::RomOrBank2Populated:     return "RomOrBank2Populated";
        case DrumKeyOccupancy::RamPopulated:            return "RamPopulated";
    }
    return "EmptyOrUnknown";
}

// Read-only per-layer summary for a parsed USERDK key (foundation for future Drum Mapping Engine).
// Verified structural fields and INFERRED advisory fields are strictly separated.
struct DrumKitLayerSummary {
    uint8_t layerIndex = 0;
    uint8_t sourceBank = 0;                   // VERIFIED: 0=ROM, 1=RAM; UNVERIFIED_HYPOTHESIS: 2=Bank2/Expansion
    uint16_t tableIndex = 0;                  // VERIFIED for RAM (matches RAM.KMP drum index); INFERRED for ROM
    uint8_t velLo = 1;                        // VERIFIED: lower velocity bound derived from monotonic velTop[]
    uint8_t velHi = 127;                      // VERIFIED: upper velocity bound velTop[layerIndex]
    bool isRam = false;                       // true iff sourceBank == 1
    bool resolved = false;                    // true iff resolved to a verified RAM.KMP DrumSample
    uint32_t drumSampleIndex = kNoZoneSample; // positional RAM.KMP DrumSample index when resolved
    Id8 drumSampleId{};                       // trailing 8-byte DrumSample ID (all-0 for ROM/Bank2)
    // Advisory INFERRED metadata ONLY (NEVER used to infer graph edges or equivalence):
    std::string advisoryDrumSampleName;       // INFERRED/advisory: RAM.KMP DrumSample::name when resolved ("" otherwise)
    uint8_t advisoryDrumFamilyByte = 0xFF;    // INFERRED: RAM.KMP DrumSample +113 (0=B..7=X, 0xFF=None)
    std::optional<uint16_t> advisoryLayerGainRaw; // INFERRED: u16 BE layer gain in 1005..1007 (0x8000 = 0 dB)
    std::optional<int8_t> advisoryTranspose;      // INFERRED: semitones
    std::optional<int8_t> advisoryFineTune;       // INFERRED: cents (-99..+99)
};

// Read-only per-key summary for MIDI note 0..127 of a parsed USERDK kit.
struct DrumKitKeySummary {
    uint8_t midiNote = 0;                     // VERIFIED: direct 0-indexed MIDI note 0..127 (C-1..G9)
    uint8_t layerCount = 0;                   // VERIFIED: 1..8
    DrumKeyOccupancy occupancy = DrumKeyOccupancy::EmptyOrUnknown;
    std::array<uint8_t, 8> velTop{};          // VERIFIED: 8-byte monotonic velocity top thresholds (<= 127)
    std::vector<DrumKitLayerSummary> layers;
    // Advisory INFERRED metadata ONLY (NEVER used to infer graph edges or equivalence):
    std::optional<uint16_t> advisoryKeyGainRaw;   // INFERRED: u16 BE key gain in 1005..1007
    uint8_t advisoryPan = 64;                 // INFERRED: 0..127 (64 = Center)
    uint8_t advisoryExclusiveGroup = 0;       // INFERRED: 0 = None, 1..127 (Choke Group)

    constexpr bool isPopulated() const noexcept {
        return occupancy == DrumKeyOccupancy::RamPopulated ||
               occupancy == DrumKeyOccupancy::RomOrBank2Populated;
    }

    // Count of distinct non-degenerate velocity zones when adjacent velTop[l] == velTop[l - 1].
    constexpr uint8_t effectiveDistinctLayers() const noexcept {
        if (layerCount == 0u) return 0u;
        const uint8_t n = layerCount <= 8u ? layerCount : 8u;
        uint8_t distinct = 1u;
        for (uint8_t l = 1u; l < n; ++l) {
            if (velTop[l] != velTop[l - 1u]) ++distinct;
        }
        return distinct;
    }

    // Highest velocity ceiling for this key (velTop[layerCount - 1], or 0 when unparsed/empty).
    constexpr uint8_t effectiveMaxVel() const noexcept {
        if (layerCount == 0u) return 0u;
        const uint8_t n = layerCount <= 8u ? layerCount : 8u;
        return velTop[n - 1u];
    }

    // True iff any adjacent velocity split repeats the same threshold (e.g. [127, 127]).
    constexpr bool hasDegenerateVelSplit() const noexcept {
        const uint8_t n = layerCount <= 8u ? layerCount : 8u;
        for (uint8_t l = 1u; l < n; ++l) {
            if (velTop[l] == velTop[l - 1u]) return true;
        }
        return false;
    }
};

// Read-only 128-key map for one parsed USERDK kit.
struct DrumKitKeyMap {
    ResourceId sound;
    uint32_t kindChunkId = 0;
    uint32_t totalLayers = 0;
    size_t ramDrumLayers = 0;
    size_t romDrumLayers = 0;                 // sourceBank == 0 && ID8 == 0
    size_t nonRamDrumLayers = 0;              // sourceBank == 2 && ID8 == 0 (UNVERIFIED_HYPOTHESIS Bank2)
    uint32_t nonRamBank2LayerCount = 0;       // count of Bank-2 (sourceBank == 0x02 && ID8 == 0) layers
    size_t danglingRamLayers = 0;
    size_t disambiguatedDuplicateLayers = 0;
    std::bitset<128> activeKeyMask{};         // bit k set iff keys[k].occupancy is RamPopulated or RomOrBank2Populated
    std::bitset<128> ramKeyMask{};            // bit k set iff keys[k].occupancy is RamPopulated
    std::array<DrumKitKeySummary, 128> keys{};

    bool hasBank2Layers() const noexcept {
        return nonRamBank2LayerCount > 0u || nonRamDrumLayers > 0u;
    }

    bool isRomOnly() const noexcept {
        return ramDrumLayers == 0u && ramKeyMask.none() && danglingRamLayers == 0u &&
               !hasBank2Layers() && (romDrumLayers > 0u || activeKeyMask.any());
    }

    bool hasKey(uint8_t midiNote) const noexcept {
        return midiNote < 128u && activeKeyMask.test(midiNote);
    }
};

// Read-only interface foundation for future Drum Mapping Engine (Track B Q3).
struct IDrumKitKeyMap {
    virtual ~IDrumKitKeyMap() = default;
    virtual const DrumKitKeyMap* findDrumKitKeyMap(const ResourceId& soundId) const = 0;
};
// Catalog-level alias avoiding cross-namespace confusion with per-kit exporters::korg::quality::IDrumKitKeyMap.
using IDrumKitCatalog = IDrumKitKeyMap;

enum class Gap { NoLayoutForKind, SizeDoesNotFit, NoKnownReference, TooManyRecords, NumberOutOfRange, DecodeFailed, NoId8InDrumLayers };
inline const char* gapName(Gap g) {
    switch (g) {
        case Gap::NoLayoutForKind:   return "NoLayoutForKind";
        case Gap::SizeDoesNotFit:    return "SizeDoesNotFit";
        case Gap::NoKnownReference:  return "NoKnownReference";
        case Gap::TooManyRecords:    return "TooManyRecords";
        case Gap::NumberOutOfRange:  return "NumberOutOfRange";
        case Gap::DecodeFailed:      return "DecodeFailed";
        case Gap::NoId8InDrumLayers: return "NoId8InDrumLayers";
    }
    return "Unknown";
}
struct Unanalysed { ResourceId sound; Gap why; };
struct UnresolvedSlot {
    ResourceId sound;
    uint32_t record = 0, slot = 0;
    bool wrongKind = false;           // ID belongs to another KMP object kind (drum/sample/multisample)
    bool inUnanalysedSound = false;   // true when the Sound had Gap::NoKnownReference (layout may not apply)
};

struct NativeRefReport {
    size_t sounds = 0, analysed = 0, emptySlots = 0;
    size_t drumKitsAnalysed = 0;
    size_t ramDrumLayers = 0;
    size_t romDrumLayers = 0;
    size_t nonRamDrumLayers = 0;
    size_t disambiguatedDuplicateDrumIds = 0;
    std::vector<Unanalysed> unanalysed;
    std::vector<UnresolvedSlot> unresolved;
    std::vector<std::string> drumDiagnostics;
    bool coverageComplete() const { return unanalysed.empty() && unresolved.empty(); }   // still no delete proof: see header note
};

namespace native_detail {
inline Id8 id8(const uint8_t* p) { Id8 r; std::copy(p, p + 8, r.begin()); return r; }
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((uint32_t(p[0]) << 8) | uint32_t(p[1])); }
inline bool allSame(const Id8& v, uint8_t b) { return std::all_of(v.begin(), v.end(), [b](uint8_t x) { return x == b; }); }
inline std::string hexId(const Id8& v) {
    static const char* d = "0123456789abcdef";
    std::string o;
    o.reserve(16);
    for (uint8_t b : v) { o += d[b >> 4]; o += d[b & 0x0Fu]; }
    return o;
}
inline std::string upperAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
inline bool isDrumKitBank(const std::string& bank) {
    return upperAscii(bank).rfind("USERDK", 0) == 0;
}
inline bool endsWithUpper(const std::string& s, const char* upperSuffix) {
    const size_t n = std::strlen(upperSuffix);
    if (s.size() <= n) return false;
    const std::string tail = upperAscii(s.substr(s.size() - n));
    return tail == upperSuffix;
}
inline std::string bankFromLabel(const std::string& label) {
    const size_t slash = label.find_last_of("/\\");
    std::string leaf = slash == std::string::npos ? label : label.substr(slash + 1);
    const size_t dot = leaf.rfind('.');
    if (dot != std::string::npos && dot > 0) leaf.resize(dot);
    return upperAscii(std::move(leaf));
}
inline bool validSampleInfo(const SampleInfo& s) {
    return s.channels >= 1 && s.channels <= 2 && (s.bits == 8 || s.bits == 16) &&
           s.rate > 0 && s.frames > 0 && s.frames <= Graph::kMaxFrames && s.loopFrames <= s.frames;
}
struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f = -1) : fd(f) {}
    ~ScopedFd() { if (fd >= 0) ::close(fd); }
    int release() { const int r = fd; fd = -1; return r; }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};
struct ScopedDir {
    DIR* dir = nullptr;
    explicit ScopedDir(DIR* d = nullptr) : dir(d) {}
    ~ScopedDir() { if (dir) ::closedir(dir); }
    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;
};
inline std::string stripTrailingSlashes(const std::filesystem::path& p) {
    std::string s = p.string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
inline bool listDirEntriesAt(int dirFd, std::vector<std::string>& names, size_t maxEntries, std::string& err) {
    names.clear();
    ScopedFd dupFd(::fcntl(dirFd, F_DUPFD_CLOEXEC, 0));
    if (dupFd.fd < 0) { err = "cannot duplicate directory descriptor"; return false; }
    ScopedDir dir(::fdopendir(dupFd.fd));
    if (!dir.dir) { err = "cannot open directory stream"; return false; }
    (void)dupFd.release();   // ownership transferred to DIR* managed by ScopedDir
    for (;;) {
        errno = 0;
        const struct dirent* ent = ::readdir(dir.dir);
        if (!ent) {
            if (errno != 0) { names.clear(); err = "directory read failed"; return false; }
            break;
        }
        const std::string n(ent->d_name);
        if (n == "." || n == "..") continue;
        if (names.size() >= maxEntries) {
            names.clear();
            err = "directory exceeds maximum entry limit (" + std::to_string(maxEntries) + ")";
            return false;
        }
        names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    return true;
}
// Reads one regular file below an open directory descriptor (O_NOFOLLOW + O_NONBLOCK + fstat S_ISREG).
inline bool readRegularAt(int dirFd, const char* name, size_t maxFileBytes, size_t remainingBudget,
                          std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    struct stat lst{};
    if (::fstatat(dirFd, name, &lst, AT_SYMLINK_NOFOLLOW) != 0) { err = "cannot stat entry"; return false; }
    if (!S_ISREG(lst.st_mode)) { err = "not a regular file (symlinks and special files are refused)"; return false; }
    const ScopedFd f(::openat(dirFd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (f.fd < 0) { err = "cannot open regular file (symlinks are not followed)"; return false; }
    struct stat st{};
    if (::fstat(f.fd, &st) != 0 || !S_ISREG(st.st_mode)) { err = "not a regular file"; return false; }
    if (st.st_size < 0) { err = "invalid file size"; return false; }
    const uint64_t fsz = static_cast<uint64_t>(st.st_size);
    if (fsz > maxFileBytes) { err = "file larger than the reader's limit"; return false; }
    if (fsz > remainingBudget) { err = "total read size exceeds the reader's limit"; return false; }
    out.resize(static_cast<size_t>(fsz));
    size_t got = 0;
    while (got < out.size()) {
        const ssize_t r = ::read(f.fd, out.data() + got, out.size() - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            out.clear();
            err = "read failed";
            return false;
        }
        if (r == 0) { out.clear(); err = "file shrank while being read"; return false; }
        got += static_cast<size_t>(r);
    }
    uint8_t extra = 0;
    for (;;) {
        const ssize_t r = ::read(f.fd, &extra, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            out.clear();
            err = "read failed";
            return false;
        }
        if (r > 0) { out.clear(); err = "file grew while being read"; return false; }
        break;
    }
    return true;
}
inline bool preadExact(int fd, uint64_t offset, uint8_t* dst, size_t len, size_t& remainingBudget, size_t& bytesRead, std::string& err) {
    if (len > remainingBudget) { err = "total read size exceeds the reader's limit"; return false; }
    size_t got = 0;
    while (got < len) {
        const ssize_t r = ::pread(fd, dst + got, len - got, static_cast<off_t>(offset + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            err = "read failed";
            return false;
        }
        if (r == 0) { err = "file shrank while being read"; return false; }
        got += static_cast<size_t>(r);
        remainingBudget -= static_cast<size_t>(r);
        bytesRead += static_cast<size_t>(r);
    }
    return true;
}
// Reads KORF container metadata (wrapper, header, directory, item chunk headers, and EOF index) via pread() without
// loading item audio payloads into memory. Verifies every structural rule checked by panative::parse().
inline bool readKorfMetadataAt(int dirFd, const char* name, size_t maxFileBytes, size_t& remainingBudget,
                               size_t& bytesRead, panative::Container& out, std::string& err) {
    out = panative::Container{};
    err.clear();
    auto fail = [&](std::string m) { out = panative::Container{}; err = std::move(m); return false; };

    struct stat lst{};
    if (::fstatat(dirFd, name, &lst, AT_SYMLINK_NOFOLLOW) != 0) return fail("cannot stat entry");
    if (!S_ISREG(lst.st_mode)) return fail("not a regular file (symlinks and special files are refused)");
    const ScopedFd f(::openat(dirFd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (f.fd < 0) return fail("cannot open regular file (symlinks are not followed)");
    struct stat st{};
    if (::fstat(f.fd, &st) != 0 || !S_ISREG(st.st_mode)) return fail("not a regular file");
    if (st.st_size < 0) return fail("invalid file size");
    const uint64_t fsz = static_cast<uint64_t>(st.st_size);
    if (fsz == 0) return fail("empty PCM container");
    if (fsz > maxFileBytes) return fail("file larger than the reader's limit");

    using panative::detail::be16;
    using panative::detail::be32;
    using panative::detail::cstr;
    using panative::detail::Chunk;

    if (fsz < 8) return fail("invalid PCM KORF container: file shorter than 8 bytes");
    if (fsz > 0xFFFFFFFFull) return fail("file larger than the KORF 32-bit offset limit");
    std::string rerr;
    uint8_t wrap[8]{};
    if (!preadExact(f.fd, 0, wrap, sizeof wrap, remainingBudget, bytesRead, rerr)) return fail(std::move(rerr));
    if (be32(wrap) != 0x01000114u) return fail("invalid PCM KORF container: not a Pa-native wrapper (bytes 0-3 are not 01 00 01 14)");
    out.wrapper = true;
    if (be32(wrap + 4) != fsz - 8) return fail("invalid PCM KORF container: wrapper length does not match the file size (truncated or padded)");
    if (fsz < 36) return fail("invalid PCM KORF container: fewer than 3 level-1 chunks (header, directory, index)");

    uint8_t hdrDir[28]{};
    if (!preadExact(f.fd, 8, hdrDir, sizeof hdrDir, remainingBudget, bytesRead, rerr)) return fail(std::move(rerr));
    const uint32_t hId = be32(hdrDir), hSz = be32(hdrDir + 4);
    if (hId != 0x02000018u || hSz != 12u || std::memcmp(hdrDir + 15, "KORF", 4) != 0)
        return fail("invalid PCM KORF container: header chunk 02000018 with KORF at 0x17 not found");

    const uint32_t dId = be32(hdrDir + 20), dSz = be32(hdrDir + 24);
    if ((dId >> 24) != 0x05u || (dId & 0xFFu) != 0x18u || ((dId >> 16) & 0xFFu) > 1u)
        return fail("invalid PCM KORF container: directory chunk 05 0x xx 18 not found");
    if (dSz > fsz - 36u) return fail("invalid PCM KORF container: directory chunk runs past the end of the file");
    if (dSz > remainingBudget) return fail("total read size exceeds the reader's limit");
    out.directoryId = dId;
    out.directoryFormat = int((dId >> 16) & 0xFFu);

    std::vector<uint8_t> dirBuf(dSz);
    if (dSz > 0 && !preadExact(f.fd, 36u, dirBuf.data(), dSz, remainingBudget, bytesRead, rerr)) return fail(std::move(rerr));

    // Each directory record consumes at least 24 B (format 0) or 25 B (format 1: 2B size + 2B count + fields 0/1/2 + 4B trailer),
    // bounding item chunks before walking so crafted 0-byte chunk floods short-circuit immediately.
    const size_t maxItems = static_cast<size_t>(dSz) / (out.directoryFormat == 0 ? 24u : 25u);
    const size_t maxL1Chunks = 3u + maxItems;

    std::vector<Chunk> l1;
    l1.push_back({hId, 8u, hSz});
    l1.push_back({dId, 28u, dSz});
    for (uint64_t off = uint64_t(36u) + static_cast<uint64_t>(dSz); off < fsz;) {
        if (l1.size() >= maxL1Chunks)
            return fail("invalid PCM KORF container: directory shorter than its item count");
        if (fsz - off < 8u) return fail("invalid PCM KORF container: trailing bytes shorter than a chunk header at " + std::to_string(off));
        uint8_t ch[8]{};
        if (!preadExact(f.fd, off, ch, sizeof ch, remainingBudget, bytesRead, rerr)) return fail(std::move(rerr));
        const uint32_t cid = be32(ch), csz = be32(ch + 4);
        if (csz > fsz - off - 8u) return fail("invalid PCM KORF container: chunk at " + std::to_string(off) + " runs past the end of the file");
        l1.push_back({cid, static_cast<uint32_t>(off), csz});
        off += 8u + static_cast<uint64_t>(csz);
    }
    if (l1.size() < 3) return fail("invalid PCM KORF container: fewer than 3 level-1 chunks (header, directory, index)");
    const Chunk& x = l1.back();
    if (x.id != 0xFE000018u) return fail("invalid PCM KORF container: index chunk FE000018 is not last");
    const size_t items = l1.size() - 3;
    if (uint64_t(x.size) != uint64_t(16u) + uint64_t(4u) * items)
        return fail("invalid PCM KORF container: index chunk does not match the chunk list");
    if (x.size > remainingBudget) return fail("total read size exceeds the reader's limit");

    std::vector<uint8_t> idxBuf(x.size);
    if (!preadExact(f.fd, static_cast<uint64_t>(x.offset) + 8u, idxBuf.data(), x.size, remainingBudget, bytesRead, rerr))
        return fail(std::move(rerr));
    const uint8_t* q = idxBuf.data();
    if (std::memcmp(q, "KBEG", 4) != 0 || std::memcmp(q + x.size - 8, "KEND", 4) != 0 || be32(q + x.size - 4) != items + 1)
        return fail("invalid PCM KORF container: index chunk does not match the chunk list");
    for (size_t i = 0; i <= items; ++i)
        if (be32(q + 4 + 4 * i) != l1[1 + i].offset)
            return fail("invalid PCM KORF container: index offset " + std::to_string(i) + " does not match the chunk list");

    const uint8_t* r = dirBuf.data();
    const size_t rn = dirBuf.size();
    size_t pos = 0;
    out.items.reserve(items);
    for (size_t i = 0; i < items; ++i) {
        const Chunk& ic = l1[2 + i];
        panative::Item it{ic.id, ic.offset, ic.size, 0, 0, 0, {}, {}};
        uint16_t sub = 0;
        if (out.directoryFormat == 0) {
            if (rn - pos < 24) return fail("invalid PCM KORF container: directory shorter than its item count");
            const uint8_t* e = r + pos;
            it.name = cstr(e, 18);
            it.kind = e[18]; it.bank = e[19]; it.slot = e[20];
            sub = be16(e + 21);
            pos += 24;
        } else {
            if (rn - pos < 4) return fail("invalid PCM KORF container: directory shorter than its item count");
            const size_t end = pos + 2 + be16(r + pos);
            if (be16(r + pos) < 6 || end > rn) return fail("invalid PCM KORF container: directory record " + std::to_string(i) + " runs past the directory");
            const unsigned nf = be16(r + pos + 2);
            size_t fpos = pos + 4;
            bool seen0 = false, seen1 = false, seen2 = false;
            for (unsigned k = 0; k < nf; ++k) {
                if (end - fpos < 4) return fail("invalid PCM KORF container: directory record " + std::to_string(i) + ": field header past the record");
                const uint16_t t = be16(r + fpos), len = be16(r + fpos + 2);
                if (len > end - fpos - 4) return fail("invalid PCM KORF container: directory record " + std::to_string(i) + ": field past the record");
                const uint8_t* v = r + fpos + 4;
                if (t == 2) { it.name = cstr(v, len); seen2 = true; }
                else if (t == 0 && len == 3) { it.kind = v[0]; it.bank = v[1]; it.slot = v[2]; seen0 = true; }
                else if (t == 1 && len == 2) { sub = be16(v); seen1 = true; }
                else if (t == 4 && len == 8) it.sampleId.assign(reinterpret_cast<const char*>(v), 8);
                fpos += 4 + size_t(len);
            }
            if (!seen0 || !seen1 || !seen2 || end - fpos != 4)
                return fail("invalid PCM KORF container: directory record " + std::to_string(i) + " lacks the observed fields");
            pos = end;
        }
        if (sub != ((ic.id >> 8) & 0xFFFFu))
            return fail("invalid PCM KORF container: directory record " + std::to_string(i) + " does not match its item chunk id");
        out.items.push_back(std::move(it));
    }
    if (pos != rn) return fail("invalid PCM KORF container: directory has bytes after its last record");

    struct stat stAfter{};
    if (::fstat(f.fd, &stAfter) != 0 || !S_ISREG(stAfter.st_mode) || stAfter.st_size < 0 || static_cast<uint64_t>(stAfter.st_size) != fsz)
        return fail("file size changed while being read");

    out.ok = true;
    return true;
}
}  // namespace native_detail

// Known verified variable-layer USERDK layouts across 10 chunk kinds (07000238, 07000338, 10000038..10070038).
// Legacy single-layer 07000018/07000038 (fixed 4,322 B) and unknown/16-layer formats intentionally return nullopt.
// Never used blindly: every candidate MUST pass validateDrumKitBody()'s 128-key running-sum, 0x8000 sentinel,
// 8-byte monotonic velocity, mode-flag bit-7, and exact byte-length closure checks.
inline std::optional<DrumKitLayout> knownDrumKitLayout(uint32_t kind) {
    switch (kind) {
        case 0x07000238u: return DrumKitLayout{250, 18, 0, 20, 16, 0, true};
        case 0x07000338u: return DrumKitLayout{259, 18, 0, 20, 16, 0, true};
        case 0x10000038u: return DrumKitLayout{362, 18, 0, 20, 18, 0, true};
        case 0x10010038u: return DrumKitLayout{367, 18, 0, 20, 18, 0, true};
        case 0x10020038u: return DrumKitLayout{518, 18, 0, 30, 18, 0, true};
        case 0x10030038u: return DrumKitLayout{502, 18, 0, 30,  2, 0, true};
        case 0x10040038u: return DrumKitLayout{510, 18, 0, 30,  2, 0, true};
        case 0x10050038u: return DrumKitLayout{514, 20, 2, 32,  2, 0, true};
        case 0x10060038u: return DrumKitLayout{516, 20, 2, 32,  2, 0, true};
        case 0x10070038u: return DrumKitLayout{653, 22, 2, 33,  2, 0, true};
        default: return std::nullopt;
    }
}

// Fail-closed structural validator for one decoded USERDK body against `L`.
// All arithmetic is done in uint64_t / size_t before any buffer access.
// Returns true iff every structural invariant holds AND key[127].cumEnd carries the 0x8000 hasId8 sentinel.
// On failure, sets `failGapOut`:
//   - Gap::NoId8InDrumLayers when all 128-key invariants hold with bit-15 == 0 and exact no-ID8 layer stride (e.g. OC32 10030038)
//   - Gap::NoLayoutForKind   when `L` itself has invalid geometry
//   - Gap::SizeDoesNotFit    on any size, mode-flag, lc, cumEnd, sentinel, or velocity invariant violation.
inline bool validateDrumKitBody(const std::vector<uint8_t>& body, const DrumKitLayout& L,
                                uint32_t& totalLayersOut, bool& hasId8Out, Gap& failGapOut) {
    totalLayersOut = 0;
    hasId8Out = false;
    failGapOut = Gap::SizeDoesNotFit;
    if (L.keyStride < 12u || L.keyStride > 64u ||
        L.headerBytes == 0u || L.modeFlagOffset >= L.headerBytes) {
        failGapOut = Gap::NoLayoutForKind;
        return false;
    }
    const uint32_t velOff = L.velOffset != 0u ? L.velOffset : (L.keyStride - 10u);
    if (velOff > L.keyStride - 10u || L.layerCountOffset >= velOff) {
        failGapOut = Gap::NoLayoutForKind;
        return false;
    }
    const uint32_t minLayerStride = L.hasId8 ? 12u : 4u;
    if (L.layerStride < minLayerStride || L.layerStride > 256u) {
        failGapOut = Gap::NoLayoutForKind;
        return false;
    }
    const uint64_t prefix = uint64_t(L.headerBytes) + 128ull * uint64_t(L.keyStride);
    if (body.size() < prefix) {
        failGapOut = Gap::SizeDoesNotFit;
        return false;
    }
    if ((body[L.modeFlagOffset] & 0x80u) == 0u) {
        failGapOut = Gap::SizeDoesNotFit;
        return false;
    }
    uint32_t cum = 0;
    bool flag8000 = false;
    for (uint32_t k = 0; k < 128u; ++k) {
        const uint8_t* kr = body.data() + size_t(L.headerBytes) + size_t(k) * size_t(L.keyStride);
        const uint8_t lc = kr[L.layerCountOffset];
        if (lc == 0u || lc > 8u) {
            failGapOut = Gap::SizeDoesNotFit;
            return false;
        }
        cum += lc;
        const uint16_t rawEnd = native_detail::be16(kr + L.keyStride - 2u);
        if (k < 127u) {
            if (rawEnd != cum) {
                failGapOut = Gap::SizeDoesNotFit;
                return false;
            }
        } else {
            flag8000 = (rawEnd & 0x8000u) != 0u;
            if ((rawEnd & 0x7FFFu) != cum) {
                failGapOut = Gap::SizeDoesNotFit;
                return false;
            }
        }
        for (uint32_t j = 0; j < 8u; ++j) {
            if (kr[velOff + j] > 127u) {
                failGapOut = Gap::SizeDoesNotFit;
                return false;
            }
        }
        for (uint32_t j = 1; j < lc; ++j) {
            if (kr[velOff + j] < kr[velOff + j - 1u]) {
                failGapOut = Gap::SizeDoesNotFit;
                return false;
            }
        }
    }
    const uint64_t layerBytes = uint64_t(body.size()) - prefix;
    if (flag8000) {
        if (!L.hasId8) {
            failGapOut = Gap::SizeDoesNotFit;
            return false;
        }
        if (uint64_t(cum) * uint64_t(L.layerStride) != layerBytes) {
            failGapOut = Gap::SizeDoesNotFit;
            return false;
        }
        totalLayersOut = cum;
        hasId8Out = true;
        return true;
    }
    // Bit 15 == 0 on key[127].cumEnd -> check if exact no-ID8 variable-layer kit (e.g. OC32 10030038).
    const uint64_t noIdStride = L.hasId8 ? (L.layerStride > 8u ? uint64_t(L.layerStride - 8u) : 0ull)
                                         : uint64_t(L.layerStride);
    if (noIdStride >= 4ull && uint64_t(cum) * noIdStride == layerBytes) {
        totalLayersOut = cum;
        hasId8Out = false;
        failGapOut = Gap::NoId8InDrumLayers;
        return false;
    }
    failGapOut = Gap::SizeDoesNotFit;
    return false;
}

// Strict separation of Drum Kit Sounds vs Melodic Sounds before melodic layout fitting or oscillator parsing.
// Strictly bank-aligned (USERDK*) to match native_kst_closure.h, m1_manifest_adapter.h, and slot_conflicts.h;
// within USERDK* banks, validateDrumKitBody() enforces the header mode-flag bit-7 guard fail-closed.
inline bool isDrumKitSound(const DecodedSound& s) {
    return native_detail::isDrumKitBank(s.bank);
}

// Data-verified / invariant-checked USERDK Drum Kit layout fitter.
// Never guesses: every candidate layout must satisfy all 128-key cumEnd, 0x8000 sentinel, 8-byte monotonic velocity,
// header mode-flag bit-7, exact byte-length closure, and (when RAM DrumSample ID8s are present) exact trailing ID8 alignment.
inline std::optional<DrumKitLayout> fitDrumKitLayout(const std::vector<const DecodedSound*>& sameKind, const KmpIndex& k) {
    const DecodedSound* firstNonNull = nullptr;
    for (const DecodedSound* s : sameKind) {
        if (s != nullptr) { firstNonNull = s; break; }
    }
    if (!firstNonNull) return std::nullopt;
    std::set<Id8> drumIds;
    for (const auto& d : k.drums) drumIds.insert(d.id);
    drumIds.insert(k.drumIds.begin(), k.drumIds.end());

    auto verifyAlignmentOnHits = [&](const DecodedSound& s, const DrumKitLayout& L) -> bool {
        if (drumIds.empty()) return true;
        const size_t prefix = size_t(L.headerBytes) + 128u * size_t(L.keyStride);
        if (s.body.size() < prefix + L.layerStride) return false;
        // Verify that any RAM DrumSample ID8 appearing in the layer array lands at the trailing 8 bytes
        // of a layer record (layerStride - 8). Do not reject sourceBank != 1 here so buildNativeGraph()
        // can surface sourceBank/ID8 contradictions (e.g. ROM bank carrying non-zero ID8) as UnresolvedSlot.
        size_t last = prefix;
        bool any = false;
        for (size_t off = prefix; off + 8u <= s.body.size(); ++off) {
            const Id8 v = native_detail::id8(&s.body[off]);
            if (drumIds.count(v) > 0 && (!any || off >= last + 8u)) {
                any = true;
                last = off;
                if ((off - prefix) % size_t(L.layerStride) != size_t(L.layerStride) - 8u) return false;
            }
        }
        return true;
    };

    // 1. Check verified structural layout candidate for known variable-layer chunk kinds.
    const uint32_t kind = firstNonNull->kind;
    if (const auto known = knownDrumKitLayout(kind); known.has_value()) {
        size_t validWithId8 = 0;
        size_t alignedValid = 0;
        for (const DecodedSound* s : sameKind) {
            if (!s) continue;
            uint32_t totalL = 0;
            bool hasId8 = false;
            Gap fg = Gap::SizeDoesNotFit;
            if (validateDrumKitBody(s->body, *known, totalL, hasId8, fg)) {
                ++validWithId8;
                if (verifyAlignmentOnHits(*s, *known)) ++alignedValid;
            }
        }
        if (validWithId8 > 0 && alignedValid == validWithId8) {
            return *known;
        }
        // If no valid hasId8 kit matched the known layout, fail closed (do not fit corrupt items).
        return std::nullopt;
    }

    // 2. Pure data-driven discovery for unknown/custom variable-layer drum-kit chunk kinds (requires RAM DrumSample hits).
    if (drumIds.empty()) return std::nullopt;
    struct HitItem { const DecodedSound* s; std::vector<size_t> offs; };
    std::vector<HitItem> hitItems;
    for (const DecodedSound* s : sameKind) {
        if (!s || s->body.size() < 256u) continue;
        std::vector<size_t> offs;
        size_t last = 0;
        bool any = false;
        for (size_t i = 0; i + 8 <= s->body.size(); ++i) {
            const Id8 v = native_detail::id8(&s->body[i]);
            if (drumIds.count(v) > 0 && (!any || i >= last + 8)) {
                any = true;
                last = i;
                offs.push_back(i);
            }
        }
        if (offs.size() >= 2) hitItems.push_back({s, std::move(offs)});
    }
    if (hitItems.empty()) return std::nullopt;

    size_t gStride = 0;
    for (const auto& hi : hitItems) {
        for (size_t i = 1; i < hi.offs.size(); ++i) {
            const size_t diff = hi.offs[i] - hi.offs[i - 1];
            gStride = gStride == 0 ? diff : std::gcd(gStride, diff);
        }
    }
    if (gStride < 12u || gStride > 256u) return std::nullopt;

    // Candidate layer strides: gStride and its divisors >= 12 that satisfy (body.size() - hitOff) % lst == 8.
    std::vector<uint32_t> candLst;
    for (size_t lst = 12u; lst <= gStride; ++lst) {
        if (gStride % lst != 0) continue;
        bool okAll = true;
        for (const auto& hi : hitItems) {
            for (size_t off : hi.offs) {
                if ((hi.s->body.size() - off) % lst != 8u) { okAll = false; break; }
            }
            if (!okAll) break;
        }
        if (okAll) candLst.push_back(static_cast<uint32_t>(lst));
    }
    // Try largest valid stride first (smallest divisor could falsely subdivide records).
    std::reverse(candLst.begin(), candLst.end());

    const uint32_t kstCands[] = {18u, 20u, 22u};
    const uint32_t mfoCands[] = {2u, 16u, 18u};
    const auto& ref = hitItems.front();
    for (uint32_t lst : candLst) {
        // prefix must be <= ref.offs.front() - (lst - 8) and satisfy (ref.s->body.size() - prefix) % lst == 0.
        const size_t maxPrefix = ref.offs.front() >= (lst - 8u) ? (ref.offs.front() - (lst - 8u)) : 0u;
        for (size_t prefix = maxPrefix; prefix >= 128u * 18u + 4u; ) {
            const size_t layerBytes = ref.s->body.size() - prefix;
            if (layerBytes % lst == 0) {
                const size_t L = layerBytes / lst;
                if (L >= 128u && L <= 1024u) {
                    const uint16_t end127 = native_detail::be16(ref.s->body.data() + prefix - 2u);
                    if (end127 == (static_cast<uint16_t>(L) | 0x8000u)) {
                        for (uint32_t kst : kstCands) {
                            if (prefix <= 128u * kst) continue;
                            const uint32_t hdr = static_cast<uint32_t>(prefix - 128u * kst);
                            const uint32_t lcOff = (kst == 18u) ? 0u : 2u;
                            for (uint32_t mfo : mfoCands) {
                                if (mfo >= hdr) continue;
                                DrumKitLayout cand{hdr, kst, lcOff, lst, mfo, 0u, true};
                                bool allOk = true;
                                for (const auto& hi : hitItems) {
                                    uint32_t tl = 0; bool h8 = false; Gap fg = Gap::SizeDoesNotFit;
                                    if (!validateDrumKitBody(hi.s->body, cand, tl, h8, fg) || !verifyAlignmentOnHits(*hi.s, cand)) {
                                        allOk = false;
                                        break;
                                    }
                                }
                                if (allOk) return cand;
                            }
                        }
                    }
                }
            }
            if (prefix < lst) break;
            prefix -= lst;
        }
    }
    return std::nullopt;
}

// Derive a melodic layout for one Sound kind from the data (the research method): items holding only multisample IDs;
// excludes USERDK Drum Kits BEFORE fitting so coincidental body sizes can never pollute melodic layouts.
inline std::optional<RecordLayout> fitLayout(const std::vector<const DecodedSound*>& sameKind, const KmpIndex& k, size_t minItems = 8) {
    std::set<Id8> multi, other;
    for (const auto& m : k.multisamples) multi.insert(m.id);
    for (const auto& s : k.samples) other.insert(s.id);
    other.insert(k.drumIds.begin(), k.drumIds.end());
    for (const auto& d : k.drums) other.insert(d.id);
    struct Item { size_t size, first; std::vector<size_t> offs; };
    std::vector<Item> items;
    for (const DecodedSound* s : sameKind) {
        if (!s || isDrumKitSound(*s)) continue;
        std::vector<size_t> o; bool onlyMulti = true; size_t last = 0; bool any = false;
        for (size_t i = 0; i + 8 <= s->body.size(); ++i) {
            const Id8 v = native_detail::id8(&s->body[i]);
            const bool m = multi.count(v) > 0, x = !m && other.count(v) > 0;
            if ((m || x) && (!any || i >= last + 8)) { any = true; last = i; if (x) onlyMulti = false; else o.push_back(i); }
        }
        if (!any || !onlyMulti || o.empty()) continue;
        items.push_back({s->body.size(), o.front(), std::move(o)});
    }
    if (items.empty() || items.size() < minItems) return std::nullopt;
    std::map<size_t, size_t> cnt; for (const Item& i : items) ++cnt[i.first];
    const size_t base = std::max_element(cnt.begin(), cnt.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
    std::vector<size_t> at; for (const Item& i : items) if (i.first == base && i.size >= base) at.push_back(i.size - base);
    if (at.empty()) return std::nullopt;
    const size_t mn = *std::min_element(at.begin(), at.end());
    size_t rec = 0;
    for (size_t r = 8; r <= mn; ++r) if (std::all_of(at.begin(), at.end(), [r](size_t a) { return a % r == 0; })) rec = r;
    if (!rec || base > 0xFFFFFFFFu) return std::nullopt;
    std::map<size_t, size_t> po;   // only items that define the base vote, so the pair offset belongs to the same layout
    for (const Item& i : items) if (i.first == base && i.size >= base) for (size_t o : i.offs) if ((o - base) % rec) ++po[(o - base) % rec];
    if (po.empty()) return std::nullopt;
    // Fail closed on ambiguity: (1) a tie for the pair offset; (2) no item with exactly one record (then the true record could be a
    // divisor of the fitted size); (3) any ID of a base item outside slot 0 / the pair slot (the fit does not describe the data).
    size_t best = 0, ties = 0, pair = 0;
    for (const auto& [off, c] : po) { if (c > best) { best = c; pair = off; ties = 1; } else if (c == best) ++ties; }
    if (ties != 1 || mn != rec) return std::nullopt;
    for (const Item& i : items) if (i.first == base && i.size >= base) for (size_t o : i.offs) { const size_t m = (o - base) % rec; if (m != 0 && m != pair) return std::nullopt; }
    return RecordLayout{static_cast<uint32_t>(base), static_cast<uint32_t>(rec), static_cast<uint32_t>(pair)};
}

inline ResourceId soundId(const DecodedSound& s) { return {false, s.bank, Kind::Sound, s.number}; }

// Builds a NEW graph with Sound, Oscillator, Multisample, DrumSample and Sample nodes plus edges; all-or-nothing on graph errors.
// It never adds to a caller's graph, so scan/completeness declarations the caller made elsewhere cannot leak onto these
// resources. Combining with other evidence is Graph::merge()'s job and that caller's responsibility.
// Identity of KMP objects is positional inside this KST (scope "KST"), never the 8-byte ID or the name.
inline std::optional<Graph> buildNativeGraph(const KmpIndex& k, const std::vector<DecodedSound>& sounds,
                                             const LayoutTable& layouts, const DrumKitLayoutTable& drumLayouts,
                                             NativeRefReport& rep, std::string* err,
                                             std::map<ResourceId, DrumKitKeyMap>* drumKeyMapsOut = nullptr) {
    Graph t; NativeRefReport r;
    std::map<ResourceId, DrumKitKeyMap> localKeyMaps;
    const std::string kScope = "KST";
    constexpr size_t kMaxObjects = 999999999;   // ResourceId numbers are at most 9 digits
    if (k.samples.size() > kMaxObjects || k.multisamples.size() > kMaxObjects || k.drums.size() > kMaxObjects) {
        if (err) *err = "KMP index too large";
        return std::nullopt;
    }
    std::map<Id8, size_t> msIndex;
    std::map<Id8, std::vector<uint32_t>> drumIdToIndices;
    std::set<Id8> drum, smp;
    for (size_t i = 0; i < k.multisamples.size(); ++i) if (!msIndex.emplace(k.multisamples[i].id, i).second) { if (err) *err = "duplicate multisample ID in KMP index"; return std::nullopt; }
    for (size_t i = 0; i < k.samples.size(); ++i) smp.insert(k.samples[i].id);
    for (size_t i = 0; i < k.drums.size(); ++i) {
        drum.insert(k.drums[i].id);
        drumIdToIndices[k.drums[i].id].push_back(static_cast<uint32_t>(i));
    }
    for (const Id8& d : k.drumIds) drum.insert(d);
    auto sid = [&](size_t i) { return ResourceId{false, kScope, Kind::Sample, static_cast<uint32_t>(i)}; };
    for (size_t i = 0; i < k.samples.size(); ++i)
        if (k.samples[i].info && !t.addNode({sid(i), k.samples[i].name, k.samples[i].info}, err)) return std::nullopt;
    for (size_t i = 0; i < k.multisamples.size(); ++i) {
        const ResourceId m{false, kScope, Kind::Multisample, static_cast<uint32_t>(i)};
        if (!t.addNode({m, k.multisamples[i].name, std::nullopt}, err)) return std::nullopt;
        for (size_t z = 0; z < k.multisamples[i].zoneSamples.size(); ++z) {
            const uint32_t s = k.multisamples[i].zoneSamples[z];
            if (s == kNoZoneSample) continue;
            if (!t.addRef({m, sid(s), Via::Zone, "zone " + std::to_string(z), Source::Package, Confidence::Confirmed, "kmp"}, err)) return std::nullopt;   // s beyond the table: dangling by design
        }
    }
    for (size_t i = 0; i < k.drums.size(); ++i) {
        const ResourceId dm{false, kScope, Kind::DrumSample, static_cast<uint32_t>(i)};
        if (!t.addNode({dm, k.drums[i].name, std::nullopt}, err)) return std::nullopt;
        if (k.drums[i].leftSample != kNoZoneSample &&
            !t.addRef({dm, sid(k.drums[i].leftSample), Via::DrumSampleOf, "L", Source::Package, Confidence::Confirmed, "kmp"}, err))
            return std::nullopt;
        if (k.drums[i].rightSample != kNoZoneSample &&
            !t.addRef({dm, sid(k.drums[i].rightSample), Via::DrumSampleOf, "R", Source::Package, Confidence::Confirmed, "kmp"}, err))
            return std::nullopt;
    }
    for (const DecodedSound& s : sounds) {
        ++r.sounds;
        const ResourceId sid_ = soundId(s);
        if (!t.addNode({sid_, s.name, std::nullopt}, err)) return std::nullopt;
        if (s.number > 999999) { r.unanalysed.push_back({sid_, Gap::NumberOutOfRange}); continue; }   // oscillator number = number * 1000 + record must stay below 10^9

        // Route USERDK Drum Kits strictly to the Drum Kit parser (NEVER melodic oscillator fitting/parsing).
        if (isDrumKitSound(s)) {
            const auto dli = drumLayouts.find(s.kind);
            if (dli == drumLayouts.end()) {
                // Fail closed: never parse edges without an explicit/fitted layout, but distinguish OC32 no-ID8
                // and corrupt/truncated bodies of known variable-layer kinds from unknown/legacy kinds.
                Gap gap = Gap::NoLayoutForKind;
                if (const auto known = knownDrumKitLayout(s.kind); known.has_value()) {
                    uint32_t tl = 0; bool h8 = false; Gap fg = Gap::SizeDoesNotFit;
                    if (!validateDrumKitBody(s.body, *known, tl, h8, fg)) {
                        gap = fg;   // Gap::NoId8InDrumLayers or Gap::SizeDoesNotFit
                    }
                }
                r.unanalysed.push_back({sid_, gap});
                continue;
            }
            const DrumKitLayout& DL = dli->second;
            uint32_t totalLayers = 0;
            bool hasId8 = false;
            Gap failGap = Gap::SizeDoesNotFit;
            if (!validateDrumKitBody(s.body, DL, totalLayers, hasId8, failGap)) {
                r.unanalysed.push_back({sid_, failGap});
                continue;
            }

            const size_t prefix = size_t(DL.headerBytes) + 128u * size_t(DL.keyStride);
            const uint32_t velOff = DL.velOffset != 0u ? DL.velOffset : (DL.keyStride - 10u);

            struct DrumHit { uint32_t key; uint32_t layer; uint8_t velLo; uint8_t velHi; uint32_t drumIdx; };
            std::vector<DrumHit> hits;
            std::vector<UnresolvedSlot> bad;
            std::vector<std::string> localDiags;
            size_t romCount = 0, nonRamCount = 0, disambigCount = 0;

            DrumKitKeyMap km;
            km.sound = sid_;
            km.kindChunkId = s.kind;
            km.totalLayers = totalLayers;

            size_t layerCursor = 0;
            for (uint32_t keyIdx = 0; keyIdx < 128u; ++keyIdx) {
                const uint8_t* kr = s.body.data() + size_t(DL.headerBytes) + size_t(keyIdx) * size_t(DL.keyStride);
                const uint8_t lc = kr[DL.layerCountOffset];
                DrumKitKeySummary& ks = km.keys[keyIdx];
                ks.midiNote = static_cast<uint8_t>(keyIdx);
                ks.layerCount = lc;
                for (uint32_t j = 0; j < 8u; ++j) ks.velTop[j] = kr[velOff + j];
                if (DL.keyStride >= 20u) ks.advisoryKeyGainRaw = native_detail::be16(kr);
                ks.advisoryPan = (DL.layerCountOffset + 1u < velOff) ? kr[DL.layerCountOffset + 1u] : 64u;
                if (DL.keyStride == 18u && DL.layerStride == 20u) {
                    ks.advisoryExclusiveGroup = kr[5];
                } else if (DL.layerCountOffset + 6u < velOff) {
                    ks.advisoryExclusiveGroup = kr[DL.layerCountOffset + 6u];
                } else {
                    ks.advisoryExclusiveGroup = 0u;
                }
                ks.layers.reserve(lc);

                uint8_t prevVel = 0;
                bool keyHasResolvedRam = false;
                bool keyHasPopulatedRomOrBank2 = false;
                for (uint32_t l = 0; l < lc; ++l) {
                    const uint8_t* lr = s.body.data() + prefix + layerCursor * size_t(DL.layerStride);
                    ++layerCursor;
                    const uint8_t srcBank = lr[0];
                    const uint16_t tblIdx = native_detail::be16(lr + 2);
                    const Id8 lid = native_detail::id8(lr + DL.layerStride - 8u);
                    const bool idIsZero = native_detail::allSame(lid, 0u);
                    const uint8_t velHi = ks.velTop[l];
                    const uint8_t rawVelLo = (l == 0u)
                                                 ? 1u
                                                 : static_cast<uint8_t>(std::min<uint32_t>(127u, uint32_t(prevVel) + 1u));
                    const uint8_t velLo = std::min(rawVelLo, velHi);
                    prevVel = velHi;

                    DrumKitLayerSummary ls;
                    ls.layerIndex = static_cast<uint8_t>(l);
                    ls.sourceBank = srcBank;
                    ls.tableIndex = tblIdx;
                    ls.velLo = velLo;
                    ls.velHi = velHi;
                    ls.isRam = (srcBank == 0x01u);
                    ls.drumSampleId = lid;
                    if (DL.layerStride >= 32u) {
                        ls.advisoryLayerGainRaw = native_detail::be16(lr + 4);
                        ls.advisoryTranspose = static_cast<int8_t>(lr[6]);
                        ls.advisoryFineTune = static_cast<int8_t>(lr[7]);
                    } else if (DL.layerStride >= 20u) {
                        ls.advisoryTranspose = static_cast<int8_t>(lr[5]);
                        ls.advisoryFineTune = static_cast<int8_t>(lr[6]);
                    }

                    if (srcBank == 0x01u) {
                        const auto dit = drumIdToIndices.find(lid);
                        if (dit != drumIdToIndices.end()) {
                            const auto& idxs = dit->second;
                            if (tblIdx < k.drums.size() && k.drums[tblIdx].id == lid) {
                                ls.resolved = true;
                                ls.drumSampleIndex = tblIdx;
                                ls.advisoryDrumSampleName = k.drums[tblIdx].name;
                                ls.advisoryDrumFamilyByte = k.drums[tblIdx].drumFamilyByte;
                                keyHasResolvedRam = true;
                                hits.push_back({keyIdx, l, velLo, velHi, tblIdx});
                                if (idxs.size() > 1) {
                                    ++disambigCount;
                                    ++km.disambiguatedDuplicateLayers;
                                    localDiags.push_back(
                                        "disambiguated duplicate drum ID via u16 index: " + sid_.str() +
                                        " key " + std::to_string(keyIdx) + " layer " + std::to_string(l) +
                                        " -> DrumSample/" + std::to_string(tblIdx) +
                                        " (id " + native_detail::hexId(lid) + ")");
                                }
                            } else {
                                // Fail closed: duplicate or out-of-sync u16 index MUST NEVER guess a DrumSample node.
                                bad.push_back({sid_, keyIdx, l, false, false});
                                ++km.danglingRamLayers;
                            }
                        } else {
                            const bool wk = (msIndex.count(lid) > 0 || smp.count(lid) > 0);
                            bad.push_back({sid_, keyIdx, l, wk, false});
                            ++km.danglingRamLayers;
                        }
                    } else if (srcBank == 0x00u || srcBank == 0x02u) {
                        if (idIsZero) {
                            if (srcBank == 0x00u) {
                                ++romCount;
                                ++km.romDrumLayers;
                                if (tblIdx != 0u) keyHasPopulatedRomOrBank2 = true;
                            } else {
                                ++nonRamCount;
                                ++km.nonRamDrumLayers;
                                ++km.nonRamBank2LayerCount;
                                keyHasPopulatedRomOrBank2 = true;
                            }
                        } else {
                            // Contradicts ROM/non-RAM zero-ID invariant -> fail-closed UnresolvedSlot.
                            const bool wk = (msIndex.count(lid) > 0 || smp.count(lid) > 0);
                            bad.push_back({sid_, keyIdx, l, wk, false});
                        }
                    } else {
                        // Unknown source bank byte -> fail-closed UnresolvedSlot.
                        const bool wk = (msIndex.count(lid) > 0 || smp.count(lid) > 0);
                        bad.push_back({sid_, keyIdx, l, wk, false});
                    }
                    ks.layers.push_back(std::move(ls));
                }

                if (keyHasResolvedRam) {
                    ks.occupancy = DrumKeyOccupancy::RamPopulated;
                    km.activeKeyMask.set(keyIdx);
                    km.ramKeyMask.set(keyIdx);
                } else if (keyHasPopulatedRomOrBank2) {
                    ks.occupancy = DrumKeyOccupancy::RomOrBank2Populated;
                    km.activeKeyMask.set(keyIdx);
                } else if (lc == 1u && ks.layers.size() == 1u &&
                           ks.layers[0].sourceBank == 0x00u &&
                           ks.layers[0].tableIndex == 0u &&
                           native_detail::allSame(ks.layers[0].drumSampleId, 0u)) {
                    ks.occupancy = DrumKeyOccupancy::RomIndexZeroPlaceholder;
                } else {
                    ks.occupancy = DrumKeyOccupancy::EmptyOrUnknown;
                }
            }

            km.ramDrumLayers = hits.size();
            if (drumKeyMapsOut) localKeyMaps.emplace(sid_, std::move(km));

            if (hits.empty()) {
                for (UnresolvedSlot& u : bad) u.inUnanalysedSound = true;
                r.unresolved.insert(r.unresolved.end(), bad.begin(), bad.end());
                r.romDrumLayers += romCount;
                r.nonRamDrumLayers += nonRamCount;
                r.unanalysed.push_back({sid_, Gap::NoKnownReference});
                continue;
            }
            r.unresolved.insert(r.unresolved.end(), bad.begin(), bad.end());
            ++r.analysed;
            ++r.drumKitsAnalysed;
            r.ramDrumLayers += hits.size();
            r.romDrumLayers += romCount;
            r.nonRamDrumLayers += nonRamCount;
            r.disambiguatedDuplicateDrumIds += disambigCount;
            r.drumDiagnostics.insert(r.drumDiagnostics.end(), localDiags.begin(), localDiags.end());

            for (const DrumHit& h : hits) {
                const ResourceId dsId{false, kScope, Kind::DrumSample, h.drumIdx};
                const std::string detail = "key " + std::to_string(h.key) + " layer " + std::to_string(h.layer) +
                                           " vel " + std::to_string(h.velLo) + "-" + std::to_string(h.velHi);
                if (!t.addRef({sid_, dsId, Via::DrumKeyLayer, detail, Source::Package, Confidence::Inferred, "native-kst"}, err))
                    return std::nullopt;
            }
            continue;
        }

        const auto li = layouts.find(s.kind);
        if (li == layouts.end() || li->second.record < 8 || li->second.pairOffset > li->second.record - 8) { r.unanalysed.push_back({sid_, Gap::NoLayoutForKind}); continue; }
        const RecordLayout& L = li->second;
        if (s.body.size() < L.base || (s.body.size() - L.base) % L.record) { r.unanalysed.push_back({sid_, Gap::SizeDoesNotFit}); continue; }
        const size_t n = (s.body.size() - L.base) / L.record;   // size_t throughout: q * record cannot wrap
        if (n > 999) { r.unanalysed.push_back({sid_, Gap::TooManyRecords}); continue; }
        std::vector<std::pair<size_t, std::pair<uint32_t, size_t>>> hits;   // record, slot, multisample index
        std::vector<UnresolvedSlot> bad; size_t empty = 0;
        for (size_t q = 0; q < n; ++q)
            for (uint32_t slot = 0; slot < 2; ++slot) {
                const Id8 v = native_detail::id8(&s.body[L.base + q * L.record + (slot ? L.pairOffset : 0)]);
                if (const auto m = msIndex.find(v); m != msIndex.end()) hits.push_back({q, {slot, m->second}});
                else if (native_detail::allSame(v, 0) || native_detail::allSame(v, 0xFF)) ++empty;
                else bad.push_back({sid_, static_cast<uint32_t>(q), slot, drum.count(v) > 0 || smp.count(v) > 0});
            }
        if (hits.empty()) {
            for (UnresolvedSlot& u : bad) u.inUnanalysedSound = true;
            r.unresolved.insert(r.unresolved.end(), bad.begin(), bad.end());   // kept even when the Sound turns out unanalysed
            r.unanalysed.push_back({sid_, Gap::NoKnownReference});
            continue;
        }
        r.unresolved.insert(r.unresolved.end(), bad.begin(), bad.end());
        ++r.analysed; r.emptySlots += empty;
        std::set<size_t> made;
        for (const auto& [q, sm] : hits) {
            const ResourceId osc{false, s.bank, Kind::Oscillator, s.number * 1000 + static_cast<uint32_t>(q)};
            if (made.insert(q).second) {
                if (!t.addNode({osc, "record " + std::to_string(q), std::nullopt}, err)) return std::nullopt;
                if (!t.addRef({sid_, osc, Via::OscillatorOf, "record " + std::to_string(q) + " (role as oscillator: HYPOTHESIS)", Source::Package, Confidence::Inferred, "native-kst"}, err)) return std::nullopt;
            }
            const ResourceId ms{false, kScope, Kind::Multisample, static_cast<uint32_t>(sm.second)};
            if (!t.addRef({osc, ms, sm.first ? Via::OscLow : Via::OscHigh, "slot " + std::to_string(sm.first) + " (High/Low role: HYPOTHESIS)", Source::Package, Confidence::Inferred, "native-kst"}, err)) return std::nullopt;
        }
    }
    rep = std::move(r);
    if (drumKeyMapsOut) *drumKeyMapsOut = std::move(localKeyMaps);
    return t;
}

// Backward-compatible 5-argument overload (melodic layouts only).
inline std::optional<Graph> buildNativeGraph(const KmpIndex& k, const std::vector<DecodedSound>& sounds, const LayoutTable& layouts,
                                             NativeRefReport& rep, std::string* err) {
    return buildNativeGraph(k, sounds, layouts, DrumKitLayoutTable{}, rep, err, nullptr);
}

// Dependencies of a Sound: everything it reaches, present or dangling, in id order (read-only).
inline std::vector<ResourceId> dependencies(const Graph& g, const ResourceId& sound) {
    auto c = g.closure({sound}); c.erase(sound);
    return {c.begin(), c.end()};
}
// Resources of kind k used by more than one Sound, with the number of distinct Sounds. A count of ANALYSED Sounds only.
inline std::map<ResourceId, size_t> sharedCounts(const Graph& g, Kind k) {
    std::map<ResourceId, size_t> r;
    for (const auto& [id, n] : g.nodes()) {
        if (id.kind != k) continue;
        size_t c = 0;
        for (const ResourceId& u : g.usedByRoots(id)) c += u.kind == Kind::Sound;
        if (c > 1) r[id] = c;
    }
    return r;
}

// ── Read-only Native-KST Integration (OC31/OC32 + Pa5X RAM.KMP + PCM -> Resource Graph) ──────────────────────────────

inline bool isOc31Magic(const uint8_t* p, size_t n) {
    return p != nullptr && n >= 4 && (std::memcmp(p, "OC31", 4) == 0 || std::memcmp(p, "OC32", 4) == 0);
}

enum class SoundPayloadMode { AutoDetectOc31, RequireOc31 };

struct PcmEntry {
    Id8 id{};
    std::string name;
    std::string file;                   // relative path or container label (e.g. "PCM/ram01.pcm")
    uint32_t payloadBytes = 0;          // item payload byte count in the KORF .pcm container
    std::optional<SampleInfo> info;     // optional explicit PCM facts; derived conservatively when absent
};

struct PcmInventory {
    std::map<Id8, PcmEntry> byId;
    size_t containersParsed = 0;
    size_t samplesTotal = 0;
    size_t samplesWithoutId = 0;        // e.g. Pa700 PCM items lacking directory field 4 (8-byte sample ID)
    std::vector<std::string> anomalies;
};

// Stages parsed KORF .pcm items into `inv` (all-or-nothing per container).
inline bool stagePcmContainerItems(const std::vector<panative::Item>& items, const std::string& label,
                                   PcmInventory& inv, std::string* err = nullptr) {
    if (err) err->clear();
    std::map<Id8, PcmEntry> staged;
    std::vector<std::string> stagedAnomalies;
    size_t withoutId = 0;
    for (const panative::Item& it : items) {
        if (it.sampleId.empty()) {
            ++withoutId;
            stagedAnomalies.push_back(label + ": sample '" + panative::printable(it.name) + "' lacks 8-byte PCM sample ID");
            continue;
        }
        if (it.sampleId.size() != 8) {
            if (err) *err = label + ": invalid PCM sample ID length";
            return false;
        }
        const Id8 id = native_detail::id8(reinterpret_cast<const uint8_t*>(it.sampleId.data()));
        if (inv.byId.count(id) || staged.count(id)) {
            if (err) *err = label + ": duplicate PCM sample ID " + native_detail::hexId(id);
            return false;
        }
        staged.emplace(id, PcmEntry{id, it.name, label, it.size, std::nullopt});
    }
    inv.byId.insert(staged.begin(), staged.end());
    inv.anomalies.insert(inv.anomalies.end(), stagedAnomalies.begin(), stagedAnomalies.end());
    ++inv.containersParsed;
    inv.samplesTotal += items.size();
    inv.samplesWithoutId += withoutId;
    return true;
}

// Parses one KORF .pcm container in memory and stages its sample IDs into `inv` (all-or-nothing per container).
inline bool addPcmContainerBytes(const uint8_t* data, size_t size, const std::string& label,
                                 PcmInventory& inv, std::string* err = nullptr) {
    if (err) err->clear();
    if (!data || size == 0) { if (err) *err = label + ": empty PCM container"; return false; }
    if (size > panative::kMaxFileBytes) { if (err) *err = label + ": PCM container larger than the reader's limit"; return false; }
    const panative::Container c = panative::parse(data, size);
    if (!c.ok) { if (err) *err = label + ": invalid PCM KORF container: " + c.error; return false; }
    return stagePcmContainerItems(c.items, label, inv, err);
}

// Reads only KORF metadata (header, directory, chunk headers, index) of a regular .pcm file below `dirFd` via pread()
// and stages its sample IDs into `inv` without loading raw audio payloads into RAM (all-or-nothing per container).
inline bool addPcmContainerFdAt(int dirFd, const char* name, const std::string& label,
                                size_t maxFileBytes, size_t& remainingBudget, size_t& bytesRead,
                                PcmInventory& inv, std::string* err = nullptr) {
    if (err) err->clear();
    panative::Container c;
    std::string rerr;
    if (!native_detail::readKorfMetadataAt(dirFd, name, maxFileBytes, remainingBudget, bytesRead, c, rerr)) {
        if (err) *err = label + ": " + rerr;
        return false;
    }
    return stagePcmContainerItems(c.items, label, inv, err);
}

struct MissingPcmSample {
    uint32_t sampleIndex = 0;
    Id8 id{};
    std::string name;
    std::string reason;
};

struct DuplicateKmpId {
    Id8 id{};
    std::vector<uint32_t> indices;
};

struct KmpBuildDiagnostics {
    bool zonesContiguous = false;
    size_t samplesTotal = 0;
    size_t samplesWithPcm = 0;
    size_t drumsTotal = 0;
    size_t zonesTotal = 0;
    size_t multisamplesTotal = 0;
    std::vector<MissingPcmSample> missingPcm;
    std::vector<DuplicateKmpId> duplicateDrumIds;   // real Pa5X backups can have duplicate drum IDs; nodes remain positional
    std::vector<PcmEntry> unmatchedPcm;             // PCM sample IDs absent from RAM.KMP (orphan PCM entries)
};

// Extracts and parses the Pa5X raw RAM.KMP item (chunk 0x11080018, version 0x0103) from a KORF container. Fail-closed.
inline bool parseKmpContainerBytes(const uint8_t* data, size_t size, panative::Pa5xKmp& out, std::string* err = nullptr) {
    out = panative::Pa5xKmp{};
    if (err) err->clear();
    auto fail = [&](const std::string& m) { out = panative::Pa5xKmp{}; if (err) *err = m; return false; };
    if (!data || size == 0) return fail("empty KMP container");
    if (size > panative::kMaxFileBytes) return fail("KMP container larger than the reader's limit");
    const panative::Container c = panative::parse(data, size);
    if (!c.ok) return fail("invalid KMP KORF container: " + c.error);
    if (c.items.empty()) return fail("KMP container holds no items");

    const panative::Item* kmpItem = nullptr;
    for (const panative::Item& it : c.items) {
        if (static_cast<size_t>(it.offset) + 8 > size || static_cast<size_t>(it.size) > size - static_cast<size_t>(it.offset) - 8)
            return fail("KMP item payload runs past the container");
        const uint8_t* body = data + static_cast<size_t>(it.offset) + 8;
        if (isOc31Magic(body, it.size))
            return fail("KMP item is OC31/OC32-encoded (only raw Pa5X RAM.KMP 0x11080018 version 0x0103 is supported)");
        if (it.chunkId == panative::kPa5xKmpChunkId) {
            if (kmpItem != nullptr) return fail("KMP container holds multiple RAM.KMP items (0x11080018)");
            kmpItem = &it;
        }
    }
    if (!kmpItem) return fail("KMP container holds no Pa5X RAM.KMP item (chunk 0x11080018)");
    return panative::parsePa5xKmp(data + static_cast<size_t>(kmpItem->offset) + 8, kmpItem->size, out, err);
}

// Converts a parsed Pa5xKmp + PcmInventory into a KmpIndex (all-or-nothing, fail-closed on structural errors).
inline bool kmpIndexFromPa5x(const panative::Pa5xKmp& kmp, const PcmInventory& pcm, KmpIndex& out,
                             KmpBuildDiagnostics* diag = nullptr, std::string* err = nullptr) {
    out = KmpIndex{};
    if (diag) *diag = KmpBuildDiagnostics{};
    if (err) err->clear();
    auto fail = [&](const std::string& m) {
        out = KmpIndex{};
        if (diag) *diag = KmpBuildDiagnostics{};
        if (err) *err = m;
        return false;
    };
    constexpr size_t kMaxObjects = 999999999;
    if (kmp.samples.size() > kMaxObjects || kmp.multisamples.size() > kMaxObjects ||
        kmp.drums.size() > kMaxObjects || kmp.zones.size() > kMaxObjects)
        return fail("Pa5X KMP index too large");

    std::set<Id8> sampleIds, multiIds;
    std::map<Id8, std::vector<uint32_t>> drumIdIndices;
    for (size_t i = 0; i < kmp.samples.size(); ++i)
        if (!sampleIds.insert(kmp.samples[i].id).second) return fail("duplicate sample ID in Pa5X KMP");
    for (size_t i = 0; i < kmp.multisamples.size(); ++i)
        if (!multiIds.insert(kmp.multisamples[i].id).second) return fail("duplicate multisample ID in Pa5X KMP");
    for (size_t i = 0; i < kmp.drums.size(); ++i)
        drumIdIndices[kmp.drums[i].id].push_back(static_cast<uint32_t>(i));

    for (size_t i = 0; i < kmp.zones.size(); ++i)
        if (kmp.zones[i].sample != panative::kNoIndex && kmp.zones[i].sample >= kmp.samples.size())
            return fail("zone " + std::to_string(i) + ": sample index out of range");
    for (size_t i = 0; i < kmp.multisamples.size(); ++i) {
        const auto& m = kmp.multisamples[i];
        if (m.firstZone > kmp.zones.size() || m.zoneCount > kmp.zones.size() - m.firstZone)
            return fail("multisample " + std::to_string(i) + ": zones run past the zone table");
    }
    for (size_t i = 0; i < kmp.drums.size(); ++i) {
        const auto& d = kmp.drums[i];
        if (d.left >= kmp.samples.size()) return fail("drum record " + std::to_string(i) + ": left sample index out of range");
        if (d.right != panative::kNoIndex && d.right >= kmp.samples.size())
            return fail("drum record " + std::to_string(i) + ": right sample index out of range");
    }

    KmpIndex tmp;
    KmpBuildDiagnostics d;
    d.zonesContiguous = kmp.zonesContiguous;
    d.samplesTotal = kmp.samples.size();
    d.drumsTotal = kmp.drums.size();
    d.zonesTotal = kmp.zones.size();
    d.multisamplesTotal = kmp.multisamples.size();

    tmp.samples.reserve(kmp.samples.size());
    for (size_t i = 0; i < kmp.samples.size(); ++i) {
        const auto& s = kmp.samples[i];
        std::optional<SampleInfo> info;
        const auto it = pcm.byId.find(s.id);
        if (it == pcm.byId.end()) {
            d.missingPcm.push_back({static_cast<uint32_t>(i), s.id, s.name, "no matching PCM entry"});
        } else {
            const PcmEntry& pe = it->second;
            if (pe.info.has_value()) {
                SampleInfo si = *pe.info;
                if (si.file.empty() && !pe.file.empty()) si.file = pe.file;
                if (!native_detail::validSampleInfo(si))
                    return fail("sample " + std::to_string(i) + " (" + panative::printable(s.name) + "): invalid PCM SampleInfo");
                info = std::move(si);
                ++d.samplesWithPcm;
            } else if (s.rate == 0) {
                d.missingPcm.push_back({static_cast<uint32_t>(i), s.id, s.name, "KMP sample rate is 0"});
            } else if (pe.payloadBytes == 0) {
                d.missingPcm.push_back({static_cast<uint32_t>(i), s.id, s.name, "PCM item payload is empty"});
            } else {
                uint64_t frames = static_cast<uint64_t>(pe.payloadBytes) / 2u;
                if (frames == 0) frames = 1;
                if (frames > Graph::kMaxFrames) frames = Graph::kMaxFrames;
                info = SampleInfo{1, s.rate, 16, frames, 0, "", pe.file};
                ++d.samplesWithPcm;
            }
        }
        tmp.samples.push_back({s.id, s.name, std::move(info)});
    }

    tmp.multisamples.reserve(kmp.multisamples.size());
    for (size_t i = 0; i < kmp.multisamples.size(); ++i)
        tmp.multisamples.push_back({kmp.multisamples[i].id, kmp.multisamples[i].name, kmp.zoneSamples(i)});

    tmp.drums.reserve(kmp.drums.size());
    for (const auto& dr : kmp.drums) {
        tmp.drumIds.insert(dr.id);
        tmp.drums.push_back({dr.id, dr.name, dr.left, dr.right, dr.drumFamilyByte});
    }

    for (const auto& [id, idxs] : drumIdIndices)
        if (idxs.size() > 1) d.duplicateDrumIds.push_back({id, idxs});
    for (const auto& [id, pe] : pcm.byId)
        if (!sampleIds.count(id)) d.unmatchedPcm.push_back(pe);

    out = std::move(tmp);
    if (diag) *diag = std::move(d);
    return true;
}

struct SoundDecodeFailure {
    std::string source;
    std::string bank;
    uint32_t number = 0;
    uint32_t kind = 0;
    std::string name;
    std::string reason;
};

struct SoundExtractResult {
    bool containerOk = false;
    std::string containerError;
    size_t itemsTotal = 0;
    size_t oc31Decoded = 0;
    size_t rawLoaded = 0;
    std::vector<DecodedSound> sounds;
    std::vector<SoundDecodeFailure> failures;
};

// Decodes one Sound item payload (OC31/OC32 via panative::decodeOc31(), or raw bytes in AutoDetectOc31 mode). Fail-closed.
inline bool decodeSoundPayload(const uint8_t* payload, size_t size, std::vector<uint8_t>& bodyOut, bool& wasOc31,
                               SoundPayloadMode mode = SoundPayloadMode::AutoDetectOc31, std::string* err = nullptr) {
    bodyOut.clear();
    wasOc31 = false;
    if (err) err->clear();
    if (!payload && size > 0) { if (err) *err = "null sound payload pointer"; return false; }
    if (size > panative::kMaxFileBytes) { if (err) *err = "sound payload larger than the reader's limit"; return false; }
    if (isOc31Magic(payload, size)) {
        wasOc31 = true;
        return panative::decodeOc31(payload, size, bodyOut, err);
    }
    if (mode == SoundPayloadMode::RequireOc31) {
        if (err) *err = "not an OC31/OC32 payload";
        return false;
    }
    if (size > 0) bodyOut.assign(payload, payload + size);
    return true;
}

// Parses one KORF SOUND/*.pcg container in memory and decodes each item payload into DecodedSound (read-only, fail-closed).
inline SoundExtractResult extractSoundsFromPcgBytes(const uint8_t* data, size_t size, const std::string& bankScope,
                                                    const std::string& sourceLabel,
                                                    SoundPayloadMode mode = SoundPayloadMode::AutoDetectOc31) {
    SoundExtractResult r;
    if (!ResourceId::validScope(bankScope)) { r.containerError = sourceLabel + ": invalid bank scope '" + bankScope + "'"; return r; }
    if (!data || size == 0) { r.containerError = sourceLabel + ": empty Sound container"; return r; }
    if (size > panative::kMaxFileBytes) { r.containerError = sourceLabel + ": Sound container larger than the reader's limit"; return r; }
    const panative::Container c = panative::parse(data, size);
    if (!c.ok) { r.containerError = sourceLabel + ": invalid Sound KORF container: " + c.error; return r; }

    r.containerOk = true;
    r.itemsTotal = c.items.size();
    std::set<uint32_t> seenSlots;
    for (const panative::Item& it : c.items) {
        if (static_cast<size_t>(it.offset) + 8 > size || static_cast<size_t>(it.size) > size - static_cast<size_t>(it.offset) - 8) {
            r.containerOk = false;
            r.containerError = sourceLabel + ": Sound item payload runs past the container";
            r.sounds.clear();
            return r;
        }
        if (!seenSlots.insert(it.slot).second) {
            r.failures.push_back({sourceLabel, bankScope, it.slot, it.chunkId, it.name, "duplicate Sound slot in bank " + bankScope});
            continue;
        }
        std::vector<uint8_t> body;
        bool wasOc31 = false;
        std::string derr;
        if (!decodeSoundPayload(data + static_cast<size_t>(it.offset) + 8, it.size, body, wasOc31, mode, &derr)) {
            r.failures.push_back({sourceLabel, bankScope, it.slot, it.chunkId, it.name, derr});
            continue;
        }
        ++(wasOc31 ? r.oc31Decoded : r.rawLoaded);
        r.sounds.push_back({bankScope, it.slot, it.chunkId, it.name, std::move(body)});
    }
    return r;
}

struct RawNamedBlob {
    std::string label;
    std::string bank;                   // optional bank scope for Sound containers; derived from label stem if empty
    std::vector<uint8_t> bytes;
};

struct NativeKstInput {
    std::optional<RawNamedBlob> kmpContainer;
    std::vector<RawNamedBlob> pcmContainers;
    std::optional<PcmInventory> extraPcm;
    std::vector<RawNamedBlob> soundContainers;
};

struct NativeKstOptions {
    LayoutTable layouts;                // caller-supplied melodic RecordLayouts by Sound chunk kind
    DrumKitLayoutTable drumLayouts;     // caller-supplied USERDK DrumKitLayouts by Sound chunk kind
    bool allowFitLayout = true;         // fit melodic layouts for remaining Sound kinds via fitLayout()
    bool allowFitDrumKitLayout = true;  // fit/verify USERDK layouts via fitDrumKitLayout()
    size_t fitMinItems = 8;
    SoundPayloadMode soundMode = SoundPayloadMode::AutoDetectOc31;
    size_t maxTotalBytes = 256u << 20;  // 256 MiB total read cap for folder reads
    size_t maxDirEntries = 1000000;     // entry cap for folder reads (matches kst_inventory.h ListLimits::entries)
};

struct SoundSlotMeta {
    std::string bank;
    uint32_t number = 0;
    uint32_t kind = 0;
    std::string name;
    std::string source;
    bool decoded = false;
};

struct NativeKstReport {
    bool kmpPresent = false;
    bool kmpParsed = false;
    std::string kmpError;
    KmpBuildDiagnostics kmp;
    size_t pcmContainers = 0;
    size_t pcmSamples = 0;
    size_t pcmSamplesWithoutId = 0;
    size_t soundContainers = 0;
    size_t soundsDecodedOc31 = 0;
    size_t soundsLoadedRaw = 0;
    size_t bytesRead = 0;               // total disk bytes read by resolveNativeKstFolder()
    LayoutTable usedLayouts;
    DrumKitLayoutTable usedDrumLayouts;
    std::map<ResourceId, DrumKitKeyMap> drumKeyMaps;
    NativeRefReport refs;
    std::vector<SoundSlotMeta> soundCatalog;   // occupied SOUND/*.pcg slots (sorted by bank, number), populated even if KMP fails
    std::vector<SoundDecodeFailure> decodeFailures;
    std::vector<std::string> containerErrors;
    std::vector<std::string> skippedPaths;
    std::vector<std::string> diagnostics;

    bool ok() const { return kmpPresent && kmpParsed && containerErrors.empty() && decodeFailures.empty(); }
    bool coverageComplete() const {
        return ok() && skippedPaths.empty() && pcmSamplesWithoutId == 0 && kmp.missingPcm.empty() &&
               kmp.duplicateDrumIds.empty() && kmp.unmatchedPcm.empty() && refs.coverageComplete();
    }
};

struct NativeKstResult : IDrumKitKeyMap {
    bool ok = false;                    // true when a fresh Graph was built (still check report.ok()/coverageComplete() for anomalies)
    std::optional<Graph> graph;         // never declared complete/scanned: deleteCheck() never says Unreferenced
    KmpIndex kmpIndex;
    NativeKstReport report;

    const DrumKitKeyMap* findDrumKitKeyMap(const ResourceId& soundId) const override {
        const auto it = report.drumKeyMaps.find(soundId);
        return it == report.drumKeyMaps.end() ? nullptr : &it->second;
    }
};

// End-to-end read-only Native-KST Resource Graph resolution in memory:
//   SOUND/*.pcg (decodeOc31) + MULTISMP/ram.kmp (parsePa5xKmp) + PCM/*.pcm -> Sound -> Oscillator -> Multisample -> Zone -> Sample -> PCM
//   and DrumSample -> L/R Sample -> PCM. Strictly fail-closed on missing/invalid RAM.KMP and corrupt inputs.
inline NativeKstResult resolveNativeKst(const NativeKstInput& in, const NativeKstOptions& opts = {}) {
    NativeKstResult res;

    // 1. PCM inventory
    PcmInventory pcm;
    if (in.extraPcm) pcm = *in.extraPcm;
    for (const RawNamedBlob& blob : in.pcmContainers) {
        std::string perr;
        if (!addPcmContainerBytes(blob.bytes.data(), blob.bytes.size(), blob.label, pcm, &perr)) {
            res.report.containerErrors.push_back(perr);
            res.report.diagnostics.push_back("pcm error: " + perr);
        }
    }
    res.report.pcmContainers = pcm.containersParsed;
    res.report.pcmSamples = pcm.byId.size() + pcm.samplesWithoutId;
    res.report.pcmSamplesWithoutId = pcm.samplesWithoutId;
    for (const std::string& a : pcm.anomalies) res.report.diagnostics.push_back("pcm: " + a);

    // 2. Sound containers (KORF parse + OC31/OC32 decode)
    std::vector<DecodedSound> sounds;
    std::set<std::pair<std::string, uint32_t>> seenSoundKeys;
    std::set<std::pair<std::string, uint32_t>> catalogKeys;
    for (const RawNamedBlob& blob : in.soundContainers) {
        const std::string bank = blob.bank.empty() ? native_detail::bankFromLabel(blob.label) : blob.bank;
        SoundExtractResult ex = extractSoundsFromPcgBytes(blob.bytes.data(), blob.bytes.size(), bank, blob.label, opts.soundMode);
        if (!ex.containerOk) {
            res.report.containerErrors.push_back(ex.containerError);
            res.report.diagnostics.push_back("sound container error: " + ex.containerError);
            continue;
        }
        ++res.report.soundContainers;
        res.report.soundsDecodedOc31 += ex.oc31Decoded;
        res.report.soundsLoadedRaw += ex.rawLoaded;
        for (DecodedSound& s : ex.sounds) {
            if (!seenSoundKeys.emplace(s.bank, s.number).second) {
                res.report.containerErrors.push_back(blob.label + ": duplicate Sound identity " + s.bank + "/" + std::to_string(s.number));
                res.report.decodeFailures.push_back({blob.label, s.bank, s.number, s.kind, s.name, "duplicate Sound identity across containers"});
                res.report.diagnostics.push_back("duplicate sound: " + s.bank + "/" + std::to_string(s.number) + " in " + blob.label);
                continue;
            }
            if (catalogKeys.emplace(s.bank, s.number).second)
                res.report.soundCatalog.push_back({s.bank, s.number, s.kind, s.name, blob.label, true});
            sounds.push_back(std::move(s));
        }
        for (const SoundDecodeFailure& f : ex.failures) {
            res.report.decodeFailures.push_back(f);
            res.report.diagnostics.push_back("sound decode failed: " + f.bank + "/" + std::to_string(f.number) +
                                             " (" + panative::printable(f.name) + ") in " + f.source + ": " + f.reason);
            if (catalogKeys.emplace(f.bank, f.number).second)
                res.report.soundCatalog.push_back({f.bank, f.number, f.kind, f.name, f.source, false});
        }
    }
    std::sort(res.report.soundCatalog.begin(), res.report.soundCatalog.end(), [](const SoundSlotMeta& a, const SoundSlotMeta& b) {
        return a.bank != b.bank ? a.bank < b.bank : a.number < b.number;
    });

    // 3. Pa5X RAM.KMP -> KmpIndex
    if (!in.kmpContainer.has_value()) {
        res.report.kmpPresent = false;
        res.report.kmpParsed = false;
        res.report.kmpError = "RAM.KMP is absent";
        res.report.diagnostics.push_back("unresolved: RAM.KMP is absent");
        return res;
    }
    res.report.kmpPresent = true;
    panative::Pa5xKmp parsedKmp;
    std::string kerr;
    if (!parseKmpContainerBytes(in.kmpContainer->bytes.data(), in.kmpContainer->bytes.size(), parsedKmp, &kerr)) {
        res.report.kmpParsed = false;
        res.report.kmpError = kerr;
        res.report.diagnostics.push_back("kmp error: " + kerr);
        return res;
    }
    if (!kmpIndexFromPa5x(parsedKmp, pcm, res.kmpIndex, &res.report.kmp, &kerr)) {
        res.report.kmpParsed = false;
        res.report.kmpError = kerr;
        res.report.diagnostics.push_back("kmp index error: " + kerr);
        return res;
    }
    res.report.kmpParsed = true;
    for (const MissingPcmSample& mp : res.report.kmp.missingPcm) {
        res.report.diagnostics.push_back("missing pcm: KMP sample " + std::to_string(mp.sampleIndex) +
                                         " (" + panative::printable(mp.name) + ", id " + native_detail::hexId(mp.id) + "): " + mp.reason);
    }
    for (const DuplicateKmpId& dup : res.report.kmp.duplicateDrumIds) {
        std::string idxs;
        for (size_t j = 0; j < dup.indices.size(); ++j) {
            if (j) idxs += ",";
            idxs += std::to_string(dup.indices[j]);
        }
        res.report.diagnostics.push_back("duplicate drum ID in KMP: " + native_detail::hexId(dup.id) + " (records " + idxs + ")");
    }
    for (const PcmEntry& pe : res.report.kmp.unmatchedPcm) {
        res.report.diagnostics.push_back("orphan pcm: " + pe.file + " (" + panative::printable(pe.name) +
                                         ", id " + native_detail::hexId(pe.id) + ") not referenced by RAM.KMP");
    }

    // 4. Layout resolution (caller-supplied + optional data-fitted melodic & USERDK layouts)
    res.report.usedLayouts = opts.layouts;
    res.report.usedDrumLayouts = opts.drumLayouts;
    if (opts.allowFitLayout || opts.allowFitDrumKitLayout) {
        std::map<uint32_t, std::vector<const DecodedSound*>> melodicByKind;
        std::map<uint32_t, std::vector<const DecodedSound*>> drumByKind;
        for (const DecodedSound& s : sounds) {
            if (isDrumKitSound(s)) drumByKind[s.kind].push_back(&s);
            else melodicByKind[s.kind].push_back(&s);
        }
        if (opts.allowFitLayout) {
            for (const auto& [kind, items] : melodicByKind) {
                if (res.report.usedLayouts.count(kind)) continue;
                if (auto fitted = fitLayout(items, res.kmpIndex, opts.fitMinItems))
                    res.report.usedLayouts.emplace(kind, *fitted);
            }
        }
        if (opts.allowFitDrumKitLayout) {
            for (const auto& [kind, items] : drumByKind) {
                if (res.report.usedDrumLayouts.count(kind)) continue;
                if (auto fitted = fitDrumKitLayout(items, res.kmpIndex))
                    res.report.usedDrumLayouts.emplace(kind, *fitted);
            }
        }
    }

    // 5. Build fresh Resource Graph
    std::string gerr;
    auto g = buildNativeGraph(res.kmpIndex, sounds, res.report.usedLayouts, res.report.usedDrumLayouts,
                              res.report.refs, &gerr, &res.report.drumKeyMaps);
    if (!g) {
        res.report.containerErrors.push_back("graph build failed: " + gerr);
        res.report.diagnostics.push_back("graph build failed: " + gerr);
        return res;
    }

    // 6. Record decode-failed Sounds as unanalysed nodes (Gap::DecodeFailed)
    for (const SoundDecodeFailure& f : res.report.decodeFailures) {
        if (!ResourceId::validScope(f.bank)) continue;
        const ResourceId sid{false, f.bank, Kind::Sound, f.number};
        if (g->find(sid)) continue;
        std::string addErr;
        if (g->addNode({sid, f.name, std::nullopt}, &addErr)) {
            ++res.report.refs.sounds;
            res.report.refs.unanalysed.push_back({sid, Gap::DecodeFailed});
        }
    }

    // 7. Consolidate diagnostics
    for (const std::string& dd : res.report.refs.drumDiagnostics) res.report.diagnostics.push_back(dd);
    for (const Unanalysed& u : res.report.refs.unanalysed) {
        if (u.why == Gap::DecodeFailed) continue;
        res.report.diagnostics.push_back("unanalysed: " + u.sound.str() + " (" + gapName(u.why) + ")");
    }
    for (const UnresolvedSlot& u : res.report.refs.unresolved) {
        res.report.diagnostics.push_back("unresolved slot: " + u.sound.str() + " record " + std::to_string(u.record) +
                                         " slot " + std::to_string(u.slot) +
                                         (u.wrongKind ? " (wrong object kind)" : " (unknown ID)") +
                                         (u.inUnanalysedSound ? " (in unanalysed Sound)" : ""));
    }
    for (const std::string& iss : g->issues()) res.report.diagnostics.push_back("graph issue: " + iss);

    res.graph = std::move(*g);
    res.ok = true;
    return res;
}

// Read-only disk folder entry point: opens the KST folder and its MULTISMP/, PCM/, SOUND/ subdirectories with O_NOFOLLOW,
// refuses symlinks/FIFOs/special files, bounds directory entries and individual/total read sizes, then runs resolveNativeKst().
inline NativeKstResult resolveNativeKstFolder(const std::filesystem::path& kstDir, const NativeKstOptions& opts = {}) {
    NativeKstResult earlyFail;
    const std::string rootPath = native_detail::stripTrailingSlashes(kstDir);
    const native_detail::ScopedFd rootFd(::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (rootFd.fd < 0) {
        const std::string msg = "cannot open KST folder (must be a real directory, symlinks are not followed): " + kstDir.string();
        earlyFail.report.containerErrors.push_back(msg);
        earlyFail.report.diagnostics.push_back(msg);
        return earlyFail;
    }

    size_t remainingEntries = opts.maxDirEntries;
    std::vector<std::string> topEntries;
    std::string lerr;
    if (!native_detail::listDirEntriesAt(rootFd.fd, topEntries, remainingEntries, lerr)) {
        const std::string msg = "cannot list KST folder " + kstDir.string() + ": " + lerr;
        earlyFail.report.containerErrors.push_back(msg);
        earlyFail.report.diagnostics.push_back(msg);
        return earlyFail;
    }
    remainingEntries -= topEntries.size();

    NativeKstInput in;
    PcmInventory folderPcm;
    std::vector<RawNamedBlob> kmpBlobs;
    std::set<std::string> seenSoundBanks;
    std::vector<std::string> folderErrors, skipped, folderDiags;
    size_t remainingBudget = opts.maxTotalBytes;
    size_t bytesRead = 0;

    for (const std::string& top : topEntries) {
        struct stat st{};
        if (::fstatat(rootFd.fd, top.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            folderErrors.push_back("cannot stat entry " + top);
            folderDiags.push_back("cannot stat entry " + top);
            continue;
        }
        const std::string topUpper = native_detail::upperAscii(top);
        const bool wantDir = topUpper == "MULTISMP" || topUpper == "PCM" || topUpper == "SOUND";
        if (!S_ISDIR(st.st_mode)) {
            if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
                skipped.push_back(top);
                folderDiags.push_back("skipped non-regular/symlink entry: " + top);
                if (wantDir) folderErrors.push_back("refusing non-directory/symlink at " + top);
            }
            continue;
        }
        if (!wantDir) continue;

        const native_detail::ScopedFd subFd(::openat(rootFd.fd, top.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (subFd.fd < 0) {
            const std::string msg = "cannot open folder " + top + " (symlinks are not followed)";
            folderErrors.push_back(msg);
            folderDiags.push_back(msg);
            continue;
        }
        std::vector<std::string> leaves;
        std::string serr;
        if (!native_detail::listDirEntriesAt(subFd.fd, leaves, remainingEntries, serr)) {
            const std::string msg = "cannot list folder " + top + ": " + serr;
            folderErrors.push_back(msg);
            folderDiags.push_back(msg);
            continue;
        }
        remainingEntries -= leaves.size();
        for (const std::string& leaf : leaves) {
            const std::string rel = top + "/" + leaf;
            struct stat lst{};
            if (::fstatat(subFd.fd, leaf.c_str(), &lst, AT_SYMLINK_NOFOLLOW) != 0) {
                folderErrors.push_back("cannot stat " + rel);
                folderDiags.push_back("cannot stat " + rel);
                continue;
            }
            if (!S_ISREG(lst.st_mode)) {
                skipped.push_back(rel);
                if (S_ISDIR(lst.st_mode)) {
                    folderDiags.push_back("skipped unexpected nested directory: " + rel);
                } else {
                    folderErrors.push_back("refusing non-regular file or symlink: " + rel);
                    folderDiags.push_back("skipped non-regular/symlink entry: " + rel);
                }
                continue;
            }
            const bool isKmp = topUpper == "MULTISMP" && native_detail::endsWithUpper(leaf, ".KMP");
            const bool isPcm = topUpper == "PCM" && native_detail::endsWithUpper(leaf, ".PCM");
            const bool isPcg = topUpper == "SOUND" && native_detail::endsWithUpper(leaf, ".PCG");
            if (!isKmp && !isPcm && !isPcg) continue;

            if (isPcm) {
                std::string perr;
                if (!addPcmContainerFdAt(subFd.fd, leaf.c_str(), rel, panative::kMaxFileBytes, remainingBudget, bytesRead, folderPcm, &perr)) {
                    folderErrors.push_back(perr);
                    folderDiags.push_back("pcm error: " + perr);
                }
                continue;
            }

            std::vector<uint8_t> bytes;
            std::string rerr;
            if (!native_detail::readRegularAt(subFd.fd, leaf.c_str(), panative::kMaxFileBytes, remainingBudget, bytes, rerr)) {
                folderErrors.push_back(rel + ": " + rerr);
                folderDiags.push_back("read error: " + rel + ": " + rerr);
                continue;
            }
            remainingBudget -= bytes.size();
            bytesRead += bytes.size();
            if (isKmp) {
                kmpBlobs.push_back({rel, "", std::move(bytes)});
            } else {
                const std::string bank = native_detail::bankFromLabel(rel);
                if (!seenSoundBanks.insert(bank).second) {
                    folderErrors.push_back("duplicate Sound bank '" + bank + "' (" + rel + "); refusing to guess");
                    folderDiags.push_back("duplicate Sound bank: " + bank + " (" + rel + ")");
                    continue;
                }
                in.soundContainers.push_back({rel, bank, std::move(bytes)});
            }
        }
    }

    in.extraPcm = std::move(folderPcm);
    if (kmpBlobs.size() == 1) {
        in.kmpContainer = std::move(kmpBlobs[0]);
    } else if (kmpBlobs.size() > 1) {
        folderErrors.push_back("multiple .kmp files in MULTISMP/ (" + std::to_string(kmpBlobs.size()) + "); refusing to guess");
        folderDiags.push_back("ambiguous KMP folder: multiple .kmp files in MULTISMP/");
    }

    NativeKstResult res = resolveNativeKst(in, opts);
    res.report.bytesRead = bytesRead;
    res.report.skippedPaths.insert(res.report.skippedPaths.begin(), skipped.begin(), skipped.end());
    res.report.containerErrors.insert(res.report.containerErrors.begin(), folderErrors.begin(), folderErrors.end());
    res.report.diagnostics.insert(res.report.diagnostics.begin(), folderDiags.begin(), folderDiags.end());
    return res;
}

}  // namespace ai_arranger::media::graph
#endif
