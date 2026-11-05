// Canonical (UASF) -> Korg Pa5X semantic model: section mapping, channel
// routing, voice-select policy, chord-event-range filter, boundary clamping,
// exact PPQN rescaling (triplets, 6/8), loss accounting.

#include "exporters/korg/genos_to_korg.h"

#include <cstdio>

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;
using uasf::MidiEventType;
using uasf::SectionType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static uasf::MidiEvent ev(MidiEventType t, uint8_t ch, uint8_t d1, uint8_t d2, uint64_t tick) {
    return {t, ch, d1, d2, tick};
}
static uasf::TrackDefinition track(uint8_t ch, std::vector<uasf::MidiEvent> evs, uasf::TrackRole role = uasf::TrackRole::Chord) {
    uasf::TrackDefinition t;
    t.midi_channel = ch;
    t.name = "T" + std::to_string(ch + 1);
    t.role = role;
    t.events = std::move(evs);
    return t;
}
static uasf::SectionDefinition section(SectionType type, std::string name, uint32_t bars,
                                       std::vector<uasf::TrackDefinition> tracks, uint32_t num = 4, uint32_t den = 4) {
    uasf::SectionDefinition s;
    s.type = type; s.name = std::move(name); s.bars = bars;
    s.resolution = 1920; s.beats_per_bar = num; s.beat_note = den;
    s.tracks = std::move(tracks);
    return s;
}
static uasf::StyleDefinition styleOf(std::vector<uasf::SectionDefinition> secs) {
    uasf::StyleDefinition s;
    s.tempo_bpm = 100; s.resolution = 1920; s.sections = std::move(secs);
    return s;
}
static const Element* find(const Style& s, const std::string& m) {
    for (const auto& e : s.elements) if (e.marker == m) return &e;
    return nullptr;
}
static uint32_t count(const Element& e, uint8_t status, int d1 = -1) {
    uint32_t n = 0;
    for (const auto& x : e.events) if (x.status == status && (d1 < 0 || x.d1 == d1)) ++n;
    return n;
}

int main() {
    std::printf("Test: Genos (UASF) -> Korg Pa5X mapper\n");
    using T = SectionType;

    // ── Channel table: Genos parts 9..16 -> Korg tracks ──
    TEST("Rhythm1 (ch9) -> Percussion", korgChannelFor(8) == kPercCh);
    TEST("Rhythm2 (ch10) -> Drum", korgChannelFor(9) == kDrumCh);
    TEST("Bass (ch11) -> Bass", korgChannelFor(10) == kBassCh);
    TEST("Chord1..Phrase2 (ch12-16) -> Acc1..Acc5", korgChannelFor(11) == 11 && korgChannelFor(15) == 15);
    TEST("ch1-8 have no Korg track", korgChannelFor(0) == 0xFF && korgChannelFor(7) == 0xFF);

    // ── Section table ──
    TEST("Main A-D -> v1..v4", std::string(korgMarkerFor(T::Main1)) == "v1cv1" && std::string(korgMarkerFor(T::Main4)) == "v4cv1");
    TEST("Intro A/B -> i1/i2", std::string(korgMarkerFor(T::Intro1)) == "i1cv1" && std::string(korgMarkerFor(T::Intro2)) == "i2cv1");
    TEST("Fill AA/BB -> f1/f2", std::string(korgMarkerFor(T::Fill1)) == "f1cv1" && std::string(korgMarkerFor(T::Fill2)) == "f2cv1");
    TEST("Ending A/B -> e1/e2", std::string(korgMarkerFor(T::Ending1)) == "e1cv1" && std::string(korgMarkerFor(T::Ending2)) == "e2cv1");
    TEST("Intro C / Fill CC,DD / Ending C / Break: no code",
         !korgMarkerFor(T::Intro3) && !korgMarkerFor(T::Fill3) && !korgMarkerFor(T::Fill4) &&
         !korgMarkerFor(T::Ending3) && !korgMarkerFor(T::Break));

    // ── Full mapping on a synthetic style ──
    const auto note = [](uint8_t ch, uint8_t p, uint64_t on, uint64_t off, uint8_t v = 100) {
        return std::vector<uasf::MidiEvent>{ev(MidiEventType::NoteOn, ch, p, v, on), ev(MidiEventType::NoteOff, ch, p, 0, off)};
    };
    auto cat = [](std::vector<uasf::MidiEvent> a, const std::vector<uasf::MidiEvent>& b) { a.insert(a.end(), b.begin(), b.end()); return a; };

    std::vector<uasf::MidiEvent> acc = {ev(MidiEventType::ControlChange, 11, 0, 63, 0),
                                        ev(MidiEventType::ControlChange, 11, 32, 120, 0),
                                        ev(MidiEventType::ProgramChange, 11, 21, 0, 0),
                                        ev(MidiEventType::ControlChange, 11, 11, 100, 0)};
    acc = cat(acc, note(11, 60, 0, 960));
    acc = cat(acc, note(11, 5, 100, 200));          // MegaVoice-style noise in C-1..B-1
    acc = cat(acc, note(11, 64, 7000, 9000));       // crosses the 1-bar (7680) end
    acc = cat(acc, note(11, 67, 7679, 7680));       // starts on the last tick
    auto drums = cat(note(9, 36, 0, 100), note(9, 13, 480, 500));   // Revo-style low note kept
    auto lowCh = note(3, 60, 0, 100);               // ch4 with no route: no Korg track
    auto routed = note(3, 62, 0, 100);              // ch4 routed by CASM to ch15

    auto st = styleOf({
        section(T::Main1, "Main A", 1, {track(11, acc), track(9, drums, uasf::TrackRole::Drum), track(3, lowCh)}),
        section(T::Main2, "Main B", 1, {track(14, note(14, 70, 0, 100)), track(3, routed)}),
        section(T::Fill3, "Fill In CC", 1, {track(9, note(9, 38, 0, 10))}),
        section(T::Break, "Fill In BA", 10, {track(9, note(9, 38, 0, 10))}),
        section(T::Fill1, "Fill In AA", 0, {}),
    });

    MapOptions opt;
    auto r = mapToKorg(st, opt);
    TEST("maps", r.ok);
    TEST("tempo 100 BPM -> 600000 us/q", r.style.mpqn == 600000);
    TEST("source PPQN kept", r.style.ppqn == 1920);
    TEST("mapped output validates", validate(r.style).empty());
    const Element* v1 = find(r.style, "v1cv1");
    TEST("Main A exported as v1cv1, 1 bar 4/4", v1 && v1->length == 7680 && v1->num == 4 && v1->den == 4);
    TEST("Fill CC, Break, zero-length Fill AA not exported", !find(r.style, "f1cv1") && r.style.elements.size() == 2);
    TEST("Break row explains why", r.report.sections.size() == 5 &&
         r.report.sections[3].marker.empty() && r.report.sections[3].note.find("Break") != std::string::npos);
    TEST("zero-length section reported", r.report.sections[4].note == "zero-length section");
    TEST("dropped sections counted", r.report.dropped[kDropSection] == 4);

    TEST("Yamaha bank/PC dropped (3 events)", r.report.dropped[kDropVoiceSelect] == 3 &&
         count(*v1, 0xC0 | 11) == 0 && count(*v1, 0xB0 | 11, 0) == 0);
    TEST("Yamaha voice recorded in the report", r.report.tracks[0].src_ch == 3 &&
         [&] { for (auto& t : r.report.tracks) if (t.src_ch == 11) return t.msb == 63 && t.lsb == 120 && t.pc == 21 && t.target == "UNRESOLVED"; return false; }());
    TEST("CC11 expression kept at tick 0", count(*v1, 0xB0 | 11, 11) == 1);
    TEST("note 5 on Acc1 dropped (chord-event range)", r.report.dropped[kDropChordRange] == 1 && count(*v1, 0x90 | 11, 5) == 0);
    TEST("note 13 on Drum kept, reported non-GM", count(*v1, 0x90 | kDrumCh, 13) == 1 && r.report.non_gm_drum_notes[13] == 1);
    TEST("note crossing section end clamped to last tick", [&] {
        for (auto& e : v1->events) if (e.status == (0x80 | 11) && e.d1 == 64) return e.tick == 7679; return false; }());
    TEST("note starting on the last tick dropped", r.report.dropped[kDropLastTick] == 1 && count(*v1, 0x90 | 11, 67) == 0);
    TEST("ch4 without route dropped", r.report.dropped[kDropChannel] == 4 && count(*v1, 0x90 | 3) == 0);

    // CASM routing: ch4 -> ch15 lands on Korg Acc4 and merges with Phrase1.
    opt.route = {{3, 14}};
    r = mapToKorg(st, opt);
    const Element* v2 = find(r.style, "v2cv1");
    TEST("routed ch4 plays on Acc4", r.ok && v2 && count(*v2, 0x90 | 14, 62) == 1 && count(*v2, 0x90 | 14, 70) == 1);
    TEST("merge warning emitted", !r.report.warnings.empty() &&
         r.report.warnings[0].find("both play Korg Acc4") != std::string::npos);

    // User sound map: Bank/PC emitted at tick 0 of every element.
    opt.route.clear();
    opt.sounds[kAcc1Ch] = {121, 3, 40};
    r = mapToKorg(st, opt);
    TEST("user sound map emits bank+PC in every element", r.ok &&
         count(r.style.elements[0], 0xC0 | 11, 40) == 1 && count(r.style.elements[1], 0xC0 | 11, 40) == 1 &&
         r.style.elements[0].events[0].tick == 0 && validate(r.style).empty());

    // ── PPQN 1920 -> 480: triplets and 6/8, no drift ──
    std::vector<uasf::MidiEvent> trip;
    for (int i = 0; i < 18; ++i) {   // eighth-note triplets over 6/8 bars: 640 ticks each at 1920
        auto n = note(11, 60, i * 640, i * 640 + 320);
        trip.insert(trip.end(), n.begin(), n.end());
    }
    auto s68 = styleOf({section(T::Main1, "Main A", 2, {track(11, trip)}, 6, 8)});
    MapOptions o480;
    o480.ppqn = 480;
    r = mapToKorg(s68, o480);
    TEST("6/8 at 480 PPQN: 2 bars = 2880 ticks", r.ok && r.style.ppqn == 480 && r.style.elements[0].length == 2880 &&
         r.style.elements[0].num == 6 && r.style.elements[0].den == 8);
    TEST("triplet onsets land exactly on 160-tick grid", [&] {
        int k = 0;
        for (auto& e : r.style.elements[0].events)
            if ((e.status & 0xF0) == 0x90) { if (e.tick != static_cast<uint32_t>(k) * 160) return false; ++k; }
        return k == 18; }());
    { MapOptions o; o.ppqn = 7;   // 7*4*6/8 = 21 ticks per bar: representable
      TEST("odd PPQN with whole-tick 6/8 bar maps", mapToKorg(s68, o).ok); }
    { MapOptions o; o.ppqn = 5;   // 5*4*6/8 = 15: fine; 5*4*7/8 not
      auto s78 = styleOf({section(T::Main1, "Main A", 1, {}, 7, 8)});
      TEST("meter without a whole-tick bar is rejected", !mapToKorg(s78, o).ok); }

    // ── Report text ──
    r = mapToKorg(st, MapOptions{});
    const std::string txt = formatReport(r.report, r.style);
    TEST("report lists section map, tracks, loss", txt.find("Main A (1 bars) -> v1cv1") != std::string::npos &&
         txt.find("NOT EXPORTED") != std::string::npos && txt.find(kDropChordRange) != std::string::npos &&
         txt.find("UNRESOLVED") != std::string::npos);

    // ── No section maps -> error ──
    TEST("style with only Break fails", !mapToKorg(styleOf({section(T::Break, "Fill In BA", 1, {})}), {}).ok);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
