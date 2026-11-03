#ifndef AI_ARRANGER_UASF_TYPES_H
#define AI_ARRANGER_UASF_TYPES_H

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <variant>

namespace ai_arranger::uasf {

// ── MIDI Event Types ─────────────────────────────────────────────────

enum class MidiEventType : uint8_t {
    NoteOff          = 0x80,
    NoteOn           = 0x90,
    PolyPressure     = 0xA0,
    ControlChange    = 0xB0,
    ProgramChange    = 0xC0,
    ChannelPressure  = 0xD0,
    PitchBend        = 0xE0,
};

struct MidiEvent {
    MidiEventType type;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
    uint64_t tick;       // Absolute tick position (0-based)
};

// ── Section Types ────────────────────────────────────────────────────

enum class SectionType : uint8_t {
    Intro1, Intro2, Intro3,
    Main1, Main2, Main3, Main4,
    Fill1, Fill2, Fill3, Fill4,
    Ending1, Ending2, Ending3,
    Break,
};

// ── Track Roles ──────────────────────────────────────────────────────

enum class TrackRole : uint8_t {
    Accompaniment,
    Chord,
    Bass,
    Drum,
    Percussion,
    Phrase1, Phrase2,
    Pad,
    Melody,
    Articulation,
};

// ── Articulation / Fidelity (ADR-013) ────────────────────────────────

enum class FidelityRequirement : uint8_t {
    Low,       // GM fallback acceptable
    Medium,    // SoundFont / AUv3 preferred
    High,      // External device or premium library required
};

enum class ArticulationProfile : uint8_t {
    Generic,
    Staccato,
    Legato,
    Mute,
    Slap,
    HammerOn,      // Guitar-specific
    PullOff,       // Guitar-specific
    MegaLike,      // Yamaha MegaVoice-style
    Tremolo,
    Trill,
    Custom,
};

// Model types hold std::string/std::vector, so `T x;` is default-initialization:
// every scalar member needs an initializer or it is indeterminate (the serializer
// used to read garbage bools here). Defaults equal value-initialization (zero /
// false / first enumerator) and are pinned by test_uasf_deterministic_init.

struct PlaybackRecommendation {
    bool external_yamaha = false;
    bool external_midi = false;
    bool premium_library = false;
    bool user_soundfont = false;
    bool degradation_allowed = false;   // GM fallback only with warning
};

struct ArticulationMetadata {
    ArticulationProfile profile = ArticulationProfile::Generic;
    FidelityRequirement fidelity = FidelityRequirement::Low;
    PlaybackRecommendation recommended;
    uint8_t ntr = 0;             // Note Transposition Rule (0=Root,1=Fifth,2=Chord,3=Bass,6=Fixed)
    uint8_t ntt = 0;             // Note Transposition Table (from CASM)
    std::string vendor;
    std::string vendor_articulation;
    bool has_keyswitch = false;
    uint8_t keyswitch_note = 0;
    std::vector<uint8_t> velocity_map;
};

// ── Track Definition ─────────────────────────────────────────────────

struct TrackDefinition {
    std::string name;
    uint8_t midi_channel = 0;
    TrackRole role = TrackRole::Accompaniment;
    bool is_drum = false;
    ArticulationMetadata articulation;
    std::vector<MidiEvent> events;
};

// ── Section Definition ───────────────────────────────────────────────

struct SectionDefinition {
    SectionType type = SectionType::Intro1;
    std::string name;
    uint32_t bars = 0;            // 0 = unset (validator error)
    uint32_t resolution = 0;      // ticks per quarter note; 0 = unset
    uint32_t beats_per_bar = 0;
    uint32_t beat_note = 0;       // denominator (4 = quarter note)
    std::vector<TrackDefinition> tracks;
};

// ── Style Definition ─────────────────────────────────────────────────

// Accepted style tempo (whole BPM), the same range the session layer persists.
inline constexpr uint32_t kMinTempoBpm = 20;
inline constexpr uint32_t kMaxTempoBpm = 400;
inline constexpr uint32_t kDefaultTempoBpm = 120;   // a style with no tempo

struct StyleDefinition {
    std::string name;
    std::string format_version;
    uint32_t tempo_bpm = 0;        // 0 = unset
    uint32_t resolution = 0;       // ticks per quarter note; 0 = unset (players fall back to 480)
    std::vector<SectionDefinition> sections;
    int32_t ottava_bass = 0;       // transpose bass tracks
    int32_t ottava_chord = 0;      // transpose chord tracks
    // UASF v1.1 extension blocks this reader does not understand, kept verbatim
    // (header + payload, file order) so a read/write round trip preserves them
    // (UASF_VERSIONING_POLICY: unknown fields MUST be preserved).
    std::vector<uint8_t> unknown_extensions;
};

} // namespace ai_arranger::uasf

#endif // AI_ARRANGER_UASF_TYPES_H
