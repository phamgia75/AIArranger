#ifndef AI_ARRANGER_INTELLIGENCE_CONTEXT_SOUND_MATCHER_H
#define AI_ARRANGER_INTELLIGENCE_CONTEXT_SOUND_MATCHER_H

#include "exporters/korg/pa5x_sound_matcher.h"
#include "intelligence/musical_semantic_analyzer.h"
#include "intelligence/target_capability.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Phase 5 Milestone P5-M3: AI-Assisted Context Sound Matcher.
//
// Boundaries & Governance Invariants (ADR-P5-002 / ADR-P5-003 / ADR-P5-004 / ADR-P5-005):
//   * Header-only portable C++20, zero external ML deps, zero mutable globals,
//     zero JUCE/Apple/src/media dependencies. Strictly Zone B (offline).
//   * Extends pa5x_sound_matcher.h + timbre_profiler.h using P5-M1 TargetCapabilityProfile,
//     P5-M2 SemanticArrangementAnalysis, and cross-track ensemble context.
//   * NEVER duplicates pa5xFactoryCatalog() and NEVER silently replaces deterministic
//     verified mappings: opt-in only (`enabled` / `enable_auto_select` default false!).
//   * User `--sound-map` (`user_overrides`) ALWAYS takes unconditional precedence
//     (`MatchOrigin::UserSoundMap`, `USER_VERIFIED`).
//   * Evaluates all 10 required musical & target factors:
//       1. Instrument family             6. Ensemble function (P5-M2 MusicalRole)
//       2. Timbral characteristics       7. Percussive vs sustained behavior
//       3. Articulation compatibility    8. Drum mapping semantics
//       4. Register suitability          9. Target instrument capabilities (5 platforms)
//       5. Dynamic response             10. Cross-track ensemble interaction / masking
//   * Enforces fail-closed invariants:
//       - 0 melodic <-> drum cross-family contaminations.
//       - 100% rejection of out-of-spec Target KST banks (USERDK07..12, Bank-2 unverified,
//         banks exceeding TargetCapabilityProfile::sound_addressing bounds).
//       - EXP15-style unknown private banks with low timbre confidence (< 40) or
//         Unknown family ALWAYS stay UNRESOLVED (never auto-guessed).
//       - Epistemic stamps preserved: hardware_verified = false, l3_status = "NOT_TESTED",
//         DOCUMENTED_NOT_HW_VERIFIED on factory entries.

namespace ai_arranger::intelligence {

enum class ContextMatchProvenance : uint8_t {
    DeterministicVerified      = 0, // UserSoundMap override or deterministic verified fallback
    ContextStatisticalAdvisory = 1, // Ranked 10-factor offline statistical context recommendation
    UnresolvedFailClosed       = 2, // Fail-closed UNRESOLVED (disabled, low conf, unknown bank, or OOS)
};

inline constexpr const char* contextMatchProvenanceName(ContextMatchProvenance p) noexcept {
    switch (p) {
        case ContextMatchProvenance::DeterministicVerified:      return "DETERMINISTIC_VERIFIED";
        case ContextMatchProvenance::ContextStatisticalAdvisory: return "CONTEXT_STATISTICAL_ADVISORY";
        case ContextMatchProvenance::UnresolvedFailClosed:       return "UNRESOLVED_FAIL_CLOSED";
    }
    return "UNRESOLVED_FAIL_CLOSED";
}

struct ContextSoundMatchOptions {
    TargetPlatformId target_platform                = TargetPlatformId::KorgPa5X;
    bool             enabled                        = false; // Explicit feature flag (default OFF!)
    bool             enable_auto_select             = false; // Default OFF (fail-closed parity with SoundMatchOptions)
    bool             prefer_target_user_sounds      = false;
    bool             use_semantic_roles             = true;  // Ablation toggle for P5-EXP-04
    bool             use_ensemble_interaction       = true;  // Ablation toggle for factor 10 masking
    double           min_auto_select_confidence     = 0.70;  // 70/100 fail-closed floor
    uint8_t          min_timbre_profile_confidence  = 40;    // EXP15 fail-closed floor
    double           role_context_weight            = 0.25;
    double           genre_context_weight           = 0.15;
    double           ensemble_masking_penalty_weight = 0.10;
    size_t           top_k                          = 3;     // Clamped to [1..5]
};

// Transparent 10-factor normalized breakdown (each factor in [0.0, 1.0]).
struct ContextFactorBreakdown {
    double instrument_family_score        = 0.0; // Factor 1
    double timbral_characteristics_score  = 0.0; // Factor 2
    double articulation_compatibility     = 0.0; // Factor 3
    double register_suitability           = 0.0; // Factor 4
    double dynamic_response               = 0.0; // Factor 5
    double ensemble_function_role         = 0.0; // Factor 6
    double percussive_vs_sustained        = 0.0; // Factor 7
    double drum_mapping_semantics         = 0.0; // Factor 8
    double target_capability_fit          = 0.0; // Factor 9
    double ensemble_interaction_clearance = 1.0; // Factor 10 (1.0 = no collision, < 1.0 = masking penalty)
};

struct RankedSoundOption {
    exporters::korg::quality::SoundCandidate base_candidate{};
    ContextFactorBreakdown                   factors{};
    double                                   base_score_01 = 0.0;
    double                                   role_fit_delta = 0.0;
    double                                   genre_fit_delta = 0.0;
    double                                   masking_delta = 0.0;
    double                                   final_confidence = 0.0; // clamped [0.0, 1.0]
    ContextMatchProvenance                   provenance_class = ContextMatchProvenance::ContextStatisticalAdvisory;
    std::string                              match_rationale;
    std::string                              known_limitations;
    std::string                              estimated_musical_impact;
    std::string                              fallback_behavior;
    ProvenanceRecord                         provenance{};
};

struct ContextTrackSoundDecision {
    uint8_t                                  source_channel = 0;
    uint8_t                                  target_channel = 0xFF;
    std::string                              track_name;
    MusicalRole                              semantic_role = MusicalRole::UnknownAbstain;
    std::vector<RankedSoundOption>           ranked_options; // descending by final_confidence (max top_k <= 5)
    std::optional<exporters::korg::SoundRef> selected;       // empty if fail-closed UNRESOLVED
    exporters::korg::quality::MatchOrigin    selected_origin = exporters::korg::quality::MatchOrigin::Pa5xFactoryCatalog;
    ContextMatchProvenance                   selected_provenance = ContextMatchProvenance::UnresolvedFailClosed;
    std::string                              epistemic_tag;
    bool                                     fell_back_unresolved = false;
    bool                                     user_override_locked = false; // true iff UserSoundMap won
    bool                                     ensemble_collision_mitigated = false;
    std::string                              fallback_behavior;
    std::string                              rationale;
};

struct ContextSoundMatchResult {
    std::string                                  schema = "aiarranger-context-sound-match/1";
    TargetPlatformId                             target_platform = TargetPlatformId::KorgPa5X;
    bool                                         auto_select_enabled = false;
    std::vector<ContextTrackSoundDecision>       tracks;
    std::map<uint8_t, exporters::korg::SoundRef> resolved_sounds;
    uint32_t                                     user_overrides_preserved = 0;
    uint32_t                                     auto_selected_count = 0;
    uint32_t                                     unresolved_fallback_count = 0;
    uint32_t                                     oos_target_entries_rejected = 0;
    uint32_t                                     cross_family_contaminations = 0; // Invariant: ALWAYS 0
    uint32_t                                     ensemble_collisions_mitigated = 0;
    bool                                         hardware_verified = false;
    std::string                                  l3_status = "NOT_TESTED";
};

namespace context_sound_detail {

inline double clamp01(double v) noexcept {
    if (!std::isfinite(v) || v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

inline double clampRange(double v, double lo, double hi) noexcept {
    if (!std::isfinite(v)) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline std::string toLowerAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

inline bool containsAny(std::string_view lower_hay, std::initializer_list<const char*> needles) noexcept {
    for (const char* n : needles) {
        if (n && *n != '\0' && lower_hay.find(n) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

inline std::string jsonEscapeStrict(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uc < 0x20u || uc == 0x7Fu) {
                    out += "\\u00";
                    out.push_back(kHex[(uc >> 4) & 0x0Fu]);
                    out.push_back(kHex[uc & 0x0Fu]);
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

inline std::string fmtFinite(double v, int prec = 4) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::fixed << std::setprecision(prec) << (std::isfinite(v) ? v : 0.0);
    return oss.str();
}

// Strict TargetCapabilityProfile-aware in-spec bank check (no hardcoded vendor magic outside profile!).
// Enforces:
//   * Base `isTargetSoundEntryInSpec()` (blocks USERDK07..12 on Pa5X)
//   * Profile `melodic_user_bank_min..max` and `drum_user_bank_min..max`
//   * Rejects unverified Bank-2 (0x02) drum kits or out-of-range user banks
inline bool isTargetEntryValidForProfile(
    const exporters::korg::quality::TargetSoundEntry& entry,
    const TargetCapabilityProfile& profile) noexcept {
    if (!entry.in_spec) return false;
    if (entry.pc >= 128u) return false;
    // Reject Bank-2 unverified drum/melodic marker (lsb == 2 when msb != gm2_melodic/drum user banks)
    if (entry.msb == 0x02u || (entry.is_drum_kit && entry.lsb == 0x02u)) return false;

    const auto& addr = profile.sound_addressing;
    if (entry.slot_address.has_value()) {
        const auto& sa = *entry.slot_address;
        if (sa.is_drum_kit != entry.is_drum_kit) return false;
        if (sa.slot >= 128u) return false;
        if (entry.is_drum_kit) {
            if (sa.bank < addr.drum_user_bank_min || sa.bank > addr.drum_user_bank_max) return false;
        } else {
            if (sa.bank < addr.melodic_user_bank_min || sa.bank > addr.melodic_user_bank_max) return false;
        }
        if (profile.id == TargetPlatformId::KorgPa5X || profile.id == TargetPlatformId::KorgPa4XPa1000) {
            if (!exporters::korg::quality::isTargetSoundEntryInSpec(entry)) return false;
        }
        return true;
    }

    // Without explicit slot_address, validate against profile GM2 user bank ranges on Korg targets
    if (profile.id == TargetPlatformId::KorgPa5X || profile.id == TargetPlatformId::KorgPa4XPa1000) {
        if (!exporters::korg::quality::isTargetSoundEntryInSpec(entry)) return false;
        // Also enforce legacy Pa4X/Pa1000 tighter caps (USER01..04, USERDK01..02)
        if (entry.lsb >= 64u) {
            const uint32_t bank = static_cast<uint32_t>(entry.lsb - 63u);
            if (entry.is_drum_kit) {
                if (bank < addr.drum_user_bank_min || bank > addr.drum_user_bank_max) return false;
            } else {
                if (bank < addr.melodic_user_bank_min || bank > addr.melodic_user_bank_max) return false;
            }
        }
        return true;
    }

    // Non-Korg target profiles: enforce MSB/LSB consistency and non-drum/drum separation
    if (entry.is_drum_kit) {
        return addr.isDrumBank(entry.msb, entry.lsb) || entry.msb == addr.gm2_drum_msb;
    }
    return !addr.isDrumBank(entry.msb, entry.lsb);
}

// Determines whether a candidate is bright/dominant in upper harmonics (for ensemble masking factor 10).
inline bool isBrightOrDominantTimbre(std::string_view lower_name_tags) noexcept {
    return containsAny(lower_name_tags, {
        "bright", "brass", "trumpet", "saw", "lead", "distortion", "overdrive",
        "clav", "harpsichord", "dx", "fm", "accordion", "sax", "grand"
    });
}

// Determines whether a candidate is naturally sustained (1.0) vs percussive/plucked (0.0).
inline double candidateSustainAffinity(exporters::korg::quality::TimbreFamily fam,
                                       std::string_view lower_tags) noexcept {
    using TF = exporters::korg::quality::TimbreFamily;
    switch (fam) {
        case TF::SynthPad:
        case TF::Strings:
        case TF::ChoirPad:
        case TF::Organ:
            return 0.92;
        case TF::Accordion:
        case TF::Brass:
        case TF::SaxReed:
        case TF::FlutePipe:
            return 0.72;
        case TF::SynthLead:
            return containsAny(lower_tags, {"pad", "warm"}) ? 0.80 : 0.65;
        case TF::ElectricPiano:
            return containsAny(lower_tags, {"clav"}) ? 0.25 : 0.55;
        case TF::Piano:
            return containsAny(lower_tags, {"harpsichord"}) ? 0.22 : 0.50;
        case TF::AcousticGuitar:
        case TF::ElectricGuitar:
            return containsAny(lower_tags, {"distortion", "overdrive"}) ? 0.68 : 0.35;
        case TF::PluckedStrings:
        case TF::MalletPerc:
        case TF::EthnicMelodic:
            return 0.30;
        case TF::AcousticBass:
        case TF::ElectricBass:
        case TF::SynthBass:
            return containsAny(lower_tags, {"slap", "pick"}) ? 0.38 : 0.52;
        case TF::DrumKit:
        case TF::PercussionKit:
            return 0.08;
        case TF::Unknown:
            return 0.50;
    }
    return 0.50;
}

// Natural pitch range for a candidate timbre family.
inline RolePitchRange candidateNaturalRange(exporters::korg::quality::TimbreFamily fam,
                                            const TargetCapabilityProfile& profile) noexcept {
    using TF = exporters::korg::quality::TimbreFamily;
    switch (fam) {
        case TF::DrumKit:        return profile.pitchRangeFor(RoleRegisterCategory::Drum);
        case TF::PercussionKit:  return profile.pitchRangeFor(RoleRegisterCategory::Percussion);
        case TF::AcousticBass:
        case TF::ElectricBass:
        case TF::SynthBass:      return profile.pitchRangeFor(RoleRegisterCategory::Bass);
        case TF::Piano:          return profile.pitchRangeFor(RoleRegisterCategory::Piano);
        case TF::ElectricPiano:
        case TF::Organ:
        case TF::Accordion:      return profile.pitchRangeFor(RoleRegisterCategory::HarmonicComp);
        case TF::AcousticGuitar:
        case TF::ElectricGuitar:
        case TF::PluckedStrings: return profile.pitchRangeFor(RoleRegisterCategory::Guitar);
        case TF::Strings:
        case TF::ChoirPad:
        case TF::SynthPad:       return profile.pitchRangeFor(RoleRegisterCategory::Pad);
        case TF::Brass:          return profile.pitchRangeFor(RoleRegisterCategory::Brass);
        case TF::SaxReed:
        case TF::FlutePipe:
        case TF::SynthLead:      return profile.pitchRangeFor(RoleRegisterCategory::Lead);
        case TF::MalletPerc:
        case TF::EthnicMelodic:  return profile.pitchRangeFor(RoleRegisterCategory::Orchestral);
        case TF::Unknown:        return RolePitchRange{21, 108, 36, 84};
    }
    return RolePitchRange{21, 108, 36, 84};
}

// Factor 4: Register suitability comparing track actual pitch span vs candidate natural range.
inline double scoreRegisterSuitability(const TrackFeatureSummary& feat,
                                       const RolePitchRange& range) noexcept {
    if (feat.note_count == 0u) return 0.70;
    double score = 1.0;
    if (feat.pitch_median < range.min_pitch) {
        score -= std::min(0.65, static_cast<double>(range.min_pitch - feat.pitch_median) * 0.045);
    } else if (feat.pitch_median > range.max_pitch) {
        score -= std::min(0.65, static_cast<double>(feat.pitch_median - range.max_pitch) * 0.045);
    } else if (feat.pitch_median < range.ideal_low) {
        score -= std::min(0.20, static_cast<double>(range.ideal_low - feat.pitch_median) * 0.015);
    } else if (feat.pitch_median > range.ideal_high) {
        score -= std::min(0.20, static_cast<double>(feat.pitch_median - range.ideal_high) * 0.015);
    }
    if (feat.pitch_min < range.min_pitch) {
        score -= std::min(0.20, static_cast<double>(range.min_pitch - feat.pitch_min) * 0.015);
    }
    if (feat.pitch_max > range.max_pitch) {
        score -= std::min(0.20, static_cast<double>(feat.pitch_max - range.max_pitch) * 0.015);
    }
    return clamp01(score);
}

// Factor 5: Dynamic response fit (track velocity variance & ghost tail vs instrument expressiveness).
inline double scoreDynamicResponse(const TrackFeatureSummary& feat,
                                   exporters::korg::quality::TimbreFamily fam,
                                   bool is_dnc) noexcept {
    using TF = exporters::korg::quality::TimbreFamily;
    if (feat.note_count == 0u) return 0.75;
    const bool high_dynamics = (feat.vel_stddev >= 14.0 || feat.velocity_dynamic_range >= 45u || feat.ghost_note_ratio >= 0.15);
    const bool flat_organ_like = (feat.vel_stddev <= 5.0 && feat.velocity_dynamic_range <= 16u);

    double score = 0.78;
    if (fam == TF::Organ || fam == TF::SynthPad || fam == TF::ChoirPad) {
        score = flat_organ_like ? 0.94 : (high_dynamics ? 0.65 : 0.82);
    } else if (fam == TF::Piano || fam == TF::ElectricPiano || fam == TF::AcousticGuitar ||
               fam == TF::ElectricGuitar || fam == TF::DrumKit || fam == TF::PercussionKit) {
        score = high_dynamics ? (is_dnc ? 0.96 : 0.88) : 0.80;
    } else if (fam == TF::Brass || fam == TF::Strings || fam == TF::SaxReed) {
        score = (high_dynamics || feat.has_expression_cc11) ? (is_dnc ? 0.94 : 0.86) : 0.78;
    }
    return clamp01(score);
}

// Factor 3: Articulation compatibility (MegaVoice / DNC / strum / mute / slide / legato).
inline double scoreArticulationCompatibility(
    const CanonicalTrackView& tr,
    bool candidate_is_dnc,
    std::string_view lower_tags,
    const TargetCapabilityProfile& profile) noexcept {
    const auto& f = tr.features;
    double score = 0.78;

    const bool track_needs_expressive_art =
        f.has_strum_offsets || f.has_pitch_bend || f.ghost_note_ratio >= 0.18 ||
        (f.legato_ratio >= 0.60 && f.has_expression_cc11);

    if (track_needs_expressive_art) {
        if (profile.articulation.supports_dnc && candidate_is_dnc) {
            score += 0.18;
        } else if (profile.articulation.supports_super_articulation ||
                   profile.articulation.supports_guitar_mode) {
            score += 0.10;
        } else {
            score -= 0.12;
        }
    }
    if (f.has_strum_offsets) {
        if (containsAny(lower_tags, {"strum", "steel", "nylon", "clean", "strat", "guitar"})) {
            score += 0.14;
        } else if (containsAny(lower_tags, {"pad", "choir", "brass", "organ"})) {
            score -= 0.30;
        }
    }
    if (f.staccato_ratio >= 0.55 && containsAny(lower_tags, {"muted", "clav", "pick", "slap", "brass", "drum", "perc"})) {
        score += 0.10;
    }
    if (f.legato_ratio >= 0.65 && containsAny(lower_tags, {"sustained", "warm", "ensemble", "pad", "choir", "sax", "flute"})) {
        score += 0.10;
    }
    return clamp01(score);
}

// Factor 6: Ensemble function compatibility (P5-M2 MusicalRole -> candidate timbre/tags).
// Returns a score in [0.0, 1.0] AND a signed delta in [-0.25, +0.20].
inline std::pair<double, double> scoreEnsembleFunctionRole(
    MusicalRole role,
    exporters::korg::quality::TimbreFamily cand_fam,
    std::string_view lower_tags,
    const exporters::korg::quality::TrackTimbreProfile& prof) noexcept {
    using MR = MusicalRole;
    using TF = exporters::korg::quality::TimbreFamily;

    if (role == MR::UnknownAbstain || role == MR::MixedRole) {
        return {0.70, 0.0};
    }

    double s = 0.65;
    double delta = 0.0;

    switch (role) {
        case MR::KickSnareFoundation:
            if (cand_fam == TF::DrumKit) { s = 0.96; delta = +0.12; }
            else if (cand_fam == TF::PercussionKit) { s = 0.45; delta = -0.14; }
            else { s = 0.0; delta = -0.35; }
            break;

        case MR::DrumKitAuxPercussion:
            if (cand_fam == TF::PercussionKit) {
                // Prevent RockPerc / standard perc from wrongly choosing SFX or Ethnic unless signaled!
                const std::string sub = toLowerAscii(prof.inferred.sub_family);
                const bool wants_sfx = containsAny(sub, {"sfx", "fx", "effect"});
                const bool wants_ethnic = containsAny(sub, {"ethnic", "oriental", "arabic", "tabla", "darbuka"});
                const bool cand_sfx = containsAny(lower_tags, {"sfx", "effects"});
                const bool cand_ethnic = containsAny(lower_tags, {"ethnic", "oriental", "darbuka", "tabla"});
                if (cand_sfx && !wants_sfx) {
                    s = 0.30; delta = -0.18;
                } else if (cand_ethnic && !wants_ethnic) {
                    s = 0.35; delta = -0.16;
                } else if (!cand_sfx && !cand_ethnic) {
                    s = 0.94; delta = +0.14;
                } else {
                    s = 0.92; delta = +0.12;
                }
            } else if (cand_fam == TF::DrumKit) {
                s = 0.68; delta = -0.02;
            } else {
                s = 0.0; delta = -0.35;
            }
            break;

        case MR::BassFoundation:
            if (cand_fam == TF::AcousticBass || cand_fam == TF::ElectricBass || cand_fam == TF::SynthBass) {
                s = 0.95; delta = +0.12;
            } else {
                s = 0.20; delta = -0.22;
            }
            break;

        case MR::GuitarStrumming:
            if (cand_fam == TF::AcousticGuitar || cand_fam == TF::ElectricGuitar) {
                s = containsAny(lower_tags, {"steel", "strum", "clean", "strat", "nylon", "overdrive", "distortion"})
                        ? 0.96 : 0.88;
                delta = +0.15;
            } else if (cand_fam == TF::PluckedStrings) {
                s = 0.65; delta = 0.0;
            } else {
                s = 0.25; delta = -0.18;
            }
            break;

        case MR::Arpeggio:
            if (cand_fam == TF::AcousticGuitar || cand_fam == TF::ElectricGuitar ||
                cand_fam == TF::PluckedStrings || cand_fam == TF::Piano ||
                cand_fam == TF::ElectricPiano || cand_fam == TF::SynthLead) {
                s = 0.90; delta = +0.10;
            } else {
                s = 0.45; delta = -0.08;
            }
            break;

        case MR::PianoAccompaniment:
            if (cand_fam == TF::Piano || cand_fam == TF::ElectricPiano) {
                s = 0.95; delta = +0.12;
            } else if (cand_fam == TF::Organ || cand_fam == TF::Accordion) {
                s = 0.68; delta = -0.02;
            } else {
                s = 0.30; delta = -0.15;
            }
            break;

        case MR::PadSustainedHarmony:
            if (cand_fam == TF::SynthPad || cand_fam == TF::Strings || cand_fam == TF::ChoirPad) {
                s = 0.96; delta = +0.15;
            } else if (cand_fam == TF::Organ) {
                s = 0.78; delta = +0.04;
            } else if (cand_fam == TF::SynthLead || cand_fam == TF::Brass) {
                // Explicitly prevent a brassy synth lead from colliding with a Pad role!
                s = 0.20; delta = -0.22;
            } else {
                s = 0.40; delta = -0.10;
            }
            break;

        case MR::ChordComping:
            if (cand_fam == TF::ElectricPiano || cand_fam == TF::Piano ||
                cand_fam == TF::ElectricGuitar || cand_fam == TF::AcousticGuitar ||
                cand_fam == TF::Organ || cand_fam == TF::Brass) {
                s = 0.90; delta = +0.10;
            } else if (cand_fam == TF::SynthPad) {
                s = 0.45; delta = -0.10;
            } else {
                s = 0.60; delta = -0.02;
            }
            break;

        case MR::HarmonicAccompaniment:
            if (cand_fam == TF::Piano || cand_fam == TF::ElectricPiano || cand_fam == TF::Organ ||
                cand_fam == TF::Accordion || cand_fam == TF::Strings || cand_fam == TF::AcousticGuitar ||
                cand_fam == TF::ElectricGuitar || cand_fam == TF::SynthPad) {
                s = 0.88; delta = +0.08;
            } else {
                s = 0.55; delta = -0.05;
            }
            break;

        case MR::BrassAccents:
            if (cand_fam == TF::Brass) {
                s = 0.96; delta = +0.15;
            } else if (cand_fam == TF::SaxReed || cand_fam == TF::SynthLead) {
                s = 0.72; delta = +0.02;
            } else if (cand_fam == TF::SynthPad) {
                s = 0.25; delta = -0.18;
            } else {
                s = 0.45; delta = -0.10;
            }
            break;

        case MR::OrchestralAccents:
            if (cand_fam == TF::Strings || cand_fam == TF::Brass ||
                cand_fam == TF::MalletPerc || cand_fam == TF::ChoirPad ||
                cand_fam == TF::PluckedStrings) {
                s = 0.92; delta = +0.12;
            } else {
                s = 0.45; delta = -0.08;
            }
            break;

        case MR::Countermelody:
        case MR::MelodicPhrase:
            if (cand_fam == TF::SaxReed || cand_fam == TF::FlutePipe || cand_fam == TF::SynthLead ||
                cand_fam == TF::Brass || cand_fam == TF::Strings || cand_fam == TF::ElectricGuitar ||
                cand_fam == TF::AcousticGuitar || cand_fam == TF::Accordion || cand_fam == TF::Piano) {
                s = 0.90; delta = +0.10;
            } else if (cand_fam == TF::SynthPad) {
                s = 0.40; delta = -0.12;
            } else {
                s = 0.65; delta = 0.0;
            }
            break;

        case MR::TransitionalEffects:
            if (cand_fam == TF::PercussionKit || cand_fam == TF::SynthPad || cand_fam == TF::SynthLead ||
                cand_fam == TF::MalletPerc || cand_fam == TF::Strings) {
                s = 0.85; delta = +0.06;
            } else {
                s = 0.60; delta = 0.0;
            }
            break;

        default:
            break;
    }

    return {clamp01(s), clampRange(delta, -0.25, +0.20)};
}

// Factor 8 + Genre feel suitability (also informs drum kit category match!).
inline std::pair<double, double> scoreGenreAndDrumSemantics(
    GenreFeelClass genre,
    const GroovePocketProfile& groove,
    exporters::korg::quality::TimbreFamily cand_fam,
    std::string_view lower_tags,
    bool is_drum_track,
    const exporters::korg::quality::TrackTimbreProfile& prof) noexcept {
    using GF = GenreFeelClass;
    using TF = exporters::korg::quality::TimbreFamily;

    double drum_sem = is_drum_track ? 0.72 : 1.0;
    double genre_delta = 0.0;

    if (is_drum_track) {
        const std::string sub = toLowerAscii(prof.inferred.sub_family);
        const bool wants_sfx = containsAny(sub, {"sfx", "fx", "effect"});
        const bool wants_ethnic = containsAny(sub, {"ethnic", "oriental", "arabic", "tabla"});
        if (containsAny(lower_tags, {"sfx", "effects"}) && !wants_sfx) {
            drum_sem = 0.25;
            genre_delta -= 0.14;
        } else if (containsAny(lower_tags, {"ethnic", "oriental", "tabla", "darbuka"}) && !wants_ethnic) {
            drum_sem = 0.30;
            genre_delta -= 0.12;
        }
    }

    if (genre == GF::UnknownAbstain) {
        return {clamp01(drum_sem), clampRange(genre_delta, -0.15, +0.15)};
    }

    switch (genre) {
        case GF::RockDrive:
            if (is_drum_track && cand_fam == TF::DrumKit) {
                if (containsAny(lower_tags, {"rock", "power", "stadium"})) { drum_sem = 0.96; genre_delta += 0.10; }
                else if (containsAny(lower_tags, {"brush", "jazz", "electronic"})) { drum_sem = 0.48; genre_delta -= 0.08; }
                else { drum_sem = 0.80; }
            } else if (is_drum_track && cand_fam == TF::PercussionKit) {
                if (containsAny(lower_tags, {"standard", "latin", "shaker"})) { drum_sem = 0.90; genre_delta += 0.08; }
            } else if (cand_fam == TF::ElectricGuitar && containsAny(lower_tags, {"distortion", "overdrive", "rock"})) {
                genre_delta += 0.10;
            } else if (cand_fam == TF::ElectricBass && containsAny(lower_tags, {"pick", "rock"})) {
                genre_delta += 0.08;
            }
            break;

        case GF::JazzSwing:
            if (is_drum_track && cand_fam == TF::DrumKit) {
                if (containsAny(lower_tags, {"jazz", "brush", "swing"})) { drum_sem = 0.96; genre_delta += 0.12; }
                else if (containsAny(lower_tags, {"electronic", "808", "rock"})) { drum_sem = 0.38; genre_delta -= 0.10; }
            } else if (cand_fam == TF::AcousticBass) {
                genre_delta += 0.10;
            } else if (cand_fam == TF::ElectricGuitar && containsAny(lower_tags, {"jazz", "hollow"})) {
                genre_delta += 0.09;
            } else if (cand_fam == TF::Brass || cand_fam == TF::SaxReed || cand_fam == TF::Piano) {
                genre_delta += 0.05;
            }
            break;

        case GF::EdmElectronic:
            if (is_drum_track && cand_fam == TF::DrumKit) {
                if (containsAny(lower_tags, {"electronic", "analog", "808", "909", "edm", "dance"})) {
                    drum_sem = 0.97; genre_delta += 0.12;
                } else if (containsAny(lower_tags, {"brush", "orchestra"})) {
                    drum_sem = 0.35; genre_delta -= 0.10;
                }
            } else if (cand_fam == TF::SynthBass || cand_fam == TF::SynthPad || cand_fam == TF::SynthLead) {
                genre_delta += 0.09;
            }
            break;

        case GF::FunkSyncopated:
            if (is_drum_track && cand_fam == TF::DrumKit) {
                if (containsAny(lower_tags, {"pop", "studio", "standard"})) { drum_sem = 0.90; genre_delta += 0.06; }
            } else if (cand_fam == TF::ElectricBass && containsAny(lower_tags, {"slap", "funk"})) {
                genre_delta += 0.11;
            } else if (cand_fam == TF::ElectricPiano && containsAny(lower_tags, {"clav", "rhodes", "stage", "wurlitzer"})) {
                genre_delta += 0.09;
            } else if (cand_fam == TF::ElectricGuitar && containsAny(lower_tags, {"clean", "strat", "muted"})) {
                genre_delta += 0.08;
            }
            break;

        case GF::LatinBossaCha:
            if (is_drum_track && cand_fam == TF::PercussionKit) {
                if (containsAny(lower_tags, {"latin", "conga", "bongo", "shaker", "standard"})) {
                    drum_sem = 0.96; genre_delta += 0.11;
                }
            } else if (cand_fam == TF::AcousticGuitar && containsAny(lower_tags, {"nylon", "classic"})) {
                genre_delta += 0.10;
            }
            break;

        case GF::OrchestralCinematic:
            if (is_drum_track && cand_fam == TF::DrumKit) {
                if (containsAny(lower_tags, {"orchestra", "symphon"})) { drum_sem = 0.96; genre_delta += 0.12; }
            } else if (cand_fam == TF::Strings || cand_fam == TF::ChoirPad ||
                       cand_fam == TF::PluckedStrings || cand_fam == TF::MalletPerc) {
                genre_delta += 0.09;
            }
            break;

        case GF::PopBallad:
        case GF::AcousticFolkWaltz:
        case GF::VietnameseBoleroSlowRock:
            if (is_drum_track && cand_fam == TF::DrumKit) {
                if (containsAny(lower_tags, {"pop", "standard", "acoustic", "studio"})) {
                    drum_sem = 0.90; genre_delta += 0.06;
                }
                if (groove.estimated_swing_ratio >= 0.58 && containsAny(lower_tags, {"brush", "jazz"})) {
                    drum_sem = 0.92; genre_delta += 0.07;
                }
            } else if (cand_fam == TF::AcousticGuitar || cand_fam == TF::Piano ||
                       cand_fam == TF::ElectricPiano || cand_fam == TF::Strings ||
                       cand_fam == TF::SynthPad || cand_fam == TF::ElectricBass) {
                genre_delta += 0.05;
            }
            break;

        case GF::UnknownAbstain:
            break;
    }

    return {clamp01(drum_sem), clampRange(genre_delta, -0.15, +0.15)};
}

// Factor 9: Target capability fit across all 5 hardware arranger platforms.
inline double scoreTargetCapabilityFit(
    const exporters::korg::quality::SoundCandidate& cand,
    bool cand_is_dnc,
    bool is_drum_track,
    const TargetCapabilityProfile& profile) noexcept {
    double score = 0.85;
    switch (profile.id) {
        case TargetPlatformId::KorgPa5X:
            score = 0.92 + (cand_is_dnc ? 0.06 : 0.0);
            break;
        case TargetPlatformId::KorgPa4XPa1000:
            // Legacy Pa4X/Pa1000 has DNC and GM2 alignment; slightly lower polyphony/user-bank capacity
            score = 0.86 + (cand_is_dnc ? 0.04 : 0.0);
            break;
        case TargetPlatformId::YamahaGenos2:
            // Genos2 uses S.Art2/MegaVoice rather than Korg DNC; GM2-aligned sounds remain portable
            score = cand_is_dnc ? 0.74 : 0.86;
            break;
        case TargetPlatformId::KetronEvent:
            // Ketron favors Real Audio Drums & Live Guitar; MIDI fallback works for all melodic tracks
            score = is_drum_track ? 0.82 : (cand_is_dnc ? 0.76 : 0.85);
            break;
        case TargetPlatformId::RolandEA7BK:
            // Roland E-A7/BK-9 uses GM2 MSB 121/120 compatible mapping without Korg DNC
            score = cand_is_dnc ? 0.72 : 0.86;
            break;
    }
    if (cand.origin == exporters::korg::quality::MatchOrigin::TargetKstUserSound) {
        if (profile.id == TargetPlatformId::KorgPa5X || profile.id == TargetPlatformId::KorgPa4XPa1000) {
            score = std::min(1.0, score + 0.05);
        } else {
            score = std::max(0.50, score - 0.15);
        }
    }
    return clamp01(score);
}

inline exporters::korg::quality::TimbreFamily familyForCandidate(
    const exporters::korg::quality::SoundCandidate& cand,
    const exporters::korg::quality::TrackTimbreProfile& fallback_prof) noexcept {
    using namespace exporters::korg::quality;
    if (cand.origin == MatchOrigin::Pa5xFactoryCatalog) {
        for (const auto& fe : pa5xFactoryCatalog()) {
            if (fe.ref.msb == cand.ref.msb && fe.ref.lsb == cand.ref.lsb && fe.ref.pc == cand.ref.pc) {
                return fe.family;
            }
        }
    }
    const std::string low = toLowerAscii(cand.display_name + " " + cand.rationale);
    if (cand.target_slot.has_value() && cand.target_slot->is_drum_kit) {
        return containsAny(low, {"perc", "conga", "shaker", "latin"}) ? TimbreFamily::PercussionKit : TimbreFamily::DrumKit;
    }
    if (containsAny(low, {"drum kit", "drumkit"})) return TimbreFamily::DrumKit;
    if (containsAny(low, {"perc kit", "percussion"})) return TimbreFamily::PercussionKit;
    if (containsAny(low, {"synth bass"})) return TimbreFamily::SynthBass;
    if (containsAny(low, {"acoustic bass", "upright"})) return TimbreFamily::AcousticBass;
    if (containsAny(low, {"bass"})) return TimbreFamily::ElectricBass;
    if (containsAny(low, {"rhodes", "wurlitzer", "clav", "electric piano", "stage", "dx"})) return TimbreFamily::ElectricPiano;
    if (containsAny(low, {"piano", "grand", "harpsichord"})) return TimbreFamily::Piano;
    if (containsAny(low, {"organ"})) return TimbreFamily::Organ;
    if (containsAny(low, {"accordion", "harmonica", "bandoneon"})) return TimbreFamily::Accordion;
    if (containsAny(low, {"nylon", "steel", "classic gtr", "acoustic guitar"})) return TimbreFamily::AcousticGuitar;
    if (containsAny(low, {"strat", "distortion", "overdrive", "guitar"})) return TimbreFamily::ElectricGuitar;
    if (containsAny(low, {"harp"})) return TimbreFamily::PluckedStrings;
    if (containsAny(low, {"string", "violin", "cello"})) return TimbreFamily::Strings;
    if (containsAny(low, {"choir", "vocal"})) return TimbreFamily::ChoirPad;
    if (containsAny(low, {"pad"})) return TimbreFamily::SynthPad;
    if (containsAny(low, {"brass", "trumpet", "trombone", "horn"})) return TimbreFamily::Brass;
    if (containsAny(low, {"sax", "clarinet", "oboe"})) return TimbreFamily::SaxReed;
    if (containsAny(low, {"flute"})) return TimbreFamily::FlutePipe;
    if (containsAny(low, {"lead", "mini"})) return TimbreFamily::SynthLead;
    if (containsAny(low, {"vibe", "marimba", "timpani", "bell"})) return TimbreFamily::MalletPerc;
    if (containsAny(low, {"sitar", "ethnic"})) return TimbreFamily::EthnicMelodic;
    return fallback_prof.inferred.family;
}

inline bool candidateIsDnc(const exporters::korg::quality::SoundCandidate& cand) noexcept {
    using namespace exporters::korg::quality;
    if (cand.origin == MatchOrigin::Pa5xFactoryCatalog) {
        for (const auto& fe : pa5xFactoryCatalog()) {
            if (fe.ref.msb == cand.ref.msb && fe.ref.lsb == cand.ref.lsb && fe.ref.pc == cand.ref.pc) {
                return fe.is_dnc;
            }
        }
    }
    const std::string low = toLowerAscii(cand.display_name);
    return containsAny(low, {"dnc", "dn1", " rr"});
}

inline std::string candidateTagsLower(const exporters::korg::quality::SoundCandidate& cand) {
    using namespace exporters::korg::quality;
    std::string combined = cand.display_name + " " + cand.rationale;
    if (cand.origin == MatchOrigin::Pa5xFactoryCatalog) {
        for (const auto& fe : pa5xFactoryCatalog()) {
            if (fe.ref.msb == cand.ref.msb && fe.ref.lsb == cand.ref.lsb && fe.ref.pc == cand.ref.pc) {
                combined += " ";
                combined += fe.sub_tags;
                break;
            }
        }
    }
    return toLowerAscii(combined);
}

// Reconstruct consolidated TrackTimbreProfile + representative CanonicalTrackView per channel.
inline CanonicalTrackView consolidateTrackForChannel(
    const CanonicalStyleView& view,
    uint8_t src_ch) {
    CanonicalTrackView best{};
    bool found = false;
    uint32_t max_notes = 0;
    for (const auto& sec : view.sections) {
        for (const auto& tr : sec.tracks) {
            if (tr.source_channel != src_ch) continue;
            if (!found || tr.features.note_count > max_notes) {
                best = tr;
                max_notes = tr.features.note_count;
                found = true;
            }
        }
    }
    return best;
}

inline std::string buildKnownLimitations(
    const exporters::korg::quality::SoundCandidate& cand,
    const TargetCapabilityProfile& profile,
    bool is_dnc) {
    std::string lim = "hardwareVerified=false; L3=NOT_TESTED (requires physical ear verification).";
    if (cand.origin == exporters::korg::quality::MatchOrigin::Pa5xFactoryCatalog) {
        lim += " Factory catalog MSB/LSB/PC follows MAN5 documentation (K8/K9 0-based GM2 caveat).";
    }
    if (profile.id != TargetPlatformId::KorgPa5X && profile.id != TargetPlatformId::KorgPa4XPa1000) {
        lim += " Catalog addresses are Pa5X/GM2-aligned; map to native " + profile.display_name + " bank table before export.";
    }
    if (is_dnc && !profile.articulation.supports_dnc) {
        lim += " Target platform lacks Korg DNC articulation engine.";
    }
    return lim;
}

} // namespace context_sound_detail

// Wraps pa5xFactoryCatalog() + TargetSoundCatalogView without duplicating catalogs;
// user_overrides ALWAYS take unconditional precedence (MatchOrigin::UserSoundMap).
inline ContextSoundMatchResult matchSoundsInContext(
    const SemanticArrangementAnalysis& semantics,
    const TargetCapabilityProfile& target_profile,
    const std::map<uint8_t, exporters::korg::SoundRef>& user_overrides = {},
    const exporters::korg::quality::TargetSoundCatalogView* target_catalog = nullptr,
    const ContextSoundMatchOptions& opt = {}) {
    using namespace exporters::korg::quality;
    using namespace context_sound_detail;

    ContextSoundMatchResult result{};
    result.target_platform = target_profile.id;
    result.auto_select_enabled = (opt.enabled || opt.enable_auto_select);
    result.hardware_verified = false;
    result.l3_status = kL3StatusNotTested;

    const size_t clamped_top_k = std::clamp<size_t>(opt.top_k == 0 ? 3u : opt.top_k, 1u, 5u);
    const double min_conf_floor = clampRange(opt.min_auto_select_confidence, 0.50, 0.99);

    // Filter target catalog strictly through TargetCapabilityProfile addressing rules (0 OOS leaks!)
    TargetSoundCatalogView filtered_catalog{};
    const TargetSoundCatalogView* safe_catalog_ptr = nullptr;
    if (target_catalog != nullptr) {
        filtered_catalog.is_partial_inspect = target_catalog->is_partial_inspect;
        filtered_catalog.partial_notice     = target_catalog->partial_notice;
        for (const auto& entry : target_catalog->entries) {
            if (isTargetEntryValidForProfile(entry, target_profile)) {
                filtered_catalog.entries.push_back(entry);
            } else {
                ++result.oos_target_entries_rejected;
            }
        }
        safe_catalog_ptr = &filtered_catalog;
    }

    // Build role lookup and register-overlap lookup from P5-M2 semantics.
    std::map<uint8_t, MusicalRole> role_by_src_ch;
    for (const auto& trd : semantics.track_roles) {
        role_by_src_ch[trd.source_channel] = opt.use_semantic_roles
                                                 ? trd.role
                                                 : MusicalRole::UnknownAbstain;
    }

    // Track already-chosen sound identities & bright timbres for Factor 10 cross-track masking.
    struct ChosenEnsembleSlot {
        uint8_t src_ch = 0;
        uint8_t dst_ch = 0xFF;
        uint8_t pitch_min = 0;
        uint8_t pitch_max = 127;
        TimbreFamily family = TimbreFamily::Unknown;
        exporters::korg::SoundRef ref{};
        bool is_bright = false;
    };
    std::vector<ChosenEnsembleSlot> chosen_ensemble;

    // Collect ordered channels from semantics.track_roles (fallback to canonical_view sections if empty).
    std::vector<uint8_t> ordered_src_channels;
    std::set<uint8_t> seen_src;
    for (const auto& trd : semantics.track_roles) {
        if (seen_src.insert(trd.source_channel).second) {
            ordered_src_channels.push_back(trd.source_channel);
        }
    }
    for (const auto& sec : semantics.canonical_view.sections) {
        for (const auto& tr : sec.tracks) {
            if (seen_src.insert(tr.source_channel).second) {
                ordered_src_channels.push_back(tr.source_channel);
            }
        }
    }

    // Deterministic ordering: Drum/Perc/Bass first so harmonic comping tracks can check masking against them.
    auto channelPriority = [&](uint8_t sc) -> int {
        const auto tr = consolidateTrackForChannel(semantics.canonical_view, sc);
        if (tr.is_drum || tr.target_channel == exporters::korg::kDrumCh) return 0;
        if (tr.target_channel == exporters::korg::kPercCh) return 1;
        if (tr.target_channel == exporters::korg::kBassCh) return 2;
        return 3 + static_cast<int>(sc);
    };
    std::sort(ordered_src_channels.begin(), ordered_src_channels.end(),
              [&](uint8_t a, uint8_t b) {
                  const int pa = channelPriority(a);
                  const int pb = channelPriority(b);
                  return (pa != pb) ? (pa < pb) : (a < b);
              });

    for (uint8_t src_ch : ordered_src_channels) {
        CanonicalTrackView tr = consolidateTrackForChannel(semantics.canonical_view, src_ch);
        if (tr.target_channel == 0xFF) {
            for (const auto& trd : semantics.track_roles) {
                if (trd.source_channel == src_ch) {
                    tr.target_channel = trd.target_channel;
                    if (tr.name.empty()) tr.name = trd.track_name;
                    break;
                }
            }
        }

        const MusicalRole sem_role = role_by_src_ch.count(src_ch)
                                         ? role_by_src_ch[src_ch]
                                         : MusicalRole::UnknownAbstain;

        // Ensure TrackTimbreProfile is populated even if caller built semantics from bare UASF.
        TrackTimbreProfile timbre_prof = tr.timbre;
        if (timbre_prof.inferred.family == TimbreFamily::Unknown && timbre_prof.confidence == 0) {
            timbre_prof = profileTrackTimbre(
                tr.sound_identity.msb, tr.sound_identity.lsb, tr.sound_identity.pc,
                tr.name, tr.uasf_role, {}, {}, src_ch, tr.target_channel);
        } else {
            timbre_prof.src_ch = src_ch;
            if (timbre_prof.korg_ch == 0xFF) timbre_prof.korg_ch = tr.target_channel;
        }

        const bool track_is_drum_domain =
            tr.is_drum || tr.sound_identity.is_drum_kit ||
            isPercussiveTimbreFamily(timbre_prof.inferred.family) ||
            isPercussiveRole(sem_role);

        ContextTrackSoundDecision dec{};
        dec.source_channel = src_ch;
        dec.target_channel = tr.target_channel;
        dec.track_name     = tr.name;
        dec.semantic_role  = sem_role;

        // ── 1. Unconditional Precedence: UserSoundMap override (`USER_VERIFIED`) ──
        const auto user_it = (tr.target_channel != 0xFF)
                                 ? user_overrides.find(tr.target_channel)
                                 : user_overrides.end();
        if (user_it != user_overrides.end()) {
            RankedSoundOption uopt{};
            uopt.base_candidate.ref               = user_it->second;
            uopt.base_candidate.display_name      = "User Sound Map";
            uopt.base_candidate.origin            = MatchOrigin::UserSoundMap;
            uopt.base_candidate.score             = 100;
            uopt.base_candidate.epistemic_tag     = kEpistemicUserVerified;
            uopt.base_candidate.hardware_verified = false;
            uopt.base_candidate.l3_status         = "USER_SUPPLIED";
            uopt.base_candidate.rationale         = "explicit --sound-map override (unconditional precedence)";
            uopt.factors = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
            uopt.base_score_01            = 1.0;
            uopt.final_confidence         = 1.0;
            uopt.provenance_class         = ContextMatchProvenance::DeterministicVerified;
            uopt.match_rationale          = "UserSoundMap override preserved unconditionally (USER_VERIFIED).";
            uopt.known_limitations        = "User-supplied slot; verify on target hardware if not previously audited.";
            uopt.estimated_musical_impact = "Preserves exact user-selected sound without automated mutation.";
            uopt.fallback_behavior        = "Not applicable (user override is authoritative).";
            uopt.provenance.origin        = DecisionOrigin::UserOverride;
            uopt.provenance.component_id  = "p5_context_sound_v1";
            uopt.provenance.confidence    = 1.0;
            uopt.provenance.abstained     = false;
            uopt.provenance.reversible    = true;
            uopt.provenance.rationale     = uopt.match_rationale;

            dec.ranked_options.push_back(std::move(uopt));
            dec.selected            = user_it->second;
            dec.selected_origin     = MatchOrigin::UserSoundMap;
            dec.selected_provenance = ContextMatchProvenance::DeterministicVerified;
            dec.epistemic_tag       = kEpistemicUserVerified;
            dec.user_override_locked = true;
            dec.fell_back_unresolved = false;
            dec.fallback_behavior    = "User override locked; never overridden by advisory matcher.";
            dec.rationale            = "USER_VERIFIED override preserved unconditionally.";

            result.resolved_sounds[tr.target_channel] = user_it->second;
            ++result.user_overrides_preserved;

            chosen_ensemble.push_back(ChosenEnsembleSlot{
                src_ch, tr.target_channel, tr.features.pitch_min, tr.features.pitch_max,
                timbre_prof.inferred.family, user_it->second, false
            });
            result.tracks.push_back(std::move(dec));
            continue;
        }

        // ── 2. Fail-Closed Unknown/Private Bank Gate (EXP15 Preservation!) ──
        const bool unknown_low_conf_bank =
            (timbre_prof.inferred.family == TimbreFamily::Unknown) ||
            (timbre_prof.confidence < opt.min_timbre_profile_confidence);

        // Query baseline matcher for full candidate pool (never duplicating pa5xFactoryCatalog!)
        SoundMatchOptions base_opt{};
        base_opt.enable_factory_catalog    = true;
        base_opt.prefer_target_user_sounds = opt.prefer_target_user_sounds;
        base_opt.min_auto_select_score     = 0; // retrieve full pool for 10-factor reranking
        base_opt.max_candidates            = 24;

        const TrackSoundMatch base_match = matchTrackSound(
            timbre_prof, {}, safe_catalog_ptr, base_opt);

        std::vector<RankedSoundOption> ranked;
        ranked.reserve(base_match.top_candidates.size());

        for (const auto& base_cand : base_match.top_candidates) {
            const TimbreFamily cand_fam = familyForCandidate(base_cand, timbre_prof);
            const bool cand_is_drum = isPercussiveTimbreFamily(cand_fam) ||
                                      (base_cand.target_slot.has_value() && base_cand.target_slot->is_drum_kit) ||
                                      target_profile.sound_addressing.isDrumBank(base_cand.ref.msb, base_cand.ref.lsb);

            // Hard Invariant: ZERO melodic <-> drum cross-family contamination!
            if (cand_is_drum != track_is_drum_domain) {
                continue;
            }

            const bool is_dnc = candidateIsDnc(base_cand);
            const std::string low_tags = candidateTagsLower(base_cand);

            ContextFactorBreakdown fb{};
            // Factor 1: Instrument family
            if (cand_fam == timbre_prof.inferred.family && cand_fam != TimbreFamily::Unknown) {
                fb.instrument_family_score = 1.0;
            } else if (track_is_drum_domain && cand_is_drum) {
                fb.instrument_family_score = 0.65;
            } else if (!track_is_drum_domain && !cand_is_drum) {
                fb.instrument_family_score = 0.55;
            } else {
                fb.instrument_family_score = 0.0;
            }

            // Factor 2: Timbral characteristics (from base_score_01)
            const double base01 = clamp01(static_cast<double>(base_cand.score) / 100.0);
            fb.timbral_characteristics_score = base01;

            // Factor 3: Articulation compatibility
            fb.articulation_compatibility = scoreArticulationCompatibility(tr, is_dnc, low_tags, target_profile);

            // Factor 4: Register suitability
            fb.register_suitability = scoreRegisterSuitability(
                tr.features, candidateNaturalRange(cand_fam, target_profile));

            // Factor 5: Dynamic response
            fb.dynamic_response = scoreDynamicResponse(tr.features, cand_fam, is_dnc);

            // Factor 6: Ensemble function (P5-M2 semantic role)
            const auto [role_score, raw_role_delta] =
                scoreEnsembleFunctionRole(sem_role, cand_fam, low_tags, timbre_prof);
            fb.ensemble_function_role = role_score;

            // Factor 7: Percussive vs sustained behavior
            const double track_sustain = (tr.features.note_count > 0u)
                                             ? clamp01(0.55 * tr.features.duty_cycle +
                                                       0.45 * clamp01(tr.features.mean_duration_beats / 2.0))
                                             : (track_is_drum_domain ? 0.10 : 0.50);
            const double cand_sustain = candidateSustainAffinity(cand_fam, low_tags);
            fb.percussive_vs_sustained = clamp01(1.0 - std::abs(track_sustain - cand_sustain));

            // Factor 8: Drum mapping semantics & genre feel
            const auto [drum_sem_score, raw_genre_delta] = scoreGenreAndDrumSemantics(
                semantics.inferred_genre, semantics.groove, cand_fam, low_tags,
                track_is_drum_domain, timbre_prof);
            fb.drum_mapping_semantics = drum_sem_score;

            // Factor 9: Target instrument capability fit
            fb.target_capability_fit = scoreTargetCapabilityFit(
                base_cand, is_dnc, track_is_drum_domain, target_profile);

            // Factor 10: Cross-track ensemble interaction / masking penalty
            double masking_pen = 0.0;
            if (opt.use_ensemble_interaction && !track_is_drum_domain && tr.features.note_count > 0u) {
                const bool cand_bright = isBrightOrDominantTimbre(low_tags);
                for (const auto& prev : chosen_ensemble) {
                    if (prev.dst_ch == tr.target_channel) continue;
                    if (isPercussiveTimbreFamily(prev.family)) continue;
                    const uint8_t ov_lo = std::max(tr.features.pitch_min, prev.pitch_min);
                    const uint8_t ov_hi = std::min(tr.features.pitch_max, prev.pitch_max);
                    if (ov_hi <= ov_lo) continue;
                    const uint8_t span_a = static_cast<uint8_t>(std::max<int>(1, int(tr.features.pitch_max) - int(tr.features.pitch_min)));
                    const uint8_t span_b = static_cast<uint8_t>(std::max<int>(1, int(prev.pitch_max) - int(prev.pitch_min)));
                    const double ov_ratio = static_cast<double>(ov_hi - ov_lo) / static_cast<double>(std::min(span_a, span_b));
                    if (ov_ratio >= 0.45) {
                        const bool exact_same_sound =
                            (prev.ref.msb == base_cand.ref.msb &&
                             prev.ref.lsb == base_cand.ref.lsb &&
                             prev.ref.pc  == base_cand.ref.pc);
                        if (exact_same_sound) {
                            masking_pen = std::max(masking_pen, 0.15);
                        } else if (cand_bright && prev.is_bright && prev.family == cand_fam) {
                            masking_pen = std::max(masking_pen, 0.05);
                        }
                    }
                }
            }
            fb.ensemble_interaction_clearance = clamp01(1.0 - masking_pen * 5.0);

            const double scaled_role_delta = raw_role_delta * (opt.role_context_weight / 0.25);
            const double scaled_genre_delta = raw_genre_delta * (opt.genre_context_weight / 0.15);
            const double scaled_mask_delta = -masking_pen * (opt.ensemble_masking_penalty_weight / 0.10);

            // Blend baseline timbre match (60%) with the 10-factor musical context score (40%) + calibrated deltas
            const double context_10f_mean =
                0.18 * fb.instrument_family_score +
                0.14 * fb.timbral_characteristics_score +
                0.10 * fb.articulation_compatibility +
                0.10 * fb.register_suitability +
                0.08 * fb.dynamic_response +
                0.14 * fb.ensemble_function_role +
                0.08 * fb.percussive_vs_sustained +
                0.08 * fb.drum_mapping_semantics +
                0.06 * fb.target_capability_fit +
                0.04 * fb.ensemble_interaction_clearance;

            double final_conf = clamp01(0.58 * base01 + 0.42 * context_10f_mean +
                                        0.35 * scaled_role_delta + 0.35 * scaled_genre_delta +
                                        scaled_mask_delta);

            // Never inflate confidence above 0.45 on EXP15 unknown/low-confidence private banks!
            if (unknown_low_conf_bank) {
                final_conf = std::min(final_conf, 0.45);
            }

            RankedSoundOption r{};
            r.base_candidate   = base_cand;
            r.factors          = fb;
            r.base_score_01    = base01;
            r.role_fit_delta   = scaled_role_delta;
            r.genre_fit_delta  = scaled_genre_delta;
            r.masking_delta    = scaled_mask_delta;
            r.final_confidence = final_conf;
            r.provenance_class = unknown_low_conf_bank
                                     ? ContextMatchProvenance::UnresolvedFailClosed
                                     : ContextMatchProvenance::ContextStatisticalAdvisory;
            r.match_rationale =
                "10-factor context match [fam=" + fmtFinite(fb.instrument_family_score, 2) +
                ", timb=" + fmtFinite(fb.timbral_characteristics_score, 2) +
                ", art=" + fmtFinite(fb.articulation_compatibility, 2) +
                ", reg=" + fmtFinite(fb.register_suitability, 2) +
                ", dyn=" + fmtFinite(fb.dynamic_response, 2) +
                ", role(" + musicalRoleName(sem_role) + ")=" + fmtFinite(fb.ensemble_function_role, 2) +
                ", sust=" + fmtFinite(fb.percussive_vs_sustained, 2) +
                ", drum=" + fmtFinite(fb.drum_mapping_semantics, 2) +
                ", cap(" + target_profile.key + ")=" + fmtFinite(fb.target_capability_fit, 2) +
                ", mask=" + fmtFinite(fb.ensemble_interaction_clearance, 2) + "]";
            r.known_limitations = buildKnownLimitations(base_cand, target_profile, is_dnc);
            r.estimated_musical_impact =
                (masking_pen > 0.0)
                    ? "Penalized for overlapping register masking with another active track."
                    : (scaled_role_delta >= 0.05
                           ? "Strong ensemble-role and register alignment with minimal masking."
                           : "Compatible baseline substitute; confirm tonal balance by ear.");
            r.fallback_behavior =
                "If rejected, low confidence (< " + fmtFinite(min_conf_floor, 2) +
                "), or feature disabled -> fall back cleanly to deterministic UNRESOLVED / Mode A.";

            r.provenance.origin       = DecisionOrigin::StatisticalInference;
            r.provenance.component_id = "p5_context_sound_v1";
            r.provenance.confidence   = final_conf;
            r.provenance.abstained    = unknown_low_conf_bank || (final_conf < min_conf_floor);
            r.provenance.reversible   = true;
            r.provenance.rationale    = r.match_rationale;
            if (r.provenance.abstained) {
                r.provenance.abstain_reason = unknown_low_conf_bank
                                                  ? "unknown_or_low_confidence_source_bank"
                                                  : "below_min_auto_select_confidence";
            }

            ranked.push_back(std::move(r));
        }

        // Deterministic sort: final_confidence desc, prefer_target_user_sounds tie-break, base score, MIDI address.
        std::sort(ranked.begin(), ranked.end(), [&](const RankedSoundOption& a, const RankedSoundOption& b) {
            if (std::abs(a.final_confidence - b.final_confidence) > 1e-6) {
                return a.final_confidence > b.final_confidence;
            }
            if (opt.prefer_target_user_sounds && a.base_candidate.origin != b.base_candidate.origin) {
                return a.base_candidate.origin == MatchOrigin::TargetKstUserSound;
            }
            if (a.base_candidate.score != b.base_candidate.score) {
                return a.base_candidate.score > b.base_candidate.score;
            }
            return std::tie(a.base_candidate.ref.msb, a.base_candidate.ref.lsb, a.base_candidate.ref.pc) <
                   std::tie(b.base_candidate.ref.msb, b.base_candidate.ref.lsb, b.base_candidate.ref.pc);
        });

        // Detect whether masking penalty actually shifted top-1 away from a colliding candidate.
        if (!ranked.empty() && ranked.size() >= 2u) {
            for (size_t i = 1; i < ranked.size(); ++i) {
                if (ranked[i].masking_delta < -1e-4 &&
                    ranked[i].base_score_01 >= ranked[0].base_score_01 &&
                    ranked[0].masking_delta > ranked[i].masking_delta) {
                    dec.ensemble_collision_mitigated = true;
                    ++result.ensemble_collisions_mitigated;
                    break;
                }
            }
        }

        if (ranked.size() > clamped_top_k) {
            ranked.resize(clamped_top_k);
        }

        // Selection gate: STRICTLY fail-closed!
        const bool can_auto_select =
            result.auto_select_enabled && !unknown_low_conf_bank && !ranked.empty() &&
            ranked.front().final_confidence >= min_conf_floor;

        if (can_auto_select) {
            const auto& top = ranked.front();
            dec.selected            = top.base_candidate.ref;
            dec.selected_origin     = top.base_candidate.origin;
            dec.selected_provenance = ContextMatchProvenance::ContextStatisticalAdvisory;
            dec.epistemic_tag       = top.base_candidate.epistemic_tag;
            dec.fell_back_unresolved = false;
            dec.fallback_behavior    = top.fallback_behavior;
            dec.rationale            = "Auto-selected top-1 context candidate \"" +
                                       top.base_candidate.display_name + "\" (conf=" +
                                       fmtFinite(top.final_confidence, 3) + ").";
            if (tr.target_channel != 0xFF && !result.resolved_sounds.count(tr.target_channel)) {
                result.resolved_sounds[tr.target_channel] = top.base_candidate.ref;
            }
            ++result.auto_selected_count;

            const TimbreFamily chosen_fam = familyForCandidate(top.base_candidate, timbre_prof);
            const std::string chosen_tags = candidateTagsLower(top.base_candidate);
            chosen_ensemble.push_back(ChosenEnsembleSlot{
                src_ch, tr.target_channel, tr.features.pitch_min, tr.features.pitch_max,
                chosen_fam, top.base_candidate.ref, isBrightOrDominantTimbre(chosen_tags)
            });
        } else {
            dec.selected            = std::nullopt;
            dec.selected_provenance = ContextMatchProvenance::UnresolvedFailClosed;
            dec.epistemic_tag       = "UNRESOLVED_FAIL_CLOSED";
            dec.fell_back_unresolved = true;
            if (!result.auto_select_enabled) {
                dec.fallback_behavior = "Auto-select feature flag disabled -> preserved deterministic UNRESOLVED.";
                dec.rationale = "Advisory ranking computed; auto-select disabled (default OFF fail-closed).";
            } else if (unknown_low_conf_bank) {
                dec.fallback_behavior = "Unknown/private bank (timbre conf=" +
                                        std::to_string(timbre_prof.confidence) +
                                        ") -> fail-closed UNRESOLVED (never guessed).";
                dec.rationale = "Fail-closed UNRESOLVED on unknown or low-confidence source bank.";
            } else {
                dec.fallback_behavior = "Top candidate below confidence floor (" +
                                        fmtFinite(min_conf_floor, 2) + ") -> fail-closed UNRESOLVED.";
                dec.rationale = "Fail-closed UNRESOLVED: no candidate cleared confidence threshold.";
            }
            ++result.unresolved_fallback_count;
        }

        dec.ranked_options = std::move(ranked);
        result.tracks.push_back(std::move(dec));
    }

    // Preserve any user_overrides for target channels not present in semantics.
    for (const auto& [kch, ref] : user_overrides) {
        if (!result.resolved_sounds.count(kch)) {
            result.resolved_sounds[kch] = ref;
            ++result.user_overrides_preserved;
        }
    }

    return result;
}

inline std::string formatContextSoundMatchJson(const ContextSoundMatchResult& r) {
    using namespace context_sound_detail;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"" << jsonEscapeStrict(r.schema) << "\",\n";
    o << "  \"targetPlatform\": \"" << jsonEscapeStrict(targetPlatformKey(r.target_platform)) << "\",\n";
    o << "  \"autoSelectEnabled\": " << (r.auto_select_enabled ? "true" : "false") << ",\n";
    o << "  \"userOverridesPreserved\": " << r.user_overrides_preserved << ",\n";
    o << "  \"autoSelectedCount\": " << r.auto_selected_count << ",\n";
    o << "  \"unresolvedFallbackCount\": " << r.unresolved_fallback_count << ",\n";
    o << "  \"oosTargetEntriesRejected\": " << r.oos_target_entries_rejected << ",\n";
    o << "  \"crossFamilyContaminations\": " << r.cross_family_contaminations << ",\n";
    o << "  \"ensembleCollisionsMitigated\": " << r.ensemble_collisions_mitigated << ",\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"" << jsonEscapeStrict(r.l3_status) << "\",\n";
    o << "  \"tracks\": [";
    for (size_t i = 0; i < r.tracks.size(); ++i) {
        const auto& t = r.tracks[i];
        if (i > 0u) o << ",";
        o << "\n    {\n";
        o << "      \"srcCh\": " << static_cast<unsigned>(t.source_channel) << ",\n";
        o << "      \"targetCh\": " << static_cast<unsigned>(t.target_channel) << ",\n";
        o << "      \"trackName\": \"" << jsonEscapeStrict(t.track_name) << "\",\n";
        o << "      \"semanticRole\": \"" << musicalRoleName(t.semantic_role) << "\",\n";
        o << "      \"userOverrideLocked\": " << (t.user_override_locked ? "true" : "false") << ",\n";
        o << "      \"fellBackUnresolved\": " << (t.fell_back_unresolved ? "true" : "false") << ",\n";
        o << "      \"selectedProvenance\": \"" << contextMatchProvenanceName(t.selected_provenance) << "\",\n";
        o << "      \"epistemicTag\": \"" << jsonEscapeStrict(t.epistemic_tag) << "\",\n";
        o << "      \"ensembleCollisionMitigated\": " << (t.ensemble_collision_mitigated ? "true" : "false") << ",\n";
        o << "      \"rationale\": \"" << jsonEscapeStrict(t.rationale) << "\",\n";
        o << "      \"fallbackBehavior\": \"" << jsonEscapeStrict(t.fallback_behavior) << "\",\n";
        o << "      \"rankedOptions\": [";
        for (size_t j = 0; j < t.ranked_options.size(); ++j) {
            const auto& c = t.ranked_options[j];
            if (j > 0u) o << ",";
            o << "\n        {"
              << "\"displayName\": \"" << jsonEscapeStrict(c.base_candidate.display_name) << "\""
              << ", \"msb\": " << static_cast<unsigned>(c.base_candidate.ref.msb)
              << ", \"lsb\": " << static_cast<unsigned>(c.base_candidate.ref.lsb)
              << ", \"pc\": " << static_cast<unsigned>(c.base_candidate.ref.pc)
              << ", \"origin\": \"" << exporters::korg::quality::matchOriginName(c.base_candidate.origin) << "\""
              << ", \"provenance\": \"" << contextMatchProvenanceName(c.provenance_class) << "\""
              << ", \"baseScore01\": " << fmtFinite(c.base_score_01, 4)
              << ", \"roleFitDelta\": " << fmtFinite(c.role_fit_delta, 4)
              << ", \"genreFitDelta\": " << fmtFinite(c.genre_fit_delta, 4)
              << ", \"maskingDelta\": " << fmtFinite(c.masking_delta, 4)
              << ", \"finalConfidence\": " << fmtFinite(c.final_confidence, 4)
              << ", \"epistemicTag\": \"" << jsonEscapeStrict(c.base_candidate.epistemic_tag) << "\""
              << ", \"rationale\": \"" << jsonEscapeStrict(c.match_rationale) << "\""
              << ", \"knownLimitations\": \"" << jsonEscapeStrict(c.known_limitations) << "\""
              << ", \"estimatedMusicalImpact\": \"" << jsonEscapeStrict(c.estimated_musical_impact) << "\""
              << "}";
        }
        if (!t.ranked_options.empty()) o << "\n      ";
        o << "]\n    }";
    }
    if (!r.tracks.empty()) o << "\n  ";
    o << "]\n}\n";
    return o.str();
}

inline std::string formatContextSoundMatchMd(const ContextSoundMatchResult& r) {
    using namespace context_sound_detail;
    std::ostringstream o;
    o << "# Context Sound Matching Report (`" << targetPlatformKey(r.target_platform) << "`)\n\n";
    o << "- **Auto-Select Enabled:** `" << (r.auto_select_enabled ? "true" : "false") << "`"
      << " | **User Overrides Preserved:** `" << r.user_overrides_preserved << "`"
      << " | **Auto-Selected:** `" << r.auto_selected_count << "`"
      << " | **Fail-Closed Unresolved:** `" << r.unresolved_fallback_count << "`\n";
    o << "- **OOS Target Entries Rejected:** `" << r.oos_target_entries_rejected << "`"
      << " | **Cross-Family Contaminations:** `" << r.cross_family_contaminations << "`"
      << " | **Ensemble Collisions Mitigated:** `" << r.ensemble_collisions_mitigated << "`\n";
    o << "- **Epistemic Boundary:** `hardwareVerified: false` | `l3Status: " << r.l3_status << "`\n\n";
    o << "| Src Ch | Dst Ch | Track | Role | Top-1 Candidate | Conf | Provenance | Epistemic |\n";
    o << "|---:|---:|---|---|---|---:|---|---|\n";
    for (const auto& t : r.tracks) {
        const std::string top_name = t.ranked_options.empty() ? "NONE" : t.ranked_options.front().base_candidate.display_name;
        const double top_conf = t.ranked_options.empty() ? 0.0 : t.ranked_options.front().final_confidence;
        o << "| " << static_cast<unsigned>(t.source_channel)
          << " | " << static_cast<unsigned>(t.target_channel)
          << " | " << t.track_name
          << " | `" << musicalRoleName(t.semantic_role) << "`"
          << " | " << top_name
          << " | " << fmtFinite(top_conf, 3)
          << " | `" << contextMatchProvenanceName(t.selected_provenance) << "`"
          << " | `" << t.epistemic_tag << "` |\n";
    }
    return o.str();
}

} // namespace ai_arranger::intelligence

#endif // AI_ARRANGER_INTELLIGENCE_CONTEXT_SOUND_MATCHER_H
