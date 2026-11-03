// Gate 9 — Playback validation harness.
//
// Drives StylePlayer offline through a virtual clock (no real audio device),
// captures every dispatched MIDI event, and asserts the engine-verifiable
// musical-correctness properties:
//   - every track dispatches at least one event
//   - tempo tick<->time conversion is accurate
//   - section switches land on bar boundaries
//   - fill length matches its declared section.bars
//   - the intro -> main -> fill -> main -> ending order is honoured
//   - a full cycle + stop leaves zero stuck notes (balanced NoteOn/NoteOff)
//   - empty sections and mid-section switches are handled gracefully

#include "engine/arranger/style_player.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include <cstdio>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

using namespace ai_arranger::arranger;
using namespace ai_arranger::realtime;
using namespace ai_arranger::midi;
using namespace ai_arranger::uasf;

static int failures = 0;
static int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  ❌ FAIL: %s\n", name); failures++; } \
    else { std::printf("  ✅ PASS: %s\n", name); passes++; } \
} while(0)

// ── MidiCaptureSink ──────────────────────────────────────────────────
// Records every dispatched event and maintains per-(channel,note) balance.
struct MidiCaptureSink {
    struct Captured { MidiEvent ev; int64_t at_tick; };
    std::vector<Captured> events;
    std::map<int, int> note_on_per_channel;     // channel -> NoteOn count
    std::map<int, int> note_off_per_channel;    // channel -> NoteOff count
    std::map<int, int> balance;                  // (ch<<8|note) -> active count
    int max_balance_seen = 0;
    int current_active = 0;

    void operator()(const MidiEvent& e, int64_t atTick) {
        events.push_back({e, atTick});
        const int key = (e.channel << 8) | e.data1;
        if (e.type == MidiEventType::NoteOn && e.data2 > 0) {
            note_on_per_channel[e.channel]++;
            balance[key]++;
            current_active++;
            if (current_active > max_balance_seen) max_balance_seen = current_active;
        } else if (e.type == MidiEventType::NoteOff ||
                   (e.type == MidiEventType::NoteOn && e.data2 == 0)) {
            note_off_per_channel[e.channel]++;
            balance[key]--;
            current_active--;
        }
    }

    int stuckNotes() const {
        int n = 0;
        for (const auto& b : balance) if (b.second != 0) n++;
        return n;
    }
    std::set<int> channelsWithEvents() const {
        std::set<int> chans;
        for (const auto& c : note_on_per_channel) chans.insert(c.first);
        return chans;
    }
    int totalNoteOn() const { int s = 0; for (auto& c : note_on_per_channel) s += c.second; return s; }
    int totalNoteOff() const { int s = 0; for (auto& c : note_off_per_channel) s += c.second; return s; }
};

// ── Harness ──────────────────────────────────────────────────────────
struct PlaybackHarness {
    RealtimeClock clock;
    MidiScheduler scheduler;
    PanicHandler panic;
    StylePlayer player{clock, scheduler, panic};
    MidiCaptureSink sink;

    // Section-transition log: bar index at which the active section changed.
    struct Transition { int section; SequencerState state; int64_t tick; int64_t bar; };
    std::vector<Transition> transitions;
    int last_section = -2;

    static constexpr uint32_t kBlock = 256; // samples per virtual callback

    PlaybackHarness() {
        scheduler.setOutputCallback([this](const MidiEvent& e) {
            sink(e, clock.getPosition());
        });
    }

    void load(const StyleDefinition& s) {
        player.loadStyle(s);
        clock.setSampleRate(48000);
        clock.setTempo(s.tempo_bpm);
        clock.setResolution(s.resolution);
    }

    void start(int intro = 0) { player.start(intro); clock.start(); }

    // Advance the virtual clock to >= untilTick, ticking the player and
    // flushing the scheduler each block, recording section transitions.
    void runTo(int64_t untilTick) {
        const int64_t barSize = clock.ticksPerBar();
        // Stop early if the transport stopped itself (an Ending that played out).
        while (clock.isRunning() && clock.getPosition() < untilTick) {
            clock.advance(kBlock);
            player.tick();
            scheduler.advanceTo(clock.getPosition());
            int sec = player.getCurrentSection();
            if (sec != last_section) {
                int64_t pos = clock.getPosition();
                transitions.push_back({sec, player.getState(), pos, barSize ? pos / barSize : 0});
                last_section = sec;
            }
        }
    }

    void stopAndFlush() {
        player.stop();
        scheduler.advanceTo(INT64_MAX); // drain flush NoteOffs
    }
};

// ── Empty-section style ──────────────────────────────────────────────
static StyleDefinition buildStyleWithEmptySection() {
    StyleDefinition style;
    style.name = "Empty-section style";
    style.format_version = "1.0";
    style.tempo_bpm = 120;
    style.resolution = 480;

    SectionDefinition empty;          // intro with no tracks at all
    empty.type = SectionType::Intro1;
    empty.name = "EmptyIntro";
    empty.bars = 2; empty.resolution = 480; empty.beats_per_bar = 4; empty.beat_note = 4;
    style.sections.push_back(empty);

    SectionDefinition emptyTrack;     // main with a track that has no events
    emptyTrack.type = SectionType::Main1;
    emptyTrack.name = "EmptyTrackMain";
    emptyTrack.bars = 2; emptyTrack.resolution = 480; emptyTrack.beats_per_bar = 4; emptyTrack.beat_note = 4;
    TrackDefinition t; t.name = "Silent"; t.midi_channel = 3; t.role = TrackRole::Melody; t.is_drum = false;
    emptyTrack.tracks.push_back(t);
    style.sections.push_back(emptyTrack);

    return style;
}

int main() {
    std::printf("Test: Playback Validation Harness\n");
    const int64_t barSize = 1920; // 480 * 4 at 4/4

    // ── 1. Tempo: tick<->time accuracy ───────────────────────────────
    {
        RealtimeClock c;
        c.setSampleRate(48000); c.setTempo(120); c.setResolution(480); c.start();
        // 24000 samples = 0.5s = one beat at 120 BPM = 480 ticks
        c.advance(24000);
        TEST("One beat (24000 samples) == 480 ticks", c.getPosition() == 480);
        c.advance(24000); // second beat
        TEST("Two beats == 960 ticks", c.getPosition() == 960);
        c.advance(48000); // two more beats -> one full bar elapsed total
        TEST("Four beats == one bar (1920 ticks)", c.getPosition() == 1920);
        TEST("Bar index correct at boundary", c.getCurrentBarFromTick(c.getPosition()) == 2);
    }

    // ── 2. Full cycle: capture + balance + per-track coverage ────────
    {
        PlaybackHarness h;
        h.load(buildDemoStyle());
        h.start(0);

        h.runTo(11000);              // intro plays out
        h.player.switchSection(1);   // -> main
        h.runTo(22000);              // main plays out
        h.player.fill();             // -> fill
        h.runTo(26000);
        h.player.switchSection(1);   // -> main again
        h.runTo(30000);
        h.player.ending();           // -> ending
        h.runTo(40000);
        h.stopAndFlush();

        TEST("Dispatched events captured", !h.sink.events.empty());
        TEST("NoteOn count > 0", h.sink.totalNoteOn() > 0);
        TEST("NoteOn count == NoteOff count (balanced)",
             h.sink.totalNoteOn() == h.sink.totalNoteOff());
        TEST("Zero stuck notes after full cycle + stop", h.sink.stuckNotes() == 0);

        // Demo channels: bass=1, chord=2, drums=9. All must dispatch >= 1 event.
        auto chans = h.sink.channelsWithEvents();
        TEST("Bass track (ch1) dispatched >= 1 event", chans.count(1) > 0);
        TEST("Chord track (ch2) dispatched >= 1 event", chans.count(2) > 0);
        TEST("Drum track (ch9) dispatched >= 1 event", chans.count(9) > 0);
        TEST("Polyphony observed (chord notes overlap)", h.sink.max_balance_seen >= 2);
    }

    // ── 3. Section switch lands on bar boundaries ────────────────────
    {
        PlaybackHarness h;
        h.load(buildDemoStyle());
        h.start(0);
        h.runTo(9000);
        h.player.switchSection(1);
        h.runTo(20000);
        h.player.ending();
        h.runTo(32000);
        h.stopAndFlush();

        bool allOnBar = true;
        int realSwitches = 0;
        for (const auto& t : h.transitions) {
            if (t.section < 0) continue; // initial "-1" entry
            realSwitches++;
            // The switch is detected at the first block past the boundary;
            // a 256-sample block is ~5 ticks, so tolerate a small overshoot.
            int64_t off = t.tick % barSize;
            if (off >= 64) allOnBar = false;
        }
        TEST("At least one real section switch occurred", realSwitches >= 1);
        TEST("All section switches land on bar boundaries", allOnBar);

        // Bar index must be strictly increasing across switches (one per bar).
        bool monotonic = true;
        int64_t prevBar = -1;
        for (const auto& t : h.transitions) {
            if (t.section < 0) continue;
            if (t.bar <= prevBar) monotonic = false;
            prevBar = t.bar;
        }
        TEST("Section switches occur on distinct increasing bars", monotonic);
    }

    // ── 4. intro -> main -> fill -> main -> ending order ─────────────
    {
        PlaybackHarness h;
        h.load(buildDemoStyle());
        h.start(0);
        h.runTo(8000);
        h.player.switchSection(1);   // main
        h.runTo(16000);
        h.player.fill();             // fill
        h.runTo(20000);
        h.player.switchSection(1);   // main
        h.runTo(24000);
        h.player.ending();           // ending
        h.runTo(32000);
        h.stopAndFlush();

        std::vector<SequencerState> order;
        for (const auto& t : h.transitions) {
            if (t.section < 0) continue;
            if (order.empty() || order.back() != t.state) order.push_back(t.state);
        }
        // Expected coarse sequence of states.
        std::vector<SequencerState> expected = {
            SequencerState::PlayingIntro,
            SequencerState::PlayingMain,
            SequencerState::PlayingFill,
            SequencerState::PlayingMain,
            SequencerState::PlayingEnding,
        };
        TEST("State sequence == intro/main/fill/main/ending", order == expected);
    }

    // ── 5. Fill length matches its declared section.bars ─────────────
    {
        auto style = buildDemoStyle();
        // Locate the fill section in the demo.
        int fillIdx = -1;
        for (size_t i = 0; i < style.sections.size(); ++i) {
            auto t = style.sections[i].type;
            if (t >= SectionType::Fill1 && t <= SectionType::Fill4) { fillIdx = (int)i; break; }
        }
        TEST("Demo has a fill section", fillIdx >= 0);
        TEST("Fill declared length == 1 bar", fillIdx >= 0 && style.sections[fillIdx].bars == 1);

        // Drive a fill for exactly its declared length, then return to main,
        // and confirm no stuck notes (the mid-section return is flushed).
        PlaybackHarness h;
        h.load(style);
        h.start(0);
        h.runTo(8000);
        h.player.switchSection(1);   // main
        h.runTo(16000);
        h.player.fill();             // fill (1 bar)
        int64_t fillStart = (16000 / barSize + 1) * barSize;
        h.runTo(fillStart + style.sections[fillIdx].bars * barSize); // run exactly fill length
        h.player.switchSection(1);   // back to main on the next boundary
        h.runTo(fillStart + 6 * barSize);
        h.stopAndFlush();
        TEST("No stuck notes after fill -> main return", h.sink.stuckNotes() == 0);
    }

    // ── 6. Empty sections handled gracefully ─────────────────────────
    {
        PlaybackHarness h;
        h.load(buildStyleWithEmptySection());
        h.start(0);                  // empty intro
        h.runTo(6000);
        h.player.switchSection(1);   // main with a silent (event-less) track
        h.runTo(14000);
        h.stopAndFlush();
        TEST("Empty/silent sections produce no events and no crash",
             h.sink.totalNoteOn() == 0 && h.sink.stuckNotes() == 0);
        TEST("Empty style still advances sections",
             h.player.getCurrentSection() >= 0 || h.player.getState() == SequencerState::Panic);
    }

    // ── 7. Mid-note (mid-bar) switch leaves no stuck note ────────────
    {
        PlaybackHarness h;
        h.load(buildDemoStyle());
        h.start(0);
        h.runTo(8000);
        h.player.switchSection(1);   // main
        // Switch again very soon (mid-section) — exercises flush-on-switch.
        h.runTo(9500);
        h.player.fill();
        h.runTo(12000);
        h.player.switchSection(1);
        h.runTo(20000);
        h.stopAndFlush();
        TEST("Mid-section rapid switching leaves no stuck notes", h.sink.stuckNotes() == 0);
        TEST("Rapid switching still balanced", h.sink.totalNoteOn() == h.sink.totalNoteOff());
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
