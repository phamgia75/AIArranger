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
#include "midi/midi_input_router.h"   // ChannelRoles
#include "engine/chord/chord_scan_mode.h"
#include "engine/trace/mpsc_ring.h"
#include "engine/uasf/types.h"
#include "midi/midi_input_source.h"
#include "midi/midi_output_provider.h"
#include <atomic>
#include <thread>
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
    // Owner thread. Like loadStyle(), start()/stop() are safe while tick() runs
    // on another thread (tick() skips its step meanwhile) and must not be called
    // from inside tick() (#10).
    void start() noexcept;
    void stop() noexcept;

    // Load a normalized style (owner thread). Before start() boot keeps it;
    // with the transport stopped it replaces the style and adopts its tempo,
    // PPQN and meter. While the transport PLAYS it is rejected: returns (and
    // records) EngineError::InvalidState and changes nothing — stop first
    // (Issue #4). Safe while tick() runs on another thread: the two exclude
    // each other, and tick() never waits — it skips its step while a load is
    // in progress (Issue #5). Never call it from inside tick() (e.g. from an
    // output callback on the tick thread): it would wait for its own tick.
    // The lifecycle tracker advances toward Ready when the engine is not
    // running; a load while running does not move state.
    EngineError loadStyle(const uasf::StyleDefinition& style) noexcept;

    // ── Commands (any thread, concurrently; lock-free) ──────────────────
    // Each enqueues onto the MPSC command queue drained in tick() and returns
    // whether it was accepted (queued). A dropped command (queue full) returns
    // false and records EngineError::QueueFull (#18). Whether the engine can
    // act on an accepted command (e.g. Start with no style) shows in snapshot().
    bool transportStart() noexcept { return push(control::ControlAction::Start); }
    bool transportStop()  noexcept { return push(control::ControlAction::Stop); }
    // Sync Start: arm the engine; the first detected chord fires playback.
    bool syncStart()      noexcept { return push(control::ControlAction::SyncArm); }
    bool intro()          noexcept { return push(control::ControlAction::Intro); }
    bool fill()           noexcept { return push(control::ControlAction::Fill); }
    bool breakSection()   noexcept { return push(control::ControlAction::Break); }
    bool ending()         noexcept { return push(control::ControlAction::Ending); }
    bool panic()          noexcept { return push(control::ControlAction::Panic); }
    // variation 0..3 -> A..D; out-of-range ignored (returns false).
    bool setVariation(int index) noexcept;
    // Applied on the next tick (the latest value wins). Outside the engine
    // range (uasf::kMinTempoBpm..kMaxTempoBpm) it is ignored and returns false.
    bool setTempo(uint32_t bpm) noexcept {
        if (bpm < uasf::kMinTempoBpm || bpm > uasf::kMaxTempoBpm) return false;
        pending_tempo_.store(bpm, std::memory_order_relaxed);
        return true;
    }

    // Auto-fill on variation change (arranger-engine-spec §5.3). A setting,
    // not a queued command: any thread, takes effect at once, kept across
    // stop()/start(). Off by default.
    void setAutoFill(bool on) noexcept { session_.player().setAutoFill(on); }
    bool autoFill() const noexcept { return session_.player().autoFill(); }

    // Device selection (delegates to the injected source/provider). A failed
    // selection records EngineError::DeviceUnavailable; the bool return is
    // unchanged.
    bool selectMidiInput(int index) noexcept  {
        const bool ok = input_ ? input_->selectSource(index) : false;
        if (!ok) recordError(EngineError::DeviceUnavailable);
        return ok;
    }
    // MIDI input channel roles for chord recognition (default: all Global, the
    // previous behaviour). Takes effect for the next message; safe from any
    // non-realtime thread while MIDI input is running.
    void setInputChannelRoles(const midi::ChannelRoles& r) noexcept {
        for (int i = 0; i < 16; ++i)
            input_roles_[i].store(static_cast<uint8_t>(r.role[i]), std::memory_order_relaxed);
    }
    midi::ChannelRoles inputChannelRoles() const noexcept {
        midi::ChannelRoles r;
        for (int i = 0; i < 16; ++i)
            r.role[i] = static_cast<midi::ChannelRole>(input_roles_[i].load(std::memory_order_relaxed));
        return r;
    }
    // Output channel routing (default identity). Refused (false) while the
    // transport is playing so no note can change channel mid-flight; stop, tick,
    // then set.
    bool setOutputChannelMap(const control::OutputChannelMap& m) noexcept {
        if (snapshot().playing) return false;
        out_bridge_.setChannelMap(m);
        return true;
    }
    control::OutputChannelMap outputChannelMap() const noexcept { return out_bridge_.channelMap(); }
    // A device command sent on its own channel, bypassing the output channel
    // map (e.g. midi::pa5x::controlEvent). Any thread; delivered on the next
    // tick, before that tick's engine output. False (QueueFull recorded) if the
    // queue is full, or if channel > 15 or a data byte > 127.
    bool sendDeviceCommand(const uasf::MidiEvent& ev) noexcept {
        if (ev.channel > 15 || ev.data1 > 127 || ev.data2 > 127) return false;
        if (device_q_.try_push(ev)) return true;
        recordError(EngineError::QueueFull);
        return false;
    }
    // MIDI Clock out (24 per quarter note) with Start/Stop, on the output
    // device, while the transport plays. Off by default. Refused (false) while
    // playing, so a receiver never sees clock without a Start.
    // shortcut: pulses leave at tick() granularity (jitter = one tick's buffer, the
    // same as the notes); timestamp them if a receiver needs sample-accurate clock.
    bool setMidiClockOut(bool on) noexcept {
        if (snapshot().playing) return false;
        clock_out_.store(on, std::memory_order_relaxed);
        return true;
    }
    bool midiClockOut() const noexcept { return clock_out_.load(std::memory_order_relaxed); }
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
    void tickLocked(uint32_t numSamples) noexcept;
    void emitMidiClock() noexcept;   // tick thread (or owner thread with tick excluded)
    // fromTick: the realtime path never waits for a concurrent lifecycle publish.
    void publishSnapshot(bool fromTick = false) noexcept;
    bool push(control::ControlAction a, int32_t param = 0) noexcept {
        // timestamp = the session generation the command belongs to (see stop()).
        if (cmd_q_.try_push({a, param, session_gen_.load(std::memory_order_acquire)})) return true;
        recordError(EngineError::QueueFull);
        return false;
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
    trace::MpscRing<uasf::MidiEvent, 64>              device_q_; // any thread -> tick -> out_bridge_
    std::atomic<uint8_t> input_roles_[16] = {};   // midi::ChannelRole per channel; 0 = Global

    // Tick-thread only: the one event per queue the engine could not accept on
    // the previous tick, delivered first on the next (keeps FIFO order).
    control::ControlEvent cmd_carry_{};
    control::ControlEvent input_carry_{};
    bool has_cmd_carry_{false};
    bool has_input_carry_{false};

    // MIDI Clock out. clock_out_ any thread; the rest tick-thread only.
    std::atomic<bool> clock_out_{false};
    bool    clock_running_{false};   // Start sent, Stop not yet
    int64_t next_pulse_{0};          // index of the next 0xF8 since Start
    int64_t last_clock_pos_{0};

    std::atomic<uint32_t>       pending_tempo_{0};
    std::atomic<uint64_t>       session_gen_{0};   // bumped by stop(); stamps commands
    SeqlockSnapshot<EngineSnapshot> snapshot_;
    EngineLifecycle             lifecycle_{};                 // Gate 4 lifecycle tracker
    std::atomic<EngineError>    last_error_{EngineError::Ok}; // most recent deterministic error
    bool started_{false};
    // Lifecycle (start/stop/loadStyle) <-> tick() exclusion. tick() only
    // try-acquires (never blocks, skips its step when taken); the lifecycle
    // calls wait for an in-flight tick (owner thread, not realtime).
    std::atomic_flag engine_busy_ = ATOMIC_FLAG_INIT;
    struct EngineGuard {
        std::atomic_flag& f;
        explicit EngineGuard(std::atomic_flag& flag) noexcept : f(flag) {
            while (f.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
        }
        ~EngineGuard() { f.clear(std::memory_order_release); }
        EngineGuard(const EngineGuard&) = delete;
        EngineGuard& operator=(const EngineGuard&) = delete;
    };
};

} // namespace ai_arranger::session

#endif // AI_ARRANGER_SESSION_LIVE_ENGINE_FACADE_H
