#include "engine/chord/chord_scan_mode.h"

namespace ai_arranger::chord {

namespace {

// 12-bit pitch-class mask relative to the lowest note.
uint16_t pcMask(const uint8_t* notes, size_t count) {
    if (count == 0) return 0;
    const int root = notes[0] % 12;
    uint16_t mask = 0;
    for (size_t i = 0; i < count; ++i) {
        int pc = (static_cast<int>(notes[i] % 12) - root + 12) % 12;
        mask |= static_cast<uint16_t>(1u << pc);
    }
    return mask;
}

constexpr uint16_t bit(int n) { return static_cast<uint16_t>(1u << n); }

} // namespace

ChordType matchChordType(const uint8_t* notes, size_t count,
                         uint8_t& rootOut) noexcept {
    rootOut = count ? notes[0] : 0;
    if (count < 2) return ChordType::NoChord;

    const uint16_t m = pcMask(notes, count);
    auto is = [&](uint16_t t) { return m == t; };

    // 9th chords (5 distinct pitch classes — checked before 4-note/3-note subsets)
    if (is(bit(0)|bit(2)|bit(4)|bit(7)|bit(10))) return ChordType::Dom9;
    if (is(bit(0)|bit(2)|bit(4)|bit(7)|bit(11))) return ChordType::Maj9;
    if (is(bit(0)|bit(2)|bit(3)|bit(7)|bit(10))) return ChordType::Min9;

    // 7th chords (4 distinct pitch classes)
    if (is(bit(0)|bit(4)|bit(7)|bit(11))) return ChordType::Maj7;
    if (is(bit(0)|bit(3)|bit(7)|bit(10))) return ChordType::Min7;
    if (is(bit(0)|bit(4)|bit(7)|bit(10))) return ChordType::Dom7;
    if (is(bit(0)|bit(3)|bit(6)|bit(9)))  return ChordType::Dim7;

    // Q3 extended 4-note chords
    if (is(bit(0)|bit(3)|bit(6)|bit(10))) return ChordType::Min7b5;
    if (is(bit(0)|bit(2)|bit(4)|bit(7)))  return ChordType::Add9;
    if (is(bit(0)|bit(4)|bit(7)|bit(9)))  return ChordType::Maj6;
    if (is(bit(0)|bit(3)|bit(7)|bit(9)))  return ChordType::Min6;
    if (is(bit(0)|bit(5)|bit(7)|bit(10))) return ChordType::Dom7Sus4;
    if (is(bit(0)|bit(3)|bit(7)|bit(11))) return ChordType::MinMaj7;
    if (is(bit(0)|bit(4)|bit(8)|bit(10))) return ChordType::Aug7;

    // Common omitted-5th 9th voicings (4 distinct pitch classes)
    if (is(bit(0)|bit(2)|bit(4)|bit(10))) return ChordType::Dom9;
    if (is(bit(0)|bit(2)|bit(4)|bit(11))) return ChordType::Maj9;
    if (is(bit(0)|bit(2)|bit(3)|bit(10))) return ChordType::Min9;

    // triads
    if (is(bit(0)|bit(4)|bit(7))) return ChordType::Major;
    if (is(bit(0)|bit(3)|bit(7))) return ChordType::Minor;
    if (is(bit(0)|bit(3)|bit(6))) return ChordType::Dim;
    if (is(bit(0)|bit(4)|bit(8))) return ChordType::Aug;
    if (is(bit(0)|bit(5)|bit(7))) return ChordType::Sus4;
    if (is(bit(0)|bit(2)|bit(7))) return ChordType::Sus2;

    // dyads
    if (is(bit(0)|bit(7))) return ChordType::Power;

    return ChordType::NoChord;
}

} // namespace ai_arranger::chord
