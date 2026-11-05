#include "engine/midi/coremidi_out.h"
#include "engine/uasf/types.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace ai_arranger::midi;
using namespace ai_arranger::uasf;

static int failures = 0;
static int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  ❌ FAIL: %s\n", name); failures++; } \
    else { std::printf("  ✅ PASS: %s\n", name); passes++; } \
} while(0)

static MidiEvent ev(MidiEventType t, uint8_t ch, uint8_t d1, uint8_t d2) {
    MidiEvent e; e.type = t; e.channel = ch; e.data1 = d1; e.data2 = d2; e.tick = 0;
    return e;
}

int main() {
    setenv("AIARR_MIDI_HW_TX", "0", 1);   // never drive real hardware, even outside CTest
    std::printf("Test: CoreMidiOut\n");

    // ── Pure conversion (no hardware) ────────────────────────────────
    uint8_t b[3] = {0,0,0};
    TEST("NoteOn → 3 bytes", midiEventToBytes(ev(MidiEventType::NoteOn, 0, 60, 100), b) == 3);
    TEST("NoteOn status byte 0x90", b[0] == 0x90 && b[1] == 60 && b[2] == 100);
    TEST("Channel folded into status", midiEventToBytes(ev(MidiEventType::NoteOn, 5, 60, 100), b) == 3 && b[0] == 0x95);
    TEST("Channel masked to 4 bits", midiEventToBytes(ev(MidiEventType::NoteOn, 200, 60, 100), b) == 3 && (b[0] & 0xF0) == 0x90);
    TEST("NoteOff → 0x80", midiEventToBytes(ev(MidiEventType::NoteOff, 0, 60, 0), b) == 3 && b[0] == 0x80);
    TEST("ControlChange → 0xB0", midiEventToBytes(ev(MidiEventType::ControlChange, 0, 7, 127), b) == 3 && b[0] == 0xB0);
    TEST("ProgramChange → 2 bytes", midiEventToBytes(ev(MidiEventType::ProgramChange, 0, 5, 0), b) == 2 && b[0] == 0xC0);
    TEST("ChannelPressure → 2 bytes", midiEventToBytes(ev(MidiEventType::ChannelPressure, 0, 64, 0), b) == 2);
    TEST("Data bytes masked to 7 bits", midiEventToBytes(ev(MidiEventType::NoteOn, 0, 200, 200), b) == 3 && b[1] == (200 & 0x7F) && b[2] == (200 & 0x7F));

    // ── Lifecycle + enumeration (works headless) ─────────────────────
    CoreMidiOut out;

    // Tap counts dispatched events on the dispatch thread.
    std::atomic<int> tapCount{0};
    out.setDispatchTap([&](const MidiEvent&){ tapCount.fetch_add(1, std::memory_order_relaxed); });

    bool init = out.initialize("AI Arranger Test");
    TEST("Initialize succeeds (even with 0 destinations)", init);
    TEST("Is initialized", out.isInitialized());

    int destCount = out.destinationCount();
    TEST("Destination count is non-negative", destCount >= 0);
    auto dests = out.enumerateDestinations();
    TEST("Enumerate size matches count", static_cast<int>(dests.size()) == destCount);

    // Selecting "none" is always valid and yields no live destination.
    TEST("Select none (-1)", out.selectDestination(-1));
    TEST("No live destination after select none", !out.hasLiveDestination());

    // Out-of-range selection is rejected.
    TEST("Select out-of-range rejected", !out.selectDestination(destCount + 100));

    // ── Send + dispatch (graceful no-op when no destination) ─────────
    out.selectDestination(-1); // ensure no real hardware is driven by the test
    const int kSendCount = 64;
    for (int i = 0; i < kSendCount; ++i) {
        out.send(ev(MidiEventType::NoteOn, 0, 60, 100));
    }
    // Wait for the dispatch thread to drain (generous budget for a cold-start
    // CoreMIDI client + thread spin-up, so the test is not timing-flaky).
    for (int i = 0; i < 1000 && out.dispatchedCount() < (uint64_t)kSendCount; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    TEST("All sent events dispatched", out.dispatchedCount() >= (uint64_t)kSendCount);
    TEST("Dispatch tap fired for every event", tapCount.load() >= kSendCount);
    TEST("No destination → events dropped gracefully (no crash)", out.droppedCount() >= (uint64_t)kSendCount);
    TEST("Nothing actually MIDISent without destination", out.sentCount() == 0);

    // ── Shutdown is clean and idempotent ─────────────────────────────
    out.shutdown();
    TEST("Not initialized after shutdown", !out.isInitialized());
    out.shutdown(); // idempotent
    TEST("Double shutdown is safe", !out.isInitialized());

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
