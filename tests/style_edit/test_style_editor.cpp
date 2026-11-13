// Style Studio editing core: every op, undo/redo, validator rejection, Chord
// Variation copy, plain SMF track import, and the UASF -> Pa5X SMF (markers) ->
// independent reader round trip. All styles are synthetic and built here.

#include "style_edit/style_editor.h"
#include "engine/uasf/deserializer.h"
#include "engine/uasf/serializer.h"
#include "exporters/korg/genos_to_korg.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <tuple>

// Test-local equality (the model types define none).
namespace ai_arranger::uasf {
static bool operator==(const MidiEvent& a, const MidiEvent& b) {
    return std::tie(a.type, a.channel, a.data1, a.data2, a.tick) == std::tie(b.type, b.channel, b.data1, b.data2, b.tick);
}
}
namespace ai_arranger::exporters::korg {
static bool operator==(const Event& a, const Event& b) {
    return std::tie(a.tick, a.status, a.d1, a.d2) == std::tie(b.tick, b.status, b.d1, b.d2);
}
}

static constexpr size_t GOLDEN_SIZE = 315;
static constexpr uint64_t GOLDEN_HASH = 0x06664c609b46c5b0ull;

using namespace ai_arranger;
using namespace ai_arranger::style_edit;
using uasf::MidiEvent;
using uasf::MidiEventType;
namespace korg = ai_arranger::exporters::korg;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s (line %d)\n", name, __LINE__); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static void note(uasf::TrackDefinition& t, uint64_t on, uint64_t len, uint8_t pitch, uint8_t vel) {
    t.events.push_back({MidiEventType::NoteOn, t.midi_channel, pitch, vel, on});
    t.events.push_back({MidiEventType::NoteOff, t.midi_channel, pitch, 0, on + len});
}

static void sortTrack(uasf::TrackDefinition& t) {
    std::stable_sort(t.events.begin(), t.events.end(), [](const MidiEvent& a, const MidiEvent& b) {
        auto r = [](const MidiEvent& e) { return e.type == MidiEventType::NoteOff ? 0 : 2; };
        return std::make_pair(a.tick, r(a)) < std::make_pair(b.tick, r(b));
    });
}

// Main A + Main B, 2 bars 4/4 at 480 PPQN; Genos part channels: drum 10, bass 11, chord 12.
static uasf::StyleDefinition makeStyle() {
    uasf::StyleDefinition s;
    s.name = "SYNTH EDIT";
    s.format_version = "1.0";
    s.tempo_bpm = 120;
    s.resolution = 480;
    for (auto type : {uasf::SectionType::Main1, uasf::SectionType::Main2}) {
        uasf::SectionDefinition sec;
        sec.type = type;
        sec.name = type == uasf::SectionType::Main1 ? "Main A" : "Main B";
        sec.bars = 2;
        sec.resolution = 480;
        sec.beats_per_bar = 4;
        sec.beat_note = 4;
        uasf::TrackDefinition drum{"Rhythm2", 9, uasf::TrackRole::Drum, true, {}, {}};
        for (uint64_t b = 0; b < 8; ++b) note(drum, b * 480, 60, b % 2 ? 38 : 36, 100);
        uasf::TrackDefinition bass{"Bass", 10, uasf::TrackRole::Bass, false, {}, {}};
        note(bass, 0, 400, 40, 90);
        note(bass, 970, 400, 43, 80);    // 10 ticks late
        uasf::TrackDefinition chord{"Chord1", 11, uasf::TrackRole::Chord, false, {}, {}};
        for (uint8_t p : {60, 64, 67}) note(chord, 25, 470, p, 70);   // 25 ticks late
        for (auto* t : {&drum, &bass, &chord}) sortTrack(*t);
        sec.tracks = {drum, bass, chord};
        s.sections.push_back(sec);
    }
    return s;
}

static size_t countOn(const uasf::TrackDefinition& t) {
    size_t n = 0;
    for (const auto& e : t.events) n += e.type == MidiEventType::NoteOn && e.data2 > 0;
    return n;
}
static const MidiEvent* firstOn(const uasf::TrackDefinition& t, uint8_t pitch) {
    for (const auto& e : t.events)
        if (e.type == MidiEventType::NoteOn && e.data2 > 0 && e.data1 == pitch) return &e;
    return nullptr;
}
static const MidiEvent* firstOff(const uasf::TrackDefinition& t, uint8_t pitch) {
    for (const auto& e : t.events)
        if (e.type == MidiEventType::NoteOff && e.data1 == pitch) return &e;
    return nullptr;
}
static bool sameStyle(const uasf::StyleDefinition& a, const uasf::StyleDefinition& b) {
    uasf::UasfSerializer s1, s2;
    return s1.serialize(a).data == s2.serialize(b).data;
}

static uint64_t fnv1a(const std::vector<uint8_t>& b) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}

static void testGrid() {
    std::printf("grid\n");
    TEST("1/16 at 480 = 120", gridTicks(480, "1/16") == 120);
    TEST("1/8t at 480 = 160", gridTicks(480, "1/8t") == 160);
    TEST("1/4 at 96 = 96", gridTicks(96, "1/4") == 96);
    TEST("1/32t at 100 has no whole tick", gridTicks(100, "1/32t") == 0);
    TEST("unknown grid rejected", gridTicks(480, "1/5") == 0);
    TEST("section length 2 bars 4/4 @480", sectionLengthTicks(makeStyle().sections[0]) == 3840);
}

static void testQuantize() {
    std::printf("quantize\n");
    StyleEditor ed(makeStyle());
    Selection sel; sel.section = 0; sel.track = 1;
    auto r = ed.quantize(sel, gridTicks(480, "1/16"), 100);
    const auto& bass = ed.style().sections[0].tracks[1];
    TEST("quantize ok", r.ok);
    TEST("one note moved", r.changed == 1);
    TEST("late bass note on grid", firstOn(bass, 43) && firstOn(bass, 43)->tick == 960);
    TEST("its NoteOff moved with it (length kept)", firstOff(bass, 43) && firstOff(bass, 43)->tick == 1360);
    TEST("other section untouched", ed.style().sections[1].tracks[1].events == makeStyle().sections[1].tracks[1].events);

    StyleEditor half(makeStyle());
    Selection all; all.section = 0; all.track = 2;
    half.quantize(all, 120, 50);
    TEST("strength 50: 25 -> 13 (half way to 0)", firstOn(half.style().sections[0].tracks[2], 60)->tick == 13);

    StyleEditor zero(makeStyle());
    TEST("strength 0 moves nothing", zero.quantize(all, 120, 0).changed == 0);
    TEST("grid 0 rejected", !zero.quantize(all, 0, 100).ok);
    TEST("strength > 100 rejected", !zero.quantize(all, 120, 101).ok);

    // A start that would round onto the element end goes to the previous grid line.
    auto s = makeStyle();
    note(s.sections[0].tracks[1], 3830, 5, 45, 90);
    StyleEditor end(s);
    Selection b; b.section = 0; b.track = 1;
    TEST("near-end quantize ok", end.quantize(b, 480, 100).ok);
    TEST("near-end note to 3360, not 3840", firstOn(end.style().sections[0].tracks[1], 45)->tick == 3360);

    // Regression: a track the op does not change is left exactly as it was, even
    // when it holds data an edit could not produce (a CC at the element end).
    auto odd = makeStyle();
    odd.sections[0].tracks[0].events.push_back({MidiEventType::ControlChange, 9, 7, 100, 3840});   // drum: on grid
    StyleEditor oe(odd);
    Selection whole; whole.section = 0;
    TEST("quantize of a section keeps an unchanged track's odd CC",
         oe.quantize(whole, 120, 100).ok && oe.style().sections[0].tracks[0].events.back().tick == 3840);
    TEST("transpose skips the drum track and keeps its CC", oe.transpose(whole, 1).ok &&
                                                            oe.style().sections[0].tracks[0].events.size() == 17);
    auto odd2 = makeStyle();
    odd2.sections[0].tracks[2].events.push_back({MidiEventType::ControlChange, 11, 7, 100, 3840});
    StyleEditor oe2(odd2);
    auto rq = oe2.quantize(whole, 120, 100);   // the chord track changes and holds the odd CC
    TEST("a changed track with an event past the end is refused, not silently fixed",
         !rq.ok && rq.error.find("past the element end") != std::string::npos);
    StyleEditor ev(makeStyle());
    TEST("non channel-voice event refused",
         !ev.replaceTrackEvents(0, 1, {{MidiEventType::TimingClock, 0, 0, 0, 0}}, "x").ok);
}

static void testTranspose() {
    std::printf("transpose\n");
    StyleEditor ed(makeStyle());
    auto r = ed.transpose(Selection{}, 5);
    TEST("transpose ok", r.ok);
    TEST("drum tracks skipped (both sections)", r.skipped_tracks == 2);
    TEST("drum pitches unchanged", ed.style().sections[0].tracks[0].events == makeStyle().sections[0].tracks[0].events);
    TEST("bass up 5", firstOn(ed.style().sections[0].tracks[1], 45) && firstOff(ed.style().sections[0].tracks[1], 45));
    TEST("chord up 5", firstOn(ed.style().sections[1].tracks[2], 65) != nullptr);
    TEST("10 notes changed", r.changed == 10);

    auto s = makeStyle();
    s.sections[0].tracks[0].role = uasf::TrackRole::Percussion;
    s.sections[0].tracks[0].is_drum = false;
    StyleEditor perc(s);
    Selection one; one.section = 0; one.track = 0;
    TEST("percussion role skipped too", perc.transpose(one, 3).skipped_tracks == 1);

    Selection ch; ch.section = 0; ch.track = 2;
    const auto before = ed.style();
    auto bad = ed.transpose(ch, 70);   // 67 + 70 > 127
    TEST("out-of-range pitch rejects the op", !bad.ok && bad.error.find("0..127") != std::string::npos);
    TEST("rejected op changed nothing", sameStyle(before, ed.style()));
}

static void testVelocity() {
    std::printf("velocity\n");
    StyleEditor ed(makeStyle());
    Selection sel; sel.section = 0; sel.track = 1;
    ed.velocity(sel, VelocityMode::Set, 64);
    TEST("set 64", firstOn(ed.style().sections[0].tracks[1], 40)->data2 == 64);
    ed.velocity(sel, VelocityMode::Offset, 100);
    TEST("offset clamps at 127", firstOn(ed.style().sections[0].tracks[1], 40)->data2 == 127);
    ed.velocity(sel, VelocityMode::ScalePercent, 0);
    TEST("scale 0% clamps at 1, never a NoteOff", firstOn(ed.style().sections[0].tracks[1], 40)->data2 == 1);
    StyleEditor sc(makeStyle());
    sc.velocity(sel, VelocityMode::ScalePercent, 50);
    TEST("scale 50%: 90 -> 45, 80 -> 40", firstOn(sc.style().sections[0].tracks[1], 40)->data2 == 45 &&
                                           firstOn(sc.style().sections[0].tracks[1], 43)->data2 == 40);
    Selection range = sel; range.from = 900;
    StyleEditor rg(makeStyle());
    TEST("tick range limits the op", rg.velocity(range, VelocityMode::Set, 10).changed == 1);
    TEST("NoteOffs untouched", firstOff(ed.style().sections[0].tracks[1], 40)->data2 == 0);
}

static void testTempoMeter() {
    std::printf("tempo / meter\n");
    StyleEditor ed(makeStyle());
    TEST("tempo 96 ok", ed.setTempo(96).ok && ed.style().tempo_bpm == 96);
    TEST("tempo 0 rejected", !ed.setTempo(0).ok);
    TEST("tempo 401 rejected", !ed.setTempo(401).ok && ed.style().tempo_bpm == 96);
    TEST("meter 3/4 refused: events past the new end", !ed.setMeter(0, 3, 4).ok);
    TEST("meter 6/4 ok (longer)", ed.setMeter(0, 6, 4).ok && sectionLengthTicks(ed.style().sections[0]) == 5760);
    TEST("meter den 3 rejected", !ed.setMeter(0, 4, 3).ok);
    TEST("meter on missing section rejected", !ed.setMeter(9, 4, 4).ok);
}

static void testInsertDeleteMove() {
    std::printf("insert / delete / move\n");
    StyleEditor ed(makeStyle());
    MidiEvent on{MidiEventType::NoteOn, 0, 50, 99, 1920};
    TEST("insert note ok", ed.insertEvent(0, 1, on, 240).ok);
    // Re-fetch after every edit: an accepted op replaces the style object.
    const auto* bassP = &ed.style().sections[0].tracks[1];
#define bass (*bassP)
    TEST("note + NoteOff inserted on track channel",
         firstOn(bass, 50) && firstOn(bass, 50)->channel == 10 && firstOff(bass, 50)->tick == 2160);
    TEST("insert NoteOn without length rejected", !ed.insertEvent(0, 1, on, 0).ok);
    TEST("insert past end rejected", !ed.insertEvent(0, 1, MidiEvent{MidiEventType::NoteOn, 0, 50, 99, 3840}, 10).ok);
    TEST("insert NoteOff rejected", !ed.insertEvent(0, 1, MidiEvent{MidiEventType::NoteOff, 0, 50, 0, 10}).ok);
    TEST("insert CC ok", ed.insertEvent(0, 1, MidiEvent{MidiEventType::ControlChange, 0, 7, 100, 0}).ok);
    TEST("insert into missing track rejected", !ed.insertEvent(0, 7, on, 10).ok);

    bassP = &ed.style().sections[0].tracks[1];
    const size_t before = bass.events.size();
    size_t idx = 0;
    while (!(bass.events[idx].type == MidiEventType::NoteOn && bass.events[idx].data1 == 50)) ++idx;
    TEST("delete note ok", ed.deleteEvents(0, 1, {idx}).ok);
    TEST("delete took the NoteOff too", ed.style().sections[0].tracks[1].events.size() == before - 2 &&
                                        !firstOff(ed.style().sections[0].tracks[1], 50));
    TEST("delete bad index rejected", !ed.deleteEvents(0, 1, {999}).ok);

#undef bass
    StyleEditor mv(makeStyle());
#define b2 (mv.style().sections[0].tracks[1])
    size_t i43 = 0;
    while (!(b2.events[i43].type == MidiEventType::NoteOn && b2.events[i43].data1 == 43)) ++i43;
    TEST("move +480 ok", mv.moveEvents(0, 1, {i43}, 480).ok);
    TEST("moved note + NoteOff", firstOn(b2, 43)->tick == 1450 && firstOff(b2, 43)->tick == 1850);
    size_t i43b = 0;
    while (!(b2.events[i43b].type == MidiEventType::NoteOn && b2.events[i43b].data1 == 43)) ++i43b;
    TEST("move before start rejected", !mv.moveEvents(0, 1, {i43b}, -5000).ok);
    TEST("move past end rejected", !mv.moveEvents(0, 1, {i43b}, 3000).ok);
    // Move via the NoteOff index carries the NoteOn as well.
    size_t off = 0;
    while (!(b2.events[off].type == MidiEventType::NoteOff && b2.events[off].data1 == 43)) ++off;
    TEST("move by NoteOff index ok", mv.moveEvents(0, 1, {off}, -10).ok && firstOn(b2, 43)->tick == 1440);
#undef b2
}

static void testCopyPaste() {
    std::printf("copy / paste\n");
    StyleEditor ed(makeStyle());
    std::string err;
    auto clip = ed.copy(0, 2, 0, 480, &err);
    TEST("copy ok", clip.has_value() && clip->events.size() == 6);
    TEST("copy bad range rejected", !ed.copy(0, 2, 480, 0, &err).has_value() && !err.empty());
    // Paste the Chord1 notes of Main A into the Bass track of Main B at bar 2.
    TEST("paste ok", ed.paste(1, 1, 1920, *clip).ok);
    const auto& dst = ed.style().sections[1].tracks[1];
    TEST("pasted on the target channel at the offset", firstOn(dst, 64) && firstOn(dst, 64)->tick == 1945 &&
                                                         firstOn(dst, 64)->channel == 10);
    TEST("pasted 3 notes", countOn(dst) == 5);

    // Clip from a 96-PPQN section is rescaled to 480.
    Clip lo{96, {{MidiEventType::NoteOn, 0, 70, 90, 96}, {MidiEventType::NoteOff, 0, 70, 0, 192}}};
    TEST("rescaled paste ok", ed.paste(0, 1, 0, lo).ok && firstOn(ed.style().sections[0].tracks[1], 70)->tick == 480);
    TEST("paste past end rejected", !ed.paste(0, 1, 3820, *clip).ok);
}

static void testUndoRedo() {
    std::printf("undo / redo\n");
    const auto orig = makeStyle();
    StyleEditor ed(orig, 3);
    TEST("nothing to undo", !ed.undo() && !ed.history().canUndo());
    ed.setTempo(100);
    Selection sel; sel.section = 0; sel.track = 1;
    ed.transpose(sel, 2);
    const auto afterTwo = ed.style();
    TEST("undo label", ed.history().undoLabel() == "Transpose");
    TEST("undo 1", ed.undo() && ed.style().tempo_bpm == 100 && firstOn(ed.style().sections[0].tracks[1], 40));
    TEST("undo 2 = original", ed.undo() && sameStyle(ed.style(), orig));
    TEST("redo 2", ed.redo() && ed.redo() && sameStyle(ed.style(), afterTwo));
    TEST("redo stack empty", !ed.redo());
    ed.undo();
    ed.setTempo(140);
    TEST("new edit clears redo", !ed.history().canRedo());
    const size_t depth = ed.history().undoDepth();
    TEST("rejected op adds no undo step", !ed.setTempo(1).ok && ed.history().undoDepth() == depth);
    ed.setTempo(141); ed.setTempo(142); ed.setTempo(143);
    TEST("history bounded at 3", ed.history().undoDepth() == 3);
    while (ed.undo()) {}
    TEST("oldest steps fell off: undo stops at 140", ed.style().tempo_bpm == 140);
}

static void testValidatorRejection() {
    std::printf("validator rejection\n");
    StyleEditor ed(makeStyle());
    auto r = ed.setMeter(1, 0, 4);   // op allows it; the UASF validator does not
    TEST("meter 0/4 rejected by the UASF validator", !r.ok && r.error.find("UASF validator") != std::string::npos);
    TEST("validator issues returned", !r.issues.empty());
    TEST("style unchanged", ed.style().sections[1].beats_per_bar == 4 && !ed.history().canUndo());

    auto bad = makeStyle();
    bad.resolution = 0;   // an already invalid style: every op is refused until fixed
    StyleEditor inv(bad);
    TEST("op on invalid style refused", !inv.setTempo(100).ok && inv.style().tempo_bpm == 120);
}

static void testChordVariationCopy() {
    std::printf("chord variation copy\n");
    const auto x = exportPa5xSmf(makeStyle());
    TEST("export ok", x.ok);
    KorgStyleEditor ed(x.korg);
    auto r = ed.copyChordVariation("v1cv1", "v1cv2");
    TEST("cv copy ok", r.ok && ed.style().elements.size() == 3);
    const auto& els = ed.style().elements;
    TEST("copy has the source events", els[2].marker == "v1cv2" && els[2].events == els[0].events &&
                                       els[2].length == els[0].length);
    TEST("result still passes the Korg validator", korg::validate(ed.style()).empty());
    TEST("overwrite existing cv ok", ed.copyChordVariation("v2cv1", "v1cv2").ok &&
                                     ed.style().elements[2].events == ed.style().elements[1].events);
    TEST("invalid marker rejected (intro has cv1-2)", !ed.copyChordVariation("v1cv1", "i1cv3").ok);
    TEST("missing source rejected", !ed.copyChordVariation("v3cv1", "v3cv2").ok);
    TEST("undo restores the first copy", ed.undo() && ed.style().elements[2].events == ed.style().elements[0].events);
    TEST("undo again removes it", ed.undo() && ed.style().elements.size() == 2);
    TEST("redo", ed.redo() && ed.style().elements.size() == 3);
    const auto w = korg::write(ed.style());
    TEST("written SMF re-reads with 3 markers", w.ok && korg::parse(w.bytes).style.elements.size() == 3);
}

static std::vector<uint8_t> smf1() {
    // Format 1, 96 PPQN, track 0 = tempo only, track 1 = ch 3 (two notes, the
    // second past a 1-bar element) + ch 4 note, running status.
    std::vector<uint8_t> t0 = {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x00, 0xFF, 0x2F, 0x00};
    std::vector<uint8_t> t1 = {0x00, 0x92, 60, 100,   // ch3 on @0
                               0x30, 0x82, 60, 0,     // off @48
                               0x00, 0x93, 72, 90,    // ch4 on @48
                               0x30, 0x83, 72, 0,     // off @96
                               0x82, 0x40, 0x92, 62, 110,   // ch3 on @96+320=416 (past 1 bar = 384)
                               0x10, 62, 0,           // running status NoteOn vel 0 @432
                               0x00, 0xFF, 0x2F, 0x00};
    std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0, 96};
    for (auto* t : {&t0, &t1}) {
        b.insert(b.end(), {'M', 'T', 'r', 'k', 0, 0, 0, static_cast<uint8_t>(t->size())});
        b.insert(b.end(), t->begin(), t->end());
    }
    return b;
}

static void testSmfImport() {
    std::printf("plain SMF track import\n");
    const auto smf = readSmf(smf1());
    TEST("SMF read", smf.ok && smf.ppqn == 96 && smf.tracks.size() == 2 && smf.tracks[1].size() == 6);
    TEST("garbage rejected", !readSmf({'M', 'T', 'h', 'd'}).ok);
    TEST("format 2 rejected", [] { auto b = smf1(); b[9] = 2; return !readSmf(b).ok; }());

    auto s = makeStyle();
    s.sections[0].bars = 1;   // 1920 ticks
    for (auto& t : s.sections[0].tracks) t.events.erase(
        std::remove_if(t.events.begin(), t.events.end(), [](const MidiEvent& e) { return e.tick >= 1920; }), t.events.end());
    StyleEditor ed(s);
    size_t dropped = 0;
    auto ev = smfTrackToUasf(smf, 1, uint8_t{2}, 480, 1920, &dropped);
    TEST("channel filter + rescale 96 -> 480", ev.size() == 2 && ev[0].tick == 0 && ev[1].tick == 240);
    TEST("note past the end dropped with its NoteOff", dropped == 2);
    TEST("import replaces the track", ed.replaceTrackEvents(0, 2, ev, "Import SMF track").ok);
    const auto& t = ed.style().sections[0].tracks[2];
    TEST("imported on the track's channel", t.events.size() == 2 && t.events[0].channel == 11 && t.events[0].data1 == 60);
    TEST("import is one undo step", ed.history().undoLabel() == "Import SMF track" && ed.undo() &&
                                    sameStyle(ed.style(), s));

    // A NoteOn without NoteOff is closed at the element end.
    std::vector<MidiEvent> hang = {{MidiEventType::NoteOn, 0, 55, 80, 100}};
    TEST("hanging note closed", ed.replaceTrackEvents(0, 1, hang, "x").ok &&
                                firstOff(ed.style().sections[0].tracks[1], 55)->tick == 1920);
}

static void testRoundTrip() {
    std::printf("UASF -> Pa5X SMF markers -> reader round trip\n");
    for (int edited = 0; edited < 2; ++edited) {
        StyleEditor ed(makeStyle());
        if (edited) {
            Selection q; q.section = 0;
            ed.quantize(q, 120, 100);
            ed.transpose(Selection{}, -2);
            ed.setTempo(98);
            ed.setMeter(1, 6, 4);
        }
        const auto x = exportPa5xSmf(ed.style());
        TEST(edited ? "edited style exports" : "original style exports", x.ok);
        const auto back = korg::parse(x.bytes);
        TEST("reader parses it", back.ok && back.notes.empty());
        bool equal = back.style.ppqn == x.korg.ppqn && back.style.mpqn == x.korg.mpqn &&
                     back.style.elements.size() == x.korg.elements.size();
        for (size_t i = 0; equal && i < x.korg.elements.size(); ++i) {
            const auto &a = x.korg.elements[i], &b = back.style.elements[i];
            equal = a.marker == b.marker && a.num == b.num && a.den == b.den && a.length == b.length && a.events == b.events;
        }
        TEST("read-back equals the written Korg style", equal);
        TEST("compareStyles finds no difference", korg::compareStyles(x.korg, back.style).differences.empty());
        if (edited) {
            TEST("edited meter exported", back.style.elements[1].num == 6 && back.style.elements[1].length == 2 * 2880);
            TEST("edited tempo exported", back.style.mpqn == 612245);
        }
        // UASF binary round trip of the edited style too.
        uasf::UasfSerializer ser;
        uasf::UasfDeserializer de;
        const auto bin = ser.serialize(ed.style());
        TEST("UASF serialize -> deserialize equal", bin.success && sameStyle(de.deserialize(bin.data).style, ed.style()));
    }
}

// Golden: fixed synthetic style + fixed edit script -> exact SMF bytes (FNV-1a).
// A change here means the edit or export semantics changed: review, then re-pin.
static void testGolden() {
    std::printf("golden\n");
    StyleEditor ed(makeStyle());
    Selection q; q.section = 0;
    ed.quantize(q, gridTicks(480, "1/16"), 100);
    Selection v; v.track = 1;
    ed.velocity(v, VelocityMode::Offset, 7);
    ed.transpose(Selection{}, 3);
    auto clip = ed.copy(0, 2, 0, 480);
    ed.paste(1, 2, 960, *clip);
    const auto x = exportPa5xSmf(ed.style());
    const uint64_t h = fnv1a(x.bytes);
    std::printf("    golden size %zu hash %016llx\n", x.bytes.size(), static_cast<unsigned long long>(h));
    TEST("golden SMF bytes", x.ok && x.bytes.size() == GOLDEN_SIZE && h == GOLDEN_HASH);
}

static void testElementOps() {
    std::printf("element and track-role ops\n");
    TEST("element names", std::string(sectionTypeName(uasf::SectionType::Main3)) == "Variation 3" &&
                          sectionTypeFromName("variation 2") == uasf::SectionType::Main2 &&
                          sectionTypeFromName("Main 4") == uasf::SectionType::Main4 &&
                          sectionTypeFromName("ending3") == uasf::SectionType::Ending3 &&
                          sectionTypeFromName("Break") == uasf::SectionType::Break && !sectionTypeFromName("Verse"));
    TEST("role names", trackRoleFromName("Percussion") == uasf::TrackRole::Percussion && !trackRoleFromName("lead"));
    const auto s0 = makeStyle();
    StyleEditor ed(s0);
    // copy onto a missing element appends it, onto an existing one replaces it (keeping its name)
    TEST("copy Variation 1 to Intro 3 (new)", ed.copySection(0, uasf::SectionType::Intro3).ok && ed.style().sections.size() == 3 &&
         ed.style().sections[2].type == uasf::SectionType::Intro3 && ed.style().sections[2].name == "Intro 3" &&
         ed.style().sections[2].tracks.size() == 3 && ed.style().sections[2].tracks[0].events == s0.sections[0].tracks[0].events);
    TEST("copy undo label", ed.history().undoLabel() == "Copy to Intro 3");
    TEST("copy over Variation 2 keeps its name", ed.copySection(2, uasf::SectionType::Main2).ok && ed.style().sections[1].name == "Main B" &&
         ed.style().sections[1].tracks[1].events == s0.sections[0].tracks[1].events && ed.style().sections.size() == 3);
    TEST("copy onto itself refused", !ed.copySection(0, uasf::SectionType::Main1).ok);
    TEST("copy from a missing element refused", !ed.copySection(9, uasf::SectionType::Fill1).ok);
    TEST("clear element", ed.clearSection(2).ok && ed.style().sections[2].tracks[0].events.empty() &&
         ed.style().sections[2].bars == 2 && ed.style().sections[0].tracks[0].events.size() == 16);
    TEST("rename element", ed.renameSection(0, "Groove A").ok && ed.style().sections[0].name == "Groove A");
    TEST("empty name refused", !ed.renameSection(0, "").ok);
    TEST("lengthen element", ed.setSectionBars(0, 4).ok && ed.style().sections[0].bars == 4);
    TEST("shorten past events refused, nothing changed", !ed.setSectionBars(1, 1).ok && ed.style().sections[1].bars == 2);
    TEST("length 0 refused", !ed.setSectionBars(0, 0).ok);
    TEST("role is style-wide per channel", ed.setChannelRole(11, uasf::TrackRole::Pad).ok &&
         std::all_of(ed.style().sections.begin(), ed.style().sections.end(),
                     [](const auto& sec) { return sec.tracks[2].role == uasf::TrackRole::Pad && !sec.tracks[2].is_drum; }));
    TEST("percussion role sets is_drum", ed.setChannelRole(10, uasf::TrackRole::Percussion).ok && ed.style().sections[0].tracks[1].is_drum);
    const auto percBefore = ed.style().sections[0].tracks[1].events;
    TEST("transpose now skips the percussion track", ed.transpose(Selection{}, 1).ok &&
         ed.style().sections[0].tracks[1].events == percBefore && ed.style().sections[0].tracks[2].events[0].data1 != percBefore[0].data1);
    TEST("role on an unused channel refused", !ed.setChannelRole(3, uasf::TrackRole::Bass).ok);
    while (ed.undo()) {}
    TEST("everything undoes to the original", sameStyle(ed.style(), s0));
}

static void testSoundsAndRoute() {
    std::printf("export with user sounds and CASM route\n");
    korg::MapOptions opt;
    opt.sounds[korg::kBassCh] = {121, 3, 34};
    opt.route[11] = 15;   // Chord1 (ch 12) routed to Phrase2 -> Korg Acc5
    const auto x = exportPa5xSmf(makeStyle(), 0, opt);
    TEST("export ok", x.ok);
    auto has = [&](uint8_t st, uint8_t d1, uint8_t d2) {
        for (const auto& e : x.korg.elements[0].events)
            if (e.tick == 0 && e.status == st && e.d1 == d1 && (st >= 0xC0 || e.d2 == d2)) return true;
        return false;
    };
    TEST("bank select + program change at element start", has(0xB0 | korg::kBassCh, 0, 121) && has(0xB0 | korg::kBassCh, 32, 3) &&
                                                          has(0xC0 | korg::kBassCh, 34, 0));
    TEST("routed track lands on Acc5, not Acc1",
         std::any_of(x.korg.elements[0].events.begin(), x.korg.elements[0].events.end(), [](const auto& e) { return e.status == 0x9F; }) &&
         std::none_of(x.korg.elements[0].events.begin(), x.korg.elements[0].events.end(), [](const auto& e) { return e.status == 0x9B; }));
    TEST("report names the user sound", x.report.find("MSB 121 LSB 3 PC 34 (user map)") != std::string::npos);
}

static void testKorgSmfImport() {
    std::printf("Korg marker SMF -> UASF import\n");
    korg::MapOptions opt;
    opt.sounds[korg::kDrumCh] = {120, 0, 16};
    StyleEditor ed(makeStyle());
    ed.copySection(0, uasf::SectionType::Fill2);
    ed.copySection(0, uasf::SectionType::Ending1);
    const auto x = exportPa5xSmf(ed.style(), 0, opt);
    const auto in = importKorgSmf(x.bytes, "Back");
    TEST("import ok", in.ok && in.style.name == "Back" && in.style.tempo_bpm == 120 && in.style.resolution == 480);
    TEST("elements by marker", in.style.sections.size() == 4 && in.style.sections[2].type == uasf::SectionType::Fill2 &&
                               in.style.sections[3].name == "Ending 1" && in.style.sections[0].bars == 2);
    TEST("tracks with Korg roles", in.style.sections[0].tracks.size() == 3 && in.style.sections[0].tracks[0].role == uasf::TrackRole::Bass &&
                                   in.style.sections[0].tracks[1].is_drum && in.style.sections[0].tracks[1].midi_channel == 9);
    TEST("sound taken out of the events", in.sounds.size() == 1 && in.sounds.at(korg::kDrumCh).msb == 120 && in.sounds.at(korg::kDrumCh).pc == 16 &&
         std::none_of(in.style.sections[0].tracks[1].events.begin(), in.style.sections[0].tracks[1].events.end(),
                      [](const auto& e) { return e.type == MidiEventType::ProgramChange || e.type == MidiEventType::ControlChange; }));
    korg::MapOptions back;
    back.sounds = in.sounds;
    const auto y = exportPa5xSmf(in.style, 0, back);
    TEST("re-export equals the first export", y.ok && korg::compareStyles(x.korg, y.korg).differences.empty() && y.bytes == x.bytes);
    // A Chord Variation 2 element is reported, not imported.
    KorgStyleEditor kd(x.korg);
    kd.copyChordVariation("v1cv1", "v1cv2");
    const auto cv = importKorgSmf(korg::write(kd.style()).bytes, "CV");
    TEST("cv2 listed in notes, not imported", cv.ok && cv.style.sections.size() == 4 &&
         std::any_of(cv.notes.begin(), cv.notes.end(), [](const std::string& n) { return n.find("v1cv2 not imported") != std::string::npos; }));
    // Codex r1 #2: different bank/program per element is reported, never silently replaced by the first
    {
        korg::Style ks = x.korg;
        auto setPc = [&](korg::Element& el, uint8_t pc) {
            for (auto& e : el.events) if (e.status == (0xC0 | korg::kDrumCh)) e.d1 = pc;
        };
        setPc(ks.elements[1], 25);
        const auto diff = importKorgSmf(korg::write(ks).bytes, "Diff");
        TEST("r1#2: conflicting element sound reported", diff.ok && diff.sounds.at(korg::kDrumCh).pc == 16 &&
             std::any_of(diff.notes.begin(), diff.notes.end(), [](const std::string& n) {
                 return n.find("v2cv1") != std::string::npos && n.find("120/0/25") != std::string::npos && n.find("120/0/16") != std::string::npos; }));
        TEST("r1#2: same sound in every element: no note", std::none_of(in.notes.begin(), in.notes.end(), [](const std::string& n) {
                 return n.find("differs") != std::string::npos; }));
        // Korg validate() allows bank/program only at tick 0 (a mid-element change is refused); a second one there:
        ks.elements[0].events.insert(ks.elements[0].events.begin(), {0, static_cast<uint8_t>(0xC0 | korg::kDrumCh), 40, 0});
        const auto mid = importKorgSmf(korg::write(ks).bytes, "Mid");
        TEST("r1#2: a later bank/program change inside an element is reported", mid.ok &&
             std::any_of(mid.notes.begin(), mid.notes.end(), [](const std::string& n) { return n.find("later bank/program") != std::string::npos; }));
    }
    TEST("plain garbage refused", !importKorgSmf({'M', 'T', 'h', 'd'}, "x").ok);
    TEST("plain SMF without markers refused", !importKorgSmf(smf1(), "x").ok);
}

int main() {
    std::printf("Test: Style Studio editing core\n");
    testGrid();
    testQuantize();
    testTranspose();
    testVelocity();
    testTempoMeter();
    testInsertDeleteMove();
    testCopyPaste();
    testUndoRedo();
    testValidatorRejection();
    testChordVariationCopy();
    testSmfImport();
    testRoundTrip();
    testGolden();
    testElementOps();
    testSoundsAndRoute();
    testKorgSmfImport();
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
