// StylePlayer tempo ownership.
//
// The performer's tempo must survive Start. start() used to re-apply the style's
// tempo on every Start, so `set_tempo(100)` followed by Start played at the
// style's 120 BPM (visible through the C ABI: aiarr_engine_set_tempo then
// aiarr_engine_transport_start -> snapshot.tempoBpm == 120). Arranger semantics:
// selecting a style (while stopped) adopts its tempo; Start/Stop keep whatever
// tempo is current; swapping style while playing keeps the running tempo.

#include "engine/arranger/style_player.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include <cstdio>

using namespace ai_arranger::arranger;
using namespace ai_arranger::realtime;
using namespace ai_arranger::midi;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: StylePlayer tempo ownership\n");
    RealtimeClock clock;
    MidiScheduler scheduler;
    PanicHandler panic;
    StylePlayer player(clock, scheduler, panic);

    auto ballad = buildDemoStyle();
    ballad.tempo_bpm = 90;
    player.loadStyle(ballad);
    TEST("load while stopped adopts the style tempo (90)", clock.getTempo() == 90);

    TEST("start succeeds", player.start(0));
    TEST("start keeps the loaded style tempo (90)", clock.getTempo() == 90);

    clock.setTempo(104);                 // performer tempo change while playing
    player.stop();
    TEST("start after a performer tempo change keeps it (104)",
         player.start(0) && clock.getTempo() == 104);

    auto fast = buildDemoStyle();
    fast.tempo_bpm = 140;
    player.loadStyle(fast);              // style swap while playing
    TEST("style swap while playing keeps the running tempo (104)", clock.getTempo() == 104);

    player.stop();
    player.loadStyle(fast);              // re-select while stopped
    TEST("re-selecting a style while stopped adopts its tempo (140)", clock.getTempo() == 140);

    auto zero = buildDemoStyle();
    zero.tempo_bpm = 0;
    player.loadStyle(zero);
    TEST("a style without a tempo (0) leaves the current tempo (140)", clock.getTempo() == 140);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
