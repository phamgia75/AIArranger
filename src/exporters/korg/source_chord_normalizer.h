#ifndef AI_ARRANGER_EXPORTERS_KORG_SOURCE_CHORD_NORMALIZER_H
#define AI_ARRANGER_EXPORTERS_KORG_SOURCE_CHORD_NORMALIZER_H

#include "engine/arranger/chord_input.h"
#include "engine/uasf/types.h"
#include "exporters/korg/korg_style_smf.h"
#include "exporters/korg/megavoice_sanitizer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Standalone, portable C++20 Yamaha Ctb2[18..19] Source-Chord Normalizer and
// 12-Root Register-Safety / Pa5X Style Edit Parameter Recommender (Track B Q3a).
// Zero JUCE/GUI dependency; 100% deterministic, offline, header-only.
// Consumes canonical CasmTrackView and NoteSpan from megavoice_sanitizer.h so
// all four quality headers can be included in any order with zero ODR conflicts.

namespace ai_arranger::exporters::korg::quality {

// Decoded Yamaha Ctb2[18..19] source chord (Wierzba/Bedesem v2.1 Table 15).
struct DecodedYamahaSourceChord {
    uint8_t             root               = 0;      // 0=C .. 11=B
    uint8_t             raw_type           = 0x00;   // Ctb2[19] byte
    arranger::ChordType chord_type         = arranger::ChordType::Major;
    bool                is_major_family    = true;
    bool                is_minor_family    = false;
    bool                is_dominant_family = false;
    bool                has_maj7           = false;
    bool                has_min7           = false;
    bool                is_cancel          = false;
    const char*         name               = "Maj";
};

// Full Yamaha SFF Ctb2[18..19] decoder (Wierzba/Bedesem v2.1 Table 15, 0x00..0x22).
inline DecodedYamahaSourceChord decodeYamahaSourceChord(uint8_t raw_root,
                                                        uint8_t raw_type) noexcept {
    DecodedYamahaSourceChord d{};
    d.root     = (raw_root <= 11) ? raw_root : 0;
    d.raw_type = raw_type;

    using CT = arranger::ChordType;
    switch (raw_type) {
        case 0x00: d.chord_type = CT::Major;    d.is_major_family = true;  d.name = "Maj";       break;
        case 0x01: d.chord_type = CT::Maj6;     d.is_major_family = true;  d.name = "Maj6";      break;
        case 0x02: d.chord_type = CT::Maj7;     d.is_major_family = true;  d.has_maj7 = true; d.name = "Maj7";     break;
        case 0x03: d.chord_type = CT::Maj7;     d.is_major_family = true;  d.has_maj7 = true; d.name = "Maj7#11";  break;
        case 0x04: d.chord_type = CT::Add9;     d.is_major_family = true;  d.name = "Maj(9)";    break;
        case 0x05: d.chord_type = CT::Maj9;     d.is_major_family = true;  d.has_maj7 = true; d.name = "Maj7(9)";  break;
        case 0x06: d.chord_type = CT::Maj6;     d.is_major_family = true;  d.name = "Maj6(9)";   break;
        case 0x07: d.chord_type = CT::Aug;      d.is_major_family = true;  d.name = "Aug";       break;

        case 0x08: d.chord_type = CT::Minor;    d.is_major_family = false; d.is_minor_family = true; d.name = "Min";       break;
        case 0x09: d.chord_type = CT::Min6;     d.is_major_family = false; d.is_minor_family = true; d.name = "Min6";      break;
        case 0x0A: d.chord_type = CT::Min7;     d.is_major_family = false; d.is_minor_family = true; d.has_min7 = true; d.name = "Min7";      break;
        case 0x0B: d.chord_type = CT::Min7b5;   d.is_major_family = false; d.is_minor_family = true; d.has_min7 = true; d.name = "Min7b5";    break;
        case 0x0C: d.chord_type = CT::Minor;    d.is_major_family = false; d.is_minor_family = true; d.name = "Min(9)";    break;
        case 0x0D: d.chord_type = CT::Min9;     d.is_major_family = false; d.is_minor_family = true; d.has_min7 = true; d.name = "Min7(9)";   break;
        case 0x0E: d.chord_type = CT::Min7;     d.is_major_family = false; d.is_minor_family = true; d.has_min7 = true; d.name = "Min7(11)";  break;
        case 0x0F: d.chord_type = CT::MinMaj7;  d.is_major_family = false; d.is_minor_family = true; d.has_maj7 = true; d.name = "MinMaj7";   break;
        case 0x10: d.chord_type = CT::MinMaj7;  d.is_major_family = false; d.is_minor_family = true; d.has_maj7 = true; d.name = "MinMaj7(9)"; break;

        case 0x11: d.chord_type = CT::Dim;      d.is_major_family = false; d.is_minor_family = true; d.name = "Dim";       break;
        case 0x12: d.chord_type = CT::Dim7;     d.is_major_family = false; d.is_minor_family = true; d.name = "Dim7";      break;

        case 0x13: d.chord_type = CT::Dom7;     d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7";       break;
        case 0x14: d.chord_type = CT::Dom7Sus4; d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7sus4";   break;
        case 0x15: d.chord_type = CT::Dom7;     d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7b5";     break;
        case 0x16: d.chord_type = CT::Dom9;     d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7(9)";    break;
        case 0x17: d.chord_type = CT::Dom7;     d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7(#11)";  break;
        case 0x18: d.chord_type = CT::Dom7;     d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7(13)";   break;
        case 0x19: d.chord_type = CT::Dom7;     d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7(b9)";   break;
        case 0x1A: d.chord_type = CT::Dom7;     d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7(b13)";  break;
        case 0x1B: d.chord_type = CT::Dom7;     d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7(#9)";   break;

        case 0x1C: d.chord_type = CT::Aug;      d.is_major_family = true;  d.has_maj7 = true; d.name = "Maj7aug"; break;
        case 0x1D: d.chord_type = CT::Aug7;     d.is_major_family = false; d.is_dominant_family = true; d.has_min7 = true; d.name = "7aug";    break;
        case 0x1E: d.chord_type = CT::Power;    d.is_major_family = true;  d.name = "1+8";     break;
        case 0x1F: d.chord_type = CT::Power;    d.is_major_family = true;  d.name = "1+5";     break;
        case 0x20: d.chord_type = CT::Sus4;     d.is_major_family = true;  d.name = "Sus4";    break;
        case 0x21: d.chord_type = CT::Sus2;     d.is_major_family = true;  d.name = "1+2+5";   break;
        case 0x22: d.chord_type = CT::NoChord;  d.is_major_family = false; d.is_cancel = true; d.name = "Cancel";  break;
        default:   d.chord_type = CT::Major;    d.is_major_family = true;  d.name = "Maj";     break;
    }
    return d;
}

struct Pa5xTrackEditAdvice {
    uint8_t     korg_ch              = 0xFF;
    std::string pa5x_ntt_type;            // "Parallel" | "Fixed" | "No Transpose" | "Guitar Mode"
    std::string pa5x_ntt_table;           // "Root" | "Parallel" | "Chord" | "i-Series" | "No Transpose" | "Guitar"
    uint8_t     wrap_around          = 6; // 1..12 semitones (derived from CASM high_key or role sweet spot)
    uint8_t     key_range_low        = 0; // Pa5X LimitKeyRange low
    uint8_t     key_range_high       = 127;
    std::string retrigger_mode;           // "Off" | "Retrigger" | "Repitch" (mapped from Yamaha RTR 0..5)
    int8_t      octave_shift_applied = 0; // -12, 0, or +12 (or +/-24 if extreme) when centered
};

struct NormalizeResult {
    std::vector<NoteSpan> notes;
    Pa5xTrackEditAdvice   advice;
    uint32_t              notes_transposed          = 0;
    uint32_t              quality_remapped          = 0;
    uint32_t              register_violations_fixed = 0;
    bool                  normalized_from_non_c     = false;
};

// Map Yamaha CASM RTR (Ctb2[33], 0..5) to Pa5X Retrigger Mode string.
inline std::string mapYamahaRtrToPa5x(uint8_t rtr) noexcept {
    switch (rtr) {
        case 0:  return "Off";       // Stop
        case 1:
        case 2:  return "Repitch";   // Pitch Shift / Pitch Shift to Root
        case 3:
        case 4:
        case 5:  return "Retrigger"; // Retrigger / Retrigger to Root / Note Generator
        default: return "Off";
    }
}

namespace norm_detail {

enum class TargetCvFamily : uint8_t {
    Major,
    Minor,
    Dom7,
    Dim,
    Sus4,
    Power
};

inline TargetCvFamily classifyTargetCvFamily(arranger::ChordType t) noexcept {
    using CT = arranger::ChordType;
    switch (t) {
        case CT::Dom7:
        case CT::Dom9:
        case CT::Dom7Sus4:
        case CT::Aug7:
            return TargetCvFamily::Dom7;
        case CT::Dim:
        case CT::Dim7:
            return TargetCvFamily::Dim;
        case CT::Sus4:
        case CT::Sus2:
            return TargetCvFamily::Sus4;
        case CT::Power:
            return TargetCvFamily::Power;
        case CT::Minor:
        case CT::Min6:
        case CT::Min7:
        case CT::Min9:
        case CT::Min7b5:
        case CT::MinMaj7:
            return TargetCvFamily::Minor;
        default:
            return TargetCvFamily::Major;
    }
}

inline bool isMinorTargetChord(arranger::ChordType t) noexcept {
    const TargetCvFamily fam = classifyTargetCvFamily(t);
    return fam == TargetCvFamily::Minor || fam == TargetCvFamily::Dim;
}

// Fold into [0, 127] by octaves so pitch class is never lost.
inline uint8_t foldMidiPitch(int p) noexcept {
    while (p < 0)   p += 12;
    while (p > 127) p -= 12;
    return static_cast<uint8_t>(p);
}

// Sanitize a NoteSpan copy to enforce strict on_tick < off_tick and valid MIDI fields.
inline NoteSpan sanitizeSpan(const NoteSpan& s) noexcept {
    NoteSpan out = s;
    out.pitch = std::clamp<uint8_t>(out.pitch, 0, 127);
    out.vel   = std::clamp<uint8_t>(out.vel, 1, 127);
    if (out.off_tick <= out.on_tick) {
        out.off_tick = (out.on_tick < std::numeric_limits<uint32_t>::max())
                           ? (out.on_tick + 1u)
                           : out.on_tick;
        if (out.off_tick == out.on_tick && out.on_tick > 0) {
            out.on_tick -= 1u;
        }
    }
    return out;
}

inline void populatePa5xNttAdvice(const CasmTrackView& casm,
                                  uint8_t korg_ch,
                                  uasf::TrackRole role,
                                  Pa5xTrackEditAdvice& advice) noexcept {
    if (!isChordDriven(korg_ch) ||
        role == uasf::TrackRole::Drum ||
        role == uasf::TrackRole::Percussion) {
        advice.pa5x_ntt_type  = "No Transpose";
        advice.pa5x_ntt_table = "No Transpose";
        return;
    }

    if (casm.has_casm) {
        const uint8_t nttLow = static_cast<uint8_t>(casm.ntt & 0x7Fu);
        if (casm.ntr == 2) {
            advice.pa5x_ntt_type  = "Guitar Mode";
            advice.pa5x_ntt_table = "Guitar";
            return;
        }
        if (casm.ntr == 1 && nttLow == 0) {
            advice.pa5x_ntt_type  = "No Transpose";
            advice.pa5x_ntt_table = "No Transpose";
            return;
        }
        if (casm.ntr == 1) {
            advice.pa5x_ntt_type  = "Fixed";
            advice.pa5x_ntt_table = (nttLow == 2) ? "Chord" : "Parallel";
            return;
        }
        // NTR == 0 (Root Transposition) or unknown -> Parallel
        advice.pa5x_ntt_type = "Parallel";
        if (casm.ntt_bass || role == uasf::TrackRole::Bass || korg_ch == kBassCh) {
            advice.pa5x_ntt_table = "Root";
        } else if (nttLow == 0) {
            advice.pa5x_ntt_table = "No Transpose";
        } else if (nttLow == 2) {
            advice.pa5x_ntt_table = "Chord";
        } else if (nttLow == 3 || nttLow == 4) {
            advice.pa5x_ntt_table = "i-Series";
        } else {
            advice.pa5x_ntt_table = "Parallel";
        }
        return;
    }

    // No CASM metadata -> infer from Korg channel / UASF role.
    advice.pa5x_ntt_type = "Parallel";
    if (korg_ch == kBassCh || role == uasf::TrackRole::Bass) {
        advice.pa5x_ntt_table = "Root";
    } else if (role == uasf::TrackRole::Chord || role == uasf::TrackRole::Pad) {
        advice.pa5x_ntt_table = "Chord";
    } else {
        advice.pa5x_ntt_table = "Parallel";
    }
}

inline uint8_t defaultWrapAroundFor(uint8_t korg_ch,
                                    uasf::TrackRole role,
                                    const CasmTrackView& casm) noexcept {
    if (casm.has_casm && casm.ntr == 2) return 5;  // Guitar: F
    if (korg_ch == kBassCh || role == uasf::TrackRole::Bass) return 6;  // Bass: F#
    return 6;  // Chord / Pad / Phrase: F#
}

inline void targetRegisterBounds(uint8_t korg_ch,
                                 uasf::TrackRole role,
                                 const CasmTrackView& casm,
                                 uint8_t& out_lo,
                                 uint8_t& out_hi) noexcept {
    if (korg_ch == kBassCh || role == uasf::TrackRole::Bass) {
        out_lo = 24;  // C1
        out_hi = 55;  // G3
    } else if (casm.has_casm && casm.ntr == 2) {
        out_lo = 40;  // E2
        out_hi = 88;  // E6
    } else {
        out_lo = 36;  // C2
        out_hi = 84;  // C6
    }
}

// Pa5X Wrap Around (w in 1..12, equivalent to high_key = w - 1):
// root r in 0..11 transposes by +r if r < w, else r - 12.
inline int pa5xRootShift(int root_pc, uint8_t wrap_around) noexcept {
    const int w = std::clamp<int>(wrap_around, 1, 12);
    return (root_pc < w) ? root_pc : (root_pc - 12);
}

inline uint32_t countRegisterViolations(const std::vector<NoteSpan>& notes,
                                        int oct_shift,
                                        uint8_t wrap_around,
                                        uint8_t lo,
                                        uint8_t hi) noexcept {
    uint32_t viol = 0;
    for (const auto& n : notes) {
        const int base = static_cast<int>(n.pitch) + oct_shift;
        if (base < 0 || base > 127) {
            viol += 12;
            continue;
        }
        for (int r = 0; r < 12; ++r) {
            const int p = base + pa5xRootShift(r, wrap_around);
            if (p < static_cast<int>(lo) || p > static_cast<int>(hi)) {
                ++viol;
            }
        }
    }
    return viol;
}

inline bool hasSimultaneousPitch(const std::vector<NoteSpan>& notes,
                                 size_t skip_idx,
                                 uint32_t on_tick,
                                 uint8_t pitch) noexcept {
    for (size_t i = 0; i < notes.size(); ++i) {
        if (i != skip_idx && notes[i].on_tick == on_tick && notes[i].pitch == pitch) {
            return true;
        }
    }
    return false;
}

// Choose the first collision-free candidate delta for a simultaneous note remap;
// falls back to the primary delta (later deduplicated if all targets are occupied).
inline int pickNonCollidingDelta(const std::vector<NoteSpan>& notes,
                                 size_t idx,
                                 std::initializer_list<int> deltas) noexcept {
    const auto& n = notes[idx];
    int primary = 0;
    bool havePrimary = false;
    for (int d : deltas) {
        if (!havePrimary) { primary = d; havePrimary = true; }
        const uint8_t cand = foldMidiPitch(static_cast<int>(n.pitch) + d);
        if (!hasSimultaneousPitch(notes, idx, n.on_tick, cand)) {
            return d;
        }
    }
    return primary;
}

// Deduplicate any identical (on_tick, pitch) spans in-place while preserving
// first-seen order and retaining max(off_tick) and max(vel).
inline void deduplicateSimultaneousPitches(std::vector<NoteSpan>& notes) noexcept {
    if (notes.size() < 2) return;
    std::vector<NoteSpan> out;
    out.reserve(notes.size());
    for (const auto& s : notes) {
        bool merged = false;
        for (auto& existing : out) {
            if (existing.on_tick == s.on_tick && existing.pitch == s.pitch) {
                if (s.off_tick > existing.off_tick) existing.off_tick = s.off_tick;
                if (s.vel > existing.vel)           existing.vel = s.vel;
                existing.from_guitar_ntr = existing.from_guitar_ntr || s.from_guitar_ntr;
                merged = true;
                break;
            }
        }
        if (!merged) {
            out.push_back(s);
        }
    }
    notes.swap(out);
}

}  // namespace norm_detail

// Independent 12-root register safety audit helper (usable by callers & tests).
// Returns total (note x root) out-of-range events across all 12 roots 0..11.
inline uint32_t auditRegisterSafetyAcross12Roots(const std::vector<NoteSpan>& notes,
                                                 uint8_t wrap_around,
                                                 uint8_t key_range_low,
                                                 uint8_t key_range_high) noexcept {
    return norm_detail::countRegisterViolations(notes, 0, wrap_around, key_range_low, key_range_high);
}

// Normalizes a chord-driven track from its Yamaha Ctb2[18..19] source chord into
// Pa5X's target CV recording chord (default: C Major for cv1, C Minor for cv2).
// Skips Bypass (NTR=1 & NTT=0) and Drum/Perc tracks untouched.
inline NormalizeResult normalizeTrackToPa5xReference(
    const std::vector<NoteSpan>& notes,
    uint8_t korg_ch,
    uasf::TrackRole role,
    const CasmTrackView& casm,
    arranger::ChordType target_cv_chord = arranger::ChordType::Major) noexcept {
    NormalizeResult res{};
    res.notes.reserve(notes.size());
    for (const auto& s : notes) {
        res.notes.push_back(norm_detail::sanitizeSpan(s));
    }

    auto& adv = res.advice;
    adv.korg_ch = korg_ch;
    adv.retrigger_mode = mapYamahaRtrToPa5x(casm.has_casm ? casm.rtr : 0);
    norm_detail::populatePa5xNttAdvice(casm, korg_ch, role, adv);

    // 1. Skip Guard: Drum/Percussion or explicit CASM RootFixed+Bypass (NTR=1 & NTT=0).
    const bool isBypass = casm.has_casm && casm.ntr == 1 && (casm.ntt & 0x7Fu) == 0;
    if (!isChordDriven(korg_ch) ||
        role == uasf::TrackRole::Drum ||
        role == uasf::TrackRole::Percussion ||
        isBypass) {
        adv.pa5x_ntt_type  = "No Transpose";
        adv.pa5x_ntt_table = "No Transpose";
        adv.wrap_around    = 12;
        adv.key_range_low  = 0;
        adv.key_range_high = 127;
        adv.octave_shift_applied = 0;
        return res;
    }

    // 2. Decode Yamaha Ctb2[18..19] source chord (defaults to C Maj if !has_casm).
    const DecodedYamahaSourceChord src =
        casm.has_casm
            ? decodeYamahaSourceChord(casm.source_chord_root, casm.source_chord_type)
            : decodeYamahaSourceChord(0, 0x00);

    // 3. Root Normalization: minimal signed shift into C (0).
    if (src.root >= 1 && src.root <= 11) {
        const int delta = (src.root <= 6) ? -static_cast<int>(src.root)
                                          : static_cast<int>(12 - src.root);
        res.normalized_from_non_c = true;
        for (auto& n : res.notes) {
            const uint8_t shifted = norm_detail::foldMidiPitch(static_cast<int>(n.pitch) + delta);
            if (shifted != n.pitch) {
                n.pitch = shifted;
                ++res.notes_transposed;
            }
        }
    }

    // 4. Quality Normalization: source chord quality -> target CV chord family
    //    (CV1 Major, CV2 Minor, CV3 Dom7, CV4 Dim/Dim7, CV5 Sus4, CV6 Power).
    const norm_detail::TargetCvFamily targetFamily = norm_detail::classifyTargetCvFamily(target_cv_chord);
    const uint8_t nttLow = static_cast<uint8_t>(casm.ntt & 0x7Fu);
    // SFF2 Guitar Mode (NTR == 2) uses NTT == 1 for Stroke (not Melody), so exclude casm.ntr == 2.
    const bool isMelodyNtt = casm.has_casm && casm.ntr != 2 && !casm.ntt_bass &&
                             (nttLow == 1 || nttLow == 3 || nttLow == 4);
    const bool isChordOrBass =
        casm.has_casm
            ? (casm.ntt_bass || nttLow == 2 || casm.ntr == 2 || korg_ch == kBassCh || role == uasf::TrackRole::Bass)
            : (korg_ch == kBassCh || role == uasf::TrackRole::Bass ||
               role == uasf::TrackRole::Chord || role == uasf::TrackRole::Pad);

    if (!src.is_cancel) {
        for (size_t i = 0; i < res.notes.size(); ++i) {
            auto& n = res.notes[i];
            const int pc = static_cast<int>(n.pitch % 12);
            int delta = 0;

            switch (targetFamily) {
                case norm_detail::TargetCvFamily::Major:
                    // Target CV is C Major (cv1): {0, 4, 7}.
                    if (src.has_maj7 && pc == 11) {
                        // Preserve B (pc 11) on Melody NTT; snap B -> C/G on Chord/Bass/Guitar to prevent C7 clash.
                        if (isChordOrBass && !isMelodyNtt) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -4, +5});
                        }
                    } else if (src.is_minor_family) {
                        if (pc == 3) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, +4, -3});  // Eb -> E
                        } else if (src.has_min7 && pc == 10) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+2, -3, +6});  // Bb -> C or G or E
                        } else if ((src.raw_type == 0x0B || src.raw_type == 0x11 || src.raw_type == 0x12) && pc == 6) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -2});      // Gb -> G
                        } else if (src.raw_type == 0x12 && pc == 9 && isChordOrBass) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-2, +3});      // Dim7 A -> G
                        }
                    } else if (src.is_dominant_family) {
                        if (pc == 10) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+2, -3, +6});  // Bb -> C or G or E
                        } else if (src.raw_type == 0x14 && pc == 5 && isChordOrBass) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +2});      // 7sus4 F -> E
                        } else if (src.raw_type == 0x15 && pc == 6) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -2});      // 7b5 Gb -> G
                        } else if (src.raw_type == 0x1D && pc == 8) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +4});      // 7aug G# -> G
                        }
                    } else {
                        // Major-family edge qualities (Aug, Maj7aug, Sus4)
                        if ((src.raw_type == 0x07 || src.raw_type == 0x1C) && pc == 8) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +4});      // Aug G# -> G
                        } else if (src.raw_type == 0x20 && pc == 5 && isChordOrBass) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +2});      // Sus4 F -> E
                        }
                    }
                    break;

                case norm_detail::TargetCvFamily::Minor:
                    // Target CV is C Minor (cv2): {0, 3, 7}.
                    if ((src.is_major_family || src.is_dominant_family) && pc == 4) {
                        delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +3});          // E -> Eb
                    } else if (src.has_maj7 && pc == 11 && isChordOrBass && !isMelodyNtt) {
                        delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -4, +4});      // B -> C
                    } else if ((src.raw_type == 0x07 || src.raw_type == 0x1C || src.raw_type == 0x1D) && pc == 8) {
                        delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +4});          // Aug G# -> G
                    } else if ((src.raw_type == 0x0B || src.raw_type == 0x11 || src.raw_type == 0x12) && pc == 6) {
                        delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -3});          // Gb -> G
                    }
                    break;

                case norm_detail::TargetCvFamily::Dom7:
                    // Target CV is C7 (cv3): {0, 4, 7, 10} — strictly preserves b7 (pc 10).
                    if (src.has_maj7 && pc == 11 && isChordOrBass && !isMelodyNtt) {
                        delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +1, -4});      // B -> Bb (or C/G)
                    } else if (src.is_minor_family) {
                        if (pc == 3) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, +4, -3});  // Eb -> E
                        } else if ((src.raw_type == 0x0B || src.raw_type == 0x11 || src.raw_type == 0x12) && pc == 6) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -2});      // Gb -> G
                        } else if (src.raw_type == 0x12 && pc == 9 && isChordOrBass) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -2});      // Dim7 A -> Bb or G
                        }
                    } else if (src.is_dominant_family) {
                        // Preserve b7 (pc 10) untouched! Only normalize altered 5ths / 7sus4 4th.
                        if (src.raw_type == 0x14 && pc == 5 && isChordOrBass) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +2});      // 7sus4 F -> E
                        } else if (src.raw_type == 0x15 && pc == 6) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -2});      // 7b5 Gb -> G
                        } else if (src.raw_type == 0x1D && pc == 8) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +4});      // 7aug G# -> G
                        }
                    } else {
                        if ((src.raw_type == 0x07 || src.raw_type == 0x1C) && pc == 8) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +4});      // Aug G# -> G
                        } else if (src.raw_type == 0x20 && pc == 5 && isChordOrBass) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +2});      // Sus4 F -> E
                        }
                    }
                    break;

                case norm_detail::TargetCvFamily::Dim:
                    // Target CV is C Dim / Dim7 (cv4): {0, 3, 6, 9} — strictly preserves b3 (3), b5 (6), bb7 (9).
                    if (src.raw_type != 0x11 && src.raw_type != 0x12) {
                        if ((src.is_major_family || src.is_dominant_family) && pc == 4) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +2});      // E -> Eb
                        } else if (pc == 7 && src.raw_type != 0x0B) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +2});      // G -> Gb (or A)
                        } else if ((src.raw_type == 0x07 || src.raw_type == 0x1C || src.raw_type == 0x1D) && pc == 8) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-2, +1});      // G# -> Gb or A
                        } else if ((src.has_min7 || src.is_dominant_family) && pc == 10) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +2});      // Bb -> A or C
                        } else if (src.has_maj7 && pc == 11 && isChordOrBass && !isMelodyNtt) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -2});      // B -> C or A
                        }
                    }
                    break;

                case norm_detail::TargetCvFamily::Sus4:
                    // Target CV is C Sus4 (cv5): {0, 5, 7} — strictly preserves 4th (pc 5).
                    if (src.raw_type != 0x20 && src.raw_type != 0x21) {
                        if ((src.is_major_family || src.is_dominant_family) && pc == 4) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, +3, -4});  // E -> F (or G/C)
                        } else if (src.is_minor_family && pc == 3) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+2, +4, -3});  // Eb -> F (or G/C)
                        } else if ((src.has_min7 || src.is_dominant_family) && pc == 10) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+2, -3});      // Bb -> C or G
                        } else if (src.has_maj7 && pc == 11 && isChordOrBass && !isMelodyNtt) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -4});      // B -> C or G
                        } else if ((src.raw_type == 0x0B || src.raw_type == 0x11 || src.raw_type == 0x12 || src.raw_type == 0x15) && pc == 6) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -1});      // Gb -> G or F
                        } else if ((src.raw_type == 0x07 || src.raw_type == 0x1C || src.raw_type == 0x1D) && pc == 8) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +4});      // G# -> G or C
                        }
                    }
                    break;

                case norm_detail::TargetCvFamily::Power:
                    // Target CV is C Power / 1+5 (cv6): {0, 7} — preserves {0, 7} untouched.
                    if (src.raw_type != 0x1E && src.raw_type != 0x1F) {
                        if ((src.is_major_family || src.is_dominant_family) && pc == 4 && isChordOrBass && !isMelodyNtt) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+3, -4, +8, -9}); // E -> G/C
                        } else if (src.is_minor_family && pc == 3 && isChordOrBass && !isMelodyNtt) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-3, +4, +9, -8}); // Eb -> C/G
                        } else if ((src.has_min7 || src.is_dominant_family) && pc == 10) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+2, -3});         // Bb -> C/G
                        } else if (src.has_maj7 && pc == 11 && isChordOrBass && !isMelodyNtt) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -4});         // B -> C/G
                        } else if ((src.raw_type == 0x0B || src.raw_type == 0x11 || src.raw_type == 0x12 || src.raw_type == 0x15) && pc == 6) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+1, -6});         // Gb -> G/C
                        } else if ((src.raw_type == 0x07 || src.raw_type == 0x1C || src.raw_type == 0x1D) && pc == 8) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {-1, +4});         // G# -> G/C
                        } else if ((src.raw_type == 0x20 || src.raw_type == 0x14) && pc == 5 && isChordOrBass) {
                            delta = norm_detail::pickNonCollidingDelta(res.notes, i, {+2, -5});         // F -> G/C
                        }
                    }
                    break;
            }

            if (delta != 0) {
                n.pitch = norm_detail::foldMidiPitch(static_cast<int>(n.pitch) + delta);
                ++res.quality_remapped;
            }
        }
    }

    // 5. 12-Root Register Safety Audit, Whole-Octave Centering & Wrap Around Selection.
    uint8_t targetLo = 36, targetHi = 84;
    norm_detail::targetRegisterBounds(korg_ch, role, casm, targetLo, targetHi);

    const uint8_t initialWrap =
        (casm.has_casm && casm.ntr == 0 && casm.high_key <= 11)
            ? static_cast<uint8_t>(casm.high_key + 1)
            : norm_detail::defaultWrapAroundFor(korg_ch, role, casm);

    adv.wrap_around = std::clamp<uint8_t>(initialWrap, 1, 12);

    // Coherent validation of CASM key range (prevents inverted or >127 bounds).
    const bool validCasmRange =
        casm.has_casm && casm.note_low <= 127 && casm.note_high <= 127 &&
        casm.note_low < casm.note_high && (casm.note_high - casm.note_low) >= 11;
    if (validCasmRange && (casm.note_low > 0 || casm.note_high < 127)) {
        adv.key_range_low  = casm.note_low;
        adv.key_range_high = casm.note_high;
    } else {
        adv.key_range_low  = targetLo;
        adv.key_range_high = targetHi;
    }
    adv.octave_shift_applied = 0;

    if (!res.notes.empty()) {
        // Check if initial (oct_shift = 0, initialWrap) is already 100% 12-root register-safe.
        uint32_t bestViol = norm_detail::countRegisterViolations(res.notes, 0, adv.wrap_around, targetLo, targetHi);
        int bestOctShift = 0;
        uint8_t bestWrap = adv.wrap_around;

        if (bestViol > 0) {
            const std::array<int, 5> octCandidates = {0, -12, 12, -24, 24};
            const std::array<uint8_t, 13> wrapCandidates = {
                adv.wrap_around, 6, 5, 7, 4, 8, 3, 9, 2, 10, 1, 11, 12
            };

            for (int oct : octCandidates) {
                for (uint8_t w : wrapCandidates) {
                    const uint32_t v = norm_detail::countRegisterViolations(res.notes, oct, w, targetLo, targetHi);
                    if (v < bestViol) {
                        bestViol = v;
                        bestOctShift = oct;
                        bestWrap = w;
                        if (bestViol == 0) break;
                    }
                }
                if (bestViol == 0) break;
            }
        }

        adv.wrap_around = bestWrap;
        if (bestOctShift != 0) {
            adv.octave_shift_applied = static_cast<int8_t>(std::clamp(bestOctShift, -24, 24));
            for (auto& n : res.notes) {
                const uint8_t moved = norm_detail::foldMidiPitch(static_cast<int>(n.pitch) + bestOctShift);
                if (moved != n.pitch) {
                    n.pitch = moved;
                    ++res.register_violations_fixed;
                }
            }
        }

        // Fold any remaining outlier notes into the 12-root-safe core band [safeLo, safeHi].
        if (bestViol > 0) {
            const int safeLo = static_cast<int>(targetLo) + (12 - static_cast<int>(adv.wrap_around));
            const int safeHi = static_cast<int>(targetHi) - (static_cast<int>(adv.wrap_around) - 1);
            if (safeLo <= safeHi) {
                for (auto& n : res.notes) {
                    int p = static_cast<int>(n.pitch);
                    const int orig = p;
                    while (p < safeLo && p + 12 <= safeHi) p += 12;
                    while (p > safeHi && p - 12 >= safeLo) p -= 12;
                    while (p < static_cast<int>(targetLo) && p + 12 <= static_cast<int>(targetHi)) p += 12;
                    while (p > static_cast<int>(targetHi) && p - 12 >= static_cast<int>(targetLo)) p -= 12;
                    p = std::clamp(p, 0, 127);
                    if (p != orig) {
                        n.pitch = static_cast<uint8_t>(p);
                        ++res.register_violations_fixed;
                    }
                }
            }
        }

        // 6. Low-Interval Limit (LIL) mud guard on Chord/Pad tracks:
        // Avoid intervals < 7 st below C3 (48) without colliding onto an occupied pitch.
        const bool isBassOrGuitar =
            (korg_ch == kBassCh || role == uasf::TrackRole::Bass || (casm.has_casm && casm.ntr == 2));
        if (!isBassOrGuitar && res.notes.size() >= 2) {
            const int safeHi = static_cast<int>(targetHi) - (static_cast<int>(adv.wrap_around) - 1);
            for (size_t i = 0; i < res.notes.size(); ++i) {
                for (size_t j = i + 1; j < res.notes.size(); ++j) {
                    if (res.notes[i].on_tick != res.notes[j].on_tick) continue;
                    const size_t loIdx = (res.notes[i].pitch <= res.notes[j].pitch) ? i : j;
                    const size_t hiIdx = (loIdx == i) ? j : i;
                    const int loP = static_cast<int>(res.notes[loIdx].pitch);
                    const int hiP = static_cast<int>(res.notes[hiIdx].pitch);
                    if (loP < 48 && hiP > loP && (hiP - loP) < 7) {
                        if (hiP + 12 <= safeHi &&
                            !norm_detail::hasSimultaneousPitch(res.notes, hiIdx, res.notes[hiIdx].on_tick,
                                                               static_cast<uint8_t>(hiP + 12))) {
                            res.notes[hiIdx].pitch = static_cast<uint8_t>(hiP + 12);
                            ++res.register_violations_fixed;
                        } else if (hiP + 24 <= safeHi &&
                                   !norm_detail::hasSimultaneousPitch(res.notes, hiIdx, res.notes[hiIdx].on_tick,
                                                                      static_cast<uint8_t>(hiP + 24))) {
                            res.notes[hiIdx].pitch = static_cast<uint8_t>(hiP + 24);
                            ++res.register_violations_fixed;
                        } else if (hiP + 12 <= safeHi) {
                            res.notes[hiIdx].pitch = static_cast<uint8_t>(hiP + 12);
                            ++res.register_violations_fixed;
                        }
                    }
                }
            }
        }

        // 7. Guarantee zero duplicate (on_tick, pitch) spans after normalization & LIL lifts.
        norm_detail::deduplicateSimultaneousPitches(res.notes);
    }

    return res;
}

}  // namespace ai_arranger::exporters::korg::quality

#endif  // AI_ARRANGER_EXPORTERS_KORG_SOURCE_CHORD_NORMALIZER_H
