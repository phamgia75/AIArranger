// P3.5 SF2 overlap check: synthetic SoundFont 2 hydras built in memory.
#include "media/sf2_check.h"
#include <cstdio>
#include <string>

using namespace ai_arranger::media::sf2;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

using Bytes = std::vector<uint8_t>;
static void le16(Bytes& b, unsigned v) { b.push_back(v & 255); b.push_back(v >> 8 & 255); }
static void le32(Bytes& b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }
static void name20(Bytes& b, const std::string& s) { for (size_t i = 0; i < 20; ++i) b.push_back(i < s.size() ? s[i] : 0); }
static Bytes chunk(const char* id, const Bytes& body) {
    Bytes c(id, id + 4); le32(c, uint32_t(body.size())); c.insert(c.end(), body.begin(), body.end());
    if (body.size() & 1) c.push_back(0);
    return c;
}

struct Gen { unsigned op, lo, hi; };  // ranges as lo/hi bytes; sampleID as lo | hi << 8
struct Smp { std::string name; unsigned link, type; };

// One instrument per entry; each zone is a generator list.
static Bytes sf2(const std::vector<std::pair<std::string, std::vector<std::vector<Gen>>>>& insts, const std::vector<Smp>& smps) {
    Bytes inst, ibag, igen, shdr;
    unsigned bag = 0, gen = 0;
    for (const auto& [n, zones] : insts) {
        name20(inst, n); le16(inst, bag);
        for (const auto& z : zones) {
            le16(ibag, gen); le16(ibag, 0); ++bag;
            for (const auto& g : z) { le16(igen, g.op); igen.push_back(uint8_t(g.lo)); igen.push_back(uint8_t(g.hi)); ++gen; }
        }
    }
    name20(inst, "EOI"); le16(inst, bag);
    le16(ibag, gen); le16(ibag, 0);
    le32(igen, 0);
    for (const auto& s : smps) {
        name20(shdr, s.name);
        for (int i = 0; i < 5; ++i) le32(shdr, 0);
        shdr.push_back(60); shdr.push_back(0); le16(shdr, s.link); le16(shdr, s.type);
    }
    name20(shdr, "EOS"); shdr.resize(shdr.size() + 26, 0);
    Bytes pdta{'p', 'd', 't', 'a'};
    for (auto c : {chunk("inst", inst), chunk("ibag", ibag), chunk("igen", igen), chunk("shdr", shdr)}) pdta.insert(pdta.end(), c.begin(), c.end());
    Bytes body{'s', 'f', 'b', 'k'};
    for (auto c : {chunk("LIST", Bytes{'I', 'N', 'F', 'O'}), chunk("LIST", pdta)}) body.insert(body.end(), c.begin(), c.end());
    return chunk("RIFF", body);
}

static Gen key(unsigned lo, unsigned hi) { return {43, lo, hi}; }
static Gen vel(unsigned lo, unsigned hi) { return {44, lo, hi}; }
static Gen smp(unsigned i) { return {53, i & 255, i >> 8}; }

int main() {
    const std::vector<Smp> mono{{"a", 0, 1}, {"b", 0, 1}, {"c", 0, 1}};

    auto r = check(sf2({{"Split", {{key(0, 59), smp(0)}, {key(60, 127), smp(1)}}}}, mono));
    TEST("adjacent key ranges do not overlap", r.ok && r.instruments.size() == 1 && r.instruments[0].zones.size() == 2 && r.overlapping() == 0);

    r = check(sf2({{"Layer", {{key(0, 64), smp(0)}, {key(60, 127), smp(1)}}}}, mono));
    TEST("intersecting key ranges overlap", r.ok && r.overlapping() == 1 && r.instruments[0].overlaps[0].a == 0 && r.instruments[0].overlaps[0].b == 1);

    r = check(sf2({{"VelSw", {{key(0, 127), vel(0, 63), smp(0)}, {key(0, 127), vel(64, 127), smp(1)}}}}, mono));
    TEST("same keys, disjoint velocities: velocity switch, no overlap", r.ok && r.overlapping() == 0);

    r = check(sf2({{"Default", {{smp(0)}, {key(10, 20), smp(1)}}}}, mono));
    TEST("zone with no keyRange defaults to 0-127 and overlaps", r.ok && r.overlapping() == 1);

    r = check(sf2({{"Global", {{key(0, 30)}, {smp(0)}, {key(40, 50), smp(1)}}}}, mono));
    TEST("global zone keyRange is the default for local zones; global zone not counted",
         r.ok && r.instruments[0].zones.size() == 2 && r.instruments[0].zones[0].key.hi == 30 && r.overlapping() == 0);

    const std::vector<Smp> stereo{{"L", 1, 4}, {"R", 0, 2}, {"x", 0, 1}};
    r = check(sf2({{"Stereo", {{key(0, 127), smp(0)}, {key(0, 127), smp(1)}}}}, stereo));
    TEST("linked L/R pair with identical ranges is stereo, not an overlap", r.ok && r.overlapping() == 0);
    r = check(sf2({{"StereoOff", {{key(0, 127), smp(0)}, {key(0, 100), smp(1)}}}}, stereo));
    TEST("L/R pair with different ranges is still an overlap", r.ok && r.overlapping() == 1);
    r = check(sf2({{"TwoMono", {{key(0, 127), smp(0)}, {key(0, 127), smp(2)}}}}, stereo));
    TEST("unlinked samples with identical ranges overlap", r.ok && r.overlapping() == 1);

    r = check(sf2({{"A", {{key(0, 59), smp(0)}, {key(60, 127), smp(1)}}}, {"B", {{key(0, 127), smp(0)}, {key(0, 127), smp(1)}}}}, mono));
    TEST("only the overlapping instrument is flagged", r.ok && r.instruments.size() == 2 && r.instruments[0].overlaps.empty() && r.instruments[1].overlaps.size() == 1 &&
         format(r).find("OVERLAP  B") != std::string::npos && format(r).find("ok       A") != std::string::npos);

    // Untrusted input: malformed files are reported, never read out of bounds.
    TEST("not RIFF is rejected", !check(Bytes(20, 0)).ok);
    auto good = sf2({{"A", {{key(0, 59), smp(0)}}}}, mono);
    auto trunc = good; trunc.resize(trunc.size() - 30);
    TEST("truncated file is rejected", !check(trunc).ok);
    r = check(sf2({{"Bad", {{key(0, 59), smp(9)}}}}, mono));
    TEST("sample index past shdr is rejected", !r.ok && r.error.find("sample index") != std::string::npos);
    for (size_t cut = 0; cut < good.size(); ++cut) check(Bytes(good.begin(), good.begin() + cut));  // ASan/UBSan job checks this
    TEST("every prefix of a valid file parses or fails cleanly", true);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
