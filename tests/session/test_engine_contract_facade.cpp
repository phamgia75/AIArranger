// Gate 4 — LiveEngineFacade public contract (headless, no CoreMIDI).
//
// Asserts the *contract* surface the Gate 5 product layer depends on: capability
// queries, high-level lifecycle state + snapshot consistency, deterministic error
// reporting (QueueFull / DeviceUnavailable), command ordering, and repeated
// create/destroy. Links the portable core ONLY (no platform-apple) — the facade
// and its whole contract surface need no Apple framework.

#include "session/live_engine_facade.h"
#include "engine/chord/fingered_detector.h"
#include "../midi/fake_midi_input_source.h"
#include "../midi/fake_midi_output_provider.h"
#include <cstdio>

using namespace ai_arranger;
using session::LiveEngineFacade;
using session::LifecycleState;
using session::EngineError;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: Gate 4 LiveEngineFacade contract\n");

    midi::FakeMidiInputSource   fin;  fin.devices = {{0, "keyboard"}};
    midi::FakeMidiOutputProvider fout; fout.setDevices({{0, "synth"}});

    // ── Capabilities reflect wired devices + contract version ──
    {
        LiveEngineFacade fac(&fin, &fout);
        const auto caps = fac.capabilities();
        TEST("caps: has MIDI input",  caps.hasMidiInput);
        TEST("caps: has MIDI output", caps.hasMidiOutput);
        TEST("caps: chord detect + sections advertised",
             caps.hasChordDetect && caps.hasSections);
        // A capability is what a host can call. Suspend/resume is modelled in
        // the lifecycle state machine but has no facade or C entry point (#19).
        TEST("caps: suspend/resume NOT advertised (no callable API)", !caps.hasSuspendResume);
        TEST("caps: contract version matches header",
             caps.contractVersion == session::kEngineContractVersion);
        TEST("contractVersion() constant matches header",
             LiveEngineFacade::contractVersion() == session::kEngineContractVersion);
    }
    {
        LiveEngineFacade headless(nullptr, nullptr);
        const auto caps = headless.capabilities();
        TEST("caps: headless has no MIDI in/out",
             !caps.hasMidiInput && !caps.hasMidiOutput);
    }

    // ── Lifecycle state progression + snapshot mirror ──
    {
        LiveEngineFacade fac(&fin, &fout);
        TEST("lifecycle: starts Created", fac.lifecycleState() == LifecycleState::Created);

        fac.start();
        TEST("lifecycle: Running after start", fac.lifecycleState() == LifecycleState::Running);
        TEST("snapshot mirrors lifecycle (Running)",
             fac.snapshot().lifecycleState == static_cast<int32_t>(LifecycleState::Running));

        fac.selectMidiInput(0); fac.selectMidiOutput(0);
        fac.tick(48);
        fac.stop();
        TEST("lifecycle: Stopped after stop", fac.lifecycleState() == LifecycleState::Stopped);
        TEST("snapshot mirrors lifecycle (Stopped)",
             fac.snapshot().lifecycleState == static_cast<int32_t>(LifecycleState::Stopped));

        fac.start();
        TEST("lifecycle: Running again after restart", fac.lifecycleState() == LifecycleState::Running);
        fac.stop();
    }

    // ── Error reporting: DeviceUnavailable ──
    {
        LiveEngineFacade fac(&fin, &fout);
        TEST("error: Ok initially", fac.lastError() == EngineError::Ok);
        const bool bad = fac.selectMidiInput(99);   // out of range
        TEST("select invalid input returns false", !bad);
        TEST("error: DeviceUnavailable recorded", fac.lastError() == EngineError::DeviceUnavailable);
        fac.clearError();
        TEST("error: cleared back to Ok", fac.lastError() == EngineError::Ok);

        LiveEngineFacade headless(nullptr, nullptr);
        headless.selectMidiOutput(0);   // null provider
        TEST("error: null provider select -> DeviceUnavailable",
             headless.lastError() == EngineError::DeviceUnavailable);
    }

    // ── Error reporting: QueueFull when commands overflow undrained queue ──
    {
        LiveEngineFacade fac(&fin, &fout);
        fac.start();
        // maxCommandQueue == 256; push well past it WITHOUT ticking (no drain).
        for (int i = 0; i < 400; ++i) fac.intro();
        TEST("error: QueueFull after overflow", fac.lastError() == EngineError::QueueFull);
        // Draining then pushing within capacity clears the way (no new error).
        fac.clearError();
        fac.tick(48);            // drains
        fac.intro();
        TEST("error: no QueueFull within capacity after drain",
             fac.lastError() == EngineError::Ok);
        fac.stop();
    }

    // ── Command ordering: start then stop drains in FIFO -> ends stopped ──
    {
        LiveEngineFacade fac(&fin, &fout);
        fac.start();
        chord::FingeredDetector det; fac.setChordScanMode(&det);
        fac.transportStart();
        fac.transportStop();      // queued AFTER start
        for (int i = 0; i < 8; ++i) fac.tick(48);
        TEST("command ordering: stop-after-start leaves engine not playing",
             !fac.snapshot().playing);
        fac.stop();
    }

    // ── Repeated create/destroy is stable ──
    {
        bool ok = true;
        for (int i = 0; i < 200; ++i) {
            LiveEngineFacade fac(&fin, &fout);
            fac.start();
            fac.tick(48);
            fac.stop();
            if (fac.lifecycleState() != LifecycleState::Stopped) { ok = false; break; }
        }
        TEST("200x create/start/tick/stop/destroy stable", ok);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
