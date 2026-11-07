// Slash-chord bass end-to-end (spec §6.3 bass): held notes -> on-bass detector -> PerformerAdapter
// -> StylePlayer (sequencer chord, Bass NTT) -> scheduler output. Offline only: the output goes to
// a callback, never to CoreMIDI. Input = the Pa5X lower-part capture of 2026-11-07 (E1 G1 C2).

#include "engine/integration/performer_adapter.h"
#include "engine/arranger/style_player.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include "engine/chord/fingered_detector.h"
#include "engine/chord/fingered_on_bass_detector.h"
#include "engine/control/control_events.h"
#include <cstdio>
#include <vector>

using namespace ai_arranger;
using arranger::ChordType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

namespace {

// Demo style: Bass track on channel 1 plays C3 (48) on beat 1 and G3 (55) on beat 3.
constexpr uint8_t kBassChannel = 1;

struct Rig {
    realtime::RealtimeClock clock; midi::MidiScheduler scheduler; midi::PanicHandler panic;
    arranger::StylePlayer player{clock, scheduler, panic};
    integration::PerformerAdapter adapter{clock, player};
    std::vector<uint8_t> bassOn;   // bass-channel Note On pitches, in output order
    explicit Rig(chord::IChordScanMode* mode) {
        scheduler.setOutputCallback([this](const uasf::MidiEvent& e) {
            if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0 && e.channel == kBassChannel) bassOn.push_back(e.data1);
        });
        player.loadStyle(arranger::buildDemoStyle());
        clock.setSampleRate(48000); clock.setResolution(480); clock.setTempo(120);
        adapter.setSplitPoint(60);
        adapter.setChordScanMode(mode);
    }
    void play(std::initializer_list<uint8_t> held, int callbacks) {
        for (uint8_t n : held) adapter.noteOn(n);
        // The app Start path (re-applies the held chord); player.start() alone resets to C major.
        adapter.postEvent({control::ControlAction::Start, 0, 0});
        adapter.tick();
        for (int i = 0; i < callbacks; ++i) { clock.advance(256); adapter.tick(); player.tick(); scheduler.advanceTo(clock.getPosition()); }
    }
};

bool allPc(const std::vector<uint8_t>& v, std::initializer_list<int> pcs) {
    if (v.empty()) return false;
    for (uint8_t n : v) {
        bool ok = false;
        for (int pc : pcs) ok |= n % 12 == pc;
        if (!ok) return false;
    }
    return true;
}

} // namespace

int main() {
    std::printf("Test: slash-chord bass end to end\n");
    chord::FingeredOnBassDetector onbass;
    chord::FingeredDetector fingered;

    {   // Pa5X capture E1-G1-C2 in Fingered on Bass: C major over E
        Rig r(&onbass);
        r.play({28, 31, 36}, 400);   // ~1 bar at 120 BPM
        const auto c = r.player.getCurrentChord();
        TEST("player chord keeps the slash bass (C major, bass E1)",
             c.type == ChordType::Major && c.root % 12 == 0 && c.bass == 28);
        TEST("bass track played", !r.bassOn.empty());
        TEST("bass root plays E (C/E), fifth stays G", !r.bassOn.empty() && r.bassOn.front() % 12 == 4 && allPc(r.bassOn, {4, 7}));
        std::printf("    bass notes:");
        for (uint8_t n : r.bassOn) std::printf(" %d", n);
        std::printf("\n");
    }
    {   // the same keys in Fingered: no bass information, bass plays the root
        Rig r(&fingered);
        r.play({28, 31, 36}, 400);
        TEST("Fingered: chord bass not set", r.player.getCurrentChord().bass == 0);
        TEST("Fingered: bass root plays C", !r.bassOn.empty() && r.bassOn.front() % 12 == 0 && allPc(r.bassOn, {0, 7}));
    }
    {   // root position in on-bass: unchanged
        Rig r(&onbass);
        r.play({36, 40, 43}, 400);
        TEST("on-bass root position C2-E2-G2: bass root plays C", !r.bassOn.empty() && r.bassOn.front() % 12 == 0 && allPc(r.bassOn, {0, 7}));
    }
    {   // F/A (A1-C2-F2): the bass plays A, so a C-reset player cannot pass by accident
        Rig r(&onbass);
        r.play({33, 36, 41}, 400);
        TEST("on-bass F/A: chord F major over A1", r.player.getCurrentChord().root % 12 == 5 && r.player.getCurrentChord().bass == 33);
        TEST("on-bass F/A: bass root plays A, fifth C", !r.bassOn.empty() && r.bassOn.front() % 12 == 9 && allPc(r.bassOn, {9, 0}));
    }
    {   // determinism: two identical runs produce identical bass output
        Rig a(&onbass), b(&onbass);
        a.play({28, 31, 36}, 400); b.play({28, 31, 36}, 400);
        TEST("deterministic output for identical input", a.bassOn == b.bassOn && !a.bassOn.empty());
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
