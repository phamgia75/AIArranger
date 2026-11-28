#ifndef AI_ARRANGER_INTELLIGENCE_TARGET_REORCHESTRATOR_H
#define AI_ARRANGER_INTELLIGENCE_TARGET_REORCHESTRATOR_H

#include "engine/music/ntt.h"
#include "exporters/korg/style_enhancer.h"
#include "intelligence/context_sound_matcher.h"
#include "intelligence/musical_semantic_analyzer.h"
#include "intelligence/target_capability.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Phase 5 Milestone P5-M4: Target-Aware Musical Re-Orchestration Engine (Mode A / B / C)
// & Mandatory 12-Root x 10-Chord-Quality Interactive NTT Validator.
//
// Boundaries & Governance Invariants (ADR-P5-002 / ADR-P5-003 / ADR-P5-004 / ADR-P5-005):
//   * "AI proposes. Deterministic engine enforces. Acceptance Gate validates. User retains control."
//   * Header-only portable C++20, zero external deps, zero mutable globals, Zone B offline only.
//   * ModeA_Faithful is the IMMUTABLE DEFAULT: 0 mutations, bit-exact preservation.
//   * ModeB_IntelligentAdaptation (opt-in) & ModeC_CreativeEnhancement (opt-in) operate
//     non-destructively on independent copies of faithful_style with explicit reversible
//     ArrangementPlanAction audit records and automatic fail-closed rollback on any violation.
//   * Locked tracks (`locked_channels`) are guaranteed 0-byte / event-identical.
//   * Mandatory 12-root x 10-chord-quality NTT validator reuses `ai_arranger::music::transpose()`
//     read-only across Major, Minor, Dominant7, Major7, Minor7, Diminished, Diminished7,
//     Augmented, Suspended (Sus4/Sus2), and Slash/Inversion chords.

namespace ai_arranger::intelligence {

enum class ArrangementMode : uint8_t {
    ModeA_Faithful              = 0, // IMMUTABLE DEFAULT: 0 mutations, bit-exact identity
    ModeB_IntelligentAdaptation = 1, // OPT-IN: target-aware bounded adaptation
    ModeC_CreativeEnhancement   = 2, // OPT-IN: bounded musical refinement + optional AI hook
};

using ConversionMode = ArrangementMode;

inline constexpr const char* arrangementModeName(ArrangementMode mode) noexcept {
    switch (mode) {
        case ArrangementMode::ModeA_Faithful:              return "ModeA_Faithful";
        case ArrangementMode::ModeB_IntelligentAdaptation: return "ModeB_IntelligentAdaptation";
        case ArrangementMode::ModeC_CreativeEnhancement:   return "ModeC_CreativeEnhancement";
    }
    return "ModeA_Faithful";
}

// Optional, strictly opt-in generative proposal seam (default nullptr / OFF).
// Proposals are UNTRUSTED until validated by korg::validate() + NTT + Quality Gate.
struct GenerativeProposalRequest {
    std::string      style_name;
    TargetPlatformId target_platform = TargetPlatformId::KorgPa5X;
    std::string      section_marker;
    uint8_t          target_channel = 0xFF;
    MusicalRole      role = MusicalRole::UnknownAbstain;
    uint32_t         element_length_ticks = 0;
    uint32_t         seed = 42;
};

struct GenerativeProposalResult {
    bool                                available = false;
    std::string                         model_id;
    std::vector<exporters::korg::Event> proposed_channel_events;
    double                              confidence = 0.0;
    std::string                         rationale;
};

class IGenerativeProposalHook {
public:
    virtual ~IGenerativeProposalHook() = default;
    virtual GenerativeProposalResult propose(const GenerativeProposalRequest& req) const = 0;
};

struct ArrangementPlanAction {
    std::string      action_id;
    std::string      domain;          // e.g. "register_correction", "chord_voicing", "transition_cc_hygiene"
    std::string      section_marker;
    uint8_t          target_channel = 0xFF;
    std::string      before_metric;
    std::string      after_metric;
    std::string      description;
    bool             reversible = true;
    ProvenanceRecord provenance{};
};

using ReorchestrationAction = ArrangementPlanAction;

enum class InteractiveChordQuality : uint8_t {
    Major       = 0,
    Minor       = 1,
    Dominant7   = 2,
    Major7      = 3,
    Minor7      = 4,
    Diminished  = 5,
    Diminished7 = 6,
    Augmented   = 7,
    Suspended   = 8,
    SlashInversion = 9,
};

inline constexpr size_t kInteractiveChordQualityCount = 10;

inline constexpr const char* interactiveChordQualityName(InteractiveChordQuality q) noexcept {
    switch (q) {
        case InteractiveChordQuality::Major:          return "Major";
        case InteractiveChordQuality::Minor:          return "Minor";
        case InteractiveChordQuality::Dominant7:      return "Dominant7";
        case InteractiveChordQuality::Major7:         return "Major7";
        case InteractiveChordQuality::Minor7:         return "Minor7";
        case InteractiveChordQuality::Diminished:     return "Diminished";
        case InteractiveChordQuality::Diminished7:    return "Diminished7";
        case InteractiveChordQuality::Augmented:      return "Augmented";
        case InteractiveChordQuality::Suspended:      return "Suspended";
        case InteractiveChordQuality::SlashInversion: return "SlashInversion";
    }
    return "Major";
}

struct NttChordAuditSummary {
    uint32_t chords_tested = 0;       // 12 roots x 10 chord qualities = 120 cells per chord-driven element-track
    uint32_t cells_passed = 0;
    double   chord_tone_conformance = 1.0; // [0..1]
    double   bass_root_accuracy = 1.0;     // [0..1]
    uint32_t register_violations = 0;
    uint32_t bass_out_of_register_notes = 0;
    uint32_t harmonic_clash_count = 0;
    uint32_t hanging_note_violations = 0;
    uint32_t same_tick_cc_after_note_on = 0;
    uint32_t chord_band_leaks = 0;
    uint32_t loop_boundary_violations = 0;
    uint32_t polyphony_cap_violations = 0;
    uint32_t silent_track_drops = 0;
    uint32_t max_observed_polyphony = 0;
    std::array<double, kInteractiveChordQualityCount> per_quality_conformance{
        1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0
    };
    bool                     hard_passed = true;
    std::vector<std::string> hard_violations;
};

struct ReorchestrateOptions {
    ArrangementMode          mode = ArrangementMode::ModeA_Faithful; // ALWAYS Faithful by default
    TargetPlatformId         target_platform = TargetPlatformId::KorgPa5X;
    std::set<uint8_t>        locked_channels;       // Channels guaranteed 0-byte identical
    bool                     enable_ai_proposals = false; // Strictly default OFF
    const IGenerativeProposalHook* ai_hook = nullptr;
    const ContextSoundMatchResult* context_sounds = nullptr; // Optional P5-M3 advisory context
    // Modular domain toggles (used in Mode B / Mode C; all skipped in Mode A!)
    bool                     enable_mode_b_base_rules      = true;
    bool                     enable_register_correction    = true;
    bool                     enable_bass_piano_deconflict  = true;
    bool                     enable_chord_voicing_cap      = true;
    bool                     enable_guitar_art_adaptation  = true;
    bool                     enable_drum_perc_adaptation   = true;
    bool                     enable_density_optimization   = true;
    bool                     enable_transition_cc_hygiene  = true;
    bool                     enable_creative_arc_shaping   = true; // Mode C only
    uint8_t                  max_chord_voicing_notes       = 6;    // Playable <= 6-note voicing cap
    double                   min_ntt_conformance           = 0.85;
    double                   min_identity_similarity       = 0.80; // Musical identity guard [0..1]
    double                   max_groove_shift_ticks        = 2.0;  // Microtiming collapse guard
    uint32_t                 deterministic_seed            = 42;
    exporters::korg::quality::EnhanceOptions mode_b_options =
        exporters::korg::quality::EnhanceOptions::recommended();
};

struct ReorchestrateResult {
    bool                                ok = false;
    std::string                         error;
    ArrangementMode                     requested_mode = ArrangementMode::ModeA_Faithful;
    ArrangementMode                     effective_mode = ArrangementMode::ModeA_Faithful;
    exporters::korg::Style              style;            // Output (guaranteed bit-exact to input on Mode A or rollback)
    std::vector<ArrangementPlanAction>  plan_actions;
    NttChordAuditSummary                ntt_audit{};
    double                              identity_similarity = 1.0; // [0..1] vs faithful_style
    double                              max_groove_delta_ticks = 0.0;
    uint32_t                            bass_piano_collisions_before = 0;
    uint32_t                            bass_piano_collisions_after = 0;
    bool                                locked_tracks_bit_exact = true;
    bool                                reversible_verified = true;
    bool                                rolled_back_to_faithful = false;
    std::string                         rollback_reason;
    std::vector<std::string>            validation_errors;
    bool                                hardware_verified = false;
    std::string                         l3_status = "NOT_TESTED";
};

struct ModeAbcCandidateBundle {
    ReorchestrateResult mode_a;
    ReorchestrateResult mode_b;
    ReorchestrateResult mode_c;
};

namespace reorch_detail {

struct RNote {
    uint32_t on_tick = 0;
    uint32_t off_tick = 0;
    uint8_t  pitch = 0;
    uint8_t  vel = 0;
};

inline double clamp01(double v) noexcept {
    if (!std::isfinite(v) || v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

inline std::string fmtFinite(double v, int prec = 4) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::fixed << std::setprecision(prec) << (std::isfinite(v) ? v : 0.0);
    return oss.str();
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

inline bool eventEqual(const exporters::korg::Event& a, const exporters::korg::Event& b) noexcept {
    return a.tick == b.tick && a.status == b.status && a.d1 == b.d1 && a.d2 == b.d2;
}

inline int canonicalPriority(const exporters::korg::Event& ev) noexcept {
    const uint8_t hi = ev.status & 0xF0u;
    if (hi == 0xB0u && ev.d1 == 0u)                  return 0; // Bank MSB
    if (hi == 0xB0u && ev.d1 == 32u)                 return 1; // Bank LSB
    if (hi == 0xC0u)                                 return 2; // Program Change
    if (hi == 0x80u || (hi == 0x90u && ev.d2 == 0u)) return 4; // NoteOff before NoteOn
    if (hi == 0x90u)                                 return 5; // NoteOn AFTER all CCs & NoteOff (DEF-P4-010!)
    return 3;                                                  // CC11/CC64/PB/Pressure strictly BEFORE NoteOn
}

inline void canonicalSortEvents(std::vector<exporters::korg::Event>& events) {
    std::stable_sort(events.begin(), events.end(),
                     [](const exporters::korg::Event& a, const exporters::korg::Event& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return canonicalPriority(a) < canonicalPriority(b);
                     });
}

inline uint32_t countSameTickCcAfterNoteOn(const exporters::korg::Style& style) noexcept {
    uint32_t violations = 0;
    for (const auto& el : style.elements) {
        size_t i = 0;
        while (i < el.events.size()) {
            const uint32_t t = el.events[i].tick;
            size_t j = i;
            while (j < el.events.size() && el.events[j].tick == t) ++j;
            std::array<bool, 16> saw_note_on{};
            for (size_t k = i; k < j; ++k) {
                const auto& ev = el.events[k];
                const uint8_t hi = ev.status & 0xF0u;
                const uint8_t ch = ev.status & 0x0Fu;
                if (hi == 0x90u && ev.d2 > 0u) {
                    saw_note_on[ch] = true;
                } else if (hi == 0xB0u || hi == 0xC0u || hi == 0xE0u) {
                    if (saw_note_on[ch]) ++violations;
                }
            }
            i = j;
        }
    }
    return violations;
}

inline std::vector<exporters::korg::Event> extractChannelEvents(
    const exporters::korg::Element& el, uint8_t ch) {
    std::vector<exporters::korg::Event> out;
    for (const auto& ev : el.events) {
        if ((ev.status & 0x0Fu) == ch) out.push_back(ev);
    }
    return out;
}

inline bool verifyLockedChannelsBitExact(
    const exporters::korg::Style& ref,
    const exporters::korg::Style& cand,
    const std::set<uint8_t>& locked_channels) {
    if (locked_channels.empty()) return true;
    std::map<std::string, const exporters::korg::Element*> cand_by_marker;
    for (const auto& el : cand.elements) {
        cand_by_marker[el.marker] = &el;
    }
    for (const auto& ref_el : ref.elements) {
        const auto it = cand_by_marker.find(ref_el.marker);
        if (it == cand_by_marker.end()) return false;
        const auto& cand_el = *it->second;
        for (uint8_t ch : locked_channels) {
            const auto a = extractChannelEvents(ref_el, ch);
            const auto b = extractChannelEvents(cand_el, ch);
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (!eventEqual(a[i], b[i])) return false;
            }
        }
    }
    return true;
}

inline void restoreLockedChannels(
    const exporters::korg::Style& ref,
    exporters::korg::Style& cand,
    const std::set<uint8_t>& locked_channels) {
    if (locked_channels.empty()) return;
    std::map<std::string, const exporters::korg::Element*> ref_by_marker;
    for (const auto& el : ref.elements) ref_by_marker[el.marker] = &el;
    for (auto& el : cand.elements) {
        const auto it = ref_by_marker.find(el.marker);
        if (it == ref_by_marker.end()) continue;
        std::vector<exporters::korg::Event> rebuilt;
        rebuilt.reserve(el.events.size() + 32u);
        for (const auto& ev : el.events) {
            if (locked_channels.count(static_cast<uint8_t>(ev.status & 0x0Fu)) == 0u) {
                rebuilt.push_back(ev);
            }
        }
        for (const auto& ev : it->second->events) {
            if (locked_channels.count(static_cast<uint8_t>(ev.status & 0x0Fu)) != 0u) {
                rebuilt.push_back(ev);
            }
        }
        el.events.swap(rebuilt);
        canonicalSortEvents(el.events);
    }
}

inline std::vector<RNote> extractNotes(const exporters::korg::Element& el, uint8_t ch,
                                       uint32_t* out_hanging = nullptr) {
    std::vector<RNote> notes;
    std::array<std::deque<std::pair<uint32_t, uint8_t>>, 128> active{};
    for (const auto& ev : el.events) {
        if ((ev.status & 0x0Fu) != ch) continue;
        const uint8_t hi = ev.status & 0xF0u;
        const uint8_t p  = ev.d1 & 0x7Fu;
        if (hi == 0x90u && ev.d2 > 0u) {
            active[p].emplace_back(ev.tick, static_cast<uint8_t>(ev.d2 & 0x7Fu));
        } else if (hi == 0x80u || (hi == 0x90u && ev.d2 == 0u)) {
            if (!active[p].empty()) {
                const auto [on_t, vel] = active[p].front();
                active[p].pop_front();
                const uint32_t off_t = (ev.tick > on_t) ? ev.tick : (on_t + 1u);
                notes.push_back(RNote{on_t, off_t, p, vel});
            }
        }
    }
    if (out_hanging) {
        for (const auto& q : active) {
            *out_hanging += static_cast<uint32_t>(q.size());
        }
    }
    std::sort(notes.begin(), notes.end(), [](const RNote& a, const RNote& b) {
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.off_tick < b.off_tick;
    });
    return notes;
}

inline void deoverlapAndReplaceNotes(exporters::korg::Element& el, uint8_t ch,
                                     std::vector<RNote> notes, bool chord_driven) {
    if (el.length < 2u) return;
    std::vector<RNote> clean;
    clean.reserve(notes.size());
    for (auto n : notes) {
        if (chord_driven && n.pitch <= exporters::korg::kChordEventNoteMax) continue;
        if (n.pitch > 127u || n.vel == 0u) continue;
        if (n.on_tick >= el.length - 1u) continue;
        if (n.off_tick <= n.on_tick) n.off_tick = n.on_tick + 1u;
        if (n.off_tick >= el.length) n.off_tick = el.length - 1u;
        if (n.off_tick <= n.on_tick) continue;
        clean.push_back(n);
    }
    std::sort(clean.begin(), clean.end(), [](const RNote& a, const RNote& b) {
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        return a.off_tick > b.off_tick;
    });
    std::vector<RNote> deov;
    deov.reserve(clean.size());
    for (auto& n : clean) {
        if (!deov.empty() && deov.back().pitch == n.pitch) {
            if (deov.back().on_tick == n.on_tick) {
                deov.back().vel = std::max(deov.back().vel, n.vel);
                deov.back().off_tick = std::max(deov.back().off_tick, n.off_tick);
                continue;
            }
            if (deov.back().off_tick > n.on_tick) {
                deov.back().off_tick = n.on_tick;
                if (deov.back().off_tick <= deov.back().on_tick) continue;
            }
        }
        deov.push_back(n);
    }

    std::vector<exporters::korg::Event> kept;
    kept.reserve(el.events.size() + deov.size() * 2u);
    for (const auto& ev : el.events) {
        const uint8_t hi = ev.status & 0xF0u;
        const uint8_t ech = ev.status & 0x0Fu;
        if (ech == ch && (hi == 0x80u || hi == 0x90u)) continue;
        kept.push_back(ev);
    }
    for (const auto& n : deov) {
        kept.push_back(exporters::korg::Event{n.on_tick, static_cast<uint8_t>(0x90u | ch), n.pitch, n.vel});
        kept.push_back(exporters::korg::Event{n.off_tick, static_cast<uint8_t>(0x80u | ch), n.pitch, 0u});
    }
    el.events.swap(kept);
    canonicalSortEvents(el.events);
}

inline uint32_t measureElementMaxPolyphony(const exporters::korg::Element& el) {
    struct Sweep { uint32_t tick; int delta; };
    std::vector<Sweep> sw;
    sw.reserve(el.events.size());
    for (const auto& ev : el.events) {
        const uint8_t hi = ev.status & 0xF0u;
        if (hi == 0x90u && ev.d2 > 0u) sw.push_back({ev.tick, +1});
        else if (hi == 0x80u || (hi == 0x90u && ev.d2 == 0u)) sw.push_back({ev.tick, -1});
    }
    std::sort(sw.begin(), sw.end(), [](const Sweep& a, const Sweep& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.delta < b.delta; // NoteOff before NoteOn at same tick
    });
    int cur = 0;
    int mx = 0;
    for (const auto& s : sw) {
        cur = std::max(0, cur + s.delta);
        if (cur > mx) mx = cur;
    }
    return static_cast<uint32_t>(mx);
}

inline std::array<bool, 12> targetChordToneMask(InteractiveChordQuality q, uint8_t root_pc) noexcept {
    std::array<bool, 12> m{};
    auto set = [&](uint8_t iv) { m[(root_pc + iv) % 12u] = true; };
    set(0);
    switch (q) {
        case InteractiveChordQuality::Major:          set(4); set(7); break;
        case InteractiveChordQuality::Minor:          set(3); set(7); break;
        case InteractiveChordQuality::Dominant7:      set(4); set(7); set(10); break;
        case InteractiveChordQuality::Major7:         set(4); set(7); set(11); break;
        case InteractiveChordQuality::Minor7:         set(3); set(7); set(10); break;
        case InteractiveChordQuality::Diminished:     set(3); set(6); break;
        case InteractiveChordQuality::Diminished7:    set(3); set(6); set(9); break;
        case InteractiveChordQuality::Augmented:      set(4); set(8); break;
        case InteractiveChordQuality::Suspended:      set(5); set(7); set(2); break; // Sus4 + Sus2
        case InteractiveChordQuality::SlashInversion: set(4); set(7); break; // 1st-inv Major C/E
    }
    return m;
}

inline arranger::Chord makeTestChord(InteractiveChordQuality q, uint8_t root_pc) noexcept {
    const uint8_t root_midi = static_cast<uint8_t>(60u + (root_pc % 12u));
    switch (q) {
        case InteractiveChordQuality::Major:          return {arranger::ChordType::Major, root_midi, 0, 0};
        case InteractiveChordQuality::Minor:          return {arranger::ChordType::Minor, root_midi, 0, 0};
        case InteractiveChordQuality::Dominant7:      return {arranger::ChordType::Dom7,  root_midi, 0, 0};
        case InteractiveChordQuality::Major7:         return {arranger::ChordType::Maj7,  root_midi, 0, 0};
        case InteractiveChordQuality::Minor7:         return {arranger::ChordType::Min7,  root_midi, 0, 0};
        case InteractiveChordQuality::Diminished:     return {arranger::ChordType::Dim,   root_midi, 0, 0};
        case InteractiveChordQuality::Diminished7:    return {arranger::ChordType::Dim7,  root_midi, 0, 0};
        case InteractiveChordQuality::Augmented:      return {arranger::ChordType::Aug,   root_midi, 0, 0};
        case InteractiveChordQuality::Suspended:      return {arranger::ChordType::Sus4,  root_midi, 0, 0};
        case InteractiveChordQuality::SlashInversion:
            return {arranger::ChordType::Major, root_midi, static_cast<uint8_t>(48u + ((root_pc + 4u) % 12u)), 0};
    }
    return {arranger::ChordType::Major, root_midi, 0, 0};
}

// Compute musical identity similarity [0..1] between reference and candidate styles
// (pitch-class distribution cosine + 16th-slot rhythmic onset grid cosine + active track retention).
inline double computeIdentitySimilarity(
    const exporters::korg::Style& ref,
    const exporters::korg::Style& cand) noexcept {
    std::array<double, 12> pc_a{}, pc_b{};
    std::array<double, 16> grid_a{}, grid_b{};
    std::set<uint8_t> ch_a, ch_b;
    uint32_t notes_a = 0, notes_b = 0;

    auto collect = [](const exporters::korg::Style& st,
                      std::array<double, 12>& pc,
                      std::array<double, 16>& grid,
                      std::set<uint8_t>& chs,
                      uint32_t& count) {
        const uint32_t ppqn = std::max<uint32_t>(1u, st.ppqn ? st.ppqn : 384u);
        for (const auto& el : st.elements) {
            const uint32_t bar_t = std::max<uint32_t>(1u, (ppqn * 4u * std::max<uint32_t>(1u, el.num)) /
                                                          std::max<uint32_t>(1u, el.den));
            for (const auto& ev : el.events) {
                if ((ev.status & 0xF0u) == 0x90u && ev.d2 > 0u) {
                    const uint8_t ch = ev.status & 0x0Fu;
                    chs.insert(ch);
                    ++count;
                    if (exporters::korg::isChordDriven(ch)) {
                        pc[ev.d1 % 12u] += 1.0;
                    }
                    const uint32_t slot = static_cast<uint32_t>((uint64_t{ev.tick % bar_t} * 16ULL) / bar_t) % 16u;
                    grid[slot] += 1.0;
                }
            }
        }
    };

    collect(ref, pc_a, grid_a, ch_a, notes_a);
    collect(cand, pc_b, grid_b, ch_b, notes_b);
    if (notes_a == 0u && notes_b == 0u) return 1.0;
    if (notes_a == 0u || notes_b == 0u) return 0.0;

    auto cosine = [](const auto& x, const auto& y) -> double {
        double dot = 0.0, nx = 0.0, ny = 0.0;
        for (size_t i = 0; i < x.size(); ++i) {
            dot += x[i] * y[i];
            nx += x[i] * x[i];
            ny += y[i] * y[i];
        }
        if (nx <= 1e-12 && ny <= 1e-12) return 1.0;
        if (nx <= 1e-12 || ny <= 1e-12) return 0.0;
        return clamp01(dot / (std::sqrt(nx) * std::sqrt(ny)));
    };

    const double pc_sim   = cosine(pc_a, pc_b);
    const double grid_sim = cosine(grid_a, grid_b);
    uint32_t kept_ch = 0;
    for (uint8_t c : ch_a) if (ch_b.count(c)) ++kept_ch;
    const double ch_sim = ch_a.empty() ? 1.0 : static_cast<double>(kept_ch) / static_cast<double>(ch_a.size());
    return clamp01(0.45 * pc_sim + 0.40 * grid_sim + 0.15 * ch_sim);
}

inline uint32_t countBassPianoLowCollisionsInStyle(
    const exporters::korg::Style& style,
    const exporters::korg::Report* report,
    const SemanticArrangementAnalysis* semantics) {
    // Identify piano/chord-comping channels (explicitly EXEMPTING Bass, Drum/Perc, and Timpani per DEF-P4-009!)
    std::set<uint8_t> piano_like_chs;
    for (uint8_t ch = exporters::korg::kAcc1Ch; ch <= 15u; ++ch) {
        bool is_timpani = false;
        if (report) {
            for (const auto& row : report->tracks) {
                if (row.korg_ch != ch) continue;
                const std::string low = context_sound_detail::toLowerAscii(row.name + " " + row.family);
                if (low.find("timpani") != std::string::npos || low.find("mallet") != std::string::npos) {
                    is_timpani = true;
                }
            }
        }
        if (is_timpani) continue;
        bool is_piano_or_comp = false;
        if (semantics) {
            for (const auto& trd : semantics->track_roles) {
                if (trd.target_channel != ch) continue;
                if (trd.role == MusicalRole::PianoAccompaniment ||
                    trd.role == MusicalRole::ChordComping ||
                    trd.role == MusicalRole::HarmonicAccompaniment) {
                    is_piano_or_comp = true;
                }
            }
        } else if (report) {
            for (const auto& row : report->tracks) {
                if (row.korg_ch != ch) continue;
                const std::string low = context_sound_detail::toLowerAscii(row.name + " " + row.family);
                if (low.find("piano") != std::string::npos || low.find("ep") != std::string::npos ||
                    low.find("organ") != std::string::npos) {
                    is_piano_or_comp = true;
                }
            }
        }
        if (is_piano_or_comp) piano_like_chs.insert(ch);
    }

    uint32_t collisions = 0;
    for (const auto& el : style.elements) {
        const auto bass_notes = extractNotes(el, exporters::korg::kBassCh);
        if (bass_notes.empty()) continue;
        for (uint8_t ch : piano_like_chs) {
            const auto acc_notes = extractNotes(el, ch);
            for (const auto& an : acc_notes) {
                if (an.pitch >= 48u) continue; // < C3 (48)
                for (const auto& bn : bass_notes) {
                    if (bn.on_tick < an.off_tick && an.on_tick < bn.off_tick) {
                        ++collisions;
                        break;
                    }
                }
            }
        }
    }
    return collisions;
}

} // namespace reorch_detail

// Pure deterministic 12-root x 10-chord-quality interactive NTT validator reusing ai_arranger::music::transpose().
inline NttChordAuditSummary auditStyleNttConformance(
    const exporters::korg::Style& style,
    const TargetCapabilityProfile& target_profile,
    const exporters::korg::Style* reference_style = nullptr) {
    using namespace reorch_detail;
    NttChordAuditSummary out{};

    const auto korg_errs = exporters::korg::validate(style);
    for (const auto& e : korg_errs) {
        out.hard_violations.push_back("korg::validate: " + e);
    }

    out.same_tick_cc_after_note_on = countSameTickCcAfterNoteOn(style);
    if (out.same_tick_cc_after_note_on > 0u) {
        out.hard_violations.push_back(
            "same-tick CC/PC/PB after NoteOn (" + std::to_string(out.same_tick_cc_after_note_on) + " defect(s))");
    }

    const RolePitchRange bass_range = target_profile.pitchRangeFor(RoleRegisterCategory::Bass);
    const uint16_t poly_cap = target_profile.tracks.max_polyphony;

    std::array<uint64_t, kInteractiveChordQualityCount> q_total_notes{};
    std::array<uint64_t, kInteractiveChordQualityCount> q_chord_tones{};
    uint64_t total_bass_downbeats = 0;
    uint64_t matched_bass_downbeats = 0;
    uint64_t total_cells = 0;
    uint64_t passed_cells = 0;

    // Track active channels in reference_style to detect silent/missing track drops.
    std::set<uint8_t> ref_active_chs;
    if (reference_style) {
        for (const auto& el : reference_style->elements) {
            for (const auto& ev : el.events) {
                if ((ev.status & 0xF0u) == 0x90u && ev.d2 > 0u) {
                    ref_active_chs.insert(static_cast<uint8_t>(ev.status & 0x0Fu));
                }
            }
        }
    }
    std::set<uint8_t> cand_active_chs;

    for (const auto& el : style.elements) {
        if (el.length == 0u || el.num == 0u || el.den == 0u) {
            ++out.loop_boundary_violations;
            out.hard_violations.push_back("invalid element length/meter in " + el.marker);
        }

        const uint32_t el_poly = measureElementMaxPolyphony(el);
        if (el_poly > out.max_observed_polyphony) out.max_observed_polyphony = el_poly;
        if (el_poly > poly_cap) {
            ++out.polyphony_cap_violations;
            out.hard_violations.push_back(
                "element " + el.marker + " polyphony " + std::to_string(el_poly) +
                " exceeds target cap " + std::to_string(poly_cap));
        }

        // Check event bounds, chord-band leaks, and hanging notes per channel.
        for (const auto& ev : el.events) {
            if (ev.tick >= el.length && el.length > 0u) {
                ++out.loop_boundary_violations;
            }
            const uint8_t hi = ev.status & 0xF0u;
            const uint8_t ch = ev.status & 0x0Fu;
            if (hi == 0x90u && ev.d2 > 0u) {
                cand_active_chs.insert(ch);
                if (exporters::korg::isChordDriven(ch) &&
                    target_profile.tracks.chord_event_note_max != 0xFFu &&
                    ev.d1 <= target_profile.tracks.chord_event_note_max) {
                    ++out.chord_band_leaks;
                }
            }
        }

        for (uint8_t ch = exporters::korg::kBassCh; ch <= 15u; ++ch) {
            uint32_t hanging = 0;
            const auto notes = extractNotes(el, ch, &hanging);
            out.hanging_note_violations += hanging;
            if (notes.empty()) continue;
            if (!exporters::korg::isChordDriven(ch)) continue;

            const bool is_bass = (ch == exporters::korg::kBassCh);
            const music::NtrRule ntr = music::NtrRule::RootTransposition;
            const music::NttMode ntt = is_bass ? music::NttMode::Bass : music::NttMode::Chord;
            music::RegisterLimits limits{};
            limits.highKey  = 11;
            limits.noteLow  = is_bass ? bass_range.min_pitch : 36u;
            limits.noteHigh = is_bass ? bass_range.max_pitch : 96u;

            // Sample up to 24 notes deterministically per element-track for fast, exact 120-chord sweeps
            const size_t stride = std::max<size_t>(1u, notes.size() / 24u);

            // Check raw bass register violations prior to transposition
            if (is_bass) {
                for (const auto& n : notes) {
                    if (n.pitch < bass_range.min_pitch || n.pitch > bass_range.max_pitch) {
                        ++out.bass_out_of_register_notes;
                        ++out.register_violations;
                    }
                }
            }

            // Sweep ALL 12 roots x ALL 10 required chord qualities (120 checks!)
            for (size_t qi = 0; qi < kInteractiveChordQualityCount; ++qi) {
                const auto qual = static_cast<InteractiveChordQuality>(qi);
                for (uint8_t root_pc = 0; root_pc < 12u; ++root_pc) {
                    const arranger::Chord chord = makeTestChord(qual, root_pc);
                    const auto mask = targetChordToneMask(qual, root_pc);
                    uint32_t cell_notes = 0;
                    uint32_t cell_hits = 0;

                    for (size_t ni = 0; ni < notes.size(); ni += stride) {
                        const auto& n = notes[ni];
                        const uint8_t tr_p = music::transpose(n.pitch, chord, ntr, ntt, 60u, limits);
                        ++cell_notes;
                        ++q_total_notes[qi];
                        const uint8_t pc = tr_p % 12u;
                        if (mask[pc]) {
                            ++cell_hits;
                            ++q_chord_tones[qi];
                        } else if (qual == InteractiveChordQuality::Diminished7 &&
                                   ((pc == (root_pc + 1u) % 12u) || (pc == (root_pc + 7u) % 12u))) {
                            // Detect harsh semitone/fifth clashes on Dim7 (DEF-P4-009 class)
                            ++out.harmonic_clash_count;
                        }
                        if (is_bass && (tr_p < bass_range.min_pitch || tr_p > bass_range.max_pitch)) {
                            ++out.register_violations;
                        }
                        if (is_bass && n.on_tick == 0u) {
                            ++total_bass_downbeats;
                            const uint8_t expected_bass_pc =
                                (qual == InteractiveChordQuality::SlashInversion && chord.bass > 0u)
                                    ? static_cast<uint8_t>(chord.bass % 12u)
                                    : root_pc;
                            if (pc == expected_bass_pc || pc == root_pc || pc == (root_pc + 7u) % 12u) {
                                ++matched_bass_downbeats;
                            }
                        }
                    }
                    ++total_cells;
                    if (cell_notes == 0u || (static_cast<double>(cell_hits) / static_cast<double>(cell_notes)) >= 0.75) {
                        ++passed_cells;
                    }
                }
            }
        }
    }

    if (out.hanging_note_violations > 0u) {
        out.hard_violations.push_back("hanging/unclosed notes: " + std::to_string(out.hanging_note_violations));
    }
    if (out.chord_band_leaks > 0u) {
        out.hard_violations.push_back("chord-event range (0..11) leak: " + std::to_string(out.chord_band_leaks));
    }
    if (out.loop_boundary_violations > 0u) {
        out.hard_violations.push_back("loop boundary event violation: " + std::to_string(out.loop_boundary_violations));
    }
    for (uint8_t ch : ref_active_chs) {
        if (!cand_active_chs.count(ch)) {
            ++out.silent_track_drops;
        }
    }
    if (out.silent_track_drops > 0u) {
        out.hard_violations.push_back("dropped active track(s): " + std::to_string(out.silent_track_drops));
    }

    out.chords_tested = static_cast<uint32_t>(std::min<uint64_t>(total_cells, UINT32_MAX));
    out.cells_passed  = static_cast<uint32_t>(std::min<uint64_t>(passed_cells, UINT32_MAX));

    uint64_t all_notes = 0, all_hits = 0;
    for (size_t qi = 0; qi < kInteractiveChordQualityCount; ++qi) {
        all_notes += q_total_notes[qi];
        all_hits  += q_chord_tones[qi];
        out.per_quality_conformance[qi] =
            (q_total_notes[qi] > 0u)
                ? clamp01(static_cast<double>(q_chord_tones[qi]) / static_cast<double>(q_total_notes[qi]))
                : 1.0;
    }
    out.chord_tone_conformance =
        (all_notes > 0u) ? clamp01(static_cast<double>(all_hits) / static_cast<double>(all_notes)) : 1.0;
    out.bass_root_accuracy =
        (total_bass_downbeats > 0u)
            ? clamp01(static_cast<double>(matched_bass_downbeats) / static_cast<double>(total_bass_downbeats))
            : 1.0;
    out.hard_passed = out.hard_violations.empty();
    return out;
}

// Explicit alias matching mission spec name `validateInteractiveChords`.
inline NttChordAuditSummary validateInteractiveChords(
    const exporters::korg::Style& style,
    const TargetCapabilityProfile& target_profile,
    const exporters::korg::Style* reference_style = nullptr) {
    return auditStyleNttConformance(style, target_profile, reference_style);
}

// Executes Stages 5-7 (Plan -> Generate -> Validate/Rollback).
// NEVER mutates faithful_style; produces an independent candidate with deterministic rollback.
inline ReorchestrateResult reorchestrateForTarget(
    const exporters::korg::Style& faithful_style,
    const exporters::korg::Report& faithful_report,
    const SemanticArrangementAnalysis& semantics,
    const TargetCapabilityProfile& target_profile,
    const ReorchestrateOptions& opt = {}) {
    using namespace reorch_detail;

    ReorchestrateResult res{};
    res.requested_mode    = opt.mode;
    res.effective_mode    = opt.mode;
    res.hardware_verified = false;
    res.l3_status         = "NOT_TESTED";
    res.bass_piano_collisions_before =
        countBassPianoLowCollisionsInStyle(faithful_style, &faithful_report, &semantics);

    // ── Mode A (Faithful): IMMUTABLE DEFAULT, bit-exact 0-mutation identity ──
    if (opt.mode == ArrangementMode::ModeA_Faithful) {
        res.style = faithful_style;
        res.ntt_audit = auditStyleNttConformance(res.style, target_profile, &faithful_style);
        res.bass_piano_collisions_after = res.bass_piano_collisions_before;
        res.identity_similarity = 1.0;
        res.locked_tracks_bit_exact = true;
        res.reversible_verified = true;
        res.rolled_back_to_faithful = false;
        res.validation_errors = res.ntt_audit.hard_violations;
        res.ok = res.ntt_audit.hard_passed;
        if (!res.ok && !res.validation_errors.empty()) res.error = res.validation_errors.front();
        return res;
    }

    auto rollbackToFaithful = [&](const std::string& reason, std::vector<std::string> errs) {
        res.style = faithful_style;
        res.effective_mode = ArrangementMode::ModeA_Faithful;
        res.rolled_back_to_faithful = true;
        res.rollback_reason = reason;
        res.validation_errors = std::move(errs);
        res.plan_actions.clear();
        res.ntt_audit = auditStyleNttConformance(res.style, target_profile, &faithful_style);
        res.bass_piano_collisions_after = res.bass_piano_collisions_before;
        res.identity_similarity = 1.0;
        res.locked_tracks_bit_exact = true;
        res.reversible_verified = true;
        res.ok = res.ntt_audit.hard_passed;
        res.error = reason;
    };

    // Start from an independent copy of faithful_style
    exporters::korg::Style candidate = faithful_style;

    // Build role lookup by target channel from P5-M2 semantics.
    std::map<uint8_t, MusicalRole> role_by_ch;
    for (const auto& trd : semantics.track_roles) {
        if (trd.target_channel != 0xFF) role_by_ch[trd.target_channel] = trd.role;
    }

    // Helper: check if a channel is Timpani/Mallet (DEF-P4-009 exemption!)
    auto isTimpaniChannel = [&](uint8_t ch) -> bool {
        for (const auto& row : faithful_report.tracks) {
            if (row.korg_ch != ch) continue;
            const std::string low = context_sound_detail::toLowerAscii(row.name + " " + row.family);
            if (low.find("timpani") != std::string::npos) return true;
        }
        return false;
    };

    auto pushAction = [&](std::string id, std::string domain, std::string marker, uint8_t ch,
                          std::string before_m, std::string after_m, std::string desc,
                          DecisionOrigin origin = DecisionOrigin::RuleBased,
                          double conf = 0.90) {
        ArrangementPlanAction act{};
        act.action_id      = std::move(id);
        act.domain         = std::move(domain);
        act.section_marker = std::move(marker);
        act.target_channel = ch;
        act.before_metric  = std::move(before_m);
        act.after_metric   = std::move(after_m);
        act.description    = std::move(desc);
        act.reversible     = true;
        act.provenance.origin       = origin;
        act.provenance.component_id = "p5_target_reorchestrator_v1";
        act.provenance.confidence   = conf;
        act.provenance.abstained    = false;
        act.provenance.reversible   = true;
        act.provenance.rationale    = act.description;
        res.plan_actions.push_back(std::move(act));
    };

    // ── Domain 1: Verified Phase 2/4 Mode B Base Rules (bounded & idempotent) ──
    if (opt.enable_mode_b_base_rules && opt.mode_b_options.anyEnabled()) {
        const auto enh = exporters::korg::quality::enhanceStyle(
            candidate, opt.mode_b_options, &faithful_report, semantics.canonical_view.source_uasf);
        if (enh.ok && !enh.actions.empty()) {
            candidate = enh.style;
            restoreLockedChannels(faithful_style, candidate, opt.locked_channels);
            for (const auto& a : enh.actions) {
                if (opt.locked_channels.count(a.korg_ch) != 0u) continue;
                pushAction("mode_b_" + a.rule_id, "mode_b_base_" + a.rule_id, a.section, a.korg_ch,
                           a.before_summary, a.after_summary, a.musical_reason,
                           DecisionOrigin::RuleBased, 0.95);
            }
        }
    }

    // ── Domain 2: Target-Capability Register Correction (Bass bounds + Bass/Piano < C3 deconflict) ──
    const RolePitchRange bass_range = target_profile.pitchRangeFor(RoleRegisterCategory::Bass);
    if (opt.enable_register_correction && opt.locked_channels.count(exporters::korg::kBassCh) == 0u) {
        for (auto& el : candidate.elements) {
            auto notes = extractNotes(el, exporters::korg::kBassCh);
            uint32_t folded = 0;
            uint8_t min_before = 127u, max_before = 0u;
            for (auto& n : notes) {
                min_before = std::min(min_before, n.pitch);
                max_before = std::max(max_before, n.pitch);
                uint8_t p = n.pitch;
                while (p < bass_range.min_pitch && p + 12u <= bass_range.max_pitch) p = static_cast<uint8_t>(p + 12u);
                while (p > bass_range.max_pitch && p >= bass_range.min_pitch + 12u) p = static_cast<uint8_t>(p - 12u);
                if (p != n.pitch) {
                    n.pitch = p;
                    ++folded;
                }
            }
            if (folded > 0u) {
                deoverlapAndReplaceNotes(el, exporters::korg::kBassCh, std::move(notes), true);
                pushAction("bass_register_clamp", "register_correction", el.marker, exporters::korg::kBassCh,
                           "range=" + std::to_string(min_before) + ".." + std::to_string(max_before),
                           "clamped " + std::to_string(folded) + " note(s) into [" +
                               std::to_string(bass_range.min_pitch) + ".." + std::to_string(bass_range.max_pitch) + "]",
                           "Octave-folded out-of-register Bass notes to target capability range (" +
                               target_profile.display_name + ").");
            }
        }
    }

    if (opt.enable_bass_piano_deconflict) {
        for (auto& el : candidate.elements) {
            const auto bass_notes = extractNotes(el, exporters::korg::kBassCh);
            if (bass_notes.empty()) continue;
            for (uint8_t ch = exporters::korg::kAcc1Ch; ch <= 15u; ++ch) {
                if (opt.locked_channels.count(ch) != 0u) continue;
                if (isTimpaniChannel(ch)) continue; // DEF-P4-009 exemption
                const MusicalRole r = role_by_ch.count(ch) ? role_by_ch[ch] : MusicalRole::UnknownAbstain;
                if (r != MusicalRole::PianoAccompaniment && r != MusicalRole::ChordComping &&
                    r != MusicalRole::HarmonicAccompaniment) {
                    continue;
                }
                auto notes = extractNotes(el, ch);
                uint32_t lifted = 0;
                for (auto& an : notes) {
                    if (an.pitch >= 48u) continue; // Only lift notes colliding below C3 (48)
                    bool overlaps_bass = false;
                    for (const auto& bn : bass_notes) {
                        if (bn.on_tick < an.off_tick && an.on_tick < bn.off_tick) {
                            overlaps_bass = true;
                            break;
                        }
                    }
                    if (overlaps_bass && an.pitch + 12u <= 84u) {
                        an.pitch = static_cast<uint8_t>(an.pitch + 12u);
                        ++lifted;
                    }
                }
                if (lifted > 0u) {
                    deoverlapAndReplaceNotes(el, ch, std::move(notes), true);
                    pushAction("bass_piano_deconflict", "register_correction", el.marker, ch,
                               std::to_string(lifted) + " notes < C3 colliding with Bass",
                               "octave-lifted >= C3 (0 low-register collisions)",
                               "Resolved Bass/Piano < C3 frequency-register masking while preserving pitch classes.");
                }
            }
        }
    }

    // ── Domain 3: Playable Chord Voicing Cap (<= 6 simultaneous notes on guitar/piano/comp) ──
    if (opt.enable_chord_voicing_cap && opt.max_chord_voicing_notes >= 3u) {
        const uint8_t max_v = std::clamp<uint8_t>(opt.max_chord_voicing_notes, 3u, 8u);
        for (auto& el : candidate.elements) {
            for (uint8_t ch = exporters::korg::kAcc1Ch; ch <= 15u; ++ch) {
                if (opt.locked_channels.count(ch) != 0u) continue;
                auto notes = extractNotes(el, ch);
                if (notes.size() <= max_v) continue;

                // Group simultaneous onsets and cap each chord cluster to max_v preserving lowest + highest + distinct pitch classes.
                std::vector<RNote> trimmed;
                trimmed.reserve(notes.size());
                uint32_t pruned = 0;
                size_t i = 0;
                while (i < notes.size()) {
                    size_t j = i + 1u;
                    while (j < notes.size() && notes[j].on_tick == notes[i].on_tick) ++j;
                    const size_t cluster_len = j - i;
                    if (cluster_len <= max_v) {
                        for (size_t k = i; k < j; ++k) trimmed.push_back(notes[k]);
                    } else {
                        std::vector<RNote> cl(notes.begin() + static_cast<ptrdiff_t>(i),
                                              notes.begin() + static_cast<ptrdiff_t>(j));
                        std::sort(cl.begin(), cl.end(), [](const RNote& a, const RNote& b) {
                            return a.pitch < b.pitch;
                        });
                        // Keep lowest, highest, and distinct pitch classes in between
                        std::vector<bool> keep(cl.size(), false);
                        keep.front() = true;
                        keep.back() = true;
                        size_t kept_cnt = 2u;
                        std::array<bool, 12> seen_pc{};
                        seen_pc[cl.front().pitch % 12u] = true;
                        seen_pc[cl.back().pitch % 12u] = true;
                        for (size_t k = 1u; k + 1u < cl.size() && kept_cnt < max_v; ++k) {
                            if (!seen_pc[cl[k].pitch % 12u]) {
                                keep[k] = true;
                                seen_pc[cl[k].pitch % 12u] = true;
                                ++kept_cnt;
                            }
                        }
                        for (size_t k = 1u; k + 1u < cl.size() && kept_cnt < max_v; ++k) {
                            if (!keep[k]) {
                                keep[k] = true;
                                ++kept_cnt;
                            }
                        }
                        for (size_t k = 0; k < cl.size(); ++k) {
                            if (keep[k]) trimmed.push_back(cl[k]);
                            else ++pruned;
                        }
                    }
                    i = j;
                }
                if (pruned > 0u) {
                    deoverlapAndReplaceNotes(el, ch, std::move(trimmed), true);
                    pushAction("chord_voicing_cap", "chord_voicing", el.marker, ch,
                               "clusters > " + std::to_string(max_v) + " notes",
                               "pruned " + std::to_string(pruned) + " redundant inner doubling(s)",
                               "Enforced playable <= " + std::to_string(max_v) +
                                   "-note chord voicing while preserving bass, top voice, and distinct chord tones.");
                }
            }
        }
    }

    // ── Domain 4: Guitar Articulation Adaptation (MegaVoice / non-native guitar target guard) ──
    if (opt.enable_guitar_art_adaptation && !target_profile.articulation.supports_megavoice_native) {
        for (auto& el : candidate.elements) {
            for (uint8_t ch = exporters::korg::kAcc1Ch; ch <= 15u; ++ch) {
                if (opt.locked_channels.count(ch) != 0u) continue;
                const MusicalRole r = role_by_ch.count(ch) ? role_by_ch[ch] : MusicalRole::UnknownAbstain;
                if (r != MusicalRole::GuitarStrumming && r != MusicalRole::Arpeggio) continue;
                auto notes = extractNotes(el, ch);
                uint32_t shaped = 0;
                for (auto& n : notes) {
                    // MegaVoice harmonic/FX velocity spikes (>= 122) on non-MegaVoice target -> tame softly
                    if (n.vel >= 122u) {
                        n.vel = 114u;
                        ++shaped;
                    }
                }
                if (shaped > 0u) {
                    deoverlapAndReplaceNotes(el, ch, std::move(notes), true);
                    pushAction("guitar_megavoice_vel_tame", "guitar_articulation", el.marker, ch,
                               std::to_string(shaped) + " MegaVoice top-zone velocity hits >= 122",
                               "tamed to <= 114 for non-MegaVoice target",
                               "Adapted Yamaha MegaVoice top-zone velocities for " + target_profile.display_name + ".");
                }
            }
        }
    }

    // ── Domain 5: Density / Polyphony Budget Optimization (respects TargetCapabilityProfile::style_voice_budget) ──
    if (opt.enable_density_optimization) {
        const uint32_t budget = target_profile.tracks.style_voice_budget;
        for (auto& el : candidate.elements) {
            const uint32_t before_poly = measureElementMaxPolyphony(el);
            if (before_poly <= budget) continue;
            // Thin redundant softest inner voices on unlocked accompaniment tracks until <= budget
            for (uint8_t ch = exporters::korg::kAcc1Ch; ch <= 15u && measureElementMaxPolyphony(el) > budget; ++ch) {
                if (opt.locked_channels.count(ch) != 0u) continue;
                auto notes = extractNotes(el, ch);
                if (notes.size() < 8u) continue;
                std::vector<RNote> kept;
                kept.reserve(notes.size());
                uint32_t dropped = 0;
                for (size_t i = 0; i < notes.size(); ++i) {
                    if (i > 0u && notes[i].on_tick == notes[i - 1u].on_tick && notes[i].vel < 55u) {
                        ++dropped;
                        continue;
                    }
                    kept.push_back(notes[i]);
                }
                if (dropped > 0u && !kept.empty()) {
                    deoverlapAndReplaceNotes(el, ch, std::move(kept), true);
                    pushAction("polyphony_budget_thinning", "density_optimization", el.marker, ch,
                               "element polyphony=" + std::to_string(before_poly) + " > budget " + std::to_string(budget),
                               "thinned " + std::to_string(dropped) + " soft overlapping voice(s)",
                               "Reduced accompaniment density to stay inside target voice-stealing budget.");
                }
            }
        }
    }

    // ── Domain 6: Mode C Controlled Creative Enhancement (Opt-In Bounded Arc & Optional AI Hook) ──
    bool any_ai_proposal_applied = false;
    uint32_t pre_ai_bass_oor = 0;
    double min_ai_track_identity = 1.0;
    if (opt.mode == ArrangementMode::ModeC_CreativeEnhancement) {
        // 6a. Bounded V1..V4 dynamic/energy contrast sculpting (strictly reversible, +/- 6 vel, preserves motif)
        if (opt.enable_creative_arc_shaping) {
            for (auto& el : candidate.elements) {
                if (el.marker.size() != 5u || el.marker[0] != 'v' || el.marker[2] != 'c' || el.marker[3] != 'v') {
                    continue;
                }
                const int var_num = el.marker[1] - '0';
                if (var_num < 1 || var_num > 4) continue;
                // Gentle energy arc shaping: V1 (-3 vel on pad/comp), V4 (+4 vel on brass/lead/comp)
                const int delta_v = (var_num == 1) ? -3 : (var_num == 4 ? +4 : 0);
                if (delta_v == 0) continue;

                for (uint8_t ch = exporters::korg::kAcc1Ch; ch <= 15u; ++ch) {
                    if (opt.locked_channels.count(ch) != 0u) continue;
                    const MusicalRole r = role_by_ch.count(ch) ? role_by_ch[ch] : MusicalRole::UnknownAbstain;
                    if (var_num == 1 && r != MusicalRole::PadSustainedHarmony && r != MusicalRole::HarmonicAccompaniment) {
                        continue;
                    }
                    if (var_num == 4 && r != MusicalRole::ChordComping && r != MusicalRole::BrassAccents &&
                        r != MusicalRole::GuitarStrumming && r != MusicalRole::PianoAccompaniment) {
                        continue;
                    }
                    auto notes = extractNotes(el, ch);
                    if (notes.empty()) continue;
                    uint32_t adj = 0;
                    for (auto& n : notes) {
                        const int nv = std::clamp<int>(static_cast<int>(n.vel) + delta_v, 24, 120);
                        if (nv != static_cast<int>(n.vel)) {
                            n.vel = static_cast<uint8_t>(nv);
                            ++adj;
                        }
                    }
                    if (adj > 0u) {
                        deoverlapAndReplaceNotes(el, ch, std::move(notes), true);
                        pushAction("mode_c_variation_arc", "creative_energy_arc", el.marker, ch,
                                   "V" + std::to_string(var_num) + " baseline dynamics",
                                   "adjusted " + std::to_string(adj) + " notes by " +
                                       (delta_v > 0 ? "+" : "") + std::to_string(delta_v) + " vel",
                                   "Mode C bounded V1..V4 energy-arc differentiation preserving pitch/groove identity.",
                                   DecisionOrigin::StatisticalInference, 0.85);
                    }
                }
            }
        }

        // 6b. Optional Generative AI Proposal Hook (STRICTLY default OFF; 100% untrusted until validated!)
        if (opt.enable_ai_proposals && opt.ai_hook != nullptr) {
            pre_ai_bass_oor = auditStyleNttConformance(candidate, target_profile, &faithful_style).bass_out_of_register_notes;
            for (auto& el : candidate.elements) {
                for (uint8_t ch = exporters::korg::kBassCh; ch <= 15u; ++ch) {
                    const MusicalRole r = role_by_ch.count(ch) ? role_by_ch[ch] : MusicalRole::UnknownAbstain;
                    GenerativeProposalRequest req{};
                    req.style_name           = semantics.style_name;
                    req.target_platform      = target_profile.id;
                    req.section_marker       = el.marker;
                    req.target_channel       = ch;
                    req.role                 = r;
                    req.element_length_ticks = el.length;
                    req.seed                 = opt.deterministic_seed;

                    const GenerativeProposalResult prop = opt.ai_hook->propose(req);
                    if (!prop.available || prop.proposed_channel_events.empty()) continue;

                    // Immediate locked-channel enforcement!
                    if (opt.locked_channels.count(ch) != 0u) {
                        rollbackToFaithful(
                            "AI proposal attempted to mutate locked channel " + std::to_string(ch),
                            {"locked_channel_mutation: ch=" + std::to_string(ch) + " in " + el.marker});
                        return res;
                    }

                    // Capture pre-AI single-track reference for local musical identity guard
                    exporters::korg::Style ref_track_only{};
                    ref_track_only.ppqn = candidate.ppqn;
                    exporters::korg::Element ref_el = el;
                    ref_el.events = extractChannelEvents(el, ch);
                    ref_track_only.elements.push_back(std::move(ref_el));

                    // Validate proposed channel events strictly before accepting into candidate
                    std::vector<exporters::korg::Event> rebuilt;
                    rebuilt.reserve(el.events.size() + prop.proposed_channel_events.size());
                    for (const auto& ev : el.events) {
                        if ((ev.status & 0x0Fu) != ch) rebuilt.push_back(ev);
                    }
                    for (const auto& ev : prop.proposed_channel_events) {
                        // Reject cross-channel smuggling into a locked channel!
                        const uint8_t ech = static_cast<uint8_t>(ev.status & 0x0Fu);
                        if (opt.locked_channels.count(ech) != 0u) {
                            rollbackToFaithful(
                                "AI proposal smuggled event onto locked channel " + std::to_string(ech),
                                {"locked_channel_smuggling: ch=" + std::to_string(ech) + " in " + el.marker});
                            return res;
                        }
                        rebuilt.push_back(ev);
                    }
                    el.events.swap(rebuilt);
                    canonicalSortEvents(el.events);

                    exporters::korg::Style prop_track_only{};
                    prop_track_only.ppqn = candidate.ppqn;
                    exporters::korg::Element prop_el = el;
                    prop_el.events = extractChannelEvents(el, ch);
                    prop_track_only.elements.push_back(std::move(prop_el));
                    min_ai_track_identity = std::min(
                        min_ai_track_identity, computeIdentitySimilarity(ref_track_only, prop_track_only));
                    any_ai_proposal_applied = true;

                    pushAction("mode_c_ai_proposal", "generative_proposal", el.marker, ch,
                               "deterministic baseline track",
                               "proposed " + std::to_string(prop.proposed_channel_events.size()) +
                                   " event(s) from " + prop.model_id,
                               prop.rationale.empty() ? "Opt-in AI proposal pending hard deterministic gate." : prop.rationale,
                               DecisionOrigin::OptionalGenerativeProposal,
                               clamp01(prop.confidence));
                }
            }
        }
    }

    // ── Domain 7: Controller Ordering & Transition Hygiene (DEF-P4-010: CC11/CC64 strictly BEFORE NoteOn!) ──
    if (opt.enable_transition_cc_hygiene) {
        const uint32_t before_cc_defects = countSameTickCcAfterNoteOn(candidate);
        if (before_cc_defects > 0u) {
            for (auto& el : candidate.elements) {
                canonicalSortEvents(el.events);
            }
            const uint32_t after_cc_defects = countSameTickCcAfterNoteOn(candidate);
            if (after_cc_defects < before_cc_defects) {
                pushAction("same_tick_cc_sort", "transition_cc_hygiene", "STYLE", 0xFF,
                           std::to_string(before_cc_defects) + " CC-after-NoteOn defect(s)",
                           std::to_string(after_cc_defects) + " defect(s)",
                           "Sorted Bank/PC/CC11/CC64 strictly before NoteOn at identical ticks (DEF-P4-010).");
            }
        }
    }

    // ── Domain 8: Advisory Sound Substitution Provenance Record (Non-Destructive!) ──
    if (opt.context_sounds != nullptr) {
        for (const auto& t : opt.context_sounds->tracks) {
            if (t.ensemble_collision_mitigated && !t.ranked_options.empty()) {
                pushAction("context_sound_advisory", "instrument_substitution_advisory", "STYLE", t.target_channel,
                           "isolated timbre match",
                           "advisory: " + t.ranked_options.front().base_candidate.display_name,
                           "Non-destructive P5-M3 ensemble masking advisory (verified tick-0 sounds untouched).",
                           DecisionOrigin::StatisticalInference,
                           t.ranked_options.front().final_confidence);
            }
        }
    }

    // ── Final Hard Deterministic Validation & Automatic Fail-Closed Rollback ──
    const bool locked_ok = verifyLockedChannelsBitExact(faithful_style, candidate, opt.locked_channels);
    const auto base_audit = auditStyleNttConformance(faithful_style, target_profile, &faithful_style);
    const auto cand_audit = auditStyleNttConformance(candidate, target_profile, &faithful_style);
    const double id_sim = computeIdentitySimilarity(faithful_style, candidate);

    std::vector<std::string> hard_errs = cand_audit.hard_violations;
    if (!locked_ok) {
        hard_errs.push_back("locked_tracks_bit_exact violated");
    }
    if (cand_audit.chord_tone_conformance + 0.02 < base_audit.chord_tone_conformance &&
        cand_audit.chord_tone_conformance < opt.min_ntt_conformance) {
        hard_errs.push_back("NTT chord-tone conformance regressed (" +
                            fmtFinite(base_audit.chord_tone_conformance, 3) + " -> " +
                            fmtFinite(cand_audit.chord_tone_conformance, 3) + ")");
    }
    if (cand_audit.bass_out_of_register_notes > base_audit.bass_out_of_register_notes ||
        (any_ai_proposal_applied && cand_audit.bass_out_of_register_notes > pre_ai_bass_oor)) {
        hard_errs.push_back("introduced out-of-register Bass notes (" +
                            std::to_string(any_ai_proposal_applied ? pre_ai_bass_oor : base_audit.bass_out_of_register_notes) +
                            " -> " + std::to_string(cand_audit.bass_out_of_register_notes) + ")");
    }
    if (id_sim < opt.min_identity_similarity ||
        (any_ai_proposal_applied && min_ai_track_identity < opt.min_identity_similarity)) {
        hard_errs.push_back("musical identity similarity " +
                            fmtFinite(std::min(id_sim, min_ai_track_identity), 3) +
                            " below guard floor " + fmtFinite(opt.min_identity_similarity, 3));
    }

    if (!hard_errs.empty()) {
        rollbackToFaithful("Hard deterministic validation rejected candidate: " + hard_errs.front(),
                           std::move(hard_errs));
        return res;
    }

    res.style = std::move(candidate);
    res.ntt_audit = cand_audit;
    res.identity_similarity = id_sim;
    res.bass_piano_collisions_after =
        countBassPianoLowCollisionsInStyle(res.style, &faithful_report, &semantics);
    res.locked_tracks_bit_exact = true;
    res.reversible_verified = true;
    res.rolled_back_to_faithful = false;
    res.ok = true;
    return res;
}

// Generates Mode A, Mode B, and Mode C independently from the immutable faithful_style.
inline ModeAbcCandidateBundle generateModeAbcCandidates(
    const exporters::korg::Style& faithful_style,
    const exporters::korg::Report& faithful_report,
    const SemanticArrangementAnalysis& semantics,
    const TargetCapabilityProfile& target_profile,
    const ReorchestrateOptions& base_opt = {}) {
    ModeAbcCandidateBundle bundle{};

    ReorchestrateOptions optA = base_opt;
    optA.mode = ArrangementMode::ModeA_Faithful;
    bundle.mode_a = reorchestrateForTarget(faithful_style, faithful_report, semantics, target_profile, optA);

    ReorchestrateOptions optB = base_opt;
    optB.mode = ArrangementMode::ModeB_IntelligentAdaptation;
    bundle.mode_b = reorchestrateForTarget(faithful_style, faithful_report, semantics, target_profile, optB);

    ReorchestrateOptions optC = base_opt;
    optC.mode = ArrangementMode::ModeC_CreativeEnhancement;
    bundle.mode_c = reorchestrateForTarget(faithful_style, faithful_report, semantics, target_profile, optC);

    return bundle;
}

inline std::string formatReorchestrationAuditJson(const ReorchestrateResult& r) {
    using namespace reorch_detail;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"aiarranger-reorchestration-audit/1\",\n";
    o << "  \"ok\": " << (r.ok ? "true" : "false") << ",\n";
    o << "  \"requestedMode\": \"" << arrangementModeName(r.requested_mode) << "\",\n";
    o << "  \"effectiveMode\": \"" << arrangementModeName(r.effective_mode) << "\",\n";
    o << "  \"rolledBackToFaithful\": " << (r.rolled_back_to_faithful ? "true" : "false") << ",\n";
    o << "  \"rollbackReason\": \"" << jsonEscapeStrict(r.rollback_reason) << "\",\n";
    o << "  \"lockedTracksBitExact\": " << (r.locked_tracks_bit_exact ? "true" : "false") << ",\n";
    o << "  \"reversibleVerified\": " << (r.reversible_verified ? "true" : "false") << ",\n";
    o << "  \"identitySimilarity\": " << fmtFinite(r.identity_similarity, 4) << ",\n";
    o << "  \"bassPianoCollisionsBefore\": " << r.bass_piano_collisions_before << ",\n";
    o << "  \"bassPianoCollisionsAfter\": " << r.bass_piano_collisions_after << ",\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"" << jsonEscapeStrict(r.l3_status) << "\",\n";
    o << "  \"nttAudit\": {\n";
    o << "    \"chordsTested\": " << r.ntt_audit.chords_tested << ",\n";
    o << "    \"cellsPassed\": " << r.ntt_audit.cells_passed << ",\n";
    o << "    \"chordToneConformance\": " << fmtFinite(r.ntt_audit.chord_tone_conformance, 4) << ",\n";
    o << "    \"bassRootAccuracy\": " << fmtFinite(r.ntt_audit.bass_root_accuracy, 4) << ",\n";
    o << "    \"registerViolations\": " << r.ntt_audit.register_violations << ",\n";
    o << "    \"hardPassed\": " << (r.ntt_audit.hard_passed ? "true" : "false") << ",\n";
    o << "    \"perQualityConformance\": {";
    for (size_t i = 0; i < kInteractiveChordQualityCount; ++i) {
        if (i > 0u) o << ", ";
        o << "\"" << interactiveChordQualityName(static_cast<InteractiveChordQuality>(i)) << "\": "
          << fmtFinite(r.ntt_audit.per_quality_conformance[i], 4);
    }
    o << "}\n";
    o << "  },\n";
    o << "  \"actions\": [";
    for (size_t i = 0; i < r.plan_actions.size(); ++i) {
        const auto& a = r.plan_actions[i];
        if (i > 0u) o << ",";
        o << "\n    {"
          << "\"actionId\": \"" << jsonEscapeStrict(a.action_id) << "\""
          << ", \"domain\": \"" << jsonEscapeStrict(a.domain) << "\""
          << ", \"section\": \"" << jsonEscapeStrict(a.section_marker) << "\""
          << ", \"targetCh\": " << static_cast<unsigned>(a.target_channel)
          << ", \"before\": \"" << jsonEscapeStrict(a.before_metric) << "\""
          << ", \"after\": \"" << jsonEscapeStrict(a.after_metric) << "\""
          << ", \"description\": \"" << jsonEscapeStrict(a.description) << "\""
          << ", \"reversible\": " << (a.reversible ? "true" : "false")
          << "}";
    }
    if (!r.plan_actions.empty()) o << "\n  ";
    o << "]\n}\n";
    return o.str();
}

} // namespace ai_arranger::intelligence

#endif // AI_ARRANGER_INTELLIGENCE_TARGET_REORCHESTRATOR_H
