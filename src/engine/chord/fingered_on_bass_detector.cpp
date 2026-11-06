#include "engine/chord/fingered_on_bass_detector.h"

namespace ai_arranger::chord {

Chord FingeredOnBassDetector::detect(const uint8_t* notes, size_t count) const noexcept {
    Chord c{ChordType::NoChord, 0, 0, 0};
    if (count == 0) return c;

    const uint8_t bass = notes[0];
    c.bass = bass;

    // The whole set first (bass included): C-E-G-Bb is C7, not "Edim over C".
    uint8_t root = 0;
    c.type = matchChordType(notes, count, root);
    c.root = root;
    if (c.type != ChordType::NoChord) return c;

    // Otherwise a chord above a foreign bass (slash): D + C-E-G = C/D.
    if (count >= 4) {
        const ChordType t = matchChordType(notes + 1, count - 1, root);
        if (t != ChordType::NoChord) { c.type = t; c.root = root; return c; }
    }
    // Last, one note off a template (spec §4.4), whole set.
    c.type = partialMatchChordType(notes, count, root);
    c.root = root;
    return c;
}

} // namespace ai_arranger::chord
