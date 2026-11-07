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
#include "session/chord_label.h"
#include "session/live_engine_facade.h"
#include "midi/pa5x_profile.h"
#include "../midi/fake_midi_input_source.h"
#include "../midi/fake_midi_output_provider.h"
#include <cstdio>
#include <map>
#include <vector>

using namespace ai_arranger;
using arranger::ChordType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

namespace {

// Demo style: Bass track on wire channel index 1 (MIDI ch 2) plays C3 (48) on beat 1 and G3 (55) on beat 3.
constexpr uint8_t kBassChannel = 1;

struct Rig {
    realtime::RealtimeClock clock; midi::MidiScheduler scheduler; midi::PanicHandler panic;
    arranger::StylePlayer player{clock, scheduler, panic};
    integration::PerformerAdapter adapter{clock, player};
    std::vector<uint8_t> bassOn;   // bass-channel Note On pitches, in output order
    std::vector<uasf::MidiEvent> all;   // every dispatched event
    explicit Rig(chord::IChordScanMode* mode) {
        scheduler.setOutputCallback([this](const uasf::MidiEvent& e) {
            all.push_back(e);
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
        run(callbacks);
    }
    void run(int callbacks) {
        for (int i = 0; i < callbacks; ++i) { clock.advance(256); adapter.tick(); player.tick(); scheduler.advanceTo(clock.getPosition()); }
    }
    void release(std::initializer_list<uint8_t> notes) { for (uint8_t n : notes) adapter.noteOff(n); }
    void press(std::initializer_list<uint8_t> notes) { for (uint8_t n : notes) adapter.noteOn(n); }
    std::string label() const {
        const auto c = player.getCurrentChord();
        return session::chordLabel(static_cast<uint8_t>(c.type), c.root, c.bass);
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

    {   // C/G (G1-C2-E2): bass plays G, labelled C/G, root stays C
        Rig r(&onbass);
        r.play({31, 36, 40}, 400);
        const auto c = r.player.getCurrentChord();
        TEST("on-bass C/G: root C, bass G1, label \"C/G\"", c.root % 12 == 0 && c.bass == 31 && r.label() == "C/G");
        TEST("on-bass C/G: bass root plays G", !r.bassOn.empty() && r.bassOn.front() % 12 == 7 && allPc(r.bassOn, {7}));
    }
    {   // labels: root vs bass distinction
        Rig a(&onbass), b(&onbass), f(&fingered);
        a.play({28, 31, 36}, 1); b.play({36, 40, 43}, 1); f.play({28, 31, 36}, 1);
        TEST("labels: C/E on-bass, \"C\" root position, \"C\" in Fingered (no bass info)",
             a.label() == "C/E" && b.label() == "C" && f.label() == "C");
        TEST("label: C2 bass under a C4 root is root position", session::chordLabel(0, 60, 36) == "C");
        TEST("labels: Am7/G, F/A, Bbm7b5, NoChord",
             session::chordLabel((uint8_t)ChordType::Min7, 57, 43) == "Am7/G" && session::chordLabel(0, 41, 33) == "F/A" &&
             session::chordLabel((uint8_t)ChordType::Min7b5, 46, 0) == "A#m7b5" &&
             session::chordLabel((uint8_t)ChordType::NoChord, 60, 28) == "--");
    }
    {   // sequencing while playing: C/E -> C/G (bass-only change) -> C -> G/B -> Am7/G
        Rig r(&onbass);
        r.play({28, 31, 36}, 380);
        struct Step { std::initializer_list<uint8_t> off, on; const char* label; int bassPc; uint8_t bass; int fifthPc; };
        const Step steps[] = {
            {{28, 31, 36}, {31, 36, 40}, "C/G", 7, 31, 7},
            {{31, 36, 40}, {36, 40, 43}, "C", 0, 36, 7},
            {{36, 40, 43}, {35, 38, 43}, "G/B", 11, 35, 2},
            {{35, 38, 43}, {31, 36, 40, 45}, "Am7/G", 7, 31, 4},
        };
        bool labels = true, basses = true, follows = true;
        for (const auto& st : steps) {
            for (uint8_t n : st.off) r.adapter.noteOff(n);
            for (uint8_t n : st.on) r.adapter.noteOn(n);
            const size_t from = r.bassOn.size();
            r.run(380);   // ~1 bar
            const auto c = r.player.getCurrentChord();
            if (r.label() != st.label) { labels = false; std::fprintf(stderr, "    label %s, want %s\n", r.label().c_str(), st.label); }
            basses &= c.bass == st.bass;
            // Each change lands just after a downbeat: beat 3 (the chord's fifth) comes first, then
            // the next downbeat plays the bass.
            follows &= r.bassOn.size() == from + 2 && r.bassOn[from] % 12 == st.fifthPc && r.bassOn[from + 1] % 12 == st.bassPc;
            std::printf("    %s at tick %lld: bass notes", st.label, (long long)r.clock.getPosition());
            for (size_t k = from; k < r.bassOn.size(); ++k) std::printf(" %d", r.bassOn[k]);
            std::printf("\n");
        }
        TEST("sequence: labels C/G, C, G/B, Am7/G", labels);
        TEST("sequence: the player keeps each new bass", basses);
        TEST("sequence: after each change the fifth, then the new bass on the next downbeat", follows);
        r.adapter.postEvent({control::ControlAction::Stop, 0, 0});
        r.run(4);
        std::map<std::pair<int, int>, int> bal;
        for (const auto& e : r.all) {
            if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) ++bal[{e.channel, e.data1}];
            else if (e.type == uasf::MidiEventType::NoteOff || e.type == uasf::MidiEventType::NoteOn) --bal[{e.channel, e.data1}];
        }
        bool ok = true;
        for (const auto& [k, v] : bal) ok &= v == 0;
        TEST("sequence incl. the bass-only change: no stuck note after Stop", ok);
    }
    {   // facade: the Pa5X capture bytes in, the snapshot (C ABI chordBass) out
        midi::FakeMidiInputSource fin; fin.devices = {{0, "Pa5X KEYBOARD"}};
        midi::FakeMidiOutputProvider fout;
        session::LiveEngineFacade fac(&fin, &fout);
        fac.setChordScanMode(&onbass);
        fac.setInputChannelRoles(midi::pa5x::defaultPresetKeyboardRoles());
        fac.start();
        fin.selectSource(0);
        const uint8_t bytes[] = {0x93, 0x1C, 0x0F, 0x93, 0x24, 0x10, 0x93, 0x1F, 0x0F};   // 12:07 capture, C/E
        fin.inject(bytes, sizeof bytes);
        fac.tick(48);
        const auto snap = fac.snapshot();
        TEST("facade snapshot from the hw bytes: chordBass 28 (E1), root C, label C/E",
             snap.chordBass == 28 && snap.chordRoot % 12 == 0 && session::chordLabel(snap.chordType, snap.chordRoot, snap.chordBass) == "C/E");
        fac.stop();
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
