#include "session/live_engine_facade.h"
#include <thread>

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
    EngineGuard guard(engine_busy_);   // tick() skips its step meanwhile (#10)
    if (started_) return;
    session_.boot();
    // Engine scheduler output -> lock-free bridge (no Apple framework here).
    control::attachEngineOutput(session_.scheduler(), out_bridge_);
    // MIDI input read thread -> route -> input_q_ (drained in tick()).
    if (input_) {
        input_->setSink([this](const midi::MidiInputMessage& m) {
            QueueSink sink{input_q_, last_error_};
            midi::routeMidiInput(m, sink, inputChannelRoles());
        });
    }
    started_ = true;
    lifecycle_.apply(LifecycleEvent::Start);   // Gate 4: track high-level state
    publishSnapshot();
}

void LiveEngineFacade::stop() noexcept {
    EngineGuard guard(engine_busy_);   // tick() skips its step meanwhile (#10)
    if (!started_) return;
    // Flush the input sink so no late callback races teardown.
    if (input_) input_->setSink(nullptr);
    // Shut down first: its panic clears the scheduler, then queues a NoteOff for
    // every note still tracked + CC 120/123 (#14). (A player stop() before it
    // would clear the tracking and its NoteOffs would be discarded by the panic.)
    session_.shutdown();
    // Then a real transport Stop through the performer (FSM -> Idle, Sync Start
    // disarmed), so no input after stop() can fire a stale arm. Nothing is left
    // sounding for it to flush.
    session_.adapter().postEvent({control::ControlAction::Stop, 0, 0});
    session_.tick();
    // Deliver the silence now rather than on a later tick (a host may stop
    // ticking after stop()). The scheduler holds only the panic's events.
    session_.scheduler().advanceTo(INT64_MAX);
    if (output_) control::pumpEngineOutput(out_bridge_, *output_);
    // Commands issued for this session must not act on a later one (a pending
    // Start used to restart playback after stop()). Draining cmd_q_ is not
    // enough: a completed push can sit behind another producer's unpublished
    // slot. Every command carries the generation it was issued in; bumping it
    // makes tick() drop older ones whenever they become visible.
    session_gen_.fetch_add(1, std::memory_order_release);
    has_cmd_carry_ = false;
    pending_tempo_.store(0, std::memory_order_relaxed);
    // Input is not observed while stopped (the sink is detached above), so a
    // key released meanwhile would stay "held" after restart. Forget input
    // state and what is still queued; restart begins with nothing held. Single
    // consumer kept: tick() is excluded and the input producer is detached.
    session_.adapter().resetInput();
    control::ControlEvent dropped;
    while (input_q_.pop(dropped)) {}
    has_input_carry_ = false;
    started_ = false;
    lifecycle_.apply(LifecycleEvent::Stop);    // Gate 4: track high-level state
    publishSnapshot();
}

bool LiveEngineFacade::setVariation(int index) noexcept {
    if (index < 0 || index > 3) return false;
    const auto action = static_cast<control::ControlAction>(
        static_cast<int>(control::ControlAction::VariationA) + index);
    return push(action);
}

void LiveEngineFacade::tick(uint32_t numSamples) noexcept {
    // A style load in progress owns the engine: skip this step rather than
    // wait (tick() may run on an audio thread). Loads only happen with the
    // transport stopped, so a skipped step loses no playback time.
    if (engine_busy_.test_and_set(std::memory_order_acquire)) return;
    tickLocked(numSamples);
    engine_busy_.clear(std::memory_order_release);
}

EngineError LiveEngineFacade::loadStyle(const uasf::StyleDefinition& style) noexcept {
    // Fast reject without touching the tick (no skipped step while playing).
    if (session_.player().isPlaying()) {
        recordError(EngineError::InvalidState);
        return EngineError::InvalidState;
    }
    bool loaded = false;
    {
        EngineGuard guard(engine_busy_);
        // Re-checked under the exclusion: a Start may have been applied meanwhile.
        // The lifecycle tracker is updated here too: tick() publishes its state.
        loaded = session_.loadStyle(style);
        if (loaded && lifecycle_.state() != LifecycleState::Running &&
            lifecycle_.state() != LifecycleState::Suspended)
            lifecycle_.apply(LifecycleEvent::LoadStyle);
    }
    if (!loaded) {
        recordError(EngineError::InvalidState);
        return EngineError::InvalidState;
    }
    return EngineError::Ok;
}

void LiveEngineFacade::tickLocked(uint32_t numSamples) noexcept {
    // Apply a pending tempo change (UI thread set it atomically).
    if (uint32_t bpm = pending_tempo_.exchange(0, std::memory_order_relaxed))
        session_.clock().setTempo(bpm);

    // Drain UI commands then MIDI input into the engine. This thread is the SOLE
    // producer of the adapter's SPSC control queue, which is the same depth as
    // each facade queue: a burst on both in one tick cannot all fit, so the drain
    // applies back-pressure instead of discarding (a lost NoteOff = stuck note).
    drainToAdapter([this](control::ControlEvent& e) noexcept {
                       const uint64_t gen = session_gen_.load(std::memory_order_acquire);
                       while (cmd_q_.try_pop(e))
                           if (e.timestamp == gen) return true;   // older session: dropped
                       return false;
                   },
                   cmd_carry_, has_cmd_carry_);
    drainToAdapter([this](control::ControlEvent& e) noexcept { return input_q_.pop(e); },
                   input_carry_, has_input_carry_);

    // Device commands go out ahead of this tick's notes. This thread is the
    // bridge's only producer. A full bridge drops and counts it (dropped()),
    // as it does engine events. Not session-stamped: a command is for the
    // device, so one queued across stop()/start() is still sent.
    for (uasf::MidiEvent dev; device_q_.try_pop(dev);) out_bridge_.sendUnmapped(dev);

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
