#ifndef AI_ARRANGER_INTELLIGENCE_TARGET_CAPABILITY_H
#define AI_ARRANGER_INTELLIGENCE_TARGET_CAPABILITY_H

#include "engine/arranger/chord_input.h"
#include "engine/uasf/types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Phase 5 Milestone P5-M1: Vendor-Neutral Target Arranger Capability Model.
//
// Boundaries & Invariants (ADR-P5-002 / ADR-P5-003 / ADR-P5-005):
//   * Header-only portable C++20, zero JUCE/Apple/src/media dependencies.
//   * Strictly Zone B (offline/non-realtime); never included by realtime audio/MIDI.
//   * Covers 5 arranger platforms cleanly without hardcoded conversion patches:
//       KorgPa5X, YamahaGenos2, KorgPa4XPa1000 (KorgPaSeries), KetronEvent,
//       RolandEA7BK (RolandArranger).
//   * Preserves epistemic & safety locks: hardware_verified = false,
//     l3_status = "NOT_TESTED", native_binary_write_authorized = false
//     (NATIVE_SOUND_WRITE_HOLD / Gate 4 LOCKED).

namespace ai_arranger::intelligence {

enum class TargetPlatformId : uint8_t {
    KorgPa5X       = 0,
    YamahaGenos2   = 1,
    KorgPa4XPa1000 = 2,
    KorgPaSeries   = KorgPa4XPa1000,
    KetronEvent    = 3,
    RolandEA7BK    = 4,
    RolandArranger = RolandEA7BK,
};

using ArrangerPlatform = TargetPlatformId;

inline constexpr uint8_t kTargetPlatformCount = 5;

enum class ArticulationFlag : uint32_t {
    None              = 0u,
    Normal            = 1u << 0,
    MegaVoice         = 1u << 1,
    DNC               = 1u << 2,
    SuperArticulation = 1u << 3,
    Legato            = 1u << 4,
    Mute              = 1u << 5,
    Slide             = 1u << 6,
    Strum             = 1u << 7,
    GhostNote         = 1u << 8,
    RealAudioDrum     = 1u << 9,
    RxNoise           = 1u << 10,
};

inline constexpr uint32_t articulationBit(ArticulationFlag f) noexcept {
    return static_cast<uint32_t>(f);
}

inline constexpr const char* articulationFlagName(ArticulationFlag f) noexcept {
    switch (f) {
        case ArticulationFlag::None:              return "None";
        case ArticulationFlag::Normal:            return "Normal";
        case ArticulationFlag::MegaVoice:         return "MegaVoice";
        case ArticulationFlag::DNC:               return "DNC";
        case ArticulationFlag::SuperArticulation: return "SuperArticulation";
        case ArticulationFlag::Legato:            return "Legato";
        case ArticulationFlag::Mute:              return "Mute";
        case ArticulationFlag::Slide:             return "Slide";
        case ArticulationFlag::Strum:             return "Strum";
        case ArticulationFlag::GhostNote:         return "GhostNote";
        case ArticulationFlag::RealAudioDrum:     return "RealAudioDrum";
        case ArticulationFlag::RxNoise:           return "RxNoise";
    }
    return "Unknown";
}

enum class ChordCapabilityFlag : uint32_t {
    None  = 0u,
    Maj   = 1u << 0,
    Min   = 1u << 1,
    Dom7  = 1u << 2,
    Maj7  = 1u << 3,
    Min7  = 1u << 4,
    Dim   = 1u << 5,
    Dim7  = 1u << 6,
    Aug   = 1u << 7,
    Sus   = 1u << 8,
    Slash = 1u << 9,
};

inline constexpr uint32_t chordCapabilityBit(ChordCapabilityFlag f) noexcept {
    return static_cast<uint32_t>(f);
}

inline constexpr const char* chordCapabilityFlagName(ChordCapabilityFlag f) noexcept {
    switch (f) {
        case ChordCapabilityFlag::None:  return "None";
        case ChordCapabilityFlag::Maj:   return "Maj";
        case ChordCapabilityFlag::Min:   return "Min";
        case ChordCapabilityFlag::Dom7:  return "7";
        case ChordCapabilityFlag::Maj7:  return "Maj7";
        case ChordCapabilityFlag::Min7:  return "Min7";
        case ChordCapabilityFlag::Dim:   return "Dim";
        case ChordCapabilityFlag::Dim7:  return "Dim7";
        case ChordCapabilityFlag::Aug:   return "Aug";
        case ChordCapabilityFlag::Sus:   return "Sus";
        case ChordCapabilityFlag::Slash: return "Slash";
    }
    return "Unknown";
}

enum class RoleRegisterCategory : uint8_t {
    Drum = 0,
    Percussion = 1,
    Bass = 2,
    HarmonicComp = 3,
    Guitar = 4,
    Piano = 5,
    Pad = 6,
    Brass = 7,
    Orchestral = 8,
    Lead = 9,
    Effects = 10,
};

inline constexpr size_t kRoleRegisterCategoryCount = 11;

struct RolePitchRange {
    uint8_t min_pitch  = 0;
    uint8_t max_pitch  = 127;
    uint8_t ideal_low  = 36;
    uint8_t ideal_high = 84;

    bool contains(uint8_t pitch) const noexcept {
        return pitch >= min_pitch && pitch <= max_pitch;
    }
    bool inIdealRange(uint8_t pitch) const noexcept {
        return pitch >= ideal_low && pitch <= ideal_high;
    }
};

struct TargetSectionCaps {
    uint8_t max_intros            = 3;
    uint8_t max_variations        = 4;
    uint8_t max_fills             = 4;
    uint8_t max_endings           = 3;
    bool    supports_break        = true;
    uint8_t break_max_bars        = 1;
    uint8_t max_cv_variation      = 6; // Pa5X: 6; Genos2: 0 (CASM chord-mute instead)
    uint8_t max_cv_intro_fill_end = 2;
    uint8_t max_cv_break          = 0;
};

struct TargetTrackTopology {
    uint8_t  total_style_tracks     = 8;   // Active style tracks (Pa5X: 8, Genos2: 8..16, Ketron: 12)
    uint8_t  max_style_tracks       = 8;   // Upper track capacity (Genos2 supports up to 16 MIDI channels)
    uint8_t  drum_channel           = 9;   // 0-based MIDI channel
    uint8_t  perc_channel           = 10;
    uint8_t  bass_channel           = 8;
    uint8_t  first_acc_channel      = 11;
    uint8_t  acc_track_count        = 5;
    uint8_t  chord_event_note_max   = 11;  // Pa5X: C-1..B-1 reserved for Chord events; 0xFF if none
    uint16_t max_polyphony          = 160; // Hardware voice polyphony
    uint16_t style_voice_budget     = 64;  // Safe voice-stealing budget for accompaniment tracks
};

struct TargetArticulationCaps {
    bool     supports_dnc                   = false;
    bool     supports_super_articulation    = false;
    bool     supports_megavoice_native      = false;
    bool     supports_guitar_mode           = false;
    bool     supports_real_audio_drums      = false;
    bool     supports_smf_style_import      = false;
    bool     native_binary_write_authorized = false; // ALWAYS false under NATIVE_SOUND_WRITE_HOLD / Gate 4!
    bool     native_binary_export_supported = false; // Alias matching P5-M1 governance lock (ALWAYS false)
    uint32_t articulation_mask              = articulationBit(ArticulationFlag::Normal);
};

struct TargetChordCaps {
    uint32_t chord_mask         = 0u;
    bool     supports_slash_bass = true;
    bool     supports_casm_mute  = false; // Yamaha CASM chord-mute table
    bool     supports_cv_tables  = false; // Korg CV1..CV6 chord-variation tables
};

struct TargetSoundAddressing {
    uint8_t melodic_user_bank_min = 1;
    uint8_t melodic_user_bank_max = 12;
    uint8_t drum_user_bank_min    = 1;
    uint8_t drum_user_bank_max    = 6; // Pa5X in-spec USERDK01..06 (rejects 07..12)
    uint8_t gm2_melodic_msb       = 121;
    uint8_t gm2_drum_msb          = 120;
};

struct TargetCapabilityProfile {
    TargetPlatformId       id = TargetPlatformId::KorgPa5X;
    std::string            key;
    std::string            display_name;
    uint16_t               preferred_ppqn = 384;
    uint16_t               min_ppqn       = 96;
    uint16_t               max_ppqn       = 1920;
    TargetSectionCaps      sections{};
    TargetTrackTopology    tracks{};
    TargetArticulationCaps articulation{};
    TargetChordCaps        chords{};
    TargetSoundAddressing  sound_addressing{};
    std::array<RolePitchRange, kRoleRegisterCategoryCount> role_pitch_ranges{};
    bool                   hardware_verified = false;      // Always false offline
    std::string            l3_status = "NOT_TESTED";       // Always NOT_TESTED offline
    std::string            export_workflow_note;

    uint8_t maxCvFor(uasf::SectionType sec) const noexcept {
        switch (sec) {
            case uasf::SectionType::Main1:
            case uasf::SectionType::Main2:
            case uasf::SectionType::Main3:
            case uasf::SectionType::Main4:
                return sections.max_cv_variation;
            case uasf::SectionType::Break:
                return sections.max_cv_break;
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
                return sections.max_cv_intro_fill_end;
        }
        return 0;
    }

    bool isStyleChannel(uint8_t ch) const noexcept {
        if (ch == tracks.drum_channel || ch == tracks.perc_channel || ch == tracks.bass_channel) {
            return true;
        }
        if (ch >= tracks.first_acc_channel &&
            ch < static_cast<uint8_t>(tracks.first_acc_channel + tracks.acc_track_count)) {
            return true;
        }
        return false;
    }

    bool isChordDrivenChannel(uint8_t ch) const noexcept {
        if (!isStyleChannel(ch)) return false;
        return ch != tracks.drum_channel && ch != tracks.perc_channel;
    }

    bool supportsArticulation(ArticulationFlag flag) const noexcept {
        return (articulation.articulation_mask & articulationBit(flag)) != 0u;
    }

    bool supportsChordFlag(ChordCapabilityFlag flag) const noexcept {
        return (chords.chord_mask & chordCapabilityBit(flag)) != 0u;
    }

    bool supportsChordType(arranger::ChordType ct) const noexcept {
        using CT = arranger::ChordType;
        switch (ct) {
            case CT::Major:
            case CT::Add9:
            case CT::Maj6:
            case CT::Power:
                return supportsChordFlag(ChordCapabilityFlag::Maj);
            case CT::Minor:
            case CT::Min6:
                return supportsChordFlag(ChordCapabilityFlag::Min);
            case CT::Dom7:
            case CT::Dom9:
            case CT::Dom7Sus4:
            case CT::Aug7:
                return supportsChordFlag(ChordCapabilityFlag::Dom7);
            case CT::Maj7:
            case CT::Maj9:
                return supportsChordFlag(ChordCapabilityFlag::Maj7);
            case CT::Min7:
            case CT::Min9:
            case CT::Min7b5:
            case CT::MinMaj7:
                return supportsChordFlag(ChordCapabilityFlag::Min7);
            case CT::Dim:
                return supportsChordFlag(ChordCapabilityFlag::Dim);
            case CT::Dim7:
                return supportsChordFlag(ChordCapabilityFlag::Dim7);
            case CT::Aug:
                return supportsChordFlag(ChordCapabilityFlag::Aug);
            case CT::Sus4:
            case CT::Sus2:
                return supportsChordFlag(ChordCapabilityFlag::Sus);
            case CT::NoChord:
                return true;
        }
        return false;
    }

    RolePitchRange pitchRangeFor(RoleRegisterCategory cat) const noexcept {
        const size_t idx = static_cast<size_t>(cat);
        if (idx < role_pitch_ranges.size()) {
            return role_pitch_ranges[idx];
        }
        return RolePitchRange{};
    }
};

enum class CapabilityGapSeverity : uint8_t {
    None = 0,
    Info = 1,
    Advisory = 2,
    LossyAdaptationRequired = 3,
};

inline constexpr const char* capabilityGapSeverityName(CapabilityGapSeverity sev) noexcept {
    switch (sev) {
        case CapabilityGapSeverity::None:                    return "NONE";
        case CapabilityGapSeverity::Info:                    return "INFO";
        case CapabilityGapSeverity::Advisory:                return "ADVISORY";
        case CapabilityGapSeverity::LossyAdaptationRequired: return "LOSSY_ADAPTATION_REQUIRED";
    }
    return "UNKNOWN";
}

struct CapabilityGapItem {
    std::string           code;
    std::string           dimension;
    CapabilityGapSeverity severity = CapabilityGapSeverity::Info;
    std::string           source_value;
    std::string           target_value;
    std::string           mitigation;
};

struct CapabilityGapReport {
    std::string                    schema = "aiarranger-capability-gap/1";
    TargetPlatformId               source_platform = TargetPlatformId::YamahaGenos2;
    TargetPlatformId               target_platform = TargetPlatformId::KorgPa5X;
    std::string                    source_key;
    std::string                    target_key;
    CapabilityGapSeverity          max_severity = CapabilityGapSeverity::None;
    int32_t                        track_count_delta = 0;        // target - source
    int32_t                        cv_count_delta = 0;           // target - source
    int32_t                        break_bar_delta = 0;          // target - source
    int32_t                        polyphony_delta = 0;          // target - source
    bool                           ppqn_resample_recommended = false;
    std::vector<ArticulationFlag>  missing_articulations;
    std::vector<ChordCapabilityFlag> missing_chord_capabilities;
    std::vector<CapabilityGapItem> items;
    bool                           hardware_verified = false;
    std::string                    l3_status = "NOT_TESTED";
};

namespace target_cap_detail {

inline constexpr uint32_t kAllBasicChordsMask =
    chordCapabilityBit(ChordCapabilityFlag::Maj) |
    chordCapabilityBit(ChordCapabilityFlag::Min) |
    chordCapabilityBit(ChordCapabilityFlag::Dom7) |
    chordCapabilityBit(ChordCapabilityFlag::Maj7) |
    chordCapabilityBit(ChordCapabilityFlag::Min7) |
    chordCapabilityBit(ChordCapabilityFlag::Dim) |
    chordCapabilityBit(ChordCapabilityFlag::Dim7) |
    chordCapabilityBit(ChordCapabilityFlag::Aug) |
    chordCapabilityBit(ChordCapabilityFlag::Sus) |
    chordCapabilityBit(ChordCapabilityFlag::Slash);

inline std::array<RolePitchRange, kRoleRegisterCategoryCount> defaultRolePitchRanges(
    uint8_t chord_event_min_note) noexcept {
    // Ensure melodic instruments stay safely above reserved chord-event notes (e.g. > 11 on Pa5X).
    const uint8_t safe_low = static_cast<uint8_t>(std::max<uint8_t>(12u, chord_event_min_note));
    std::array<RolePitchRange, kRoleRegisterCategoryCount> r{};
    r[static_cast<size_t>(RoleRegisterCategory::Drum)]         = {24, 96, 35, 81};
    r[static_cast<size_t>(RoleRegisterCategory::Percussion)]   = {24, 96, 54, 81};
    r[static_cast<size_t>(RoleRegisterCategory::Bass)]         = {std::max<uint8_t>(safe_low, 28u), 67, 33, 55};
    r[static_cast<size_t>(RoleRegisterCategory::HarmonicComp)] = {std::max<uint8_t>(safe_low, 40u), 96, 48, 76};
    r[static_cast<size_t>(RoleRegisterCategory::Guitar)]       = {std::max<uint8_t>(safe_low, 40u), 88, 45, 76};
    r[static_cast<size_t>(RoleRegisterCategory::Piano)]        = {std::max<uint8_t>(safe_low, 21u), 108, 43, 84};
    r[static_cast<size_t>(RoleRegisterCategory::Pad)]          = {std::max<uint8_t>(safe_low, 36u), 96, 48, 79};
    r[static_cast<size_t>(RoleRegisterCategory::Brass)]        = {std::max<uint8_t>(safe_low, 45u), 96, 55, 82};
    r[static_cast<size_t>(RoleRegisterCategory::Orchestral)]   = {std::max<uint8_t>(safe_low, 36u), 100, 48, 86};
    r[static_cast<size_t>(RoleRegisterCategory::Lead)]         = {std::max<uint8_t>(safe_low, 52u), 108, 60, 88};
    r[static_cast<size_t>(RoleRegisterCategory::Effects)]      = {safe_low, 127, 36, 96};
    return r;
}

inline TargetCapabilityProfile makeKorgPa5X() {
    TargetCapabilityProfile p{};
    p.id = TargetPlatformId::KorgPa5X;
    p.key = "korg_pa5x";
    p.display_name = "Korg Pa5X";
    p.preferred_ppqn = 384;
    p.min_ppqn = 96;
    p.max_ppqn = 1920;

    p.sections.max_intros = 3;
    p.sections.max_variations = 4;
    p.sections.max_fills = 4;
    p.sections.max_endings = 3;
    p.sections.supports_break = true;
    p.sections.break_max_bars = 1;
    p.sections.max_cv_variation = 6;
    p.sections.max_cv_intro_fill_end = 2;
    p.sections.max_cv_break = 0;

    p.tracks.total_style_tracks = 8;
    p.tracks.max_style_tracks = 8;
    p.tracks.drum_channel = 9;
    p.tracks.perc_channel = 10;
    p.tracks.bass_channel = 8;
    p.tracks.first_acc_channel = 11;
    p.tracks.acc_track_count = 5;
    p.tracks.chord_event_note_max = 11; // C-1..B-1 reserved for Pa5X Chord events
    p.tracks.max_polyphony = 160;
    p.tracks.style_voice_budget = 64;

    p.articulation.supports_dnc = true;
    p.articulation.supports_super_articulation = false;
    p.articulation.supports_megavoice_native = false;
    p.articulation.supports_guitar_mode = true;
    p.articulation.supports_real_audio_drums = false;
    p.articulation.supports_smf_style_import = true;
    p.articulation.native_binary_write_authorized = false;
    p.articulation.native_binary_export_supported = false;
    p.articulation.articulation_mask =
        articulationBit(ArticulationFlag::Normal) |
        articulationBit(ArticulationFlag::DNC) |
        articulationBit(ArticulationFlag::Legato) |
        articulationBit(ArticulationFlag::Mute) |
        articulationBit(ArticulationFlag::Slide) |
        articulationBit(ArticulationFlag::Strum) |
        articulationBit(ArticulationFlag::GhostNote) |
        articulationBit(ArticulationFlag::RxNoise);

    p.chords.chord_mask = kAllBasicChordsMask;
    p.chords.supports_slash_bass = true;
    p.chords.supports_casm_mute = false;
    p.chords.supports_cv_tables = true;

    p.sound_addressing.melodic_user_bank_min = 1;
    p.sound_addressing.melodic_user_bank_max = 12;
    p.sound_addressing.drum_user_bank_min = 1;
    p.sound_addressing.drum_user_bank_max = 6; // USERDK01..06 only
    p.sound_addressing.gm2_melodic_msb = 121;
    p.sound_addressing.gm2_drum_msb = 120;

    p.role_pitch_ranges = defaultRolePitchRanges(12);
    p.hardware_verified = false;
    p.l3_status = "NOT_TESTED";
    p.export_workflow_note =
        "Documented SMF0 marker import (Style Edit > Import SMF); native .STY/.KST binary writes LOCKED (NATIVE_SOUND_WRITE_HOLD).";
    return p;
}

inline TargetCapabilityProfile makeYamahaGenos2() {
    TargetCapabilityProfile p{};
    p.id = TargetPlatformId::YamahaGenos2;
    p.key = "yamaha_genos2";
    p.display_name = "Yamaha Genos2";
    p.preferred_ppqn = 480;
    p.min_ppqn = 96;
    p.max_ppqn = 1920;

    p.sections.max_intros = 3;
    p.sections.max_variations = 4;
    p.sections.max_fills = 4;
    p.sections.max_endings = 3;
    p.sections.supports_break = true;
    p.sections.break_max_bars = 1;
    p.sections.max_cv_variation = 0; // Uses CASM chord-mute masks rather than CV1..CV6 tables
    p.sections.max_cv_intro_fill_end = 0;
    p.sections.max_cv_break = 0;

    p.tracks.total_style_tracks = 8;
    p.tracks.max_style_tracks = 16; // SFF2 allows up to 16 source MIDI channels routed to 8 style parts
    p.tracks.drum_channel = 9;      // Rhythm2 (default main drum kit)
    p.tracks.perc_channel = 8;      // Rhythm1 (sub-percussion)
    p.tracks.bass_channel = 10;     // Bass on Yamaha ch 11 (0-based 10)
    p.tracks.first_acc_channel = 11;
    p.tracks.acc_track_count = 5;   // Chord1, Chord2, Pad, Phrase1, Phrase2
    p.tracks.chord_event_note_max = 0xFF;
    p.tracks.max_polyphony = 384;   // 128 AWM + 128 FM + 128 Expansion
    p.tracks.style_voice_budget = 96;

    p.articulation.supports_dnc = false;
    p.articulation.supports_super_articulation = true;
    p.articulation.supports_megavoice_native = true;
    p.articulation.supports_guitar_mode = true; // SFF2 NTR=2 Guitar mode
    p.articulation.supports_real_audio_drums = false;
    p.articulation.supports_smf_style_import = false;
    p.articulation.native_binary_write_authorized = false;
    p.articulation.native_binary_export_supported = false;
    p.articulation.articulation_mask =
        articulationBit(ArticulationFlag::Normal) |
        articulationBit(ArticulationFlag::MegaVoice) |
        articulationBit(ArticulationFlag::SuperArticulation) |
        articulationBit(ArticulationFlag::Legato) |
        articulationBit(ArticulationFlag::Mute) |
        articulationBit(ArticulationFlag::Slide) |
        articulationBit(ArticulationFlag::Strum) |
        articulationBit(ArticulationFlag::GhostNote);

    p.chords.chord_mask = kAllBasicChordsMask;
    p.chords.supports_slash_bass = true;
    p.chords.supports_casm_mute = true;
    p.chords.supports_cv_tables = false;

    p.sound_addressing.melodic_user_bank_min = 0;
    p.sound_addressing.melodic_user_bank_max = 127;
    p.sound_addressing.drum_user_bank_min = 0;
    p.sound_addressing.drum_user_bank_max = 127;
    p.sound_addressing.gm2_melodic_msb = 0;
    p.sound_addressing.gm2_drum_msb = 127;

    p.role_pitch_ranges = defaultRolePitchRanges(0);
    p.hardware_verified = false;
    p.l3_status = "NOT_TESTED";
    p.export_workflow_note =
        "Primary SFF1/SFF2 source profile (CASM/MegaVoice/S.Art2/Revo!Drum); binary .sty export not authorized.";
    return p;
}

inline TargetCapabilityProfile makeKorgPa4XPa1000() {
    TargetCapabilityProfile p = makeKorgPa5X();
    p.id = TargetPlatformId::KorgPa4XPa1000;
    p.key = "korg_pa4x_pa1000";
    p.display_name = "Korg Pa4X / Pa1000 / Pa700";
    p.tracks.max_polyphony = 128;
    p.tracks.style_voice_budget = 56;
    p.sound_addressing.melodic_user_bank_min = 1;
    p.sound_addressing.melodic_user_bank_max = 4; // Legacy Pa4X/Pa1000 User01..04
    p.sound_addressing.drum_user_bank_min = 1;
    p.sound_addressing.drum_user_bank_max = 2;    // Legacy UserDK01..02
    p.export_workflow_note =
        "Legacy Korg Pa-Series SMF0 marker import (.SET container binary write LOCKED).";
    return p;
}

inline TargetCapabilityProfile makeKetronEvent() {
    TargetCapabilityProfile p{};
    p.id = TargetPlatformId::KetronEvent;
    p.key = "ketron_event";
    p.display_name = "Ketron Event";
    p.preferred_ppqn = 480;
    p.min_ppqn = 96;
    p.max_ppqn = 960;

    p.sections.max_intros = 3;
    p.sections.max_variations = 4;
    p.sections.max_fills = 4;
    p.sections.max_endings = 3;
    p.sections.supports_break = true;
    p.sections.break_max_bars = 2;
    p.sections.max_cv_variation = 3; // Maj/Min/7th style chord tables
    p.sections.max_cv_intro_fill_end = 2;
    p.sections.max_cv_break = 1;

    p.tracks.total_style_tracks = 12; // Real Drums + Groove + Live Bass + Live Guitar + Chords 1..5 + Lower
    p.tracks.max_style_tracks = 12;
    p.tracks.drum_channel = 9;
    p.tracks.perc_channel = 10;
    p.tracks.bass_channel = 4;
    p.tracks.first_acc_channel = 5;
    p.tracks.acc_track_count = 5;
    p.tracks.chord_event_note_max = 0xFF;
    p.tracks.max_polyphony = 216;
    p.tracks.style_voice_budget = 72;

    p.articulation.supports_dnc = false;
    p.articulation.supports_super_articulation = false;
    p.articulation.supports_megavoice_native = false;
    p.articulation.supports_guitar_mode = true; // Live Guitar accompaniment
    p.articulation.supports_real_audio_drums = true;
    p.articulation.supports_smf_style_import = false;
    p.articulation.native_binary_write_authorized = false;
    p.articulation.native_binary_export_supported = false;
    p.articulation.articulation_mask =
        articulationBit(ArticulationFlag::Normal) |
        articulationBit(ArticulationFlag::Legato) |
        articulationBit(ArticulationFlag::Mute) |
        articulationBit(ArticulationFlag::Slide) |
        articulationBit(ArticulationFlag::Strum) |
        articulationBit(ArticulationFlag::GhostNote) |
        articulationBit(ArticulationFlag::RealAudioDrum);

    p.chords.chord_mask = kAllBasicChordsMask;
    p.chords.supports_slash_bass = true;
    p.chords.supports_casm_mute = false;
    p.chords.supports_cv_tables = true;

    p.sound_addressing.melodic_user_bank_min = 1;
    p.sound_addressing.melodic_user_bank_max = 8;
    p.sound_addressing.drum_user_bank_min = 1;
    p.sound_addressing.drum_user_bank_max = 4;
    p.sound_addressing.gm2_melodic_msb = 0;
    p.sound_addressing.gm2_drum_msb = 120;

    p.role_pitch_ranges = defaultRolePitchRanges(0);
    p.hardware_verified = false;
    p.l3_status = "NOT_TESTED";
    p.export_workflow_note =
        "Future target profile (Real Audio/Live Drums + MIDI accompaniment); exporter not yet implemented.";
    return p;
}

inline TargetCapabilityProfile makeRolandEA7BK() {
    TargetCapabilityProfile p{};
    p.id = TargetPlatformId::RolandEA7BK;
    p.key = "roland_ea7_bk";
    p.display_name = "Roland E-A7 / BK-9";
    p.preferred_ppqn = 480;
    p.min_ppqn = 96;
    p.max_ppqn = 960;

    p.sections.max_intros = 4;
    p.sections.max_variations = 4;
    p.sections.max_fills = 4;
    p.sections.max_endings = 4;
    p.sections.supports_break = true;
    p.sections.break_max_bars = 1;
    p.sections.max_cv_variation = 3; // Maj / Min / 7th division tables
    p.sections.max_cv_intro_fill_end = 3;
    p.sections.max_cv_break = 1;

    p.tracks.total_style_tracks = 8; // Adr, Bass, Acc1..Acc6
    p.tracks.max_style_tracks = 8;
    p.tracks.drum_channel = 9;
    p.tracks.perc_channel = 10;
    p.tracks.bass_channel = 1;
    p.tracks.first_acc_channel = 2;
    p.tracks.acc_track_count = 5;
    p.tracks.chord_event_note_max = 0xFF;
    p.tracks.max_polyphony = 128;
    p.tracks.style_voice_budget = 56;

    p.articulation.supports_dnc = false;
    p.articulation.supports_super_articulation = false;
    p.articulation.supports_megavoice_native = false;
    p.articulation.supports_guitar_mode = false;
    p.articulation.supports_real_audio_drums = false;
    p.articulation.supports_smf_style_import = true; // SMF Style Composer import
    p.articulation.native_binary_write_authorized = false;
    p.articulation.native_binary_export_supported = false;
    p.articulation.articulation_mask =
        articulationBit(ArticulationFlag::Normal) |
        articulationBit(ArticulationFlag::Legato) |
        articulationBit(ArticulationFlag::Mute) |
        articulationBit(ArticulationFlag::Strum) |
        articulationBit(ArticulationFlag::GhostNote);

    // Roland style tables cover core chord families; augmented/dim7 may fold to Maj/Min/7th tables.
    p.chords.chord_mask =
        chordCapabilityBit(ChordCapabilityFlag::Maj) |
        chordCapabilityBit(ChordCapabilityFlag::Min) |
        chordCapabilityBit(ChordCapabilityFlag::Dom7) |
        chordCapabilityBit(ChordCapabilityFlag::Maj7) |
        chordCapabilityBit(ChordCapabilityFlag::Min7) |
        chordCapabilityBit(ChordCapabilityFlag::Dim) |
        chordCapabilityBit(ChordCapabilityFlag::Sus) |
        chordCapabilityBit(ChordCapabilityFlag::Slash);
    p.chords.supports_slash_bass = true;
    p.chords.supports_casm_mute = false;
    p.chords.supports_cv_tables = true;

    p.sound_addressing.melodic_user_bank_min = 1;
    p.sound_addressing.melodic_user_bank_max = 4;
    p.sound_addressing.drum_user_bank_min = 1;
    p.sound_addressing.drum_user_bank_max = 2;
    p.sound_addressing.gm2_melodic_msb = 121;
    p.sound_addressing.gm2_drum_msb = 120;

    p.role_pitch_ranges = defaultRolePitchRanges(0);
    p.hardware_verified = false;
    p.l3_status = "NOT_TESTED";
    p.export_workflow_note =
        "Future target profile (Roland E-A7 / BK-9 Style Composer SMF workflow); exporter not yet implemented.";
    return p;
}

inline std::string toLowerAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

inline std::string jsonEscape(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 8);
    for (char c : sv) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    out += ' ';
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

} // namespace target_cap_detail

inline const TargetCapabilityProfile& targetCapabilityProfile(TargetPlatformId id) noexcept {
    static const TargetCapabilityProfile kPa5X   = target_cap_detail::makeKorgPa5X();
    static const TargetCapabilityProfile kGenos2 = target_cap_detail::makeYamahaGenos2();
    static const TargetCapabilityProfile kPa4X   = target_cap_detail::makeKorgPa4XPa1000();
    static const TargetCapabilityProfile kKetron = target_cap_detail::makeKetronEvent();
    static const TargetCapabilityProfile kRoland = target_cap_detail::makeRolandEA7BK();

    switch (id) {
        case TargetPlatformId::KorgPa5X:       return kPa5X;
        case TargetPlatformId::YamahaGenos2:   return kGenos2;
        case TargetPlatformId::KorgPa4XPa1000: return kPa4X;
        case TargetPlatformId::KetronEvent:    return kKetron;
        case TargetPlatformId::RolandEA7BK:    return kRoland;
    }
    return kPa5X;
}

inline const char* targetPlatformKey(TargetPlatformId id) noexcept {
    return targetCapabilityProfile(id).key.c_str();
}

inline std::optional<TargetPlatformId> parseTargetPlatformId(std::string_view token) noexcept {
    const std::string t = target_cap_detail::toLowerAscii(token);
    if (t == "korg_pa5x" || t == "pa5x" || t == "korg-pa5x") {
        return TargetPlatformId::KorgPa5X;
    }
    if (t == "yamaha_genos2" || t == "genos2" || t == "genos" || t == "yamaha-genos2") {
        return TargetPlatformId::YamahaGenos2;
    }
    if (t == "korg_pa4x_pa1000" || t == "korg_pa_series" || t == "pa4x" || t == "pa1000" || t == "pa700") {
        return TargetPlatformId::KorgPa4XPa1000;
    }
    if (t == "ketron_event" || t == "ketron" || t == "ketron-event") {
        return TargetPlatformId::KetronEvent;
    }
    if (t == "roland_ea7_bk" || t == "roland_arranger" || t == "roland" || t == "ea7" || t == "bk9") {
        return TargetPlatformId::RolandEA7BK;
    }
    return std::nullopt;
}

inline std::vector<TargetPlatformId> supportedTargetPlatforms() {
    return {
        TargetPlatformId::KorgPa5X,
        TargetPlatformId::YamahaGenos2,
        TargetPlatformId::KorgPa4XPa1000,
        TargetPlatformId::KetronEvent,
        TargetPlatformId::RolandEA7BK,
    };
}

inline CapabilityGapReport compareTargetCapabilities(
    TargetPlatformId source_platform,
    TargetPlatformId target_platform) {
    const auto& src = targetCapabilityProfile(source_platform);
    const auto& dst = targetCapabilityProfile(target_platform);

    CapabilityGapReport r{};
    r.source_platform = source_platform;
    r.target_platform = target_platform;
    r.source_key = src.key;
    r.target_key = dst.key;
    r.track_count_delta = static_cast<int32_t>(dst.tracks.max_style_tracks) -
                          static_cast<int32_t>(src.tracks.max_style_tracks);
    r.cv_count_delta = static_cast<int32_t>(dst.sections.max_cv_variation) -
                       static_cast<int32_t>(src.sections.max_cv_variation);
    r.break_bar_delta = static_cast<int32_t>(dst.sections.break_max_bars) -
                        static_cast<int32_t>(src.sections.break_max_bars);
    r.polyphony_delta = static_cast<int32_t>(dst.tracks.max_polyphony) -
                        static_cast<int32_t>(src.tracks.max_polyphony);
    r.ppqn_resample_recommended = (src.preferred_ppqn != dst.preferred_ppqn);

    auto bumpSeverity = [&](CapabilityGapSeverity sev) {
        if (static_cast<uint8_t>(sev) > static_cast<uint8_t>(r.max_severity)) {
            r.max_severity = sev;
        }
    };

    if (r.track_count_delta < 0) {
        bumpSeverity(CapabilityGapSeverity::LossyAdaptationRequired);
        r.items.push_back({
            "CAP_GAP_TRACK_COMPRESSION",
            "track_topology",
            CapabilityGapSeverity::LossyAdaptationRequired,
            std::to_string(src.tracks.max_style_tracks) + " max tracks",
            std::to_string(dst.tracks.max_style_tracks) + " max tracks",
            "Route or fold excess source channels via CASM/CV split without colliding bass/drum roles.",
        });
    }

    if (src.chords.supports_casm_mute && !dst.chords.supports_casm_mute && dst.chords.supports_cv_tables) {
        bumpSeverity(CapabilityGapSeverity::Advisory);
        r.items.push_back({
            "CAP_GAP_CASM_TO_CV_TRANSLATION",
            "chord_variation_model",
            CapabilityGapSeverity::Advisory,
            "CASM chord-mute masks",
            "CV1..CV" + std::to_string(dst.sections.max_cv_variation) + " tables",
            "Translate mutually exclusive CASM chord-mute tracks into target CV1..CVn elements.",
        });
    } else if (src.sections.max_cv_variation > dst.sections.max_cv_variation && dst.sections.max_cv_variation > 0) {
        bumpSeverity(CapabilityGapSeverity::LossyAdaptationRequired);
        r.items.push_back({
            "CAP_GAP_CV_SLOT_REDUCTION",
            "chord_variation_model",
            CapabilityGapSeverity::LossyAdaptationRequired,
            std::to_string(src.sections.max_cv_variation) + " CVs",
            std::to_string(dst.sections.max_cv_variation) + " CVs",
            "Consolidate secondary chord variations into primary Maj/Min/7th target CV tables.",
        });
    }

    if (r.break_bar_delta < 0) {
        bumpSeverity(CapabilityGapSeverity::LossyAdaptationRequired);
        r.items.push_back({
            "CAP_GAP_BREAK_BAR_LIMIT",
            "section_structure",
            CapabilityGapSeverity::LossyAdaptationRequired,
            std::to_string(src.sections.break_max_bars) + " bars",
            std::to_string(dst.sections.break_max_bars) + " bars",
            "Clamp or truncate Break section cleanly to target max bar cap.",
        });
    }

    if (dst.tracks.chord_event_note_max != 0xFF &&
        (src.tracks.chord_event_note_max == 0xFF || dst.tracks.chord_event_note_max > src.tracks.chord_event_note_max)) {
        bumpSeverity(CapabilityGapSeverity::Advisory);
        r.items.push_back({
            "CAP_GAP_CHORD_EVENT_NOTE_RESERVATION",
            "pitch_range",
            CapabilityGapSeverity::Advisory,
            src.tracks.chord_event_note_max == 0xFF ? "none" : ("0.." + std::to_string(src.tracks.chord_event_note_max)),
            "0.." + std::to_string(dst.tracks.chord_event_note_max) + " reserved",
            "Strip or fold notes in target reserved chord-event pitch band on chord-driven tracks.",
        });
    }

    if (r.ppqn_resample_recommended) {
        bumpSeverity(CapabilityGapSeverity::Info);
        r.items.push_back({
            "CAP_GAP_PPQN_RESCALE",
            "timing_resolution",
            CapabilityGapSeverity::Info,
            std::to_string(src.preferred_ppqn) + " PPQN",
            std::to_string(dst.preferred_ppqn) + " PPQN",
            "Rescale MIDI ticks deterministically to target preferred PPQN while preserving microtiming.",
        });
    }

    if (r.polyphony_delta < 0) {
        bumpSeverity(CapabilityGapSeverity::Advisory);
        r.items.push_back({
            "CAP_GAP_POLYPHONY_HEADROOM",
            "polyphony",
            CapabilityGapSeverity::Advisory,
            std::to_string(src.tracks.max_polyphony) + " voices",
            std::to_string(dst.tracks.max_polyphony) + " voices",
            "Monitor dense sustained chord + pedal stacks against target style voice budget.",
        });
    }

    static constexpr std::array<ArticulationFlag, 11> kArtFlags = {
        ArticulationFlag::Normal,
        ArticulationFlag::MegaVoice,
        ArticulationFlag::DNC,
        ArticulationFlag::SuperArticulation,
        ArticulationFlag::Legato,
        ArticulationFlag::Mute,
        ArticulationFlag::Slide,
        ArticulationFlag::Strum,
        ArticulationFlag::GhostNote,
        ArticulationFlag::RealAudioDrum,
        ArticulationFlag::RxNoise,
    };
    for (ArticulationFlag f : kArtFlags) {
        if (src.supportsArticulation(f) && !dst.supportsArticulation(f)) {
            r.missing_articulations.push_back(f);
            const bool lossy = (f == ArticulationFlag::MegaVoice ||
                                f == ArticulationFlag::SuperArticulation ||
                                f == ArticulationFlag::DNC ||
                                f == ArticulationFlag::RealAudioDrum);
            const auto sev = lossy ? CapabilityGapSeverity::LossyAdaptationRequired
                                   : CapabilityGapSeverity::Advisory;
            bumpSeverity(sev);
            r.items.push_back({
                std::string("CAP_GAP_ARTICULATION_") + articulationFlagName(f),
                "articulation",
                sev,
                std::string("supported (") + articulationFlagName(f) + ")",
                "unsupported",
                "Sanitize velocity-switched/key-switched articulation notes or remap to target-native articulation.",
            });
        }
    }

    static constexpr std::array<ChordCapabilityFlag, 10> kChordFlags = {
        ChordCapabilityFlag::Maj,
        ChordCapabilityFlag::Min,
        ChordCapabilityFlag::Dom7,
        ChordCapabilityFlag::Maj7,
        ChordCapabilityFlag::Min7,
        ChordCapabilityFlag::Dim,
        ChordCapabilityFlag::Dim7,
        ChordCapabilityFlag::Aug,
        ChordCapabilityFlag::Sus,
        ChordCapabilityFlag::Slash,
    };
    for (ChordCapabilityFlag cf : kChordFlags) {
        if (src.supportsChordFlag(cf) && !dst.supportsChordFlag(cf)) {
            r.missing_chord_capabilities.push_back(cf);
            bumpSeverity(CapabilityGapSeverity::Advisory);
            r.items.push_back({
                std::string("CAP_GAP_CHORD_") + chordCapabilityFlagName(cf),
                "chord_response",
                CapabilityGapSeverity::Advisory,
                std::string("native (") + chordCapabilityFlagName(cf) + ")",
                "folded_or_unsupported",
                "Fold unsupported source chord quality into nearest supported target chord table.",
            });
        }
    }

    if (src.sound_addressing.drum_user_bank_max > dst.sound_addressing.drum_user_bank_max) {
        bumpSeverity(CapabilityGapSeverity::Advisory);
        r.items.push_back({
            "CAP_GAP_DRUM_USER_BANK_LIMIT",
            "sound_addressing",
            CapabilityGapSeverity::Advisory,
            "banks " + std::to_string(src.sound_addressing.drum_user_bank_min) + ".." +
                std::to_string(src.sound_addressing.drum_user_bank_max),
            "banks " + std::to_string(dst.sound_addressing.drum_user_bank_min) + ".." +
                std::to_string(dst.sound_addressing.drum_user_bank_max),
            "Reject out-of-spec drum user banks fail-closed and fall back to verified target kits.",
        });
    }

    return r;
}

inline std::string formatCapabilityGapReportJson(const CapabilityGapReport& r) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": \"" << target_cap_detail::jsonEscape(r.schema) << "\",\n";
    os << "  \"sourcePlatform\": \"" << target_cap_detail::jsonEscape(r.source_key) << "\",\n";
    os << "  \"targetPlatform\": \"" << target_cap_detail::jsonEscape(r.target_key) << "\",\n";
    os << "  \"maxSeverity\": \"" << capabilityGapSeverityName(r.max_severity) << "\",\n";
    os << "  \"trackCountDelta\": " << r.track_count_delta << ",\n";
    os << "  \"cvCountDelta\": " << r.cv_count_delta << ",\n";
    os << "  \"breakBarDelta\": " << r.break_bar_delta << ",\n";
    os << "  \"polyphonyDelta\": " << r.polyphony_delta << ",\n";
    os << "  \"ppqnResampleRecommended\": " << (r.ppqn_resample_recommended ? "true" : "false") << ",\n";
    os << "  \"hardwareVerified\": " << (r.hardware_verified ? "true" : "false") << ",\n";
    os << "  \"l3Status\": \"" << target_cap_detail::jsonEscape(r.l3_status) << "\",\n";
    os << "  \"items\": [";
    for (size_t i = 0; i < r.items.size(); ++i) {
        const auto& it = r.items[i];
        if (i > 0) os << ",";
        os << "\n    {\"code\": \"" << target_cap_detail::jsonEscape(it.code)
           << "\", \"dimension\": \"" << target_cap_detail::jsonEscape(it.dimension)
           << "\", \"severity\": \"" << capabilityGapSeverityName(it.severity)
           << "\", \"source\": \"" << target_cap_detail::jsonEscape(it.source_value)
           << "\", \"target\": \"" << target_cap_detail::jsonEscape(it.target_value)
           << "\", \"mitigation\": \"" << target_cap_detail::jsonEscape(it.mitigation) << "\"}";
    }
    if (!r.items.empty()) os << "\n  ";
    os << "]\n}\n";
    return os.str();
}

} // namespace ai_arranger::intelligence

#endif // AI_ARRANGER_INTELLIGENCE_TARGET_CAPABILITY_H
