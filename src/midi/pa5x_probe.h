// Pure helpers for tools/pa5x_probe.cpp (hardware test driver). No CoreMIDI
// here, so the logic is unit-tested without a device (tests/midi/test_pa5x_probe).
#ifndef AI_ARRANGER_MIDI_PA5X_PROBE_H
#define AI_ARRANGER_MIDI_PA5X_PROBE_H

#include "engine/arranger/chord_input.h"
#include "midi/midi_input_parser.h"
#include "midi/pa5x_control.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace ai_arranger::midi::pa5x::probe {

/// Keys held per channel, from received Note On/Off (Note On velocity 0 = Off).
struct HeldNotes {
    std::array<std::array<bool, 128>, 16> on{};

    /// Returns true when the message changed the held set.
    bool apply(const MidiInputMessage& m) noexcept {
        if (m.channel > 15 || m.data1 > 127) return false;
        const bool down = m.type == MidiInMsgType::NoteOn && m.data2 > 0;
        const bool up = m.type == MidiInMsgType::NoteOff || (m.type == MidiInMsgType::NoteOn && m.data2 == 0);
        if (!down && !up) return false;
        on[m.channel][m.data1] = down;
        return true;
    }
    /// Ascending held notes on `channel` strictly below `split`; returns the count.
    size_t below(uint8_t channel, uint8_t split, uint8_t* out, size_t cap) const noexcept {
        size_t n = 0;
        for (int k = 0; k < split && k < 128 && n < cap; ++k)
            if (on[channel & 15][k]) out[n++] = static_cast<uint8_t>(k);
        return n;
    }
};

inline const char* chordTypeName(arranger::ChordType t) noexcept {
    using T = arranger::ChordType;
    switch (t) {
        case T::Major: return "maj";   case T::Minor: return "m";     case T::Dim: return "dim";
        case T::Aug: return "aug";     case T::Maj7: return "maj7";   case T::Min7: return "m7";
        case T::Dom7: return "7";      case T::Dim7: return "dim7";   case T::Sus4: return "sus4";
        case T::Sus2: return "sus2";   case T::Power: return "5";     case T::NoChord: return "NoChord";
        case T::Maj6: return "6";      case T::Min6: return "m6";     case T::Dom9: return "9";
        case T::Min7b5: return "m7b5";
    }
    return "?";
}

inline const char* pitchName(uint8_t note) noexcept {
    static const char* n[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    return n[note % 12];
}

/// "1".."16" -> 0..15; anything else -> nullopt.
inline std::optional<uint8_t> parseChannel(const std::string& s) noexcept {
    if (s.empty() || s.size() > 2 || s.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    const int v = std::atoi(s.c_str());
    if (v < 1 || v > 16) return std::nullopt;
    return static_cast<uint8_t>(v - 1);
}

/// Control-channel command by name (case-sensitive, as printed by `--help`).
inline std::optional<Command> commandFromName(const std::string& s) noexcept {
    static const struct { const char* name; Command cmd; } table[] = {
        {"variation1", Command::Variation1}, {"variation2", Command::Variation2},
        {"variation3", Command::Variation3}, {"variation4", Command::Variation4},
        {"fill1", Command::Fill1}, {"fill2", Command::Fill2}, {"fill3", Command::Fill3}, {"fill4", Command::Fill4},
        {"break", Command::Break},
        {"intro1", Command::Intro1}, {"intro2", Command::Intro2},
        {"ending1", Command::Ending1}, {"ending2", Command::Ending2}, {"ending3", Command::Ending3},
        {"playstop1", Command::PlayStopPlayer1}, {"playstop2", Command::PlayStopPlayer2},
    };
    for (const auto& e : table) if (s == e.name) return e.cmd;
    return std::nullopt;
}

} // namespace ai_arranger::midi::pa5x::probe

#endif
