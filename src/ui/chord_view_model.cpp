#include "ui/chord_view_model.h"

namespace ai_arranger::ui {

std::string formatChordName(const arranger::Chord& chord) {
    using arranger::ChordType;
    static const char* kNames[12] =
        {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (chord.type == ChordType::NoChord) return "--";

    std::string s = kNames[chord.root % 12];
    switch (chord.type) {
        case ChordType::Major:                    break;       // "C"
        case ChordType::Minor:    s += "m";       break;
        case ChordType::Dim:      s += "dim";     break;
        case ChordType::Aug:      s += "aug";     break;
        case ChordType::Maj7:     s += "maj7";    break;
        case ChordType::Min7:     s += "m7";      break;
        case ChordType::Dom7:     s += "7";       break;
        case ChordType::Dim7:     s += "dim7";    break;
        case ChordType::Sus4:     s += "sus4";    break;
        case ChordType::Sus2:     s += "sus2";    break;
        case ChordType::Power:    s += "5";       break;
        case ChordType::NoChord:  return "--";
        case ChordType::Min7b5:   s += "m7b5";    break;
        case ChordType::Add9:     s += "add9";    break;
        case ChordType::Maj6:     s += "6";       break;
        case ChordType::Min6:     s += "m6";      break;
        case ChordType::Dom9:     s += "9";       break;
        case ChordType::Maj9:     s += "maj9";    break;
        case ChordType::Min9:     s += "m9";      break;
        case ChordType::Dom7Sus4: s += "7sus4";   break;
        case ChordType::MinMaj7:  s += "mMaj7";   break;
        case ChordType::Aug7:     s += "aug7";    break;
    }
    if (chord.bass != 0 && (chord.bass % 12) != (chord.root % 12)) {
        s += '/';
        s += kNames[chord.bass % 12];
    }
    return s;
}

std::string ChordViewModel::displayName() const {
    const arranger::Chord c{static_cast<arranger::ChordType>(type_), root_, 0, 0};
    return formatChordName(c);
}

} // namespace ai_arranger::ui
