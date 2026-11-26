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
// Existing 0..10 enumerators are preserved bit-for-bit; extended 12..21 chords
// keep degree order (root, 3rd/sus, 5th, 7th/6th/9th, 9th) so C-E-G-B maps to
// the matching harmonic degrees cleanly.
PcSet chordTones(ChordType t) noexcept {
    switch (t) {
        case ChordType::Major:    return {{0, 4, 7}, 3};
        case ChordType::Minor:    return {{0, 3, 7}, 3};
        case ChordType::Dim:      return {{0, 3, 6}, 3};
        case ChordType::Aug:      return {{0, 4, 8}, 3};
        case ChordType::Maj7:     return {{0, 4, 7, 11}, 4};
        case ChordType::Min7:     return {{0, 3, 7, 10}, 4};
        case ChordType::Dom7:     return {{0, 4, 7, 10}, 4};
        case ChordType::Dim7:     return {{0, 3, 6, 9}, 4};
        case ChordType::Sus4:     return {{0, 5, 7}, 3};
        case ChordType::Sus2:     return {{0, 2, 7}, 3};
        case ChordType::Power:    return {{0, 7}, 2};
        case ChordType::NoChord:  return {{0, 4, 7}, 3};
        case ChordType::Min7b5:   return {{0, 3, 6, 10}, 4};
        case ChordType::Add9:     return {{0, 4, 7, 2}, 4};
        case ChordType::Maj6:     return {{0, 4, 7, 9}, 4};
        case ChordType::Min6:     return {{0, 3, 7, 9}, 4};
        case ChordType::Dom9:     return {{0, 4, 7, 10, 2}, 5};
        case ChordType::Maj9:     return {{0, 4, 7, 11, 2}, 5};
        case ChordType::Min9:     return {{0, 3, 7, 10, 2}, 5};
        case ChordType::Dom7Sus4: return {{0, 5, 7, 10}, 4};
        case ChordType::MinMaj7:  return {{0, 3, 7, 11}, 4};
        case ChordType::Aug7:     return {{0, 4, 8, 10}, 4};
    }
    return {{0, 4, 7}, 3};
}

bool isMinorish(ChordType t) noexcept {
    return t == ChordType::Minor   || t == ChordType::Min7 ||
           t == ChordType::Dim     || t == ChordType::Dim7 ||
           t == ChordType::Min7b5  || t == ChordType::Min6 ||
           t == ChordType::Min9    || t == ChordType::MinMaj7;
}

bool isDominantish(ChordType t) noexcept {
    return t == ChordType::Dom7     || t == ChordType::Sus4 ||
           t == ChordType::Sus2     || t == ChordType::Dom9 ||
           t == ChordType::Dom7Sus4 || t == ChordType::Aug7;
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

    // ── On-Bass slash-chord handling (Bass NTT + source root only) ───
    // Zero behavioral change when target.bass == 0 or bass%12 == root%12.
    if (ntt == NttMode::Bass && hasSlashBass(target) && rel == 0) {
        int bassDelta = (static_cast<int>(target.bass) - static_cast<int>(srcRootMidi)) % 12;
        if (bassDelta < 0) bassDelta += 12;
        if (ntr == NtrRule::RootFixed) {
            if (bassDelta > 6) bassDelta -= 12;
        } else if (ntr == NtrRule::RootTransposition && bassDelta > limits.highKey) {
            bassDelta -= 12;
        }
        int result = static_cast<int>(note) + bassDelta;
        if (limits.noteHigh <= 127 && limits.noteHigh >= limits.noteLow + 11) {
            while (result < limits.noteLow)  result += 12;
            while (result > limits.noteHigh) result -= 12;
        }
        while (result < 0)   result += 12;
        while (result > 127) result -= 12;
        return static_cast<uint8_t>(result);
    }

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

size_t transposeVoicing(const uint8_t* inNotes, size_t count,
                        uint8_t* outNotes,
                        Chord target, NtrRule ntr, NttMode ntt,
                        uint8_t srcRootMidi,
                        RegisterLimits limits,
                        VoicingOptions vopt) noexcept {
    if (!inNotes || !outNotes || count == 0) return 0;
    constexpr size_t kMaxVoicing = 16;
    const size_t n = count < kMaxVoicing ? count : kMaxVoicing;

    if (target.type == ChordType::NoChord ||
        ntr == NtrRule::Bypass || ntt == NttMode::Bypass) {
        for (size_t i = 0; i < n; ++i) outNotes[i] = inNotes[i];
        return n;
    }

    const bool hasValidRange = (limits.noteHigh <= 127 && limits.noteHigh >= limits.noteLow + 11);
    const int effLo = hasValidRange ? static_cast<int>(limits.noteLow) : 0;
    const int effHi = hasValidRange ? static_cast<int>(limits.noteHigh) : 127;

    for (size_t i = 0; i < n; ++i) {
        outNotes[i] = transpose(inNotes[i], target, ntr, ntt, srcRootMidi, limits);
    }

    // ── Optional smooth RootFixed voice-leading inversion toward prev_centroid_midi ──
    if (vopt.prev_centroid_midi >= 0 && ntr == NtrRule::RootFixed && n >= 1) {
        const int targetSum = static_cast<int>(vopt.prev_centroid_midi) * static_cast<int>(n);
        int curSum = 0;
        for (size_t i = 0; i < n; ++i) curSum += static_cast<int>(outNotes[i]);
        auto absDiff = [](int a, int b) noexcept { return a > b ? a - b : b - a; };

        // Try whole-octave shift (-12, 0, +12) if all notes stay in bounds.
        int bestShift = 0;
        int bestErr = absDiff(curSum, targetSum);
        for (int shift : {-12, 12}) {
            bool ok = true;
            for (size_t i = 0; i < n; ++i) {
                const int p = static_cast<int>(outNotes[i]) + shift;
                if (p < effLo || p > effHi) { ok = false; break; }
            }
            if (!ok) continue;
            const int err = absDiff(curSum + shift * static_cast<int>(n), targetSum);
            if (err < bestErr) { bestErr = err; bestShift = shift; }
        }
        if (bestShift != 0) {
            for (size_t i = 0; i < n; ++i) {
                outNotes[i] = static_cast<uint8_t>(static_cast<int>(outNotes[i]) + bestShift);
            }
            curSum += bestShift * static_cast<int>(n);
        }

        // Single-voice octave inversion smoothing (raise lowest or lower highest by 12 st).
        if (n >= 2) {
            for (size_t pass = 0; pass < n; ++pass) {
                size_t loIdx = 0, hiIdx = 0;
                for (size_t i = 1; i < n; ++i) {
                    if (outNotes[i] < outNotes[loIdx]) loIdx = i;
                    if (outNotes[i] > outNotes[hiIdx]) hiIdx = i;
                }
                const int curErr = absDiff(curSum, targetSum);
                int bestMove = 0;
                size_t moveIdx = 0;
                int nextErr = curErr;

                if (static_cast<int>(outNotes[loIdx]) + 12 <= effHi) {
                    const uint8_t cand = static_cast<uint8_t>(outNotes[loIdx] + 12);
                    bool dup = false;
                    for (size_t i = 0; i < n; ++i) if (outNotes[i] == cand) { dup = true; break; }
                    const int e = absDiff(curSum + 12, targetSum);
                    if (!dup && e < nextErr) { nextErr = e; bestMove = 12; moveIdx = loIdx; }
                }
                if (static_cast<int>(outNotes[hiIdx]) - 12 >= effLo) {
                    const uint8_t cand = static_cast<uint8_t>(outNotes[hiIdx] - 12);
                    bool dup = false;
                    for (size_t i = 0; i < n; ++i) if (outNotes[i] == cand) { dup = true; break; }
                    const int e = absDiff(curSum - 12, targetSum);
                    if (!dup && e < nextErr) { nextErr = e; bestMove = -12; moveIdx = hiIdx; }
                }
                if (bestMove == 0) break;
                outNotes[moveIdx] = static_cast<uint8_t>(static_cast<int>(outNotes[moveIdx]) + bestMove);
                curSum += bestMove;
            }
        }
    }

    // ── Low-Interval Limit (LIL) mud guard: avoid intervals < 7 st below C2 (36) ──
    // Run BEFORE unison deduplication so lifting a low voice never creates an
    // unchecked duplicate; also requires hiPitch > loPitch so intentional unisons
    // (when avoid_unison_collapse == false) are left intact.
    if (vopt.enforce_lil_mud_guard && n >= 2 && ntt != NttMode::Bass) {
        auto isPitchUsed = [&](uint8_t p, size_t skipIdx) noexcept {
            for (size_t j = 0; j < n; ++j) {
                if (j != skipIdx && outNotes[j] == p) return true;
            }
            return false;
        };
        size_t order[kMaxVoicing];
        for (size_t iter = 0; iter < n * 2; ++iter) {
            for (size_t i = 0; i < n; ++i) order[i] = i;
            // Insertion sort indices by ascending output pitch (stack-only).
            for (size_t i = 1; i < n; ++i) {
                const size_t key = order[i];
                size_t j = i;
                while (j > 0 && outNotes[order[j - 1]] > outNotes[key]) {
                    order[j] = order[j - 1];
                    --j;
                }
                order[j] = key;
            }

            bool fixedAny = false;
            for (size_t k = 0; k + 1 < n; ++k) {
                const size_t loIdx = order[k];
                const size_t hiIdx = order[k + 1];
                const int loPitch = static_cast<int>(outNotes[loIdx]);
                const int hiPitch = static_cast<int>(outNotes[hiIdx]);
                if (loPitch < 36 && hiPitch > loPitch && (hiPitch - loPitch) < 7) {
                    // Prefer +12 if unoccupied (or when collapse-avoidance is disabled),
                    // else +24 if unoccupied and in bounds, else +12 (deduped below).
                    if (hiPitch + 12 <= effHi) {
                        const uint8_t up12 = static_cast<uint8_t>(hiPitch + 12);
                        if (!vopt.avoid_unison_collapse || !isPitchUsed(up12, hiIdx)) {
                            outNotes[hiIdx] = up12;
                        } else if (hiPitch + 24 <= effHi &&
                                   !isPitchUsed(static_cast<uint8_t>(hiPitch + 24), hiIdx)) {
                            outNotes[hiIdx] = static_cast<uint8_t>(hiPitch + 24);
                        } else {
                            outNotes[hiIdx] = up12;
                        }
                        fixedAny = true;
                        break;
                    } else if (loPitch + 12 <= effHi) {
                        outNotes[loIdx] = static_cast<uint8_t>(loPitch + 12);
                        fixedAny = true;
                        break;
                    }
                }
            }
            if (!fixedAny) break;
        }
    }

    // ── Unison-collapse prevention (reassign colliding note to nearest unused chord tone) ──
    if (vopt.avoid_unison_collapse && n >= 2) {
        const PcSet ct = chordTones(target.type);
        const int rootPc = static_cast<int>(target.root % 12);
        auto isPitchUsed = [&](uint8_t p, size_t skipIdx) noexcept {
            for (size_t j = 0; j < n; ++j) {
                if (j != skipIdx && outNotes[j] == p) return true;
            }
            return false;
        };
        auto isPcUsed = [&](int pc, size_t skipIdx) noexcept {
            for (size_t j = 0; j < n; ++j) {
                if (j != skipIdx && static_cast<int>(outNotes[j] % 12) == pc) return true;
            }
            return false;
        };
        auto createsLilMud = [&](int cand, size_t skipIdx) noexcept {
            if (!vopt.enforce_lil_mud_guard || ntt == NttMode::Bass) return false;
            for (size_t j = 0; j < n; ++j) {
                if (j == skipIdx) continue;
                const int other = static_cast<int>(outNotes[j]);
                const int lo = cand < other ? cand : other;
                const int hi = cand < other ? other : cand;
                if (lo < 36 && hi > lo && (hi - lo) < 7) return true;
            }
            return false;
        };

        for (size_t i = 1; i < n; ++i) {
            bool collides = false;
            for (size_t j = 0; j < i; ++j) {
                if (outNotes[j] == outNotes[i]) { collides = true; break; }
            }
            if (!collides) continue;

            const int orig = static_cast<int>(outNotes[i]);
            int bestPitch = -1;
            int bestDist = 1000;
            bool bestUnusedPc = false;
            bool bestNoMud = false;

            // Search all target chord tones across octaves in [effLo, effHi].
            for (int t = 0; t < ct.n; ++t) {
                const int targetPc = (rootPc + ct.v[t]) % 12;
                const bool pcFree = !isPcUsed(targetPc, i);
                for (int oct = 0; oct <= 10; ++oct) {
                    const int cand = oct * 12 + targetPc;
                    if (cand < effLo || cand > effHi) continue;
                    if (isPitchUsed(static_cast<uint8_t>(cand), i)) continue;
                    const int dist = cand > orig ? cand - orig : orig - cand;
                    const bool noMud = !createsLilMud(cand, i);
                    // Priority: (1) no LIL mud, (2) unused pitch class within 12 st, (3) nearest pitch.
                    const bool preferUnusedPc = pcFree && dist <= 12;
                    if (bestPitch < 0 ||
                        (noMud && !bestNoMud) ||
                        (noMud == bestNoMud && preferUnusedPc && !bestUnusedPc) ||
                        (noMud == bestNoMud && preferUnusedPc == bestUnusedPc && dist < bestDist)) {
                        bestPitch = cand;
                        bestDist = dist;
                        bestUnusedPc = preferUnusedPc;
                        bestNoMud = noMud;
                    }
                }
            }
            if (bestPitch >= 0) {
                outNotes[i] = static_cast<uint8_t>(bestPitch);
            }
        }
    }

    return n;
}

void fromCasmRule(uint8_t casmNtr, uint8_t casmNtt, bool nttBass,
                  NtrRule& outNtr, NttMode& outNtt) noexcept {
    const uint8_t nttLow = static_cast<uint8_t>(casmNtt & 0x7Fu);

    switch (casmNtr) {
        case 0:  outNtr = NtrRule::RootTransposition; break;
        case 1:  outNtr = NtrRule::RootFixed;         break;
        case 2:  outNtr = NtrRule::Guitar;            break;
        default: outNtr = NtrRule::RootTransposition; break;
    }

    // SFF2 Guitar NTR (0=All-Purpose, 1=Stroke, 2=Arpeggio) always voices on Guitar NTT.
    if (casmNtr == 2) {
        outNtt = NttMode::Guitar;
        return;
    }

    // ONLY RootFixed + Bypass (NTR=1, NTT=0) freezes pitches (Drums / SFX / Fixed pedal).
    if (casmNtr == 1 && nttLow == 0) {
        outNtr = NtrRule::Bypass;
        outNtt = NttMode::Bypass;
        return;
    }

    // NTT bit 7 set -> Bass-note tracking (On-Bass / Bass NTT), including NTR=0 + NTT=0x80.
    if (nttBass) {
        outNtt = NttMode::Bass;
        return;
    }

    switch (nttLow) {
        case 0:
            // NTR=0 + NTT=0 is parallel root transposition (NOT freeze/No-Transpose).
            outNtt = NttMode::Melody;
            break;
        case 1:  outNtt = NttMode::Melody;        break;
        case 2:  outNtt = NttMode::Chord;         break;
        case 3:  outNtt = NttMode::MelodicMinor;  break;
        case 4:  outNtt = NttMode::HarmonicMinor; break;
        case 5:  outNtt = NttMode::NaturalMinor;  break;
        case 6:  outNtt = NttMode::Dorian;        break;
        default: outNtt = NttMode::Melody;        break;
    }
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
