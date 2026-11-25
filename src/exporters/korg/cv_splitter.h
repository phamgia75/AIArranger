#ifndef AI_ARRANGER_EXPORTERS_KORG_CV_SPLITTER_H
#define AI_ARRANGER_EXPORTERS_KORG_CV_SPLITTER_H

#include "engine/arranger/chord_input.h"
#include "engine/uasf/types.h"
#include "exporters/korg/korg_style_smf.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Standalone, portable C++20 Yamaha CASM Chord-Mute / Note-Mute (Ctb2[11..17])
// to Korg Pa5X Chord Variation (CV1..CV6) splitter.
// Zero JUCE/GUI dependency; 100% deterministic; every emitted marker is validated
// against korg::isValidMarker().

namespace ai_arranger::exporters::korg::quality {

using ai_arranger::arranger::ChordType;

#ifndef AI_ARRANGER_EXPORTERS_KORG_QUALITY_CASM_TRACK_VIEW_DEFINED
#define AI_ARRANGER_EXPORTERS_KORG_QUALITY_CASM_TRACK_VIEW_DEFINED
// Shared lightweight read-only CASM track metadata view (guarded identically
// in megavoice_sanitizer.h for order-independent inclusion).
struct CasmTrackView {
    uint8_t  src_ch            = 0;
    uint8_t  dest_ch           = 0;               // Ctb2[9] (0..15)
    uint8_t  ntr               = 0;               // Ctb2[28]: 0=RootTrans, 1=RootFixed, 2=Guitar
    uint8_t  ntt               = 0;               // Ctb2[29] & 0x7F
    bool     ntt_bass          = false;           // (Ctb2[29] & 0x80) != 0
    uint8_t  high_key          = 11;              // Ctb2[30]: 0..11
    uint8_t  note_low          = 0;               // Ctb2[31]: 0..127
    uint8_t  note_high         = 127;             // Ctb2[32]: 0..127
    uint8_t  rtr               = 0;               // Ctb2[33]
    uint8_t  source_chord_root = 0;               // Ctb2[18]: 0=C..11=B, 0x7F=unset
    uint8_t  source_chord_type = 0x00;            // Ctb2[19]: Yamaha SFF chord code
    uint16_t note_mute_mask    = 0x0FFF;          // Ctb2[11..12] 12-bit active note mask (bit 0=C .. bit 11=B)
    uint64_t chord_mute_mask   = 0x3FFFFFFFFULL;  // Ctb2[13..17] 34-bit active chord mask (all 1s = all chords)
    bool     has_casm          = false;           // true iff parsed from Ctb2 or SFF1 Ctab/Cntt
};
#endif

// ── Wierzba/Bedesem v2.1 Table 13–14 Exact 34-Bit Chord-Mute Mask Constants ──
// Bit index b (0..33, bit 0 = LSB of Ctb2[17]) corresponds to Yamaha SFF chord code 0x00..0x21:
//   0..7   : Maj, Maj6, Maj7, Maj7#11, Maj(9), Maj7(9), Maj6(9), Aug
//   8..16  : Min, Min6, Min7, Min7b5, Min(9), Min7(9), Min7(11), MinMaj7, MinMaj7(9)
//   17..18 : Dim, Dim7
//   19..27 : 7, 7sus4, 7b5, 7(9), 7(#11), 7(13), 7(b9), 7(b13), 7(#9)
//   28     : Maj7aug
//   29     : 7aug
//   30..31 : 1+8, 1+5
//   32..33 : Sus4, 1+2+5 (Sus2)

inline constexpr uint16_t kMaskAllNotes  = 0x0FFFU;
inline constexpr uint64_t kMaskAllChords = 0x3FFFFFFFFULL;

inline constexpr uint64_t kMaskMajor = 0x0000000FFULL | (1ULL << 28);             // bits 0..7, 28
inline constexpr uint64_t kMaskMinor = 0x00001FF00ULL;                             // bits 8..16
inline constexpr uint64_t kMaskDim   = 0x000060000ULL;                             // bits 17..18
inline constexpr uint64_t kMaskDom7  = (0x1FFULL << 19) | (1ULL << 29);           // bits 19..27, 29
inline constexpr uint64_t kMaskOther = (1ULL << 30) | (1ULL << 31);               // bits 30..31 (1+8, 1+5)
inline constexpr uint64_t kMaskSus   = (1ULL << 32) | (1ULL << 33);               // bits 32..33 (Sus4, Sus2)

static_assert((kMaskMajor | kMaskMinor | kMaskDom7 | kMaskDim | kMaskSus | kMaskOther) == kMaskAllChords,
              "Chord-family masks must partition all 34 Yamaha SFF2 chord-mute bits");
static_assert((kMaskMajor & kMaskMinor) == 0 && (kMaskMajor & kMaskDom7) == 0 &&
              (kMaskMajor & kMaskDim) == 0 && (kMaskMajor & kMaskSus) == 0 &&
              (kMaskMajor & kMaskOther) == 0 && (kMaskMinor & kMaskDom7) == 0 &&
              (kMaskMinor & kMaskDim) == 0 && (kMaskMinor & kMaskSus) == 0 &&
              (kMaskMinor & kMaskOther) == 0 && (kMaskDom7 & kMaskDim) == 0 &&
              (kMaskDom7 & kMaskSus) == 0 && (kMaskDom7 & kMaskOther) == 0 &&
              (kMaskDim & kMaskSus) == 0 && (kMaskDim & kMaskOther) == 0 &&
              (kMaskSus & kMaskOther) == 0,
              "Chord-family masks must be pairwise disjoint");

struct SectionTrackInput {
    const uasf::TrackDefinition* track = nullptr;
    CasmTrackView                casm;
    uint8_t                      korg_ch = 0xFF;  // resolved target Korg channel (8..15)
};

struct CvBucket {
    uint8_t                               cv_index = 1;  // 1..6 for Main, 1..2 for Intro/Fill/End, 0 for Break
    std::string                           marker;        // validated korg marker ("v1cv1", "v1cv2", "i1cv2", "br")
    ChordType                             reference_chord = ChordType::Major;
    std::vector<const SectionTrackInput*> tracks;        // guaranteed mutually compatible on each korg_ch
};

struct CvSplitDiagnostic {
    std::string code;      // "STYLE_QUAL_CHORD_MUTE_SPLIT" | "STYLE_QUAL_CV_SLOTS_EXHAUSTED"
    std::string section;
    uint8_t     korg_ch = 0xFF;
    std::string detail;
};

struct CvSplitPlan {
    std::vector<CvBucket>          buckets;     // deterministic ascending order: cv1, cv2, ...
    std::vector<CvSplitDiagnostic> diagnostics;
    uint32_t                       clashes_prevented = 0;
    uint32_t                       shadowed_tracks_dropped = 0;
};

// Maximum Pa5X CVs permitted by korg::isValidMarker() (korg_style_smf.cpp:34-48):
//   Main1..4   -> 6   (v1cv1..v4cv6)
//   Intro1..3  -> 2   (i1cv1..i3cv2)
//   Fill1..4   -> 2   (f1cv1..f4cv2)
//   Ending1..3 -> 2   (e1cv1..e3cv2)
//   Break      -> 0   ("br" ONLY — NEVER emits CV suffixes!)
inline uint8_t maxCvCountFor(uasf::SectionType type) noexcept {
    switch (type) {
        case uasf::SectionType::Main1:
        case uasf::SectionType::Main2:
        case uasf::SectionType::Main3:
        case uasf::SectionType::Main4:
            return 6;
        case uasf::SectionType::Intro1:
        case uasf::SectionType::Intro2:
        case uasf::SectionType::Intro3:
        case uasf::SectionType::Fill1:
        case uasf::SectionType::Fill2:
        case uasf::SectionType::Fill3:
        case uasf::SectionType::Fill4:
        case uasf::SectionType::Ending1:
        case uasf::SectionType::Ending2:
        case uasf::SectionType::Ending3:
            return 2;
        case uasf::SectionType::Break:
            return 0;
    }
    return 0;
}

// Builds exact validated lowercase Korg marker ("v1cv2", "i2cv1", "br"). Returns "" if invalid.
inline std::string korgMarkerForCv(uasf::SectionType type, uint8_t cv_index) noexcept {
    if (type == uasf::SectionType::Break) {
        return (cv_index == 0 && korg::isValidMarker("br")) ? std::string("br") : std::string();
    }
    const uint8_t max_cv = maxCvCountFor(type);
    if (cv_index < 1 || cv_index > max_cv) return {};

    char prefix = 0;
    char num = 0;
    switch (type) {
        case uasf::SectionType::Main1:   prefix = 'v'; num = '1'; break;
        case uasf::SectionType::Main2:   prefix = 'v'; num = '2'; break;
        case uasf::SectionType::Main3:   prefix = 'v'; num = '3'; break;
        case uasf::SectionType::Main4:   prefix = 'v'; num = '4'; break;
        case uasf::SectionType::Intro1:  prefix = 'i'; num = '1'; break;
        case uasf::SectionType::Intro2:  prefix = 'i'; num = '2'; break;
        case uasf::SectionType::Intro3:  prefix = 'i'; num = '3'; break;
        case uasf::SectionType::Fill1:   prefix = 'f'; num = '1'; break;
        case uasf::SectionType::Fill2:   prefix = 'f'; num = '2'; break;
        case uasf::SectionType::Fill3:   prefix = 'f'; num = '3'; break;
        case uasf::SectionType::Fill4:   prefix = 'f'; num = '4'; break;
        case uasf::SectionType::Ending1: prefix = 'e'; num = '1'; break;
        case uasf::SectionType::Ending2: prefix = 'e'; num = '2'; break;
        case uasf::SectionType::Ending3: prefix = 'e'; num = '3'; break;
        case uasf::SectionType::Break:   return {};
    }

    std::string marker;
    marker.reserve(5);
    marker.push_back(prefix);
    marker.push_back(num);
    marker.push_back('c');
    marker.push_back('v');
    marker.push_back(static_cast<char>('0' + cv_index));
    return korg::isValidMarker(marker) ? marker : std::string();
}

namespace cv_detail {

struct CvSlotMeta {
    uint8_t     cv_index;
    uint64_t    family_mask;
    ChordType   reference_chord;
    const char* label;
};

inline constexpr std::array<CvSlotMeta, 6> kCvSlots = {{
    {1, kMaskMajor, ChordType::Major, "Major"},
    {2, kMaskMinor, ChordType::Minor, "Minor"},
    {3, kMaskDom7,  ChordType::Dom7,  "Dom7"},
    {4, kMaskDim,   ChordType::Dim,   "Dim"},
    {5, kMaskSus,   ChordType::Sus4,  "Sus"},
    {6, kMaskOther, ChordType::Power, "Alt"},
}};

inline uint64_t effectiveChordMask(const SectionTrackInput& t) noexcept {
    return t.casm.chord_mute_mask & kMaskAllChords;
}

inline uint16_t effectiveNoteMask(const SectionTrackInput& t) noexcept {
    return static_cast<uint16_t>(t.casm.note_mute_mask & kMaskAllNotes);
}

inline bool isValidKorgChannel(uint8_t ch) noexcept {
    return ch >= korg::kBassCh && ch <= 15;
}

inline bool areMutuallyExclusive(const SectionTrackInput& a,
                                 const SectionTrackInput& b) noexcept {
    const uint64_t cm_a = effectiveChordMask(a);
    const uint64_t cm_b = effectiveChordMask(b);
    const uint16_t nm_a = effectiveNoteMask(a);
    const uint16_t nm_b = effectiveNoteMask(b);
    return ((cm_a & cm_b) == 0) || ((nm_a & nm_b) == 0);
}

inline bool compatibleWithAll(const SectionTrackInput& candidate,
                              const std::vector<const SectionTrackInput*>& group) noexcept {
    for (const auto* existing : group) {
        if (areMutuallyExclusive(candidate, *existing)) return false;
    }
    return true;
}

struct RankedEntry {
    const SectionTrackInput* ptr = nullptr;
    size_t                   order = 0;
};

// Scores how strongly `entry` fits `family_mask`: prefers dedicated tracks whose
// chord_mute_mask lies exclusively inside `family_mask`, then highest overlap
// count, then highest note_mute coverage, then earliest input order.
inline bool betterForFamily(const RankedEntry& a, const RankedEntry& b,
                            uint64_t family_mask) noexcept {
    const uint64_t ca = effectiveChordMask(*a.ptr);
    const uint64_t cb = effectiveChordMask(*b.ptr);
    const int ov_a = std::popcount(ca & family_mask);
    const int ov_b = std::popcount(cb & family_mask);
    const bool exact_a = (ov_a > 0) && ((ca & ~family_mask) == 0);
    const bool exact_b = (ov_b > 0) && ((cb & ~family_mask) == 0);
    if (exact_a != exact_b) return exact_a;

    // Higher concentration ratio ov / total_bits prefers focused tracks over all-chords tracks.
    const int tot_a = std::max(1, std::popcount(ca));
    const int tot_b = std::max(1, std::popcount(cb));
    const int lhs = ov_a * tot_b;
    const int rhs = ov_b * tot_a;
    if (lhs != rhs) return lhs > rhs;

    if (ov_a != ov_b) return ov_a > ov_b;

    const int nm_a = std::popcount(static_cast<unsigned>(effectiveNoteMask(*a.ptr)));
    const int nm_b = std::popcount(static_cast<unsigned>(effectiveNoteMask(*b.ptr)));
    if (nm_a != nm_b) return nm_a > nm_b;

    return a.order < b.order;
}

} // namespace cv_detail

// Computes a deterministic, conflict-free Pa5X CV split for one section.
inline CvSplitPlan planSectionCvs(
    uasf::SectionType section_type,
    const std::string& section_name,
    const std::vector<SectionTrackInput>& tracks) noexcept {

    CvSplitPlan plan;

    // Collect valid, non-zero-mask tracks grouped by Korg channel (8..15).
    std::vector<cv_detail::RankedEntry> active_tracks;
    active_tracks.reserve(tracks.size());
    std::map<uint8_t, std::vector<cv_detail::RankedEntry>> by_ch;

    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        if (!cv_detail::isValidKorgChannel(t.korg_ch)) continue;
        // Zero-mask guard: explicitly muted everywhere -> drop cleanly.
        if (cv_detail::effectiveChordMask(t) == 0 || cv_detail::effectiveNoteMask(t) == 0) {
            continue;
        }
        cv_detail::RankedEntry entry{&t, i};
        active_tracks.push_back(entry);
        by_ch[t.korg_ch].push_back(entry);
    }

    const uint8_t max_cv = maxCvCountFor(section_type);

    // 1. Break Short-Circuit ("br" ONLY — NEVER emits CV suffixes!).
    if (section_type == uasf::SectionType::Break || max_cv == 0) {
        CvBucket br;
        br.cv_index = 0;
        br.marker = korgMarkerForCv(uasf::SectionType::Break, 0);
        br.reference_chord = ChordType::Major;

        std::vector<cv_detail::RankedEntry> chosen;
        for (const auto& [kch, group] : by_ch) {
            bool has_clash = false;
            for (size_t a = 0; a < group.size() && !has_clash; ++a) {
                for (size_t b = a + 1; b < group.size(); ++b) {
                    if (cv_detail::areMutuallyExclusive(*group[a].ptr, *group[b].ptr)) {
                        has_clash = true;
                        break;
                    }
                }
            }

            if (!has_clash) {
                for (const auto& e : group) chosen.push_back(e);
                continue;
            }

            // Pick Major-active track with highest overlap on kMaskMajor; drop mutually exclusive peers.
            std::vector<cv_detail::RankedEntry> sorted = group;
            std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                const uint64_t ca = cv_detail::effectiveChordMask(*a.ptr);
                const uint64_t cb = cv_detail::effectiveChordMask(*b.ptr);
                const int maj_a = std::popcount(ca & kMaskMajor);
                const int maj_b = std::popcount(cb & kMaskMajor);
                if (maj_a != maj_b) return maj_a > maj_b;
                const int tot_a = std::popcount(ca);
                const int tot_b = std::popcount(cb);
                if (tot_a != tot_b) return tot_a > tot_b;
                const int nm_a = std::popcount(static_cast<unsigned>(cv_detail::effectiveNoteMask(*a.ptr)));
                const int nm_b = std::popcount(static_cast<unsigned>(cv_detail::effectiveNoteMask(*b.ptr)));
                if (nm_a != nm_b) return nm_a > nm_b;
                return a.order < b.order;
            });

            std::vector<const SectionTrackInput*> kept_on_ch;
            for (const auto& e : sorted) {
                if (cv_detail::compatibleWithAll(*e.ptr, kept_on_ch)) {
                    kept_on_ch.push_back(e.ptr);
                    chosen.push_back(e);
                } else {
                    ++plan.shadowed_tracks_dropped;
                    ++plan.clashes_prevented;
                    plan.diagnostics.push_back({
                        "STYLE_QUAL_CV_SLOTS_EXHAUSTED",
                        section_name,
                        kch,
                        "Break ('br') permits 0 CVs; dropped shadowed mutually exclusive src_ch " +
                            std::to_string(e.ptr->casm.src_ch)
                    });
                }
            }
        }

        std::stable_sort(chosen.begin(), chosen.end(),
                         [](const auto& a, const auto& b) { return a.order < b.order; });
        br.tracks.reserve(chosen.size());
        for (const auto& e : chosen) br.tracks.push_back(e.ptr);
        plan.buckets.push_back(std::move(br));
        return plan;
    }

    // 2. Collision & Minor-only detection per Korg channel.
    bool any_collision = false;
    bool any_minor_only = false;
    for (const auto& [kch, group] : by_ch) {
        (void)kch;
        for (size_t a = 0; a < group.size(); ++a) {
            const uint64_t cm = cv_detail::effectiveChordMask(*group[a].ptr);
            if ((cm & kMaskMajor) == 0 && (cm & kMaskMinor) != 0) {
                any_minor_only = true;
            }
            for (size_t b = a + 1; b < group.size(); ++b) {
                if (cv_detail::areMutuallyExclusive(*group[a].ptr, *group[b].ptr)) {
                    any_collision = true;
                }
            }
        }
    }

    // Single cv1 fast-path: zero bloat when no clashes and no Minor-only track exist.
    if (!any_collision && !any_minor_only) {
        CvBucket cv1;
        cv1.cv_index = 1;
        cv1.marker = korgMarkerForCv(section_type, 1);
        cv1.reference_chord = ChordType::Major;
        cv1.tracks.reserve(active_tracks.size());
        for (const auto& e : active_tracks) cv1.tracks.push_back(e.ptr);
        plan.buckets.push_back(std::move(cv1));
        return plan;
    }

    // 3. Deterministic per-channel assignment into candidate slots 0 .. max_cv-1.
    // slot_picks[s][kch] holds the mutually compatible track entries for slot s on channel kch.
    struct ChannelPlan {
        std::array<std::vector<cv_detail::RankedEntry>, 6> slot_tracks;
        std::array<bool, 6>                                slot_has_dedicated{};
        bool                                               had_collision = false;
        bool                                               had_minor_only = false;
    };
    std::map<uint8_t, ChannelPlan> ch_plans;

    for (const auto& [kch, group] : by_ch) {
        ChannelPlan cp;

        for (size_t a = 0; a < group.size(); ++a) {
            const uint64_t cm = cv_detail::effectiveChordMask(*group[a].ptr);
            if ((cm & kMaskMajor) == 0 && (cm & kMaskMinor) != 0) {
                cp.had_minor_only = true;
            }
            for (size_t b = a + 1; b < group.size(); ++b) {
                if (cv_detail::areMutuallyExclusive(*group[a].ptr, *group[b].ptr)) {
                    cp.had_collision = true;
                }
            }
        }

        if (!cp.had_collision) {
            // Non-colliding channel: include each track in every slot whose chord family it covers.
            for (uint8_t s = 0; s < max_cv; ++s) {
                const uint64_t fmask = cv_detail::kCvSlots[s].family_mask;
                for (const auto& e : group) {
                    if ((cv_detail::effectiveChordMask(*e.ptr) & fmask) != 0) {
                        cp.slot_tracks[s].push_back(e);
                    }
                }
            }
            if (cp.had_minor_only) {
                plan.diagnostics.push_back({
                    "STYLE_QUAL_CHORD_MUTE_SPLIT",
                    section_name,
                    kch,
                    "Routed Minor-only track on Korg ch " + std::to_string(kch) + " to cv2"
                });
            }
            ch_plans.emplace(kch, std::move(cp));
            continue;
        }

        // Colliding channel: resolve per-slot winners first using chord-family masks.
        std::set<const SectionTrackInput*> placed;
        for (uint8_t s = 0; s < max_cv; ++s) {
            const uint64_t fmask = cv_detail::kCvSlots[s].family_mask;
            std::vector<cv_detail::RankedEntry> candidates;
            for (const auto& e : group) {
                if ((cv_detail::effectiveChordMask(*e.ptr) & fmask) != 0) {
                    candidates.push_back(e);
                }
            }
            if (candidates.empty()) continue;

            std::stable_sort(candidates.begin(), candidates.end(),
                             [fmask](const auto& a, const auto& b) {
                                 return cv_detail::betterForFamily(a, b, fmask);
                             });

            std::vector<const SectionTrackInput*> kept_ptrs;
            for (const auto& e : candidates) {
                if (cv_detail::compatibleWithAll(*e.ptr, kept_ptrs)) {
                    kept_ptrs.push_back(e.ptr);
                    cp.slot_tracks[s].push_back(e);
                    placed.insert(e.ptr);
                }
            }

            // Mark slot as having a dedicated chord-family winner if its winner is
            // NOT identical to cv1's primary track (or for cv1 itself).
            if (!cp.slot_tracks[s].empty()) {
                if (s == 0) {
                    cp.slot_has_dedicated[0] = true;
                } else {
                    const auto* primary = cp.slot_tracks[s].front().ptr;
                    const bool same_as_cv1 = !cp.slot_tracks[0].empty() &&
                                             cp.slot_tracks[0].front().ptr == primary;
                    if (!same_as_cv1) {
                        cp.slot_has_dedicated[s] = true;
                    }
                }
            }
        }

        // Place residual mutually exclusive tracks (e.g., disjoint note_mute_mask sharing
        // the same chord_mute_mask) into any spare non-cv1 slot without displacing dedicated slots.
        std::vector<uint8_t> spare_order;
        if (max_cv == 6) {
            spare_order = {5, 1, 2, 3, 4}; // Prefer cv6 (Alt) first for residual tracks on Main
        } else if (max_cv == 2) {
            spare_order = {1};             // Use cv2 on Intro/Fill/Ending if free
        }

        uint32_t split_count_on_ch = 0;
        for (const auto& e : group) {
            if (placed.count(e.ptr) != 0) continue;

            const uint64_t cm = cv_detail::effectiveChordMask(*e.ptr);
            bool assigned_residual = false;
            for (uint8_t s : spare_order) {
                if (s >= max_cv) continue;
                if (cp.slot_has_dedicated[s]) continue;
                // On cv6 (Alt, s == 5) any residual mutually exclusive track may reside;
                // on typed chord slots cv2..cv5, require the track to be active on that family.
                const bool slot_compatible =
                    (s == 5) || ((cm & cv_detail::kCvSlots[s].family_mask) != 0);
                if (!slot_compatible) continue;

                cp.slot_tracks[s].clear();
                cp.slot_tracks[s].push_back(e);
                cp.slot_has_dedicated[s] = true;
                placed.insert(e.ptr);
                assigned_residual = true;
                ++split_count_on_ch;
                break;
            }

            if (!assigned_residual) {
                // Overflow: more mutually exclusive variants than max_cv -> drop cleanly!
                ++plan.shadowed_tracks_dropped;
                ++plan.clashes_prevented;
                plan.diagnostics.push_back({
                    "STYLE_QUAL_CV_SLOTS_EXHAUSTED",
                    section_name,
                    kch,
                    "Exceeded max " + std::to_string(max_cv) +
                        " CV slot(s); dropped shadowed src_ch " +
                        std::to_string(e.ptr->casm.src_ch)
                });
            }
        }

        // Count clashes prevented by separating mutually exclusive variants into distinct CVs.
        for (uint8_t s = 1; s < max_cv; ++s) {
            if (cp.slot_has_dedicated[s]) {
                ++split_count_on_ch;
            }
        }

        // Deduplicate count: number of distinct non-cv1 tracks separated from cv1 on this channel.
        std::set<const SectionTrackInput*> cv1_set;
        for (const auto& e : cp.slot_tracks[0]) cv1_set.insert(e.ptr);
        std::set<const SectionTrackInput*> non_cv1_placed;
        for (uint8_t s = 1; s < max_cv; ++s) {
            if (!cp.slot_has_dedicated[s]) continue;
            for (const auto& e : cp.slot_tracks[s]) {
                if (cv1_set.count(e.ptr) == 0) non_cv1_placed.insert(e.ptr);
            }
        }
        if (!non_cv1_placed.empty()) {
            plan.clashes_prevented += static_cast<uint32_t>(non_cv1_placed.size());
            plan.diagnostics.push_back({
                "STYLE_QUAL_CHORD_MUTE_SPLIT",
                section_name,
                kch,
                "Separated " + std::to_string(non_cv1_placed.size()) +
                    " mutually exclusive variant(s) on Korg ch " + std::to_string(kch) +
                    " across Pa5X CVs"
            });
        }
        (void)split_count_on_ch;

        ch_plans.emplace(kch, std::move(cp));
    }

    // 4. Determine minimal set of active CV slots (always cv1, plus only slots that introduce
    // distinct tracks relative to cv1 and earlier active CVs).
    std::array<bool, 6> slot_active{};
    slot_active[0] = true; // cv1 is always created.

    auto trackSetOf = [](const std::vector<cv_detail::RankedEntry>& vec) {
        std::set<const SectionTrackInput*> s;
        for (const auto& e : vec) s.insert(e.ptr);
        return s;
    };

    for (uint8_t s = 1; s < max_cv; ++s) {
        bool needed = false;
        for (const auto& [kch, cp] : ch_plans) {
            (void)kch;
            const auto cur = trackSetOf(cp.slot_tracks[s]);
            if (cur.empty()) continue;

            const auto cv1_set = trackSetOf(cp.slot_tracks[0]);
            if (cur == cv1_set) continue;

            if (s == 1) {
                // cv2 (Minor): created whenever any channel selects a different set from cv1.
                needed = true;
                break;
            }

            // cv3..cv6: created when this slot has a dedicated/new track not already identical
            // to an earlier active CV on this channel.
            bool identical_to_earlier = false;
            for (uint8_t prev = 0; prev < s; ++prev) {
                if (!slot_active[prev]) continue;
                if (cur == trackSetOf(cp.slot_tracks[prev])) {
                    identical_to_earlier = true;
                    break;
                }
            }
            if (!identical_to_earlier && cp.slot_has_dedicated[s]) {
                needed = true;
                break;
            }
        }
        slot_active[s] = needed;
    }

    // 5. Materialize buckets in deterministic ascending cv_index order (cv1, cv2, ...),
    // cloning shared non-colliding tracks across every active CV.
    for (uint8_t s = 0; s < max_cv; ++s) {
        if (!slot_active[s]) continue;

        CvBucket bucket;
        bucket.cv_index = cv_detail::kCvSlots[s].cv_index;
        bucket.marker = korgMarkerForCv(section_type, bucket.cv_index);
        bucket.reference_chord = cv_detail::kCvSlots[s].reference_chord;

        std::vector<cv_detail::RankedEntry> bucket_entries;
        for (const auto& [kch, cp] : ch_plans) {
            (void)kch;
            for (const auto& e : cp.slot_tracks[s]) {
                bucket_entries.push_back(e);
            }
        }

        // Preserve original input order for deterministic SMF emission.
        std::stable_sort(bucket_entries.begin(), bucket_entries.end(),
                         [](const auto& a, const auto& b) { return a.order < b.order; });

        bucket.tracks.reserve(bucket_entries.size());
        for (const auto& e : bucket_entries) {
            bucket.tracks.push_back(e.ptr);
        }

        plan.buckets.push_back(std::move(bucket));
    }

    return plan;
}

} // namespace ai_arranger::exporters::korg::quality

#endif // AI_ARRANGER_EXPORTERS_KORG_CV_SPLITTER_H
