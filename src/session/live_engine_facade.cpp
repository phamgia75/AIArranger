#include "session/live_engine_facade.h"

#include "control/engine_midi_route.h"   // attachEngineOutput / pumpEngineOutput
#include "midi/midi_input_router.h"      // routeMidiInput

namespace ai_arranger::session {

namespace {
// Adapts the lock-free SPSC input queue to the router's `postEvent` sink
// contract. A full queue (the tick thread stalled for >255 input events) drops
// the event on the read thread, so it is surfaced as QueueFull, not lost silently.
struct QueueSink {
    control::UiEventQueue<control::ControlEvent, 256>& q;
    std::atomic<EngineError>& lastError;
    bool postEvent(const control::ControlEvent& e) noexcept {
        if (q.push(e)) return true;
        lastError.store(EngineError::QueueFull, std::memory_order_relaxed);
        return false;
    }
};
} // namespace

LiveEngineFacade::LiveEngineFacade(midi::IMidiInputSource* input,
                                   midi::IMidiOutputProvider* output) noexcept
    : input_(input), output_(output) {}

void LiveEngineFacade::start() noexcept {
    if (started_) return;
    session_.boot();
    // Engine scheduler output -> lock-free bridge (no Apple framework here).
    control::attachEngineOutput(session_.scheduler(), out_bridge_);
    // MIDI input read thread -> route -> input_q_ (drained in tick()).
    if (input_) {
        input_->setSink([this](const midi::MidiInputMessage& m) {
            QueueSink sink{input_q_, last_error_};
            midi::routeMidiInput(m, sink);
        });
    }
    started_ = true;
    lifecycle_.apply(LifecycleEvent::Start);   // Gate 4: track high-level state
    publishSnapshot();
}

void LiveEngineFacade::stop() noexcept {
    if (!started_) return;
    // Flush the input sink so no late callback races teardown.
    if (input_) input_->setSink(nullptr);
    session_.shutdown();   // stops playback + flushes all notes (no hanging notes)
    started_ = false;
    lifecycle_.apply(LifecycleEvent::Stop);    // Gate 4: track high-level state
    publishSnapshot();
}

void LiveEngineFacade::setVariation(int index) noexcept {
    if (index < 0 || index > 3) return;
    const auto action = static_cast<control::ControlAction>(
        static_cast<int>(control::ControlAction::VariationA) + index);
    push(action);
}

void LiveEngineFacade::tick(uint32_t numSamples) noexcept {
    // Apply a pending tempo change (UI thread set it atomically).
    if (uint32_t bpm = pending_tempo_.exchange(0, std::memory_order_relaxed))
        session_.clock().setTempo(bpm);

    // Drain UI commands then MIDI input into the engine. This thread is the SOLE
    // producer of the adapter's SPSC control queue, which is the same depth as
    // each facade queue: a burst on both in one tick cannot all fit, so the drain
    // applies back-pressure instead of discarding (a lost NoteOff = stuck note).
    drainToAdapter([this](control::ControlEvent& e) noexcept { return cmd_q_.try_pop(e); },
                   cmd_carry_, has_cmd_carry_);
    drainToAdapter([this](control::ControlEvent& e) noexcept { return input_q_.pop(e); },
                   input_carry_, has_input_carry_);

    // Advance the engine and schedule output (mirrors the macOS EngineDriver).
    session_.clock().advance(numSamples);
    session_.tick();
    session_.scheduler().advanceTo(session_.clock().getPosition());
    if (output_) control::pumpEngineOutput(out_bridge_, *output_);

    publishSnapshot(/*fromTick=*/true);
}

template <class PopFn>
void LiveEngineFacade::drainToAdapter(PopFn&& pop, control::ControlEvent& carry,
                                      bool& hasCarry) noexcept {
    auto& adapter = session_.adapter();
    if (hasCarry) {
        if (!adapter.postEvent(carry)) return;   // engine queue still full
        hasCarry = false;
    }
    control::ControlEvent e;
    while (pop(e)) {
        if (!adapter.postEvent(e)) {             // full: keep it for next tick
            carry = e;
            hasCarry = true;
            return;
        }
    }
}

void LiveEngineFacade::publishSnapshot(bool fromTick) noexcept {
    EngineSnapshot s;
    const auto chord = session_.player().getCurrentChord();
    s.playing        = session_.player().isPlaying();
    s.chordRoot      = chord.root;
    s.chordType      = static_cast<uint8_t>(chord.type);
    s.chordBass      = chord.bass;
    s.section        = static_cast<int32_t>(session_.player().getCurrentSection());
    s.tempoBpm       = session_.clock().getTempo();
    s.positionTicks  = session_.clock().getPosition();
    s.performerState = static_cast<int32_t>(session_.adapter().state());
    s.lifecycleState = static_cast<int32_t>(lifecycle_.state());
    s.midiInLive     = input_  ? input_->hasLiveSource() : false;
    s.midiOutLive    = output_ ? output_->hasLiveDestination() : false;
    s.receivedMessages = input_  ? input_->receivedCount() : 0;
    s.dispatchedNotes  = output_ ? output_->dispatchedCount() : 0;
    // A skipped tick publish (lifecycle publish in flight) is republished on
    // the next tick; the lifecycle path waits so its state change is visible.
    if (fromTick) snapshot_.tryStore(s);
    else          snapshot_.store(s);
}

} // namespace ai_arranger::session
