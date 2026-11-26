#ifndef AI_ARRANGER_EXPORTERS_KORG_PA5X_SOUND_MATCHER_H
#define AI_ARRANGER_EXPORTERS_KORG_PA5X_SOUND_MATCHER_H

#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/korg_style_smf.h"
#include "exporters/korg/sound_map.h"
#include "exporters/korg/timbre_profiler.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Track B Milestone Q4a: Standalone, header-only Korg Pa5X Factory Catalog &
// Read-Only Target-KST Sound Matcher.
//
// Boundaries, HOLD Gates & Epistemic Discipline:
//   * Pure C++20 + exporters/korg/ headers ONLY: zero dependency on src/media/
//     or apps/macos/, zero JUCE, zero direct .PCG/.KMP/.PCM disk I/O.
//   * Strictly READ-ONLY & OPT-IN (`enable_factory_catalog = false` by default,
//     preserving `UNRESOLVED` behavior unless explicitly opted in).
//   * User `--sound-map` ALWAYS wins unconditionally (`MatchOrigin::UserSoundMap`,
//     `epistemic_tag = "USER_VERIFIED"`).
//   * Every Pa5X Factory ROM catalog entry is explicitly tagged
//     `"DOCUMENTED_NOT_HW_VERIFIED"` (`hardware_verified = false`, `L3 = NOT_TESTED`)
//     and documents the K8/K9 0-based vs 1-based Program Change & Import SMF caveat.
//   * Target-KST matching consumes strictly an injected `TargetSoundCatalogView`
//     (POD mirror of `media::selective::TargetOccupancyMap`), enforcing `isInSpec()`
//     (rejecting out-of-spec `USERDK07..12` banks), strict drum/melodic separation,
//     and fail-closed minimum auto-select score (`>= 70`).
//   * Zero private owner Sound/Kit/Bank names, bytes, or local paths anywhere.

namespace ai_arranger::exporters::korg::quality {

// ── Epistemic & Hardware-Verification Constants ──────────────────────────────

inline constexpr const char* kEpistemicUserVerified      = "USER_VERIFIED";
inline constexpr const char* kEpistemicTargetKstOccupied = "TARGET_KST_OCCUPIED_UNVERIFIED_EAR";
inline constexpr const char* kEpistemicFactoryDocumented = "DOCUMENTED_NOT_HW_VERIFIED";
inline constexpr const char* kL3StatusNotTested          = "NOT_TESTED";

inline constexpr const char* kPa5xK8K9Caveat =
    "K8/K9 caveat: Korg Pa5X User Manual v1.4 (MAN5 pp.1572-1622) appendix does not "
    "explicitly state 0-based vs 1-based Program Change (entries follow 0-based GM2 "
    "alignment, e.g. Austrian Grand = 121/20/0), and tick-0 CC0/CC32/PC survival on "
    "Pa5X Import SMF is unverified on hardware (hardwareVerified=false, L3=NOT_TESTED).";

// ── Match Origin & Read-Only Target-KST POD Interface ────────────────────────

enum class MatchOrigin : uint8_t {
    UserSoundMap = 0,     // Highest priority: explicit --sound-map entry (never overridden!)
    TargetKstUserSound,   // Matched read-only from injected TargetSoundCatalogView (USER01..12 / USERDK01..06)
    Pa5xFactoryCatalog    // Curated MAN5 pp.1572-1622 catalog (explicitly DOCUMENTED_NOT_HW_VERIFIED)
};

inline constexpr const char* matchOriginName(MatchOrigin origin) noexcept {
    switch (origin) {
        case MatchOrigin::UserSoundMap:       return "UserSoundMap";
        case MatchOrigin::TargetKstUserSound: return "TargetKstUserSound";
        case MatchOrigin::Pa5xFactoryCatalog: return "Pa5xFactoryCatalog";
    }
    return "Pa5xFactoryCatalog";
}

// Lightweight POD mirror of `ai_arranger::media::selective::SlotAddress`
// (avoids any circular dependency from exporter-korg -> src/media/).
struct TargetSlotAddress {
    bool     is_drum_kit = false;
    uint32_t bank        = 1;     // 1..12 melodic (USER01..12); 1..6 in-spec DK (USERDK01..06), 7..12 out-of-spec
    uint32_t slot        = 0;     // 0..127

    constexpr uint8_t msb() const noexcept { return is_drum_kit ? 120u : 121u; }
    constexpr uint8_t lsb() const noexcept { return static_cast<uint8_t>(63u + bank); }
    constexpr uint8_t pc()  const noexcept { return static_cast<uint8_t>(slot); }

    constexpr bool isInSpec() const noexcept {
        const uint32_t max_bank = is_drum_kit ? 6u : 12u;
        return bank >= 1u && bank <= max_bank && slot < 128u;
    }

    constexpr bool isOutOfSpecDrumKitBank() const noexcept {
        return is_drum_kit && bank >= 7u && bank <= 12u && slot < 128u;
    }

    std::string shortLabel() const {
        char buf[32];
        std::snprintf(buf, sizeof(buf), is_drum_kit ? "USERDK%02u/%u" : "USER%02u/%u", bank, slot);
        return std::string(buf);
    }

    static std::optional<TargetSlotAddress> fromMidi(uint8_t msb_v, uint8_t lsb_v, uint8_t pc_v) noexcept {
        if ((msb_v != 121u && msb_v != 120u) || lsb_v < 64u || lsb_v > 75u || pc_v >= 128u) {
            return std::nullopt;
        }
        return TargetSlotAddress{msb_v == 120u, static_cast<uint32_t>(lsb_v - 63u), static_cast<uint32_t>(pc_v)};
    }
};

// Read-only Target KST sound slot entry injected by CLI/adapter from TargetOccupancyMap.
struct TargetSoundEntry {
    uint8_t  msb         = 121;
    uint8_t  lsb         = 64;
    uint8_t  pc          = 0;
    bool     is_drum_kit = false;
    bool     in_spec     = true;  // Defense-in-depth: matcher ALSO verifies MIDI/bank range (rejects USERDK07..12!)
    std::string display_name;
    std::optional<TargetSlotAddress> slot_address;
    std::optional<uint8_t>  gm_alignment_score; // optional 0..100 hint (e.g. from Track A DrumKitProfile)
    std::optional<uint32_t> family_bitmask;     // optional bitmask of timbreFamilyBit(TimbreFamily)
};

struct TargetSoundCatalogView {
    std::vector<TargetSoundEntry> entries;
    bool        is_partial_inspect = false;
    std::string partial_notice;
};

// Defense-in-depth validation: strictly enforces Pa5X documented User Sound / Drum Kit grid
// (USER01..12 = MSB 121 LSB 64..75 PC 0..127; USERDK01..06 = MSB 120 LSB 64..69 PC 0..127).
// Out-of-spec USERDK07..12 (LSB 70..75 / bank 7..12) are ALWAYS rejected!
inline bool isTargetSoundEntryInSpec(const TargetSoundEntry& entry) noexcept {
    if (!entry.in_spec) return false;
    if (entry.pc >= 128u) return false;
    if (entry.is_drum_kit) {
        if (entry.msb != 120u || entry.lsb < 64u || entry.lsb > 69u) return false;
    } else {
        if (entry.msb != 121u || entry.lsb < 64u || entry.lsb > 75u) return false;
    }
    if (entry.slot_address.has_value()) {
        const auto& sa = *entry.slot_address;
        if (sa.is_drum_kit != entry.is_drum_kit) return false;
        if (!sa.isInSpec()) return false;
        if (sa.msb() != entry.msb || sa.lsb() != entry.lsb || sa.pc() != entry.pc) return false;
    }
    return true;
}

// Structural zero-dependency adapter converting `media::selective::TargetOccupancyMap`
// (or any structurally compatible map) into `TargetSoundCatalogView` without `#include "media/..."`.
template <typename OccupancyMapT>
inline TargetSoundCatalogView buildTargetSoundCatalogFromOccupancy(const OccupancyMapT& target_map) {
    TargetSoundCatalogView view{};
    view.is_partial_inspect = target_map.partialInspect.isPartial;
    view.partial_notice     = target_map.partialInspect.notice;
    view.entries.reserve(target_map.occupiedSlots.size());
    for (const auto& [addr, slot_entry] : target_map.occupiedSlots) {
        TargetSoundEntry e{};
        e.msb          = addr.msb();
        e.lsb          = addr.lsb();
        e.pc           = addr.pc();
        e.is_drum_kit  = addr.isDrumKit;
        e.in_spec      = addr.isInSpec();
        e.display_name = slot_entry.displayName;
        e.slot_address = TargetSlotAddress{addr.isDrumKit, addr.bank, addr.slot};
        view.entries.push_back(std::move(e));
    }
    return view;
}

// ── Curated Documented Korg Pa5X Factory ROM Catalog (MAN5 pp.1572–1622) ─────

struct Pa5xFactorySoundEntry {
    TimbreFamily family       = TimbreFamily::Unknown;
    const char*  display_name = "";
    const char*  sub_tags     = ""; // space-separated sub-family match tokens
    SoundRef     ref{};
    bool         is_drum_kit  = false;
    bool         is_dnc       = false;
    const char*  man5_ref     = "MAN5 pp.1572-1622";
    const char*  epistemic    = kEpistemicFactoryDocumented;
    bool         hardware_verified = false;
    const char*  l3_status    = kL3StatusNotTested;
};

// Public Korg Pa5X Factory Sound Catalog covering all 21 concrete TimbreFamilies
// and key sub-families. Every entry carries explicit `DOCUMENTED_NOT_HW_VERIFIED`
// epistemic tag, `hardware_verified = false`, `l3_status = "NOT_TESTED"`, and K8/K9 caveat.
//
// Sources:
//   * 14 named Pa5X factory sounds verified against Korg Pa5X User Manual v1.4
//     appendix (also listed in docs/exporters/sound-maps/pa5x-starter.soundmap).
//   * Additional sub-families use documented Korg Pa5X GM2 bank alignment
//     (MSB 121 LSB 0 PC 0..127 melodic; MSB 120 LSB 0 PC 0..56 drum/perc kits,
//     MAN5 pp.1572-1622).
inline const std::vector<Pa5xFactorySoundEntry>& pa5xFactoryCatalog() {
    static const std::vector<Pa5xFactorySoundEntry> kCatalog = {
        // ── Piano ──
        {TimbreFamily::Piano, "Austrian Grand",        "acoustic grand concert piano",      {121, 20,  0}, false, false, "MAN5 p.1572", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::Piano, "Bright Acoustic Piano", "bright pop upright piano",          {121,  0,  1}, false, false, "MAN5 p.1572 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::Piano, "Harpsichord",           "harpsichord harpsi cembalo",        {121,  0,  6}, false, false, "MAN5 p.1573 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Electric Piano ──
        {TimbreFamily::ElectricPiano, "MK I Stage",    "rhodes stage tine electric piano ep", {121, 46,  4}, false, false, "MAN5 p.1573", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::ElectricPiano, "Wurlitzer EP",  "wurlitzer wurl reed electric piano",  {121,  0,  4}, false, false, "MAN5 p.1573 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::ElectricPiano, "DX / FM EP",    "dx fm digital electric piano ep",     {121,  0,  5}, false, false, "MAN5 p.1573 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::ElectricPiano, "Clavinet",      "clav clavinet funk electric piano",   {121,  0,  7}, false, false, "MAN5 p.1573 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Organ ──
        {TimbreFamily::Organ, "Jimmy Organ",           "drawbar jazz b3 hammond rotary organ", {121, 13, 18}, false, false, "MAN5 p.1576", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::Organ, "Drawbar Organ GM2",     "drawbar percussive rock organ",        {121,  0, 16}, false, false, "MAN5 p.1576 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::Organ, "Church Pipe Organ",     "church pipe cathedral reed organ",     {121,  0, 19}, false, false, "MAN5 p.1576 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Accordion ──
        {TimbreFamily::Accordion, "Accordion GM2",     "accordion musette french folk",        {121,  0, 21}, false, false, "MAN5 p.1576 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::Accordion, "Harmonica GM2",     "harmonica blues reed",                 {121,  0, 22}, false, false, "MAN5 p.1576 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::Accordion, "Tango Bandoneon",   "tango bandoneon accordion",            {121,  0, 23}, false, false, "MAN5 p.1576 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Acoustic Guitar ──
        {TimbreFamily::AcousticGuitar, "Classic Gtr 1 DNC", "nylon classic flamenco spanish acoustic guitar", {121, 29, 24}, false, true,  "MAN5 p.1577", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::AcousticGuitar, "Steel Guitar1 DNC", "steel folk acoustic strum guitar",               {121, 47, 25}, false, true,  "MAN5 p.1577", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Electric Guitar ──
        {TimbreFamily::ElectricGuitar, "Strat N Cln DNC",        "clean strat pop muted electric guitar",      {121, 38, 27}, false, true,  "MAN5 p.1577", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::ElectricGuitar, "Jazz Electric Guitar",   "jazz hollow electric guitar",                {121,  0, 26}, false, false, "MAN5 p.1577 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::ElectricGuitar, "Distortion Guitar GM2",  "distortion overdrive rock crunch electric guitar", {121,  0, 30}, false, false, "MAN5 p.1577 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Plucked Strings ──
        {TimbreFamily::PluckedStrings, "Orchestral Harp GM2",    "harp orchestral plucked strings",            {121,  0, 46}, false, false, "MAN5 p.1578 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Acoustic Bass ──
        {TimbreFamily::AcousticBass, "Acoustic Bass 1",          "acoustic upright double wood bass",          {121, 13, 32}, false, false, "MAN5 p.1588", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Electric Bass ──
        {TimbreFamily::ElectricBass, "JBass Fing DN1",           "finger jbass electric bass",                 {121, 29, 33}, false, true,  "MAN5 p.1588", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::ElectricBass, "Pick Electric Bass GM2",   "pick rock electric bass",                    {121,  0, 34}, false, false, "MAN5 p.1588 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::ElectricBass, "Slap Electric Bass GM2",   "slap funk fretless electric bass",           {121,  0, 36}, false, false, "MAN5 p.1588 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Synth Bass ──
        {TimbreFamily::SynthBass, "Synth Bass 1 GM2",            "synth analog sub bass",                      {121,  0, 38}, false, false, "MAN5 p.1588 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Strings ──
        {TimbreFamily::Strings, "StringEnsembleDNC",             "string ensemble orchestral sustained strings", {121, 13, 49}, false, true,  "MAN5 p.1578", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::Strings, "Solo Violin GM2",               "solo violin viola cello strings",              {121,  0, 40}, false, false, "MAN5 p.1578 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Choir / Vocal Pad ──
        {TimbreFamily::ChoirPad, "Choir Aahs GM2",               "choir vocal voice aah ooh pad",              {121,  0, 52}, false, false, "MAN5 p.1580 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Synth Pad ──
        {TimbreFamily::SynthPad, "OB Pad",                       "warm analog ob synth pad sweep",             {121, 22, 89}, false, false, "MAN5 p.1582", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::SynthPad, "Polysynth Pad GM2",            "polysynth new age halo crystal pad",         {121,  0, 90}, false, false, "MAN5 p.1582 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Brass ──
        {TimbreFamily::Brass, "Big Band Brass 1",                "brass section big band horn synth brass",    {121, 32, 61}, false, false, "MAN5 p.1581", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::Brass, "Trumpet GM2",                     "trumpet muted flugelhorn brass",             {121,  0, 56}, false, false, "MAN5 p.1581 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::Brass, "Trombone GM2",                    "trombone tuba brass",                        {121,  0, 57}, false, false, "MAN5 p.1581 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Sax / Reed ──
        {TimbreFamily::SaxReed, "Tenor Sax GM2",                 "tenor soprano baritone sax saxophone",       {121,  0, 66}, false, false, "MAN5 p.1581 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::SaxReed, "Alto Sax GM2",                  "alto sax saxophone",                         {121,  0, 65}, false, false, "MAN5 p.1581 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::SaxReed, "Clarinet / Oboe GM2",           "clarinet oboe bassoon reed woodwind",        {121,  0, 71}, false, false, "MAN5 p.1581 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Flute / Pipe ──
        {TimbreFamily::FlutePipe, "Flute GM2",                   "flute piccolo recorder whistle pipe",        {121,  0, 73}, false, false, "MAN5 p.1581 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::FlutePipe, "Pan Flute GM2",               "pan flute shakuhachi ocarina pipe",          {121,  0, 75}, false, false, "MAN5 p.1581 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Synth Lead ──
        {TimbreFamily::SynthLead, "Mini Classic 1",              "mini analog sawtooth mono synth lead",       {121, 26, 87}, false, false, "MAN5 p.1584", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::SynthLead, "Square Lead GM2",             "square synth lead arp",                      {121,  0, 80}, false, false, "MAN5 p.1584 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Mallet / Chromatic Percussion ──
        {TimbreFamily::MalletPerc, "Vibraphone GM2",             "vibraphone vibe marimba mallet bell celesta", {121,  0, 11}, false, false, "MAN5 p.1574 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::MalletPerc, "Marimba GM2",                "marimba xylophone glockenspiel mallet",       {121,  0, 12}, false, false, "MAN5 p.1574 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Ethnic Melodic ──
        {TimbreFamily::EthnicMelodic, "Sitar / Ethnic GM2",      "sitar oud koto shamisen banjo ethnic melodic", {121,  0, 104}, false, false, "MAN5 p.1586 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Drum Kits (MSB 120 LSB 0) ──
        {TimbreFamily::DrumKit, "Pop Kit 1 RR",                  "standard pop studio room acoustic revo drum kit", {120, 0, 100}, true, true,  "MAN5 p.1621", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::DrumKit, "Brush Drum Kit GM2",            "brush jazz acoustic drum kit",                    {120, 0,  40}, true, false, "MAN5 p.1621 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::DrumKit, "Rock / Power Kit GM2",          "rock power heavy stadium drum kit",               {120, 0,  16}, true, false, "MAN5 p.1621 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::DrumKit, "Electronic / Analog Kit GM2",   "electronic analog 808 909 dance edm drum kit",    {120, 0,  24}, true, false, "MAN5 p.1621 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::DrumKit, "Jazz Drum Kit GM2",             "jazz swing drum kit",                             {120, 0,  32}, true, false, "MAN5 p.1621 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::DrumKit, "Ethnic / Oriental Drum Kit GM2","ethnic oriental arabic tabla drum kit",           {120, 0,  48}, true, false, "MAN5 p.1622 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested},

        // ── Percussion Kits (MSB 120 LSB 0) ──
        {TimbreFamily::PercussionKit, "Standard Perc Kit",       "standard latin percussion conga bongo shaker kit", {120, 0,  69}, true, false, "MAN5 p.1622", kEpistemicFactoryDocumented, false, kL3StatusNotTested},
        {TimbreFamily::PercussionKit, "Ethnic / Oriental Perc Kit", "ethnic oriental arabic sfx percussion kit",     {120, 0,  56}, true, false, "MAN5 p.1622 (GM2)", kEpistemicFactoryDocumented, false, kL3StatusNotTested}
    };
    return kCatalog;
}

// ── Sound Match Contracts ────────────────────────────────────────────────────

struct SoundCandidate {
    SoundRef    ref{};
    std::string display_name;
    MatchOrigin origin            = MatchOrigin::Pa5xFactoryCatalog;
    uint8_t     score             = 0;     // 0..100 weighted multi-attribute similarity
    std::string epistemic_tag     = kEpistemicFactoryDocumented;
    bool        hardware_verified = false;
    std::string l3_status         = kL3StatusNotTested;
    std::string caveat;
    std::optional<TargetSlotAddress> target_slot; // populated iff TargetKstUserSound
    std::string rationale;
};

struct TrackSoundMatch {
    uint8_t                     korg_ch = 0xFF;
    TrackTimbreProfile          profile;
    std::vector<SoundCandidate> top_candidates; // sorted desc by score, max options.max_candidates
    std::optional<SoundRef>     selected;       // populated ONLY if UserSoundMap OR (opt-in enabled && score >= threshold)
    MatchOrigin                 selected_origin = MatchOrigin::Pa5xFactoryCatalog;
    std::string                 selected_epistemic_tag;
    // Per-element mid-style voice override matches (populated when section voice differs from dominant track voice):
    std::map<uasf::SectionType, SoundCandidate> element_candidates;
    std::map<uasf::SectionType, SoundRef>       element_selected;
};

struct SoundMatchOptions {
    bool    enable_factory_catalog    = false; // opt-in (--auto-sound-map); default OFF preserves UNRESOLVED!
    bool    prefer_target_user_sounds = false; // opt-in (--prefer-target-user-sounds)
    uint8_t min_auto_select_score     = 70;    // below 70 stays UNRESOLVED (fail-closed!)
    uint8_t target_user_bonus         = 10;    // tie-breaker bonus for high-confidence Target KST matches
    size_t  max_candidates            = 3;     // top-N candidates retained per track
};

struct StyleSoundMatchResult {
    std::vector<TrackSoundMatch> tracks;
    std::map<uint8_t, SoundRef>  resolved_sounds;                                // ready for MapOptions::sounds
    std::map<std::pair<uasf::SectionType, uint8_t>, SoundRef> element_sounds;    // ready for MapOptions::element_sounds
    uint32_t target_entries_considered  = 0;
    uint32_t target_entries_skipped_oos = 0; // out-of-spec Target entries skipped (e.g. USERDK07..12)
    bool     target_partial_inspect     = false;
    std::string target_partial_notice;
    bool     hardware_verified          = false;
    std::string l3_status               = kL3StatusNotTested;
};

namespace matcher_detail {

inline bool isStopWord(std::string_view w) noexcept {
    static constexpr std::array<const char*, 12> kStop = {
        "1", "2", "3", "gm2", "dnc", "dn1", "rr", "kit", "sound", "signal", "inferred", "megavoice"
    };
    for (const char* s : kStop) {
        if (w == s) return true;
    }
    return false;
}

inline uint8_t computeSubFamilyOverlapBonus(std::string_view query_sub,
                                            std::string_view candidate_tags,
                                            std::string_view candidate_name) {
    const auto q_toks = timbre_detail::tokenizeSafeWords(query_sub);
    if (q_toks.empty()) return 0;

    auto c_toks = timbre_detail::tokenizeSafeWords(candidate_tags);
    const auto name_toks = timbre_detail::tokenizeSafeWords(candidate_name);
    for (const auto& nt : name_toks) {
        if (std::find(c_toks.begin(), c_toks.end(), nt) == c_toks.end()) {
            c_toks.push_back(nt);
        }
    }

    uint32_t meaningful_q = 0;
    uint32_t hits = 0;
    for (const auto& qt : q_toks) {
        if (qt.size() <= 1 || isStopWord(qt)) continue;
        ++meaningful_q;
        if (timbre_detail::hasToken(c_toks, qt)) ++hits;
    }
    if (meaningful_q == 0 || hits == 0) return 0;
    const double ratio = static_cast<double>(hits) / static_cast<double>(meaningful_q);
    return static_cast<uint8_t>(std::min<int>(16, static_cast<int>(std::lround(ratio * 16.0))));
}

inline bool areFamiliesRelated(TimbreFamily a, TimbreFamily b) noexcept {
    if (a == b || a == TimbreFamily::Unknown || b == TimbreFamily::Unknown) return false;
    auto pairMatch = [&](TimbreFamily x, TimbreFamily y) {
        return (a == x && b == y) || (a == y && b == x);
    };
    return pairMatch(TimbreFamily::Piano, TimbreFamily::ElectricPiano) ||
           pairMatch(TimbreFamily::AcousticGuitar, TimbreFamily::ElectricGuitar) ||
           pairMatch(TimbreFamily::AcousticGuitar, TimbreFamily::PluckedStrings) ||
           pairMatch(TimbreFamily::AcousticBass, TimbreFamily::ElectricBass) ||
           pairMatch(TimbreFamily::ElectricBass, TimbreFamily::SynthBass) ||
           pairMatch(TimbreFamily::Strings, TimbreFamily::SynthPad) ||
           pairMatch(TimbreFamily::ChoirPad, TimbreFamily::SynthPad) ||
           pairMatch(TimbreFamily::Brass, TimbreFamily::SaxReed) ||
           pairMatch(TimbreFamily::SaxReed, TimbreFamily::FlutePipe) ||
           pairMatch(TimbreFamily::DrumKit, TimbreFamily::PercussionKit);
}

inline bool wantsDrumKit(const TrackTimbreProfile& profile) noexcept {
    if (profile.korg_ch == kDrumCh || profile.korg_ch == kPercCh) return true;
    if (profile.korg_ch == kBassCh || (profile.korg_ch >= kAcc1Ch && profile.korg_ch <= 15u)) return false;
    return isPercussiveTimbreFamily(profile.inferred.family);
}

inline bool soundRefEquals(const SoundRef& a, const SoundRef& b) noexcept {
    return a.msb == b.msb && a.lsb == b.lsb && a.pc == b.pc;
}

inline const char* sectionShortName(uasf::SectionType t) noexcept {
    const char* m = korgMarkerFor(t, BreakPolicy::Yamaha1BarCap);
    return m ? m : "unknown";
}

inline int originSortRank(MatchOrigin origin, bool prefer_target) noexcept {
    if (origin == MatchOrigin::UserSoundMap) return 0;
    if (prefer_target) {
        return origin == MatchOrigin::TargetKstUserSound ? 1 : 2;
    }
    return origin == MatchOrigin::Pa5xFactoryCatalog ? 1 : 2;
}

inline void sortAndTruncateCandidates(std::vector<SoundCandidate>& candidates,
                                      bool prefer_target,
                                      size_t max_candidates) {
    std::sort(candidates.begin(), candidates.end(), [&](const SoundCandidate& a, const SoundCandidate& b) {
        // UserSoundMap ALWAYS ranks #1 unconditionally.
        if (a.origin == MatchOrigin::UserSoundMap && b.origin != MatchOrigin::UserSoundMap) return true;
        if (b.origin == MatchOrigin::UserSoundMap && a.origin != MatchOrigin::UserSoundMap) return false;
        if (a.score != b.score) return a.score > b.score;
        const int ra = originSortRank(a.origin, prefer_target);
        const int rb = originSortRank(b.origin, prefer_target);
        if (ra != rb) return ra < rb;
        const auto ta = std::make_tuple(a.ref.msb, a.ref.lsb, a.ref.pc);
        const auto tb = std::make_tuple(b.ref.msb, b.ref.lsb, b.ref.pc);
        if (ta != tb) return ta < tb;
        return a.display_name < b.display_name;
    });
    if (max_candidates > 0 && candidates.size() > max_candidates) {
        candidates.resize(max_candidates);
    }
}

inline std::vector<SoundCandidate> scoreCandidatesForProfile(
    const TrackTimbreProfile& profile,
    const TargetSoundCatalogView* target_catalog,
    const SoundMatchOptions& opt,
    uint32_t* out_considered,
    uint32_t* out_skipped_oos) {
    std::vector<SoundCandidate> candidates;
    const bool want_drum = wantsDrumKit(profile);
    const TimbreFamily fam = profile.inferred.family;

    if (fam == TimbreFamily::Unknown) {
        // Still audit target catalog out-of-spec count if provided, but fail closed with 0 candidates.
        if (target_catalog && (out_considered || out_skipped_oos)) {
            for (const auto& entry : target_catalog->entries) {
                if (!isTargetSoundEntryInSpec(entry)) {
                    if (out_skipped_oos) ++(*out_skipped_oos);
                } else {
                    if (out_considered) ++(*out_considered);
                }
            }
        }
        return candidates;
    }

    // 1. Score Curated Documented Pa5X Factory Catalog
    for (const auto& entry : pa5xFactoryCatalog()) {
        if (entry.is_drum_kit != want_drum) continue;

        int raw_score = 0;
        std::string why;
        if (entry.family == fam) {
            // Scale base cleanly when profile confidence is very low (< 40 fails closed < 70).
            const int base = (profile.confidence >= 40) ? 74 : 52;
            const int sub_bonus = static_cast<int>(
                computeSubFamilyOverlapBonus(profile.inferred.sub_family, entry.sub_tags, entry.display_name));
            const int dnc_bonus = (profile.inferred.prefers_dnc && entry.is_dnc)
                                      ? 5
                                      : ((!profile.inferred.prefers_dnc && !entry.is_dnc) ? 1 : 0);
            const int conf_bonus = (profile.confidence >= 85) ? 4 : (profile.confidence >= 70 ? 2 : 0);
            raw_score = std::clamp(base + sub_bonus + dnc_bonus + conf_bonus, 0, 98);
            why = std::string("exact-family:") + timbreFamilyName(fam) +
                  " (" + entry.man5_ref + ", sub=+" + std::to_string(sub_bonus) +
                  (dnc_bonus > 0 ? ", dnc=+" + std::to_string(dnc_bonus) : "") + ")";
        } else if (areFamiliesRelated(entry.family, fam)) {
            // Related family stays strictly below 70 (advisory only, never auto-selected).
            const int sub_bonus = static_cast<int>(
                computeSubFamilyOverlapBonus(profile.inferred.sub_family, entry.sub_tags, entry.display_name));
            raw_score = std::clamp(50 + sub_bonus / 2, 0, 62);
            why = std::string("related-family:") + timbreFamilyName(entry.family);
        } else {
            continue;
        }

        SoundCandidate cand{};
        cand.ref               = entry.ref;
        cand.display_name      = entry.display_name;
        cand.origin            = MatchOrigin::Pa5xFactoryCatalog;
        cand.score             = static_cast<uint8_t>(raw_score);
        cand.epistemic_tag     = entry.epistemic; // "DOCUMENTED_NOT_HW_VERIFIED"
        cand.hardware_verified = entry.hardware_verified; // false
        cand.l3_status         = entry.l3_status;         // "NOT_TESTED"
        cand.caveat            = kPa5xK8K9Caveat;
        cand.rationale         = std::move(why);
        candidates.push_back(std::move(cand));
    }

    // 2. Score Read-Only Injected Target-KST User Sounds (strictly isInSpec() + drum/melodic separation)
    if (target_catalog) {
        for (const auto& entry : target_catalog->entries) {
            if (!isTargetSoundEntryInSpec(entry)) {
                if (out_skipped_oos) ++(*out_skipped_oos);
                continue;
            }
            if (out_considered) ++(*out_considered);

            // Strict drum vs melodic separation
            if (entry.is_drum_kit != want_drum) continue;

            const uasf::TrackRole entry_role = entry.is_drum_kit
                                                   ? (profile.korg_ch == kPercCh ? uasf::TrackRole::Percussion : uasf::TrackRole::Drum)
                                                   : uasf::TrackRole::Accompaniment;
            const auto kw = timbre_detail::classifyByKeywords(entry.display_name, entry_role, profile.korg_ch);

            bool exact_family = false;
            bool related_family = false;
            if (entry.family_bitmask.has_value()) {
                exact_family = ((*entry.family_bitmask & timbreFamilyBit(fam)) != 0u);
            }
            if (!exact_family && kw.confidence > 0) {
                if (kw.sub.family == fam) exact_family = true;
                else if (areFamiliesRelated(kw.sub.family, fam)) related_family = true;
            }
            if (!exact_family && !related_family) continue;

            int base_score = 0;
            if (exact_family) {
                const int base = (profile.confidence >= 40) ? 74 : 52;
                const int sub_bonus = static_cast<int>(
                    computeSubFamilyOverlapBonus(profile.inferred.sub_family, kw.sub.sub_family, entry.display_name));
                const int gm_bonus = (entry.gm_alignment_score.has_value() && *entry.gm_alignment_score >= 80u) ? 4 : 0;
                const int dnc_bonus = (profile.inferred.prefers_dnc && kw.sub.prefers_dnc) ? 4 : 0;
                base_score = std::clamp(base + sub_bonus + gm_bonus + dnc_bonus, 0, 96);
            } else {
                // Related Target sound stays strictly below threshold (never falsely nominated).
                base_score = 54;
            }

            // Apply `target_user_bonus` ONLY when base match is already high-confidence (>= min_auto_select_score)
            // so the bonus acts purely as a tie-breaker favoring Target KST sounds, never lifting weak matches.
            int final_score = base_score;
            const bool bonus_applied = opt.prefer_target_user_sounds &&
                                       exact_family &&
                                       base_score >= static_cast<int>(opt.min_auto_select_score);
            if (bonus_applied) {
                final_score = std::min<int>(99, base_score + static_cast<int>(opt.target_user_bonus));
            }

            std::optional<TargetSlotAddress> slot = entry.slot_address;
            if (!slot.has_value()) {
                slot = TargetSlotAddress::fromMidi(entry.msb, entry.lsb, entry.pc);
            }

            SoundCandidate cand{};
            cand.ref               = SoundRef{entry.msb, entry.lsb, entry.pc};
            cand.display_name      = entry.display_name;
            cand.origin            = MatchOrigin::TargetKstUserSound;
            cand.score             = static_cast<uint8_t>(final_score);
            cand.epistemic_tag     = kEpistemicTargetKstOccupied;
            cand.hardware_verified = false;
            cand.l3_status         = kL3StatusNotTested;
            cand.target_slot       = slot;
            cand.rationale         = std::string(exact_family ? "target-kst:exact-family" : "target-kst:related-family") +
                                     (slot.has_value() ? (" slot=" + slot->shortLabel()) : "") +
                                     (bonus_applied ? (" +" + std::to_string(opt.target_user_bonus) + " target-bonus") : "");
            candidates.push_back(std::move(cand));
        }
    }

    return candidates;
}

} // namespace matcher_detail

// Matches a single profiled track against:
//   1. User `--sound-map` override (ALWAYS wins unconditionally if present for `profile.korg_ch`)
//   2. Optional read-only Target-KST catalog (`target_catalog`)
//   3. Curated documented Pa5X Factory ROM catalog (`DOCUMENTED_NOT_HW_VERIFIED`)
// Also independently matches distinct per-element mid-style voice switches in
// `profile.element_yamaha_voices` (and respects `user_element_sounds` overrides).
inline TrackSoundMatch matchTrackSound(
    const TrackTimbreProfile& profile,
    const std::map<uint8_t, SoundRef>& user_sounds = {},
    const TargetSoundCatalogView* target_catalog = nullptr,
    const SoundMatchOptions& opt = {},
    const std::map<std::pair<uasf::SectionType, uint8_t>, SoundRef>& user_element_sounds = {},
    uint32_t* out_target_considered = nullptr,
    uint32_t* out_target_skipped_oos = nullptr) {
    TrackSoundMatch result{};
    result.korg_ch = profile.korg_ch;
    result.profile = profile;

    auto candidates = matcher_detail::scoreCandidatesForProfile(
        profile, target_catalog, opt, out_target_considered, out_target_skipped_oos);

    const auto user_it = (profile.korg_ch != 0xFF) ? user_sounds.find(profile.korg_ch) : user_sounds.end();
    const bool has_user_override = (user_it != user_sounds.end());

    if (has_user_override) {
        SoundCandidate user_cand{};
        user_cand.ref               = user_it->second;
        user_cand.display_name      = "User Sound Map";
        user_cand.origin            = MatchOrigin::UserSoundMap;
        user_cand.score             = 100;
        user_cand.epistemic_tag     = kEpistemicUserVerified;
        user_cand.hardware_verified = false;
        user_cand.l3_status         = "USER_SUPPLIED";
        user_cand.rationale         = "explicit --sound-map override (always wins)";
        candidates.push_back(std::move(user_cand));
    }

    matcher_detail::sortAndTruncateCandidates(
        candidates, opt.prefer_target_user_sounds, opt.max_candidates);
    result.top_candidates = std::move(candidates);

    // Selection gate:
    //   - UserSoundMap ALWAYS wins unconditionally.
    //   - Otherwise strictly opt-in AND fail-closed >= min_auto_select_score.
    if (has_user_override) {
        result.selected               = user_it->second;
        result.selected_origin        = MatchOrigin::UserSoundMap;
        result.selected_epistemic_tag = kEpistemicUserVerified;
    } else if (!result.top_candidates.empty()) {
        const auto& best = result.top_candidates.front();
        const bool origin_allowed =
            (best.origin == MatchOrigin::Pa5xFactoryCatalog && opt.enable_factory_catalog) ||
            (best.origin == MatchOrigin::TargetKstUserSound &&
             (opt.enable_factory_catalog || opt.prefer_target_user_sounds));
        if (origin_allowed && best.score >= opt.min_auto_select_score) {
            result.selected               = best.ref;
            result.selected_origin        = best.origin;
            result.selected_epistemic_tag = best.epistemic_tag;
        }
    }

    // ── Per-Element Mid-Style Voice Change Matching ──
    const auto dominant_voice = std::make_tuple(profile.yamaha_msb, profile.yamaha_lsb, profile.yamaha_pc);
    for (const auto& [sec_type, el_voice] : profile.element_yamaha_voices) {
        const auto [el_msb, el_lsb, el_pc] = el_voice;
        if (el_voice == dominant_voice) continue;

        // Check explicit user per-element override first.
        const auto uel_it = (profile.korg_ch != 0xFF)
                                ? user_element_sounds.find({sec_type, profile.korg_ch})
                                : user_element_sounds.end();
        if (uel_it != user_element_sounds.end()) {
            SoundCandidate ucand{};
            ucand.ref               = uel_it->second;
            ucand.display_name      = "User Element Sound Override";
            ucand.origin            = MatchOrigin::UserSoundMap;
            ucand.score             = 100;
            ucand.epistemic_tag     = kEpistemicUserVerified;
            ucand.hardware_verified = false;
            ucand.l3_status         = "USER_SUPPLIED";
            ucand.rationale         = "explicit user element_sounds override";
            result.element_candidates[sec_type] = std::move(ucand);
            result.element_selected[sec_type]   = uel_it->second;
            continue;
        }

        if (el_pc < 0) continue;

        // Profile this section's distinct Yamaha voice independently (no stale track name override).
        const uasf::TrackRole el_role = (profile.korg_ch == kDrumCh) ? uasf::TrackRole::Drum
                                      : (profile.korg_ch == kPercCh) ? uasf::TrackRole::Percussion
                                      : (profile.korg_ch == kBassCh) ? uasf::TrackRole::Bass
                                                                     : uasf::TrackRole::Accompaniment;
        const TrackTimbreProfile el_prof = profileTrackTimbre(
            el_msb, el_lsb, el_pc, "", el_role, {}, {}, profile.src_ch, profile.korg_ch);

        auto el_cands = matcher_detail::scoreCandidatesForProfile(
            el_prof, target_catalog, opt, nullptr, nullptr);
        matcher_detail::sortAndTruncateCandidates(el_cands, opt.prefer_target_user_sounds, 1);
        if (el_cands.empty()) continue;

        const auto& best_el = el_cands.front();
        result.element_candidates[sec_type] = best_el;

        // Do not overwrite an explicit user track-level sound unless user supplied element override.
        if (has_user_override) continue;

        const bool origin_allowed =
            (best_el.origin == MatchOrigin::Pa5xFactoryCatalog && opt.enable_factory_catalog) ||
            (best_el.origin == MatchOrigin::TargetKstUserSound &&
             (opt.enable_factory_catalog || opt.prefer_target_user_sounds));
        if (origin_allowed && best_el.score >= opt.min_auto_select_score) {
            if (!result.selected.has_value() || !matcher_detail::soundRefEquals(*result.selected, best_el.ref)) {
                result.element_selected[sec_type] = best_el.ref;
            }
        }
    }

    // Also preserve any explicit user_element_sounds even if element_yamaha_voices had no Yamaha change.
    if (profile.korg_ch != 0xFF) {
        for (const auto& [key, ref] : user_element_sounds) {
            if (key.second != profile.korg_ch) continue;
            if (result.element_selected.count(key.first)) continue;
            SoundCandidate ucand{};
            ucand.ref               = ref;
            ucand.display_name      = "User Element Sound Override";
            ucand.origin            = MatchOrigin::UserSoundMap;
            ucand.score             = 100;
            ucand.epistemic_tag     = kEpistemicUserVerified;
            ucand.hardware_verified = false;
            ucand.l3_status         = "USER_SUPPLIED";
            ucand.rationale         = "explicit user element_sounds override";
            result.element_candidates[key.first] = std::move(ucand);
            result.element_selected[key.first]   = ref;
        }
    }

    return result;
}

// Matches all profiled tracks in a style, producing ready-to-wire `resolved_sounds`
// (`MapOptions::sounds`) and `element_sounds` (`MapOptions::element_sounds`).
inline StyleSoundMatchResult matchStyleSounds(
    const std::vector<TrackTimbreProfile>& profiles,
    const std::map<uint8_t, SoundRef>& user_sounds = {},
    const TargetSoundCatalogView* target_catalog = nullptr,
    const SoundMatchOptions& opt = {},
    const std::map<std::pair<uasf::SectionType, uint8_t>, SoundRef>& user_element_sounds = {}) {
    StyleSoundMatchResult out{};
    out.hardware_verified = false;
    out.l3_status         = kL3StatusNotTested;
    if (target_catalog) {
        out.target_partial_inspect = target_catalog->is_partial_inspect;
        out.target_partial_notice  = target_catalog->partial_notice;
        for (const auto& e : target_catalog->entries) {
            if (isTargetSoundEntryInSpec(e)) ++out.target_entries_considered;
            else ++out.target_entries_skipped_oos;
        }
    }

    out.tracks.reserve(profiles.size());
    for (const auto& prof : profiles) {
        auto m = matchTrackSound(prof, user_sounds, target_catalog, opt, user_element_sounds);
        if (m.korg_ch != 0xFF) {
            if (m.selected.has_value()) {
                // First track on a Korg channel wins if merged, unless overridden by UserSoundMap.
                if (m.selected_origin == MatchOrigin::UserSoundMap || !out.resolved_sounds.count(m.korg_ch)) {
                    out.resolved_sounds[m.korg_ch] = *m.selected;
                }
            }
            for (const auto& [sec_type, el_ref] : m.element_selected) {
                out.element_sounds[{sec_type, m.korg_ch}] = el_ref;
            }
        }
        out.tracks.push_back(std::move(m));
    }

    // Ensure explicit user_sounds for tracks not present in `profiles` are still preserved.
    for (const auto& [kch, ref] : user_sounds) {
        out.resolved_sounds[kch] = ref;
    }
    return out;
}

// Emits a `parseSoundMap()`-compatible `.soundmap` template enriched with ranked
// `# Suggested:` recommendations, scores, Target KST slots, epistemic tags, and
// K8/K9 caveats.
//
// Default behavior (`opt.enable_factory_catalog == false && !opt.prefer_target_user_sounds`):
//   Preserves `<track> unresolved` verbatim on every track (unless overridden in `user_sounds`)
//   while appending ranked advisory `# Suggested:` comments.
// Opt-in behavior (`opt.enable_factory_catalog == true` or `opt.prefer_target_user_sounds == true`):
//   Emits `<track> <msb> <lsb> <pc>  # auto score=<N> <epistemic_tag>` for tracks
//   with `selected.has_value()`, and `<track> unresolved` for fail-closed tracks (`< 70`).
inline std::string formatSoundMapTemplateWithRecommendations(
    const Report& report,
    const StyleSoundMatchResult& matches,
    const std::string& style_name,
    const SoundMatchOptions& opt = {}) {
    static constexpr struct { const char* key; uint8_t ch; } kTracks[] = {
        {"bass", kBassCh}, {"drum", kDrumCh}, {"perc", kPercCh}, {"acc1", 11},
        {"acc2", 12}, {"acc3", 13}, {"acc4", 14}, {"acc5", 15}
    };

    const bool auto_emit = opt.enable_factory_catalog || opt.prefer_target_user_sounds;

    std::string out;
    out += "# Pa5X sound map for " + style_name + "\n";
    out += "# Status: hardwareVerified=false | L3=NOT_TESTED\n";
    out += std::string("# ") + kPa5xK8K9Caveat + "\n";
    out += "# Replace 'unresolved' with '<msb> <lsb> <pc>' confirmed on the Pa5X Sound\n";
    out += "# Select window. Never copy the Yamaha numbers below.\n";
    if (matches.target_partial_inspect && !matches.target_partial_notice.empty()) {
        out += "# Target KST partial inspect notice: " + matches.target_partial_notice + "\n";
    }

    for (const auto& t : kTracks) {
        out += "\n";
        bool any_row = false;
        for (const auto& row : report.tracks) {
            if (row.korg_ch != t.ch) continue;
            any_row = true;
            out += "# Genos ch " + std::to_string(row.src_ch + 1) + " '" + row.name + "'  family: " + row.family +
                   "  Yamaha MSB/LSB/PC " + std::to_string(row.msb) + "/" + std::to_string(row.lsb) + "/" +
                   std::to_string(row.pc) +
                   (row.megavoice ? ("  MEGAVOICE CANDIDATE: " + row.megavoice_why) : "") + "\n";
        }

        const TrackSoundMatch* primary_match = nullptr;
        for (const auto& m : matches.tracks) {
            if (m.korg_ch != t.ch) continue;
            if (!primary_match) primary_match = &m;
            if (!any_row) {
                any_row = true;
                out += "# Genos ch " + std::to_string(m.profile.src_ch + 1) +
                       "  inferred: " + std::string(timbreFamilyName(m.profile.inferred.family)) +
                       " (" + m.profile.inferred.sub_family + ", conf=" +
                       std::to_string(m.profile.confidence) + ")" +
                       "  Yamaha MSB/LSB/PC " + std::to_string(m.profile.yamaha_msb) + "/" +
                       std::to_string(m.profile.yamaha_lsb) + "/" +
                       std::to_string(m.profile.yamaha_pc) + "\n";
            } else {
                out += "# Timbre profile: " + std::string(timbreFamilyName(m.profile.inferred.family)) +
                       " (" + m.profile.inferred.sub_family + ", conf=" +
                       std::to_string(m.profile.confidence) + ", " + m.profile.rationale + ")\n";
            }
            for (size_t rank_idx = 0; rank_idx < m.top_candidates.size(); ++rank_idx) {
                const auto& c = m.top_candidates[rank_idx];
                out += "# Suggested #" + std::to_string(rank_idx + 1) + ": " +
                       std::to_string(c.ref.msb) + " " + std::to_string(c.ref.lsb) + " " +
                       std::to_string(c.ref.pc) + "  \"" + c.display_name + "\"" +
                       "  score=" + std::to_string(c.score) +
                       "  origin=" + matchOriginName(c.origin);
                if (c.target_slot.has_value()) {
                    out += "  slot=" + c.target_slot->shortLabel();
                }
                out += "  [" + c.epistemic_tag + "]\n";
            }
            for (const auto& [sec_type, el_cand] : m.element_candidates) {
                out += "# Element voice change (" + std::string(matcher_detail::sectionShortName(sec_type)) + "): " +
                       std::to_string(el_cand.ref.msb) + " " + std::to_string(el_cand.ref.lsb) + " " +
                       std::to_string(el_cand.ref.pc) + "  \"" + el_cand.display_name + "\"" +
                       "  score=" + std::to_string(el_cand.score) +
                       "  [" + el_cand.epistemic_tag + "]\n";
            }
        }

        if (!any_row) {
            out += "# no Genos part plays this track\n";
        }

        // Emit parseSoundMap()-compatible assignment line.
        if (primary_match && primary_match->selected.has_value() &&
            (primary_match->selected_origin == MatchOrigin::UserSoundMap || auto_emit)) {
            const auto& ref = *primary_match->selected;
            const uint8_t top_score = primary_match->top_candidates.empty()
                                          ? uint8_t{100}
                                          : primary_match->top_candidates.front().score;
            const char* mode_tag = (primary_match->selected_origin == MatchOrigin::UserSoundMap)
                                       ? "user"
                                       : "auto";
            out += std::string(t.key) + " " +
                   std::to_string(ref.msb) + " " + std::to_string(ref.lsb) + " " +
                   std::to_string(ref.pc) + "  # " + mode_tag +
                   " score=" + std::to_string(top_score) + " " +
                   primary_match->selected_epistemic_tag + "\n";
        } else {
            out += std::string(t.key) + " unresolved\n";
        }
    }
    return out;
}

} // namespace ai_arranger::exporters::korg::quality

#endif // AI_ARRANGER_EXPORTERS_KORG_PA5X_SOUND_MATCHER_H
