// SFF1 mapper — each CASM section track keeps its OWN NTR/NTT.
//
// A Ctb2 block's NTR/NTT can differ per section (C_WHISPER: "Strings" is
// NTR 0 / NTT 1 in Main B, Main D, Fill In BB... but NTR 1 / NTT 2 elsewhere).
// The mapper ignored the track's own values and took the first config ANYWHERE
// in the file whose name was a substring of the track name (an empty config name
// matched every track), so 6 of C_WHISPER's 62 CASM tracks got another
// section's transposition rule.

#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include <cstdio>
#include <string>

using namespace ai_arranger::importers::sff1;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

static CasmTrackConfig cfg(const std::string& name, uint8_t ch, uint8_t ntr, uint8_t ntt) {
    CasmTrackConfig c{};
    c.name = name; c.source_channel = ch; c.dest_channel = ch; c.ntr = ntr; c.ntt = ntt;
    c.low_key = 0; c.high_key = 127;
    return c;
}

int main() {
    std::printf("Test: SFF1 mapper per-section CASM NTR/NTT\n");

    // ── Synthetic: same track name, different rules per section; empty name ──
    {
        ParseResult p{};
        p.success = true;
        SffSection smf{}; smf.resolution = 1920; smf.bars = 4; smf.name = "SMF";
        p.sections.push_back(smf);

        CasmSection a; a.name = "Main A";
        a.tracks = {cfg("Strings", 13, 1, 2), cfg("", 14, 2, 3)};
        CasmSection b; b.name = "Main B";
        b.tracks = {cfg("Strings", 13, 0, 1), cfg("Pad", 15, 2, 0)};
        p.casm_sections = {a, b};
        for (const auto& s : p.casm_sections)
            for (const auto& t : s.tracks) p.casm_configs.push_back(t);

        Sff1ToUasfMapper m;
        const auto r = m.map(p);
        const bool ok = r.success && r.casm_sections.size() == 2;
        TEST("synthetic: two CASM sections mapped", ok);
        if (ok) {
            const auto& ma = r.casm_sections[0].tracks;
            const auto& mb = r.casm_sections[1].tracks;
            TEST("Main A/Strings keeps NTR 1 / NTT 2", ma[0].articulation.ntr == 1 && ma[0].articulation.ntt == 2);
            TEST("Main B/Strings keeps its own NTR 0 / NTT 1",
                 mb[0].articulation.ntr == 0 && mb[0].articulation.ntt == 1);
            TEST("Main B/Pad is not captured by the empty-named config",
                 mb[1].articulation.ntr == 2 && mb[1].articulation.ntt == 0);
        }
    }

    // ── Real corpus: every CASM track's mapped NTR/NTT equals its Ctb2 ──
    {
        const std::string path = std::string(CORPUS_DIR) +
            "/C_WHISPER_SC_GENOS.S718---da1df5af-1207-4d84-bf3d-99b5d051b92b.sty";
        Sff1Reader reader;
        const auto p = reader.parseFile(path);
        TEST("C_WHISPER fixture present and parsed", p.success && !p.casm_sections.empty());
        Sff1ToUasfMapper m;
        const auto r = m.map(p);
        int total = 0, mismatched = 0;
        for (size_t s = 0; s < p.casm_sections.size() && s < r.casm_sections.size(); ++s)
            for (size_t t = 0; t < p.casm_sections[s].tracks.size(); ++t, ++total) {
                const auto& own = p.casm_sections[s].tracks[t];
                const auto& got = r.casm_sections[s].tracks[t].articulation;
                if (own.ntr != got.ntr || own.ntt != got.ntt) ++mismatched;
            }
        std::printf("    (C_WHISPER: %d CASM tracks, %d mismatched)\n", total, mismatched);
        TEST("C_WHISPER: every CASM track keeps its own NTR/NTT", total > 0 && mismatched == 0);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
