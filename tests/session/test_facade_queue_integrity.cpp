// Gate 4 contract — LiveEngineFacade command/input queue integrity (headless).
//
// Pins two promises of docs/architecture/ENGINE_PRODUCT_CONTRACT.md §2 that the
// Gate 5 host relies on:
//
//  1. "Transport / section / tempo / variation / panic commands: ANY thread."
//     Several threads may issue commands concurrently (e.g. the SwiftUI main
//     actor plus a foot-controller / background task). The command queue must be
//     multi-producer safe and hold exactly capabilities().maxCommandQueue items.
//
//  2. "Dropped commands are not silently lost." When tick() hands the drained
//     UI commands + MIDI input to the performer adapter, a full adapter queue
//     must not swallow events (a lost NoteOff leaves a held note -> stuck chord).
//     Anything the adapter cannot take this tick is delivered on the next one.
//
// Links the portable core ONLY. Run under -fsanitize=thread for the race check.

#include "session/live_engine_facade.h"
#include "../midi/fake_midi_input_source.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace ai_arranger;
using session::LiveEngineFacade;
using session::EngineError;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: LiveEngineFacade queue integrity (multi-producer + lossless drain)\n");

    // ── 1. Exactly maxCommandQueue commands fit; the next one is QueueFull ──
    {
        LiveEngineFacade fac(nullptr, nullptr);
        fac.start();
        const uint32_t cap = fac.capabilities().maxCommandQueue;
        for (uint32_t i = 0; i < cap; ++i) fac.setVariation(0);
        TEST("capacity: advertised maxCommandQueue commands all accepted",
             fac.lastError() == EngineError::Ok);
        fac.setVariation(0);
        TEST("capacity: command maxCommandQueue+1 reports QueueFull",
             fac.lastError() == EngineError::QueueFull);
        fac.stop();
    }

    // ── 2. Concurrent producers: no lost/duplicated slots, exact capacity ──
    // 4 threads race to fill the (undrained) queue with exactly `cap` commands.
    // An SPSC ring under concurrent producers loses slots (two producers claim the
    // same head), leaving spare room -> the extra push would wrongly succeed.
    {
        bool allExact = true;
        for (int round = 0; round < 50 && allExact; ++round) {
            LiveEngineFacade fac(nullptr, nullptr);
            fac.start();
            const uint32_t cap = fac.capabilities().maxCommandQueue;
            constexpr int kThreads = 4;
            std::atomic<bool> go{false};
            std::vector<std::thread> ts;
            for (int t = 0; t < kThreads; ++t) {
                ts.emplace_back([&] {
                    while (!go.load(std::memory_order_acquire)) {}
                    for (uint32_t i = 0; i < cap / kThreads; ++i) fac.setVariation(1);
                });
            }
            go.store(true, std::memory_order_release);
            for (auto& th : ts) th.join();
            const bool noneDropped = fac.lastError() == EngineError::Ok;
            fac.setVariation(1);
            const bool nowFull = fac.lastError() == EngineError::QueueFull;
            allExact = noneDropped && nowFull;
            fac.stop();
        }
        TEST("multi-producer: 4 threads fill exactly maxCommandQueue slots (50 rounds)",
             allExact);
    }

    // ── 3. Concurrent producers while the engine thread ticks (TSan target) ──
    {
        LiveEngineFacade fac(nullptr, nullptr);
        fac.start();
        std::atomic<bool> done{false};
        std::thread engine([&] {
            while (!done.load(std::memory_order_acquire)) fac.tick(48);
            fac.tick(48);
        });
        std::vector<std::thread> producers;
        for (int t = 0; t < 3; ++t) {
            producers.emplace_back([&, t] {
                for (int i = 0; i < 20000; ++i) {
                    if (t == 0) fac.setVariation(i & 3);
                    else if (t == 1) fac.fill();
                    else fac.setTempo(100 + (i % 40));
                }
            });
        }
        for (auto& th : producers) th.join();
        done.store(true, std::memory_order_release);
        engine.join();
        TEST("multi-producer + ticking engine: completes, engine still running",
             fac.snapshot().lifecycleState ==
                 static_cast<int32_t>(session::LifecycleState::Running));
        fac.stop();
    }

    // ── 4. Drain is lossless: adapter-queue overflow must not drop a NoteOff ──
    // Hold a C-major triad below the split, then fill the command queue to
    // capacity and release the triad in the same tick window. Commands drain
    // first, so without back-pressure the NoteOffs overflow the adapter's queue,
    // are silently discarded, and the triad stays "held" (stuck chord).
    {
        midi::FakeMidiInputSource fin; fin.devices = {{0, "keyboard"}};
        LiveEngineFacade fac(&fin, nullptr);
        fac.start();
        const uint8_t on[]  = {0x90, 48, 100,  0x90, 52, 100,  0x90, 55, 100};
        const uint8_t off[] = {0x80, 48, 0,    0x80, 52, 0,    0x80, 55, 0};

        fin.inject(on, sizeof(on));
        fac.tick(48);
        TEST("lossless drain: triad held before the burst",
             fac.session().adapter().heldCount() == 3);

        const uint32_t cap = fac.capabilities().maxCommandQueue;
        for (uint32_t i = 0; i < cap; ++i) fac.setVariation(0);
        fin.inject(off, sizeof(off));
        for (int i = 0; i < 4; ++i) fac.tick(48);
        TEST("lossless drain: NoteOffs behind a full command burst arrive (no stuck notes)",
             fac.session().adapter().heldCount() == 0);
        TEST("lossless drain: nothing dropped -> no QueueFull recorded",
             fac.lastError() == EngineError::Ok);

        // Same burst in the other direction: NoteOns behind the burst are kept.
        for (uint32_t i = 0; i < cap; ++i) fac.setVariation(0);
        fin.inject(on, sizeof(on));
        for (int i = 0; i < 4; ++i) fac.tick(48);
        TEST("lossless drain: NoteOns behind a full command burst arrive",
             fac.session().adapter().heldCount() == 3);
        fac.stop();
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
