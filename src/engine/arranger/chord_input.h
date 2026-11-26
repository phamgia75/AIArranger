#ifndef AI_ARRANGER_ARRANGER_CHORD_INPUT_H
#define AI_ARRANGER_ARRANGER_CHORD_INPUT_H

#include "engine/uasf/types.h"
#include <cstdint>
#include <array>
#include <atomic>

namespace ai_arranger::arranger {

/**
 * Chord representation.
 * Root is MIDI note number (0-127), 60 = C4.
 * Type is a simple enum for common chord types.
 */

enum class ChordType : uint8_t {
    Major    = 0,
    Minor    = 1,
    Dim      = 2,
    Aug      = 3,
    Maj7     = 4,
    Min7     = 5,
    Dom7     = 6,
    Dim7     = 7,
    Sus4     = 8,
    Sus2     = 9,
    Power    = 10,
    NoChord  = 11,   // PINNED at 11 for binary/snapshot ABI compatibility
    // ── Q3 Extended Arranger Chords (appended safely at 12..21) ──
    Min7b5   = 12,   // Half-diminished: {0, 3, 6, 10}
    Add9     = 13,   // Major add9:      {0, 2, 4, 7}
    Maj6     = 14,   // Major 6th:       {0, 4, 7, 9}
    Min6     = 15,   // Minor 6th:       {0, 3, 7, 9}
    Dom9     = 16,   // Dominant 9th:    {0, 2, 4, 7, 10}
    Maj9     = 17,   // Major 9th:       {0, 2, 4, 7, 11}
    Min9     = 18,   // Minor 9th:       {0, 2, 3, 7, 10}
    Dom7Sus4 = 19,   // Dominant 7sus4:  {0, 5, 7, 10}
    MinMaj7  = 20,   // Minor-Major 7th: {0, 3, 7, 11}
    Aug7     = 21,   // Augmented 7th:   {0, 4, 8, 10}
};

struct Chord {
    ChordType type;
    uint8_t   root;    // 0-127 (60 = C4)
    uint8_t   bass;    // Bass note (0 = root)
    int16_t   octave;  // Octave offset (0 = default)
};

/**
 * Simulated chord input for Gate 2.
 * Allows setting chords from code (no MIDI input yet).
 */
class ChordInput {
public:
    ChordInput();

    // Set current chord (non-realtime)
    void setChord(Chord chord) noexcept;
    
    // Get current chord (realtime-safe, atomic)
    Chord getChord() const noexcept;

    // Chord progression helpers
    void setNextChord(int index) noexcept;
    void setProgression(const Chord* chords, size_t count) noexcept;
    Chord nextProgressionChord() noexcept;

private:
    std::atomic<uint32_t> chord_data_{0}; // Packed: type(8) | root(8) | bass(8) | octave(8)

    // Progression
    Chord* progression_{nullptr};
    size_t progression_count_{0};
    size_t progression_index_{0};

    static uint32_t packChord(Chord c) noexcept;
    static Chord unpackChord(uint32_t data) noexcept;
};

// ── Common chord utility ───────────────────────────────────────────

namespace chords {
    constexpr Chord CMaj  = {ChordType::Major, 60, 0, 0};
    constexpr Chord CMin  = {ChordType::Minor, 60, 0, 0};
    constexpr Chord DMaj  = {ChordType::Major, 62, 0, 0};
    constexpr Chord DMin  = {ChordType::Minor, 62, 0, 0};
    constexpr Chord EMaj  = {ChordType::Major, 64, 0, 0};
    constexpr Chord EMin  = {ChordType::Minor, 64, 0, 0};
    constexpr Chord FMaj  = {ChordType::Major, 65, 0, 0};
    constexpr Chord GMaj  = {ChordType::Major, 67, 0, 0};
    constexpr Chord AMaj  = {ChordType::Major, 69, 0, 0};
    constexpr Chord AMin  = {ChordType::Minor, 69, 0, 0};
    constexpr Chord NoCh  = {ChordType::NoChord, 0, 0, 0};
}

} // namespace ai_arranger::arranger
#endif // AI_ARRANGER_ARRANGER_CHORD_INPUT_H
