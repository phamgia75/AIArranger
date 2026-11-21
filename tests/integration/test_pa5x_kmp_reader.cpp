// Pa5X raw RAM.KMP reader (src/media/panative/pa5x_kmp.h). Synthetic, original fixtures only; the real-data check runs only when
// AIARR_PA5X_RAM_KMP names a private KORF file (owner data is never in the repository) and is printed as SKIPPED otherwise.
// SOFTWARE evidence: parsing is not Pa5X compatibility.

#include "media/panative/pa5x_kmp.h"
#include "media/panative/panative_reader.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>

using namespace ai_arranger::media::panative;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

using Bytes = std::vector<uint8_t>;
static void be32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i)); }
static void put(Bytes& b, size_t at, const std::string& s) { for (size_t i = 0; i < s.size(); ++i) b[at + i] = uint8_t(s[i]); }

struct Spec {
    struct S { std::string name; uint16_t rate; };
    struct D { std::string name; uint32_t l, r; };
    struct M { std::string name; uint32_t first; uint8_t count; };
    std::vector<S> s; std::vector<D> d; std::vector<std::pair<uint32_t, uint8_t>> z; std::vector<M> m; uint16_t version = 0x0103;
};
static Bytes build(const Spec& sp) {
    const size_t ns = sp.s.size(), nd = sp.d.size(), nz = sp.z.size(), nm = sp.m.size();
    Bytes b(18 + 122 * ns + 126 * nd + 16 * nz + 176 * nm, 0x5A);   // 0x5A filler stands for the unknown parameter bytes
    b[0] = uint8_t(sp.version >> 8); b[1] = uint8_t(sp.version);
    be32(b, 2, uint32_t(ns)); be32(b, 6, uint32_t(nd)); be32(b, 10, uint32_t(nz)); be32(b, 14, uint32_t(nm));
    size_t S = 18, D = S + 122 * ns, Z = D + 126 * nd, M = Z + 16 * nz;
    for (size_t i = 0; i < ns; ++i) {
        std::fill(b.begin() + S + 122 * i + 75, b.begin() + S + 122 * i + 107, 0); put(b, S + 122 * i + 75, sp.s[i].name);
        for (int k = 0; k < 8; ++k) b[S + 122 * i + 107 + k] = uint8_t(0x10 + i + k * 16);
        b[S + 122 * i + 119] = uint8_t(sp.s[i].rate >> 8); b[S + 122 * i + 120] = uint8_t(sp.s[i].rate);
    }
    for (size_t i = 0; i < nd; ++i) {
        std::fill(b.begin() + D + 126 * i, b.begin() + D + 126 * i + 32, 0); put(b, D + 126 * i, sp.d[i].name);
        be32(b, D + 126 * i + 32, sp.d[i].l); be32(b, D + 126 * i + 36, sp.d[i].r);
        for (int k = 0; k < 8; ++k) b[D + 126 * i + 118 + k] = uint8_t(0x80 + i + k);
    }
    for (size_t i = 0; i < nz; ++i) { be32(b, Z + 16 * i, sp.z[i].first); b[Z + 16 * i + 10] = sp.z[i].second; }
    for (size_t i = 0; i < nm; ++i) {
        std::fill(b.begin() + M + 176 * i, b.begin() + M + 176 * i + 32, 0); put(b, M + 176 * i, sp.m[i].name);
        be32(b, M + 176 * i + 32, sp.m[i].first); b[M + 176 * i + 166] = sp.m[i].count;
        for (int k = 0; k < 8; ++k) b[M + 176 * i + 168 + k] = uint8_t(0xC0 + i + k);
    }
    return b;
}
static Spec good() {
    Spec s;
    s.s = {{"s0", 48000}, {"s1", 44100}, {"s2", 32000}};
    s.d = {{"kick", 0, kNoIndex}, {"snare", 1, 2}};
    s.z = {{0, 60}, {1, 127}, {kNoIndex, 127}, {1, 127}};     // multisample A: zones 0-1; B: zones 2-3 (second empty then s1)
    s.m = {{"A", 0, 2}, {"B", 2, 2}};
    return s;
}
static bool parses(const Bytes& b, Pa5xKmp* o = nullptr, std::string* e = nullptr) { Pa5xKmp k; const bool ok = parsePa5xKmp(b.data(), b.size(), k, e); if (o) *o = k; return ok; }

int main() {
    {
        const Bytes b = build(good()); Pa5xKmp k; std::string e;
        TEST("valid synthetic item parses", parsePa5xKmp(b.data(), b.size(), k, &e));
        TEST("counts", k.samples.size() == 3 && k.drums.size() == 2 && k.zones.size() == 4 && k.multisamples.size() == 2);
        TEST("sample name, rate and ID read at the documented offsets", k.samples[1].name == "s1" && k.samples[1].rate == 44100 && k.samples[1].id[0] == 0x11 && k.samples[1].id[7] == 0x81);
        TEST("drum record L/R, with 0xFFFFFFFF as none", k.drums[0].left == 0 && k.drums[0].right == kNoIndex && k.drums[1].right == 2 && k.drums[1].name == "snare");
        TEST("zone sample index and top key", k.zones[0].sample == 0 && k.zones[0].topKey == 60 && k.zones[2].sample == kNoIndex);
        TEST("multisample chain: zones -> sample indices", k.zoneSamples(0) == std::vector<uint32_t>({0, 1}) && k.zoneSamples(1) == std::vector<uint32_t>({kNoIndex, 1}));
        TEST("zoneSamples of an out-of-range multisample is empty", k.zoneSamples(2).empty());
        TEST("contiguous zones reported", k.zonesContiguous);
        TEST("unknown parameter bytes are not interpreted (name stops at NUL)", k.multisamples[0].name == "A");
    }
    {   Spec s = good(); s.m = {{"A", 2, 2}, {"B", 0, 2}};
        Pa5xKmp k; TEST("non-contiguous zone assignment parses but is reported", parses(build(s), &k) && !k.zonesContiguous); }
    {   Spec s = good(); s.m = {{"A", 0, 2}};
        Pa5xKmp k; TEST("zones not owned by any multisample: parses, not contiguous", parses(build(s), &k) && !k.zonesContiguous); }
    {   Bytes b = build(good()); std::string e;
        Pa5xKmp dummy; TEST("empty input refused", !parsePa5xKmp(nullptr, 0, dummy, &e) && !e.empty());
        TEST("shorter than the header refused", !parses(Bytes(17, 0)));
        Bytes t = b; t.pop_back(); TEST("truncated by one byte refused", !parses(t));
        Bytes p = b; p.push_back(0); TEST("padded by one byte refused", !parses(p));
        Bytes v = b; v[1] = 0x04; TEST("other version refused", !parses(v));
        Bytes c = b; be32(c, 2, 4); TEST("sample count that does not match the size refused", !parses(c));
        Bytes h = b; for (int i = 2; i < 18; ++i) h[i] = 0xFF; Pa5xKmp o; TEST("all counts 0xFFFFFFFF refused without allocating", !parses(h, &o) && o.samples.empty());
        Bytes z = b; be32(z, 2, 0xFFFFFFFF); z.resize(18); TEST("one huge count with no body refused", !parses(z));
    }
    {   Spec s = good(); s.d[0].l = 3;           TEST("drum left index out of range refused", !parses(build(s)));
        s = good(); s.d[1].r = 3;                TEST("drum right index out of range refused", !parses(build(s)));
        s = good(); s.z[1].first = 3;            TEST("zone sample index out of range refused", !parses(build(s)));
        s = good(); s.m[1].first = 3;            TEST("multisample zones running past the table refused", !parses(build(s)));
        s = good(); s.m[0].count = 255;          TEST("zone count 255 past the table refused", !parses(build(s)));
        s = good(); s.m[1].first = 0xFFFFFFFF;   TEST("first zone 0xFFFFFFFF refused (no 32-bit wrap)", !parses(build(s)));
        Pa5xKmp o; std::string e; Spec bad = good(); bad.d[0].l = 9;
        TEST("failure leaves the output empty and says why", !parses(build(bad), &o, &e) && o.samples.empty() && e.find("drum record 0") != std::string::npos);
        Spec empty; TEST("an item with all-zero counts is a valid empty KMP", parses(build(empty), &o) && o.multisamples.empty());
        Bytes n = build(good()); for (int i = 0; i < 32; ++i) n[18 + 75 + i] = 'x';
        TEST("a 32-byte name with no NUL is bounded to 32", parses(n, &o) && o.samples[0].name.size() == 32);
    }
    if (const char* path = std::getenv("AIARR_PA5X_RAM_KMP")) {   // private Pa5X backup file; read only
        std::ifstream f(path, std::ios::binary); Bytes raw((std::istreambuf_iterator<char>(f)), {});
        const auto c = parse(raw.data(), raw.size());
        const Item* it = nullptr; if (c.ok) for (const Item& x : c.items) if (x.chunkId == kPa5xKmpChunkId) it = &x;
        Pa5xKmp k; std::string e;
        const bool ok = it && parsePa5xKmp(raw.data() + it->offset + 8, it->size, k, &e);
        TEST("REAL DATA: private RAM.KMP parses", ok);
        if (ok) {
            std::printf("  real file counts: samples %zu drums %zu zones %zu multisamples %zu contiguous %d\n", k.samples.size(), k.drums.size(), k.zones.size(), k.multisamples.size(), int(k.zonesContiguous));
            std::set<std::array<uint8_t, 8>> ids; for (auto& m : k.multisamples) ids.insert(m.id);
            size_t zsum = 0; for (auto& m : k.multisamples) zsum += m.zoneCount;
            TEST("REAL DATA: multisample IDs distinct and zone counts sum to the zone table", ids.size() == k.multisamples.size() && zsum == k.zones.size() && k.zonesContiguous);
        }
    } else std::printf("  REAL-DATA CHECK SKIPPED (AIARR_PA5X_RAM_KMP not set)\n");
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
