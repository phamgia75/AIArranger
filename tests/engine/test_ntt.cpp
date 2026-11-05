// Gate 10 Task B — full NTR/NTT note-transposition tables.
//
// Unit tests for engine/music/ntt. Verifies the table-driven transform against
// the spec (arranger-engine-spec-v0.9.md §6): bass tracking across a chord
// progression, chord-quality remap (major <-> minor 3rd), scale modes, the
// bypass short-circuit, and role -> rule defaults.

#include "engine/music/ntt.h"
#include "engine/arranger/chord_input.h"
#include <cstdio>

using namespace ai_arranger;
using namespace ai_arranger::music;
using arranger::Chord;
using arranger::ChordType;

static int failures = 0;
static int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  \xE2\x9D\x8C FAIL: %s\n", name); failures++; } \
    else { std::printf("  \xE2\x9C\x85 PASS: %s\n", name); passes++; } \
} while(0)

#define TEST_EQ(name, got, want) do { \
    int g = (int)(got), w = (int)(want); \
    if (g != w) { std::fprintf(stderr, "  \xE2\x9D\x8C FAIL: %s (got %d want %d)\n", name, g, w); failures++; } \
    else { std::printf("  \xE2\x9C\x85 PASS: %s\n", name); passes++; } \
} while(0)

static Chord maj(uint8_t root) { return {ChordType::Major, root, 0, 0}; }
static Chord min(uint8_t root) { return {ChordType::Minor, root, 0, 0}; }

int main() {
    std::printf("Test: Gate 10 Task B — NTR/NTT transposition tables\n");

    // ── Bass NTT: root note tracks the chord root across C->F->G->Am->Dm ──
    // Recorded bass root = C2 (36), source reference chord root = C (60).
    const uint8_t bassRoot = 36; // C2
    TEST_EQ("Bass root over C  -> C2 (36)",
            transpose(bassRoot, maj(60), NtrRule::RootTransposition, NttMode::Bass), 36);
    TEST_EQ("Bass root over F  -> F2 (41)",
            transpose(bassRoot, maj(65), NtrRule::RootTransposition, NttMode::Bass), 41);
    TEST_EQ("Bass root over G  -> G2 (43)",
            transpose(bassRoot, maj(67), NtrRule::RootTransposition, NttMode::Bass), 43);
    TEST_EQ("Bass root over Am -> A2 (45)",
            transpose(bassRoot, min(69), NtrRule::RootTransposition, NttMode::Bass), 45);
    TEST_EQ("Bass root over Dm -> D2 (38)",
            transpose(bassRoot, min(62), NtrRule::RootTransposition, NttMode::Bass), 38);

    // ── Bass NTT: recorded fifth maps to the target chord's fifth ──
    // Recorded G3 (55) is the fifth of the C source chord.
    TEST_EQ("Bass fifth over C -> G3 (55, fifth of C)",
            transpose(55, maj(60), NtrRule::RootTransposition, NttMode::Bass), 55);
    TEST_EQ("Bass fifth over F -> C4 (60, fifth of F)",
            transpose(55, maj(65), NtrRule::RootTransposition, NttMode::Bass), 60);

    // ── Chord NTT: major 3rd remapped to minor 3rd over a minor target ──
    // Recorded E4 (64) is the major 3rd of C. Over C minor it must become Eb4.
    TEST_EQ("Chord 3rd: E4 over Cmaj stays E4 (64)",
            transpose(64, maj(60), NtrRule::RootTransposition, NttMode::Chord), 64);
    TEST_EQ("Chord 3rd: E4 over Cmin -> Eb4 (63)",
            transpose(64, min(60), NtrRule::RootTransposition, NttMode::Chord), 63);

    // ── Melody NTT keeps the root in place over the same-root chord ──
    TEST_EQ("Melody root over C stays C4 (60)",
            transpose(60, maj(60), NtrRule::RootTransposition, NttMode::Melody), 60);

    // ── Bypass short-circuits (drums / NTR-bypass): note unchanged ──
    TEST_EQ("Bypass NTT leaves note unchanged",
            transpose(38, maj(67), NtrRule::RootTransposition, NttMode::Bypass), 38);
    TEST_EQ("Bypass NTR leaves note unchanged",
            transpose(38, maj(67), NtrRule::Bypass, NttMode::Bass), 38);

    // ── NoChord leaves note unchanged ──
    Chord noChord = {ChordType::NoChord, 0, 0, 0};
    TEST_EQ("NoChord leaves note unchanged",
            transpose(60, noChord, NtrRule::RootTransposition, NttMode::Bass), 60);

    // ── Output is always a valid MIDI note even at the extremes ──
    uint8_t hi = transpose(127, maj(71), NtrRule::RootTransposition, NttMode::Melody);
    uint8_t lo = transpose(0,   maj(48), NtrRule::RootTransposition, NttMode::Bass);
    TEST("High note stays in MIDI range", hi <= 127);
    TEST("Low note stays in MIDI range", lo <= 127); // uint8_t, just guard the fold

    // ── The chord root's octave never transposes the pattern (Pa5X 2026-11-05) ──
    // Detectors report the lowest played MIDI note as the root, so a left-hand F2
    // (41) and an F4 (65) are the same chord. Before the fix F2 shifted the whole
    // pattern down 19 semitones (bass C2 -> F0).
    TEST_EQ("Bass C2 over F2 chord -> F2 (41), same as over F4",
            transpose(bassRoot, maj(41), NtrRule::RootTransposition, NttMode::Bass), 41);
    TEST_EQ("Chord E4 over F2 chord -> A4 (69), same as over F4",
            transpose(64, maj(41), NtrRule::RootTransposition, NttMode::Chord),
            transpose(64, maj(65), NtrRule::RootTransposition, NttMode::Chord));
    {
        const ChordType types[] = {ChordType::Major, ChordType::Minor, ChordType::Dom7,
                                   ChordType::Min7, ChordType::Maj7, ChordType::Dim, ChordType::Dim7};
        const NttMode modes[] = {NttMode::Bass, NttMode::Chord, NttMode::Melody, NttMode::Guitar,
                                 NttMode::Bypass, NttMode::NaturalMinor, NttMode::Dorian};
        const NtrRule rules[] = {NtrRule::RootTransposition, NtrRule::RootFixed, NtrRule::Guitar, NtrRule::Bypass};
        bool octaveFree = true, inRange = true, rootFixedStill = true;
        for (int pc = 0; pc < 12; ++pc)
            for (ChordType t : types)
                for (NttMode m : modes)
                    for (NtrRule r : rules)
                        for (int note = 0; note < 128; ++note) {
                            const Chord ref{t, static_cast<uint8_t>(60 + pc), 0, 0};
                            const uint8_t want = transpose(static_cast<uint8_t>(note), ref, r, m);
                            if (want > 127) inRange = false;
                            for (int root = pc; root < 128; root += 12) {
                                const Chord c{t, static_cast<uint8_t>(root), 0, 0};
                                const uint8_t got = transpose(static_cast<uint8_t>(note), c, r, m);
                                if (got != want) octaveFree = false;
                                if (got > 127) inRange = false;
                            }
                            // RootFixed: never moves more than the NTT remap (< 1 octave).
                            if (r == NtrRule::RootFixed && (want > note + 11 || want + 11 < note)) rootFixedStill = false;
                        }
        TEST("all 12 roots x 7 chord types x NTT modes x NTR rules: root octave 0..10 gives identical output",
             octaveFree);
        TEST("every output is a valid MIDI note (0..127)", inRange);
        TEST("RootFixed never shifts by the root", rootFixedStill);

        // Root transposition goes up 0..11 semitones from the C reference (existing
        // contract), regardless of the octave the chord is played in.
        bool upward = true;
        for (int root = 0; root < 128; ++root)
            if (transpose(bassRoot, maj(static_cast<uint8_t>(root)), NtrRule::RootTransposition, NttMode::Bass)
                != bassRoot + root % 12) upward = false;
        TEST("bass C2 over any major root (any octave) -> C2 + pitch class", upward);
    }

    // ── Role -> default rule pairs ──
    NtrRule ntr; NttMode ntt;
    defaultRuleForRole(uasf::TrackRole::Bass, ntr, ntt);
    TEST("Bass role -> Bass NTT", ntt == NttMode::Bass && ntr == NtrRule::RootTransposition);
    defaultRuleForRole(uasf::TrackRole::Chord, ntr, ntt);
    TEST("Chord role -> Chord NTT", ntt == NttMode::Chord);
    defaultRuleForRole(uasf::TrackRole::Drum, ntr, ntt);
    TEST("Drum role -> Bypass", ntt == NttMode::Bypass && ntr == NtrRule::Bypass);
    defaultRuleForRole(uasf::TrackRole::Phrase1, ntr, ntt);
    TEST("Phrase role -> Melody NTT", ntt == NttMode::Melody);

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
