#ifndef AI_ARRANGER_MIDI_PA5X_PROFILE_H
#define AI_ARRANGER_MIDI_PA5X_PROFILE_H

#include "midi/midi_input_router.h"   // ChannelRoles
#include "control/midi_output_bridge.h"  // OutputChannelMap
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

// Chord-recognition roles for the "Pa5X KEYBOARD" port under the Pa5X factory
// "Default" MIDI preset (User Manual v1.4 p.585-588): MIDI OUT ch1-3 = Upper
// 1-3, ch4 = Lower. The Lower part only exists with SPLIT on at the Pa5X, so its
// notes are chord notes (split already applied by the Pa5X); Upper parts never
// feed chord detection; channels the preset does not send are ignored.
// shortcut: assumes the factory Default preset; a user-edited preset needs its own roles.
inline ChannelRoles defaultPresetKeyboardRoles() noexcept {
    ChannelRoles r;
    for (auto& x : r.role) x = ChannelRole::Ignore;
    r.role[3] = ChannelRole::Chord;   // ch4 Lower
    return r;
}

// Live output for a Genos/Yamaha SFF1 style onto the Pa5X Style tracks (Pa5X
// User Manual v1.4 p.1035: Bass 9, Drum 10, Percussion 11, Acc1-5 12-16; Default
// MIDI preset: ch9-16 double as Style tracks, p.585-588). Same part mapping as the
// marker-SMF exporter (exporters/korg korgChannelFor; a test keeps them equal):
// Rhythm1 9 -> Perc 11, Rhythm2 10 -> Drum 10, Bass 11 -> Bass 9, Chord1..Phrase2
// 12-16 -> Acc1-5. Channels 1-8 are dropped, and so are Yamaha Bank Select /
// Program Change, which select unrelated sounds on a Pa5X.
inline control::OutputChannelMap genosStyleTrackMap() noexcept {
    control::OutputChannelMap m;
    for (auto& d : m.dst) d = control::OutputChannelMap::kDrop;
    m.dst[8] = 10;  m.dst[9] = 9;  m.dst[10] = 8;
    for (uint8_t ch = 11; ch < 16; ++ch) m.dst[ch] = ch;
    m.dropVoiceSelect = true;
    return m;
}

// The built-in demo style (buildDemoStyle: Drums index 9, Bass index 1, Chord index 2) onto
// the Pa5X Style-track receive channels of the "Ai Arranger" preset (photos 2026-11-07:
// ch 9 STY Bass, ch 10 STY Drum, ch 12 STY Acc1). With the identity map the demo bass
// (ch 2) and chord (ch 3) land on channels that preset has Off. Everything else is dropped.
inline control::OutputChannelMap demoStyleTrackMap() noexcept {
    control::OutputChannelMap m;
    for (auto& d : m.dst) d = control::OutputChannelMap::kDrop;
    m.dst[1] = 8;  m.dst[9] = 9;  m.dst[2] = 11;
    m.dropVoiceSelect = true;
    return m;
}

} // namespace ai_arranger::midi::pa5x

#endif // AI_ARRANGER_MIDI_PA5X_PROFILE_H
