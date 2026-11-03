// Tempo precedence through the product path (LiveEngineFacade) — Issue #2.
//
// Contract (unchanged from fd18ad4, now exercised with real style tempos):
//   1. Selecting a style while the transport is stopped adopts the style tempo,
//      whether the style is loaded before or after the engine boots.
//   2. A performer tempo (setTempo, applied on the next tick) set after the style
//      is selected is kept by Start; Start never re-applies the style tempo.
//   3. Stop / Start keeps the current tempo (no drift, no reset).
//   4. Selecting another style while stopped adopts the new style's tempo;
//      selecting one while playing is rejected (Issue #4) and changes nothing.

#include "session/live_engine_facade.h"
#include "engine/arranger/style_player.h"
#include "../midi/fake_midi_output_provider.h"
#include <cstdio>
#include <string>

using namespace ai_arranger;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

static uasf::StyleDefinition styleAt(uint32_t bpm) {
    auto s = arranger::buildDemoStyle();
    s.tempo_bpm = bpm;
    return s;
}

int main() {
    std::printf("Test: tempo precedence through LiveEngineFacade\n");

    {   // Case 1 (load, then boot): style tempo reaches the running clock.
        midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
        session::LiveEngineFacade f(nullptr, &out);
        f.loadStyle(styleAt(90));
        f.start();
        f.transportStart();
        f.tick(48);
        TEST("load 90 -> boot -> Start: plays at 90", f.snapshot().playing && f.snapshot().tempoBpm == 90);

        // Case 3: Stop / Start keeps it.
        f.transportStop();  f.tick(48);
        f.transportStart(); f.tick(48);
        TEST("Stop/Start keeps 90", f.snapshot().playing && f.snapshot().tempoBpm == 90);

        // Case 2: performer tempo set after selection wins at the next Start.
        f.transportStop(); f.tick(48);
        f.setTempo(110);   f.tick(48);
        f.transportStart(); f.tick(48);
        TEST("setTempo(110) then Start: plays at 110, not the style's 90",
             f.snapshot().playing && f.snapshot().tempoBpm == 110);
        f.transportStop(); f.tick(48);
        f.transportStart(); f.tick(48);
        TEST("Stop/Start after a performer change keeps 110", f.snapshot().tempoBpm == 110);

        // Case 4a: a load while playing is rejected (Issue #4); tempo unchanged.
        TEST("load while playing rejected", f.loadStyle(styleAt(76)) == session::EngineError::InvalidState);
        f.tick(48);
        TEST("rejected load keeps the running 110", f.snapshot().tempoBpm == 110);

        // Case 4b: another style while stopped adopts its tempo.
        f.transportStop(); f.tick(48);
        f.loadStyle(styleAt(76));
        f.transportStart(); f.tick(48);
        TEST("style selected while stopped: plays at its 76", f.snapshot().playing && f.snapshot().tempoBpm == 76);
        f.stop();
    }
    {   // Case 1 (boot, then load): same result in the other order.
        midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
        session::LiveEngineFacade f(nullptr, &out);
        f.start();                       // boots with the demo style at 120
        f.loadStyle(styleAt(105));
        f.transportStart();
        f.tick(48);
        TEST("boot -> load 105 -> Start: plays at 105", f.snapshot().playing && f.snapshot().tempoBpm == 105);
        f.stop();
    }
    {   // A style without a tempo (0) keeps whatever tempo is current.
        midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
        session::LiveEngineFacade f(nullptr, &out);
        f.loadStyle(styleAt(90));
        f.loadStyle(styleAt(0));
        f.start();
        f.transportStart();
        f.tick(48);
        TEST("tempo-less style keeps the current 90", f.snapshot().tempoBpm == 90);
        f.stop();
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
