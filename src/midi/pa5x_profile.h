#ifndef AI_ARRANGER_MIDI_PA5X_PROFILE_H
#define AI_ARRANGER_MIDI_PA5X_PROFILE_H

#include <cctype>
#include <string_view>

// ── Korg Pa5X USB-MIDI port profile (vendor data, no I/O) ───────────────────
//
// Pa5X User Manual v1.4 p.582: with the KORG USB-MIDI driver the computer sees
//   "Pa5X KEYBOARD"  data from the Pa5X to the computer  (a CoreMIDI source)
//   "Pa5X SOUND"     data from the computer to the Pa5X  (a CoreMIDI destination)
// The same names were seen on macOS 26.1 on 2026-11-05 (hardware audit). Names
// under a class-compliant driver are unverified, so any other endpoint whose name
// contains "pa5x" is reported as a candidate, never auto-selected.

namespace ai_arranger::midi::pa5x {

inline constexpr std::string_view kSoundPort    = "Pa5X SOUND";
inline constexpr std::string_view kKeyboardPort = "Pa5X KEYBOARD";

enum class PortMatch { None, Sound, Keyboard, Candidate };

inline bool containsPa5x(std::string_view name) noexcept {
    constexpr std::string_view key = "pa5x";
    for (size_t i = 0; i + key.size() <= name.size(); ++i) {
        size_t k = 0;
        while (k < key.size() &&
               std::tolower(static_cast<unsigned char>(name[i + k])) == key[k]) ++k;
        if (k == key.size()) return true;
    }
    return false;
}

// `isSource`: CoreMIDI source (data from the device) vs destination.
inline PortMatch classify(std::string_view name, bool isSource) noexcept {
    if (isSource && name == kKeyboardPort) return PortMatch::Keyboard;
    if (!isSource && name == kSoundPort) return PortMatch::Sound;
    return containsPa5x(name) ? PortMatch::Candidate : PortMatch::None;
}

} // namespace ai_arranger::midi::pa5x

#endif // AI_ARRANGER_MIDI_PA5X_PROFILE_H
