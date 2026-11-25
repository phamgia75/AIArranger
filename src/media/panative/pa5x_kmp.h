#ifndef AI_ARRANGER_MEDIA_PANATIVE_PA5X_KMP_H
#define AI_ARRANGER_MEDIA_PANATIVE_PA5X_KMP_H

// READ-ONLY reader for the raw Pa5X multisample item (KORF item chunk 0x11080018, "RAM.KMP"). Pa5X-specific: it is NOT the
// Kronos/Triton KMP+KSF format (korg_kmp_source.h) and not the Pa700/Pa4X OC31 KMP, whose layout is unknown.
// Layout (all integers big-endian) OBSERVED and checked on ONE owner Pa5X backup (research: pa5x-kst/KMP_MULTISAMPLE_ANALYSIS.md):
//   header 18 B: u16 0x0103, u32 samples, u32 drum records, u32 zones, u32 multisamples
//   samples   122 B each: name @75 (32), ID @107 (8), rate u16 @119
//   drum recs 126 B each: name @0 (32), L sample index u32 @32, R sample index u32 @36 (0xFFFFFFFF = none),
//                         advisory drumFamilyByte @113 (INFERRED, 0..7/0xFF; never used for edges/equivalence), ID @118 (8)
//   zones      16 B each: sample index u32 @0 (0xFFFFFFFF = empty zone), top key @10
//   multisamples 176 B each: name @0 (32), first zone u32 @32, zone count u8 @166, ID @168 (8)
// The four sections must fill the item to the byte. Every index is range-checked; any inconsistency is an error, never a
// guess. Unknown parameter bytes are not read. Nothing here is a writer, and a parse is not a Pa5X compatibility claim.
// Input is untrusted: all size arithmetic is done in 64 bit and checked before any read.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ai_arranger::media::panative {

constexpr uint32_t kPa5xKmpChunkId = 0x11080018u;
constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

struct Pa5xSample { std::array<uint8_t, 8> id{}; std::string name; uint16_t rate = 0; };
struct Pa5xDrumRecord {
    std::array<uint8_t, 8> id{};
    std::string name;
    uint32_t left = 0, right = kNoIndex;
    // INFERRED advisory metadata only (+113 in 126-byte RAM.KMP drum record: 0=B,1=S,2=H,3=T,4=C,5=L,6=E,7=X,0xFF=None).
    // NEVER used to infer graph edges or equivalence.
    uint8_t drumFamilyByte = 0xFF;
};
struct Pa5xZone { uint32_t sample = kNoIndex; uint8_t topKey = 0; };
struct Pa5xMultisample { std::array<uint8_t, 8> id{}; std::string name; uint32_t firstZone = 0; uint8_t zoneCount = 0; };

struct Pa5xKmp {
    std::vector<Pa5xSample> samples;
    std::vector<Pa5xDrumRecord> drums;
    std::vector<Pa5xZone> zones;
    std::vector<Pa5xMultisample> multisamples;
    bool zonesContiguous = false;   // multisample i's zones start where i-1's end (true on the observed file); reported, not required

    // Sample-table indices of one multisample's zones (kNoIndex = empty zone). Empty vector if out of range.
    std::vector<uint32_t> zoneSamples(size_t multisample) const {
        std::vector<uint32_t> r;
        if (multisample >= multisamples.size()) return r;
        const auto& m = multisamples[multisample];
        if (m.firstZone > zones.size() || m.zoneCount > zones.size() - m.firstZone) return r;
        r.reserve(m.zoneCount);
        for (size_t z = 0; z < m.zoneCount; ++z) r.push_back(zones[size_t(m.firstZone) + z].sample);
        return r;
    }
};

namespace pa5x_detail {
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(uint32_t(p[0]) << 8 | uint32_t(p[1])); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }
inline std::string name32(const uint8_t* p) {   // NUL-terminated within 32 bytes; bytes after the NUL are ignored
    size_t n = 0; while (n < 32 && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}
inline std::array<uint8_t, 8> id8(const uint8_t* p) { std::array<uint8_t, 8> r; for (size_t i = 0; i < 8; ++i) r[i] = p[i]; return r; }
}

// `p`/`n` = the item payload (after the 8-byte chunk header). Returns false and sets *err on any inconsistency; `out` is then empty.
inline bool parsePa5xKmp(const uint8_t* p, size_t n, Pa5xKmp& out, std::string* err) {
    using namespace pa5x_detail;
    out = Pa5xKmp{};
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    constexpr uint64_t kHeader = 18, kSample = 122, kDrum = 126, kZone = 16, kMulti = 176;
    if (!p || n < kHeader) return fail("shorter than the 18-byte header");
    if (be16(p) != 0x0103) return fail("version is not 0x0103 (the one observed layout)");
    const uint64_t ns = be32(p + 2), nd = be32(p + 6), nz = be32(p + 10), nm = be32(p + 14);
    const uint64_t total = kHeader + kSample * ns + kDrum * nd + kZone * nz + kMulti * nm;   // <= 18 + 4.3e9 * 440: fits in 64 bit
    if (total != n) return fail("sections do not fill the item exactly (header counts vs size)");
    if (nz > 0xFFFFFFFEull || ns > 0xFFFFFFFEull) return fail("count too large");
    const uint8_t* S = p + kHeader;
    const uint8_t* D = S + kSample * ns;
    const uint8_t* Z = D + kDrum * nd;
    const uint8_t* M = Z + kZone * nz;
    Pa5xKmp r;
    r.samples.reserve(static_cast<size_t>(ns)); r.drums.reserve(static_cast<size_t>(nd));
    r.zones.reserve(static_cast<size_t>(nz)); r.multisamples.reserve(static_cast<size_t>(nm));
    for (uint64_t i = 0; i < ns; ++i) {
        const uint8_t* q = S + kSample * i;
        r.samples.push_back({id8(q + 107), name32(q + 75), be16(q + 119)});
    }
    for (uint64_t i = 0; i < nd; ++i) {
        const uint8_t* q = D + kDrum * i;
        Pa5xDrumRecord d{id8(q + 118), name32(q), be32(q + 32), be32(q + 36), q[113]};
        if (d.left >= ns) return fail("drum record " + std::to_string(i) + ": left sample index out of range");
        if (d.right != kNoIndex && d.right >= ns) return fail("drum record " + std::to_string(i) + ": right sample index out of range");
        r.drums.push_back(std::move(d));
    }
    for (uint64_t i = 0; i < nz; ++i) {
        const uint8_t* q = Z + kZone * i;
        Pa5xZone z{be32(q), q[10]};
        if (z.sample != kNoIndex && z.sample >= ns) return fail("zone " + std::to_string(i) + ": sample index out of range");
        r.zones.push_back(z);
    }
    uint64_t expect = 0; bool contiguous = true;
    for (uint64_t i = 0; i < nm; ++i) {
        const uint8_t* q = M + kMulti * i;
        Pa5xMultisample m{id8(q + 168), name32(q), be32(q + 32), q[166]};
        if (uint64_t(m.firstZone) + m.zoneCount > nz) return fail("multisample " + std::to_string(i) + ": zones run past the zone table");
        contiguous = contiguous && (m.firstZone == expect); expect = uint64_t(m.firstZone) + m.zoneCount;
        r.multisamples.push_back(std::move(m));
    }
    r.zonesContiguous = contiguous && expect == nz;
    out = std::move(r);
    return true;
}

}  // namespace ai_arranger::media::panative
#endif
