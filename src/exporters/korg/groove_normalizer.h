#ifndef AI_ARRANGER_EXPORTERS_KORG_GROOVE_NORMALIZER_H
#define AI_ARRANGER_EXPORTERS_KORG_GROOVE_NORMALIZER_H

// Reuse canonical NoteSpan from megavoice_sanitizer.h (already on origin/main)
// so groove_normalizer.h and megavoice_sanitizer.h compile cleanly in ANY
// include order with zero ODR conflicts. Also publish the NoteSpan macro guard
// for downstream sibling quality headers.
#include "exporters/korg/megavoice_sanitizer.h"
#ifndef AI_ARRANGER_EXPORTERS_KORG_QUALITY_NOTE_SPAN_DEFINED
#define AI_ARRANGER_EXPORTERS_KORG_QUALITY_NOTE_SPAN_DEFINED
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// Standalone, portable C++20 groove normalizer for rescaled style elements:
//   1. Optional --bake-rubato piecewise-constant microsecond integration across
//      >= 2 distinct FF 51 tempo events with whole-bar length rounding.
//   2. Anticipated downbeat preservation in [len - max(1, bar/64), len - 1]
//      without silent deletion.
//   3. Strict same-pitch de-overlap after PPQN rescaling (1920 -> 384/480):
//      0 <= on_tick < off_tick <= next.on_tick < element_length_ticks.
// Zero JUCE/GUI dependency; 100% deterministic, offline, overflow-guarded.

namespace ai_arranger::exporters::korg::quality {

struct GrooveNormalizeOptions {
    bool     preserve_anticipations = true;  // preserve short anticipations in [len - max(1, bar/64), len - 1]
    bool     deoverlap_same_pitch   = true;  // enforce strict on < off <= next.on < len per pitch
    bool     bake_rubato            = false; // opt-in (--bake-rubato): warp ticks for >= 2 FF 51 tempos
    bool     is_looping_main        = false; // true on looping Mains (allows tick len-1 anticipation snap -> 0)
    uint32_t base_mpqn              = 500000;
    uint32_t ppqn                   = 480;   // used for max_timing_drift_ms reporting
};

struct GrooveNormalizeStats {
    uint32_t same_pitch_overlaps_fixed = 0;
    uint32_t zero_length_notes_fixed   = 0;
    uint32_t anticipations_preserved   = 0;
    bool     rubato_baked              = false;
    double   max_timing_drift_ms       = 0.0;
    uint32_t new_element_length_ticks  = 0;  // updated whole-bar element length
};

namespace detail {

inline bool safeMulAddU64(uint64_t a, uint64_t b, uint64_t c, uint64_t& out) noexcept {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return false;
    const uint64_t prod = a * b;
    if (c > std::numeric_limits<uint64_t>::max() - prod) return false;
    out = prod + c;
    return true;
}

struct TempoSegment {
    uint32_t start_tick = 0;
    uint32_t end_tick   = 0;
    uint32_t mpqn       = 500000;
    uint64_t start_acc  = 0; // cumulative sum(dt * mpqn) at start_tick
};

// Evaluates exact cumulative (microseconds * ppqn) = sum(dt * mpqn) at `tick`.
inline bool evalTempoAcc(const std::vector<TempoSegment>& segs,
                         uint32_t tick,
                         uint64_t& acc_out) noexcept {
    if (segs.empty()) return false;
    for (const auto& s : segs) {
        if (tick <= s.end_tick) {
            const uint64_t dt = (tick > s.start_tick) ? static_cast<uint64_t>(tick - s.start_tick) : 0ULL;
            return safeMulAddU64(dt, static_cast<uint64_t>(s.mpqn), s.start_acc, acc_out);
        }
    }
    const auto& last = segs.back();
    const uint64_t dt = (tick > last.start_tick) ? static_cast<uint64_t>(tick - last.start_tick) : 0ULL;
    return safeMulAddU64(dt, static_cast<uint64_t>(last.mpqn), last.start_acc, acc_out);
}

// Rounds acc / base_mpqn to nearest integer (half-up) with zero overflow risk.
inline uint64_t roundDivHalfUpU64(uint64_t acc, uint64_t denom) noexcept {
    const uint64_t q = acc / denom;
    const uint64_t r = acc % denom;
    const uint64_t half = denom - (denom / 2ULL); // ceil(denom / 2.0)
    return q + (r >= half ? 1ULL : 0ULL);
}

struct GrooveNoteItem {
    NoteSpan note;
    uint32_t orig_dur = 0;
};

} // namespace detail

// Normalizes rescaled note spans on a single channel before MIDI NoteOn/NoteOff
// emission. Returns updated element length in stats.new_element_length_ticks.
inline GrooveNormalizeStats normalizeTrackGroove(
    std::vector<NoteSpan>& notes,
    uint32_t element_length_ticks,
    uint32_t bar_ticks,
    const std::vector<std::pair<uint32_t, uint32_t>>& section_tempos = {},
    const GrooveNormalizeOptions& opt = {}) noexcept {

    GrooveNormalizeStats stats;
    stats.new_element_length_ticks = element_length_ticks;

    // Need at least 2 ticks ([0, 1]) to hold any valid NoteSpan with 0 <= on < off <= len-1.
    if (element_length_ticks < 2u) {
        notes.clear();
        return stats;
    }

    try {
        // Filter malformed MIDI values or out-of-bounds onsets and capture original duration.
        std::vector<detail::GrooveNoteItem> items;
        items.reserve(notes.size());
        for (const auto& n : notes) {
            if (n.pitch > 127u || n.vel == 0u || n.vel > 127u) continue;
            if (n.on_tick >= element_length_ticks) continue;
            const uint32_t dur = (n.off_tick > n.on_tick) ? (n.off_tick - n.on_tick) : 0u;
            items.push_back({n, dur});
        }

        // ── 1. Optional --bake-rubato (Piecewise-Constant Microsecond Warp) ──
        if (opt.bake_rubato && opt.base_mpqn > 0u && bar_ticks > 0u && !section_tempos.empty()) {
            // Collect valid (rel_tick, mpqn) pairs within [0, element_length_ticks).
            std::vector<std::pair<uint32_t, uint32_t>> valid_tempos;
            valid_tempos.reserve(section_tempos.size());
            for (const auto& [tk, mpqn] : section_tempos) {
                if (mpqn == 0u || tk >= element_length_ticks) continue;
                valid_tempos.emplace_back(tk, mpqn);
            }
            std::stable_sort(valid_tempos.begin(), valid_tempos.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });

            // Deduplicate identical ticks (last tempo at same tick wins).
            std::vector<std::pair<uint32_t, uint32_t>> dedup;
            for (const auto& kv : valid_tempos) {
                if (!dedup.empty() && dedup.back().first == kv.first) {
                    dedup.back().second = kv.second;
                } else {
                    dedup.push_back(kv);
                }
            }

            // Build contiguous piecewise-constant segments covering [0, element_length_ticks].
            std::vector<detail::TempoSegment> segs;
            bool overflow = false;
            uint64_t running_acc = 0;

            auto pushSeg = [&](uint32_t s, uint32_t e, uint32_t mpqn) {
                if (overflow || e <= s) return;
                detail::TempoSegment seg;
                seg.start_tick = s;
                seg.end_tick = e;
                seg.mpqn = mpqn;
                seg.start_acc = running_acc;
                uint64_t next_acc = 0;
                if (!detail::safeMulAddU64(static_cast<uint64_t>(e - s),
                                           static_cast<uint64_t>(mpqn),
                                           running_acc,
                                           next_acc)) {
                    overflow = true;
                    return;
                }
                running_acc = next_acc;
                segs.push_back(seg);
            };

            if (!dedup.empty()) {
                if (dedup.front().first > 0u) {
                    pushSeg(0u, dedup.front().first, opt.base_mpqn);
                }
                for (size_t i = 0; i < dedup.size(); ++i) {
                    const uint32_t s = dedup[i].first;
                    const uint32_t e = (i + 1 < dedup.size()) ? dedup[i + 1].first : element_length_ticks;
                    pushSeg(s, e, dedup[i].second);
                }
            }

            // Count distinct active tempos across built segments (require >= 2).
            std::vector<uint32_t> distinct_mpqns;
            for (const auto& s : segs) {
                if (std::find(distinct_mpqns.begin(), distinct_mpqns.end(), s.mpqn) == distinct_mpqns.end()) {
                    distinct_mpqns.push_back(s.mpqn);
                }
            }

            if (!overflow && distinct_mpqns.size() >= 2u) {
                const uint64_t base_m = static_cast<uint64_t>(opt.base_mpqn);
                const uint64_t total_warped = detail::roundDivHalfUpU64(running_acc, base_m);
                const uint64_t bt = static_cast<uint64_t>(bar_ticks);
                const uint64_t bars = (total_warped == 0ULL)
                    ? 1ULL
                    : ((total_warped - 1ULL) / bt) + 1ULL;

                uint64_t new_len64 = 0;
                const bool len_ok = detail::safeMulAddU64(bars, bt, 0ULL, new_len64) &&
                                    new_len64 >= 2ULL &&
                                    new_len64 <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());

                if (len_ok) {
                    const double safe_ppqn = static_cast<double>(opt.ppqn > 0u ? opt.ppqn : 480u);
                    std::vector<detail::GrooveNoteItem> warped_items = items;
                    double max_drift = 0.0;
                    bool note_overflow = false;

                    for (auto& it : warped_items) {
                        const uint32_t clamped_on = std::min(it.note.on_tick, element_length_ticks);
                        const uint32_t clamped_off = std::min(it.note.off_tick, element_length_ticks);
                        uint64_t acc_on = 0;
                        uint64_t acc_off = 0;
                        if (!detail::evalTempoAcc(segs, clamped_on, acc_on) ||
                            !detail::evalTempoAcc(segs, clamped_off, acc_off)) {
                            note_overflow = true;
                            break;
                        }
                        uint64_t w_on = detail::roundDivHalfUpU64(acc_on, base_m);
                        const uint64_t w_off = detail::roundDivHalfUpU64(acc_off, base_m);
                        if (w_on > new_len64 || w_off > new_len64) {
                            note_overflow = true;
                            break;
                        }
                        // Prevent half-up rounding from pushing a pre-end onset onto new_len64.
                        if (w_on == new_len64) {
                            w_on = new_len64 - 1ULL;
                        }

                        const double true_on_us = static_cast<double>(acc_on) / safe_ppqn;
                        const double quant_on_us = (static_cast<double>(w_on) * static_cast<double>(base_m)) / safe_ppqn;
                        const double drift_ms = std::abs(quant_on_us - true_on_us) / 1000.0;
                        if (drift_ms > max_drift) max_drift = drift_ms;

                        it.note.on_tick = static_cast<uint32_t>(w_on);
                        it.note.off_tick = static_cast<uint32_t>(w_off);
                        if (it.orig_dur > 0u) {
                            it.orig_dur = (w_off > w_on) ? static_cast<uint32_t>(w_off - w_on) : 1u;
                        }
                    }

                    if (!note_overflow) {
                        items = std::move(warped_items);
                        element_length_ticks = static_cast<uint32_t>(new_len64);
                        stats.new_element_length_ticks = element_length_ticks;
                        stats.rubato_baked = true;
                        stats.max_timing_drift_ms = max_drift;
                    }
                }
            }
        }

        // ── 2. Anticipated Downbeat Preservation & Bounds Clamping ───────
        const uint32_t max_tick = element_length_ticks - 1u;
        const uint32_t antic_window = std::max<uint32_t>(1u, bar_ticks / 64u);
        const uint32_t antic_start = (element_length_ticks > antic_window)
            ? (element_length_ticks - antic_window)
            : 0u;

        std::vector<NoteSpan> bounded;
        bounded.reserve(items.size());

        for (auto& it : items) {
            auto& n = it.note;

            // Notes starting strictly at or past element_length_ticks are out of bounds.
            if (n.on_tick >= element_length_ticks) {
                continue;
            }

            const bool in_antic_window = (n.on_tick >= antic_start) && (n.on_tick <= max_tick);
            if (opt.preserve_anticipations && in_antic_window && it.orig_dur > 0u) {
                if (n.on_tick == max_tick) {
                    if (opt.is_looping_main) {
                        // Snap final-tick anticipation on looping Mains cleanly to tick 0.
                        n.on_tick = 0u;
                        n.off_tick = std::min<uint32_t>(std::max<uint32_t>(1u, it.orig_dur), max_tick);
                    } else if (element_length_ticks >= 3u) {
                        n.on_tick = element_length_ticks - 2u;
                        n.off_tick = max_tick;
                    } else {
                        n.on_tick = 0u;
                        n.off_tick = 1u;
                    }
                    ++stats.anticipations_preserved;
                } else {
                    if (n.off_tick > max_tick || n.off_tick <= n.on_tick) {
                        n.off_tick = max_tick;
                    }
                    ++stats.anticipations_preserved;
                }
                bounded.push_back(n);
                continue;
            }

            // Non-anticipation note starting at len - 1 cannot satisfy on < off <= len - 1.
            if (n.on_tick >= max_tick) {
                continue;
            }
            if (n.off_tick > max_tick) {
                n.off_tick = max_tick;
            }
            if (n.off_tick <= n.on_tick) {
                n.off_tick = n.on_tick + 1u;
                ++stats.zero_length_notes_fixed;
            }
            bounded.push_back(n);
        }

        // ── 3. Same-Pitch De-Overlap (on < off <= next.on < len) ─────────
        if (opt.deoverlap_same_pitch && !bounded.empty()) {
            std::array<std::vector<NoteSpan>, 128> by_pitch{};
            for (const auto& n : bounded) {
                by_pitch[n.pitch].push_back(n);
            }

            bounded.clear();
            for (uint8_t p = 0; p < 128u; ++p) {
                auto& seq = by_pitch[p];
                if (seq.empty()) continue;

                std::stable_sort(seq.begin(), seq.end(),
                                 [](const NoteSpan& a, const NoteSpan& b) {
                                     if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
                                     return a.off_tick < b.off_tick;
                                 });

                std::vector<NoteSpan> resolved;
                resolved.reserve(seq.size());

                for (auto cur : seq) {
                    if (cur.on_tick >= max_tick) continue;
                    if (cur.off_tick > max_tick) cur.off_tick = max_tick;
                    if (cur.off_tick <= cur.on_tick) {
                        cur.off_tick = cur.on_tick + 1u;
                        ++stats.zero_length_notes_fixed;
                    }

                    if (!resolved.empty()) {
                        auto& prev = resolved.back();
                        if (cur.on_tick <= prev.on_tick) {
                            // Collapsed identical-tick flam (or pushed by prior shift).
                            if (prev.on_tick + 1u < max_tick) {
                                cur.on_tick = prev.on_tick + 1u;
                                prev.off_tick = cur.on_tick;
                                if (cur.off_tick <= cur.on_tick) {
                                    cur.off_tick = cur.on_tick + 1u;
                                    ++stats.zero_length_notes_fixed;
                                }
                                ++stats.same_pitch_overlaps_fixed;
                            } else {
                                // At the extreme right boundary (prev.on == max_tick - 1):
                                // try nudging prev left by 1 tick if space exists before prev.
                                const uint32_t min_prev_on = (resolved.size() >= 2u)
                                    ? resolved[resolved.size() - 2u].off_tick
                                    : 0u;
                                if (prev.on_tick > min_prev_on) {
                                    --prev.on_tick;
                                    prev.off_tick = prev.on_tick + 1u;
                                    cur.on_tick = prev.off_tick;
                                    cur.off_tick = max_tick;
                                    ++stats.same_pitch_overlaps_fixed;
                                } else {
                                    // Physically saturated (zero ticks available): keep louder note.
                                    if (cur.vel > prev.vel) prev.vel = cur.vel;
                                    ++stats.same_pitch_overlaps_fixed;
                                    continue;
                                }
                            }
                        } else if (prev.off_tick > cur.on_tick) {
                            // Standard same-pitch overlap: truncate previous note at next onset.
                            prev.off_tick = cur.on_tick;
                            ++stats.same_pitch_overlaps_fixed;
                        }
                    }

                    resolved.push_back(cur);
                }

                for (const auto& n : resolved) {
                    bounded.push_back(n);
                }
            }
        }

        // Deterministic chronological output sort.
        std::stable_sort(bounded.begin(), bounded.end(),
                         [](const NoteSpan& a, const NoteSpan& b) {
                             if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
                             if (a.pitch != b.pitch)     return a.pitch < b.pitch;
                             return a.off_tick < b.off_tick;
                         });

        notes = std::move(bounded);
    } catch (...) {
        return stats;
    }

    return stats;
}

} // namespace ai_arranger::exporters::korg::quality

#endif // AI_ARRANGER_EXPORTERS_KORG_GROOVE_NORMALIZER_H
