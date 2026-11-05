// Sound map loader + schema, fill-in template, MegaVoice heuristic, unused-map
// warning, and the round-trip comparator used for Pa5X re-exports.

#include "exporters/korg/sff1_route.h"
#include "exporters/korg/sound_map.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;
using uasf::MidiEventType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static SoundMapResult load(const std::string& text) {
    std::istringstream in(text);
    return parseSoundMap(in, "map");
}
static bool errorFor(const std::string& text, const char* needle) {
    const auto r = load(text);
    return !r.ok && r.error.find(needle) != std::string::npos;
}

static uasf::TrackDefinition part(uint8_t ch, const char* name, uasf::TrackRole role,
                                  const std::vector<std::pair<uint8_t, uint8_t>>& notes) {
    uasf::TrackDefinition t;
    t.midi_channel = ch;
    t.name = name;
    t.role = role;
    uint64_t tick = 0;
    for (auto [p, v] : notes) {
        t.events.push_back({MidiEventType::NoteOn, ch, p, v, tick});
        t.events.push_back({MidiEventType::NoteOff, ch, p, 0, tick + 100});
        tick += 200;
    }
    return t;
}

static const TrackRow* row(const Report& r, uint8_t srcCh) {
    for (const auto& t : r.tracks) if (t.src_ch == srcCh) return &t;
    return nullptr;
}

int main() {
    std::printf("Test: Pa5X sound map, MegaVoice report, round-trip compare\n");

    // ── Loader / schema ──
    {
        const auto r = load("# header\n\nacc1 121 0 25   # guitar\nbass unresolved\n  drum 120 0 0\n");
        TEST("valid map loads", r.ok);
        TEST("numeric lines -> sounds", r.sounds.size() == 2 && r.sounds.at(11).msb == 121 && r.sounds.at(11).pc == 25 &&
             r.sounds.at(kDrumCh).msb == 120);
        TEST("'unresolved' recorded, not emitted", r.unresolved.count(kBassCh) == 1 && r.sounds.count(kBassCh) == 0);
    }
    TEST("empty map is valid", load("").ok && load("# nothing\n").ok);
    TEST("unknown track rejected", errorFor("acc9 1 2 3\n", "unknown track"));
    TEST("duplicate track rejected", errorFor("acc1 1 2 3\nacc1 unresolved\n", "twice"));
    TEST("value 128 rejected", errorFor("acc1 128 0 0\n", "expected"));
    TEST("negative value rejected", errorFor("acc1 -1 0 0\n", "expected"));
    TEST("trailing junk in number rejected", errorFor("acc1 12abc 0 0\n", "expected"));
    TEST("missing values rejected", errorFor("acc1 1 2\n", "expected"));
    TEST("extra token rejected", errorFor("acc1 1 2 3 4\n", "expected"));
    TEST("'unresolved' with extra token rejected", errorFor("acc1 unresolved 5\n", "expected"));
    TEST("error names file and line", errorFor("acc1 1 2 3\n\nperc x\n", "map:3:"));
    TEST("track keys round-trip", std::string(soundMapKey(kPercCh)) == "perc" && std::string(soundMapKey(15)) == "acc5" &&
         soundMapKey(3) == nullptr);

    // ── MegaVoice heuristic + template on a synthetic style ──
    uasf::SectionDefinition sec;
    sec.type = uasf::SectionType::Main1;
    sec.name = "Main A";
    sec.bars = 2;
    sec.resolution = 1920;
    sec.beats_per_bar = 4;
    sec.beat_note = 4;
    sec.tracks = {
        part(11, "Gtr Mega", uasf::TrackRole::Chord, {{60, 50}, {100, 80}}),            // C7+ on guitar
        part(12, "Piano", uasf::TrackRole::Chord, {{60, 40}, {62, 70}, {64, 95}, {65, 120}}),  // dynamics only
        part(13, "Pad", uasf::TrackRole::Pad, {{5, 80}, {60, 80}}),                        // C-1..B-1 note
        part(10, "Bass", uasf::TrackRole::Bass, {{36, 40}, {36, 80}, {36, 95}}),           // 3 zones, ordinary dynamics
        part(14, "Chord2", uasf::TrackRole::Chord, {{98, 70}, {99, 95}, {100, 120}}),     // unnamed part, FX keys
        part(9, "Rhythm2", uasf::TrackRole::Drum, {{13, 40}, {36, 127}, {120, 95}}),       // drums never flagged
    };
    uasf::StyleDefinition style;
    style.tempo_bpm = 90;
    style.resolution = 1920;
    style.sections = {sec};

    MapOptions opt;
    opt.sounds[15] = {1, 2, 3};   // Acc5: no Genos part plays it
    const auto m = mapToKorg(style, opt);
    TEST("maps", m.ok);
    TEST("guitar with notes >= C7 flagged", row(m.report, 11) && row(m.report, 11)->megavoice &&
         row(m.report, 11)->megavoice_why.find("C7") != std::string::npos);
    TEST("piano dynamics alone not flagged", row(m.report, 12) && !row(m.report, 12)->megavoice);
    TEST("pad with C-1..B-1 note flagged", row(m.report, 13) && row(m.report, 13)->megavoice &&
         row(m.report, 13)->chord_range_dropped == 1);
    TEST("velocity zones alone do not flag (ordinary dynamics)", row(m.report, 10) && !row(m.report, 10)->megavoice);
    TEST("unidentified part with C7+ keys flagged, zones as support", row(m.report, 14) && row(m.report, 14)->megavoice &&
         row(m.report, 14)->megavoice_why.find("zones") != std::string::npos);
    TEST("families: Guitar, Piano/EP, Pad, Bass, Drum Kit, Unidentified",
         row(m.report, 11)->family == "Guitar" && row(m.report, 12)->family == "Piano/EP" &&
         row(m.report, 13)->family == "Pad" && row(m.report, 10)->family == "Bass" &&
         row(m.report, 9)->family == "Drum Kit" && row(m.report, 14)->family == "Unidentified");
    TEST("drum part never flagged", row(m.report, 9) && !row(m.report, 9)->megavoice);
    TEST("unused sound-map track warned", std::any_of(m.report.warnings.begin(), m.report.warnings.end(),
         [](const std::string& w) { return w.find("Acc5 but no Genos part") != std::string::npos; }));
    const std::string rep = formatReport(m.report, m.style);
    TEST("report has MegaVoice and confirm sections", rep.find("MegaVoice / articulation candidates") != std::string::npos &&
         rep.find("Confirm on Pa5X") != std::string::npos && rep.find("Gtr Mega") != std::string::npos);

    const std::string tpl = soundMapTemplate(m.report, "SYN.sty");
    TEST("template lists all 8 tracks unresolved", [&] {
        for (const char* k : {"bass", "drum", "perc", "acc1", "acc2", "acc3", "acc4", "acc5"})
            if (tpl.find(std::string("\n") + k + " unresolved\n") == std::string::npos) return false;
        return true; }());
    TEST("template comments carry Genos voice + MegaVoice flag",
         tpl.find("Genos ch 12 'Gtr Mega'") != std::string::npos && tpl.find("MEGAVOICE CANDIDATE") != std::string::npos);
    { const auto back = load(tpl);
      TEST("template loads: 0 sounds, 8 unresolved", back.ok && back.sounds.empty() && back.unresolved.size() == 8); }

    // ── Corpus templates ──
    size_t styles = 0;
    for (const auto& f : std::filesystem::directory_iterator(CORPUS_DIR)) {
        if (f.path().extension() != ".sty") continue;
        importers::sff1::Sff1Reader reader;
        const auto p = reader.parseFile(f.path().string());
        importers::sff1::Sff1ToUasfMapper toUasf;
        const auto c = toUasf.map(p);
        MapOptions o;
        o.route = routeFromCasm(p);
        const auto r = mapToKorg(c.style, o);
        const auto t = load(soundMapTemplate(r.report, f.path().filename().string()));
        if (r.ok && t.ok && t.sounds.empty() && t.unresolved.size() == 8) ++styles;
    }
    TEST("corpus: 4 templates load with every track unresolved", styles == 4);

    // ── Round-trip comparator ──
    Style a;
    a.ppqn = 1920;
    a.mpqn = 666667;
    a.elements = {{"v1cv1", 6, 8, 11520, {{0, 0x90 | 11, 60, 100}, {960, 0x80 | 11, 60, 0},
                                          {640, 0x99, 36, 90}, {700, 0x89, 36, 0}}},
                  {"e1cv1", 6, 8, 5760, {{0, 0x90 | 8, 36, 80}, {5759, 0x80 | 8, 36, 0}}}};
    std::stable_sort(a.elements[0].events.begin(), a.elements[0].events.end(),
                     [](const Event& x, const Event& y) { return x.tick < y.tick; });
    TEST("identical styles compare equal", compareStyles(a, a).differences.empty());

    Style b;   // what a Pa5X re-export plausibly looks like: other PPQN, header data, extra element
    b.ppqn = 480;
    b.mpqn = 666667;
    b.elements = {{"e1cv1", 6, 8, 1440, {{0, 0xC8, 5, 0}, {0, 0x90 | 8, 36, 80}, {1440, 0x80 | 8, 36, 0}}},
                  {"v1cv1", 6, 8, 2880, {{0, 0xBB, 0, 121}, {0, 0x90 | 11, 60, 100}, {160, 0x99, 36, 90},
                                          {175, 0x89, 36, 0}, {240, 0x80 | 11, 60, 0}}},
                  {"b1cv1", 6, 8, 1440, {}}};
    auto cr = compareStyles(a, b);
    TEST("rescaled re-export with header data compares equal", cr.differences.empty());
    TEST("extra Pa5X element reported as info (reveals marker code)",
         std::any_of(cr.info.begin(), cr.info.end(), [](const std::string& i) { return i.find("'b1cv1'") != std::string::npos; }));
    { Style c = b;   // Pa5X Chord event exported as note D-1 on Acc1: not a style note
      c.elements[1].events.insert(c.elements[1].events.begin() + 2, {{0, 0x90 | 11, 2, 7}, {100, 0x80 | 11, 2, 0}});
      std::stable_sort(c.elements[1].events.begin(), c.elements[1].events.end(),
                       [](const Event& x, const Event& y) { return x.tick < y.tick; });
      TEST("exported Chord-event notes ignored", compareStyles(a, c).differences.empty()); }
    { Style c = b; c.elements[1].events[1].d2 = 99;
      TEST("velocity change is a difference", !compareStyles(a, c).differences.empty()); }
    { Style c = b; c.elements[1].events[3].tick = 200;
      TEST("note-off moved beyond tolerance is a difference", !compareStyles(a, c).differences.empty()); }
    { Style c = b; c.elements.erase(c.elements.begin());
      TEST("missing element is a difference", compareStyles(a, c).differences.size() == 1); }
    { Style c = b; c.elements[0].length = 2880;
      TEST("length change is a difference", !compareStyles(a, c).differences.empty()); }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
