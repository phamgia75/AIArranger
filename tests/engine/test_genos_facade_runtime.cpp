// Real-Genos product-path runtime probe — all 4 committed corpus styles driven
// through session::LiveEngineFacade (the surface the macOS host and C ABI use):
//
//   Start -> Intro A -> (auto) Main A -> Main A loops -> Variation B -> Main B
//   -> Fill -> Fill In BB -> back to Main B -> Break (if present) -> Main B
//   -> Ending -> transport stops
//
// Checks per style: imported PPQN/meter reach the clock (bar grid), every
// section switch lands on a bar boundary, the accompaniment sounds in Main, and
// NoteOn/NoteOff are balanced per (channel, note) at the end (no stuck notes).
// Prints a measurement line per style for the validation report.
//
// Tempo (Issue #2): the imported tempo must equal the file's Set Tempo (expected
// values decoded independently from the corpus), the clock must run at it, and
// the measured tick rate over 1 s of audio must be PPQN x BPM / 60 (a quarter is
// the SMF tempo unit for every meter, so 6/8 uses the same formula).

#include "session/live_engine_facade.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "engine/arranger/style_player.h"
#include "../midi/fake_midi_output_provider.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace ai_arranger;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } \
} while(0)

#ifndef CORPUS_DIR
#define CORPUS_DIR "."
#endif

static std::string findCorpusSty(const std::string& needle) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(CORPUS_DIR, ec)) {
        const std::string fn = e.path().filename().string();
        if (fn.find(needle) != std::string::npos && fn.size() > 4 &&
            fn.substr(fn.size() - 4) == ".sty")
            return e.path().string();
    }
    return {};
}

static void probe(const char* needle, uint32_t expectedBpm) {
    const std::string tag = needle;
    const std::string sty = findCorpusSty(needle);
    TEST(tag + ": corpus .sty present", !sty.empty());
    if (sty.empty()) return;

    importers::sff1::Sff1Reader reader;
    const auto pr = reader.parseFile(sty);
    importers::sff1::Sff1ToUasfMapper mapper;
    const auto mr = mapper.map(pr);
    TEST(tag + ": import succeeds", pr.success && mr.success && !mr.style.sections.empty());
    if (!mr.success || mr.style.sections.empty()) return;
    const auto& secs = mr.style.sections;
    auto indexOf = [&](uasf::SectionType t) {
        for (size_t i = 0; i < secs.size(); ++i) if (secs[i].type == t) return static_cast<int>(i);
        return -1;
    };
    const int introA = indexOf(uasf::SectionType::Intro1);
    const int mainA  = indexOf(uasf::SectionType::Main1);
    const int mainB  = indexOf(uasf::SectionType::Main2);
    const int fillBB = indexOf(uasf::SectionType::Fill2);
    const int brk    = indexOf(uasf::SectionType::Break);
    const int endA   = indexOf(uasf::SectionType::Ending1);
    TEST(tag + ": Intro A, Main A/B, Fill In BB, Ending A present",
         introA >= 0 && mainA >= 0 && mainB >= 0 && fillBB >= 0 && endA >= 0);
    if (introA < 0 || mainA < 0 || mainB < 0 || fillBB < 0 || endA < 0) return;

    TEST(tag + ": imported tempo " + std::to_string(expectedBpm) + " BPM",
         mr.style.tempo_bpm == expectedBpm);
    const auto& m = secs[mainA];
    const int64_t tpBar = static_cast<int64_t>(m.resolution) * m.beats_per_bar * 4 / m.beat_note;

    midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
    session::LiveEngineFacade f(nullptr, &out);
    f.selectMidiOutput(0);
    f.loadStyle(mr.style);
    f.start();
    f.transportStart();

    std::vector<std::pair<int, int64_t>> trace;   // (section, bar) at each change
    bool offGrid = false;
    int last = -1;
    auto step = [&] {
        f.tick(48);
        const auto s = f.snapshot();
        if (s.playing && s.section != last) {
            last = s.section;
            const int64_t pos = s.positionTicks;
            trace.emplace_back(last, tpBar ? pos / tpBar : -1);
            // 48 samples @48 kHz, <=1920 PPQN, <=120 BPM here = <4 ticks per step.
            if (tpBar && pos % tpBar > 8 && trace.size() > 1) offGrid = true;
        }
    };
    // Steps (48 samples @ 48 kHz) per bar at the clock's tempo: a bar is
    // tpBar / (res x bpm / 60) seconds = tpBar x 60 x 1000 / (res x bpm) steps.
    const int64_t stepsPerBar =
        tpBar * 60 * 1000 / (static_cast<int64_t>(m.resolution) * f.session().clock().getTempo()) + 1;
    auto runUntil = [&](const std::function<bool()>& pred, int maxBars) {
        for (int64_t i = 0; i < maxBars * stepsPerBar && !pred(); ++i) step();
        return pred();
    };
    auto inSection = [&](int idx) { return [&, idx] { return f.snapshot().section == idx; }; };
    auto notesOn = [&] {
        long n = 0;
        for (const auto& e : out.sent) if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) ++n;
        return n;
    };

    TEST(tag + ": clock adopted the style's PPQN + meter (bar grid)",
         f.session().clock().ticksPerBar() == tpBar);
    step();                                             // commands apply inside tick()
    TEST(tag + ": starts in Intro A", f.snapshot().section == introA && f.snapshot().playing);
    TEST(tag + ": clock runs at the style tempo", f.session().clock().getTempo() == expectedBpm &&
                                                  f.snapshot().tempoBpm == expectedBpm);
    // Measured progression: 1000 steps x 48 samples = 1 s at 48 kHz.
    const int64_t p0 = f.session().clock().getPosition();
    for (int i = 0; i < 1000; ++i) step();
    const int64_t measured = f.session().clock().getPosition() - p0;
    const int64_t wantTps = static_cast<int64_t>(m.resolution) * expectedBpm / 60;
    std::printf("  [tempo] %s expected=%lld ticks/s measured=%lld\n", needle,
                static_cast<long long>(wantTps), static_cast<long long>(measured));
    TEST(tag + ": measured tick rate == PPQN x BPM / 60 (+/-0.1%)",
         std::llabs(measured - wantTps) * 1000 <= wantTps);
    TEST(tag + ": Intro A hands over to Main A", runUntil(inSection(mainA), 16));
    // Stay in Main A for (bars + 2): it must loop, not fall silent or move on.
    const long beforeLoop = notesOn();
    for (int64_t i = 0; i < (m.bars + 2) * stepsPerBar; ++i) step();
    TEST(tag + ": Main A loops (still Main A after its length + 2 bars)",
         f.snapshot().section == mainA && f.snapshot().playing);
    TEST(tag + ": Main A sounds while looping", notesOn() > beforeLoop);

    f.setVariation(1);                                  // Variation B
    TEST(tag + ": Variation B -> Main B", runUntil(inSection(mainB), 4));
    step();
    f.fill();
    TEST(tag + ": Fill during Main B -> Fill In BB", runUntil(inSection(fillBB), 4));
    TEST(tag + ": Fill In BB returns to Main B", runUntil(inSection(mainB), 8));
    if (brk >= 0) {
        step();
        f.breakSection();
        TEST(tag + ": Break plays", runUntil(inSection(brk), 4));
        // Break length comes from the import: C_WHISPER's "Fill In BA" carries ~9
        // unmarked tail bars (known importer limitation), so wait its own length.
        TEST(tag + ": Break returns to Main B after its length",
             runUntil(inSection(mainB), static_cast<int>(secs[brk].bars) + 2));
    }
    step();
    f.ending();
    TEST(tag + ": Ending A plays", runUntil(inSection(endA), 4));
    TEST(tag + ": Ending stops the transport",
         runUntil([&] { return !f.snapshot().playing; }, 32));
    for (int i = 0; i < 4000; ++i) f.tick(48);          // drain anything scheduled
    TEST(tag + ": every section switch on a bar boundary", !offGrid);
    TEST(tag + ": tempo unchanged through the whole run", f.session().clock().getTempo() == expectedBpm);

    std::map<int, long> open;                            // (ch<<8|note) -> outstanding
    long on = 0, off = 0;
    for (const auto& e : out.sent) {
        const int key = (e.channel << 8) | e.data1;
        if (e.type == uasf::MidiEventType::NoteOn && e.data2 > 0) { ++on; ++open[key]; }
        else if (e.type == uasf::MidiEventType::NoteOff ||
                 (e.type == uasf::MidiEventType::NoteOn && e.data2 == 0)) {
            ++off; if (open[key] > 0) --open[key];
        }
    }
    long outstanding = 0;
    for (const auto& [k, n] : open) outstanding += n;
    TEST(tag + ": no outstanding notes at the end (NoteOn/NoteOff balanced)", outstanding == 0);

    std::printf("  [genos] %s ppqn=%u meter=%u/%u tpBar=%lld sections=%zu break=%s "
                "noteOn=%ld noteOff=%ld outstanding=%ld playing=%d trace=",
                needle, m.resolution, m.beats_per_bar, m.beat_note,
                static_cast<long long>(tpBar), secs.size(), brk >= 0 ? "yes" : "no",
                on, off, outstanding, f.snapshot().playing ? 1 : 0);
    for (const auto& [s, bar] : trace)
        std::printf("%s@%lld ", secs[s].name.c_str(), static_cast<long long>(bar));
    std::printf("\n");
    f.stop();
}

int main() {
    std::printf("Test: real Genos styles through LiveEngineFacade\n");
    // Expected BPM from each file's FF 51 (600000 / 666667 / 571429 / 789473 us/q).
    probe("POP_ACOUSTIC_2", 100);
    probe("CLASSIC_6_8", 90);
    probe("BALLAD_FOLK", 105);
    probe("C_WHISPER", 76);

    // A style swapped in while playing keeps the running grid (no mid-song
    // jump), but the next Start must adopt the new style's PPQN + meter.
    {
        importers::sff1::Sff1Reader reader;
        importers::sff1::Sff1ToUasfMapper mapper;
        const auto mr = mapper.map(reader.parseFile(findCorpusSty("CLASSIC_6_8")));
        midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
        session::LiveEngineFacade f(nullptr, &out);
        f.selectMidiOutput(0);
        f.loadStyle(mr.style);                           // 1920 PPQN, 6/8
        f.start();
        f.transportStart();
        for (int i = 0; i < 4000; ++i) f.tick(48);
        f.loadStyle(arranger::buildDemoStyle());         // 480 PPQN, 4/4, while running
        f.transportStop();
        for (int i = 0; i < 10; ++i) f.tick(48);
        f.transportStart();
        for (int i = 0; i < 10; ++i) f.tick(48);
        TEST("restart after a running style swap adopts the new PPQN + meter",
             f.snapshot().playing && f.session().clock().ticksPerBeat() == 480 &&
             f.session().clock().ticksPerBar() == 1920);
        f.stop();
    }
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
