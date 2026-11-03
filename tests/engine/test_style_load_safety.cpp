// Style load safety — Issues #4 (grid) and #5 (concurrency).
//
// #4: a style loaded while the transport played kept the old PPQN/meter until
//     the next Start, so its events ran on the wrong grid (e.g. a 1920-PPQN
//     style on a 480-PPQN clock = 4x slow, loops cut at the old bar length).
//     Contract now: loadStyle is rejected while playing (EngineError::
//     InvalidState, nothing changes); with the transport stopped it adopts the
//     new style's tempo, PPQN and meter at once.
// #5: LiveEngineFacade::loadStyle wrote the style, section mappings and grid
//     while tick() could read them on another thread. Contract now: the two
//     exclude each other; tick() never waits (it skips its step during a load).
//     The stress block below is the TSan reproducer.

#include "session/live_engine_facade.h"
#include "engine/arranger/style_player.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "../midi/fake_midi_output_provider.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <thread>

using namespace ai_arranger;
using session::EngineError;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } \
} while(0)

static uasf::StyleDefinition corpus(const char* needle) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(CORPUS_DIR, ec)) {
        const std::string fn = e.path().filename().string();
        if (fn.find(needle) != std::string::npos && e.path().extension() == ".sty") {
            importers::sff1::Sff1Reader r;
            importers::sff1::Sff1ToUasfMapper m;
            return m.map(r.parseFile(e.path().string())).style;
        }
    }
    return {};
}

// Ticks per (meter) beat + ticks per bar pin PPQN and meter: 4/4 @1920 =
// 1920/7680, 6/8 @1920 = 960/5760, 4/4 @480 = 480/1920.
struct Grid { int64_t beat; int64_t bar; uint32_t bpm; };
static Grid grid(session::LiveEngineFacade& f) {
    auto& c = f.session().clock();
    return {c.ticksPerBeat(), c.ticksPerBar(), c.getTempo()};
}
static void ticks(session::LiveEngineFacade& f, int n) { for (int i = 0; i < n; ++i) f.tick(48); }

// NoteOn/NoteOff balance per (channel, note) over everything sent.
static long outstanding(const midi::FakeMidiOutputProvider& out) {
    std::map<int, long> open;
    for (const auto& e : out.sent) {
        const int k = (e.channel << 8) | e.data1;
        if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) ++open[k];
        else if ((e.type == uasf::MidiEventType::NoteOff ||
                  (e.type == uasf::MidiEventType::NoteOn && e.data2 == 0)) && open[k] > 0) --open[k];
    }
    long n = 0; for (const auto& [k, v] : open) n += v; return n;
}

int main() {
    std::printf("Test: style load safety (grid while playing, concurrency with tick)\n");
    const auto demo = arranger::buildDemoStyle();        // 480 PPQN, 4/4, 120 BPM
    const auto pop  = corpus("POP_ACOUSTIC_2");          // 1920 PPQN, 4/4, 100 BPM
    const auto cls  = corpus("CLASSIC_6_8");             // 1920 PPQN, 6/8, 90 BPM
    TEST("corpus styles imported", !pop.sections.empty() && !cls.sections.empty());

    midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
    session::LiveEngineFacade f(nullptr, &out);
    f.selectMidiOutput(0);
    f.start();

    // ── #4: loads while stopped adopt the new grid at once, every direction ──
    struct Step { const char* name; const uasf::StyleDefinition* s; int64_t beat; int64_t bar; uint32_t bpm; };
    const Step steps[] = {{"480 4/4 -> 1920 4/4", &pop, 1920, 7680, 100},
                          {"1920 4/4 -> 1920 6/8", &cls, 960, 5760, 90},
                          {"6/8 1920 -> 4/4 480", &demo, 480, 1920, 120},
                          {"4/4 480 -> 6/8 1920", &cls, 960, 5760, 90},
                          {"6/8 1920 -> 4/4 1920", &pop, 1920, 7680, 100}};
    for (const auto& st : steps) {
        const EngineError e = f.loadStyle(*st.s);
        const Grid g = grid(f);
        TEST(std::string("stopped load ") + st.name + ": Ok, beat/bar " + std::to_string(st.beat) + "/" +
             std::to_string(st.bar) + ", " + std::to_string(st.bpm) + " BPM",
             e == EngineError::Ok && g.beat == st.beat && g.bar == st.bar && g.bpm == st.bpm);
    }

    // ── #4: a load while playing is rejected and changes nothing ──
    f.loadStyle(cls);                                    // 1920, 6/8, 90
    f.transportStart(); ticks(f, 3000);                  // into the style
    const auto before = f.snapshot();
    const Grid g0 = grid(f);
    f.clearError();
    const EngineError rej = f.loadStyle(demo);
    TEST("load while playing: rejected with InvalidState", rej == EngineError::InvalidState);
    TEST("load while playing: lastError records it", f.lastError() == EngineError::InvalidState);
    const Grid g1 = grid(f);
    TEST("load while playing: PPQN, bar and tempo unchanged",
         g1.beat == g0.beat && g1.bar == g0.bar && g1.bpm == g0.bpm);
    ticks(f, 3000);
    TEST("load while playing: the playing style keeps playing its sections",
         f.snapshot().playing && f.snapshot().section >= 0 &&
         f.snapshot().positionTicks > before.positionTicks);

    // ── restart after a load: Stop -> load -> Start plays the new style ──
    f.transportStop(); ticks(f, 10);
    TEST("after Stop: no outstanding notes", outstanding(out) == 0);
    TEST("stopped load after playing: Ok", f.loadStyle(demo) == EngineError::Ok);
    f.transportStart(); ticks(f, 10);
    const Grid g2 = grid(f);
    TEST("restart after load: plays on the new grid (480, 1920-tick bar, 120 BPM)",
         f.snapshot().playing && g2.beat == 480 && g2.bar == 1920 && g2.bpm == 120);
    TEST("restart after load: starts in section 0 (Intro)", f.snapshot().section == 0);
    f.transportStop(); ticks(f, 10);

    // ── repeated loads while stopped: the last one wins ──
    bool allOk = true;
    for (int i = 0; i < 50; ++i) allOk = allOk && f.loadStyle(i % 2 ? pop : cls) == EngineError::Ok;
    TEST("50 alternating stopped loads all Ok", allOk);
    const Grid g3 = grid(f);
    TEST("after 50 loads the last style's grid is active (POP: 1920, 7680)", g3.beat == 1920 && g3.bar == 7680);
    f.stop();

    // ── #5: loadStyle on one thread, tick() on another (TSan reproducer) ──
    {
        midi::FakeMidiOutputProvider o2; o2.setDevices({{0, "synth"}});
        session::LiveEngineFacade g(nullptr, &o2);
        g.selectMidiOutput(0);
        g.loadStyle(demo);
        g.start();
        std::atomic<bool> stopTicking{false};
        std::atomic<long> tickCalls{0};
        std::thread engine([&] {
            while (!stopTicking.load(std::memory_order_acquire)) {
                g.tick(48);
                tickCalls.fetch_add(1, std::memory_order_relaxed);
            }
        });
        int ok = 0, rejected = 0, other = 0;
        for (int i = 0; i < 400; ++i) {
            // While stopped, the engine thread applies commands that read the
            // style as this thread replaces it: Variation reads the Main-section
            // mapping, Fill / Intro walk the sequencer's section array (which
            // points into the style being reallocated). Every 50th round also
            // starts the transport, so some loads land while playing.
            g.setVariation(i % 4);
            g.fill();
            g.intro();
            if (i % 50 == 25) g.transportStart();
            const EngineError e = g.loadStyle(i % 3 == 0 ? pop : i % 3 == 1 ? cls : demo);
            if (e == EngineError::Ok) ++ok; else if (e == EngineError::InvalidState) ++rejected; else ++other;
            if (i % 50 == 26) g.transportStop();
        }
        // Quiesce deterministically: stop the engine thread, drain every queued
        // command on this thread (an earlier Start may still be queued), and only
        // then send Stop — under load the 256-slot command queue can be full, and
        // a push into a full queue is dropped (EngineError::QueueFull, by contract).
        stopTicking.store(true, std::memory_order_release);
        engine.join();
        ticks(g, 1000);
        g.transportStop();
        ticks(g, 10);
        std::printf("  [concurrent] loads ok=%d rejected=%d ticks=%ld\n", ok, rejected, tickCalls.load());
        TEST("concurrent: every load returned Ok or InvalidState", other == 0 && ok + rejected == 400);
        TEST("concurrent: loads succeeded while stopped", ok > 0);
        TEST("concurrent: the engine thread kept ticking", tickCalls.load() > 0);
        // A final stopped load + Start plays that style's grid, cleanly.
        TEST("concurrent: transport stopped after the drain", !g.snapshot().playing);
        TEST("concurrent: final stopped load Ok", g.loadStyle(cls) == EngineError::Ok);
        g.transportStart(); ticks(g, 10);
        TEST("concurrent: lifecycle after swaps: playing CLASSIC_6_8 on 1920 / 5760",
             g.snapshot().playing && g.session().clock().ticksPerBar() == 5760);
        g.transportStop(); ticks(g, 10);
        TEST("concurrent: no outstanding notes after Stop", outstanding(o2) == 0);
        g.stop();
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
