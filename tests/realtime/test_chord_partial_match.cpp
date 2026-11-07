// P1.7b partial chord match (chord-engine spec §4.4, tests §12.2): one chord
// tone missing or one extra note is still recognised; exact matches always win;
// anything two notes away stays NoChord; the result depends only on the bass and
// the pitch-class set.

#include "engine/chord/chord_scan_mode.h"
#include "engine/chord/fingered_detector.h"
#include "engine/chord/fingered_on_bass_detector.h"
#include "engine/chord/single_finger_detector.h"
#include <algorithm>
#include <cstdio>
#include <vector>

using namespace ai_arranger;
using arranger::Chord;
using arranger::ChordType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static Chord det(const chord::IChordScanMode& m, std::vector<uint8_t> n) { return m.detect(n.data(), n.size()); }
static bool is(const Chord& c, ChordType t, int rootPc) { return c.type == t && c.root % 12 == rootPc; }

// Independent template table (spec §5.1 + phase 2 types the engine knows).
struct T { ChordType t; std::vector<int> iv; };
static const std::vector<T> kTemplates = {
    {ChordType::Major, {0, 4, 7}}, {ChordType::Minor, {0, 3, 7}}, {ChordType::Dom7, {0, 4, 7, 10}},
    {ChordType::Maj7, {0, 4, 7, 11}}, {ChordType::Min7, {0, 3, 7, 10}}, {ChordType::Sus4, {0, 5, 7}},
    {ChordType::Dim, {0, 3, 6}}, {ChordType::Aug, {0, 4, 8}}, {ChordType::Maj6, {0, 4, 7, 9}},
    {ChordType::Min6, {0, 3, 7, 9}}, {ChordType::Dom9, {0, 2, 4, 7, 10}}, {ChordType::Dim7, {0, 3, 6, 9}},
    {ChordType::Min7b5, {0, 3, 6, 10}}, {ChordType::Sus2, {0, 2, 7}},
};
static int maskOf(const T& t, int root) {
    int m = 0;
    for (int i : t.iv) m |= 1 << ((root + i) % 12);
    return m;
}
static int pop(int m) { int n = 0; for (; m; m &= m - 1) ++n; return n; }

// Voicing of a pitch-class mask over bass pc `bass`: bass at C2+bass, the rest
// ascending (spread=false) or descending across octaves (spread=true).
static std::vector<uint8_t> voice(int mask, int bass, bool spread) {
    std::vector<uint8_t> n = {static_cast<uint8_t>(36 + bass)};
    std::vector<int> up;
    for (int pc = 0; pc < 12; ++pc) if (pc != bass && (mask >> pc & 1)) up.push_back(pc);
    if (spread) {
        for (size_t k = up.size(); k-- > 0;)
            n.push_back(static_cast<uint8_t>(48 + 12 * (up.size() - 1 - k) + up[k]));
    } else {
        for (int pc : up) n.push_back(static_cast<uint8_t>(48 + pc));
    }
    std::sort(n.begin(), n.end());
    return n;
}

int main() {
    std::printf("Test: partial chord match (P1.7b)\n");
    chord::FingeredDetector f;
    chord::FingeredOnBassDetector ob;
    chord::SingleFingerDetector sf;

    // ── Spec §12.2 ──
    TEST("spec: Cmaj7 without fifth (C E B) = Cmaj7", is(det(f, {60, 64, 71}), ChordType::Maj7, 0));
    TEST("spec: Cmaj7 + D (C D E G B) = Cmaj7, not over-recognised",
         is(det(f, {60, 62, 64, 67, 71}), ChordType::Maj7, 0));

    // ── One missing / one extra ──
    TEST("C7 without fifth (C E Bb)", is(det(f, {48, 52, 58}), ChordType::Dom7, 0));
    TEST("Cm7 without fifth (C Eb Bb)", is(det(f, {48, 51, 58}), ChordType::Min7, 0));
    TEST("C9 without fifth (C E Bb D)", is(det(f, {48, 52, 58, 62}), ChordType::Dom9, 0));
    TEST("C + F# (#11) = C", is(det(f, {48, 52, 55, 66}), ChordType::Major, 0));
    TEST("Am + B (add9) = Am", is(det(f, {45, 48, 52, 59}), ChordType::Minor, 9));
    TEST("C7 no 3rd (C G Bb): 7 before m7 by spec order", is(det(f, {48, 55, 58}), ChordType::Dom7, 0));
    TEST("low register (C1 E1 B1) = Cmaj7", is(det(f, {24, 28, 35}), ChordType::Maj7, 0));
    TEST("tie: the bass decides (A C E G B = Am7 add9, not Cmaj7/A)", is(det(f, {57, 60, 64, 67, 71}), ChordType::Min7, 9));
    TEST("root = lowest held note of the root pc", det(f, {60, 64, 71, 72}).root == 60);

    // ── Rejections ──
    TEST("cluster C C# D Eb = NoChord", det(f, {60, 61, 62, 63}).type == ChordType::NoChord);
    TEST("two notes off (C Db F#) = NoChord", det(f, {60, 61, 66}).type == ChordType::NoChord);
    TEST("C E F# B: Bsus4 and Esus2 both one extra (C) -> sus4 by spec order", is(det(f, {60, 64, 66, 71}), ChordType::Sus4, 11));
    TEST("fifth + one note (C F# G) is not a 'power + extra'", det(f, {48, 54, 55}).type == ChordType::NoChord);
    TEST("2 notes never partial-match", det(f, {60, 64}).type == ChordType::NoChord);
    TEST("single note stays NoChord in Fingered", det(f, {60}).type == ChordType::NoChord);

    // ── Detector ordering ──
    TEST("exact still wins (C E G A = C6, not C + extra A)", is(det(f, {60, 64, 67, 69}), ChordType::Maj6, 0));
    {
        const Chord c = det(ob, {50, 60, 64, 67});
        TEST("on-bass: exact slash before partial (D + C E G = C/D)", is(c, ChordType::Major, 0) && c.bass == 50);
        const Chord p = det(ob, {48, 52, 59});
        TEST("on-bass: partial when nothing exact (C E B = Cmaj7)", is(p, ChordType::Maj7, 0) && p.bass == 48);
    }
    TEST("single finger 3+ notes uses partial (C E B = Cmaj7, not the Major fallback)",
         is(det(sf, {60, 64, 71}), ChordType::Maj7, 0));
    TEST("single finger 2 notes unchanged (C Bb = C7)", is(det(sf, {60, 70}), ChordType::Dom7, 0));

    // ── Exhaustive: every pitch-class set of 3-6 notes over every bass ──
    bool exactWins = true, validDeviation = true, voicingFree = true, deterministic = true;
    int partials = 0;
    for (int mask = 1; mask < 4096; ++mask) {
        const int k = pop(mask);
        if (k < 3 || k > 6) continue;
        for (int bass = 0; bass < 12; ++bass) {
            if (!(mask >> bass & 1)) continue;
            const auto a = voice(mask, bass, false), b = voice(mask, bass, true);
            uint8_t r = 0;
            const ChordType exact = chord::matchChordType(a.data(), a.size(), r);
            const Chord ca = det(f, a), cb = det(f, b);
            voicingFree &= ca.type == cb.type && ca.root % 12 == cb.root % 12;
            deterministic &= det(f, a).type == ca.type && det(f, a).root == ca.root;
            if (exact != ChordType::NoChord) { exactWins &= ca.type == exact; continue; }
            if (ca.type == ChordType::NoChord) {
                // No template may be within one note with a held root.
                for (const auto& t : kTemplates)
                    for (int root = 0; root < 12; ++root)
                        if ((mask >> root & 1) && pop(mask ^ maskOf(t, root)) == 1) validDeviation = false;
                continue;
            }
            ++partials;
            bool found = false;   // the reported chord is one note off the input
            for (const auto& t : kTemplates)
                if (t.t == ca.type) found |= pop(mask ^ maskOf(t, ca.root % 12)) == 1;
            validDeviation &= found && (mask >> (ca.root % 12) & 1);
        }
    }
    TEST("exhaustive: an exact match is never replaced by a partial one", exactWins);
    TEST("exhaustive: partial = exactly one note off a template, root held; NoChord only when none is",
         validDeviation);
    TEST("exhaustive: result independent of voicing (same bass, upper voices spread/reordered)", voicingFree);
    TEST("exhaustive: deterministic", deterministic);
    std::printf("  [info] %d partial matches over all 3-6 note sets x bass\n", partials);
    TEST("exhaustive: partial matching actually fires", partials > 0);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
