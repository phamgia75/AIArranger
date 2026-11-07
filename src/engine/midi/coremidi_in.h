#ifndef AI_ARRANGER_MIDI_COREMIDI_IN_H
#define AI_ARRANGER_MIDI_COREMIDI_IN_H

#include "midi/midi_input_source.h"     // IMidiInputSource + MidiSourceInfo + MidiInputMessage
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <CoreMIDI/CoreMIDI.h>

namespace ai_arranger::midi {

/**
 * Real CoreMIDI input from an external keyboard/controller — the symmetric
 * counterpart to CoreMidiOut.
 *
 * Threading model:
 *   - CoreMIDI delivers packets on its own high-priority read thread
 *     (readProc). readProc parses each packet with the pure parseMidiInput()
 *     and hands every message to the sink. The sink MUST be cheap and lock-free
 *     (it typically just posts a ControlEvent onto the engine's SPSC queue).
 *   - No allocation / locking is done in readProc.
 *
 * CI safety:
 *   - With zero sources (headless CI) initialize() still succeeds and the port
 *     simply receives nothing; selectSource()/hasLiveSource() report "none".
 *   - The parsing/routing logic is fully testable without hardware via
 *     parseMidiInput() + routeMidiInput() (see tests/midi/test_midi_input_parser).
 */
class CoreMidiIn final : public IMidiInputSource {
public:
    CoreMidiIn() = default;
    ~CoreMidiIn() override;

    CoreMidiIn(const CoreMidiIn&) = delete;
    CoreMidiIn& operator=(const CoreMidiIn&) = delete;

    // ── Lifecycle (non-realtime) ─────────────────────────────────────
    bool initialize(const char* clientName = "AI Arranger") noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool isInitialized() const noexcept;

    // ── IMidiInputSource ─────────────────────────────────────────────
    std::vector<MidiSourceInfo> enumerateSources() const noexcept override;
    int sourceCount() const noexcept override;
    // Connect the port to source `index`; -1 disconnects. Returns false if out
    // of range. Remembers the name for hot-plug re-resolution.
    bool selectSource(int index) noexcept override;
    int  selectedSource() const noexcept override;
    bool hasLiveSource() const noexcept override;

    // Invoked for every parsed message on the read thread. Set before selecting.
    // Swapping the sink first stops delivery (accepting_ = false) and BLOCKS until
    // any in-flight readProc has returned (quiesceReadCallback), so the read thread
    // can never touch a std::function target that is being destroyed underneath it.
    void setSink(MessageSink sink) noexcept override {
        quiesceReadCallback();
        sink_ = std::move(sink);
        accepting_.store(sink_ != nullptr, std::memory_order_release);
    }

    uint64_t receivedCount() const noexcept override {
        return received_.load(std::memory_order_relaxed);
    }

    // Hot-plug recovery (non-realtime; call from a UI/control thread, e.g. a
    // periodic timer). The selected source is remembered by CoreMIDI unique ID.
    // If that device disappeared, the port is disconnected (hasLiveSource() is
    // false); when a source with the same unique ID reappears it is reconnected.
    // Never connects to a different device. Cheap (one CoreMIDI source scan), so
    // a ~30 Hz UI timer is fine. Call on the thread that calls selectSource().
    // Returns hasLiveSource().
    bool pollReconnect() noexcept;
    int32_t selectedUniqueId() const noexcept {
        return selected_uid_.load(std::memory_order_acquire);
    }
    uint64_t reconnectCount() const noexcept {
        return reconnects_.load(std::memory_order_relaxed);
    }

    // CoreMIDI read callback (readRefCon = this). Public only so a test can feed it a
    // MIDIPacketList built in memory, without a MIDI server or device.
    static void readProc(const MIDIPacketList* pktList, void* readRefCon, void* srcRefCon);

private:
    static void notifyProc(const MIDINotification* msg, void* refCon);

    // Stop sink delivery and spin until any in-flight readProc has returned. Call
    // on a non-realtime thread before destroying/replacing the sink or disposing
    // the port — MIDIPortDisconnectSource does NOT join in-flight read callbacks,
    // so this is what actually guarantees the read thread is out of the sink.
    void quiesceReadCallback() noexcept;

    MIDIClientRef                client_{0};
    MIDIPortRef                  in_port_{0};
    std::atomic<MIDIEndpointRef> source_{0};   // 0 = none/disconnected
    std::string                  selected_name_;
    std::atomic<int>             selected_index_{-1};
    std::atomic<int32_t>         selected_uid_{0};  // 0 = nothing selected
    std::atomic<uint64_t>        reconnects_{0};

    MessageSink                  sink_{nullptr};
    std::atomic<bool>            accepting_{false}; // true only while sink_ is live
    std::atomic<int>             in_flight_{0};     // readProc invocations in progress
    std::atomic<uint64_t>        received_{0};
    std::atomic<bool>            initialized_{false};
};

} // namespace ai_arranger::midi

#endif // AI_ARRANGER_MIDI_COREMIDI_IN_H
