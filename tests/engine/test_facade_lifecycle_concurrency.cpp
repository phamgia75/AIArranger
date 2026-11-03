// LiveEngineFacade start()/stop() vs a concurrently ticking engine thread (#10).
//
// start() boots the session, wires the output route and the lifecycle tracker;
// stop() shuts the session down (stopping playback, flushing notes) and updates
// the tracker. tick() reads all of that on the engine thread. A host that kept
// ticking across start()/stop() raced them (the macOS host quiesces its timer,
// so it never did). They now share loadStyle's exclusion: tick() skips its step
// while a lifecycle call runs and never waits. Run under TSan, this test is the
// reproducer.

#include "session/live_engine_facade.h"
#include "engine/arranger/style_player.h"
#include "../midi/fake_midi_output_provider.h"
#include <atomic>
#include <cstdio>
#include <map>
#include <thread>

using namespace ai_arranger;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: facade start/stop vs a concurrently ticking engine thread\n");
    midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
    session::LiveEngineFacade f(nullptr, &out);
    f.selectMidiOutput(0);
    f.loadStyle(arranger::buildDemoStyle());

    std::atomic<bool> stopTicking{false};
    std::atomic<long> tickCalls{0};
    std::thread engine([&] {
        while (!stopTicking.load(std::memory_order_acquire)) {
            f.tick(48);
            tickCalls.fetch_add(1, std::memory_order_relaxed);
        }
    });

    long cycles = 0;
    for (int i = 0; i < 200; ++i) {
        f.start();
        f.transportStart();
        f.setVariation(i % 4);
        for (int k = 0; k < 50; ++k) std::this_thread::yield();   // let it play a little
        f.stop();
        ++cycles;
    }
    stopTicking.store(true, std::memory_order_release);
    engine.join();
    for (int i = 0; i < 100; ++i) f.tick(0);   // drain anything still queued

    TEST("200 start/stop cycles completed while the engine thread ticked",
         cycles == 200 && tickCalls.load() > 0);
    TEST("final lifecycle state is Stopped",
         f.lifecycleState() == session::LifecycleState::Stopped &&
         f.snapshot().lifecycleState == static_cast<int32_t>(session::LifecycleState::Stopped));
    TEST("transport not playing after the final stop()", !f.snapshot().playing);

    // Note state as a MIDI receiver sees it: NoteOff / velocity-0 NoteOn close
    // one note; CC 120 (All Sound Off) and CC 123 (All Notes Off) close every
    // note on their channel (stop() silences with these — see the issue on
    // stop() dropping its explicit NoteOffs).
    std::map<int, long> open;
    for (const auto& e : out.sent) {
        const int k = (e.channel << 8) | e.data1;
        if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) ++open[k];
        else if ((e.type == uasf::MidiEventType::NoteOff ||
                  (e.type == uasf::MidiEventType::NoteOn && e.data2 == 0)) && open[k] > 0) --open[k];
        else if (e.type == uasf::MidiEventType::ControlChange && (e.data1 == 120 || e.data1 == 123))
            for (auto& [key, n] : open) if ((key >> 8) == e.channel) n = 0;
    }
    long stuck = 0; for (const auto& [k, v] : open) stuck += v;
    TEST("no note left sounding after the cycles (receiver view)", stuck == 0);

    // A command queued before stop() must not act after it: a transportStart
    // not yet applied used to be drained by the next tick and restart playback
    // on a stopped engine (lifecycle Stopped, transport playing, notes sent).
    {
        midi::FakeMidiOutputProvider o2; o2.setDevices({{0, "synth"}});
        session::LiveEngineFacade g(nullptr, &o2);
        g.selectMidiOutput(0);
        g.loadStyle(arranger::buildDemoStyle());
        g.start();
        g.transportStart();                 // queued, not applied yet
        g.stop();                           // before any tick
        for (int i = 0; i < 100; ++i) g.tick(48);
        size_t ons = 0;
        for (const auto& e : o2.sent) if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) ++ons;
        TEST("command queued before stop() is discarded: not playing, no notes sent",
             !g.snapshot().playing && ons == 0);
        g.start(); g.transportStart();
        for (int i = 0; i < 10; ++i) g.tick(48);
        TEST("a new start() + transportStart() after that plays normally", g.snapshot().playing);
        g.stop();
    }

    // The engine still works normally afterwards (single-threaded).
    f.start(); f.transportStart();
    for (int i = 0; i < 100; ++i) f.tick(48);
    TEST("restart afterwards plays", f.snapshot().playing);
    f.stop();

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
