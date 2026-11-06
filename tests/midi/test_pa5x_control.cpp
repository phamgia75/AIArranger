// Pa5X Control-channel commands (manual v1.4 p.613-614): byte-exact Program
// Changes, sent on the caller's Control channel and never remapped or dropped
// by the style output channel map.

#include "session/live_engine_facade.h"
#include "midi/pa5x_control.h"
#include "midi/pa5x_profile.h"
#include "engine/chord/fingered_detector.h"
#include "fake_midi_input_source.h"
#include "fake_midi_output_provider.h"
#include <cstdio>

using namespace ai_arranger;
using midi::pa5x::Command;
using uasf::MidiEvent;
using uasf::MidiEventType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

int main() {
    std::printf("Test: Pa5X control-channel commands\n");

    // ── Table, against the manual ──
    struct Row { Command c; uint8_t pc; };
    const Row rows[] = {
        {Command::KbdSet1, 64}, {Command::KbdSet4, 67},
        {Command::Intro1, 80}, {Command::Intro3CountIn, 82},
        {Command::Variation1, 83}, {Command::Variation4, 86},
        {Command::Fill1, 87}, {Command::Fill4, 90}, {Command::Break, 91},
        {Command::Ending1, 92}, {Command::Ending3, 94},
        {Command::FadeInOut, 95}, {Command::TempoLock, 101},
        {Command::PlayStopPlayer1, 104}, {Command::PlayStopPlayer2, 105},
        {Command::SynchroStart, 106}, {Command::SynchroStop, 107},
    };
    bool table = true;
    for (const auto& r : rows) {
        const MidiEvent e = midi::pa5x::controlEvent(r.c, 15);
        table &= e.type == MidiEventType::ProgramChange && e.channel == 15 &&
                 e.data1 == r.pc && e.data2 == 0;
    }
    TEST("PC numbers match manual p.613-614", table);
    TEST("channel is masked to 0-15", midi::pa5x::controlEvent(Command::Break, 0x13).channel == 3);

    // ── Facade: bypasses the Genos map (which drops ch1-8 and every PC) ──
    midi::FakeMidiInputSource  fin;  fin.devices = {{0, "Pa5X KEYBOARD"}};
    midi::FakeMidiOutputProvider fout; fout.setDevices({{0, "Pa5X SOUND"}});
    session::LiveEngineFacade fac(&fin, &fout);
    chord::FingeredDetector det;
    fac.setChordScanMode(&det);
    fac.start();
    fac.selectMidiOutput(0);
    fac.tick(48);
    TEST("Genos map set", fac.setOutputChannelMap(midi::pa5x::genosStyleTrackMap()));
    fout.sent.clear();

    TEST("command accepted", fac.sendDeviceCommand(midi::pa5x::controlEvent(Command::Variation2, 0)));
    TEST("nothing sent before tick", fout.sent.empty());
    fac.tick(0);
    TEST("delivered on ch1 unchanged despite map",
         fout.sent.size() == 1 && fout.sent[0].type == MidiEventType::ProgramChange &&
         fout.sent[0].channel == 0 && fout.sent[0].data1 == 84);

    MidiEvent bad = midi::pa5x::controlEvent(Command::Fill1, 0);
    bad.channel = 16;
    TEST("channel 16 refused", !fac.sendDeviceCommand(bad));
    bad.channel = 0; bad.data1 = 200;
    TEST("data byte > 127 refused", !fac.sendDeviceCommand(bad));

    // Order: commands go out in the order given, before the tick's notes.
    fout.sent.clear();
    fac.sendDeviceCommand(midi::pa5x::controlEvent(Command::Intro1, 15));
    fac.sendDeviceCommand(midi::pa5x::controlEvent(Command::PlayStopPlayer1, 15));
    fac.transportStart();
    fac.tick(480);
    TEST("two commands first, in order",
         fout.sent.size() >= 2 && fout.sent[0].data1 == 80 && fout.sent[1].data1 == 104 &&
         fout.sent[0].channel == 15);

    // Full ring: 64 accepted, the 65th refused with QueueFull, all 64 delivered later.
    fac.transportStop(); fac.tick(0);
    fout.sent.clear();
    fac.clearError();
    int accepted = 0;
    for (int i = 0; i < 65; ++i)
        accepted += fac.sendDeviceCommand(midi::pa5x::controlEvent(Command::Fill1, 2)) ? 1 : 0;
    TEST("ring full after 64, QueueFull recorded",
         accepted == 64 && fac.lastError() == session::EngineError::QueueFull);
    fac.tick(0);
    int pcs = 0;
    for (const auto& e : fout.sent) pcs += e.type == MidiEventType::ProgramChange && e.data1 == 87;
    TEST("all 64 delivered", pcs == 64);

    fac.stop();
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
