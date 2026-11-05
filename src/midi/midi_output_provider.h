#ifndef AI_ARRANGER_MIDI_MIDI_OUTPUT_PROVIDER_H
#define AI_ARRANGER_MIDI_MIDI_OUTPUT_PROVIDER_H

#include "engine/uasf/types.h"
#include <cstdint>
#include <string>
#include <vector>

// ── MIDI output provider abstraction (Gate 14C Task A) ───────────────
//
// Abstracts the MIDI output endpoint so the manager/bridge are testable with a
// synthetic provider (no CoreMIDI in CI). The real provider wraps the Gate 9
// CoreMidiOut (lock-free send, dedicated dispatch thread, hot-plug by name,
// graceful no-op with zero destinations) WITHOUT changing its behaviour.

namespace ai_arranger::midi {

struct MidiOutDeviceInfo {
    int         index = -1;
    std::string name;
    int64_t     uid = 0;   // stable device identity (CoreMIDI unique ID); 0 = provider has none
};

class IMidiOutputProvider {
public:
    virtual ~IMidiOutputProvider() = default;

    virtual std::vector<MidiOutDeviceInfo> enumerate() = 0;

    // Select by index (-1 = none). Returns false if out of range.
    virtual bool select(int index) = 0;
    virtual int  selected() const = 0;

    // True when a real, connected destination is currently resolved.
    virtual bool hasLiveDestination() const = 0;

    // Realtime-safe enqueue/send (producer side). Returns false if dropped.
    virtual bool send(const uasf::MidiEvent& ev) = 0;

    // Events handed onward to the endpoint (for telemetry).
    virtual uint64_t dispatchedCount() const = 0;
};

} // namespace ai_arranger::midi

#endif // AI_ARRANGER_MIDI_MIDI_OUTPUT_PROVIDER_H
