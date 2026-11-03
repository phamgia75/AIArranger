// Performer FSM follows the transport when an Ending stops it by itself.
//
// Since sections have a lifecycle (Ending plays out -> STOPPED), the transport
// can stop without a Stop command. The adapter's FSM stayed in Playing, so the
// snapshot reported performerState=Playing with playing=0 — the UI and any
// FSM-gated logic disagreed with the engine.

#include "session/live_engine_facade.h"
#include "engine/performance/realtime_state_machine.h"
#include <cstdio>

using namespace ai_arranger;
using performance::PerformerState;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

static int st(const session::LiveEngineFacade& f) { return f.snapshot().performerState; }

int main() {
    std::printf("Test: performer state after an Ending plays out\n");
    session::LiveEngineFacade f(nullptr, nullptr);
    f.start();
    f.transportStart();
    auto bars = [&](int n) { for (int i = 0; i < n * 2000; ++i) f.tick(48); };   // 120 BPM, 1 ms ticks
    bars(5);
    TEST("playing Main: FSM Playing", f.snapshot().playing && st(f) == static_cast<int>(PerformerState::Playing));

    f.ending();
    bars(4);                               // 2-bar demo Ending plays out
    TEST("ending played out: transport stopped", !f.snapshot().playing);
    TEST("ending played out: FSM Idle (not stuck in Playing)",
         st(f) == static_cast<int>(PerformerState::Idle));

    f.transportStart();
    bars(1);
    TEST("Start after the ending: playing again, FSM Playing",
         f.snapshot().playing && st(f) == static_cast<int>(PerformerState::Playing));

    f.syncStart();                         // armed while stopped must not be touched
    f.transportStop();
    bars(1);
    f.syncStart();
    bars(1);
    TEST("sync-armed while stopped stays Armed", st(f) == static_cast<int>(PerformerState::Armed));
    f.stop();

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
