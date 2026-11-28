#ifndef AI_ARRANGER_INTELLIGENCE_MUSICAL_SEMANTIC_ANALYZER_H
#define AI_ARRANGER_INTELLIGENCE_MUSICAL_SEMANTIC_ANALYZER_H

#include "intelligence/canonical_arrangement.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Phase 5 Milestone P5-M2: Offline Statistical & Feature-Based Musical Semantic Analyzer.
//
// Boundaries & Invariants (ADR-P5-003 / ADR-P5-004 / ADR-P5-005):
//   * Pure C++20 offline statistical & music-theory inference (0 external ML deps).
//   * Infers all 14 canonical musical track roles + MixedRole + explicit UnknownAbstain
//     primarily from musical content (13 feature groups), NEVER blindly trusting MIDI
//     channel numbers, program changes, or track names.
//   * Calibrated softmax confidence + normalized Shannon entropy + explicit abstain
//     reasons — NEVER fabricates certainty on empty, sparse, ambiguous, or noise tracks.
//   * 100% deterministic, ODR-safe header-only, Zone B offline only.

namespace ai_arranger::intelligence {

enum class GenreFeelClass : uint8_t {
    UnknownAbstain = 0,
    PopBallad,
    RockDrive,
    FunkSyncopated,
    EdmElectronic,
    JazzSwing,
    LatinBossaCha,
    AcousticFolkWaltz,
    OrchestralCinematic,
    VietnameseBoleroSlowRock,
};

inline constexpr const char* genreFeelClassName(GenreFeelClass g) noexcept {
    switch (g) {
        case GenreFeelClass::UnknownAbstain:           return "UnknownAbstain";
        case GenreFeelClass::PopBallad:                return "PopBallad";
        case GenreFeelClass::RockDrive:                return "RockDrive";
        case GenreFeelClass::FunkSyncopated:           return "FunkSyncopated";
        case GenreFeelClass::EdmElectronic:            return "EdmElectronic";
        case GenreFeelClass::JazzSwing:                return "JazzSwing";
        case GenreFeelClass::LatinBossaCha:            return "LatinBossaCha";
        case GenreFeelClass::AcousticFolkWaltz:        return "AcousticFolkWaltz";
        case GenreFeelClass::OrchestralCinematic:      return "OrchestralCinematic";
        case GenreFeelClass::VietnameseBoleroSlowRock: return "VietnameseBoleroSlowRock";
    }
    return "UnknownAbstain";
}

struct SemanticAnalyzeOptions {
    double           role_abstain_threshold  = 0.55; // Fail-closed: role -> UnknownAbstain below floor
    double           min_confidence_margin   = 0.08; // Fail-closed: top1 - top2 margin floor
    double           max_entropy_threshold   = 0.78; // Fail-closed: normalized Shannon entropy cap
    double           genre_abstain_threshold = 0.50; // Fail-closed: genre -> UnknownAbstain below floor
    bool             use_metadata_hints      = true; // Weak bounded melodic prior (<=0.55 logit) + profile drum-bank consistency check (abstains on conflict); set false for strict content-only ablation
    bool             emit_explainability_trace = true;
    TargetPlatformId target_platform         = TargetPlatformId::KorgPa5X;
};

struct SectionRoleObservation {
    uasf::SectionType section_type = uasf::SectionType::Main1;
    std::string       section_name;
    MusicalRole       role = MusicalRole::UnknownAbstain;
    MusicalRole       secondary_role = MusicalRole::UnknownAbstain;
    double            confidence = 0.0;
    double            uncertainty_entropy = 1.0;
    double            notes_per_bar = 0.0;
    double            mean_velocity = 0.0;
    bool              abstained = true;
    std::string       abstain_reason;
};

struct TrackRoleDecision {
    uint8_t                             source_channel = 0;
    uint8_t                             target_channel = 0xFF;
    std::string                         track_name;
    MusicalRole                         role = MusicalRole::UnknownAbstain; // UnknownAbstain if abstained, else primary_role
    MusicalRole                         primary_role = MusicalRole::UnknownAbstain;
    MusicalRole                         secondary_role = MusicalRole::UnknownAbstain;
    MusicalRole                         runner_up_role = MusicalRole::UnknownAbstain;
    bool                                is_mixed_role = false;
    std::string                         mixed_role_reason;
    double                              confidence = 0.0;
    double                              margin = 0.0;                // top1 - top2 probability margin
    double                              uncertainty_entropy = 1.0;   // normalized Shannon entropy [0..1]
    bool                                abstained = true;
    std::string                         abstain_reason;
    std::vector<std::string>            supporting_evidence;
    std::array<double, kCanonicalSemanticRoleCount> role_probabilities{}; // calibrated posterior over roles 1..14
    // Group 13: Section-dependent behavior
    std::vector<SectionRoleObservation> per_section_roles;
    double                              role_stability_ratio = 0.0;
    bool                                section_role_shift_detected = false;
    ProvenanceRecord                    provenance{};

    MusicalRole effectiveRoleOrMixed() const noexcept {
        if (abstained) return MusicalRole::UnknownAbstain;
        return is_mixed_role ? MusicalRole::MixedRole : role;
    }
};

struct GroovePocketProfile {
    double estimated_swing_ratio   = 0.50; // 0.50..0.72
    double kick_bass_lock_ratio    = 0.0;  // [0..1]
    double syncopation_index       = 0.0;  // [0..1]
    bool   compound_meter_detected = false;
    bool   mixed_meter_detected    = false;
};

struct SectionEnergyArcPoint {
    uasf::SectionType section_type = uasf::SectionType::Main1;
    std::string       section_name;
    double            note_density_per_bar = 0.0;
    double            mean_velocity = 0.0;
    uint32_t          active_track_count = 0;
    double            energy_score = 0.0; // [0..100]
};

struct SemanticArrangementAnalysis {
    std::string                        schema = "aiarranger-semantic-analysis/1";
    std::string                        style_name;
    CanonicalStyleView                 canonical_view;
    std::vector<TrackRoleDecision>     track_roles; // style-wide consolidated per channel
    GenreFeelClass                     inferred_genre = GenreFeelClass::UnknownAbstain;
    double                             genre_confidence = 0.0;
    bool                               genre_abstained = true;
    GroovePocketProfile                groove{};
    InterTrackRelationships            inter_track{};
    std::vector<SectionEnergyArcPoint> energy_arc;
    bool                               variation_arc_monotonic = true;
    uint32_t                           abstained_track_count = 0;
    uint32_t                           mixed_role_track_count = 0;
    std::vector<std::string>           explainability_notes;
    bool                               hardware_verified = false;
    std::string                        l3_status = "NOT_TESTED";
};

namespace semantic_detail {

inline constexpr size_t roleToIdx(MusicalRole r) noexcept {
    const uint8_t v = static_cast<uint8_t>(r);
    if (v >= 1u && v <= kCanonicalSemanticRoleCount) return static_cast<size_t>(v - 1u);
    return 0u;
}

inline constexpr MusicalRole idxToRole(size_t idx) noexcept {
    if (idx < kCanonicalSemanticRoleCount) {
        return static_cast<MusicalRole>(idx + 1u);
    }
    return MusicalRole::UnknownAbstain;
}

struct SingleClassification {
    MusicalRole role = MusicalRole::UnknownAbstain;
    MusicalRole primary_role = MusicalRole::UnknownAbstain;
    MusicalRole secondary_role = MusicalRole::UnknownAbstain;
    bool        is_mixed_role = false;
    std::string mixed_role_reason;
    double      confidence = 0.0;
    double      margin = 0.0;
    double      entropy = 1.0;
    bool        abstained = true;
    std::string abstain_reason;
    std::vector<std::string> evidence;
    std::array<double, kCanonicalSemanticRoleCount> probs{};
};

inline SingleClassification classifyTrackFeatures(
    const CanonicalTrackView& tr,
    const SemanticAnalyzeOptions& opt,
    bool is_fill_or_transition_section = false) {
    using MR = MusicalRole;
    SingleClassification out{};
    const auto& f = tr.features;

    // ── Fail-closed Guard 1: Empty track ──
    if (f.note_count == 0) {
        out.abstained = true;
        out.abstain_reason = "empty_track_zero_notes";
        out.evidence.push_back("note_count=0");
        return out;
    }

    // P1-4: Vendor-neutral drum metadata lookup via TargetCapabilityProfile (zero hardcoded bank magic numbers).
    // If bank metadata claims drum while musical content is strongly sustained & tonal melodic, fail closed!
    const bool drum_bank_hint = opt.use_metadata_hints && tr.sound_identity.is_drum_bank;
    const bool explicit_drum_flag = opt.use_metadata_hints && tr.is_drum;
    const bool strongly_melodic_content =
        (f.note_count >= 4u &&
         ((f.mean_duration_beats >= 0.55 && f.chord_tone_ratio >= 0.72 &&
           f.drum_hat_cymbal_ratio < 0.15 && f.dissonant_simultaneous_ratio < 0.10) ||
          (f.max_polyphony <= 1u && f.mean_duration_beats >= 0.50 && f.chord_tone_ratio >= 0.80 &&
           f.drum_hat_cymbal_ratio < 0.05 && f.drum_aux_perc_ratio < 0.15)));

    if ((drum_bank_hint || explicit_drum_flag) && !tr.is_drum && strongly_melodic_content) {
        out.abstained = true;
        out.role = MR::UnknownAbstain;
        out.primary_role = MR::UnknownAbstain;
        out.secondary_role = MR::UnknownAbstain;
        out.confidence = 0.0;
        out.entropy = 1.0;
        out.abstain_reason = "metadata_content_conflict";
        out.evidence.push_back("drum_bank_metadata_contradicts_sustained_tonal_content");
        return out;
    }

    const bool explicit_drum_domain = explicit_drum_flag || (drum_bank_hint && !strongly_melodic_content);

    const bool is_riser_sweep =
        (f.note_count >= 5u && f.monotonic_ramp_ratio >= 0.70 && f.pitch_span >= 8u &&
         (f.velocity_dynamic_range >= 18u || f.has_expression_cc11 || f.has_pitch_bend));

    const bool is_sparse_boundary_fx =
        ((f.active_bar_ratio <= 0.55 || f.notes_per_bar <= 2.2) &&
         f.boundary_note_ratio >= 0.60 &&
         (f.drum_fx_cymbal_ratio >= 0.45 || f.has_pitch_bend || f.has_expression_cc11 ||
          (f.unique_pitch_count <= 2u && f.mean_duration_beats >= 1.4)));

    // ── Fail-closed Guard 2: Insufficient note evidence ──
    if (f.note_count <= 2u && !is_sparse_boundary_fx) {
        out.abstained = true;
        out.role = MR::UnknownAbstain;
        out.primary_role = MR::UnknownAbstain;
        out.entropy = 1.0;
        out.abstain_reason = "insufficient_note_evidence";
        out.evidence.push_back("note_count=" + std::to_string(f.note_count) + " (<3 without boundary/FX cue)");
        return out;
    }

    // ── Fail-closed Guard 2b (P2-4): Degenerate single-onset cluster (e.g., 6 simultaneous 0-dur notes at tick 0) ──
    if (!explicit_drum_domain && f.unique_onsets_per_bar <= 1.0 && f.active_bars <= 1u &&
        f.mean_duration_beats < 1.5 && !is_sparse_boundary_fx) {
        out.abstained = true;
        out.role = MR::UnknownAbstain;
        out.primary_role = MR::UnknownAbstain;
        out.entropy = 1.0;
        out.abstain_reason = "single_onset_insufficient_temporal_context";
        out.evidence.push_back("unique_onsets_per_bar=" + canonical_detail::fmtDouble(f.unique_onsets_per_bar, 2));
        return out;
    }

    // ── Fail-closed Guard 3: Ambiguous single-pitch mid-register generic fragment ──
    if (!explicit_drum_domain && f.note_count <= 4u && f.unique_pitch_count == 1u &&
        !is_sparse_boundary_fx && f.pitch_median >= 50u && f.pitch_median <= 78u &&
        f.mean_duration_beats < 1.8) {
        out.abstained = true;
        out.role = MR::UnknownAbstain;
        out.primary_role = MR::UnknownAbstain;
        out.entropy = 1.0;
        out.abstain_reason = "ambiguous_single_pitch_fragment";
        out.evidence.push_back("unique_pitch_count=1, note_count=" + std::to_string(f.note_count));
        return out;
    }

    // Pure content-based percussive domain detection (works with zero channel/program/name hints!)
    // Real drum kits play simultaneous multi-piece hits (max_polyphony >= 2: kick+hat / snare+hat)
    // with non-tonal drum pitch clusters, cleanly separating them from monophonic funk slap bass!
    const bool content_kick_snare_drum =
        (f.max_polyphony >= 2u &&
         f.mean_duration_beats <= 0.48 &&
         f.unique_pitch_count <= 14u &&
         (f.drum_kick_snare_ratio + f.drum_hat_cymbal_ratio >= 0.58) &&
         ((f.drum_kick_snare_ratio >= 0.15 && f.drum_hat_cymbal_ratio >= 0.18) ||
          (f.drum_hat_cymbal_ratio >= 0.50 && f.non_chord_tone_ratio >= 0.55) ||
          (f.drum_kick_snare_ratio >= 0.25 && f.staccato_ratio >= 0.70 &&
           (f.dissonant_simultaneous_ratio >= 0.18 || (f.downbeat_ratio >= 0.12 && f.backbeat_ratio >= 0.12)))) &&
         (f.non_chord_tone_ratio >= 0.35 || f.dissonant_simultaneous_ratio >= 0.18 || f.staccato_ratio >= 0.72));

    // P2-4: Gate on unique_onsets_per_bar >= 2.5 (not simultaneous notes_per_bar!) so a single chord cluster never matches aux perc.
    const bool content_aux_perc =
        (f.mean_duration_beats <= 0.42 &&
         f.staccato_ratio >= 0.55 &&
         f.unique_pitch_count <= 8u &&
         f.drum_aux_perc_ratio >= 0.70 &&
         f.drum_kick_snare_ratio < 0.05 &&
         f.chord_tone_ratio < 0.85 &&
         f.unique_onsets_per_bar >= 2.5 &&
         (f.max_pitch_dominance >= 0.28 || f.dissonant_simultaneous_ratio >= 0.15 || f.unique_pitch_count <= 4u));

    const bool percussive_domain = explicit_drum_domain || content_kick_snare_drum || content_aux_perc;

    // Detect unstructured random / noise track (high pitch spread, low chord-tone, low motif repetition, non-percussive)
    const bool looks_random_noise =
        (!percussive_domain && !is_riser_sweep && !f.has_strum_offsets &&
         f.note_count >= 6u &&
         f.chord_tone_ratio < 0.45 &&
         f.motif_repetition_ratio < 0.22 &&
         f.pitch_stddev >= 14.0 &&
         f.interval_large_leap_ratio >= 0.30);

    std::array<double, kCanonicalSemanticRoleCount> logits{};
    logits.fill(0.0);

    auto add = [&](MR r, double delta) {
        logits[roleToIdx(r)] += delta;
    };

    // ── 1. KickSnareFoundation ──
    {
        if (percussive_domain) {
            if (f.drum_kick_snare_ratio >= 0.18) {
                add(MR::KickSnareFoundation, 3.6 + 2.4 * std::min(1.0, f.drum_kick_snare_ratio / 0.50));
            } else if (explicit_drum_domain && f.drum_hat_cymbal_ratio >= 0.40 && f.drum_aux_perc_ratio < 0.30) {
                add(MR::KickSnareFoundation, 2.8);
            } else {
                add(MR::KickSnareFoundation, -3.2);
            }
            if (f.drum_hat_cymbal_ratio >= 0.20) add(MR::KickSnareFoundation, 1.8);
            if (f.downbeat_ratio >= 0.12 && f.backbeat_ratio >= 0.10) add(MR::KickSnareFoundation, 1.4);
            if (f.staccato_ratio >= 0.65) add(MR::KickSnareFoundation, 0.8);
            if (f.drum_kick_snare_ratio < 0.08 && f.drum_aux_perc_ratio >= 0.50) add(MR::KickSnareFoundation, -4.5);
            if (is_sparse_boundary_fx && f.drum_fx_cymbal_ratio >= 0.50 && f.drum_kick_snare_ratio < 0.15) {
                add(MR::KickSnareFoundation, -4.0);
            }
        } else {
            add(MR::KickSnareFoundation, -6.0);
        }
    }

    // ── 2. DrumKitAuxPercussion ──
    {
        if (percussive_domain) {
            if (f.drum_aux_perc_ratio >= 0.50) {
                add(MR::DrumKitAuxPercussion, 4.4 + 2.2 * f.drum_aux_perc_ratio);
            } else if (explicit_drum_domain && f.drum_kick_snare_ratio < 0.08 && f.drum_fx_cymbal_ratio < 0.50) {
                add(MR::DrumKitAuxPercussion, 3.4);
            } else {
                add(MR::DrumKitAuxPercussion, -3.0);
            }
            if (f.drum_kick_snare_ratio < 0.08) add(MR::DrumKitAuxPercussion, 1.6);
            if (f.drum_kick_snare_ratio >= 0.18) add(MR::DrumKitAuxPercussion, -4.2);
            if (f.notes_per_bar >= 2.5) add(MR::DrumKitAuxPercussion, 1.0);
            if (is_sparse_boundary_fx && f.drum_fx_cymbal_ratio >= 0.50) add(MR::DrumKitAuxPercussion, -4.2);
        } else {
            add(MR::DrumKitAuxPercussion, -6.0);
        }
    }

    // Strict domain separation when explicitly flagged drum kit (0% drum<->melodic contamination)
    if (explicit_drum_domain) {
        static constexpr std::array<MR, 11> kMelodicRoles = {
            MR::BassFoundation, MR::HarmonicAccompaniment, MR::ChordComping,
            MR::GuitarStrumming, MR::Arpeggio, MR::PianoAccompaniment,
            MR::PadSustainedHarmony, MR::Countermelody, MR::BrassAccents,
            MR::OrchestralAccents, MR::MelodicPhrase
        };
        for (MR mr : kMelodicRoles) add(mr, -12.0);
    } else if (content_kick_snare_drum || content_aux_perc) {
        static constexpr std::array<MR, 11> kMelodicRoles = {
            MR::BassFoundation, MR::HarmonicAccompaniment, MR::ChordComping,
            MR::GuitarStrumming, MR::Arpeggio, MR::PianoAccompaniment,
            MR::PadSustainedHarmony, MR::Countermelody, MR::BrassAccents,
            MR::OrchestralAccents, MR::MelodicPhrase
        };
        for (MR mr : kMelodicRoles) add(mr, -6.5);
    }

    // ── 3. BassFoundation ──
    {
        if (f.pitch_median <= 50u) {
            add(MR::BassFoundation, 4.2 + (f.pitch_median <= 45u ? 1.0 : 0.0));
        } else if (f.pitch_median <= 53u && f.low_register_ratio >= 0.55) {
            add(MR::BassFoundation, 1.5);
        } else {
            add(MR::BassFoundation, -5.5);
        }
        if (f.mean_polyphony <= 1.25 && f.chordal_ratio <= 0.18) {
            add(MR::BassFoundation, 2.4);
        } else if (f.chordal_ratio >= 0.35) {
            add(MR::BassFoundation, -4.8);
        }
        if (f.low_register_ratio >= 0.70) add(MR::BassFoundation, 1.5);
        if (f.root_ratio + f.fifth_ratio >= 0.40 || f.chord_tone_ratio >= 0.65) add(MR::BassFoundation, 1.2);
        if (f.notes_per_bar >= 1.8) add(MR::BassFoundation, 0.7);
        if (f.downbeat_ratio >= 0.15) add(MR::BassFoundation, 0.5);
    }

    // ── 4. HarmonicAccompaniment (steady, smooth, compact mid-register chord backing) ──
    {
        if (f.max_polyphony >= 2u && f.chordal_ratio >= 0.45) {
            add(MR::HarmonicAccompaniment, 2.6);
        } else {
            add(MR::HarmonicAccompaniment, -4.2);
        }
        if (f.has_strum_offsets) add(MR::HarmonicAccompaniment, -4.5);
        else add(MR::HarmonicAccompaniment, 0.8);

        if (f.mean_duration_beats >= 0.68 && f.mean_duration_beats <= 1.75) {
            add(MR::HarmonicAccompaniment, 2.8);
        } else if (f.mean_duration_beats < 0.55 || f.mean_duration_beats >= 2.05) {
            add(MR::HarmonicAccompaniment, -3.2);
        }
        if (f.syncopation_ratio < 0.26 && f.staccato_ratio < 0.30) add(MR::HarmonicAccompaniment, 1.9);
        else if (f.syncopation_ratio >= 0.35 || f.staccato_ratio >= 0.45) add(MR::HarmonicAccompaniment, -2.5);

        if (!f.has_sustain_pedal && !f.has_two_hand_split && f.pitch_span <= 15u) {
            add(MR::HarmonicAccompaniment, 2.0);
        } else if (f.has_sustain_pedal || f.has_two_hand_split || f.pitch_span >= 17u) {
            add(MR::HarmonicAccompaniment, -2.6);
        }
        if (f.vel_stddev <= 12.0 && f.mean_velocity < 96.0) add(MR::HarmonicAccompaniment, 0.8);
        if (f.pitch_median >= 50u && f.pitch_median <= 74u) add(MR::HarmonicAccompaniment, 0.7);
        if (f.has_expression_cc11 && f.cc11_range >= 20u) add(MR::HarmonicAccompaniment, -2.2);
    }

    // ── 5. ChordComping (rhythmic, punchy, syncopated/staccato chord comping) ──
    {
        if (f.max_polyphony >= 2u && f.chordal_ratio >= 0.40) {
            add(MR::ChordComping, 2.6);
        } else {
            add(MR::ChordComping, -4.2);
        }
        if (f.has_strum_offsets) add(MR::ChordComping, -4.5);
        else add(MR::ChordComping, 0.8);

        if (f.mean_duration_beats <= 0.65) add(MR::ChordComping, 2.6);
        else if (f.mean_duration_beats > 0.85) add(MR::ChordComping, -3.4);

        if (f.staccato_ratio >= 0.35) add(MR::ChordComping, 1.5);
        if (f.syncopation_ratio >= 0.25) add(MR::ChordComping, 2.3);
        else if (f.syncopation_ratio < 0.12 && f.staccato_ratio < 0.30) add(MR::ChordComping, -1.8);

        if (f.notes_per_bar >= 4.5 && f.duty_cycle >= 0.16) add(MR::ChordComping, 1.1);
        else if (f.duty_cycle <= 0.16 && f.inter_onset_gap_max_beats >= 2.0) add(MR::ChordComping, -2.4);
        if (!f.has_sustain_pedal && !f.has_two_hand_split && f.pitch_span <= 17u) add(MR::ChordComping, 1.2);
        else if (f.has_sustain_pedal || f.has_two_hand_split) add(MR::ChordComping, -2.2);

        if (f.mean_velocity < 92.0 || f.inter_onset_gap_max_beats < 1.50) add(MR::ChordComping, 1.0);
        else add(MR::ChordComping, -2.2);
    }

    // ── 6. GuitarStrumming (micro-arpeggio strum offsets <= 45 ticks) ──
    {
        if (f.has_strum_offsets) {
            add(MR::GuitarStrumming, 5.4 + 2.0 * f.strum_onset_ratio);
        } else {
            add(MR::GuitarStrumming, -4.8);
        }
        if (f.max_polyphony >= 3u && f.chordal_ratio >= 0.35) add(MR::GuitarStrumming, 2.0);
        if (f.pitch_min >= 40u && f.pitch_median >= 48u && f.pitch_median <= 70u && f.pitch_span <= 28u) {
            add(MR::GuitarStrumming, 1.2);
        }
        if (f.chord_tone_ratio >= 0.70) add(MR::GuitarStrumming, 0.8);
    }

    // ── 7. Arpeggio (sequential broken chords, chord-tone leaps, steady subdivision) ──
    {
        if (f.chordal_ratio <= 0.25 && f.unique_onsets_per_bar >= 3.5) {
            add(MR::Arpeggio, 2.8);
        } else {
            add(MR::Arpeggio, -4.0);
        }
        if (f.chord_tone_ratio >= 0.78) add(MR::Arpeggio, 2.4);
        else if (f.chord_tone_ratio < 0.60) add(MR::Arpeggio, -2.5);

        if (f.interval_chord_leap_ratio >= 0.45 && f.interval_stepwise_ratio <= 0.35) {
            add(MR::Arpeggio, 3.0);
        } else if (f.interval_stepwise_ratio >= 0.48) {
            add(MR::Arpeggio, -3.5);
        }
        if (f.motif_repetition_ratio >= 0.35) add(MR::Arpeggio, 1.2);
        if (f.pitch_median >= 48u && f.pitch_median <= 78u && f.pitch_span >= 7u) add(MR::Arpeggio, 1.0);
        else if (f.pitch_median <= 46u) add(MR::Arpeggio, -3.5);
        if (f.eighth_subdivision_ratio + f.sixteenth_subdivision_ratio + f.triplet_subdivision_ratio >= 0.60) {
            add(MR::Arpeggio, 0.8);
        }
    }

    // ── 8. PianoAccompaniment (two-hand split, wide chordal voicing, or sustain pedal) ──
    {
        if (f.max_polyphony >= 3u && f.chordal_ratio >= 0.45) {
            add(MR::PianoAccompaniment, 2.5);
        } else {
            add(MR::PianoAccompaniment, -4.2);
        }
        if (f.has_strum_offsets) add(MR::PianoAccompaniment, -4.5);
        else add(MR::PianoAccompaniment, 0.8);

        if (f.has_sustain_pedal) add(MR::PianoAccompaniment, 3.0);
        if (f.has_two_hand_split) add(MR::PianoAccompaniment, 2.8);
        if (f.pitch_span >= 17u) add(MR::PianoAccompaniment, 2.2);
        else if (f.pitch_span >= 15u) add(MR::PianoAccompaniment, 1.2);

        if (!f.has_sustain_pedal && !f.has_two_hand_split && f.pitch_span < 15u) {
            add(MR::PianoAccompaniment, -3.0);
        }
        if (f.mean_duration_beats >= 0.45 && f.mean_duration_beats <= 2.10) add(MR::PianoAccompaniment, 1.0);
        if (f.chord_tone_ratio >= 0.70) add(MR::PianoAccompaniment, 0.8);
        if (f.mean_duration_beats >= 2.60 && !f.has_sustain_pedal && f.unique_onsets_per_bar <= 1.5) {
            add(MR::PianoAccompaniment, -2.5);
        }
    }

    // ── 9. PadSustainedHarmony (long sustained harmonic bed) ──
    {
        if (f.mean_duration_beats >= 1.85) {
            add(MR::PadSustainedHarmony, 3.8 + (f.mean_duration_beats >= 2.50 ? 1.2 : 0.0));
        } else {
            add(MR::PadSustainedHarmony, -4.5);
        }
        if (f.legato_ratio >= 0.70 && f.duty_cycle >= 0.72) add(MR::PadSustainedHarmony, 2.4);
        if (f.unique_onsets_per_bar <= 2.5) add(MR::PadSustainedHarmony, 2.0);
        else if (f.unique_onsets_per_bar > 4.0) add(MR::PadSustainedHarmony, -3.5);

        if (f.max_polyphony >= 2u && f.chordal_ratio >= 0.45) add(MR::PadSustainedHarmony, 1.5);
        if (f.syncopation_ratio <= 0.25 && f.vel_stddev <= 14.0) add(MR::PadSustainedHarmony, 1.0);
        if (f.has_strum_offsets) add(MR::PadSustainedHarmony, -4.0);
        if (f.has_expression_cc11 && f.cc11_range >= 25u && f.vel_stddev >= 14.0 && f.duty_cycle < 0.75) {
            add(MR::PadSustainedHarmony, -2.8);
        }
    }

    // ── 10. Countermelody (mid-register 53..66 stepwise melodic answer) ──
    {
        if (f.mean_polyphony <= 1.35 && f.chordal_ratio <= 0.22) {
            add(MR::Countermelody, 2.5);
        } else {
            add(MR::Countermelody, -4.0);
        }
        if (f.pitch_median >= 53u && f.pitch_median <= 66u) {
            add(MR::Countermelody, 3.4);
        } else {
            add(MR::Countermelody, -3.8);
        }
        if (f.interval_stepwise_ratio >= 0.40) add(MR::Countermelody, 2.8);
        else if (f.interval_stepwise_ratio < 0.25) add(MR::Countermelody, -3.2);

        if (f.mean_velocity <= 86.0) add(MR::Countermelody, 1.4);
        else if (f.mean_velocity >= 96.0) add(MR::Countermelody, -1.5);

        if (f.notes_per_bar >= 2.0 && f.notes_per_bar <= 9.0) add(MR::Countermelody, 1.0);
        if (f.pitch_max <= 74u) add(MR::Countermelody, 0.8);
        if (is_riser_sweep) add(MR::Countermelody, -9.5);
    }

    // ── 11. BrassAccents (punchy, high-velocity, short rhythmic stabs with rests) ──
    {
        if (f.mean_velocity >= 96.0 && f.accent_ratio >= 0.45) {
            add(MR::BrassAccents, 3.6);
        } else if (f.mean_velocity >= 90.0 && f.duty_cycle <= 0.24 && f.inter_onset_gap_max_beats >= 2.0) {
            add(MR::BrassAccents, 3.4);
        } else if (f.mean_velocity < 86.0) {
            add(MR::BrassAccents, -3.8);
        }
        if (f.mean_duration_beats <= 0.60 && (f.staccato_ratio >= 0.35 || f.duty_cycle <= 0.20)) {
            add(MR::BrassAccents, 2.5);
        } else if (f.mean_duration_beats > 0.85) {
            add(MR::BrassAccents, -3.5);
        }
        if (f.duty_cycle <= 0.38 && f.inter_onset_gap_max_beats >= 1.25) {
            add(MR::BrassAccents, 2.8 + (f.duty_cycle <= 0.18 && f.inter_onset_gap_max_beats >= 2.5 ? 1.4 : 0.0));
        } else if (f.duty_cycle > 0.55) {
            add(MR::BrassAccents, -3.0);
        }
        if (f.pitch_median >= 56u && f.pitch_median <= 82u) add(MR::BrassAccents, 1.2);
        if (f.max_polyphony >= 1u && f.max_polyphony <= 4u) add(MR::BrassAccents, 0.8);
        if (f.has_strum_offsets || f.has_sustain_pedal) add(MR::BrassAccents, -3.5);
        if (f.has_expression_cc11 && f.cc11_range >= 25u && f.mean_duration_beats >= 0.70) {
            add(MR::BrassAccents, -3.0);
        }
    }

    // ── 12. OrchestralAccents (wide dynamic range / CC11 swells / dramatic marcato hits) ──
    {
        const bool strong_dyn = (f.velocity_dynamic_range >= 26u || f.vel_stddev >= 13.0);
        const bool strong_cc11 = (f.has_expression_cc11 && f.cc11_range >= 20u);
        if (strong_dyn) add(MR::OrchestralAccents, 2.8);
        if (strong_cc11) add(MR::OrchestralAccents, 3.2);
        if (!strong_dyn && !strong_cc11) add(MR::OrchestralAccents, -3.8);

        if (f.mean_duration_beats >= 0.65 && f.mean_duration_beats <= 2.30) {
            add(MR::OrchestralAccents, 2.0);
        } else if (f.mean_duration_beats < 0.35) {
            add(MR::OrchestralAccents, -2.5);
        }
        if (f.duty_cycle <= 0.68 && f.notes_per_bar <= 10.0) add(MR::OrchestralAccents, 1.6);
        if (f.downbeat_ratio >= 0.28 || f.pitch_span >= 18u) add(MR::OrchestralAccents, 1.4);
        if (f.has_strum_offsets) add(MR::OrchestralAccents, -4.0);
        if (is_riser_sweep) add(MR::OrchestralAccents, -7.5);
    }

    // ── 13. TransitionalEffects (riser/sweep/glissando or sparse boundary hit/cymbal) ──
    {
        if (is_riser_sweep) {
            add(MR::TransitionalEffects, 8.6);
        } else if (is_sparse_boundary_fx) {
            add(MR::TransitionalEffects, 6.8 + (is_fill_or_transition_section ? 0.6 : 0.0));
        } else {
            add(MR::TransitionalEffects, -4.8);
        }
    }

    // ── 14. MelodicPhrase (prominent upper-mid/treble stepwise melody/hook) ──
    {
        if (f.mean_polyphony <= 1.30 && f.chordal_ratio <= 0.20) {
            add(MR::MelodicPhrase, 2.5);
        } else {
            add(MR::MelodicPhrase, -4.0);
        }
        if (f.pitch_median >= 67u) {
            add(MR::MelodicPhrase, 3.5 + (f.pitch_max >= 74u ? 0.8 : 0.0));
        } else {
            add(MR::MelodicPhrase, -3.8);
        }
        if (f.interval_stepwise_ratio >= 0.38) add(MR::MelodicPhrase, 2.6);
        else if (f.interval_stepwise_ratio < 0.25) add(MR::MelodicPhrase, -3.2);

        if (f.notes_per_bar >= 3.0) add(MR::MelodicPhrase, 1.0);
        if (f.mean_velocity >= 80.0 || f.vel_stddev >= 5.0 || f.has_pitch_bend) add(MR::MelodicPhrase, 1.2);
        if (f.non_chord_tone_ratio >= 0.08 || f.duration_stddev_beats >= 0.12) add(MR::MelodicPhrase, 1.0);
        if (is_riser_sweep) add(MR::MelodicPhrase, -9.5);
    }

    // ── Optional strictly bounded weak metadata prior (<= 0.55 logit, NEVER overrides musical content) ──
    if (opt.use_metadata_hints) {
        using TF = exporters::korg::quality::TimbreFamily;
        const TF fam = tr.timbre.inferred.family;
        if (fam == TF::DrumKit) add(MR::KickSnareFoundation, 0.50);
        else if (fam == TF::PercussionKit) add(MR::DrumKitAuxPercussion, 0.50);
        else if (fam == TF::AcousticBass || fam == TF::ElectricBass || fam == TF::SynthBass) {
            if (f.pitch_median <= 55u) add(MR::BassFoundation, 0.50);
        } else if (fam == TF::Piano || fam == TF::ElectricPiano) {
            if (f.chordal_ratio >= 0.35) add(MR::PianoAccompaniment, 0.45);
        } else if (fam == TF::AcousticGuitar || fam == TF::ElectricGuitar) {
            if (f.has_strum_offsets) add(MR::GuitarStrumming, 0.45);
            else if (f.chordal_ratio <= 0.25) add(MR::Arpeggio, 0.40);
        } else if (fam == TF::SynthPad || fam == TF::ChoirPad) {
            if (f.mean_duration_beats >= 1.5) add(MR::PadSustainedHarmony, 0.45);
        } else if (fam == TF::Brass) {
            if (f.mean_velocity >= 90.0 && f.mean_duration_beats <= 0.8) add(MR::BrassAccents, 0.50);
        } else if (fam == TF::Strings) {
            if (f.velocity_dynamic_range >= 20u || f.has_expression_cc11) add(MR::OrchestralAccents, 0.45);
            else if (f.mean_duration_beats >= 1.6) add(MR::PadSustainedHarmony, 0.35);
        } else if (fam == TF::Organ || fam == TF::Accordion) {
            if (f.chordal_ratio >= 0.40) add(MR::HarmonicAccompaniment, 0.40);
        } else if (fam == TF::SynthLead || fam == TF::SaxReed || fam == TF::FlutePipe) {
            if (f.chordal_ratio <= 0.22) {
                if (f.pitch_median >= 66u) add(MR::MelodicPhrase, 0.45);
                else add(MR::Countermelody, 0.40);
            }
        }
    }

    // Apply unstructured noise compression so posterior flattens honestly -> UnknownAbstain
    if (looks_random_noise) {
        for (double& lg : logits) lg *= 0.15;
    }

    // ── Calibrated Softmax Posterior & Normalized Shannon Entropy ──
    const double temperature = 1.65;
    double max_lg = logits[0];
    for (double lg : logits) max_lg = std::max(max_lg, lg);

    double sum_exp = 0.0;
    for (size_t i = 0; i < kCanonicalSemanticRoleCount; ++i) {
        out.probs[i] = std::exp((logits[i] - max_lg) / temperature);
        sum_exp += out.probs[i];
    }
    double entropy = 0.0;
    const double log_k = std::log(static_cast<double>(kCanonicalSemanticRoleCount));
    for (size_t i = 0; i < kCanonicalSemanticRoleCount; ++i) {
        out.probs[i] /= (sum_exp > 0.0 ? sum_exp : 1.0);
        if (out.probs[i] > 1e-12) {
            entropy -= out.probs[i] * std::log(out.probs[i]);
        }
    }
    out.entropy = canonical_detail::clamp01(entropy / log_k);

    // Find top-1 and top-2 roles
    size_t top1 = 0, top2 = 1;
    if (out.probs[top2] > out.probs[top1]) std::swap(top1, top2);
    for (size_t i = 2; i < kCanonicalSemanticRoleCount; ++i) {
        if (out.probs[i] > out.probs[top1]) {
            top2 = top1;
            top1 = i;
        } else if (out.probs[i] > out.probs[top2]) {
            top2 = i;
        }
    }

    out.primary_role   = idxToRole(top1);
    out.secondary_role = idxToRole(top2);
    out.confidence     = out.probs[top1];
    out.margin         = out.probs[top1] - out.probs[top2];

    // Check intra-section dual-role hybrid (e.g., stride piano / LH bass + RH comping:
    // separate low bass events + upper chordal comping!)
    const bool stride_lh_bass_rh_comp =
        (!percussive_domain &&
         f.has_two_hand_split &&
         f.low_register_ratio >= 0.20 && f.low_register_ratio <= 0.58 &&
         f.chordal_ratio >= 0.35 && f.monophonic_ratio >= 0.25 &&
         f.pitch_min <= 45u && f.pitch_max >= 64u &&
         (out.primary_role == MR::PianoAccompaniment || out.primary_role == MR::ChordComping ||
          out.primary_role == MR::HarmonicAccompaniment));

    if (stride_lh_bass_rh_comp) {
        out.is_mixed_role = true;
        out.secondary_role = MR::BassFoundation;
        out.mixed_role_reason = "intra_section_dual_lh_bass_and_rh_comping";
    }

    // Fail-closed abstain gates (NEVER fabricate certainty!)
    if (looks_random_noise) {
        out.abstained = true;
        out.role = MR::UnknownAbstain;
        out.primary_role = MR::UnknownAbstain;
        out.abstain_reason = "unstructured_or_random_note_distribution";
    } else if (logits[top1] < 2.2) {
        out.abstained = true;
        out.role = MR::UnknownAbstain;
        out.primary_role = MR::UnknownAbstain;
        out.abstain_reason = "weak_musical_signal_evidence";
    } else if (out.confidence < opt.role_abstain_threshold) {
        out.abstained = true;
        out.role = MR::UnknownAbstain;
        out.primary_role = MR::UnknownAbstain;
        out.abstain_reason = "low_calibrated_confidence";
    } else if (out.entropy > opt.max_entropy_threshold) {
        out.abstained = true;
        out.role = MR::UnknownAbstain;
        out.primary_role = MR::UnknownAbstain;
        out.abstain_reason = "high_posterior_entropy";
    } else if (out.margin < opt.min_confidence_margin && !out.is_mixed_role) {
        out.abstained = true;
        out.role = MR::UnknownAbstain;
        out.primary_role = MR::UnknownAbstain;
        out.abstain_reason = "ambiguous_top2_margin";
    } else {
        out.abstained = false;
        out.role = out.primary_role;
    }

    // Supporting human-readable evidence strings
    if (opt.emit_explainability_trace) {
        out.evidence.push_back("register=" + std::string(registerBandName(f.register_band)) +
                               " (med=" + std::to_string(f.pitch_median) +
                               ", span=" + std::to_string(f.pitch_span) + ")");
        out.evidence.push_back("poly=" + canonical_detail::fmtDouble(f.mean_polyphony, 2) +
                               " (max=" + std::to_string(f.max_polyphony) +
                               ", chordal=" + canonical_detail::fmtDouble(f.chordal_ratio, 2) + ")");
        out.evidence.push_back("rhythm: dur=" + canonical_detail::fmtDouble(f.mean_duration_beats, 2) +
                               "b, sync=" + canonical_detail::fmtDouble(f.syncopation_ratio, 2) +
                               ", npb=" + canonical_detail::fmtDouble(f.notes_per_bar, 1));
        if (f.has_strum_offsets) {
            out.evidence.push_back("strum_micro_arpeggio=" + canonical_detail::fmtDouble(f.mean_strum_spread_ticks, 1) + "t");
        }
        if (f.has_sustain_pedal) out.evidence.push_back("cc64_sustain_active");
        if (f.has_expression_cc11) out.evidence.push_back("cc11_range=" + std::to_string(f.cc11_range));
        if (f.drum_kick_snare_ratio >= 0.15) {
            out.evidence.push_back("kick_snare_ratio=" + canonical_detail::fmtDouble(f.drum_kick_snare_ratio, 2));
        }
        if (f.drum_aux_perc_ratio >= 0.40) {
            out.evidence.push_back("aux_perc_ratio=" + canonical_detail::fmtDouble(f.drum_aux_perc_ratio, 2));
        }
        if (out.is_mixed_role) {
            out.evidence.push_back("mixed_role=" + out.mixed_role_reason);
        }
    }

    return out;
}

inline std::pair<GenreFeelClass, double> inferGenreFeel(
    const CanonicalStyleView& view,
    const GroovePocketProfile& groove,
    const std::vector<TrackRoleDecision>& roles,
    double abstain_threshold) {
    using GF = GenreFeelClass;
    std::array<double, 10> scores{};
    scores.fill(0.0);
    auto add = [&](GF g, double v) { scores[static_cast<size_t>(g)] += v; };

    const uint32_t bpm = view.tempo_bpm;
    bool has_3_4 = false;
    bool has_6_8 = groove.compound_meter_detected;
    for (const auto& s : view.sections) {
        if (s.meter_num == 3u && s.meter_den == 4u) has_3_4 = true;
    }

    uint32_t strum_ct = 0, orch_ct = 0, brass_ct = 0, aux_perc_ct = 0, elec_kit = 0, brush_jazz_kit = 0;
    uint32_t active_tracks = 0;
    double mean_vel_sum = 0.0;
    for (const auto& r : roles) {
        if (r.abstained) continue;
        ++active_tracks;
        if (r.role == MusicalRole::GuitarStrumming) ++strum_ct;
        if (r.role == MusicalRole::OrchestralAccents) ++orch_ct;
        if (r.role == MusicalRole::BrassAccents) ++brass_ct;
        if (r.role == MusicalRole::DrumKitAuxPercussion) ++aux_perc_ct;
    }
    for (const auto& s : view.sections) {
        for (const auto& tr : s.tracks) {
            if (tr.features.note_count == 0) continue;
            mean_vel_sum += tr.features.mean_velocity;
            if (tr.sound_identity.sub_family.find("Electronic") != std::string::npos ||
                tr.sound_identity.sub_family.find("EDM") != std::string::npos) {
                ++elec_kit;
            }
            if (tr.sound_identity.sub_family.find("Jazz") != std::string::npos ||
                tr.sound_identity.sub_family.find("Brush") != std::string::npos) {
                ++brush_jazz_kit;
            }
        }
    }
    (void)mean_vel_sum;

    if (active_tracks == 0) {
        return {GF::UnknownAbstain, 0.0};
    }

    // JazzSwing: clear swing ratio >= 0.57
    if (groove.estimated_swing_ratio >= 0.57) add(GF::JazzSwing, 4.2);
    if (brush_jazz_kit > 0) add(GF::JazzSwing, 1.5);
    if (bpm >= 100 && bpm <= 200 && groove.estimated_swing_ratio >= 0.56) add(GF::JazzSwing, 1.0);

    // AcousticFolkWaltz: 3/4 meter or acoustic strummed ballad
    if (has_3_4) add(GF::AcousticFolkWaltz, 4.0);
    if (strum_ct > 0 && bpm >= 75 && bpm <= 135) add(GF::AcousticFolkWaltz, 1.2);

    // VietnameseBoleroSlowRock: slow 6/8 or bolero 72..94 BPM with aux perc/arpeggio
    if (has_6_8 && bpm <= 95) add(GF::VietnameseBoleroSlowRock, 3.8);
    if (bpm >= 70 && bpm <= 94 && aux_perc_ct > 0 && groove.syncopation_index >= 0.20) {
        add(GF::VietnameseBoleroSlowRock, 2.2);
    }

    // OrchestralCinematic: orchestral accents or heavy dynamic swells without heavy drum kit
    if (orch_ct > 0) add(GF::OrchestralCinematic, 3.6);

    // EdmElectronic: fast 4/4 >= 118 BPM + straight + electronic kit or strong kick-bass lock
    if (bpm >= 118 && bpm <= 150 && groove.estimated_swing_ratio < 0.54) {
        add(GF::EdmElectronic, 1.8 + (elec_kit > 0 ? 2.5 : 0.0));
    }

    // FunkSyncopated: high syncopation + 95..122 BPM + brass/comping
    if (groove.syncopation_index >= 0.34 && bpm >= 92 && bpm <= 125) {
        add(GF::FunkSyncopated, 3.2 + (brass_ct > 0 ? 1.2 : 0.0));
    }

    // LatinBossaCha: aux percussion + moderate syncopation + 105..140 BPM
    if (aux_perc_ct > 0 && bpm >= 105 && bpm <= 145 && groove.estimated_swing_ratio < 0.55) {
        add(GF::LatinBossaCha, 2.8 + (groove.syncopation_index >= 0.25 ? 1.0 : 0.0));
    }

    // RockDrive: 112..155 BPM straight + strong kick-bass lock
    if (bpm >= 112 && bpm <= 155 && groove.estimated_swing_ratio < 0.54 && elec_kit == 0) {
        add(GF::RockDrive, 2.4 + (groove.kick_bass_lock_ratio >= 0.45 ? 1.0 : 0.0));
    }

    // PopBallad: 62..105 BPM straight 4/4
    if (bpm >= 60 && bpm <= 105 && !has_3_4 && !has_6_8 && groove.estimated_swing_ratio < 0.55) {
        add(GF::PopBallad, 2.8);
    }

    // Softmax across 1..9
    double max_s = scores[1];
    size_t best = 1;
    for (size_t i = 2; i < 10; ++i) {
        if (scores[i] > max_s) { max_s = scores[i]; best = i; }
    }
    if (max_s < 1.5) return {GF::UnknownAbstain, 0.0};
    double sum_e = 0.0;
    for (size_t i = 1; i < 10; ++i) sum_e += std::exp((scores[i] - max_s) / 1.4);
    const double conf = 1.0 / (sum_e > 0.0 ? sum_e : 1.0);
    if (conf < abstain_threshold) {
        return {GF::UnknownAbstain, conf};
    }
    return {static_cast<GF>(best), conf};
}

} // namespace semantic_detail

inline SemanticArrangementAnalysis analyzeMusicalSemantics(
    const uasf::StyleDefinition& uasf_style,
    const exporters::korg::Style* korg_style = nullptr,
    const exporters::korg::Report* korg_report = nullptr,
    const SemanticAnalyzeOptions& opt = {}) {
    SemanticArrangementAnalysis out{};
    out.style_name = uasf_style.name;
    out.canonical_view = buildCanonicalStyleView(uasf_style, korg_style, korg_report, opt.target_platform);
    out.hardware_verified = false;
    out.l3_status = "NOT_TESTED";

    // Step 1: Classify every track in every section independently (enables Group 13 section-role shift detection!)
    struct ChannelAgg {
        uint8_t src_ch = 0;
        uint8_t dst_ch = 0xFF;
        std::string name;
        std::vector<SectionRoleObservation> obs;
        std::array<double, kCanonicalSemanticRoleCount> weighted_probs{};
        double weight_sum = 0.0;
        bool any_intra_mixed = false;
        MusicalRole intra_mixed_secondary = MusicalRole::UnknownAbstain;
        std::string intra_mixed_reason;
        std::vector<std::string> evidence;
        uint32_t total_notes = 0;
        double entropy_weighted = 0.0;
        std::string last_abstain_reason;
    };

    std::map<uint8_t, ChannelAgg> by_ch;

    bool has_compound = false;
    bool has_mixed_meter = false;
    std::set<std::pair<uint8_t, uint8_t>> meters_seen;
    double sync_sum = 0.0, sync_w = 0.0;
    double swing_sum = 0.0, swing_w = 0.0;

    std::map<uasf::SectionType, double> variation_energies;

    for (auto& sec : out.canonical_view.sections) {
        meters_seen.insert({sec.meter_num, sec.meter_den});
        if ((sec.meter_num == 6u || sec.meter_num == 9u || sec.meter_num == 12u) && sec.meter_den == 8u) {
            has_compound = true;
        }
        if (sec.meter_num == 5u || sec.meter_num == 7u || sec.meter_num == 11u) {
            has_mixed_meter = true;
        }

        const bool is_fill_or_trans =
            (sec.type == uasf::SectionType::Fill1 || sec.type == uasf::SectionType::Fill2 ||
             sec.type == uasf::SectionType::Fill3 || sec.type == uasf::SectionType::Fill4 ||
             sec.type == uasf::SectionType::Break ||
             sec.type == uasf::SectionType::Intro1 || sec.type == uasf::SectionType::Ending1);
        const bool is_variation =
            (sec.type == uasf::SectionType::Main1 || sec.type == uasf::SectionType::Main2 ||
             sec.type == uasf::SectionType::Main3 || sec.type == uasf::SectionType::Main4);

        double sec_density = 0.0;
        double sec_vel_sum = 0.0;
        uint32_t sec_note_sum = 0;
        uint32_t sec_active_tracks = 0;

        for (auto& tr : sec.tracks) {
            const auto cls = semantic_detail::classifyTrackFeatures(tr, opt, is_fill_or_trans);
            tr.semantic_role = cls.role;
            tr.secondary_role = cls.secondary_role;
            tr.is_mixed_role = cls.is_mixed_role;
            tr.role_confidence = cls.confidence;
            tr.uncertainty_entropy = cls.entropy;
            tr.role_abstained = cls.abstained;
            tr.abstain_reason = cls.abstain_reason;
            tr.supporting_evidence = cls.evidence;

            tr.provenance.origin = DecisionOrigin::StatisticalInference;
            tr.provenance.component_id = "p5_semantic_v1";
            tr.provenance.confidence = cls.confidence;
            tr.provenance.uncertainty_entropy = cls.entropy;
            tr.provenance.abstained = cls.abstained;
            tr.provenance.abstain_reason = cls.abstain_reason;
            tr.provenance.reversible = true;
            tr.provenance.evidence = cls.evidence;
            tr.provenance.rationale = cls.abstained
                                          ? ("Abstained: " + cls.abstain_reason)
                                          : (std::string("Inferred ") + musicalRoleName(cls.role) +
                                             " (conf=" + canonical_detail::fmtDouble(cls.confidence, 3) + ")");

            if (tr.features.note_count > 0) {
                ++sec_active_tracks;
                sec_density += tr.features.notes_per_bar;
                sec_vel_sum += tr.features.mean_velocity * static_cast<double>(tr.features.note_count);
                sec_note_sum += tr.features.note_count;

                const double sw = is_variation ? 2.0 : 1.0;
                sync_sum += tr.features.syncopation_ratio * sw;
                sync_w += sw;
                if (tr.features.swing_offbeat_count >= 2u) {
                    const double gw = (tr.semantic_role == MusicalRole::KickSnareFoundation ? 3.0 : 1.0) *
                                      std::min<double>(8.0, static_cast<double>(tr.features.swing_offbeat_count)) * sw;
                    swing_sum += tr.features.swing_ratio_estimate * gw;
                    swing_w += gw;
                }
            }

            auto& agg = by_ch[tr.source_channel];
            if (agg.obs.empty()) {
                agg.src_ch = tr.source_channel;
                agg.dst_ch = tr.target_channel;
                agg.name = tr.name;
            } else if (agg.name.empty() && !tr.name.empty()) {
                agg.name = tr.name;
            }

            SectionRoleObservation obs{};
            obs.section_type = sec.type;
            obs.section_name = sec.name;
            obs.role = cls.role;
            obs.secondary_role = cls.secondary_role;
            obs.confidence = cls.confidence;
            obs.uncertainty_entropy = cls.entropy;
            obs.notes_per_bar = tr.features.notes_per_bar;
            obs.mean_velocity = tr.features.mean_velocity;
            obs.abstained = cls.abstained;
            obs.abstain_reason = cls.abstain_reason;
            agg.obs.push_back(std::move(obs));

            if (tr.features.note_count > 0) {
                const double w = static_cast<double>(tr.features.note_count) * (is_variation ? 1.5 : 1.0);
                for (size_t k = 0; k < kCanonicalSemanticRoleCount; ++k) {
                    agg.weighted_probs[k] += cls.probs[k] * w;
                }
                agg.weight_sum += w;
                agg.entropy_weighted += cls.entropy * w;
                agg.total_notes += tr.features.note_count;
            }
            if (cls.is_mixed_role) {
                agg.any_intra_mixed = true;
                agg.intra_mixed_secondary = cls.secondary_role;
                agg.intra_mixed_reason = cls.mixed_role_reason;
            }
            if (cls.abstained && !cls.abstain_reason.empty()) {
                agg.last_abstain_reason = cls.abstain_reason;
            }
            if (agg.evidence.empty() && !cls.evidence.empty()) {
                agg.evidence = cls.evidence;
            }
        }

        // Update register overlap role labels now that section roles are inferred
        for (auto& ov : sec.inter_track.register_overlaps) {
            for (const auto& tr : sec.tracks) {
                if (tr.source_channel == ov.channel_a) ov.role_a = tr.semantic_role;
                if (tr.source_channel == ov.channel_b) ov.role_b = tr.semantic_role;
            }
        }

        SectionEnergyArcPoint pt{};
        pt.section_type = sec.type;
        pt.section_name = sec.name;
        pt.note_density_per_bar = sec_density;
        pt.mean_velocity = (sec_note_sum > 0) ? (sec_vel_sum / static_cast<double>(sec_note_sum)) : 0.0;
        pt.active_track_count = sec_active_tracks;
        pt.energy_score = sec.composite_energy;
        out.energy_arc.push_back(std::move(pt));

        if (is_variation) {
            variation_energies[sec.type] = sec.composite_energy;
        }
    }

    if (meters_seen.size() > 1u) {
        has_mixed_meter = true;
    }

    // Step 2: Consolidate style-wide per-channel decisions & detect cross-section role shifts
    for (const auto& [ch, agg] : by_ch) {
        TrackRoleDecision dec{};
        dec.source_channel = agg.src_ch;
        dec.target_channel = agg.dst_ch;
        dec.track_name = agg.name;
        dec.per_section_roles = agg.obs;
        dec.supporting_evidence = agg.evidence;

        if (agg.total_notes == 0 || agg.weight_sum <= 0.0) {
            dec.abstained = true;
            dec.role = MusicalRole::UnknownAbstain;
            dec.primary_role = MusicalRole::UnknownAbstain;
            dec.secondary_role = MusicalRole::UnknownAbstain;
            dec.runner_up_role = MusicalRole::UnknownAbstain;
            dec.abstain_reason = agg.last_abstain_reason.empty() ? "empty_track_zero_notes" : agg.last_abstain_reason;
            dec.uncertainty_entropy = 1.0;
            dec.confidence = 0.0;
            dec.margin = 0.0;
        } else {
            // Normalize aggregated posterior probabilities (P1-3: guard all-zero posteriors from early abstain guards!)
            double p_sum = 0.0;
            for (size_t k = 0; k < kCanonicalSemanticRoleCount; ++k) {
                dec.role_probabilities[k] = agg.weighted_probs[k] / agg.weight_sum;
                p_sum += dec.role_probabilities[k];
            }
            if (p_sum <= 1e-12) {
                dec.role_probabilities.fill(0.0);
                dec.uncertainty_entropy = 1.0;
                dec.role = MusicalRole::UnknownAbstain;
                dec.primary_role = MusicalRole::UnknownAbstain;
                dec.secondary_role = MusicalRole::UnknownAbstain;
                dec.runner_up_role = MusicalRole::UnknownAbstain;
                dec.confidence = 0.0;
                dec.margin = 0.0;
                dec.abstained = true;
                dec.abstain_reason = agg.last_abstain_reason.empty()
                                         ? "insufficient_note_evidence"
                                         : agg.last_abstain_reason;
                dec.role_stability_ratio = 0.0;
            } else {
                double ent = 0.0;
                const double log_k = std::log(static_cast<double>(kCanonicalSemanticRoleCount));
                for (size_t k = 0; k < kCanonicalSemanticRoleCount; ++k) {
                    dec.role_probabilities[k] /= p_sum;
                    if (dec.role_probabilities[k] > 1e-12) {
                        ent -= dec.role_probabilities[k] * std::log(dec.role_probabilities[k]);
                    }
                }
                dec.uncertainty_entropy = canonical_detail::clamp01(ent / log_k);

                // Count confident section-level role votes (and check for substantive distinct roles across sections!)
                std::map<MusicalRole, double> confident_role_weight;
                std::map<MusicalRole, bool> role_seen_in_variation;
                uint32_t non_empty_sections = 0;
                uint32_t confident_sections = 0;
                double total_conf_weight = 0.0;
                for (const auto& ob : agg.obs) {
                    if (ob.notes_per_bar <= 0.0) continue;
                    ++non_empty_sections;
                    if (!ob.abstained && ob.role != MusicalRole::UnknownAbstain) {
                        ++confident_sections;
                        const double rw = std::max(0.1, ob.confidence * ob.notes_per_bar);
                        confident_role_weight[ob.role] += rw;
                        total_conf_weight += rw;
                        if (ob.section_type == uasf::SectionType::Main1 ||
                            ob.section_type == uasf::SectionType::Main2 ||
                            ob.section_type == uasf::SectionType::Main3 ||
                            ob.section_type == uasf::SectionType::Main4) {
                            role_seen_in_variation[ob.role] = true;
                        }
                    }
                }

                size_t top1 = 0, top2 = 1;
                if (dec.role_probabilities[top2] > dec.role_probabilities[top1]) std::swap(top1, top2);
                for (size_t k = 2; k < kCanonicalSemanticRoleCount; ++k) {
                    if (dec.role_probabilities[k] > dec.role_probabilities[top1]) {
                        top2 = top1;
                        top1 = k;
                    } else if (dec.role_probabilities[k] > dec.role_probabilities[top2]) {
                        top2 = k;
                    }
                }

                dec.primary_role   = semantic_detail::idxToRole(top1);
                dec.secondary_role = semantic_detail::idxToRole(top2);
                dec.runner_up_role = dec.secondary_role;
                dec.confidence     = dec.role_probabilities[top1];
                dec.margin         = dec.role_probabilities[top1] - dec.role_probabilities[top2];

                // Cross-section role shift detection: >= 2 substantive confident roles across sections!
                // Ignore trivial 1-hit Ending/Break cymbal punctuation on a drum foundation track.
                bool substantive_cross_shift = false;
                std::vector<std::pair<MusicalRole, double>> ranked(confident_role_weight.begin(),
                                                                   confident_role_weight.end());
                std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                    return a.second > b.second;
                });
                if (ranked.size() >= 2u && total_conf_weight > 0.0) {
                    const double sec_share = ranked[1].second / total_conf_weight;
                    const bool sec_in_var = role_seen_in_variation[ranked[1].first];
                    const bool trivial_ending_cymbal =
                        (ranked[0].first == MusicalRole::KickSnareFoundation &&
                         ranked[1].first == MusicalRole::TransitionalEffects &&
                         !sec_in_var && sec_share < 0.25);
                    if (!trivial_ending_cymbal && (sec_share >= 0.15 || sec_in_var)) {
                        substantive_cross_shift = true;
                    }
                }

                if (substantive_cross_shift) {
                    dec.primary_role = ranked[0].first;
                    dec.secondary_role = ranked[1].first;
                    dec.runner_up_role = ranked[1].first;
                    dec.is_mixed_role = true;
                    dec.section_role_shift_detected = true;
                    dec.mixed_role_reason = std::string("cross_section_role_shift:") +
                                            musicalRoleName(dec.primary_role) + "->" +
                                            musicalRoleName(dec.secondary_role);
                    const double p1 = dec.role_probabilities[semantic_detail::roleToIdx(dec.primary_role)];
                    const double p2 = dec.role_probabilities[semantic_detail::roleToIdx(dec.secondary_role)];
                    dec.confidence = std::min(0.95, p1 + p2);
                    dec.abstained = false;
                    dec.role = dec.primary_role;
                } else if (agg.any_intra_mixed && confident_sections > 0) {
                    dec.is_mixed_role = true;
                    dec.secondary_role = agg.intra_mixed_secondary;
                    dec.runner_up_role = agg.intra_mixed_secondary;
                    dec.mixed_role_reason = agg.intra_mixed_reason;
                    dec.abstained = false;
                    dec.role = dec.primary_role;
                } else if (confident_sections == 0 ||
                           dec.confidence < opt.role_abstain_threshold ||
                           dec.uncertainty_entropy > opt.max_entropy_threshold ||
                           dec.margin < opt.min_confidence_margin) {
                    // P1-3: Abstained tracks ALWAYS set primary_role = UnknownAbstain & honest non-zero entropy!
                    dec.abstained = true;
                    dec.role = MusicalRole::UnknownAbstain;
                    dec.primary_role = MusicalRole::UnknownAbstain;
                    if (dec.uncertainty_entropy < 1e-6) {
                        dec.uncertainty_entropy = 1.0;
                    }
                    if (!agg.last_abstain_reason.empty()) {
                        dec.abstain_reason = agg.last_abstain_reason;
                    } else if (dec.confidence < opt.role_abstain_threshold) {
                        dec.abstain_reason = "low_calibrated_confidence";
                    } else if (dec.uncertainty_entropy > opt.max_entropy_threshold) {
                        dec.abstain_reason = "high_posterior_entropy";
                    } else {
                        dec.abstain_reason = "ambiguous_top2_margin";
                    }
                } else {
                    dec.abstained = false;
                    dec.role = dec.primary_role;
                }

                // Role stability across active sections
                if (non_empty_sections > 0 && !dec.abstained) {
                    uint32_t agree = 0;
                    for (const auto& ob : agg.obs) {
                        if (ob.notes_per_bar > 0.0 && ob.role == dec.primary_role && !ob.abstained) {
                            ++agree;
                        }
                    }
                    dec.role_stability_ratio = static_cast<double>(agree) / static_cast<double>(non_empty_sections);
                }
            }
        }

        dec.provenance.origin = DecisionOrigin::StatisticalInference;
        dec.provenance.component_id = "p5_semantic_v1";
        dec.provenance.confidence = dec.confidence;
        dec.provenance.uncertainty_entropy = dec.uncertainty_entropy;
        dec.provenance.abstained = dec.abstained;
        dec.provenance.abstain_reason = dec.abstain_reason;
        dec.provenance.reversible = true;
        dec.provenance.evidence = dec.supporting_evidence;
        if (dec.abstained) {
            ++out.abstained_track_count;
            dec.provenance.rationale = "Fail-closed UnknownAbstain (" + dec.abstain_reason + ")";
        } else if (dec.is_mixed_role) {
            ++out.mixed_role_track_count;
            dec.provenance.rationale = std::string("MixedRole primary=") + musicalRoleName(dec.primary_role) +
                                       ", secondary=" + musicalRoleName(dec.secondary_role) +
                                       " (" + dec.mixed_role_reason + ")";
        } else {
            dec.provenance.rationale = std::string("Classified ") + musicalRoleName(dec.role) +
                                       " (conf=" + canonical_detail::fmtDouble(dec.confidence, 3) +
                                       ", H=" + canonical_detail::fmtDouble(dec.uncertainty_entropy, 3) + ")";
        }

        out.track_roles.push_back(std::move(dec));
    }

    // Recompute role-aware overall inter-track relationships across ALL active sections (P1-1)
    {
        std::map<uint8_t, MusicalRole> role_by_ch;
        for (const auto& dec : out.track_roles) {
            role_by_ch[dec.source_channel] = dec.role;
        }
        std::map<uint8_t, CanonicalTrackView> ov_track_map;
        std::map<uint8_t, std::vector<exporters::korg::quality::NoteSpan>> ov_notes_map;
        uint32_t offset = 0;
        uint32_t tot_bars = 0;
        for (size_t sidx = 0; sidx < uasf_style.sections.size() && sidx < out.canonical_view.sections.size(); ++sidx) {
            const auto& u_sec = uasf_style.sections[sidx];
            const auto& c_sec = out.canonical_view.sections[sidx];
            for (size_t tidx = 0; tidx < u_sec.tracks.size() && tidx < c_sec.tracks.size(); ++tidx) {
                const auto& ut = u_sec.tracks[tidx];
                const auto& ct = c_sec.tracks[tidx];
                const auto spans = exporters::korg::quality::extractNoteSpansFromEvents(ut.events, c_sec.length_ticks);
                if (ov_track_map.find(ct.source_channel) == ov_track_map.end() ||
                    (ov_track_map[ct.source_channel].features.note_count == 0 && ct.features.note_count > 0)) {
                    ov_track_map[ct.source_channel] = ct;
                }
                for (auto ns : spans) {
                    ns.on_tick  = canonical_detail::saturatingAddU32(ns.on_tick, offset);
                    ns.off_tick = canonical_detail::saturatingAddU32(ns.off_tick, offset);
                    ov_notes_map[ct.source_channel].push_back(ns);
                }
            }
            offset = canonical_detail::saturatingAddU32(offset, c_sec.length_ticks);
            tot_bars = canonical_detail::saturatingAddU32(tot_bars, c_sec.bars);
        }
        std::vector<CanonicalTrackView> ov_tracks;
        std::vector<std::vector<exporters::korg::quality::NoteSpan>> ov_notes;
        const uint32_t eff_bars = std::max<uint32_t>(1u, tot_bars);
        for (auto& [ch, tr] : ov_track_map) {
            const auto& merged = ov_notes_map[ch];
            if (!merged.empty()) {
                canonical_detail::ControllerTrace empty_ctrl{};
                tr.features = canonical_detail::computeTrackFeatures(
                    merged, empty_ctrl, out.canonical_view.ppqn, 4u, 4u, eff_bars,
                    tr.casm.source_chord_root, tr.casm.source_chord_type);
            }
            if (auto rit = role_by_ch.find(ch); rit != role_by_ch.end()) {
                tr.semantic_role = rit->second;
            }
            ov_tracks.push_back(tr);
            ov_notes.push_back(merged);
        }
        out.inter_track = canonical_detail::computeInterTrackRelationships(
            ov_tracks, ov_notes, out.canonical_view.ppqn);
        out.canonical_view.inter_track_overall = out.inter_track;
    }

    // Step 3: Groove pocket profile & Variation energy monotonicity
    out.groove.estimated_swing_ratio = (swing_w > 0.0) ? (swing_sum / swing_w) : 0.50;
    out.groove.kick_bass_lock_ratio = out.inter_track.kick_bass_lock_correlation;
    out.groove.syncopation_index = (sync_w > 0.0) ? (sync_sum / sync_w) : 0.0;
    out.groove.compound_meter_detected = has_compound;
    out.groove.mixed_meter_detected = has_mixed_meter;

    // Monotonic check across Main1..Main4 (allow tiny <= 1.0 noise tolerance)
    out.variation_arc_monotonic = true;
    static constexpr std::array<uasf::SectionType, 4> kVars = {
        uasf::SectionType::Main1, uasf::SectionType::Main2,
        uasf::SectionType::Main3, uasf::SectionType::Main4
    };
    double prev_e = -1.0;
    for (auto vt : kVars) {
        auto it = variation_energies.find(vt);
        if (it != variation_energies.end()) {
            if (prev_e >= 0.0 && it->second + 1.0 < prev_e) {
                out.variation_arc_monotonic = false;
            }
            prev_e = it->second;
        }
    }

    // Step 4: Genre / feel inference (fail-closed)
    const auto [gcls, gconf] = semantic_detail::inferGenreFeel(
        out.canonical_view, out.groove, out.track_roles, opt.genre_abstain_threshold);
    out.inferred_genre = gcls;
    out.genre_confidence = gconf;
    out.genre_abstained = (gcls == GenreFeelClass::UnknownAbstain);

    if (opt.emit_explainability_trace) {
        out.explainability_notes.push_back(
            "Analyzed " + std::to_string(out.canonical_view.sections.size()) + " sections, " +
            std::to_string(out.track_roles.size()) + " tracks (" +
            std::to_string(out.abstained_track_count) + " abstained, " +
            std::to_string(out.mixed_role_track_count) + " mixed-role).");
        if (out.inter_track.bass_piano_low_collision) {
            out.explainability_notes.push_back(
                "Detected Bass/Piano low-register collision (" +
                std::to_string(out.inter_track.bass_piano_low_collision_notes) + " notes < C3).");
        }
    }

    return out;
}

inline std::string formatSemanticAnalysisJson(const SemanticArrangementAnalysis& a) {
    using target_cap_detail::jsonEscape;
    using canonical_detail::fmtDouble;
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": \"" << jsonEscape(a.schema) << "\",\n";
    os << "  \"styleName\": \"" << jsonEscape(a.style_name) << "\",\n";
    os << "  \"inferredGenre\": \"" << genreFeelClassName(a.inferred_genre) << "\",\n";
    os << "  \"genreConfidence\": " << fmtDouble(a.genre_confidence, 4) << ",\n";
    os << "  \"genreAbstained\": " << (a.genre_abstained ? "true" : "false") << ",\n";
    os << "  \"abstainedTrackCount\": " << a.abstained_track_count << ",\n";
    os << "  \"mixedRoleTrackCount\": " << a.mixed_role_track_count << ",\n";
    os << "  \"variationArcMonotonic\": " << (a.variation_arc_monotonic ? "true" : "false") << ",\n";
    os << "  \"hardwareVerified\": " << (a.hardware_verified ? "true" : "false") << ",\n";
    os << "  \"l3Status\": \"" << jsonEscape(a.l3_status) << "\",\n";
    os << "  \"groove\": {\n";
    os << "    \"estimatedSwingRatio\": " << fmtDouble(a.groove.estimated_swing_ratio, 4) << ",\n";
    os << "    \"kickBassLockRatio\": " << fmtDouble(a.groove.kick_bass_lock_ratio, 4) << ",\n";
    os << "    \"syncopationIndex\": " << fmtDouble(a.groove.syncopation_index, 4) << ",\n";
    os << "    \"compoundMeterDetected\": " << (a.groove.compound_meter_detected ? "true" : "false") << ",\n";
    os << "    \"mixedMeterDetected\": " << (a.groove.mixed_meter_detected ? "true" : "false") << "\n";
    os << "  },\n";
    os << "  \"interTrack\": {\n";
    os << "    \"kickBassLockCorrelation\": " << fmtDouble(a.inter_track.kick_bass_lock_correlation, 4) << ",\n";
    os << "    \"bassPianoLowCollision\": " << (a.inter_track.bass_piano_low_collision ? "true" : "false") << ",\n";
    os << "    \"bassPianoLowCollisionNotes\": " << a.inter_track.bass_piano_low_collision_notes << ",\n";
    os << "    \"registerOverlaps\": " << a.inter_track.register_overlaps.size() << "\n";
    os << "  },\n";
    os << "  \"trackRoles\": [";
    for (size_t i = 0; i < a.track_roles.size(); ++i) {
        const auto& r = a.track_roles[i];
        if (i > 0) os << ",";
        os << "\n    {"
           << "\"srcCh\": " << static_cast<int>(r.source_channel)
           << ", \"dstCh\": " << static_cast<int>(r.target_channel)
           << ", \"name\": \"" << jsonEscape(r.track_name) << "\""
           << ", \"role\": \"" << musicalRoleName(r.role) << "\""
           << ", \"primaryRole\": \"" << musicalRoleName(r.primary_role) << "\""
           << ", \"secondaryRole\": \"" << musicalRoleName(r.secondary_role) << "\""
           << ", \"isMixedRole\": " << (r.is_mixed_role ? "true" : "false")
           << ", \"sectionShift\": " << (r.section_role_shift_detected ? "true" : "false")
           << ", \"stability\": " << fmtDouble(r.role_stability_ratio, 4)
           << ", \"confidence\": " << fmtDouble(r.confidence, 4)
           << ", \"margin\": " << fmtDouble(r.margin, 4)
           << ", \"entropy\": " << fmtDouble(r.uncertainty_entropy, 4)
           << ", \"abstained\": " << (r.abstained ? "true" : "false")
           << ", \"abstainReason\": \"" << jsonEscape(r.abstain_reason) << "\""
           << "}";
    }
    if (!a.track_roles.empty()) os << "\n  ";
    os << "],\n";
    os << "  \"energyArc\": [";
    for (size_t i = 0; i < a.energy_arc.size(); ++i) {
        const auto& pt = a.energy_arc[i];
        if (i > 0) os << ",";
        os << "\n    {"
           << "\"section\": \"" << jsonEscape(pt.section_name) << "\""
           << ", \"density\": " << fmtDouble(pt.note_density_per_bar, 2)
           << ", \"meanVelocity\": " << fmtDouble(pt.mean_velocity, 2)
           << ", \"activeTracks\": " << pt.active_track_count
           << ", \"energy\": " << fmtDouble(pt.energy_score, 2)
           << "}";
    }
    if (!a.energy_arc.empty()) os << "\n  ";
    os << "]\n}\n";
    return os.str();
}

inline std::string formatSemanticAnalysisMd(const SemanticArrangementAnalysis& a) {
    using canonical_detail::fmtDouble;
    std::ostringstream os;
    os << "# Musical Semantic Analysis — " << a.style_name << "\n\n";
    os << "- **Inferred Genre/Feel:** `" << genreFeelClassName(a.inferred_genre) << "`"
       << " (conf=" << fmtDouble(a.genre_confidence, 3)
       << ", abstained=" << (a.genre_abstained ? "true" : "false") << ")\n";
    os << "- **Groove Pocket:** swing=" << fmtDouble(a.groove.estimated_swing_ratio, 3)
       << " | kick-bass lock=" << fmtDouble(a.groove.kick_bass_lock_ratio, 3)
       << " | syncopation=" << fmtDouble(a.groove.syncopation_index, 3)
       << " | compound=" << (a.groove.compound_meter_detected ? "yes" : "no")
       << " | mixedMeter=" << (a.groove.mixed_meter_detected ? "yes" : "no") << "\n";
    os << "- **Variation Energy Monotonic:** " << (a.variation_arc_monotonic ? "true" : "false")
       << " | **Abstained Tracks:** " << a.abstained_track_count
       << " | **Mixed-Role Tracks:** " << a.mixed_role_track_count << "\n";
    os << "- **Epistemic Lock:** `hardwareVerified: false` | `l3Status: " << a.l3_status << "`\n\n";
    os << "| Src Ch | Dst Ch | Track | Role | Mixed | Secondary | Conf | Entropy | Stability | Status |\n";
    os << "|---:|---:|---|---|---|---|---:|---:|---:|---|\n";
    for (const auto& r : a.track_roles) {
        os << "| " << static_cast<int>(r.source_channel)
           << " | " << static_cast<int>(r.target_channel)
           << " | " << r.track_name
           << " | `" << musicalRoleName(r.role) << "`"
           << " | " << (r.is_mixed_role ? "yes" : "no")
           << " | `" << musicalRoleName(r.secondary_role) << "`"
           << " | " << fmtDouble(r.confidence, 3)
           << " | " << fmtDouble(r.uncertainty_entropy, 3)
           << " | " << fmtDouble(r.role_stability_ratio, 2)
           << " | " << (r.abstained ? ("ABSTAIN:" + r.abstain_reason) : "OK") << " |\n";
    }
    return os.str();
}

} // namespace ai_arranger::intelligence

#endif // AI_ARRANGER_INTELLIGENCE_MUSICAL_SEMANTIC_ANALYZER_H
