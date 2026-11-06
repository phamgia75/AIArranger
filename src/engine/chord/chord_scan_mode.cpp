#include "engine/chord/chord_scan_mode.h"

namespace ai_arranger::chord {

namespace {

// 12-bit pitch-class mask relative to `rootPc`.
uint16_t pcMask(const uint8_t* notes, size_t count, int rootPc) {
    uint16_t mask = 0;
    for (size_t i = 0; i < count; ++i) {
        int pc = (static_cast<int>(notes[i] % 12) - rootPc + 12) % 12;
        mask |= static_cast<uint16_t>(1u << pc);
    }
    return mask;
}

constexpr uint16_t bit(int n) { return static_cast<uint16_t>(1u << n); }

ChordType matchMask(uint16_t m) noexcept {
    auto is = [&](uint16_t t) { return m == t; };

    // 7th chords (4 distinct pitch classes)
    if (is(bit(0)|bit(4)|bit(7)|bit(11))) return ChordType::Maj7;
    if (is(bit(0)|bit(3)|bit(7)|bit(10))) return ChordType::Min7;
    if (is(bit(0)|bit(4)|bit(7)|bit(10))) return ChordType::Dom7;
    if (is(bit(0)|bit(3)|bit(6)|bit(9)))  return ChordType::Dim7;

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

} // namespace

// Chord-engine spec §4.3: exact match over every rotation (inversions), with the
// bass note preferred as root. Candidates are tried lowest note first, so root
// position always wins and symmetric chords (aug, dim7) keep the bass as root.
ChordType matchChordType(const uint8_t* notes, size_t count,
                         uint8_t& rootOut) noexcept {
    rootOut = count ? notes[0] : 0;
    if (count < 2) return ChordType::NoChord;

    for (size_t i = 0; i < count; ++i) {
        const int rootPc = notes[i] % 12;
        bool tried = false;  // same pitch class as an earlier, lower candidate
        for (size_t j = 0; j < i; ++j) tried |= (notes[j] % 12) == rootPc;
        if (tried) continue;
        const ChordType t = matchMask(pcMask(notes, count, rootPc));
        if (t != ChordType::NoChord) { rootOut = notes[i]; return t; }
    }
    return ChordType::NoChord;
}

} // namespace ai_arranger::chord
