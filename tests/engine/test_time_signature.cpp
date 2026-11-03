// Time signature — x/8 styles on the right bar grid.
//
// RealtimeClock computed ticksPerBar = resolution x beatsPerBar, ignoring the
// beat note, and nothing ever set the clock's time signature from a style. A
// 6/8 Genos style (CLASSIC_6_8: 5760-tick bars at 1920 PPQN) therefore ran on a
// 7680-tick 4/4 grid: section switches landed mid-bar and a 1-bar 6/8 fill was
// padded with a quarter note of silence. 4/4 behaviour must stay bit-identical.

#include "engine/realtime/clock.h"
#include "engine/arranger/style_player.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include <cstdio>
#include <string>

using namespace ai_arranger;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: time signature bar grid\n");

    {
        realtime::RealtimeClock c;
        c.setResolution(1920);
        TEST("4/4 @1920: 7680-tick bar, 1920-tick beat (unchanged)",
             c.ticksPerBar() == 7680 && c.ticksPerBeat() == 1920);
        c.setTimeSignature(6, 8);
        TEST("6/8 @1920: 5760-tick bar, 960-tick (eighth) beat",
             c.ticksPerBar() == 5760 && c.ticksPerBeat() == 960);
        c.setTimeSignature(3, 4);
        TEST("3/4 @1920: 5760-tick bar", c.ticksPerBar() == 5760);
        c.setTimeSignature(7, 8);
        TEST("7/8 @480: 1680-tick bar", (c.setResolution(480), c.ticksPerBar() == 1680));
    }

    {
        importers::sff1::Sff1Reader reader;
        const auto pr = reader.parseFile(std::string(CORPUS_DIR) +
            "/CLASSIC_6_8_SC_GENOS.S718---36428220-aac8-423d-9a87-79f1594df699.sty");
        TEST("CLASSIC_6_8: SMF time signature read as 6/8",
             pr.success && pr.time_sig_num == 6 && pr.time_sig_den == 8);
        importers::sff1::Sff1ToUasfMapper mapper;
        const auto mr = mapper.map(pr);
        bool fillOneBar = false, mainEight = false, ts = !mr.style.sections.empty();
        for (const auto& s : mr.style.sections) {
            if (s.beats_per_bar != 6 || s.beat_note != 8) ts = false;
            if (s.type == uasf::SectionType::Fill1) fillOneBar = (s.bars == 1);
            if (s.type == uasf::SectionType::Main1) mainEight = (s.bars == 8);
        }
        TEST("CLASSIC_6_8: sections carry 6/8", ts);
        TEST("CLASSIC_6_8: Fill In AA is 1 bar of 6/8, Main A 8 bars", fillOneBar && mainEight);

        realtime::RealtimeClock clk; midi::MidiScheduler sch; midi::PanicHandler pan;
        arranger::StylePlayer p(clk, sch, pan);
        p.loadStyle(mr.style);
        TEST("load while stopped adopts 6/8 (5760-tick bars)", clk.ticksPerBar() == 5760);

        // Fill In AA from Main A must return after exactly one 6/8 bar.
        int mainA = -1, fillAA = -1;
        for (size_t i = 0; i < mr.style.sections.size(); ++i) {
            if (mr.style.sections[i].type == uasf::SectionType::Main1) mainA = static_cast<int>(i);
            if (mr.style.sections[i].type == uasf::SectionType::Fill1) fillAA = static_cast<int>(i);
        }
        p.start(mainA);
        auto runTo = [&](int64_t tick) { while (clk.getPosition() < tick) { clk.advance(48); p.tick(); sch.advanceTo(clk.getPosition()); } };
        runTo(5760 + 10);                 // inside bar 1
        p.fill();                         // commits at bar 2 (tick 11520)
        runTo(2 * 5760 + 10);
        TEST("fill starts on the 6/8 bar line (bar 2)", p.getCurrentSection() == fillAA);
        runTo(3 * 5760 + 10);
        TEST("fill returns to Main A after exactly one 6/8 bar", p.getCurrentSection() == mainA);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
