#include "midi/midi_output_manager.h"

namespace ai_arranger::midi {

const char* MidiOutputManager::stateName() const noexcept {
    switch (state_) {
        case OutputState::NoDevice:    return "NO_DEVICE";
        case OutputState::Selected:    return "SELECTED";
        case OutputState::Connected:   return "CONNECTED";
        case OutputState::Disconnected:return "DISCONNECTED";
        case OutputState::Reconnecting:return "RECONNECTING";
    }
    return "?";
}

void MidiOutputManager::resolve() {
    devices_ = provider_.enumerate();

    if (selected_name_.empty()) {
        provider_.select(-1);
        selected_index_ = -1;
        state_ = OutputState::NoDevice;
        return;
    }

    // Re-resolve the remembered device by identity when known: a different device
    // that merely shares the name is never adopted (hardware safety).
    int idx = -1;
    for (int i = 0; i < (int)devices_.size(); ++i) {
        const bool same = selected_uid_ != 0 ? devices_[i].uid == selected_uid_
                                             : devices_[i].name == selected_name_;
        if (same) { idx = i; break; }
    }

    if (idx < 0) {
        // Remembered device is gone — stay "armed" by name, report disconnected.
        provider_.select(-1);
        selected_index_ = -1;
        state_ = OutputState::Disconnected;
        return;
    }

    provider_.select(idx);
    selected_index_ = idx;
    state_ = provider_.hasLiveDestination() ? OutputState::Connected
                                            : OutputState::Selected;
}

void MidiOutputManager::refresh() {
    resolve();
    if (on_changed_) on_changed_();
}

void MidiOutputManager::notifyHotPlug() {
    if (state_ == OutputState::Disconnected) {
        state_ = OutputState::Reconnecting;
        if (on_changed_) on_changed_();
    }
    resolve();
    if (on_changed_) on_changed_();
}

bool MidiOutputManager::selectDevice(int index) {
    if (index == -1) {
        selected_name_.clear();
        selected_uid_ = 0;
        provider_.select(-1);
        selected_index_ = -1;
        state_ = OutputState::NoDevice;
        if (on_changed_) on_changed_();
        return true;
    }
    if (index < 0 || index >= (int)devices_.size()) return false;

    selected_name_ = devices_[index].name;
    selected_uid_  = devices_[index].uid;
    provider_.select(index);
    selected_index_ = index;
    state_ = provider_.hasLiveDestination() ? OutputState::Connected
                                            : OutputState::Selected;
    if (on_changed_) on_changed_();
    return true;
}

} // namespace ai_arranger::midi
