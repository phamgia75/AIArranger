// Auto-fill on variation change (arranger-engine-spec §5.3, testing-strategy
// "Main A->B with autoFill=on -> Fill AA before Main B").
//
// With auto-fill ON, changing Main X -> Main Y plays the Fill of the SOURCE Main
// X (or the nearest fill if X has none), committed at the bar boundary, and the
// fill lands on Main Y. With it OFF, the variation change is a plain bar-
// quantized switch. Requests made while an Intro / Fill / Break plays retarget
// where that section lands, without inserting another fill.

#include "session/live_engine_facade.h"
#include "engine/arranger/style_player.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "../midi/fake_midi_output_provider.h"
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace ai_arranger;
using uasf::SectionType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } \
} while(0)

#ifndef CORPUS_DIR
#define CORPUS_DIR "."
#endif

// Intro(2) Main A-D(2 each) Fill AA-DD(1) Break(1) Ending(1), 4/4 @480.
static uasf::StyleDefinition fullStyle(bool withFills = true) {
    const auto demo = arranger::buildDemoStyle();
    const auto& intro = demo.sections[0];
    const auto& main = demo.sections[1];
    const auto& fill = demo.sections[2];
    const auto& ending = demo.sections[3];
    uasf::StyleDefinition s = demo;
    s.sections.clear();
    auto add = [&](uasf::SectionDefinition d, SectionType t, uint32_t bars, const char* n) {
        d.type = t; d.bars = bars; d.name = n; s.sections.push_back(std::move(d));
    };
    add(intro, SectionType::Intro1, 2, "Intro A");
    add(main, SectionType::Main1, 2, "Main A");
    add(main, SectionType::Main2, 2, "Main B");
    add(main, SectionType::Main3, 2, "Main C");
    add(main, SectionType::Main4, 2, "Main D");
    if (withFills) {
        add(fill, SectionType::Fill1, 1, "Fill In AA");
        add(fill, SectionType::Fill2, 1, "Fill In BB");
        add(fill, SectionType::Fill3, 1, "Fill In CC");
        add(fill, SectionType::Fill4, 1, "Fill In DD");
    }
    add(fill, SectionType::Break, 1, "Fill In BA");
    add(ending, SectionType::Ending1, 1, "Ending A");
    return s;
}

struct Rig {
    midi::FakeMidiOutputProvider out;
    session::LiveEngineFacade f{nullptr, &out};
    uasf::StyleDefinition style;
    std::vector<int> trace;          // section index at each change
    std::vector<int64_t> traceTick;
    int64_t tpBar = 0;

    explicit Rig(const uasf::StyleDefinition& st) : style(st) {
        out.setDevices({{0, "synth"}});
        f.selectMidiOutput(0);
        f.loadStyle(style);
        f.start();
        tpBar = f.session().clock().ticksPerBar();
    }
    int idx(SectionType t) const {
        for (size_t i = 0; i < style.sections.size(); ++i)
            if (style.sections[i].type == t) return static_cast<int>(i);
        return -1;
    }
    void step() {
        f.tick(48);
        const auto s = f.snapshot();
        if (s.playing && (trace.empty() || s.section != trace.back())) {
            trace.push_back(s.section);
            traceTick.push_back(s.positionTicks);
        }
    }
    bool runUntil(const std::function<bool()>& pred, int maxSteps = 200000) {
        for (int i = 0; i < maxSteps && !pred(); ++i) step();
        return pred();
    }
    bool until(SectionType t) { const int i = idx(t); return runUntil([&] { return f.snapshot().section == i; }); }
    // Play from Intro into Main A and settle there.
    void startInMainA() {
        f.transportStart();
        step();
        until(SectionType::Main1);
        trace.clear(); traceTick.clear();
        trace.push_back(f.snapshot().section);
        traceTick.push_back(f.snapshot().positionTicks);
    }
    // Section types visited since the mark, in order.
    std::vector<SectionType> visited() const {
        std::vector<SectionType> v;
        for (int i : trace) v.push_back(style.sections[static_cast<size_t>(i)].type);
        return v;
    }
    bool onGrid() const {
        for (size_t i = 1; i < traceTick.size(); ++i)
            if (traceTick[i] % tpBar > 8) return false;   // < 4 ticks per step here
        return true;
    }
    long stuck() const {
        std::map<int, long> open;
        for (const auto& e : out.sent) {
            const int k = (e.channel << 8) | e.data1;
            if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) ++open[k];
            else if ((e.type == uasf::MidiEventType::NoteOff ||
                      (e.type == uasf::MidiEventType::NoteOn && e.data2 == 0)) && open[k] > 0) --open[k];
            else if (e.type == uasf::MidiEventType::ControlChange && (e.data1 == 120 || e.data1 == 123))
                for (auto& [key, n] : open) if ((key >> 8) == e.channel) n = 0;
        }
        long n = 0; for (const auto& [k, v] : open) n += v;
        return n;
    }
};

using V = std::vector<SectionType>;

int main() {
    std::printf("Test: auto-fill on variation change\n");

    {   // OFF (default): A -> B is a plain switch, no fill.
        Rig r(fullStyle());
        TEST("auto-fill is off by default", !r.f.autoFill());
        r.startInMainA();
        r.f.setVariation(1);
        r.until(SectionType::Main2);
        TEST("OFF: Main A -> Main B directly", r.visited() == V({SectionType::Main1, SectionType::Main2}));
    }
    {   // ON: A -> B plays Fill AA, lands on Main B, on the bar grid.
        Rig r(fullStyle());
        r.f.setAutoFill(true);
        TEST("setAutoFill(true) reads back", r.f.autoFill());
        r.startInMainA();
        r.f.setVariation(1);
        r.until(SectionType::Main2);
        TEST("ON: Main A -> Fill In AA -> Main B",
             r.visited() == V({SectionType::Main1, SectionType::Fill1, SectionType::Main2}));
        TEST("ON: every switch on a bar boundary", r.onGrid());
        // Steady on Main B afterwards (loops, no further fill).
        for (int i = 0; i < 6000; ++i) r.step();
        TEST("ON: stays on Main B after landing", r.trace.back() == r.idx(SectionType::Main2) &&
                                                   r.visited().size() == 3);
        r.f.stop();
        TEST("ON: no stuck notes", r.stuck() == 0);
    }
    {   // ON: C -> D plays Fill CC.
        Rig r(fullStyle());
        r.f.setAutoFill(true);
        r.startInMainA();
        r.f.setVariation(2);
        r.until(SectionType::Main3);
        r.trace.clear(); r.trace.push_back(r.idx(SectionType::Main3));
        r.f.setVariation(3);
        r.until(SectionType::Main4);
        TEST("ON: Main C -> Fill In CC -> Main D",
             r.visited() == V({SectionType::Main3, SectionType::Fill3, SectionType::Main4}));
    }
    {   // ON: repeated requests before the boundary — the latest target wins, one fill.
        Rig r(fullStyle());
        r.f.setAutoFill(true);
        r.startInMainA();
        r.f.setVariation(1);
        r.step();
        r.f.setVariation(2);
        r.until(SectionType::Main3);
        TEST("ON: A->B then A->C before the bar: Fill In AA -> Main C",
             r.visited() == V({SectionType::Main1, SectionType::Fill1, SectionType::Main3}));
    }
    {   // ON: request while the fill already plays retargets its landing.
        Rig r(fullStyle());
        r.f.setAutoFill(true);
        r.startInMainA();
        r.f.setVariation(1);
        r.until(SectionType::Fill1);
        r.f.setVariation(3);
        r.until(SectionType::Main4);
        TEST("ON: request during the fill: Fill In AA lands on Main D, no second fill",
             r.visited() == V({SectionType::Main1, SectionType::Fill1, SectionType::Main4}));
    }
    {   // ON: manual Fill keeps its meaning (fill of current Main, back to it).
        Rig r(fullStyle());
        r.f.setAutoFill(true);
        r.startInMainA();
        r.f.fill();
        r.until(SectionType::Fill1);
        r.until(SectionType::Main1);
        TEST("ON: manual Fill: Main A -> Fill In AA -> Main A",
             r.visited() == V({SectionType::Main1, SectionType::Fill1, SectionType::Main1}));
        // Variation pressed during the manual fill: it lands on the new Main.
        r.trace.clear(); r.trace.push_back(r.idx(SectionType::Main1));
        r.f.fill();
        r.until(SectionType::Fill1);
        r.f.setVariation(1);
        r.until(SectionType::Main2);
        TEST("ON: variation during a manual Fill lands on the new Main",
             r.visited() == V({SectionType::Main1, SectionType::Fill1, SectionType::Main2}));
    }
    {   // ON: request during the Intro — no fill; the Intro lands on the target.
        Rig r(fullStyle());
        r.f.setAutoFill(true);
        r.f.transportStart();
        r.step();
        TEST("intro playing", r.f.snapshot().section == r.idx(SectionType::Intro1));
        r.trace.clear(); r.trace.push_back(r.idx(SectionType::Intro1));
        r.f.setVariation(1);
        r.until(SectionType::Main2);
        TEST("ON: request during Intro: Intro A -> Main B, no fill, Intro not cut",
             r.visited() == V({SectionType::Intro1, SectionType::Main2}) &&
             r.traceTick.size() == 2 && r.traceTick[1] >= 2 * r.tpBar);
    }
    {   // ON: same Main again — no fill.
        Rig r(fullStyle());
        r.f.setAutoFill(true);
        r.startInMainA();
        r.f.setVariation(0);
        for (int i = 0; i < 6000; ++i) r.step();
        TEST("ON: Main A -> Main A request plays no fill",
             r.visited() == V({SectionType::Main1}));
    }
    {   // ON: a style without fills — plain switch.
        Rig r(fullStyle(false));
        r.f.setAutoFill(true);
        r.startInMainA();
        r.f.setVariation(1);
        r.until(SectionType::Main2);
        TEST("ON, style without fills: Main A -> Main B directly",
             r.visited() == V({SectionType::Main1, SectionType::Main2}));
    }
    {   // A landing pending at Stop does not survive into the next Start.
        Rig r(fullStyle());
        r.f.setAutoFill(true);
        r.startInMainA();
        r.f.setVariation(1);
        r.step();
        r.f.transportStop();
        for (int i = 0; i < 10; ++i) r.step();
        r.f.transportStart();
        r.step();
        r.trace.clear(); r.trace.push_back(r.f.snapshot().section);
        r.until(SectionType::Main1);
        for (int i = 0; i < 6000; ++i) r.step();
        TEST("ON: a landing pending at Stop is forgotten (restart: Intro -> Main A)",
             r.visited() == V({SectionType::Intro1, SectionType::Main1}));
    }
    {   // Demo style: one Main, every variation maps to it — never a fill.
        Rig r(arranger::buildDemoStyle());
        r.f.setAutoFill(true);
        r.startInMainA();
        r.f.setVariation(1);
        for (int i = 0; i < 6000; ++i) r.step();
        TEST("demo style (one Main): variation change plays no fill",
             r.visited() == V({SectionType::Main1}));
    }
    {   // Imported Genos style.
        std::string sty;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(CORPUS_DIR, ec)) {
            const auto fn = e.path().filename().string();
            if (fn.find("POP_ACOUSTIC_2") != std::string::npos && fn.size() > 4 &&
                fn.substr(fn.size() - 4) == ".sty") sty = e.path().string();
        }
        TEST("POP_ACOUSTIC_2 corpus present", !sty.empty());
        if (!sty.empty()) {
            importers::sff1::Sff1Reader reader;
            const auto pr = reader.parseFile(sty);
            importers::sff1::Sff1ToUasfMapper mapper;
            const auto mr = mapper.map(pr);
            Rig r(mr.style);
            r.f.setAutoFill(true);
            r.startInMainA();
            r.f.setVariation(1);
            r.until(SectionType::Main2);
            TEST("Genos POP_ACOUSTIC_2: Main A -> Fill In AA -> Main B",
                 r.visited() == V({SectionType::Main1, SectionType::Fill1, SectionType::Main2}));
            TEST("Genos: switches on the bar grid", r.onGrid());
            r.f.stop();
            TEST("Genos: no stuck notes", r.stuck() == 0);
        }
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
