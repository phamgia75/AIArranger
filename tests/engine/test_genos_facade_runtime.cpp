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
// Tempo is NOT checked: SMF Set Tempo is not imported yet (known P1; playback
// runs at the default 120 BPM).

#include "session/live_engine_facade.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "engine/arranger/style_player.h"
#include "../midi/fake_midi_output_provider.h"
#include <cstdio>
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

static void probe(const char* needle) {
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
            // 48 samples @48 kHz, 120 BPM, <=1920 PPQN = <4 ticks per step.
            if (tpBar && pos % tpBar > 8 && trace.size() > 1) offGrid = true;
        }
    };
    auto runUntil = [&](const std::function<bool()>& pred, int maxBars) {
        // a 4/4 bar at 120 BPM / 48 kHz is 2000 steps; allow generous headroom
        for (int i = 0; i < maxBars * 2000 && !pred(); ++i) step();
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
    TEST(tag + ": Intro A hands over to Main A", runUntil(inSection(mainA), 16));
    // Stay in Main A for (bars + 2): it must loop, not fall silent or move on.
    const long beforeLoop = notesOn();
    for (uint32_t i = 0; i < (m.bars + 2) * 2000u; ++i) step();
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
    for (const char* n : {"POP_ACOUSTIC_2", "CLASSIC_6_8", "BALLAD_FOLK", "C_WHISPER"}) probe(n);

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
