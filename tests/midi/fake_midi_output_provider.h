#ifndef AI_ARRANGER_TESTS_FAKE_MIDI_OUTPUT_PROVIDER_H
#define AI_ARRANGER_TESTS_FAKE_MIDI_OUTPUT_PROVIDER_H

// Synthetic IMidiOutputProvider for headless tests — no CoreMIDI. Lets tests
// drive enumeration, hot-plug (add/remove), connect/disconnect, and capture
// sent events deterministically.

#include "midi/midi_output_provider.h"
#include <vector>

namespace ai_arranger::midi {

struct FakeMidiOutputProvider : IMidiOutputProvider {
    std::vector<MidiOutDeviceInfo> devices;       // simulated destinations
    int  selected_ = -1;
    bool live_ = false;
    bool drop_all_ = false;                        // simulate a dead endpoint
    std::vector<uasf::MidiEvent> sent;             // captured events
    uint64_t dispatched_ = 0;

    // ── test helpers ──
    void setDevices(std::vector<MidiOutDeviceInfo> d) {
        devices = std::move(d);
        // re-validate current selection
        if (selected_ < 0 || selected_ >= (int)devices.size()) { selected_ = -1; live_ = false; }
    }
    void disconnect() { live_ = false; drop_all_ = true; }   // device vanished
    void reconnect()  { if (selected_ >= 0 && selected_ < (int)devices.size()) { live_ = true; drop_all_ = false; } }

    // ── IMidiOutputProvider ──
    std::vector<MidiOutDeviceInfo> enumerate() override { return devices; }
    bool select(int index) override {
        if (index == -1) { selected_ = -1; live_ = false; return true; }
        if (index < 0 || index >= (int)devices.size()) return false;
        selected_ = index; live_ = true; drop_all_ = false; return true;
    }
    bool selectByUid(int64_t uid) override {
        for (int i = 0; uid != 0 && i < (int)devices.size(); ++i)
            if (devices[i].uid == uid) return select(i);
        return false;
    }
    int  selected() const override { return selected_; }
    bool hasLiveDestination() const override { return live_; }
    bool send(const uasf::MidiEvent& ev) override {
        if (drop_all_ || !live_) return false;     // no-op when not connected
        sent.push_back(ev); ++dispatched_; return true;
    }
    uint64_t dispatchedCount() const override { return dispatched_; }
};

} // namespace ai_arranger::midi

#endif
