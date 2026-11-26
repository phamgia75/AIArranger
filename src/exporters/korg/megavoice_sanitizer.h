#ifndef AI_ARRANGER_EXPORTERS_KORG_MEGAVOICE_SANITIZER_H
#define AI_ARRANGER_EXPORTERS_KORG_MEGAVOICE_SANITIZER_H

#include "engine/uasf/types.h"
#include "exporters/korg/korg_style_smf.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Standalone, portable C++20 MegaVoice / Super Articulation classifier,
// velocity-zone curve sanitizer, and SFF2 Guitar (NTR=2) translator.
// Zero JUCE/GUI dependency; 100% deterministic and offline.

namespace ai_arranger::exporters::korg::quality {

#ifndef AI_ARRANGER_EXPORTERS_KORG_QUALITY_CASM_TRACK_VIEW_DEFINED
#define AI_ARRANGER_EXPORTERS_KORG_QUALITY_CASM_TRACK_VIEW_DEFINED
// Shared lightweight read-only CASM track metadata view (no dependency on UASF
// serializer internals; guarded identically in cv_splitter.h for order-independent inclusion).
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

enum class MegaVoiceMode : uint8_t {
    Auto,      // Default: apply sanitization ONLY when confidence gate passes (>= 60 pts & no veto)
    Sanitize,  // Force sanitization on any non-drum track (explicit user override)
    Preserve   // Never modify pitches/velocities except mandatory 0..11 chord-event strip (diff/debug mode)
};

enum class InstrumentClass : uint8_t {
    Unknown,
    GuitarPlucked,     // Acoustic/Nylon/Steel/Clean/Jazz/Overdrive/Dist Guitar, Banjo, Harp
    BassPlucked,       // Finger/Pick/Fretless/Slap/Acoustic Bass
    PianoKeyboard,     // Acoustic Piano, Electric Piano, Clav, Organ, Accordion
    StringsOrchestral, // Solo/Ensemble Strings
    BrassWoodwind,     // Trumpet, Trombone, Sax, Flute, Clarinet, Horn
    SynthLeadPad       // Synth Lead, Synth Pad, Synth Bass
};

struct NoteSpan {
    uint32_t on_tick  = 0;
    uint32_t off_tick = 0;   // strictly > on_tick
    uint8_t  pitch    = 60;  // 0..127
    uint8_t  vel      = 80;  // 1..127
    bool     from_guitar_ntr = false;
};

struct MegaVoiceClassification {
    bool            is_megavoice   = false;
    bool            is_sart        = false;
    uint8_t         confidence     = 0;     // 0..100
    InstrumentClass family         = InstrumentClass::Unknown;
    uint8_t         zone_mask      = 0;     // bit0: 1..64, bit1: 65..90, bit2: 91..100, bit3: 101..127
    uint32_t        chord_range_ct = 0;     // notes in 0..11
    uint32_t        high_fx_ct     = 0;     // notes >= 96
    uint32_t        low_ks_ct      = 0;     // notes in 12..(floor-1)
    bool            vetoed         = false; // true if acoustic piano/organ/strings/pad veto fired
    std::string     rationale;              // deterministic single-line explanation
};

struct SanitizeOptions {
    MegaVoiceMode mode = MegaVoiceMode::Auto;
    uint32_t      ppqn = 480;               // used for muted-note gate clamp (<= 1/16) & strum timing (ms)
    uint32_t      tempo_mpqn = 500000;      // used for 5..60 ms guitar strum coherence check
    bool          drop_noise_zone = true;   // true: strip vel 101..127 noise triggers; false: ghost vel 24..30
    bool          emit_guitar_mode_hint = true;
};

struct SanitizeStats {
    uint32_t chord_range_stripped  = 0;  // 0..11
    uint32_t fx_keys_stripped      = 0;  // >= high_ceiling or < low_floor
    uint32_t noise_zone_stripped   = 0;  // vel 101..127 dropped/ghosted
    uint32_t vel_reshaped_notes    = 0;  // notes whose velocity was re-curved
    uint32_t muted_gates_clamped   = 0;  // vel 65..90 gates shortened <= 1/16
    uint32_t guitar_voices_trimmed = 0;  // excess >6 simultaneous voices pruned on NTR=2
    uint32_t guitar_span_folded    = 0;  // notes folded within <=24 st fretboard span
    bool     guitar_mode_candidate = false;
    double   normal_zone_rms_vel   = 0.0; // post-sanitize mean vel of source [1..64]
    double   upper_zone_rms_vel    = 0.0; // post-sanitize mean vel of source [65..127] surviving notes
};

struct SanitizeResult {
    MegaVoiceClassification  classification;
    SanitizeStats            stats;
    std::vector<NoteSpan>    notes;
    std::vector<std::string> diagnostic_codes; // e.g., STYLE_QUAL_MEGAVOICE_VEL_ZONES
};

namespace detail {

inline bool isDrumRole(uasf::TrackRole role) noexcept {
    return role == uasf::TrackRole::Drum || role == uasf::TrackRole::Percussion;
}

inline std::string toLowerAscii(std::string_view sv) noexcept {
    std::string out;
    out.reserve(sv.size());
    for (const char c : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

inline bool containsAny(const std::string& lower_str,
                        std::initializer_list<const char*> needles) noexcept {
    for (const char* needle : needles) {
        if (lower_str.find(needle) != std::string::npos) return true;
    }
    return false;
}

inline const char* instrumentClassName(InstrumentClass cls) noexcept {
    switch (cls) {
        case InstrumentClass::Unknown:           return "Unknown";
        case InstrumentClass::GuitarPlucked:     return "GuitarPlucked";
        case InstrumentClass::BassPlucked:       return "BassPlucked";
        case InstrumentClass::PianoKeyboard:     return "PianoKeyboard";
        case InstrumentClass::StringsOrchestral: return "StringsOrchestral";
        case InstrumentClass::BrassWoodwind:     return "BrassWoodwind";
        case InstrumentClass::SynthLeadPad:      return "SynthLeadPad";
    }
    return "Unknown";
}

inline const char* sff2GuitarNttName(uint8_t ntt) noexcept {
    switch (ntt) {
        case 0: return "All-Purpose";
        case 1: return "Stroke";
        case 2: return "Arpeggio";
        default: return "Other";
    }
}

inline bool isMegaVoiceSignal1(int yamaha_msb, int yamaha_lsb,
                               const std::string& lower_name) noexcept {
    const bool mega_bank = (yamaha_msb == 63 && (yamaha_lsb == 120 || yamaha_lsb == 60)) ||
                           (yamaha_msb == 8 && yamaha_lsb == 60);
    return mega_bank || (lower_name.find("mega") != std::string::npos);
}

inline bool isSArtSignal2(int yamaha_msb, int yamaha_lsb,
                          const std::string& lower_name) noexcept {
    const bool sart_bank = (yamaha_msb == 8 && yamaha_lsb >= 64 && yamaha_lsb <= 127);
    return sart_bank || containsAny(lower_name, {"s.art", "sa2"});
}

inline InstrumentClass inferInstrumentClass(
    int yamaha_msb, int yamaha_lsb, int yamaha_pc,
    const std::string& lower_name,
    uasf::TrackRole role,
    const CasmTrackView& casm) noexcept {

    // 1. Confirmed Yamaha MegaVoice bank defaults (name can refine if explicit).
    if (yamaha_msb == 63 && yamaha_lsb == 120) {
        if (containsAny(lower_name, {"bass"})) return InstrumentClass::BassPlucked;
        return InstrumentClass::GuitarPlucked;
    }
    if (yamaha_msb == 63 && yamaha_lsb == 60) {
        if (containsAny(lower_name, {"guitar", "gtr"})) return InstrumentClass::GuitarPlucked;
        return InstrumentClass::BassPlucked;
    }

    // 2. Track name keywords (ordered to resolve compounds like "synth bass", "harpsi" vs "harp",
    // "organ" vs "reed", and "tbn bs").
    if (!lower_name.empty()) {
        if (containsAny(lower_name, {"synth bass", "synbs", "syn bs"})) {
            return InstrumentClass::SynthLeadPad;
        }
        if (containsAny(lower_name, {"harpsi", "piano", "pno", "grand", "e.p", "ep ", "ep.",
                                     "rhodes", "wurli", "clav", "organ", "org",
                                     "accord", "celesta", "glock", "vibraph",
                                     "marimba", "xyloph", "keys", "kbd"})) {
            return InstrumentClass::PianoKeyboard;
        }
        if (containsAny(lower_name, {"brass", "trumpet", "trp", "tpt", "tp ",
                                     "trombone", "tbn", "trb", "horn", "tuba",
                                     "sax", "flute", "flt", "clarinet", "oboe",
                                     "bassoon", "piccolo", "woodwind", "reed"})) {
            return InstrumentClass::BrassWoodwind;
        }
        if (containsAny(lower_name, {"guitar", "gtr", "gt.", "gt ", "acgt", "clngt",
                                     "nylon", "steel", "banjo", "harp", "ukulele",
                                     "mandolin", "sitar", "plucked", "strum"})) {
            return InstrumentClass::GuitarPlucked;
        }
        if (containsAny(lower_name, {"bass", "fingbs", "pickbs", "slapbs",
                                     "acbs", "fretless", "upright"})) {
            return InstrumentClass::BassPlucked;
        }
        if (containsAny(lower_name, {"string", "str", "violin", "viola",
                                     "cello", "fiddle", "orch", "pizz"})) {
            return InstrumentClass::StringsOrchestral;
        }
        if (containsAny(lower_name, {"synth", "syn", "lead", "pad", "saw",
                                     "square", "choir", "vox"})) {
            return InstrumentClass::SynthLeadPad;
        }
    }

    // 3. SFF2 Guitar CASM NTR == 2 implies GuitarPlucked.
    if (casm.has_casm && casm.ntr == 2) {
        return InstrumentClass::GuitarPlucked;
    }

    // 4. GM / XG / Yamaha Panel Program Change family (exclude Drum/Mega banks).
    const bool is_non_gm_bank = (yamaha_msb == 63 || yamaha_msb == 62 ||
                                 yamaha_msb == 127 || yamaha_msb == 126);
    if (!is_non_gm_bank && yamaha_pc >= 0 && yamaha_pc <= 127) {
        // 0..7 Piano, 8..15 Chromatic Percussion (Celesta/Glockenspiel/Vibes/Marimba/Bells), 16..23 Organ
        if (yamaha_pc >= 0 && yamaha_pc <= 23) {
            return InstrumentClass::PianoKeyboard;
        }
        if ((yamaha_pc >= 24 && yamaha_pc <= 31) || yamaha_pc == 46 ||
            (yamaha_pc >= 104 && yamaha_pc <= 107)) {
            return InstrumentClass::GuitarPlucked;
        }
        if (yamaha_pc >= 32 && yamaha_pc <= 37) {
            return InstrumentClass::BassPlucked;
        }
        if (yamaha_pc == 38 || yamaha_pc == 39) {
            return InstrumentClass::SynthLeadPad;
        }
        if ((yamaha_pc >= 40 && yamaha_pc <= 45) || (yamaha_pc >= 48 && yamaha_pc <= 51)) {
            return InstrumentClass::StringsOrchestral;
        }
        // 52..55 Choir Aahs, Voice Oohs, Synth Voice, Orchestra Hit
        if (yamaha_pc >= 52 && yamaha_pc <= 55) {
            return InstrumentClass::SynthLeadPad;
        }
        if (yamaha_pc >= 56 && yamaha_pc <= 79) {
            return InstrumentClass::BrassWoodwind;
        }
        if (yamaha_pc >= 80 && yamaha_pc <= 95) {
            return InstrumentClass::SynthLeadPad;
        }
    }

    // 5. TrackRole fallback.
    if (role == uasf::TrackRole::Bass) return InstrumentClass::BassPlucked;
    if (role == uasf::TrackRole::Pad)  return InstrumentClass::SynthLeadPad;

    return InstrumentClass::Unknown;
}

// Low key-switch floor: pitches in 12..(floor - 1) are treated as key-switches.
inline uint8_t lowKeySwitchFloor(InstrumentClass cls, bool is_megavoice_ctx) noexcept {
    switch (cls) {
        case InstrumentClass::GuitarPlucked:     return 36; // C2
        case InstrumentClass::BassPlucked:       return 24; // C1
        case InstrumentClass::BrassWoodwind:     return 36; // C2
        case InstrumentClass::StringsOrchestral: return 31; // G1
        case InstrumentClass::PianoKeyboard:     return 21; // A0
        case InstrumentClass::SynthLeadPad:      return 24; // C1
        case InstrumentClass::Unknown:           return is_megavoice_ctx ? 24 : 12;
    }
    return 12;
}

// High FX ceiling: pitches >= ceiling are stripped when MegaVoice sanitization is active.
inline uint8_t highFxCeiling(InstrumentClass cls, bool is_megavoice_ctx) noexcept {
    switch (cls) {
        case InstrumentClass::GuitarPlucked:     return 96;  // C7
        case InstrumentClass::BassPlucked:       return 84;  // C6
        case InstrumentClass::BrassWoodwind:     return 108; // C8
        case InstrumentClass::StringsOrchestral: return 108; // C8
        case InstrumentClass::PianoKeyboard:     return 109; // C#8 (preserves full 88-key 21..108)
        case InstrumentClass::SynthLeadPad:      return 108; // C8
        case InstrumentClass::Unknown:           return is_megavoice_ctx ? 96 : 128;
    }
    return 128;
}

// Piecewise-linear half-up integer velocity mapping for MegaVoice zones 1..4.
// Guarantees unconditionally: max(surviving 65..127) <= 66 < 68 <= min(1..64).
inline uint8_t mapMegaVoiceVelocity(uint8_t src_vel) noexcept {
    if (src_vel <= 64) {
        // Zone 1 [1..64] -> [68..106]
        const uint32_t v = src_vel < 1 ? 1u : static_cast<uint32_t>(src_vel);
        const uint32_t num = (v - 1u) * (106u - 68u);
        return static_cast<uint8_t>(68u + (num * 2u + 63u) / (2u * 63u));
    }
    if (src_vel <= 90) {
        // Zone 2 [65..90] -> [38..56]
        const uint32_t num = static_cast<uint32_t>(src_vel - 65u) * (56u - 38u);
        return static_cast<uint8_t>(38u + (num * 2u + 25u) / (2u * 25u));
    }
    if (src_vel <= 100) {
        // Zone 3 [91..100] -> [50..66]
        const uint32_t num = static_cast<uint32_t>(src_vel - 91u) * (66u - 50u);
        return static_cast<uint8_t>(50u + (num * 2u + 9u) / (2u * 9u));
    }
    // Zone 4 [101..127] ghost mode -> [24..30]
    const uint32_t v = src_vel > 127 ? 127u : static_cast<uint32_t>(src_vel);
    const uint32_t num = (v - 101u) * (30u - 24u);
    return static_cast<uint8_t>(24u + (num * 2u + 26u) / (2u * 26u));
}

inline bool isBimodalOrSteppedVelocity(const std::array<uint32_t, 128>& hist,
                                       uint32_t total_notes,
                                       uint32_t z1_ct,
                                       uint32_t z2_ct,
                                       uint32_t z34_ct) noexcept {
    if (total_notes == 0) return false;

    uint32_t distinct = 0;
    int prev_vel = -1;
    int max_gap = 0;
    for (int v = 1; v <= 127; ++v) {
        if (hist[static_cast<size_t>(v)] > 0) {
            ++distinct;
            if (prev_vel >= 0) {
                const int gap = v - prev_vel - 1;
                if (gap > max_gap) max_gap = gap;
            }
            prev_vel = v;
        }
    }

    // (a) Explicit velocity gap >= 8 between active clusters (stepped zone programming).
    if (max_gap >= 8) return true;

    // (b) Bimodal valley: empty middle zone [65..90] or middle density < half of both outer zones.
    if (z1_ct > 0 && z34_ct > 0) {
        if (z2_ct == 0) return true;
        const double d1 = static_cast<double>(z1_ct) / 64.0;
        const double d2 = static_cast<double>(z2_ct) / 26.0;
        const double d34 = static_cast<double>(z34_ct) / 37.0;
        if (d2 * 2.0 < d1 && d2 * 2.0 < d34) return true;
    }

    // (c) Quantized/stepped distribution across >=3 zones with few distinct velocities.
    if (total_notes >= 4 && distinct <= 10) return true;

    return false;
}

inline void addUniqueCode(std::vector<std::string>& codes, const char* code) noexcept {
    for (const auto& existing : codes) {
        if (existing == code) return;
    }
    codes.emplace_back(code);
}

inline uint32_t msToTicks(uint64_t ms, uint32_t ppqn, uint32_t tempo_mpqn) noexcept {
    const uint64_t safe_ppqn = (ppqn == 0) ? 480ULL : static_cast<uint64_t>(ppqn);
    const uint64_t safe_mpqn = (tempo_mpqn == 0) ? 500000ULL : static_cast<uint64_t>(tempo_mpqn);
    const uint64_t ticks = (ms * 1000ULL * safe_ppqn) / safe_mpqn;
    if (ticks > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(ticks);
}

// Applies SFF2 Guitar (NTR == 2) fretboard span folding, strum monotonicity,
// and 6-string polyphony cap. Tracks source velocity zone metadata alongside each note.
struct TrackedNote {
    NoteSpan note;
    uint8_t  src_vel = 80;
};

inline void applySff2GuitarTranslator(std::vector<TrackedNote>& items,
                                      const CasmTrackView& casm,
                                      const SanitizeOptions& opt,
                                      SanitizeStats& stats) noexcept {
    if (items.empty()) return;

    const uint32_t strum_window = std::max<uint32_t>(1u, msToTicks(60, opt.ppqn, opt.tempo_mpqn));
    const uint32_t stagger_step = std::max<uint32_t>(1u, msToTicks(12, opt.ppqn, opt.tempo_mpqn));

    for (auto& item : items) {
        item.note.from_guitar_ntr = true;
    }

    // Deterministic order by onset, then pitch, then offset.
    std::stable_sort(items.begin(), items.end(), [](const TrackedNote& a, const TrackedNote& b) {
        if (a.note.on_tick != b.note.on_tick) return a.note.on_tick < b.note.on_tick;
        if (a.note.pitch != b.note.pitch)     return a.note.pitch < b.note.pitch;
        return a.note.off_tick < b.note.off_tick;
    });

    uint32_t max_end_tick = 0;
    for (const auto& it : items) {
        if (it.note.off_tick > max_end_tick) max_end_tick = it.note.off_tick;
    }

    // 1. Detect strum clusters (adjacent on_tick deltas <= 60 ms).
    struct ClusterRange { size_t begin; size_t end; };
    std::vector<ClusterRange> clusters;
    size_t c_start = 0;
    for (size_t i = 1; i < items.size(); ++i) {
        if (items[i].note.on_tick - items[i - 1].note.on_tick > strum_window) {
            clusters.push_back({c_start, i});
            c_start = i;
        }
    }
    clusters.push_back({c_start, items.size()});

    // 2. Per-cluster: (a) fretboard span folding (<= 24 st inside [40, 88]),
    //                 (b) same-pitch deduplication & 6-string cap (<= 6 voices),
    //                 (c) Stroke (NTT == 1) collapsed-chord down-strum stagger on surviving <= 6 voices.
    constexpr int kGuitarMin = 40; // E2
    constexpr int kGuitarMax = 88; // E6
    constexpr int kMaxSpan   = 24; // 2 octaves playable fretboard span

    std::vector<bool> keep(items.size(), true);

    for (const auto& cr : clusters) {
        const size_t count = cr.end - cr.begin;
        if (count < 2) continue;

        // 2a. Check fretboard span.
        std::vector<int> pitches;
        pitches.reserve(count);
        int min_p = 127;
        int max_p = 0;
        for (size_t i = cr.begin; i < cr.end; ++i) {
            const int p = static_cast<int>(items[i].note.pitch);
            pitches.push_back(p);
            if (p < min_p) min_p = p;
            if (p > max_p) max_p = p;
        }

        if (max_p - min_p > kMaxSpan) {
            std::sort(pitches.begin(), pitches.end());
            const int median = pitches[pitches.size() / 2];
            const int lo = std::clamp(median - 12, kGuitarMin, kGuitarMax - kMaxSpan);
            const int hi = lo + kMaxSpan; // always <= kGuitarMax (88)

            // Collect pitches already inside [lo, hi] so folded notes avoid collisions.
            std::set<int> used;
            for (size_t i = cr.begin; i < cr.end; ++i) {
                const int p = static_cast<int>(items[i].note.pitch);
                if (p >= lo && p <= hi) used.insert(p);
            }

            for (size_t i = cr.begin; i < cr.end; ++i) {
                const int orig = static_cast<int>(items[i].note.pitch);
                if (orig >= lo && orig <= hi) continue;

                int cand = orig;
                while (cand < lo) cand += 12;
                while (cand > hi) cand -= 12;
                cand = std::clamp(cand, kGuitarMin, kGuitarMax);

                // Avoid colliding on an already-sounding pitch if another valid octave in [lo, hi] is free.
                if (used.count(cand) != 0) {
                    if (cand + 12 <= hi && used.count(cand + 12) == 0) {
                        cand += 12;
                    } else if (cand - 12 >= lo && used.count(cand - 12) == 0) {
                        cand -= 12;
                    }
                }

                if (cand != orig) {
                    items[i].note.pitch = static_cast<uint8_t>(cand);
                    ++stats.guitar_span_folded;
                }
                used.insert(cand);
            }
        }

        // 2b. Per-cluster same-pitch deduplication (keep most recent) & 6-string cap
        //     BEFORE Stroke stagger so step budget & spacing reflect the <= 6 surviving strings.
        std::vector<size_t> unique_idx;
        unique_idx.reserve(count);
        for (size_t i = cr.begin; i < cr.end; ++i) {
            bool dup = false;
            for (size_t& u : unique_idx) {
                if (items[u].note.pitch == items[i].note.pitch) {
                    keep[u] = false;
                    ++stats.guitar_voices_trimmed;
                    u = i;
                    dup = true;
                    break;
                }
            }
            if (!dup) unique_idx.push_back(i);
        }

        if (unique_idx.size() > 6) {
            std::vector<size_t> by_pitch = unique_idx;
            std::stable_sort(by_pitch.begin(), by_pitch.end(), [&](size_t a, size_t b) {
                if (items[a].note.pitch != items[b].note.pitch) {
                    return items[a].note.pitch < items[b].note.pitch;
                }
                return items[a].note.on_tick > items[b].note.on_tick;
            });

            std::set<size_t> survivors;
            survivors.insert(by_pitch.front()); // lowest bass string
            for (size_t k = 0; k < 5; ++k) {
                survivors.insert(by_pitch[by_pitch.size() - 1 - k]); // top 5 upper strings
            }

            for (size_t idx : unique_idx) {
                if (survivors.count(idx) == 0) {
                    keep[idx] = false;
                    ++stats.guitar_voices_trimmed;
                }
            }
        }

        // Collect surviving indices for this cluster (<= 6 distinct pitches).
        std::vector<size_t> surv;
        surv.reserve(6);
        for (size_t i = cr.begin; i < cr.end; ++i) {
            if (keep[i]) surv.push_back(i);
        }

        // 2c. Strum monotonicity preservation / Stroke (NTT == 1) collapsed-chord stagger.
        if (casm.ntt == 1 && surv.size() >= 2) {
            uint32_t first_on = items[surv.front()].note.on_tick;
            uint32_t last_on = items[surv.front()].note.on_tick;
            for (size_t idx : surv) {
                if (items[idx].note.on_tick < first_on) first_on = items[idx].note.on_tick;
                if (items[idx].note.on_tick > last_on)  last_on  = items[idx].note.on_tick;
            }
            const uint32_t spread = last_on - first_on;

            if (spread == 0) {
                // Sort surviving strings ascending by pitch for even down-strum (bass string first).
                std::stable_sort(surv.begin(), surv.end(), [&](size_t a, size_t b) {
                    return items[a].note.pitch < items[b].note.pitch;
                });

                const uint32_t max_steps = static_cast<uint32_t>(surv.size() - 1);
                // Keep total spread <= 60 ms and within available end tick budget.
                uint32_t step = std::min<uint32_t>(stagger_step, strum_window / max_steps);
                if (max_end_tick > first_on + 1u) {
                    const uint32_t end_budget = max_end_tick - 1u - first_on;
                    step = std::min<uint32_t>(step, end_budget / max_steps);
                } else {
                    step = 0;
                }

                if (step > 0) {
                    for (size_t k = 0; k < surv.size(); ++k) {
                        auto& n = items[surv[k]].note;
                        const uint32_t delta = static_cast<uint32_t>(k) * step;
                        const uint32_t desired_on = first_on + delta;
                        if (desired_on < n.off_tick) {
                            n.on_tick = desired_on;
                        } else if (n.off_tick > first_on + 1u) {
                            n.on_tick = n.off_tick - 1u;
                        }
                    }
                }
            }
        }
    }

    // 3. Compact cluster-filtered notes and re-sort deterministically before cross-cluster sweep.
    std::vector<TrackedNote> filtered;
    filtered.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (keep[i]) filtered.push_back(items[i]);
    }

    std::stable_sort(filtered.begin(), filtered.end(), [](const TrackedNote& a, const TrackedNote& b) {
        if (a.note.on_tick != b.note.on_tick) return a.note.on_tick < b.note.on_tick;
        if (a.note.pitch != b.note.pitch)     return a.note.pitch < b.note.pitch;
        return a.note.off_tick < b.note.off_tick;
    });

    // 4. Cross-cluster active overlap sweep: ensure <= 6 simultaneous notes at every tick
    //    and zero same-pitch overlap.
    std::vector<size_t> active; // indices into `filtered`
    std::vector<bool> final_keep(filtered.size(), true);
    for (size_t i = 0; i < filtered.size(); ++i) {
        const uint32_t t = filtered[i].note.on_tick;
        // Drop notes that have already finished at or before `t`.
        active.erase(std::remove_if(active.begin(), active.end(), [&](size_t idx) {
            return !final_keep[idx] || filtered[idx].note.off_tick <= t;
        }), active.end());

        // Prevent same-pitch overlap from an earlier cluster by closing the earlier voice at `t`.
        for (size_t j = 0; j < active.size(); ) {
            const size_t idx = active[j];
            if (filtered[idx].note.pitch == filtered[i].note.pitch) {
                if (filtered[idx].note.on_tick < t) {
                    filtered[idx].note.off_tick = t;
                } else {
                    final_keep[idx] = false;
                    ++stats.guitar_voices_trimmed;
                }
                active.erase(active.begin() + static_cast<std::ptrdiff_t>(j));
            } else {
                ++j;
            }
        }

        active.push_back(i);

        // If > 6 voices overlap simultaneously, trim oldest overlapping voice(s).
        while (active.size() > 6) {
            size_t victim_pos = 0;
            // Find the oldest active voice (smallest on_tick, excluding current note i).
            for (size_t j = 1; j < active.size(); ++j) {
                if (active[j] == i) continue;
                if (filtered[active[j]].note.on_tick < filtered[active[victim_pos]].note.on_tick) {
                    victim_pos = j;
                }
            }
            const size_t victim = active[victim_pos];
            if (filtered[victim].note.on_tick < t) {
                // Close the ringing old string cleanly at the new chord attack.
                filtered[victim].note.off_tick = t;
            } else {
                final_keep[victim] = false;
            }
            ++stats.guitar_voices_trimmed;
            active.erase(active.begin() + static_cast<std::ptrdiff_t>(victim_pos));
        }
    }

    items.clear();
    items.reserve(filtered.size());
    for (size_t i = 0; i < filtered.size(); ++i) {
        if (final_keep[i] && filtered[i].note.off_tick > filtered[i].note.on_tick) {
            items.push_back(filtered[i]);
        }
    }
}

} // namespace detail

inline MegaVoiceClassification classifyTrackArticulation(
    const std::vector<NoteSpan>& notes,
    int yamaha_msb, int yamaha_lsb, int yamaha_pc,
    const std::string& track_name,
    uasf::TrackRole role,
    const CasmTrackView& casm) noexcept {

    MegaVoiceClassification out;
    if (detail::isDrumRole(role)) {
        out.family = InstrumentClass::Unknown;
        out.rationale = "drum/percussion track bypassed";
        return out;
    }

    const std::string lower_name = detail::toLowerAscii(track_name);
    out.family = detail::inferInstrumentClass(yamaha_msb, yamaha_lsb, yamaha_pc,
                                              lower_name, role, casm);

    // For Unknown family, use floor 24 when scanning low key-switches (12..23).
    const uint8_t scan_low_floor = detail::lowKeySwitchFloor(out.family, /*is_megavoice_ctx=*/true);

    std::array<uint32_t, 128> vel_hist{};
    uint32_t valid_notes = 0;
    uint32_t z1_ct = 0;
    uint32_t z2_ct = 0;
    uint32_t z34_ct = 0;

    for (const auto& n : notes) {
        if (n.pitch > 127 || n.vel == 0 || n.vel > 127 ||
            n.on_tick == std::numeric_limits<uint32_t>::max()) {
            continue;
        }
        ++valid_notes;
        ++vel_hist[n.vel];

        if (n.vel <= 64) {
            out.zone_mask |= 0x01u;
            ++z1_ct;
        } else if (n.vel <= 90) {
            out.zone_mask |= 0x02u;
            ++z2_ct;
        } else if (n.vel <= 100) {
            out.zone_mask |= 0x04u;
            ++z34_ct;
        } else {
            out.zone_mask |= 0x08u;
            ++z34_ct;
        }

        if (n.pitch <= kChordEventNoteMax) {
            ++out.chord_range_ct;
        } else if (n.pitch < scan_low_floor) {
            ++out.low_ks_ct;
        }
        if (n.pitch >= 96) {
            ++out.high_fx_ct;
        }
    }

    // Additive Confidence Gate (0..100) + Hard Non-MegaVoice Veto.
    const bool has_s1 = detail::isMegaVoiceSignal1(yamaha_msb, yamaha_lsb, lower_name);
    const bool has_s2 = detail::isSArtSignal2(yamaha_msb, yamaha_lsb, lower_name);
    const bool plucked_or_unknown = (out.family == InstrumentClass::GuitarPlucked ||
                                     out.family == InstrumentClass::BassPlucked ||
                                     out.family == InstrumentClass::Unknown);
    const bool has_s3 = (out.high_fx_ct >= 2) && plucked_or_unknown;
    const bool has_s4 = (out.chord_range_ct >= 1);

    const int zone_count = std::popcount(static_cast<unsigned>(out.zone_mask));
    const bool has_s5 = (valid_notes > 0) &&
                        (zone_count >= 3) &&
                        (static_cast<uint64_t>(z1_ct) * 100ULL >= static_cast<uint64_t>(valid_notes) * 40ULL) &&
                        (static_cast<uint64_t>(z34_ct) * 100ULL >= static_cast<uint64_t>(valid_notes) * 5ULL) &&
                        detail::isBimodalOrSteppedVelocity(vel_hist, valid_notes, z1_ct, z2_ct, z34_ct);

    const bool has_s6 = casm.has_casm && casm.ntr == 2;

    const bool in_veto_family = (out.family == InstrumentClass::PianoKeyboard ||
                                 out.family == InstrumentClass::StringsOrchestral ||
                                 out.family == InstrumentClass::SynthLeadPad);
    out.vetoed = in_veto_family && !has_s1 && out.chord_range_ct == 0 && out.low_ks_ct == 0;

    int score_mega = 0;
    if (has_s1) score_mega += 75;
    if (has_s3) score_mega += 45;
    if (has_s4) score_mega += 25;
    if (has_s5) score_mega += 25;
    if (has_s6) score_mega += 20;
    if (score_mega > 100) score_mega = 100;

    out.is_megavoice = !out.vetoed && !(has_s2 && !has_s1) && (score_mega >= 60);

    const bool sart_family = (out.family == InstrumentClass::BrassWoodwind ||
                              out.family == InstrumentClass::StringsOrchestral ||
                              out.family == InstrumentClass::GuitarPlucked);
    out.is_sart = !out.is_megavoice && (has_s2 || (out.low_ks_ct >= 2 && sart_family));

    if (out.is_megavoice) {
        out.confidence = static_cast<uint8_t>(score_mega);
    } else if (out.is_sart) {
        const int sart_score = has_s2 ? 55 : 40;
        out.confidence = static_cast<uint8_t>(std::min(100, std::max(out.vetoed ? 0 : score_mega, sart_score)));
    } else if (out.vetoed) {
        out.confidence = 0;
    } else {
        out.confidence = static_cast<uint8_t>(score_mega);
    }

    // Build deterministic single-line rationale.
    std::string sigs;
    auto addSig = [&](const char* tag) {
        if (!sigs.empty()) sigs.push_back(',');
        sigs += tag;
    };
    if (has_s1) addSig("S1+75");
    if (has_s2) addSig("S2+55");
    if (has_s3) addSig("S3+45");
    if (has_s4) addSig("S4+25");
    if (has_s5) addSig("S5+25");
    if (has_s6) addSig("S6+20");

    out.rationale = std::string("family=") + detail::instrumentClassName(out.family) +
                    " score=" + std::to_string(out.confidence);
    if (!sigs.empty()) {
        out.rationale += " (" + sigs + ")";
    }
    if (out.vetoed) {
        out.rationale += " vetoed=true";
    }
    if (out.is_megavoice) {
        out.rationale += " megavoice=true";
    } else if (out.is_sart) {
        out.rationale += " sart=true";
    }
    if (has_s6) {
        out.rationale += std::string(" sff2_guitar=") + detail::sff2GuitarNttName(casm.ntt);
    }

    return out;
}

inline SanitizeResult sanitizeMegaVoiceTrack(
    const std::vector<NoteSpan>& notes,
    int yamaha_msb, int yamaha_lsb, int yamaha_pc,
    const std::string& track_name,
    uasf::TrackRole role,
    const CasmTrackView& casm,
    const SanitizeOptions& opt = {}) noexcept {

    SanitizeResult res;
    res.classification = classifyTrackArticulation(notes, yamaha_msb, yamaha_lsb, yamaha_pc,
                                                   track_name, role, casm);

    const uint32_t safe_ppqn = (opt.ppqn == 0) ? 480u : opt.ppqn;
    const uint32_t mute_max_dur = std::max<uint32_t>(1u, safe_ppqn / 4u);
    const uint32_t ghost_max_dur = std::max<uint32_t>(1u, safe_ppqn / 8u);

    // Drum / Percussion tracks are never chord-driven or MegaVoice-reshaped.
    if (detail::isDrumRole(role)) {
        res.notes.reserve(notes.size());
        for (auto n : notes) {
            if (n.pitch > 127 || n.vel == 0 || n.vel > 127) continue;
            if (n.on_tick == std::numeric_limits<uint32_t>::max()) continue;
            if (n.off_tick <= n.on_tick) n.off_tick = n.on_tick + 1u;
            res.notes.push_back(n);
        }
        return res;
    }

    const bool apply_megavoice =
        (opt.mode == MegaVoiceMode::Sanitize) ||
        (opt.mode == MegaVoiceMode::Auto && res.classification.is_megavoice);

    const bool apply_sart =
        (opt.mode == MegaVoiceMode::Auto && !apply_megavoice && res.classification.is_sart);

    const bool is_guitar_ntr = casm.has_casm && casm.ntr == 2;
    const uint8_t low_floor = detail::lowKeySwitchFloor(res.classification.family, apply_megavoice);
    const uint8_t high_ceil = detail::highFxCeiling(res.classification.family, apply_megavoice);

    std::vector<detail::TrackedNote> tracked;
    tracked.reserve(notes.size());

    for (auto n : notes) {
        // Fail-safe guard against malformed MIDI data or uint32 overflow.
        if (n.pitch > 127 || n.vel == 0 || n.vel > 127) continue;
        if (n.on_tick == std::numeric_limits<uint32_t>::max()) continue;
        if (n.off_tick <= n.on_tick) n.off_tick = n.on_tick + 1u;

        // Mandatory 0..11 Chord-Event range strip on ALL chord-driven tracks in ALL modes (including Preserve).
        if (n.pitch <= kChordEventNoteMax) {
            ++res.stats.chord_range_stripped;
            continue;
        }

        if (opt.mode == MegaVoiceMode::Preserve) {
            tracked.push_back({n, n.vel});
            continue;
        }

        if (apply_megavoice) {
            // Strip low key-switches (< floor) and high FX keys (>= ceiling).
            if (n.pitch < low_floor || n.pitch >= high_ceil) {
                ++res.stats.fx_keys_stripped;
                continue;
            }

            const uint8_t src_vel = n.vel;
            if (src_vel >= 101) {
                ++res.stats.noise_zone_stripped;
                if (opt.drop_noise_zone) {
                    continue;
                }
                // Ghost mode: velocity [24..30], duration clamped <= 1/32.
                n.vel = detail::mapMegaVoiceVelocity(src_vel);
                ++res.stats.vel_reshaped_notes;
                if (n.off_tick - n.on_tick > ghost_max_dur) {
                    n.off_tick = n.on_tick + ghost_max_dur;
                }
            } else {
                const uint8_t new_vel = detail::mapMegaVoiceVelocity(src_vel);
                if (new_vel != n.vel) {
                    n.vel = new_vel;
                }
                ++res.stats.vel_reshaped_notes;

                if (src_vel >= 65 && src_vel <= 90) {
                    // Zone 2 muted/dead note: clamp gate <= 1/16.
                    if (n.off_tick - n.on_tick > mute_max_dur) {
                        n.off_tick = n.on_tick + mute_max_dur;
                        ++res.stats.muted_gates_clamped;
                    }
                } else if (src_vel >= 91 && src_vel <= 100) {
                    // Zone 3 harmonics/hammer-on: Bass pitch > 67 folded down 12 st.
                    if (res.classification.family == InstrumentClass::BassPlucked && n.pitch > 67) {
                        n.pitch = static_cast<uint8_t>(n.pitch - 12);
                    }
                }
            }

            tracked.push_back({n, src_vel});
        } else if (apply_sart) {
            // S.Art: strip out-of-range key-switches (< floor) while preserving natural velocities untouched.
            if (n.pitch < low_floor) {
                ++res.stats.fx_keys_stripped;
                continue;
            }
            tracked.push_back({n, n.vel});
        } else {
            // Normal / vetoed track: untouched.
            tracked.push_back({n, n.vel});
        }
    }

    // SFF2 Guitar Translator (NTR == 2) when not in Preserve mode.
    if (is_guitar_ntr) {
        res.stats.guitar_mode_candidate = true;
        if (opt.emit_guitar_mode_hint) {
            detail::addUniqueCode(res.diagnostic_codes, "STYLE_QUAL_SFF2_GUITAR_MODE");
        }
        if (opt.mode != MegaVoiceMode::Preserve) {
            detail::applySff2GuitarTranslator(tracked, casm, opt, res.stats);
        }
    }

    // Emit structured diagnostic codes.
    if (apply_megavoice) {
        if (res.stats.vel_reshaped_notes > 0 || res.stats.noise_zone_stripped > 0) {
            detail::addUniqueCode(res.diagnostic_codes, "STYLE_QUAL_MEGAVOICE_VEL_ZONES");
        }
        if (res.stats.chord_range_stripped > 0 || res.stats.fx_keys_stripped > 0) {
            detail::addUniqueCode(res.diagnostic_codes, "STYLE_QUAL_MEGAVOICE_FX_KEYS");
        }
    } else if (apply_sart) {
        detail::addUniqueCode(res.diagnostic_codes, "STYLE_QUAL_SART_ARTICULATION");
    } else if (res.stats.chord_range_stripped > 0) {
        detail::addUniqueCode(res.diagnostic_codes, "STYLE_QUAL_MEGAVOICE_FX_KEYS");
    }

    // Finalize surviving notes and post-sanitize velocity zone means.
    uint64_t norm_sum = 0;
    uint32_t norm_ct  = 0;
    uint64_t upper_sum = 0;
    uint32_t upper_ct  = 0;

    res.notes.reserve(tracked.size());
    for (const auto& item : tracked) {
        res.notes.push_back(item.note);
        if (item.src_vel <= 64) {
            norm_sum += item.note.vel;
            ++norm_ct;
        } else {
            upper_sum += item.note.vel;
            ++upper_ct;
        }
    }

    res.stats.normal_zone_rms_vel = norm_ct > 0
        ? static_cast<double>(norm_sum) / static_cast<double>(norm_ct)
        : 0.0;
    res.stats.upper_zone_rms_vel = upper_ct > 0
        ? static_cast<double>(upper_sum) / static_cast<double>(upper_ct)
        : 0.0;

    return res;
}

} // namespace ai_arranger::exporters::korg::quality

#endif // AI_ARRANGER_EXPORTERS_KORG_MEGAVOICE_SANITIZER_H
