#include "engine/music/ntt.h"

namespace ai_arranger::music {
namespace {

// Fixed-size interval set (pitch classes 0..11). All buffers live on the stack
// — no allocation, realtime-safe.
struct PcSet {
    int v[7];
    int n;
};

// Chord tones (intervals from the chord root) for each target chord type.
PcSet chordTones(ChordType t) noexcept {
    switch (t) {
        case ChordType::Major: return {{0, 4, 7}, 3};
        case ChordType::Minor: return {{0, 3, 7}, 3};
        case ChordType::Dim:   return {{0, 3, 6}, 3};
        case ChordType::Aug:   return {{0, 4, 8}, 3};
        case ChordType::Maj7:  return {{0, 4, 7, 11}, 4};
        case ChordType::Min7:  return {{0, 3, 7, 10}, 4};
        case ChordType::Dom7:  return {{0, 4, 7, 10}, 4};
        case ChordType::Dim7:  return {{0, 3, 6, 9}, 4};
        case ChordType::Sus4:  return {{0, 5, 7}, 3};
        case ChordType::Sus2:  return {{0, 2, 7}, 3};
        case ChordType::Power: return {{0, 7}, 2};
        // The 4th tone takes the source maj7's degree: the 6th / b7 replaces it.
        case ChordType::Maj6:   return {{0, 4, 7, 9}, 4};
        case ChordType::Min6:   return {{0, 3, 7, 9}, 4};
        case ChordType::Min7b5: return {{0, 3, 6, 10}, 4};
        case ChordType::Dom9:   return {{0, 4, 7, 10, 2}, 5};   // 9th last: degree map uses 0-3
        default:               return {{0, 4, 7}, 3};
    }
}

bool isMinorish(ChordType t) noexcept {
    return t == ChordType::Minor || t == ChordType::Min7 ||
           t == ChordType::Dim   || t == ChordType::Dim7 ||
           t == ChordType::Min6  || t == ChordType::Min7b5;
}

bool isDominantish(ChordType t) noexcept {
    return t == ChordType::Dom7 || t == ChordType::Dom9 ||
           t == ChordType::Sus4 || t == ChordType::Sus2;
}

// Target scale for an NTT mode + chord type. Scale modes select an explicit
// minor scale; the melodic/chord/bass modes pick a scale by chord quality.
PcSet scaleFor(NttMode mode, ChordType t) noexcept {
    switch (mode) {
        case NttMode::MelodicMinor:  return {{0, 2, 3, 5, 7, 9, 11}, 7};
        case NttMode::HarmonicMinor: return {{0, 2, 3, 5, 7, 8, 11}, 7};
        case NttMode::NaturalMinor:  return {{0, 2, 3, 5, 7, 8, 10}, 7};
        case NttMode::Dorian:        return {{0, 2, 3, 5, 7, 9, 10}, 7};
        default:
            // Chord-specific scales where natural minor would clash with a chord tone.
            if (t == ChordType::Min6)   return {{0, 2, 3, 5, 7, 9, 10}, 7}; // dorian (natural 6)
            if (t == ChordType::Min7b5) return {{0, 1, 3, 5, 6, 8, 10}, 7}; // locrian (b5)
            if (isMinorish(t))    return {{0, 2, 3, 5, 7, 8, 10}, 7}; // natural minor
            if (isDominantish(t)) return {{0, 2, 4, 5, 7, 9, 10}, 7}; // mixolydian
            return {{0, 2, 4, 5, 7, 9, 11}, 7};                       // major
    }
}

// Source reference chord tones (Yamaha SFF convention: C maj7 reference).
constexpr PcSet kSourceChordTones = {{0, 4, 7, 11}, 4};

int indexOf(const PcSet& s, int pc) noexcept {
    for (int i = 0; i < s.n; ++i)
        if (s.v[i] == pc) return i;
    return -1;
}

bool contains(const PcSet& s, int pc) noexcept {
    return indexOf(s, pc) >= 0;
}

// Nearest representative of `rel` drawn from `s`, allowing the chosen tone to
// fall in the octave below/above so the result is the minimal *signed* shift
// (e.g. a maj7 over a triad resolves up to the root, not down a major 7th).
int nearestRepresentative(int rel, const PcSet& s) noexcept {
    int best = rel;
    int bestDist = 1000;
    for (int i = 0; i < s.n; ++i) {
        for (int k = -12; k <= 12; k += 12) {
            const int cand = s.v[i] + k;
            const int dist = cand > rel ? cand - rel : rel - cand;
            if (dist < bestDist) { bestDist = dist; best = cand; }
        }
    }
    return best;
}

} // namespace

uint8_t transpose(uint8_t note, Chord target,
                  NtrRule ntr, NttMode ntt,
                  uint8_t srcRootMidi, RegisterLimits limits) noexcept {
    if (target.type == ChordType::NoChord) return note;
    if (ntr == NtrRule::Bypass || ntt == NttMode::Bypass) return note;

    const PcSet ct = chordTones(target.type);
    const PcSet sc = scaleFor(ntt, target.type);

    int rel = (static_cast<int>(note) - static_cast<int>(srcRootMidi)) % 12;
    if (rel < 0) rel += 12;

    // ── NTT remap: source pitch class -> target pitch class (signed) ──
    int dstRel;
    const int degree = indexOf(kSourceChordTones, rel);
    if (degree >= 0) {
        // Source chord tone -> target chord tone of the same degree.
        dstRel = ct.v[degree < ct.n ? degree : ct.n - 1];
    } else if (contains(sc, rel)) {
        // Valid tension in the target scale -> keep it.
        dstRel = rel;
    } else {
        // Non-scale tone -> snap to the nearest chord/scale tone.
        const int nc = nearestRepresentative(rel, ct);
        const int ns = nearestRepresentative(rel, sc);
        const int dc = nc > rel ? nc - rel : rel - nc;
        const int ds = ns > rel ? ns - rel : rel - ns;
        dstRel = (dc <= ds) ? nc : ns;
    }

    // Bass, Chord and Guitar modes pull every note onto a target chord tone
    // (guitar comping voices the chord, like chord mode but on its own track so
    // the SFF2 Guitar NTR can drive register/voicing separately).
    if (ntt == NttMode::Bass || ntt == NttMode::Chord || ntt == NttMode::Guitar) {
        dstRel = nearestRepresentative(dstRel, ct);
    }

    // ── NTR root handling ────────────────────────────────────────────
    // Only the root's pitch class transposes (up 0..11 from the source root): the
    // detectors report the lowest played note, so F2 and F4 are the same chord
    // and must not move the pattern by octaves.
    int rootDelta = 0;
    if (ntr != NtrRule::RootFixed) {
        rootDelta = (static_cast<int>(target.root) - static_cast<int>(srcRootMidi)) % 12;
        if (rootDelta < 0) rootDelta += 12;
        // High Key (Yamaha: "chords with a root higher than the limit are played
        // in the octave immediately below"); 11 = never.
        if (ntr == NtrRule::RootTransposition && rootDelta > limits.highKey) rootDelta -= 12;
    }

    int result = static_cast<int>(note) + rootDelta + (dstRel - rel);

    // Note Limit: nearest octave inside the range. A range under an octave
    // cannot hold every pitch class, so it is ignored (the importer never sets one).
    if (limits.noteHigh <= 127 && limits.noteHigh >= limits.noteLow + 11) {
        while (result < limits.noteLow)  result += 12;
        while (result > limits.noteHigh) result -= 12;
    }

    // Fold into valid MIDI range without losing pitch class.
    while (result < 0)   result += 12;
    while (result > 127) result -= 12;
    return static_cast<uint8_t>(result);
}

void defaultRuleForRole(uasf::TrackRole role,
                        NtrRule& ntr, NttMode& ntt) noexcept {
    switch (role) {
        case uasf::TrackRole::Drum:
        case uasf::TrackRole::Percussion:
            ntr = NtrRule::Bypass; ntt = NttMode::Bypass; break;
        case uasf::TrackRole::Bass:
            ntr = NtrRule::RootTransposition; ntt = NttMode::Bass; break;
        case uasf::TrackRole::Chord:
        case uasf::TrackRole::Pad:
            ntr = NtrRule::RootTransposition; ntt = NttMode::Chord; break;
        default:
            // Phrase1/Phrase2/Melody/Accompaniment/Articulation
            ntr = NtrRule::RootTransposition; ntt = NttMode::Melody; break;
    }
}

} // namespace ai_arranger::music
