#ifndef AI_ARRANGER_MUSIC_NTT_H
#define AI_ARRANGER_MUSIC_NTT_H

#include "engine/arranger/chord_input.h"  // Chord, ChordType
#include "engine/uasf/types.h"            // uasf::TrackRole
#include <cstdint>

// ── Note Transposition (NTR + NTT) ───────────────────────────────────
//
// Vendor-agnostic implementation of the chord transform described in
// arranger-engine-spec-v0.9.md §6. A recorded note (relative to the style's
// source reference chord — C major/maj7 by convention) is transposed into the
// chord the user is currently playing:
//
//   1. NTR (Note Transposition Rule) handles the root: shift the whole pattern
//      by the root interval (RootTransposition) or keep it in place (RootFixed).
//   2. NTT (Note Transposition Table) remaps each pitch class to fit the target
//      chord *quality* (major/minor/dom7/…) and, for scale modes, the target
//      scale (melodic/harmonic/natural minor, dorian).
//
// The transform is PURE and REALTIME-SAFE: it computes on stack-only fixed-size
// buffers, performs no allocation, takes no locks, and makes no syscalls, so it
// is safe to call from the dispatch / audio path.

namespace ai_arranger::music {

using arranger::Chord;
using arranger::ChordType;

// Note Transposition Rule — how the root is handled (UASF-neutral).
enum class NtrRule : uint8_t {
    RootTransposition = 0,  // shift up by the root pitch-class interval (0..11); keep contour
    RootFixed         = 1,  // no root shift; only the NTT remap applies
    Guitar            = 2,  // SFF2; v1 falls back to RootTransposition
    Fifth             = 3,  // root+5th rule; v1 falls back to RootTransposition
    Bypass            = 4,  // no transposition at all (drums / SFX)
};

// Note Transposition Table — how chord quality / scale is remapped.
enum class NttMode : uint8_t {
    Bypass = 0,     // map[i] = i (identity, NTR only)
    Melody,         // chord tone -> chord tone; scale tone kept; else nearest
    Chord,          // force every note onto a target chord tone
    Bass,           // bass-optimised: root->root, fifth->fifth, snap to chord
    Guitar,         // SFF2 guitar comping: voice onto target chord tones
    MelodicMinor,
    HarmonicMinor,
    NaturalMinor,
    Dorian,
};

// Transpose one recorded note into `target`. `srcRootMidi` is the MIDI root of
// the style's source reference chord (60 = C by convention). Returns a clamped
// MIDI note (0..127).
uint8_t transpose(uint8_t note, Chord target,
                  NtrRule ntr, NttMode ntt,
                  uint8_t srcRootMidi = 60) noexcept;

// Default (NTR, NTT) pair for a track role, used when a track carries no
// explicit CASM transposition metadata. Vendor-agnostic mapping.
void defaultRuleForRole(uasf::TrackRole role,
                        NtrRule& ntr, NttMode& ntt) noexcept;

} // namespace ai_arranger::music

#endif // AI_ARRANGER_MUSIC_NTT_H
