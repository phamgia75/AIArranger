// Gate 13 Task F — SplitRouter <-> StylePlayer end-to-end.

#include "engine/integration/performer_adapter.h"
#include "engine/arranger/style_player.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include "engine/chord/fingered_detector.h"
#include "engine/arranger/chord_input.h"
#include <cstdio>

using namespace ai_arranger;
using namespace ai_arranger::integration;
using arranger::Chord;
using arranger::ChordType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  \xE2\x9D\x8C FAIL: %s\n", name); failures++; } \
    else { std::printf("  \xE2\x9C\x85 PASS: %s\n", name); passes++; } \
} while(0)

struct Rig {
    realtime::RealtimeClock clock; midi::MidiScheduler scheduler; midi::PanicHandler panic;
    arranger::StylePlayer player{clock, scheduler, panic};
    PerformerAdapter adapter{clock, player};
    chord::FingeredDetector fingered;
    Rig() {
        player.loadStyle(arranger::buildDemoStyle());
        clock.setSampleRate(48000); clock.setResolution(480); clock.setTempo(120);
        adapter.setChordScanMode(&fingered);
        adapter.setSplitPoint(60);  // C4 split
    }
};

int main() {
    std::printf("Test: Gate 13 Task F — split router E2E\n");

    // ── Lower-zone notes form the chord; upper-zone ignored ──
    {
        Rig r;
        // lower-zone C major: 48,52,55 (all < 60)
        r.adapter.noteOn(48); r.adapter.noteOn(52); r.adapter.noteOn(55);
        Chord c = r.player.getCurrentChord();
        TEST("lower-zone chord detected (C major root 48)",
             c.type == ChordType::Major && c.root == 48);

        // upper-zone melody note (>= 60) must NOT change the chord
        r.adapter.noteOn(72);
        Chord c2 = r.player.getCurrentChord();
        TEST("upper-zone note ignored for chord", c2.root == 48 && c2.type == ChordType::Major);
    }

    // ── Manual bass override -> lowest lower-zone note as chord bass ──
    {
        Rig r;
        r.adapter.setManualBass(true);
        // C major triad (36,48,52,55 -> C/E/G) all in the lower zone.
        r.adapter.noteOn(36); r.adapter.noteOn(48); r.adapter.noteOn(52); r.adapter.noteOn(55);
        // The split router applies manual bass on the adapter's chord; the sequencer keeps it too.
        TEST("manual bass = lowest lower note (36)", r.adapter.lastChord().bass == 36);
        TEST("manual bass reaches the player chord", r.player.getCurrentChord().bass == 36);

        // off -> bass not forced (0 = root)
        Rig r2;
        r2.adapter.noteOn(48); r2.adapter.noteOn(52); r2.adapter.noteOn(55);
        TEST("manual bass off -> bass 0 (root)", r2.adapter.lastChord().bass == 0);
    }

    // ── No channel collision: routing classifies, never assigns a channel ──
    {
        Rig r;
        r.adapter.noteOn(50); r.adapter.noteOn(54); r.adapter.noteOn(57); // D major lower
        r.adapter.noteOn(70);          // upper (ignored)
        TEST("only lower notes in chord (D major root 50)",
             r.player.getCurrentChord().root == 50 &&
             r.player.getCurrentChord().type == ChordType::Major);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
