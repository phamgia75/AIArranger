#include "ui/chord_view_model.h"

namespace ai_arranger::ui {

std::string ChordViewModel::displayName() const {
    using arranger::ChordType;
    static const char* kNames[12] =
        {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    const auto t = static_cast<ChordType>(type_);
    if (t == ChordType::NoChord) return "--";

    std::string s = kNames[root_ % 12];
    switch (t) {
        case ChordType::Major:                 break;       // "C"
        case ChordType::Minor: s += "m";       break;
        case ChordType::Dim:   s += "dim";     break;
        case ChordType::Aug:   s += "aug";     break;
        case ChordType::Maj7:  s += "maj7";    break;
        case ChordType::Min7:  s += "m7";      break;
        case ChordType::Dom7:  s += "7";       break;
        case ChordType::Dim7:  s += "dim7";    break;
        case ChordType::Sus4:  s += "sus4";    break;
        case ChordType::Sus2:  s += "sus2";    break;
        case ChordType::Power: s += "5";       break;
        case ChordType::Maj6:  s += "6";       break;
        case ChordType::Min6:  s += "m6";      break;
        case ChordType::Dom9:  s += "9";       break;
        case ChordType::Min7b5: s += "m7b5";   break;
        default:                                break;
    }
    return s;
}

} // namespace ai_arranger::ui
