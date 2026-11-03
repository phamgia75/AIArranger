// MIDI scheduler timing (Issue #16): a future event at the head of the queue
// must not hold back events that are already due.
//
// advanceTo() popped FIFO and, on a future event, pushed it to the back and
// stopped — so with swing (off-beats scheduled slightly ahead) every due event
// behind a swung one waited for a later block: up to 56 ticks (~17 ms) late on
// POP_ACOUSTIC_2 at 75% swing, and the pushed-back event moved behind events
// scheduled after it.

#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include "engine/realtime/clock.h"
#include "engine/arranger/style_player.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace ai_arranger;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } \
} while(0)

static uasf::MidiEvent ev(uint64_t tick, uint8_t note) {
    return {uasf::MidiEventType::NoteOn, 0, note, 100, tick};
}

int main() {
    std::printf("Test: scheduler dispatches every due event (no head-of-line blocking)\n");

    {   // A future event at the head does not hold back due events.
        midi::MidiScheduler s;
        std::vector<uint8_t> got;
        s.setOutputCallback([&](const uasf::MidiEvent& e) { got.push_back(e.data1); });
        s.scheduleEvent(ev(100, 1));   // future (e.g. a swung off-beat)
        s.scheduleEvent(ev(10, 2));    // due
        s.scheduleEvent(ev(20, 3));    // due
        s.advanceTo(50);
        TEST("due events behind a future one are dispatched now", got == std::vector<uint8_t>({2, 3}));
        TEST("the future event stays pending", s.pendingCount() == 1);
        s.advanceTo(100);
        TEST("the future event is dispatched when due", got.size() == 3 && got[2] == 1);
    }
    {   // Future events keep their relative order across re-queueing.
        midi::MidiScheduler s;
        std::vector<uint8_t> got;
        s.setOutputCallback([&](const uasf::MidiEvent& e) { got.push_back(e.data1); });
        s.scheduleEvent(ev(200, 1));
        s.scheduleEvent(ev(150, 2));
        s.scheduleEvent(ev(10, 3));
        s.advanceTo(50);
        s.advanceTo(150);
        TEST("only the event due at 150 fires at 150", got == std::vector<uint8_t>({3, 2}));
        s.advanceTo(200);
        TEST("the 200 event fires at 200", got == std::vector<uint8_t>({3, 2, 1}));
        TEST("nothing left", s.pendingCount() == 0);
    }
    {   // Real Genos style, heavy swing: nothing dispatched more than one block late.
        std::string sty;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(CORPUS_DIR, ec))
            if (e.path().filename().string().find("POP_ACOUSTIC_2") != std::string::npos &&
                e.path().extension() == ".sty") sty = e.path().string();
        TEST("corpus .sty present", !sty.empty());
        if (!sty.empty()) {
            importers::sff1::Sff1Reader r;
            importers::sff1::Sff1ToUasfMapper m;
            const auto style = m.map(r.parseFile(sty)).style;
            for (uint8_t swing : {50, 66, 75}) {
                realtime::RealtimeClock clock; midi::MidiScheduler sch; midi::PanicHandler pan;
                arranger::StylePlayer p(clock, sch, pan);
                long total = 0, late = 0; int64_t worst = 0;
                sch.setOutputCallback([&](const uasf::MidiEvent& e) {
                    ++total;
                    const int64_t d = clock.getPosition() - static_cast<int64_t>(e.tick);
                    if (e.tick && d > worst) worst = d;
                });
                p.loadStyle(style); p.setSwing(swing); p.start(0);
                const int64_t block = static_cast<int64_t>(std::ceil(48 * clock.getTicksPerSample()));
                for (int i = 0; i < 30000; ++i) { clock.advance(48); p.tick(); sch.advanceTo(clock.getPosition()); }
                (void)late;
                std::printf("  [swing %u] events=%ld worst lateness=%lld ticks (block %lld)\n", swing, total,
                            static_cast<long long>(worst), static_cast<long long>(block));
                TEST("POP_ACOUSTIC_2 swing " + std::to_string(swing) + ": no event more than one block late",
                     total > 1000 && worst <= block);
            }
        }
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
