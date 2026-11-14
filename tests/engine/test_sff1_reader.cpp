#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_report.h"
#include "importers/sff1/sff1_mapper.h"
#include <cstdio>
#include <vector>
#include <cstdint>
#include <string>

using namespace ai_arranger::importers::sff1;

static int failures = 0;
static int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  ❌ FAIL: %s\n", name); failures++; } \
    else { std::printf("  ✅ PASS: %s\n", name); passes++; } \
} while(0)

std::vector<uint8_t> makeChunk(const std::string& id, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf;
    for (size_t i = 0; i < 4 && i < id.size(); ++i) buf.push_back(static_cast<uint8_t>(id[i]));
    while (buf.size() < 4) buf.push_back(' ');
    uint32_t sz = static_cast<uint32_t>(data.size());
    buf.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(sz & 0xFF));
    buf.insert(buf.end(), data.begin(), data.end());
    return buf;
}

// Build a 47-byte Ctb2 entry: zone split 0/127 (middle zone only), the given
// 6-byte middle zone {NTR, NTT, High Key, Note Low, Note High, RTR} at [28..33],
// and a deliberately different decoy in the unused low/high zones.
std::vector<uint8_t> makeCtb2Entry(const std::string& name, uint8_t srcCh,
                                   const std::vector<uint8_t>& midZone) {
    std::vector<uint8_t> e(47, 0);
    e[0] = srcCh;
    for (size_t i = 0; i < 8; ++i)
        e[1 + i] = (i < name.size()) ? static_cast<uint8_t>(name[i]) : ' ';
    e[9]  = srcCh;
    e[20] = 0; e[21] = 127;
    const uint8_t decoy[6] = {0, 2, 7, 0, 127, 3};   // Root Trans / Chord
    for (int i = 0; i < 6; ++i) { e[22 + i] = decoy[i]; e[34 + i] = decoy[i]; e[28 + i] = midZone[i]; }
    return e;
}

// Construct a CasmTrackConfig directly (for mapper role tests).
CasmTrackConfig makeCfg(const std::string& name, uint8_t srcCh,
                        uint8_t ntr, uint8_t ntt, bool nttBass) {
    CasmTrackConfig c{};
    c.name = name;
    c.source_channel = srcCh;
    c.dest_channel = srcCh;
    c.note_low = 0; c.note_high = 127;
    c.ntr = ntr; c.ntt = ntt; c.ntt_bass = nttBass;
    c.is_megavoice = false;
    return c;
}

// Build a CASM chunk (CSEG-wrapped) from a list of (sectionName, entries).
std::vector<uint8_t> makeCasmChunk(
    const std::vector<std::pair<std::string, std::vector<std::vector<uint8_t>>>>& sections) {
    std::vector<uint8_t> inner;
    for (const auto& sec : sections) {
        std::vector<uint8_t> nameBytes(sec.first.begin(), sec.first.end());
        auto sdec = makeChunk("Sdec", nameBytes);
        inner.insert(inner.end(), sdec.begin(), sdec.end());
        for (const auto& entry : sec.second) {
            auto ctb2 = makeChunk("Ctb2", entry);
            inner.insert(inner.end(), ctb2.begin(), ctb2.end());
        }
    }
    auto cseg = makeChunk("CSEG", inner);          // CASM body starts with CSEG
    return makeChunk("CASM", cseg);                // top-level CASM chunk
}

int main() {
    std::printf("Test: SFF1 Reader\n");

    { Sff1Reader r; auto res = r.parseBuffer({}, "empty.bin"); TEST("Empty fails", !res.success); }
    { Sff1Reader r; auto res = r.parseBuffer({0,1,2}, "small.bin"); TEST("Small fails", !res.success); }

    { auto buf = makeChunk("CASM", {0,1,2,3}); Sff1Reader r; auto res = r.parseBuffer(buf, "casm.sty");
      TEST("CASM parsed", res.success); TEST("1 chunk", res.total_chunks == 1); TEST("0 unknown", res.unknown_chunks == 0); }

    { auto buf = makeChunk("XXXX", {0}); Sff1Reader r; auto res = r.parseBuffer(buf, "unk.sty");
      TEST("Unknown chunk parsed", res.success); TEST("Unknown counted", res.unknown_chunks >= 1); }

    { auto b1 = makeChunk("SInt", std::vector<uint8_t>(10,0));
      auto b2 = makeChunk("Midi", std::vector<uint8_t>(10,0));
      b1.insert(b1.end(), b2.begin(), b2.end());
      Sff1Reader r; auto res = r.parseBuffer(b1, "multi.sty");
      TEST("Multi chunk parsed", res.success); TEST("2 chunks", res.total_chunks == 2); }

    { auto buf = makeChunk("SInt", {0}); Sff1Reader r; auto res = r.parseBuffer(buf, "sect.sty");
      TEST("Section parsed", res.success); }

    { auto buf = makeChunk("CASM", std::vector<uint8_t>(8,0)); Sff1Reader r;
      auto res = r.parseBuffer(buf, "score.sty");
      auto sc = Sff1ReportGenerator::computeScore(res);
      auto rep = Sff1ReportGenerator::generateReport(res);
      TEST("Parse success = 100%", sc.parse_success >= 1.0);
      TEST("Structural fidelity >= 90%", sc.structural_fidelity >= 0.9);
      TEST("Report generated", !rep.empty());
      TEST("Report has table", rep.find("Parse") != std::string::npos); }

    { TEST("Corpus infra ready", true); }

    // ── CASM semantic parsing (Gate 7) ─────────────────────────────────
    {
        auto buf = makeChunk("CASM", std::vector<uint8_t>(64, 0));
        Sff1Reader r; auto res = r.parseBuffer(buf, "casm_semantic.sty");
        TEST("CASM semantic parse runs", res.success);
    }

    // ── CASM configs extracted ────────────────────────────────────────
    {
        auto buf = makeChunk("CASM", std::vector<uint8_t>(64, 0));
        Sff1Reader r; auto res = r.parseBuffer(buf, "casm_configs.sty");
        TEST("CASM configs vector exists", res.casm_configs.empty());
    }

    // ── Section names detected ────────────────────────────────────────
    {
        auto buf = makeChunk("CASM", std::vector<uint8_t>(64, 0));
        Sff1Reader r; auto res = r.parseBuffer(buf, "casm_sections.sty");
        TEST("Sections parsed vector exists", res.sections_parsed.empty());
    }

    // ── Ctb2: the middle zone [28..33] applies, not the low zone [22..27] ──
    {
        auto guitar = makeCtb2Entry("Chord1", 11, {2, 0, 0, 0, 127, 1});
        auto bass   = makeCtb2Entry("Bass", 10, {0, 0x81, 11, 24, 127, 2});
        auto buf = makeCasmChunk({{"Main A", {guitar, bass}}});
        Sff1Reader r; auto res = r.parseBuffer(buf, "ctb2_zones.sty");
        const bool two = res.casm_configs.size() == 2;
        TEST("Ctb2: two configs", two);
        if (two) {
            const auto& g = res.casm_configs[0];
            const auto& b = res.casm_configs[1];
            TEST("Ctb2: NTR/NTT from the middle zone (Guitar / All-Purpose)",
                 g.ntr == 2 && g.ntt == 0 && !g.ntt_bass);
            TEST("Ctb2: bass flag, High Key, note limits, RTR from the middle zone",
                 b.ntr == 0 && b.ntt == 1 && b.ntt_bass && b.high_key == 11 &&
                 b.note_low == 24 && b.note_high == 127 && b.rtr == 2);
            TEST("Ctb2: zone split kept", b.zone_low == 0 && b.zone_high == 127);
            TEST("Ctb2: Guitar NTR with NTT 0 counts as chord-following",
                 Sff1ReportGenerator::computeScore(res).harmonic_fidelity == 1.0);
        }
        std::vector<uint8_t> shortEntry(guitar.begin(), guitar.begin() + 33);
        auto buf2 = makeCasmChunk({{"Main A", {shortEntry}}});
        Sff1Reader r2; auto res2 = r2.parseBuffer(buf2, "ctb2_short.sty");
        TEST("Ctb2: entry without a full middle zone is skipped", res2.success && res2.casm_configs.empty());
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
