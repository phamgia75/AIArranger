#include "engine/chord/single_finger_detector.h"

namespace ai_arranger::chord {

Chord SingleFingerDetector::detect(const uint8_t* notes, size_t count) const noexcept {
    Chord c{ChordType::NoChord, 0, 0, 0};
    if (count == 0) return c;

    c.root = notes[0];

    if (count == 1) { c.type = ChordType::Major; return c; }

    if (count == 2) {
        const int iv = (static_cast<int>(notes[1] % 12) -
                        static_cast<int>(notes[0] % 12) + 12) % 12;
        switch (iv) {
            case 3:  c.type = ChordType::Minor; break;
            case 10: c.type = ChordType::Dom7;  break;
            case 7:  c.type = ChordType::Power; break;
            default: c.type = ChordType::Major; break;
        }
        return c;
    }

    // 3+ notes: fall through to the full fingered templates.
    uint8_t root = 0;
    ChordType t = matchChordType(notes, count, root);
    if (t == ChordType::NoChord) t = partialMatchChordType(notes, count, root);
    c.root = root;
    c.type = (t == ChordType::NoChord) ? ChordType::Major : t;
    return c;
}

} // namespace ai_arranger::chord
