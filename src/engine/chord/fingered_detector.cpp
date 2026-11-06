#include "engine/chord/fingered_detector.h"

namespace ai_arranger::chord {

Chord FingeredDetector::detect(const uint8_t* notes, size_t count) const noexcept {
    Chord c{ChordType::NoChord, 0, 0, 0};
    uint8_t root = 0;
    c.type = matchChordType(notes, count, root);
    if (c.type == ChordType::NoChord) c.type = partialMatchChordType(notes, count, root);
    c.root = root;
    return c;
}

} // namespace ai_arranger::chord
