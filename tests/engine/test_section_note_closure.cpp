// Notes crossing a section boundary (Issue #3).
//
// A NoteOn inside a section whose NoteOff lies past the section end used to stay
// sounding: the importer's marker split dropped the NoteOff into the next
// section, a looping Main never re-closed the note, and the dispatcher suppressed
// every later NoteOn of it as a duplicate. Result: one held note for as long as
// the Main looped (released only by a section switch or Stop).
//
// Contract: a note the section cannot close is closed exactly at the section end
// (uasf::closeOpenNotes, applied by the importer and again by StylePlayer::
// loadStyle for styles from any source). Notes ending inside the section or
// exactly on its end are untouched. Measured DURING playback, per loop.

#include "engine/uasf/note_closure.h"
#include "engine/arranger/style_player.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace ai_arranger;
using uasf::MidiEvent;
using uasf::MidiEventType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } \
} while(0)

static MidiEvent on(uint64_t t, uint8_t n, uint8_t ch = 9) { return {MidiEventType::NoteOn, ch, n, 100, t}; }
static MidiEvent off(uint64_t t, uint8_t n, uint8_t ch = 9) { return {MidiEventType::NoteOff, ch, n, 0, t}; }

static int count(const uasf::TrackDefinition& t, MidiEventType type, uint8_t note, uint64_t tick) {
    int n = 0;
    for (const auto& e : t.events)
        if (e.type == type && e.data1 == note && e.tick == tick) ++n;
    return n;
}

// One-bar 480-PPQN 4/4 section (1920 ticks) holding one drum track (no chord
// transposition) with the given events.
static uasf::SectionDefinition section(uasf::SectionType type, std::vector<MidiEvent> evs,
                                       uint32_t bars = 1) {
    uasf::SectionDefinition s;
    s.type = type; s.name = "s"; s.bars = bars; s.resolution = 480;
    s.beats_per_bar = 4; s.beat_note = 4;
    uasf::TrackDefinition t;
    t.midi_channel = 9; t.role = uasf::TrackRole::Drum; t.is_drum = true;
    t.events = std::move(evs);
    s.tracks.push_back(std::move(t));
    return s;
}

namespace {
// Player rig recording each output event with the clock position at which it
// was actually emitted (flush NoteOffs carry tick 0, so e.tick is not enough).
struct Rig {
    realtime::RealtimeClock clock;
    midi::MidiScheduler     sch;
    midi::PanicHandler      pan;
    arranger::StylePlayer   player{clock, sch, pan};
    struct Out { MidiEvent e; int64_t at; };
    std::vector<Out> out;
    explicit Rig(const uasf::StyleDefinition& st) {
        sch.setOutputCallback([this](const MidiEvent& e) { out.push_back({e, clock.getPosition()}); });
        player.loadStyle(st);
    }
    void runTo(int64_t tick) {
        while (clock.isRunning() && clock.getPosition() < tick) {
            clock.advance(48);
            player.tick();
            sch.advanceTo(clock.getPosition());
        }
    }
    int ons(uint8_t note, int64_t from, int64_t to) const {   // emitted in [from, to)
        int n = 0;
        for (const auto& o : out)
            if (o.e.type == MidiEventType::NoteOn && o.e.data2 > 0 && o.e.data1 == note &&
                o.at >= from && o.at < to) ++n;
        return n;
    }
    // NoteOn/NoteOff of `note` strictly alternate (on, off, on, off, ...).
    bool alternates(uint8_t note) const {
        bool sounding = false;
        for (const auto& o : out) {
            if (o.e.data1 != note) continue;
            const bool isOn = o.e.type == MidiEventType::NoteOn && o.e.data2 > 0;
            const bool isOff = o.e.type == MidiEventType::NoteOff ||
                               (o.e.type == MidiEventType::NoteOn && o.e.data2 == 0);
            if (isOn) { if (sounding) return false; sounding = true; }
            else if (isOff) { if (!sounding) return false; sounding = false; }
        }
        return true;
    }
};
} // namespace

int main() {
    std::printf("Test: notes crossing a section boundary\n");
    constexpr int64_t kBar = 1920;

    {   // ── closeOpenNotes: which notes get closed ──
        uasf::TrackDefinition t;
        t.events = {on(100, 36), off(200, 36),       // ends inside
                    on(300, 38), off(kBar, 38),      // ends exactly on the boundary
                    on(1440, 42), off(2400, 42),     // ends past the boundary
                    on(1500, 46),                    // never ends
                    on(1600, 49), {MidiEventType::NoteOn, 9, 49, 0, 1700}};   // vel-0 off inside
        const size_t added = uasf::closeOpenNotes(t, kBar);
        TEST("closes exactly the 2 notes the section cannot close", added == 2);
        TEST("note ending inside: untouched", count(t, MidiEventType::NoteOff, 36, kBar) == 0);
        TEST("note ending exactly on the boundary: no extra NoteOff",
             count(t, MidiEventType::NoteOff, 38, kBar) == 1);
        TEST("note ending past the boundary: NoteOff added at the boundary",
             count(t, MidiEventType::NoteOff, 42, kBar) == 1);
        TEST("note ending past the boundary: original NoteOff left as is",
             count(t, MidiEventType::NoteOff, 42, 2400) == 1);
        TEST("note without NoteOff: closed at the boundary", count(t, MidiEventType::NoteOff, 46, kBar) == 1);
        TEST("velocity-0 NoteOn counts as NoteOff", count(t, MidiEventType::NoteOff, 49, kBar) == 0);
        TEST("idempotent: a second call adds nothing", uasf::closeOpenNotes(t, kBar) == 0);

        uasf::TrackDefinition u;   // events stored out of time order
        u.events = {off(900, 40), on(100, 40), on(1000, 40)};
        TEST("unsorted events are walked in time order (1 open note)", uasf::closeOpenNotes(u, kBar) == 1);
        TEST("unknown-length section is skipped",
             [] { auto s = section(uasf::SectionType::Main1, {on(0, 36)}, 0);
                  return uasf::closeOpenNotes(s) == 0; }());
    }

    // Main A (1 bar): kick inside, snare ending on the boundary, hi-hat crossing it.
    const std::vector<MidiEvent> crossing = {on(0, 36), off(240, 36), on(480, 38), off(kBar, 38),
                                             on(1440, 42), off(2400, 42)};
    {   // ── Main loop: measured per loop while it plays ──
        uasf::StyleDefinition st;
        st.resolution = 480; st.tempo_bpm = 120;
        st.sections.push_back(section(uasf::SectionType::Main1, crossing));
        Rig r(st);
        r.player.start(0);
        r.runTo(4 * kBar);                              // through the 4th pass
        bool everyLoop = true;
        for (int k = 0; k < 4; ++k)
            everyLoop = everyLoop && r.ons(42, k * kBar, (k + 1) * kBar) == 1;
        TEST("crossing note re-triggers in every one of 4 loops (not held from loop 1)", everyLoop);
        TEST("crossing note: NoteOn/NoteOff strictly alternate while looping", r.alternates(42));
        TEST("boundary note: NoteOn/NoteOff strictly alternate while looping", r.alternates(38));
        TEST("inside note plays every loop", r.ons(36, 0, 4 * kBar) == 4);
        r.runTo(4 * kBar + 1);                          // just past the 4th boundary
        TEST("at a loop boundary nothing from the previous pass is still sounding",
             !r.pan.isNoteActive(9, 42) && !r.pan.isNoteActive(9, 38));
    }
    {   // ── Intro -> Main and Fill -> Main hand-overs, Ending -> Stop ──
        uasf::StyleDefinition st;
        st.resolution = 480; st.tempo_bpm = 120;
        st.sections.push_back(section(uasf::SectionType::Intro1, crossing));
        st.sections.push_back(section(uasf::SectionType::Main1, {on(0, 50), off(960, 50)}));
        st.sections.push_back(section(uasf::SectionType::Fill1, crossing));
        st.sections.push_back(section(uasf::SectionType::Ending1, crossing));
        Rig r(st);
        r.player.start(0);
        r.runTo(kBar + kBar / 2);                       // Intro done, inside Main
        TEST("Intro hands over to Main", r.player.getCurrentSection() == 1);
        TEST("Intro's crossing note closed at the hand-over", !r.pan.isNoteActive(9, 42));
        r.player.fill();
        r.runTo(2 * kBar + kBar / 2);
        TEST("Fill plays", r.player.getCurrentSection() == 2);
        r.runTo(3 * kBar + kBar / 2);
        TEST("Fill returns to Main", r.player.getCurrentSection() == 1);
        TEST("Fill's crossing note closed at the return", !r.pan.isNoteActive(9, 42));
        r.player.ending();
        r.runTo(6 * kBar);
        TEST("Ending stops the transport", !r.player.isPlaying());
        TEST("every note alternates on/off through Intro, Main, Fill, Ending",
             r.alternates(36) && r.alternates(38) && r.alternates(42) && r.alternates(50));
    }
    {   // ── Importer: the marker split closes a note at the section end ──
        // 480 PPQN 4/4. "Main A" 0..1920, "Ending A" 1920..3840.
        const std::vector<uint8_t> trk = {
            0x00, 0xFF, 0x06, 0x06, 'M', 'a', 'i', 'n', ' ', 'A',               // t0
            0x00, 0x99, 0x24, 0x64,                                            // t0    kick on
            0x81, 0x70, 0x89, 0x24, 0x00,                                      // t240  kick off
            0x81, 0x70, 0x99, 0x26, 0x64,                                      // t480  snare on
            0x87, 0x40, 0x99, 0x2A, 0x64,                                      // t1440 hat on
            0x83, 0x60, 0x89, 0x26, 0x00,                                      // t1920 snare off
            0x00, 0xFF, 0x06, 0x08, 'E', 'n', 'd', 'i', 'n', 'g', ' ', 'A',    // t1920
            0x83, 0x60, 0x89, 0x2A, 0x00,                                      // t2400 hat off
            0x8B, 0x20, 0xFF, 0x2F, 0x00};                                     // t3840 EOT
        std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                                  'M', 'T', 'r', 'k', 0, 0, 0, static_cast<uint8_t>(trk.size())};
        f.insert(f.end(), trk.begin(), trk.end());
        importers::sff1::Sff1Reader reader;
        importers::sff1::Sff1ToUasfMapper mapper;
        const auto mr = mapper.map(reader.parseBuffer(f, "synthetic"));
        const uasf::SectionDefinition* mainA = nullptr;
        for (const auto& s : mr.style.sections) if (s.type == uasf::SectionType::Main1) mainA = &s;
        TEST("importer: Main A split from the markers", mr.success && mainA && mainA->bars == 1);
        if (mainA && !mainA->tracks.empty()) {
            const auto& t = mainA->tracks[0];
            TEST("importer: hi-hat crossing into Ending A is closed at Main A's end (1920)",
                 count(t, MidiEventType::NoteOff, 0x2A, kBar) == 1);
            TEST("importer: snare ending on the boundary keeps its single NoteOff",
                 count(t, MidiEventType::NoteOff, 0x26, kBar) == 1);
            TEST("importer: kick ending inside is untouched", count(t, MidiEventType::NoteOff, 0x24, kBar) == 0);
        }
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
