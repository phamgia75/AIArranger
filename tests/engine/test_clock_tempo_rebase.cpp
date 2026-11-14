// A tempo change while running must keep the musical position and change only
// the speed from there on. Position used to be samples * ticks/sample at the
// *current* tempo, so 60 -> 240 BPM after 1 s jumped 1440 ticks ahead (the
// scheduler then flushed every note in between) and 240 -> 60 jumped back.

#include "engine/realtime/clock.h"
#include <cstdio>

using ai_arranger::realtime::RealtimeClock;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

int main() {
    std::printf("Test: clock tempo rebase\n");
    RealtimeClock c;
    c.setSampleRate(48000);
    c.setResolution(480);
    c.setTempo(60);            // 1 quarter per second = 480 ticks/s
    c.reset();
    c.start();
    c.advance(48000);
    TEST("60 BPM, 1 s -> tick 480", c.getPosition() == 480);

    c.setTempo(240);
    TEST("speed-up keeps the position", c.getPosition() == 480);
    c.advance(48000);
    TEST("240 BPM, +1 s -> +1920 ticks", c.getPosition() == 2400);

    c.setTempo(60);
    TEST("slow-down keeps the position (no jump back)", c.getPosition() == 2400);
    c.advance(24000);
    TEST("60 BPM, +0.5 s -> +240 ticks", c.getPosition() == 2640);

    TEST("sample position stays monotonic", c.getSamplePosition() == 120000);

    bool mono = true;
    int64_t last = c.getPosition();
    for (uint32_t bpm = 40; bpm <= 400; bpm += 7) {
        c.setTempo(bpm);
        mono &= c.getPosition() == last;
        c.advance(511);
        mono &= c.getPosition() >= last;
        last = c.getPosition();
    }
    TEST("many tempo changes: never jumps, never goes back", mono);

    c.setTempo(400);
    c.reset();
    TEST("reset -> tick 0", c.getPosition() == 0);
    c.advance(48000);
    TEST("after reset the current tempo applies from 0 (400 BPM: 3200)", c.getPosition() == 3200);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
