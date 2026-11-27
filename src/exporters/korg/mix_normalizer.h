#ifndef AI_ARRANGER_EXPORTERS_KORG_MIX_NORMALIZER_H
#define AI_ARRANGER_EXPORTERS_KORG_MIX_NORMALIZER_H

#include "exporters/korg/korg_style_smf.h"
#include "importers/sff1/xg_sysex_parser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Standalone, portable C++20 Korg Pa5X style element mix/CC hygiene,
// loop-boundary safety reset, XG SysEx tick-0 CC synthesis, effective loudness
// headroom normalizer, and Markdown Pa5X Style Edit Parameter Sheet formatter.
// Zero JUCE/GUI dependency; 100% offline, deterministic, hardwareVerified=false.

namespace ai_arranger::exporters::korg::quality {

struct MixNormalizeOptions {
    bool filter_proprietary_ccs = true;  // strip NRPN CC98/99 (+ paired CC6/38), CC94 (blend 30% into CC93 if absent)
    bool enforce_loop_resets    = true;  // emit CC64=0 and PitchBend=8192 at len-1 if left open/bent
    bool normalize_headroom     = true;  // keep effective loudness proxy in [0.20, 0.92] on melodic/bass tracks
    bool inject_xg_mix_ccs      = true;  // synthesize missing tick-0 CC7/10/91/93 from XgPartMixParams
};

struct MixNormalizeStats {
    uint32_t proprietary_ccs_stripped = 0; // NRPN 98/99 (+ paired 6/38/96/97), stray CC94, invalid >=128
    uint32_t sustain_resets_added     = 0; // CC64=0 inserted at len-1
    uint32_t pitchbend_resets_added   = 0; // 0xE0 0x00 0x40 inserted at len-1
    uint32_t tracks_gain_adjusted     = 0; // melodic/bass tracks boosted or attenuated
    uint32_t xg_params_translated     = 0; // tick-0 mix CCs synthesized from XG SysEx or CC94->CC93 blend
    uint32_t expression_resets_added  = 0; // CC11 restored at len-1 if element faded below CC11 < 32
};

// Lightweight POD summary views for formatPa5xStyleEditSheetMd so Q4/Q5 wiring
// adapts without header coupling across independent milestones.
struct StyleEditSheetTrackView {
    uint8_t               korg_ch               = 0xFF;
    std::string           track_name;
    std::string           pa5x_ntt_type         = "Parallel";
    std::string           pa5x_ntt_table        = "Root";
    bool                  bass_inversion        = false;
    uint8_t               wrap_around           = 6;
    uint8_t               key_range_low         = 0;
    uint8_t               key_range_high        = 127;
    std::string           retrigger_mode        = "Retrigger";
    int8_t                octave_shift_applied  = 0;
    bool                  guitar_mode_candidate = false;
    std::optional<int8_t> eq_low_gain_db;
    std::optional<int8_t> eq_mid_gain_db;
    std::optional<int8_t> eq_high_gain_db;
    std::string           merged_warning;
    std::string           zone_advisory;
};

struct StyleEditSheetCvEntry {
    std::string marker;               // e.g. "v1cv1", "v1cv2", "br"
    std::string source_section;       // e.g. "Main A"
    uint32_t    bars = 0;
    uint8_t     meter_num = 4;
    uint8_t     meter_den = 4;
    uint8_t     cv_index = 1;         // 0 = Break, 1..6 = CV1..CV6
    std::string target_key_chord;     // e.g. "C Maj", "C Min", "C 7", "C dim", "C sus4", "C 5"
    std::string chord_table_families; // e.g. "Maj, Maj6, Maj7, Maj(9), Aug"
};

struct StyleEditSheetElementNttOverride {
    std::string marker;
    uint8_t     korg_ch = 0xFF;
    std::string track_name;
    std::string pa5x_ntt_type;
    std::string pa5x_ntt_table;
    bool        bass_inversion = false;
    uint8_t     wrap_around = 6;
    uint8_t     key_range_low = 0;
    uint8_t     key_range_high = 127;
    std::string retrigger_mode;
};

struct StyleEditSheetSoundView {
    uint8_t     korg_ch       = 0xFF;
    std::string track_name;
    std::string inferred_family;
    std::string selected_name;        // empty => UNRESOLVED
    int         msb           = -1;   // -1 => unresolved
    int         lsb           = -1;
    int         pc            = -1;
    uint8_t     score         = 0;
    std::string origin        = "Unresolved";
    std::string epistemic_tag = "DOCUMENTED_NOT_HW_VERIFIED";
    std::string rationale;
};

namespace mix_detail {

inline const char* mixTrackLabel(uint8_t korg_ch) noexcept {
    switch (korg_ch) {
        case kBassCh:     return "Bass";
        case kDrumCh:     return "Drum";
        case kPercCh:     return "Perc";
        case kAcc1Ch:     return "Acc1";
        case kAcc1Ch + 1: return "Acc2";
        case kAcc1Ch + 2: return "Acc3";
        case kAcc1Ch + 3: return "Acc4";
        case kAcc1Ch + 4: return "Acc5";
        default:          return "Other";
    }
}

inline bool isTick0VoiceSelect(const Event& ev) noexcept {
    if (ev.tick != 0) return false;
    const uint8_t hi = ev.status & 0xF0u;
    if (hi == 0xC0u) return true;
    return hi == 0xB0u && (ev.d1 == 0u || ev.d1 == 32u);
}

// Inserts a vector of tick-0 CCs immediately after any tick-0 Bank/PC events
// and before any tick-0 NoteOns, keeping el.events sorted by tick.
inline void insertTick0Events(std::vector<Event>& events, const std::vector<Event>& to_insert) {
    if (to_insert.empty()) return;
    size_t pos = 0;
    while (pos < events.size() && events[pos].tick == 0 && isTick0VoiceSelect(events[pos])) {
        ++pos;
    }
    events.insert(events.begin() + static_cast<std::ptrdiff_t>(pos),
                  to_insert.begin(), to_insert.end());
}

// Updates the last tick-0 CC `cc_num` on `ch` in place, or inserts one if absent.
inline void upsertTick0Cc(std::vector<Event>& events, uint8_t ch, uint8_t cc_num, uint8_t val) {
    const uint8_t clamped = std::min<uint8_t>(val, 127u);
    size_t last_idx = events.size();
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].tick > 0) break;
        const uint8_t hi = events[i].status & 0xF0u;
        const uint8_t ech = events[i].status & 0x0Fu;
        if (hi == 0xB0u && ech == ch && events[i].d1 == cc_num) {
            last_idx = i;
        }
    }
    if (last_idx < events.size()) {
        events[last_idx].d2 = clamped;
    } else {
        std::vector<Event> one;
        one.push_back(Event{0u, static_cast<uint8_t>(0xB0u | (ch & 0x0Fu)), cc_num, clamped});
        insertTick0Events(events, one);
    }
}

inline std::string formatHex16(uint16_t v) {
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << v;
    return oss.str();
}

inline std::string formatOptDb(const std::optional<int8_t>& v) {
    if (!v.has_value()) return "-";
    const int x = static_cast<int>(*v);
    return (x > 0 ? "+" : "") + std::to_string(x) + " dB";
}

inline std::string escapeMarkdownCell(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\r' || c == '\n') {
            out.push_back(' ');
        } else if (c == '|') {
            out += "\\|";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// True for one-shot Pa5X Ending markers ("e1cv1".."e3cv6", "e1".."e3").
// Endings terminate playback instead of looping/chaining into a Variation, so
// intentional trailing CC11 fade-outs (< 32) must NOT snap back at el.length - 1.
inline bool isOneShotEndingMarker(std::string_view marker) noexcept {
    if (marker.size() < 2u) return false;
    const char c0 = marker[0];
    const char c1 = marker[1];
    return (c0 == 'e' || c0 == 'E') && (c1 >= '1' && c1 <= '3');
}

} // namespace mix_detail

// Cleans proprietary/invalid CCs, injects missing tick-0 XG mix CCs, balances
// melodic/bass effective loudness proxy headroom into [0.20, 0.92], and appends
// loop-boundary CC64=0 / PitchBend=8192 resets at el.length - 1.
inline MixNormalizeStats normalizeElementMix(
    Element& el,
    const std::map<uint8_t, importers::sff1::XgPartMixParams>& korg_ch_xg_params,
    const MixNormalizeOptions& opt = {}) noexcept {

    MixNormalizeStats stats;

    try {
        // Ensure deterministic chronological order before stateful scanning.
        std::stable_sort(el.events.begin(), el.events.end(),
                         [](const Event& a, const Event& b) { return a.tick < b.tick; });

        // ── 1. CC Hygiene (M5.2 == 0) ────────────────────────────────────
        std::array<int, 16> cc94_val{};
        cc94_val.fill(-1);
        std::array<bool, 16> cc94_from_tick0{};
        std::array<bool, 16> has_any_cc93{};

        if (opt.filter_proprietary_ccs) {
            std::array<bool, 16> in_nrpn{};
            std::vector<Event> cleaned;
            cleaned.reserve(el.events.size());

            for (const auto& ev : el.events) {
                const uint8_t hi = ev.status & 0xF0u;
                const uint8_t ch = ev.status & 0x0Fu;

                if (hi == 0xB0u) {
                    // Fail-closed rejection of any non-7-bit CC byte (> 127).
                    if (ev.d1 > 127u || ev.d2 > 127u) {
                        ++stats.proprietary_ccs_stripped;
                        continue;
                    }
                    // Stateful NRPN filter: drop CC99/CC98 and subsequent Data Entry
                    // CC6/CC38 until RPN (CC100/CC101) or null-RPN (127/127) resets state.
                    if (ev.d1 == 99u || ev.d1 == 98u) {
                        in_nrpn[ch] = true;
                        ++stats.proprietary_ccs_stripped;
                        continue;
                    }
                    if (ev.d1 == 101u || ev.d1 == 100u) {
                        in_nrpn[ch] = false;
                        cleaned.push_back(ev);
                        continue;
                    }
                    if ((ev.d1 == 6u || ev.d1 == 38u || ev.d1 == 96u || ev.d1 == 97u) && in_nrpn[ch]) {
                        ++stats.proprietary_ccs_stripped;
                        continue;
                    }
                    // Drop XG Variation Send CC94; capture value for optional 30% CC93 blend.
                    if (ev.d1 == 94u) {
                        if (ev.tick == 0) {
                            cc94_val[ch] = static_cast<int>(ev.d2);
                            cc94_from_tick0[ch] = true;
                        } else if (!cc94_from_tick0[ch]) {
                            cc94_val[ch] = static_cast<int>(ev.d2);
                        }
                        ++stats.proprietary_ccs_stripped;
                        continue;
                    }
                    if (ev.d1 == 93u) {
                        has_any_cc93[ch] = true;
                    }
                }
                cleaned.push_back(ev);
            }
            el.events = std::move(cleaned);
        }

        // Guard el.length == 0: no valid tick exists for injecting or appending events.
        if (el.length == 0) {
            return stats;
        }

        // Track which tick-0 mix CCs already exist on each channel.
        std::array<bool, 16> has_t0_cc7{};
        std::array<bool, 16> has_t0_cc10{};
        std::array<bool, 16> has_t0_cc91{};
        std::array<bool, 16> has_t0_cc93{};
        for (const auto& ev : el.events) {
            if (ev.tick > 0) break;
            if ((ev.status & 0xF0u) != 0xB0u) continue;
            const uint8_t ch = ev.status & 0x0Fu;
            if (ev.d1 == 7u)       has_t0_cc7[ch] = true;
            else if (ev.d1 == 10u) has_t0_cc10[ch] = true;
            else if (ev.d1 == 91u) has_t0_cc91[ch] = true;
            else if (ev.d1 == 93u) has_t0_cc93[ch] = true;
        }

        // ── 2. XG SysEx -> Tick-0 Mix CC Injection & CC94 -> CC93 Blend ──
        std::vector<Event> tick0_synth;
        if (opt.inject_xg_mix_ccs) {
            for (const auto& [ch, params] : korg_ch_xg_params) {
                if (ch < kBassCh || ch > 15u) continue;
                const uint8_t cc_status = static_cast<uint8_t>(0xB0u | ch);
                if (params.volume.has_value() && !has_t0_cc7[ch]) {
                    tick0_synth.push_back(Event{0u, cc_status, 7u, std::min<uint8_t>(*params.volume, 127u)});
                    has_t0_cc7[ch] = true;
                    ++stats.xg_params_translated;
                }
                if (params.pan.has_value() && !has_t0_cc10[ch]) {
                    tick0_synth.push_back(Event{0u, cc_status, 10u, std::min<uint8_t>(*params.pan, 127u)});
                    has_t0_cc10[ch] = true;
                    ++stats.xg_params_translated;
                }
                if (params.reverb_send.has_value() && !has_t0_cc91[ch]) {
                    tick0_synth.push_back(Event{0u, cc_status, 91u, std::min<uint8_t>(*params.reverb_send, 127u)});
                    has_t0_cc91[ch] = true;
                    ++stats.xg_params_translated;
                }
                if (params.chorus_send.has_value() && !has_t0_cc93[ch]) {
                    tick0_synth.push_back(Event{0u, cc_status, 93u, std::min<uint8_t>(*params.chorus_send, 127u)});
                    has_t0_cc93[ch] = true;
                    has_any_cc93[ch] = true;
                    ++stats.xg_params_translated;
                } else if (params.variation_send.has_value() && *params.variation_send > 0u &&
                           !has_any_cc93[ch] && !has_t0_cc93[ch] && cc94_val[ch] <= 0) {
                    const uint8_t blended = static_cast<uint8_t>(
                        std::min<int>(127, (static_cast<int>(*params.variation_send) * 30) / 100));
                    if (blended > 0u) {
                        tick0_synth.push_back(Event{0u, cc_status, 93u, blended});
                        has_t0_cc93[ch] = true;
                        has_any_cc93[ch] = true;
                        ++stats.xg_params_translated;
                    }
                }
            }
        }

        if (opt.filter_proprietary_ccs) {
            for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                if (cc94_val[ch] > 0 && !has_any_cc93[ch] && !has_t0_cc93[ch]) {
                    const uint8_t blended = static_cast<uint8_t>(
                        std::min<int>(127, (cc94_val[ch] * 30) / 100));
                    if (blended > 0u) {
                        tick0_synth.push_back(Event{0u, static_cast<uint8_t>(0xB0u | ch), 93u, blended});
                        has_t0_cc93[ch] = true;
                        has_any_cc93[ch] = true;
                        ++stats.xg_params_translated;
                    }
                }
            }
        }

        mix_detail::insertTick0Events(el.events, tick0_synth);

        // ── 3. Effective Loudness Proxy Headroom (M5.3 >= 95%) ───────────
        // Applies ONLY to active chord-driven melodic/bass channels (NEVER Drum/Perc!).
        // P1-3: Evaluate effective CC11 across active sounding note spans (peak CC11
        // during sounding notes) rather than tick-0-only CC11 so EDM sidechain-ducked
        // pads (e.g. tick-0 CC11=24 -> offbeat CC11=116) are NEVER falsely over-boosted!
        if (opt.normalize_headroom) {
            constexpr double kMinHealthyL = 0.20;
            constexpr double kTargetLowL  = 0.24;
            constexpr double kMaxHealthyL = 0.92;
            constexpr double kTargetHighL = 0.86;

            for (uint8_t ch = kBassCh; ch <= 15u; ++ch) {
                if (!isChordDriven(ch)) continue;

                uint8_t cc7 = 100u;
                uint8_t tick0_cc11 = 127u;
                uint8_t cur_cc11 = 127u;
                bool has_mid_cc11 = false;
                uint8_t max_active_cc11 = 0u;
                bool sampled_active_cc11 = false;
                int active_notes = 0;
                double sum_sq = 0.0;
                uint32_t note_ct = 0;

                for (const auto& ev : el.events) {
                    if ((ev.status & 0x0Fu) != ch) continue;
                    const uint8_t hi = ev.status & 0xF0u;
                    if (hi == 0xB0u) {
                        if (ev.tick == 0u && ev.d1 == 7u) {
                            cc7 = std::min<uint8_t>(ev.d2, 127u);
                        } else if (ev.d1 == 11u) {
                            const uint8_t val = std::min<uint8_t>(ev.d2, 127u);
                            cur_cc11 = val;
                            if (ev.tick == 0u) {
                                tick0_cc11 = val;
                            } else {
                                has_mid_cc11 = true;
                            }
                            if (active_notes > 0) {
                                if (!sampled_active_cc11 || val > max_active_cc11) {
                                    max_active_cc11 = val;
                                    sampled_active_cc11 = true;
                                }
                            }
                        }
                    } else if (hi == 0x90u && ev.d2 > 0u) {
                        ++active_notes;
                        if (!sampled_active_cc11 || cur_cc11 > max_active_cc11) {
                            max_active_cc11 = cur_cc11;
                            sampled_active_cc11 = true;
                        }
                        const double v = static_cast<double>(std::min<uint8_t>(ev.d2, 127u));
                        sum_sq += v * v;
                        ++note_ct;
                    } else if (hi == 0x80u || (hi == 0x90u && ev.d2 == 0u)) {
                        if (active_notes > 0) --active_notes;
                    }
                }
                if (note_ct == 0) continue;

                uint8_t eff_cc11 = sampled_active_cc11 ? max_active_cc11 : tick0_cc11;
                const double rms_vel = std::sqrt(sum_sq / static_cast<double>(note_ct));
                const double loudness = (static_cast<double>(cc7) / 127.0) *
                                        (static_cast<double>(eff_cc11) / 127.0) *
                                        (rms_vel / 127.0);

                if (loudness < kMinHealthyL) {
                    // Guard against silent CC11=0 across all active notes which blocks any CC7/vel boost.
                    if (eff_cc11 == 0u) {
                        eff_cc11 = 100u;
                        mix_detail::upsertTick0Cc(el.events, ch, 11u, eff_cc11);
                    }

                    const double denom_no_cc7 = (static_cast<double>(eff_cc11) / 127.0) *
                                                (rms_vel / 127.0);
                    uint8_t new_cc7 = cc7;
                    if (cc7 < 120u) {
                        int needed_cc7 = 120;
                        if (denom_no_cc7 > 1e-9) {
                            needed_cc7 = static_cast<int>(
                                std::ceil(kTargetLowL * 127.0 / denom_no_cc7));
                        }
                        new_cc7 = static_cast<uint8_t>(
                            std::clamp(needed_cc7, static_cast<int>(cc7), 120));
                        mix_detail::upsertTick0Cc(el.events, ch, 7u, new_cc7);
                    }

                    double after_cc7_l = (static_cast<double>(new_cc7) / 127.0) * denom_no_cc7;
                    if (after_cc7_l < kTargetLowL) {
                        // Gently scale note velocities proportionally, strictly clamped [1..127].
                        const double scale = (kTargetLowL + 0.005) / std::max(1e-9, after_cc7_l);
                        double new_sum_sq = 0.0;
                        for (auto& ev : el.events) {
                            if ((ev.status & 0x0Fu) != ch) continue;
                            if ((ev.status & 0xF0u) == 0x90u && ev.d2 > 0u) {
                                const int scaled = static_cast<int>(
                                    std::lround(static_cast<double>(ev.d2) * scale));
                                ev.d2 = static_cast<uint8_t>(std::clamp(scaled, 1, 127));
                                const double v = static_cast<double>(ev.d2);
                                new_sum_sq += v * v;
                            }
                        }
                        const double new_rms = std::sqrt(new_sum_sq / static_cast<double>(note_ct));
                        double after_vel_l = (static_cast<double>(new_cc7) / 127.0) *
                                             (static_cast<double>(eff_cc11) / 127.0) *
                                             (new_rms / 127.0);
                        // If tiny CC11 still kept L below 0.24 even at max vel, lift CC7 (and CC11 if needed).
                        if (after_vel_l < kTargetLowL && new_cc7 < 127u) {
                            const double f = (static_cast<double>(eff_cc11) / 127.0) * (new_rms / 127.0);
                            const int lift_cc7 = (f > 1e-9)
                                ? static_cast<int>(std::ceil(kTargetLowL * 127.0 / f))
                                : 127;
                            new_cc7 = static_cast<uint8_t>(std::clamp(lift_cc7, static_cast<int>(new_cc7), 127));
                            mix_detail::upsertTick0Cc(el.events, ch, 7u, new_cc7);
                            after_vel_l = (static_cast<double>(new_cc7) / 127.0) * f;
                        }
                        if (after_vel_l < kTargetLowL && eff_cc11 < 127u) {
                            const double f = (static_cast<double>(new_cc7) / 127.0) * (new_rms / 127.0);
                            const int lift_cc11 = (f > 1e-9)
                                ? static_cast<int>(std::ceil(kTargetLowL * 127.0 / f))
                                : 127;
                            const uint8_t target_cc11 = static_cast<uint8_t>(
                                std::clamp(lift_cc11, static_cast<int>(eff_cc11), 127));
                            if (!has_mid_cc11) {
                                mix_detail::upsertTick0Cc(el.events, ch, 11u, target_cc11);
                            } else if (eff_cc11 > 0u) {
                                const double cc11_scale =
                                    static_cast<double>(target_cc11) / static_cast<double>(eff_cc11);
                                for (auto& ev : el.events) {
                                    if ((ev.status & 0x0Fu) == ch && (ev.status & 0xF0u) == 0xB0u && ev.d1 == 11u) {
                                        const int sv = static_cast<int>(
                                            std::lround(static_cast<double>(ev.d2) * cc11_scale));
                                        ev.d2 = static_cast<uint8_t>(std::clamp(sv, 1, 127));
                                    }
                                }
                            }
                        }
                    }
                    ++stats.tracks_gain_adjusted;
                } else if (loudness > kMaxHealthyL) {
                    const double denom_no_cc7 = (static_cast<double>(eff_cc11) / 127.0) *
                                                (rms_vel / 127.0);
                    const int desired_cc7 = (denom_no_cc7 > 1e-9)
                        ? static_cast<int>(std::floor(kTargetHighL * 127.0 / denom_no_cc7))
                        : 100;
                    const uint8_t new_cc7 = static_cast<uint8_t>(std::clamp(desired_cc7, 1, 127));
                    mix_detail::upsertTick0Cc(el.events, ch, 7u, new_cc7);
                    ++stats.tracks_gain_adjusted;
                }
            }
        }

        // ── 4. Loop-Boundary Safety (M3.3 == 0 & CC11 Fade-Out Reset) ────
        if (opt.enforce_loop_resets) {
            std::array<uint8_t, 16>  last_cc64{};
            std::array<uint16_t, 16> last_pb{};
            last_pb.fill(8192u);
            std::array<bool, 16>     had_t0_cc11{};
            std::array<uint8_t, 16>  t0_cc11{};
            t0_cc11.fill(127u);
            std::array<bool, 16>     saw_mid_cc11{};
            std::array<uint8_t, 16>  last_cc11{};
            last_cc11.fill(127u);
            std::array<uint8_t, 16>  max_cc11{};

            for (const auto& ev : el.events) {
                const uint8_t hi = ev.status & 0xF0u;
                const uint8_t ch = ev.status & 0x0Fu;
                if (hi == 0xB0u && ev.d1 == 64u) {
                    last_cc64[ch] = ev.d2;
                } else if (hi == 0xB0u && ev.d1 == 11u) {
                    const uint8_t v = std::min<uint8_t>(ev.d2, 127u);
                    if (ev.tick == 0u) {
                        had_t0_cc11[ch] = true;
                        t0_cc11[ch] = v;
                    } else {
                        saw_mid_cc11[ch] = true;
                    }
                    last_cc11[ch] = v;
                    if (v > max_cc11[ch]) max_cc11[ch] = v;
                } else if (hi == 0xE0u) {
                    last_pb[ch] = static_cast<uint16_t>(
                        static_cast<uint16_t>(ev.d1 & 0x7Fu) |
                        (static_cast<uint16_t>(ev.d2 & 0x7Fu) << 7));
                }
            }

            const uint32_t reset_tick = el.length - 1u;
            const bool is_one_shot_ending = mix_detail::isOneShotEndingMarker(el.marker);
            for (uint8_t ch = 0; ch < 16u; ++ch) {
                if (last_cc64[ch] >= 64u) {
                    el.events.push_back(Event{reset_tick, static_cast<uint8_t>(0xB0u | ch), 64u, 0u});
                    ++stats.sustain_resets_added;
                }
                if (last_pb[ch] != 8192u) {
                    el.events.push_back(Event{reset_tick, static_cast<uint8_t>(0xE0u | ch), 0x00u, 0x40u});
                    ++stats.pitchbend_resets_added;
                }
                if (!is_one_shot_ending && saw_mid_cc11[ch] && last_cc11[ch] < 32u) {
                    uint8_t restore = 127u;
                    if (had_t0_cc11[ch] && t0_cc11[ch] >= 32u) {
                        restore = t0_cc11[ch];
                    } else if (!had_t0_cc11[ch]) {
                        restore = 127u;
                    } else {
                        restore = std::min<uint8_t>(std::max<uint8_t>(max_cc11[ch], 100u), 127u);
                    }
                    el.events.push_back(Event{reset_tick, static_cast<uint8_t>(0xB0u | ch), 11u, restore});
                    ++stats.expression_resets_added;
                }
            }
        }

        // ── 5. Final Stable Sort by Tick ─────────────────────────────────
        std::stable_sort(el.events.begin(), el.events.end(),
                         [](const Event& a, const Event& b) { return a.tick < b.tick; });
    } catch (...) {
        return stats;
    }

    return stats;
}

// Emits a deterministic Markdown Pa5X Style Edit Parameter Sheet covering
// Section 0 (Import SMF + CV1..CV6 Key/Chord & Chord Table setup), NTT,
// Bass Inversion, Wrap Around, LimitKeyRange, Retrigger, Guitar Mode,
// per-element NTT overrides, 3-Band EQ, Sound matches, and XG SysEx FX/Mix/Drum
// advisories. Always stamps hardwareVerified: false and L3 = NOT_TESTED.
inline std::string formatPa5xStyleEditSheetMd(
    const std::vector<StyleEditSheetTrackView>& track_advice,
    const importers::sff1::XgSysExSummary& xg,
    const std::vector<StyleEditSheetSoundView>& sound_matches,
    const std::string& style_name = "",
    const std::vector<StyleEditSheetCvEntry>& cv_entries = {},
    const std::vector<StyleEditSheetElementNttOverride>& element_ntt_overrides = {},
    uint16_t ppqn = 0,
    uint32_t tempo_bpm = 0) noexcept {
    try {
        std::ostringstream out;
        out << "# Korg Pa5X Style Edit Parameter Sheet";
        if (!style_name.empty()) {
            out << " — " << mix_detail::escapeMarkdownCell(style_name);
        }
        out << "\n\n";
        out << "> **Status:** `hardwareVerified: false` | `L3 = NOT_TESTED` — "
               "STRUCTURALLY VALID, NOT VERIFIED ON A PA5X HARDWARE UNIT.\n\n";

        // 0. Pa5X Import SMF & CV1..CV6 Chord Table / Key-Chord Setup (P1-W1)
        out << "## 0. Pa5X Import SMF & CV1..CV6 Chord Table / Key-Chord Setup\n\n";
        out << "1. On the Pa5X, open **Style Edit > Menu > Import/Export > Import SMF** (MAN5 pp.1035-1040).\n";
        out << "2. Check **Initialize** (for a fresh Style) and press **SHIFT + Execute** to import every marker element in a single pass.\n";
        if (ppqn > 0 || tempo_bpm > 0) {
            out << "3. **Imported Grid / Tempo:** ";
            if (ppqn > 0) out << "PPQN = `" << ppqn << "`";
            if (ppqn > 0 && tempo_bpm > 0) out << " | ";
            if (tempo_bpm > 0) out << "Tempo = `" << tempo_bpm << " BPM`";
            out << "\n";
        }
        out << "\n### Standard CV1..CV6 Recording Key/Chord Convention\n\n";
        out << "| CV Slot | Example Marker | Pa5X Element Key/Chord | Recommended Pa5X Chord Table Families |\n";
        out << "|---|---|---|---|\n";
        out << "| CV1 | `v1cv1` | `C Maj` | Maj, Maj6, Maj7, Maj7#11, Maj(9), Maj7(9), Maj6(9), Aug, Maj7aug |\n";
        out << "| CV2 | `v1cv2` | `C Min` | Min, Min6, Min7, Min7b5, Min(9), Min7(9), Min7(11), MinMaj7, MinMaj7(9) |\n";
        out << "| CV3 | `v1cv3` | `C 7` | 7, 7sus4, 7b5, 7(9), 7(#11), 7(13), 7(b9), 7(b13), 7(#9), 7aug |\n";
        out << "| CV4 | `v1cv4` | `C dim` | Dim, Dim7 |\n";
        out << "| CV5 | `v1cv5` | `C sus4` | Sus4, Sus2 |\n";
        out << "| CV6 | `v1cv6` | `C 5` | 1+5, 1+8 (Power / Alt) |\n\n";

        if (!cv_entries.empty()) {
            bool has_multi_cv = false;
            out << "### Exported Elements & Per-CV Key/Chord Setup\n\n";
            out << "| Marker | Source Section | Bars | Meter | Key/Chord | Chord Table Families |\n";
            out << "|---|---|---:|---|---|---|\n";
            for (const auto& cv : cv_entries) {
                if (cv.cv_index >= 2) has_multi_cv = true;
                out << "| `" << mix_detail::escapeMarkdownCell(cv.marker) << "`"
                    << " | " << mix_detail::escapeMarkdownCell(cv.source_section)
                    << " | " << cv.bars
                    << " | " << static_cast<int>(cv.meter_num) << "/" << static_cast<int>(cv.meter_den)
                    << " | " << mix_detail::escapeMarkdownCell(cv.target_key_chord.empty() ? "C Maj" : cv.target_key_chord)
                    << " | " << mix_detail::escapeMarkdownCell(cv.chord_table_families.empty() ? "-" : cv.chord_table_families)
                    << " |\n";
            }
            out << "\n";
            if (has_multi_cv) {
                out << "> **Multi-CV Operator Action Required:** SMF Format 0 cannot encode Pa5X's XML Chord Table. "
                       "After `Import SMF (Initialize + SHIFT + Execute)`, set each `cv2..cv6` element's **Key/Chord** "
                       "per the table above and bind matching chord qualities in **Style Edit > Element Controls > Chord Table**.\n\n";
            }
        }

        // 1. Track NTT, Register, Bass Inversion & EQ Advice
        out << "## 1. Track NTT, Register, Bass Inversion & EQ Parameters\n\n";
        if (track_advice.empty()) {
            out << "_No per-track Style Edit parameter overrides recorded._\n\n";
        } else {
            out << "| Korg Ch | Track | NTT Type | NTT Table | Bass Inversion | Wrap Around | Key Range | Retrigger | Oct Shift | Guitar Mode | EQ Lo / Mid / Hi |\n";
            out << "|---:|---|---|---|---|---:|---|---|---:|---|---|\n";
            for (const auto& t : track_advice) {
                const std::string label = t.track_name.empty()
                    ? std::string(mix_detail::mixTrackLabel(t.korg_ch))
                    : mix_detail::escapeMarkdownCell(t.track_name);
                out << "| " << (t.korg_ch <= 15u ? std::to_string(static_cast<int>(t.korg_ch) + 1) : "-")
                    << " | " << label
                    << " | " << mix_detail::escapeMarkdownCell(t.pa5x_ntt_type)
                    << " | " << mix_detail::escapeMarkdownCell(t.pa5x_ntt_table)
                    << " | " << (t.bass_inversion ? "On" : "Off")
                    << " | " << static_cast<int>(t.wrap_around)
                    << " | " << static_cast<int>(t.key_range_low) << ".." << static_cast<int>(t.key_range_high)
                    << " | " << mix_detail::escapeMarkdownCell(t.retrigger_mode)
                    << " | " << (t.octave_shift_applied > 0 ? "+" : "") << static_cast<int>(t.octave_shift_applied)
                    << " | " << (t.guitar_mode_candidate ? "Candidate" : "No")
                    << " | " << mix_detail::formatOptDb(t.eq_low_gain_db)
                    << " / " << mix_detail::formatOptDb(t.eq_mid_gain_db)
                    << " / " << mix_detail::formatOptDb(t.eq_high_gain_db)
                    << " |\n";
            }
            out << "\n";

            bool any_note = false;
            for (const auto& t : track_advice) {
                if (!t.merged_warning.empty()) {
                    if (!any_note) { out << "### Track Merge & Multi-Zone Advisories\n\n"; any_note = true; }
                    out << "- **" << (t.track_name.empty() ? std::string(mix_detail::mixTrackLabel(t.korg_ch)) : mix_detail::escapeMarkdownCell(t.track_name))
                        << " (Merged Track):** " << mix_detail::escapeMarkdownCell(t.merged_warning) << "\n";
                }
                if (!t.zone_advisory.empty()) {
                    if (!any_note) { out << "### Track Merge & Multi-Zone Advisories\n\n"; any_note = true; }
                    out << "- **" << (t.track_name.empty() ? std::string(mix_detail::mixTrackLabel(t.korg_ch)) : mix_detail::escapeMarkdownCell(t.track_name))
                        << " (SFF2 Multi-Zone):** " << mix_detail::escapeMarkdownCell(t.zone_advisory) << "\n";
                }
            }
            if (any_note) out << "\n";
        }

        if (!element_ntt_overrides.empty()) {
            out << "### Per-Element NTT / Wrap / Key-Range Overrides\n\n";
            out << "| Marker | Korg Ch | Track | NTT Type | NTT Table | Bass Inversion | Wrap Around | Key Range | Retrigger |\n";
            out << "|---|---:|---|---|---|---|---:|---|---|\n";
            for (const auto& o : element_ntt_overrides) {
                const std::string label = o.track_name.empty()
                    ? std::string(mix_detail::mixTrackLabel(o.korg_ch))
                    : mix_detail::escapeMarkdownCell(o.track_name);
                out << "| `" << mix_detail::escapeMarkdownCell(o.marker) << "`"
                    << " | " << (o.korg_ch <= 15u ? std::to_string(static_cast<int>(o.korg_ch) + 1) : "-")
                    << " | " << label
                    << " | " << mix_detail::escapeMarkdownCell(o.pa5x_ntt_type)
                    << " | " << mix_detail::escapeMarkdownCell(o.pa5x_ntt_table)
                    << " | " << (o.bass_inversion ? "On" : "Off")
                    << " | " << static_cast<int>(o.wrap_around)
                    << " | " << static_cast<int>(o.key_range_low) << ".." << static_cast<int>(o.key_range_high)
                    << " | " << mix_detail::escapeMarkdownCell(o.retrigger_mode)
                    << " |\n";
            }
            out << "\n";
        }

        // 2. Sound Selection & Epistemic Status
        out << "## 2. Track Sound Assignments\n\n";
        if (sound_matches.empty()) {
            out << "_No sound matching entries provided (tracks default to UNRESOLVED)._\n\n";
        } else {
            out << "| Korg Ch | Track | Inferred Family | Selected Sound | Bank/PC | Score | Origin | Epistemic |\n";
            out << "|---:|---|---|---|---|---:|---|---|\n";
            for (const auto& s : sound_matches) {
                const std::string label = s.track_name.empty()
                    ? std::string(mix_detail::mixTrackLabel(s.korg_ch))
                    : mix_detail::escapeMarkdownCell(s.track_name);
                const std::string sound_col = s.selected_name.empty()
                    ? "UNRESOLVED"
                    : mix_detail::escapeMarkdownCell(s.selected_name);
                const std::string bank_pc = (s.msb >= 0 && s.lsb >= 0 && s.pc >= 0)
                    ? (std::to_string(s.msb) + "/" + std::to_string(s.lsb) + "/" + std::to_string(s.pc))
                    : "UNRESOLVED";
                out << "| " << (s.korg_ch <= 15u ? std::to_string(static_cast<int>(s.korg_ch) + 1) : "-")
                    << " | " << label
                    << " | " << (s.inferred_family.empty() ? "Unknown" : mix_detail::escapeMarkdownCell(s.inferred_family))
                    << " | " << sound_col
                    << " | " << bank_pc
                    << " | " << static_cast<int>(s.score)
                    << " | " << (s.origin.empty() ? "Unresolved" : mix_detail::escapeMarkdownCell(s.origin))
                    << " | " << (s.epistemic_tag.empty() ? "DOCUMENTED_NOT_HW_VERIFIED" : mix_detail::escapeMarkdownCell(s.epistemic_tag))
                    << " |\n";
            }
            out << "\n";
        }

        // 3. XG SysEx Mix, FX & Drum Key Override Advisory
        out << "## 3. XG SysEx Mix, FX & Drum Key Advisory\n\n";
        out << "- **SysEx Summary:** seen=" << xg.total_sysex_seen
            << ", recognized=" << xg.recognized_xg_count
            << ", unrecognized=" << xg.unrecognized_count
            << ", malformed=" << xg.malformed_count
            << ", dsp_sysex=" << xg.dsp_sysex_count << "\n";
        out << "- **Master Volume:** "
            << (xg.master_volume.has_value() ? std::to_string(*xg.master_volume) : "not set") << "\n";
        out << "- **Effect Types:** Reverb="
            << (xg.reverb_type.has_value() ? mix_detail::formatHex16(*xg.reverb_type) : "none")
            << ", Chorus="
            << (xg.chorus_type.has_value() ? mix_detail::formatHex16(*xg.chorus_type) : "none")
            << ", Variation="
            << (xg.variation_type.has_value() ? mix_detail::formatHex16(*xg.variation_type) : "none")
            << "\n";
        if (!xg.advisory_codes.empty()) {
            out << "- **Advisory Codes:** ";
            for (size_t i = 0; i < xg.advisory_codes.size(); ++i) {
                if (i > 0) out << ", ";
                out << "`" << mix_detail::escapeMarkdownCell(xg.advisory_codes[i]) << "`";
            }
            out << "\n";
        }
        out << "\n";

        if (!xg.parts.empty()) {
            out << "### XG Multi-Part Mix & EQ Parameters\n\n";
            out << "| XG Part | Vol | Pan | Dry | Rev | Cho | Var | EQ Bass | EQ Treble |\n";
            out << "|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
            auto optU8 = [](const std::optional<uint8_t>& v) -> std::string {
                return v.has_value() ? std::to_string(*v) : "-";
            };
            for (const auto& [part, p] : xg.parts) {
                out << "| " << (static_cast<int>(part) + 1)
                    << " | " << optU8(p.volume)
                    << " | " << optU8(p.pan)
                    << " | " << optU8(p.dry_level)
                    << " | " << optU8(p.reverb_send)
                    << " | " << optU8(p.chorus_send)
                    << " | " << optU8(p.variation_send)
                    << " | " << mix_detail::formatOptDb(p.eq_low_gain_db)
                    << " | " << mix_detail::formatOptDb(p.eq_high_gain_db)
                    << " |\n";
            }
            out << "\n";
        }

        if (!xg.drum_overrides.empty()) {
            out << "### XG Drum Setup Key Overrides\n\n";
            out << "| Setup | Key | Pitch Coarse | Level | Pan | Reverb |\n";
            out << "|---:|---:|---:|---:|---:|---:|\n";
            auto optU8 = [](const std::optional<uint8_t>& v) -> std::string {
                return v.has_value() ? std::to_string(*v) : "-";
            };
            for (const auto& d : xg.drum_overrides) {
                const std::string pitch_str = d.pitch_coarse.has_value()
                    ? ((static_cast<int>(*d.pitch_coarse) > 0 ? "+" : "") +
                       std::to_string(static_cast<int>(*d.pitch_coarse)))
                    : "-";
                out << "| " << static_cast<int>(d.drum_setup)
                    << " | " << static_cast<int>(d.key)
                    << " | " << pitch_str
                    << " | " << optU8(d.level)
                    << " | " << optU8(d.pan)
                    << " | " << optU8(d.reverb_send)
                    << " |\n";
            }
            out << "\n";
        }

        return out.str();
    } catch (...) {
        return {};
    }
}

} // namespace ai_arranger::exporters::korg::quality

#endif // AI_ARRANGER_EXPORTERS_KORG_MIX_NORMALIZER_H
