// M2e-1 Exhaustive Synthetic Tests: Portable SMF Bank/PC Remapper & Root-Tree Round-Trip Verifier
//
// 100% synthetic, original fixtures only (zero private owner data, zero real Sound/Style/KSC/Pad
// names, zero proprietary byte dumps, zero local filesystem paths). Strictly in-memory tests.

#include "media/panative/smf_remap.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace ai_arranger::media::panative;
using namespace ai_arranger::media::panative::smf_remap;

static int failures = 0, passes = 0;
static void check(const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "  FAIL: %s\n", name);
        ++failures;
    } else {
        std::printf("  PASS: %s\n", name);
        ++passes;
    }
}
#define TEST(name, ...) check((name), (__VA_ARGS__))

using Bytes = std::vector<uint8_t>;

// ── Synthetic SMF & Root-Tree Builders (100% original test-only helpers) ──────────────────────────────────────────────

static void pushBe16(Bytes& b, uint16_t v) {
    b.push_back(uint8_t((v >> 8) & 0xFFu));
    b.push_back(uint8_t(v & 0xFFu));
}

static void pushBe32(Bytes& b, uint32_t v) {
    b.push_back(uint8_t((v >> 24) & 0xFFu));
    b.push_back(uint8_t((v >> 16) & 0xFFu));
    b.push_back(uint8_t((v >> 8) & 0xFFu));
    b.push_back(uint8_t(v & 0xFFu));
}

static void pushVlq(Bytes& b, uint32_t v) {
    uint8_t buf[4];
    int n = 0;
    buf[n++] = uint8_t(v & 0x7Fu);
    while ((v >>= 7) != 0u) {
        buf[n++] = uint8_t((v & 0x7Fu) | 0x80u);
    }
    while (n > 0) b.push_back(buf[--n]);
}

static void pushBytes(Bytes& dst, const std::initializer_list<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

static void pushEot(Bytes& trk, uint32_t delta = 0) {
    pushVlq(trk, delta);
    pushBytes(trk, {0xFF, 0x2F, 0x00});
}

static Bytes buildSmf(uint16_t format, const std::vector<Bytes>& tracks,
                      uint16_t division = 0x0180, const Bytes& extraHeader = {}) {
    Bytes out{'M', 'T', 'h', 'd'};
    pushBe32(out, uint32_t(6u + extraHeader.size()));
    pushBe16(out, format);
    pushBe16(out, uint16_t(tracks.size()));
    pushBe16(out, division);
    out.insert(out.end(), extraHeader.begin(), extraHeader.end());
    for (const auto& t : tracks) {
        out.push_back('M');
        out.push_back('T');
        out.push_back('r');
        out.push_back('k');
        pushBe32(out, uint32_t(t.size()));
        out.insert(out.end(), t.begin(), t.end());
    }
    return out;
}

static Bytes makeSimpleSmf(uint8_t ch0, uint8_t msb, uint8_t lsb, uint8_t pc) {
    Bytes trk;
    pushBytes(trk, {0x00, uint8_t(0xB0u | ch0), 0x00, msb});
    pushBytes(trk, {0x00, uint8_t(0xB0u | ch0), 0x20, lsb});
    pushBytes(trk, {0x00, uint8_t(0xC0u | ch0), pc});
    pushEot(trk);
    return buildSmf(0, {trk});
}

static Bytes makeRootNode(const std::string& tag, const std::string& name, const Bytes& payload) {
    Bytes out;
    out.insert(out.end(), tag.begin(), tag.end());
    out.push_back(0x00);
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0x00);
    pushBe32(out, uint32_t(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

static Bytes makeLeaf(const std::string& name, const Bytes& payload) {
    return makeRootNode("Leaf", name, payload);
}

static Bytes makeLeafStr(const std::string& name, const std::string& payload) {
    return makeRootNode("Leaf", name, Bytes(payload.begin(), payload.end()));
}

static Bytes concatBytes(const std::vector<Bytes>& parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

static Bytes makeBranch(const std::string& name, const std::vector<Bytes>& kids) {
    return makeRootNode("Branch", name, concatBytes(kids));
}

static Bytes makeRoot(const std::vector<Bytes>& kids) {
    return makeRootNode("Root", "\x01", concatBytes(kids));
}

static std::vector<closure::Select> readSelects(const Bytes& smf, bool* okOut = nullptr) {
    std::vector<closure::Select> sel;
    std::string err;
    const bool ok = closure::selects(smf.data(), smf.size(), sel, err);
    if (okOut) *okOut = ok;
    return sel;
}

static bool hasSubseq(const Bytes& haystack, const Bytes& needle) {
    if (needle.empty()) return true;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

// ── Tests ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

static void testFormat0And1Basic() {
    std::printf("Section 1: Format 0 & Format 1 basic single/multi-channel remaps\n");

    // Format 0 single channel in-place (CC0, CC32, PC)
    const Bytes smf0 = makeSimpleSmf(0, 121, 64, 5);
    const std::map<MidiTriple, MidiTriple> rules = {
        {{121, 64, 5}, {121, 67, 12}}
    };
    const auto r0 = remapSmf(smf0.data(), smf0.size(), rules);
    bool selOk = false;
    const auto sel0 = readSelects(r0.bytes, &selOk);
    TEST("Format 0 basic remap succeeds and preserves byte length (in-place)",
         r0.ok && r0.modified && r0.remappedEvents == 1 && r0.unchangedEvents == 0 &&
         r0.bytes.size() == smf0.size() && selOk && sel0.size() == 1 &&
         sel0[0].channel == 1 && sel0[0].msb == 121 && sel0[0].lsb == 67 && sel0[0].program == 12);

    // Format 0 reversed bank order (CC32 before CC0) in-place
    Bytes trkRev;
    pushBytes(trkRev, {0x00, 0xB2, 0x20, 65});
    pushBytes(trkRev, {0x00, 0xB2, 0x00, 121});
    pushBytes(trkRev, {0x00, 0xC2, 9});
    pushEot(trkRev);
    const Bytes smfRev = buildSmf(0, {trkRev});
    const auto rRev = remapSmf(smfRev.data(), smfRev.size(), {{{121, 65, 9}, {120, 66, 3}}});
    const auto selRev = readSelects(rRev.bytes, &selOk);
    TEST("Format 0 reversed CC32/CC0 order remaps cleanly in-place",
         rRev.ok && rRev.modified && rRev.bytes.size() == smfRev.size() && selOk &&
         selRev.size() == 1 && selRev[0].channel == 3 &&
         selRev[0].msb == 120 && selRev[0].lsb == 66 && selRev[0].program == 3);

    // PC-only remap (same MSB & LSB, only PC changes)
    const auto rPcOnly = remapSmf(smf0.data(), smf0.size(), {{{121, 64, 5}, {121, 64, 99}}});
    const auto selPcOnly = readSelects(rPcOnly.bytes, &selOk);
    TEST("PC-only remap (unchanged bank) rewrites in-place cleanly",
         rPcOnly.ok && rPcOnly.modified && rPcOnly.bytes.size() == smf0.size() && selOk &&
         selPcOnly.size() == 1 && selPcOnly[0].msb == 121 && selPcOnly[0].lsb == 64 && selPcOnly[0].program == 99);

    // Format 1 multi-track multi-channel remap (ch 1, ch 10 drum, ch 16)
    Bytes t0, t1, t2;
    pushBytes(t0, {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});  // Tempo meta
    pushEot(t0);
    pushBytes(t1, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5});
    pushBytes(t1, {0x00, 0xB9, 0x00, 120, 0x00, 0xB9, 0x20, 64, 0x00, 0xC9, 2});
    pushEot(t1);
    pushBytes(t2, {0x00, 0xBF, 0x00, 121, 0x00, 0xBF, 0x20, 1, 0x00, 0xCF, 7});  // Factory untouched
    pushEot(t2);
    const Bytes smf1 = buildSmf(1, {t0, t1, t2});
    const std::map<MidiTriple, MidiTriple> multiRules = {
        {{121, 64, 5}, {121, 70, 44}},
        {{120, 64, 2}, {120, 68, 19}}
    };
    const auto r1 = remapSmf(smf1.data(), smf1.size(), multiRules);
    const auto sel1 = readSelects(r1.bytes, &selOk);
    TEST("Format 1 multi-channel remaps matched channels and leaves Factory untouched",
         r1.ok && r1.modified && r1.remappedEvents == 2 && r1.unchangedEvents == 1 && selOk &&
         sel1.size() == 3 &&
         sel1[0].channel == 1 && sel1[0].msb == 121 && sel1[0].lsb == 70 && sel1[0].program == 44 &&
         sel1[1].channel == 10 && sel1[1].msb == 120 && sel1[1].lsb == 68 && sel1[1].program == 19 &&
         sel1[2].channel == 16 && sel1[2].msb == 121 && sel1[2].lsb == 1 && sel1[2].program == 7);

    // Non-adjacent CC0/CC32 separated from PC by a Note-On (triggers general rewrite, preserves Note-On & deltas)
    Bytes trkNonAdj;
    pushBytes(trkNonAdj, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64});
    pushBytes(trkNonAdj, {0x18, 0x90, 0x3C, 0x60});  // intervening Note-On at delta 24
    pushBytes(trkNonAdj, {0x24, 0xC0, 5});           // PC at delta 36 (remapped -> (121, 70, 44))
    pushBytes(trkNonAdj, {0x10, 0x80, 0x3C, 0x00});
    pushEot(trkNonAdj);
    const Bytes smfNonAdj = buildSmf(0, {trkNonAdj});
    const auto rNonAdj = remapSmf(smfNonAdj.data(), smfNonAdj.size(), {{{121, 64, 5}, {121, 70, 44}}});
    const auto selNonAdj = readSelects(rNonAdj.bytes, &selOk);
    TEST("Non-adjacent bank select separated by Note-On preserves Note-On/delta and remaps cleanly",
         rNonAdj.ok && rNonAdj.modified && rNonAdj.remappedEvents == 1 && selOk && selNonAdj.size() == 1 &&
         selNonAdj[0].msb == 121 && selNonAdj[0].lsb == 70 && selNonAdj[0].program == 44 &&
         hasSubseq(rNonAdj.bytes, {0x18, 0x90, 0x3C, 0x60, 0x24, 0xB0, 0x00, 121}) &&
         hasSubseq(rNonAdj.bytes, {0x10, 0x80, 0x3C, 0x00}));

    // MidiTriple validity and ordering checks
    const MidiTriple tMin{0, 0, 0};
    const MidiTriple tMax{127, 127, 127};
    const MidiTriple tBad{128, 0, 0};
    TEST("MidiTriple valid() and strict <=> / == comparisons work across 0..127 boundary",
         tMin.valid() && tMax.valid() && !tBad.valid() && tMin < tMax && !(tMax < tMin) &&
         tMin == MidiTriple{0, 0, 0} && tMin != tMax);
}

static void testSharedBankRestoration() {
    std::printf("Section 2: Shared (CC0, CC32) bank restoration & partial CC override\n");

    // One (CC0=121, CC32=64) followed by PC_A (remapped) then PC_B (unremapped)
    Bytes trk;
    pushBytes(trk, {0x00, 0xB0, 0x00, 121});
    pushBytes(trk, {0x00, 0xB0, 0x20, 64});
    pushBytes(trk, {0x10, 0xC0, 5});   // PC_A remapped -> (120, 69, 33)
    pushBytes(trk, {0x20, 0xC0, 6});   // PC_B unremapped -> must still see (121, 64, 6)!
    pushEot(trk);
    const Bytes smf = buildSmf(0, {trk});
    const auto res = remapSmf(smf.data(), smf.size(), {{{121, 64, 5}, {120, 69, 33}}});
    bool selOk = false;
    const auto sel = readSelects(res.bytes, &selOk);
    TEST("Shared bank: PC_A remapped and PC_B sees restored original (MSB, LSB)",
         res.ok && res.modified && res.remappedEvents == 1 && res.unchangedEvents == 1 && selOk &&
         sel.size() == 2 &&
         sel[0].msb == 120 && sel[0].lsb == 69 && sel[0].program == 33 &&
         sel[1].msb == 121 && sel[1].lsb == 64 && sel[1].program == 6);

    // Partial CC override: CC0=121, CC32=64 -> PC=5 (remapped to MSB=120, LSB=69, PC=1) ->
    // later CC32=66 only (relying on CC0 still being 121!) -> PC=7
    Bytes trkPartial;
    pushBytes(trkPartial, {0x00, 0xB0, 0x00, 121});
    pushBytes(trkPartial, {0x00, 0xB0, 0x20, 64});
    pushBytes(trkPartial, {0x00, 0xC0, 5});
    pushBytes(trkPartial, {0x40, 0xB0, 0x20, 66});  // Updates LSB only; depends on MSB=121!
    pushBytes(trkPartial, {0x00, 0xC0, 7});
    pushEot(trkPartial);
    const Bytes smfPartial = buildSmf(0, {trkPartial});
    const auto rPartial = remapSmf(smfPartial.data(), smfPartial.size(), {{{121, 64, 5}, {120, 69, 1}}});
    const auto selPartial = readSelects(rPartial.bytes, &selOk);
    TEST("Partial later CC32 update still sees restored CC0 (MSB=121)",
         rPartial.ok && rPartial.modified && rPartial.remappedEvents == 1 && rPartial.unchangedEvents == 1 &&
         selOk && selPartial.size() == 2 &&
         selPartial[0].msb == 120 && selPartial[0].lsb == 69 && selPartial[0].program == 1 &&
         selPartial[1].msb == 121 && selPartial[1].lsb == 66 && selPartial[1].program == 7);

    // Three PCs sharing one bank: PC1 remapped (bank changes), PC2 remapped (PC-only), PC3 unremapped
    Bytes trk3;
    pushBytes(trk3, {0x00, 0xB1, 0x00, 121, 0x00, 0xB1, 0x20, 64});
    pushBytes(trk3, {0x00, 0xC1, 1});
    pushBytes(trk3, {0x00, 0xC1, 2});
    pushBytes(trk3, {0x00, 0xC1, 3});
    pushEot(trk3);
    const Bytes smf3 = buildSmf(0, {trk3});
    const auto r3 = remapSmf(smf3.data(), smf3.size(), {
        {{121, 64, 1}, {121, 65, 10}},
        {{121, 64, 2}, {121, 64, 20}}
    });
    const auto sel3 = readSelects(r3.bytes, &selOk);
    TEST("Three PCs on shared bank: bank-change remap + PC-only remap + unremapped all exact",
         r3.ok && r3.modified && r3.remappedEvents == 2 && r3.unchangedEvents == 1 && selOk &&
         sel3.size() == 3 &&
         sel3[0].msb == 121 && sel3[0].lsb == 65 && sel3[0].program == 10 &&
         sel3[1].msb == 121 && sel3[1].lsb == 64 && sel3[1].program == 20 &&
         sel3[2].msb == 121 && sel3[2].lsb == 64 && sel3[2].program == 3);
}

static void testCrossTrackBankSelect() {
    std::printf("Section 3: Format 1 cross-track bank select\n");

    // Track 0 sets CC0/CC32 on ch 1 at tick 0; Track 1 emits PC_A (remapped) at tick 0;
    // Track 2 emits PC_B (unremapped) at tick 0 and PC_C (unremapped) at tick 120.
    Bytes t0, t1, t2;
    pushBytes(t0, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64});
    pushEot(t0);
    pushBytes(t1, {0x00, 0xC0, 10});   // remapped -> (121, 71, 55)
    pushEot(t1);
    pushBytes(t2, {0x00, 0xC0, 11});   // unremapped -> must see (121, 64, 11)
    pushVlq(t2, 120);
    pushBytes(t2, {0xC0, 12});         // unremapped -> must see (121, 64, 12)
    pushEot(t2);

    const Bytes smf = buildSmf(1, {t0, t1, t2});
    const auto res = remapSmf(smf.data(), smf.size(), {{{121, 64, 10}, {121, 71, 55}}});
    bool selOk = false;
    const auto sel = readSelects(res.bytes, &selOk);
    TEST("Format 1 cross-track bank select restores bank for Track 2 PCs",
         res.ok && res.modified && res.remappedEvents == 1 && res.unchangedEvents == 2 && selOk &&
         sel.size() == 3 &&
         sel[0].msb == 121 && sel[0].lsb == 71 && sel[0].program == 55 &&
         sel[1].msb == 121 && sel[1].lsb == 64 && sel[1].program == 11 &&
         sel[2].msb == 121 && sel[2].lsb == 64 && sel[2].program == 12);

    // Split CC0 on Track 0, CC32 + remapped PC on Track 1, unremapped PC on Track 0 at tick 50
    Bytes s0, s1;
    pushBytes(s0, {0x00, 0xB4, 0x00, 121});
    pushVlq(s0, 50);
    pushBytes(s0, {0xC4, 8});   // tick 50: must still see (121, 66, 8)
    pushEot(s0);
    pushBytes(s1, {0x00, 0xB4, 0x20, 66, 0x00, 0xC4, 4});  // tick 0: remapped -> (120, 65, 9)
    pushEot(s1);
    const Bytes smfSplit = buildSmf(1, {s0, s1});
    const auto rSplit = remapSmf(smfSplit.data(), smfSplit.size(), {{{121, 66, 4}, {120, 65, 9}}});
    const auto selSplit = readSelects(rSplit.bytes, &selOk);
    TEST("Format 1 split CC0/CC32 across tracks with later PC on Track 0 restores both MSB and LSB",
         rSplit.ok && rSplit.modified && rSplit.remappedEvents == 1 && rSplit.unchangedEvents == 1 &&
         selOk && selSplit.size() == 2 &&
         selSplit[0].channel == 5 && selSplit[0].msb == 120 && selSplit[0].lsb == 65 && selSplit[0].program == 9 &&
         selSplit[1].channel == 5 && selSplit[1].msb == 121 && selSplit[1].lsb == 66 && selSplit[1].program == 8);
}

static void testRunningStatusHandling() {
    std::printf("Section 4: Running status preservation & expansion\n");

    // 0xB0 running status + 0xC0 running status across remapped PC followed by unremapped PC,
    // followed by Note-On running status run.
    Bytes trk;
    pushBytes(trk, {0x00, 0xB0, 0x00, 121});
    pushBytes(trk, {0x00, 0x20, 64});          // running status 0xB0 -> CC32=64
    pushBytes(trk, {0x00, 0xC0, 5});           // PC=5 (remapped -> (121, 68, 22))
    pushBytes(trk, {0x00, 6});                 // running status 0xC0 -> PC=6 (unremapped! needs explicit 0xC0 after restore)
    pushBytes(trk, {0x10, 0x90, 0x3C, 0x64});  // Note-On
    pushBytes(trk, {0x20, 0x3C, 0x00});        // running status 0x90 -> Note-Off (vel 0)
    pushBytes(trk, {0x00, 0x40, 0x50});        // running status 0x90 -> Note-On
    pushEot(trk);

    const Bytes smf = buildSmf(0, {trk});
    const auto res = remapSmf(smf.data(), smf.size(), {{{121, 64, 5}, {121, 68, 22}}});
    bool selOk = false;
    const auto sel = readSelects(res.bytes, &selOk);
    TEST("Running status 0xC0 expands safely after bank restore and Note-On run survives",
         res.ok && res.modified && res.remappedEvents == 1 && res.unchangedEvents == 1 && selOk &&
         sel.size() == 2 &&
         sel[0].msb == 121 && sel[0].lsb == 68 && sel[0].program == 22 &&
         sel[1].msb == 121 && sel[1].lsb == 64 && sel[1].program == 6 &&
         hasSubseq(res.bytes, {0x10, 0x90, 0x3C, 0x64, 0x20, 0x3C, 0x00, 0x00, 0x40, 0x50}));

    // CC running status immediately after a remapped PC is impossible in valid SMF,
    // but CC running status BEFORE a remapped PC (CC7 -> CC0 -> CC32 -> PC) must expand/preserve cleanly!
    Bytes trkCcRun;
    pushBytes(trkCcRun, {0x00, 0xB0, 0x07, 100});  // CC7
    pushBytes(trkCcRun, {0x00, 0x00, 121});        // running status 0xB0 -> CC0=121
    pushBytes(trkCcRun, {0x00, 0x20, 64});         // running status 0xB0 -> CC32=64
    pushBytes(trkCcRun, {0x00, 0xC0, 5});          // PC=5 (remapped in-place)
    pushEot(trkCcRun);
    const Bytes smfCcRun = buildSmf(0, {trkCcRun});
    const auto rCcRun = remapSmf(smfCcRun.data(), smfCcRun.size(), {{{121, 64, 5}, {121, 69, 30}}});
    const auto selCcRun = readSelects(rCcRun.bytes, &selOk);
    TEST("CC running status run feeding into in-place remapped PC preserves running status & length",
         rCcRun.ok && rCcRun.modified && rCcRun.bytes.size() == smfCcRun.size() && selOk &&
         selCcRun.size() == 1 && selCcRun[0].msb == 121 && selCcRun[0].lsb == 69 && selCcRun[0].program == 30 &&
         hasSubseq(rCcRun.bytes, {0x00, 0xB0, 0x07, 100}));

    // Running-status remapped PC (omitted 0xC0 status byte on the remapped PC itself),
    // followed by a running-status PC-only remapped PC and a running-status unremapped PC!
    Bytes trkRsRemap;
    pushBytes(trkRsRemap, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64});
    pushBytes(trkRsRemap, {0x00, 0xC0, 1});  // explicit 0xC0, unremapped
    pushBytes(trkRsRemap, {0x08, 2});        // running status 0xC0 -> remapped (bank changes + restore!)
    pushBytes(trkRsRemap, {0x08, 3});        // running status 0xC0 -> PC-only remapped (needs explicit 0xC0 after restore!)
    pushBytes(trkRsRemap, {0x08, 4});        // running status 0xC0 -> unremapped (inherits restored 0xC0!)
    pushEot(trkRsRemap);
    const Bytes smfRsRemap = buildSmf(0, {trkRsRemap});
    const auto rRsRemap = remapSmf(smfRsRemap.data(), smfRsRemap.size(), {
        {{121, 64, 2}, {121, 70, 22}},
        {{121, 64, 3}, {121, 64, 33}}
    });
    const auto selRsRemap = readSelects(rRsRemap.bytes, &selOk);
    TEST("Running-status remapped PC + following running-status PC-only remap + trailing unremapped PC",
         rRsRemap.ok && rRsRemap.modified && rRsRemap.remappedEvents == 2 && rRsRemap.unchangedEvents == 2 &&
         selOk && selRsRemap.size() == 4 &&
         selRsRemap[0].msb == 121 && selRsRemap[0].lsb == 64 && selRsRemap[0].program == 1 &&
         selRsRemap[1].msb == 121 && selRsRemap[1].lsb == 70 && selRsRemap[1].program == 22 &&
         selRsRemap[2].msb == 121 && selRsRemap[2].lsb == 64 && selRsRemap[2].program == 33 &&
         selRsRemap[3].msb == 121 && selRsRemap[3].lsb == 64 && selRsRemap[3].program == 4);
}

static void testSimultaneousSwapAndIdentityRules() {
    std::printf("Section 5: Simultaneous swap (A <-> B), shared-bank swap, chain, 3-cycle, and identity rules\n");

    const MidiTriple A{121, 64, 1};
    const MidiTriple B{121, 65, 2};
    const MidiTriple C{121, 66, 3};

    // Separate-bank simultaneous swap A -> B and B -> A in one SMF
    Bytes trk;
    pushBytes(trk, {0x00, 0xB0, 0x00, A.msb, 0x00, 0xB0, 0x20, A.lsb, 0x00, 0xC0, A.pc});
    pushBytes(trk, {0x10, 0xB0, 0x00, B.msb, 0x00, 0xB0, 0x20, B.lsb, 0x00, 0xC0, B.pc});
    pushEot(trk);
    const Bytes smfSwap = buildSmf(0, {trk});
    const auto rSwap = remapSmf(smfSwap.data(), smfSwap.size(), {{A, B}, {B, A}});
    bool selOk = false;
    const auto selSwap = readSelects(rSwap.bytes, &selOk);
    TEST("Separate-bank simultaneous swap A <-> B executes in single pass with zero double-remap",
         rSwap.ok && rSwap.modified && rSwap.remappedEvents == 2 && rSwap.unchangedEvents == 0 &&
         selOk && selSwap.size() == 2 &&
         selSwap[0].msb == B.msb && selSwap[0].lsb == B.lsb && selSwap[0].program == B.pc &&
         selSwap[1].msb == A.msb && selSwap[1].lsb == A.lsb && selSwap[1].program == A.pc);

    // Shared-bank simultaneous swap on a single (CC0=121, CC32=64) run + trailing unremapped PC
    const MidiTriple S1{121, 64, 1};
    const MidiTriple S2{121, 64, 2};
    Bytes trkSharedSwap;
    pushBytes(trkSharedSwap, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64});
    pushBytes(trkSharedSwap, {0x00, 0xC0, 1, 0x10, 0xC0, 2, 0x10, 0xC0, 3});
    pushEot(trkSharedSwap);
    const Bytes smfSharedSwap = buildSmf(0, {trkSharedSwap});
    const auto rSharedSwap = remapSmf(smfSharedSwap.data(), smfSharedSwap.size(), {
        {S1, S2},
        {S2, {121, 67, 9}}
    });
    const auto selSharedSwap = readSelects(rSharedSwap.bytes, &selOk);
    TEST("Shared-bank swap/chain (S1 -> S2, S2 -> (121,67,9)) + unremapped S3 has zero double-remap",
         rSharedSwap.ok && rSharedSwap.modified && rSharedSwap.remappedEvents == 2 &&
         rSharedSwap.unchangedEvents == 1 && selOk && selSharedSwap.size() == 3 &&
         selSharedSwap[0].msb == 121 && selSharedSwap[0].lsb == 64 && selSharedSwap[0].program == 2 &&
         selSharedSwap[1].msb == 121 && selSharedSwap[1].lsb == 67 && selSharedSwap[1].program == 9 &&
         selSharedSwap[2].msb == 121 && selSharedSwap[2].lsb == 64 && selSharedSwap[2].program == 3);

    // Non-cyclic chain A -> B, B -> C (verifies A becomes B, NOT C!)
    Bytes trkChain;
    pushBytes(trkChain, {0x00, 0xB0, 0x00, A.msb, 0x00, 0xB0, 0x20, A.lsb, 0x00, 0xC0, A.pc});
    pushBytes(trkChain, {0x10, 0xB0, 0x00, B.msb, 0x00, 0xB0, 0x20, B.lsb, 0x00, 0xC0, B.pc});
    pushEot(trkChain);
    const Bytes smfChain = buildSmf(0, {trkChain});
    const auto rChain = remapSmf(smfChain.data(), smfChain.size(), {{A, B}, {B, C}});
    const auto selChain = readSelects(rChain.bytes, &selOk);
    TEST("Non-cyclic chain A -> B, B -> C maps A->B and B->C without double-remapping A->C",
         rChain.ok && rChain.modified && rChain.remappedEvents == 2 && selOk && selChain.size() == 2 &&
         selChain[0].msb == B.msb && selChain[0].lsb == B.lsb && selChain[0].program == B.pc &&
         selChain[1].msb == C.msb && selChain[1].lsb == C.lsb && selChain[1].program == C.pc);

    // 3-way cycle A -> B -> C -> A on same channel
    Bytes trkCycle;
    pushBytes(trkCycle, {0x00, 0xB3, 0x00, A.msb, 0x00, 0xB3, 0x20, A.lsb, 0x00, 0xC3, A.pc});
    pushBytes(trkCycle, {0x00, 0xB3, 0x00, B.msb, 0x00, 0xB3, 0x20, B.lsb, 0x00, 0xC3, B.pc});
    pushBytes(trkCycle, {0x00, 0xB3, 0x00, C.msb, 0x00, 0xB3, 0x20, C.lsb, 0x00, 0xC3, C.pc});
    pushEot(trkCycle);
    const Bytes smfCycle = buildSmf(0, {trkCycle});
    const auto rCycle = remapSmf(smfCycle.data(), smfCycle.size(), {{A, B}, {B, C}, {C, A}});
    const auto selCycle = readSelects(rCycle.bytes, &selOk);
    TEST("3-way cycle A -> B -> C -> A remaps without chaining",
         rCycle.ok && rCycle.modified && rCycle.remappedEvents == 3 && selOk && selCycle.size() == 3 &&
         selCycle[0].lsb == B.lsb && selCycle[0].program == B.pc &&
         selCycle[1].lsb == C.lsb && selCycle[1].program == C.pc &&
         selCycle[2].lsb == A.lsb && selCycle[2].program == A.pc);

    // Valid identity rule (A -> A) is cleanly ignored as no-op (byte-identical output)
    const Bytes smfId = makeSimpleSmf(0, A.msb, A.lsb, A.pc);
    const auto rId = remapSmf(smfId.data(), smfId.size(), {{A, A}});
    TEST("Valid identity rule A -> A is ignored as no-op with byte-identical output",
         rId.ok && !rId.modified && rId.remappedEvents == 0 && rId.unchangedEvents == 1 &&
         rId.bytes == smfId);
}

static void testSysExMetaAndEventPreservation() {
    std::printf("Section 6: Byte-identical preservation of Korg SysEx, Meta, Notes, CCs, MThd\n");

    const Bytes korgSysex = {0xF0, 0x07, 0x42, 0x30, 0x00, 0x01, 0x10, 0x00, 0xF7};
    const Bytes contSysex = {0xF7, 0x03, 0x01, 0x02, 0xF7};
    const Bytes tempoMeta = {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20};
    const Bytes markerMeta = {0xFF, 0x06, 0x06, 'V', 'a', 'r', '1', '_', 'A'};
    const Bytes trackNameMeta = {0xFF, 0x03, 0x05, 'S', 'y', 'n', 't', 'h'};
    const Bytes noteAndCcBlock = {
        0x81, 0x00, 0x90, 0x3C, 0x70,   // multi-byte VLQ delta 128 + Note-On
        0x40, 0xB0, 0x07, 0x64,         // CC7 Volume
        0x00, 0xB0, 0x0A, 0x40,         // CC10 Pan
        0x00, 0xD0, 0x30,               // Channel Pressure
        0x00, 0xE0, 0x00, 0x40,         // Pitch Bend
        0x40, 0x80, 0x3C, 0x00          // Note-Off
    };
    const Bytes extraHdr = {0xDE, 0xAD, 0xBE, 0xEF};

    Bytes trk;
    trk.push_back(0x00); trk.insert(trk.end(), trackNameMeta.begin(), trackNameMeta.end());
    trk.push_back(0x00); trk.insert(trk.end(), tempoMeta.begin(), tempoMeta.end());
    trk.push_back(0x00); trk.insert(trk.end(), markerMeta.begin(), markerMeta.end());
    trk.push_back(0x00); trk.insert(trk.end(), korgSysex.begin(), korgSysex.end());
    trk.push_back(0x00); trk.insert(trk.end(), contSysex.begin(), contSysex.end());
    pushBytes(trk, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5});
    trk.insert(trk.end(), noteAndCcBlock.begin(), noteAndCcBlock.end());
    pushEot(trk, 12);

    const Bytes smf = buildSmf(0, {trk}, 0x0180, extraHdr);
    const auto res = remapSmf(smf.data(), smf.size(), {{{121, 64, 5}, {121, 72, 18}}});
    TEST("Korg SysEx, F7 SysEx, Meta events, Notes/CCs/PB/Aftertouch, and extended MThd preserved byte-for-byte",
         res.ok && res.modified && res.remappedEvents == 1 &&
         res.bytes.size() == smf.size() &&
         std::equal(smf.begin(), smf.begin() + 18, res.bytes.begin()) &&
         hasSubseq(res.bytes, korgSysex) &&
         hasSubseq(res.bytes, contSysex) &&
         hasSubseq(res.bytes, tempoMeta) &&
         hasSubseq(res.bytes, markerMeta) &&
         hasSubseq(res.bytes, trackNameMeta) &&
         hasSubseq(res.bytes, noteAndCcBlock));
}

static void testNoOpPreservation() {
    std::printf("Section 7: Unmatched leaf returns exact byte-identical input\n");

    Bytes trk;
    pushBytes(trk, {0x00, 0xB0, 0x00, 121, 0x00, 0x20, 64, 0x00, 0xC0, 5});
    pushEot(trk);
    const Bytes smf = buildSmf(0, {trk});

    const auto rUnmatched = remapSmf(smf.data(), smf.size(), {{{121, 64, 99}, {121, 65, 1}}});
    TEST("Unmatched rules return modified=false and byte-identical SMF",
         rUnmatched.ok && !rUnmatched.modified && rUnmatched.remappedEvents == 0 &&
         rUnmatched.unchangedEvents == 1 && rUnmatched.bytes == smf);

    const auto rEmptyRules = remapSmf(smf.data(), smf.size(), {});
    TEST("Empty rules map returns modified=false and byte-identical SMF",
         rEmptyRules.ok && !rEmptyRules.modified && rEmptyRules.remappedEvents == 0 &&
         rEmptyRules.unchangedEvents == 1 && rEmptyRules.bytes == smf);

    Bytes trkNoPc;
    pushBytes(trkNoPc, {0x00, 0x90, 0x3C, 0x64, 0x40, 0x80, 0x3C, 0x00});
    pushEot(trkNoPc);
    const Bytes smfNoPc = buildSmf(0, {trkNoPc});
    const auto rNoPc = remapSmf(smfNoPc.data(), smfNoPc.size(), {{{121, 64, 5}, {121, 65, 6}}});
    TEST("SMF with 0 PCs returns modified=false and byte-identical SMF",
         rNoPc.ok && !rNoPc.modified && rNoPc.remappedEvents == 0 && rNoPc.unchangedEvents == 0 &&
         rNoPc.bytes == smfNoPc);
}

static void testFailClosedMalformedAndUnresolved() {
    std::printf("Section 8: Fail-closed malformed MIDI, unresolved bank, invalid rule, event limit\n");

    const std::map<MidiTriple, MidiTriple> oneRule = {{{121, 64, 5}, {121, 65, 6}}};

    // 1. CC11 data byte >= 0x80 (0x80 and 0xFF) — fails even with empty rules!
    auto makeBadCc11 = [](uint8_t badVal) {
        Bytes t;
        pushBytes(t, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5});
        pushBytes(t, {0x10, 0xB0, 0x0B, badVal});  // CC11 Expression with high bit set
        pushEot(t);
        return buildSmf(0, {t});
    };
    const Bytes badCc11_80 = makeBadCc11(0x80);
    const Bytes badCc11_FF = makeBadCc11(0xFF);
    const auto rBad80 = remapSmf(badCc11_80.data(), badCc11_80.size(), {});
    const auto rBadFF = remapSmf(badCc11_FF.data(), badCc11_FF.size(), oneRule);
    TEST("CC11 data byte 0x80 fails closed with MALFORMED_MIDI_LEAF even with empty rules",
         !rBad80.ok && rBad80.bytes.empty() && rBad80.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);
    TEST("CC11 data byte 0xFF fails closed with MALFORMED_MIDI_LEAF",
         !rBadFF.ok && rBadFF.bytes.empty() && rBadFF.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    // 2. Truncated VLQ
    Bytes tTruncVlq;
    pushBytes(tTruncVlq, {0x81, 0x82, 0x83, 0x84, 0x00});
    const Bytes smfTruncVlq = buildSmf(0, {tTruncVlq});
    const auto rTruncVlq = remapSmf(smfTruncVlq.data(), smfTruncVlq.size(), {});
    TEST("5-byte VLQ fails closed with MALFORMED_MIDI_LEAF",
         !rTruncVlq.ok && rTruncVlq.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    // 3. Event after EndOfTrack
    Bytes tAfterEot;
    pushBytes(tAfterEot, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5});
    pushEot(tAfterEot);
    pushBytes(tAfterEot, {0x00, 0x90, 0x3C, 0x40});
    const Bytes smfAfterEot = buildSmf(0, {tAfterEot});
    const auto rAfterEot = remapSmf(smfAfterEot.data(), smfAfterEot.size(), {});
    TEST("Event after EndOfTrack fails closed with MALFORMED_MIDI_LEAF",
         !rAfterEot.ok && rAfterEot.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    // 4. Missing EndOfTrack
    Bytes tNoEot;
    pushBytes(tNoEot, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5});
    const Bytes smfNoEot = buildSmf(0, {tNoEot});
    const auto rNoEot = remapSmf(smfNoEot.data(), smfNoEot.size(), {});
    TEST("Missing EndOfTrack fails closed with MALFORMED_MIDI_LEAF",
         !rNoEot.ok && rNoEot.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    // 5. Unknown chunk ("XTrk") & trailing bytes
    Bytes smfGood = makeSimpleSmf(0, 121, 64, 5);
    Bytes smfTrailing = smfGood;
    smfTrailing.push_back(0x00);
    const auto rTrailing = remapSmf(smfTrailing.data(), smfTrailing.size(), {});
    TEST("Trailing bytes fail closed with MALFORMED_MIDI_LEAF",
         !rTrailing.ok && rTrailing.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    Bytes smfUnknownChunk = smfGood;
    smfUnknownChunk[14] = 'X';  // "XTrk"
    const auto rUnknownChunk = remapSmf(smfUnknownChunk.data(), smfUnknownChunk.size(), {});
    TEST("Unknown chunk type fails closed with MALFORMED_MIDI_LEAF",
         !rUnknownChunk.ok && rUnknownChunk.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    // 6. Meta canceling running status followed by statusless channel byte
    Bytes tCancelRs;
    pushBytes(tCancelRs, {0x00, 0x90, 0x3C, 0x64});
    pushBytes(tCancelRs, {0x00, 0xFF, 0x01, 0x01, 'A'});  // cancels running status
    pushBytes(tCancelRs, {0x00, 0x3C, 0x00});             // illegal without status
    pushEot(tCancelRs);
    const Bytes smfCancelRs = buildSmf(0, {tCancelRs});
    const auto rCancelRs = remapSmf(smfCancelRs.data(), smfCancelRs.size(), {});
    TEST("Running status after Meta event fails closed with MALFORMED_MIDI_LEAF",
         !rCancelRs.ok && rCancelRs.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    // 7. Format 2, Format 0 with 2 tracks, empty / null input
    Bytes tEotOnly; pushEot(tEotOnly);
    const Bytes smfFmt2 = buildSmf(2, {tEotOnly});
    const Bytes smfFmt0Two = buildSmf(0, {tEotOnly, tEotOnly});
    TEST("Format 2 SMF fails closed", !remapSmf(smfFmt2.data(), smfFmt2.size(), {}).ok);
    TEST("Format 0 with 2 tracks fails closed", !remapSmf(smfFmt0Two.data(), smfFmt0Two.size(), {}).ok);
    TEST("Null/empty SMF fails closed",
         !remapSmf(nullptr, 10, {}).ok && !remapSmf(smfGood.data(), 0, {}).ok);

    // 8. Unresolved bank select (strictUnresolved = true vs false)
    Bytes tUnresNone, tUnresNoLsb, tUnresNoMsb;
    pushBytes(tUnresNone, {0x00, 0xC0, 5}); pushEot(tUnresNone);
    pushBytes(tUnresNoLsb, {0x00, 0xB0, 0x00, 121, 0x00, 0xC0, 5}); pushEot(tUnresNoLsb);
    pushBytes(tUnresNoMsb, {0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5}); pushEot(tUnresNoMsb);
    const Bytes smfUnresNone = buildSmf(0, {tUnresNone});
    const Bytes smfUnresNoLsb = buildSmf(0, {tUnresNoLsb});
    const Bytes smfUnresNoMsb = buildSmf(0, {tUnresNoMsb});

    const auto rUnres1 = remapSmf(smfUnresNone.data(), smfUnresNone.size(), {});
    const auto rUnres2 = remapSmf(smfUnresNoLsb.data(), smfUnresNoLsb.size(), {});
    const auto rUnres3 = remapSmf(smfUnresNoMsb.data(), smfUnresNoMsb.size(), {});
    TEST("Unresolved PC (missing CC0/CC32) fails closed under strictUnresolved=true",
         !rUnres1.ok && rUnres1.error.find("UNRESOLVED_MIDI_BANK_SELECT") != std::string::npos &&
         !rUnres2.ok && rUnres2.error.find("UNRESOLVED_MIDI_BANK_SELECT") != std::string::npos &&
         !rUnres3.ok && rUnres3.error.find("UNRESOLVED_MIDI_BANK_SELECT") != std::string::npos);

    // Non-strict mode: unresolved PC untouched, subsequent resolved PC remaps cleanly
    Bytes tMixed;
    pushBytes(tMixed, {0x00, 0xC0, 1});  // unresolved (-1, -1, 1)
    pushBytes(tMixed, {0x10, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5});  // remapped
    pushEot(tMixed);
    const Bytes smfMixed = buildSmf(0, {tMixed});
    RemapOptions nonStrict;
    nonStrict.strictUnresolved = false;
    const auto rNonStrict = remapSmf(smfMixed.data(), smfMixed.size(), oneRule, nonStrict);
    bool selOk = false;
    const auto selMixed = readSelects(rNonStrict.bytes, &selOk);
    TEST("strictUnresolved=false leaves unresolved PC untouched and remaps resolved PC",
         rNonStrict.ok && rNonStrict.modified && rNonStrict.remappedEvents == 1 &&
         rNonStrict.unchangedEvents == 1 && selOk && selMixed.size() == 2 &&
         selMixed[0].msb == -1 && selMixed[0].lsb == -1 && selMixed[0].program == 1 &&
         selMixed[1].msb == 121 && selMixed[1].lsb == 65 && selMixed[1].program == 6);

    // 9. Invalid remap rules (MSB/LSB/PC > 127, including invalid identity rule)
    const auto rBadSrcMsb = remapSmf(smfGood.data(), smfGood.size(), {{{128, 0, 0}, {0, 0, 0}}});
    const auto rBadSrcLsb = remapSmf(smfGood.data(), smfGood.size(), {{{0, 200, 0}, {0, 0, 0}}});
    const auto rBadSrcPc  = remapSmf(smfGood.data(), smfGood.size(), {{{0, 0, 128}, {0, 0, 0}}});
    const auto rBadDstMsb = remapSmf(smfGood.data(), smfGood.size(), {{{0, 0, 0}, {128, 0, 0}}});
    const auto rBadDstLsb = remapSmf(smfGood.data(), smfGood.size(), {{{0, 0, 0}, {0, 255, 0}}});
    const auto rBadDstPc  = remapSmf(smfGood.data(), smfGood.size(), {{{0, 0, 0}, {0, 0, 128}}});
    const auto rBadIdent  = remapSmf(smfGood.data(), smfGood.size(), {{{128, 0, 0}, {128, 0, 0}}});
    TEST("Invalid remap rules (> 127) fail closed with INVALID_REMAP_RULE",
         !rBadSrcMsb.ok && rBadSrcMsb.error.find("INVALID_REMAP_RULE") != std::string::npos &&
         !rBadSrcLsb.ok && rBadSrcLsb.error.find("INVALID_REMAP_RULE") != std::string::npos &&
         !rBadSrcPc.ok  && rBadSrcPc.error.find("INVALID_REMAP_RULE")  != std::string::npos &&
         !rBadDstMsb.ok && rBadDstMsb.error.find("INVALID_REMAP_RULE") != std::string::npos &&
         !rBadDstLsb.ok && rBadDstLsb.error.find("INVALID_REMAP_RULE") != std::string::npos &&
         !rBadDstPc.ok  && rBadDstPc.error.find("INVALID_REMAP_RULE")  != std::string::npos &&
         !rBadIdent.ok  && rBadIdent.error.find("INVALID_REMAP_RULE")  != std::string::npos);

    // 10. Event-limit overflow (> 4096 select events on input AND on post-remap expansion)
    Bytes tOverInput;
    pushBytes(tOverInput, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64});
    for (size_t i = 0; i < closure::kMaxSelectEvents - 1; ++i) {
        pushBytes(tOverInput, {0x00, 0xC0, uint8_t(i & 0x7Fu)});
    }
    pushEot(tOverInput);
    const Bytes smfOverInput = buildSmf(0, {tOverInput});  // 2 + 4095 = 4097 > 4096
    const auto rOverInput = remapSmf(smfOverInput.data(), smfOverInput.size(), {});
    TEST("Input exceeding kMaxSelectEvents (4097) fails closed with EVENT_LIMIT_EXCEEDED",
         !rOverInput.ok && rOverInput.error.find("EVENT_LIMIT_EXCEEDED") != std::string::npos);

    // Input with 4095 select events (valid), but shared-bank remap inserts +3 events -> 4098 > 4096!
    Bytes tOverPost;
    pushBytes(tOverPost, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64});
    pushBytes(tOverPost, {0x00, 0xC0, 5});  // matched (will add CC0 + CC32 + restore CC32 = +3 events)
    for (size_t i = 0; i < closure::kMaxSelectEvents - 4; ++i) {
        pushBytes(tOverPost, {0x00, 0xC0, 6});  // unremapped
    }
    pushEot(tOverPost);
    const Bytes smfOverPost = buildSmf(0, {tOverPost});  // 2 + 1 + 4092 = 4095 <= 4096
    const auto rOverPost = remapSmf(smfOverPost.data(), smfOverPost.size(), oneRule);
    TEST("Post-remap expansion exceeding kMaxSelectEvents fails closed with EVENT_LIMIT_EXCEEDED",
         !rOverPost.ok && rOverPost.bytes.empty() &&
         rOverPost.error.find("EVENT_LIMIT_EXCEEDED") != std::string::npos);

    // 11. Size limit exceeded (> kMaxFileBytes) fails closed before reading buffer
    const auto rOverSizeSmf = remapSmf(smfGood.data(), kMaxFileBytes + 1u, {});
    const auto rOverSizeTree = remapRootTree(smfGood.data(), kMaxFileBytes + 1u, {});
    TEST("Size exceeding kMaxFileBytes fails closed with SIZE_LIMIT_EXCEEDED",
         !rOverSizeSmf.ok && rOverSizeSmf.error.find("SIZE_LIMIT_EXCEEDED") != std::string::npos &&
         !rOverSizeTree.ok && rOverSizeTree.error.find("SIZE_LIMIT_EXCEEDED") != std::string::npos);

    // 12. Exact kMaxSelectEvents (4096) succeeds when no extra events are inserted
    Bytes tExactMax;
    pushBytes(tExactMax, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64});
    for (size_t i = 0; i < closure::kMaxSelectEvents - 2; ++i) {
        pushBytes(tExactMax, {0x00, 0xC0, 5});  // 2 + 4094 = 4096 exact
    }
    pushEot(tExactMax);
    const Bytes smfExactMax = buildSmf(0, {tExactMax});
    const auto rExactMax = remapSmf(smfExactMax.data(), smfExactMax.size(), {{{121, 64, 5}, {121, 64, 7}}});
    TEST("Exact kMaxSelectEvents (4096) with PC-only remap succeeds with 4094 remapped PCs",
         rExactMax.ok && rExactMax.modified && rExactMax.remappedEvents == 4094 && rExactMax.unchangedEvents == 0);
}

static void testRootTreeRemap() {
    std::printf("Section 9: Full remapRootTree() over synthetic .stg / .ksc / .pdc\n");

    const std::string stageXml   = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<STAGE version=\"1\" release=\"2\"/>\n";
    const std::string sessionXml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SESSION/>\n";
    const std::string sceneXml   = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SCENE/>\n";
    const std::string clipXml    = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CLIP type=\"TrackClip\"/>\n";
    const std::string padXml     = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CLIP type=\"PadClip\"/>\n";
    const std::string kscXml     = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CLIP type=\"KeyboardSetClip\"/>\n";

    // Build synthetic .stg with:
    // - Element scene Var1.scn/Bass.pdc/Main.mid (remapped: 121/64/5 -> 121/68/14)
    // - Element scene Var1.scn/Drum.pdc/Main.mid (unmatched Factory 120/0/0)
    // - Embedded KbdSet1.scn/SynthKsc.ksc/Main.ssn/RTTracks.scn/Upper1.plc/Main.mid (remapped: 121/65/9 -> 121/70/20)
    // - Embedded PadSet1.scn/SynthPad.pdc/Main.mid (shared bank: 121/64/5 remapped + 121/64/6 unremapped)
    // - Multiple XML leaves that MUST stay byte-identical!
    Bytes padTrk;
    pushBytes(padTrk, {0x00, 0xF0, 0x05, 0x42, 0x30, 0x00, 0x01, 0xF7});
    pushBytes(padTrk, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5, 0x20, 0xC0, 6});
    pushEot(padTrk);
    const Bytes padMid = buildSmf(0, {padTrk});
    const Bytes bassMid = makeSimpleSmf(8, 121, 64, 5);
    const Bytes drumMid = makeSimpleSmf(9, 120, 0, 0);
    const Bytes upper1Mid = makeSimpleSmf(0, 121, 65, 9);
    const Bytes mfxMid = makeSimpleSmf(0, 0, 0, 0);

    const Bytes embeddedKsc = makeBranch("SynthKsc.ksc", {
        makeBranch("Main.ssn", {
            makeBranch("RTTracks.scn", {
                makeBranch("Upper1.plc", {
                    makeLeaf("Main.mid", upper1Mid),
                    makeLeafStr("ClipDescription.xml", clipXml)
                }),
                makeLeafStr("SceneDescription.xml", sceneXml)
            }),
            makeLeafStr("SessionDescription.xml", sessionXml)
        }),
        makeLeaf("MFX.mid", mfxMid),
        makeLeafStr("ClipDescription.xml", kscXml)
    });

    const Bytes embeddedPadSet = makeBranch("PadSet1.scn", {
        makeBranch("SynthPad.pdc", {
            makeLeaf("Main.mid", padMid),
            makeLeafStr("ClipDescription.xml", padXml)
        }),
        makeLeafStr("SceneDescription.xml", sceneXml)
    });

    const Bytes synthStg = makeRoot({
        makeBranch("Main.ssn", {
            makeBranch("SongStyle1.scn", {
                makeBranch("SynthStyle.stc", {
                    makeBranch("Main.ssn", {
                        makeBranch("Var1.scn", {
                            makeBranch("Bass.pdc", {
                                makeLeaf("Main.mid", bassMid),
                                makeLeafStr("ClipDescription.xml", clipXml)
                            }),
                            makeBranch("Drum.pdc", {
                                makeLeaf("Main.mid", drumMid),
                                makeLeafStr("ClipDescription.xml", clipXml)
                            }),
                            makeLeafStr("SceneDescription.xml", sceneXml)
                        }),
                        makeLeafStr("SessionDescription.xml", sessionXml)
                    }),
                    makeLeaf("MFX.mid", mfxMid)
                }),
                makeBranch("KbdSet1-4.scn", {
                    embeddedKsc,
                    makeLeafStr("SceneDescription.xml", sceneXml)
                }),
                embeddedPadSet
            }),
            makeLeafStr("SessionDescription.xml", sessionXml)
        }),
        makeLeafStr("StageDescription.xml", stageXml)
    });

    const std::map<MidiTriple, MidiTriple> rules = {
        {{121, 64, 5}, {121, 68, 14}},
        {{121, 65, 9}, {121, 70, 20}}
    };

    const auto rStg = remapRootTree(synthStg.data(), synthStg.size(), rules);
    TEST("remapRootTree(.stg) succeeds across element scenes, embedded KBD Set, and embedded Pad",
         rStg.ok && rStg.modified && rStg.inspectedMidiLeaves == 6 &&
         rStg.modifiedMidiLeaves == 3 && rStg.remappedEvents == 3 && rStg.failedLeafPath.empty());

    // Re-parse and independently verify all XML leaves byte-identical and all MIDI leaves exact
    const auto parsedOut = roottree::parse(rStg.bytes.data(), rStg.bytes.size());
    const auto parsedOrig = roottree::parse(synthStg.data(), synthStg.size());
    std::vector<const roottree::Node*> outMids;
    if (parsedOut.ok) closure::collect(parsedOut.root, outMids);

    bool allXmlIdentical = parsedOut.ok && parsedOrig.ok;
    auto checkXml = [&](auto&& self, const roottree::Node& a, const roottree::Node& b) -> void {
        if (a.kind != b.kind || a.name != b.name || a.children.size() != b.children.size()) {
            allXmlIdentical = false;
            return;
        }
        if (a.kind == roottree::Node::Leaf) {
            if (a.name.size() >= 4 && a.name.compare(a.name.size() - 4, 4, ".xml") == 0) {
                if (a.payload != b.payload) allXmlIdentical = false;
            }
            return;
        }
        for (size_t i = 0; i < a.children.size(); ++i) self(self, a.children[i], b.children[i]);
    };
    if (allXmlIdentical) checkXml(checkXml, parsedOrig.root, parsedOut.root);

    bool allMidsValid = (outMids.size() == 6);
    std::vector<closure::Select> allOutSelects;
    for (const auto* m : outMids) {
        std::vector<closure::Select> s;
        std::string err;
        if (!closure::leafSelects(*m, s, err)) {
            allMidsValid = false;
            break;
        }
        allOutSelects.insert(allOutSelects.end(), s.begin(), s.end());
    }
    TEST("Re-parsed .stg has byte-identical XML leaves and exact 1:1 post-remap selects",
         allXmlIdentical && allMidsValid && allOutSelects.size() == 7 &&
         allOutSelects[0].channel == 9 && allOutSelects[0].msb == 121 && allOutSelects[0].lsb == 68 && allOutSelects[0].program == 14 &&
         allOutSelects[1].channel == 10 && allOutSelects[1].msb == 120 && allOutSelects[1].lsb == 0 && allOutSelects[1].program == 0 &&
         allOutSelects[2].msb == 0 && allOutSelects[2].lsb == 0 && allOutSelects[2].program == 0 &&
         allOutSelects[3].channel == 1 && allOutSelects[3].msb == 121 && allOutSelects[3].lsb == 70 && allOutSelects[3].program == 20 &&
         allOutSelects[4].msb == 0 && allOutSelects[4].lsb == 0 && allOutSelects[4].program == 0 &&
         allOutSelects[5].channel == 1 && allOutSelects[5].msb == 121 && allOutSelects[5].lsb == 68 && allOutSelects[5].program == 14 &&
         allOutSelects[6].channel == 1 && allOutSelects[6].msb == 121 && allOutSelects[6].lsb == 64 && allOutSelects[6].program == 6);

    // No-op remapRootTree returns exact byte-identical bytes
    const auto rNoOp = remapRootTree(synthStg.data(), synthStg.size(), {{{121, 75, 127}, {121, 74, 0}}});
    TEST("Unmatched remapRootTree returns modified=false and byte-identical .stg",
         rNoOp.ok && !rNoOp.modified && rNoOp.inspectedMidiLeaves == 6 &&
         rNoOp.modifiedMidiLeaves == 0 && rNoOp.remappedEvents == 0 && rNoOp.bytes == synthStg);

    // Non-lowercase .MID / .Mid leaf fails closed with failedLeafPath
    const Bytes badUpperMidStg = makeRoot({
        makeBranch("Main.ssn", {
            makeBranch("PadSet1.scn", {
                makeLeaf("Main.MID", bassMid),
                makeLeafStr("ClipDescription.xml", padXml)
            })
        })
    });
    const auto rUpperMid = remapRootTree(badUpperMidStg.data(), badUpperMidStg.size(), rules);
    TEST("Non-lowercase .MID leaf fails closed with MALFORMED_MIDI_LEAF and failedLeafPath",
         !rUpperMid.ok && rUpperMid.bytes.empty() &&
         rUpperMid.error.find("MALFORMED_MIDI_LEAF") != std::string::npos &&
         rUpperMid.failedLeafPath == "Main.ssn/PadSet1.scn/Main.MID");

    const Bytes badMixedMidStg = makeRoot({
        makeLeaf("Main.Mid", bassMid)
    });
    const auto rMixedMid = remapRootTree(badMixedMidStg.data(), badMixedMidStg.size(), rules);
    TEST("Mixed-case .Mid leaf fails closed with MALFORMED_MIDI_LEAF and failedLeafPath",
         !rMixedMid.ok && rMixedMid.bytes.empty() &&
         rMixedMid.error.find("MALFORMED_MIDI_LEAF") != std::string::npos &&
         rMixedMid.failedLeafPath == "Main.Mid");

    const Bytes emptyTree = makeRoot({makeLeafStr("StageDescription.xml", stageXml)});
    const auto rEmptyTree = remapRootTree(emptyTree.data(), emptyTree.size(), rules);
    TEST("Root tree with zero MIDI leaves succeeds unmodified with byte-identical output",
         rEmptyTree.ok && !rEmptyTree.modified && rEmptyTree.inspectedMidiLeaves == 0 &&
         rEmptyTree.bytes == emptyTree);

    // Malformed MIDI leaf nested inside Root tree fails closed with exact failedLeafPath
    Bytes badLeafTrk;
    pushBytes(badLeafTrk, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5, 0x00, 0xB0, 0x0B, 0x80});
    pushEot(badLeafTrk);
    const Bytes badNestedPdc = makeRoot({
        makeBranch("PadSet1.scn", {
            makeLeaf("Main.mid", buildSmf(0, {badLeafTrk})),
            makeLeafStr("ClipDescription.xml", padXml)
        })
    });
    const auto rBadNested = remapRootTree(badNestedPdc.data(), badNestedPdc.size(), {});
    TEST("Malformed nested .mid leaf (CC11 >= 0x80) fails closed with exact failedLeafPath",
         !rBadNested.ok && rBadNested.bytes.empty() &&
         rBadNested.error.find("MALFORMED_MIDI_LEAF") != std::string::npos &&
         rBadNested.failedLeafPath == "PadSet1.scn/Main.mid");

    // Malformed Root-tree container & invalid rules fail closed
    Bytes truncatedStg = synthStg;
    truncatedStg.pop_back();
    const auto rTruncTree = remapRootTree(truncatedStg.data(), truncatedStg.size(), rules);
    TEST("Malformed Root-tree container fails closed with MALFORMED_ROOT_TREE",
         !rTruncTree.ok && rTruncTree.error.find("MALFORMED_ROOT_TREE") != std::string::npos);

    const auto rBadRuleTree = remapRootTree(synthStg.data(), synthStg.size(), {{{128, 64, 5}, {121, 64, 5}}});
    TEST("Invalid rule on remapRootTree fails closed with INVALID_REMAP_RULE",
         !rBadRuleTree.ok && rBadRuleTree.error.find("INVALID_REMAP_RULE") != std::string::npos);

    // Determinism check: identical inputs produce byte-identical outputs across repeated runs
    const auto rDet1 = remapRootTree(synthStg.data(), synthStg.size(), rules);
    const auto rDet2 = remapRootTree(synthStg.data(), synthStg.size(), rules);
    const auto rSmfDet1 = remapSmf(padMid.data(), padMid.size(), rules);
    const auto rSmfDet2 = remapSmf(padMid.data(), padMid.size(), rules);
    TEST("Determinism: repeated remapSmf and remapRootTree calls produce byte-identical outputs",
         rDet1.ok && rDet2.ok && rDet1.bytes == rDet2.bytes &&
         rSmfDet1.ok && rSmfDet2.ok && rSmfDet1.bytes == rSmfDet2.bytes);

    // P2-2: Failure after a prior remapped leaf MUST reset modifiedMidiLeaves & remappedEvents to 0
    // while keeping inspectedMidiLeaves and failedLeafPath for diagnostics.
    const Bytes partialFailTree = makeRoot({
        makeLeaf("Good1.mid", bassMid),               // leaf 1: valid & matches rules (121/64/5 -> 121/68/14)
        makeLeaf("Bad2.mid", buildSmf(0, {badLeafTrk})) // leaf 2: malformed CC11 >= 0x80
    });
    const auto rPartialFail = remapRootTree(partialFailTree.data(), partialFailTree.size(), rules);
    TEST("P2-2: remapRootTree failure after a modified leaf resets modifiedMidiLeaves & remappedEvents to 0",
         !rPartialFail.ok && !rPartialFail.modified && rPartialFail.bytes.empty() &&
         rPartialFail.modifiedMidiLeaves == 0 && rPartialFail.remappedEvents == 0 &&
         rPartialFail.inspectedMidiLeaves == 2 && rPartialFail.failedLeafPath == "Bad2.mid" &&
         rPartialFail.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    // P3-1: verifyRoundTrip = false produces identical output bytes for both remapSmf and remapRootTree
    RemapOptions noVerify;
    noVerify.verifyRoundTrip = false;
    const auto rSmfNoVerify = remapSmf(padMid.data(), padMid.size(), rules, noVerify);
    const auto rTreeNoVerify = remapRootTree(synthStg.data(), synthStg.size(), rules, noVerify);
    TEST("P3-1: verifyRoundTrip=false succeeds and matches verifyRoundTrip=true byte-for-byte",
         rSmfNoVerify.ok && rSmfNoVerify.bytes == rSmfDet1.bytes &&
         rTreeNoVerify.ok && rTreeNoVerify.bytes == rStg.bytes);

    // P3-1: Duplicate sibling leaf names in Root tree remap cleanly & pass round-trip verification
    const Bytes dupSiblingTree = makeRoot({
        makeBranch("Dup.scn", {
            makeLeaf("Main.mid", bassMid),
            makeLeaf("Main.mid", upper1Mid),
            makeLeafStr("ClipDescription.xml", clipXml)
        })
    });
    const auto rDupSib = remapRootTree(dupSiblingTree.data(), dupSiblingTree.size(), rules);
    TEST("P3-1: duplicate sibling Main.mid leaves remap independently and round-trip cleanly",
         rDupSib.ok && rDupSib.modified && rDupSib.inspectedMidiLeaves == 2 &&
         rDupSib.modifiedMidiLeaves == 2 && rDupSib.remappedEvents == 2);

    // P3-1: Empty 0-byte .mid leaf in Root tree fails closed with MALFORMED_MIDI_LEAF and resets counters
    const Bytes emptyMidLeafTree = makeRoot({
        makeLeaf("Good.mid", bassMid),
        makeLeaf("Empty.mid", {})
    });
    const auto rEmptyMidLeaf = remapRootTree(emptyMidLeafTree.data(), emptyMidLeafTree.size(), rules);
    TEST("P3-1: empty 0-byte .mid leaf fails closed with MALFORMED_MIDI_LEAF and reset counters",
         !rEmptyMidLeaf.ok && !rEmptyMidLeaf.modified && rEmptyMidLeaf.bytes.empty() &&
         rEmptyMidLeaf.inspectedMidiLeaves == 2 && rEmptyMidLeaf.modifiedMidiLeaves == 0 &&
         rEmptyMidLeaf.remappedEvents == 0 && rEmptyMidLeaf.failedLeafPath == "Empty.mid" &&
         rEmptyMidLeaf.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    // P3-1: Format 1, declared == 0 header-only SMF succeeds unmodified
    const Bytes smfFmt1Zero = buildSmf(1, {});
    const auto rFmt1Zero = remapSmf(smfFmt1Zero.data(), smfFmt1Zero.size(), rules);
    TEST("P3-1: Format 1 header-only SMF (declared=0) succeeds unmodified byte-for-byte",
         rFmt1Zero.ok && !rFmt1Zero.modified && rFmt1Zero.remappedEvents == 0 &&
         rFmt1Zero.unchangedEvents == 0 && rFmt1Zero.bytes == smfFmt1Zero);

    // P2-1: Hostile 14-byte header declaring 65535 tracks fails cleanly without over-reservation
    Bytes hostileHdr{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0xFF, 0xFF, 0x01, 0x80};
    const auto rHostileHdr = remapSmf(hostileHdr.data(), hostileHdr.size(), rules);
    TEST("P2-1: tiny SMF header declaring 65535 tracks fails closed cleanly",
         !rHostileHdr.ok && rHostileHdr.bytes.empty() &&
         rHostileHdr.error.find("MALFORMED_MIDI_LEAF") != std::string::npos);

    // P2-1: Large untouched note run (10,000 notes) alongside a remapped PC remaps cleanly
    // in O(selects) memory and preserves the entire note block byte-for-byte
    Bytes bigTrk;
    pushBytes(bigTrk, {0x00, 0xB0, 0x00, 121, 0x00, 0xB0, 0x20, 64, 0x00, 0xC0, 5});
    Bytes notePayload;
    notePayload.reserve(10000u * 6u);
    pushBytes(notePayload, {0x00, 0x90, 0x3C, 0x64});
    for (int i = 0; i < 9999; ++i) {
        pushBytes(notePayload, {0x04, 0x3C, 0x00, 0x04, 0x3C, 0x64});  // running-status 0x90 run
    }
    pushBytes(notePayload, {0x04, 0x3C, 0x00});
    bigTrk.insert(bigTrk.end(), notePayload.begin(), notePayload.end());
    pushEot(bigTrk);
    const Bytes smfBig = buildSmf(0, {bigTrk});
    const auto rBig = remapSmf(smfBig.data(), smfBig.size(), rules);
    bool bigSelOk = false;
    const auto selBig = readSelects(rBig.bytes, &bigSelOk);
    TEST("P2-1: large 10,000-note running-status track remaps cleanly and preserves note block verbatim",
         rBig.ok && rBig.modified && rBig.remappedEvents == 1 && rBig.bytes.size() == smfBig.size() &&
         bigSelOk && selBig.size() == 1 &&
         selBig[0].msb == 121 && selBig[0].lsb == 68 && selBig[0].program == 14 &&
         hasSubseq(rBig.bytes, notePayload));
}

int main() {
    std::printf("Test: M2e-1 Portable SMF Bank/PC Remapper & Root-Tree Verifier\n");
    testFormat0And1Basic();
    testSharedBankRestoration();
    testCrossTrackBankSelect();
    testRunningStatusHandling();
    testSimultaneousSwapAndIdentityRules();
    testSysExMetaAndEventPreservation();
    testNoOpPreservation();
    testFailClosedMalformedAndUnresolved();
    testRootTreeRemap();

    std::printf("\nSummary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
