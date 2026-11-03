#pragma once

// LiveHostDriver (Gate 2) — the Mac live-performance reference host's engine
// pump. Unlike the output-only Gate 14 EngineDriver, this drives the portable
// LiveEngineFacade end-to-end: a real CoreMIDI keyboard feeds chords in, the
// engine scheduler feeds a CoreMIDI destination out, and the UI polls the
// facade's atomic snapshot. CoreMIDI lives ONLY in the platform adapters
// (CoreMidiIn / CoreMidiOutputProvider); the facade + engine stay portable.
//
// Threading:
//   - hiResTimerCallback() (this timer thread) is the SOLE producer of the
//     engine's control queue. It drains the facade's two SPSC queues (UI + MIDI
//     input) inside tick(), advances the engine, and pumps output.
//   - The UI thread posts commands via facade() (lock-free SPSC) and reads a
//     thread-safe atomic snapshot().
//   - The CoreMIDI read thread routes messages onto the facade's input queue.
//
// No malloc / lock / ObjC in the timer callback or the CoreMIDI read callback. The
// tick's snapshot publish is a lock-free seqlock (session/seqlock_snapshot.h).

#include <juce_core/juce_core.h>
#include "session/live_engine_facade.h"
#include "engine/midi/coremidi_in.h"
#include "engine/chord/fingered_detector.h"
#include "midi/coremidi/coremidi_output_provider.h"

namespace ai_arranger::app {

class LiveHostDriver : private juce::HighResolutionTimer {
public:
    LiveHostDriver() : facade_(&midi_in_, &midi_out_) {}
    ~LiveHostDriver() override { stop(); }

    void start() {
        if (running_) return;
        midi_in_.initialize("AI Arranger");
        midi_out_.initialize("AI Arranger");
        facade_.setChordScanMode(&detector_);   // fingered chord detection by default
        facade_.start();                         // boots engine + wires input sink
        startTimer(1);                           // ~1 ms engine tick (MIDI only, no DSP)
        running_ = true;
    }

    void stop() {
        if (!running_) return;
        stopTimer();   // tick thread quiesced: engine is single-threaded here now
        // Silence the output device SYNCHRONOUSLY before detaching MIDI, so closing
        // while playing never leaves hanging notes. panic() = all-sound-off (CC120)
        // per active channel + clock stop; tick(0) drains+pumps it onto the output
        // queue now (with the tick thread stopped, enqueuing Stop alone would never
        // be pumped). CoreMidiOut::shutdown() then drains that queue before joining,
        // so the CC120 reaches the endpoint before the port is disposed.
        facade_.panic();
        facade_.tick(0);
        // N1 (review #27): disconnect the CoreMIDI source BEFORE the facade clears
        // the input sink (the sink teardown itself now also drains in-flight reads).
        midi_in_.selectSource(-1);
        facade_.stop();
        midi_in_.shutdown();
        midi_out_.shutdown();
        running_ = false;
    }

    // Load a new style safely while the engine may be running. loadStyle() copy-
    // assigns the whole StyleDefinition and repoints the sequencer, which the
    // portable facade treats as non-realtime — so it must NOT run concurrently
    // with the tick thread. stopTimer() blocks until any in-flight
    // hiResTimerCallback() has returned (JUCE: "may block while it waits for
    // pending callbacks to complete. Once it returns, no more callbacks will be
    // made"), so the swap runs with the tick thread fully quiesced. The tick is
    // always restarted (RAII, even if the swap throws), and the transport is left
    // stopped — the UI must press Start again. Caller passes an already-parsed
    // style, so a bad file never reaches here and the old style is untouched.
    void reloadStyle(const uasf::StyleDefinition& style) {
        if (!running_) {                 // no tick thread yet: nothing to race
            facade_.loadStyle(style);
            return;
        }
        stopTimer();                     // quiesce: waits for the in-flight tick
        // Restart the tick on every exit path (normal return or exception).
        struct TickGuard { LiveHostDriver* d; ~TickGuard() { d->restartTick(); } } guard{this};
        // The engine is now single-threaded on this (message) thread.
        facade_.transportStop();         // enqueue Stop …
        facade_.tick(0);                 // … and drain it now: flush notes + pump, no time advance
        facade_.loadStyle(style);        // safe swap: no concurrent reader, clock stopped
        // transport left stopped; tick restarts via guard.
    }

    // Switch the MIDI output safely while the engine may be running. Quiesce the
    // tick thread, then panic (all-sound-off + clear the engine's active-note
    // tracking so no leftover note-off later targets the NEW device) and stop the
    // transport, then repoint the output — selectDestination() silences the OLD
    // device synchronously first, so its all-notes-off never leaks to the new one.
    // Transport is left stopped after the switch (press Start to resume).
    void switchOutput(int index) {
        if (!running_) {                 // no tick thread yet: plain select
            facade_.selectMidiOutput(index);
            return;
        }
        stopTimer();                     // quiesce: waits for the in-flight tick
        struct TickGuard { LiveHostDriver* d; ~TickGuard() { d->restartTick(); } } guard{this};
        facade_.panic();                 // stop + all-sound-off + clear active notes …
        facade_.tick(0);                 // … applied now on the quiesced engine
        facade_.selectMidiOutput(index); // silences old device, then attaches new
        // transport left stopped; tick restarts via guard.
    }

    // ── Accessors for the UI ──────────────────────────────────────────
    // Commands are posted lock-free; the snapshot is read through the facade as a
    // thread-safe atomic; enumeration is read straight off the platform adapters.
    session::LiveEngineFacade&    facade()  noexcept { return facade_; }
    midi::CoreMidiIn&             midiIn()  noexcept { return midi_in_; }
    midi::CoreMidiOutputProvider& midiOut() noexcept { return midi_out_; }
    bool isRunning() const noexcept { return running_; }

private:
    void hiResTimerCallback() override { facade_.tick(48 /* ~1 ms @ 48 kHz */); }

    // Restart the engine tick (member access to the protected base method, so the
    // RAII TickGuard in reloadStyle() can re-arm it on any exit path).
    void restartTick() noexcept { startTimer(1); }

    // Declared before facade_: the facade constructor captures &midi_in_/&midi_out_.
    midi::CoreMidiIn             midi_in_;
    midi::CoreMidiOutputProvider midi_out_;
    chord::FingeredDetector      detector_;
    session::LiveEngineFacade    facade_;
    bool                         running_{false};
};

} // namespace ai_arranger::app
