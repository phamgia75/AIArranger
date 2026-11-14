#ifndef AI_ARRANGER_SESSION_CHORD_LABEL_H
#define AI_ARRANGER_SESSION_CHORD_LABEL_H

// Display name of a chord from the engine snapshot fields (chordType, chordRoot, chordBass):
// "C", "Am7", "C/E". The slash shows only when the bass pitch class differs from the root's
// (a C2 bass under a C4 root is root position). NoChord -> "--". Portable, no allocation
// beyond the returned string.

#include "engine/arranger/chord_input.h"
#include <cstdint>
#include <string>

namespace ai_arranger::session {

inline const char* chordSuffix(arranger::ChordType t) noexcept {
    using CT = arranger::ChordType;
    switch (t) {
        case CT::Major: return "";       case CT::Minor: return "m";
        case CT::Dim:   return "dim";    case CT::Aug:   return "aug";
        case CT::Maj7:  return "maj7";   case CT::Min7:  return "m7";
        case CT::Dom7:  return "7";      case CT::Dim7:  return "dim7";
        case CT::Sus4:  return "sus4";   case CT::Sus2:  return "sus2";
        case CT::Power: return "5";      case CT::Maj6:  return "6";
        case CT::Min6:  return "m6";     case CT::Dom9:  return "9";
        case CT::Min7b5: return "m7b5";  default:        return "";
    }
}

inline std::string chordLabel(uint8_t type, uint8_t root, uint8_t bass) {
    static const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const auto t = static_cast<arranger::ChordType>(type);
    if (t == arranger::ChordType::NoChord) return "--";
    std::string name = kNoteNames[root % 12];
    name += chordSuffix(t);
    if (bass != 0 && bass % 12 != root % 12) { name += '/'; name += kNoteNames[bass % 12]; }
    return name;
}

} // namespace ai_arranger::session

#endif // AI_ARRANGER_SESSION_CHORD_LABEL_H
