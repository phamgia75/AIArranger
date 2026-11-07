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

    // 9th (5 distinct pitch classes)
    if (is(bit(0)|bit(2)|bit(4)|bit(7)|bit(10))) return ChordType::Dom9;

    // 7th and 6th chords (4 distinct pitch classes). C6 = Am7 and Cm6 = Am7b5 as
    // sets; the caller tries roots lowest note first, so the bass decides.
    if (is(bit(0)|bit(4)|bit(7)|bit(11))) return ChordType::Maj7;
    if (is(bit(0)|bit(3)|bit(7)|bit(10))) return ChordType::Min7;
    if (is(bit(0)|bit(4)|bit(7)|bit(10))) return ChordType::Dom7;
    if (is(bit(0)|bit(3)|bit(6)|bit(9)))  return ChordType::Dim7;
    if (is(bit(0)|bit(3)|bit(6)|bit(10))) return ChordType::Min7b5;
    if (is(bit(0)|bit(4)|bit(7)|bit(9)))  return ChordType::Maj6;
    if (is(bit(0)|bit(3)|bit(7)|bit(9)))  return ChordType::Min6;

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

// Tie-break between non-bass roots: the order of chord-engine spec §5.1 (MVP
// table: maj, min, 7, maj7, m7, sus4, dim, aug, 6, m6, 9), then the phase-2
// types (§5.2) and the dyad. Lower = preferred.
int priority(ChordType t) noexcept {
    switch (t) {
        case ChordType::Major: return 0;   case ChordType::Minor: return 1;
        case ChordType::Dom7:  return 2;   case ChordType::Maj7:  return 3;
        case ChordType::Min7:  return 4;   case ChordType::Sus4:  return 5;
        case ChordType::Dim:   return 6;   case ChordType::Aug:   return 7;
        case ChordType::Maj6:  return 8;   case ChordType::Min6:  return 9;
        case ChordType::Dom9:  return 10;  case ChordType::Dim7:  return 11;
        case ChordType::Min7b5: return 12; case ChordType::Sus2:  return 13;
        case ChordType::Power: return 14;
        default:               return 99;
    }
}

// Templates for partial match (no dyads: a fifth plus one note is not a chord).
struct Template { ChordType type; uint16_t mask; };
constexpr Template kTemplates[] = {
    {ChordType::Major, bit(0)|bit(4)|bit(7)},  {ChordType::Minor, bit(0)|bit(3)|bit(7)},
    {ChordType::Dom7, bit(0)|bit(4)|bit(7)|bit(10)}, {ChordType::Maj7, bit(0)|bit(4)|bit(7)|bit(11)},
    {ChordType::Min7, bit(0)|bit(3)|bit(7)|bit(10)}, {ChordType::Sus4, bit(0)|bit(5)|bit(7)},
    {ChordType::Dim, bit(0)|bit(3)|bit(6)},    {ChordType::Aug, bit(0)|bit(4)|bit(8)},
    {ChordType::Maj6, bit(0)|bit(4)|bit(7)|bit(9)},  {ChordType::Min6, bit(0)|bit(3)|bit(7)|bit(9)},
    {ChordType::Dom9, bit(0)|bit(2)|bit(4)|bit(7)|bit(10)}, {ChordType::Dim7, bit(0)|bit(3)|bit(6)|bit(9)},
    {ChordType::Min7b5, bit(0)|bit(3)|bit(6)|bit(10)}, {ChordType::Sus2, bit(0)|bit(2)|bit(7)},
};

int popcount12(uint16_t m) noexcept {
    int n = 0;
    for (; m; m &= static_cast<uint16_t>(m - 1)) ++n;
    return n;
}

} // namespace

// Chord-engine spec §4.4. Only one deviation from a template is accepted: one
// chord tone missing OR one extra note, with the root among the held notes.
// score = matched*2 - extra - missing*3; ties: the bass as root, then spec §5.1
// order, then the root closest above the bass (never the upper-voice order).
ChordType partialMatchChordType(const uint8_t* notes, size_t count,
                                uint8_t& rootOut) noexcept {
    rootOut = count ? notes[0] : 0;
    if (count < 3) return ChordType::NoChord;
    const int bassPc = notes[0] % 12;
    if (popcount12(pcMask(notes, count, 0)) < 3) return ChordType::NoChord;

    ChordType best = ChordType::NoChord;
    int bestScore = 0, bestRel = 0;
    for (size_t i = 0; i < count; ++i) {
        const int rootPc = notes[i] % 12;
        bool tried = false;
        for (size_t j = 0; j < i; ++j) tried |= (notes[j] % 12) == rootPc;
        if (tried) continue;
        const uint16_t in = pcMask(notes, count, rootPc);
        const int rel = (rootPc - bassPc + 12) % 12;   // 0 = the bass is the root
        for (const auto& t : kTemplates) {
            const int extra = popcount12(in & static_cast<uint16_t>(~t.mask));
            const int missing = popcount12(t.mask & static_cast<uint16_t>(~in));
            if (extra + missing != 1) continue;
            const int score = popcount12(in & t.mask) * 2 - extra - missing * 3;
            const bool better =
                best == ChordType::NoChord || score > bestScore ||
                (score == bestScore &&
                 ((rel == 0) != (bestRel == 0) ? rel == 0
                  : priority(t.type) != priority(best) ? priority(t.type) < priority(best)
                  : rel < bestRel));
            if (better) { best = t.type; bestScore = score; bestRel = rel; rootOut = notes[i]; }
        }
    }
    return best;
}

// Chord-engine spec §4.3: exact match over every rotation (inversions). The bass
// note is the root whenever its pitch class matches (root position always wins;
// symmetric aug/dim7 keep the bass; C-E-G-A = C6 but A-C-E-G = Am7). Otherwise
// the type earliest in the spec's table wins, so the answer depends only on the
// bass and the pitch-class set, never on the order of the upper voices
// (E-G-A-C and E-G-C-A are both Am7/E; Eb-G-A-C is Cm6/Eb).
ChordType matchChordType(const uint8_t* notes, size_t count,
                         uint8_t& rootOut) noexcept {
    rootOut = count ? notes[0] : 0;
    if (count < 2) return ChordType::NoChord;

    ChordType best = ChordType::NoChord;
    for (size_t i = 0; i < count; ++i) {
        const int rootPc = notes[i] % 12;
        bool tried = false;  // same pitch class as an earlier, lower candidate
        for (size_t j = 0; j < i; ++j) tried |= (notes[j] % 12) == rootPc;
        if (tried) continue;
        const ChordType t = matchMask(pcMask(notes, count, rootPc));
        if (t == ChordType::NoChord) continue;
        if (i == 0) { rootOut = notes[0]; return t; }   // bass is the root
        if (best == ChordType::NoChord || priority(t) < priority(best)) {
            best = t;
            rootOut = notes[i];   // lowest note of this pitch class
        }
    }
    return best;
}

} // namespace ai_arranger::chord
