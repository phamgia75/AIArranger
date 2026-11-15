// P3.5 SF2 pre-import check (read-only). The Pa5X manual v1.4 p.1206 says SF2
// Instruments "with overlapping zones" may not import. This parses the public
// SoundFont 2.04 hydra (pdta: inst/ibag/igen/shdr) and reports, per Instrument,
// pairs of zones whose key AND velocity ranges intersect.
// A linked left/right stereo pair with identical ranges is not reported: the
// manual says SF2 import is "mono or stereo", and SF2 encodes stereo that way.
// Whether Pa5X rejects every overlap reported here is HARDWARE VALIDATION PENDING.
#ifndef AI_ARRANGER_MEDIA_SF2_CHECK_H
#define AI_ARRANGER_MEDIA_SF2_CHECK_H

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace ai_arranger::media::sf2 {

struct Range { uint8_t lo = 0, hi = 127; };
struct Zone { Range key, vel; int sample = -1; };
struct Overlap { size_t a, b; };  // zone indices within the instrument
struct Instrument {
    std::string name;
    std::vector<Zone> zones;     // local zones only (global zone folded in as defaults)
    std::vector<Overlap> overlaps;
};
struct Sample { std::string name; uint16_t link = 0, type = 0; };
struct Report {
    bool ok = false;
    std::string error;
    std::vector<Instrument> instruments;
    std::vector<Sample> samples;
    size_t overlapping() const {
        size_t n = 0;
        for (const auto& i : instruments) n += !i.overlaps.empty();
        return n;
    }
};

namespace detail {
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline std::string name20(const uint8_t* p) { return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), 20)); }
inline bool intersects(Range x, Range y) { return x.lo <= y.hi && y.lo <= x.hi; }
inline bool same(Range x, Range y) { return x.lo == y.lo && x.hi == y.hi; }
struct Chunk { const uint8_t* p = nullptr; size_t n = 0; };
}

/// Parses an in-memory SF2 file. Every offset and index is bounds-checked (untrusted input).
inline Report check(const std::vector<uint8_t>& f) {
    using namespace detail;
    Report r;
    auto fail = [&](const char* m) { r.error = m; return r; };
    if (f.size() < 12 || std::memcmp(f.data(), "RIFF", 4) || std::memcmp(f.data() + 8, "sfbk", 4))
        return fail("not a RIFF sfbk (SoundFont 2) file");
    const size_t end = std::min<size_t>(f.size(), size_t(u32(f.data() + 4)) + 8);
    Chunk inst, ibag, igen, shdr;
    for (size_t o = 12; o + 8 <= end;) {
        const size_t sz = u32(f.data() + o + 4);
        if (sz > end - o - 8) return fail("chunk runs past end of file");
        if (!std::memcmp(f.data() + o, "LIST", 4) && sz >= 4 && !std::memcmp(f.data() + o + 8, "pdta", 4)) {
            for (size_t q = o + 12; q + 8 <= o + 8 + sz;) {
                const size_t s2 = u32(f.data() + q + 4);
                if (s2 > o + 8 + sz - q - 8) return fail("pdta sub-chunk runs past its list");
                const Chunk c{f.data() + q + 8, s2};
                if (!std::memcmp(f.data() + q, "inst", 4)) inst = c;
                else if (!std::memcmp(f.data() + q, "ibag", 4)) ibag = c;
                else if (!std::memcmp(f.data() + q, "igen", 4)) igen = c;
                else if (!std::memcmp(f.data() + q, "shdr", 4)) shdr = c;
                q += 8 + s2 + (s2 & 1);
            }
        }
        o += 8 + sz + (sz & 1);
    }
    if (!inst.p || !ibag.p || !igen.p || !shdr.p) return fail("missing pdta inst/ibag/igen/shdr");
    if (inst.n % 22 || inst.n < 44 || ibag.n % 4 || ibag.n < 4 || igen.n % 4 || igen.n < 4 || shdr.n % 46 || shdr.n < 46)
        return fail("pdta record sizes are not valid SoundFont 2 sizes");
    const size_t nInst = inst.n / 22, nBag = ibag.n / 4, nGen = igen.n / 4, nShdr = shdr.n / 46;
    for (size_t i = 0; i + 1 < nShdr; ++i)  // last record is the EOS terminator
        r.samples.push_back({name20(shdr.p + i * 46), u16(shdr.p + i * 46 + 42), u16(shdr.p + i * 46 + 44)});

    for (size_t i = 0; i + 1 < nInst; ++i) {  // last record is the EOI terminator
        Instrument in{name20(inst.p + i * 22), {}, {}};
        const size_t b0 = u16(inst.p + i * 22 + 20), b1 = u16(inst.p + (i + 1) * 22 + 20);
        if (b0 > b1 || b1 >= nBag) return fail("instrument bag index out of range");
        Zone defaults;
        for (size_t b = b0; b < b1; ++b) {
            const size_t g0 = u16(ibag.p + b * 4), g1 = u16(ibag.p + (b + 1) * 4);
            if (g0 > g1 || g1 >= nGen) return fail("zone generator index out of range");
            Zone z = defaults;
            z.sample = -1;
            for (size_t g = g0; g < g1; ++g) {
                const uint8_t* p = igen.p + g * 4;
                const uint16_t op = u16(p);
                if (op == 43) z.key = {p[2], p[3]};
                else if (op == 44) z.vel = {p[2], p[3]};
                else if (op == 53) z.sample = u16(p + 2);
            }
            if (z.sample < 0) {  // a zone without sampleID: global if first, otherwise ignored (spec 7.9)
                if (b == b0) defaults = z;
                continue;
            }
            if (size_t(z.sample) + 1 >= nShdr) return fail("zone sample index out of range");
            in.zones.push_back(z);
        }
        for (size_t a = 0; a < in.zones.size(); ++a)
            for (size_t b = a + 1; b < in.zones.size(); ++b) {
                const Zone &x = in.zones[a], &y = in.zones[b];
                if (!intersects(x.key, y.key) || !intersects(x.vel, y.vel)) continue;
                const auto &sx = r.samples[x.sample], &sy = r.samples[y.sample];
                const bool lr = ((sx.type & 0x7FFF) == 4 && (sy.type & 0x7FFF) == 2) || ((sx.type & 0x7FFF) == 2 && (sy.type & 0x7FFF) == 4);
                const bool linked = sx.link == uint16_t(y.sample) || sy.link == uint16_t(x.sample);
                if (lr && linked && same(x.key, y.key) && same(x.vel, y.vel)) continue;  // stereo pair
                in.overlaps.push_back({a, b});
            }
        r.instruments.push_back(std::move(in));
    }
    r.ok = true;
    return r;
}

inline Report checkFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { Report r; r.error = "cannot open file"; return r; }
    return check(std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {}));
}

inline std::string format(const Report& r) {
    std::ostringstream o;
    if (!r.ok) { o << "error: " << r.error << "\n"; return o.str(); }
    o << r.instruments.size() << " instrument(s), " << r.samples.size() << " sample(s); "
      << r.overlapping() << " with overlapping zones (Pa5X may not import these, manual p.1206)\n";
    for (const auto& i : r.instruments) {
        o << (i.overlaps.empty() ? "  ok       " : "  OVERLAP  ") << i.name << "  (" << i.zones.size() << " zones)\n";
        for (const auto& ov : i.overlaps) {
            const Zone &x = i.zones[ov.a], &y = i.zones[ov.b];
            o << "           zone " << ov.a << " key " << int(x.key.lo) << "-" << int(x.key.hi) << " vel " << int(x.vel.lo) << "-" << int(x.vel.hi)
              << "  x  zone " << ov.b << " key " << int(y.key.lo) << "-" << int(y.key.hi) << " vel " << int(y.vel.lo) << "-" << int(y.vel.hi) << "\n";
        }
    }
    return o.str();
}

} // namespace ai_arranger::media::sf2

#endif
