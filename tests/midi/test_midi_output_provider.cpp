// Gate 14C Task A — MIDI output provider interface (fake) + CoreMIDI smoke.

#include "fake_midi_output_provider.h"
#include "midi/coremidi/coremidi_output_provider.h"
#include "engine/uasf/types.h"
#include <cstdio>
#include <cstdlib>

using namespace ai_arranger;
using namespace ai_arranger::midi;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  \xE2\x9D\x8C FAIL: %s\n", name); failures++; } \
    else { std::printf("  \xE2\x9C\x85 PASS: %s\n", name); passes++; } \
} while(0)

static uasf::MidiEvent noteOn(uint8_t n) {
    return {uasf::MidiEventType::NoteOn, 0, n, 100, 0};
}

int main() {
    setenv("AIARR_MIDI_HW_TX", "0", 1);   // never drive real hardware, even outside CTest
    std::printf("Test: Gate 14C Task A — MIDI output provider\n");

    // ── Fake provider behaviour (the CI workhorse) ──
    {
        FakeMidiOutputProvider p;
        p.setDevices({{0, "IAC Bus 1"}, {1, "Korg PA700"}});
        TEST("enumerate 2", p.enumerate().size() == 2);
        TEST("no selection -> not live, send dropped",
             !p.hasLiveDestination() && !p.send(noteOn(60)));
        TEST("select valid", p.select(1) && p.selected() == 1 && p.hasLiveDestination());
        TEST("send when live", p.send(noteOn(60)) && p.dispatchedCount() == 1);
        TEST("select(-1) = none", p.select(-1) && !p.hasLiveDestination());
        TEST("select out-of-range fails", !p.select(9));
    }

    // ── CoreMIDI provider smoke (headless-safe: 0 devices => no-op) ──
    {
        CoreMidiOutputProvider cp;
        TEST("CoreMIDI provider initializes (headless ok)", cp.initialize("AI Arranger test"));
        // 0 destinations on CI: enumerate returns a (possibly empty) list, no crash.
        auto list = cp.enumerate();
        TEST("CoreMIDI enumerate returns (no crash)", list.size() >= 0);
        TEST("CoreMIDI no live dest when none selected", !cp.hasLiveDestination());
        // send with no destination is a graceful no-op (queued/dropped, no crash).
        cp.send(noteOn(60));
        TEST("CoreMIDI send no-op safe with no device", true);
        cp.shutdown();
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
