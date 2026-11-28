#ifndef AI_ARRANGER_INTELLIGENCE_QUALITY_INTELLIGENCE_H
#define AI_ARRANGER_INTELLIGENCE_QUALITY_INTELLIGENCE_H

#include "exporters/korg/acceptance_gate.h"
#include "exporters/korg/musical_quality_analyzer.h"
#include "intelligence/target_reorchestrator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Phase 5 Milestone P5-M5: 4-Layer Evidence-Based Musical Quality Intelligence
// & Mandatory AI-Rejection Gate.
//
// Boundaries & Governance Invariants (ADR-P5-003 / ADR-P5-004 / ADR-P5-005):
//   * "AI proposes. Deterministic engine enforces. Acceptance Gate validates. User retains control."
//   * Header-only portable C++20, zero external deps, zero mutable globals, Zone B offline only.
//   * Strictly independent of frozen Q1 benchmark (style_quality_bench.h).
//   * 4-Layer Evaluation Architecture:
//       - Layer 1 — Technical Integrity (Hard Pass/Fail Gate — NO AI MAY BYPASS)
//       - Layer 2 — Musical Integrity (Anti-ceiling-compression transparent weighted metrics)
//       - Layer 3 — Target Suitability (Parameterized over TargetCapabilityProfile)
//       - Layer 4 — Human Preference (Future framework: STRICTLY NOT_TESTED offline!)
//   * Mandatory AI-Rejection Gate (`evaluateCandidateAcceptance`, `selectBestCandidate`):
//       - Compares A (Deterministic Mode A), B (Mode B Adaptation), C (Mode C Enhancement).
//       - Automatically rejects ANY B/C/AI candidate failing Layer 1, breaking locked tracks,
//         losing source identity, introducing new critical defects, or failing to beat Mode A
//         cleanly without regression — falling back deterministically to untouched Mode A!
//   * Anti-Overfitting Epistemic Stamp on every JSON/MD report:
//       `offlineScoreIsNotAudibleProof: true`, `hardwareVerified: false`, `l3Status: NOT_TESTED`.

namespace ai_arranger::intelligence {

struct Layer1TechnicalScore {
    bool                     passed = false; // Hard gate: MUST be true (NO AI MAY BYPASS)
    uint32_t                 korg_validate_errors = 0;
    uint32_t                 locked_track_mutations = 0;
    uint32_t                 same_tick_cc_after_note_on = 0;
    uint32_t                 loop_boundary_violations = 0;
    uint32_t                 hanging_note_violations = 0;
    uint32_t                 chord_band_leaks = 0;
    uint32_t                 polyphony_cap_violations = 0;
    bool                     deterministic_roundtrip_ok = true;
    double                   score_0_100 = 0.0;
    std::vector<std::string> errors;
};

struct Layer2MusicalScore {
    exporters::korg::quality::MusicalQualityReport base_mq12{};
    NttChordAuditSummary     ntt_audit{};
    uint32_t                 saturated_dimensions_at_100 = 0; // Anti-ceiling-compression transparency
    // Transparent anti-ceiling-compression sub-metrics [0..100]:
    double                   harmonic_correctness_score = 0.0;
    double                   bass_behavior_score = 0.0;
    double                   groove_preservation_score = 0.0;
    double                   phrase_continuity_score = 0.0;
    double                   arrangement_coherence_score = 0.0;
    double                   track_role_preservation_score = 0.0;
    double                   section_differentiation_score = 0.0;
    double                   musical_identity_preservation_score = 100.0;
    double                   decompressed_musical_score = 0.0; // Weighted transparent Layer 2 [0..100]
    uint32_t                 critical_findings = 0;
    uint32_t                 warning_findings = 0;
};

struct Layer3TargetRealismScore {
    TargetPlatformId         target_platform = TargetPlatformId::KorgPa5X;
    double                   instrument_compatibility_score = 0.0;
    double                   register_suitability_score = 0.0;
    double                   articulation_compatibility_score = 0.0;
    double                   arrangement_balance_score = 0.0;
    double                   sound_mapping_confidence_score = 0.0;
    double                   target_capability_utilization_score = 0.0;
    double                   score_0_100 = 0.0;
    uint32_t                 capability_cap_violations = 0;
    uint32_t                 unresolved_sound_count = 0;
    uint32_t                 register_out_of_idiom_notes = 0;
    std::vector<std::string> advisories;
};

struct HumanPreferenceHooks {
    std::string status = "NOT_TESTED";
    std::string blind_abc_session_id;
    uint32_t    pairwise_comparisons_count = 0;
    uint32_t    expert_evaluations_count = 0;
    double      user_weight_layer2_musical = 0.55; // User-adjustable offline/human weighting hook
    double      user_weight_layer3_target = 0.45;
    bool        feedback_ranking_active = false;
};

struct Layer4HumanEvaluationState {
    bool                                      hardware_verified = false;       // Always false without real HW record
    std::string                               l3_status = "NOT_TESTED";        // Always NOT_TESTED offline
    bool                                      audible_proof_available = false; // Always false offline
    exporters::korg::quality::AcceptanceGrade grade = exporters::korg::quality::AcceptanceGrade::U;
    bool                                      human_record_present = false;
    HumanPreferenceHooks                      preference_hooks{};
    std::string                               epistemic_notice =
        "OFFLINE_ONLY — HUMAN_AND_HARDWARE_VERIFICATION_REQUIRED (Layer 4 NOT_TESTED)";
};

struct QualityIntelligenceReport {
    std::string                schema = "aiarranger-quality-intelligence/1";
    std::string                style_name;
    ArrangementMode            mode = ArrangementMode::ModeA_Faithful;
    Layer1TechnicalScore       layer1_technical{};
    Layer2MusicalScore         layer2_musical{};
    Layer3TargetRealismScore   layer3_target{};
    Layer4HumanEvaluationState layer4_human{};
    double                     offline_composite = 0.0; // Uncalibrated offline diagnostic ONLY
    bool                       independent_from_frozen_q1 = true;
    bool                       offline_score_is_not_audible_proof = true;
    std::string                anti_overfitting_warning =
        "OFFLINE_DIAGNOSTIC_ONLY: Offline scores are uncalibrated software heuristics, NOT audible/hardware proof. "
        "Do not optimize solely against offline composites; Layer 4 physical listening is mandatory.";
};

struct RejectionGateOptions {
    double min_ntt_conformance = 0.85;
    double min_identity_preservation = 85.0;  // [0..100]
    double min_net_gain_for_adapt = 0.05;     // Mode B must beat A cleanly (zero-noise tolerance)
    double min_net_gain_for_creative = 0.25;  // Mode C / AI must show clear net positive gain
    bool   reject_any_new_warning = false;
    bool   reject_any_new_critical = true;    // Hard invariant
    bool   require_locked_tracks_bit_exact = true; // Hard invariant
};

struct CandidateGateDecision {
    bool                      accepted = false;
    bool                      fallback_to_faithful = false;
    double                    musical_delta = 0.0;
    double                    target_delta = 0.0;
    double                    ntt_conformance_delta = 0.0;
    double                    identity_preservation_score = 100.0;
    std::vector<std::string>  rejection_reasons;
    QualityIntelligenceReport baseline_report;
    QualityIntelligenceReport candidate_report;
};

struct AbcSelectionDecision {
    ArrangementMode           selected_mode = ArrangementMode::ModeA_Faithful;
    std::string               selected_label = "ModeA_Faithful";
    bool                      deterministic_a_won = true;
    QualityIntelligenceReport selected_report;
    CandidateGateDecision     mode_b_decision;
    CandidateGateDecision     mode_c_decision;
    std::string               rationale;
};

namespace qi_detail {

inline double clamp0_100(double v) noexcept {
    if (!std::isfinite(v) || v < 0.0) return 0.0;
    if (v > 100.0) return 100.0;
    return std::round(v * 100.0) / 100.0;
}

inline double round2(double v) noexcept {
    if (!std::isfinite(v)) return 0.0;
    return std::round(v * 100.0) / 100.0;
}

inline std::string fmt2(double v) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::fixed << std::setprecision(2) << (std::isfinite(v) ? v : 0.0);
    return o.str();
}

inline std::string fmt4(double v) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::fixed << std::setprecision(4) << (std::isfinite(v) ? v : 0.0);
    return o.str();
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

inline uint32_t countLockedMutations(
    const exporters::korg::Style& ref,
    const exporters::korg::Style& cand,
    const std::set<uint8_t>& locked_channels) {
    if (locked_channels.empty()) return 0u;
    uint32_t muts = 0u;
    std::map<std::string, const exporters::korg::Element*> cand_map;
    for (const auto& el : cand.elements) cand_map[el.marker] = &el;
    for (const auto& ref_el : ref.elements) {
        const auto it = cand_map.find(ref_el.marker);
        if (it == cand_map.end()) {
            muts += static_cast<uint32_t>(locked_channels.size());
            continue;
        }
        for (uint8_t ch : locked_channels) {
            const auto a = reorch_detail::extractChannelEvents(ref_el, ch);
            const auto b = reorch_detail::extractChannelEvents(*it->second, ch);
            if (a.size() != b.size()) {
                ++muts;
                continue;
            }
            for (size_t i = 0; i < a.size(); ++i) {
                if (!reorch_detail::eventEqual(a[i], b[i])) {
                    ++muts;
                    break;
                }
            }
        }
    }
    return muts;
}

inline bool verifyDeterministicSmfRoundtrip(const exporters::korg::Style& style) {
    const auto w = exporters::korg::write(style);
    if (!w.ok) return false;
    const auto p = exporters::korg::parse(w.bytes);
    if (!p.ok) return false;
    return exporters::korg::validate(p.style).empty();
}

// Continuous anti-ceiling-compression variation differentiation score [0..100]
// (avoids pinning at 100.0 when variations have nearly flat energy!).
inline double scoreVariationDifferentiationDecompressed(
    const exporters::korg::quality::MusicalQualityReport& mq) noexcept {
    if (mq.variation_curve.size() < 2u) return 75.0;
    double e_min = mq.variation_curve.front().composite_energy;
    double e_max = e_min;
    uint32_t drops = 0;
    for (size_t i = 0; i < mq.variation_curve.size(); ++i) {
        const double e = mq.variation_curve[i].composite_energy;
        e_min = std::min(e_min, e);
        e_max = std::max(e_max, e);
        if (i > 0u && e + 1.5 < mq.variation_curve[i - 1u].composite_energy) ++drops;
    }
    const double spread = e_max - e_min;
    // Reward natural dynamic build across V1..V4 (ideal spread ~20..50 points), penalize flatness or inversions
    const double spread_score = std::min(100.0, 52.0 + spread * 1.15);
    const double mono_penalty = static_cast<double>(drops) * 10.0;
    return clamp0_100(spread_score - mono_penalty);
}

// Continuous anti-ceiling-compression groove & dynamic contrast score [0..100].
inline double scoreGrooveDecompressed(
    const exporters::korg::quality::MusicalQualityReport& mq,
    const CanonicalStyleView& cview) noexcept {
    const double d05 = mq.dimensions[4].score; // Velocity & Dynamics
    const double d08 = mq.dimensions[7].score; // Groove & Rhythmic Feel
    // Use actual velocity stddev across tracks so flat-ish styles don't saturate at 100.0
    double std_sum = 0.0;
    uint32_t trk_cnt = 0;
    for (const auto& t : mq.tracks) {
        if (t.note_count >= 4u) {
            std_sum += std::min(22.0, t.vel_stddev);
            ++trk_cnt;
        }
    }
    const double mean_std = (trk_cnt > 0u) ? (std_sum / static_cast<double>(trk_cnt)) : 8.0;
    const double dynamic_vitality = std::min(100.0, 58.0 + mean_std * 1.9);
    const double lock_bonus = std::min(100.0, 70.0 + cview.inter_track_overall.kick_bass_lock_correlation * 30.0);
    return clamp0_100(0.35 * d05 + 0.30 * d08 + 0.20 * dynamic_vitality + 0.15 * lock_bonus);
}

} // namespace qi_detail

inline QualityIntelligenceReport evaluateQualityIntelligence(
    const exporters::korg::Style& style,
    const exporters::korg::Report& report,
    const TargetCapabilityProfile& target_profile,
    ArrangementMode mode = ArrangementMode::ModeA_Faithful,
    const exporters::korg::Style* reference_faithful = nullptr,
    const std::set<uint8_t>& locked_channels = {},
    const exporters::korg::quality::HumanListeningRecord* listening = nullptr,
    const ContextSoundMatchResult* context_sounds = nullptr) {
    using namespace qi_detail;

    QualityIntelligenceReport out{};
    out.mode = mode;
    out.independent_from_frozen_q1 = true;
    out.offline_score_is_not_audible_proof = true;

    // ── Layer 1: Technical Integrity (HARD PASS/FAIL GATE — NO AI MAY BYPASS!) ──
    const auto korg_errs = exporters::korg::validate(style);
    const auto ntt_audit = auditStyleNttConformance(style, target_profile, reference_faithful);
    const uint32_t locked_muts =
        reference_faithful ? countLockedMutations(*reference_faithful, style, locked_channels) : 0u;
    const bool roundtrip_ok = korg_errs.empty() ? verifyDeterministicSmfRoundtrip(style) : false;

    auto& l1 = out.layer1_technical;
    l1.korg_validate_errors       = static_cast<uint32_t>(korg_errs.size());
    l1.locked_track_mutations     = locked_muts;
    l1.same_tick_cc_after_note_on = ntt_audit.same_tick_cc_after_note_on;
    l1.loop_boundary_violations   = ntt_audit.loop_boundary_violations;
    l1.hanging_note_violations    = ntt_audit.hanging_note_violations;
    l1.chord_band_leaks           = ntt_audit.chord_band_leaks;
    l1.polyphony_cap_violations   = ntt_audit.polyphony_cap_violations;
    l1.deterministic_roundtrip_ok = roundtrip_ok;

    for (const auto& e : korg_errs) l1.errors.push_back("korg::validate: " + e);
    if (locked_muts > 0u) {
        l1.errors.push_back("locked_track_mutations=" + std::to_string(locked_muts));
    }
    if (l1.same_tick_cc_after_note_on > 0u) {
        l1.errors.push_back("same_tick_cc_after_note_on=" + std::to_string(l1.same_tick_cc_after_note_on));
    }
    if (l1.loop_boundary_violations > 0u) {
        l1.errors.push_back("loop_boundary_violations=" + std::to_string(l1.loop_boundary_violations));
    }
    if (l1.hanging_note_violations > 0u) {
        l1.errors.push_back("hanging_note_violations=" + std::to_string(l1.hanging_note_violations));
    }
    if (l1.chord_band_leaks > 0u) {
        l1.errors.push_back("chord_band_leaks=" + std::to_string(l1.chord_band_leaks));
    }
    if (l1.polyphony_cap_violations > 0u) {
        l1.errors.push_back("polyphony_cap_violations=" + std::to_string(l1.polyphony_cap_violations));
    }
    if (!roundtrip_ok && korg_errs.empty()) {
        l1.errors.push_back("deterministic SMF write->parse roundtrip failed");
    }

    l1.passed = l1.errors.empty();
    const uint32_t total_l1_defects =
        l1.korg_validate_errors + l1.locked_track_mutations + l1.same_tick_cc_after_note_on +
        l1.loop_boundary_violations + l1.hanging_note_violations + l1.chord_band_leaks +
        l1.polyphony_cap_violations + (roundtrip_ok ? 0u : 1u);
    l1.score_0_100 = l1.passed ? 100.0 : clamp0_100(100.0 - static_cast<double>(total_l1_defects) * 25.0);

    // ── Layer 2: Musical Integrity (Decompressed Anti-Ceiling-Compression Metrics) ──
    auto& l2 = out.layer2_musical;
    l2.base_mq12 = exporters::korg::quality::analyzeMusicalQuality(style, &report);
    out.style_name = l2.base_mq12.style_name;
    l2.ntt_audit = ntt_audit;

    for (const auto& dim : l2.base_mq12.dimensions) {
        if (dim.applicable && dim.score >= 99.99) {
            ++l2.saturated_dimensions_at_100;
        }
    }
    for (const auto& f : l2.base_mq12.all_findings) {
        if (f.severity == exporters::korg::quality::MusicalFindingSeverity::Critical) ++l2.critical_findings;
        else if (f.severity == exporters::korg::quality::MusicalFindingSeverity::Warning) ++l2.warning_findings;
    }

    const CanonicalStyleView cview = buildCanonicalStyleViewFromKorg(
        style, out.style_name, &report, target_profile.id);

    // 2.1 Harmonic correctness (12-root x 10-chord-quality NTT + clash penalty)
    const double clash_pen = std::min(30.0, static_cast<double>(ntt_audit.harmonic_clash_count) * 1.5);
    l2.harmonic_correctness_score = clamp0_100(
        (0.65 * ntt_audit.chord_tone_conformance + 0.35 * ntt_audit.bass_root_accuracy) * 100.0 - clash_pen);

    // 2.2 Bass behavior (D02 + bass register + Bass/Piano < C3 separation)
    const uint32_t bp_coll = reorch_detail::countBassPianoLowCollisionsInStyle(style, &report, nullptr);
    const double bass_reg_pen = std::min(30.0, static_cast<double>(ntt_audit.bass_out_of_register_notes) * 3.0);
    const double bp_coll_pen  = std::min(20.0, static_cast<double>(bp_coll) * 2.0);
    l2.bass_behavior_score = clamp0_100(
        0.55 * l2.base_mq12.dimensions[1].score +
        0.45 * (ntt_audit.bass_root_accuracy * 100.0) - bass_reg_pen - bp_coll_pen);

    // 2.3 Groove preservation (anti-ceiling-compression)
    l2.groove_preservation_score = scoreGrooveDecompressed(l2.base_mq12, cview);

    // 2.4 Phrase continuity (D06 + D10 + D11)
    l2.phrase_continuity_score = clamp0_100(
        0.35 * l2.base_mq12.dimensions[5].score +
        0.35 * l2.base_mq12.dimensions[9].score +
        0.30 * l2.base_mq12.dimensions[10].score);

    // 2.5 Arrangement coherence (D01 + D04 + D12 + register overlap clarity)
    const uint32_t warn_overlaps = static_cast<uint32_t>(std::count_if(
        cview.inter_track_overall.register_overlaps.begin(),
        cview.inter_track_overall.register_overlaps.end(),
        [](const RegisterOverlapEntry& e) { return e.collision_warning; }));
    const double ov_pen = std::min(15.0, static_cast<double>(warn_overlaps) * 4.0);
    l2.arrangement_coherence_score = clamp0_100(
        0.30 * l2.base_mq12.dimensions[0].score +
        0.35 * l2.base_mq12.dimensions[3].score +
        0.35 * l2.base_mq12.dimensions[11].score - ov_pen);

    // 2.6 Track-role preservation (via P5-M2 semantic overlay comparison vs reference if provided)
    if (reference_faithful != nullptr) {
        const CanonicalStyleView ref_view = buildCanonicalStyleViewFromKorg(
            *reference_faithful, out.style_name, &report, target_profile.id);
        std::map<uint8_t, RegisterBand> ref_bands;
        for (const auto& s : ref_view.sections) {
            for (const auto& t : s.tracks) {
                if (t.features.note_count > 0u) ref_bands[t.target_channel] = t.features.register_band;
            }
        }
        uint32_t total_ch = 0, matched_ch = 0;
        for (const auto& s : cview.sections) {
            for (const auto& t : s.tracks) {
                if (t.features.note_count == 0u) continue;
                auto it = ref_bands.find(t.target_channel);
                if (it != ref_bands.end()) {
                    ++total_ch;
                    const int diff = std::abs(static_cast<int>(t.features.register_band) -
                                              static_cast<int>(it->second));
                    if (diff <= 1) ++matched_ch;
                }
            }
        }
        l2.track_role_preservation_score =
            (total_ch > 0u) ? clamp0_100(100.0 * static_cast<double>(matched_ch) / static_cast<double>(total_ch))
                            : 100.0;
    } else {
        // Baseline standalone role clarity (active tracks vs expected 8-track layout)
        const double active_ratio = std::min(1.0, static_cast<double>(l2.base_mq12.tracks.size()) / 6.0);
        l2.track_role_preservation_score = clamp0_100(70.0 + active_ratio * 25.0);
    }

    // 2.7 Section differentiation (decompressed V1..V4 arc spread)
    l2.section_differentiation_score = scoreVariationDifferentiationDecompressed(l2.base_mq12);

    // 2.8 Musical identity preservation (vs reference_faithful; 100.0 for standalone Mode A)
    const double id_sim_01 =
        (reference_faithful != nullptr)
            ? reorch_detail::computeIdentitySimilarity(*reference_faithful, style)
            : 1.0;
    l2.musical_identity_preservation_score = clamp0_100(id_sim_01 * 100.0);

    // Weighted transparent Layer 2 composite (deliberately decompressed!)
    const double crit_pen = static_cast<double>(l2.critical_findings) * 12.0;
    const double warn_pen = static_cast<double>(l2.warning_findings) * 3.0;
    l2.decompressed_musical_score = clamp0_100(
        0.20 * l2.harmonic_correctness_score +
        0.15 * l2.bass_behavior_score +
        0.14 * l2.groove_preservation_score +
        0.12 * l2.phrase_continuity_score +
        0.12 * l2.arrangement_coherence_score +
        0.09 * l2.track_role_preservation_score +
        0.10 * l2.section_differentiation_score +
        0.08 * l2.musical_identity_preservation_score - crit_pen - warn_pen);

    // ── Layer 3: Target Suitability (Parameterized by TargetCapabilityProfile) ──
    auto& l3 = out.layer3_target;
    l3.target_platform = target_profile.id;

    // 3.1 Instrument/Platform capability compatibility
    uint32_t cap_viols = 0;
    uint32_t max_cv_seen = 0;
    for (const auto& el : style.elements) {
        if (el.marker.size() == 5u && el.marker[0] == 'v' && el.marker[2] == 'c' && el.marker[3] == 'v') {
            const uint32_t cv = static_cast<uint32_t>(el.marker[4] - '0');
            if (cv > max_cv_seen) max_cv_seen = cv;
            if (target_profile.sections.max_cv_variation > 0u &&
                cv > target_profile.sections.max_cv_variation) {
                ++cap_viols;
                l3.advisories.push_back("CV" + std::to_string(cv) + " exceeds " + target_profile.display_name +
                                        " cap (" + std::to_string(target_profile.sections.max_cv_variation) + ")");
            }
        }
        if (el.marker == "br" && target_profile.sections.break_max_bars > 0u) {
            const uint32_t ppqn = std::max<uint32_t>(1u, style.ppqn ? style.ppqn : 384u);
            const uint32_t bar_t = std::max<uint32_t>(1u, (ppqn * 4u * std::max<uint32_t>(1u, el.num)) /
                                                          std::max<uint32_t>(1u, el.den));
            if (el.length > bar_t * target_profile.sections.break_max_bars) {
                ++cap_viols;
                l3.advisories.push_back("Break exceeds " + std::to_string(target_profile.sections.break_max_bars) +
                                        "-bar cap for " + target_profile.display_name);
            }
        }
    }
    if (ntt_audit.max_observed_polyphony > target_profile.tracks.style_voice_budget) {
        ++cap_viols;
        l3.advisories.push_back("Observed polyphony " + std::to_string(ntt_audit.max_observed_polyphony) +
                                " exceeds style voice budget " + std::to_string(target_profile.tracks.style_voice_budget));
    }
    l3.capability_cap_violations = cap_viols;
    double compat_base = target_profile.articulation.supports_smf_style_import ? 94.0 : 82.0;
    if (style.ppqn != 0u && style.ppqn != target_profile.preferred_ppqn) {
        compat_base -= 3.0;
    }
    l3.instrument_compatibility_score = clamp0_100(compat_base - static_cast<double>(cap_viols) * 12.0);

    // 3.2 Register suitability against TargetCapabilityProfile::role_pitch_ranges
    uint64_t total_melodic_notes = 0;
    uint64_t out_of_idiom = 0;
    for (const auto& el : style.elements) {
        for (uint8_t ch = exporters::korg::kBassCh; ch <= 15u; ++ch) {
            if (!exporters::korg::isChordDriven(ch)) continue;
            const auto range = (ch == exporters::korg::kBassCh)
                                   ? target_profile.pitchRangeFor(RoleRegisterCategory::Bass)
                                   : target_profile.pitchRangeFor(RoleRegisterCategory::HarmonicComp);
            const auto notes = reorch_detail::extractNotes(el, ch);
            for (const auto& n : notes) {
                ++total_melodic_notes;
                if (!range.contains(n.pitch)) ++out_of_idiom;
            }
        }
    }
    l3.register_out_of_idiom_notes = static_cast<uint32_t>(std::min<uint64_t>(out_of_idiom, UINT32_MAX));
    l3.register_suitability_score =
        (total_melodic_notes > 0u)
            ? clamp0_100(100.0 * (1.0 - static_cast<double>(out_of_idiom) / static_cast<double>(total_melodic_notes)))
            : 95.0;

    // 3.3 Articulation compatibility (MegaVoice / Guitar / DNC)
    l3.articulation_compatibility_score = clamp0_100(
        0.65 * l2.base_mq12.dimensions[2].score +
        0.35 * (target_profile.articulation.supports_guitar_mode ? 95.0 : 80.0));

    // 3.4 Arrangement balance (mix CC7/CC10/CC91/CC93 + absence of low mud collisions)
    l3.arrangement_balance_score = clamp0_100(
        0.55 * l2.base_mq12.dimensions[11].score +
        0.45 * l2.base_mq12.dimensions[3].score - std::min(15.0, static_cast<double>(bp_coll) * 2.5));

    // 3.5 Sound mapping confidence (from report + optional P5-M3 ContextSoundMatchResult)
    uint32_t resolved_cnt = 0;
    uint32_t unresolved_cnt = 0;
    double mean_ctx_conf = 0.0;
    if (context_sounds != nullptr && !context_sounds->tracks.empty()) {
        double csum = 0.0;
        for (const auto& t : context_sounds->tracks) {
            if (t.selected.has_value()) {
                ++resolved_cnt;
                csum += t.ranked_options.empty() ? 1.0 : t.ranked_options.front().final_confidence;
            } else {
                ++unresolved_cnt;
            }
        }
        mean_ctx_conf = (resolved_cnt > 0u) ? (csum / static_cast<double>(resolved_cnt)) : 0.0;
    } else {
        for (const auto& row : report.tracks) {
            if (row.korg_ch == 0xFF) continue;
            if (row.match_status == "USER_OVERRIDE" || row.match_status == "EXACT" || row.match_status == "APPROX") {
                ++resolved_cnt;
                mean_ctx_conf += static_cast<double>(row.match_score) / 100.0;
            } else {
                ++unresolved_cnt;
            }
        }
        if (resolved_cnt > 0u) mean_ctx_conf /= static_cast<double>(resolved_cnt);
    }
    l3.unresolved_sound_count = unresolved_cnt;
    const uint32_t total_snd = resolved_cnt + unresolved_cnt;
    if (total_snd > 0u) {
        const double res_ratio = static_cast<double>(resolved_cnt) / static_cast<double>(total_snd);
        l3.sound_mapping_confidence_score = clamp0_100(100.0 * (0.65 * res_ratio + 0.35 * mean_ctx_conf));
    } else {
        l3.sound_mapping_confidence_score = 75.0;
    }
    if (unresolved_cnt > 0u) {
        l3.advisories.push_back(std::to_string(unresolved_cnt) + " track(s) remain UNRESOLVED (manual soundmap recommended).");
    }

    // 3.6 Target capability utilization
    const double trk_util = std::min(1.0, static_cast<double>(l2.base_mq12.tracks.size()) /
                                          std::max<double>(1.0, static_cast<double>(target_profile.tracks.total_style_tracks)));
    const double cv_util = (target_profile.sections.max_cv_variation > 0u && max_cv_seen > 0u)
                               ? std::min(1.0, 0.55 + 0.45 * (static_cast<double>(max_cv_seen) /
                                                              static_cast<double>(target_profile.sections.max_cv_variation)))
                               : 0.78;
    l3.target_capability_utilization_score = clamp0_100(100.0 * (0.55 * trk_util + 0.45 * cv_util));

    l3.score_0_100 = clamp0_100(
        0.20 * l3.instrument_compatibility_score +
        0.20 * l3.register_suitability_score +
        0.15 * l3.articulation_compatibility_score +
        0.15 * l3.arrangement_balance_score +
        0.18 * l3.sound_mapping_confidence_score +
        0.12 * l3.target_capability_utilization_score);

    // ── Layer 4: Human Preference (STRICTLY NOT_TESTED Offline — NEVER manufactured!) ──
    auto& l4 = out.layer4_human;
    l4.hardware_verified = false;
    l4.l3_status = "NOT_TESTED";
    l4.audible_proof_available = false;
    l4.human_record_present = (listening != nullptr && listening->present);
    l4.preference_hooks.status = "NOT_TESTED";

    if (!l1.passed) {
        l4.grade = exporters::korg::quality::AcceptanceGrade::D;
    } else if (l3.unresolved_sound_count > 0u) {
        // Check if any critical track (drum/bass/acc1) is unresolved -> Grade C
        bool critical_unres = false;
        for (const auto& row : report.tracks) {
            if (row.korg_ch == exporters::korg::kDrumCh ||
                row.korg_ch == exporters::korg::kBassCh ||
                row.korg_ch == exporters::korg::kAcc1Ch) {
                if (row.match_status != "USER_OVERRIDE" && row.match_status != "EXACT" &&
                    row.match_status != "APPROX") {
                    critical_unres = true;
                    break;
                }
            }
        }
        l4.grade = critical_unres ? exporters::korg::quality::AcceptanceGrade::C
                                  : exporters::korg::quality::AcceptanceGrade::U;
    } else {
        // Offline PASS alone ALWAYS lands at Grade U (Unverified) — NEVER Grade A or B!
        l4.grade = exporters::korg::quality::AcceptanceGrade::U;
    }

    // Strictly labeled uncalibrated offline diagnostic composite
    out.offline_composite = l1.passed
                                ? clamp0_100(0.55 * l2.decompressed_musical_score + 0.45 * l3.score_0_100)
                                : 0.0;
    return out;
}

// Mandatory Anti-Degradation & AI-Rejection Gate comparing any Mode B/C/AI candidate against untouched Mode A.
inline CandidateGateDecision evaluateCandidateAcceptance(
    const exporters::korg::Style& faithful_style,
    const exporters::korg::Report& faithful_report,
    const ReorchestrateResult& candidate,
    const TargetCapabilityProfile& target_profile,
    const RejectionGateOptions& opt = {},
    const exporters::korg::quality::HumanListeningRecord* listening = nullptr,
    const std::set<uint8_t>& locked_channels = {},
    const ContextSoundMatchResult* context_sounds = nullptr) {
    using namespace qi_detail;

    CandidateGateDecision dec{};
    dec.baseline_report = evaluateQualityIntelligence(
        faithful_style, faithful_report, target_profile, ArrangementMode::ModeA_Faithful,
        &faithful_style, locked_channels, listening, context_sounds);
    dec.candidate_report = evaluateQualityIntelligence(
        candidate.style, faithful_report, target_profile, candidate.effective_mode,
        &faithful_style, locked_channels, listening, context_sounds);

    dec.musical_delta = round2(dec.candidate_report.layer2_musical.decompressed_musical_score -
                               dec.baseline_report.layer2_musical.decompressed_musical_score);
    dec.target_delta  = round2(dec.candidate_report.layer3_target.score_0_100 -
                               dec.baseline_report.layer3_target.score_0_100);
    dec.ntt_conformance_delta =
        std::round((dec.candidate_report.layer2_musical.ntt_audit.chord_tone_conformance -
                    dec.baseline_report.layer2_musical.ntt_audit.chord_tone_conformance) * 10000.0) / 10000.0;
    dec.identity_preservation_score =
        dec.candidate_report.layer2_musical.musical_identity_preservation_score;

    // 1. Re-orchestrator already triggered fail-closed rollback or failed
    if (candidate.rolled_back_to_faithful || !candidate.ok) {
        dec.rejection_reasons.push_back(
            candidate.rollback_reason.empty()
                ? "Candidate rolled back to Mode A by deterministic re-orchestrator."
                : ("Re-orchestrator rollback: " + candidate.rollback_reason));
    }

    // 2. Layer 1 Technical Integrity Hard Gate (NO AI MAY BYPASS!)
    if (!dec.candidate_report.layer1_technical.passed) {
        dec.rejection_reasons.push_back(
            "Layer 1 Technical Integrity FAILED (" +
            std::to_string(dec.candidate_report.layer1_technical.errors.size()) + " hard violation(s)).");
        for (const auto& e : dec.candidate_report.layer1_technical.errors) {
            dec.rejection_reasons.push_back("  L1: " + e);
        }
    }

    // 3. Locked tracks bit-exact invariant
    if (opt.require_locked_tracks_bit_exact &&
        (!candidate.locked_tracks_bit_exact ||
         dec.candidate_report.layer1_technical.locked_track_mutations > 0u)) {
        dec.rejection_reasons.push_back("Locked-track bit-exact invariance violated.");
    }

    // 4. NTT chord conformance floor & anti-regression
    const double cand_ntt = dec.candidate_report.layer2_musical.ntt_audit.chord_tone_conformance;
    if (cand_ntt < opt.min_ntt_conformance && dec.ntt_conformance_delta < -0.001) {
        dec.rejection_reasons.push_back(
            "NTT chord conformance below floor (" + fmt4(cand_ntt) + " < " + fmt4(opt.min_ntt_conformance) + ").");
    } else if (dec.ntt_conformance_delta < -0.005) {
        dec.rejection_reasons.push_back(
            "NTT chord conformance regressed vs Mode A (delta=" + fmt4(dec.ntt_conformance_delta) + ").");
    }

    // 5. Critical / Warning finding regression guard
    if (opt.reject_any_new_critical &&
        dec.candidate_report.layer2_musical.critical_findings >
            dec.baseline_report.layer2_musical.critical_findings) {
        dec.rejection_reasons.push_back("Candidate introduced new Critical musical finding(s).");
    }
    if (opt.reject_any_new_warning &&
        dec.candidate_report.layer2_musical.warning_findings >
            dec.baseline_report.layer2_musical.warning_findings) {
        dec.rejection_reasons.push_back("Candidate introduced new Warning musical finding(s).");
    }

    // 6. Musical identity preservation guard
    if (dec.identity_preservation_score < opt.min_identity_preservation) {
        dec.rejection_reasons.push_back(
            "Musical identity preservation dropped below guard (" + fmt2(dec.identity_preservation_score) +
            " < " + fmt2(opt.min_identity_preservation) + ").");
    }

    // 7. Net improvement over Deterministic Mode A (rejects regressions & zero-value noise!)
    if (candidate.requested_mode != ArrangementMode::ModeA_Faithful) {
        const double combined_delta = round2(dec.musical_delta + dec.target_delta);
        const double req_gain = (candidate.requested_mode == ArrangementMode::ModeC_CreativeEnhancement)
                                    ? opt.min_net_gain_for_creative
                                    : opt.min_net_gain_for_adapt;
        if (dec.musical_delta < -0.10) {
            dec.rejection_reasons.push_back(
                "Layer 2 Musical Integrity regressed vs Mode A (delta=" + fmt2(dec.musical_delta) + ").");
        }
        if (dec.target_delta < -0.10) {
            dec.rejection_reasons.push_back(
                "Layer 3 Target Suitability regressed vs Mode A (delta=" + fmt2(dec.target_delta) + ").");
        }
        if (combined_delta < req_gain) {
            dec.rejection_reasons.push_back(
                "Candidate failed to beat Mode A by required margin (L2+L3 delta=" + fmt2(combined_delta) +
                " < " + fmt2(req_gain) + ") -> keeping deterministic Mode A.");
        }
    }

    dec.accepted = dec.rejection_reasons.empty();
    dec.fallback_to_faithful = !dec.accepted;
    return dec;
}

// Mandatory A/B/C AI-Rejection Gate selector: compares A (Faithful), B (Adapt), and C (Enhance),
// strictly keeping deterministic Mode A whenever B and C fail Layer 1 or fail to meaningfully beat A!
inline AbcSelectionDecision selectBestCandidate(
    const exporters::korg::Style& faithful_style,
    const exporters::korg::Report& faithful_report,
    const ReorchestrateResult& mode_b_result,
    const ReorchestrateResult& mode_c_result,
    const TargetCapabilityProfile& target_profile,
    const RejectionGateOptions& opt = {},
    const std::set<uint8_t>& locked_channels = {},
    const ContextSoundMatchResult* context_sounds = nullptr) {
    using namespace qi_detail;
    AbcSelectionDecision sel{};
    sel.mode_b_decision = evaluateCandidateAcceptance(
        faithful_style, faithful_report, mode_b_result, target_profile, opt, nullptr, locked_channels, context_sounds);
    sel.mode_c_decision = evaluateCandidateAcceptance(
        faithful_style, faithful_report, mode_c_result, target_profile, opt, nullptr, locked_channels, context_sounds);

    // Start with Deterministic Mode A as the authoritative default
    sel.selected_mode = ArrangementMode::ModeA_Faithful;
    sel.selected_label = arrangementModeName(sel.selected_mode);
    sel.deterministic_a_won = true;
    sel.selected_report = sel.mode_b_decision.baseline_report;
    double best_score = sel.selected_report.offline_composite;

    if (sel.mode_b_decision.accepted &&
        sel.mode_b_decision.candidate_report.offline_composite > best_score) {
        best_score = sel.mode_b_decision.candidate_report.offline_composite;
        sel.selected_mode = ArrangementMode::ModeB_IntelligentAdaptation;
        sel.selected_label = arrangementModeName(sel.selected_mode);
        sel.deterministic_a_won = false;
        sel.selected_report = sel.mode_b_decision.candidate_report;
    }

    if (sel.mode_c_decision.accepted &&
        sel.mode_c_decision.candidate_report.offline_composite > best_score + 0.05) {
        sel.selected_mode = ArrangementMode::ModeC_CreativeEnhancement;
        sel.selected_label = arrangementModeName(sel.selected_mode);
        sel.deterministic_a_won = false;
        sel.selected_report = sel.mode_c_decision.candidate_report;
    }

    if (sel.deterministic_a_won) {
        sel.rationale = "AI-Rejection Gate retained Deterministic Mode A (Faithful): neither Mode B nor Mode C "
                        "achieved net verified gain without regression.";
    } else if (sel.selected_mode == ArrangementMode::ModeB_IntelligentAdaptation) {
        sel.rationale = "Selected Mode B (IntelligentAdaptation) with L2 delta=" +
                        fmt2(sel.mode_b_decision.musical_delta) + ", L3 delta=" +
                        fmt2(sel.mode_b_decision.target_delta) + ", 0 hard violations.";
    } else {
        sel.rationale = "Selected Mode C (CreativeEnhancement) with L2 delta=" +
                        fmt2(sel.mode_c_decision.musical_delta) + ", L3 delta=" +
                        fmt2(sel.mode_c_decision.target_delta) + ", 0 hard violations.";
    }
    return sel;
}

inline std::string formatQualityIntelligenceJson(const QualityIntelligenceReport& r) {
    using namespace qi_detail;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"" << jsonEscapeStrict(r.schema) << "\",\n";
    o << "  \"styleName\": \"" << jsonEscapeStrict(r.style_name) << "\",\n";
    o << "  \"mode\": \"" << arrangementModeName(r.mode) << "\",\n";
    o << "  \"offlineComposite\": " << fmt2(r.offline_composite) << ",\n";
    o << "  \"independentFromFrozenQ1\": " << (r.independent_from_frozen_q1 ? "true" : "false") << ",\n";
    o << "  \"offlineScoreIsNotAudibleProof\": " << (r.offline_score_is_not_audible_proof ? "true" : "false") << ",\n";
    o << "  \"antiOverfittingWarning\": \"" << jsonEscapeStrict(r.anti_overfitting_warning) << "\",\n";
    o << "  \"layer1Technical\": {\n";
    o << "    \"passed\": " << (r.layer1_technical.passed ? "true" : "false") << ",\n";
    o << "    \"score\": " << fmt2(r.layer1_technical.score_0_100) << ",\n";
    o << "    \"korgValidateErrors\": " << r.layer1_technical.korg_validate_errors << ",\n";
    o << "    \"lockedTrackMutations\": " << r.layer1_technical.locked_track_mutations << ",\n";
    o << "    \"sameTickCcAfterNoteOn\": " << r.layer1_technical.same_tick_cc_after_note_on << ",\n";
    o << "    \"loopBoundaryViolations\": " << r.layer1_technical.loop_boundary_violations << ",\n";
    o << "    \"hangingNoteViolations\": " << r.layer1_technical.hanging_note_violations << ",\n";
    o << "    \"chordBandLeaks\": " << r.layer1_technical.chord_band_leaks << ",\n";
    o << "    \"polyphonyCapViolations\": " << r.layer1_technical.polyphony_cap_violations << ",\n";
    o << "    \"deterministicRoundtripOk\": " << (r.layer1_technical.deterministic_roundtrip_ok ? "true" : "false") << "\n";
    o << "  },\n";
    o << "  \"layer2Musical\": {\n";
    o << "    \"decompressedMusicalScore\": " << fmt2(r.layer2_musical.decompressed_musical_score) << ",\n";
    o << "    \"baseMq12Overall\": " << fmt2(r.layer2_musical.base_mq12.overall_musical_score) << ",\n";
    o << "    \"saturatedDimensionsAt100\": " << r.layer2_musical.saturated_dimensions_at_100 << ",\n";
    o << "    \"nttChordToneConformance\": " << fmt4(r.layer2_musical.ntt_audit.chord_tone_conformance) << ",\n";
    o << "    \"nttBassRootAccuracy\": " << fmt4(r.layer2_musical.ntt_audit.bass_root_accuracy) << ",\n";
    o << "    \"harmonicCorrectness\": " << fmt2(r.layer2_musical.harmonic_correctness_score) << ",\n";
    o << "    \"bassBehavior\": " << fmt2(r.layer2_musical.bass_behavior_score) << ",\n";
    o << "    \"groovePreservation\": " << fmt2(r.layer2_musical.groove_preservation_score) << ",\n";
    o << "    \"phraseContinuity\": " << fmt2(r.layer2_musical.phrase_continuity_score) << ",\n";
    o << "    \"arrangementCoherence\": " << fmt2(r.layer2_musical.arrangement_coherence_score) << ",\n";
    o << "    \"trackRolePreservation\": " << fmt2(r.layer2_musical.track_role_preservation_score) << ",\n";
    o << "    \"sectionDifferentiation\": " << fmt2(r.layer2_musical.section_differentiation_score) << ",\n";
    o << "    \"musicalIdentityPreservation\": " << fmt2(r.layer2_musical.musical_identity_preservation_score) << ",\n";
    o << "    \"criticalFindings\": " << r.layer2_musical.critical_findings << ",\n";
    o << "    \"warningFindings\": " << r.layer2_musical.warning_findings << "\n";
    o << "  },\n";
    o << "  \"layer3TargetSuitability\": {\n";
    o << "    \"targetPlatform\": \"" << jsonEscapeStrict(targetPlatformKey(r.layer3_target.target_platform)) << "\",\n";
    o << "    \"score\": " << fmt2(r.layer3_target.score_0_100) << ",\n";
    o << "    \"instrumentCompatibility\": " << fmt2(r.layer3_target.instrument_compatibility_score) << ",\n";
    o << "    \"registerSuitability\": " << fmt2(r.layer3_target.register_suitability_score) << ",\n";
    o << "    \"articulationCompatibility\": " << fmt2(r.layer3_target.articulation_compatibility_score) << ",\n";
    o << "    \"arrangementBalance\": " << fmt2(r.layer3_target.arrangement_balance_score) << ",\n";
    o << "    \"soundMappingConfidence\": " << fmt2(r.layer3_target.sound_mapping_confidence_score) << ",\n";
    o << "    \"targetCapabilityUtilization\": " << fmt2(r.layer3_target.target_capability_utilization_score) << ",\n";
    o << "    \"capabilityCapViolations\": " << r.layer3_target.capability_cap_violations << ",\n";
    o << "    \"unresolvedSoundCount\": " << r.layer3_target.unresolved_sound_count << ",\n";
    o << "    \"registerOutOfIdiomNotes\": " << r.layer3_target.register_out_of_idiom_notes << "\n";
    o << "  },\n";
    o << "  \"layer4HumanPreference\": {\n";
    o << "    \"status\": \"" << jsonEscapeStrict(r.layer4_human.preference_hooks.status) << "\",\n";
    o << "    \"hardwareVerified\": false,\n";
    o << "    \"l3Status\": \"" << jsonEscapeStrict(r.layer4_human.l3_status) << "\",\n";
    o << "    \"audibleProofAvailable\": false,\n";
    o << "    \"grade\": \"" << exporters::korg::quality::acceptanceGradeCode(r.layer4_human.grade) << "\",\n";
    o << "    \"humanRecordPresent\": " << (r.layer4_human.human_record_present ? "true" : "false") << ",\n";
    o << "    \"pairwiseComparisonsCount\": " << r.layer4_human.preference_hooks.pairwise_comparisons_count << ",\n";
    o << "    \"expertEvaluationsCount\": " << r.layer4_human.preference_hooks.expert_evaluations_count << ",\n";
    o << "    \"epistemicNotice\": \"" << jsonEscapeStrict(r.layer4_human.epistemic_notice) << "\"\n";
    o << "  }\n";
    o << "}\n";
    return o.str();
}

inline std::string formatQualityIntelligenceMd(const QualityIntelligenceReport& r) {
    using namespace qi_detail;
    std::ostringstream o;
    o << "# 4-Layer Quality Intelligence Report — " << r.style_name << "\n\n";
    o << "> **Epistemic Warning:** `" << r.anti_overfitting_warning << "`\n\n";
    o << "- **Mode:** `" << arrangementModeName(r.mode) << "`"
      << " | **Target:** `" << targetPlatformKey(r.layer3_target.target_platform) << "`"
      << " | **Offline Diagnostic Composite:** `" << fmt2(r.offline_composite) << " / 100.00`\n";
    o << "- **Epistemic Lock:** `hardwareVerified: false` | `l3Status: " << r.layer4_human.l3_status
      << "` | `Layer 4 Human Preference: " << r.layer4_human.preference_hooks.status
      << "` | `Grade: " << exporters::korg::quality::acceptanceGradeCode(r.layer4_human.grade) << "`\n\n";
    o << "| Layer | Score / Status | Key Diagnostics |\n";
    o << "|---|---|---|\n";
    o << "| **Layer 1: Technical Integrity (Hard Gate)** | `"
      << (r.layer1_technical.passed ? "PASS" : "FAIL") << " (" << fmt2(r.layer1_technical.score_0_100) << ")`"
      << " | korgErr=" << r.layer1_technical.korg_validate_errors
      << ", lockedMut=" << r.layer1_technical.locked_track_mutations
      << ", ccAfterOn=" << r.layer1_technical.same_tick_cc_after_note_on
      << ", loopViol=" << r.layer1_technical.loop_boundary_violations
      << ", hang=" << r.layer1_technical.hanging_note_violations << " |\n";
    o << "| **Layer 2: Musical Integrity (Decompressed)** | `"
      << fmt2(r.layer2_musical.decompressed_musical_score) << "`"
      << " | MQ12=" << fmt2(r.layer2_musical.base_mq12.overall_musical_score)
      << " (" << r.layer2_musical.saturated_dimensions_at_100 << "/12 @100), NTT="
      << fmt4(r.layer2_musical.ntt_audit.chord_tone_conformance)
      << ", identity=" << fmt2(r.layer2_musical.musical_identity_preservation_score) << " |\n";
    o << "| **Layer 3: Target Suitability** | `"
      << fmt2(r.layer3_target.score_0_100) << "`"
      << " | capViol=" << r.layer3_target.capability_cap_violations
      << ", unres=" << r.layer3_target.unresolved_sound_count
      << ", outOfIdiom=" << r.layer3_target.register_out_of_idiom_notes << " |\n";
    o << "| **Layer 4: Human Preference** | **`" << r.layer4_human.preference_hooks.status << "`**"
      << " | `hardwareVerified=false`, `audibleProofAvailable=false` |\n";
    return o.str();
}

} // namespace ai_arranger::intelligence

#endif // AI_ARRANGER_INTELLIGENCE_QUALITY_INTELLIGENCE_H
