// MIDI input is never forwarded to the output (no MIDI-thru / feedback path).
//
// The Pa5X exposes its keyboard (source) and its sound engine (destination) on
// the same device. If played notes were echoed to the output, the keyboard
// would double every note and could feed back. Headless: fake input + output.

#include "session/live_engine_facade.h"
#include "engine/chord/fingered_detector.h"
#include "fake_midi_input_source.h"
#include "fake_midi_output_provider.h"
#include <cstdio>

using namespace ai_arranger;
using session::LiveEngineFacade;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

int main() {
    std::printf("Test: MIDI input is never forwarded to the output\n");

    midi::FakeMidiInputSource  fin;  fin.devices = {{0, "Pa5X KEYBOARD"}};
    midi::FakeMidiOutputProvider fout; fout.setDevices({{0, "Pa5X SOUND"}});
    LiveEngineFacade fac(&fin, &fout);
    chord::FingeredDetector det;
    fac.setChordScanMode(&det);
    fac.start();
    fac.selectMidiInput(0);
    fac.selectMidiOutput(0);
    fac.tick(48);
    fout.sent.clear();

    // Everything a keyboard sends: notes (both hands), sustain, PC, pressure,
    // pitch bend, all-notes-off.
    const uint8_t played[] = {0x90, 48, 77, 0x90, 52, 77, 0x90, 55, 77, 0x90, 100, 77,
                              0xB0, 64, 127, 0xC0, 5, 0xD0, 40, 0xE0, 0, 64,
                              0x80, 100, 0, 0xB0, 64, 0};
    fin.inject(played, sizeof(played));
    for (int i = 0; i < 50; ++i) fac.tick(480);
    TEST("transport stopped: input produces no output at all", fout.sent.empty());

    // While playing, accompaniment is output, but the played melody note (100,
    // velocity 77, above the split) is never echoed.
    fac.transportStart();
    const uint8_t melody[] = {0x90, 100, 77, 0x80, 100, 0};
    for (int i = 0; i < 200; ++i) {
        if (i % 20 == 0) fin.inject(melody, sizeof(melody));
        fac.tick(480);
    }
    bool echoed = false;
    for (const auto& e : fout.sent)
        if (e.type == uasf::MidiEventType::NoteOn && e.data1 == 100 && e.data2 == 77) echoed = true;
    TEST("playing: accompaniment is output", !fout.sent.empty());
    TEST("playing: played melody note never echoed", !echoed);
    fac.transportStop();

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
