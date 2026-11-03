// Tempo control model (Issue #7) — the logic behind the macOS tempo slider,
// UI-toolkit-agnostic and headless.
//
// The slider used to start at 120 and never follow the engine: after loading a
// 76-BPM style the label said 76 while the slider still said 120, and the first
// touch jumped the tempo to the slider's absolute value. Its range (40-300) also
// differed from the engine's 20-400.

#include "ui/tempo_control_model.h"
#include <cstdio>
#include <vector>

using namespace ai_arranger::ui;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: tempo control model (slider <-> engine tempo)\n");
    std::vector<uint32_t> sent;
    TempoControlModel m;
    m.setTempoSink([&](uint32_t bpm) { sent.push_back(bpm); });

    TEST("range matches the engine: 20..400", TempoControlModel::kMin == 20 && TempoControlModel::kMax == 400);

    // Engine boots at 120: the first poll sets the slider.
    TEST("first poll: slider follows the engine (changed)", m.syncFromEngine(120) && m.displayed() == 120);
    TEST("same tempo again: no change", !m.syncFromEngine(120));

    // Load a 76-BPM style: the slider follows.
    TEST("style load: slider follows the style tempo (76)", m.syncFromEngine(76) && m.displayed() == 76);
    // Stop / Start keep the tempo: nothing to do.
    TEST("Stop/Start (same tempo): no change", !m.syncFromEngine(76) && m.displayed() == 76);
    TEST("syncing never sends a tempo (no feedback loop)", sent.empty());

    // User moves the slider to 110: sent once; the engine applies it on its next
    // tick, so a poll that still reports 76 must not snap the slider back.
    m.userSet(110.0);
    TEST("user change: sent once, clamped value 110", sent.size() == 1 && sent[0] == 110);
    TEST("user change: displayed 110 at once", m.displayed() == 110);
    TEST("engine not caught up yet (76): slider holds 110", !m.syncFromEngine(76) && m.displayed() == 110);
    TEST("engine applied it (110): steady", !m.syncFromEngine(110) && m.displayed() == 110);
    TEST("later engine changes are followed again (load 90)", m.syncFromEngine(90) && m.displayed() == 90);
    TEST("still no tempo sent by syncing", sent.size() == 1);

    // Clamping to the engine range.
    m.userSet(500.0);
    m.userSet(5.0);
    m.userSet(104.6);
    TEST("user values clamp to 20..400 and round to whole BPM",
         sent.size() == 4 && sent[1] == 400 && sent[2] == 20 && sent[3] == 105);
    m.syncFromEngine(105);

    // A user value the engine never reports (e.g. dropped command) is not held
    // forever: after kHoldPolls polls the slider follows the engine again.
    m.userSet(130.0);
    bool held = true;
    for (int i = 0; i < TempoControlModel::kHoldPolls - 1; ++i) held = held && !m.syncFromEngine(105);
    TEST("unconfirmed user value held for the hold window", held && m.displayed() == 130);
    TEST("after the hold window the slider follows the engine (105)", m.syncFromEngine(105) && m.displayed() == 105);

    // An engine tempo outside the slider range is shown clamped (never off-scale).
    TempoControlModel n;
    TEST("engine tempo above range shown at 400", n.syncFromEngine(450) && n.displayed() == 400);

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
