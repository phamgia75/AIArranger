#include "engine/integration/performer_adapter.h"

namespace ai_arranger::integration {

using control::ControlAction;
using performance::Variation;

PerformerAdapter::PerformerAdapter(realtime::RealtimeClock& clock,
                                   arranger::StylePlayer& player) noexcept
    : clock_(clock), player_(player) {}

bool PerformerAdapter::postEvent(const control::ControlEvent& ev) noexcept {
    return queue_.push(ev);
}

void PerformerAdapter::setVariationSections(int a, int b, int c, int d) noexcept {
    var_section_[0] = a; var_section_[1] = b; var_section_[2] = c; var_section_[3] = d;
}

void PerformerAdapter::startPlayback() noexcept {
    sync_.start();
    sm_.apply(performance::PerformerInput::Start);
    player_.start(0);   // StylePlayer also starts the clock
}

void PerformerAdapter::doPanic() noexcept {
    // Reuse the engine panic (single PanicHandler owner) and reset performer
    // module state so the layer is immediately re-armable.
    player_.panic();
    sync_.reset();
    fill_.reset();
    var_.reset();
    sm_.apply(performance::PerformerInput::Panic);
}

void PerformerAdapter::handleControl(const control::ControlEvent& ev) noexcept {
    switch (ev.action) {
        case ControlAction::SyncArm:
            sync_.arm();
            sm_.apply(performance::PerformerInput::Arm);
            break;
        case ControlAction::Start:
            startPlayback();
            break;
        case ControlAction::Stop:
            sync_.requestStop();
            sm_.apply(performance::PerformerInput::Stop);
            player_.stop();
            sync_.notifyStopped();
            sm_.apply(performance::PerformerInput::Stopped);
            break;
        case ControlAction::Fill:
            // Collapse via the module, then forward to the sequencer (bar-quantized).
            fill_.queueFill(ev.param);
            player_.fill();
            break;
        case ControlAction::VariationA:
        case ControlAction::VariationB:
        case ControlAction::VariationC:
        case ControlAction::VariationD: {
            const int idx = static_cast<int>(ev.action) -
                            static_cast<int>(ControlAction::VariationA);
            var_.queue(static_cast<Variation>(idx));
            player_.switchSection(var_section_[idx]);  // sequencer quantizes
            break;
        }
        case ControlAction::Intro:
            player_.intro();   // sequencer switches at next bar boundary
            break;
        case ControlAction::Break:
            player_.breakSection();
            break;
        case ControlAction::Ending:
            player_.ending();
            break;
        case ControlAction::Chord: {
            // UI chord intent decoded on the engine thread (thread-safe via the
            // bridge): param = root | (type << 8).
            arranger::Chord c;
            c.root = static_cast<uint8_t>(ev.param & 0xFF);
            c.type = static_cast<arranger::ChordType>((ev.param >> 8) & 0xFF);
            c.bass = 0; c.octave = 0;
            last_chord_ = c;
            if (c.type != arranger::ChordType::NoChord) {
                if (sync_.isArmed() && sync_.onChordPresent()) {
                    sm_.apply(performance::PerformerInput::ChordDetected);
                    player_.start(0);
                }
                player_.setChord(c);
            }
            break;
        }
        case ControlAction::NoteOn:
            noteOn(static_cast<uint8_t>(ev.param & 0x7F));
            break;
        case ControlAction::NoteOff:
            noteOff(static_cast<uint8_t>(ev.param & 0x7F));
            break;
        case ControlAction::Sustain:
            setSustain(ev.param != 0);
            break;
        case ControlAction::Panic:
            doPanic();
            break;
        default:
            break;  // None / Tap: no-op here
    }
}

void PerformerAdapter::tick() noexcept {
    // Drain pending control events (lock-free).
    control::ControlEvent ev;
    while (queue_.pop(ev)) handleControl(ev);

    // Chord release latch (hold-mode OFF): once the keys have stayed released
    // past the debounce window, clear the chord to NoChord (a re-press before the
    // deadline cancelled it in redetectChord()).
    if (latch_pending_ &&
        clock_.getSamplePosition() >= latch_deadline_samples_) {
        latch_pending_ = false;
        last_chord_ = arranger::Chord{arranger::ChordType::NoChord, 0, 0, 0};
        player_.setChord(last_chord_);
    }

    // Keep performer module bookkeeping coherent with the sequencer's own
    // bar-boundary resolution (clears pending intent; sequencer is the authority).
    fill_.onBarBoundary();
    var_.onBarBoundary();

    player_.tick();

    // The transport can stop by itself (an Ending that played out). Mirror the
    // Stop command's FSM inputs so the performer state does not stay Playing
    // while nothing plays. Armed (sync-start waiting) is not playing yet: untouched.
    if (!player_.isPlaying()) {
        const auto st = sm_.state();
        if (st == performance::PerformerState::Playing ||
            st == performance::PerformerState::PendingTransition) {
            sync_.requestStop();
            sm_.apply(performance::PerformerInput::Stop);
            sync_.notifyStopped();
            sm_.apply(performance::PerformerInput::Stopped);
        }
    }
}

void PerformerAdapter::noteOn(uint8_t note) noexcept {
    if (split_.zoneOf(note) != performance::Zone::Lower) return; // upper zone: ignore for chord
    // A re-press cancels any pending sustain-hold release for this note.
    forgetSustained(note);
    if (held_count_ >= kMaxHeld) return;
    // insert keeping ascending order, dedupe
    int i = held_count_;
    while (i > 0 && held_[i - 1] > note) { held_[i] = held_[i - 1]; --i; }
    if (i < held_count_ && held_[i] == note) return; // already held (shifted back)
    held_[i] = note;
    ++held_count_;
    redetectChord();
}

void PerformerAdapter::noteOff(uint8_t note) noexcept {
    // Sustain pedal down: keep the note sounding (in the chord set) and defer the
    // actual release until the pedal lifts. Only defer notes that are truly held.
    if (sustain_) {
        for (int i = 0; i < held_count_; ++i) {
            if (held_[i] == note) { rememberSustained(note); return; }
        }
        return; // not held (e.g. upper zone) — nothing to defer
    }
    if (removeHeld(note)) redetectChord();
}

void PerformerAdapter::setSustain(bool on) noexcept {
    if (sustain_ == on) return;
    sustain_ = on;
    if (on) return;
    // Pedal lifted: apply every deferred release at once, then re-detect.
    bool changed = false;
    for (int i = 0; i < sustained_count_; ++i) changed |= removeHeld(sustained_[i]);
    sustained_count_ = 0;
    if (changed) redetectChord();
}

bool PerformerAdapter::removeHeld(uint8_t note) noexcept {
    int idx = -1;
    for (int i = 0; i < held_count_; ++i) if (held_[i] == note) { idx = i; break; }
    if (idx < 0) return false;
    for (int i = idx; i + 1 < held_count_; ++i) held_[i] = held_[i + 1];
    --held_count_;
    return true;
}

void PerformerAdapter::rememberSustained(uint8_t note) noexcept {
    for (int i = 0; i < sustained_count_; ++i) if (sustained_[i] == note) return; // dedupe
    if (sustained_count_ >= kMaxHeld) return;
    sustained_[sustained_count_++] = note;
}

void PerformerAdapter::forgetSustained(uint8_t note) noexcept {
    for (int i = 0; i < sustained_count_; ++i) {
        if (sustained_[i] == note) {
            for (int j = i; j + 1 < sustained_count_; ++j) sustained_[j] = sustained_[j + 1];
            --sustained_count_;
            return;
        }
    }
}

void PerformerAdapter::redetectChord() noexcept {
    if (!scan_) return;
    arranger::Chord c{arranger::ChordType::NoChord, 0, 0, 0};
    if (held_count_ > 0) {
        c = scan_->detect(held_, static_cast<size_t>(held_count_));
        // Manual-bass override: the lowest held lower-zone note becomes the chord
        // bass (slash). Routing only — no engine-core change.
        const uint8_t mb = split_.manualBassNote(held_, static_cast<size_t>(held_count_));
        if (mb != performance::kNoManualBass) c.bass = mb;
    }

    if (c.type != arranger::ChordType::NoChord) {
        // A valid chord: apply immediately and cancel any pending release latch.
        latch_pending_ = false;
        last_chord_ = c;
        // Sync start: first chord while armed fires playback.
        if (sync_.isArmed() && sync_.onChordPresent()) {
            sm_.apply(performance::PerformerInput::ChordDetected);
            player_.start(0);
        }
        player_.setChord(c);
        return;
    }

    // No valid chord (all keys released, or an unrecognised transient set).
    if (hold_mode_) return;                 // keep the last chord (arranger memory)
    if (!latch_pending_) {                   // arm the release debounce (hold OFF)
        latch_pending_ = true;
        latch_deadline_samples_ = clock_.getSamplePosition() +
            static_cast<int64_t>(clock_.getSampleRate()) * static_cast<int64_t>(latch_ms_) / 1000;
    }
}

} // namespace ai_arranger::integration
