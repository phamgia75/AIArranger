// Section lifecycle — arranger-engine-spec-v0.9 §5 (state machine):
//   Main loops ("currentTick >= sectionLengthTick -> wrap to 0");
//   Intro does not loop and hands over to Main;
//   Fill / Break play their length, then return to Main;
//   Ending plays its length, then STOPPED.
//
// Before this test the sequencer only switched on explicit commands: after
// Start, the demo style played its 4-bar Intro and then fell silent forever
// (clock running, section stuck at Intro, zero notes) — through the product
// path (LiveEngineFacade) too. Also pinned: loading a style while stopped adopts
// its resolution (a 1920-PPQN Genos import ran on the engine's 480-PPQN clock,
// i.e. 4x too slow).

#include "engine/arranger/style_player.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include "session/live_engine_facade.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include <string>
#include "../midi/fake_midi_output_provider.h"
#include <cstdio>
#include <map>
#include <set>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::arranger;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

namespace {
struct Rig {
    realtime::RealtimeClock clock;
    midi::MidiScheduler     sch;
    midi::PanicHandler      pan;
    StylePlayer             player{clock, sch, pan};
    std::vector<uasf::MidiEvent> out;
    std::map<int64_t, int> sectionAtBar;   // bar -> section index seen at its start
    int64_t tpBar = 0;

    explicit Rig(const uasf::StyleDefinition& st) {
        sch.setOutputCallback([this](const uasf::MidiEvent& e) { out.push_back(e); });
        player.loadStyle(st);
        tpBar = clock.ticksPerBar();
    }
    // Advance `bars` bars in 1 ms-ish steps (48 samples @48 kHz). Returns on the
    // first tick INSIDE the target bar, so a command issued afterwards commits at
    // the following bar boundary.
    void run(int bars) {
        const int64_t endTick = clock.getPosition() + bars * tpBar;
        while (clock.isRunning() && clock.getPosition() < endTick) {
            clock.advance(48);
            player.tick();
            sch.advanceTo(clock.getPosition());
            const int64_t bar = clock.getPosition() / tpBar;
            if (!sectionAtBar.count(bar)) sectionAtBar[bar] = player.getCurrentSection();
        }
    }
    int notesInBar(int64_t bar) const {
        int n = 0;
        for (const auto& e : out)
            if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0 &&
                static_cast<int64_t>(e.tick) / tpBar == bar) ++n;
        return n;
    }
    int balance() const {
        std::map<int, int> on;
        for (const auto& e : out) {
            const int key = e.channel * 128 + e.data1;
            if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) ++on[key];
            else if (e.type == uasf::MidiEventType::NoteOff ||
                     (e.type == uasf::MidiEventType::NoteOn && e.data2 == 0)) --on[key];
        }
        int open = 0;
        for (const auto& [k, v] : on) open += v > 0 ? v : 0;
        return open;
    }
};
} // namespace

int main() {
    std::printf("Test: section lifecycle (loop / auto-advance / fill return / ending stop)\n");
    const auto demo = buildDemoStyle();   // 0 Intro(4) 1 Main(4) 2 Fill(1) 3 Ending(2)

    // ── 1. Intro -> Main, Main loops: accompaniment never falls silent ──
    {
        Rig r(demo);
        r.player.start(0);
        r.run(16);
        bool allBarsSound = true;
        for (int b = 0; b < 16; ++b) if (r.notesInBar(b) == 0) allBarsSound = false;
        TEST("intro->main: every one of 16 bars produces notes", allBarsSound);
        TEST("intro plays bars 0-3", r.sectionAtBar[0] == 0 && r.sectionAtBar[3] == 0);
        TEST("main takes over at bar 4 without a command", r.sectionAtBar[4] == 1);
        TEST("main is still playing at bar 15 (looped twice)", r.sectionAtBar[15] == 1);
        TEST("still running", r.player.isPlaying());

        // Loop is sample-exact: pass 2 (bars 8-11) == pass 1 (bars 4-7) shifted by 4 bars.
        std::multiset<std::tuple<int64_t, int, int>> p1, p2;
        for (const auto& e : r.out) {
            if (e.type != uasf::MidiEventType::NoteOn || e.data2 == 0) continue;
            const int64_t bar = static_cast<int64_t>(e.tick) / r.tpBar;
            if (bar >= 4 && bar < 8)  p1.insert({static_cast<int64_t>(e.tick) - 4 * r.tpBar, e.channel, e.data1});
            if (bar >= 8 && bar < 12) p2.insert({static_cast<int64_t>(e.tick) - 8 * r.tpBar, e.channel, e.data1});
        }
        TEST("main loop repeats identically (no drift, nothing dropped)", !p1.empty() && p1 == p2);
    }

    // ── 2. Fill plays its length (1 bar) then returns to Main ──────────
    {
        Rig r(demo);
        r.player.start(0);
        r.run(5);                 // now inside bar 5 (Main)
        r.player.fill();          // commits at the next bar boundary (bar 6)
        r.run(4);                 // through bar 9
        TEST("fill active at bar 6", r.sectionAtBar[6] == 2);
        TEST("fill returns to main at bar 7", r.sectionAtBar[7] == 1);
        TEST("main keeps sounding after the fill", r.notesInBar(8) > 0 && r.notesInBar(9) > 0);
    }

    // ── 3. Ending plays its length (2 bars) then stops, no stuck notes ─
    {
        Rig r(demo);
        r.player.start(0);
        r.run(5);
        r.player.ending();        // commits at bar 6, 2 bars long (bars 6-7)
        r.run(6);
        TEST("ending active at bar 6", r.sectionAtBar[6] == 3 && r.sectionAtBar[7] == 3);
        TEST("ending stops the transport", !r.player.isPlaying());
        TEST("no notes after the ending", r.notesInBar(8) == 0 && r.notesInBar(9) == 0);
        TEST("every NoteOn has its NoteOff (no stuck notes)", r.balance() == 0);
    }

    // ── 4. A pending user switch still wins over the automatic hand-over ─
    {
        Rig r(demo);
        r.player.start(0);
        r.run(2);
        r.player.switchSection(3);   // user asks for Ending during the Intro (bar 2)
        r.run(1);
        TEST("queued user switch commits at the next bar boundary (ending at bar 3)",
             r.sectionAtBar[3] == 3);
    }

    // ── 5. Loading a style while stopped adopts its resolution ─────────
    {
        auto hi = demo;
        hi.resolution = 1920;
        for (auto& s : hi.sections) {
            s.resolution = 1920;
            for (auto& t : s.tracks) for (auto& e : t.events) e.tick *= 4;
        }
        Rig r(hi);
        TEST("load while stopped adopts 1920 PPQN", r.clock.ticksPerBeat() == 1920);
        r.player.start(0);
        r.run(8);
        TEST("1920-PPQN copy: main reached at bar 4 and sounding",
             r.sectionAtBar[4] == 1 && r.notesInBar(4) > 0 && r.notesInBar(7) > 0);
    }

    // ── 6. Imported Genos style through the facade: marker sections ────
    // POP_ACOUSTIC_2's MTrk is split at its SMF markers into Intro A, Main A-D,
    // Fill In AA-DD, Fill In BA (= Break) and Ending A. Variation A-D address
    // Main A-D by type; Fill picks the fill of the playing Main.
    {
        importers::sff1::Sff1Reader reader;
        const auto pr = reader.parseFile(std::string(CORPUS_DIR) +
            "/POP_ACOUSTIC_2_SC_GENOS.S718---7ba40ed3-527f-49ce-a22c-2414d5de2ec5.sty");
        importers::sff1::Sff1ToUasfMapper mapper;
        const auto mr = mapper.map(pr);
        const auto& secs = mr.style.sections;
        auto indexOf = [&](uasf::SectionType t) {
            for (size_t i = 0; i < secs.size(); ++i) if (secs[i].type == t) return static_cast<int>(i);
            return -1;
        };
        const int introA = indexOf(uasf::SectionType::Intro1);
        const int mainA  = indexOf(uasf::SectionType::Main1);
        const int mainC  = indexOf(uasf::SectionType::Main3);
        const int fillCC = indexOf(uasf::SectionType::Fill3);
        TEST("import: split into 11 marker sections", mr.success && secs.size() == 11);
        TEST("import: Intro A first, Main A-D / Fill AA-DD / Break / Ending A present",
             introA == 0 && mainA >= 0 && mainC >= 0 && fillCC >= 0 &&
             indexOf(uasf::SectionType::Main4) >= 0 && indexOf(uasf::SectionType::Break) >= 0 &&
             indexOf(uasf::SectionType::Ending1) >= 0);
        TEST("import: Main A is 4 bars, Intro A 1 bar",
             mainA >= 0 && secs[mainA].bars == 4 && secs[0].bars == 1);

        midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
        session::LiveEngineFacade f(nullptr, &out);
        f.selectMidiOutput(0);
        f.loadStyle(mr.style);
        f.start();
        f.transportStart();
        // 120 BPM, 48 kHz: one 4/4 bar = 96000 samples = 2000 ticks of 48.
        auto bars = [&](int n) { for (int i = 0; i < n * 2000; ++i) f.tick(48); };
        auto notes = [&] {
            int n = 0;
            for (const auto& e : out.sent) if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) ++n;
            return n;
        };
        bars(1);
        f.tick(48);
        TEST("import: Intro A (1 bar) hands over to Main A", f.snapshot().section == mainA);
        bars(5);
        TEST("import: Main A still playing after 5 more bars (looped)", f.snapshot().section == mainA);
        const int before = notes();
        f.setVariation(2);                       // Variation C
        bars(2);
        TEST("import: Variation C -> Main C", f.snapshot().section == mainC);
        f.fill();
        bars(1);
        TEST("import: Fill during Main C plays Fill In CC", f.snapshot().section == fillCC);
        bars(1);
        TEST("import: Fill In CC returns to Main C", f.snapshot().section == mainC);
        TEST("import: accompaniment kept sounding throughout", notes() > before);
        f.stop();
    }

    // ── Demo style: Variation A-D address Mains, not raw indexes 0-3 ────
    {
        session::LiveEngineFacade f(nullptr, nullptr);
        f.start();
        f.transportStart();
        for (int i = 0; i < 2000; ++i) f.tick(48);    // bar 0 (Intro)
        f.setVariation(0);                            // Variation A
        for (int i = 0; i < 2000; ++i) f.tick(48);
        TEST("demo: Variation A selects the Main (index 1), not the Intro (index 0)",
             f.snapshot().section == 1);
        f.setVariation(3);                            // Variation D (no Main D) -> first Main
        for (int i = 0; i < 2000; ++i) f.tick(48);
        TEST("demo: Variation D without a Main D stays on the Main (not the Ending)",
             f.snapshot().section == 1 && f.snapshot().playing);
        f.stop();
    }

    // ── 7. Product path: the facade keeps playing past the intro ───────
    {
        midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
        session::LiveEngineFacade f(nullptr, &out);
        f.selectMidiOutput(0);
        f.start();
        f.transportStart();
        std::map<int, int> perBar; size_t seen = 0;
        for (int i = 0; i < 12 * 2000; ++i) {       // 12 bars @120 BPM, 1 ms ticks
            f.tick(48);
            for (; seen < out.sent.size(); ++seen)
                if (out.sent[seen].type == uasf::MidiEventType::NoteOn && out.sent[seen].data2 > 0)
                    perBar[i / 2000]++;
        }
        bool all = true;
        for (int b = 0; b < 12; ++b) if (perBar[b] == 0) all = false;
        TEST("facade: demo style sounds in all 12 bars", all);
        TEST("facade: snapshot reports Main after the intro", f.snapshot().section == 1);
        f.stop();
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
