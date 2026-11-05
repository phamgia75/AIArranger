#ifndef AI_ARRANGER_MIDI_COREMIDI_OUTPUT_PROVIDER_H
#define AI_ARRANGER_MIDI_COREMIDI_OUTPUT_PROVIDER_H

#include "midi/midi_output_provider.h"
#include "engine/midi/coremidi_out.h"

// CoreMIDI-backed provider (Gate 14C). Thin wrapper over the Gate 9 CoreMidiOut
// — no behaviour change to that class; this only adapts it to the
// IMidiOutputProvider interface. Real-device output; headless-safe (no-op with
// zero destinations).

namespace ai_arranger::midi {

class CoreMidiOutputProvider : public IMidiOutputProvider {
public:
    bool initialize(const char* clientName = "AI Arranger") noexcept {
        return out_.initialize(clientName);
    }
    void shutdown() noexcept { out_.shutdown(); }

    std::vector<MidiOutDeviceInfo> enumerate() override {
        std::vector<MidiOutDeviceInfo> v;
        for (const auto& d : out_.enumerateDestinations())
            v.push_back({d.index, d.name, d.uid});
        return v;
    }
    bool select(int index) override { return out_.selectDestination(index); }
    int  selected() const override { return out_.selectedDestination(); }
    bool hasLiveDestination() const override { return out_.hasLiveDestination(); }
    bool send(const uasf::MidiEvent& ev) override { return out_.send(ev); }
    uint64_t dispatchedCount() const override { return out_.dispatchedCount(); }

    CoreMidiOut& raw() noexcept { return out_; }

private:
    CoreMidiOut out_;
};

} // namespace ai_arranger::midi

#endif // AI_ARRANGER_MIDI_COREMIDI_OUTPUT_PROVIDER_H
