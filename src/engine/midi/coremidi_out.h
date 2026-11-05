#ifndef AI_ARRANGER_MIDI_COREMIDI_OUT_H
#define AI_ARRANGER_MIDI_COREMIDI_OUT_H

#include "engine/uasf/types.h"
#include "engine/midi/scheduler.h" // LockFreeQueue
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <CoreMIDI/CoreMIDI.h>

namespace ai_arranger::midi {

/**
 * Real CoreMIDI output to an external device (e.g. Korg / Yamaha arranger).
 *
 * Threading model (ADR-013 realtime safety):
 *   - Producer  = audio / scheduler thread → send() pushes onto a lock-free
 *     SPSC queue. send() does NO malloc / mutex / syscall / MIDISend.
 *   - Consumer  = a dedicated dispatch thread (NOT the audio render thread)
 *     pops events, builds a MIDIPacketList, and calls MIDISend.
 *
 * Hotplug:
 *   - The selected destination is remembered by NAME. A MIDINotifyProc marks
 *     the setup dirty (when a CoreMIDI run loop is present), and the dispatch
 *     thread also re-resolves the endpoint by name periodically, so a device
 *     that disappears and reappears reconnects without caller intervention.
 *
 * Fail-closed output policy (hardware safety):
 *   - initialize() NEVER selects a destination. Nothing reaches any device until
 *     the caller selects one explicitly with selectDestination(); a connected
 *     device is never used as a default or fallback.
 *   - With no destination selected, send()/dispatch are a graceful no-op (events
 *     counted as dropped) — on CI and on a machine with a keyboard attached alike.
 *   - Hardware-transmission guard at the MIDISend boundary: if the environment
 *     variable AIARR_MIDI_HW_TX is "0" when initialize() runs, every MIDISend
 *     (events AND cleanup CCs) is blocked and counted in blockedCount(). CTest sets
 *     it for every registered test, so a software test cannot drive hardware
 *     even if it selects a real destination.
 *   - shutdown() silences the selected destination (CC123 + CC120, 16 channels)
 *     after draining the queue, so a stray Note On never outlives the process.
 *   - midiEventToBytes() is a pure function, fully testable without hardware.
 */

// Convert a UASF MidiEvent into raw MIDI status/data bytes.
// Writes up to 3 bytes into out (must hold 3). Returns the byte count
// (2 for ProgramChange/ChannelPressure, 3 otherwise, 0 if unsupported).
// Pure function — no CoreMIDI dependency, safe to unit test on CI.
size_t midiEventToBytes(const uasf::MidiEvent& ev, uint8_t* out) noexcept;

struct MidiDestinationInfo {
    int         index;   // index into the CoreMIDI destination list
    std::string name;    // display name (endpoint or its device)
};

class CoreMidiOut {
public:
    static constexpr size_t kQueueSize = 8192;

    CoreMidiOut() = default;
    ~CoreMidiOut();

    CoreMidiOut(const CoreMidiOut&) = delete;
    CoreMidiOut& operator=(const CoreMidiOut&) = delete;

    // ── Lifecycle (non-realtime) ─────────────────────────────────────
    // Creates the CoreMIDI client + output port and starts the dispatch
    // thread. Selects NO destination (fail-closed). Returns false only on a hard
    // CoreMIDI failure (not on "zero destinations", a valid headless state).
    bool initialize(const char* clientName = "AI Arranger") noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool isInitialized() const noexcept;

    // ── Destination management (non-realtime) ────────────────────────
    [[nodiscard]] std::vector<MidiDestinationInfo> enumerateDestinations() const noexcept;
    [[nodiscard]] int destinationCount() const noexcept;
    // Select destination by index; -1 selects "none". Remembers the name
    // for hotplug re-resolution. Returns false if index out of range.
    bool selectDestination(int index) noexcept;
    [[nodiscard]] int  selectedDestination() const noexcept;
    [[nodiscard]] bool hasLiveDestination() const noexcept;

    // ── Realtime-safe send (producer / audio thread) ─────────────────
    // Enqueue an event for the dispatch thread. Returns false if the queue
    // is full (event dropped). NEVER blocks, allocates, or calls MIDISend.
    bool send(const uasf::MidiEvent& ev) noexcept;

    // ── Diagnostics / test hooks ─────────────────────────────────────
    // Tap invoked on the dispatch thread for every event just before it
    // would be sent to hardware. Used by the latency harness and unit tests
    // to observe dispatch without real hardware. Not realtime — set before
    // playback. NOTE: runs on the dispatch thread, keep it cheap.
    using DispatchTap = std::function<void(const uasf::MidiEvent&)>;
    void setDispatchTap(DispatchTap tap) noexcept; // call before initialize()

    [[nodiscard]] uint64_t sentCount() const noexcept;     // bytes actually MIDISent
    [[nodiscard]] uint64_t dispatchedCount() const noexcept; // events popped
    [[nodiscard]] uint64_t droppedCount() const noexcept;  // dropped (queue full)
    [[nodiscard]] uint64_t reconnectCount() const noexcept;
    // Messages a selected destination did NOT receive because the hardware-
    // transmission guard (AIARR_MIDI_HW_TX=0) is active.
    [[nodiscard]] uint64_t blockedCount() const noexcept;
    // False when the guard read at initialize() blocks all transmission.
    [[nodiscard]] bool hardwareTransmitAllowed() const noexcept { return hw_tx_allowed_; }

private:
    void dispatchLoop() noexcept;          // consumer thread body
    void dispatchOne(const uasf::MidiEvent& ev) noexcept; // send one event now
    // Directly send All-Notes-Off (CC123) + All-Sound-Off (CC120) on every channel
    // to `ep` (non-realtime, calling thread). Used to silence the outgoing device
    // before a destination switch/disconnect.
    void sendAllSoundOff(MIDIEndpointRef ep) noexcept;
    void resolveEndpointByName() noexcept; // hotplug re-resolution
    static void notifyProc(const MIDINotification* msg, void* refCon);

    LockFreeQueue<uasf::MidiEvent, kQueueSize> queue_;

    MIDIClientRef    client_{0};
    MIDIPortRef      out_port_{0};
    std::atomic<MIDIEndpointRef> endpoint_{0}; // 0 = none/disconnected

    std::string      selected_name_;          // remembered for hotplug
    std::atomic<int> selected_index_{-1};
    std::atomic<bool> needs_reresolve_{false};

    std::thread       dispatch_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    DispatchTap dispatch_tap_{nullptr};

    std::atomic<uint64_t> sent_count_{0};
    std::atomic<uint64_t> dispatched_count_{0};
    std::atomic<uint64_t> dropped_count_{0};
    std::atomic<uint64_t> reconnect_count_{0};
    std::atomic<uint64_t> blocked_count_{0};
    // Written only in initialize() before the dispatch thread starts.
    bool hw_tx_allowed_{false};
};

} // namespace ai_arranger::midi

#endif // AI_ARRANGER_MIDI_COREMIDI_OUT_H
