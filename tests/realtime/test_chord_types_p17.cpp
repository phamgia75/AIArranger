// P1.7 chord types: 6, m6, 9, m7b5. Root position, inversions, low register,
// open voicings, doubled notes, ambiguous pitch-class sets (the bass decides,
// then chord-engine spec §5.1 order), on-bass slash, NTT targets, display names.

#include "engine/chord/fingered_detector.h"
#include "engine/chord/fingered_on_bass_detector.h"
#include "engine/music/ntt.h"
#include "ui/chord_view_model.h"
#include <cstdio>
#include <vector>
#include <algorithm>

using namespace ai_arranger;
using arranger::Chord;
using arranger::ChordType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static Chord det(const chord::IChordScanMode& m, std::vector<uint8_t> n) {
    std::sort(n.begin(), n.end());
    n.erase(std::unique(n.begin(), n.end()), n.end());   // the adapter dedupes held notes
    return m.detect(n.data(), n.size());
}
static bool is(Chord c, ChordType t, int rootPc) { return c.type == t && c.root % 12 == rootPc; }

int main() {
    std::printf("Test: P1.7 chord types\n");
    chord::FingeredDetector f;
    chord::FingeredOnBassDetector ob;

    // ── Root position (C4) ──
    TEST("C6  = C E G A",      is(det(f, {60, 64, 67, 69}), ChordType::Maj6, 0));
    TEST("Cm6 = C Eb G A",     is(det(f, {60, 63, 67, 69}), ChordType::Min6, 0));
    TEST("C9  = C E G Bb D",   is(det(f, {60, 64, 67, 70, 74}), ChordType::Dom9, 0));
    TEST("Cm7b5 = C Eb Gb Bb", is(det(f, {60, 63, 66, 70}), ChordType::Min7b5, 0));

    // ── Low register and open voicings ──
    TEST("F6 low (F1 A1 C2 D2)",        is(det(f, {29, 33, 36, 38}), ChordType::Maj6, 5));
    TEST("Bm7b5 low (B0 D1 F1 A1)",     is(det(f, {23, 26, 29, 33}), ChordType::Min7b5, 11));
    TEST("G9 open (G2 B3 F4 A4 D5)",    is(det(f, {43, 59, 65, 69, 74}), ChordType::Dom9, 7));
    TEST("Dm6 open (D2 F3 B3 A4)",      is(det(f, {38, 53, 59, 69}), ChordType::Min6, 2));
    TEST("C9 doubled root (C2 C3 E3 G3 Bb3 D4)", is(det(f, {36, 48, 52, 55, 58, 62}), ChordType::Dom9, 0));
    TEST("root reported = lowest note of the root pc", det(f, {36, 48, 52, 55, 58, 62}).root == 36);

    // ── Inversions ──
    TEST("C9 / E (E G Bb C D)",          is(det(f, {52, 55, 58, 60, 62}), ChordType::Dom9, 0));
    TEST("C9 / D (D E G Bb C)",          is(det(f, {50, 52, 55, 58, 60}), ChordType::Dom9, 0));
    TEST("Cm6 / Eb (Eb G A C)",          is(det(f, {51, 55, 57, 60}), ChordType::Min6, 0));
    TEST("Cm6 / G (G A C Eb)",           is(det(f, {55, 57, 60, 63}), ChordType::Min6, 0));

    // ── Ambiguous sets: the bass decides, then spec §5.1 order ──
    TEST("A C E G = Am7 (bass A)",       is(det(f, {57, 60, 64, 67}), ChordType::Min7, 9));
    TEST("C E G A = C6 (bass C)",        is(det(f, {60, 64, 67, 69}), ChordType::Maj6, 0));
    TEST("E G A C = Am7 (m7 before 6)",  is(det(f, {52, 55, 57, 60}), ChordType::Min7, 9));
    TEST("E G C A = Am7 too (upper-voice order irrelevant)", is(det(f, {52, 55, 60, 69}), ChordType::Min7, 9));
    TEST("A C Eb G = Am7b5 (bass A)",    is(det(f, {57, 60, 63, 67}), ChordType::Min7b5, 9));
    TEST("Eb G A C = Cm6 (m6 before m7b5)", is(det(f, {51, 55, 57, 60}), ChordType::Min6, 0));
    TEST("Eb G Bb C = Eb6 (bass Eb), not Cm7", is(det(f, {51, 55, 58, 60}), ChordType::Maj6, 3));
    TEST("D C E G (no 5-note template) -> NoChord in Fingered", det(f, {50, 60, 64, 67}).type == ChordType::NoChord);
    TEST("C E Bb D (9 without 5th) -> NoChord (no partial match yet, spec §4.4)",
         det(f, {60, 64, 70, 74}).type == ChordType::NoChord);
    bool stable = true;   // every permutation of the upper voices over the same bass agrees
    std::vector<uint8_t> upper = {55, 57, 60};
    std::sort(upper.begin(), upper.end());
    const Chord ref = det(f, {52, 55, 57, 60});
    do {
        std::vector<uint8_t> n = {52};
        for (size_t k = 0; k < upper.size(); ++k) n.push_back(static_cast<uint8_t>(upper[k] + 12 * k));
        const Chord c = det(f, n);
        stable &= c.type == ref.type && c.root % 12 == ref.root % 12;
    } while (std::next_permutation(upper.begin(), upper.end()));
    TEST("ambiguous set: result independent of upper-voice order", stable);

    // ── On-bass (slash) ──
    {
        const Chord c7 = det(ob, {48, 52, 55, 58});
        TEST("on-bass: C E G Bb = C7, not Edim/C", is(c7, ChordType::Dom7, 0) && c7.bass == 48);
        const Chord c9 = det(ob, {48, 52, 55, 58, 62});
        TEST("on-bass: C9 = C9, not Em7b5/C", is(c9, ChordType::Dom9, 0));
        const Chord slash = det(ob, {50, 60, 64, 67});
        TEST("on-bass: D + C E G = C / D", is(slash, ChordType::Major, 0) && slash.bass == 50);
        const Chord c6e = det(ob, {40, 60, 64, 67, 69});
        TEST("on-bass: E + C6 above -> Am7 over E (whole set, spec order)",
             is(c6e, ChordType::Min7, 9) && c6e.bass == 40);
    }

    // ── NTT: source C maj7 tones map onto the new chords ──
    using music::NtrRule; using music::NttMode;
    auto tr = [](uint8_t n, ChordType t, uint8_t root, NttMode m) {
        return music::transpose(n, Chord{t, root, 0, 0}, NtrRule::RootTransposition, m);
    };
    TEST("C6: source B (maj7) -> A (6th)",   tr(71, ChordType::Maj6, 60, NttMode::Chord) == 69);
    TEST("Cm6: source E -> Eb, B -> A",      tr(64, ChordType::Min6, 60, NttMode::Chord) == 63 &&
                                             tr(71, ChordType::Min6, 60, NttMode::Chord) == 69);
    TEST("Cm7b5: source G -> Gb, B -> Bb",   tr(67, ChordType::Min7b5, 60, NttMode::Chord) == 66 &&
                                             tr(71, ChordType::Min7b5, 60, NttMode::Chord) == 70);
    TEST("C9: source B -> Bb, D kept (9th is a chord tone)",
         tr(71, ChordType::Dom9, 60, NttMode::Chord) == 70 && tr(62, ChordType::Dom9, 60, NttMode::Chord) == 62);
    TEST("G6: root shifts up 7, B -> E (6th of G)", tr(71, ChordType::Maj6, 67, NttMode::Chord) == 76);
    bool inChord = true;   // Chord mode lands on chord tones for every new type and root
    for (ChordType t : {ChordType::Maj6, ChordType::Min6, ChordType::Dom9, ChordType::Min7b5})
        for (uint8_t root = 48; root < 60; ++root)
            for (uint8_t n = 36; n < 96; ++n) {
                const int out = tr(n, t, root, NttMode::Chord);
                const int rel = ((out - root) % 12 + 12) % 12;
                const int tones[4][5] = {{0, 4, 7, 9, -1}, {0, 3, 7, 9, -1}, {0, 2, 4, 7, 10}, {0, 3, 6, 10, -1}};
                const int k = t == ChordType::Maj6 ? 0 : t == ChordType::Min6 ? 1 : t == ChordType::Dom9 ? 2 : 3;
                bool hit = false;
                for (int v : tones[k]) hit |= v == rel;
                inChord &= hit;
            }
    TEST("NTT Chord mode: every output is a chord tone (4 types x 12 roots x 60 notes)", inChord);

    // ── Display ──
    ui::ChordViewModel vm;
    auto name = [&](uint8_t root, ChordType t) {
        control::EngineTelemetry tel{};
        tel.chord_root = root; tel.chord_type = static_cast<uint8_t>(t);
        vm.applyTelemetry(tel);
        return vm.displayName();
    };
    TEST("names: F6 Gm6 A9 Bm7b5", name(65, ChordType::Maj6) == "F6" && name(67, ChordType::Min6) == "Gm6" &&
                                   name(69, ChordType::Dom9) == "A9" && name(71, ChordType::Min7b5) == "Bm7b5");
    TEST("C ABI values: existing types unchanged, new ones after NoChord",
         static_cast<int>(ChordType::NoChord) == 11 && static_cast<int>(ChordType::Maj6) == 12 &&
         static_cast<int>(ChordType::Min7b5) == 15);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
