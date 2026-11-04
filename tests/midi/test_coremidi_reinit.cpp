// CoreMIDI adapters can be initialised again (#23).
//
// The macOS host's Refresh re-attempts a failed CoreMIDI init: it calls
// initialize() again on the same CoreMidiIn / CoreMidiOutputProvider. That is
// only safe if initialize() is idempotent once it has succeeded and a torn-down
// adapter starts cleanly again, every time (no leaked client/port/thread). A
// real init failure (MIDIClientCreate -304, #21) cannot be forced here; the
// failure path leaves the adapter uninitialised, the same state as after
// shutdown(), which this test covers.

#include "engine/midi/coremidi_in.h"
#include "midi/coremidi/coremidi_output_provider.h"
#include <cstdio>

using namespace ai_arranger;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: CoreMIDI adapters re-initialise cleanly\n");
    midi::CoreMidiIn in;
    midi::CoreMidiOutputProvider out;

    TEST("input: not initialised before initialize()", !in.isInitialized());
    TEST("input: initialize", in.initialize("AIA reinit test"));
    TEST("input: second initialize() is a no-op success", in.initialize("AIA reinit test") && in.isInitialized());
    TEST("output: initialize", out.initialize("AIA reinit test"));
    TEST("output: second initialize() is a no-op success", out.initialize("AIA reinit test") && out.raw().isInitialized());

    bool cycles = true;
    for (int i = 0; i < 25 && cycles; ++i) {
        in.shutdown(); out.shutdown();
        cycles = !in.isInitialized() && !out.raw().isInitialized() &&
                 in.initialize("AIA reinit test") && out.initialize("AIA reinit test");
    }
    TEST("25 shutdown -> initialize cycles succeed on the same objects", cycles);
    TEST("enumeration works after re-init (headless: may be empty)",
         in.enumerateSources().size() == static_cast<size_t>(in.sourceCount()) && out.enumerate().size() == static_cast<size_t>(out.raw().destinationCount()));
    in.shutdown(); out.shutdown();
    TEST("shutdown is idempotent", (in.shutdown(), out.shutdown(), !in.isInitialized() && !out.raw().isInitialized()));

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
