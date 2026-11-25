#ifndef AI_ARRANGER_MEDIA_SOUND_SLOT_CONFLICTS_H
#define AI_ARRANGER_MEDIA_SOUND_SLOT_CONFLICTS_H

// M2a Target Sound Bank/Slot Occupancy Grid, Strict 3-Way Sound Equivalence Fingerprinting,
// Deterministic Canonical Free-Slot Allocator, and Complete Collision Matrix (READ-ONLY).
//
// Boundaries & Safety Invariants:
//  * Strictly READ-ONLY: zero disk writes, zero mutations to source/target libraries, zero OC31 encoder.
//  * Safe POSIX folder reads: O_RDONLY | O_NOFOLLOW | O_NONBLOCK, S_ISREG/S_ISDIR, trailing-slash stripping,
//    F_DUPFD_CLOEXEC, pre-allocated byte/file/entry caps, cooperative keepGoing cancellation.
//  * Bank identity comes from file name (SOUND/USER01..12.PCG, SOUND/USERDK01..06.PCG), NEVER KORF dir bank byte.
//  * 3-Way Sound Equivalence (`Identical`, `Different`, `InsufficientEvidence`):
//      - NEVER matches by Sound name or filename alone.
//      - `Identical` ONLY when: same chunk kind, exact decoded body SHA-256, BOTH Sounds Analysed (0 UnresolvedSlots),
//        AND every referenced Multisample Id8, ordered zone table (topKey, sampleId8), and Sample Id8 + PCM payload size match.
//      - `InsufficientEvidence` whenever either side is unanalysed (USERDK, NoKnownReference, unknown kind, OC31 KMP,
//        unresolved slots, missing PCM) — NEVER auto-reused, treated fail-closed as conflict.
//  * NO policy ever silently overwrites an occupied target Sound slot or colliding object path.
//  * FAT32/exFAT R-18 ASCII case-folded object path collision refusal (`STYLE/`, `KEYBOARDSET/`, `PAD/`, `SETTINGS/`).
//  * Fail-closed `TargetUninspectable` with graceful `PartialNativeInspect` summary on OC31 RAM.KMP / corrupt PCGs.

#include "media/sha256.h"
#include "media/sound/native_kst_closure.h"
#include "media/sound/selective_diagnostics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ai_arranger::media::selective {

// ── Pa5X User Sound / Drum Kit Grid Constants ─────────────────────────────────────────────────────────────────────────

inline constexpr uint8_t  kUserSoundMsb            = 121;
inline constexpr uint8_t  kUserDrumKitMsb          = 120;
inline constexpr uint8_t  kUserBankLsbBase         = 64;   // bank 1 -> LSB 64
inline constexpr uint32_t kUserSoundBankCount      = 12;   // USER01..USER12 (LSB 64..75)
inline constexpr uint32_t kUserDrumKitBankCount    = 6;    // USERDK01..USERDK06 (LSB 64..69)
inline constexpr uint32_t kSlotsPerBank            = 128;  // PC 0..127
inline constexpr uint32_t kTotalUserSoundSlots     = kUserSoundBankCount * kSlotsPerBank;     // 1,536
inline constexpr uint32_t kTotalUserDrumKitSlots   = kUserDrumKitBankCount * kSlotsPerBank;   // 768
inline constexpr uint8_t  kOutOfSpecDrumKitLsbMin  = 70;   // USERDK07..USERDK12 (outside documented 6 DK banks)
inline constexpr uint8_t  kOutOfSpecDrumKitLsbMax  = 75;

// ── Slot Addressing Model ─────────────────────────────────────────────────────────────────────────────────────────────

struct SlotAddress {
    bool isDrumKit = false;
    uint32_t bank = 1;  // 1..12 for melodic (USER01..12); 1..6 in-spec (7..12 out-of-spec) for DK
    uint32_t slot = 0;  // 0..127

    auto operator<=>(const SlotAddress&) const = default;

    uint8_t msb() const { return isDrumKit ? kUserDrumKitMsb : kUserSoundMsb; }
    uint8_t lsb() const { return static_cast<uint8_t>(63u + bank); }
    uint8_t pc() const { return static_cast<uint8_t>(slot); }

    bool isInSpec() const {
        const uint32_t maxBank = isDrumKit ? kUserDrumKitBankCount : kUserSoundBankCount;
        return bank >= 1 && bank <= maxBank && slot < kSlotsPerBank;
    }

    bool isOutOfSpecDrumKitBank() const {
        return isDrumKit && bank >= 7 && bank <= 12 && slot < kSlotsPerBank;
    }

    std::string bankName() const {
        char buf[24];
        std::snprintf(buf, sizeof buf, isDrumKit ? "USERDK%02u" : "USER%02u", bank);
        return buf;
    }

    std::string shortLabel() const {
        return bankName() + "/" + std::to_string(slot);
    }

    std::string label() const {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s/%u (MSB %u LSB %u PC %u)",
                      bankName().c_str(), slot,
                      static_cast<unsigned>(msb()),
                      static_cast<unsigned>(lsb()),
                      static_cast<unsigned>(pc()));
        return buf;
    }

    graph::ResourceId resourceId() const {
        return graph::ResourceId{false, bankName(), graph::Kind::Sound, slot};
    }

    static std::optional<SlotAddress> fromBankAndSlot(const std::string& bankNameRaw, uint32_t slot) {
        if (slot >= kSlotsPerBank) return std::nullopt;
        const std::string b = graph::native_detail::upperAscii(bankNameRaw);
        bool dk = false;
        std::string numStr;
        if (b.rfind("USERDK", 0) == 0 && b.size() == 8) {
            dk = true;
            numStr = b.substr(6, 2);
        } else if (b.rfind("USER", 0) == 0 && b.size() == 6) {
            dk = false;
            numStr = b.substr(4, 2);
        } else {
            return std::nullopt;
        }
        if (!std::isdigit(static_cast<unsigned char>(numStr[0])) ||
            !std::isdigit(static_cast<unsigned char>(numStr[1]))) {
            return std::nullopt;
        }
        const uint32_t bankNum = static_cast<uint32_t>((numStr[0] - '0') * 10 + (numStr[1] - '0'));
        if (bankNum < 1 || bankNum > 12) return std::nullopt;
        return SlotAddress{dk, bankNum, slot};
    }

    static std::optional<SlotAddress> fromMidi(int msb, int lsb, int pc) {
        if ((msb != kUserSoundMsb && msb != kUserDrumKitMsb) || lsb < 64 || lsb > 75 || pc < 0 || pc >= 128) {
            return std::nullopt;
        }
        return SlotAddress{msb == kUserDrumKitMsb, static_cast<uint32_t>(lsb - 63), static_cast<uint32_t>(pc)};
    }
};

// ── Strict 3-Way Sound Equivalence Fingerprinting ─────────────────────────────────────────────────────────────────────

enum class EquivalenceVerdict {
    Identical,
    Different,
    InsufficientEvidence
};

inline const char* equivalenceVerdictName(EquivalenceVerdict v) {
    switch (v) {
        case EquivalenceVerdict::Identical:            return "Identical";
        case EquivalenceVerdict::Different:            return "Different";
        case EquivalenceVerdict::InsufficientEvidence: return "InsufficientEvidence";
    }
    return "InsufficientEvidence";
}

struct ZoneFingerprint {
    uint8_t topKey = 0;
    bool hasSample = false;         // false when KMP zone sample == kNoIndex
    graph::Id8 sampleId{};          // 8-byte KMP Sample Id8
    uint32_t pcmPayloadBytes = 0;   // exact PCM KORF item payload byte count
    bool pcmPresent = false;        // true iff sampleId resolved in PCM/*.pcm with payloadBytes > 0

    auto operator<=>(const ZoneFingerprint&) const = default;
};

struct MultisampleFingerprint {
    graph::Id8 multisampleId{};
    std::vector<ZoneFingerprint> zones;  // ordered zone table (exact zone order from RAM.KMP)

    auto operator<=>(const MultisampleFingerprint&) const = default;
};

// Holds ONLY structural & cryptographic evidence for Sound equivalence.
// Intentionally excludes Sound name and filename so matching by name/filename is structurally impossible.
struct SoundEquivalenceProfile {
    uint32_t kindChunkId = 0;
    std::string bodySha256;         // 64-hex SHA-256 of decoded Sound body bytes
    bool decoded = false;
    bool analysed = false;          // true iff analysed by buildNativeGraph (not unanalysed)
    std::string unanalysedReason;   // "" when analysed, else Gap name / KmpUnavailable / Oc31KmpUnsupported
    size_t unresolvedSlotCount = 0; // MUST be 0 for equivalence proof
    bool isDrumKit = false;         // USERDK bodies are unparsed -> always InsufficientEvidence
    bool kmpComplete = false;       // true iff raw Pa5X RAM.KMP parsed and zone/PCM tables are populated
    std::vector<MultisampleFingerprint> referencedMultisamples; // canonically sorted by multisampleId
    std::string equivalenceDigest;  // deterministic 64-hex composite digest ("" unless provablyComparable())

    bool allReferencedSamplesHavePcm() const {
        for (const auto& ms : referencedMultisamples) {
            for (const auto& z : ms.zones) {
                if (z.hasSample && (!z.pcmPresent || z.pcmPayloadBytes == 0)) return false;
            }
        }
        return true;
    }

    bool provablyComparable() const {
        return decoded && analysed && !isDrumKit && kmpComplete &&
               unresolvedSlotCount == 0 && kindChunkId != 0 &&
               bodySha256.size() == 64 && !referencedMultisamples.empty() &&
               allReferencedSamplesHavePcm();
    }
};

namespace conflict_detail {

inline std::string asciiLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string normalizeRelPath(std::string p) {
    for (char& c : p) if (c == '\\') c = '/';
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    return p;
}

inline std::string sha256Bytes(const uint8_t* data, size_t n) {
    Sha256 h;
    if (data && n > 0) h.update(data, n);
    return h.hex();
}

inline void hashU32Be(Sha256& h, uint32_t v) {
    const uint8_t b[4] = {
        static_cast<uint8_t>((v >> 24) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>(v & 0xFFu)
    };
    h.update(b, 4);
}

inline void hashU64Be(Sha256& h, uint64_t v) {
    const uint8_t b[8] = {
        static_cast<uint8_t>((v >> 56) & 0xFFu),
        static_cast<uint8_t>((v >> 48) & 0xFFu),
        static_cast<uint8_t>((v >> 40) & 0xFFu),
        static_cast<uint8_t>((v >> 32) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>(v & 0xFFu)
    };
    h.update(b, 8);
}

inline void hashStr(Sha256& h, const std::string& s) {
    hashU64Be(h, static_cast<uint64_t>(s.size()));
    if (!s.empty()) h.update(s.data(), s.size());
}

inline std::string insufficientEvidenceReason(const SoundEquivalenceProfile& p, const char* side) {
    if (p.isDrumKit) return std::string(side) + " is a User Drum Kit (USERDK body unparsed)";
    if (!p.decoded) return std::string(side) + " Sound payload is undecoded";
    if (!p.kmpComplete) return std::string(side) + " RAM.KMP or PCM metadata is unavailable";
    if (!p.analysed) {
        return std::string(side) + " Sound is unanalysed" +
               (p.unanalysedReason.empty() ? "" : " (" + p.unanalysedReason + ")");
    }
    if (p.unresolvedSlotCount > 0) {
        return std::string(side) + " Sound has " + std::to_string(p.unresolvedSlotCount) +
               " unresolved oscillator slot(s)";
    }
    if (p.kindChunkId == 0 || p.bodySha256.size() != 64) {
        return std::string(side) + " Sound lacks chunk kind or decoded body SHA-256";
    }
    if (p.referencedMultisamples.empty()) {
        return std::string(side) + " Sound has no verified RAM Multisample references";
    }
    if (!p.allReferencedSamplesHavePcm()) {
        return std::string(side) + " Sound references Sample(s) with missing PCM payload metadata";
    }
    return std::string(side) + " Sound lacks sufficient structural evidence";
}

// Streams a regular file under `dirFd` in 64 KiB chunks to compute SHA-256 + size without loading the file into RAM.
inline bool hashRegularFileAt(int dirFd, const char* name, size_t maxFileBytes,
                              size_t& remainingBudget, size_t& bytesRead,
                              uint64_t& sizeOut, std::string& shaOut, std::string& err) {
    sizeOut = 0;
    shaOut.clear();
    err.clear();

    struct stat lst{};
    if (::fstatat(dirFd, name, &lst, AT_SYMLINK_NOFOLLOW) != 0) { err = "cannot stat entry"; return false; }
    if (!S_ISREG(lst.st_mode)) { err = "not a regular file (symlinks and special files are refused)"; return false; }
    const graph::native_detail::ScopedFd f(::openat(dirFd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (f.fd < 0) { err = "cannot open regular file (symlinks are not followed)"; return false; }
    struct stat st{};
    if (::fstat(f.fd, &st) != 0 || !S_ISREG(st.st_mode)) { err = "not a regular file"; return false; }
    if (st.st_size < 0) { err = "invalid file size"; return false; }
    const uint64_t fsz = static_cast<uint64_t>(st.st_size);
    if (fsz > maxFileBytes) { err = "file larger than the reader's limit"; return false; }
    if (fsz > remainingBudget) { err = "total read size exceeds the reader's limit"; return false; }

    std::vector<uint8_t> buf(65536);
    Sha256 hasher;
    uint64_t total = 0;
    while (total < fsz) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), fsz - total));
        const ssize_t r = ::read(f.fd, buf.data(), want);
        if (r < 0) {
            if (errno == EINTR) continue;
            err = "read failed";
            return false;
        }
        if (r == 0) { err = "file shrank while being read"; return false; }
        hasher.update(buf.data(), static_cast<size_t>(r));
        total += static_cast<uint64_t>(r);
        remainingBudget -= static_cast<size_t>(r);
        bytesRead += static_cast<size_t>(r);
    }
    uint8_t extra = 0;
    for (;;) {
        const ssize_t r = ::read(f.fd, &extra, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            err = "read failed";
            return false;
        }
        if (r > 0) { err = "file grew while being read"; return false; }
        break;
    }
    sizeOut = total;
    shaOut = hasher.hex();
    return true;
}

inline bool isBankedObjectRootDir(const std::string& topUpper) {
    return topUpper == "STYLE" || topUpper == "KEYBOARDSET" || topUpper == "PAD";
}

inline bool isTrackedObjectExt(const std::string& name) {
    return graph::native_detail::endsWithUpper(name, ".STG") ||
           graph::native_detail::endsWithUpper(name, ".KSC") ||
           graph::native_detail::endsWithUpper(name, ".PDC");
}

inline std::string applyTargetPathOverride(const std::string& sourceRelPath,
                                           const std::string& targetBank,
                                           const std::string& targetName) {
    const std::string norm = normalizeRelPath(sourceRelPath);
    if (targetBank.empty() && targetName.empty()) return norm;

    std::string locCat, bank, leaf;
    const size_t firstSlash = norm.find('/');
    const size_t lastSlash = norm.rfind('/');
    if (firstSlash == std::string::npos) {
        leaf = norm;
    } else if (firstSlash == lastSlash) {
        locCat = norm.substr(0, firstSlash);
        leaf = norm.substr(firstSlash + 1);
    } else {
        locCat = norm.substr(0, firstSlash);
        bank = norm.substr(firstSlash + 1, lastSlash - firstSlash - 1);
        leaf = norm.substr(lastSlash + 1);
    }

    const std::string effLeaf = targetName.empty() ? leaf : normalizeRelPath(targetName);
    if (!targetBank.empty()) {
        const std::string tb = normalizeRelPath(targetBank);
        if (tb.find('/') != std::string::npos) {
            // Caller passed full "TYPE/Bank"
            return tb + "/" + effLeaf;
        }
        if (!locCat.empty()) return locCat + "/" + tb + "/" + effLeaf;
        return tb + "/" + effLeaf;
    }
    if (!locCat.empty() && !bank.empty()) return locCat + "/" + bank + "/" + effLeaf;
    if (!locCat.empty()) return locCat + "/" + effLeaf;
    return effLeaf;
}

}  // namespace conflict_detail

// Canonicalizes `p.referencedMultisamples` (sorted unique by multisampleId) and computes `p.equivalenceDigest`.
// Digest is non-empty ONLY when `p.provablyComparable()` is true.
inline std::string finalizeSoundEquivalenceProfile(SoundEquivalenceProfile& p) {
    std::sort(p.referencedMultisamples.begin(), p.referencedMultisamples.end(),
              [](const MultisampleFingerprint& a, const MultisampleFingerprint& b) {
                  return a.multisampleId != b.multisampleId ? a.multisampleId < b.multisampleId : a.zones < b.zones;
              });
    p.referencedMultisamples.erase(
        std::unique(p.referencedMultisamples.begin(), p.referencedMultisamples.end()),
        p.referencedMultisamples.end());

    if (!p.provablyComparable()) {
        p.equivalenceDigest.clear();
        return {};
    }

    Sha256 h;
    const std::string domain = "aiarranger-sound-equiv/1";
    conflict_detail::hashStr(h, domain);
    conflict_detail::hashU32Be(h, p.kindChunkId);
    conflict_detail::hashStr(h, p.bodySha256);
    conflict_detail::hashU64Be(h, static_cast<uint64_t>(p.referencedMultisamples.size()));
    for (const auto& ms : p.referencedMultisamples) {
        h.update(ms.multisampleId.data(), ms.multisampleId.size());
        conflict_detail::hashU64Be(h, static_cast<uint64_t>(ms.zones.size()));
        for (const auto& z : ms.zones) {
            const uint8_t flags[2] = {z.topKey, static_cast<uint8_t>(z.hasSample ? 1u : 0u)};
            h.update(flags, 2);
            if (z.hasSample) {
                h.update(z.sampleId.data(), z.sampleId.size());
                conflict_detail::hashU32Be(h, z.pcmPayloadBytes);
            }
        }
    }
    p.equivalenceDigest = h.hex();
    return p.equivalenceDigest;
}

struct EquivalenceResult {
    EquivalenceVerdict verdict = EquivalenceVerdict::InsufficientEvidence;
    std::string reason;
};

// Strict 3-way Sound equivalence comparison.
// NEVER matches by Sound name or filename. Fail-closed to InsufficientEvidence whenever either side is unanalysed.
inline EquivalenceResult compareSoundEquivalence(const SoundEquivalenceProfile& src,
                                                 const SoundEquivalenceProfile& dst) {
    if (!src.provablyComparable()) {
        return {EquivalenceVerdict::InsufficientEvidence,
                conflict_detail::insufficientEvidenceReason(src, "source")};
    }
    if (!dst.provablyComparable()) {
        return {EquivalenceVerdict::InsufficientEvidence,
                conflict_detail::insufficientEvidenceReason(dst, "target")};
    }
    if (src.kindChunkId != dst.kindChunkId) {
        return {EquivalenceVerdict::Different, "Sound chunk kind differs"};
    }
    if (src.bodySha256 != dst.bodySha256) {
        return {EquivalenceVerdict::Different, "decoded Sound body SHA-256 differs"};
    }

    auto sortedMs = [](const SoundEquivalenceProfile& p) {
        std::vector<MultisampleFingerprint> v = p.referencedMultisamples;
        std::sort(v.begin(), v.end(), [](const MultisampleFingerprint& a, const MultisampleFingerprint& b) {
            return a.multisampleId != b.multisampleId ? a.multisampleId < b.multisampleId : a.zones < b.zones;
        });
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    };
    const auto sMs = sortedMs(src);
    const auto dMs = sortedMs(dst);
    if (sMs.size() != dMs.size()) {
        return {EquivalenceVerdict::Different, "referenced Multisample count differs"};
    }
    for (size_t i = 0; i < sMs.size(); ++i) {
        if (sMs[i].multisampleId != dMs[i].multisampleId) {
            return {EquivalenceVerdict::Different, "referenced Multisample Id8 differs"};
        }
        if (sMs[i].zones != dMs[i].zones) {
            return {EquivalenceVerdict::Different,
                    "referenced Multisample zone table (topKey, sampleId8, PCM payload size) differs"};
        }
    }
    return {EquivalenceVerdict::Identical,
            "proven identical (chunk kind, decoded body SHA-256, and full analysed Multisample/Zone/Sample/PCM fingerprint match)"};
}

// ── Target Occupancy Map & Graceful Partial Native Inspect ────────────────────────────────────────────────────────────

enum class SlotState {
    Free,            // Bank file present (or empty grid), slot is unoccupied
    BankFileAbsent,  // SOUND/USERnn.PCG or SOUND/USERDKnn.PCG is absent in target KST
    Occupied         // Slot is occupied by a Sound/DrumKit
};

inline const char* slotStateName(SlotState s) {
    switch (s) {
        case SlotState::Free:           return "Free";
        case SlotState::BankFileAbsent: return "BankFileAbsent";
        case SlotState::Occupied:       return "Occupied";
    }
    return "Free";
}

struct OccupiedSlotEntry {
    SlotAddress address;
    std::string displayName;        // Printable/raw label for reporting ONLY (never identity/equivalence)
    std::string sourceContainer;    // e.g. "SOUND/USER03.PCG"
    SoundEquivalenceProfile equivalence;
};

struct ObjectPathEntry {
    std::string relPath;            // Canonical original-case path, e.g. "STYLE/BankA/Demo.stg"
    std::string foldedPath;         // ASCII lowercase path for FAT32/exFAT R-18 checks
    std::string sha256;             // 64-hex SHA-256 of file content
    uint64_t sizeBytes = 0;

    auto operator<=>(const ObjectPathEntry&) const = default;
};

// Gate UX-2: structured graceful degradation payload when target has OC31 RAM.KMP or PCG errors.
struct PartialNativeInspect {
    bool isPartial = false;
    bool kmpPresent = false;
    bool kmpIsOc31 = false;
    bool kmpParsed = false;
    std::string kmpError;
    size_t soundContainerCount = 0;
    size_t occupiedUserSounds = 0;
    size_t occupiedUserDrumKits = 0;
    size_t pcmContainerCount = 0;
    size_t pcmSampleCount = 0;
    std::vector<graph::SoundSlotMeta> soundInventory;  // sorted by (bank, number)
    std::string notice;
};

struct TargetInspectOptions {
    graph::NativeKstOptions downstream;
    size_t maxObjectTotalBytes = sizeof(size_t) >= 8 ? (size_t(2) << 30) : (size_t(512) << 20);
    size_t maxObjectFileBytes = panative::kMaxFileBytes;
    size_t maxObjectFiles = 100000;
    size_t maxDirEntries = 1000000;
    bool hashObjectFiles = true;               // stream SHA-256 of STYLE/KEYBOARDSET/PAD files for exact-duplicate checks
    std::function<bool()> keepGoing = {};      // UX Gate #4 cooperative cancellation (checked every <= 256 items)
};

struct TargetOccupancyMap {
    bool valid = false;
    bool fullyInspectable = false;
    bool cancelled = false;
    std::string uninspectableReason;           // path-free canonical explanation
    std::string targetLibraryKey;              // deterministic 64-hex SHA-256

    std::array<bool, kUserSoundBankCount> userSoundBankPresent{};
    std::array<bool, kUserDrumKitBankCount> userDrumKitBankPresent{};
    std::set<std::string> extraOutOfSpecDrumKitBanksPresent;

    std::map<SlotAddress, OccupiedSlotEntry> occupiedSlots;
    std::map<std::string, std::vector<ObjectPathEntry>> objectPathsByFolded;
    std::map<std::string, std::vector<SlotAddress>> equivalenceIndex;  // equivalenceDigest -> sorted SlotAddresses

    PartialNativeInspect partialInspect;
    CoverageSummary coverage;
    std::vector<Diagnostic> targetDiagnostics;

    size_t bytesReadDownstream = 0;
    size_t bytesReadObjects = 0;

    static TargetOccupancyMap makeEmpty(bool allBanksPresent = true) {
        TargetOccupancyMap m;
        m.valid = true;
        m.fullyInspectable = true;
        m.userSoundBankPresent.fill(allBanksPresent);
        m.userDrumKitBankPresent.fill(allBanksPresent);
        m.coverage.coverageComplete = true;
        m.coverage.deleteProofAvailable = false;
        m.rebuildIndexesAndKey();
        return m;
    }

    bool isBankFilePresent(bool isDrumKit, uint32_t bank) const {
        if (!isDrumKit) {
            return bank >= 1 && bank <= kUserSoundBankCount && userSoundBankPresent[bank - 1];
        }
        if (bank >= 1 && bank <= kUserDrumKitBankCount) {
            return userDrumKitBankPresent[bank - 1];
        }
        if (bank >= 7 && bank <= 12) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "USERDK%02u", bank);
            return extraOutOfSpecDrumKitBanksPresent.count(buf) > 0;
        }
        return false;
    }

    SlotState slotState(const SlotAddress& addr) const {
        if (occupiedSlots.count(addr)) return SlotState::Occupied;
        if (!isBankFilePresent(addr.isDrumKit, addr.bank)) return SlotState::BankFileAbsent;
        return SlotState::Free;
    }

    const OccupiedSlotEntry* findOccupied(const SlotAddress& addr) const {
        const auto it = occupiedSlots.find(addr);
        return it == occupiedSlots.end() ? nullptr : &it->second;
    }

    size_t occupiedCount(bool isDrumKit) const {
        size_t n = 0;
        for (const auto& [addr, e] : occupiedSlots) {
            if (addr.isDrumKit == isDrumKit && addr.isInSpec()) ++n;
        }
        return n;
    }

    size_t freeCount(bool isDrumKit) const {
        const size_t total = isDrumKit ? kTotalUserDrumKitSlots : kTotalUserSoundSlots;
        const size_t occ = occupiedCount(isDrumKit);
        return occ >= total ? 0 : (total - occ);
    }

    // Helper for synthetic/in-memory construction: adds an occupied slot and marks its bank present.
    void addOccupiedSlot(OccupiedSlotEntry entry) {
        finalizeSoundEquivalenceProfile(entry.equivalence);
        if (!entry.address.isDrumKit && entry.address.bank >= 1 && entry.address.bank <= kUserSoundBankCount) {
            userSoundBankPresent[entry.address.bank - 1] = true;
        } else if (entry.address.isDrumKit && entry.address.bank >= 1 && entry.address.bank <= kUserDrumKitBankCount) {
            userDrumKitBankPresent[entry.address.bank - 1] = true;
        } else if (entry.address.isOutOfSpecDrumKitBank()) {
            extraOutOfSpecDrumKitBanksPresent.insert(entry.address.bankName());
        }
        const SlotAddress key = entry.address;
        occupiedSlots[key] = std::move(entry);
    }

    // Helper for synthetic/in-memory construction: registers an existing Style/KSC/Pad object path.
    void addObjectPath(const std::string& relPath, const std::string& sha256, uint64_t sizeBytes = 0) {
        const std::string norm = conflict_detail::normalizeRelPath(relPath);
        const std::string folded = conflict_detail::asciiLower(norm);
        auto& vec = objectPathsByFolded[folded];
        for (const auto& existing : vec) {
            if (existing.relPath == norm) return;
        }
        vec.push_back({norm, folded, sha256, sizeBytes});
    }

    // Rebuilds deterministic equivalenceIndex, sorts objectPath vectors, and recomputes targetLibraryKey (SHA-256).
    void rebuildIndexesAndKey() {
        equivalenceIndex.clear();
        for (auto& [addr, entry] : occupiedSlots) {
            if (entry.equivalence.equivalenceDigest.empty() && entry.equivalence.provablyComparable()) {
                finalizeSoundEquivalenceProfile(entry.equivalence);
            }
            if (entry.equivalence.provablyComparable() && !entry.equivalence.equivalenceDigest.empty()) {
                equivalenceIndex[entry.equivalence.equivalenceDigest].push_back(addr);
            }
        }
        for (auto& [digest, addrs] : equivalenceIndex) {
            std::sort(addrs.begin(), addrs.end());
            addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
        }
        for (auto& [folded, vec] : objectPathsByFolded) {
            std::sort(vec.begin(), vec.end());
            vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
        }
        canonicalizeDiagnostics(targetDiagnostics);

        Sha256 h;
        conflict_detail::hashStr(h, "aiarranger-target-occupancy-key/1");
        const uint8_t flags[5] = {
            static_cast<uint8_t>(valid ? 1u : 0u),
            static_cast<uint8_t>(fullyInspectable ? 1u : 0u),
            static_cast<uint8_t>(partialInspect.kmpPresent ? 1u : 0u),
            static_cast<uint8_t>(partialInspect.kmpIsOc31 ? 1u : 0u),
            static_cast<uint8_t>(partialInspect.kmpParsed ? 1u : 0u)
        };
        h.update(flags, sizeof flags);
        for (bool b : userSoundBankPresent) { const uint8_t v = b ? 1u : 0u; h.update(&v, 1); }
        for (bool b : userDrumKitBankPresent) { const uint8_t v = b ? 1u : 0u; h.update(&v, 1); }
        conflict_detail::hashU64Be(h, static_cast<uint64_t>(extraOutOfSpecDrumKitBanksPresent.size()));
        for (const auto& b : extraOutOfSpecDrumKitBanksPresent) conflict_detail::hashStr(h, b);

        conflict_detail::hashU64Be(h, static_cast<uint64_t>(occupiedSlots.size()));
        for (const auto& [addr, e] : occupiedSlots) {
            conflict_detail::hashStr(h, addr.bankName());
            conflict_detail::hashU32Be(h, addr.slot);
            conflict_detail::hashU32Be(h, e.equivalence.kindChunkId);
            conflict_detail::hashStr(h, e.equivalence.bodySha256);
            conflict_detail::hashStr(h, e.equivalence.equivalenceDigest);
        }

        conflict_detail::hashU64Be(h, static_cast<uint64_t>(objectPathsByFolded.size()));
        for (const auto& [folded, vec] : objectPathsByFolded) {
            conflict_detail::hashStr(h, folded);
            conflict_detail::hashU64Be(h, static_cast<uint64_t>(vec.size()));
            for (const auto& o : vec) {
                conflict_detail::hashStr(h, o.relPath);
                conflict_detail::hashU64Be(h, o.sizeBytes);
                conflict_detail::hashStr(h, o.sha256);
            }
        }
        conflict_detail::hashU64Be(h, static_cast<uint64_t>(partialInspect.pcmContainerCount));
        conflict_detail::hashU64Be(h, static_cast<uint64_t>(partialInspect.pcmSampleCount));
        targetLibraryKey = h.hex();
    }
};

// ── Target Inspection (In-Memory + Read-Only Disk Folder) ─────────────────────────────────────────────────────────────

namespace conflict_detail {

inline bool isOc31KmpError(const std::string& err) {
    return err.find("OC31") != std::string::npos || err.find("OC32") != std::string::npos;
}

inline TargetOccupancyMap buildTargetOccupancyFromInMemory(
    const graph::NativeKstInput& in,
    const std::vector<ObjectPathEntry>& objectPaths,
    const std::vector<std::string>& extraBankFilesPresent,
    const std::vector<std::string>& folderErrors,
    const std::vector<std::string>& skippedPaths,
    const TargetInspectOptions& opts) {

    TargetOccupancyMap map;
    map.valid = true;
    map.coverage.deleteProofAvailable = false;

    // 1. Canonical sort inputs for 100% determinism regardless of caller ordering
    graph::NativeKstInput sorted = in;
    std::sort(sorted.pcmContainers.begin(), sorted.pcmContainers.end(),
              [](const graph::RawNamedBlob& a, const graph::RawNamedBlob& b) { return a.label < b.label; });
    std::sort(sorted.soundContainers.begin(), sorted.soundContainers.end(),
              [](const graph::RawNamedBlob& a, const graph::RawNamedBlob& b) {
                  return a.label != b.label ? a.label < b.label : a.bank < b.bank;
              });

    // 2. Register object paths & detect any pre-existing FAT32 R-18 case collisions in target
    for (const auto& obj : objectPaths) {
        map.addObjectPath(obj.relPath, obj.sha256, obj.sizeBytes);
    }
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

    // 3. Mark bank files present strictly by file name (USER01..12, USERDK01..06, optional USERDK07..12)
    auto markBankNamePresent = [&](const std::string& bankUpper) {
        if (auto probe = SlotAddress::fromBankAndSlot(bankUpper, 0); probe.has_value()) {
            if (!probe->isDrumKit && probe->bank >= 1 && probe->bank <= kUserSoundBankCount) {
                map.userSoundBankPresent[probe->bank - 1] = true;
            } else if (probe->isDrumKit && probe->bank >= 1 && probe->bank <= kUserDrumKitBankCount) {
                map.userDrumKitBankPresent[probe->bank - 1] = true;
            } else if (probe->isOutOfSpecDrumKitBank()) {
                map.extraOutOfSpecDrumKitBanksPresent.insert(probe->bankName());
            }
        }
    };
    for (const auto& b : extraBankFilesPresent) markBankNamePresent(b);
    for (const auto& blob : sorted.soundContainers) {
        const std::string bank = blob.bank.empty() ? graph::native_detail::bankFromLabel(blob.label) : blob.bank;
        markBankNamePresent(bank);
    }

    // 4. Parse PCM inventory
    graph::PcmInventory pcm;
    if (sorted.extraPcm) pcm = *sorted.extraPcm;
    std::vector<std::string> containerErrors = folderErrors;
    for (const auto& blob : sorted.pcmContainers) {
        std::string perr;
        if (!graph::addPcmContainerBytes(blob.bytes.data(), blob.bytes.size(), blob.label, pcm, &perr)) {
            containerErrors.push_back(perr);
        }
    }
    map.partialInspect.pcmContainerCount = pcm.containersParsed;
    map.partialInspect.pcmSampleCount = pcm.byId.size() + pcm.samplesWithoutId;

    // 5. Parse & decode Sound containers (getting exact decoded body SHA-256!)
    std::vector<graph::DecodedSound> decodedSounds;
    std::map<SlotAddress, std::string> bodyShaByAddr;
    std::set<std::pair<std::string, uint32_t>> seenSoundKeys;
    std::set<std::pair<std::string, uint32_t>> catalogKeys;
    std::vector<graph::SoundDecodeFailure> decodeFailures;

    for (const auto& blob : sorted.soundContainers) {
        const std::string bank = blob.bank.empty() ? graph::native_detail::bankFromLabel(blob.label) : blob.bank;
        graph::SoundExtractResult ex = graph::extractSoundsFromPcgBytes(
            blob.bytes.data(), blob.bytes.size(), bank, blob.label, opts.downstream.soundMode);
        if (!ex.containerOk) {
            containerErrors.push_back(ex.containerError);
            continue;
        }
        ++map.partialInspect.soundContainerCount;
        for (graph::DecodedSound& s : ex.sounds) {
            if (!seenSoundKeys.emplace(s.bank, s.number).second) {
                containerErrors.push_back(blob.label + ": duplicate Sound identity " + s.bank + "/" + std::to_string(s.number));
                decodeFailures.push_back({blob.label, s.bank, s.number, s.kind, s.name, "duplicate Sound identity across containers"});
                continue;
            }
            if (catalogKeys.emplace(s.bank, s.number).second) {
                map.partialInspect.soundInventory.push_back({s.bank, s.number, s.kind, s.name, blob.label, true});
            }
            if (auto addr = SlotAddress::fromBankAndSlot(s.bank, s.number); addr.has_value()) {
                bodyShaByAddr[*addr] = sha256Bytes(s.body.data(), s.body.size());
                if (addr->isDrumKit) ++map.partialInspect.occupiedUserDrumKits;
                else ++map.partialInspect.occupiedUserSounds;
            }
            decodedSounds.push_back(std::move(s));
        }
        for (const auto& f : ex.failures) {
            decodeFailures.push_back(f);
            if (catalogKeys.emplace(f.bank, f.number).second) {
                map.partialInspect.soundInventory.push_back({f.bank, f.number, f.kind, f.name, f.source, false});
            }
            if (auto addr = SlotAddress::fromBankAndSlot(f.bank, f.number); addr.has_value()) {
                if (addr->isDrumKit) ++map.partialInspect.occupiedUserDrumKits;
                else ++map.partialInspect.occupiedUserSounds;
            }
        }
    }
    std::sort(map.partialInspect.soundInventory.begin(), map.partialInspect.soundInventory.end(),
              [](const graph::SoundSlotMeta& a, const graph::SoundSlotMeta& b) {
                  return a.bank != b.bank ? a.bank < b.bank : a.number < b.number;
              });
    map.coverage.decodeFailures = decodeFailures.size();

    // 6. Parse raw Pa5X RAM.KMP (preserving zone topKey and exact PCM payloadBytes!)
    panative::Pa5xKmp rawKmp;
    graph::KmpIndex kmpIndex;
    graph::KmpBuildDiagnostics kmpDiag;
    bool kmpOk = false;
    if (sorted.kmpContainer.has_value()) {
        map.partialInspect.kmpPresent = true;
        std::string kerr;
        if (!graph::parseKmpContainerBytes(sorted.kmpContainer->bytes.data(),
                                           sorted.kmpContainer->bytes.size(), rawKmp, &kerr)) {
            map.partialInspect.kmpParsed = false;
            map.partialInspect.kmpError = kerr;
            map.partialInspect.kmpIsOc31 = isOc31KmpError(kerr);
        } else if (!graph::kmpIndexFromPa5x(rawKmp, pcm, kmpIndex, &kmpDiag, &kerr)) {
            map.partialInspect.kmpParsed = false;
            map.partialInspect.kmpError = kerr;
        } else {
            map.partialInspect.kmpParsed = true;
            kmpOk = true;
        }
    } else {
        map.partialInspect.kmpPresent = false;
        map.partialInspect.kmpParsed = false;
        map.partialInspect.kmpError = "RAM.KMP is absent";
    }

    // 7. Build Multisample fingerprints from raw Pa5xKmp + PcmInventory when KMP parsed cleanly
    std::vector<MultisampleFingerprint> msFingerprints;
    if (kmpOk) {
        msFingerprints.reserve(rawKmp.multisamples.size());
        for (size_t mi = 0; mi < rawKmp.multisamples.size(); ++mi) {
            const auto& m = rawKmp.multisamples[mi];
            MultisampleFingerprint fp;
            fp.multisampleId = m.id;
            fp.zones.reserve(m.zoneCount);
            for (size_t z = 0; z < m.zoneCount; ++z) {
                const panative::Pa5xZone& pz = rawKmp.zones[static_cast<size_t>(m.firstZone) + z];
                ZoneFingerprint zf;
                zf.topKey = pz.topKey;
                if (pz.sample != panative::kNoIndex && pz.sample < rawKmp.samples.size()) {
                    zf.hasSample = true;
                    zf.sampleId = rawKmp.samples[pz.sample].id;
                    const auto pit = pcm.byId.find(zf.sampleId);
                    if (pit != pcm.byId.end() && pit->second.payloadBytes > 0) {
                        zf.pcmPresent = true;
                        zf.pcmPayloadBytes = pit->second.payloadBytes;
                    }
                }
                fp.zones.push_back(zf);
            }
            msFingerprints.push_back(std::move(fp));
        }
    }

    // 8. Run layout fitting & native graph construction when KMP parsed cleanly
    graph::NativeRefReport refRep;
    std::optional<graph::Graph> nativeGraph;
    if (kmpOk) {
        graph::LayoutTable layouts = opts.downstream.layouts;
        if (opts.downstream.allowFitLayout) {
            std::map<uint32_t, std::vector<const graph::DecodedSound*>> byKind;
            for (const auto& s : decodedSounds) byKind[s.kind].push_back(&s);
            for (const auto& [kind, items] : byKind) {
                if (layouts.count(kind)) continue;
                if (auto fitted = graph::fitLayout(items, kmpIndex, opts.downstream.fitMinItems)) {
                    layouts.emplace(kind, *fitted);
                }
            }
        }
        std::string gerr;
        nativeGraph = graph::buildNativeGraph(kmpIndex, decodedSounds, layouts, refRep, &gerr);
        if (!nativeGraph) {
            containerErrors.push_back("graph build failed: " + gerr);
        }
    }

    // Map unanalysed reasons & unresolved slot counts per Sound ResourceId
    std::map<graph::ResourceId, graph::Gap> unanalysedGapById;
    for (const auto& u : refRep.unanalysed) unanalysedGapById[u.sound] = u.why;

    std::map<graph::ResourceId, size_t> unresolvedCountBySound;
    for (const auto& u : refRep.unresolved) {
        ++unresolvedCountBySound[u.sound];
        if (u.inUnanalysedSound) {
            ++map.coverage.unresolvedInUnanalysedSounds;
        } else {
            ++map.coverage.unresolvedInAnalysedSounds;
            map.targetDiagnostics.push_back({
                DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                diag_code::kUnresolvedSoundSlotAnalysed,
                DiagnosticSeverity::Warning,
                EvidenceLevel::Inferred,
                u.sound.str(),
                "record " + std::to_string(u.record) + " slot " + std::to_string(u.slot),
                "Analysed target Sound contains a non-empty, non-RAM 8-byte multisample slot.",
                "Could be a Factory/ROM multisample ID or dangling reference; Sound is excluded from automatic equivalence reuse.",
                {}
            });
        }
    }
    map.coverage.duplicateDrumIds = kmpDiag.duplicateDrumIds.size();
    for (const auto& dup : kmpDiag.duplicateDrumIds) {
        map.targetDiagnostics.push_back({
            DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
            diag_code::kDuplicateKmpDrumId,
            DiagnosticSeverity::Warning,
            EvidenceLevel::Verified,
            "pa5x:KST/DrumSample/" + (dup.indices.empty() ? std::string("0") : std::to_string(dup.indices[0])),
            graph::native_detail::hexId(dup.id),
            "Target RAM.KMP contains duplicate 8-byte DrumSample ID across " + std::to_string(dup.indices.size()) + " records.",
            "Nodes kept positional; coverageComplete is false; never deduce deletion safety.",
            {}
        });
    }
    for (const auto& mp : kmpDiag.missingPcm) {
        map.targetDiagnostics.push_back({
            DiagnosticCategory::VERIFIED_MISSING,
            diag_code::kMissingPcmForKmpSample,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Verified,
            "pa5x:KST/Sample/" + std::to_string(mp.sampleIndex),
            graph::native_detail::hexId(mp.id),
            "Target RAM.KMP sample lacks matching PCM payload entry: " + mp.reason,
            "Restore missing PCM/*.pcm container before relying on target sample equivalence.",
            {}
        });
    }

    // 9. Populate TargetOccupancyMap::occupiedSlots with full SoundEquivalenceProfile
    for (const auto& sm : map.partialInspect.soundInventory) {
        const auto addrOpt = SlotAddress::fromBankAndSlot(sm.bank, sm.number);
        if (!addrOpt.has_value()) continue;
        const SlotAddress& addr = *addrOpt;

        OccupiedSlotEntry occ;
        occ.address = addr;
        occ.displayName = sm.name;
        occ.sourceContainer = sm.source;

        SoundEquivalenceProfile& eq = occ.equivalence;
        eq.kindChunkId = sm.kind;
        eq.decoded = sm.decoded;
        eq.isDrumKit = addr.isDrumKit;
        eq.kmpComplete = kmpOk && nativeGraph.has_value();

        if (const auto shaIt = bodyShaByAddr.find(addr); shaIt != bodyShaByAddr.end()) {
            eq.bodySha256 = shaIt->second;
        }

        const graph::ResourceId sid = addr.resourceId();
        if (const auto uit = unresolvedCountBySound.find(sid); uit != unresolvedCountBySound.end()) {
            eq.unresolvedSlotCount = uit->second;
        }

        if (!sm.decoded) {
            eq.analysed = false;
            eq.unanalysedReason = "DecodeFailed";
        } else if (!kmpOk || !nativeGraph.has_value()) {
            eq.analysed = false;
            eq.unanalysedReason = map.partialInspect.kmpIsOc31 ? "Oc31KmpUnsupported" : "KmpUnavailable";
            if (addr.isDrumKit) ++map.coverage.unanalysedDrumKits;
        } else if (const auto git = unanalysedGapById.find(sid); git != unanalysedGapById.end()) {
            eq.analysed = false;
            eq.unanalysedReason = graph::gapName(git->second);
            if (addr.isDrumKit) {
                ++map.coverage.unanalysedDrumKits;
            } else if (git->second == graph::Gap::NoKnownReference) {
                ++map.coverage.unanalysedNoKnownRef;
            } else {
                ++map.coverage.unanalysedUnknownKind;
            }
        } else {
            eq.analysed = true;
            ++map.coverage.analysedSounds;
            // Collect referenced Multisample fingerprints via Graph closure
            const auto cl = nativeGraph->closure({sid});
            for (const auto& dep : cl) {
                if (dep.kind == graph::Kind::Multisample && dep.number < msFingerprints.size()) {
                    eq.referencedMultisamples.push_back(msFingerprints[dep.number]);
                }
            }
        }

        finalizeSoundEquivalenceProfile(eq);
        map.occupiedSlots[addr] = std::move(occ);
    }

    // 10. Determine fail-closed inspectability & Gate UX-2 PartialNativeInspect status
    const bool hasAnySounds = !map.partialInspect.soundInventory.empty();
    const bool kmpRequiredAndFailed = map.partialInspect.kmpPresent ? !kmpOk : hasAnySounds;

    if (map.partialInspect.kmpIsOc31) {
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
            "Requires authentic Pa5X raw RAM.KMP (0x11080018 v0x0103) for Multisample/Sample equivalence & closure; Sound bank inventory remains readable.",
            {}
        });
    }

    if (!containerErrors.empty() || !decodeFailures.empty() || kmpRequiredAndFailed) {
        map.fullyInspectable = false;
        map.partialInspect.isPartial = true;
        if (map.partialInspect.kmpIsOc31) {
            map.uninspectableReason = kOc31KmpDegradedNotice;
        } else if (!containerErrors.empty()) {
            map.uninspectableReason = "Target KST container error: " + containerErrors.front();
        } else if (!decodeFailures.empty()) {
            map.uninspectableReason = "Target KST contains " + std::to_string(decodeFailures.size()) + " undecodable Sound item(s).";
        } else {
            map.uninspectableReason = "Target KST RAM.KMP unavailable or invalid: " + map.partialInspect.kmpError;
        }
        if (map.partialInspect.notice.empty()) {
            map.partialInspect.notice = map.uninspectableReason;
        }
        map.targetDiagnostics.push_back({
            DiagnosticCategory::UNSUPPORTED_FORMAT,
            diag_code::kTargetUninspectable,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Unsupported,
            "<target>",
            "",
            map.uninspectableReason,
            "Cannot safely verify target Sound equivalence or slot collisions; failing closed with partial Sound inventory only.",
            {}
        });
    } else {
        map.fullyInspectable = true;
        map.partialInspect.isPartial = false;
    }

    map.coverage.coverageComplete =
        map.fullyInspectable &&
        skippedPaths.empty() &&
        pcm.samplesWithoutId == 0 &&
        kmpDiag.missingPcm.empty() &&
        kmpDiag.duplicateDrumIds.empty() &&
        kmpDiag.unmatchedPcm.empty() &&
        refRep.coverageComplete() &&
        map.coverage.unanalysedDrumKits == 0 &&
        map.coverage.unanalysedNoKnownRef == 0 &&
        map.coverage.unanalysedUnknownKind == 0 &&
        map.coverage.decodeFailures == 0;

    map.rebuildIndexesAndKey();
    return map;
}

}  // namespace conflict_detail

// In-memory read-only target occupancy inspection (100% deterministic across shuffled inputs).
inline TargetOccupancyMap inspectTargetKstInput(const graph::NativeKstInput& in,
                                                const std::vector<ObjectPathEntry>& objectPaths = {},
                                                const TargetInspectOptions& opts = {}) {
    return conflict_detail::buildTargetOccupancyFromInMemory(in, objectPaths, {}, {}, {}, opts);
}

// Builds a TargetOccupancyMap directly from an already-computed M1 NativeKstClosureResult.
// Because M1's manifest does not retain raw decoded Sound body bytes or zone topKeys, occupied Sounds
// are marked `InsufficientEvidence` for equivalence (fail-closed) while bank/slot occupancy, object paths,
// and partial OC31 KMP status are populated accurately. Use `inspectTargetKstFolder()` or
// `inspectTargetKstInput()` when full Sound Equivalence Fingerprinting is required.
inline TargetOccupancyMap buildTargetOccupancyFromM1Closure(const graph::NativeKstClosureResult& closure) {
    TargetOccupancyMap map;
    map.valid = closure.ok;
    map.coverage.deleteProofAvailable = false;

    const auto& M = closure.manifest;
    map.partialInspect.kmpPresent = M.coverage.downstreamKmpPresent;
    map.partialInspect.kmpParsed = M.coverage.downstreamKmpParsed;
    map.partialInspect.kmpError = M.coverage.kmpError;
    map.partialInspect.kmpIsOc31 = conflict_detail::isOc31KmpError(M.coverage.kmpError);
    map.partialInspect.soundContainerCount = M.summary.soundContainers;
    map.partialInspect.pcmContainerCount = M.summary.pcmContainers;
    map.partialInspect.pcmSampleCount = M.summary.pcmSamplesTotal;
    map.partialInspect.soundInventory = closure.downstream.report.soundCatalog;

    for (const auto& sm : closure.downstream.report.soundCatalog) {
        const auto addrOpt = SlotAddress::fromBankAndSlot(sm.bank, sm.number);
        if (!addrOpt.has_value()) continue;
        OccupiedSlotEntry occ;
        occ.address = *addrOpt;
        occ.displayName = sm.name;
        occ.sourceContainer = sm.source;
        occ.equivalence.kindChunkId = sm.kind;
        occ.equivalence.decoded = sm.decoded;
        occ.equivalence.isDrumKit = addrOpt->isDrumKit;
        occ.equivalence.analysed = false;  // M1 manifest does not carry decoded body SHA-256 / zone topKeys
        occ.equivalence.unanalysedReason = "BodyShaAndZoneTableNotInM1Summary";
        map.addOccupiedSlot(std::move(occ));
        if (addrOpt->isDrumKit) ++map.partialInspect.occupiedUserDrumKits;
        else ++map.partialInspect.occupiedUserSounds;
    }

    for (const auto& r : M.roots) {
        map.addObjectPath(r.relPath, "", 0);
    }

    map.coverage.analysedSounds = M.summary.soundsAnalysed;
    map.coverage.unresolvedInAnalysedSounds = M.summary.unresolvedSoundSlotsInAnalysed;
    map.coverage.unresolvedInUnanalysedSounds = M.summary.unresolvedSoundSlotsInUnanalysed;
    map.coverage.duplicateDrumIds = M.summary.duplicateDrumIdGroups;
    map.coverage.decodeFailures = closure.downstream.report.decodeFailures.size();
    map.coverage.coverageComplete = M.coverage.coverageComplete;

    const bool hasSounds = !closure.downstream.report.soundCatalog.empty();
    const bool kmpFailed = M.coverage.downstreamKmpPresent ? !M.coverage.downstreamKmpParsed : hasSounds;
    if (map.partialInspect.kmpIsOc31) {
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
            "Requires authentic Pa5X raw RAM.KMP (0x11080018 v0x0103) for sample-level closure.",
            {}
        });
    }
    if (!closure.ok || !M.containerErrors.empty() || !closure.downstream.report.decodeFailures.empty() || kmpFailed) {
        map.fullyInspectable = false;
        map.partialInspect.isPartial = true;
        map.uninspectableReason = map.partialInspect.kmpIsOc31
            ? std::string(kOc31KmpDegradedNotice)
            : (!M.containerErrors.empty() ? M.containerErrors.front() : M.coverage.kmpError);
        map.targetDiagnostics.push_back({
            DiagnosticCategory::UNSUPPORTED_FORMAT,
            diag_code::kTargetUninspectable,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Unsupported,
            "<target>",
            "",
            map.uninspectableReason,
            "Target KST could not be fully inspected; failing closed.",
            {}
        });
    } else {
        map.fullyInspectable = true;
    }

    map.rebuildIndexesAndKey();
    return map;
}

// Read-only disk folder target KST inspector.
// Enforces O_RDONLY | O_NOFOLLOW | O_NONBLOCK, S_ISREG/S_ISDIR, trailing-slash stripping, F_DUPFD_CLOEXEC,
// pre-allocated byte/file/entry budgets, metadata-only pread() on PCM/*.pcm, streaming SHA-256 on object files,
// and cooperative keepGoing cancellation every <= 256 files. Never leaks local filesystem paths into diagnostics.
inline TargetOccupancyMap inspectTargetKstFolder(const std::filesystem::path& kstDir,
                                                 const TargetInspectOptions& opts = {}) {
    TargetOccupancyMap earlyFail;
    earlyFail.coverage.deleteProofAvailable = false;

    const std::string rootPath = graph::native_detail::stripTrailingSlashes(kstDir);
    const graph::native_detail::ScopedFd rootFd(
        ::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (rootFd.fd < 0) {
        earlyFail.valid = false;
        earlyFail.fullyInspectable = false;
        earlyFail.partialInspect.isPartial = true;
        earlyFail.uninspectableReason =
            "cannot open target KST folder <target> (must be a real directory, symlinks are not followed)";
        earlyFail.partialInspect.notice = earlyFail.uninspectableReason;
        earlyFail.targetDiagnostics.push_back({
            DiagnosticCategory::UNSUPPORTED_FORMAT,
            diag_code::kTargetUninspectable,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Unsupported,
            "<target>",
            "",
            earlyFail.uninspectableReason,
            "Verify target path is an existing, readable local KST directory (not a symlink).",
            {}
        });
        earlyFail.rebuildIndexesAndKey();
        return earlyFail;
    }

    size_t remainingEntries = opts.maxDirEntries;
    std::vector<std::string> topEntries;
    std::string lerr;
    if (!graph::native_detail::listDirEntriesAt(rootFd.fd, topEntries, remainingEntries, lerr)) {
        earlyFail.valid = false;
        earlyFail.fullyInspectable = false;
        earlyFail.partialInspect.isPartial = true;
        earlyFail.uninspectableReason = "cannot list target KST folder <target>: " + lerr;
        earlyFail.partialInspect.notice = earlyFail.uninspectableReason;
        earlyFail.targetDiagnostics.push_back({
            DiagnosticCategory::UNSUPPORTED_FORMAT,
            diag_code::kTargetUninspectable,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Unsupported,
            "<target>",
            "",
            earlyFail.uninspectableReason,
            "Check target directory permissions and entry count limits.",
            {}
        });
        earlyFail.rebuildIndexesAndKey();
        return earlyFail;
    }
    remainingEntries -= topEntries.size();

    graph::NativeKstInput in;
    graph::PcmInventory folderPcm;
    std::vector<graph::RawNamedBlob> kmpBlobs;
    std::vector<ObjectPathEntry> objectPaths;
    std::vector<std::string> bankFilesPresent;
    std::set<std::string> seenSoundBanks;
    std::vector<std::string> folderErrors, skipped;

    size_t remainingDownBudget = opts.downstream.maxTotalBytes;
    size_t bytesDown = 0;
    size_t remainingObjBudget = opts.maxObjectTotalBytes;
    size_t bytesObj = 0;
    size_t scannedItems = 0;
    bool hitObjCap = false;

    auto checkCancel = [&]() -> bool {
        const bool due = (scannedItems & 0xFFu) == 0;
        ++scannedItems;
        return due && opts.keepGoing && !opts.keepGoing();
    };

    auto makeCancelled = [&]() {
        TargetOccupancyMap c;
        c.valid = false;
        c.fullyInspectable = false;
        c.cancelled = true;
        c.uninspectableReason = "target KST inspection cancelled";
        c.coverage.deleteProofAvailable = false;
        c.rebuildIndexesAndKey();
        return c;
    };

    for (const std::string& top : topEntries) {
        if (checkCancel()) return makeCancelled();

        struct stat st{};
        if (::fstatat(rootFd.fd, top.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            folderErrors.push_back("cannot stat entry " + top);
            continue;
        }
        const std::string topUpper = graph::native_detail::upperAscii(top);
        const bool isDownDir = (topUpper == "MULTISMP" || topUpper == "PCM" || topUpper == "SOUND");
        const bool isBankedObjDir = conflict_detail::isBankedObjectRootDir(topUpper);
        const bool isSettingsDir = (topUpper == "SETTINGS");

        if (!isDownDir && !isBankedObjDir && !isSettingsDir) continue;
        if (!S_ISDIR(st.st_mode)) {
            skipped.push_back(top);
            folderErrors.push_back("refusing non-directory/symlink at " + top);
            continue;
        }

        const graph::native_detail::ScopedFd subFd(
            ::openat(rootFd.fd, top.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (subFd.fd < 0) {
            folderErrors.push_back("cannot open folder " + top + " (symlinks are not followed)");
            continue;
        }
        std::vector<std::string> level1;
        std::string serr;
        if (!graph::native_detail::listDirEntriesAt(subFd.fd, level1, remainingEntries, serr)) {
            folderErrors.push_back("cannot list folder " + top + ": " + serr);
            continue;
        }
        remainingEntries -= level1.size();

        if (isDownDir) {
            for (const std::string& leaf : level1) {
                if (checkCancel()) return makeCancelled();
                const std::string rel = top + "/" + leaf;
                struct stat lst{};
                if (::fstatat(subFd.fd, leaf.c_str(), &lst, AT_SYMLINK_NOFOLLOW) != 0) {
                    folderErrors.push_back("cannot stat " + rel);
                    continue;
                }
                if (!S_ISREG(lst.st_mode)) {
                    skipped.push_back(rel);
                    if (!S_ISDIR(lst.st_mode)) folderErrors.push_back("refusing non-regular file or symlink: " + rel);
                    continue;
                }
                const bool isKmp = topUpper == "MULTISMP" && graph::native_detail::endsWithUpper(leaf, ".KMP");
                const bool isPcm = topUpper == "PCM" && graph::native_detail::endsWithUpper(leaf, ".PCM");
                const bool isPcg = topUpper == "SOUND" && graph::native_detail::endsWithUpper(leaf, ".PCG");
                if (!isKmp && !isPcm && !isPcg) continue;

                if (isPcm) {
                    std::string perr;
                    if (!graph::addPcmContainerFdAt(subFd.fd, leaf.c_str(), rel, panative::kMaxFileBytes,
                                                    remainingDownBudget, bytesDown, folderPcm, &perr)) {
                        folderErrors.push_back(perr);
                    }
                    continue;
                }

                std::vector<uint8_t> bytes;
                std::string rerr;
                if (!graph::native_detail::readRegularAt(subFd.fd, leaf.c_str(), panative::kMaxFileBytes,
                                                         remainingDownBudget, bytes, rerr)) {
                    folderErrors.push_back(rel + ": " + rerr);
                    continue;
                }
                remainingDownBudget -= bytes.size();
                bytesDown += bytes.size();
                if (isKmp) {
                    kmpBlobs.push_back({rel, "", std::move(bytes)});
                } else {
                    const std::string bank = graph::native_detail::bankFromLabel(rel);
                    bankFilesPresent.push_back(bank);
                    if (!seenSoundBanks.insert(bank).second) {
                        folderErrors.push_back("duplicate Sound bank '" + bank + "' (" + rel + "); refusing to guess");
                        continue;
                    }
                    in.soundContainers.push_back({rel, bank, std::move(bytes)});
                }
            }
            continue;
        }

        // Object path registry: STYLE/<bank>/*.stg, KEYBOARDSET/<bank>/*.ksc, PAD/<bank>/*.pdc, SETTINGS/*.ksc
        auto recordObjectFile = [&](int parentFd, const std::string& leafName, const std::string& rel) {
            if (hitObjCap) return;
            if (objectPaths.size() >= opts.maxObjectFiles) {
                hitObjCap = true;
                folderErrors.push_back("target object count exceeds maximum limit (" + std::to_string(opts.maxObjectFiles) + ")");
                return;
            }
            uint64_t fsz = 0;
            std::string sha;
            if (opts.hashObjectFiles) {
                std::string herr;
                if (!conflict_detail::hashRegularFileAt(parentFd, leafName.c_str(), opts.maxObjectFileBytes,
                                                        remainingObjBudget, bytesObj, fsz, sha, herr)) {
                    folderErrors.push_back(rel + ": " + herr);
                    return;
                }
            } else {
                struct stat fst{};
                if (::fstatat(parentFd, leafName.c_str(), &fst, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(fst.st_mode)) {
                    folderErrors.push_back(rel + ": not a regular file");
                    return;
                }
                fsz = fst.st_size > 0 ? static_cast<uint64_t>(fst.st_size) : 0;
            }
            objectPaths.push_back({rel, conflict_detail::asciiLower(rel), sha, fsz});
        };

        for (const std::string& e1 : level1) {
            if (checkCancel()) return makeCancelled();
            const std::string rel1 = top + "/" + e1;
            struct stat st1{};
            if (::fstatat(subFd.fd, e1.c_str(), &st1, AT_SYMLINK_NOFOLLOW) != 0) {
                folderErrors.push_back("cannot stat " + rel1);
                continue;
            }
            if (S_ISREG(st1.st_mode)) {
                if (conflict_detail::isTrackedObjectExt(e1)) {
                    recordObjectFile(subFd.fd, e1, rel1);
                }
                continue;
            }
            if (!S_ISDIR(st1.st_mode)) {
                skipped.push_back(rel1);
                folderErrors.push_back("refusing non-regular file or symlink: " + rel1);
                continue;
            }
            if (isSettingsDir) continue;

            const graph::native_detail::ScopedFd bankFd(
                ::openat(subFd.fd, e1.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
            if (bankFd.fd < 0) {
                folderErrors.push_back("cannot open bank folder " + rel1 + " (symlinks are not followed)");
                continue;
            }
            std::vector<std::string> level2;
            std::string berr;
            if (!graph::native_detail::listDirEntriesAt(bankFd.fd, level2, remainingEntries, berr)) {
                folderErrors.push_back("cannot list bank folder " + rel1 + ": " + berr);
                continue;
            }
            remainingEntries -= level2.size();

            for (const std::string& e2 : level2) {
                if (checkCancel()) return makeCancelled();
                const std::string rel2 = rel1 + "/" + e2;
                struct stat st2{};
                if (::fstatat(bankFd.fd, e2.c_str(), &st2, AT_SYMLINK_NOFOLLOW) != 0) {
                    folderErrors.push_back("cannot stat " + rel2);
                    continue;
                }
                if (!S_ISREG(st2.st_mode)) {
                    skipped.push_back(rel2);
                    if (!S_ISDIR(st2.st_mode)) folderErrors.push_back("refusing non-regular file or symlink: " + rel2);
                    continue;
                }
                if (conflict_detail::isTrackedObjectExt(e2)) {
                    recordObjectFile(bankFd.fd, e2, rel2);
                }
            }
        }
    }

    in.extraPcm = std::move(folderPcm);
    if (kmpBlobs.size() == 1) {
        in.kmpContainer = std::move(kmpBlobs[0]);
    } else if (kmpBlobs.size() > 1) {
        folderErrors.push_back("multiple .kmp files in MULTISMP/ (" + std::to_string(kmpBlobs.size()) + "); refusing to guess");
    }

    TargetOccupancyMap map = conflict_detail::buildTargetOccupancyFromInMemory(
        in, objectPaths, bankFilesPresent, folderErrors, skipped, opts);
    map.bytesReadDownstream = bytesDown;
    map.bytesReadObjects = bytesObj;
    return map;
}

// ── Deterministic Canonical Free-Slot Allocator (§4.3) ────────────────────────────────────────────────────────────────

struct SlotAllocationRequest {
    SlotAddress preferred;
    std::string canonicalName;
    std::string contentSha256;
    std::string requestId;
};

struct SlotAllocationOutcome {
    std::string requestId;
    SlotAddress requested;
    std::optional<SlotAddress> assigned;  // std::nullopt iff BANK_GRID_EXHAUSTED
    bool preservedSlot = false;
    bool exhausted = false;
};

class CanonicalSlotAllocator {
public:
    explicit CanonicalSlotAllocator(const TargetOccupancyMap& target) {
        for (const auto& [addr, entry] : target.occupiedSlots) {
            if (addr.isInSpec()) reserve(addr);
        }
    }

    bool isReserved(const SlotAddress& addr) const {
        if (!addr.isInSpec()) return true;
        if (!addr.isDrumKit) return soundUsed_[addr.bank - 1][addr.slot];
        return dkUsed_[addr.bank - 1][addr.slot];
    }

    bool reserve(const SlotAddress& addr) {
        if (!addr.isInSpec()) return false;
        bool& cell = addr.isDrumKit ? dkUsed_[addr.bank - 1][addr.slot] : soundUsed_[addr.bank - 1][addr.slot];
        if (cell) return false;
        cell = true;
        return true;
    }

    size_t freeSlotsRemaining(bool isDrumKit) const {
        size_t freeCnt = 0;
        const uint32_t maxBank = isDrumKit ? kUserDrumKitBankCount : kUserSoundBankCount;
        for (uint32_t b = 0; b < maxBank; ++b) {
            for (uint32_t s = 0; s < kSlotsPerBank; ++s) {
                const bool used = isDrumKit ? dkUsed_[b][s] : soundUsed_[b][s];
                if (!used) ++freeCnt;
            }
        }
        return freeCnt;
    }

    // Allocates one slot: Step A (preserve preferred if in-spec & free) ->
    // Step B (lowest free in same bank) -> Step C (lowest free in same class) -> Step D (exhausted).
    std::optional<SlotAddress> allocateOne(const SlotAddress& preferred, bool* preservedOut = nullptr) {
        if (preservedOut) *preservedOut = false;
        const uint32_t maxBank = preferred.isDrumKit ? kUserDrumKitBankCount : kUserSoundBankCount;

        // Step A: Preserve preferred slot if in-spec & free
        if (preferred.isInSpec() && !isReserved(preferred)) {
            reserve(preferred);
            if (preservedOut) *preservedOut = true;
            return preferred;
        }
        // Step B: Lowest free slot in the same bank (if preferred.bank is in-spec)
        if (preferred.bank >= 1 && preferred.bank <= maxBank) {
            for (uint32_t s = 0; s < kSlotsPerBank; ++s) {
                const SlotAddress cand{preferred.isDrumKit, preferred.bank, s};
                if (!isReserved(cand)) {
                    reserve(cand);
                    return cand;
                }
            }
        }
        // Step C: Lowest free slot across remaining banks in the same class
        for (uint32_t b = 1; b <= maxBank; ++b) {
            if (b == preferred.bank) continue;
            for (uint32_t s = 0; s < kSlotsPerBank; ++s) {
                const SlotAddress cand{preferred.isDrumKit, b, s};
                if (!isReserved(cand)) {
                    reserve(cand);
                    return cand;
                }
            }
        }
        // Step D: Exhausted (all 1,536 User Sound or 768 User Drum Kit slots full)
        return std::nullopt;
    }

    // Sorts batch canonically by (isDrumKit, bank, slot, canonicalName, contentSha256, requestId)
    // before allocating, guaranteeing byte-identical assignments regardless of input order.
    std::vector<SlotAllocationOutcome> allocateBatch(std::vector<SlotAllocationRequest> requests) {
        std::sort(requests.begin(), requests.end(), [](const SlotAllocationRequest& a, const SlotAllocationRequest& b) {
            return std::tie(a.preferred.isDrumKit, a.preferred.bank, a.preferred.slot,
                            a.canonicalName, a.contentSha256, a.requestId) <
                   std::tie(b.preferred.isDrumKit, b.preferred.bank, b.preferred.slot,
                            b.canonicalName, b.contentSha256, b.requestId);
        });
        std::vector<SlotAllocationOutcome> out;
        out.reserve(requests.size());
        for (const auto& req : requests) {
            SlotAllocationOutcome res;
            res.requestId = req.requestId;
            res.requested = req.preferred;
            bool preserved = false;
            res.assigned = allocateOne(req.preferred, &preserved);
            res.preservedSlot = preserved;
            res.exhausted = !res.assigned.has_value();
            out.push_back(std::move(res));
        }
        return out;
    }

private:
    std::array<std::array<bool, kSlotsPerBank>, kUserSoundBankCount> soundUsed_{};
    std::array<std::array<bool, kSlotsPerBank>, kUserDrumKitBankCount> dkUsed_{};
};

// ── Complete Collision Matrix (§5.2) ──────────────────────────────────────────────────────────────────────────────────

struct SoundCollisionRequest {
    std::string requestId;               // Canonical ID, e.g. "pa5x:USER03/Sound/12"
    SlotAddress sourceAddress;           // (isDrumKit, bank, slot)
    std::string sourceLabel;             // Human label for reporting ONLY (never equivalence!)
    SoundEquivalenceProfile equivalence;
    bool expectExistingInTarget = false; // true if Style expects Sound to already exist at this slot in target
    std::vector<std::string> affectedRoots;
};

struct ObjectPathCollisionRequest {
    SelectedKind kind = SelectedKind::Style;
    std::string sourceRelPath;           // Source-relative path, e.g. "STYLE/BankA/Demo.stg"
    std::string targetBank;              // Optional destination bank override
    std::string targetName;              // Optional destination leaf name override
    std::string fileSha256;              // 64-hex SHA-256 of source file bytes
    std::vector<std::string> affectedRoots;
};

struct CollisionMatrixInput {
    ConflictPolicy policy = ConflictPolicy::BlockWithReport;
    ExportMode mode = ExportMode::AdvisoryPlan;
    std::vector<SoundCollisionRequest> soundRequests;
    std::vector<ObjectPathCollisionRequest> objectRequests;
};

struct SoundResolutionEntry {
    std::string requestId;
    SlotAddress sourceAddress;
    std::optional<SlotAddress> resolvedAddress;
    bool remapped = false;
    std::string action;                  // "allow-free" | "reuse-target" | "remap-reuse-target" | "remap-free-slot" | "blocked"
    EquivalenceVerdict equivalence = EquivalenceVerdict::InsufficientEvidence;
    bool blocking = false;
    std::string detail;

    auto operator<=>(const SoundResolutionEntry&) const = default;
};

struct ObjectPathResolutionEntry {
    SelectedKind kind = SelectedKind::Style;
    std::string sourceRelPath;
    std::string targetRelPath;
    std::string foldedTargetRelPath;
    std::string action;                  // "allow-new" | "already-present-noop" | "blocked-collision" | "blocked-fat-case-r18"
    bool blocking = false;
    std::string detail;

    auto operator<=>(const ObjectPathResolutionEntry&) const = default;
};

struct CollisionMatrixResult {
    bool targetInspectable = false;
    ConflictPolicy policy = ConflictPolicy::BlockWithReport;
    std::vector<SoundResolutionEntry> soundResolutions;
    std::vector<ObjectPathResolutionEntry> objectResolutions;
    std::vector<SlotConflict> conflicts;
    std::vector<Diagnostic> diagnostics;

    bool hasBlockingConflicts() const {
        for (const auto& c : conflicts) if (c.blocking) return true;
        for (const auto& d : diagnostics) if (d.severity == DiagnosticSeverity::Blocking) return true;
        for (const auto& s : soundResolutions) if (s.blocking) return true;
        for (const auto& o : objectResolutions) if (o.blocking) return true;
        return false;
    }

    bool anyRemappedOrRenamed() const {
        for (const auto& s : soundResolutions) if (s.remapped) return true;
        for (const auto& o : objectResolutions) {
            if (conflict_detail::normalizeRelPath(o.sourceRelPath) != o.targetRelPath) return true;
        }
        return false;
    }
};

// Evaluates the full §5.2 Collision Matrix deterministically across all 3 ConflictPolicies.
// Invariant: NO policy ever silently overwrites an occupied slot or colliding object path.
inline CollisionMatrixResult evaluateCollisionMatrix(const TargetOccupancyMap& target,
                                                     CollisionMatrixInput input) {
    CollisionMatrixResult result;
    result.targetInspectable = target.valid && target.fullyInspectable;
    result.policy = input.policy;

    // Canonicalize sound & object inputs before processing (100% order-independent determinism)
    for (auto& sr : input.soundRequests) {
        finalizeSoundEquivalenceProfile(sr.equivalence);
        std::sort(sr.affectedRoots.begin(), sr.affectedRoots.end());
        sr.affectedRoots.erase(std::unique(sr.affectedRoots.begin(), sr.affectedRoots.end()), sr.affectedRoots.end());
    }
    std::sort(input.soundRequests.begin(), input.soundRequests.end(),
              [](const SoundCollisionRequest& a, const SoundCollisionRequest& b) {
                  return std::tie(a.sourceAddress.isDrumKit, a.sourceAddress.bank, a.sourceAddress.slot,
                                  a.sourceLabel, a.equivalence.bodySha256, a.requestId) <
                         std::tie(b.sourceAddress.isDrumKit, b.sourceAddress.bank, b.sourceAddress.slot,
                                  b.sourceLabel, b.equivalence.bodySha256, b.requestId);
              });

    for (auto& obj : input.objectRequests) {
        obj.sourceRelPath = conflict_detail::normalizeRelPath(obj.sourceRelPath);
        std::sort(obj.affectedRoots.begin(), obj.affectedRoots.end());
        obj.affectedRoots.erase(std::unique(obj.affectedRoots.begin(), obj.affectedRoots.end()), obj.affectedRoots.end());
    }
    std::sort(input.objectRequests.begin(), input.objectRequests.end(),
              [](const ObjectPathCollisionRequest& a, const ObjectPathCollisionRequest& b) {
                  const std::string ta = conflict_detail::applyTargetPathOverride(a.sourceRelPath, a.targetBank, a.targetName);
                  const std::string tb = conflict_detail::applyTargetPathOverride(b.sourceRelPath, b.targetBank, b.targetName);
                  const std::string fa = conflict_detail::asciiLower(ta);
                  const std::string fb = conflict_detail::asciiLower(tb);
                  return std::tie(fa, ta, a.sourceRelPath, a.fileSha256) <
                         std::tie(fb, tb, b.sourceRelPath, b.fileSha256);
              });

    // Carry forward target-level diagnostics
    result.diagnostics.insert(result.diagnostics.end(),
                              target.targetDiagnostics.begin(), target.targetDiagnostics.end());

    // Fail-closed gate: TargetUninspectable blocks ALL requests under ALL 3 policies
    if (!result.targetInspectable) {
        const std::string reason = target.uninspectableReason.empty()
            ? std::string("Target KST is uninspectable; refusing fail-closed.")
            : target.uninspectableReason;
        result.conflicts.push_back({
            SlotConflict::Kind::TargetUninspectable,
            "<target>",
            "",
            "",
            "BLOCKED",
            true,
            reason
        });
        bool hasUninspectDiag = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == diag_code::kTargetUninspectable) { hasUninspectDiag = true; break; }
        }
        if (!hasUninspectDiag) {
            result.diagnostics.push_back({
                DiagnosticCategory::UNSUPPORTED_FORMAT,
                diag_code::kTargetUninspectable,
                DiagnosticSeverity::Blocking,
                EvidenceLevel::Unsupported,
                "<target>",
                "",
                reason,
                "Target KST could not be fully inspected (OC31 RAM.KMP, corrupt container, or unreadable path); refusing fail-closed.",
                {}
            });
        }
        for (const auto& sr : input.soundRequests) {
            result.soundResolutions.push_back({
                sr.requestId, sr.sourceAddress, std::nullopt, false, "blocked",
                EquivalenceVerdict::InsufficientEvidence, true, reason
            });
        }
        for (const auto& obj : input.objectRequests) {
            const std::string tp = conflict_detail::applyTargetPathOverride(obj.sourceRelPath, obj.targetBank, obj.targetName);
            result.objectResolutions.push_back({
                obj.kind, obj.sourceRelPath, tp, conflict_detail::asciiLower(tp),
                "blocked", true, reason
            });
        }
        canonicalizeConflicts(result.conflicts);
        canonicalizeDiagnostics(result.diagnostics);
        return result;
    }

    // Helper: find lowest-address proven-identical Sound of the same class anywhere in target
    auto findIdenticalInTarget = [&](const SoundCollisionRequest& req,
                                     std::optional<SlotAddress> excludeAddr) -> std::optional<SlotAddress> {
        if (!req.equivalence.provablyComparable() || req.equivalence.equivalenceDigest.empty()) {
            return std::nullopt;
        }
        const auto it = target.equivalenceIndex.find(req.equivalence.equivalenceDigest);
        if (it == target.equivalenceIndex.end()) return std::nullopt;
        for (const SlotAddress& cand : it->second) {
            if (cand.isDrumKit != req.sourceAddress.isDrumKit) continue;
            if (excludeAddr.has_value() && cand == *excludeAddr) continue;
            const OccupiedSlotEntry* occ = target.findOccupied(cand);
            if (!occ) continue;
            if (compareSoundEquivalence(req.equivalence, occ->equivalence).verdict == EquivalenceVerdict::Identical) {
                return cand;
            }
        }
        return std::nullopt;
    };

    // Phase 1: Preserve free slots, detect exact matches, detect conflicts, and queue remaps
    CanonicalSlotAllocator allocator(target);
    result.soundResolutions.resize(input.soundRequests.size());
    std::vector<size_t> remapQueueIndices;

    for (size_t i = 0; i < input.soundRequests.size(); ++i) {
        const auto& req = input.soundRequests[i];
        auto& outRes = result.soundResolutions[i];
        outRes.requestId = req.requestId;
        outRes.sourceAddress = req.sourceAddress;

        const std::string addrStr = req.sourceAddress.label();
        const std::string subj = req.requestId.empty() ? req.sourceAddress.resourceId().str() : req.requestId;

        // Out-of-spec User Drum Kit bank check (MSB 120 LSB 70..75 -> USERDK07..USERDK12)
        if (req.sourceAddress.isOutOfSpecDrumKitBank()) {
            const bool bankPresent = target.isBankFilePresent(true, req.sourceAddress.bank);
            result.diagnostics.push_back({
                DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                diag_code::kOutOfSpecDrumkitBank,
                DiagnosticSeverity::Warning,
                EvidenceLevel::Unsupported,
                subj,
                addrStr,
                "MIDI address selects out-of-spec User Drum Kit bank " + req.sourceAddress.bankName() +
                    " (MSB 120 LSB " + std::to_string(req.sourceAddress.lsb()) + ", outside documented USERDK01..USERDK06).",
                "Verify target Drum Kit bank layout or remap track to USERDK01..USERDK06.",
                req.affectedRoots
            });
            if (!bankPresent) {
                result.conflicts.push_back({
                    SlotConflict::Kind::TargetBankAbsent,
                    addrStr,
                    req.sourceLabel,
                    "(bank file absent)",
                    "BLOCKED",
                    true,
                    "Out-of-spec Drum Kit bank file " + req.sourceAddress.bankName() + ".PCG is absent in target."
                });
                result.diagnostics.push_back({
                    DiagnosticCategory::VERIFIED_MISSING,
                    diag_code::kMissingUserSoundBank,
                    DiagnosticSeverity::Blocking,
                    EvidenceLevel::Verified,
                    subj,
                    req.sourceAddress.bankName() + ".PCG",
                    "Target Drum Kit bank file " + req.sourceAddress.bankName() + ".PCG is absent (MSB 120 LSB " +
                        std::to_string(req.sourceAddress.lsb()) + " is out-of-spec).",
                    "Remap track to an occupied Drum Kit in USERDK01..USERDK06.",
                    req.affectedRoots
                });
                outRes.action = "blocked";
                outRes.blocking = true;
                outRes.detail = "out-of-spec Drum Kit bank absent in target";
                continue;
            }
        }

        const SlotState st = target.slotState(req.sourceAddress);

        if (st == SlotState::BankFileAbsent || st == SlotState::Free) {
            if (st == SlotState::BankFileAbsent) {
                if (req.expectExistingInTarget) {
                    result.conflicts.push_back({
                        SlotConflict::Kind::TargetBankAbsent,
                        addrStr,
                        req.sourceLabel,
                        "(bank file absent)",
                        "BLOCKED",
                        true,
                        "Expected target Sound bank file " + req.sourceAddress.bankName() + ".PCG is absent."
                    });
                    result.diagnostics.push_back({
                        DiagnosticCategory::VERIFIED_MISSING,
                        diag_code::kMissingUserSoundBank,
                        DiagnosticSeverity::Blocking,
                        EvidenceLevel::Verified,
                        subj,
                        req.sourceAddress.bankName() + ".PCG",
                        "Target bank file SOUND/" + req.sourceAddress.bankName() + ".PCG is absent.",
                        "Supply " + req.sourceAddress.bankName() + ".PCG from backup or remap track to an existing bank.",
                        req.affectedRoots
                    });
                    outRes.action = "blocked";
                    outRes.blocking = true;
                    outRes.detail = "target bank file absent";
                    continue;
                }
                // Free for new allocation, but record non-blocking TargetBankAbsent notice
                result.conflicts.push_back({
                    SlotConflict::Kind::TargetBankAbsent,
                    addrStr,
                    req.sourceLabel,
                    "(bank file absent; slot free for new import)",
                    "ALLOW_NEW_BANK",
                    false,
                    "SOUND/" + req.sourceAddress.bankName() + ".PCG is absent in target; slot is free for new import."
                });
            }

            if (req.expectExistingInTarget && st == SlotState::Free) {
                result.diagnostics.push_back({
                    DiagnosticCategory::VERIFIED_MISSING,
                    diag_code::kMissingUserSoundSlot,
                    DiagnosticSeverity::Blocking,
                    EvidenceLevel::Verified,
                    subj,
                    addrStr,
                    "Expected target Sound slot " + req.sourceAddress.shortLabel() + " is empty.",
                    "Remap track to an occupied User/Factory Sound or supply missing Sound via Import Kit.",
                    req.affectedRoots
                });
                outRes.action = "blocked";
                outRes.blocking = true;
                outRes.detail = "expected target slot is empty";
                continue;
            }

            // Try to reserve the preferred slot in the allocator (prevents two source requests claiming the same slot)
            if (req.sourceAddress.isInSpec() && allocator.reserve(req.sourceAddress)) {
                outRes.resolvedAddress = req.sourceAddress;
                outRes.remapped = false;
                outRes.action = "allow-free";
                outRes.blocking = false;
                outRes.detail = (st == SlotState::BankFileAbsent)
                    ? "target bank absent; slot reserved in new output grid"
                    : "target slot is free; preserved";
            } else if (input.policy == ConflictPolicy::RemapToFreeSlotInNewLib) {
                remapQueueIndices.push_back(i);
            } else {
                result.conflicts.push_back({
                    SlotConflict::Kind::SoundSlotOccupiedDifferent,
                    addrStr,
                    req.sourceLabel,
                    "(reserved by another request in batch)",
                    "BLOCKED",
                    true,
                    "Multiple source requests target the same slot " + req.sourceAddress.shortLabel() + "."
                });
                result.diagnostics.push_back({
                    DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                    diag_code::kSoundSlotOccupiedDifferent,
                    DiagnosticSeverity::Blocking,
                    EvidenceLevel::Verified,
                    subj,
                    addrStr,
                    "Multiple requests target slot " + req.sourceAddress.shortLabel() + ".",
                    "Use RemapToFreeSlotInNewLib or resolve duplicate target slot assignment.",
                    req.affectedRoots
                });
                outRes.action = "blocked";
                outRes.blocking = true;
                outRes.detail = "slot already reserved by another request in batch";
            }
            continue;
        }

        // Target slot IS OCCUPIED
        const OccupiedSlotEntry* targetEntry = target.findOccupied(req.sourceAddress);
        const EquivalenceResult eq = compareSoundEquivalence(req.equivalence, targetEntry->equivalence);
        outRes.equivalence = eq.verdict;

        if (eq.verdict == EquivalenceVerdict::Identical) {
            // Proven equivalent at exact same slot -> ALLOW reuse under ALL 3 policies (0 bytes copied, 0 remap)
            outRes.resolvedAddress = req.sourceAddress;
            outRes.remapped = false;
            outRes.action = "reuse-target";
            outRes.blocking = false;
            outRes.detail = "target slot holds proven-identical Sound; reusing existing slot";
            continue;
        }

        // Occupied by different or unprovable Sound: check if proven-identical exists at another slot
        const std::optional<SlotAddress> identicalElsewhere = findIdenticalInTarget(req, req.sourceAddress);
        if (identicalElsewhere.has_value()) {
            const std::string targetSlotStr = identicalElsewhere->shortLabel();
            if (input.policy == ConflictPolicy::BlockWithReport) {
                result.conflicts.push_back({
                    SlotConflict::Kind::SoundSlotIdenticalElsewhere,
                    addrStr,
                    req.sourceLabel,
                    targetEntry->displayName,
                    "BLOCKED",
                    true,
                    "Occupied by different Sound; proven-identical Sound exists at " + targetSlotStr + "."
                });
                result.diagnostics.push_back({
                    DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                    diag_code::kSoundSlotOccupiedDifferent,
                    DiagnosticSeverity::Blocking,
                    EvidenceLevel::Verified,
                    subj,
                    addrStr,
                    "Target slot " + req.sourceAddress.shortLabel() + " is occupied by a different Sound; proven-identical Sound exists at " + targetSlotStr + ".",
                    "Switch policy to ReuseIdenticalTargetSlot or RemapToFreeSlotInNewLib to safely reuse " + targetSlotStr + ".",
                    req.affectedRoots
                });
                outRes.action = "blocked";
                outRes.blocking = true;
                outRes.detail = "blocked under BlockWithReport (identical Sound exists at " + targetSlotStr + ")";
            } else {
                // ReuseIdenticalTargetSlot AND RemapToFreeSlotInNewLib both prefer reusing the proven-identical target slot!
                result.conflicts.push_back({
                    SlotConflict::Kind::SoundSlotIdenticalElsewhere,
                    addrStr,
                    req.sourceLabel,
                    targetEntry->displayName,
                    "REMAP_TO " + targetSlotStr,
                    false,
                    "Reusing proven-identical Sound already at " + targetSlotStr + " (0 Sound bytes written)."
                });
                outRes.resolvedAddress = *identicalElsewhere;
                outRes.remapped = true;
                outRes.action = "remap-reuse-target";
                outRes.equivalence = EquivalenceVerdict::Identical;
                outRes.blocking = false;
                outRes.detail = "remapped to proven-identical Sound at " + targetSlotStr;
            }
            continue;
        }

        // No proven-identical Sound anywhere in target (either Different or InsufficientEvidence)
        const bool isInsufficient = (eq.verdict == EquivalenceVerdict::InsufficientEvidence);
        if (req.sourceAddress.isDrumKit || targetEntry->address.isDrumKit) {
            result.diagnostics.push_back({
                DiagnosticCategory::UNSUPPORTED_FORMAT,
                diag_code::kDrumkitBodyUnparsed,
                DiagnosticSeverity::Warning,
                EvidenceLevel::Unsupported,
                subj,
                addrStr,
                "User Drum Kit slot " + req.sourceAddress.shortLabel() + " cannot be proven equivalent because Drum Kit bodies are unparsed.",
                "Never auto-reused; remap to a free USERDK slot or load individually on Pa5X (MAN5 p.736).",
                req.affectedRoots
            });
        }

        if (input.policy == ConflictPolicy::RemapToFreeSlotInNewLib) {
            // Queue for Phase 2 free-slot allocation (never overwrites occupied slot; never auto-reuses unprovable Sound)
            if (isInsufficient) {
                result.diagnostics.push_back({
                    DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                    diag_code::kSoundSlotEquivalenceInsufficient,
                    DiagnosticSeverity::Warning,
                    EvidenceLevel::Unsupported,
                    subj,
                    addrStr,
                    "Target slot " + req.sourceAddress.shortLabel() + " equivalence unprovable (" + eq.reason + "); remapping to a free slot instead of reusing.",
                    "Verify Drum Kit / unanalysed Sound on Pa5X after remapping.",
                    req.affectedRoots
                });
            }
            remapQueueIndices.push_back(i);
        } else {
            // BlockWithReport OR ReuseIdenticalTargetSlot -> BLOCK (never overwrite!)
            result.conflicts.push_back({
                SlotConflict::Kind::SoundSlotOccupiedDifferent,
                addrStr,
                req.sourceLabel,
                targetEntry->displayName,
                "BLOCKED",
                true,
                isInsufficient
                    ? ("Equivalence unprovable (" + eq.reason + "); fail-closed refusal to reuse or overwrite.")
                    : ("Target slot occupied by different Sound (" + eq.reason + "); never overwritten.")
            });
            result.diagnostics.push_back({
                DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                isInsufficient ? diag_code::kSoundSlotEquivalenceInsufficient : diag_code::kSoundSlotOccupiedDifferent,
                DiagnosticSeverity::Blocking,
                isInsufficient ? EvidenceLevel::Unsupported : EvidenceLevel::Verified,
                subj,
                addrStr,
                isInsufficient
                    ? ("Target slot " + req.sourceAddress.shortLabel() + " equivalence cannot be proven (" + eq.reason + ").")
                    : ("Target slot " + req.sourceAddress.shortLabel() + " is occupied by a different Sound (" + eq.reason + ")."),
                "Never overwritten. Switch policy to RemapToFreeSlotInNewLib or remap manually.",
                req.affectedRoots
            });
            outRes.action = "blocked";
            outRes.blocking = true;
            outRes.detail = eq.reason;
        }
    }

    // Phase 2: Deterministic Free-Slot Allocation for RemapToFreeSlotInNewLib
    for (size_t idx : remapQueueIndices) {
        const auto& req = input.soundRequests[idx];
        auto& outRes = result.soundResolutions[idx];
        const OccupiedSlotEntry* targetEntry = target.findOccupied(req.sourceAddress);
        const std::string targetLbl = targetEntry ? targetEntry->displayName : "(reserved)";
        const std::string addrStr = req.sourceAddress.label();
        const std::string subj = req.requestId.empty() ? req.sourceAddress.resourceId().str() : req.requestId;

        const auto assigned = allocator.allocateOne(req.sourceAddress);
        if (!assigned.has_value()) {
            const std::string classDesc = req.sourceAddress.isDrumKit
                ? "User Drum Kit (768 slots across USERDK01..USERDK06)"
                : "User Sound (1,536 slots across USER01..USER12)";
            result.conflicts.push_back({
                SlotConflict::Kind::BankGridExhausted,
                addrStr,
                req.sourceLabel,
                targetLbl,
                "BLOCKED",
                true,
                "All " + classDesc + " slots are exhausted; cannot allocate a free slot."
            });
            result.diagnostics.push_back({
                DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                diag_code::kBankGridExhausted,
                DiagnosticSeverity::Blocking,
                EvidenceLevel::Verified,
                subj,
                addrStr,
                "Target " + classDesc + " grid is completely full (BANK_GRID_EXHAUSTED).",
                "Free unused target slots on the Pa5X or reduce the selected Style/Sound set.",
                req.affectedRoots
            });
            outRes.resolvedAddress = std::nullopt;
            outRes.remapped = false;
            outRes.action = "blocked";
            outRes.blocking = true;
            outRes.detail = "BANK_GRID_EXHAUSTED";
        } else {
            const std::string dstShort = assigned->shortLabel();
            result.conflicts.push_back({
                SlotConflict::Kind::SoundSlotOccupiedDifferent,
                addrStr,
                req.sourceLabel,
                targetLbl,
                "REMAP_TO " + dstShort,
                false,
                "Target slot occupied; remapped to free slot " + assigned->label() + "."
            });
            outRes.resolvedAddress = *assigned;
            outRes.remapped = (*assigned != req.sourceAddress);
            outRes.action = "remap-free-slot";
            outRes.blocking = false;
            outRes.detail = "remapped to free slot " + dstShort;
        }
    }

    // Phase 3: Object Path Collisions + FAT32/exFAT R-18 Case-Folded Collision Checks
    std::map<std::string, std::vector<size_t>> batchByFolded;
    std::vector<std::string> effTargetPaths(input.objectRequests.size());
    std::vector<std::string> effFoldedPaths(input.objectRequests.size());
    for (size_t i = 0; i < input.objectRequests.size(); ++i) {
        effTargetPaths[i] = conflict_detail::applyTargetPathOverride(
            input.objectRequests[i].sourceRelPath,
            input.objectRequests[i].targetBank,
            input.objectRequests[i].targetName);
        effFoldedPaths[i] = conflict_detail::asciiLower(effTargetPaths[i]);
        batchByFolded[effFoldedPaths[i]].push_back(i);
    }

    result.objectResolutions.reserve(input.objectRequests.size());
    for (size_t i = 0; i < input.objectRequests.size(); ++i) {
        const auto& obj = input.objectRequests[i];
        const std::string& effPath = effTargetPaths[i];
        const std::string& folded = effFoldedPaths[i];

        ObjectPathResolutionEntry res;
        res.kind = obj.kind;
        res.sourceRelPath = obj.sourceRelPath;
        res.targetRelPath = effPath;
        res.foldedTargetRelPath = folded;

        // Intra-batch collision / FAT32 R-18 check
        if (batchByFolded[folded].size() > 1) {
            result.conflicts.push_back({
                SlotConflict::Kind::StyleOrObjectPathCollision,
                effPath,
                obj.sourceRelPath,
                "(batch case-folded collision)",
                "RENAME_REQUIRED",
                true,
                "Multiple selected objects collide under ASCII case-folding (FAT32/exFAT R-18)."
            });
            result.diagnostics.push_back({
                DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                diag_code::kFatCaseCollisionR18,
                DiagnosticSeverity::Blocking,
                EvidenceLevel::Verified,
                effPath,
                folded,
                "Output path collides with another selected item under ASCII case-folding (FAT32/exFAT R-18).",
                "Provide distinct targetBank or targetName overrides before export.",
                obj.affectedRoots.empty() ? std::vector<std::string>{obj.sourceRelPath} : obj.affectedRoots
            });
            res.action = "blocked-fat-case-r18";
            res.blocking = true;
            res.detail = "batch FAT32/exFAT R-18 case collision";
            result.objectResolutions.push_back(std::move(res));
            continue;
        }

        // Target registry lookup
        const auto tit = target.objectPathsByFolded.find(folded);
        if (tit == target.objectPathsByFolded.end() || tit->second.empty()) {
            res.action = "allow-new";
            res.blocking = false;
            res.detail = "target path is free";
            result.objectResolutions.push_back(std::move(res));
            continue;
        }

        const auto& targetMatches = tit->second;
        if (targetMatches.size() > 1 || targetMatches[0].relPath != effPath) {
            // Case-only difference (FAT32/exFAT R-18) -> BLOCK under ALL 3 policies
            result.conflicts.push_back({
                SlotConflict::Kind::StyleOrObjectPathCollision,
                effPath,
                obj.sourceRelPath,
                targetMatches[0].relPath,
                "RENAME_REQUIRED",
                true,
                "Target contains '" + targetMatches[0].relPath + "' which collides case-insensitively with '" + effPath + "' (FAT32/exFAT R-18)."
            });
            result.diagnostics.push_back({
                DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                diag_code::kFatCaseCollisionR18,
                DiagnosticSeverity::Blocking,
                EvidenceLevel::Verified,
                effPath,
                targetMatches[0].relPath,
                "Output path '" + effPath + "' collides case-insensitively with target path '" + targetMatches[0].relPath + "' (FAT32/exFAT R-18).",
                "Rename targetBank or targetName to eliminate the case-insensitive collision.",
                obj.affectedRoots.empty() ? std::vector<std::string>{obj.sourceRelPath} : obj.affectedRoots
            });
            res.action = "blocked-fat-case-r18";
            res.blocking = true;
            res.detail = "FAT32/exFAT R-18 case collision with target path " + targetMatches[0].relPath;
            result.objectResolutions.push_back(std::move(res));
            continue;
        }

        const ObjectPathEntry& tObj = targetMatches[0];
        if (!obj.fileSha256.empty() && obj.fileSha256.size() == 64 && obj.fileSha256 == tObj.sha256) {
            // Exact path + identical SHA-256 -> WARN (already-present, no-op) under ALL 3 policies
            result.conflicts.push_back({
                SlotConflict::Kind::StyleOrObjectPathCollision,
                effPath,
                obj.sourceRelPath,
                tObj.relPath,
                "ALREADY_PRESENT_NOOP",
                false,
                "Target already holds a byte-identical file (matching SHA-256); no-op."
            });
            result.diagnostics.push_back({
                DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                diag_code::kStyleOrObjectPathCollision,
                DiagnosticSeverity::Warning,
                EvidenceLevel::Verified,
                effPath,
                tObj.relPath,
                "Target already contains a byte-identical copy of '" + effPath + "'; skipping duplicate copy.",
                "No action required unless you want a separately named copy.",
                obj.affectedRoots.empty() ? std::vector<std::string>{obj.sourceRelPath} : obj.affectedRoots
            });
            res.action = "already-present-noop";
            res.blocking = false;
            res.detail = "byte-identical file already present in target";
            result.objectResolutions.push_back(std::move(res));
            continue;
        }

        // Exact path + different bytes (or missing SHA-256 -> fail-closed!) -> BLOCK under ALL 3 policies
        result.conflicts.push_back({
            SlotConflict::Kind::StyleOrObjectPathCollision,
            effPath,
            obj.sourceRelPath,
            tObj.relPath,
            "RENAME_REQUIRED",
            true,
            "Target already contains a different (or unverified) file at '" + effPath + "'; never overwritten."
        });
        result.diagnostics.push_back({
            DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
            diag_code::kStyleOrObjectPathCollision,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Verified,
            effPath,
            tObj.relPath,
            "Target path '" + effPath + "' already exists with different bytes (or unverified digest); never overwritten.",
            "Provide an explicit targetBank or targetName override to export without overwriting.",
            obj.affectedRoots.empty() ? std::vector<std::string>{obj.sourceRelPath} : obj.affectedRoots
        });
        res.action = "blocked-collision";
        res.blocking = true;
        res.detail = "target path exists with different bytes";
        result.objectResolutions.push_back(std::move(res));
    }

    // Phase 4: Mandatory SongBook unrewritten warning whenever any Sound is remapped or object renamed (§5.3)
    if (result.anyRemappedOrRenamed()) {
        result.diagnostics.push_back({
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

    canonicalizeConflicts(result.conflicts);
    canonicalizeDiagnostics(result.diagnostics);
    return result;
}

}  // namespace ai_arranger::media::selective

#endif  // AI_ARRANGER_MEDIA_SOUND_SLOT_CONFLICTS_H
