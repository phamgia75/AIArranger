// Gate 10 Task B — full NTR/NTT note-transposition tables.
//
// Unit tests for engine/music/ntt. Verifies the table-driven transform against
// the spec (arranger-engine-spec-v0.9.md §6): bass tracking across a chord
// progression, chord-quality remap (major <-> minor 3rd), scale modes, the
// bypass short-circuit, and role -> rule defaults.

#include "engine/music/ntt.h"
#include "engine/arranger/chord_input.h"
#include "engine/arranger/style_player.h"
#include "engine/chord/chord_scan_mode.h"
#include "engine/chord/fingered_on_bass_detector.h"
#include "engine/midi/panic.h"
#include "engine/midi/scheduler.h"
#include "engine/realtime/clock.h"
#include "ui/chord_view_model.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace ai_arranger::ui {
std::string formatChordName(const arranger::Chord& chord);
}

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

    // ── Q3a: ABI-pinned enum values (0..11 untouched, 12..21 appended) ──
    TEST("ABI pin: Major == 0, NoChord == 11",
         static_cast<uint8_t>(ChordType::Major) == 0 &&
         static_cast<uint8_t>(ChordType::NoChord) == 11);
    TEST("ABI pin: Min7b5 == 12 .. Aug7 == 21",
         static_cast<uint8_t>(ChordType::Min7b5) == 12 &&
         static_cast<uint8_t>(ChordType::Add9) == 13 &&
         static_cast<uint8_t>(ChordType::Maj6) == 14 &&
         static_cast<uint8_t>(ChordType::Min6) == 15 &&
         static_cast<uint8_t>(ChordType::Dom9) == 16 &&
         static_cast<uint8_t>(ChordType::Maj9) == 17 &&
         static_cast<uint8_t>(ChordType::Min9) == 18 &&
         static_cast<uint8_t>(ChordType::Dom7Sus4) == 19 &&
         static_cast<uint8_t>(ChordType::MinMaj7) == 20 &&
         static_cast<uint8_t>(ChordType::Aug7) == 21);

    // ── Q3a: All 10 extended ChordTypes in NTT ──
    {
        auto ch = [](ChordType t, uint8_t root = 60) { return Chord{t, root, 0, 0}; };
        // Min7b5: C E G B -> C Eb Gb Bb
        TEST_EQ("Min7b5 3rd: E4 -> Eb4 (63)", transpose(64, ch(ChordType::Min7b5), NtrRule::RootTransposition, NttMode::Chord), 63);
        TEST_EQ("Min7b5 5th: G4 -> Gb4 (66)", transpose(67, ch(ChordType::Min7b5), NtrRule::RootTransposition, NttMode::Chord), 66);
        TEST_EQ("Min7b5 7th: B4 -> Bb4 (70)", transpose(71, ch(ChordType::Min7b5), NtrRule::RootTransposition, NttMode::Chord), 70);
        // Add9: C E G stay C E G, D stays D
        TEST_EQ("Add9 3rd: E4 -> E4 (64)", transpose(64, ch(ChordType::Add9), NtrRule::RootTransposition, NttMode::Chord), 64);
        TEST_EQ("Add9 9th: D4 -> D4 (62)", transpose(62, ch(ChordType::Add9), NtrRule::RootTransposition, NttMode::Chord), 62);
        // Maj6 / Min6: B4 -> A4 (69), Min6 E4 -> Eb4 (63)
        TEST_EQ("Maj6 6th: B4 -> A4 (69)", transpose(71, ch(ChordType::Maj6), NtrRule::RootTransposition, NttMode::Chord), 69);
        TEST_EQ("Min6 3rd: E4 -> Eb4 (63)", transpose(64, ch(ChordType::Min6), NtrRule::RootTransposition, NttMode::Chord), 63);
        TEST_EQ("Min6 6th: B4 -> A4 (69)", transpose(71, ch(ChordType::Min6), NtrRule::RootTransposition, NttMode::Chord), 69);
        // Dom9 / Maj9 / Min9
        TEST_EQ("Dom9 7th: B4 -> Bb4 (70)", transpose(71, ch(ChordType::Dom9), NtrRule::RootTransposition, NttMode::Chord), 70);
        TEST_EQ("Dom9 9th: D4 -> D4 (62)",  transpose(62, ch(ChordType::Dom9), NtrRule::RootTransposition, NttMode::Chord), 62);
        TEST_EQ("Maj9 7th: B4 -> B4 (71)",  transpose(71, ch(ChordType::Maj9), NtrRule::RootTransposition, NttMode::Chord), 71);
        TEST_EQ("Maj9 9th: D4 -> D4 (62)",  transpose(62, ch(ChordType::Maj9), NtrRule::RootTransposition, NttMode::Chord), 62);
        TEST_EQ("Min9 3rd: E4 -> Eb4 (63)", transpose(64, ch(ChordType::Min9), NtrRule::RootTransposition, NttMode::Chord), 63);
        TEST_EQ("Min9 7th: B4 -> Bb4 (70)", transpose(71, ch(ChordType::Min9), NtrRule::RootTransposition, NttMode::Chord), 70);
        // Dom7Sus4: E4 -> F4 (65), B4 -> Bb4 (70)
        TEST_EQ("Dom7Sus4 4th: E4 -> F4 (65)",  transpose(64, ch(ChordType::Dom7Sus4), NtrRule::RootTransposition, NttMode::Chord), 65);
        TEST_EQ("Dom7Sus4 7th: B4 -> Bb4 (70)", transpose(71, ch(ChordType::Dom7Sus4), NtrRule::RootTransposition, NttMode::Chord), 70);
        // MinMaj7: E4 -> Eb4 (63), B4 -> B4 (71)
        TEST_EQ("MinMaj7 3rd: E4 -> Eb4 (63)", transpose(64, ch(ChordType::MinMaj7), NtrRule::RootTransposition, NttMode::Chord), 63);
        TEST_EQ("MinMaj7 7th: B4 -> B4 (71)",  transpose(71, ch(ChordType::MinMaj7), NtrRule::RootTransposition, NttMode::Chord), 71);
        // Aug7: G4 -> G#4 (68), B4 -> Bb4 (70)
        TEST_EQ("Aug7 #5: G4 -> G#4 (68)",  transpose(67, ch(ChordType::Aug7), NtrRule::RootTransposition, NttMode::Chord), 68);
        TEST_EQ("Aug7 b7: B4 -> Bb4 (70)",  transpose(71, ch(ChordType::Aug7), NtrRule::RootTransposition, NttMode::Chord), 70);

        // Exhaustive octave-independence & range check across all 10 extended types
        const ChordType extTypes[] = {
            ChordType::Min7b5, ChordType::Add9, ChordType::Maj6, ChordType::Min6,
            ChordType::Dom9, ChordType::Maj9, ChordType::Min9, ChordType::Dom7Sus4,
            ChordType::MinMaj7, ChordType::Aug7
        };
        bool extOk = true;
        for (ChordType t : extTypes) {
            for (int pc = 0; pc < 12; ++pc) {
                const Chord c4{t, static_cast<uint8_t>(60 + pc), 0, 0};
                const Chord c2{t, static_cast<uint8_t>(36 + pc), 0, 0};
                for (int note = 24; note <= 96; note += 5) {
                    const uint8_t a = transpose(static_cast<uint8_t>(note), c4, NtrRule::RootTransposition, NttMode::Chord);
                    const uint8_t b = transpose(static_cast<uint8_t>(note), c2, NtrRule::RootTransposition, NttMode::Chord);
                    if (a != b || a > 127) extOk = false;
                }
            }
        }
        TEST("all 10 extended ChordTypes are octave-independent and in [0..127]", extOk);
    }

    // ── Q3a: On-Bass slash-chord handling ──
    {
        const Chord cOverE{ChordType::Major, 60, 64, 0};   // C/E
        const Chord cOverG{ChordType::Major, 60, 67, 0};   // C/G
        const Chord g7OverB{ChordType::Dom7, 67, 71, 0};   // G7/B
        const RegisterLimits hk5{5, 0, 127};

        TEST_EQ("On-Bass C/E: C2 (36) -> E2 (40)",
                transpose(36, cOverE, NtrRule::RootTransposition, NttMode::Bass), 40);
        TEST_EQ("On-Bass C/G (hk=11): C2 (36) -> G2 (43)",
                transpose(36, cOverG, NtrRule::RootTransposition, NttMode::Bass), 43);
        TEST_EQ("On-Bass C/G (hk=5): C2 (36) -> G1 (31)",
                transpose(36, cOverG, NtrRule::RootTransposition, NttMode::Bass, 60, hk5), 31);
        TEST_EQ("On-Bass G7/B (hk=5): C2 (36) -> B1 (35)",
                transpose(36, g7OverB, NtrRule::RootTransposition, NttMode::Bass, 60, hk5), 35);
        // Non-root bass note (5th = G2=43) still tracks chord root C, not slash bass E
        TEST_EQ("On-Bass C/E: non-root 5th G2 (43) stays G2 (43)",
                transpose(43, cOverE, NtrRule::RootTransposition, NttMode::Bass), 43);
        // Chord track ignores slash bass
        TEST_EQ("On-Bass C/E: Chord track root C4 stays C4 (60)",
                transpose(60, cOverE, NtrRule::RootTransposition, NttMode::Chord), 60);
        // Same-pc bass == root hasSlashBass() is false
        TEST("hasSlashBass false when bass==0 or bass%12==root%12",
             !hasSlashBass(maj(60)) && !hasSlashBass(Chord{ChordType::Major, 60, 48, 0}) &&
             hasSlashBass(cOverE));
    }

    // ── Q3a: transposeVoicing() anti-unison-collapse, LIL mud guard, smooth centroid ──
    {
        // Unison collapse: C4 (60), D4 (62), E4 (64), B4 (71) over C Major triad
        // Single-note Chord NTT snaps D4->E4 or C4, colliding with C4/E4; transposeVoicing deduplicates.
        const uint8_t inCollapse[4] = {60, 62, 64, 71};
        uint8_t outCollapse[4] = {};
        const size_t nOut = transposeVoicing(inCollapse, 4, outCollapse, maj(60),
                                             NtrRule::RootTransposition, NttMode::Chord);
        std::set<uint8_t> uniq(outCollapse, outCollapse + nOut);
        TEST("transposeVoicing prevents unison collapse (4 distinct outputs)",
             nOut == 4 && uniq.size() == 4);

        // LIL mud guard: C1 (24), E1 (28), G1 (31) -> low < 36 with < 7 st intervals raised!
        const uint8_t inMud[3] = {24, 28, 31};
        uint8_t outMud[3] = {};
        const size_t nMud = transposeVoicing(inMud, 3, outMud, maj(60),
                                             NtrRule::RootTransposition, NttMode::Chord);
        std::vector<uint8_t> sortedMud(outMud, outMud + nMud);
        std::sort(sortedMud.begin(), sortedMud.end());
        bool hasMud = false;
        for (size_t i = 0; i + 1 < sortedMud.size(); ++i) {
            if (sortedMud[i] < 36 && (sortedMud[i + 1] - sortedMud[i]) < 7) hasMud = true;
        }
        TEST("transposeVoicing LIL mud guard eliminates intervals < 7 st below C2 (36)",
             nMud == 3 && !hasMud);

        // Smooth RootFixed centroid inversion
        const uint8_t inFixed[3] = {48, 52, 55}; // C3 E3 G3 (centroid ~51.7)
        uint8_t outSmooth[3] = {};
        VoicingOptions vopt{};
        vopt.prev_centroid_midi = 64; // target centroid near E4
        const size_t nSm = transposeVoicing(inFixed, 3, outSmooth, maj(60),
                                            NtrRule::RootFixed, NttMode::Chord, 60, {}, vopt);
        int sumSm = 0;
        for (size_t i = 0; i < nSm; ++i) sumSm += outSmooth[i];
        TEST("transposeVoicing smooth centroid shifts RootFixed voicing toward prev_centroid_midi",
             nSm == 3 && (sumSm / 3) >= 60);

        // Safety: null / empty
        TEST("transposeVoicing handles null/zero safely",
             transposeVoicing(nullptr, 4, outSmooth, maj(60), NtrRule::RootTransposition, NttMode::Chord) == 0 &&
             transposeVoicing(inFixed, 0, outSmooth, maj(60), NtrRule::RootTransposition, NttMode::Chord) == 0);

        // Audit Round 1 P2-2: LIL mud guard on {24, 28, 31, 40} must NOT create duplicate 40!
        const uint8_t inMudDup[4] = {24, 28, 31, 40};
        uint8_t outMudDup[4] = {};
        const size_t nMudDup = transposeVoicing(inMudDup, 4, outMudDup, maj(60),
                                                NtrRule::RootTransposition, NttMode::Chord);
        std::set<uint8_t> uniqMudDup(outMudDup, outMudDup + nMudDup);
        std::vector<uint8_t> sortedMudDup(outMudDup, outMudDup + nMudDup);
        std::sort(sortedMudDup.begin(), sortedMudDup.end());
        bool mudDupStillMud = false;
        for (size_t i = 0; i + 1 < sortedMudDup.size(); ++i) {
            if (sortedMudDup[i] < 36 && (sortedMudDup[i + 1] - sortedMudDup[i]) < 7) mudDupStillMud = true;
        }
        TEST("P2-2: transposeVoicing({24,28,31,40}) preserves 4 unique pitches AND zero LIL mud",
             nMudDup == 4 && uniqMudDup.size() == 4 && !mudDupStillMud);

        // Audit Round 1 P2-2: intentional unison when avoid_unison_collapse==false is NOT split by LIL
        const uint8_t inUnison[2] = {24, 24};
        uint8_t outUnison[2] = {};
        VoicingOptions noDedup{};
        noDedup.avoid_unison_collapse = false;
        noDedup.enforce_lil_mud_guard = true;
        const size_t nUni = transposeVoicing(inUnison, 2, outUnison, maj(60),
                                             NtrRule::RootTransposition, NttMode::Chord, 60, {}, noDedup);
        TEST("P2-2: intentional unison {24,24} with avoid_unison_collapse=false stays {24,24}",
             nUni == 2 && outUnison[0] == 24 && outUnison[1] == 24);
    }

    // ── Q3a: fromCasmRule() Yamaha Ctb2 -> (NtrRule, NttMode) ──
    {
        NtrRule r; NttMode m;
        fromCasmRule(1, 0, false, r, m);
        TEST("fromCasmRule(1, 0, false) -> Bypass/Bypass", r == NtrRule::Bypass && m == NttMode::Bypass);
        // Audit Round 1 P1-2: NTR=0, NTT=0x80 (ntt=0, ntt_bass=true) must transpose Bass, NOT freeze!
        fromCasmRule(0, 0, true, r, m);
        TEST("P1-2: fromCasmRule(0, 0, true) -> RootTransposition/Bass",
             r == NtrRule::RootTransposition && m == NttMode::Bass);
        TEST_EQ("P1-2: NTR=0/NTT=0x80 Bass C2 (36) over G -> G2 (43)",
                transpose(36, maj(67), r, m), 43);
        TEST_EQ("P1-2: NTR=0/NTT=0x80 Bass C2 (36) over C/E -> E2 (40)",
                transpose(36, Chord{ChordType::Major, 60, 64, 0}, r, m), 40);
        // Audit Round 1 P1-2: NTR=0, NTT=0 (ntt_bass=false) must transpose with root, NOT freeze!
        fromCasmRule(0, 0, false, r, m);
        TEST("P1-2: fromCasmRule(0, 0, false) -> RootTransposition/Melody (non-freezing)",
             r == NtrRule::RootTransposition && m == NttMode::Melody);
        TEST_EQ("P1-2: NTR=0/NTT=0 C4 (60) over G -> G4 (67)",
                transpose(60, maj(67), r, m), 67);

        fromCasmRule(0, 1, true, r, m);
        TEST("fromCasmRule(0, 1, true) -> RootTransposition/Bass", r == NtrRule::RootTransposition && m == NttMode::Bass);
        fromCasmRule(0, 2, false, r, m);
        TEST("fromCasmRule(0, 2, false) -> RootTransposition/Chord", r == NtrRule::RootTransposition && m == NttMode::Chord);
        fromCasmRule(1, 2, false, r, m);
        TEST("fromCasmRule(1, 2, false) -> RootFixed/Chord", r == NtrRule::RootFixed && m == NttMode::Chord);
        fromCasmRule(2, 1, false, r, m);
        TEST("fromCasmRule(2, 1, false) -> Guitar/Guitar", r == NtrRule::Guitar && m == NttMode::Guitar);
        fromCasmRule(0, 3, false, r, m);
        TEST("fromCasmRule(0, 3, false) -> MelodicMinor", r == NtrRule::RootTransposition && m == NttMode::MelodicMinor);
        fromCasmRule(0, 4, false, r, m);
        TEST("fromCasmRule(0, 4, false) -> HarmonicMinor", r == NtrRule::RootTransposition && m == NttMode::HarmonicMinor);
    }

    // ── Q3a: chord_scan_mode detects all 10 extended chords & ChordViewModel formats them + slash bass ──
    {
        uint8_t rootOut = 0;
        const uint8_t dm7b5[]    = {62, 65, 68, 72};
        const uint8_t cadd9[]    = {60, 62, 64, 67};
        const uint8_t cmaj6[]    = {60, 64, 67, 69};
        const uint8_t cmin6[]    = {60, 63, 67, 69};
        const uint8_t cdom9[]    = {60, 64, 67, 70, 74};
        const uint8_t cmaj9[]    = {60, 64, 67, 71, 74};
        const uint8_t cmin9[]    = {60, 63, 67, 70, 74};
        const uint8_t g7sus4[]   = {67, 72, 74, 77};
        const uint8_t cminmaj7[] = {60, 63, 67, 71};
        const uint8_t caug7[]    = {60, 64, 68, 70};

        TEST("scan Min7b5",   chord::matchChordType(dm7b5, 4, rootOut) == ChordType::Min7b5 && rootOut == 62);
        TEST("scan Add9",     chord::matchChordType(cadd9, 4, rootOut) == ChordType::Add9 && rootOut == 60);
        TEST("scan Maj6",     chord::matchChordType(cmaj6, 4, rootOut) == ChordType::Maj6);
        TEST("scan Min6",     chord::matchChordType(cmin6, 4, rootOut) == ChordType::Min6);
        TEST("scan Dom9",     chord::matchChordType(cdom9, 5, rootOut) == ChordType::Dom9);
        TEST("scan Maj9",     chord::matchChordType(cmaj9, 5, rootOut) == ChordType::Maj9);
        TEST("scan Min9",     chord::matchChordType(cmin9, 5, rootOut) == ChordType::Min9);
        TEST("scan Dom7Sus4", chord::matchChordType(g7sus4, 4, rootOut) == ChordType::Dom7Sus4 && rootOut == 67);
        TEST("scan MinMaj7",  chord::matchChordType(cminmaj7, 4, rootOut) == ChordType::MinMaj7);
        TEST("scan Aug7",     chord::matchChordType(caug7, 4, rootOut) == ChordType::Aug7);

        // ChordViewModel + formatChordName checks
        ui::ChordViewModel vm;
        auto setVm = [&](uint8_t root, ChordType t) {
            control::EngineTelemetry e{};
            e.chord_root = root;
            e.chord_type = static_cast<uint8_t>(t);
            vm.applyTelemetry(e);
            return vm.displayName();
        };
        TEST("format Dm7b5",  setVm(62, ChordType::Min7b5)   == "Dm7b5");
        TEST("format Cadd9",  setVm(60, ChordType::Add9)     == "Cadd9");
        TEST("format C6",     setVm(60, ChordType::Maj6)     == "C6");
        TEST("format Cm6",    setVm(60, ChordType::Min6)     == "Cm6");
        TEST("format C9",     setVm(60, ChordType::Dom9)     == "C9");
        TEST("format Cmaj9",  setVm(60, ChordType::Maj9)     == "Cmaj9");
        TEST("format Cm9",    setVm(60, ChordType::Min9)     == "Cm9");
        TEST("format G7sus4", setVm(67, ChordType::Dom7Sus4) == "G7sus4");
        TEST("format CmMaj7", setVm(60, ChordType::MinMaj7)  == "CmMaj7");
        TEST("format Caug7",  setVm(60, ChordType::Aug7)     == "Caug7");
        TEST("format slash C/E",  ui::formatChordName({ChordType::Major, 60, 64, 0}) == "C/E");
        TEST("format slash G7/B", ui::formatChordName({ChordType::Dom7, 67, 71, 0}) == "G7/B");
    }

    // ── Q3a: StylePlayer honors persisted UASF 'CASM' metadata (NTR=1/NTT=0 Bypass & source_chord_root) ──
    {
        uasf::StyleDefinition style{};
        style.name = "CASM Honor Test";
        style.tempo_bpm = 120;
        style.resolution = 480;
        uasf::SectionDefinition sec{};
        sec.type = uasf::SectionType::Main1;
        sec.bars = 1;
        sec.resolution = 480;
        sec.beats_per_bar = 4;
        sec.beat_note = 4;

        // Track 0: Chord role BUT CASM says NTR=1, NTT=0 (Bypass pedal) -> must NOT transpose!
        uasf::TrackDefinition pedal{};
        pedal.name = "Pedal";
        pedal.midi_channel = 1;
        pedal.role = uasf::TrackRole::Chord;
        pedal.articulation.has_casm_meta = true;
        pedal.articulation.ntr = 1;
        pedal.articulation.ntt = 0;
        pedal.events.push_back({uasf::MidiEventType::NoteOn,  1, 60, 90, 0});
        pedal.events.push_back({uasf::MidiEventType::NoteOff, 1, 60, 0, 240});
        sec.tracks.push_back(pedal);

        // Track 1: Bass with CASM ntt_bass=true -> honors On-Bass C/E!
        uasf::TrackDefinition bass{};
        bass.name = "Bass";
        bass.midi_channel = 2;
        bass.role = uasf::TrackRole::Bass;
        bass.articulation.has_casm_meta = true;
        bass.articulation.ntr = 0;
        bass.articulation.ntt = 1;
        bass.articulation.ntt_bass = true;
        bass.articulation.source_chord_root = 0; // C
        bass.events.push_back({uasf::MidiEventType::NoteOn,  2, 36, 100, 0});
        bass.events.push_back({uasf::MidiEventType::NoteOff, 2, 36, 0, 240});
        sec.tracks.push_back(bass);
        style.sections.push_back(sec);

        realtime::RealtimeClock clk;
        midi::MidiScheduler sch;
        midi::PanicHandler pn;
        arranger::StylePlayer pl(clk, sch, pn);
        uint8_t gotPedal = 0, gotBass = 0;
        sch.setOutputCallback([&](const uasf::MidiEvent& e) {
            if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) {
                if (e.channel == 1) gotPedal = e.data1;
                if (e.channel == 2) gotBass = e.data1;
            }
        });
        clk.setSampleRate(48000);
        pl.loadStyle(style);
        pl.start(0);
        pl.setChord({ChordType::Major, 60, 64, 0}); // C/E
        for (int i = 0; i < 40; ++i) {
            clk.advance(256);
            pl.tick();
            sch.advanceTo(clk.getPosition());
        }
        pl.stop();
        TEST_EQ("StylePlayer honors CASM NTR=1/NTT=0 Bypass (pedal stays 60)", gotPedal, 60);
        TEST_EQ("StylePlayer honors CASM ntt_bass + On-Bass C/E (bass 36 -> 40)", gotBass, 40);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
