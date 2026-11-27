#ifndef AI_ARRANGER_EXPORTERS_KORG_STYLE_ENHANCER_H
#define AI_ARRANGER_EXPORTERS_KORG_STYLE_ENHANCER_H

#include "engine/uasf/types.h"
#include "exporters/korg/cv_splitter.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/korg_style_smf.h"
#include "exporters/korg/source_chord_normalizer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Opt-in, bounded, explainable, reversible Mode B Style Enhancer (Phase 2 Mission 4).
//
// Architectural invariants:
//   - Explicit ConversionMode: Faithful (Mode A, DEFAULT) vs IntelligentEnhance (Mode B, OPT-IN).
//   - Bit-exact Mode A Invariance: when mode == Faithful OR all rule flags are false,
//     enhanceStyle() performs ZERO mutations and returns actions.empty() == true.
//   - Every Mode B edit emits a structured EnhancementAction audit record with
//     rule_id, section, track, bar, tick, before_summary, after_summary, musical_reason,
//     and reversible = true.
//   - Hard post-enhancement anti-degradation guardrails run before returning; any
//     invariant violation triggers fail-closed rollback to Mode A input.
//   - Portable C++20 header-only, zero JUCE/Apple dependencies, ODR-safe.

namespace ai_arranger::exporters::korg::quality {

enum class ConversionMode : uint8_t {
    Faithful = 0,           // Mode A (DEFAULT): bit-exact identity, 0 mutations
    IntelligentEnhance = 1, // Mode B (OPT-IN): bounded, reversible rules with audit trail
};

inline const char* conversionModeName(ConversionMode mode) noexcept {
    switch (mode) {
        case ConversionMode::Faithful:           return "Faithful";
        case ConversionMode::IntelligentEnhance: return "IntelligentEnhance";
    }
    return "Faithful";
}

struct EnhanceOptions {
    // ── Mode Switch (DEFAULT = Mode A Faithful -> guaranteed 0 mutations!) ──
    ConversionMode mode = ConversionMode::Faithful;

    // ── Per-Rule Opt-In Switches (ALL DEFAULT FALSE -> independently reversible!) ──
    bool enhance_variation_energy_curve = false; // Rule 1: gentle V1..V4 velocity shaping
    bool enhance_drum_ghost_dynamics    = false; // Rule 2: flat drum/snare/fill dynamic shaping
    bool enhance_bass_register_guard    = false; // Rule 3: octave-up fold strictly for bass < E1 (28)
    bool enhance_piano_mud_voicing      = false; // Rule 4: open-voicing relief for dense < C3 (48) chords
    bool enhance_ending_fade_pop_guard  = false; // Rule 5: protect one-shot Ending CC11 fade + loop de-click ramp
    bool enhance_jazz_cv_melody_split   = false; // Rule 6: synthesize CV2/CV3/CV4 for single-CV1 Maj7/walking-bass styles

    // ── Bounded Safety Parameters (hard-clamped internally) ──
    double  max_variation_vel_scale_pct = 6.0;  // Clamped [1.0, 8.0] (%), delta also capped +/-12
    uint8_t max_drum_vel_delta          = 6;    // Clamped [1, 10] MIDI velocity units
    bool    enhance_fill_crescendo      = true; // Sub-option of Rule 2: gentle 2nd-half fill lift
    uint8_t bass_floor_midi             = 28;   // E1 (28); clamped [23..36]
    uint8_t piano_mud_ceiling_midi      = 48;   // C3 (48); clamped [40..55]
    uint8_t cc11_declick_jump_threshold = 48;   // Clamped [24..96]
    uint8_t cc11_ramp_steps             = 4;    // Clamped [2..6]
    bool    enable_loop_cc11_declick_ramp = true; // Sub-option of Rule 5 for looping V/F/I elements
    bool    synthesize_cv4_dim_on_mains = true; // Sub-option of Rule 6 for Main1..4

    static EnhanceOptions faithful() noexcept { return EnhanceOptions{}; }
    static EnhanceOptions off() noexcept { return faithful(); }

    static EnhanceOptions recommended() noexcept {
        EnhanceOptions o;
        o.mode = ConversionMode::IntelligentEnhance;
        o.enhance_variation_energy_curve = true;
        o.enhance_drum_ghost_dynamics    = true;
        o.enhance_bass_register_guard    = true;
        o.enhance_piano_mud_voicing      = true;
        o.enhance_ending_fade_pop_guard  = true;
        o.enhance_jazz_cv_melody_split   = true;
        return o;
    }

    static EnhanceOptions musicalRecommended() noexcept { return recommended(); }

    bool anyRuleEnabled() const noexcept {
        return enhance_variation_energy_curve ||
               enhance_drum_ghost_dynamics ||
               enhance_bass_register_guard ||
               enhance_piano_mud_voicing ||
               enhance_ending_fade_pop_guard ||
               enhance_jazz_cv_melody_split;
    }

    bool anyEnabled() const noexcept {
        return mode == ConversionMode::IntelligentEnhance && anyRuleEnabled();
    }
};

struct EnhancementAction {
    std::string rule_id;         // e.g. "variation_energy_curve", "ending_fade_pop_guard"
    std::string section;         // e.g. "v1cv1", "e1cv1", "v1cv2"
    uint8_t     korg_ch = 0xFF;  // 8..15 or 0xFF for element-level action
    std::string track;           // "Bass", "Drum", "Acc1".."Acc5", or "Element"
    uint32_t    bar = 0;         // 1-based bar (0 = whole element)
    uint32_t    tick = 0;        // element-relative tick
    std::string before_summary;  // Exact pre-change summary
    std::string after_summary;   // Exact post-change summary
    std::string musical_reason;  // Plain-English arranger rationale
    bool        reversible = true;
};

struct EnhanceStats {
    uint32_t actions_applied         = 0;
    uint32_t notes_velocity_adjusted = 0;
    uint32_t notes_pitch_adjusted    = 0;
    uint32_t cc_events_added         = 0;
    uint32_t cc_events_removed       = 0;
    uint32_t cv_elements_synthesized = 0;
    bool     mode_a_bit_exact_bypass = true;
    bool     rolled_back_on_guardrail = false;
};

struct EnhanceResult {
    bool                           ok = false;
    std::string                    error;
    ConversionMode                 mode = ConversionMode::Faithful;
    Style                          style;
    EnhanceStats                   stats;
    std::vector<EnhancementAction> actions;
    std::vector<std::string>       validation_errors;
    bool                           hardware_verified = false;
    std::string                    l3_status = "NOT_TESTED";
    bool                           independent_from_frozen_q1 = true;
};

using EnhancementReport = EnhanceResult;

namespace style_enhancer_detail {

struct EnhNote {
    uint32_t on_tick = 0;
    uint32_t off_tick = 0;
    uint8_t  pitch = 0;
    uint8_t  vel = 0;
};

inline std::string toLowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

inline bool containsInsensitive(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return false;
    return toLowerAscii(hay).find(toLowerAscii(needle)) != std::string::npos;
}

inline uint8_t clampMidi7(int v, uint8_t lo = 0u, uint8_t hi = 127u) noexcept {
    if (v < static_cast<int>(lo)) return lo;
    if (v > static_cast<int>(hi)) return hi;
    return static_cast<uint8_t>(v);
}

inline std::string pitchName(uint8_t p) {
    static constexpr const char* kNames[12] = {
        "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
    };
    const int oct = static_cast<int>(p / 12u) - 1;
    return std::string(kNames[p % 12u]) + std::to_string(oct);
}

inline uint32_t barTicksFor(uint16_t ppqn, uint8_t num, uint8_t den) noexcept {
    if (ppqn == 0 || num == 0 || den == 0) return 0;
    const uint64_t numer = static_cast<uint64_t>(ppqn) * 4ull * static_cast<uint64_t>(num);
    if (numer % den != 0) return 0;
    const uint64_t bt = numer / den;
    if (bt == 0 || bt > std::numeric_limits<uint32_t>::max()) return 0;
    return static_cast<uint32_t>(bt);
}

inline uint32_t bar1Based(uint32_t tick, uint32_t bar_ticks) noexcept {
    return bar_ticks > 0u ? ((tick / bar_ticks) + 1u) : 1u;
}

inline bool isVariationMarker(std::string_view m) noexcept {
    return m.size() == 5 && m[0] == 'v' && m[1] >= '1' && m[1] <= '4' &&
           m[2] == 'c' && m[3] == 'v' && m[4] >= '1' && m[4] <= '6';
}

inline bool isFillMarker(std::string_view m) noexcept {
    return m.size() == 5 && m[0] == 'f' && m[1] >= '1' && m[1] <= '4' &&
           m[2] == 'c' && m[3] == 'v' && m[4] >= '1' && m[4] <= '2';
}

inline bool isEndingMarker(std::string_view m) noexcept {
    return m.size() == 5 && m[0] == 'e' && m[1] >= '1' && m[1] <= '3' &&
           m[2] == 'c' && m[3] == 'v' && m[4] >= '1' && m[4] <= '2';
}

inline uint8_t cvNumberOf(std::string_view m) noexcept {
    if (m.size() == 5 && m[2] == 'c' && m[3] == 'v' && m[4] >= '1' && m[4] <= '6') {
        return static_cast<uint8_t>(m[4] - '0');
    }
    return 0;
}

inline std::string sectionBaseOf(std::string_view m) {
    if (m == "br") return "br";
    if (m.size() == 5 && m[2] == 'c' && m[3] == 'v') return std::string(m.substr(0, 2));
    return std::string(m);
}

inline int eventSortPriority(const Event& ev) noexcept {
    const uint8_t hi = ev.status & 0xF0u;
    if (ev.tick == 0u) {
        if (hi == 0xB0u && ev.d1 == 0u)  return 0;  // Bank MSB
        if (hi == 0xB0u && ev.d1 == 32u) return 1;  // Bank LSB
        if (hi == 0xC0u)                 return 2;  // Program Change
        if (hi == 0xB0u || hi == 0xE0u)  return 3;  // Mix / Controller init
    }
    if (hi == 0x80u || (hi == 0x90u && ev.d2 == 0u)) return 4;  // NoteOff before NoteOn
    if (hi == 0x90u) return 5;                                   // NoteOn
    return 6;
}

inline void canonicalSortElement(Element& el) {
    std::stable_sort(el.events.begin(), el.events.end(),
                     [](const Event& a, const Event& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return eventSortPriority(a) < eventSortPriority(b);
                     });
}

inline std::vector<EnhNote> extractChannelNotes(const Element& el, uint8_t ch) {
    std::vector<EnhNote> out;
    std::array<std::deque<std::pair<uint32_t, uint8_t>>, 128> active{};
    for (const auto& ev : el.events) {
        if ((ev.status & 0x0Fu) != ch) continue;
        const uint8_t hi = ev.status & 0xF0u;
        const uint8_t p  = ev.d1 & 0x7Fu;
        if (hi == 0x90u && ev.d2 > 0u) {
            active[p].emplace_back(ev.tick, static_cast<uint8_t>(ev.d2 & 0x7Fu));
        } else if (hi == 0x80u || (hi == 0x90u && ev.d2 == 0u)) {
            auto& q = active[p];
            if (!q.empty()) {
                const auto [on_t, vel] = q.front();
                q.pop_front();
                const uint32_t off_t = (ev.tick > on_t) ? ev.tick : (on_t + 1u);
                out.push_back(EnhNote{on_t, off_t, p, vel});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const EnhNote& a, const EnhNote& b) {
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.off_tick < b.off_tick;
    });
    return out;
}

// Enforce strict same-pitch non-overlap and [0, len-1] bounds on a channel's note list.
inline void deoverlapChannelNotes(std::vector<EnhNote>& notes, uint32_t el_length, bool chord_driven) {
    if (notes.empty() || el_length < 2u) {
        if (el_length < 2u) notes.clear();
        return;
    }
    // Clamp bounds and remove duplicates at identical (on_tick, pitch).
    std::vector<EnhNote> cleaned;
    cleaned.reserve(notes.size());
    for (auto n : notes) {
        if (chord_driven && n.pitch <= kChordEventNoteMax) continue;
        if (n.pitch > 127u || n.vel == 0u) continue;
        if (n.on_tick >= el_length - 1u) continue;
        if (n.off_tick <= n.on_tick) n.off_tick = n.on_tick + 1u;
        if (n.off_tick >= el_length) n.off_tick = el_length - 1u;
        if (n.off_tick <= n.on_tick) continue;
        cleaned.push_back(n);
    }
    std::sort(cleaned.begin(), cleaned.end(), [](const EnhNote& a, const EnhNote& b) {
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        return a.off_tick > b.off_tick;
    });
    std::vector<EnhNote> out;
    out.reserve(cleaned.size());
    for (auto& n : cleaned) {
        if (!out.empty() && out.back().pitch == n.pitch) {
            if (out.back().on_tick == n.on_tick) {
                // Merge identical (on_tick, pitch) keeping louder vel and longer off_tick.
                if (n.vel > out.back().vel) out.back().vel = n.vel;
                if (n.off_tick > out.back().off_tick) out.back().off_tick = n.off_tick;
                continue;
            }
            if (out.back().off_tick > n.on_tick) {
                if (n.on_tick > out.back().on_tick) {
                    out.back().off_tick = n.on_tick;
                } else {
                    continue;
                }
            }
        }
        out.push_back(n);
    }
    std::sort(out.begin(), out.end(), [](const EnhNote& a, const EnhNote& b) {
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.off_tick < b.off_tick;
    });
    notes.swap(out);
}

inline void replaceChannelNotes(Element& el, uint8_t ch, const std::vector<EnhNote>& notes) {
    std::vector<Event> kept;
    kept.reserve(el.events.size() + notes.size() * 2u);
    for (const auto& ev : el.events) {
        const uint8_t hi = ev.status & 0xF0u;
        const uint8_t ech = ev.status & 0x0Fu;
        if (ech == ch && (hi == 0x80u || hi == 0x90u)) continue;
        kept.push_back(ev);
    }
    for (const auto& n : notes) {
        kept.push_back(Event{n.on_tick, static_cast<uint8_t>(0x90u | ch), n.pitch, n.vel});
        kept.push_back(Event{n.off_tick, static_cast<uint8_t>(0x80u | ch), n.pitch, 0u});
    }
    el.events.swap(kept);
    canonicalSortElement(el);
}

inline double computeMeanVel(const std::vector<uint8_t>& vels) noexcept {
    if (vels.empty()) return 0.0;
    double sum = 0.0;
    for (uint8_t v : vels) sum += static_cast<double>(v);
    return sum / static_cast<double>(vels.size());
}

inline double computeStddevVel(const std::vector<uint8_t>& vels) noexcept {
    if (vels.empty()) return 0.0;
    const double mean = computeMeanVel(vels);
    double var = 0.0;
    for (uint8_t v : vels) {
        const double d = static_cast<double>(v) - mean;
        var += d * d;
    }
    return std::sqrt(var / static_cast<double>(vels.size()));
}

inline std::string fmt2(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << (std::isfinite(v) ? v : 0.0);
    return oss.str();
}

inline int firstTick0Pc(const Element& el, uint8_t ch) noexcept {
    for (const auto& ev : el.events) {
        if (ev.tick > 0u) break;
        if ((ev.status & 0xF0u) == 0xC0u && (ev.status & 0x0Fu) == ch) {
            return static_cast<int>(ev.d1);
        }
    }
    return -1;
}

inline bool isExemptFromPianoMud(uint8_t ch, int pc, const Report* report) {
    if (ch < kAcc1Ch || ch > 15u) return true;
    if (pc == 47 || pc == 43 || pc == 58) return true;  // Timpani, Contrabass, Tuba
    if (report != nullptr) {
        for (const auto& tr : report->tracks) {
            if (tr.korg_ch != ch) continue;
            if (containsInsensitive(tr.family, "timpani") || containsInsensitive(tr.name, "timpani") ||
                containsInsensitive(tr.family, "contrabass") || containsInsensitive(tr.name, "contrabass") ||
                containsInsensitive(tr.name, "contrabs") || containsInsensitive(tr.family, "bass") ||
                containsInsensitive(tr.family, "drum") || containsInsensitive(tr.family, "perc") ||
                containsInsensitive(tr.family, "tuba")) {
                return true;
            }
        }
    }
    return false;
}

inline bool hasSimultaneousPitchIn(const std::vector<EnhNote>& notes,
                                   size_t skip_idx,
                                   uint32_t on_tick,
                                   uint32_t onset_win,
                                   uint8_t pitch) noexcept {
    for (size_t i = 0; i < notes.size(); ++i) {
        if (i == skip_idx) continue;
        const uint32_t diff = (notes[i].on_tick >= on_tick)
            ? (notes[i].on_tick - on_tick)
            : (on_tick - notes[i].on_tick);
        if (diff <= onset_win && notes[i].pitch == pitch) return true;
    }
    return false;
}

struct LoopHygieneSnapshot {
    uint32_t open_cc64 = 0;
    uint32_t uncentered_pb = 0;
    uint32_t nrpn_leaks = 0;
    uint32_t cc94_leaks = 0;
};

inline LoopHygieneSnapshot measureLoopHygiene(const Style& s) noexcept {
    LoopHygieneSnapshot snap;
    for (const auto& el : s.elements) {
        std::array<uint8_t, 16> cc64{};
        std::array<uint16_t, 16> pb{};
        pb.fill(8192u);
        for (const auto& ev : el.events) {
            const uint8_t hi = ev.status & 0xF0u;
            const uint8_t ch = ev.status & 0x0Fu;
            if (hi == 0xB0u) {
                if (ev.d1 == 64u) cc64[ch] = ev.d2;
                else if (ev.d1 == 98u || ev.d1 == 99u) ++snap.nrpn_leaks;
                else if (ev.d1 == 94u) ++snap.cc94_leaks;
            } else if (hi == 0xE0u) {
                pb[ch] = static_cast<uint16_t>(
                    static_cast<uint16_t>(ev.d1 & 0x7Fu) |
                    (static_cast<uint16_t>(ev.d2 & 0x7Fu) << 7));
            }
        }
        for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
            if (cc64[ch] >= 64u) ++snap.open_cc64;
            if (pb[ch] != 8192u) ++snap.uncentered_pb;
        }
    }
    return snap;
}

inline bool verifyNoSamePitchOverlap(const Style& s) {
    for (const auto& el : s.elements) {
        for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
            std::array<bool, 128> sounding{};
            for (const auto& ev : el.events) {
                if ((ev.status & 0x0Fu) != ch) continue;
                const uint8_t hi = ev.status & 0xF0u;
                const uint8_t p  = ev.d1 & 0x7Fu;
                if (hi == 0x90u && ev.d2 > 0u) {
                    if (sounding[p]) return false;
                    sounding[p] = true;
                } else if (hi == 0x80u || (hi == 0x90u && ev.d2 == 0u)) {
                    sounding[p] = false;
                }
            }
        }
    }
    return true;
}

inline std::map<std::pair<std::string, uint8_t>, std::tuple<int, int, int>>
captureTick0Voices(const Style& s) {
    std::map<std::pair<std::string, uint8_t>, std::tuple<int, int, int>> m;
    for (const auto& el : s.elements) {
        for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
            int msb = -1, lsb = -1, pc = -1;
            for (const auto& ev : el.events) {
                if (ev.tick > 0u) break;
                if ((ev.status & 0x0Fu) != ch) continue;
                const uint8_t hi = ev.status & 0xF0u;
                if (hi == 0xB0u && ev.d1 == 0u) msb = ev.d2;
                if (hi == 0xB0u && ev.d1 == 32u) lsb = ev.d2;
                if (hi == 0xC0u) pc = ev.d1;
            }
            if (msb >= 0 || lsb >= 0 || pc >= 0) {
                m[{el.marker, ch}] = {msb, lsb, pc};
            }
        }
    }
    return m;
}

inline std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20u) {
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned>(c);
                    out += oss.str();
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

inline std::string mdEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '|') out += "\\|";
        else if (c == '\n' || c == '\r') out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

// =========================================================================
// Rule 6: jazz_cv_melody_split (executes first so B3..B5 cover synthesized CVs)
// =========================================================================
inline void applyJazzCvMelodySplit(Style& style,
                                   const EnhanceOptions& opt,
                                   const Report* report,
                                   EnhanceStats& stats,
                                   std::vector<EnhancementAction>& actions) {
    std::map<std::string, uint8_t> sec_cv_count;
    std::set<std::string> existing_markers;
    for (const auto& el : style.elements) {
        existing_markers.insert(el.marker);
        const std::string sb = sectionBaseOf(el.marker);
        if (sb != "br") {
            sec_cv_count[sb] += 1u;
        }
    }

    std::vector<Element> rebuilt;
    rebuilt.reserve(style.elements.size() * 2u);

    for (const auto& el : style.elements) {
        rebuilt.push_back(el);

        const std::string sb = sectionBaseOf(el.marker);
        if (sb == "br" || cvNumberOf(el.marker) != 1u) continue;
        if (sec_cv_count[sb] != 1u) continue;  // Only trigger on CV1-only sections

        // Inspect CV1 for Maj7 melody notes (pc==11) and walking bass (pc==4 or 7).
        bool has_maj7_melody = false;
        bool has_walking_bass = false;
        bool has_bass_pc4 = false;
        bool has_bass_pc7 = false;
        bool has_acc_pc4 = false;

        const auto bass_notes = extractChannelNotes(el, kBassCh);
        if (bass_notes.size() >= 4u) {
            uint32_t non_rf = 0, root_cnt = 0;
            for (const auto& n : bass_notes) {
                const uint8_t pc = static_cast<uint8_t>(n.pitch % 12u);
                if (pc == 0u) ++root_cnt;
                if (pc != 0u && pc != 7u) ++non_rf;
                if (pc == 4u) has_bass_pc4 = true;
                if (pc == 7u) has_bass_pc7 = true;
            }
            const double pct = 100.0 * static_cast<double>(non_rf) / static_cast<double>(bass_notes.size());
            if (root_cnt > 0u && pct >= 15.0 && (has_bass_pc4 || has_bass_pc7)) {
                has_walking_bass = true;
            }
        }

        for (uint8_t ch = kAcc1Ch; ch <= 15u; ++ch) {
            if (isExemptFromPianoMud(ch, firstTick0Pc(el, ch), report)) continue;
            const auto an = extractChannelNotes(el, ch);
            for (const auto& n : an) {
                const uint8_t pc = static_cast<uint8_t>(n.pitch % 12u);
                if (pc == 11u) has_maj7_melody = true;
                if (pc == 4u) has_acc_pc4 = true;
            }
        }

        if (!has_maj7_melody && !has_walking_bass) continue;

        const bool is_main = isVariationMarker(el.marker);
        const uint32_t onset_win = (style.ppqn >= 16u) ? (style.ppqn / 16u) : 1u;

        auto synthCv = [&](char cv_digit, const char* chord_label,
                           bool map_e_to_eb, bool map_g_to_gb, int b_delta) {
            std::string new_marker = sb + "cv" + cv_digit;
            if (!isValidMarker(new_marker) || existing_markers.count(new_marker) > 0u) return;

            Element clone = el;
            clone.marker = new_marker;
            uint32_t total_adapted = 0;
            std::ostringstream detail_ss;

            for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                if (!isChordDriven(ch)) continue;
                if (ch >= kAcc1Ch && isExemptFromPianoMud(ch, firstTick0Pc(el, ch), report)) continue;
                if (ch == kBassCh && !has_walking_bass) continue;

                auto notes = extractChannelNotes(clone, ch);
                if (notes.empty()) continue;
                uint32_t ch_adapted = 0;
                for (size_t i = 0; i < notes.size(); ++i) {
                    const uint8_t pc = static_cast<uint8_t>(notes[i].pitch % 12u);
                    int delta = 0;
                    if (map_e_to_eb && pc == 4u) delta = -1;
                    else if (map_g_to_gb && pc == 7u) delta = -1;
                    else if (b_delta != 0 && pc == 11u) delta = b_delta;
                    if (delta == 0) continue;

                    const int cand_i = static_cast<int>(notes[i].pitch) + delta;
                    if (cand_i <= static_cast<int>(kChordEventNoteMax) || cand_i > 127) continue;
                    uint8_t cand = static_cast<uint8_t>(cand_i);
                    if (hasSimultaneousPitchIn(notes, i, notes[i].on_tick, onset_win, cand)) {
                        // Try alternate safe neighbor (+1 for B->C if Bb collides)
                        if (pc == 11u && notes[i].pitch < 127u &&
                            !hasSimultaneousPitchIn(notes, i, notes[i].on_tick, onset_win,
                                                    static_cast<uint8_t>(notes[i].pitch + 1u))) {
                            cand = static_cast<uint8_t>(notes[i].pitch + 1u);
                        } else {
                            continue;
                        }
                    }
                    notes[i].pitch = cand;
                    ++ch_adapted;
                    ++total_adapted;
                    ++stats.notes_pitch_adjusted;
                }
                if (ch_adapted > 0u) {
                    deoverlapChannelNotes(notes, clone.length, true);
                    replaceChannelNotes(clone, ch, notes);
                    if (detail_ss.tellp() > 0) detail_ss << ", ";
                    detail_ss << korgTrackName(ch) << ":" << ch_adapted << " notes";
                }
            }

            if (total_adapted == 0u) return;
            existing_markers.insert(new_marker);
            rebuilt.push_back(std::move(clone));
            ++stats.cv_elements_synthesized;

            EnhancementAction act;
            act.rule_id = "jazz_cv_melody_split";
            act.section = new_marker;
            act.korg_ch = 0xFF;
            act.track = "Element";
            act.bar = 0;
            act.tick = 0;
            act.before_summary = el.marker + " only (single CV1)";
            act.after_summary = "synthesized " + new_marker + " (" + chord_label + "; " + detail_ss.str() + ")";
            act.musical_reason = "Preserves authentic Maj7/walking-bass CV1 while eliminating " +
                                 std::string(chord_label) + " chord clashes under Pa5X Parallel/Root";
            act.reversible = true;
            actions.push_back(std::move(act));
        };

        // CV2 (C Minor): valid on all v/i/f/e when E or B is present
        if (has_bass_pc4 || has_acc_pc4 || has_maj7_melody) {
            synthCv('2', "C Minor", true, false, -1);
        }
        // CV3 (C Dom7): valid on Main1..4 when Maj7 B (pc=11) is present
        if (is_main && has_maj7_melody) {
            synthCv('3', "C Dom7", false, false, -1);
        }
        // CV4 (C Dim): valid on Main1..4 when enabled and walking bass/melody has E, G, or B
        if (is_main && opt.synthesize_cv4_dim_on_mains && (has_walking_bass || has_maj7_melody)) {
            synthCv('4', "C Dim", true, true, -2);
        }
    }

    style.elements.swap(rebuilt);
}

// =========================================================================
// Rule 3: bass_register_guard
// =========================================================================
inline void applyBassRegisterGuard(Style& style,
                                   const EnhanceOptions& opt,
                                   EnhanceStats& stats,
                                   std::vector<EnhancementAction>& actions) {
    const uint8_t floor_midi = clampMidi7(static_cast<int>(opt.bass_floor_midi), 23u, 36u);
    for (auto& el : style.elements) {
        auto notes = extractChannelNotes(el, kBassCh);
        if (notes.empty()) continue;
        const uint32_t bar_ticks = barTicksFor(style.ppqn, el.num, el.den);
        bool changed = false;
        for (auto& n : notes) {
            if (n.pitch < floor_midi) {
                const uint8_t orig = n.pitch;
                int folded = static_cast<int>(orig);
                while (folded < static_cast<int>(floor_midi) && folded + 12 <= 127) {
                    folded += 12;
                }
                const uint8_t target = static_cast<uint8_t>(folded);
                if (target != orig) {
                    n.pitch = target;
                    changed = true;
                    ++stats.notes_pitch_adjusted;
                    EnhancementAction act;
                    act.rule_id = "bass_register_guard";
                    act.section = el.marker;
                    act.korg_ch = kBassCh;
                    act.track = korgTrackName(kBassCh);
                    act.bar = bar1Based(n.on_tick, bar_ticks);
                    act.tick = n.on_tick;
                    act.before_summary = "pitch=" + std::to_string(orig) + "(" + pitchName(orig) + ")";
                    act.after_summary = "pitch=" + std::to_string(target) + "(" + pitchName(target) + ")";
                    act.musical_reason = "Octave-folded sub-floor (<MIDI " + std::to_string(floor_midi) +
                                         ") bass note up for clear Pa5X low-end articulation";
                    act.reversible = true;
                    actions.push_back(std::move(act));
                }
            }
        }
        if (changed) {
            deoverlapChannelNotes(notes, el.length, true);
            replaceChannelNotes(el, kBassCh, notes);
        }
    }
}

// =========================================================================
// Rule 4: piano_mud_voicing
// =========================================================================
inline void applyPianoMudVoicing(Style& style,
                                 const EnhanceOptions& opt,
                                 const Report* report,
                                 EnhanceStats& stats,
                                 std::vector<EnhancementAction>& actions) {
    const uint8_t ceil_midi = clampMidi7(static_cast<int>(opt.piano_mud_ceiling_midi), 40u, 55u);
    const uint32_t onset_win = (style.ppqn >= 16u) ? (style.ppqn / 16u) : 1u;

    for (auto& el : style.elements) {
        const uint32_t bar_ticks = barTicksFor(style.ppqn, el.num, el.den);
        for (uint8_t ch = kAcc1Ch; ch <= 15u; ++ch) {
            if (isExemptFromPianoMud(ch, firstTick0Pc(el, ch), report)) continue;
            auto notes = extractChannelNotes(el, ch);
            if (notes.size() < 2u) continue;

            bool ch_changed = false;
            size_t i = 0;
            while (i < notes.size()) {
                const uint32_t t0 = notes[i].on_tick;
                size_t j = i;
                std::vector<size_t> low_idx;
                while (j < notes.size() && (notes[j].on_tick - t0) <= onset_win) {
                    if (notes[j].pitch < ceil_midi) {
                        low_idx.push_back(j);
                    }
                    ++j;
                }
                if (low_idx.size() >= 2u) {
                    std::sort(low_idx.begin(), low_idx.end(),
                              [&](size_t a, size_t b) { return notes[a].pitch < notes[b].pitch; });
                    // Check if any adjacent pair in low_idx has interval <= 4 semitones (mud cluster).
                    bool has_mud = false;
                    for (size_t k = 1u; k < low_idx.size(); ++k) {
                        const uint8_t p_prev = notes[low_idx[k - 1u]].pitch;
                        const uint8_t p_curr = notes[low_idx[k]].pitch;
                        if (p_curr > p_prev && (p_curr - p_prev) <= 4u) {
                            has_mud = true;
                            break;
                        }
                    }
                    if (has_mud) {
                        std::ostringstream before_ss, after_ss;
                        before_ss << "[";
                        for (size_t k = 0; k < low_idx.size(); ++k) {
                            if (k > 0u) before_ss << ",";
                            before_ss << static_cast<unsigned>(notes[low_idx[k]].pitch);
                        }
                        before_ss << "]";

                        const std::vector<size_t> orig_low_idx = low_idx;
                        std::vector<uint8_t> orig_cluster_pitches;
                        orig_cluster_pitches.reserve(j - i);
                        for (size_t idx = i; idx < j; ++idx) {
                            orig_cluster_pitches.push_back(notes[idx].pitch);
                        }

                        // Iterate to fixed point within this chord cluster so sub-C2 (< MIDI 36)
                        // close clusters (e.g. [32, 35, 44] -> [32, 44, 47] -> [32, 44, 59])
                        // converge cleanly in a single enhanceStyle() pass.
                        for (int iter = 0; iter < 6; ++iter) {
                            std::vector<size_t> cur_low;
                            for (size_t idx = i; idx < j; ++idx) {
                                if (notes[idx].pitch < ceil_midi) cur_low.push_back(idx);
                            }
                            if (cur_low.size() < 2u) break;
                            std::sort(cur_low.begin(), cur_low.end(),
                                      [&](size_t a, size_t b) { return notes[a].pitch < notes[b].pitch; });

                            bool cur_has_mud = false;
                            for (size_t k = 1u; k < cur_low.size(); ++k) {
                                const uint8_t p_prev = notes[cur_low[k - 1u]].pitch;
                                const uint8_t p_curr = notes[cur_low[k]].pitch;
                                if (p_curr > p_prev && (p_curr - p_prev) <= 4u) {
                                    cur_has_mud = true;
                                    break;
                                }
                            }
                            if (!cur_has_mud) break;

                            uint32_t moved_in_iter = 0u;
                            // Keep lowest anchor tone cur_low[0] untouched; open-voice colliding upper tones.
                            uint8_t prev_kept = notes[cur_low[0]].pitch;
                            for (size_t k = 1u; k < cur_low.size(); ++k) {
                                const size_t idx = cur_low[k];
                                const uint8_t p = notes[idx].pitch;
                                if (p > prev_kept && (p - prev_kept) <= 4u) {
                                    const int lifted_i = static_cast<int>(p) + 12;
                                    if (lifted_i <= 84 &&
                                        !hasSimultaneousPitchIn(notes, idx, t0, onset_win,
                                                                static_cast<uint8_t>(lifted_i))) {
                                        notes[idx].pitch = static_cast<uint8_t>(lifted_i);
                                        ++moved_in_iter;
                                        continue;
                                    }
                                }
                                prev_kept = p;
                            }
                            if (moved_in_iter == 0u) break;
                        }

                        uint32_t moved = 0u;
                        for (size_t idx = i; idx < j; ++idx) {
                            if (notes[idx].pitch != orig_cluster_pitches[idx - i]) {
                                ++moved;
                            }
                        }

                        if (moved > 0u) {
                            ch_changed = true;
                            stats.notes_pitch_adjusted += moved;
                            std::vector<uint8_t> post_pitches;
                            post_pitches.reserve(orig_low_idx.size());
                            for (size_t idx : orig_low_idx) post_pitches.push_back(notes[idx].pitch);
                            std::sort(post_pitches.begin(), post_pitches.end());
                            after_ss << "[";
                            for (size_t k = 0; k < post_pitches.size(); ++k) {
                                if (k > 0u) after_ss << ",";
                                after_ss << static_cast<unsigned>(post_pitches[k]);
                            }
                            after_ss << "] (open voicing)";

                            EnhancementAction act;
                            act.rule_id = "piano_mud_voicing";
                            act.section = el.marker;
                            act.korg_ch = ch;
                            act.track = korgTrackName(ch);
                            act.bar = bar1Based(t0, bar_ticks);
                            act.tick = t0;
                            act.before_summary = before_ss.str();
                            act.after_summary = after_ss.str();
                            act.musical_reason = "Opened dense <C3 close-position chord cluster while preserving bass anchor";
                            act.reversible = true;
                            actions.push_back(std::move(act));
                        }
                    }
                }
                i = j;
            }
            if (ch_changed) {
                deoverlapChannelNotes(notes, el.length, true);
                replaceChannelNotes(el, ch, notes);
            }
        }
    }
}

// =========================================================================
// Rule 2: drum_ghost_dynamics
// =========================================================================
inline void applyDrumGhostDynamics(Style& style,
                                   const EnhanceOptions& opt,
                                   EnhanceStats& stats,
                                   std::vector<EnhancementAction>& actions) {
    const int max_delta = static_cast<int>(clampMidi7(static_cast<int>(opt.max_drum_vel_delta), 1u, 10u));
    const uint32_t q = style.ppqn;
    if (q == 0u) return;
    const uint32_t beat_tol = std::max<uint32_t>(1u, q / 16u);

    for (auto& el : style.elements) {
        auto notes = extractChannelNotes(el, kDrumCh);
        if (notes.empty()) continue;
        const uint32_t bar_ticks = barTicksFor(style.ppqn, el.num, el.den);
        if (bar_ticks == 0u) continue;

        // Snapshot pre-rule velocities so cumulative delta across Sub-steps 1..3
        // never exceeds max_drum_vel_delta on any note.
        std::vector<uint8_t> orig_vel;
        orig_vel.reserve(notes.size());
        for (const auto& n : notes) orig_vel.push_back(n.vel);

        std::vector<size_t> snare_idx, hat_idx;
        std::vector<uint8_t> snare_v, hat_v;
        uint32_t ghost_snares = 0;

        for (size_t i = 0; i < notes.size(); ++i) {
            const uint8_t p = notes[i].pitch;
            if (p == 38u || p == 40u) {
                snare_idx.push_back(i);
                snare_v.push_back(notes[i].vel);
                if (notes[i].vel < 55u) ++ghost_snares;
            } else if (p == 42u) {
                hat_idx.push_back(i);
                hat_v.push_back(notes[i].vel);
            }
        }

        const double snare_std = computeStddevVel(snare_v);
        const double hat_std   = computeStddevVel(hat_v);
        const bool already_dynamic_drum = (snare_std >= 3.0 || ghost_snares > 0u);
        const bool is_fill = isFillMarker(el.marker);
        double current_hat_std = hat_std;
        bool el_changed = false;

        // 1. Flat snare velocity shaping (strictly skips already-dynamic drums AND Fill elements,
        //    since Fills are turnaround rolls/crescendos rather than backbeat grooves!)
        if (!is_fill && !already_dynamic_drum && snare_idx.size() >= 2u) {
            auto isBackbeat = [&](uint32_t tick) -> int {
                const uint32_t in_bar = tick % bar_ticks;
                const uint32_t beat_len = (el.den == 8u) ? (q / 2u) : q;
                if (beat_len == 0u) return 0;
                const uint32_t b_idx = (in_bar + beat_tol) / beat_len;
                const uint32_t nearest = b_idx * beat_len;
                const uint32_t dist = (in_bar >= nearest) ? (in_bar - nearest) : (nearest - in_bar);
                if (dist > beat_tol) return 0;  // off-beat
                if (el.num == 4u && el.den == 4u) {
                    if (b_idx == 1u) return 2;  // beat 2 primary backbeat
                    if (b_idx == 3u) return 1;  // beat 4 secondary backbeat
                } else if (el.num == 3u && el.den == 4u) {
                    if (b_idx == 1u) return 2;
                    if (b_idx == 2u) return 1;
                } else if (el.num == 6u && el.den == 8u) {
                    if (b_idx == 3u) return 2;
                }
                return (b_idx % 2u == 1u) ? 2 : 1;
            };

            bool has_offbeat = false;
            bool has_backbeat = false;
            for (size_t idx : snare_idx) {
                const int cls = isBackbeat(notes[idx].on_tick);
                if (cls == 0) has_offbeat = true;
                else has_backbeat = true;
            }

            std::vector<uint8_t> cand_v;
            cand_v.reserve(snare_idx.size());
            const int hi_d = std::min(max_delta, 4);
            const int lo_d = std::min(max_delta, 6);
            for (size_t k = 0; k < snare_idx.size(); ++k) {
                const size_t idx = snare_idx[k];
                const int cls = isBackbeat(notes[idx].on_tick);
                int d = 0;
                if (has_offbeat && has_backbeat) {
                    d = (cls > 0) ? hi_d : -lo_d;
                } else {
                    // Differentiate primary vs secondary backbeat so std rises cleanly >= 3.0.
                    d = ((cls == 2) || (cls == 0 && (k % 2u == 0u))) ? hi_d : -lo_d;
                }
                cand_v.push_back(clampMidi7(static_cast<int>(notes[idx].vel) + d, 1u, 127u));
            }
            const double new_std = computeStddevVel(cand_v);
            if (new_std >= 3.0 && new_std > snare_std) {
                for (size_t k = 0; k < snare_idx.size(); ++k) {
                    if (notes[snare_idx[k]].vel != cand_v[k]) {
                        notes[snare_idx[k]].vel = cand_v[k];
                        ++stats.notes_velocity_adjusted;
                    }
                }
                el_changed = true;
                EnhancementAction act;
                act.rule_id = "drum_ghost_dynamics";
                act.section = el.marker;
                act.korg_ch = kDrumCh;
                act.track = korgTrackName(kDrumCh);
                act.bar = 0;
                act.tick = 0;
                act.before_summary = "snare vel_std=" + fmt2(snare_std);
                act.after_summary = "snare vel_std=" + fmt2(new_std) + " (bounded +-" + std::to_string(max_delta) + ")";
                act.musical_reason = "Added bounded metric backbeat/ghost velocity nuance to flat snare groove";
                act.reversible = true;
                actions.push_back(std::move(act));
            }
        }

        // 2. Flat closed Hi-Hat (42) velocity shaping (strictly skips already-dynamic drums!)
        if (!already_dynamic_drum && hat_idx.size() >= 2u && hat_std < 3.0) {
            std::vector<uint8_t> cand_v;
            cand_v.reserve(hat_idx.size());
            const int hi_d = std::min(max_delta, 4);
            const int lo_d = std::min(max_delta, 5);
            for (size_t k = 0; k < hat_idx.size(); ++k) {
                const uint32_t rem = notes[hat_idx[k]].on_tick % q;
                const bool on_beat = (rem <= beat_tol || (q - rem) <= beat_tol);
                const int d = on_beat ? hi_d : -lo_d;
                cand_v.push_back(clampMidi7(static_cast<int>(notes[hat_idx[k]].vel) + d, 1u, 127u));
            }
            const double new_std = computeStddevVel(cand_v);
            if (new_std >= 3.0 && new_std > hat_std) {
                for (size_t k = 0; k < hat_idx.size(); ++k) {
                    if (notes[hat_idx[k]].vel != cand_v[k]) {
                        notes[hat_idx[k]].vel = cand_v[k];
                        ++stats.notes_velocity_adjusted;
                    }
                }
                current_hat_std = new_std;
                el_changed = true;
                EnhancementAction act;
                act.rule_id = "drum_ghost_dynamics";
                act.section = el.marker;
                act.korg_ch = kDrumCh;
                act.track = korgTrackName(kDrumCh);
                act.bar = 0;
                act.tick = 0;
                act.before_summary = "hi-hat(42) vel_std=" + fmt2(hat_std);
                act.after_summary = "hi-hat(42) vel_std=" + fmt2(new_std);
                act.musical_reason = "Added bounded down-up stick accent contour to flat closed hi-hat";
                act.reversible = true;
                actions.push_back(std::move(act));
            }
        }

        // 3. Optional gentle 2nd-half Fill turnaround crescendo (zero invented notes; skips dynamic drums!)
        if (!already_dynamic_drum && opt.enhance_fill_crescendo && is_fill && el.length >= 2u) {
            const uint32_t half = el.length / 2u;
            std::vector<uint8_t> h1, h2;
            std::vector<size_t> h2_idx;
            for (size_t i = 0; i < notes.size(); ++i) {
                if (notes[i].on_tick < half) h1.push_back(notes[i].vel);
                else { h2.push_back(notes[i].vel); h2_idx.push_back(i); }
            }
            if (!h1.empty() && !h2.empty()) {
                const double m1 = computeMeanVel(h1);
                const double m2 = computeMeanVel(h2);
                if (m2 <= m1) {
                    const int gap = static_cast<int>(std::ceil(m1 - m2));
                    if (gap < max_delta) {
                        const int lift_lo = std::clamp(gap + 1, 1, max_delta);
                        const int lift_hi = std::clamp(gap + 3, lift_lo, max_delta);
                        const double span = static_cast<double>(el.length - half);
                        std::vector<uint8_t> new_h2;
                        new_h2.reserve(h2_idx.size());
                        for (size_t idx : h2_idx) {
                            const double frac = (span > 0.0)
                                ? (static_cast<double>(notes[idx].on_tick - half) / span)
                                : 1.0;
                            const int add = std::clamp(
                                static_cast<int>(std::lround(
                                    static_cast<double>(lift_lo) +
                                    frac * static_cast<double>(lift_hi - lift_lo))),
                                1, max_delta);
                            const uint8_t lo_bound = clampMidi7(static_cast<int>(orig_vel[idx]) - max_delta, 1u, 127u);
                            const uint8_t hi_bound = clampMidi7(static_cast<int>(orig_vel[idx]) + max_delta, 1u, 127u);
                            new_h2.push_back(clampMidi7(static_cast<int>(notes[idx].vel) + add, lo_bound, hi_bound));
                        }
                        const double m2_after = computeMeanVel(new_h2);
                        // Guard Pass-2 idempotence: never let Sub-step 3 collapse hat_std back < 3.0.
                        bool preserves_hat_std = true;
                        if (hat_idx.size() >= 2u && current_hat_std >= 3.0) {
                            std::vector<uint8_t> post_vel(notes.size());
                            for (size_t i = 0; i < notes.size(); ++i) post_vel[i] = notes[i].vel;
                            for (size_t k = 0; k < h2_idx.size(); ++k) post_vel[h2_idx[k]] = new_h2[k];
                            std::vector<uint8_t> post_hat;
                            post_hat.reserve(hat_idx.size());
                            for (size_t idx : hat_idx) post_hat.push_back(post_vel[idx]);
                            if (computeStddevVel(post_hat) < 3.0) preserves_hat_std = false;
                        }
                        if (m2_after > m1 && preserves_hat_std) {
                            for (size_t k = 0; k < h2_idx.size(); ++k) {
                                if (notes[h2_idx[k]].vel != new_h2[k]) {
                                    notes[h2_idx[k]].vel = new_h2[k];
                                    ++stats.notes_velocity_adjusted;
                                }
                            }
                            el_changed = true;
                            EnhancementAction act;
                            act.rule_id = "drum_ghost_dynamics";
                            act.section = el.marker;
                            act.korg_ch = kDrumCh;
                            act.track = korgTrackName(kDrumCh);
                            act.bar = 1;
                            act.tick = half;
                            act.before_summary = "fill 2nd-half mean_vel=" + fmt2(m2) + " (1st-half=" + fmt2(m1) + ")";
                            act.after_summary = "fill 2nd-half mean_vel=" + fmt2(m2_after);
                            act.musical_reason = "Applied gentle 2nd-half turnaround crescendo on flat/decrescendo Fill";
                            act.reversible = true;
                            actions.push_back(std::move(act));
                        }
                    }
                }
            }
        }

        if (el_changed) {
            replaceChannelNotes(el, kDrumCh, notes);
        }
    }
}

// =========================================================================
// Rule 1: variation_energy_curve
// =========================================================================
inline void applyVariationEnergyCurve(Style& style,
                                      const EnhanceOptions& opt,
                                      EnhanceStats& stats,
                                      std::vector<EnhancementAction>& actions) {
    struct VarInfo {
        char                    digit = '1';
        double                  mean_vel = 0.0;
        double                  equal_track_mean = 0.0;
        double                  notes_per_bar = 0.0;
        uint32_t                note_count = 0;
        std::array<double, 8>   ch_mean_vel{};
        std::array<bool, 8>     ch_active{};
    };

    std::map<char, VarInfo> var_map;
    for (const auto& el : style.elements) {
        if (!isVariationMarker(el.marker) || cvNumberOf(el.marker) != 1u) continue;
        const uint32_t bt = barTicksFor(style.ppqn, el.num, el.den);
        const uint32_t bars = (bt > 0u && el.length >= bt) ? (el.length / bt) : 1u;
        std::vector<uint8_t> vels;
        std::array<std::vector<uint8_t>, 8> ch_vels;
        for (const auto& ev : el.events) {
            if ((ev.status & 0xF0u) == 0x90u && ev.d2 > 0u) {
                const uint8_t v = static_cast<uint8_t>(ev.d2 & 0x7Fu);
                vels.push_back(v);
                const uint8_t ch = static_cast<uint8_t>(ev.status & 0x0Fu);
                if (ch >= kBassCh && ch <= 15u) {
                    ch_vels[ch - kBassCh].push_back(v);
                }
            }
        }
        if (vels.empty()) continue;
        VarInfo vi;
        vi.digit = el.marker[1];
        vi.mean_vel = computeMeanVel(vels);
        vi.notes_per_bar = static_cast<double>(vels.size()) / static_cast<double>(bars);
        vi.note_count = static_cast<uint32_t>(vels.size());
        double track_sum = 0.0;
        uint32_t active_tracks = 0u;
        for (size_t c = 0u; c < 8u; ++c) {
            if (!ch_vels[c].empty()) {
                vi.ch_active[c] = true;
                vi.ch_mean_vel[c] = computeMeanVel(ch_vels[c]);
                track_sum += vi.ch_mean_vel[c];
                ++active_tracks;
            }
        }
        vi.equal_track_mean = (active_tracks > 0u) ? (track_sum / static_cast<double>(active_tracks)) : vi.mean_vel;
        var_map[vi.digit] = vi;
    }

    if (var_map.size() < 2u) return;

    std::vector<VarInfo> vars;
    for (const auto& [d, vi] : var_map) {
        (void)d;
        vars.push_back(vi);
    }

    // Equal-weight pairwise step over shared active channels (immune to Simpson's paradox
    // when later Variations add dense soft percussion or backing layers).
    auto sharedTrackDelta = [](const VarInfo& a, const VarInfo& b) -> double {
        double sum = 0.0;
        uint32_t count = 0u;
        for (size_t c = 0u; c < 8u; ++c) {
            if (a.ch_active[c] && b.ch_active[c]) {
                sum += (b.ch_mean_vel[c] - a.ch_mean_vel[c]);
                ++count;
            }
        }
        if (count > 0u) return sum / static_cast<double>(count);
        return b.equal_track_mean - a.equal_track_mean;
    };

    // Exempt any Variation with intentional sparse-fortissimo contrast (>= +6.0 above V1).
    const double v1_vel = vars.front().mean_vel;
    std::vector<size_t> eligible_idx;
    for (size_t i = 0; i < vars.size(); ++i) {
        if (i > 0u && vars[i].notes_per_bar < vars[0].notes_per_bar * 0.90 &&
            (vars[i].mean_vel >= v1_vel + 6.0 || sharedTrackDelta(vars[0], vars[i]) >= 6.0)) {
            continue;
        }
        eligible_idx.push_back(i);
    }
    if (eligible_idx.size() < 2u) return;

    // Build chained equal-weight shared-track energy curve across eligible Variations.
    const size_t n = eligible_idx.size();
    std::vector<double> eff_vel(n, 0.0);
    eff_vel[0] = vars[eligible_idx[0]].equal_track_mean;
    double min_v = eff_vel[0], max_v = eff_vel[0];
    bool has_inversion = false;
    for (size_t k = 1u; k < n; ++k) {
        const auto& prev = vars[eligible_idx[k - 1u]];
        const auto& cur  = vars[eligible_idx[k]];
        const double step = sharedTrackDelta(prev, cur);
        eff_vel[k] = eff_vel[k - 1u] + step;
        if (eff_vel[k] < min_v) min_v = eff_vel[k];
        if (eff_vel[k] > max_v) max_v = eff_vel[k];
        if (step < -2.0 && cur.notes_per_bar >= prev.notes_per_bar) {
            has_inversion = true;
        }
    }

    // Exempt styles with an already-strong monotonic arrangement-density arc (e.g., EDM build-up
    // escalating >= 1.5x notes/bar across V1..V4 without velocity inversion).
    if (!has_inversion) {
        const double first_npb = vars[eligible_idx.front()].notes_per_bar;
        const double last_npb  = vars[eligible_idx.back()].notes_per_bar;
        if (first_npb > 0.0 && last_npb >= first_npb * 1.5 &&
            eff_vel.back() >= eff_vel.front() - 1.5) {
            bool monotonic_density = true;
            for (size_t k = 1u; k < n; ++k) {
                if (vars[eligible_idx[k]].notes_per_bar <
                    vars[eligible_idx[k - 1u]].notes_per_bar * 0.95) {
                    monotonic_density = false;
                    break;
                }
            }
            if (monotonic_density) return;
        }
    }

    const bool is_flat = ((max_v - min_v) < 3.5);
    if (!is_flat && !has_inversion) return;

    const double max_pct = std::clamp(std::isfinite(opt.max_variation_vel_scale_pct)
                                          ? opt.max_variation_vel_scale_pct : 6.0,
                                      1.0, 8.0);
    const double half_span = max_pct / 100.0;

    // Compute per-variation bounded additive delta (preserves track velocity std-dev 100%
    // verbatim without dynamic compression, capped within +/-12 MIDI velocity units).
    std::map<char, int> var_delta;
    const double avg_v = 0.5 * (min_v + max_v);
    for (size_t k = 0; k < n; ++k) {
        const double frac = (n > 1u) ? (2.0 * static_cast<double>(k) / static_cast<double>(n - 1u) - 1.0) : 0.0;
        const auto& vi = vars[eligible_idx[k]];
        double target_mean = avg_v * (1.0 + frac * half_span);
        if (avg_v * 2.0 * half_span < 4.5) {
            target_mean = avg_v + frac * 2.5;
        }
        int delta = static_cast<int>(std::lround(target_mean - eff_vel[k]));
        delta = std::clamp(delta, -12, 12);
        var_delta[vi.digit] = delta;
    }

    for (auto& el : style.elements) {
        if (!isVariationMarker(el.marker)) continue;
        const auto it = var_delta.find(el.marker[1]);
        if (it == var_delta.end()) continue;
        const int delta = it->second;
        if (delta == 0) continue;

        for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
            std::vector<uint8_t> before_v, after_v;
            std::vector<size_t> ev_idx;
            for (size_t idx = 0; idx < el.events.size(); ++idx) {
                const auto& ev = el.events[idx];
                if ((ev.status & 0xF0u) == 0x90u && (ev.status & 0x0Fu) == ch && ev.d2 > 0u) {
                    before_v.push_back(ev.d2);
                    ev_idx.push_back(idx);
                    after_v.push_back(clampMidi7(static_cast<int>(ev.d2) + delta, 1u, 127u));
                }
            }
            if (ev_idx.empty()) continue;

            // Anti-compression guard: never compress track stddev below 95% of pre-shift stddev.
            const double std_before = computeStddevVel(before_v);
            const double std_after  = computeStddevVel(after_v);
            if (std_before >= 1.0 && std_after < 0.95 * std_before) {
                continue;
            }

            uint32_t changed_notes = 0;
            for (size_t k = 0; k < ev_idx.size(); ++k) {
                if (el.events[ev_idx[k]].d2 != after_v[k]) {
                    el.events[ev_idx[k]].d2 = after_v[k];
                    ++changed_notes;
                    ++stats.notes_velocity_adjusted;
                }
            }
            if (changed_notes > 0u) {
                EnhancementAction act;
                act.rule_id = "variation_energy_curve";
                act.section = el.marker;
                act.korg_ch = ch;
                act.track = korgTrackName(ch);
                act.bar = 0;
                act.tick = 0;
                act.before_summary = "mean_vel=" + fmt2(computeMeanVel(before_v));
                act.after_summary = "mean_vel=" + fmt2(computeMeanVel(after_v)) +
                                    " (delta=" + (delta >= 0 ? "+" : "") + std::to_string(delta) +
                                    ", " + std::to_string(changed_notes) + " notes)";
                act.musical_reason = "Shaped flat/inverted V1..V4 energy progression without clipping or compression";
                act.reversible = true;
                actions.push_back(std::move(act));
            }
        }
    }
}

// =========================================================================
// Rule 5: ending_fade_pop_guard (Ending CC11 fade pop + looping CC11 de-click ramp)
// =========================================================================
inline void applyEndingFadePopGuard(Style& style,
                                    const EnhanceOptions& opt,
                                    EnhanceStats& stats,
                                    std::vector<EnhancementAction>& actions) {
    const int jump_thresh = static_cast<int>(clampMidi7(static_cast<int>(opt.cc11_declick_jump_threshold), 24u, 96u));
    const uint8_t ramp_steps = clampMidi7(static_cast<int>(opt.cc11_ramp_steps), 2u, 6u);

    for (auto& el : style.elements) {
        if (el.length < 2u) continue;
        const uint32_t bar_ticks = barTicksFor(style.ppqn, el.num, el.den);
        const bool is_ending = isEndingMarker(el.marker);
        const uint32_t sixteenth_bar = (bar_ticks >= 16u) ? (bar_ticks / 16u) : 1u;

        std::set<size_t> remove_indices;
        std::vector<Event> added_events;

        for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
            struct CcPt { size_t idx; uint32_t tick; uint8_t val; };
            std::vector<CcPt> seq;
            for (size_t idx = 0; idx < el.events.size(); ++idx) {
                const auto& ev = el.events[idx];
                if ((ev.status & 0xF0u) == 0xB0u && (ev.status & 0x0Fu) == ch && ev.d1 == 11u) {
                    seq.push_back(CcPt{idx, ev.tick, static_cast<uint8_t>(ev.d2 & 0x7Fu)});
                }
            }
            if (seq.size() < 2u) continue;

            uint32_t last_note_on_tick = 0u;
            bool has_note_on = false;
            for (const auto& ev : el.events) {
                if ((ev.status & 0xF0u) == 0x90u && (ev.status & 0x0Fu) == ch && ev.d2 > 0u) {
                    if (!has_note_on || ev.tick > last_note_on_tick) last_note_on_tick = ev.tick;
                    has_note_on = true;
                }
            }

            if (is_ending) {
                const auto& last = seq.back();
                const auto& prev = seq[seq.size() - 2u];
                const bool trailing_fade_low = (prev.tick >= el.length / 2u && prev.val < 32u && prev.tick < last.tick);
                const bool boundary_snapback = (last.tick + 2u >= el.length && last.val >= 64u &&
                                                (static_cast<int>(last.val) - static_cast<int>(prev.val)) >= 32 &&
                                                (!has_note_on || last.tick > last_note_on_tick));
                if (trailing_fade_low && boundary_snapback) {
                    remove_indices.insert(last.idx);
                    ++stats.cc_events_removed;

                    EnhancementAction act;
                    act.rule_id = "ending_fade_pop_guard";
                    act.section = el.marker;
                    act.korg_ch = ch;
                    act.track = korgTrackName(ch);
                    act.bar = bar1Based(last.tick, bar_ticks);
                    act.tick = last.tick;
                    act.before_summary = "CC11@" + std::to_string(last.tick) + "=" + std::to_string(last.val) +
                                         " after trailing fade to " + std::to_string(prev.val);
                    act.after_summary = "removed final-tick CC11 snap-back (fade ends cleanly at CC11=" +
                                        std::to_string(prev.val) + ")";
                    act.musical_reason = "Prevents audible reverb/release tail-swell pop on one-shot Ending fade-out";
                    act.reversible = true;
                    actions.push_back(std::move(act));
                }
            } else if (opt.enable_loop_cc11_declick_ramp) {
                const auto& last = seq.back();
                const auto& prev = seq[seq.size() - 2u];
                const int delta = static_cast<int>(last.val) - static_cast<int>(prev.val);
                const int abs_delta = std::abs(delta);
                // Detect intentional periodic EDM sidechain pumping (repeated CC11 dip-release
                // cycles across beats/bars) so rhythmic sidechain envelopes are never ramped.
                uint32_t earlier_large_jumps = 0u;
                uint32_t earlier_up_jumps = 0u;
                uint32_t earlier_down_jumps = 0u;
                for (size_t k = 1u; k + 1u < seq.size(); ++k) {
                    const int step = static_cast<int>(seq[k].val) - static_cast<int>(seq[k - 1u].val);
                    if (std::abs(step) >= jump_thresh) {
                        ++earlier_large_jumps;
                        if (step > 0) ++earlier_up_jumps;
                        else ++earlier_down_jumps;
                    }
                }
                const bool periodic_sidechain_pump =
                    (earlier_large_jumps >= 4u && earlier_up_jumps >= 2u && earlier_down_jumps >= 2u);
                if (last.tick + 2u >= el.length && !periodic_sidechain_pump &&
                    abs_delta >= jump_thresh && last.tick > prev.tick + 1u) {
                    const int max_step_allowed = std::max(1, jump_thresh - 1);
                    const uint32_t intervals_needed = static_cast<uint32_t>(
                        (abs_delta + max_step_allowed - 1) / max_step_allowed);
                    const uint32_t min_steps_needed = (intervals_needed > 1u) ? (intervals_needed - 1u) : 1u;
                    const uint32_t target_steps = std::clamp<uint32_t>(
                        std::max<uint32_t>(ramp_steps, min_steps_needed), 1u, 6u);
                    const uint32_t win = std::max<uint32_t>(sixteenth_bar, target_steps + 1u);
                    const uint32_t desired_start = (last.tick > win) ? (last.tick - win) : (prev.tick + 1u);
                    const uint32_t ramp_start = std::max(prev.tick + 1u, desired_start);
                    if (last.tick > ramp_start) {
                        const uint32_t span = last.tick - ramp_start;
                        const uint32_t steps = std::min<uint32_t>(target_steps, span);
                        const int ceil_step = (abs_delta + static_cast<int>(steps)) / static_cast<int>(steps + 1u);
                        if (steps >= 1u && ceil_step < jump_thresh) {
                            std::vector<Event> cand_ramp;
                            cand_ramp.reserve(steps);
                            uint8_t prev_pt = prev.val;
                            int max_actual_step = 0;
                            for (uint32_t s = 1u; s <= steps; ++s) {
                                const uint32_t t = ramp_start + (span * (s - 1u)) / steps;
                                if (t <= prev.tick || t >= last.tick) continue;
                                const double frac = static_cast<double>(s) / static_cast<double>(steps + 1u);
                                const uint8_t v = clampMidi7(
                                    static_cast<int>(std::lround(static_cast<double>(prev.val) +
                                                                 frac * static_cast<double>(delta))),
                                    0u, 127u);
                                const int step_diff = std::abs(static_cast<int>(v) - static_cast<int>(prev_pt));
                                if (step_diff > max_actual_step) max_actual_step = step_diff;
                                prev_pt = v;
                                cand_ramp.push_back(Event{t, static_cast<uint8_t>(0xB0u | ch), 11u, v});
                            }
                            const int final_diff = std::abs(static_cast<int>(last.val) - static_cast<int>(prev_pt));
                            if (final_diff > max_actual_step) max_actual_step = final_diff;
                            if (!cand_ramp.empty() && max_actual_step < jump_thresh) {
                                for (const auto& ev : cand_ramp) {
                                    added_events.push_back(ev);
                                    ++stats.cc_events_added;
                                }
                                EnhancementAction act;
                                act.rule_id = "ending_fade_pop_guard";
                                act.section = el.marker;
                                act.korg_ch = ch;
                                act.track = korgTrackName(ch);
                                act.bar = bar1Based(last.tick, bar_ticks);
                                act.tick = last.tick;
                                act.before_summary = "single-step CC11 jump " + std::to_string(prev.val) +
                                                     " -> " + std::to_string(last.val) + " at tick " + std::to_string(last.tick);
                                act.after_summary = "inserted " + std::to_string(cand_ramp.size()) +
                                                    "-step CC11 de-click ramp preserving loop reset target " +
                                                    std::to_string(last.val);
                                act.musical_reason = "Smooths abrupt loop-boundary CC11 recovery while preserving loop reset";
                                act.reversible = true;
                                actions.push_back(std::move(act));
                            }
                        }
                    }
                }
            }
        }

        if (!remove_indices.empty() || !added_events.empty()) {
            std::vector<Event> rebuilt;
            rebuilt.reserve(el.events.size() + added_events.size());
            for (size_t idx = 0; idx < el.events.size(); ++idx) {
                if (remove_indices.count(idx) == 0u) rebuilt.push_back(el.events[idx]);
            }
            for (const auto& ev : added_events) rebuilt.push_back(ev);
            el.events.swap(rebuilt);
            canonicalSortElement(el);
        }
    }
}

} // namespace style_enhancer_detail

inline EnhanceResult enhanceStyle(
    const Style& mode_a_style,
    const EnhanceOptions& opt = EnhanceOptions::faithful(),
    const Report* mode_a_report = nullptr,
    const uasf::StyleDefinition* source_uasf = nullptr) noexcept {
    (void)source_uasf;
    EnhanceResult res;
    res.mode = opt.mode;
    res.hardware_verified = false;
    res.l3_status = "NOT_TESTED";
    res.independent_from_frozen_q1 = true;

    try {
        using namespace style_enhancer_detail;

        // ── Mode A Bit-Exact Invariance Guarantee ───────────────────────
        if (!opt.anyEnabled()) {
            res.style = mode_a_style;
            res.stats.mode_a_bit_exact_bypass = true;
            res.validation_errors = validate(res.style);
            res.ok = res.validation_errors.empty();
            if (!res.ok) res.error = res.validation_errors.front();
            return res;
        }

        res.stats.mode_a_bit_exact_bypass = false;

        // Pre-capture Mode A invariants for anti-degradation guardrail verification.
        const auto before_hygiene = measureLoopHygiene(mode_a_style);
        const auto before_voices  = captureTick0Voices(mode_a_style);

        Style candidate = mode_a_style;
        EnhanceStats stats;
        stats.mode_a_bit_exact_bypass = false;
        std::vector<EnhancementAction> actions;

        // Canonical deterministic order: B6 -> B3 -> B4 -> B2 -> B1 -> B5
        if (opt.enhance_jazz_cv_melody_split) {
            applyJazzCvMelodySplit(candidate, opt, mode_a_report, stats, actions);
        }
        if (opt.enhance_bass_register_guard) {
            applyBassRegisterGuard(candidate, opt, stats, actions);
        }
        if (opt.enhance_piano_mud_voicing) {
            applyPianoMudVoicing(candidate, opt, mode_a_report, stats, actions);
        }
        if (opt.enhance_drum_ghost_dynamics) {
            applyDrumGhostDynamics(candidate, opt, stats, actions);
        }
        if (opt.enhance_variation_energy_curve) {
            applyVariationEnergyCurve(candidate, opt, stats, actions);
        }
        if (opt.enhance_ending_fade_pop_guard) {
            applyEndingFadePopGuard(candidate, opt, stats, actions);
        }

        for (auto& el : candidate.elements) {
            canonicalSortElement(el);
        }

        // ── Hard Post-Enhancement Anti-Degradation Guardrails ───────────
        std::vector<std::string> guard_errs = validate(candidate);
        if (!verifyNoSamePitchOverlap(candidate)) {
            guard_errs.push_back("post-enhancement guardrail: same-pitch overlap detected");
        }
        const auto after_hygiene = measureLoopHygiene(candidate);
        if (after_hygiene.open_cc64 > before_hygiene.open_cc64) {
            guard_errs.push_back("post-enhancement guardrail: introduced open CC64 at element end");
        }
        if (after_hygiene.uncentered_pb > before_hygiene.uncentered_pb) {
            guard_errs.push_back("post-enhancement guardrail: introduced uncentered Pitch Bend at element end");
        }
        if (after_hygiene.nrpn_leaks > before_hygiene.nrpn_leaks ||
            after_hygiene.cc94_leaks > before_hygiene.cc94_leaks) {
            guard_errs.push_back("post-enhancement guardrail: introduced proprietary NRPN/CC94 leak");
        }
        const auto after_voices = captureTick0Voices(candidate);
        for (const auto& [key, voice] : before_voices) {
            const auto it = after_voices.find(key);
            if (it == after_voices.end() || it->second != voice) {
                guard_errs.push_back("post-enhancement guardrail: tick-0 Bank/PC mutated on existing element");
                break;
            }
        }

        if (!guard_errs.empty()) {
            // Fail-closed rollback to Mode A input.
            res.style = mode_a_style;
            res.validation_errors = std::move(guard_errs);
            res.error = res.validation_errors.front();
            res.stats = EnhanceStats{};
            res.stats.mode_a_bit_exact_bypass = false;
            res.stats.rolled_back_on_guardrail = true;
            res.actions.clear();
            res.ok = false;
            return res;
        }

        stats.actions_applied = static_cast<uint32_t>(actions.size());
        res.style = std::move(candidate);
        res.stats = stats;
        res.actions = std::move(actions);
        res.ok = true;
        return res;
    } catch (...) {
        res.style = mode_a_style;
        res.ok = false;
        res.error = "unexpected exception during style enhancement (rolled back to Mode A)";
        res.stats = EnhanceStats{};
        res.stats.rolled_back_on_guardrail = true;
        res.actions.clear();
        return res;
    }
}

inline std::string formatEnhancementAuditJson(const EnhanceResult& res) {
    using namespace style_enhancer_detail;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"aiarranger-style-enhancement-audit/1\",\n";
    o << "  \"mode\": \"" << conversionModeName(res.mode) << "\",\n";
    o << "  \"ok\": " << (res.ok ? "true" : "false") << ",\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"NOT_TESTED\",\n";
    o << "  \"independentFromFrozenQ1\": true,\n";
    o << "  \"stats\": {\n";
    o << "    \"actionsApplied\": " << res.stats.actions_applied << ",\n";
    o << "    \"notesVelocityAdjusted\": " << res.stats.notes_velocity_adjusted << ",\n";
    o << "    \"notesPitchAdjusted\": " << res.stats.notes_pitch_adjusted << ",\n";
    o << "    \"ccEventsAdded\": " << res.stats.cc_events_added << ",\n";
    o << "    \"ccEventsRemoved\": " << res.stats.cc_events_removed << ",\n";
    o << "    \"cvElementsSynthesized\": " << res.stats.cv_elements_synthesized << ",\n";
    o << "    \"modeABitExactBypass\": " << (res.stats.mode_a_bit_exact_bypass ? "true" : "false") << ",\n";
    o << "    \"rolledBackOnGuardrail\": " << (res.stats.rolled_back_on_guardrail ? "true" : "false") << "\n";
    o << "  },\n";
    o << "  \"actions\": [\n";
    for (size_t i = 0; i < res.actions.size(); ++i) {
        const auto& a = res.actions[i];
        o << "    {\n";
        o << "      \"ruleId\": \"" << jsonEscape(a.rule_id) << "\",\n";
        o << "      \"section\": \"" << jsonEscape(a.section) << "\",\n";
        o << "      \"korgCh\": " << static_cast<unsigned>(a.korg_ch) << ",\n";
        o << "      \"track\": \"" << jsonEscape(a.track) << "\",\n";
        o << "      \"bar\": " << a.bar << ",\n";
        o << "      \"tick\": " << a.tick << ",\n";
        o << "      \"beforeSummary\": \"" << jsonEscape(a.before_summary) << "\",\n";
        o << "      \"afterSummary\": \"" << jsonEscape(a.after_summary) << "\",\n";
        o << "      \"musicalReason\": \"" << jsonEscape(a.musical_reason) << "\",\n";
        o << "      \"reversible\": " << (a.reversible ? "true" : "false") << "\n";
        o << "    }" << (i + 1u < res.actions.size() ? "," : "") << "\n";
    }
    o << "  ]\n";
    o << "}\n";
    return o.str();
}

inline std::string formatEnhancementAuditMarkdown(const EnhanceResult& res) {
    using namespace style_enhancer_detail;
    std::ostringstream o;
    o << "# Style Enhancement Audit Report\n\n";
    o << "- **Mode:** `" << conversionModeName(res.mode) << "`\n";
    o << "- **Status:** `" << (res.ok ? "OK" : "FAILED / ROLLED BACK") << "`\n";
    o << "- **Mode A Bit-Exact Bypass:** `" << (res.stats.mode_a_bit_exact_bypass ? "true" : "false") << "`\n";
    o << "- **Epistemic Boundary:** `hardwareVerified: false` | `l3Status: NOT_TESTED` | `independentFromFrozenQ1: true`\n";
    o << "- **Actions Applied:** `" << res.stats.actions_applied
      << "` (`vel=" << res.stats.notes_velocity_adjusted
      << "`, `pitch=" << res.stats.notes_pitch_adjusted
      << "`, `cc+=" << res.stats.cc_events_added
      << "`, `cc-=" << res.stats.cc_events_removed
      << "`, `cv+=" << res.stats.cv_elements_synthesized << "`)\n\n";

    if (res.actions.empty()) {
        o << "_Zero enhancements applied (bit-exact Mode A fidelity preserved)._\n";
        return o.str();
    }

    o << "| Rule | Section | Track | Bar:Tick | Before | After | Reason | Reversible |\n";
    o << "|---|---|---|---|---|---|---|---|\n";
    for (const auto& a : res.actions) {
        o << "| `" << mdEscape(a.rule_id) << "`"
          << " | `" << mdEscape(a.section) << "`"
          << " | " << mdEscape(a.track)
          << " | " << a.bar << ":" << a.tick
          << " | " << mdEscape(a.before_summary)
          << " | " << mdEscape(a.after_summary)
          << " | " << mdEscape(a.musical_reason)
          << " | " << (a.reversible ? "yes" : "no") << " |\n";
    }
    return o.str();
}

inline std::string formatEnhancementReportJson(const EnhanceResult& res) {
    return formatEnhancementAuditJson(res);
}

inline std::string formatEnhancementReportMd(const EnhanceResult& res) {
    return formatEnhancementAuditMarkdown(res);
}

} // namespace ai_arranger::exporters::korg::quality

#endif // AI_ARRANGER_EXPORTERS_KORG_STYLE_ENHANCER_H
