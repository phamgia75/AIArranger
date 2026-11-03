#include "engine/arranger/style_player.h"
#include "engine/music/ntt.h"
#include "engine/trace/latency_trace.h"
#include "engine/uasf/note_closure.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace ai_arranger::arranger {

StylePlayer::StylePlayer(realtime::RealtimeClock& clock,
                          midi::MidiScheduler& scheduler,
                          midi::PanicHandler& panic)
    : clock_(clock)
    , scheduler_(scheduler)
    , panic_handler_(panic) {}

void StylePlayer::loadStyle(const uasf::StyleDefinition& style) noexcept {
    style_ = style;
    // Close notes a section cannot close itself (NoteOff past its end), whatever
    // produced the style: a looping Main would otherwise hold them forever.
    for (auto& sec : style_.sections) uasf::closeOpenNotes(sec);
    style_loaded_ = true;
    sequencer_.setSections(style_.sections.data(), style_.sections.size());
    // Selecting a style (while stopped) adopts its tempo and resolution;
    // start() then keeps whatever tempo the performer has set since. A style
    // swap while playing keeps the running tempo/resolution (no audible jump,
    // no position discontinuity mid-song). Without the resolution, a 1920-PPQN
    // Genos import ran on the engine's default 480-PPQN clock (4x too slow).
    if (!clock_.isRunning()) {
        if (style_.tempo_bpm > 0)  clock_.setTempo(style_.tempo_bpm);
        applyStyleGrid();
    }
    return_main_ = -1;
}

// The clock's PPQN and bar grid must match the loaded style's event ticks.
void StylePlayer::applyStyleGrid() noexcept {
    if (style_.resolution > 0) clock_.setResolution(style_.resolution);
    // Bar grid from the style's meter (first section; styles are one meter).
    if (!style_.sections.empty() && style_.sections[0].beats_per_bar > 0 &&
        style_.sections[0].beat_note > 0)
        clock_.setTimeSignature(style_.sections[0].beats_per_bar,
                                style_.sections[0].beat_note);
}

void StylePlayer::clearStyle() noexcept {
    style_loaded_ = false;
    style_ = {};
}

bool StylePlayer::start(int introSectionIndex) noexcept {
    if (!style_loaded_ || style_.sections.empty()) return false;

    // Find intro section
    if (introSectionIndex < 0 || introSectionIndex >= static_cast<int>(style_.sections.size())) {
        introSectionIndex = 0;
    }

    // A style swapped in while playing kept the old grid (no mid-song jump);
    // a (re)start is the first safe point to adopt its own. Tempo is not
    // touched: Start keeps the performer's tempo.
    if (!clock_.isRunning()) applyStyleGrid();
    clock_.reset();
    clock_.start();

    AIARR_TRACE_SET_TEMPO(clock_.getTempo());
    AIARR_TRACE_SET_SECTION(introSectionIndex);
    AIARR_TRACE_LIFECYCLE(::ai_arranger::trace::LifecycleTag::kStart);

    sequencer_.setCurrentChord(chords::CMaj);
    section_origin_tick_ = 0;
    section_rel_cursor_ = -1;
    return_main_ = -1;

    // Queue intro section
    sequencer_.queueSection(introSectionIndex);

    if (event_cb_) event_cb_("START");

    return true;
}

void StylePlayer::stop() noexcept {
    // Flush any notes still sounding so stopping never leaves stuck notes.
    panic_handler_.flushActiveNotes([this](const uasf::MidiEvent& ev) {
        scheduler_.scheduleEvent(ev);
    });
    clock_.stop();
    sequencer_.panic();
    if (event_cb_) event_cb_("STOP");
}

void StylePlayer::panic() noexcept {
    scheduler_.clear();
    panic_handler_.panic(scheduler_, [this](const uasf::MidiEvent& ev) {
        scheduler_.scheduleEvent(ev);
    });
    sequencer_.panic();
    clock_.stop();
    AIARR_TRACE_COUNT_PANIC();
    AIARR_TRACE_LIFECYCLE(::ai_arranger::trace::LifecycleTag::kPanic);
    if (event_cb_) event_cb_("PANIC");
}

void StylePlayer::intro() noexcept {
    sequencer_.queueIntro();
    if (event_cb_) event_cb_("INTRO_QUEUED");
}

void StylePlayer::fill() noexcept {
    sequencer_.queueFill();
    if (event_cb_) event_cb_("FILL_QUEUED");
}

void StylePlayer::breakSection() noexcept {
    sequencer_.queueBreak();
    if (event_cb_) event_cb_("BREAK_QUEUED");
}

void StylePlayer::ending() noexcept {
    sequencer_.queueEnding();
    if (event_cb_) event_cb_("ENDING_QUEUED");
}

void StylePlayer::switchSection(int index) noexcept {
    sequencer_.queueSection(index);
    if (event_cb_) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SECTION_QUEUED=%d", index);
        event_cb_(buf);
    }
}

void StylePlayer::setChord(Chord chord) noexcept {
    // Flush notes voiced under the previous chord so the re-voicing under the
    // new chord does not leave stuck notes (NoteOff would otherwise transpose
    // to a different pitch than the NoteOn that started the note).
    if (clock_.isRunning()) {
        panic_handler_.flushActiveNotes([this](const uasf::MidiEvent& ev) {
            scheduler_.scheduleEvent(ev);
        });
    }
    chord_input_.setChord(chord);
    sequencer_.setCurrentChord(chord);
    // Trace point (2): chord confirmed after latch/hold, now handed to the
    // arranger. Opens a fresh correlation id linking this chord's downstream
    // accompaniment → enqueue → send waypoints. Compiled out when the gate is off.
    AIARR_TRACE_CHORD_CONFIRMED(chord.root, static_cast<uint8_t>(chord.type));
    if (event_cb_) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "CHORD=%d,%d", static_cast<int>(chord.type), chord.root);
        event_cb_(buf);
    }
}

Chord StylePlayer::getCurrentChord() const noexcept {
    return sequencer_.getCurrentChord();
}

void StylePlayer::tick() noexcept {
    if (!style_loaded_ || !clock_.isRunning()) return;

    int64_t currentTick = clock_.getPosition();
    int64_t barSize = clock_.ticksPerBar();

    // Advance section sequencer (check bar boundary)
    bool sectionSwitched = sequencer_.advance(currentTick, barSize);
    if (sectionSwitched) {
        // Stop any notes left sounding from the previous section before the
        // new section begins, so a mid-note switch never leaves stuck notes.
        panic_handler_.flushActiveNotes([this](const uasf::MidiEvent& ev) {
            scheduler_.scheduleEvent(ev);
        });
        // Anchor the section on the bar line it was committed at, not on this
        // engine tick (which lands up to one tick period later): keeps every
        // section — and its end / loop boundary — on the bar grid. Events
        // between the bar line and now fire in this same tick.
        section_origin_tick_ = sequencer_.lastBarStart();
        section_rel_cursor_ = -1;
        AIARR_TRACE_SET_SECTION(sequencer_.getCurrentSectionIndex());
        AIARR_TRACE_LIFECYCLE(::ai_arranger::trace::LifecycleTag::kSectionSwitch);
    }

    // End of the current section: loop Main, hand Intro/Fill/Break back to
    // Main, stop after Ending (arranger-engine-spec §5).
    if (!handleSectionEnd(currentTick)) return;

    // Get current section
    int sectionIdx = sequencer_.getCurrentSectionIndex();
    if (sectionIdx < 0 || sectionIdx >= static_cast<int>(style_.sections.size())) return;

    const auto& section = style_.sections[sectionIdx];
    if (section.type >= uasf::SectionType::Main1 && section.type <= uasf::SectionType::Main4)
        return_main_ = sectionIdx;
    Chord currentChord = sequencer_.getCurrentChord();

    // Dispatch events for this section
    dispatchSectionEvents(section, currentTick, currentChord);
}

int StylePlayer::defaultMainIndex() const noexcept {
    if (return_main_ >= 0 && return_main_ < static_cast<int>(style_.sections.size()))
        return return_main_;
    for (size_t i = 0; i < style_.sections.size(); ++i) {
        const auto t = style_.sections[i].type;
        if (t >= uasf::SectionType::Main1 && t <= uasf::SectionType::Main4)
            return static_cast<int>(i);
    }
    return -1;
}

bool StylePlayer::handleSectionEnd(int64_t currentTick) noexcept {
    const int idx = sequencer_.getCurrentSectionIndex();
    if (idx < 0 || idx >= static_cast<int>(style_.sections.size())) return true;
    const auto& section = style_.sections[idx];

    // Section length on the sequencer's bar grid. bars == 0 means "length
    // unknown": keep the legacy behaviour (play once, switch on command only).
    const int64_t len = static_cast<int64_t>(section.bars) * clock_.ticksPerBar();
    if (len <= 0 || currentTick - section_origin_tick_ < len) return true;

    // Fire what is left of this pass. A NoteOff exactly at the section end
    // closes this pass; anything else at that tick is the next pass's tick 0.
    dispatchSectionEvents(section, section_origin_tick_ + len, sequencer_.getCurrentChord(),
                          /*noteOffOnlyFrom=*/len);
    const int64_t boundary = section_origin_tick_ + len;
    const auto type = section.type;

    if (type >= uasf::SectionType::Ending1 && type <= uasf::SectionType::Ending3) {
        stop();                          // Ending finished -> STOPPED (flushes notes)
        return false;
    }

    int next = idx;                      // Main (and anything unclassified) loops
    const bool handsBack =
        (type >= uasf::SectionType::Intro1 && type <= uasf::SectionType::Intro3) ||
        (type >= uasf::SectionType::Fill1  && type <= uasf::SectionType::Fill4)  ||
        type == uasf::SectionType::Break;
    if (handsBack) {
        const int main = defaultMainIndex();
        if (main >= 0) next = main;
    }

    if (next != idx) {
        panic_handler_.flushActiveNotes([this](const uasf::MidiEvent& ev) {
            scheduler_.scheduleEvent(ev);
        });
        sequencer_.commitSection(next);
        AIARR_TRACE_SET_SECTION(next);
        AIARR_TRACE_LIFECYCLE(::ai_arranger::trace::LifecycleTag::kSectionSwitch);
    }
    // Re-anchor exactly on the boundary (no drift across loops) and dispatch the
    // new pass from its tick 0 in this same tick.
    section_origin_tick_ = boundary;
    section_rel_cursor_ = -1;
    return true;
}

bool StylePlayer::isPlaying() const noexcept {
    return clock_.isRunning();
}

int StylePlayer::getCurrentSection() const noexcept {
    return sequencer_.getCurrentSectionIndex();
}

const uasf::SectionDefinition* StylePlayer::getCurrentSectionDef() const noexcept {
    int idx = sequencer_.getCurrentSectionIndex();
    if (idx < 0 || idx >= static_cast<int>(style_.sections.size())) return nullptr;
    return &style_.sections[idx];
}

SequencerState StylePlayer::getState() const noexcept {
    return sequencer_.getState();
}

void StylePlayer::setEventCallback(PlaybackEventCallback cb) noexcept {
    event_cb_ = std::move(cb);
}

void StylePlayer::setArticulationRenderer(
        const articulation::IArticulationRenderer& r) noexcept {
    renderer_ = &r;
}

void StylePlayer::setSwing(uint8_t swingPercent) noexcept {
    if (swingPercent > music::kMaxSwing) swingPercent = music::kMaxSwing;
    swing_percent_.store(swingPercent, std::memory_order_relaxed);
}

uint8_t StylePlayer::getSwing() const noexcept {
    return swing_percent_.load(std::memory_order_relaxed);
}

namespace {
// Realtime-safe sink: forwards rendered events straight to the scheduler.
// Stack-constructed per dispatch pass (tiny, no heap).
struct SchedulerSink final : articulation::EventSink {
    midi::MidiScheduler& scheduler;
    explicit SchedulerSink(midi::MidiScheduler& s) noexcept : scheduler(s) {}
    void emit(const uasf::MidiEvent& ev) noexcept override {
        scheduler.scheduleEvent(ev);
    }
};
} // namespace

void StylePlayer::dispatchSectionEvents(const uasf::SectionDefinition& section,
                                          int64_t currentTick,
                                          Chord chord,
                                          int64_t noteOffOnlyFrom) noexcept {
    // Monotonic cursor: dispatch every event whose section-relative tick falls
    // in (section_rel_cursor_, rel]. Each event fires exactly once as playback
    // time advances, so NoteOn/NoteOff pairs stay balanced (no stuck notes).
    const int64_t rel = currentTick - section_origin_tick_;
    if (rel < 0 || rel <= section_rel_cursor_) return;

    SchedulerSink sink(scheduler_);
    const uint8_t swing = swing_percent_.load(std::memory_order_relaxed);

    for (const auto& track : section.tracks) {
        for (const auto& event : track.events) {
            const int64_t et = static_cast<int64_t>(event.tick);
            if (et <= section_rel_cursor_ || et > rel) continue;
            if (et >= noteOffOnlyFrom &&
                !(event.type == uasf::MidiEventType::NoteOff ||
                  (event.type == uasf::MidiEventType::NoteOn && event.data2 == 0)))
                continue;   // a NoteOn at the very end starts the next pass

            uasf::MidiEvent dispatched = event;
            if (dispatched.type == uasf::MidiEventType::NoteOn ||
                dispatched.type == uasf::MidiEventType::NoteOff) {
                dispatched.data1 = transposeNote(dispatched.data1, chord, track.role);
            }
            // Absolute tick = section origin + event's in-section position,
            // with swing applied so off-beat notes slide toward a shuffle feel
            // (Task B). NoteOn/NoteOff in the same off-beat half shift equally,
            // so pairs stay balanced. ticks-per-beat = section resolution.
            uint64_t absTick = static_cast<uint64_t>(section_origin_tick_ + et);
            if (swing > music::kStraightSwing) {
                absTick = music::applySwing(absTick, section.resolution, swing);
            }
            dispatched.tick = absTick;

            // Track active notes so section switch / stop / chord change can
            // flush them and keep on/off balanced. Dedupe against the active
            // set: chord transposition can voice two source notes onto the
            // same MIDI note, so suppress redundant retriggers and orphan
            // note-offs to keep one NoteOn/NoteOff pair per sounding note.
            const bool isNoteOn  = dispatched.type == uasf::MidiEventType::NoteOn &&
                                   dispatched.data2 > 0;
            const bool isNoteOff = dispatched.type == uasf::MidiEventType::NoteOff ||
                                   (dispatched.type == uasf::MidiEventType::NoteOn &&
                                    dispatched.data2 == 0);
            if (isNoteOn) {
                if (panic_handler_.isNoteActive(dispatched.channel, dispatched.data1)) {
                    continue; // already sounding — skip redundant retrigger
                }
                panic_handler_.noteOn(dispatched.channel, dispatched.data1);
                AIARR_TRACE_NOTE_ON();
            } else if (isNoteOff) {
                if (!panic_handler_.isNoteActive(dispatched.channel, dispatched.data1)) {
                    continue; // nothing to turn off — skip orphan/duplicate
                }
                panic_handler_.noteOff(dispatched.channel, dispatched.data1);
                AIARR_TRACE_NOTE_OFF();
            }

            // Trace point (3): the FIRST accompaniment event produced after the
            // current chord confirm. markAccompFirst() dedupes to one record per
            // chord, so this fires on every event yet only records the first.
            AIARR_TRACE_ACCOMP_FIRST(dispatched.channel, dispatched.data1,
                                     dispatched.data2);

            // Route through the articulation render strategy (Task C). The
            // default naive renderer emits exactly this one event, preserving
            // the previous behaviour; a keyswitch renderer may inject the
            // articulation-select note ahead of the main note.
            renderer_->render(dispatched, track.articulation, sink);
        }
    }

    section_rel_cursor_ = rel;
}

uint8_t StylePlayer::transposeNote(uint8_t note, Chord chord, uasf::TrackRole role) const noexcept {
    // Full NTR/NTT transposition (Gate 10 Task B). The crude root/fifth rule is
    // replaced by the table-driven transform in engine/music/ntt. The rule pair
    // is derived from the track role, which round-trips through the UASF format
    // (the raw CASM NTR/NTT integers do not), so the transform stays correct for
    // a deserialised style. Drums map to Bypass and are left untouched.
    music::NtrRule ntr;
    music::NttMode ntt;
    music::defaultRuleForRole(role, ntr, ntt);
    if (ntr == music::NtrRule::Bypass || ntt == music::NttMode::Bypass) return note;
    if (chord.type == ChordType::NoChord) return note;
    return music::transpose(note, chord, ntr, ntt);
}

// ── Demo style builder ─────────────────────────────────────────────

static uasf::MidiEvent makeEvent(uasf::MidiEventType type, uint8_t ch,
                                  uint8_t d1, uint8_t d2, uint64_t tick) {
    uasf::MidiEvent ev;
    ev.type = type;
    ev.channel = ch;
    ev.data1 = d1;
    ev.data2 = d2;
    ev.tick = tick;
    return ev;
}

static uasf::TrackDefinition makeDrumTrack(uint64_t tickOffset) {
    uasf::TrackDefinition track;
    track.name = "Drums";
    track.midi_channel = 9;
    track.role = uasf::TrackRole::Drum;
    track.is_drum = true;

    // Basic rock beat: kick (36) on 1&3, snare (38) on 2&4, hihat (42) every 8th
    for (int bar = 0; bar < 4; ++bar) {
        int64_t barStart = bar * 1920; // 4 beats * 480 ticks
        // Kick: beat 1, 3
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 9, 36, 100, tickOffset + barStart));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 9, 36, 0, tickOffset + barStart + 240));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 9, 36, 100, tickOffset + barStart + 960));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 9, 36, 0, tickOffset + barStart + 1200));
        // Snare: beat 2, 4
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 9, 38, 90, tickOffset + barStart + 480));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 9, 38, 0, tickOffset + barStart + 720));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 9, 38, 90, tickOffset + barStart + 1440));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 9, 38, 0, tickOffset + barStart + 1680));
        // Hi-hat: every 8th note
        for (int eighth = 0; eighth < 8; ++eighth) {
            int64_t hihat = tickOffset + barStart + eighth * 240;
            track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 9, 42, 80, hihat));
            track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 9, 42, 0, hihat + 120));
        }
    }
    return track;
}

static uasf::TrackDefinition makeBassTrack(uint64_t tickOffset) {
    uasf::TrackDefinition track;
    track.name = "Bass";
    track.midi_channel = 1;
    track.role = uasf::TrackRole::Bass;
    track.is_drum = false;

    // Simple bass line: root on beat 1, 5th on beat 3
    for (int bar = 0; bar < 4; ++bar) {
        int64_t barStart = bar * 1920;
        // Root note (C3 = 48) on beat 1
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 1, 48, 90, tickOffset + barStart));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 1, 48, 0, tickOffset + barStart + 480));
        // 5th (G3 = 55) on beat 3
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 1, 55, 80, tickOffset + barStart + 960));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 1, 55, 0, tickOffset + barStart + 1440));
    }
    return track;
}

static uasf::TrackDefinition makeChordTrack(uint64_t tickOffset) {
    uasf::TrackDefinition track;
    track.name = "Chord";
    track.midi_channel = 2;
    track.role = uasf::TrackRole::Chord;
    track.is_drum = false;

    // Simple held chords: C Major (60, 64, 67) held for 2 bars
    for (int bar = 0; bar < 4; ++bar) {
        int64_t barStart = bar * 1920;
        // Chord voicing (transposed by style player based on chord)
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 2, 60, 70, tickOffset + barStart));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 2, 64, 70, tickOffset + barStart));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOn, 2, 67, 70, tickOffset + barStart));
        // Hold for 2 beats
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 2, 60, 0, tickOffset + barStart + 960));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 2, 64, 0, tickOffset + barStart + 960));
        track.events.push_back(makeEvent(uasf::MidiEventType::NoteOff, 2, 67, 0, tickOffset + barStart + 960));
    }
    return track;
}

uasf::StyleDefinition buildDemoStyle() noexcept {
    uasf::StyleDefinition style;
    style.name = "Gate 2 Demo Style (Rock)";
    style.format_version = "1.0";
    style.tempo_bpm = 120;
    style.resolution = 480;

    // Intro section (4 bars)
    uasf::SectionDefinition intro;
    intro.type = uasf::SectionType::Intro1;
    intro.name = "Intro";
    intro.bars = 4;
    intro.resolution = 480;
    intro.beats_per_bar = 4;
    intro.beat_note = 4;
    intro.tracks.push_back(makeDrumTrack(0));
    intro.tracks.push_back(makeBassTrack(0));
    style.sections.push_back(std::move(intro));

    // Main section (4 bars)
    uasf::SectionDefinition main;
    main.type = uasf::SectionType::Main1;
    main.name = "Main";
    main.bars = 4;
    main.resolution = 480;
    main.beats_per_bar = 4;
    main.beat_note = 4;
    main.tracks.push_back(makeDrumTrack(0));
    main.tracks.push_back(makeBassTrack(0));
    main.tracks.push_back(makeChordTrack(0));
    style.sections.push_back(std::move(main));

    // Fill section (1 bar)
    uasf::SectionDefinition fill;
    fill.type = uasf::SectionType::Fill1;
    fill.name = "Fill";
    fill.bars = 1;
    fill.resolution = 480;
    fill.beats_per_bar = 4;
    fill.beat_note = 4;
    fill.tracks.push_back(makeDrumTrack(0));
    style.sections.push_back(std::move(fill));

    // Ending section (2 bars)
    uasf::SectionDefinition ending;
    ending.type = uasf::SectionType::Ending1;
    ending.name = "Ending";
    ending.bars = 2;
    ending.resolution = 480;
    ending.beats_per_bar = 4;
    ending.beat_note = 4;
    ending.tracks.push_back(makeDrumTrack(0));
    ending.tracks.push_back(makeBassTrack(0));
    ending.tracks.push_back(makeChordTrack(0));
    style.sections.push_back(std::move(ending));

    return style;
}

} // namespace ai_arranger::arranger
