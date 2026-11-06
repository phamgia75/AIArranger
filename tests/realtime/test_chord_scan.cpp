// Gate 12 Task E — chord scan modes (deterministic, synthetic fingerings).

#include "engine/chord/fingered_detector.h"
#include "engine/chord/fingered_on_bass_detector.h"
#include "engine/chord/single_finger_detector.h"
#include "engine/chord/ai_fingered_placeholder.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace ai_arranger::chord;
using ai_arranger::arranger::ChordType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  \xE2\x9D\x8C FAIL: %s\n", name); failures++; } \
    else { std::printf("  \xE2\x9C\x85 PASS: %s\n", name); passes++; } \
} while(0)

static Chord det(const IChordScanMode& m, std::vector<uint8_t> n) {
    return m.detect(n.data(), n.size());
}

int main() {
    std::printf("Test: Gate 12 Task E — chord scan modes\n");

    FingeredDetector fing;
    FingeredOnBassDetector onbass;
    SingleFingerDetector single;
    AIFingeredPlaceholder ai;

    // ── Fingered triads / 7ths ──
    {
        Chord c = det(fing, {60, 64, 67});           // C E G
        TEST("Fingered C major", c.type == ChordType::Major && c.root == 60);
        c = det(fing, {60, 63, 67});                 // C Eb G
        TEST("Fingered C minor", c.type == ChordType::Minor && c.root == 60);
        c = det(fing, {62, 66, 69, 72});             // D F# A C
        TEST("Fingered D7", c.type == ChordType::Dom7 && c.root == 62);
        c = det(fing, {60, 64, 67, 71});             // C E G B
        TEST("Fingered Cmaj7", c.type == ChordType::Maj7);
        c = det(fing, {60});                         // single note
        TEST("Fingered single note -> NoChord", c.type == ChordType::NoChord);
    }

    // ── Fingered-on-bass (slash) ──
    {
        // E bass + C E G above -> C major / E
        Chord c = det(onbass, {52, 60, 64, 67});
        TEST("on-bass: C major over E", c.type == ChordType::Major &&
             c.root == 60 && c.bass == 52);
        // plain triad -> bass = lowest
        c = det(onbass, {60, 64, 67});
        TEST("on-bass: bass = lowest note", c.bass == 60);
    }

    // ── Single-finger convention ──
    {
        TEST("single: 1 note -> Major", det(single, {60}).type == ChordType::Major);
        TEST("single: root+min3 -> Minor", det(single, {60, 63}).type == ChordType::Minor);
        TEST("single: root+b7 -> Dom7", det(single, {60, 70}).type == ChordType::Dom7);
        TEST("single: root+5th -> Power", det(single, {60, 67}).type == ChordType::Power);
    }

    // ── AI placeholder == Fingered fallback (documented, no model) ──
    {
        Chord a = det(ai, {60, 64, 67});
        Chord f = det(fing, {60, 64, 67});
        TEST("AI placeholder falls back to Fingered",
             a.type == f.type && a.root == f.root);
        TEST("AI placeholder name flags placeholder",
             std::string(ai.name()) == "ai-fingered-placeholder");
    }

    // ── Inversions (chord-engine spec §4.3) ──
    {
        Chord c = det(fing, {52, 55, 60});           // E G C
        TEST("Fingered C/E 1st inversion -> C major, root C4",
             c.type == ChordType::Major && c.root == 60);
        c = det(fing, {55, 60, 64});                 // G C E
        TEST("Fingered C/G 2nd inversion -> C major, root C4",
             c.type == ChordType::Major && c.root == 60);
        c = det(fing, {43, 48, 51});                 // G2 C3 Eb3
        TEST("Fingered Cm 2nd inversion low register -> root C3",
             c.type == ChordType::Minor && c.root == 48);
        c = det(fing, {54, 57, 60, 62});             // F# A C D
        TEST("Fingered D7 3rd-inversion-ish (F# bass) -> D7, root D4",
             c.type == ChordType::Dom7 && c.root == 62);
        c = det(fing, {55, 62, 72});                 // G D C: root position Gsus4 wins over Csus2
        TEST("Fingered G C D -> Gsus4 (bass preferred over Csus2)",
             c.type == ChordType::Sus4 && c.root == 55);
        c = det(fing, {56, 60, 64});                 // Ab C E: symmetric aug keeps bass root
        TEST("Fingered aug keeps lowest note as root",
             c.type == ChordType::Aug && c.root == 56);
        c = det(fing, {60, 61, 62});                 // cluster
        TEST("Fingered cluster still NoChord", c.type == ChordType::NoChord);

        Chord s = det(onbass, {52, 55, 60});         // E G C
        TEST("on-bass: E G C -> C major over E",
             s.type == ChordType::Major && s.root == 60 && s.bass == 52);

        // Every template, every root, every rotation, two octaves: type and root
        // pitch class are rotation-invariant, and root position is unchanged.
        struct Tpl { ChordType t; std::vector<int> iv; };
        const std::vector<Tpl> tpls = {
            {ChordType::Major, {0, 4, 7}}, {ChordType::Minor, {0, 3, 7}},
            {ChordType::Dim, {0, 3, 6}},   {ChordType::Maj7, {0, 4, 7, 11}},
            {ChordType::Min7, {0, 3, 7, 10}}, {ChordType::Dom7, {0, 4, 7, 10}},
        };
        bool ok = true;
        for (const auto& tp : tpls)
            for (int root = 0; root < 12; ++root)
                for (int oct : {36, 60})
                    for (size_t rot = 0; rot < tp.iv.size(); ++rot) {
                        std::vector<uint8_t> n;
                        for (size_t k = 0; k < tp.iv.size(); ++k) {
                            const size_t idx = (rot + k) % tp.iv.size();
                            const int wrap = (idx < rot) ? 12 : 0;  // voice above
                            n.push_back(static_cast<uint8_t>(oct + root + tp.iv[idx] + wrap));
                        }
                        Chord r = fing.detect(n.data(), n.size());
                        if (r.type != tp.t || r.root % 12 != root ||
                            (rot == 0 && r.root != n[0])) ok = false;
                    }
        TEST("all 6 templates x 12 roots x every inversion x 2 octaves", ok);
    }

    // ── Determinism: same input twice -> identical ──
    {
        Chord a = det(fing, {65, 69, 72});
        Chord b = det(fing, {65, 69, 72});
        TEST("deterministic detect", a.type == b.type && a.root == b.root);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
