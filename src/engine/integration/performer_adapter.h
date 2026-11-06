#ifndef AI_ARRANGER_INTEGRATION_PERFORMER_ADAPTER_H
#define AI_ARRANGER_INTEGRATION_PERFORMER_ADAPTER_H

#include "engine/realtime/clock.h"
#include "engine/arranger/style_player.h"
#include "engine/control/control_surface.h"
#include "engine/control/control_events.h"
#include "engine/performance/sync_controller.h"
#include "engine/performance/fill_scheduler.h"
#include "engine/performance/variation_manager.h"
#include "engine/performance/split_router.h"
#include "engine/performance/realtime_state_machine.h"
#include "engine/chord/chord_scan_mode.h"
#include <cstdint>

// ── Performer integration adapter (Gate 13) ──────────────────────────
//
// Bridges the Gate 12 performer modules to the real StylePlayer playback path.
// Stateless translator: it OWNS the performer modules (control intent) but holds
// NO playback state — StylePlayer + SectionSequencer remain the playback/bar
// authority. The adapter forwards collapsed intent to existing StylePlayer hooks
// (which bar-quantize via the sequencer); it never bypasses the scheduler and
// makes no engine-core behaviour change. Lock-free: control events arrive via a
// SPSC queue drained inside tick().

namespace ai_arranger::integration {

class PerformerAdapter {
public:
    PerformerAdapter(realtime::RealtimeClock& clock,
                     arranger::StylePlayer& player) noexcept;

    // ── Producer side (control thread / front-end) ─────────────────
    // Post a control event (lock-free). Returns false if the queue is full.
    bool postEvent(const control::ControlEvent& ev) noexcept;

    // Map Variation A/B/C/D to section indices in the loaded style.
    void setVariationSections(int a, int b, int c, int d) noexcept;

    // Select the active chord-scan mode (caller owns the instance).
    void setChordScanMode(const chord::IChordScanMode* mode) noexcept { scan_ = mode; }
    void setSplitPoint(uint8_t note) noexcept { split_.setSplitPoint(note); }
    void setManualBass(bool on) noexcept { split_.setManualBass(on); }

    // Held-note input (lower zone feeds chord detection; upper zone ignored for
    // chord). Re-runs detection and pushes the chord to StylePlayer.
    // These apply immediately on the CALLER thread — safe to call directly from
    // the engine thread or a test. For asynchronous input (a MIDI read thread),
    // post ControlAction::NoteOn/NoteOff/Sustain instead so the change is drained
    // lock-free inside tick() rather than racing the engine thread.
    // bypassSplit: the note feeds chord detection whatever the split point.
    void noteOn(uint8_t note, bool bypassSplit = false) noexcept;
    void noteOff(uint8_t note) noexcept;

    // Sustain pedal (CC64). While down, physical note-offs are deferred: the
    // released notes stay in the chord set until the pedal is lifted, then all
    // pending releases are applied at once and detection re-runs.
    void setSustain(bool on) noexcept;
    bool sustain() const noexcept { return sustain_; }

    // Chord latch / hold behaviour (chord-engine-spec v0.9 §6).
    //  - Hold mode ON (default): releasing all keys keeps the last chord playing
    //    forever (arranger "chord memory"); the latch is irrelevant.
    //  - Hold mode OFF: releasing all keys arms a ~latchMs debounce; if the keys
    //    stay released past the window the chord clears to NoChord, but a re-press
    //    within the window keeps it (no flicker during re-fingering).
    void setHoldMode(bool on) noexcept { hold_mode_ = on; }
    bool holdMode() const noexcept { return hold_mode_; }
    void setChordLatchMs(uint32_t ms) noexcept { latch_ms_ = ms; }
    uint32_t chordLatchMs() const noexcept { return latch_ms_; }

    // ── Realtime tick: drain control events, drive the player ──────
    void tick() noexcept;

    // Forget physical input state (held lower-zone keys, sustain, deferred
    // releases, pending chord-release latch, last chord). For an engine stop:
    // input is not observed while stopped, so restart must not trust it.
    void resetInput() noexcept;

    // ── Observability (testing) ────────────────────────────────────
    performance::PerformerState state() const noexcept { return sm_.state(); }
    bool isPlaying() const noexcept { return player_.isPlaying(); }
    performance::SyncController&  sync() noexcept { return sync_; }
    performance::FillScheduler&   fill() noexcept { return fill_; }
    performance::VariationManager& variation() noexcept { return var_; }
    performance::SplitRouter&     split() noexcept { return split_; }
    arranger::Chord lastChord() const noexcept { return last_chord_; }
    int heldCount() const noexcept { return held_count_; }

private:
    void handleControl(const control::ControlEvent& ev) noexcept;
    void startPlayback() noexcept;
    void doPanic() noexcept;
    void redetectChord() noexcept;
    bool removeHeld(uint8_t note) noexcept;   // returns true if it was held
    void rememberSustained(uint8_t note) noexcept;
    void forgetSustained(uint8_t note) noexcept;

    realtime::RealtimeClock& clock_;
    arranger::StylePlayer&   player_;

    control::ControlEventQueue<256> queue_;
    performance::SyncController     sync_;
    performance::FillScheduler      fill_;
    performance::VariationManager   var_;
    performance::SplitRouter        split_;
    performance::RealtimeStateMachine sm_;

    const chord::IChordScanMode* scan_{nullptr};
    arranger::Chord last_chord_{arranger::ChordType::NoChord, 0, 0, 0};

    int  var_section_[4]{0, 1, 2, 3};

    // Held lower-zone notes (sorted ascending), fixed buffer (RT-safe).
    static constexpr int kMaxHeld = 16;
    uint8_t held_[kMaxHeld]{};
    int     held_count_{0};

    // Sustain pedal: notes physically released while the pedal is down are kept
    // in held_ and remembered here until the pedal lifts. Fixed buffer, RT-safe.
    bool    sustain_{false};
    uint8_t sustained_[kMaxHeld]{};
    int     sustained_count_{0};

    // Chord latch (hold-mode OFF only): a pending clear-to-NoChord armed on
    // release, committed in tick() once the clock passes the deadline.
    bool     hold_mode_{true};       // default: keep last chord (arranger memory)
    uint32_t latch_ms_{80};          // release debounce window
    bool     latch_pending_{false};
    int64_t  latch_deadline_samples_{0};
};

} // namespace ai_arranger::integration

#endif // AI_ARRANGER_INTEGRATION_PERFORMER_ADAPTER_H
