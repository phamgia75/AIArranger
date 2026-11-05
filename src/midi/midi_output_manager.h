#ifndef AI_ARRANGER_MIDI_MIDI_OUTPUT_MANAGER_H
#define AI_ARRANGER_MIDI_MIDI_OUTPUT_MANAGER_H

#include "midi/midi_output_provider.h"
#include <functional>
#include <string>
#include <vector>

// ── MIDI output manager + state machine (Gate 14C Task B) ────────────
//
// Enumerates output destinations, selects one (remembered by device identity
// for hot-plug when the provider reports one, else by name),
// and tracks a connection state machine:
//   NoDevice → Selected → Connected → Disconnected → (Reconnecting) → Connected
// Provider-abstracted → CI-testable with a synthetic provider. No arranger logic.

namespace ai_arranger::midi {

enum class OutputState { NoDevice, Selected, Connected, Disconnected, Reconnecting };

class MidiOutputManager {
public:
    explicit MidiOutputManager(IMidiOutputProvider& provider) noexcept
        : provider_(provider) {}

    void setOnChanged(std::function<void()> cb) { on_changed_ = std::move(cb); }

    // Re-enumerate and re-resolve the selection (by name). Updates state.
    void refresh();

    // Hot-plug hook (the real MIDINotifyProc calls this). If currently
    // disconnected, transitions through Reconnecting before re-resolving.
    void notifyHotPlug();

    // Select by index into the current device list (-1 = none). Remembers the
    // name so hot-plug can reconnect. Returns false if out of range.
    bool selectDevice(int index);

    OutputState state() const noexcept { return state_; }
    const char* stateName() const noexcept;
    const std::vector<MidiOutDeviceInfo>& devices() const noexcept { return devices_; }
    int selectedIndex() const noexcept { return selected_index_; }
    const std::string& selectedName() const noexcept { return selected_name_; }

private:
    void resolve();   // shared enumerate + re-resolve logic

    IMidiOutputProvider&           provider_;
    std::vector<MidiOutDeviceInfo>  devices_;
    std::string                     selected_name_;   // "" = none
    int64_t                         selected_uid_ = 0; // identity when the provider has one
    int                             selected_index_ = -1;
    OutputState                     state_ = OutputState::NoDevice;
    std::function<void()>           on_changed_;
};

} // namespace ai_arranger::midi

#endif // AI_ARRANGER_MIDI_MIDI_OUTPUT_MANAGER_H
