#ifndef AI_ARRANGER_EXPORTERS_KORG_DRUM_REMAP_H
#define AI_ARRANGER_EXPORTERS_KORG_DRUM_REMAP_H

#include "engine/uasf/types.h"
#include "exporters/korg/korg_style_smf.h"
#include "media/sound/drumkit.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Standalone, portable C++20 Yamaha XG / SFF2 / Revo! Drum extended key remapper,
// Rhythm1/Rhythm2 swap detector, and read-only IDrumKitKeyMap seam (Track B Q3a
// + Track A Next USERDK reuse seam).
// Zero JUCE/GUI dependency; zero dependency on native_refs.h / pa5x_kmp.h /
// slot_conflicts.h (strictly zero circular library deps).

namespace ai_arranger::exporters::korg::quality {

#ifndef AI_ARRANGER_EXPORTERS_KORG_QUALITY_CASM_TRACK_VIEW_DEFINED
#define AI_ARRANGER_EXPORTERS_KORG_QUALITY_CASM_TRACK_VIEW_DEFINED
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
    uint16_t note_mute_mask    = 0x0FFF;          // Ctb2[11..12] 12-bit active note mask
    uint64_t chord_mute_mask   = 0x3FFFFFFFFULL;  // Ctb2[13..17] 34-bit active chord mask
    bool     has_casm          = false;           // true iff parsed from Ctb2 or SFF1 Ctab/Cntt
};
#endif

#if !defined(AI_ARRANGER_EXPORTERS_KORG_QUALITY_NOTE_SPAN_DEFINED) && \
    !defined(AI_ARRANGER_EXPORTERS_KORG_MEGAVOICE_SANITIZER_H)
#define AI_ARRANGER_EXPORTERS_KORG_QUALITY_NOTE_SPAN_DEFINED
struct NoteSpan {
    uint32_t on_tick         = 0;
    uint32_t off_tick        = 0;   // strictly > on_tick
    uint8_t  pitch           = 60;  // 0..127
    uint8_t  vel             = 80;  // 1..127
    bool     from_guitar_ntr = false;
};
#endif

// ── (a) Abstract IDrumKitKeyMap Seam (Factory GM2, TXPERF, and Track A USERDK) ──

// Read-only key-level view of ANY target Pa5X Drum Kit.
// Supports both Q3a's 5 core fields and Track A Next's optional velocity-ceiling /
// ROM(idx==0) placeholder / multi-layer metadata with clean defaults.
struct DrumKeyInfo {
    bool                   populated               = false;
    char                   family                  = 0;     // 'B','S','H','T','C','L','E','X', or '\0'
    uint8_t                layer_count             = 0;     // 0..16
    uint8_t                choke_group             = 0;     // 0 = none, 1..127 (hi-hat = 1)
    std::string            label;                           // optional sample/instrument name
    uint8_t                effective_max_vel       = 127;   // top layer velocity ceiling (velTop[lc-1])
    uint8_t                effective_layers        = 0;     // distinct non-degenerate velocity zones
    bool                   is_ram                  = false; // true iff resolved RAM.KMP layer
    bool                   is_rom_zero_placeholder = false; // true iff ROM(idx==0) unassigned placeholder
    bool                   has_degenerate_vel      = false; // true iff adjacent velTop[l] == velTop[l-1]
    uint8_t                pan                     = 64;    // 0..127, 64 = center
    std::array<uint8_t, 8> vel_tops{};                      // monotonic ascending top velocities
};

class IDrumKitKeyMap {
public:
    virtual ~IDrumKitKeyMap() = default;
    virtual std::string kitName() const = 0;
    virtual bool        hasKey(uint8_t note) const noexcept = 0;
    virtual char        familyOf(uint8_t note) const noexcept = 0;
    virtual DrumKeyInfo keyInfo(uint8_t note) const noexcept = 0;
    virtual bool        isUserdkTarget() const noexcept { return false; }
    virtual double      gmAlignmentScore() const noexcept { return 1.0; }
};

// Maps RAM.KMP DrumRecord byte +113 -> drumkit.h family char ('B'..'X', '\0').
constexpr char kmpDrumFamilyToChar(uint8_t code) noexcept {
    constexpr char kMap[8] = {'B', 'S', 'H', 'T', 'C', 'L', 'E', 'X'};
    return code < 8 ? kMap[code] : '\0';
}

enum class DrumKitCategory : uint8_t {
    StandardPopRock,  // MSB 127/0 PC 0,1,8,16,24; Revo! 62/100 Standard/Pop/Rock/Studio/Acoustic
    BrushJazz,        // MSB 127/0 PC 32 (Jazz), 40 (Brush); Revo! Jazz/Brush; name "brush"/"jazz"
    Electronic,       // MSB 127/0 PC 24 (Electro), 25 (Analog/TR808/909), 26..31; name "808"/"909"/"dance"/"edm"
    LatinPerc,        // MSB 127/0 PC 48 (Symphony/Perc), 126/0 Perc kits; Rhythm1 perc; name "latin"/"perc"/"cuban"
    EthnicArabic,     // MSB 127/0 PC 112..127 / 126/0 Arabic/Oriental/Asian; name "arab"/"orient"/"tabla"/"asian"
    Sfx               // MSB 126/0 PC 0, 1 (SFX1/SFX2); name "sfx"/"fx"
};

namespace detail {

inline char gm2DefaultFamily(uint8_t note) noexcept {
    switch (note) {
        case 35: case 36: return 'B';  // Acoustic/Electric Bass Drum
        case 24:          return 'B';  // Extended Alt Kick
        case 25: case 26: case 28: case 31:
        case 37: case 38: case 39: case 40: return 'S';  // Snare / Side Stick / Clap / Brush
        case 42: case 44: case 46: return 'H';  // Closed / Pedal / Open Hi-Hat
        case 41: case 43: case 45: case 47: case 48: case 50: return 'T';  // Toms
        case 49: case 51: case 52: case 53: case 55: case 57: case 59:
        case 84:          return 'C';  // Cymbals / Belltree
        case 54: case 56: case 58:
        case 60: case 61: case 62: case 63: case 64: case 65: case 66: case 67: case 68:
        case 69: case 70: case 71: case 72: case 73: case 74: case 75: case 76: case 77:
        case 78: case 79: case 80: case 81: case 82: case 83: case 85: case 86: case 87:
            return 'L';  // Latin / Hand Percussion
        case 27: case 29: case 30: case 32: case 33: case 34: return 'X';  // GM2 SFX / Clicks / Scratch
        default:
            if (note >= 88 && note <= 91) return (note <= 89) ? 'S' : 'C';
            if (note >= 92 && note <= 94) return 'L';
            if (note >= 95 && note <= 98) return 'T';
            if (note == 99) return 'C';
            return '\0';
    }
}

// Standard 7 GM2 / Pa5X Exclusive (Choke) Groups.
inline uint8_t gm2DefaultChokeGroup(uint8_t note) noexcept {
    switch (note) {
        case 42: case 44: case 46: return 1;  // Hi-Hat
        case 71: case 72:          return 2;  // Whistle
        case 73: case 74:          return 3;  // Guiro
        case 78: case 79:          return 4;  // Cuica
        case 80: case 81:          return 5;  // Triangle
        case 86: case 87:          return 6;  // Surdo
        case 25: case 29: case 30: return 7;  // Brush / Scratch
        default:                   return 0;
    }
}

inline std::string toLowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline bool containsAny(std::string_view hay,
                        std::initializer_list<std::string_view> needles) noexcept {
    for (std::string_view needle : needles) {
        if (hay.find(needle) != std::string_view::npos) return true;
    }
    return false;
}

inline NoteSpan sanitizeDrumSpan(const NoteSpan& s) noexcept {
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

}  // namespace detail

// ── Concrete Adapter 1: Factory Pa5X GM2 / Extended Drum Kits ────────────────

class FactoryPa5xDrumKitKeyMap final : public IDrumKitKeyMap {
public:
    explicit FactoryPa5xDrumKitKeyMap(DrumKitCategory cat = DrumKitCategory::StandardPopRock)
        : category_(cat) {
        for (uint8_t n = 0; n < 128; ++n) {
            keys_[n] = {};
        }

        if (cat == DrumKitCategory::Sfx) {
            // SFX kits expose GM2 SFX/extended keys 27..34, 82..87 plus standard SFX region 35..81.
            for (uint8_t n = 27; n <= 87; ++n) {
                populateKey(n, (n >= 27 && n <= 34) ? 'X' : detail::gm2DefaultFamily(n),
                            detail::gm2DefaultChokeGroup(n), 2, "Pa5X SFX");
            }
            return;
        }

        // Core GM2 / Korg Extended populated range 24..99.
        for (uint8_t n = 24; n <= 99; ++n) {
            char fam = detail::gm2DefaultFamily(n);
            if (cat == DrumKitCategory::BrushJazz && (n >= 25 && n <= 28)) {
                fam = 'S';  // Brush Tap / Swirl / Slap / Sweep
            } else if (cat == DrumKitCategory::EthnicArabic && (n >= 60 && n <= 87)) {
                fam = 'E';
            }
            if (fam == '\0') fam = 'X';
            populateKey(n, fam, detail::gm2DefaultChokeGroup(n), 4, "Pa5X Factory");
        }
    }

    DrumKitCategory category() const noexcept { return category_; }

    std::string kitName() const override {
        switch (category_) {
            case DrumKitCategory::StandardPopRock: return "Pa5X Factory Standard/Pop/Rock";
            case DrumKitCategory::BrushJazz:       return "Pa5X Factory Brush/Jazz";
            case DrumKitCategory::Electronic:      return "Pa5X Factory Electronic";
            case DrumKitCategory::LatinPerc:       return "Pa5X Factory Latin/Perc";
            case DrumKitCategory::EthnicArabic:    return "Pa5X Factory Ethnic/Arabic";
            case DrumKitCategory::Sfx:             return "Pa5X Factory SFX";
        }
        return "Pa5X Factory Standard";
    }

    bool hasKey(uint8_t note) const noexcept override {
        return note < 128 && keys_[note].populated;
    }

    char familyOf(uint8_t note) const noexcept override {
        return note < 128 ? keys_[note].family : '\0';
    }

    DrumKeyInfo keyInfo(uint8_t note) const noexcept override {
        return note < 128 ? keys_[note] : DrumKeyInfo{};
    }

private:
    void populateKey(uint8_t n, char fam, uint8_t choke, uint8_t layers, const char* lbl) {
        auto& k = keys_[n];
        k.populated        = true;
        k.family           = fam;
        k.layer_count      = layers;
        k.effective_layers = layers;
        k.choke_group      = choke;
        k.effective_max_vel = 127;
        k.pan              = 64;
        k.label            = lbl;
    }

    DrumKitCategory                category_;
    std::array<DrumKeyInfo, 128>   keys_{};
};

// ── Concrete Adapter 2: Zero-Copy TXPERF DrumKit Adapter ─────────────────────

class TxperfDrumKitKeyMap final : public IDrumKitKeyMap {
public:
    explicit TxperfDrumKitKeyMap(const media::sound::DrumKit& kit) noexcept
        : kit_(&kit) {
        keys_.fill(nullptr);
        chokes_.fill(0);
        for (const auto& inst : kit.instruments) {
            for (const auto& dk : inst.keys) {
                if (dk.key < 128 && !dk.layers.empty()) {
                    keys_[dk.key]   = &dk;
                    chokes_[dk.key] = inst.choke;
                }
            }
        }
    }

    std::string kitName() const override {
        return kit_ ? kit_->name : std::string{};
    }

    bool hasKey(uint8_t note) const noexcept override {
        return note < 128 && keys_[note] != nullptr;
    }

    char familyOf(uint8_t note) const noexcept override {
        if (note >= 128 || !keys_[note]) return '\0';
        return keys_[note]->family;
    }

    DrumKeyInfo keyInfo(uint8_t note) const noexcept override {
        if (note >= 128 || !keys_[note]) return {};
        const auto& dk = *keys_[note];
        DrumKeyInfo info{};
        info.populated        = true;
        info.is_ram           = true;
        info.family           = dk.family;
        info.layer_count      = static_cast<uint8_t>(std::min<size_t>(dk.layers.size(), 16));
        info.effective_layers = info.layer_count;
        info.choke_group      = chokes_[note];
        info.effective_max_vel = 127;
        // Convert TXPERF panPct (-100..100) -> MIDI pan (0..127, 64=center).
        const int panMidi = std::clamp(64 + static_cast<int>(dk.panPct) * 63 / 100, 0, 127);
        info.pan   = static_cast<uint8_t>(panMidi);
        info.label = dk.name;
        return info;
    }

private:
    const media::sound::DrumKit*                    kit_ = nullptr;
    std::array<const media::sound::DrumKey*, 128>   keys_{};
    std::array<uint8_t, 128>                        chokes_{};
};

// ── Concrete Adapter 3: Track A Read-Only USERDK Seam ────────────────────────

// Read-only POD contract matching Track A's USERDK reader / DrumKitProfile output.
// Includes optional Track A Next fields (is_rom_zero_placeholder, resolved_family, pan)
// with clean defaults so both minimal Q3a structs and enriched Track A profiles plug in
// with zero changes to drum_remap.h and zero circular library dependencies.
struct UserdkKeySlotSummary {
    bool        populated               = false; // true iff >=1 resolved RAM or non-zero ROM/Bank2 layer
    bool        has_ram_layer           = false; // true iff >=1 resolved RAM.KMP DrumSample layer
    uint8_t     layer_count             = 0;     // key.lc (1..8)
    uint8_t     vel_tops[8]             = {};    // monotonic ascending top velocity thresholds
    uint8_t     choke_group             = 0;     // Exclusive Group (0=none, 1..127)
    uint8_t     kmp_family_code         = 0xFF;  // RAM.KMP byte +113: 0=B..7=X, 0xFF=None
    std::string primary_sample_name;             // RAM.KMP DrumSample name (empty if ROM-only)
    // Track A Next (§2.1–§2.3) optional enrichment fields:
    bool        is_rom_zero_placeholder = false; // true iff lc==1 && srcBank==0 && tblIdx==0
    char        resolved_family         = 0;     // optional 5-tier hierarchical family override ('B'..'X')
    uint8_t     pan                     = 64;    // 0..127, 64 = center
};

struct DrumKitKeySummaryView {
    std::string                           kit_name;              // e.g. "USERDK01/0"
    double                                gm_alignment_score = 1.0; // Track A Next §2.2 Tier 5 (0.0..1.0)
    std::array<UserdkKeySlotSummary, 128> keys{};
};

class UserdkDrumKitKeyMap final : public IDrumKitKeyMap {
public:
    explicit UserdkDrumKitKeyMap(DrumKitKeySummaryView s) : summary_(std::move(s)) {}

    std::string kitName() const override { return summary_.kit_name; }

    bool isUserdkTarget() const noexcept override { return true; }

    double gmAlignmentScore() const noexcept override { return summary_.gm_alignment_score; }

    bool hasKey(uint8_t note) const noexcept override {
        if (note >= 128) return false;
        const auto& k = summary_.keys[note];
        // Track A Next §2.1: ROM(idx==0) placeholder MUST return false!
        return k.populated && !k.is_rom_zero_placeholder;
    }

    char familyOf(uint8_t note) const noexcept override {
        if (!hasKey(note)) return '\0';
        const auto& k = summary_.keys[note];
        return k.resolved_family != 0 ? k.resolved_family : kmpDrumFamilyToChar(k.kmp_family_code);
    }

    DrumKeyInfo keyInfo(uint8_t note) const noexcept override {
        if (note >= 128) return {};
        const auto& k = summary_.keys[note];
        DrumKeyInfo info{};
        info.is_rom_zero_placeholder = k.is_rom_zero_placeholder;
        info.populated               = k.populated && !k.is_rom_zero_placeholder;
        info.is_ram                  = info.populated && k.has_ram_layer;
        info.family                  = info.populated ? familyOf(note) : '\0';
        info.layer_count             = info.populated ? k.layer_count : 0;
        info.choke_group             = info.populated ? k.choke_group : 0;
        info.pan                     = k.pan;
        info.label                   = info.populated ? k.primary_sample_name : std::string{};
        info.effective_max_vel       = 127;

        const uint8_t lc = std::min<uint8_t>(info.layer_count, 8);
        for (uint8_t i = 0; i < lc; ++i) {
            info.vel_tops[i] = k.vel_tops[i];
        }
        if (info.populated && lc > 0) {
            uint8_t distinct = 1;
            bool degen = false;
            for (uint8_t i = 1; i < lc; ++i) {
                if (k.vel_tops[i] == k.vel_tops[i - 1]) {
                    degen = true;
                } else {
                    ++distinct;
                }
            }
            info.effective_layers   = distinct;
            info.has_degenerate_vel = degen;
            if (k.vel_tops[lc - 1] > 0 && k.vel_tops[lc - 1] <= 127) {
                info.effective_max_vel = k.vel_tops[lc - 1];
            }
        }
        return info;
    }

private:
    DrumKitKeySummaryView summary_;
};

// ── (b) Kit-Category Classifier & Smart Rhythm1/Rhythm2 Swap Detector ────────

inline DrumKitCategory classifyDrumKitCategory(int yamaha_msb,
                                               int yamaha_lsb,
                                               int yamaha_pc,
                                               const std::string& track_name,
                                               uasf::TrackRole role = uasf::TrackRole::Drum) noexcept {
    const std::string low = detail::toLowerAscii(track_name);

    // 1. Explicit name cues (high signal across XG and Genos Revo! kits).
    if (detail::containsAny(low, {"sfx", "fxkit", "fx kit"})) return DrumKitCategory::Sfx;
    if (detail::containsAny(low, {"brush", "jazz"})) return DrumKitCategory::BrushJazz;
    if (detail::containsAny(low, {"808", "909", "electro", "dance", "edm", "techno", "house", "hiphop", "analog"})) {
        return DrumKitCategory::Electronic;
    }
    if (detail::containsAny(low, {"arab", "orient", "tabla", "asian", "ethnic", "indian", "turk", "darbuka"})) {
        return DrumKitCategory::EthnicArabic;
    }
    if (detail::containsAny(low, {"latin", "perc", "cuban", "samba", "bossa", "salsa"})) {
        return DrumKitCategory::LatinPerc;
    }

    // 2. Yamaha XG / Genos Drum Bank/PC taxonomy.
    if (yamaha_msb == 126 && yamaha_lsb == 0) {
        if (yamaha_pc == 0 || yamaha_pc == 1) return DrumKitCategory::Sfx;
        if (yamaha_pc >= 32 && yamaha_pc <= 47) return DrumKitCategory::EthnicArabic;
        return DrumKitCategory::LatinPerc;
    }
    if (yamaha_msb == 127 && yamaha_lsb == 0 && yamaha_pc >= 0) {
        if (yamaha_pc >= 24 && yamaha_pc <= 31) return DrumKitCategory::Electronic;
        if (yamaha_pc >= 32 && yamaha_pc <= 47) return DrumKitCategory::BrushJazz;
        if (yamaha_pc >= 48 && yamaha_pc <= 55) return DrumKitCategory::LatinPerc;
        if (yamaha_pc >= 112)                   return DrumKitCategory::EthnicArabic;
        return DrumKitCategory::StandardPopRock;
    }
    if (yamaha_msb == 62 && yamaha_lsb == 100) {
        // Revo! Drum kits: Standard/Pop/Rock unless name/role indicates otherwise.
        return (role == uasf::TrackRole::Percussion) ? DrumKitCategory::LatinPerc
                                                     : DrumKitCategory::StandardPopRock;
    }

    if (role == uasf::TrackRole::Percussion) return DrumKitCategory::LatinPerc;
    return DrumKitCategory::StandardPopRock;
}

struct RhythmSwapDecision {
    bool        swap_drum_and_perc = false;
    double      ch8_kit_score      = 0.0;  // fraction of hits in core kit {35,36,38,40,42,44,46}
    double      ch9_kit_score      = 0.0;
    double      ch8_perc_score     = 0.0;  // fraction of hits in hand perc {54,56,60..87}
    double      ch9_perc_score     = 0.0;
    std::string reason;
};

namespace detail {

inline bool isCoreKitPitch(uint8_t p) noexcept {
    return p == 35 || p == 36 || p == 38 || p == 40 || p == 42 || p == 44 || p == 46;
}

inline bool isHandPercPitch(uint8_t p) noexcept {
    return p == 54 || p == 56 || (p >= 60 && p <= 87);
}

}  // namespace detail

// Detects when Genos Rhythm1 (ch8) holds the main Kick+Snare+HiHat kit while
// Rhythm2 (ch9) holds secondary percussion (or is empty), preventing swapped
// Drum/Percussion channels (M4.3).
inline RhythmSwapDecision detectRhythmTrackSwap(const std::vector<NoteSpan>& ch8_notes,
                                                const std::vector<NoteSpan>& ch9_notes) noexcept {
    RhythmSwapDecision d{};

    auto scoreNotes = [](const std::vector<NoteSpan>& ns, double& kitScore, double& percScore) noexcept {
        if (ns.empty()) { kitScore = 0.0; percScore = 0.0; return; }
        uint32_t kitHits = 0, percHits = 0;
        for (const auto& n : ns) {
            if (detail::isCoreKitPitch(n.pitch))  ++kitHits;
            if (detail::isHandPercPitch(n.pitch)) ++percHits;
        }
        kitScore  = static_cast<double>(kitHits) / static_cast<double>(ns.size());
        percScore = static_cast<double>(percHits) / static_cast<double>(ns.size());
    };

    scoreNotes(ch8_notes, d.ch8_kit_score, d.ch8_perc_score);
    scoreNotes(ch9_notes, d.ch9_kit_score, d.ch9_perc_score);

    const bool ch8IsMainKit = (ch8_notes.size() >= 8) && (d.ch8_kit_score >= 0.45);
    const bool ch9EmptyOrPerc =
        ch9_notes.empty() || (d.ch9_kit_score < 0.20 && d.ch9_perc_score >= 0.40);

    if (ch8IsMainKit && ch9EmptyOrPerc) {
        d.swap_drum_and_perc = true;
        d.reason = ch9_notes.empty()
                       ? "Rhythm1 (ch8) carries main kit and Rhythm2 (ch9) is empty -> route ch8 to Korg Drum"
                       : "Rhythm1 (ch8) carries main kit while Rhythm2 (ch9) carries hand percussion -> swap to ch8=Drum, ch9=Perc";
    } else {
        d.swap_drum_and_perc = false;
        d.reason = "Default routing preserved (ch8=Perc, ch9=Drum)";
    }
    return d;
}

// ── (c) Complete Yamaha XG / Revo! Extended Drum Remapper ────────────────────

struct DrumRemapRule {
    uint8_t target_note    = 0;
    uint8_t fallback_note  = 0;     // 0 = none
    char    family         = 0;     // 'B','S','H','T','C','L','E','X'
    uint8_t choke_group    = 0;     // 0 = none, 1..7
    double  vel_scale      = 1.0;   // constant per-(cat, note) scale preserving contour r >= 0.95
    bool    short_gate     = false; // clamp gate <= 1/16th (e.g., Half-Open HH, Crash Choke)
    bool    drop_if_absent = false; // drop cleanly when target kit lacks this key (SFX / unassigned)
};

struct DrumRemapOptions {
    bool clamp_to_target_vel_ceiling = true; // Track A Next §2.3 soft-clamp to effective_max_vel
    bool preserve_userdk_native_keys = true; // preserve populated keys on EthnicArabic / non-GM USERDK
};

struct DrumRemapStats {
    uint32_t total_input_notes          = 0;
    uint32_t passthrough_notes          = 0; // kept on original pitch (35..81 or native USERDK)
    uint32_t remapped_notes             = 0; // pitch changed via XG/Revo! rule or target fallback
    uint32_t fallback_substituted_notes = 0; // target key absent in IDrumKitKeyMap -> family/choke fallback
    uint32_t notes_dropped              = 0; // unmapped SFX / high unassigned dropped cleanly
    uint32_t family_preserved_notes     = 0; // surviving notes whose final key matches expected family
    uint32_t choke_preserved_notes      = 0; // surviving choked notes whose final key preserves choke group
    uint32_t vel_ceiling_clamped        = 0; // notes clamped to target USERDK effective_max_vel < 127
};

struct DrumRemapResult {
    DrumKitCategory          category = DrumKitCategory::StandardPopRock;
    std::vector<NoteSpan>    notes;
    DrumRemapStats           stats;
    std::vector<std::string> diagnostic_codes;
};

namespace detail {

// Lookup deterministic Yamaha XG / Revo! -> Pa5X mapping rule for one MIDI note.
inline DrumRemapRule lookupDrumRemapRule(uint8_t note,
                                         DrumKitCategory cat,
                                         bool is_revo_kit) noexcept {
    // ── Core GM1 35..81: 1:1 backbone across Standard, Brush, Electronic, Latin, Ethnic ──
    if (note >= 35 && note <= 81) {
        DrumRemapRule r{};
        r.target_note = note;
        r.family      = gm2DefaultFamily(note);
        r.choke_group = gm2DefaultChokeGroup(note);
        r.vel_scale   = 1.0;
        if (cat == DrumKitCategory::EthnicArabic && note >= 60 && note <= 81) {
            r.family = 'E';
        }
        return r;
    }

    // ── Group 1: Default Standard / Pop / Rock (XG 127/0 & Revo! 62/100) ──
    DrumRemapRule r{};
    switch (note) {
        case 13:
            r = is_revo_kit ? DrumRemapRule{35, 36, 'B', 0, 0.95, false, false}
                            : DrumRemapRule{86, 64, 'L', 6, 0.95, false, false};
            break;
        case 14:
            r = is_revo_kit ? DrumRemapRule{36, 35, 'B', 0, 1.00, false, false}
                            : DrumRemapRule{87, 64, 'L', 6, 1.00, false, false};
            break;
        case 15: r = {27, 37, 'X', 0, 0.90, false, false}; break;
        case 16: r = {38, 40, 'S', 0, 0.65, false, false}; break;
        case 17: r = {38, 40, 'S', 0, 0.65, false, false}; break;
        case 18: r = {40, 38, 'S', 0, 0.95, false, false}; break;
        case 19: r = {37, 39, 'S', 0, 0.85, false, false}; break;
        case 20: r = {42, 44, 'H', 1, 0.85, false, false}; break;
        case 21: r = {42, 44, 'H', 1, 0.90, false, false}; break;
        case 22: r = {46, 42, 'H', 1, 0.80, true,  false}; break;
        case 23: r = {44, 42, 'H', 1, 0.85, false, false}; break;
        case 24: r = {35, 36, 'B', 0, 0.95, false, false}; break;
        case 25: r = {38, 40, 'S', 0, 0.75, false, false}; break;
        case 26: r = {39, 38, 'S', 0, 0.80, false, false}; break;
        case 27: r = {27, 37, 'X', 0, 1.00, false, false}; break;
        case 28: r = {28, 39, 'S', 0, 1.00, false, false}; break;
        case 29: r = {29, 73, 'X', 7, 1.00, false, false}; break;
        case 30: r = {30, 74, 'X', 7, 1.00, false, false}; break;
        case 31: r = {31, 37, 'S', 0, 1.00, false, false}; break;
        case 32: r = {32, 37, 'X', 0, 1.00, false, false}; break;
        case 33: r = {33, 76, 'X', 0, 1.00, false, false}; break;
        case 34: r = {34, 56, 'X', 0, 1.00, false, false}; break;

        // GM2 82..87 (1:1 with fallback)
        case 82: r = {82, 70, 'L', 0, 1.00, false, false}; break;
        case 83: r = {83, 54, 'L', 0, 1.00, false, false}; break;
        case 84: r = {84, 83, 'C', 0, 1.00, false, false}; break;
        case 85: r = {85, 75, 'L', 0, 1.00, false, false}; break;
        case 86: r = {86, 64, 'L', 6, 1.00, false, false}; break;
        case 87: r = {87, 64, 'L', 6, 1.00, false, false}; break;

        // 88..91: Snare Roll / Rim / Ride Edge / Crash Choke
        case 88: r = {38, 40, 'S', 0, 0.90, false, false}; break;
        case 89: r = {40, 38, 'S', 0, 0.90, false, false}; break;
        case 90: r = {51, 59, 'C', 0, 0.90, false, false}; break;
        case 91: r = {49, 57, 'C', 0, 0.90, true,  false}; break;

        // 92..99: Shaker/Cabasa/Tambourine/Tom Rim/Crash variations
        case 92: r = {82, 70, 'L', 0, 0.90, false, false}; break;
        case 93: r = {69, 82, 'L', 0, 0.90, false, false}; break;
        case 94: r = {54, 82, 'L', 0, 0.90, false, false}; break;
        case 95: r = {48, 50, 'T', 0, 0.90, false, false}; break;
        case 96: r = {45, 47, 'T', 0, 0.90, false, false}; break;
        case 97: r = {43, 41, 'T', 0, 0.90, false, false}; break;
        case 98: r = {41, 43, 'T', 0, 0.90, false, false}; break;
        case 99: r = {57, 49, 'C', 0, 0.90, false, false}; break;

        // 100..115: Revo! Articulation / Ambience / Ghost variations
        case 100: r = {38, 40, 'S', 0, 0.80, false, false}; break;
        case 101: r = {42, 44, 'H', 1, 0.80, false, false}; break;
        case 102: r = {46, 42, 'H', 1, 0.80, false, false}; break;
        case 103: r = {51, 59, 'C', 0, 0.80, false, false}; break;
        case 104: r = {49, 57, 'C', 0, 0.80, false, false}; break;
        case 105: case 110: case 115: r = {36, 35, 'B', 0, 0.80, false, false}; break;
        case 106: case 111:           r = {38, 40, 'S', 0, 0.80, false, false}; break;
        case 107: case 112:           r = {42, 44, 'H', 1, 0.80, false, false}; break;
        case 108: case 113:           r = {51, 49, 'C', 0, 0.80, false, false}; break;
        case 109: case 114:           r = {82, 70, 'L', 0, 0.80, false, false}; break;

        // 116..127: Revo! RX / Release / Unassigned high SFX
        case 121: r = {49, 57, 'C', 0, 0.85, false, false}; break;
        default:
            if (note < 13) {
                // Ultra-low sub-XG triggers (0..12): drop cleanly if absent.
                r = {note, 0, 'X', 0, 1.00, false, true};
            } else {
                // 116..120, 122..127: keep only if target kit explicitly defines key, else drop.
                r = {note, 0, 'X', 0, 1.00, false, true};
            }
            break;
    }

    // ── Group 2–6: Category-Specific Overrides ──
    switch (cat) {
        case DrumKitCategory::StandardPopRock:
            break;

        case DrumKitCategory::BrushJazz:
            if (note == 25)      r = {38, 40, 'S', 0, 0.85, false, false};
            else if (note == 26) r = {40, 38, 'S', 0, 0.90, false, false};
            else if (note == 27) r = {39, 38, 'S', 0, 0.90, false, false};
            else if (note == 28) r = {40, 38, 'S', 0, 0.85, false, false};
            else if (note >= 16 && note <= 18) r = {38, 40, 'S', 0, 0.60, false, false};
            else if (note >= 88 && note <= 96) {
                if (note <= 90)      r = {40, 38, 'S', 0, 0.85, false, false};
                else if (note <= 93) r = {51, 59, 'C', 0, 0.85, false, false};
                else                 r = {53, 51, 'C', 0, 0.85, false, false};
            }
            break;

        case DrumKitCategory::Electronic:
            if (note == 13 || note == 14)      r = {36, 35, 'B', 0, 1.00, false, false};
            else if (note >= 16 && note <= 18) r = {38, 40, 'S', 0, 0.90, false, false};
            else if (note == 19)               r = {39, 38, 'S', 0, 0.95, false, false};
            else if (note >= 20 && note <= 22) r = {42, 44, 'H', 1, 0.90, false, false};
            else if (note == 23)               r = {46, 42, 'H', 1, 0.90, false, false};
            else if (note == 25)               r = {38, 40, 'S', 0, 0.85, false, false};
            else if (note == 26)               r = {39, 38, 'S', 0, 0.90, false, false};
            else if (note >= 88 && note <= 95) {
                if (note <= 89)      r = {75, 37, 'L', 0, 0.95, false, false};  // 808 Clave
                else if (note <= 91) r = {56, 75, 'L', 0, 0.95, false, false};  // 808 Cowbell
                else if (note <= 93) r = {39, 38, 'S', 0, 0.95, false, false};  // 808 Clap
                else                 r = {82, 70, 'L', 0, 0.90, false, false};  // 808 Maraca/Shaker
            }
            break;

        case DrumKitCategory::LatinPerc:
            if (note == 13)                    r = {86, 64, 'L', 6, 0.95, false, false};
            else if (note == 14)               r = {87, 64, 'L', 6, 1.00, false, false};
            else if (note == 16)               r = {62, 63, 'L', 0, 0.90, false, false};
            else if (note == 17)               r = {63, 62, 'L', 0, 0.90, false, false};
            else if (note == 18)               r = {64, 63, 'L', 0, 0.95, false, false};
            else if (note == 19)               r = {75, 76, 'L', 0, 0.90, false, false};
            else if (note == 20 || note == 21) r = {82, 70, 'L', 0, 0.90, false, false};
            else if (note == 22 || note == 23) r = {69, 82, 'L', 0, 0.90, false, false};
            else if (note == 88)               r = {65, 66, 'L', 0, 0.95, false, false};
            else if (note == 89)               r = {66, 65, 'L', 0, 0.95, false, false};
            else if (note == 90)               r = {60, 61, 'L', 0, 0.90, false, false};
            else if (note == 91)               r = {61, 60, 'L', 0, 0.90, false, false};
            else if (note == 92)               r = {73, 74, 'L', 3, 0.90, false, false};
            else if (note == 93)               r = {74, 73, 'L', 3, 0.90, false, false};
            else if (note == 94)               r = {78, 79, 'L', 4, 0.90, false, false};
            else if (note == 95)               r = {79, 78, 'L', 4, 0.90, false, false};
            else if (note >= 96 && note <= 103) {
                constexpr uint8_t kLatTbl[8] = {54, 56, 67, 68, 82, 70, 69, 75};
                r = {kLatTbl[note - 96], 54, 'L', 0, 0.90, false, false};
            }
            break;

        case DrumKitCategory::EthnicArabic:
            if (note >= 13 && note <= 34) {
                if (note <= 15)      r = {36, 41, 'B', 0, 0.95, false, false};  // Doholla / Frame low
                else if (note <= 23) r = {60, 62, 'E', 0, 0.90, false, false};  // Darbuka / Tabla high
                else if (note <= 28) r = {63, 64, 'E', 0, 0.90, false, false};  // Darbuka / Tabla open
                else                 r = {54, 62, 'L', 0, 0.90, false, false};  // Req / Riqq
            } else if (note >= 88 && note <= 115) {
                constexpr uint8_t kEthTbl[6] = {60, 61, 62, 63, 64, 54};
                const uint8_t tgt = kEthTbl[(note - 88) % 6];
                r = {tgt, 62, (tgt == 54) ? 'L' : 'E', 0, 0.90, false, false};
            }
            break;

        case DrumKitCategory::Sfx:
            if ((note >= 27 && note <= 34) || (note >= 82 && note <= 87)) {
                r.target_note    = note;
                r.fallback_note  = 0;
                r.vel_scale      = 1.0;
                r.drop_if_absent = true;
            } else {
                r.target_note    = note;
                r.fallback_note  = 0;
                r.family         = 'X';
                r.vel_scale      = 1.0;
                r.drop_if_absent = true;
            }
            break;
    }

    return r;
}

inline bool familyCompatible(char want, char got) noexcept {
    if (want == 0 || got == 0) return false;
    return want == got;
}

// Track A Next §2.2 Tier 4: allow cross-family fallback between 'L' and 'E'.
inline bool percCrossFamilyCompatible(char want, char got) noexcept {
    return (want == 'L' && got == 'E') || (want == 'E' && got == 'L');
}

// Find the best populated fallback key in `kit` matching family + choke group.
// Returns -1 if kit has zero populated keys.
inline int findFallbackKeyInKit(const IDrumKitKeyMap& kit,
                                uint8_t desired_note,
                                char want_family,
                                uint8_t want_choke) noexcept {
    auto absDist = [](uint8_t a, uint8_t b) noexcept {
        return a > b ? static_cast<int>(a - b) : static_cast<int>(b - a);
    };

    // Priority 1: Exact family AND exact choke group (when want_choke > 0).
    if (want_choke > 0) {
        int best = -1, bestDist = 1000;
        for (uint8_t k = 0; k < 128; ++k) {
            if (!kit.hasKey(k)) continue;
            const DrumKeyInfo info = kit.keyInfo(k);
            if (familyCompatible(want_family, info.family) && info.choke_group == want_choke) {
                const int d = absDist(k, desired_note);
                if (best < 0 || d < bestDist) { best = k; bestDist = d; }
            }
        }
        if (best >= 0) return best;

        // Priority 1b: Same choke group even if kit left family unlabelled ('\0').
        for (uint8_t k = 0; k < 128; ++k) {
            if (!kit.hasKey(k)) continue;
            const DrumKeyInfo info = kit.keyInfo(k);
            if (info.choke_group == want_choke) {
                const int d = absDist(k, desired_note);
                if (best < 0 || d < bestDist) { best = k; bestDist = d; }
            }
        }
        if (best >= 0) return best;
    }

    // Priority 2: Exact family match, nearest pitch.
    if (want_family != 0) {
        int best = -1, bestDist = 1000;
        for (uint8_t k = 0; k < 128; ++k) {
            if (!kit.hasKey(k)) continue;
            if (familyCompatible(want_family, kit.familyOf(k))) {
                const int d = absDist(k, desired_note);
                if (best < 0 || d < bestDist) { best = k; bestDist = d; }
            }
        }
        if (best >= 0) return best;
    }

    // Priority 3: Cross-family Latin <-> Ethnic fallback (Track A Next §2.2 Tier 4).
    if (want_family == 'L' || want_family == 'E') {
        int best = -1, bestDist = 1000;
        for (uint8_t k = 0; k < 128; ++k) {
            if (!kit.hasKey(k)) continue;
            if (percCrossFamilyCompatible(want_family, kit.familyOf(k))) {
                const int d = absDist(k, desired_note);
                if (best < 0 || d < bestDist) { best = k; bestDist = d; }
            }
        }
        if (best >= 0) return best;
    }

    // Priority 4: Nearest populated key in kit (last resort for sparse custom kits).
    int best = -1, bestDist = 1000;
    for (uint8_t k = 0; k < 128; ++k) {
        if (!kit.hasKey(k)) continue;
        const int d = absDist(k, desired_note);
        if (best < 0 || d < bestDist) { best = k; bestDist = d; }
    }
    return best;
}

inline void addUniqueDiagnostic(std::vector<std::string>& diags, const char* code) {
    for (const auto& d : diags) {
        if (d == code) return;
    }
    diags.emplace_back(code);
}

}  // namespace detail

// Remap a Yamaha XG / SFF2 / Revo! drum or percussion track onto `target_kit`.
inline DrumRemapResult remapDrumTrack(const std::vector<NoteSpan>& notes,
                                      DrumKitCategory category,
                                      const IDrumKitKeyMap& target_kit,
                                      bool is_revo_kit = false,
                                      uint32_t ppqn = 480,
                                      const DrumRemapOptions& opt = {}) noexcept {
    DrumRemapResult res{};
    res.category = category;
    res.stats.total_input_notes = static_cast<uint32_t>(notes.size());
    res.notes.reserve(notes.size());

    const uint32_t shortGateCap = std::max<uint32_t>(1u, (ppqn > 0 ? ppqn : 480u) / 4u);
    const bool nativeUserdkPass =
        opt.preserve_userdk_native_keys &&
        target_kit.isUserdkTarget() &&
        (category == DrumKitCategory::EthnicArabic || target_kit.gmAlignmentScore() < 0.60);

    bool emittedEthnicFactoryAdvisory = false;

    for (const auto& rawSpan : notes) {
        NoteSpan span = detail::sanitizeDrumSpan(rawSpan);
        const uint8_t srcPitch = span.pitch;

        // 1. Custom / Regional USERDK pass-through for populated native keys.
        if (nativeUserdkPass && target_kit.hasKey(srcPitch)) {
            const DrumKeyInfo info = target_kit.keyInfo(srcPitch);
            if (opt.clamp_to_target_vel_ceiling && info.populated &&
                info.effective_max_vel < 127 && span.vel > info.effective_max_vel) {
                span.vel = std::max<uint8_t>(1u, info.effective_max_vel);
                ++res.stats.vel_ceiling_clamped;
                detail::addUniqueDiagnostic(res.diagnostic_codes, "STYLE_QUAL_TARGET_VEL_CEILING");
            }
            ++res.stats.passthrough_notes;
            ++res.stats.family_preserved_notes;
            if (info.choke_group > 0) ++res.stats.choke_preserved_notes;
            res.notes.push_back(span);
            continue;
        }

        const DrumRemapRule rule = detail::lookupDrumRemapRule(srcPitch, category, is_revo_kit);

        if (category == DrumKitCategory::EthnicArabic && !target_kit.isUserdkTarget() &&
            (srcPitch < 35 || srcPitch > 87) && !emittedEthnicFactoryAdvisory) {
            detail::addUniqueDiagnostic(res.diagnostic_codes, "STYLE_QUAL_EXTENDED_DRUM_MAP");
            emittedEthnicFactoryAdvisory = true;
        }

        // 2. Resolve target pitch against IDrumKitKeyMap occupancy.
        int resolvedPitch = -1;
        bool usedFallback = false;

        if (target_kit.hasKey(rule.target_note)) {
            resolvedPitch = static_cast<int>(rule.target_note);
        } else if (rule.drop_if_absent) {
            // Unpopulated SFX or high unassigned Revo! RX key -> drop cleanly.
            ++res.stats.notes_dropped;
            detail::addUniqueDiagnostic(res.diagnostic_codes, "STYLE_QUAL_EXTENDED_DRUM_MAP");
            continue;
        } else {
            // Check explicit table fallback first if it matches family/choke expectations.
            bool explicitOk = false;
            if (rule.fallback_note > 0 && target_kit.hasKey(rule.fallback_note)) {
                const DrumKeyInfo fbInfo = target_kit.keyInfo(rule.fallback_note);
                const bool chokeOk = (rule.choke_group == 0) || (fbInfo.choke_group == rule.choke_group);
                const bool famOk   = (rule.family == 0) || (fbInfo.family == 0) ||
                                     detail::familyCompatible(rule.family, fbInfo.family) ||
                                     detail::percCrossFamilyCompatible(rule.family, fbInfo.family);
                if (chokeOk && famOk) {
                    resolvedPitch = static_cast<int>(rule.fallback_note);
                    explicitOk = true;
                }
            }
            if (!explicitOk) {
                resolvedPitch = detail::findFallbackKeyInKit(
                    target_kit, rule.target_note, rule.family, rule.choke_group);
            }
            usedFallback = (resolvedPitch >= 0);
        }

        if (resolvedPitch < 0) {
            ++res.stats.notes_dropped;
            detail::addUniqueDiagnostic(res.diagnostic_codes, "STYLE_QUAL_EXTENDED_DRUM_MAP");
            continue;
        }

        const uint8_t finalPitch = static_cast<uint8_t>(std::clamp(resolvedPitch, 0, 127));
        const DrumKeyInfo finalInfo = target_kit.keyInfo(finalPitch);

        // 3. Apply constant per-(category, note) velocity scaling (preserves dynamic contour).
        const int scaledVel = static_cast<int>(std::lround(static_cast<double>(span.vel) * rule.vel_scale));
        uint8_t finalVel = static_cast<uint8_t>(std::clamp(scaledVel, 1, 127));

        // 4. Top-velocity ceiling guard (Track A Next §2.3).
        if (opt.clamp_to_target_vel_ceiling && finalInfo.populated &&
            finalInfo.effective_max_vel < 127 && finalVel > finalInfo.effective_max_vel) {
            finalVel = std::max<uint8_t>(1u, finalInfo.effective_max_vel);
            ++res.stats.vel_ceiling_clamped;
            detail::addUniqueDiagnostic(res.diagnostic_codes, "STYLE_QUAL_TARGET_VEL_CEILING");
        }

        // 5. Short gate conditioning (e.g., Half-Open Hi-Hat, Crash Choke), preserving on < off.
        uint32_t finalOff = span.off_tick;
        if (rule.short_gate && (finalOff - span.on_tick) > shortGateCap) {
            finalOff = span.on_tick + shortGateCap;
        }

        span.pitch    = finalPitch;
        span.vel      = finalVel;
        span.off_tick = finalOff;
        res.notes.push_back(span);

        if (finalPitch == srcPitch) {
            ++res.stats.passthrough_notes;
        } else {
            ++res.stats.remapped_notes;
        }
        if (usedFallback) {
            ++res.stats.fallback_substituted_notes;
        }
        if (rule.family == 0 || finalInfo.family == 0 ||
            detail::familyCompatible(rule.family, finalInfo.family) ||
            detail::percCrossFamilyCompatible(rule.family, finalInfo.family)) {
            ++res.stats.family_preserved_notes;
        }
        if (rule.choke_group > 0 && finalInfo.choke_group == rule.choke_group) {
            ++res.stats.choke_preserved_notes;
        }
    }

    return res;
}

// Convenience overload that classifies kit category & Revo! bank from Yamaha Bank/PC + name.
inline DrumRemapResult remapDrumTrack(const std::vector<NoteSpan>& notes,
                                      int yamaha_msb,
                                      int yamaha_lsb,
                                      int yamaha_pc,
                                      const std::string& track_name,
                                      uasf::TrackRole role,
                                      const IDrumKitKeyMap& target_kit,
                                      uint32_t ppqn = 480,
                                      const DrumRemapOptions& opt = {}) noexcept {
    const DrumKitCategory cat =
        classifyDrumKitCategory(yamaha_msb, yamaha_lsb, yamaha_pc, track_name, role);
    const bool isRevo = (yamaha_msb == 62 && yamaha_lsb == 100);
    return remapDrumTrack(notes, cat, target_kit, isRevo, ppqn, opt);
}

}  // namespace ai_arranger::exporters::korg::quality

#endif  // AI_ARRANGER_EXPORTERS_KORG_DRUM_REMAP_H
