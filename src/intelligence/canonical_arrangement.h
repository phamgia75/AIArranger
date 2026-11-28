#ifndef AI_ARRANGER_INTELLIGENCE_CANONICAL_ARRANGEMENT_H
#define AI_ARRANGER_INTELLIGENCE_CANONICAL_ARRANGEMENT_H

#include "engine/uasf/types.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/korg_style_smf.h"
#include "exporters/korg/megavoice_sanitizer.h"
#include "exporters/korg/source_chord_normalizer.h"
#include "exporters/korg/timbre_profiler.h"
#include "intelligence/target_capability.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Phase 5 Milestone P5-M1: Non-Duplicating Canonical Musical Representation.
//
// Boundaries & Invariants (ADR-P5-001 / ADR-P5-003 / ADR-P5-005):
//   * Zero-duplication read-only semantic overlay referencing existing
//     uasf::StyleDefinition and/or exporters::korg::Style buffers.
//   * Never copies or replaces note payload ownership; stores only canonical
//     indices, extracted musical feature vectors, harmonic/groove/phrase
//     summaries, inter-track relationships, target capability references,
//     confidence/uncertainty metadata, and provenance.
//   * Header-only portable C++20, ODR-safe, zero JUCE/Apple/media deps.

namespace ai_arranger::intelligence {

enum class MusicalRole : uint8_t {
    UnknownAbstain        = 0,  // Explicit fail-closed abstain when uncertain/ambiguous
    KickSnareFoundation   = 1,  // Main drum kit (kick/snare/hi-hat rhythmic foundation)
    DrumKitAuxPercussion  = 2,  // Auxiliary percussion (shaker/conga/tambourine/claves/timbale)
    BassFoundation        = 3,  // Bass foundation (electric/acoustic/synth/walking bass)
    HarmonicAccompaniment = 4,  // General harmonic accompaniment (organ/accordion/strings comp)
    ChordComping          = 5,  // Rhythmic syncopated chord comping (stabs/clav/funk keys)
    GuitarStrumming       = 6,  // Strummed guitar (micro-arpeggiated chord strums <= 45 ticks)
    Arpeggio              = 7,  // Broken-chord / arpeggiated picking (guitar/harp/synth arp)
    PianoAccompaniment    = 8,  // Piano accompaniment (two-hand / wide chordal voicing / pedal)
    PadSustainedHarmony   = 9,  // Sustained harmonic pad / strings / choir bed
    Countermelody         = 10, // Mid-register melodic answer / obbligato / secondary line
    BrassAccents          = 11, // Short punchy high-velocity brass stabs / hits / falls
    OrchestralAccents     = 12, // Orchestral swells / marcato / timpani / tutti hits
    TransitionalEffects   = 13, // Sparse risers / cymbal swells / sweeps / SFX / turnarounds
    MelodicPhrase         = 14, // Prominent upper-register melodic lead / riff / hook
    MixedRole             = 15, // Explicit dual/multi-function track (inspect primary + secondary)

    // Exact compatibility aliases with PHASE5_ARCHITECTURE.md §5.2:
    DrumKitMain      = KickSnareFoundation,
    PercussionLayer  = DrumKitAuxPercussion,
    HarmonicPad      = PadSustainedHarmony,
    RhythmicComp     = ChordComping,
    GuitarStrum      = GuitarStrumming,
    GuitarArpeggio   = Arpeggio,
    CounterMelody    = Countermelody,
    LeadPhrase       = MelodicPhrase,
    OrchestralAccent = OrchestralAccents,
    EffectTrigger    = TransitionalEffects,
};

inline constexpr size_t kCanonicalSemanticRoleCount = 14; // Roles 1..14

inline constexpr const char* musicalRoleName(MusicalRole role) noexcept {
    switch (role) {
        case MusicalRole::UnknownAbstain:        return "UnknownAbstain";
        case MusicalRole::KickSnareFoundation:   return "KickSnareFoundation";
        case MusicalRole::DrumKitAuxPercussion:  return "DrumKitAuxPercussion";
        case MusicalRole::BassFoundation:        return "BassFoundation";
        case MusicalRole::HarmonicAccompaniment: return "HarmonicAccompaniment";
        case MusicalRole::ChordComping:          return "ChordComping";
        case MusicalRole::GuitarStrumming:       return "GuitarStrumming";
        case MusicalRole::Arpeggio:              return "Arpeggio";
        case MusicalRole::PianoAccompaniment:    return "PianoAccompaniment";
        case MusicalRole::PadSustainedHarmony:   return "PadSustainedHarmony";
        case MusicalRole::Countermelody:         return "Countermelody";
        case MusicalRole::BrassAccents:          return "BrassAccents";
        case MusicalRole::OrchestralAccents:     return "OrchestralAccents";
        case MusicalRole::TransitionalEffects:   return "TransitionalEffects";
        case MusicalRole::MelodicPhrase:         return "MelodicPhrase";
        case MusicalRole::MixedRole:             return "MixedRole";
    }
    return "UnknownAbstain";
}

inline constexpr bool isPercussiveRole(MusicalRole role) noexcept {
    return role == MusicalRole::KickSnareFoundation ||
           role == MusicalRole::DrumKitAuxPercussion;
}

inline constexpr RoleRegisterCategory roleToRegisterCategory(MusicalRole role) noexcept {
    switch (role) {
        case MusicalRole::KickSnareFoundation:   return RoleRegisterCategory::Drum;
        case MusicalRole::DrumKitAuxPercussion:  return RoleRegisterCategory::Percussion;
        case MusicalRole::BassFoundation:        return RoleRegisterCategory::Bass;
        case MusicalRole::HarmonicAccompaniment: return RoleRegisterCategory::HarmonicComp;
        case MusicalRole::ChordComping:          return RoleRegisterCategory::HarmonicComp;
        case MusicalRole::GuitarStrumming:       return RoleRegisterCategory::Guitar;
        case MusicalRole::Arpeggio:              return RoleRegisterCategory::Guitar;
        case MusicalRole::PianoAccompaniment:    return RoleRegisterCategory::Piano;
        case MusicalRole::PadSustainedHarmony:   return RoleRegisterCategory::Pad;
        case MusicalRole::Countermelody:         return RoleRegisterCategory::Lead;
        case MusicalRole::BrassAccents:          return RoleRegisterCategory::Brass;
        case MusicalRole::OrchestralAccents:     return RoleRegisterCategory::Orchestral;
        case MusicalRole::TransitionalEffects:   return RoleRegisterCategory::Effects;
        case MusicalRole::MelodicPhrase:         return RoleRegisterCategory::Lead;
        case MusicalRole::MixedRole:             return RoleRegisterCategory::HarmonicComp;
        case MusicalRole::UnknownAbstain:        return RoleRegisterCategory::HarmonicComp;
    }
    return RoleRegisterCategory::HarmonicComp;
}

inline RolePitchRange pitchRangeForMusicalRole(const TargetCapabilityProfile& profile,
                                               MusicalRole role) noexcept {
    return profile.pitchRangeFor(roleToRegisterCategory(role));
}

enum class RegisterBand : uint8_t {
    SubBass = 0, // MIDI < 36 (below C2)
    Bass    = 1, // MIDI 36..51 (C2..D#3)
    LowMid  = 2, // MIDI 52..63 (E3..D#4)
    Mid     = 3, // MIDI 64..75 (E4..D#5)
    HighMid = 4, // MIDI 76..87 (E5..D#6)
    Treble  = 5, // MIDI >= 88 (E6+)
};

inline constexpr RegisterBand registerBandForPitch(uint8_t pitch) noexcept {
    if (pitch < 36) return RegisterBand::SubBass;
    if (pitch < 52) return RegisterBand::Bass;
    if (pitch < 64) return RegisterBand::LowMid;
    if (pitch < 76) return RegisterBand::Mid;
    if (pitch < 88) return RegisterBand::HighMid;
    return RegisterBand::Treble;
}

inline constexpr const char* registerBandName(RegisterBand band) noexcept {
    switch (band) {
        case RegisterBand::SubBass: return "SubBass";
        case RegisterBand::Bass:    return "Bass";
        case RegisterBand::LowMid:  return "LowMid";
        case RegisterBand::Mid:     return "Mid";
        case RegisterBand::HighMid: return "HighMid";
        case RegisterBand::Treble:  return "Treble";
    }
    return "Mid";
}

enum class DecisionOrigin : uint8_t {
    Deterministic              = 0,
    RuleBased                  = 1,
    StatisticalInference       = 2,
    OptionalGenerativeProposal = 3,
    UserOverride               = 4,
};

inline constexpr const char* decisionOriginName(DecisionOrigin origin) noexcept {
    switch (origin) {
        case DecisionOrigin::Deterministic:              return "Deterministic";
        case DecisionOrigin::RuleBased:                  return "RuleBased";
        case DecisionOrigin::StatisticalInference:       return "StatisticalInference";
        case DecisionOrigin::OptionalGenerativeProposal: return "OptionalGenerativeProposal";
        case DecisionOrigin::UserOverride:               return "UserOverride";
    }
    return "Deterministic";
}

struct ProvenanceRecord {
    DecisionOrigin           origin = DecisionOrigin::Deterministic;
    std::string              component_id;
    double                   confidence = 1.0;          // [0.0, 1.0]
    double                   uncertainty_entropy = 0.0; // Normalized Shannon entropy [0.0, 1.0]
    bool                     abstained = false;
    std::string              abstain_reason;
    bool                     reversible = true;
    std::string              rationale;
    std::vector<std::string> evidence;
};

struct SourceProvenance {
    std::string              source_format = "UASF";
    std::string              source_name;
    std::string              converter_sha = "50b3a3b50c277362cd07f7d9667161b821fe7d90";
    std::string              schema_version = "aiarranger-canonical-arrangement/1";
    std::vector<std::string> feature_flags;
    bool                     hardware_verified = false; // ALWAYS false offline
    std::string              l3_status = "NOT_TESTED";  // ALWAYS NOT_TESTED offline
};

struct SoundIdentity {
    int         msb = -1;
    int         lsb = -1;
    int         pc  = -1;
    bool        is_drum_kit  = false;
    bool        is_drum_bank = false; // True iff (msb, lsb) matches a TargetCapabilityProfile drum bank
    std::string family;
    std::string sub_family;
};

struct HarmonicBehavior {
    uint8_t     ntr = 0;
    uint8_t     ntt = 0;
    bool        ntt_bass = false;
    uint8_t     source_chord_root = 0; // 0=C .. 11=B
    uint8_t     source_chord_type = 0; // Yamaha SFF chord code
    std::string source_chord_name = "Maj";
    uint8_t     high_key = 11;
    uint8_t     note_low = 0;
    uint8_t     note_high = 127;
    double      chord_tone_ratio = 0.0;
    double      root_ratio = 0.0;
    double      third_ratio = 0.0;
    double      fifth_ratio = 0.0;
    double      seventh_ratio = 0.0;
    double      non_chord_tone_ratio = 0.0;
};

// Complete 13-group offline musical feature summary (Groups 1..11 per-track;
// Group 12 inter-track & Group 13 section-dependent live at section/style level).
struct TrackFeatureSummary {
    // Group 1: Pitch distribution
    uint32_t                 note_count = 0;
    uint8_t                  unique_pitch_count = 0;
    uint8_t                  pitch_min = 0;
    uint8_t                  pitch_median = 60;
    uint8_t                  pitch_max = 127;
    double                   pitch_mean = 60.0;
    double                   pitch_stddev = 0.0;
    uint8_t                  pitch_span = 0;
    std::array<uint32_t, 12> pitch_class_hist{};

    // Group 2: Register band
    RegisterBand             register_band = RegisterBand::Mid;
    double                   low_register_ratio = 0.0;  // pitch < 52
    double                   mid_register_ratio = 0.0;  // 52 <= pitch < 76
    double                   high_register_ratio = 0.0; // pitch >= 76

    // Group 3: Note density
    double                   notes_per_bar = 0.0;
    double                   unique_onsets_per_bar = 0.0;
    uint32_t                 active_bars = 0;
    uint32_t                 total_bars = 0;
    double                   active_bar_ratio = 0.0;

    // Group 4: Rhythmic patterns & microtiming
    double                   downbeat_ratio = 0.0;
    double                   backbeat_ratio = 0.0;
    double                   eighth_subdivision_ratio = 0.0;
    double                   sixteenth_subdivision_ratio = 0.0;
    double                   triplet_subdivision_ratio = 0.0;
    std::array<double, 16>   onset_grid_16th{};
    double                   swing_ratio_estimate = 0.50;
    uint32_t                 swing_offbeat_count = 0;
    double                   microtiming_stddev_ticks = 0.0;

    // Group 5: Syncopation
    double                   syncopation_ratio = 0.0;  // off-beat / weak-beat onset fraction [0..1]
    double                   anticipation_ratio = 0.0;

    // Group 6: Chord-tone relationships
    uint8_t                  source_chord_root = 0;
    std::string              source_chord_type_name = "Maj";
    double                   chord_tone_ratio = 0.0;
    double                   root_ratio = 0.0;
    double                   third_ratio = 0.0;
    double                   fifth_ratio = 0.0;
    double                   seventh_ratio = 0.0;
    double                   non_chord_tone_ratio = 0.0;

    // Group 7: Repeated motifs & melodic intervals
    double                   motif_repetition_ratio = 0.0;
    double                   interval_stepwise_ratio = 0.0;   // |delta| in 1..2
    double                   interval_chord_leap_ratio = 0.0; // |delta| in 3..7
    double                   interval_large_leap_ratio = 0.0; // |delta| >= 8
    double                   monotonic_ramp_ratio = 0.0;      // rising/falling sweep continuity
    std::string              motif_signature;

    // Group 8: Articulation & controller indicators
    double                   mean_duration_beats = 0.0;
    double                   duration_stddev_beats = 0.0;
    double                   staccato_ratio = 0.0; // dur <= 0.35 beats
    double                   legato_ratio = 0.0;   // dur >= 0.90 beats
    double                   duty_cycle = 0.0;     // sounding coverage [0..1]
    bool                     has_strum_offsets = false;
    double                   strum_onset_ratio = 0.0;
    double                   mean_strum_spread_ticks = 0.0;
    double                   ghost_note_ratio = 0.0; // vel <= 45
    bool                     has_pitch_bend = false;
    uint32_t                 pitch_bend_count = 0;
    bool                     has_sustain_pedal = false;
    uint32_t                 sustain_pedal_count = 0;
    bool                     has_expression_cc11 = false;
    uint32_t                 cc11_count = 0;
    uint8_t                  cc11_range = 0;
    int                      cc7_volume = -1;
    int                      cc10_pan = -1;
    // Percussive / boundary content cues (purely pitch/timing derived):
    double                   drum_kick_snare_ratio = 0.0;
    double                   drum_hat_cymbal_ratio = 0.0;
    double                   drum_aux_perc_ratio = 0.0;
    double                   drum_fx_cymbal_ratio = 0.0;
    double                   max_pitch_dominance = 0.0; // fraction of notes on most frequent pitch
    double                   boundary_note_ratio = 0.0; // notes near bar 0 beat 0 or final beat
    double                   dissonant_simultaneous_ratio = 0.0; // semitone/tritone dyads (drum-like)
    bool                     has_two_hand_split = false; // concurrent low (<=54) + upper (>=60) notes

    // Group 9: Velocity distribution
    double                   mean_velocity = 0.0;
    double                   vel_stddev = 0.0;
    uint8_t                  vel_min = 0;
    uint8_t                  vel_max = 0;
    uint8_t                  velocity_dynamic_range = 0;
    double                   accent_ratio = 0.0; // vel >= 100 or vel >= mean + 15

    // Group 10: Polyphony
    uint8_t                  max_polyphony = 0;
    double                   mean_polyphony = 0.0;
    double                   chordal_ratio = 0.0;    // simultaneous onset clusters (>= 2 notes)
    double                   triad_plus_ratio = 0.0; // simultaneous onset clusters (>= 3 notes)
    double                   monophonic_ratio = 0.0; // single-note onset clusters

    // Group 11: Phrase boundaries
    uint32_t                 phrase_count = 0;
    double                   mean_phrase_length_beats = 0.0;
    double                   inter_onset_gap_max_beats = 0.0;
    std::vector<uint32_t>    phrase_boundary_ticks;
};

struct RegisterOverlapEntry {
    uint8_t     channel_a = 0;
    uint8_t     channel_b = 0;
    MusicalRole role_a = MusicalRole::UnknownAbstain;
    MusicalRole role_b = MusicalRole::UnknownAbstain;
    uint8_t     overlap_low = 0;
    uint8_t     overlap_high = 0;
    uint8_t     overlap_semitones = 0;
    double      overlap_ratio = 0.0; // intersection / min_span [0..1]
    bool        collision_warning = false;
    std::string note;
};

struct InterTrackRelationships {
    double                            kick_bass_lock_correlation = 0.0; // [0..1]
    bool                              bass_piano_low_collision = false;
    uint32_t                          bass_piano_low_collision_notes = 0;
    std::vector<RegisterOverlapEntry> register_overlaps;
};

struct CanonicalTrackView {
    uint8_t                                     source_channel = 0;    // 0..15 from UASF
    uint8_t                                     target_channel = 0xFF; // mapped target channel
    std::string                                 name;
    uasf::TrackRole                             uasf_role = uasf::TrackRole::Accompaniment;
    bool                                        is_drum = false;
    exporters::korg::quality::CasmTrackView     casm{};
    exporters::korg::quality::TrackTimbreProfile timbre{};
    SoundIdentity                               sound_identity{};
    HarmonicBehavior                            harmonic{};
    TrackFeatureSummary                         features{};
    MusicalRole                                 semantic_role = MusicalRole::UnknownAbstain;
    MusicalRole                                 secondary_role = MusicalRole::UnknownAbstain;
    bool                                        is_mixed_role = false;
    double                                      role_confidence = 0.0;
    double                                      uncertainty_entropy = 1.0;
    bool                                        role_abstained = true;
    std::string                                 abstain_reason;
    std::vector<std::string>                    supporting_evidence;
    bool                                        locked = false; // Hard guard: locked tracks bit-exact
    ProvenanceRecord                            provenance{};
};

struct CanonicalSectionView {
    size_t                          uasf_section_index = 0;
    uasf::SectionType               type = uasf::SectionType::Main1;
    std::string                     name;
    std::string                     korg_marker;
    uint32_t                        bars = 0;
    uint32_t                        ppqn = 480;
    uint8_t                         meter_num = 4;
    uint8_t                         meter_den = 4;
    uint32_t                        length_ticks = 0;
    uint32_t                        loop_start_tick = 0;
    uint32_t                        loop_end_tick = 0;
    uint8_t                         cv_count = 1;
    std::vector<std::string>        cv_markers;
    double                          composite_energy = 0.0; // normalized [0..100]
    double                          swing_ratio = 0.50;     // 0.50 straight .. 0.67 triplet swing
    std::vector<CanonicalTrackView> tracks;
    InterTrackRelationships         inter_track{};
};

struct CanonicalTransitionSummary {
    uint32_t intro_count = 0;
    uint32_t variation_count = 0;
    uint32_t fill_count = 0;
    uint32_t break_count = 0;
    uint32_t ending_count = 0;
    uint32_t total_cv_elements = 0;
};

// Non-duplicating semantic overlay referencing existing UASF / KorgStyle buffers.
struct CanonicalStyleView {
    std::string                       style_name;
    uint32_t                          tempo_bpm = 120;
    uint32_t                          ppqn = 480;
    const uasf::StyleDefinition*      source_uasf = nullptr;     // non-owning
    const exporters::korg::Style*     target_korg = nullptr;     // optional non-owning
    const exporters::korg::Report*    target_report = nullptr;   // optional non-owning
    TargetPlatformId                  target_platform = TargetPlatformId::KorgPa5X;
    const TargetCapabilityProfile*    target_capability = &targetCapabilityProfile(TargetPlatformId::KorgPa5X);
    SourceProvenance                  provenance{};
    CanonicalTransitionSummary        transitions{};
    std::vector<CanonicalSectionView> sections;
    InterTrackRelationships           inter_track_overall{};
    bool                              hardware_verified = false;
    std::string                       l3_status = "NOT_TESTED";
};

namespace canonical_detail {

inline double clamp01(double v) noexcept {
    if (!std::isfinite(v) || v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

inline uint32_t saturatingAddU32(uint32_t a, uint32_t b) noexcept {
    const uint64_t sum = uint64_t{a} + uint64_t{b};
    return (sum > uint64_t{UINT32_MAX}) ? UINT32_MAX : static_cast<uint32_t>(sum);
}

inline uint64_t safeOffTick64(uint32_t on_t, uint32_t off_t) noexcept {
    return (off_t > on_t) ? uint64_t{off_t} : (uint64_t{on_t} + 1ULL);
}

inline std::array<bool, 12> chordPitchClassMask(const exporters::korg::quality::DecodedYamahaSourceChord& ch) noexcept {
    std::array<bool, 12> mask{};
    const auto setRel = [&](uint8_t semis) {
        mask[(ch.root + semis) % 12u] = true;
    };
    using CT = arranger::ChordType;
    setRel(0);
    switch (ch.chord_type) {
        case CT::Major:
            setRel(4); setRel(7); break;
        case CT::Minor:
            setRel(3); setRel(7); break;
        case CT::Dim:
            setRel(3); setRel(6); break;
        case CT::Aug:
            setRel(4); setRel(8); break;
        case CT::Maj7:
            setRel(4); setRel(7); setRel(11); break;
        case CT::Min7:
            setRel(3); setRel(7); setRel(10); break;
        case CT::Dom7:
            setRel(4); setRel(7); setRel(10); break;
        case CT::Dim7:
            setRel(3); setRel(6); setRel(9); break;
        case CT::Sus4:
            setRel(5); setRel(7); break;
        case CT::Sus2:
            setRel(2); setRel(7); break;
        case CT::Power:
            setRel(7); break;
        case CT::Min7b5:
            setRel(3); setRel(6); setRel(10); break;
        case CT::Add9:
            setRel(2); setRel(4); setRel(7); break;
        case CT::Maj6:
            setRel(4); setRel(7); setRel(9); break;
        case CT::Min6:
            setRel(3); setRel(7); setRel(9); break;
        case CT::Dom9:
            setRel(2); setRel(4); setRel(7); setRel(10); break;
        case CT::Maj9:
            setRel(2); setRel(4); setRel(7); setRel(11); break;
        case CT::Min9:
            setRel(2); setRel(3); setRel(7); setRel(10); break;
        case CT::Dom7Sus4:
            setRel(5); setRel(7); setRel(10); break;
        case CT::MinMaj7:
            setRel(3); setRel(7); setRel(11); break;
        case CT::Aug7:
            setRel(4); setRel(8); setRel(10); break;
        case CT::NoChord:
            setRel(4); setRel(7); break;
    }
    return mask;
}

inline bool isKickSnareKey(uint8_t p) noexcept {
    return p == 35 || p == 36 || p == 37 || p == 38 || p == 40;
}

inline bool isHatCymbalKey(uint8_t p) noexcept {
    return p == 42 || p == 44 || p == 46 || p == 49 || p == 51 || p == 52 || p == 53 || p == 55 || p == 57 || p == 59;
}

inline bool isAuxPercKey(uint8_t p) noexcept {
    return (p >= 54 && p <= 81 && p != 55 && p != 57 && p != 59) || p == 39;
}

inline bool isFxCymbalKey(uint8_t p) noexcept {
    return p == 49 || p == 52 || p == 55 || p == 57 || p == 58 || p == 71 || p == 72 || p == 78 || p == 79 || p == 81;
}

struct ControllerTrace {
    bool     has_pitch_bend = false;
    uint32_t pitch_bend_count = 0;
    bool     has_sustain_pedal = false;
    uint32_t sustain_pedal_count = 0;
    bool     has_expression_cc11 = false;
    uint32_t cc11_count = 0;
    uint8_t  cc11_min = 127;
    uint8_t  cc11_max = 0;
    int      cc7_volume = -1;
    int      cc10_pan = -1;
    int      msb = -1;
    int      lsb = -1;
    int      pc = -1;
};

inline ControllerTrace extractControllersFromUasf(const std::vector<uasf::MidiEvent>& events) noexcept {
    ControllerTrace c{};
    for (const auto& ev : events) {
        if (ev.type == uasf::MidiEventType::PitchBend) {
            c.has_pitch_bend = true;
            ++c.pitch_bend_count;
        } else if (ev.type == uasf::MidiEventType::ProgramChange) {
            if (c.pc < 0) c.pc = ev.data1;
        } else if (ev.type == uasf::MidiEventType::ControlChange) {
            if (ev.data1 == 0 && c.msb < 0) c.msb = ev.data2;
            else if (ev.data1 == 32 && c.lsb < 0) c.lsb = ev.data2;
            else if (ev.data1 == 7 && c.cc7_volume < 0) c.cc7_volume = ev.data2;
            else if (ev.data1 == 10 && c.cc10_pan < 0) c.cc10_pan = ev.data2;
            else if (ev.data1 == 64 && ev.data2 >= 64) {
                c.has_sustain_pedal = true;
                ++c.sustain_pedal_count;
            } else if (ev.data1 == 11) {
                c.has_expression_cc11 = true;
                ++c.cc11_count;
                c.cc11_min = std::min(c.cc11_min, ev.data2);
                c.cc11_max = std::max(c.cc11_max, ev.data2);
            }
        }
    }
    return c;
}

inline ControllerTrace extractControllersFromKorg(const std::vector<exporters::korg::Event>& events,
                                                  uint8_t ch) noexcept {
    ControllerTrace c{};
    for (const auto& ev : events) {
        if ((ev.status & 0x0Fu) != ch) continue;
        const uint8_t kind = ev.status & 0xF0u;
        if (kind == 0xE0u) {
            c.has_pitch_bend = true;
            ++c.pitch_bend_count;
        } else if (kind == 0xC0u) {
            if (c.pc < 0) c.pc = ev.d1;
        } else if (kind == 0xB0u) {
            if (ev.d1 == 0 && c.msb < 0) c.msb = ev.d2;
            else if (ev.d1 == 32 && c.lsb < 0) c.lsb = ev.d2;
            else if (ev.d1 == 7 && c.cc7_volume < 0) c.cc7_volume = ev.d2;
            else if (ev.d1 == 10 && c.cc10_pan < 0) c.cc10_pan = ev.d2;
            else if (ev.d1 == 64 && ev.d2 >= 64) {
                c.has_sustain_pedal = true;
                ++c.sustain_pedal_count;
            } else if (ev.d1 == 11) {
                c.has_expression_cc11 = true;
                ++c.cc11_count;
                c.cc11_min = std::min(c.cc11_min, ev.d2);
                c.cc11_max = std::max(c.cc11_max, ev.d2);
            }
        }
    }
    return c;
}

inline std::vector<exporters::korg::quality::NoteSpan> extractNoteSpansFromKorg(
    const std::vector<exporters::korg::Event>& events,
    uint8_t ch,
    uint32_t section_end) {
    using exporters::korg::quality::NoteSpan;
    std::vector<NoteSpan> spans;
    std::array<std::vector<std::pair<uint32_t, uint8_t>>, 128> active{};
    for (const auto& ev : events) {
        if ((ev.status & 0x0Fu) != ch) continue;
        const uint8_t kind = ev.status & 0xF0u;
        const uint8_t pitch = ev.d1 & 0x7Fu;
        if (kind == 0x90u && ev.d2 > 0) {
            active[pitch].push_back({ev.tick, ev.d2});
        } else if (kind == 0x80u || (kind == 0x90u && ev.d2 == 0)) {
            auto& q = active[pitch];
            if (!q.empty()) {
                const auto [on_t, vel] = q.front();
                q.erase(q.begin());
                const uint32_t off_t = (ev.tick > on_t) ? ev.tick : saturatingAddU32(on_t, 1u);
                spans.push_back(NoteSpan{on_t, off_t, pitch, vel, false});
            }
        }
    }
    for (uint8_t p = 0; p < 128; ++p) {
        for (const auto& [on_t, vel] : active[p]) {
            const uint32_t off_t = (section_end > on_t) ? section_end : saturatingAddU32(on_t, 1u);
            spans.push_back(NoteSpan{on_t, off_t, p, vel, false});
        }
    }
    std::sort(spans.begin(), spans.end(), [](const NoteSpan& a, const NoteSpan& b) {
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        return a.pitch < b.pitch;
    });
    return spans;
}

inline TrackFeatureSummary computeTrackFeatures(
    const std::vector<exporters::korg::quality::NoteSpan>& raw_notes,
    const ControllerTrace& ctrl,
    uint32_t ppqn,
    uint8_t meter_num,
    uint8_t meter_den,
    uint32_t bars,
    uint8_t source_chord_root,
    uint8_t source_chord_type) {
    TrackFeatureSummary f{};
    f.has_pitch_bend      = ctrl.has_pitch_bend;
    f.pitch_bend_count    = ctrl.pitch_bend_count;
    f.has_sustain_pedal   = ctrl.has_sustain_pedal;
    f.sustain_pedal_count = ctrl.sustain_pedal_count;
    f.has_expression_cc11 = ctrl.has_expression_cc11;
    f.cc11_count          = ctrl.cc11_count;
    f.cc11_range          = (ctrl.has_expression_cc11 && ctrl.cc11_max >= ctrl.cc11_min)
                                ? static_cast<uint8_t>(ctrl.cc11_max - ctrl.cc11_min)
                                : 0u;
    f.cc7_volume          = ctrl.cc7_volume;
    f.cc10_pan            = ctrl.cc10_pan;

    const uint32_t eff_ppqn = ppqn ? std::clamp<uint32_t>(ppqn, 1u, 19200u) : 480u;
    const uint32_t eff_num  = meter_num ? uint32_t{meter_num} : 4u;
    const uint32_t eff_den  = meter_den ? uint32_t{meter_den} : 4u;
    const uint64_t beat_ticks = std::max<uint64_t>(1u, (uint64_t{eff_ppqn} * 4ULL) / uint64_t{eff_den});
    const uint64_t bar_ticks  = std::max<uint64_t>(1u, beat_ticks * uint64_t{eff_num});
    const uint32_t eff_bars   = std::max<uint32_t>(1u, bars);
    const uint64_t total_ticks = (uint64_t{eff_bars} > UINT64_MAX / bar_ticks)
                                     ? UINT64_MAX
                                     : (bar_ticks * uint64_t{eff_bars});
    f.total_bars = eff_bars;

    const auto decoded_chord = exporters::korg::quality::decodeYamahaSourceChord(
        source_chord_root <= 11 ? source_chord_root : 0u, source_chord_type);
    f.source_chord_root      = decoded_chord.root;
    f.source_chord_type_name = decoded_chord.name ? decoded_chord.name : "Maj";
    const auto chord_mask    = chordPitchClassMask(decoded_chord);

    if (raw_notes.empty()) {
        f.pitch_min = 0;
        f.pitch_max = 0;
        f.pitch_median = 0;
        f.pitch_mean = 0.0;
        return f;
    }

    auto notes = raw_notes;
    std::sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        return a.pitch < b.pitch;
    });

    const size_t n = notes.size();
    f.note_count = static_cast<uint32_t>(n);

    // ── Group 1, 2, 6, 8 (drum/duration), 9 (velocity) ──
    std::vector<uint8_t> pitches;
    pitches.reserve(n);
    std::array<uint32_t, 128> pitch_counts{};
    std::set<uint32_t> active_bar_set;

    double pitch_sum = 0.0;
    double vel_sum = 0.0;
    double dur_beats_sum = 0.0;
    uint8_t p_min = 127, p_max = 0;
    uint8_t v_min = 127, v_max = 0;
    uint32_t low_ct = 0, mid_ct = 0, high_ct = 0;
    uint32_t chord_ct = 0, root_ct = 0, third_ct = 0, fifth_ct = 0, seventh_ct = 0;
    uint32_t staccato_ct = 0, legato_ct = 0, ghost_ct = 0;
    uint32_t kick_snare_ct = 0, hat_cym_ct = 0, aux_perc_ct = 0, fx_cym_ct = 0;
    uint32_t boundary_ct = 0;

    // Rhythmic counters
    uint32_t downbeat_ct = 0, backbeat_ct = 0;
    uint32_t eighth_ct = 0, sixteenth_ct = 0, triplet_ct = 0;
    uint32_t syncopated_ct = 0, anticipation_ct = 0;
    std::vector<double> offbeat_ratios;
    std::vector<double> micro_errors;
    std::array<uint32_t, 16> grid16_counts{};
    constexpr uint32_t kMaxGridBars = 4096u;
    const uint32_t grid_bars = std::min<uint32_t>(eff_bars, kMaxGridBars);
    std::vector<std::array<uint32_t, 16>> per_bar_grid(grid_bars);

    const double tol_ticks = std::max<double>(8.0, static_cast<double>(eff_ppqn) * 0.06);
    const double slot16_ticks = static_cast<double>(bar_ticks) / 16.0;
    const double quarter_ticks = static_cast<double>(eff_ppqn);

    std::vector<double> dur_beats_vec;
    dur_beats_vec.reserve(n);

    for (const auto& ns : notes) {
        const uint8_t p = std::min<uint8_t>(ns.pitch, 127u);
        pitches.push_back(p);
        pitch_counts[p]++;
        f.pitch_class_hist[p % 12u]++;
        pitch_sum += static_cast<double>(p);
        p_min = std::min(p_min, p);
        p_max = std::max(p_max, p);

        if (p < 52) ++low_ct;
        else if (p < 76) ++mid_ct;
        else ++high_ct;

        vel_sum += static_cast<double>(ns.vel);
        v_min = std::min(v_min, ns.vel);
        v_max = std::max(v_max, ns.vel);
        if (ns.vel <= 45) ++ghost_ct;

        const uint64_t dur_ticks = (ns.off_tick > ns.on_tick) ? (uint64_t{ns.off_tick} - uint64_t{ns.on_tick}) : 1ULL;
        const double dur_beats = static_cast<double>(dur_ticks) / quarter_ticks;
        dur_beats_sum += dur_beats;
        dur_beats_vec.push_back(dur_beats);
        if (dur_beats <= 0.35) ++staccato_ct;
        if (dur_beats >= 0.90) ++legato_ct;

        const uint8_t pc = p % 12u;
        const uint8_t rel = static_cast<uint8_t>((pc + 12u - decoded_chord.root) % 12u);
        if (chord_mask[pc]) ++chord_ct;
        if (rel == 0) ++root_ct;
        else if (rel == 3 || rel == 4) ++third_ct;
        else if (rel == 7) ++fifth_ct;
        else if (rel == 10 || rel == 11) ++seventh_ct;

        if (isKickSnareKey(p)) ++kick_snare_ct;
        if (isHatCymbalKey(p)) ++hat_cym_ct;
        if (isAuxPercKey(p))   ++aux_perc_ct;
        if (isFxCymbalKey(p))  ++fx_cym_ct;

        const uint32_t bar_idx = static_cast<uint32_t>(
            std::min<uint64_t>(uint64_t{eff_bars - 1u}, uint64_t{ns.on_tick} / bar_ticks));
        active_bar_set.insert(bar_idx);

        if (uint64_t{ns.on_tick} <= beat_ticks / 2ULL ||
            (total_ticks > beat_ticks && uint64_t{ns.on_tick} >= total_ticks - beat_ticks)) {
            ++boundary_ct;
        }

        const uint64_t in_bar = uint64_t{ns.on_tick} % bar_ticks;
        const size_t slot16 = std::min<size_t>(
            15u, static_cast<size_t>(std::floor((static_cast<double>(in_bar) / slot16_ticks) + 0.5)) % 16u);
        grid16_counts[slot16]++;
        if (bar_idx < grid_bars) {
            per_bar_grid[bar_idx][slot16]++;
        }

        // Beat alignment & syncopation
        const double beat_pos = static_cast<double>(in_bar) / static_cast<double>(beat_ticks);
        const double nearest_beat = std::round(beat_pos);
        const double beat_err_ticks = std::abs(beat_pos - nearest_beat) * static_cast<double>(beat_ticks);

        if (in_bar <= static_cast<uint64_t>(tol_ticks) ||
            (bar_ticks > in_bar && (bar_ticks - in_bar) <= static_cast<uint64_t>(tol_ticks))) {
            ++downbeat_ct;
        } else if (eff_num == 4u && beat_err_ticks <= tol_ticks) {
            const uint32_t bi = static_cast<uint32_t>(nearest_beat) % eff_num;
            if (bi == 1u || bi == 3u) ++backbeat_ct;
            if (bi == 0u) ++downbeat_ct;
        } else if (eff_num == 3u && beat_err_ticks <= tol_ticks) {
            const uint32_t bi = static_cast<uint32_t>(nearest_beat) % eff_num;
            if (bi == 1u || bi == 2u) ++backbeat_ct;
            if (bi == 0u) ++downbeat_ct;
        }

        // Subdivision classification (within a quarter note)
        const double in_q = std::fmod(static_cast<double>(ns.on_tick), quarter_ticks);
        const double frac_q = in_q / quarter_ticks;
        const double d_eighth = std::min({std::abs(frac_q - 0.0), std::abs(frac_q - 0.5), std::abs(frac_q - 1.0)});
        const double d_16th   = std::min(std::abs(frac_q - 0.25), std::abs(frac_q - 0.75));
        const double d_trip   = std::min(std::abs(frac_q - (1.0 / 3.0)), std::abs(frac_q - (2.0 / 3.0)));
        if (d_16th < 0.08 && d_16th < d_eighth && d_16th < d_trip) {
            ++sixteenth_ct;
        } else if (d_trip < 0.08 && d_trip < d_eighth) {
            ++triplet_ct;
        } else if (d_eighth < 0.10) {
            ++eighth_ct;
        }

        // Swing offbeat measurement (notes landing in [0.45..0.74] of a quarter beat)
        if (frac_q >= 0.45 && frac_q <= 0.74) {
            offbeat_ratios.push_back(frac_q);
        }

        // Microtiming error relative to nearest 16th grid
        const double step16_q = quarter_ticks / 4.0;
        const double nearest16 = std::round(static_cast<double>(ns.on_tick) / step16_q) * step16_q;
        micro_errors.push_back(static_cast<double>(ns.on_tick) - nearest16);

        // Syncopation: off-beat onset (not near integer beat) OR weak-beat anticipation crossing a beat
        const bool is_on_beat = (beat_err_ticks <= tol_ticks);
        if (!is_on_beat) {
            ++syncopated_ct;
            if (frac_q >= 0.70 || (bar_ticks > in_bar && (bar_ticks - in_bar) <= beat_ticks / 2u)) {
                ++anticipation_ct;
            }
        }
    }

    std::sort(pitches.begin(), pitches.end());
    f.pitch_min    = p_min;
    f.pitch_max    = p_max;
    f.pitch_span   = static_cast<uint8_t>(p_max - p_min);
    f.pitch_median = pitches[n / 2u];
    f.pitch_mean   = pitch_sum / static_cast<double>(n);

    double pitch_var = 0.0;
    uint32_t max_pc_count = 0;
    uint8_t unique_p = 0;
    for (uint8_t p = 0; p < 128; ++p) {
        if (pitch_counts[p] > 0) {
            ++unique_p;
            max_pc_count = std::max(max_pc_count, pitch_counts[p]);
        }
    }
    f.unique_pitch_count = unique_p;
    f.max_pitch_dominance = static_cast<double>(max_pc_count) / static_cast<double>(n);
    for (uint8_t p : pitches) {
        const double d = static_cast<double>(p) - f.pitch_mean;
        pitch_var += d * d;
    }
    f.pitch_stddev = std::sqrt(pitch_var / static_cast<double>(n));

    f.register_band      = registerBandForPitch(f.pitch_median);
    f.low_register_ratio = static_cast<double>(low_ct) / static_cast<double>(n);
    f.mid_register_ratio = static_cast<double>(mid_ct) / static_cast<double>(n);
    f.high_register_ratio = static_cast<double>(high_ct) / static_cast<double>(n);

    f.mean_velocity = vel_sum / static_cast<double>(n);
    f.vel_min = v_min;
    f.vel_max = v_max;
    f.velocity_dynamic_range = static_cast<uint8_t>(v_max - v_min);
    double vel_var = 0.0;
    uint32_t accent_ct = 0;
    for (const auto& ns : notes) {
        const double dv = static_cast<double>(ns.vel) - f.mean_velocity;
        vel_var += dv * dv;
        if (ns.vel >= 100 || static_cast<double>(ns.vel) >= f.mean_velocity + 15.0) {
            ++accent_ct;
        }
    }
    f.vel_stddev   = std::sqrt(vel_var / static_cast<double>(n));
    f.accent_ratio = static_cast<double>(accent_ct) / static_cast<double>(n);

    f.mean_duration_beats = dur_beats_sum / static_cast<double>(n);
    double dur_var = 0.0;
    for (double db : dur_beats_vec) {
        const double dd = db - f.mean_duration_beats;
        dur_var += dd * dd;
    }
    f.duration_stddev_beats = std::sqrt(dur_var / static_cast<double>(n));
    f.staccato_ratio        = static_cast<double>(staccato_ct) / static_cast<double>(n);
    f.legato_ratio          = static_cast<double>(legato_ct) / static_cast<double>(n);
    f.ghost_note_ratio      = static_cast<double>(ghost_ct) / static_cast<double>(n);

    f.chord_tone_ratio     = static_cast<double>(chord_ct) / static_cast<double>(n);
    f.root_ratio           = static_cast<double>(root_ct) / static_cast<double>(n);
    f.third_ratio          = static_cast<double>(third_ct) / static_cast<double>(n);
    f.fifth_ratio          = static_cast<double>(fifth_ct) / static_cast<double>(n);
    f.seventh_ratio        = static_cast<double>(seventh_ct) / static_cast<double>(n);
    f.non_chord_tone_ratio = 1.0 - f.chord_tone_ratio;

    f.drum_kick_snare_ratio = static_cast<double>(kick_snare_ct) / static_cast<double>(n);
    f.drum_hat_cymbal_ratio = static_cast<double>(hat_cym_ct) / static_cast<double>(n);
    f.drum_aux_perc_ratio   = static_cast<double>(aux_perc_ct) / static_cast<double>(n);
    f.drum_fx_cymbal_ratio  = static_cast<double>(fx_cym_ct) / static_cast<double>(n);
    f.boundary_note_ratio   = static_cast<double>(boundary_ct) / static_cast<double>(n);

    f.active_bars      = static_cast<uint32_t>(active_bar_set.size());
    f.active_bar_ratio = static_cast<double>(f.active_bars) / static_cast<double>(eff_bars);
    f.notes_per_bar    = static_cast<double>(n) / static_cast<double>(eff_bars);

    f.downbeat_ratio              = static_cast<double>(downbeat_ct) / static_cast<double>(n);
    f.backbeat_ratio              = static_cast<double>(backbeat_ct) / static_cast<double>(n);
    f.eighth_subdivision_ratio    = static_cast<double>(eighth_ct) / static_cast<double>(n);
    f.sixteenth_subdivision_ratio = static_cast<double>(sixteenth_ct) / static_cast<double>(n);
    f.triplet_subdivision_ratio   = static_cast<double>(triplet_ct) / static_cast<double>(n);
    f.syncopation_ratio           = static_cast<double>(syncopated_ct) / static_cast<double>(n);
    f.anticipation_ratio          = static_cast<double>(anticipation_ct) / static_cast<double>(n);

    for (size_t s = 0; s < 16u; ++s) {
        f.onset_grid_16th[s] = static_cast<double>(grid16_counts[s]) / static_cast<double>(n);
    }
    if (!offbeat_ratios.empty()) {
        std::sort(offbeat_ratios.begin(), offbeat_ratios.end());
        f.swing_ratio_estimate = offbeat_ratios[offbeat_ratios.size() / 2u];
        f.swing_offbeat_count = static_cast<uint32_t>(offbeat_ratios.size());
    } else {
        f.swing_ratio_estimate = 0.50;
        f.swing_offbeat_count = 0;
    }
    if (!micro_errors.empty()) {
        double mmean = 0.0;
        for (double e : micro_errors) mmean += e;
        mmean /= static_cast<double>(micro_errors.size());
        double mvar = 0.0;
        for (double e : micro_errors) {
            const double d = e - mmean;
            mvar += d * d;
        }
        f.microtiming_stddev_ticks = std::sqrt(mvar / static_cast<double>(micro_errors.size()));
    }

    // ── Duty cycle via union of sounding intervals (P1-2 & P2-3: uint64_t underflow-safe) ──
    {
        std::vector<std::pair<uint64_t, uint64_t>> ivs;
        ivs.reserve(n);
        for (const auto& ns : notes) {
            if (uint64_t{ns.on_tick} >= total_ticks) continue;
            const uint64_t s = uint64_t{ns.on_tick};
            const uint64_t e = std::min<uint64_t>(total_ticks, safeOffTick64(ns.on_tick, ns.off_tick));
            if (e > s) ivs.push_back({s, e});
        }
        if (!ivs.empty()) {
            std::sort(ivs.begin(), ivs.end());
            uint64_t covered = 0;
            uint64_t cur_s = ivs[0].first;
            uint64_t cur_e = ivs[0].second;
            for (size_t i = 1; i < ivs.size(); ++i) {
                if (ivs[i].first <= cur_e) {
                    cur_e = std::max(cur_e, ivs[i].second);
                } else {
                    if (cur_e > cur_s) covered += (cur_e - cur_s);
                    cur_s = ivs[i].first;
                    cur_e = ivs[i].second;
                }
            }
            if (cur_e > cur_s) covered += (cur_e - cur_s);
            f.duty_cycle = clamp01(static_cast<double>(covered) /
                                   static_cast<double>(std::max<uint64_t>(1ULL, total_ticks)));
        } else {
            f.duty_cycle = 0.0;
        }
    }

    // ── Group 10: Polyphony, chord clusters, strum offsets, two-hand split, dissonance ──
    {
        // Sweep line for exact max & time-weighted mean polyphony (uint64_t overflow-safe)
        struct Edge { uint64_t tick; int delta; };
        std::vector<Edge> edges;
        edges.reserve(n * 2u);
        for (const auto& ns : notes) {
            edges.push_back({uint64_t{ns.on_tick}, +1});
            edges.push_back({safeOffTick64(ns.on_tick, ns.off_tick), -1});
        }
        std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            return a.delta < b.delta; // process NoteOff before NoteOn at identical tick
        });
        int cur_poly = 0;
        int max_poly = 0;
        uint64_t poly_weighted = 0;
        uint64_t active_dur = 0;
        for (size_t i = 0; i < edges.size(); ++i) {
            if (i > 0 && edges[i].tick > edges[i - 1].tick && cur_poly > 0) {
                const uint64_t dt = static_cast<uint64_t>(edges[i].tick - edges[i - 1].tick);
                poly_weighted += dt * static_cast<uint64_t>(cur_poly);
                active_dur += dt;
            }
            cur_poly += edges[i].delta;
            if (cur_poly > max_poly) max_poly = cur_poly;
        }
        f.max_polyphony  = static_cast<uint8_t>(std::min<int>(max_poly, 127));
        f.mean_polyphony = (active_dur > 0) ? (static_cast<double>(poly_weighted) / static_cast<double>(active_dur)) : 0.0;

        // Group onsets into exact-simultaneous clusters (<= 3 ticks) and strum clusters (<= 45 ticks)
        const uint32_t strum_max_gap = std::max<uint32_t>(
            12u, std::min<uint32_t>(45u, static_cast<uint32_t>((uint64_t{eff_ppqn} * 45u) / 480u)));
        struct Cluster {
            uint32_t start_tick = 0;
            uint32_t end_tick = 0;
            std::vector<uint8_t> pitches;
            std::vector<uint32_t> ticks;
        };
        std::vector<Cluster> clusters;
        for (const auto& ns : notes) {
            if (!clusters.empty() && ns.on_tick >= clusters.back().end_tick &&
                (ns.on_tick - clusters.back().end_tick) <= strum_max_gap &&
                (ns.on_tick - clusters.back().start_tick) <= strum_max_gap * 3u &&
                std::find(clusters.back().pitches.begin(), clusters.back().pitches.end(), ns.pitch) ==
                    clusters.back().pitches.end()) {
                clusters.back().end_tick = ns.on_tick;
                clusters.back().pitches.push_back(ns.pitch);
                clusters.back().ticks.push_back(ns.on_tick);
            } else {
                Cluster cl{};
                cl.start_tick = ns.on_tick;
                cl.end_tick = ns.on_tick;
                cl.pitches.push_back(ns.pitch);
                cl.ticks.push_back(ns.on_tick);
                clusters.push_back(std::move(cl));
            }
        }

        uint32_t chord_clusters = 0;
        uint32_t triad_clusters = 0;
        uint32_t mono_clusters = 0;
        uint32_t strum_clusters = 0;
        uint32_t two_hand_clusters = 0;
        uint32_t dissonant_pairs = 0;
        uint32_t total_pairs = 0;
        double strum_spread_sum = 0.0;

        for (const auto& cl : clusters) {
            const size_t sz = cl.pitches.size();
            if (sz >= 2u) {
                ++chord_clusters;
                if (sz >= 3u) ++triad_clusters;
                const uint32_t spread = cl.end_tick - cl.start_tick;
                if (sz >= 3u && spread >= 3u && spread <= strum_max_gap * 3u) {
                    // Check monotonic or consistent staggered strum ticks (at least 2 distinct ticks)
                    bool monotonic_pitch = true;
                    bool asc = (cl.pitches[1] >= cl.pitches[0]);
                    uint32_t distinct_ticks = 1;
                    for (size_t k = 1; k < sz; ++k) {
                        if (cl.ticks[k] > cl.ticks[k - 1]) ++distinct_ticks;
                        if (asc && cl.pitches[k] < cl.pitches[k - 1]) monotonic_pitch = false;
                        if (!asc && cl.pitches[k] > cl.pitches[k - 1]) monotonic_pitch = false;
                    }
                    if (distinct_ticks >= 3u && monotonic_pitch) {
                        ++strum_clusters;
                        strum_spread_sum += static_cast<double>(spread) / static_cast<double>(sz - 1u);
                    }
                }
                uint8_t lo = 127, hi = 0;
                for (uint8_t p : cl.pitches) {
                    lo = std::min(lo, p);
                    hi = std::max(hi, p);
                }
                if (lo <= 54 && hi >= 60 && (hi - lo) >= 14) {
                    ++two_hand_clusters;
                }
                for (size_t a = 0; a < sz; ++a) {
                    for (size_t b = a + 1; b < sz; ++b) {
                        ++total_pairs;
                        const uint8_t iv = static_cast<uint8_t>(std::abs(int(cl.pitches[a]) - int(cl.pitches[b])) % 12);
                        if (iv == 1 || iv == 2 || iv == 6 || iv == 11) {
                            ++dissonant_pairs;
                        }
                    }
                }
            } else {
                ++mono_clusters;
            }
        }

        const double c_tot = static_cast<double>(std::max<size_t>(1u, clusters.size()));
        f.unique_onsets_per_bar = static_cast<double>(clusters.size()) / static_cast<double>(eff_bars);
        f.chordal_ratio         = static_cast<double>(chord_clusters) / c_tot;
        f.triad_plus_ratio      = static_cast<double>(triad_clusters) / c_tot;
        f.monophonic_ratio      = static_cast<double>(mono_clusters) / c_tot;
        f.strum_onset_ratio     = (chord_clusters > 0)
                                      ? (static_cast<double>(strum_clusters) / static_cast<double>(chord_clusters))
                                      : 0.0;
        f.has_strum_offsets     = (strum_clusters >= 2u || (strum_clusters >= 1u && f.strum_onset_ratio >= 0.30));
        f.mean_strum_spread_ticks = (strum_clusters > 0)
                                        ? (strum_spread_sum / static_cast<double>(strum_clusters))
                                        : 0.0;
        f.has_two_hand_split    = (chord_clusters > 0 &&
                                   static_cast<double>(two_hand_clusters) / static_cast<double>(chord_clusters) >= 0.30);
        f.dissonant_simultaneous_ratio = (total_pairs > 0)
                                             ? (static_cast<double>(dissonant_pairs) / static_cast<double>(total_pairs))
                                             : 0.0;

        // ── Group 7 & 11: Melodic contour, motif repetition, phrase boundaries ──
        std::vector<int> intervals;
        std::vector<uint32_t> cluster_ticks;
        cluster_ticks.reserve(clusters.size());
        uint8_t prev_top_pitch = 0;
        bool has_prev = false;
        uint32_t stepwise_ct = 0, chord_leap_ct = 0, large_leap_ct = 0;
        uint32_t max_mono_run = 1, cur_run = 1;
        int prev_dir = 0;

        for (const auto& cl : clusters) {
            cluster_ticks.push_back(cl.start_tick);
            const uint8_t top_p = *std::max_element(cl.pitches.begin(), cl.pitches.end());
            if (has_prev) {
                const int diff = static_cast<int>(top_p) - static_cast<int>(prev_top_pitch);
                const int ad = std::abs(diff);
                if (ad > 0) {
                    intervals.push_back(diff);
                    if (ad <= 2) ++stepwise_ct;
                    else if (ad <= 7) ++chord_leap_ct;
                    else ++large_leap_ct;

                    const int dir = (diff > 0) ? 1 : -1;
                    if (ad <= 3 && (prev_dir == 0 || dir == prev_dir)) {
                        ++cur_run;
                        max_mono_run = std::max(max_mono_run, cur_run);
                    } else {
                        cur_run = 1;
                    }
                    prev_dir = dir;
                }
            }
            prev_top_pitch = top_p;
            has_prev = true;
        }
        if (!intervals.empty()) {
            const double ic = static_cast<double>(intervals.size());
            f.interval_stepwise_ratio   = static_cast<double>(stepwise_ct) / ic;
            f.interval_chord_leap_ratio = static_cast<double>(chord_leap_ct) / ic;
            f.interval_large_leap_ratio = static_cast<double>(large_leap_ct) / ic;
            f.monotonic_ramp_ratio      = clamp01(static_cast<double>(max_mono_run) /
                                                  static_cast<double>(std::max<size_t>(1u, clusters.size())));
        }

        // Motif repetition: inter-bar 16th-grid cosine similarity (or intra-bar half-bar symmetry for 1-bar sections)
        if (grid_bars >= 2u) {
            double sim_sum = 0.0;
            uint32_t pairs = 0;
            for (uint32_t b = 1; b < grid_bars; ++b) {
                double dot = 0.0, na = 0.0, nb = 0.0;
                for (size_t s = 0; s < 16u; ++s) {
                    const double va = static_cast<double>(per_bar_grid[b - 1u][s]);
                    const double vb = static_cast<double>(per_bar_grid[b][s]);
                    dot += va * vb;
                    na += va * va;
                    nb += vb * vb;
                }
                if (na > 0.0 && nb > 0.0) {
                    sim_sum += dot / (std::sqrt(na) * std::sqrt(nb));
                    ++pairs;
                }
            }
            f.motif_repetition_ratio = (pairs > 0) ? clamp01(sim_sum / static_cast<double>(pairs)) : 0.0;
        } else {
            // 1-bar pattern: check half-bar rhythmic symmetry (slots 0..7 vs 8..15)
            double dot = 0.0, na = 0.0, nb = 0.0;
            for (size_t s = 0; s < 8u; ++s) {
                const double va = static_cast<double>(grid16_counts[s]);
                const double vb = static_cast<double>(grid16_counts[s + 8u]);
                dot += va * vb;
                na += va * va;
                nb += vb * vb;
            }
            f.motif_repetition_ratio = (na > 0.0 && nb > 0.0)
                                           ? clamp01(dot / (std::sqrt(na) * std::sqrt(nb)))
                                           : 0.0;
        }

        // Deterministic compact motif signature (up to 8 onset-slot/interval n-grams)
        {
            std::ostringstream ms;
            const size_t limit = std::min<size_t>(8u, intervals.size());
            ms << "g16:";
            for (size_t s = 0; s < 16u; ++s) {
                ms << (grid16_counts[s] > 0 ? '1' : '0');
            }
            ms << "|iv:";
            for (size_t k = 0; k < limit; ++k) {
                if (k > 0) ms << ',';
                ms << intervals[k];
            }
            f.motif_signature = ms.str();
        }

        // Phrase boundary segmentation via inter-onset gap >= 1.5 beats
        const uint64_t phrase_gap_ticks = std::max<uint64_t>(1ULL, (beat_ticks * 3ULL) / 2ULL);
        f.phrase_count = 1u;
        f.phrase_boundary_ticks.push_back(cluster_ticks.front());
        uint64_t max_gap = 0;
        double phrase_len_sum = 0.0;
        uint32_t cur_phrase_start = cluster_ticks.front();
        for (size_t i = 1; i < cluster_ticks.size(); ++i) {
            const uint64_t gap = (cluster_ticks[i] > cluster_ticks[i - 1])
                                     ? uint64_t{cluster_ticks[i] - cluster_ticks[i - 1]}
                                     : 0ULL;
            max_gap = std::max(max_gap, gap);
            if (gap >= phrase_gap_ticks) {
                const uint64_t plen = (cluster_ticks[i - 1] >= cur_phrase_start)
                                          ? (uint64_t{cluster_ticks[i - 1] - cur_phrase_start} + beat_ticks / 2ULL)
                                          : beat_ticks;
                phrase_len_sum += static_cast<double>(plen) / static_cast<double>(beat_ticks);
                ++f.phrase_count;
                if (f.phrase_boundary_ticks.size() < 64u) {
                    f.phrase_boundary_ticks.push_back(cluster_ticks[i]);
                }
                cur_phrase_start = cluster_ticks[i];
            }
        }
        {
            const uint64_t last_plen = (cluster_ticks.back() >= cur_phrase_start)
                                           ? (uint64_t{cluster_ticks.back() - cur_phrase_start} + beat_ticks / 2ULL)
                                           : beat_ticks;
            phrase_len_sum += static_cast<double>(last_plen) / static_cast<double>(beat_ticks);
        }
        f.mean_phrase_length_beats  = phrase_len_sum / static_cast<double>(std::max<uint32_t>(1u, f.phrase_count));
        f.inter_onset_gap_max_beats = static_cast<double>(max_gap) / static_cast<double>(beat_ticks);
    }

    return f;
}

inline InterTrackRelationships computeInterTrackRelationships(
    const std::vector<CanonicalTrackView>& tracks,
    const std::vector<std::vector<exporters::korg::quality::NoteSpan>>& track_notes,
    uint32_t ppqn) {
    InterTrackRelationships rel{};
    const uint32_t eff_ppqn = ppqn ? std::clamp<uint32_t>(ppqn, 1u, 19200u) : 480u;
    const uint64_t lock_tol = std::max<uint64_t>(16ULL, uint64_t{eff_ppqn} / 12ULL); // ~1/48th note tolerance

    // Find kick/drum onset ticks and bass onset ticks
    std::vector<uint64_t> kick_ticks;
    std::vector<uint64_t> bass_ticks;
    std::vector<exporters::korg::quality::NoteSpan> bass_spans;
    std::vector<exporters::korg::quality::NoteSpan> piano_or_comp_spans;

    for (size_t i = 0; i < tracks.size() && i < track_notes.size(); ++i) {
        const auto& tr = tracks[i];
        const auto& ns = track_notes[i];
        if (ns.empty()) continue;

        const bool has_valid_pitch = (tr.features.note_count > 0 && tr.features.pitch_median > 0);
        const bool looks_drum = tr.is_drum || tr.semantic_role == MusicalRole::KickSnareFoundation ||
                                tr.uasf_role == uasf::TrackRole::Drum ||
                                (has_valid_pitch && tr.features.drum_kick_snare_ratio >= 0.35 &&
                                 tr.features.staccato_ratio >= 0.75);
        const bool looks_bass = !looks_drum && has_valid_pitch &&
                                (tr.semantic_role == MusicalRole::BassFoundation ||
                                 (tr.semantic_role == MusicalRole::UnknownAbstain &&
                                  (tr.uasf_role == uasf::TrackRole::Bass ||
                                   (tr.features.pitch_median <= 50 && tr.features.mean_polyphony <= 1.25))));
        const bool looks_piano = !looks_drum && !looks_bass && has_valid_pitch &&
                                 (tr.semantic_role == MusicalRole::PianoAccompaniment ||
                                  tr.semantic_role == MusicalRole::ChordComping ||
                                  tr.semantic_role == MusicalRole::HarmonicAccompaniment ||
                                  tr.timbre.inferred.family == exporters::korg::quality::TimbreFamily::Piano ||
                                  tr.timbre.inferred.family == exporters::korg::quality::TimbreFamily::ElectricPiano ||
                                  (tr.features.chordal_ratio >= 0.40 && tr.features.pitch_min < 48));

        if (looks_drum) {
            for (const auto& n : ns) {
                if (n.pitch == 35 || n.pitch == 36) {
                    kick_ticks.push_back(uint64_t{n.on_tick});
                }
            }
        }
        if (looks_bass) {
            for (const auto& n : ns) {
                bass_ticks.push_back(uint64_t{n.on_tick});
                bass_spans.push_back(n);
            }
        }
        if (looks_piano) {
            for (const auto& n : ns) {
                if (n.pitch < 48) {
                    piano_or_comp_spans.push_back(n);
                }
            }
        }
    }

    if (!kick_ticks.empty() && !bass_ticks.empty()) {
        std::sort(kick_ticks.begin(), kick_ticks.end());
        kick_ticks.erase(std::unique(kick_ticks.begin(), kick_ticks.end()), kick_ticks.end());
        std::sort(bass_ticks.begin(), bass_ticks.end());
        bass_ticks.erase(std::unique(bass_ticks.begin(), bass_ticks.end()), bass_ticks.end());

        uint32_t kick_matched = 0;
        for (uint64_t kt : kick_ticks) {
            const uint64_t lo = (kt > lock_tol) ? (kt - lock_tol) : 0ULL;
            const uint64_t hi = (kt <= UINT64_MAX - lock_tol) ? (kt + lock_tol) : UINT64_MAX;
            auto it = std::lower_bound(bass_ticks.begin(), bass_ticks.end(), lo);
            if (it != bass_ticks.end() && (*it <= hi)) {
                ++kick_matched;
            }
        }
        uint32_t bass_matched = 0;
        for (uint64_t bt : bass_ticks) {
            const uint64_t lo = (bt > lock_tol) ? (bt - lock_tol) : 0ULL;
            const uint64_t hi = (bt <= UINT64_MAX - lock_tol) ? (bt + lock_tol) : UINT64_MAX;
            auto it = std::lower_bound(kick_ticks.begin(), kick_ticks.end(), lo);
            if (it != kick_ticks.end() && (*it <= hi)) {
                ++bass_matched;
            }
        }
        const double prec = static_cast<double>(bass_matched) / static_cast<double>(bass_ticks.size());
        const double rec  = static_cast<double>(kick_matched) / static_cast<double>(kick_ticks.size());
        rel.kick_bass_lock_correlation = (prec + rec > 0.0) ? ((2.0 * prec * rec) / (prec + rec)) : 0.0;
    }

    // Bass vs Piano/Comp low-register concurrent collision check (< C3 = MIDI 48)
    if (!bass_spans.empty() && !piano_or_comp_spans.empty()) {
        uint32_t collide_ct = 0;
        for (const auto& pn : piano_or_comp_spans) {
            for (const auto& bn : bass_spans) {
                if (pn.on_tick < bn.off_tick && bn.on_tick < pn.off_tick) {
                    ++collide_ct;
                    break;
                }
            }
        }
        rel.bass_piano_low_collision_notes = collide_ct;
        rel.bass_piano_low_collision = (collide_ct > 0);
    }

    // Pairwise melodic register overlap matrix
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& a = tracks[i];
        if (a.is_drum || isPercussiveRole(a.semantic_role) || a.features.note_count == 0) continue;
        for (size_t j = i + 1; j < tracks.size(); ++j) {
            const auto& b = tracks[j];
            if (b.is_drum || isPercussiveRole(b.semantic_role) || b.features.note_count == 0) continue;

            const uint8_t ov_lo = std::max(a.features.pitch_min, b.features.pitch_min);
            const uint8_t ov_hi = std::min(a.features.pitch_max, b.features.pitch_max);
            if (ov_hi >= ov_lo) {
                const uint8_t ov_st = static_cast<uint8_t>(ov_hi - ov_lo + 1u);
                const uint8_t min_span = std::max<uint8_t>(
                    1u, std::min<uint8_t>(static_cast<uint8_t>(a.features.pitch_span + 1u),
                                          static_cast<uint8_t>(b.features.pitch_span + 1u)));
                const double ratio = clamp01(static_cast<double>(ov_st) / static_cast<double>(min_span));

                const bool involves_bass = (a.features.pitch_median <= 50 || b.features.pitch_median <= 50 ||
                                            a.semantic_role == MusicalRole::BassFoundation ||
                                            b.semantic_role == MusicalRole::BassFoundation);
                const bool warn = (involves_bass && ov_lo < 48 && ov_st >= 4) || (ratio >= 0.75 && ov_st >= 12);

                RegisterOverlapEntry e{};
                e.channel_a = a.source_channel;
                e.channel_b = b.source_channel;
                e.role_a = a.semantic_role;
                e.role_b = b.semantic_role;
                e.overlap_low = ov_lo;
                e.overlap_high = ov_hi;
                e.overlap_semitones = ov_st;
                e.overlap_ratio = ratio;
                e.collision_warning = warn;
                if (involves_bass && warn) {
                    e.note = "low_register_bass_intrusion";
                } else if (warn) {
                    e.note = "heavy_mid_register_overlap";
                } else {
                    e.note = "benign_register_touch";
                }
                rel.register_overlaps.push_back(std::move(e));
            }
        }
    }

    return rel;
}

inline uasf::SectionType sectionTypeFromKorgMarker(std::string_view marker) noexcept {
    if (marker.size() >= 2 && marker[0] == 'i') {
        if (marker[1] == '1') return uasf::SectionType::Intro1;
        if (marker[1] == '2') return uasf::SectionType::Intro2;
        return uasf::SectionType::Intro3;
    }
    if (marker.size() >= 2 && marker[0] == 'v') {
        if (marker[1] == '1') return uasf::SectionType::Main1;
        if (marker[1] == '2') return uasf::SectionType::Main2;
        if (marker[1] == '3') return uasf::SectionType::Main3;
        return uasf::SectionType::Main4;
    }
    if (marker.size() >= 2 && marker[0] == 'f') {
        if (marker[1] == '1') return uasf::SectionType::Fill1;
        if (marker[1] == '2') return uasf::SectionType::Fill2;
        if (marker[1] == '3') return uasf::SectionType::Fill3;
        return uasf::SectionType::Fill4;
    }
    if (marker.size() >= 2 && marker[0] == 'e') {
        if (marker[1] == '1') return uasf::SectionType::Ending1;
        if (marker[1] == '2') return uasf::SectionType::Ending2;
        return uasf::SectionType::Ending3;
    }
    if (marker == "br") return uasf::SectionType::Break;
    return uasf::SectionType::Main1;
}

inline std::string sectionBasePrefix(uasf::SectionType t) {
    switch (t) {
        case uasf::SectionType::Intro1:  return "i1";
        case uasf::SectionType::Intro2:  return "i2";
        case uasf::SectionType::Intro3:  return "i3";
        case uasf::SectionType::Main1:   return "v1";
        case uasf::SectionType::Main2:   return "v2";
        case uasf::SectionType::Main3:   return "v3";
        case uasf::SectionType::Main4:   return "v4";
        case uasf::SectionType::Fill1:   return "f1";
        case uasf::SectionType::Fill2:   return "f2";
        case uasf::SectionType::Fill3:   return "f3";
        case uasf::SectionType::Fill4:   return "f4";
        case uasf::SectionType::Ending1: return "e1";
        case uasf::SectionType::Ending2: return "e2";
        case uasf::SectionType::Ending3: return "e3";
        case uasf::SectionType::Break:   return "br";
    }
    return "v1";
}

inline double computeSectionCompositeEnergy(const std::vector<CanonicalTrackView>& tracks) noexcept {
    double density_sum = 0.0;
    double vel_weighted = 0.0;
    uint32_t note_total = 0;
    uint32_t active_tracks = 0;
    for (const auto& tr : tracks) {
        if (tr.features.note_count == 0) continue;
        ++active_tracks;
        density_sum += tr.features.notes_per_bar;
        vel_weighted += tr.features.mean_velocity * static_cast<double>(tr.features.note_count);
        note_total += tr.features.note_count;
    }
    if (note_total == 0) return 0.0;
    const double mean_vel = vel_weighted / static_cast<double>(note_total);
    const double density_norm = clamp01(density_sum / 64.0);
    const double vel_norm = clamp01(mean_vel / 127.0);
    const double track_norm = clamp01(static_cast<double>(active_tracks) / 8.0);
    return 100.0 * (0.45 * density_norm + 0.35 * vel_norm + 0.20 * track_norm);
}

inline std::string fmtDouble(double v, int prec = 4) {
    if (!std::isfinite(v)) v = 0.0;
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

} // namespace canonical_detail

// Builds a zero-copy-payload CanonicalStyleView over an existing UASF StyleDefinition.
inline CanonicalStyleView buildCanonicalStyleView(
    const uasf::StyleDefinition& uasf_style,
    const exporters::korg::Style* korg_style = nullptr,
    const exporters::korg::Report* korg_report = nullptr,
    TargetPlatformId target_platform = TargetPlatformId::KorgPa5X) {
    CanonicalStyleView view{};
    view.style_name = uasf_style.name;
    view.tempo_bpm = uasf_style.tempo_bpm ? uasf_style.tempo_bpm : uasf::kDefaultTempoBpm;
    view.ppqn = uasf_style.resolution ? uasf_style.resolution : 480u;
    view.source_uasf = &uasf_style;
    view.target_korg = korg_style;
    view.target_report = korg_report;
    view.target_platform = target_platform;
    view.target_capability = &targetCapabilityProfile(target_platform);
    view.hardware_verified = false;
    view.l3_status = "NOT_TESTED";

    view.provenance.source_format = uasf_style.format_version.empty() ? "UASF/1.2" : ("UASF/" + uasf_style.format_version);
    view.provenance.source_name = uasf_style.name;
    view.provenance.hardware_verified = false;
    view.provenance.l3_status = "NOT_TESTED";

    // Pre-compute style-wide timbre profiles using existing Stage-5 profiler (zero duplication!)
    const auto timbre_profiles = exporters::korg::quality::profileStyleTracks(uasf_style, uasf_style.korg_export.route);
    std::map<uint8_t, exporters::korg::quality::TrackTimbreProfile> timbre_by_src;
    for (const auto& tp : timbre_profiles) {
        timbre_by_src[tp.src_ch] = tp;
    }

    // Map optional report track rows by source channel for target_channel hints
    std::map<uint8_t, uint8_t> report_korg_ch;
    if (korg_report) {
        for (const auto& row : korg_report->tracks) {
            if (row.korg_ch <= 15) {
                report_korg_ch[row.src_ch] = row.korg_ch;
            }
        }
    }

    // Collect CV markers per section prefix if korg_style is provided
    std::map<std::string, std::vector<std::string>> cv_markers_by_prefix;
    if (korg_style) {
        for (const auto& el : korg_style->elements) {
            const std::string pfx = (el.marker == "br") ? "br" : el.marker.substr(0, std::min<size_t>(2u, el.marker.size()));
            cv_markers_by_prefix[pfx].push_back(el.marker);
        }
        view.transitions.total_cv_elements = static_cast<uint32_t>(korg_style->elements.size());
    }

    // Aggregate per-channel notes across ALL active sections for overall inter-track metrics (P1-1 / P2-3)
    std::map<uint8_t, CanonicalTrackView> overall_tracks_map;
    std::map<uint8_t, std::vector<exporters::korg::quality::NoteSpan>> overall_notes_map;
    uint32_t running_tick_offset = 0;
    uint32_t total_style_bars = 0;

    view.sections.reserve(uasf_style.sections.size());
    for (size_t sidx = 0; sidx < uasf_style.sections.size(); ++sidx) {
        const auto& sec = uasf_style.sections[sidx];
        CanonicalSectionView sv{};
        sv.uasf_section_index = sidx;
        sv.type = sec.type;
        sv.name = sec.name;
        sv.bars = sec.bars ? sec.bars : 1u;
        sv.ppqn = sec.resolution ? std::clamp<uint32_t>(sec.resolution, 1u, 19200u)
                                 : std::clamp<uint32_t>(view.ppqn, 1u, 19200u);
        sv.meter_num = sec.beats_per_bar ? static_cast<uint8_t>(std::min<uint32_t>(sec.beats_per_bar, 32u)) : 4u;
        sv.meter_den = sec.beat_note ? static_cast<uint8_t>(std::min<uint32_t>(sec.beat_note, 32u)) : 4u;

        const uint64_t beat_ticks64 = std::max<uint64_t>(1ULL, (uint64_t{sv.ppqn} * 4ULL) / std::max<uint64_t>(1ULL, sv.meter_den));
        const uint64_t bar_ticks64  = std::max<uint64_t>(1ULL, beat_ticks64 * std::max<uint64_t>(1ULL, sv.meter_num));
        const uint64_t len64        = (uint64_t{sv.bars} > uint64_t{UINT32_MAX} / bar_ticks64)
                                          ? uint64_t{UINT32_MAX}
                                          : (bar_ticks64 * uint64_t{sv.bars});
        sv.length_ticks = static_cast<uint32_t>(len64);
        sv.loop_start_tick = 0;
        sv.loop_end_tick = sv.length_ticks;

        const std::string pfx = canonical_detail::sectionBasePrefix(sec.type);
        const char* mk = exporters::korg::korgMarkerFor(sec.type, exporters::korg::BreakPolicy::Yamaha1BarCap);
        sv.korg_marker = mk ? mk : "";
        if (auto it = cv_markers_by_prefix.find(pfx); it != cv_markers_by_prefix.end() && !it->second.empty()) {
            sv.cv_markers = it->second;
            sv.cv_count = static_cast<uint8_t>(std::min<size_t>(it->second.size(), 255u));
        } else {
            sv.cv_count = 1;
            if (!sv.korg_marker.empty()) sv.cv_markers.push_back(sv.korg_marker);
        }

        switch (sec.type) {
            case uasf::SectionType::Intro1:
            case uasf::SectionType::Intro2:
            case uasf::SectionType::Intro3:  ++view.transitions.intro_count; break;
            case uasf::SectionType::Main1:
            case uasf::SectionType::Main2:
            case uasf::SectionType::Main3:
            case uasf::SectionType::Main4:   ++view.transitions.variation_count; break;
            case uasf::SectionType::Fill1:
            case uasf::SectionType::Fill2:
            case uasf::SectionType::Fill3:
            case uasf::SectionType::Fill4:   ++view.transitions.fill_count; break;
            case uasf::SectionType::Break:   ++view.transitions.break_count; break;
            case uasf::SectionType::Ending1:
            case uasf::SectionType::Ending2:
            case uasf::SectionType::Ending3: ++view.transitions.ending_count; break;
        }

        std::vector<std::vector<exporters::korg::quality::NoteSpan>> sec_track_notes;
        sec_track_notes.reserve(sec.tracks.size());
        sv.tracks.reserve(sec.tracks.size());

        double swing_weighted = 0.0;
        double swing_weight_sum = 0.0;

        for (const auto& tr : sec.tracks) {
            CanonicalTrackView tv{};
            tv.source_channel = tr.midi_channel;
            if (auto rit = report_korg_ch.find(tr.midi_channel); rit != report_korg_ch.end()) {
                tv.target_channel = rit->second;
            } else {
                const uint8_t routed = (tr.articulation.has_casm_meta && tr.articulation.dest_channel <= 15)
                                           ? tr.articulation.dest_channel
                                           : tr.midi_channel;
                tv.target_channel = exporters::korg::korgChannelFor(routed);
            }
            tv.name = tr.name;
            tv.uasf_role = tr.role;
            const auto xg_it = uasf_style.xg_sysex.parts.find(tr.midi_channel);
            const bool xg_drum_mode = (xg_it != uasf_style.xg_sysex.parts.end() && xg_it->second.isDrumMode());
            tv.is_drum = tr.is_drum || tr.role == uasf::TrackRole::Drum ||
                         tr.role == uasf::TrackRole::Percussion || xg_drum_mode;

            // Populate CASM view without duplicating logic
            tv.casm.src_ch = tr.midi_channel;
            tv.casm.dest_ch = (tr.articulation.has_casm_meta && tr.articulation.dest_channel <= 15)
                                  ? tr.articulation.dest_channel
                                  : tr.midi_channel;
            tv.casm.ntr = tr.articulation.ntr;
            tv.casm.ntt = tr.articulation.ntt;
            tv.casm.ntt_bass = tr.articulation.ntt_bass;
            tv.casm.high_key = tr.articulation.high_key;
            tv.casm.note_low = tr.articulation.note_low;
            tv.casm.note_high = tr.articulation.note_high;
            tv.casm.rtr = tr.articulation.rtr;
            tv.casm.source_chord_root = tr.articulation.source_chord_root;
            tv.casm.source_chord_type = tr.articulation.source_chord_type;
            tv.casm.note_mute_mask = tr.articulation.note_mute_mask;
            tv.casm.chord_mute_mask = tr.articulation.chord_mute_mask;
            tv.casm.has_casm = tr.articulation.has_casm_meta;

            if (auto tit = timbre_by_src.find(tr.midi_channel); tit != timbre_by_src.end()) {
                tv.timbre = tit->second;
            }

            const auto ctrl = canonical_detail::extractControllersFromUasf(tr.events);
            const auto spans = exporters::korg::quality::extractNoteSpansFromEvents(tr.events, sv.length_ticks);

            tv.sound_identity.msb = (ctrl.msb >= 0) ? ctrl.msb : tv.timbre.yamaha_msb;
            tv.sound_identity.lsb = (ctrl.lsb >= 0) ? ctrl.lsb : tv.timbre.yamaha_lsb;
            tv.sound_identity.pc  = (ctrl.pc  >= 0) ? ctrl.pc  : tv.timbre.yamaha_pc;
            tv.sound_identity.is_drum_bank = isKnownArrangerDrumBank(tv.sound_identity.msb, tv.sound_identity.lsb);
            tv.sound_identity.is_drum_kit = tv.is_drum || tv.sound_identity.is_drum_bank ||
                                            exporters::korg::quality::isPercussiveTimbreFamily(tv.timbre.inferred.family);
            tv.sound_identity.family = exporters::korg::quality::timbreFamilyName(tv.timbre.inferred.family);
            tv.sound_identity.sub_family = tv.timbre.inferred.sub_family;

            tv.features = canonical_detail::computeTrackFeatures(
                spans, ctrl, sv.ppqn, sv.meter_num, sv.meter_den, sv.bars,
                tv.casm.source_chord_root, tv.casm.source_chord_type);

            tv.harmonic.ntr = tv.casm.ntr;
            tv.harmonic.ntt = tv.casm.ntt;
            tv.harmonic.ntt_bass = tv.casm.ntt_bass;
            tv.harmonic.source_chord_root = tv.features.source_chord_root;
            tv.harmonic.source_chord_type = tv.casm.source_chord_type;
            tv.harmonic.source_chord_name = tv.features.source_chord_type_name;
            tv.harmonic.high_key = tv.casm.high_key;
            tv.harmonic.note_low = tv.casm.note_low;
            tv.harmonic.note_high = tv.casm.note_high;
            tv.harmonic.chord_tone_ratio = tv.features.chord_tone_ratio;
            tv.harmonic.root_ratio = tv.features.root_ratio;
            tv.harmonic.third_ratio = tv.features.third_ratio;
            tv.harmonic.fifth_ratio = tv.features.fifth_ratio;
            tv.harmonic.seventh_ratio = tv.features.seventh_ratio;
            tv.harmonic.non_chord_tone_ratio = tv.features.non_chord_tone_ratio;

            tv.provenance.origin = DecisionOrigin::Deterministic;
            tv.provenance.component_id = "p5_canonical_view_v1";
            tv.provenance.confidence = 1.0;
            tv.provenance.abstained = false;
            tv.provenance.reversible = true;
            tv.provenance.rationale = "Zero-copy semantic overlay extracted from UASF StyleDefinition.";

            if (tv.features.swing_offbeat_count >= 2u) {
                const double w = (tv.is_drum ? 2.5 : 1.0) * std::min<double>(8.0, static_cast<double>(tv.features.swing_offbeat_count));
                swing_weighted += tv.features.swing_ratio_estimate * w;
                swing_weight_sum += w;
            }

            // Accumulate overall track spans across ALL active sections (P1-1: never keep empty section-0 stub!)
            if (overall_tracks_map.find(tv.source_channel) == overall_tracks_map.end()) {
                overall_tracks_map[tv.source_channel] = tv;
            } else {
                auto& ot = overall_tracks_map[tv.source_channel];
                if (ot.features.note_count == 0 && tv.features.note_count > 0) {
                    ot = tv;
                } else if (tv.features.note_count > 0) {
                    ot.is_drum = ot.is_drum || tv.is_drum;
                    if (ot.name.empty() && !tv.name.empty()) ot.name = tv.name;
                }
            }
            auto& dst_notes = overall_notes_map[tv.source_channel];
            for (auto ns : spans) {
                ns.on_tick  = canonical_detail::saturatingAddU32(ns.on_tick, running_tick_offset);
                ns.off_tick = canonical_detail::saturatingAddU32(ns.off_tick, running_tick_offset);
                dst_notes.push_back(ns);
            }

            sv.tracks.push_back(std::move(tv));
            sec_track_notes.push_back(spans);
        }

        sv.composite_energy = canonical_detail::computeSectionCompositeEnergy(sv.tracks);
        sv.swing_ratio = (swing_weight_sum > 0.0) ? (swing_weighted / swing_weight_sum) : 0.50;
        sv.inter_track = canonical_detail::computeInterTrackRelationships(sv.tracks, sec_track_notes, sv.ppqn);

        view.sections.push_back(std::move(sv));
        running_tick_offset = canonical_detail::saturatingAddU32(running_tick_offset, view.sections.back().length_ticks);
        total_style_bars = canonical_detail::saturatingAddU32(total_style_bars, view.sections.back().bars);
    }

    if (view.transitions.total_cv_elements == 0) {
        view.transitions.total_cv_elements = static_cast<uint32_t>(view.sections.size());
    }

    // Recompute exact style-wide features over merged active-section notes for inter_track_overall (P1-1)
    {
        std::vector<CanonicalTrackView> ov_tracks;
        std::vector<std::vector<exporters::korg::quality::NoteSpan>> ov_notes;
        ov_tracks.reserve(overall_tracks_map.size());
        ov_notes.reserve(overall_tracks_map.size());
        const uint32_t eff_bars = std::max<uint32_t>(1u, total_style_bars);
        for (auto& [ch, tr] : overall_tracks_map) {
            const auto& merged = overall_notes_map[ch];
            if (!merged.empty()) {
                canonical_detail::ControllerTrace empty_ctrl{};
                tr.features = canonical_detail::computeTrackFeatures(
                    merged, empty_ctrl, view.ppqn, 4u, 4u, eff_bars,
                    tr.casm.source_chord_root, tr.casm.source_chord_type);
            }
            ov_tracks.push_back(tr);
            ov_notes.push_back(merged);
        }
        view.inter_track_overall = canonical_detail::computeInterTrackRelationships(ov_tracks, ov_notes, view.ppqn);
    }

    return view;
}

// Builds a zero-copy-payload CanonicalStyleView directly over an exported/parsed KorgStyle SMF0.
inline CanonicalStyleView buildCanonicalStyleViewFromKorg(
    const exporters::korg::Style& korg_style,
    std::string_view style_name = "",
    const exporters::korg::Report* korg_report = nullptr,
    TargetPlatformId target_platform = TargetPlatformId::KorgPa5X) {
    CanonicalStyleView view{};
    view.style_name = std::string(style_name);
    view.ppqn = korg_style.ppqn ? korg_style.ppqn : 384u;
    view.tempo_bpm = (korg_style.mpqn > 0)
                         ? static_cast<uint32_t>(std::max<uint64_t>(1u, (60'000'000ULL + korg_style.mpqn / 2u) / korg_style.mpqn))
                         : 120u;
    view.source_uasf = nullptr;
    view.target_korg = &korg_style;
    view.target_report = korg_report;
    view.target_platform = target_platform;
    view.target_capability = &targetCapabilityProfile(target_platform);
    view.hardware_verified = false;
    view.l3_status = "NOT_TESTED";

    view.provenance.source_format = "KorgSMF0";
    view.provenance.source_name = std::string(style_name);
    view.provenance.hardware_verified = false;
    view.provenance.l3_status = "NOT_TESTED";
    view.transitions.total_cv_elements = static_cast<uint32_t>(korg_style.elements.size());

    std::map<uint8_t, CanonicalTrackView> overall_tracks_map;
    std::map<uint8_t, std::vector<exporters::korg::quality::NoteSpan>> overall_notes_map;
    uint32_t running_tick_offset = 0;
    uint32_t total_style_bars = 0;

    for (size_t eidx = 0; eidx < korg_style.elements.size(); ++eidx) {
        const auto& el = korg_style.elements[eidx];
        CanonicalSectionView sv{};
        sv.uasf_section_index = eidx;
        sv.type = canonical_detail::sectionTypeFromKorgMarker(el.marker);
        sv.name = el.marker;
        sv.korg_marker = el.marker;
        sv.ppqn = std::clamp<uint32_t>(view.ppqn, 1u, 19200u);
        sv.meter_num = el.num ? el.num : 4u;
        sv.meter_den = el.den ? el.den : 4u;
        const uint64_t beat_ticks64 = std::max<uint64_t>(1ULL, (uint64_t{sv.ppqn} * 4ULL) / std::max<uint64_t>(1ULL, sv.meter_den));
        const uint64_t bar_ticks64  = std::max<uint64_t>(1ULL, beat_ticks64 * std::max<uint64_t>(1ULL, sv.meter_num));
        sv.bars = static_cast<uint32_t>(std::max<uint64_t>(1ULL, uint64_t{el.length} / bar_ticks64));
        sv.length_ticks = el.length ? el.length : static_cast<uint32_t>(std::min<uint64_t>(bar_ticks64, uint64_t{UINT32_MAX}));
        sv.loop_start_tick = 0;
        sv.loop_end_tick = sv.length_ticks;
        sv.cv_count = 1;
        sv.cv_markers.push_back(el.marker);

        switch (sv.type) {
            case uasf::SectionType::Intro1:
            case uasf::SectionType::Intro2:
            case uasf::SectionType::Intro3:  ++view.transitions.intro_count; break;
            case uasf::SectionType::Main1:
            case uasf::SectionType::Main2:
            case uasf::SectionType::Main3:
            case uasf::SectionType::Main4:   ++view.transitions.variation_count; break;
            case uasf::SectionType::Fill1:
            case uasf::SectionType::Fill2:
            case uasf::SectionType::Fill3:
            case uasf::SectionType::Fill4:   ++view.transitions.fill_count; break;
            case uasf::SectionType::Break:   ++view.transitions.break_count; break;
            case uasf::SectionType::Ending1:
            case uasf::SectionType::Ending2:
            case uasf::SectionType::Ending3: ++view.transitions.ending_count; break;
        }

        // Discover active channels present in this element
        std::set<uint8_t> channels;
        for (const auto& ev : el.events) {
            if ((ev.status & 0xF0u) >= 0x80u && (ev.status & 0xF0u) <= 0xE0u) {
                channels.insert(static_cast<uint8_t>(ev.status & 0x0Fu));
            }
        }

        std::vector<std::vector<exporters::korg::quality::NoteSpan>> sec_notes;
        double swing_w_sum = 0.0, swing_val = 0.0;
        for (uint8_t ch : channels) {
            CanonicalTrackView tv{};
            tv.source_channel = ch;
            tv.target_channel = ch;
            tv.name = exporters::korg::korgTrackName(ch);
            tv.is_drum = (ch == exporters::korg::kDrumCh || ch == exporters::korg::kPercCh);
            if (ch == exporters::korg::kDrumCh) tv.uasf_role = uasf::TrackRole::Drum;
            else if (ch == exporters::korg::kPercCh) tv.uasf_role = uasf::TrackRole::Percussion;
            else if (ch == exporters::korg::kBassCh) tv.uasf_role = uasf::TrackRole::Bass;
            else tv.uasf_role = uasf::TrackRole::Accompaniment;

            const auto ctrl = canonical_detail::extractControllersFromKorg(el.events, ch);
            const auto spans = canonical_detail::extractNoteSpansFromKorg(el.events, ch, sv.length_ticks);

            tv.sound_identity.msb = ctrl.msb;
            tv.sound_identity.lsb = ctrl.lsb;
            tv.sound_identity.pc  = ctrl.pc;
            tv.sound_identity.is_drum_bank = isKnownArrangerDrumBank(ctrl.msb, ctrl.lsb);
            tv.sound_identity.is_drum_kit = tv.is_drum || tv.sound_identity.is_drum_bank;
            tv.features = canonical_detail::computeTrackFeatures(
                spans, ctrl, sv.ppqn, sv.meter_num, sv.meter_den, sv.bars, 0u, 0u);
            tv.harmonic.chord_tone_ratio = tv.features.chord_tone_ratio;
            tv.harmonic.root_ratio = tv.features.root_ratio;
            tv.harmonic.third_ratio = tv.features.third_ratio;
            tv.harmonic.fifth_ratio = tv.features.fifth_ratio;
            tv.harmonic.seventh_ratio = tv.features.seventh_ratio;
            tv.harmonic.non_chord_tone_ratio = tv.features.non_chord_tone_ratio;

            tv.provenance.origin = DecisionOrigin::Deterministic;
            tv.provenance.component_id = "p5_canonical_korg_view_v1";
            tv.provenance.confidence = 1.0;
            tv.provenance.abstained = false;
            tv.provenance.reversible = true;
            tv.provenance.rationale = "Zero-copy semantic overlay extracted from KorgStyle SMF0.";

            if (tv.features.swing_offbeat_count >= 2u) {
                const double w = (tv.is_drum ? 2.5 : 1.0) * std::min<double>(8.0, static_cast<double>(tv.features.swing_offbeat_count));
                swing_val += tv.features.swing_ratio_estimate * w;
                swing_w_sum += w;
            }

            if (overall_tracks_map.find(ch) == overall_tracks_map.end()) {
                overall_tracks_map[ch] = tv;
            } else {
                auto& ot = overall_tracks_map[ch];
                if (ot.features.note_count == 0 && tv.features.note_count > 0) {
                    ot = tv;
                }
            }
            for (auto ns : spans) {
                ns.on_tick  = canonical_detail::saturatingAddU32(ns.on_tick, running_tick_offset);
                ns.off_tick = canonical_detail::saturatingAddU32(ns.off_tick, running_tick_offset);
                overall_notes_map[ch].push_back(ns);
            }

            sv.tracks.push_back(std::move(tv));
            sec_notes.push_back(spans);
        }

        sv.composite_energy = canonical_detail::computeSectionCompositeEnergy(sv.tracks);
        sv.swing_ratio = (swing_w_sum > 0.0) ? (swing_val / swing_w_sum) : 0.50;
        sv.inter_track = canonical_detail::computeInterTrackRelationships(sv.tracks, sec_notes, sv.ppqn);
        view.sections.push_back(std::move(sv));
        running_tick_offset = canonical_detail::saturatingAddU32(running_tick_offset, view.sections.back().length_ticks);
        total_style_bars = canonical_detail::saturatingAddU32(total_style_bars, view.sections.back().bars);
    }

    {
        std::vector<CanonicalTrackView> ov_tracks;
        std::vector<std::vector<exporters::korg::quality::NoteSpan>> ov_notes;
        const uint32_t eff_bars = std::max<uint32_t>(1u, total_style_bars);
        for (auto& [ch, tr] : overall_tracks_map) {
            const auto& merged = overall_notes_map[ch];
            if (!merged.empty()) {
                canonical_detail::ControllerTrace empty_ctrl{};
                tr.features = canonical_detail::computeTrackFeatures(
                    merged, empty_ctrl, view.ppqn, 4u, 4u, eff_bars, 0u, 0u);
            }
            ov_tracks.push_back(tr);
            ov_notes.push_back(merged);
        }
        view.inter_track_overall = canonical_detail::computeInterTrackRelationships(ov_tracks, ov_notes, view.ppqn);
    }

    return view;
}

inline std::string formatCanonicalStyleJson(const CanonicalStyleView& view) {
    using target_cap_detail::jsonEscape;
    using canonical_detail::fmtDouble;
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": \"" << jsonEscape(view.provenance.schema_version) << "\",\n";
    os << "  \"styleName\": \"" << jsonEscape(view.style_name) << "\",\n";
    os << "  \"tempoBpm\": " << view.tempo_bpm << ",\n";
    os << "  \"ppqn\": " << view.ppqn << ",\n";
    os << "  \"targetPlatform\": \"" << jsonEscape(targetPlatformKey(view.target_platform)) << "\",\n";
    os << "  \"hardwareVerified\": " << (view.hardware_verified ? "true" : "false") << ",\n";
    os << "  \"l3Status\": \"" << jsonEscape(view.l3_status) << "\",\n";
    os << "  \"provenance\": {\n";
    os << "    \"sourceFormat\": \"" << jsonEscape(view.provenance.source_format) << "\",\n";
    os << "    \"converterSha\": \"" << jsonEscape(view.provenance.converter_sha) << "\",\n";
    os << "    \"zeroCopyUasfBound\": " << (view.source_uasf != nullptr ? "true" : "false") << ",\n";
    os << "    \"zeroCopyKorgBound\": " << (view.target_korg != nullptr ? "true" : "false") << "\n";
    os << "  },\n";
    os << "  \"transitions\": {\n";
    os << "    \"intros\": " << view.transitions.intro_count << ",\n";
    os << "    \"variations\": " << view.transitions.variation_count << ",\n";
    os << "    \"fills\": " << view.transitions.fill_count << ",\n";
    os << "    \"breaks\": " << view.transitions.break_count << ",\n";
    os << "    \"endings\": " << view.transitions.ending_count << ",\n";
    os << "    \"totalCvElements\": " << view.transitions.total_cv_elements << "\n";
    os << "  },\n";
    os << "  \"interTrackOverall\": {\n";
    os << "    \"kickBassLockCorrelation\": " << fmtDouble(view.inter_track_overall.kick_bass_lock_correlation) << ",\n";
    os << "    \"bassPianoLowCollision\": " << (view.inter_track_overall.bass_piano_low_collision ? "true" : "false") << ",\n";
    os << "    \"bassPianoLowCollisionNotes\": " << view.inter_track_overall.bass_piano_low_collision_notes << ",\n";
    os << "    \"registerOverlapCount\": " << view.inter_track_overall.register_overlaps.size() << "\n";
    os << "  },\n";
    os << "  \"sections\": [";
    for (size_t i = 0; i < view.sections.size(); ++i) {
        const auto& s = view.sections[i];
        if (i > 0) os << ",";
        os << "\n    {\n";
        os << "      \"index\": " << s.uasf_section_index << ",\n";
        os << "      \"name\": \"" << jsonEscape(s.name) << "\",\n";
        os << "      \"marker\": \"" << jsonEscape(s.korg_marker) << "\",\n";
        os << "      \"bars\": " << s.bars << ",\n";
        os << "      \"meter\": \"" << static_cast<int>(s.meter_num) << "/" << static_cast<int>(s.meter_den) << "\",\n";
        os << "      \"lengthTicks\": " << s.length_ticks << ",\n";
        os << "      \"cvCount\": " << static_cast<int>(s.cv_count) << ",\n";
        os << "      \"compositeEnergy\": " << fmtDouble(s.composite_energy, 2) << ",\n";
        os << "      \"swingRatio\": " << fmtDouble(s.swing_ratio, 4) << ",\n";
        os << "      \"tracks\": [";
        for (size_t j = 0; j < s.tracks.size(); ++j) {
            const auto& t = s.tracks[j];
            if (j > 0) os << ",";
            os << "\n        {"
               << "\"srcCh\": " << static_cast<int>(t.source_channel)
               << ", \"dstCh\": " << static_cast<int>(t.target_channel)
               << ", \"name\": \"" << jsonEscape(t.name) << "\""
               << ", \"role\": \"" << musicalRoleName(t.semantic_role) << "\""
               << ", \"secondaryRole\": \"" << musicalRoleName(t.secondary_role) << "\""
               << ", \"isMixedRole\": " << (t.is_mixed_role ? "true" : "false")
               << ", \"confidence\": " << fmtDouble(t.role_confidence, 4)
               << ", \"entropy\": " << fmtDouble(t.uncertainty_entropy, 4)
               << ", \"abstained\": " << (t.role_abstained ? "true" : "false")
               << ", \"register\": \"" << registerBandName(t.features.register_band) << "\""
               << ", \"notes\": " << t.features.note_count
               << ", \"notesPerBar\": " << fmtDouble(t.features.notes_per_bar, 2)
               << ", \"chordToneRatio\": " << fmtDouble(t.features.chord_tone_ratio, 4)
               << ", \"syncopation\": " << fmtDouble(t.features.syncopation_ratio, 4)
               << ", \"maxPoly\": " << static_cast<int>(t.features.max_polyphony)
               << ", \"motif\": \"" << jsonEscape(t.features.motif_signature) << "\""
               << "}";
        }
        if (!s.tracks.empty()) os << "\n      ";
        os << "]\n    }";
    }
    if (!view.sections.empty()) os << "\n  ";
    os << "]\n}\n";
    return os.str();
}

inline std::string formatCanonicalStyleSummaryMd(const CanonicalStyleView& view) {
    using canonical_detail::fmtDouble;
    std::ostringstream os;
    os << "# Canonical Arrangement Summary — " << view.style_name << "\n\n";
    os << "- **Target Platform:** `" << targetPlatformKey(view.target_platform) << "`"
       << " | **Tempo:** " << view.tempo_bpm << " BPM | **PPQN:** " << view.ppqn << "\n";
    os << "- **Transitions:** " << view.transitions.intro_count << " Intros, "
       << view.transitions.variation_count << " Variations, "
       << view.transitions.fill_count << " Fills, "
       << view.transitions.break_count << " Breaks, "
       << view.transitions.ending_count << " Endings (" << view.transitions.total_cv_elements << " CVs)\n";
    os << "- **Kick-Bass Lock:** " << fmtDouble(view.inter_track_overall.kick_bass_lock_correlation, 3)
       << " | **Bass/Piano Low Collisions:** " << view.inter_track_overall.bass_piano_low_collision_notes << "\n";
    os << "- **Epistemic Lock:** `hardwareVerified: false` | `l3Status: " << view.l3_status << "`\n\n";
    os << "| Section | Marker | Bars | Meter | Energy | Swing | Active Tracks |\n";
    os << "|---|---|---:|---|---:|---:|---:|\n";
    for (const auto& s : view.sections) {
        uint32_t act = 0;
        for (const auto& t : s.tracks) if (t.features.note_count > 0) ++act;
        os << "| " << s.name << " | `" << s.korg_marker << "` | " << s.bars
           << " | " << static_cast<int>(s.meter_num) << "/" << static_cast<int>(s.meter_den)
           << " | " << fmtDouble(s.composite_energy, 1)
           << " | " << fmtDouble(s.swing_ratio, 3)
           << " | " << act << " |\n";
    }
    return os.str();
}

} // namespace ai_arranger::intelligence

#endif // AI_ARRANGER_INTELLIGENCE_CANONICAL_ARRANGEMENT_H
