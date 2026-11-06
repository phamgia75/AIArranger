#ifndef AI_ARRANGER_CONTROL_MIDI_OUTPUT_BRIDGE_H
#define AI_ARRANGER_CONTROL_MIDI_OUTPUT_BRIDGE_H

#include "control/ui_event_queue.h"
#include "midi/midi_output_provider.h"
#include "engine/uasf/types.h"
#include <atomic>
#include <cstddef>

// ── Engine → MIDI output bridge (Gate 14C Task C) ────────────────────
//
// Lock-free SPSC hand-off from the engine/scheduler thread to the MIDI output.
// The engine calls send() (producer, realtime-safe, no alloc/lock); a consumer
// calls pump(provider) to drain and forward to the active provider. Keeps the
// playback path non-blocking and provider-agnostic.

namespace ai_arranger::control {

// Output channel routing applied to every engine event before it is queued.
// dst[ch] = output channel (0-15) or kDrop. Default: identity, nothing dropped,
// so output is byte-identical to the engine's own channels.
struct OutputChannelMap {
    static constexpr uint8_t kDrop = 0xFF;
    uint8_t dst[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    bool dropVoiceSelect = false;   // drop Bank Select (CC0/CC32) and Program Change
};

class MidiOutputBridge {
public:
    // Producer (engine thread). Returns false if the queue is full (dropped).
    // An event the channel map drops is not an error: returns true, counted in
    // routedOut().
    bool send(const uasf::MidiEvent& in) noexcept {
        uasf::MidiEvent ev = in;
        const uint8_t to = map_dst_[ev.channel & 0x0F].load(std::memory_order_relaxed);
        const bool voiceSelect = ev.type == uasf::MidiEventType::ProgramChange ||
            (ev.type == uasf::MidiEventType::ControlChange && (ev.data1 == 0 || ev.data1 == 32));
        if (to == OutputChannelMap::kDrop ||
            (voiceSelect && drop_voice_select_.load(std::memory_order_relaxed))) {
            routed_out_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        ev.channel = to;
        if (queue_.push(ev)) return true;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Producer (same thread as send()). Queues `ev` as is: no channel map, no
    // voice-select drop. For device commands (e.g. Pa5X Control-channel PCs)
    // that must reach their own channel whatever the style routing is.
    bool sendUnmapped(const uasf::MidiEvent& ev) noexcept {
        if (queue_.push(ev)) return true;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Consumer. Drains all queued events into the provider; returns the number
    // forwarded. (Provider may itself drop if not connected.)
    size_t pump(midi::IMidiOutputProvider& provider) noexcept {
        size_t n = 0;
        uasf::MidiEvent ev;
        while (queue_.pop(ev)) { provider.send(ev); ++n; }
        forwarded_.fetch_add(n, std::memory_order_relaxed);
        return n;
    }

    // Change only while no note is sounding (the facade enforces transport
    // stopped), or a Note Off could leave on a different channel than its Note On.
    void setChannelMap(const OutputChannelMap& m) noexcept {
        for (int i = 0; i < 16; ++i)
            map_dst_[i].store(m.dst[i] < 16 ? m.dst[i] : OutputChannelMap::kDrop,
                              std::memory_order_relaxed);
        drop_voice_select_.store(m.dropVoiceSelect, std::memory_order_relaxed);
    }
    OutputChannelMap channelMap() const noexcept {
        OutputChannelMap m;
        for (int i = 0; i < 16; ++i) m.dst[i] = map_dst_[i].load(std::memory_order_relaxed);
        m.dropVoiceSelect = drop_voice_select_.load(std::memory_order_relaxed);
        return m;
    }
    uint64_t routedOut() const noexcept { return routed_out_.load(std::memory_order_relaxed); }

    uint64_t forwarded() const noexcept { return forwarded_.load(std::memory_order_relaxed); }
    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    static constexpr size_t capacity() noexcept { return 8192 - 1; }

private:
    UiEventQueue<uasf::MidiEvent, 8192> queue_;
    std::atomic<uint64_t> forwarded_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint8_t>  map_dst_[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    std::atomic<bool>     drop_voice_select_{false};
    std::atomic<uint64_t> routed_out_{0};
};

} // namespace ai_arranger::control

#endif // AI_ARRANGER_CONTROL_MIDI_OUTPUT_BRIDGE_H
