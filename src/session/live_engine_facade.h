#ifndef AI_ARRANGER_SESSION_LIVE_ENGINE_FACADE_H
#define AI_ARRANGER_SESSION_LIVE_ENGINE_FACADE_H

#include "session/engine_session.h"
#include "session/engine_snapshot.h"
#include "session/seqlock_snapshot.h"
#include "session/engine_contract.h"
#include "session/engine_lifecycle.h"
#include "control/ui_event_queue.h"
#include "control/midi_output_bridge.h"
#include "engine/control/control_events.h"
#include "engine/chord/chord_scan_mode.h"
#include "engine/trace/mpsc_ring.h"
#include "midi/midi_input_source.h"
#include "midi/midi_output_provider.h"
#include <atomic>
#include <cstdint>

// ── LiveEngineFacade — the single entry point the UI/bridge drives ───────────
//
// Portable (no Apple framework). Owns an EngineSession and wires the full live
// loop around it: an injected MIDI input source feeds chords in; the engine
// scheduler feeds an injected MIDI output provider out. The Swift/C++ bridge
// calls the command methods and polls snapshot(); it never touches the engine
// directly.
//
// Threading (matches the macOS EngineDriver, generalised + testable):
//   - Commands (transport/tempo/variation/panic) may be called from any thread,
//     concurrently — each enqueues onto a bounded lock-free MPSC queue.
//   - The MIDI input source's read thread routes messages onto a SPSC queue
//     (input thread = the one producer). Both are drained inside tick() so the
//     engine's own SPSC control queue keeps a single producer (this tick thread).
//     The drain is lossless: an event the engine queue cannot take this tick is
//     held back and delivered first on the next tick (never silently dropped).
//   - tick() runs on the engine thread; it advances the clock/sequencer, pumps
//     output, and republishes the snapshot the UI polls. Publication is a
//     lock-free seqlock (never a mutex), so tick() may run on an audio thread.

namespace ai_arranger::session {

class LiveEngineFacade {
public:
    // Both pointers may be null (headless / not-yet-selected). The facade does
    // not own them; the platform/app owns their lifetime.
    LiveEngineFacade(midi::IMidiInputSource* input,
                     midi::IMidiOutputProvider* output) noexcept;

    // Boot the engine, attach the output route, and wire the input source's sink.
    void start() noexcept;
    void stop() noexcept;

    // Load a normalized style. Call before start() (boot keeps a pre-loaded
    // style); calling while stopped re-arms the sequencer with a new style. The
    // lifecycle tracker advances toward Ready when not running; a load while
    // running is a pass-through (behaviour unchanged) and does not move state.
    void loadStyle(const uasf::StyleDefinition& style) noexcept {
        session_.loadStyle(style);
        if (lifecycle_.state() != LifecycleState::Running &&
            lifecycle_.state() != LifecycleState::Suspended)
            lifecycle_.apply(LifecycleEvent::LoadStyle);
    }

    // ── Commands (any thread, concurrently; lock-free) ──────────────────
    // Each enqueues onto the MPSC command queue drained in tick(). A dropped
    // command (queue full) records EngineError::QueueFull, observable via
    // lastError() — the enqueue itself stays fire-and-forget (unchanged behaviour).
    void transportStart() noexcept { push(control::ControlAction::Start); }
    void transportStop()  noexcept { push(control::ControlAction::Stop); }
    // Sync Start: arm the engine; the first detected chord fires playback.
    void syncStart()      noexcept { push(control::ControlAction::SyncArm); }
    void intro()          noexcept { push(control::ControlAction::Intro); }
    void fill()           noexcept { push(control::ControlAction::Fill); }
    void breakSection()   noexcept { push(control::ControlAction::Break); }
    void ending()         noexcept { push(control::ControlAction::Ending); }
    void panic()          noexcept { push(control::ControlAction::Panic); }
    // variation 0..3 -> A..D; out-of-range ignored.
    void setVariation(int index) noexcept;
    void setTempo(uint32_t bpm) noexcept { pending_tempo_.store(bpm, std::memory_order_relaxed); }

    // Device selection (delegates to the injected source/provider). A failed
    // selection records EngineError::DeviceUnavailable; the bool return is
    // unchanged.
    bool selectMidiInput(int index) noexcept  {
        const bool ok = input_ ? input_->selectSource(index) : false;
        if (!ok) recordError(EngineError::DeviceUnavailable);
        return ok;
    }
    bool selectMidiOutput(int index) noexcept {
        const bool ok = output_ ? output_->select(index) : false;
        if (!ok) recordError(EngineError::DeviceUnavailable);
        return ok;
    }

    // Chord detector (caller owns the instance; Fingered is a sensible default).
    void setChordScanMode(const chord::IChordScanMode* mode) noexcept {
        session_.adapter().setChordScanMode(mode);
    }

    // ── Engine step (engine thread) ───────────────────────────────────
    // Advance the engine by numSamples of wall-clock; drains queues, schedules
    // output, republishes the snapshot.
    void tick(uint32_t numSamples) noexcept;

    // ── UI read ───────────────────────────────────────────────────────
    EngineSnapshot snapshot() const noexcept { return snapshot_.load(); }

    EngineSession& session() noexcept { return session_; }

    // ── Contract queries (Gate 4) ─────────────────────────────────────
    // What this engine build/instance can do. Constant for the instance's life
    // except for the device-presence flags. Safe to call from any thread.
    EngineCapabilities capabilities() const noexcept {
        EngineCapabilities c;
        c.hasMidiInput  = (input_  != nullptr);
        c.hasMidiOutput = (output_ != nullptr);
#ifdef AIARR_LATENCY_TRACE
        c.latencyTrace = true;
#endif
        return c;
    }

    // High-level lifecycle state (Created/Ready/Running/Stopped/...). Advanced by
    // start()/stop()/loadStyle() on the owner thread; also mirrored into
    // snapshot().lifecycleState for lock-free UI polling from another thread.
    LifecycleState lifecycleState() const noexcept { return lifecycle_.state(); }

    // The most recent deterministic error (QueueFull / DeviceUnavailable / ...),
    // or Ok if none. Cleared by clearError(). Reads/writes are relaxed-atomic.
    EngineError lastError() const noexcept { return last_error_.load(std::memory_order_relaxed); }
    void clearError() noexcept { last_error_.store(EngineError::Ok, std::memory_order_relaxed); }

    static constexpr uint32_t contractVersion() noexcept { return kEngineContractVersion; }

private:
    // fromTick: the realtime path never waits for a concurrent lifecycle publish.
    void publishSnapshot(bool fromTick = false) noexcept;
    void push(control::ControlAction a, int32_t param = 0) noexcept {
        if (!cmd_q_.try_push({a, param, 0})) recordError(EngineError::QueueFull);
    }
    // Hand queued events to the engine's control queue without dropping any.
    template <class PopFn>
    void drainToAdapter(PopFn&& pop, control::ControlEvent& carry, bool& hasCarry) noexcept;
    void recordError(EngineError e) noexcept {
        last_error_.store(e, std::memory_order_relaxed);
    }

    EngineSession              session_;
    midi::IMidiInputSource*     input_;
    midi::IMidiOutputProvider*  output_;
    control::MidiOutputBridge   out_bridge_;

    // Any command thread(s) -> tick. Holds exactly EngineCapabilities::maxCommandQueue.
    trace::MpscRing<control::ControlEvent, 256>       cmd_q_;
    control::UiEventQueue<control::ControlEvent, 256> input_q_;  // MIDI-in thread -> tick

    // Tick-thread only: the one event per queue the engine could not accept on
    // the previous tick, delivered first on the next (keeps FIFO order).
    control::ControlEvent cmd_carry_{};
    control::ControlEvent input_carry_{};
    bool has_cmd_carry_{false};
    bool has_input_carry_{false};

    std::atomic<uint32_t>       pending_tempo_{0};
    SeqlockSnapshot<EngineSnapshot> snapshot_;
    EngineLifecycle             lifecycle_{};                 // Gate 4 lifecycle tracker
    std::atomic<EngineError>    last_error_{EngineError::Ok}; // most recent deterministic error
    bool started_{false};
};

} // namespace ai_arranger::session

#endif // AI_ARRANGER_SESSION_LIVE_ENGINE_FACADE_H
