#ifndef AI_ARRANGER_EXPORTERS_KORG_MUSICAL_QUALITY_ANALYZER_H
#define AI_ARRANGER_EXPORTERS_KORG_MUSICAL_QUALITY_ANALYZER_H

#include "engine/uasf/types.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/korg_style_smf.h"

#include <algorithm>
#include <array>
#include <cctype>
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

// Standalone read-only 12-dimension track-by-track arranger musical quality
// analyzer (Phase 2 Mission 4).
//
// Architectural invariants:
//   - Strictly READ-ONLY: never mutates Korg Style, Report, or UASF input.
//   - Completely independent of frozen Q1 benchmark (style_quality_bench.h).
//   - Always stamps hardwareVerified=false, l3Status="NOT_TESTED", and
//     independentFromFrozenQ1=true (offline evaluation never replaces Pa5X ear tests).
//   - Portable C++20 header-only, zero JUCE/Apple dependencies, ODR-safe.

namespace ai_arranger::exporters::korg::quality {

enum class MusicalFindingSeverity : uint8_t {
    Info = 0,       // Informational strength or stylistic trait
    Advisory = 1,   // Mode B enhancement opportunity
    Warning = 2,    // Musical quality risk
    Critical = 3,   // Hard musical defect
};

inline const char* musicalFindingSeverityName(MusicalFindingSeverity sev) noexcept {
    switch (sev) {
        case MusicalFindingSeverity::Info:     return "INFO";
        case MusicalFindingSeverity::Advisory: return "ADVISORY";
        case MusicalFindingSeverity::Warning:  return "WARNING";
        case MusicalFindingSeverity::Critical: return "CRITICAL";
    }
    return "INFO";
}

enum class MusicalDimensionId : uint8_t {
    D01_DrumPercBalance = 1,
    D02_BassRegisterArticulation = 2,
    D03_GuitarStrumMegaVoice = 3,
    D04_PianoAccompanimentVoicing = 4,
    D05_VelocityDynamics = 5,
    D06_ExpressionControllers = 6,
    D07_SoundFamilySuitability = 7,
    D08_GrooveRhythmicFeel = 8,
    D09_VariationConsistency = 9,
    D10_FillTransitionQuality = 10,
    D11_IntroEndingIntegrity = 11,
    D12_MixBalanceEffects = 12,
};

inline const char* musicalDimensionName(MusicalDimensionId id) noexcept {
    switch (id) {
        case MusicalDimensionId::D01_DrumPercBalance:           return "Drum & Percussion Balance";
        case MusicalDimensionId::D02_BassRegisterArticulation:  return "Bass Register, Duration & Articulation";
        case MusicalDimensionId::D03_GuitarStrumMegaVoice:      return "Guitar Strumming & MegaVoice Integrity";
        case MusicalDimensionId::D04_PianoAccompanimentVoicing: return "Piano & Accompaniment Voicing";
        case MusicalDimensionId::D05_VelocityDynamics:          return "Velocity & Dynamics Preservation";
        case MusicalDimensionId::D06_ExpressionControllers:     return "Expression & Controller Translation";
        case MusicalDimensionId::D07_SoundFamilySuitability:    return "Sound-Family Suitability";
        case MusicalDimensionId::D08_GrooveRhythmicFeel:        return "Groove & Rhythmic Feel";
        case MusicalDimensionId::D09_VariationConsistency:      return "Variation-to-Variation Energy Curve";
        case MusicalDimensionId::D10_FillTransitionQuality:     return "Fill-to-Variation Transition Quality";
        case MusicalDimensionId::D11_IntroEndingIntegrity:      return "Intro & Ending Musical Integrity";
        case MusicalDimensionId::D12_MixBalanceEffects:         return "Mix Balance & Effects Translation";
    }
    return "Unknown";
}

inline const char* musicalDimensionKey(MusicalDimensionId id) noexcept {
    switch (id) {
        case MusicalDimensionId::D01_DrumPercBalance:           return "D01_drum_perc_balance";
        case MusicalDimensionId::D02_BassRegisterArticulation:  return "D02_bass_register_articulation";
        case MusicalDimensionId::D03_GuitarStrumMegaVoice:      return "D03_guitar_strum_megavoice";
        case MusicalDimensionId::D04_PianoAccompanimentVoicing: return "D04_piano_accompaniment_voicing";
        case MusicalDimensionId::D05_VelocityDynamics:          return "D05_velocity_dynamics";
        case MusicalDimensionId::D06_ExpressionControllers:     return "D06_expression_controllers";
        case MusicalDimensionId::D07_SoundFamilySuitability:    return "D07_sound_family_suitability";
        case MusicalDimensionId::D08_GrooveRhythmicFeel:        return "D08_groove_rhythmic_feel";
        case MusicalDimensionId::D09_VariationConsistency:      return "D09_variation_consistency";
        case MusicalDimensionId::D10_FillTransitionQuality:     return "D10_fill_transition_quality";
        case MusicalDimensionId::D11_IntroEndingIntegrity:      return "D11_intro_ending_integrity";
        case MusicalDimensionId::D12_MixBalanceEffects:         return "D12_mix_balance_effects";
    }
    return "unknown";
}

struct MusicalFinding {
    MusicalDimensionId     dimension = MusicalDimensionId::D01_DrumPercBalance;
    MusicalFindingSeverity severity  = MusicalFindingSeverity::Info;
    std::string            code;            // Stable code, e.g. "MQ_D06_ENDING_CC11_SNAPBACK"
    std::string            section_marker;  // e.g. "e1cv1", "v1cv1", or "STYLE"
    uint8_t                korg_ch = 0xFF;  // 8..15 or 0xFF for style-wide
    uint32_t               bar = 0;         // 1-based bar (0 = element-wide)
    uint32_t               tick = 0;        // element-relative tick
    std::string            summary;         // Concise musician-readable diagnosis
    std::string            suggested_rule;  // Optional Mode B rule_id (empty if informational)
};

struct TrackMusicalSummary {
    uint8_t     korg_ch = 0xFF;
    std::string track_name;
    std::string inferred_family;
    uint32_t    note_count = 0;
    uint8_t     min_pitch = 0;
    uint8_t     max_pitch = 0;
    double      mean_velocity = 0.0;
    double      vel_stddev = 0.0;
    uint8_t     vel_p5 = 0;
    uint8_t     vel_p95 = 0;
    double      crest_factor = 0.0;
    double      mean_duty_ratio = 0.0;
    uint32_t    max_polyphony = 0;
    uint32_t    mud_clusters_lt_c3 = 0;     // Family-aware: exempts Bass, Drum/Perc, Timpani
    int         cc7_tick0 = -1;
    int         cc10_tick0 = -1;
    int         cc91_tick0 = -1;
    int         cc93_tick0 = -1;
    bool        flat_velocity = false;
    bool        sound_resolved = false;
};

struct DimensionEvaluation {
    MusicalDimensionId            id = MusicalDimensionId::D01_DrumPercBalance;
    std::string                   name;
    bool                          applicable = true;
    double                        score = 100.0;   // 0.0..100.0 diagnostic score
    std::string                   status = "PASS"; // "PASS" | "ADVISORY" | "WARN" | "N/A"
    std::map<std::string, double> metrics;
    std::vector<MusicalFinding>   findings;
};

struct VariationEnergyPoint {
    std::string marker;         // "v1cv1".."v4cv1"
    uint32_t    bars = 0;
    double      notes_per_bar = 0.0;
    double      mean_velocity = 0.0;
    uint32_t    active_tracks = 0;
    double      composite_energy = 0.0;
};

struct MusicalAnalyzeOptions {
    bool    include_info_findings = true;
    uint8_t bass_floor_midi       = 28;    // E1 (28); 23 for 5-string B0
    uint8_t mud_ceiling_midi      = 48;    // C3 (48)
    uint8_t mud_max_interval_st   = 4;     // <= major 3rd below C3
    double  flat_vel_std_thresh   = 2.0;
    uint8_t abrupt_cc11_jump_min  = 48;
};

struct MusicalQualityReport {
    std::string                         schema = "aiarranger-musical-quality/1";
    std::string                         style_name;
    bool                                hardware_verified = false;
    std::string                         l3_status = "NOT_TESTED";
    bool                                independent_from_frozen_q1 = true;
    std::string                         status_label = "STRUCTURALLY VALID — NOT VERIFIED ON A PA5X";
    uint16_t                            ppqn = 0;
    double                              tempo_bpm = 120.0;
    uint32_t                            element_count = 0;
    double                              overall_musical_score = 0.0;
    std::array<DimensionEvaluation, 12> dimensions{};
    std::vector<TrackMusicalSummary>    tracks;
    std::vector<VariationEnergyPoint>   variation_curve;
    std::vector<MusicalFinding>         all_findings;
};

struct MusicalQualityDelta {
    double                   overall_delta = 0.0;
    std::array<double, 12>   dimension_deltas{};
    std::vector<std::string> resolved_finding_codes;
    std::vector<std::string> newly_introduced_warnings;
    bool                     any_critical_regression = false;
};

namespace musical_quality_detail {

struct ParsedNote {
    uint32_t on_tick = 0;
    uint32_t off_tick = 0;
    uint8_t  ch = 0;
    uint8_t  pitch = 0;
    uint8_t  vel = 0;
};

struct VelStats {
    uint32_t count = 0;
    double   mean = 0.0;
    double   stddev = 0.0;
    uint8_t  p5 = 0;
    uint8_t  p95 = 0;
    uint8_t  max_v = 0;
    double   crest = 0.0;
};

inline double sanitizeFinite(double v, double fallback = 0.0) noexcept {
    return std::isfinite(v) ? v : fallback;
}

inline double clampScore(double s) noexcept {
    if (!std::isfinite(s)) return 0.0;
    if (s < 0.0) return 0.0;
    if (s > 100.0) return 100.0;
    return s;
}

inline double round2(double v) noexcept {
    if (!std::isfinite(v)) return 0.0;
    return std::round(v * 100.0) / 100.0;
}

inline std::string toLowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline bool containsInsensitive(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return false;
    return toLowerAscii(hay).find(toLowerAscii(needle)) != std::string::npos;
}

inline uint32_t barTicksFor(uint16_t ppqn, uint8_t num, uint8_t den) noexcept {
    if (ppqn == 0 || num == 0 || den == 0) return 0;
    const uint64_t numer = static_cast<uint64_t>(ppqn) * 4ull * static_cast<uint64_t>(num);
    if (numer % den != 0) return 0;
    const uint64_t bt = numer / den;
    if (bt == 0 || bt > std::numeric_limits<uint32_t>::max()) return 0;
    return static_cast<uint32_t>(bt);
}

inline uint32_t barNumber1Based(uint32_t tick, uint32_t bar_ticks) noexcept {
    if (bar_ticks == 0) return 1;
    return (tick / bar_ticks) + 1u;
}

inline bool isVariationMarker(std::string_view m) noexcept {
    return m.size() == 5 && m[0] == 'v' && m[1] >= '1' && m[1] <= '4' &&
           m[2] == 'c' && m[3] == 'v' && m[4] >= '1' && m[4] <= '6';
}

inline bool isFillMarker(std::string_view m) noexcept {
    return m.size() == 5 && m[0] == 'f' && m[1] >= '1' && m[1] <= '4' &&
           m[2] == 'c' && m[3] == 'v' && m[4] >= '1' && m[4] <= '2';
}

inline bool isIntroMarker(std::string_view m) noexcept {
    return m.size() == 5 && m[0] == 'i' && m[1] >= '1' && m[1] <= '3' &&
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
    if (m.size() == 5 && m[2] == 'c' && m[3] == 'v') {
        return std::string(m.substr(0, 2));
    }
    return std::string(m);
}

inline bool isKickPitch(uint8_t p) noexcept { return p == 35u || p == 36u; }
inline bool isSnarePitch(uint8_t p) noexcept { return p == 38u || p == 40u; }
inline bool isHatPitch(uint8_t p) noexcept { return p == 42u || p == 44u || p == 46u; }
inline bool isHandPercPitch(uint8_t p) noexcept {
    return p == 54u || p == 56u || (p >= 60u && p <= 87u);
}

inline std::string gmProgramFamilyName(int pc) {
    if (pc < 0 || pc > 127) return "Unknown";
    if (pc <= 7)   return "Piano";
    if (pc <= 15)  return "Chromatic Percussion";
    if (pc <= 23)  return "Organ";
    if (pc <= 31)  return "Guitar";
    if (pc <= 39)  return "Bass";
    if (pc == 43)  return "Contrabass";
    if (pc == 47)  return "Timpani";
    if (pc <= 55)  return "Strings";
    if (pc <= 63)  return "Brass";
    if (pc <= 79)  return "Reed/Pipe";
    if (pc <= 95)  return "Synth Lead/Pad";
    if (pc <= 111) return "Ethnic/FX";
    return "Percussive/SFX";
}

inline std::string timbreBroadClass(int pc, std::string_view family_hint) {
    const std::string fl = toLowerAscii(family_hint);
    if (fl.find("synth") != std::string::npos || fl.find("pad") != std::string::npos ||
        fl.find("edm") != std::string::npos || fl.find("lead") != std::string::npos) {
        return "synth";
    }
    if (fl.find("electric") != std::string::npos || fl.find("e.piano") != std::string::npos ||
        fl.find("rhodes") != std::string::npos || fl.find("organ") != std::string::npos ||
        fl.find("clav") != std::string::npos || fl.find("clean gtr") != std::string::npos ||
        fl.find("dist") != std::string::npos) {
        return "electric";
    }
    if (pc >= 80 && pc <= 103) return "synth";
    if (pc == 4 || pc == 5 || pc == 7 || (pc >= 16 && pc <= 20) ||
        (pc >= 26 && pc <= 31) || (pc >= 33 && pc <= 37) || pc == 38 || pc == 39) {
        return "electric";
    }
    if (pc >= 0 && pc <= 79) return "acoustic";
    return "unknown";
}

inline VelStats computeVelStats(const std::vector<uint8_t>& vels) {
    VelStats s;
    if (vels.empty()) return s;
    s.count = static_cast<uint32_t>(vels.size());
    double sum = 0.0;
    uint8_t mx = 0;
    for (uint8_t v : vels) {
        sum += static_cast<double>(v);
        if (v > mx) mx = v;
    }
    s.mean = sum / static_cast<double>(s.count);
    s.max_v = mx;
    double var = 0.0;
    for (uint8_t v : vels) {
        const double d = static_cast<double>(v) - s.mean;
        var += d * d;
    }
    s.stddev = std::sqrt(var / static_cast<double>(s.count));
    std::vector<uint8_t> sorted = vels;
    std::sort(sorted.begin(), sorted.end());
    const size_t i5  = (sorted.size() - 1u) * 5u / 100u;
    const size_t i95 = (sorted.size() - 1u) * 95u / 100u;
    s.p5 = sorted[i5];
    s.p95 = sorted[i95];
    s.crest = (s.mean > 1e-9) ? (static_cast<double>(mx) / s.mean) : 0.0;
    return s;
}

inline std::vector<ParsedNote> extractNotes(const Element& el) {
    std::vector<ParsedNote> out;
    std::array<std::array<std::deque<std::pair<uint32_t, uint8_t>>, 128>, 16> active{};
    for (const auto& ev : el.events) {
        const uint8_t hi = ev.status & 0xF0u;
        const uint8_t ch = ev.status & 0x0Fu;
        const uint8_t p  = ev.d1 & 0x7Fu;
        if (hi == 0x90u && ev.d2 > 0u) {
            active[ch][p].emplace_back(ev.tick, static_cast<uint8_t>(ev.d2 & 0x7Fu));
        } else if (hi == 0x80u || (hi == 0x90u && ev.d2 == 0u)) {
            auto& q = active[ch][p];
            if (!q.empty()) {
                const auto [on_t, vel] = q.front();
                q.pop_front();
                const uint32_t off_t = (ev.tick > on_t) ? ev.tick : (on_t + 1u);
                out.push_back(ParsedNote{on_t, off_t, ch, p, vel});
            }
        }
    }
    const uint32_t fallback_end = (el.length > 0u) ? el.length : 1u;
    for (uint8_t ch = 0; ch < 16u; ++ch) {
        for (uint8_t p = 0; p < 128u; ++p) {
            for (const auto& [on_t, vel] : active[ch][p]) {
                const uint32_t off_t = (fallback_end > on_t) ? fallback_end : (on_t + 1u);
                out.push_back(ParsedNote{on_t, off_t, ch, p, vel});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const ParsedNote& a, const ParsedNote& b) {
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        if (a.ch != b.ch) return a.ch < b.ch;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.off_tick < b.off_tick;
    });
    return out;
}

inline uint32_t computeMaxPolyphony(const std::vector<ParsedNote>& notes) {
    if (notes.empty()) return 0;
    std::vector<std::pair<uint32_t, int>> pts;
    pts.reserve(notes.size() * 2u);
    for (const auto& n : notes) {
        pts.emplace_back(n.on_tick, +1);
        pts.emplace_back(n.off_tick, -1);
    }
    std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;  // -1 (off) before +1 (on) at identical tick
    });
    int cur = 0;
    int best = 0;
    for (const auto& pt : pts) {
        cur += pt.second;
        if (cur > best) best = cur;
    }
    return best > 0 ? static_cast<uint32_t>(best) : 0u;
}

inline double computeMeanDutyRatio(const std::vector<ParsedNote>& notes, uint16_t ppqn) {
    if (notes.empty()) return 0.0;
    std::vector<ParsedNote> seq = notes;
    std::sort(seq.begin(), seq.end(), [](const ParsedNote& a, const ParsedNote& b) {
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        return a.off_tick > b.off_tick;
    });
    double sum = 0.0;
    uint32_t cnt = 0;
    const double beat = (ppqn > 0) ? static_cast<double>(ppqn) : 480.0;
    for (size_t i = 0; i < seq.size(); ++i) {
        const double dur = (seq[i].off_tick > seq[i].on_tick)
                               ? static_cast<double>(seq[i].off_tick - seq[i].on_tick)
                               : 0.0;
        // Find next strictly later onset for inter-onset interval.
        double ioi = beat;
        for (size_t j = i + 1; j < seq.size(); ++j) {
            if (seq[j].on_tick > seq[i].on_tick) {
                ioi = static_cast<double>(seq[j].on_tick - seq[i].on_tick);
                break;
            }
        }
        if (ioi > 0.0) {
            const double r = std::min(1.0, dur / ioi);
            sum += r;
            ++cnt;
        }
    }
    return cnt > 0 ? (sum / static_cast<double>(cnt)) : 0.0;
}

inline bool isExemptFromPianoMud(uint8_t ch, int pc, std::string_view family, std::string_view name) {
    if (ch < kAcc1Ch || ch > 15u) return true;  // Bass, Drum, Percussion exempt
    if (pc == 47 || pc == 43 || pc == 58) return true;  // Timpani, Contrabass, Tuba
    if (containsInsensitive(family, "timpani") || containsInsensitive(name, "timpani")) return true;
    if (containsInsensitive(family, "contrabass") || containsInsensitive(name, "contrabass") ||
        containsInsensitive(name, "contrabs")) return true;
    if (containsInsensitive(family, "bass") || containsInsensitive(family, "drum") ||
        containsInsensitive(family, "perc") || containsInsensitive(family, "tuba")) return true;
    return false;
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

inline std::string formatDouble(double v, int prec = 2) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(prec) << sanitizeFinite(v, 0.0);
    return oss.str();
}

inline std::string statusFromScoreAndFindings(bool applicable, double score,
                                              const std::vector<MusicalFinding>& findings) {
    if (!applicable) return "N/A";
    bool has_crit_or_warn = false;
    bool has_adv = false;
    for (const auto& f : findings) {
        if (f.severity == MusicalFindingSeverity::Critical ||
            f.severity == MusicalFindingSeverity::Warning) {
            has_crit_or_warn = true;
        } else if (f.severity == MusicalFindingSeverity::Advisory) {
            has_adv = true;
        }
    }
    if (has_crit_or_warn || score < 80.0) return "WARN";
    if (has_adv || score < 95.0) return "ADVISORY";
    return "PASS";
}

} // namespace musical_quality_detail

inline MusicalQualityReport analyzeMusicalQuality(
    const Style& korg_style,
    const Report* report = nullptr,
    const uasf::StyleDefinition* source_uasf = nullptr,
    const MusicalAnalyzeOptions& opt = {}) noexcept {
    MusicalQualityReport out;
    try {
        using namespace musical_quality_detail;

        out.schema = "aiarranger-musical-quality/1";
        out.hardware_verified = false;
        out.l3_status = "NOT_TESTED";
        out.independent_from_frozen_q1 = true;
        out.ppqn = korg_style.ppqn;
        out.element_count = static_cast<uint32_t>(korg_style.elements.size());
        if (korg_style.mpqn > 0u) {
            out.tempo_bpm = round2(60'000'000.0 / static_cast<double>(korg_style.mpqn));
        } else if (source_uasf != nullptr && source_uasf->tempo_bpm > 0u) {
            out.tempo_bpm = static_cast<double>(source_uasf->tempo_bpm);
        } else {
            out.tempo_bpm = 120.0;
        }
        if (source_uasf != nullptr && !source_uasf->name.empty()) {
            out.style_name = source_uasf->name;
        } else {
            out.style_name = "unnamed-style";
        }

        const auto val_errs = validate(korg_style);
        out.status_label = val_errs.empty()
            ? "STRUCTURALLY VALID — NOT VERIFIED ON A PA5X"
            : "STRUCTURAL VALIDATION ERRORS — NOT VERIFIED ON A PA5X";

        for (size_t i = 0; i < 12u; ++i) {
            const auto id = static_cast<MusicalDimensionId>(i + 1u);
            out.dimensions[i].id = id;
            out.dimensions[i].name = musicalDimensionName(id);
            out.dimensions[i].applicable = true;
            out.dimensions[i].score = 100.0;
            out.dimensions[i].status = "PASS";
        }

        auto addFinding = [&](MusicalDimensionId dim, MusicalFindingSeverity sev,
                              std::string code, std::string marker, uint8_t ch,
                              uint32_t bar, uint32_t tick, std::string summary,
                              std::string rule = "") {
            if (sev == MusicalFindingSeverity::Info && !opt.include_info_findings) return;
            MusicalFinding f;
            f.dimension = dim;
            f.severity = sev;
            f.code = std::move(code);
            f.section_marker = std::move(marker);
            f.korg_ch = ch;
            f.bar = bar;
            f.tick = tick;
            f.summary = std::move(summary);
            f.suggested_rule = std::move(rule);
            out.dimensions[static_cast<size_t>(dim) - 1u].findings.push_back(f);
        };

        // ── Gather report track metadata per Korg channel 8..15 ─────────
        std::array<std::string, 16> report_family{};
        std::array<std::string, 16> report_name{};
        std::array<std::string, 16> report_target{};
        std::array<bool, 16>        report_guitar_hint{};
        std::array<bool, 16>        report_melody_ntt{};
        std::array<bool, 16>        report_megavoice{};
        std::array<bool, 16>        report_resolved{};
        bool any_megavoice_sanitized = false;

        if (report != nullptr) {
            any_megavoice_sanitized = report->megavoice_tracks_sanitized > 0u;
            for (const auto& tr : report->tracks) {
                if (tr.korg_ch < 8u || tr.korg_ch > 15u) continue;
                const uint8_t c = tr.korg_ch;
                if (!tr.family.empty()) report_family[c] = tr.family;
                if (!tr.name.empty()) report_name[c] = tr.name;
                if (!tr.target.empty()) {
                    report_target[c] = tr.target;
                    if (tr.target != "UNRESOLVED" && tr.target.find("UNRESOLVED") == std::string::npos) {
                        report_resolved[c] = true;
                    }
                }
                if (tr.ntt == 1u) report_melody_ntt[c] = true;
                if (tr.megavoice) report_megavoice[c] = true;
                if (containsInsensitive(tr.family, "guitar") ||
                    containsInsensitive(tr.target, "guitar") ||
                    containsInsensitive(tr.target, "gtr") ||
                    containsInsensitive(tr.name, "gtr") ||
                    containsInsensitive(tr.name, "guitar")) {
                    report_guitar_hint[c] = true;
                }
            }
            for (const auto& adv : report->track_edit_advice) {
                if (adv.korg_ch >= 8u && adv.korg_ch <= 15u) {
                    if (containsInsensitive(adv.pa5x_ntt_type, "guitar") ||
                        containsInsensitive(adv.pa5x_ntt_table, "guitar")) {
                        report_guitar_hint[adv.korg_ch] = true;
                    }
                }
            }
        }

        // ── First pass: parse elements, per-channel notes, tick-0 state ──
        struct ElemParsed {
            const Element* el = nullptr;
            uint32_t bar_ticks = 0;
            uint32_t bars = 0;
            std::vector<ParsedNote> notes;
            std::array<std::vector<ParsedNote>, 16> by_ch;
            std::array<int, 16> msb0{};
            std::array<int, 16> lsb0{};
            std::array<int, 16> pc0{};
            std::array<int, 16> cc7_0{};
            std::array<int, 16> cc10_0{};
            std::array<int, 16> cc91_0{};
            std::array<int, 16> cc93_0{};
        };

        std::vector<ElemParsed> parsed;
        parsed.reserve(korg_style.elements.size());
        std::map<std::string, uint8_t> section_max_cv;
        uint8_t global_cv_max = 0;

        for (const auto& el : korg_style.elements) {
            ElemParsed ep;
            ep.el = &el;
            ep.msb0.fill(-1);
            ep.lsb0.fill(-1);
            ep.pc0.fill(-1);
            ep.cc7_0.fill(-1);
            ep.cc10_0.fill(-1);
            ep.cc91_0.fill(-1);
            ep.cc93_0.fill(-1);
            ep.bar_ticks = barTicksFor(korg_style.ppqn, el.num, el.den);
            if (ep.bar_ticks > 0u && el.length > 0u) {
                ep.bars = el.length / ep.bar_ticks;
                if (ep.bars == 0u) ep.bars = 1u;
            }
            ep.notes = extractNotes(el);
            for (const auto& n : ep.notes) {
                if (n.ch < 16u) ep.by_ch[n.ch].push_back(n);
            }
            for (const auto& ev : el.events) {
                if (ev.tick > 0u) break;
                const uint8_t hi = ev.status & 0xF0u;
                const uint8_t ch = ev.status & 0x0Fu;
                if (hi == 0xB0u) {
                    if (ev.d1 == 0u)  ep.msb0[ch] = ev.d2;
                    if (ev.d1 == 32u) ep.lsb0[ch] = ev.d2;
                    if (ev.d1 == 7u)  ep.cc7_0[ch] = ev.d2;
                    if (ev.d1 == 10u) ep.cc10_0[ch] = ev.d2;
                    if (ev.d1 == 91u) ep.cc91_0[ch] = ev.d2;
                    if (ev.d1 == 93u) ep.cc93_0[ch] = ev.d2;
                } else if (hi == 0xC0u) {
                    ep.pc0[ch] = ev.d1;
                }
            }
            const uint8_t cv = cvNumberOf(el.marker);
            if (cv > global_cv_max) global_cv_max = cv;
            const std::string sb = sectionBaseOf(el.marker);
            if (cv > section_max_cv[sb]) section_max_cv[sb] = cv;
            parsed.push_back(std::move(ep));
        }

        // ── Aggregate per-channel track summaries (ch 8..15) ────────────
        std::array<std::vector<uint8_t>, 16> ch_vels{};
        std::array<uint8_t, 16> ch_min_p{};
        ch_min_p.fill(127u);
        std::array<uint8_t, 16> ch_max_p{};
        std::array<uint32_t, 16> ch_max_poly{};
        std::array<double, 16> ch_duty_sum{};
        std::array<uint32_t, 16> ch_duty_elems{};
        std::array<int, 16> first_pc{};
        first_pc.fill(-1);
        std::array<int, 16> first_msb{};
        first_msb.fill(-1);
        std::array<int, 16> first_cc7{};
        first_cc7.fill(-1);
        std::array<int, 16> first_cc10{};
        first_cc10.fill(-1);
        std::array<int, 16> first_cc91{};
        first_cc91.fill(-1);
        std::array<int, 16> first_cc93{};
        first_cc93.fill(-1);
        std::array<uint32_t, 16> ch_mud_clusters{};
        std::array<bool, 16> is_guitar_ch{};

        for (const auto& ep : parsed) {
            for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                if (first_pc[ch] < 0 && ep.pc0[ch] >= 0) first_pc[ch] = ep.pc0[ch];
                if (first_msb[ch] < 0 && ep.msb0[ch] >= 0) first_msb[ch] = ep.msb0[ch];
                if (first_cc7[ch] < 0 && ep.cc7_0[ch] >= 0) first_cc7[ch] = ep.cc7_0[ch];
                if (first_cc10[ch] < 0 && ep.cc10_0[ch] >= 0) first_cc10[ch] = ep.cc10_0[ch];
                if (first_cc91[ch] < 0 && ep.cc91_0[ch] >= 0) first_cc91[ch] = ep.cc91_0[ch];
                if (first_cc93[ch] < 0 && ep.cc93_0[ch] >= 0) first_cc93[ch] = ep.cc93_0[ch];
                if (ch >= kAcc1Ch && ep.pc0[ch] >= 24 && ep.pc0[ch] <= 31) {
                    is_guitar_ch[ch] = true;
                }
                const auto& cn = ep.by_ch[ch];
                if (cn.empty()) continue;
                for (const auto& n : cn) {
                    ch_vels[ch].push_back(n.vel);
                    if (n.pitch < ch_min_p[ch]) ch_min_p[ch] = n.pitch;
                    if (n.pitch > ch_max_p[ch]) ch_max_p[ch] = n.pitch;
                }
                const uint32_t mp = computeMaxPolyphony(cn);
                if (mp > ch_max_poly[ch]) ch_max_poly[ch] = mp;
                ch_duty_sum[ch] += computeMeanDutyRatio(cn, korg_style.ppqn);
                ++ch_duty_elems[ch];
            }
        }
        for (uint8_t ch = kAcc1Ch; ch <= 15u; ++ch) {
            if (report_guitar_hint[ch]) is_guitar_ch[ch] = true;
        }

        auto inferredFamilyFor = [&](uint8_t ch) -> std::string {
            if (!report_family[ch].empty()) return report_family[ch];
            if (ch == kDrumCh) return "Drum Kit";
            if (ch == kPercCh) return "Percussion";
            if (ch == kBassCh) return "Bass";
            if (first_pc[ch] >= 0) return gmProgramFamilyName(first_pc[ch]);
            return "Unknown";
        };

        // =====================================================================
        // D01 — Drum & Percussion Balance
        // =====================================================================
        {
            auto& d = out.dimensions[0];
            std::vector<uint8_t> kick_v, snare_v, hat_v;
            uint32_t drum_hits = 0, perc_hits = 0;
            uint32_t ghost_snares = 0;
            uint32_t hand_on_drum = 0, hand_on_perc = 0;
            uint32_t hh_choke_overlaps = 0;

            for (const auto& ep : parsed) {
                const auto& dn = ep.by_ch[kDrumCh];
                const auto& pn = ep.by_ch[kPercCh];
                drum_hits += static_cast<uint32_t>(dn.size());
                perc_hits += static_cast<uint32_t>(pn.size());

                std::vector<ParsedNote> open_hats;
                std::vector<uint32_t> closed_hat_ticks;
                for (const auto& n : dn) {
                    if (isKickPitch(n.pitch)) kick_v.push_back(n.vel);
                    if (isSnarePitch(n.pitch)) {
                        snare_v.push_back(n.vel);
                        if (n.vel < 55u) ++ghost_snares;
                    }
                    if (isHatPitch(n.pitch)) {
                        hat_v.push_back(n.vel);
                        if (n.pitch == 46u) open_hats.push_back(n);
                        else closed_hat_ticks.push_back(n.on_tick);
                    }
                    if (isHandPercPitch(n.pitch)) ++hand_on_drum;
                }
                for (const auto& n : pn) {
                    if (isHandPercPitch(n.pitch)) ++hand_on_perc;
                }
                for (const auto& oh : open_hats) {
                    for (uint32_t ct : closed_hat_ticks) {
                        if (ct > oh.on_tick && ct < oh.off_tick) {
                            ++hh_choke_overlaps;
                            addFinding(MusicalDimensionId::D01_DrumPercBalance,
                                       MusicalFindingSeverity::Warning,
                                       "MQ_D01_HH_CHOKE_OVERLAP", ep.el->marker, kDrumCh,
                                       barNumber1Based(ct, ep.bar_ticks), ct,
                                       "Open hi-hat (46) overlaps closed/pedal hi-hat choke tick");
                            break;
                        }
                    }
                }
            }

            d.applicable = (drum_hits > 0u || perc_hits > 0u);
            const auto ks = computeVelStats(kick_v);
            const auto ss = computeVelStats(snare_v);
            const auto hs = computeVelStats(hat_v);
            const uint32_t hand_total = hand_on_drum + hand_on_perc;
            const double hand_iso = (hand_total > 0u)
                ? (static_cast<double>(hand_on_perc) / static_cast<double>(hand_total))
                : 1.0;
            const bool flat_snare = (ss.count >= 8u && ss.stddev < 3.0 && ghost_snares == 0u);

            d.metrics["drum_hits"] = static_cast<double>(drum_hits);
            d.metrics["perc_hits"] = static_cast<double>(perc_hits);
            d.metrics["kick_vel_mean"] = round2(ks.mean);
            d.metrics["kick_vel_stddev"] = round2(ks.stddev);
            d.metrics["snare_vel_mean"] = round2(ss.mean);
            d.metrics["snare_vel_stddev"] = round2(ss.stddev);
            d.metrics["hat_vel_mean"] = round2(hs.mean);
            d.metrics["hat_vel_stddev"] = round2(hs.stddev);
            d.metrics["ghost_snare_count"] = static_cast<double>(ghost_snares);
            d.metrics["hh_choke_overlaps"] = static_cast<double>(hh_choke_overlaps);
            d.metrics["hand_perc_isolation_ratio"] = round2(hand_iso);
            d.metrics["flat_snare_flag"] = flat_snare ? 1.0 : 0.0;

            double sc = 100.0;
            if (hh_choke_overlaps > 0u) {
                sc -= std::min(45.0, static_cast<double>(hh_choke_overlaps) * 15.0);
            }
            if (flat_snare) {
                sc -= 10.0;
                addFinding(MusicalDimensionId::D01_DrumPercBalance,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D01_FLAT_SNARE_DYNAMICS", "STYLE", kDrumCh, 0, 0,
                           "Flat snare velocity profile (std=" + formatDouble(ss.stddev) +
                           ", 0 ghost notes); Mode B drum_ghost_dynamics can add subtle feel",
                           "drum_ghost_dynamics");
            }
            if (hand_total >= 16u && hand_iso < 0.5 && perc_hits == 0u) {
                sc -= 6.0;
                addFinding(MusicalDimensionId::D01_DrumPercBalance,
                           MusicalFindingSeverity::Info,
                           "MQ_D01_HAND_PERC_ON_MAIN_KIT", "STYLE", kDrumCh, 0, 0,
                           "Hand percussion hits share Drum Ch10 with empty Percussion Ch11");
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D02 — Bass Register, Duration & Articulation
        // =====================================================================
        {
            auto& d = out.dimensions[1];
            uint32_t bass_notes = 0;
            uint8_t min_p = 127u, max_p = 0u;
            uint32_t below_floor = 0, below_b0 = 0;
            uint32_t non_root_fifth = 0;
            uint32_t pb_events = 0;
            uint32_t bass_acc_collisions = 0;

            for (const auto& ep : parsed) {
                for (const auto& ev : ep.el->events) {
                    if ((ev.status & 0xF0u) == 0xE0u && (ev.status & 0x0Fu) == kBassCh) {
                        ++pb_events;
                    }
                }
                const auto& bn = ep.by_ch[kBassCh];
                for (const auto& n : bn) {
                    ++bass_notes;
                    if (n.pitch < min_p) min_p = n.pitch;
                    if (n.pitch > max_p) max_p = n.pitch;
                    if (n.pitch < opt.bass_floor_midi) ++below_floor;
                    if (n.pitch < 23u) ++below_b0;
                    const uint8_t pc = static_cast<uint8_t>(n.pitch % 12u);
                    if (pc != 0u && pc != 7u) ++non_root_fifth;

                    // Check low-register collision against non-Timpani accompaniment tracks.
                    if (n.pitch < 36u) {
                        for (uint8_t ach = kAcc1Ch; ach <= 15u; ++ach) {
                            if (isExemptFromPianoMud(ach, first_pc[ach], inferredFamilyFor(ach), report_name[ach])) {
                                continue;
                            }
                            for (const auto& an : ep.by_ch[ach]) {
                                if (an.pitch >= opt.mud_ceiling_midi) continue;
                                if (an.on_tick < n.off_tick && n.on_tick < an.off_tick) {
                                    const int diff = std::abs(static_cast<int>(an.pitch) - static_cast<int>(n.pitch));
                                    if (diff <= 2) {
                                        ++bass_acc_collisions;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            d.applicable = (bass_notes > 0u);
            const double duty = (ch_duty_elems[kBassCh] > 0u)
                ? (ch_duty_sum[kBassCh] / static_cast<double>(ch_duty_elems[kBassCh]))
                : 0.0;
            const double nrf_pct = (bass_notes > 0u)
                ? (100.0 * static_cast<double>(non_root_fifth) / static_cast<double>(bass_notes))
                : 0.0;
            const bool single_cv1_walking = (nrf_pct >= 15.0 && global_cv_max <= 1u && bass_notes >= 8u);

            d.metrics["bass_note_count"] = static_cast<double>(bass_notes);
            d.metrics["min_pitch"] = d.applicable ? static_cast<double>(min_p) : 0.0;
            d.metrics["max_pitch"] = d.applicable ? static_cast<double>(max_p) : 0.0;
            d.metrics["notes_below_e1"] = static_cast<double>(below_floor);
            d.metrics["notes_below_b0"] = static_cast<double>(below_b0);
            d.metrics["mean_duty_ratio"] = round2(duty);
            d.metrics["non_root_fifth_pct"] = round2(nrf_pct);
            d.metrics["pitchbend_events"] = static_cast<double>(pb_events);
            d.metrics["bass_acc_collision_count"] = static_cast<double>(bass_acc_collisions);
            d.metrics["single_cv1_walking_risk"] = single_cv1_walking ? 1.0 : 0.0;

            double sc = 100.0;
            if (below_b0 > 0u) {
                sc -= 25.0;
                addFinding(MusicalDimensionId::D02_BassRegisterArticulation,
                           MusicalFindingSeverity::Critical,
                           "MQ_D02_BASS_BELOW_B0", "STYLE", kBassCh, 0, 0,
                           std::to_string(below_b0) + " bass note(s) fall below B0 (MIDI 23)",
                           "bass_register_guard");
            }
            if (below_floor > 0u) {
                sc -= 15.0;
                addFinding(MusicalDimensionId::D02_BassRegisterArticulation,
                           MusicalFindingSeverity::Warning,
                           "MQ_D02_BASS_BELOW_E1", "STYLE", kBassCh, 0, 0,
                           std::to_string(below_floor) + " bass note(s) below register floor MIDI " +
                           std::to_string(opt.bass_floor_midi),
                           "bass_register_guard");
            }
            if (single_cv1_walking) {
                sc -= 12.0;
                addFinding(MusicalDimensionId::D02_BassRegisterArticulation,
                           MusicalFindingSeverity::Warning,
                           "MQ_D02_SINGLE_CV1_WALKING_BASS", "STYLE", kBassCh, 0, 0,
                           "Walking bass (" + formatDouble(nrf_pct) +
                           "% non-root/5th degrees) in CV1-only export risks minor/dim clashes under Parallel/Root",
                           "jazz_cv_melody_split");
            }
            if (bass_acc_collisions > 0u) {
                sc -= 8.0;
                addFinding(MusicalDimensionId::D02_BassRegisterArticulation,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D02_BASS_ACC_LOW_COLLISION", "STYLE", kBassCh, 0, 0,
                           std::to_string(bass_acc_collisions) +
                           " simultaneous low-register bass/accompaniment unison collision(s)",
                           "piano_mud_voicing");
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D03 — Guitar Strumming & MegaVoice Articulation Integrity
        // =====================================================================
        {
            auto& d = out.dimensions[2];
            uint32_t gtr_notes = 0;
            uint32_t max_poly = 0;
            uint32_t max_span = 0;
            double stagger_sum_ms = 0.0;
            uint32_t stagger_cnt = 0;
            std::vector<uint8_t> muted_v, open_v, all_gtr_v;
            uint32_t ks_leaks = 0;
            uint32_t fret_noise_leaks = 0;

            const double ms_per_tick = (korg_style.ppqn > 0 && out.tempo_bpm > 0.0)
                ? (60000.0 / (out.tempo_bpm * static_cast<double>(korg_style.ppqn)))
                : 0.0;

            for (const auto& ep : parsed) {
                for (uint8_t ch = kAcc1Ch; ch <= 15u; ++ch) {
                    if (!is_guitar_ch[ch]) continue;
                    const auto& gn = ep.by_ch[ch];
                    if (gn.empty()) continue;
                    gtr_notes += static_cast<uint32_t>(gn.size());
                    const uint32_t mp = computeMaxPolyphony(gn);
                    if (mp > max_poly) max_poly = mp;

                    for (const auto& n : gn) {
                        all_gtr_v.push_back(n.vel);
                        if (n.vel <= 56u) muted_v.push_back(n.vel);
                        if (n.vel >= 68u) open_v.push_back(n.vel);
                        if (n.pitch < 28u || n.pitch >= 96u) ++ks_leaks;
                        if (n.vel <= 20u) ++fret_noise_leaks;
                    }

                    // Fretboard span across overlapping notes & strum stagger within 60ms.
                    for (size_t i = 0; i < gn.size(); ++i) {
                        uint8_t lo = gn[i].pitch;
                        uint8_t hi = gn[i].pitch;
                        for (size_t j = i + 1; j < gn.size(); ++j) {
                            if (gn[j].on_tick >= gn[i].off_tick) break;
                            if (gn[j].pitch < lo) lo = gn[j].pitch;
                            if (gn[j].pitch > hi) hi = gn[j].pitch;
                        }
                        const uint32_t sp = static_cast<uint32_t>(hi - lo);
                        if (sp > max_span) max_span = sp;

                        if (i > 0u && ms_per_tick > 0.0) {
                            const uint32_t dt = gn[i].on_tick - gn[i - 1u].on_tick;
                            if (dt > 0u) {
                                const double dt_ms = static_cast<double>(dt) * ms_per_tick;
                                if (dt_ms >= 1.0 && dt_ms <= 60.0) {
                                    stagger_sum_ms += dt_ms;
                                    ++stagger_cnt;
                                }
                            }
                        }
                    }
                }
            }

            d.applicable = (gtr_notes > 0u);
            const auto all_gs = computeVelStats(all_gtr_v);
            const auto ms = computeVelStats(muted_v);
            const auto os = computeVelStats(open_v);
            const double vel_gap = (ms.count > 0u && os.count > 0u) ? (os.mean - ms.mean) : 0.0;
            const double mean_stagger = (stagger_cnt > 0u)
                ? (stagger_sum_ms / static_cast<double>(stagger_cnt))
                : 0.0;
            const bool flat_gtr = (all_gs.count >= 8u && all_gs.stddev < opt.flat_vel_std_thresh);

            d.metrics["guitar_note_count"] = static_cast<double>(gtr_notes);
            d.metrics["max_polyphony"] = static_cast<double>(max_poly);
            d.metrics["max_fretboard_span_st"] = static_cast<double>(max_span);
            d.metrics["mean_strum_stagger_ms"] = round2(mean_stagger);
            d.metrics["muted_note_count"] = static_cast<double>(muted_v.size());
            d.metrics["open_note_count"] = static_cast<double>(open_v.size());
            d.metrics["vel_separation_gap"] = round2(vel_gap);
            d.metrics["keyswitch_leak_count"] = static_cast<double>(ks_leaks);
            d.metrics["fret_noise_vel_leak_count"] = static_cast<double>(fret_noise_leaks);
            d.metrics["flat_strum_vel_flag"] = flat_gtr ? 1.0 : 0.0;

            double sc = 100.0;
            if (max_poly > 6u) {
                sc -= 20.0;
                addFinding(MusicalDimensionId::D03_GuitarStrumMegaVoice,
                           MusicalFindingSeverity::Warning,
                           "MQ_D03_GUITAR_POLY_EXCEEDS_6", "STYLE", 0xFF, 0, 0,
                           "Guitar track exceeds physical 6-string polyphony (max=" +
                           std::to_string(max_poly) + ")");
            }
            if (max_span > 24u) {
                sc -= 15.0;
                addFinding(MusicalDimensionId::D03_GuitarStrumMegaVoice,
                           MusicalFindingSeverity::Warning,
                           "MQ_D03_GUITAR_FRET_SPAN_GT_24", "STYLE", 0xFF, 0, 0,
                           "Guitar voicing fretboard span exceeds 24 semitones (" +
                           std::to_string(max_span) + " st)");
            }
            if (ks_leaks > 0u) {
                sc -= 15.0;
                addFinding(MusicalDimensionId::D03_GuitarStrumMegaVoice,
                           MusicalFindingSeverity::Warning,
                           "MQ_D03_MEGAVOICE_KEYSWITCH_LEAK", "STYLE", 0xFF, 0, 0,
                           std::to_string(ks_leaks) + " possible MegaVoice keyswitch/high-FX note(s) on guitar track");
            }
            if (fret_noise_leaks > 0u) {
                sc -= 10.0;
                addFinding(MusicalDimensionId::D03_GuitarStrumMegaVoice,
                           MusicalFindingSeverity::Warning,
                           "MQ_D03_MEGAVOICE_FRET_NOISE_LEAK", "STYLE", 0xFF, 0, 0,
                           std::to_string(fret_noise_leaks) + " low-velocity (<=20) fret-noise trigger(s) on guitar track");
            }
            if (flat_gtr) {
                sc -= 5.0;
                addFinding(MusicalDimensionId::D03_GuitarStrumMegaVoice,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D03_FLAT_GUITAR_STRUM_VEL", "STYLE", 0xFF, 0, 0,
                           "Guitar strum velocity is flat (std=" + formatDouble(all_gs.stddev) + ")");
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D04 — Piano & Accompaniment Voicing (Family-Aware!)
        // =====================================================================
        {
            auto& d = out.dimensions[3];
            uint32_t acc_notes = 0;
            uint32_t total_mud = 0;
            uint32_t single_cv1_maj7_notes = 0;
            double leap_sum = 0.0;
            uint32_t leap_cnt = 0;
            const uint32_t onset_win = (korg_style.ppqn >= 16u) ? (korg_style.ppqn / 16u) : 1u;

            for (const auto& ep : parsed) {
                const std::string sb = sectionBaseOf(ep.el->marker);
                const bool is_cv1_only = (sb != "br" && section_max_cv[sb] <= 1u);

                for (uint8_t ch = kAcc1Ch; ch <= 15u; ++ch) {
                    const auto& cn = ep.by_ch[ch];
                    if (cn.empty()) continue;
                    acc_notes += static_cast<uint32_t>(cn.size());

                    const int eff_pc = (ep.pc0[ch] >= 0) ? ep.pc0[ch] : first_pc[ch];
                    const std::string fam = inferredFamilyFor(ch);
                    const bool exempt = isExemptFromPianoMud(ch, eff_pc, fam, report_name[ch]);

                    // Mud-cluster detection (< mud_ceiling_midi, interval <= mud_max_interval_st)
                    if (!exempt) {
                        size_t i = 0;
                        while (i < cn.size()) {
                            const uint32_t t0 = cn[i].on_tick;
                            size_t j = i;
                            std::vector<uint8_t> low_pitches;
                            while (j < cn.size() && (cn[j].on_tick - t0) <= onset_win) {
                                if (cn[j].pitch < opt.mud_ceiling_midi) {
                                    low_pitches.push_back(cn[j].pitch);
                                }
                                ++j;
                            }
                            if (low_pitches.size() >= 2u) {
                                std::sort(low_pitches.begin(), low_pitches.end());
                                low_pitches.erase(std::unique(low_pitches.begin(), low_pitches.end()),
                                                  low_pitches.end());
                                if (low_pitches.size() >= 2u) {
                                    uint8_t min_gap = 127u;
                                    for (size_t k = 1; k < low_pitches.size(); ++k) {
                                        const uint8_t gap = static_cast<uint8_t>(low_pitches[k] - low_pitches[k - 1u]);
                                        if (gap < min_gap) min_gap = gap;
                                    }
                                    if (min_gap <= opt.mud_max_interval_st) {
                                        ++total_mud;
                                        ++ch_mud_clusters[ch];
                                        if (total_mud <= 4u) {
                                            addFinding(MusicalDimensionId::D04_PianoAccompanimentVoicing,
                                                       MusicalFindingSeverity::Warning,
                                                       "MQ_D04_LOW_REGISTER_MUD_CLUSTER", ep.el->marker, ch,
                                                       barNumber1Based(t0, ep.bar_ticks), t0,
                                                       "Dense low-register (<C3) voicing cluster (" +
                                                       std::to_string(low_pitches.size()) + " voices, min gap " +
                                                       std::to_string(min_gap) + " st)",
                                                       "piano_mud_voicing");
                                        }
                                    }
                                }
                            }
                            i = j;
                        }
                    }

                    // Single-CV1 Maj7 melody clash risk (pc==11 in CV1-only section)
                    if (is_cv1_only && !exempt) {
                        uint32_t maj7_here = 0;
                        for (const auto& n : cn) {
                            if ((n.pitch % 12u) == 11u) ++maj7_here;
                        }
                        if (maj7_here > 0u && (report_melody_ntt[ch] || report == nullptr || maj7_here >= 2u)) {
                            single_cv1_maj7_notes += maj7_here;
                        }
                    }

                    // Bar-to-bar voicing centroid leap
                    if (ep.bar_ticks > 0u) {
                        std::map<uint32_t, std::pair<double, uint32_t>> bar_cent;
                        for (const auto& n : cn) {
                            const uint32_t b = n.on_tick / ep.bar_ticks;
                            bar_cent[b].first += static_cast<double>(n.pitch);
                            bar_cent[b].second += 1u;
                        }
                        double prev_c = -1.0;
                        for (const auto& [b, kv] : bar_cent) {
                            (void)b;
                            const double c = kv.first / static_cast<double>(kv.second);
                            if (prev_c >= 0.0) {
                                leap_sum += std::abs(c - prev_c);
                                ++leap_cnt;
                            }
                            prev_c = c;
                        }
                    }
                }
            }

            d.applicable = (acc_notes > 0u);
            const double mean_leap = (leap_cnt > 0u) ? (leap_sum / static_cast<double>(leap_cnt)) : 0.0;
            d.metrics["acc_note_count"] = static_cast<double>(acc_notes);
            d.metrics["mud_clusters_lt_c3"] = static_cast<double>(total_mud);
            d.metrics["cv_depth_max"] = static_cast<double>(global_cv_max);
            d.metrics["single_cv1_maj7_melody_notes"] = static_cast<double>(single_cv1_maj7_notes);
            d.metrics["mean_voicing_centroid_jump_st"] = round2(mean_leap);

            double sc = 100.0;
            if (total_mud > 0u) {
                sc -= std::min(45.0, static_cast<double>(total_mud) * 15.0);
            }
            if (single_cv1_maj7_notes > 0u) {
                sc -= 12.0;
                addFinding(MusicalDimensionId::D04_PianoAccompanimentVoicing,
                           MusicalFindingSeverity::Warning,
                           "MQ_D04_SINGLE_CV1_MAJ7_CLASH_RISK", "STYLE", 0xFF, 0, 0,
                           std::to_string(single_cv1_maj7_notes) +
                           " Maj7 degree (B) note(s) in CV1-only section(s) risk clashes on Dom7/Min chords",
                           "jazz_cv_melody_split");
            }
            if (mean_leap > 14.0) {
                sc -= 8.0;
                addFinding(MusicalDimensionId::D04_PianoAccompanimentVoicing,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D04_LARGE_VOICING_LEAP", "STYLE", 0xFF, 0, 0,
                           "Large bar-to-bar accompaniment voicing centroid jump (" +
                           formatDouble(mean_leap) + " st)");
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D05 — Velocity & Dynamics Preservation
        // =====================================================================
        {
            auto& d = out.dimensions[4];
            std::vector<uint8_t> all_vels;
            uint32_t flat_tracks = 0;
            uint32_t active_tracks = 0;
            for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                if (ch_vels[ch].empty()) continue;
                ++active_tracks;
                all_vels.insert(all_vels.end(), ch_vels[ch].begin(), ch_vels[ch].end());
                const auto vs = computeVelStats(ch_vels[ch]);
                if (vs.count >= 8u && vs.stddev < opt.flat_vel_std_thresh) {
                    ++flat_tracks;
                }
            }
            d.applicable = !all_vels.empty();
            const auto gvs = computeVelStats(all_vels);
            d.metrics["mean_velocity"] = round2(gvs.mean);
            d.metrics["vel_stddev"] = round2(gvs.stddev);
            d.metrics["vel_p5"] = static_cast<double>(gvs.p5);
            d.metrics["vel_p95"] = static_cast<double>(gvs.p95);
            d.metrics["dynamic_range"] = static_cast<double>(gvs.p95 >= gvs.p5 ? gvs.p95 - gvs.p5 : 0u);
            d.metrics["crest_factor"] = round2(gvs.crest);
            d.metrics["flat_track_count"] = static_cast<double>(flat_tracks);

            double sc = 100.0;
            if (flat_tracks > 0u) {
                sc -= std::min(20.0, static_cast<double>(flat_tracks) * 5.0);
                addFinding(MusicalDimensionId::D05_VelocityDynamics,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D05_FLAT_VELOCITY_TRACKS", "STYLE", 0xFF, 0, 0,
                           std::to_string(flat_tracks) + " of " + std::to_string(active_tracks) +
                           " active track(s) exhibit flat velocities (std < " +
                           formatDouble(opt.flat_vel_std_thresh) + ")");
            }

            if (source_uasf != nullptr) {
                std::vector<uint8_t> src_vels;
                for (const auto& sec : source_uasf->sections) {
                    for (const auto& tr : sec.tracks) {
                        for (const auto& ev : tr.events) {
                            if (ev.type == uasf::MidiEventType::NoteOn && ev.data2 > 0u) {
                                src_vels.push_back(static_cast<uint8_t>(ev.data2 & 0x7Fu));
                            }
                        }
                    }
                }
                const auto svs = computeVelStats(src_vels);
                const double vel_delta = gvs.mean - svs.mean;
                const double crest_pres = (svs.stddev > 1e-6) ? (gvs.stddev / svs.stddev) : 1.0;
                d.metrics["source_mean_velocity"] = round2(svs.mean);
                d.metrics["source_vel_stddev"] = round2(svs.stddev);
                d.metrics["source_to_export_vel_delta"] = round2(vel_delta);
                d.metrics["stddev_preservation_ratio"] = round2(crest_pres);
                if (!any_megavoice_sanitized && svs.stddev >= 4.0 && gvs.stddev < 0.85 * svs.stddev) {
                    sc -= 20.0;
                    addFinding(MusicalDimensionId::D05_VelocityDynamics,
                               MusicalFindingSeverity::Warning,
                               "MQ_D05_UNINTENDED_DYNAMIC_COMPRESSION", "STYLE", 0xFF, 0, 0,
                               "Export velocity stddev compressed vs source (" +
                               formatDouble(gvs.stddev) + " vs " + formatDouble(svs.stddev) + ")");
                }
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D06 — Expression & Controller Translation
        // =====================================================================
        uint32_t shared_ending_snapbacks = 0;
        {
            auto& d = out.dimensions[5];
            uint32_t open_cc64 = 0, uncentered_pb = 0, nrpn_leaks = 0, cc94_leaks = 0;
            uint32_t cc11_events = 0, cc1_events = 0, cc74_events = 0;
            uint8_t cc11_min = 127u, cc11_max = 0u;
            uint32_t abrupt_loop_jumps = 0;
            uint32_t ending_snapbacks = 0;

            for (const auto& ep : parsed) {
                std::array<uint8_t, 16> last_cc64{};
                std::array<uint16_t, 16> last_pb{};
                last_pb.fill(8192u);
                std::array<std::vector<std::pair<uint32_t, uint8_t>>, 16> cc11_seq{};

                for (const auto& ev : ep.el->events) {
                    const uint8_t hi = ev.status & 0xF0u;
                    const uint8_t ch = ev.status & 0x0Fu;
                    if (hi == 0xB0u) {
                        if (ev.d1 == 64u) last_cc64[ch] = ev.d2;
                        else if (ev.d1 == 98u || ev.d1 == 99u) ++nrpn_leaks;
                        else if (ev.d1 == 94u) ++cc94_leaks;
                        else if (ev.d1 == 1u) ++cc1_events;
                        else if (ev.d1 == 74u) ++cc74_events;
                        else if (ev.d1 == 11u) {
                            ++cc11_events;
                            const uint8_t v = static_cast<uint8_t>(ev.d2 & 0x7Fu);
                            if (v < cc11_min) cc11_min = v;
                            if (v > cc11_max) cc11_max = v;
                            cc11_seq[ch].emplace_back(ev.tick, v);
                        }
                    } else if (hi == 0xE0u) {
                        last_pb[ch] = static_cast<uint16_t>(
                            static_cast<uint16_t>(ev.d1 & 0x7Fu) |
                            (static_cast<uint16_t>(ev.d2 & 0x7Fu) << 7));
                    }
                }

                const bool is_ending = isEndingMarker(ep.el->marker);
                const uint32_t len = ep.el->length;
                const uint32_t eighth_bar = (ep.bar_ticks >= 8u) ? (ep.bar_ticks / 8u) : 1u;
                const uint32_t loop_win_start = (len > eighth_bar) ? (len - eighth_bar) : 0u;

                for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                    if (last_cc64[ch] >= 64u) {
                        ++open_cc64;
                        addFinding(MusicalDimensionId::D06_ExpressionControllers,
                                   MusicalFindingSeverity::Critical,
                                   "MQ_D06_OPEN_CC64_AT_END", ep.el->marker, ch, 0, len > 0u ? len - 1u : 0u,
                                   "Sustain pedal CC64 left open (" + std::to_string(last_cc64[ch]) + ") at element end");
                    }
                    if (last_pb[ch] != 8192u) {
                        ++uncentered_pb;
                        addFinding(MusicalDimensionId::D06_ExpressionControllers,
                                   MusicalFindingSeverity::Critical,
                                   "MQ_D06_UNCENTERED_PB_AT_END", ep.el->marker, ch, 0, len > 0u ? len - 1u : 0u,
                                   "Pitch Bend left uncentered (" + std::to_string(last_pb[ch]) + ") at element end");
                    }
                    const auto& seq = cc11_seq[ch];
                    if (seq.size() >= 2u && len > 0u) {
                        if (is_ending) {
                            const auto [last_t, last_v] = seq.back();
                            const auto [prev_t, prev_v] = seq[seq.size() - 2u];
                            uint32_t last_note_on_t = 0u;
                            bool has_note_ch = false;
                            for (const auto& n : ep.by_ch[ch]) {
                                if (!has_note_ch || n.on_tick > last_note_on_t) last_note_on_t = n.on_tick;
                                has_note_ch = true;
                            }
                            const bool trailing_fade_low = (prev_t >= len / 2u && prev_v < 32u && prev_t < last_t);
                            const bool boundary_snapback = (last_t + 2u >= len && last_v >= 64u &&
                                                            (static_cast<int>(last_v) - static_cast<int>(prev_v)) >= 32 &&
                                                            (!has_note_ch || last_t > last_note_on_t));
                            if (trailing_fade_low && boundary_snapback) {
                                ++ending_snapbacks;
                                addFinding(MusicalDimensionId::D06_ExpressionControllers,
                                           MusicalFindingSeverity::Warning,
                                           "MQ_D06_ENDING_CC11_SNAPBACK", ep.el->marker, ch,
                                           barNumber1Based(last_t, ep.bar_ticks), last_t,
                                           "One-shot Ending CC11 fades to " + std::to_string(prev_v) +
                                           " then snaps back to " + std::to_string(last_v) +
                                           " at tick " + std::to_string(last_t) + " (risks tail-swell pop)",
                                           "ending_fade_pop_guard");
                            }
                        } else {
                            for (size_t k = 1u; k < seq.size(); ++k) {
                                const int delta = std::abs(static_cast<int>(seq[k].second) -
                                                           static_cast<int>(seq[k - 1u].second));
                                if (seq[k].first >= loop_win_start &&
                                    delta >= static_cast<int>(opt.abrupt_cc11_jump_min)) {
                                    ++abrupt_loop_jumps;
                                    addFinding(MusicalDimensionId::D06_ExpressionControllers,
                                               MusicalFindingSeverity::Advisory,
                                               "MQ_D06_ABRUPT_LOOP_CC11_JUMP", ep.el->marker, ch,
                                               barNumber1Based(seq[k].first, ep.bar_ticks), seq[k].first,
                                               "Abrupt single-step CC11 jump (" +
                                               std::to_string(seq[k - 1u].second) + " -> " +
                                               std::to_string(seq[k].second) + ") near loop end",
                                               "ending_fade_pop_guard");
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            shared_ending_snapbacks = ending_snapbacks;
            const bool sidechain_ok = (cc11_events >= 16u && cc11_max >= cc11_min &&
                                       (cc11_max - cc11_min) >= 40u);
            d.applicable = !parsed.empty();
            d.metrics["open_cc64_at_end"] = static_cast<double>(open_cc64);
            d.metrics["uncentered_pb_at_end"] = static_cast<double>(uncentered_pb);
            d.metrics["nrpn_leaks"] = static_cast<double>(nrpn_leaks);
            d.metrics["cc94_leaks"] = static_cast<double>(cc94_leaks);
            d.metrics["cc11_event_count"] = static_cast<double>(cc11_events);
            d.metrics["cc11_range"] = (cc11_events > 0u && cc11_max >= cc11_min)
                ? static_cast<double>(cc11_max - cc11_min) : 0.0;
            d.metrics["cc11_sidechain_preserved"] = sidechain_ok ? 1.0 : 0.0;
            d.metrics["cc1_event_count"] = static_cast<double>(cc1_events);
            d.metrics["cc74_event_count"] = static_cast<double>(cc74_events);
            d.metrics["ending_cc11_snapback_count"] = static_cast<double>(ending_snapbacks);
            d.metrics["abrupt_loop_cc11_jumps"] = static_cast<double>(abrupt_loop_jumps);

            double sc = 100.0;
            if (open_cc64 > 0u) sc -= std::min(50.0, static_cast<double>(open_cc64) * 25.0);
            if (uncentered_pb > 0u) sc -= std::min(50.0, static_cast<double>(uncentered_pb) * 25.0);
            if (nrpn_leaks > 0u) {
                sc -= 20.0;
                addFinding(MusicalDimensionId::D06_ExpressionControllers,
                           MusicalFindingSeverity::Critical,
                           "MQ_D06_NRPN_LEAK", "STYLE", 0xFF, 0, 0,
                           std::to_string(nrpn_leaks) + " proprietary NRPN (CC98/99) event(s) leaked into export");
            }
            if (cc94_leaks > 0u) {
                sc -= 15.0;
                addFinding(MusicalDimensionId::D06_ExpressionControllers,
                           MusicalFindingSeverity::Warning,
                           "MQ_D06_CC94_LEAK", "STYLE", 0xFF, 0, 0,
                           std::to_string(cc94_leaks) + " Yamaha Variation Send (CC94) event(s) leaked into export");
            }
            if (ending_snapbacks > 0u) sc -= std::min(25.0, static_cast<double>(ending_snapbacks) * 12.0);
            if (abrupt_loop_jumps > 0u) sc -= std::min(15.0, static_cast<double>(abrupt_loop_jumps) * 5.0);
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D07 — Sound-Family Suitability
        // =====================================================================
        {
            auto& d = out.dimensions[6];
            uint32_t evaluated = 0, exact_m = 0, approx_m = 0, user_ov = 0, unresolved = 0;
            uint32_t elem_switches = 0, contaminations = 0, timbre_swaps = 0;

            if (report != nullptr && report->sound_match_stats.tracks_evaluated > 0u) {
                const auto& st = report->sound_match_stats;
                evaluated = st.tracks_evaluated;
                exact_m = st.exact_matches;
                approx_m = st.approx_matches;
                user_ov = st.user_overrides;
                unresolved = st.unresolved;
                elem_switches = st.element_voice_switches;
                contaminations = st.cross_family_contaminations;
            } else {
                for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                    if (ch_vels[ch].empty()) continue;
                    ++evaluated;
                    if (first_pc[ch] >= 0) {
                        ++exact_m;
                        report_resolved[ch] = true;
                        // Detect obvious drum<->melodic bank contamination
                        if ((ch == kDrumCh || ch == kPercCh) && first_msb[ch] == 121) {
                            ++contaminations;
                        } else if (isChordDriven(ch) && first_msb[ch] == 120) {
                            ++contaminations;
                        }
                    } else {
                        ++unresolved;
                    }
                }
            }

            // Cross-variation timbre coherence (v1..v4): classify strictly per-element PC
            // so acoustic->synth AND synth->acoustic swaps are detected symmetrically.
            std::array<std::set<std::string>, 16> var_classes{};
            for (const auto& ep : parsed) {
                if (!isVariationMarker(ep.el->marker) || cvNumberOf(ep.el->marker) != 1u) continue;
                for (uint8_t ch = kAcc1Ch; ch <= 15u; ++ch) {
                    if (ep.pc0[ch] >= 0) {
                        const std::string cls = timbreBroadClass(ep.pc0[ch], "");
                        if (cls != "unknown") var_classes[ch].insert(cls);
                    }
                }
            }
            for (uint8_t ch = kAcc1Ch; ch <= 15u; ++ch) {
                if (var_classes[ch].count("acoustic") > 0u && var_classes[ch].count("synth") > 0u) {
                    ++timbre_swaps;
                }
            }

            d.applicable = (evaluated > 0u);
            const uint32_t resolved_cnt = (evaluated >= unresolved) ? (evaluated - unresolved) : 0u;
            const double resolved_ratio = (evaluated > 0u)
                ? (static_cast<double>(resolved_cnt) / static_cast<double>(evaluated))
                : 1.0;

            d.metrics["tracks_evaluated"] = static_cast<double>(evaluated);
            d.metrics["exact_matches"] = static_cast<double>(exact_m);
            d.metrics["approx_matches"] = static_cast<double>(approx_m);
            d.metrics["user_overrides"] = static_cast<double>(user_ov);
            d.metrics["unresolved_count"] = static_cast<double>(unresolved);
            d.metrics["resolved_track_ratio"] = round2(resolved_ratio);
            d.metrics["element_voice_switches"] = static_cast<double>(elem_switches);
            d.metrics["drum_melodic_contaminations"] = static_cast<double>(contaminations);
            d.metrics["cross_variation_timbre_swaps"] = static_cast<double>(timbre_swaps);

            double sc = 100.0 * resolved_ratio;
            if (contaminations > 0u) {
                sc -= 40.0;
                addFinding(MusicalDimensionId::D07_SoundFamilySuitability,
                           MusicalFindingSeverity::Critical,
                           "MQ_D07_DRUM_MELODIC_CONTAMINATION", "STYLE", 0xFF, 0, 0,
                           std::to_string(contaminations) + " drum<->melodic sound-family cross-contamination(s)");
            }
            if (unresolved > 0u) {
                addFinding(MusicalDimensionId::D07_SoundFamilySuitability,
                           MusicalFindingSeverity::Warning,
                           "MQ_D07_UNRESOLVED_TRACKS", "STYLE", 0xFF, 0, 0,
                           std::to_string(unresolved) + " of " + std::to_string(evaluated) +
                           " track(s) remain fail-closed UNRESOLVED (private/unknown bank never silently guessed)");
            }
            if (timbre_swaps > 0u) {
                sc -= 6.0;
                addFinding(MusicalDimensionId::D07_SoundFamilySuitability,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D07_CROSS_VAR_TIMBRE_SWAP", "STYLE", 0xFF, 0, 0,
                           std::to_string(timbre_swaps) + " accompaniment track(s) jump acoustic<->synth across Variations");
            }
            addFinding(MusicalDimensionId::D07_SoundFamilySuitability,
                       MusicalFindingSeverity::Info,
                       "MQ_D07_OFFLINE_NOT_HW_VERIFIED", "STYLE", 0xFF, 0, 0,
                       "Sound assignments are offline catalog/map matches (hardwareVerified=false, l3Status=NOT_TESTED)");
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D08 — Groove & Rhythmic Feel
        // =====================================================================
        {
            auto& d = out.dimensions[7];
            double swing_sum = 0.0;
            uint32_t swing_cnt = 0;
            double jitter_sum_ms = 0.0;
            uint32_t jitter_cnt = 0;
            uint32_t anticipations = 0;
            std::set<std::pair<uint8_t, uint8_t>> meters;
            bool odd_meter = false;

            const uint32_t q = korg_style.ppqn;
            const double ms_per_tick = (q > 0u && out.tempo_bpm > 0.0)
                ? (60000.0 / (out.tempo_bpm * static_cast<double>(q)))
                : 0.0;
            const uint32_t sixteenth = (q >= 4u) ? (q / 4u) : 1u;

            for (const auto& ep : parsed) {
                if (ep.el->num > 0u && ep.el->den > 0u) {
                    meters.emplace(ep.el->num, ep.el->den);
                    if (ep.el->num != 4u || ep.el->den != 4u) odd_meter = true;
                }
                if (q == 0u) continue;
                for (const auto& n : ep.notes) {
                    const uint32_t beat_pos = n.on_tick % q;
                    const double frac = static_cast<double>(beat_pos) / static_cast<double>(q);
                    if (frac >= 0.35 && frac <= 0.78) {
                        swing_sum += frac * 100.0;
                        ++swing_cnt;
                    }
                    if (ms_per_tick > 0.0) {
                        const uint32_t rem = n.on_tick % sixteenth;
                        const uint32_t dist = std::min(rem, sixteenth - rem);
                        jitter_sum_ms += static_cast<double>(dist) * ms_per_tick;
                        ++jitter_cnt;
                    }
                    if (ep.bar_ticks > sixteenth && ep.bar_ticks > 0u) {
                        const uint32_t in_bar = n.on_tick % ep.bar_ticks;
                        if (in_bar >= (ep.bar_ticks - sixteenth) && in_bar < ep.bar_ticks) {
                            ++anticipations;
                        }
                    }
                }
            }

            d.applicable = !parsed.empty();
            const double swing_pct = (swing_cnt > 0u) ? (swing_sum / static_cast<double>(swing_cnt)) : 50.0;
            const double jitter_ms = (jitter_cnt > 0u) ? (jitter_sum_ms / static_cast<double>(jitter_cnt)) : 0.0;

            d.metrics["swing_ratio_pct"] = round2(swing_pct);
            d.metrics["microtiming_jitter_ms"] = round2(jitter_ms);
            d.metrics["anticipation_count"] = static_cast<double>(anticipations);
            d.metrics["meter_count"] = static_cast<double>(meters.size());
            d.metrics["odd_meter_present"] = odd_meter ? 1.0 : 0.0;

            double sc = 100.0;
            if (q == 0u) {
                sc = 50.0;
                addFinding(MusicalDimensionId::D08_GrooveRhythmicFeel,
                           MusicalFindingSeverity::Warning,
                           "MQ_D08_INVALID_PPQN", "STYLE", 0xFF, 0, 0,
                           "PPQN is 0; rhythmic timing metrics unavailable");
            } else {
                if (swing_pct >= 57.0) {
                    addFinding(MusicalDimensionId::D08_GrooveRhythmicFeel,
                               MusicalFindingSeverity::Info,
                               "MQ_D08_SWING_FEEL_PRESERVED", "STYLE", 0xFF, 0, 0,
                               "Swing feel preserved (" + formatDouble(swing_pct) + "% 8th-note offbeat)");
                }
                if (odd_meter) {
                    addFinding(MusicalDimensionId::D08_GrooveRhythmicFeel,
                               MusicalFindingSeverity::Info,
                               "MQ_D08_ODD_OR_COMPOUND_METER", "STYLE", 0xFF, 0, 0,
                               "Style contains non-4/4 meter(s) (" + std::to_string(meters.size()) + " distinct)");
                }
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D09 — Variation-to-Variation Consistency (V1 -> V2 -> V3 -> V4)
        // =====================================================================
        {
            auto& d = out.dimensions[8];
            std::map<char, const ElemParsed*> var_cv1;
            for (const auto& ep : parsed) {
                if (isVariationMarker(ep.el->marker) && cvNumberOf(ep.el->marker) == 1u) {
                    var_cv1[ep.el->marker[1]] = &ep;
                }
            }
            double base_dens = 0.0;
            for (const auto& [idx_ch, ep] : var_cv1) {
                (void)idx_ch;
                VariationEnergyPoint pt;
                pt.marker = ep->el->marker;
                pt.bars = (ep->bars > 0u) ? ep->bars : 1u;
                pt.notes_per_bar = round2(static_cast<double>(ep->notes.size()) / static_cast<double>(pt.bars));
                std::vector<uint8_t> vv;
                vv.reserve(ep->notes.size());
                uint32_t act = 0;
                for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                    if (!ep->by_ch[ch].empty()) ++act;
                }
                for (const auto& n : ep->notes) vv.push_back(n.vel);
                pt.mean_velocity = round2(computeVelStats(vv).mean);
                pt.active_tracks = act;
                if (base_dens <= 0.0) base_dens = std::max(1.0, pt.notes_per_bar);
                const double dens_ratio = pt.notes_per_bar / base_dens;
                pt.composite_energy = round2(pt.mean_velocity * (1.0 + 0.15 * std::log2(1.0 + std::max(0.0, dens_ratio))));
                out.variation_curve.push_back(std::move(pt));
            }

            d.applicable = (out.variation_curve.size() >= 2u);
            double min_v = 127.0, max_v = 0.0;
            double min_d = 1e9, max_d = 0.0;
            uint32_t vel_inversions = 0;
            uint32_t dens_inversions = 0;
            uint32_t intentional_sparse_ff = 0;
            for (size_t i = 0; i < out.variation_curve.size(); ++i) {
                const auto& cur = out.variation_curve[i];
                if (cur.mean_velocity < min_v) min_v = cur.mean_velocity;
                if (cur.mean_velocity > max_v) max_v = cur.mean_velocity;
                if (cur.notes_per_bar < min_d) min_d = cur.notes_per_bar;
                if (cur.notes_per_bar > max_d) max_d = cur.notes_per_bar;
                if (i > 0u) {
                    const auto& prev = out.variation_curve[i - 1u];
                    if (cur.mean_velocity < prev.mean_velocity - 2.0) {
                        ++vel_inversions;
                    }
                    if (prev.notes_per_bar > 0.0 && cur.notes_per_bar < prev.notes_per_bar * 0.85) {
                        if (cur.mean_velocity >= prev.mean_velocity + 6.0) {
                            ++intentional_sparse_ff;
                        } else {
                            ++dens_inversions;
                        }
                    }
                }
            }
            const bool flat_curve = d.applicable && ((max_v - min_v) < 3.5) && ((max_d - min_d) < 1.5);

            // CC7 consistency across Variations
            uint32_t cc7_jumps = 0;
            for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                int lo_c = 128, hi_c = -1;
                for (const auto& [idx_ch, ep] : var_cv1) {
                    (void)idx_ch;
                    if (ep->cc7_0[ch] >= 0) {
                        if (ep->cc7_0[ch] < lo_c) lo_c = ep->cc7_0[ch];
                        if (ep->cc7_0[ch] > hi_c) hi_c = ep->cc7_0[ch];
                    }
                }
                if (hi_c >= 0 && lo_c <= 127 && (hi_c - lo_c) > 20) ++cc7_jumps;
            }

            d.metrics["variation_count"] = static_cast<double>(out.variation_curve.size());
            d.metrics["vel_span"] = d.applicable ? round2(max_v - min_v) : 0.0;
            d.metrics["density_span"] = d.applicable ? round2(max_d - min_d) : 0.0;
            d.metrics["flat_variation_curve"] = flat_curve ? 1.0 : 0.0;
            d.metrics["velocity_inversions"] = static_cast<double>(vel_inversions);
            d.metrics["density_inversions"] = static_cast<double>(dens_inversions);
            d.metrics["intentional_sparse_ff"] = static_cast<double>(intentional_sparse_ff);
            d.metrics["cc7_variation_jumps"] = static_cast<double>(cc7_jumps);

            double sc = 100.0;
            if (flat_curve) {
                sc -= 12.0;
                addFinding(MusicalDimensionId::D09_VariationConsistency,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D09_FLAT_VARIATION_CURVE", "STYLE", 0xFF, 0, 0,
                           "Variation V1..V" + std::to_string(out.variation_curve.size()) +
                           " energy curve is flat (vel span=" + formatDouble(max_v - min_v) + ")",
                           "variation_energy_curve");
            }
            if (vel_inversions > 0u) {
                sc -= std::min(20.0, static_cast<double>(vel_inversions) * 10.0);
                addFinding(MusicalDimensionId::D09_VariationConsistency,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D09_VARIATION_VEL_INVERSION", "STYLE", 0xFF, 0, 0,
                           std::to_string(vel_inversions) + " Variation velocity inversion(s) detected",
                           "variation_energy_curve");
            }
            if (dens_inversions > 0u) {
                sc -= 6.0;
                addFinding(MusicalDimensionId::D09_VariationConsistency,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D09_VARIATION_DENSITY_DIP", "STYLE", 0xFF, 0, 0,
                           std::to_string(dens_inversions) + " Variation density dip(s) without compensating dynamic lift");
            }
            if (intentional_sparse_ff > 0u) {
                addFinding(MusicalDimensionId::D09_VariationConsistency,
                           MusicalFindingSeverity::Info,
                           "MQ_D09_INTENTIONAL_SPARSE_FF_CONTRAST", "STYLE", 0xFF, 0, 0,
                           "Intentional sparse-fortissimo orchestral Variation contrast preserved");
            }
            if (cc7_jumps > 0u) {
                sc -= 12.0;
                addFinding(MusicalDimensionId::D09_VariationConsistency,
                           MusicalFindingSeverity::Warning,
                           "MQ_D09_CC7_VARIATION_JUMP", "STYLE", 0xFF, 0, 0,
                           std::to_string(cc7_jumps) + " channel(s) jump >20 CC7 units across Variations");
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D10 — Fill-to-Variation Transition Quality
        // =====================================================================
        {
            auto& d = out.dimensions[9];
            uint32_t fill_count = 0;
            bool break_present = false;
            bool break_1bar_valid = true;
            uint32_t flat_or_decresc_fills = 0;
            double cresc_sum = 0.0;
            uint32_t cresc_cnt = 0;
            uint32_t handoff_viol = 0;

            for (const auto& ep : parsed) {
                const bool is_f = isFillMarker(ep.el->marker);
                const bool is_br = (ep.el->marker == "br");
                if (!is_f && !is_br) continue;

                if (is_f) ++fill_count;
                if (is_br) {
                    break_present = true;
                    if (ep.bar_ticks > 0u && ep.el->length != ep.bar_ticks) {
                        break_1bar_valid = false;
                    }
                }

                // Check handoff CC64 / PB cleanliness on Fill/Break
                std::array<uint8_t, 16> last_cc64{};
                std::array<uint16_t, 16> last_pb{};
                last_pb.fill(8192u);
                for (const auto& ev : ep.el->events) {
                    const uint8_t hi = ev.status & 0xF0u;
                    const uint8_t ch = ev.status & 0x0Fu;
                    if (hi == 0xB0u && ev.d1 == 64u) last_cc64[ch] = ev.d2;
                    if (hi == 0xE0u) {
                        last_pb[ch] = static_cast<uint16_t>(
                            static_cast<uint16_t>(ev.d1 & 0x7Fu) |
                            (static_cast<uint16_t>(ev.d2 & 0x7Fu) << 7));
                    }
                }
                for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                    if (last_cc64[ch] >= 64u || last_pb[ch] != 8192u) ++handoff_viol;
                }

                if (is_f && ep.el->length > 0u && !ep.notes.empty()) {
                    const uint32_t half = ep.el->length / 2u;
                    std::vector<uint8_t> h1, h2;
                    for (const auto& n : ep.notes) {
                        if (n.on_tick < half) h1.push_back(n.vel);
                        else h2.push_back(n.vel);
                    }
                    if (!h1.empty() && !h2.empty()) {
                        const double diff = computeVelStats(h2).mean - computeVelStats(h1).mean;
                        cresc_sum += diff;
                        ++cresc_cnt;
                        if (diff <= 0.0) ++flat_or_decresc_fills;
                    }
                }
            }

            d.applicable = (fill_count > 0u || break_present);
            const double mean_cresc = (cresc_cnt > 0u) ? (cresc_sum / static_cast<double>(cresc_cnt)) : 0.0;
            d.metrics["fill_count"] = static_cast<double>(fill_count);
            d.metrics["break_present"] = break_present ? 1.0 : 0.0;
            d.metrics["break_1bar_valid"] = break_1bar_valid ? 1.0 : 0.0;
            d.metrics["fill_crescendo_delta"] = round2(mean_cresc);
            d.metrics["fill_flat_or_decresc_count"] = static_cast<double>(flat_or_decresc_fills);
            d.metrics["fill_handoff_violations"] = static_cast<double>(handoff_viol);

            double sc = 100.0;
            if (handoff_viol > 0u) {
                sc -= std::min(50.0, static_cast<double>(handoff_viol) * 25.0);
                addFinding(MusicalDimensionId::D10_FillTransitionQuality,
                           MusicalFindingSeverity::Critical,
                           "MQ_D10_FILL_HANDOFF_VIOLATION", "STYLE", 0xFF, 0, 0,
                           std::to_string(handoff_viol) + " Fill/Break handoff controller violation(s) (CC64/PB)");
            }
            if (break_present && !break_1bar_valid) {
                sc -= 20.0;
                addFinding(MusicalDimensionId::D10_FillTransitionQuality,
                           MusicalFindingSeverity::Warning,
                           "MQ_D10_BREAK_NOT_1BAR", "br", 0xFF, 0, 0,
                           "Break element length is not exactly 1 bar");
            }
            if (flat_or_decresc_fills > 0u) {
                sc -= 8.0;
                addFinding(MusicalDimensionId::D10_FillTransitionQuality,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D10_FILL_NO_CRESCENDO", "STYLE", kDrumCh, 0, 0,
                           std::to_string(flat_or_decresc_fills) +
                           " Fill element(s) show flat/decrescendo turnaround velocity",
                           "drum_ghost_dynamics");
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D11 — Intro & Ending Musical Integrity
        // =====================================================================
        {
            auto& d = out.dimensions[10];
            uint32_t intro_cnt = 0, ending_cnt = 0;
            uint32_t max_harmonic_arc = 0;
            double intro_off_sum = 0.0;
            uint32_t intro_off_cnt = 0;
            double end_off_sum = 0.0;
            uint32_t end_off_cnt = 0;

            for (const auto& ep : parsed) {
                const bool is_i = isIntroMarker(ep.el->marker);
                const bool is_e = isEndingMarker(ep.el->marker);
                if (!is_i && !is_e) continue;
                if (is_i) ++intro_cnt;
                if (is_e) ++ending_cnt;

                uint32_t max_off = 0;
                std::set<uint16_t> distinct_bar_pcs;
                std::map<uint32_t, uint16_t> bar_pc_mask;
                for (const auto& n : ep.notes) {
                    if (!isChordDriven(n.ch)) continue;
                    if (n.off_tick > max_off) max_off = n.off_tick;
                    if (ep.bar_ticks > 0u) {
                        const uint32_t b = n.on_tick / ep.bar_ticks;
                        bar_pc_mask[b] |= static_cast<uint16_t>(1u << (n.pitch % 12u));
                    }
                }
                for (const auto& [b, mask] : bar_pc_mask) {
                    (void)b;
                    distinct_bar_pcs.insert(mask);
                }
                const uint32_t arc = static_cast<uint32_t>(distinct_bar_pcs.size());
                if (arc > max_harmonic_arc) max_harmonic_arc = arc;

                if (ep.el->length > 0u && max_off > 0u) {
                    const double ratio = std::min(1.0, static_cast<double>(max_off) /
                                                       static_cast<double>(ep.el->length));
                    if (is_i) { intro_off_sum += ratio; ++intro_off_cnt; }
                    if (is_e) { end_off_sum += ratio; ++end_off_cnt; }
                }
            }

            d.applicable = (intro_cnt > 0u || ending_cnt > 0u);
            const double intro_ratio = (intro_off_cnt > 0u) ? (intro_off_sum / static_cast<double>(intro_off_cnt)) : 0.0;
            const double end_ratio = (end_off_cnt > 0u) ? (end_off_sum / static_cast<double>(end_off_cnt)) : 0.0;

            d.metrics["intro_count"] = static_cast<double>(intro_cnt);
            d.metrics["ending_count"] = static_cast<double>(ending_cnt);
            d.metrics["max_harmonic_arc_bars"] = static_cast<double>(max_harmonic_arc);
            d.metrics["intro_mean_leadin_ratio"] = round2(intro_ratio);
            d.metrics["ending_mean_release_ratio"] = round2(end_ratio);
            d.metrics["ending_cc11_snapback_count"] = static_cast<double>(shared_ending_snapbacks);

            double sc = 100.0;
            if (shared_ending_snapbacks > 0u) {
                sc -= std::min(25.0, static_cast<double>(shared_ending_snapbacks) * 15.0);
                addFinding(MusicalDimensionId::D11_IntroEndingIntegrity,
                           MusicalFindingSeverity::Warning,
                           "MQ_D11_ENDING_FADE_POP_RISK", "STYLE", 0xFF, 0, 0,
                           std::to_string(shared_ending_snapbacks) +
                           " one-shot Ending track(s) suffer final-tick CC11 snap-back after fade-out",
                           "ending_fade_pop_guard");
            }
            if (intro_off_cnt > 0u && intro_ratio < 0.70) {
                sc -= 8.0;
                addFinding(MusicalDimensionId::D11_IntroEndingIntegrity,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D11_INTRO_EARLY_CUTOFF", "STYLE", 0xFF, 0, 0,
                           "Intro melodic lead-in cuts off early (ratio=" + formatDouble(intro_ratio) + ")");
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // =====================================================================
        // D12 — Mix Balance & Effects Translation
        // =====================================================================
        {
            auto& d = out.dimensions[11];
            int anchor_cc7 = -1;
            double acc_cc7_sum = 0.0;
            uint32_t acc_cc7_cnt = 0;
            int min_pan = 128, max_pan = -1;
            uint32_t pan_cnt = 0;
            uint32_t cc91_cnt = 0, cc93_cnt = 0;
            bool bass_drum_centered = true;
            bool bass_fx_clean = true;
            uint32_t active_tracks = 0;

            for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                if (ch_vels[ch].empty()) continue;
                ++active_tracks;
                if (first_cc7[ch] >= 0) {
                    if (ch == kBassCh || ch == kDrumCh) {
                        if (first_cc7[ch] > anchor_cc7) anchor_cc7 = first_cc7[ch];
                    } else if (ch >= kAcc1Ch) {
                        acc_cc7_sum += static_cast<double>(first_cc7[ch]);
                        ++acc_cc7_cnt;
                    }
                }
                if (first_cc10[ch] >= 0) {
                    ++pan_cnt;
                    if (first_cc10[ch] < min_pan) min_pan = first_cc10[ch];
                    if (first_cc10[ch] > max_pan) max_pan = first_cc10[ch];
                    if ((ch == kBassCh || ch == kDrumCh) && std::abs(first_cc10[ch] - 64) > 8) {
                        bass_drum_centered = false;
                    }
                }
                if (first_cc91[ch] >= 0) {
                    ++cc91_cnt;
                    if (ch == kBassCh && first_cc91[ch] > 36) bass_fx_clean = false;
                }
                if (first_cc93[ch] >= 0) {
                    ++cc93_cnt;
                    if (ch == kBassCh && first_cc93[ch] > 12) bass_fx_clean = false;
                }
            }

            d.applicable = (active_tracks > 0u);
            const double acc_cc7_mean = (acc_cc7_cnt > 0u)
                ? (acc_cc7_sum / static_cast<double>(acc_cc7_cnt)) : 0.0;
            const bool hierarchy_ok = (anchor_cc7 < 0 || acc_cc7_cnt == 0u ||
                                       static_cast<double>(anchor_cc7) >= acc_cc7_mean - 4.0);
            const int pan_spread = (pan_cnt >= 2u && max_pan >= min_pan) ? (max_pan - min_pan) : 0;
            const bool dry_mono = (active_tracks >= 2u && pan_spread == 0 && cc91_cnt == 0u && cc93_cnt == 0u);

            d.metrics["cc7_anchor_hierarchy_ok"] = hierarchy_ok ? 1.0 : 0.0;
            d.metrics["pan_spread"] = static_cast<double>(pan_spread);
            d.metrics["bass_drum_centered_ok"] = bass_drum_centered ? 1.0 : 0.0;
            d.metrics["bass_reverb_chorus_clean"] = bass_fx_clean ? 1.0 : 0.0;
            d.metrics["cc91_tracks_set"] = static_cast<double>(cc91_cnt);
            d.metrics["cc93_tracks_set"] = static_cast<double>(cc93_cnt);
            d.metrics["dry_mono_mix_flag"] = dry_mono ? 1.0 : 0.0;

            double sc = 100.0;
            if (!hierarchy_ok) {
                sc -= 12.0;
                addFinding(MusicalDimensionId::D12_MixBalanceEffects,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D12_CC7_HIERARCHY_INVERTED", "STYLE", 0xFF, 0, 0,
                           "Accompaniment mean CC7 (" + formatDouble(acc_cc7_mean) +
                           ") exceeds Drum/Bass anchor CC7 (" + std::to_string(anchor_cc7) + ")");
            }
            if (!bass_drum_centered) {
                sc -= 10.0;
                addFinding(MusicalDimensionId::D12_MixBalanceEffects,
                           MusicalFindingSeverity::Warning,
                           "MQ_D12_BASS_DRUM_OFF_CENTER", "STYLE", 0xFF, 0, 0,
                           "Bass or Drum track CC10 pan is off-center (>8 units from 64)");
            }
            if (!bass_fx_clean) {
                sc -= 10.0;
                addFinding(MusicalDimensionId::D12_MixBalanceEffects,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D12_BASS_EXCESS_FX", "STYLE", kBassCh, 0, 0,
                           "Bass track has elevated Reverb (CC91>36) or Chorus (CC93>12) send");
            }
            if (dry_mono) {
                sc -= 8.0;
                addFinding(MusicalDimensionId::D12_MixBalanceEffects,
                           MusicalFindingSeverity::Advisory,
                           "MQ_D12_DRY_MONO_DEFAULT_MIX", "STYLE", 0xFF, 0, 0,
                           "Style omits stereo panning (CC10) and Reverb/Chorus sends (CC91/CC93)");
            }
            d.score = d.applicable ? clampScore(sc) : 100.0;
            d.status = statusFromScoreAndFindings(d.applicable, d.score, d.findings);
        }

        // ── Populate ordered TrackMusicalSummary (ch 8..15) ─────────────
        for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
            if (ch_vels[ch].empty()) continue;
            TrackMusicalSummary ts;
            ts.korg_ch = ch;
            ts.track_name = korgTrackName(ch);
            ts.inferred_family = inferredFamilyFor(ch);
            const auto vs = computeVelStats(ch_vels[ch]);
            ts.note_count = vs.count;
            ts.min_pitch = ch_min_p[ch];
            ts.max_pitch = ch_max_p[ch];
            ts.mean_velocity = round2(vs.mean);
            ts.vel_stddev = round2(vs.stddev);
            ts.vel_p5 = vs.p5;
            ts.vel_p95 = vs.p95;
            ts.crest_factor = round2(vs.crest);
            ts.mean_duty_ratio = round2(ch_duty_elems[ch] > 0u
                ? (ch_duty_sum[ch] / static_cast<double>(ch_duty_elems[ch]))
                : 0.0);
            ts.max_polyphony = ch_max_poly[ch];
            ts.mud_clusters_lt_c3 = ch_mud_clusters[ch];
            ts.cc7_tick0 = first_cc7[ch];
            ts.cc10_tick0 = first_cc10[ch];
            ts.cc91_tick0 = first_cc91[ch];
            ts.cc93_tick0 = first_cc93[ch];
            ts.flat_velocity = (vs.count >= 8u && vs.stddev < opt.flat_vel_std_thresh);
            ts.sound_resolved = report_resolved[ch];
            out.tracks.push_back(std::move(ts));
        }

        // ── Overall score (unweighted mean of applicable dimensions) & sorted findings ──
        double score_sum = 0.0;
        uint32_t app_cnt = 0;
        for (const auto& dim : out.dimensions) {
            if (dim.applicable) {
                score_sum += dim.score;
                ++app_cnt;
            }
            for (const auto& f : dim.findings) {
                out.all_findings.push_back(f);
            }
        }
        out.overall_musical_score = (app_cnt > 0u) ? round2(score_sum / static_cast<double>(app_cnt)) : 0.0;

        std::sort(out.all_findings.begin(), out.all_findings.end(),
                  [](const MusicalFinding& a, const MusicalFinding& b) {
                      if (a.severity != b.severity) return static_cast<int>(a.severity) > static_cast<int>(b.severity);
                      if (a.dimension != b.dimension) return static_cast<int>(a.dimension) < static_cast<int>(b.dimension);
                      if (a.code != b.code) return a.code < b.code;
                      if (a.section_marker != b.section_marker) return a.section_marker < b.section_marker;
                      if (a.korg_ch != b.korg_ch) return a.korg_ch < b.korg_ch;
                      return a.tick < b.tick;
                  });
    } catch (...) {
        out.status_label = "ANALYSIS ERROR — NOT VERIFIED ON A PA5X";
    }
    return out;
}

inline MusicalQualityDelta compareMusicalQuality(
    const MusicalQualityReport& mode_a,
    const MusicalQualityReport& mode_b) noexcept {
    MusicalQualityDelta delta;
    try {
        using namespace musical_quality_detail;
        delta.overall_delta = round2(mode_b.overall_musical_score - mode_a.overall_musical_score);
        for (size_t i = 0; i < 12u; ++i) {
            delta.dimension_deltas[i] = round2(mode_b.dimensions[i].score - mode_a.dimensions[i].score);
        }
        std::set<std::string> a_warn_codes, b_warn_codes;
        std::set<std::string> a_crit_codes, b_crit_codes;
        std::set<std::string> a_all_codes, b_all_codes;
        for (const auto& f : mode_a.all_findings) {
            a_all_codes.insert(f.code);
            if (f.severity == MusicalFindingSeverity::Warning ||
                f.severity == MusicalFindingSeverity::Critical) {
                a_warn_codes.insert(f.code);
            }
            if (f.severity == MusicalFindingSeverity::Critical) {
                a_crit_codes.insert(f.code);
            }
        }
        for (const auto& f : mode_b.all_findings) {
            b_all_codes.insert(f.code);
            if (f.severity == MusicalFindingSeverity::Warning ||
                f.severity == MusicalFindingSeverity::Critical) {
                b_warn_codes.insert(f.code);
            }
            if (f.severity == MusicalFindingSeverity::Critical) {
                b_crit_codes.insert(f.code);
            }
        }
        for (const auto& c : a_all_codes) {
            if (b_all_codes.count(c) == 0u) {
                delta.resolved_finding_codes.push_back(c);
            }
        }
        for (const auto& c : b_warn_codes) {
            if (a_warn_codes.count(c) == 0u) {
                delta.newly_introduced_warnings.push_back(c);
            }
        }
        for (const auto& c : b_crit_codes) {
            if (a_crit_codes.count(c) == 0u) {
                delta.any_critical_regression = true;
                break;
            }
        }
    } catch (...) {
        delta.any_critical_regression = true;
    }
    return delta;
}

inline std::string formatMusicalQualityJson(const MusicalQualityReport& r) {
    using namespace musical_quality_detail;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"" << jsonEscape(r.schema) << "\",\n";
    o << "  \"styleName\": \"" << jsonEscape(r.style_name) << "\",\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"NOT_TESTED\",\n";
    o << "  \"independentFromFrozenQ1\": true,\n";
    o << "  \"statusLabel\": \"" << jsonEscape(r.status_label) << "\",\n";
    o << "  \"ppqn\": " << r.ppqn << ",\n";
    o << "  \"tempoBpm\": " << formatDouble(r.tempo_bpm) << ",\n";
    o << "  \"elementCount\": " << r.element_count << ",\n";
    o << "  \"overallMusicalScore\": " << formatDouble(r.overall_musical_score) << ",\n";

    o << "  \"dimensions\": [\n";
    for (size_t i = 0; i < r.dimensions.size(); ++i) {
        const auto& d = r.dimensions[i];
        o << "    {\n";
        o << "      \"id\": " << static_cast<unsigned>(d.id) << ",\n";
        o << "      \"key\": \"" << musicalDimensionKey(d.id) << "\",\n";
        o << "      \"name\": \"" << jsonEscape(d.name) << "\",\n";
        o << "      \"applicable\": " << (d.applicable ? "true" : "false") << ",\n";
        o << "      \"score\": " << formatDouble(d.score) << ",\n";
        o << "      \"status\": \"" << jsonEscape(d.status) << "\",\n";
        o << "      \"metrics\": {";
        size_t mi = 0;
        for (const auto& [k, v] : d.metrics) {
            if (mi++ > 0u) o << ",";
            o << " \"" << jsonEscape(k) << "\": " << formatDouble(v);
        }
        o << " }\n";
        o << "    }" << (i + 1u < r.dimensions.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"tracks\": [\n";
    for (size_t i = 0; i < r.tracks.size(); ++i) {
        const auto& t = r.tracks[i];
        o << "    {\n";
        o << "      \"korgCh\": " << static_cast<unsigned>(t.korg_ch) << ",\n";
        o << "      \"trackName\": \"" << jsonEscape(t.track_name) << "\",\n";
        o << "      \"inferredFamily\": \"" << jsonEscape(t.inferred_family) << "\",\n";
        o << "      \"noteCount\": " << t.note_count << ",\n";
        o << "      \"minPitch\": " << static_cast<unsigned>(t.min_pitch) << ",\n";
        o << "      \"maxPitch\": " << static_cast<unsigned>(t.max_pitch) << ",\n";
        o << "      \"meanVelocity\": " << formatDouble(t.mean_velocity) << ",\n";
        o << "      \"velStddev\": " << formatDouble(t.vel_stddev) << ",\n";
        o << "      \"velP5\": " << static_cast<unsigned>(t.vel_p5) << ",\n";
        o << "      \"velP95\": " << static_cast<unsigned>(t.vel_p95) << ",\n";
        o << "      \"crestFactor\": " << formatDouble(t.crest_factor) << ",\n";
        o << "      \"meanDutyRatio\": " << formatDouble(t.mean_duty_ratio) << ",\n";
        o << "      \"maxPolyphony\": " << t.max_polyphony << ",\n";
        o << "      \"mudClustersLtC3\": " << t.mud_clusters_lt_c3 << ",\n";
        o << "      \"cc7Tick0\": " << t.cc7_tick0 << ",\n";
        o << "      \"cc10Tick0\": " << t.cc10_tick0 << ",\n";
        o << "      \"cc91Tick0\": " << t.cc91_tick0 << ",\n";
        o << "      \"cc93Tick0\": " << t.cc93_tick0 << ",\n";
        o << "      \"flatVelocity\": " << (t.flat_velocity ? "true" : "false") << ",\n";
        o << "      \"soundResolved\": " << (t.sound_resolved ? "true" : "false") << "\n";
        o << "    }" << (i + 1u < r.tracks.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"variationCurve\": [\n";
    for (size_t i = 0; i < r.variation_curve.size(); ++i) {
        const auto& v = r.variation_curve[i];
        o << "    { \"marker\": \"" << jsonEscape(v.marker)
          << "\", \"bars\": " << v.bars
          << ", \"notesPerBar\": " << formatDouble(v.notes_per_bar)
          << ", \"meanVelocity\": " << formatDouble(v.mean_velocity)
          << ", \"activeTracks\": " << v.active_tracks
          << ", \"compositeEnergy\": " << formatDouble(v.composite_energy)
          << " }" << (i + 1u < r.variation_curve.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"findings\": [\n";
    for (size_t i = 0; i < r.all_findings.size(); ++i) {
        const auto& f = r.all_findings[i];
        o << "    {\n";
        o << "      \"dimension\": " << static_cast<unsigned>(f.dimension) << ",\n";
        o << "      \"severity\": \"" << musicalFindingSeverityName(f.severity) << "\",\n";
        o << "      \"code\": \"" << jsonEscape(f.code) << "\",\n";
        o << "      \"section\": \"" << jsonEscape(f.section_marker) << "\",\n";
        o << "      \"korgCh\": " << static_cast<unsigned>(f.korg_ch) << ",\n";
        o << "      \"bar\": " << f.bar << ",\n";
        o << "      \"tick\": " << f.tick << ",\n";
        o << "      \"summary\": \"" << jsonEscape(f.summary) << "\",\n";
        o << "      \"suggestedRule\": \"" << jsonEscape(f.suggested_rule) << "\"\n";
        o << "    }" << (i + 1u < r.all_findings.size() ? "," : "") << "\n";
    }
    o << "  ]\n";
    o << "}\n";
    return o.str();
}

inline std::string formatMusicalQualityMarkdown(const MusicalQualityReport& r) {
    using namespace musical_quality_detail;
    std::ostringstream o;
    o << "# Musical Quality Track-by-Track Report: " << mdEscape(r.style_name) << "\n\n";
    o << "- **Schema:** `" << r.schema << "`\n";
    o << "- **Overall Musical Score:** `" << formatDouble(r.overall_musical_score) << " / 100.00`\n";
    o << "- **Status:** `" << mdEscape(r.status_label) << "`\n";
    o << "- **Epistemic Boundary:** `hardwareVerified: false` | `l3Status: NOT_TESTED` | `independentFromFrozenQ1: true`\n";
    o << "- **Timing:** `PPQN=" << r.ppqn << "` | `Tempo=" << formatDouble(r.tempo_bpm)
      << " BPM` | `Elements=" << r.element_count << "`\n\n";

    o << "## 12 Arranger Musical Dimensions\n\n";
    o << "| # | Dimension | Applicable | Score | Status |\n";
    o << "|---:|---|---:|---:|---|\n";
    for (const auto& d : r.dimensions) {
        o << "| " << static_cast<unsigned>(d.id)
          << " | " << mdEscape(d.name)
          << " | " << (d.applicable ? "yes" : "no")
          << " | " << formatDouble(d.score)
          << " | " << d.status << " |\n";
    }

    if (!r.tracks.empty()) {
        o << "\n## Track-by-Track Summary\n\n";
        o << "| Ch | Track | Family | Notes | Range | Vel (mean +/- std) | Duty | Poly | Mud<C3 | CC7/10/91/93 |\n";
        o << "|---:|---|---|---:|---|---:|---:|---:|---:|---|\n";
        for (const auto& t : r.tracks) {
            o << "| " << static_cast<unsigned>(t.korg_ch + 1u)
              << " | " << mdEscape(t.track_name)
              << " | " << mdEscape(t.inferred_family)
              << " | " << t.note_count
              << " | " << static_cast<unsigned>(t.min_pitch) << ".." << static_cast<unsigned>(t.max_pitch)
              << " | " << formatDouble(t.mean_velocity) << " +/- " << formatDouble(t.vel_stddev)
              << " | " << formatDouble(t.mean_duty_ratio)
              << " | " << t.max_polyphony
              << " | " << t.mud_clusters_lt_c3
              << " | " << t.cc7_tick0 << "/" << t.cc10_tick0 << "/"
                       << t.cc91_tick0 << "/" << t.cc93_tick0 << " |\n";
        }
    }

    if (!r.variation_curve.empty()) {
        o << "\n## Variation Energy Curve\n\n";
        o << "| Marker | Bars | Notes/Bar | Mean Vel | Active Tracks | Composite Energy |\n";
        o << "|---|---:|---:|---:|---:|---:|\n";
        for (const auto& v : r.variation_curve) {
            o << "| `" << mdEscape(v.marker) << "` | " << v.bars
              << " | " << formatDouble(v.notes_per_bar)
              << " | " << formatDouble(v.mean_velocity)
              << " | " << v.active_tracks
              << " | " << formatDouble(v.composite_energy) << " |\n";
        }
    }

    if (!r.all_findings.empty()) {
        o << "\n## Findings & Opt-In Mode B Advisories\n\n";
        o << "| Severity | Code | Section | Summary | Suggested Mode B Rule |\n";
        o << "|---|---|---|---|---|\n";
        for (const auto& f : r.all_findings) {
            o << "| " << musicalFindingSeverityName(f.severity)
              << " | `" << mdEscape(f.code) << "`"
              << " | `" << mdEscape(f.section_marker) << "`"
              << " | " << mdEscape(f.summary)
              << " | " << (f.suggested_rule.empty() ? "-" : ("`" + mdEscape(f.suggested_rule) + "`"))
              << " |\n";
        }
    }
    return o.str();
}

inline std::string formatMusicalQualityMd(const MusicalQualityReport& r) {
    return formatMusicalQualityMarkdown(r);
}

} // namespace ai_arranger::exporters::korg::quality

#endif // AI_ARRANGER_EXPORTERS_KORG_MUSICAL_QUALITY_ANALYZER_H
