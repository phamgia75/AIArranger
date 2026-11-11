#ifndef AI_ARRANGER_TESTS_SUPPORT_KORG_FIXTURE_H
#define AI_ARRANGER_TESTS_SUPPORT_KORG_FIXTURE_H

// Test-only writer of KRONOS Korg-format .KMP/.KSF files, byte for byte from KRONOS Parameter Guide En 3 p.1158-1159
// (big endian; MSP1 NAME RLP1 RLP2 MNO1 / SMP1 SMD1 SNO1). Synthetic audio only. Shared by test_kronos_source and
// the app smoke test.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace korgfix {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
inline void be32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
inline void str(Bytes& b, const std::string& s, size_t n) { for (size_t i = 0; i < n; ++i) b.push_back(i < s.size() ? uint8_t(s[i]) : 0); }
inline void chunk(Bytes& b, const char* id, const Bytes& body) { str(b, id, 4); be32(b, uint32_t(body.size())); b.insert(b.end(), body.begin(), body.end()); }

struct Reg { uint8_t root, top; int8_t tune, level; std::string ksf; bool fixed = false; };
inline Bytes kmp(const std::string& name, const std::vector<Reg>& rs, bool withName24 = true, int8_t resonance = 0) {
    Bytes b, msp, rlp1, nm, rlp2;
    str(msp, name, 16); msp.push_back(uint8_t(rs.size())); msp.push_back(0);
    chunk(b, "MSP1", msp);
    if (withName24) { str(nm, name, 24); chunk(b, "NAME", nm); }
    for (const auto& r : rs) {
        rlp1.push_back(uint8_t(r.root | (r.fixed ? 0x80 : 0))); rlp1.push_back(r.top);
        rlp1.push_back(uint8_t(r.tune)); rlp1.push_back(uint8_t(r.level)); rlp1.push_back(64); rlp1.push_back(0);
        str(rlp1, r.ksf, 12);
    }
    chunk(b, "RLP1", rlp1);
    rlp2 = {0, uint8_t(resonance), 0, 0};
    chunk(b, "RLP2", rlp2);
    Bytes mno; be32(mno, 0); chunk(b, "MNO1", mno);
    return b;
}
struct Ksf { std::vector<int16_t> pcm; uint32_t rate = 48000, start = 0, loopStart = 0, end = 0; uint8_t attrs = 0, bits = 16, ch = 1; int8_t loopTune = 0; };
inline Bytes ksf(const Ksf& k, const char* extra = nullptr, const std::string& name = "SYNTH") {
    Bytes b, smp, smd, sno;
    str(smp, name, 16); be32(smp, k.start); be32(smp, k.start); be32(smp, k.loopStart); be32(smp, k.end);
    chunk(b, "SMP1", smp);
    be32(smd, k.rate); smd.push_back(k.attrs); smd.push_back(uint8_t(k.loopTune)); smd.push_back(k.ch); smd.push_back(k.bits);
    be32(smd, uint32_t(k.pcm.size()));
    for (int16_t v : k.pcm) { smd.push_back(uint8_t(uint16_t(v) >> 8)); smd.push_back(uint8_t(v)); }   // big endian
    chunk(b, "SMD1", smd);
    be32(sno, 0); chunk(b, "SNO1", sno);
    if (extra) { Bytes x(12, 0); chunk(b, extra, x); }
    return b;
}
inline std::vector<int16_t> tone(size_t n, double f, double amp, int salt) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = int16_t(std::lround(amp * std::sin(2 * 3.14159265358979 * f * double(i) / 48000) + (int(i * 7 + salt) % 5) - 2));
    return v;
}
inline void put(const fs::path& p, const Bytes& b) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())); }
}  // namespace korgfix

#endif
