#ifndef AI_ARRANGER_MEDIA_SOUND_DRUMKIT_PROFILE_H
#define AI_ARRANGER_MEDIA_SOUND_DRUMKIT_PROFILE_H

// Track A Next — Slice A2: Pure Portable C++20 Header-Only Read-Only Drum Kit Profiler.
//
// Consumes a read-only `DrumKitKeyMap` (`media/sound/native_refs.h`) and produces an advisory
// `DrumKitProfile` via a deterministic 5-tier drum-family classifier, GM-alignment scorer,
// choke-group membership index, and velocity-layer dynamic profile (Track A Next Spec §2.2–§2.3).
//
// Boundaries & Safety Invariants:
//  * Strictly READ-ONLY and strictly ADVISORY (`INFERRED`): ZERO impact on Resource Graph edges,
//    closure, or Sound equivalence verdicts.
//  * Pure portable C++20 header-only in `namespace ai_arranger::media::graph`: ZERO `src/exporters/`
//    includes, ZERO JUCE/Apple dependencies, ZERO disk I/O.
//  * `ROM(idx == 0)` unassigned placeholders (`DrumKeyOccupancy::RomIndexZeroPlaceholder`) and
//    `EmptyOrUnknown` keys are NEVER included in active/family bitmasks, choke-group lists,
//    GM-alignment scoring, or velocity-ceiling statistics.
//  * Non-GM / regional / custom kits (`gmAlignmentScore < 0.60`) NEVER have GM roles fabricated
//    onto unlabelled keys.

#include "media/sound/native_refs.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ai_arranger::media::graph {

// Identifies which tier of the deterministic 5-tier advisory cascade classified a populated key.
enum class DrumFamilyTier : uint8_t {
    Unresolved = 0,
    Tier1CoreFamilyByte = 1,       // INFERRED: RAM.KMP +113 in {0..4} -> B/S/H/T/C (High confidence)
    Tier2ChokeGroupHint = 2,       // INFERRED: standard GM2/Korg exclusive group 1..7 on canonical keys
    Tier3SampleNameKeyword = 3,    // INFERRED: case-insensitive RAM DrumSample::name keyword classifier
    Tier4PercussionFamilyByte = 4, // INFERRED: RAM.KMP +113 in {5, 6} -> L/E
    Tier5GmPositionFallback = 5    // INFERRED: GM-alignment-gated GM2 note position (35..87)
};

inline const char* drumFamilyTierName(DrumFamilyTier t) {
    switch (t) {
        case DrumFamilyTier::Unresolved:                return "Unresolved";
        case DrumFamilyTier::Tier1CoreFamilyByte:       return "Tier1CoreFamilyByte";
        case DrumFamilyTier::Tier2ChokeGroupHint:       return "Tier2ChokeGroupHint";
        case DrumFamilyTier::Tier3SampleNameKeyword:    return "Tier3SampleNameKeyword";
        case DrumFamilyTier::Tier4PercussionFamilyByte: return "Tier4PercussionFamilyByte";
        case DrumFamilyTier::Tier5GmPositionFallback:   return "Tier5GmPositionFallback";
    }
    return "Unresolved";
}

enum class DrumFamilyConfidence : uint8_t {
    None = 0,
    Low = 1,
    Medium = 2,
    High = 3
};

inline const char* drumFamilyConfidenceName(DrumFamilyConfidence c) {
    switch (c) {
        case DrumFamilyConfidence::None:   return "None";
        case DrumFamilyConfidence::Low:    return "Low";
        case DrumFamilyConfidence::Medium: return "Medium";
        case DrumFamilyConfidence::High:   return "High";
    }
    return "None";
}

// Read-only advisory profile for one MIDI key (0..127) of a parsed USERDK kit.
struct DrumKitKeyProfile {
    uint8_t midiNote = 0;                                       // VERIFIED: 0..127
    DrumKeyOccupancy occupancy = DrumKeyOccupancy::EmptyOrUnknown;
    bool populated = false;                                     // true iff RamPopulated || RomOrBank2Populated
    bool isRam = false;                                         // true iff RamPopulated (VERIFIED)
    bool isRomZeroPlaceholder = false;                          // true iff RomIndexZeroPlaceholder (INFERRED)
    char advisoryFamily = '\0';                                 // INFERRED: 'B','S','H','T','C','L','E','X', or '\0'
    DrumFamilyTier familyTier = DrumFamilyTier::Unresolved;
    DrumFamilyConfidence familyConfidence = DrumFamilyConfidence::None;
    uint8_t layerCount = 0;                                     // raw 1..8 (or 0 if unparsed/empty)
    uint8_t effectiveDistinctLayers = 0;                        // distinct non-degenerate velocity zones
    uint8_t effectiveMaxVel = 0;                                // velTop[layerCount - 1] (0 if layerCount == 0)
    bool hasDegenerateVelSplit = false;                         // true iff any adjacent velTop[l] == velTop[l-1]
    bool hasSubMaxVelCeiling = false;                           // true iff populated && effectiveMaxVel < 127
    uint8_t chokeGroup = 0;                                     // INFERRED: 0 = none, 1..127
    uint8_t pan = 64;                                           // INFERRED: 0..127, 64 = center
    std::array<uint8_t, 8> velTop{};                            // VERIFIED: monotonic velocity top thresholds
    std::string primarySampleName;                              // INFERRED/advisory: top resolved RAM sample name
};

// Read-only advisory profile for one parsed USERDK Drum Kit.
// All family classifications and GM-alignment scores are strictly INFERRED advisory metadata.
struct DrumKitProfile {
    ResourceId sound;
    uint32_t kindChunkId = 0;
    bool isRomOnly = false;
    bool hasBank2Layers = false;

    std::array<DrumKitKeyProfile, 128> keys{};

    // Populated-key bitmasks & occupancy counters
    std::bitset<128> activeKeyMask{};
    std::bitset<128> ramKeyMask{};
    uint32_t populatedKeyCount = 0;
    uint32_t ramPopulatedCount = 0;
    uint32_t romOrBank2PopulatedCount = 0;
    uint32_t romPlaceholderCount = 0;
    uint32_t emptyOrUnknownCount = 0;

    // GM-alignment assessment over core anchor keys {35, 36, 38, 40, 42, 44, 46, 49, 51}
    double gmAlignmentScore = 0.0;                              // in [0.0, 1.0]
    uint32_t gmAnchorPopulatedCount = 0;
    uint32_t gmAnchorMatchedCount = 0;
    bool isGmAligned = false;                                   // true iff gmAlignmentScore >= 0.60
    bool hasStandardGmChokeGroups = false;                      // true iff standard GM choke group 1..6 hit on canonical key

    // Per-family populated key bitmasks (strictly populated keys; ROM(idx==0) placeholders excluded)
    std::bitset<128> familyB{};                                 // 'B' Bass Drum / Kick
    std::bitset<128> familyS{};                                 // 'S' Snare / Rim / Clap / Brush
    std::bitset<128> familyH{};                                 // 'H' Hi-Hat
    std::bitset<128> familyT{};                                 // 'T' Tom
    std::bitset<128> familyC{};                                 // 'C' Cymbal
    std::bitset<128> familyL{};                                 // 'L' Latin Percussion
    std::bitset<128> familyE{};                                 // 'E' Ethnic / Regional Percussion
    std::bitset<128> familyX{};                                 // 'X' Special FX / Scratch / Loop

    // Choke-group membership lists: chokeGroup (> 0) -> sorted populated MIDI notes
    std::map<uint8_t, std::vector<uint8_t>> chokeGroups;

    // Velocity-layer & dynamic-ceiling summary across populated keys
    uint32_t multiLayerKeyCount = 0;                            // populated keys with effectiveDistinctLayers >= 2
    uint32_t maxEffectiveLayers = 0;                            // max effectiveDistinctLayers across populated keys
    uint32_t degenerateVelKeyCount = 0;                         // populated keys with hasDegenerateVelSplit == true
    uint32_t subMaxVelCeilingKeyCount = 0;                      // populated keys with effectiveMaxVel < 127
    uint8_t minEffectiveMaxVel = 127;                           // min effectiveMaxVel across populated keys (127 if none)

    bool hasKey(uint8_t midiNote) const noexcept {
        return midiNote < 128u && activeKeyMask.test(midiNote);
    }

    char familyOf(uint8_t midiNote) const noexcept {
        return midiNote < 128u ? keys[midiNote].advisoryFamily : '\0';
    }

    const std::bitset<128>& familyMask(char family) const noexcept {
        static const std::bitset<128> kEmpty{};
        switch (family) {
            case 'B': return familyB;
            case 'S': return familyS;
            case 'H': return familyH;
            case 'T': return familyT;
            case 'C': return familyC;
            case 'L': return familyL;
            case 'E': return familyE;
            case 'X': return familyX;
            default:  return kEmpty;
        }
    }
};

namespace drumkit_profile_detail {

inline constexpr double kGmAlignmentThreshold = 0.60;

inline constexpr std::array<uint8_t, 9> kGmCoreAnchorNotes = {
    35u, 36u, 38u, 40u, 42u, 44u, 46u, 49u, 51u
};

// Tier 1: High-precision core RAM.KMP +113 family byte {0..4} -> {'B','S','H','T','C'}.
inline constexpr char classifyCoreFamilyByte(uint8_t code) noexcept {
    switch (code) {
        case 0u: return 'B';
        case 1u: return 'S';
        case 2u: return 'H';
        case 3u: return 'T';
        case 4u: return 'C';
        default: return '\0';
    }
}

// Tier 4: Percussion-tier RAM.KMP +113 family byte {5, 6} -> {'L', 'E'}.
inline constexpr char classifyPercussionFamilyByte(uint8_t code) noexcept {
    switch (code) {
        case 5u: return 'L';
        case 6u: return 'E';
        default: return '\0';
    }
}

// Tier 2: Standard GM2/Korg exclusive (choke) group structural hints on canonical keys (INFERRED).
inline constexpr char classifyChokeGroupHint(uint8_t midiNote, uint8_t chokeGroup) noexcept {
    switch (chokeGroup) {
        case 1u:
            if (midiNote == 42u || midiNote == 44u || midiNote == 46u) return 'H';
            return '\0';
        case 2u:
            if (midiNote == 71u || midiNote == 72u) return 'L';
            return '\0';
        case 3u:
            if (midiNote == 73u || midiNote == 74u) return 'L';
            return '\0';
        case 4u:
            if (midiNote == 78u || midiNote == 79u) return 'L';
            return '\0';
        case 5u:
            if (midiNote == 80u || midiNote == 81u) return 'L';
            return '\0';
        case 6u:
            if (midiNote == 86u || midiNote == 87u) return 'L';
            return '\0';
        case 7u:
            if (midiNote == 25u || midiNote == 29u || midiNote == 30u) return 'X';
            return '\0';
        default:
            return '\0';
    }
}

inline constexpr bool isStandardGmChokeGroup1To6(uint8_t midiNote, uint8_t chokeGroup) noexcept {
    return chokeGroup >= 1u && chokeGroup <= 6u && classifyChokeGroupHint(midiNote, chokeGroup) != '\0';
}

inline bool isAsciiAlpha(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline std::string toUpperAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

inline bool containsSubstr(std::string_view haystack, std::string_view needle) noexcept {
    return haystack.find(needle) != std::string_view::npos;
}

// Matches short 2-letter abbreviations (BD, SD, HH, FX) bounded by non-letter characters
// (start/end, digits, underscores, hyphens, spaces) to avoid false matches like "KBD" -> BD.
inline bool containsAlphaBounded(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        const bool leftOk = (pos == 0u) || !isAsciiAlpha(haystack[pos - 1u]);
        const size_t after = pos + needle.size();
        const bool rightOk = (after >= haystack.size()) || !isAsciiAlpha(haystack[after]);
        if (leftOk && rightOk) return true;
        ++pos;
    }
    return false;
}

// Tier 3: Case-insensitive RAM DrumSample::name keyword classifier (INFERRED).
inline char classifySampleNameKeyword(std::string_view rawName) {
    if (rawName.empty()) return '\0';
    const std::string up = toUpperAscii(rawName);
    const std::string_view sv(up);

    // 'B' — Bass Drum / Kick
    if (containsSubstr(sv, "KICK") || containsAlphaBounded(sv, "BD")) return 'B';
    // 'S' — Snare / Rim / Clap / Brush
    if (containsSubstr(sv, "SNARE") || containsAlphaBounded(sv, "SD") ||
        containsSubstr(sv, "RIM") || containsSubstr(sv, "CLAP") || containsSubstr(sv, "BRUSH")) {
        return 'S';
    }
    // 'H' — Hi-Hat
    if (containsSubstr(sv, "HAT") || containsAlphaBounded(sv, "HH") ||
        containsSubstr(sv, "CHH") || containsSubstr(sv, "OHH") || containsSubstr(sv, "PHH")) {
        return 'H';
    }
    // 'T' — Tom
    if (containsSubstr(sv, "TOM")) return 'T';
    // 'C' — Cymbal
    if (containsSubstr(sv, "CRASH") || containsSubstr(sv, "RIDE") ||
        containsSubstr(sv, "CYM") || containsSubstr(sv, "SPLASH") || containsSubstr(sv, "CHINA")) {
        return 'C';
    }
    // 'L' — Latin Percussion
    if (containsSubstr(sv, "CONGA") || containsSubstr(sv, "BONGO") ||
        containsSubstr(sv, "TIMB") || containsSubstr(sv, "SHAK") ||
        containsSubstr(sv, "TAMB") || containsSubstr(sv, "COW") ||
        containsSubstr(sv, "GUIRO") || containsSubstr(sv, "CLAVE") ||
        containsSubstr(sv, "SURDO") || containsSubstr(sv, "CUICA") ||
        containsSubstr(sv, "TRIANG")) {
        return 'L';
    }
    // 'X' — Special FX / Scratch / Loop
    if (containsSubstr(sv, "SFX") || containsAlphaBounded(sv, "FX") ||
        containsSubstr(sv, "LOOP") || containsSubstr(sv, "SCRAT")) {
        return 'X';
    }
    return '\0';
}

// Expected GM core anchor family for gmAlignmentScore computation.
inline constexpr char gmCoreAnchorFamily(uint8_t midiNote) noexcept {
    switch (midiNote) {
        case 35u: case 36u: return 'B';
        case 38u: case 40u: return 'S';
        case 42u: case 44u: case 46u: return 'H';
        case 49u: case 51u: return 'C';
        default: return '\0';
    }
}

// Tier 5: Standard GM2 positional percussion map (MIDI notes 35..87 only).
inline constexpr char gm2PositionalFamily(uint8_t midiNote) noexcept {
    if (midiNote < 35u || midiNote > 87u) return '\0';
    switch (midiNote) {
        case 35u: case 36u:
            return 'B';
        case 37u: case 38u: case 39u: case 40u:
            return 'S';
        case 42u: case 44u: case 46u:
            return 'H';
        case 41u: case 43u: case 45u: case 47u: case 48u: case 50u:
            return 'T';
        case 49u: case 51u: case 52u: case 53u: case 55u: case 57u: case 59u:
            return 'C';
        default:
            // Remaining GM2 notes in 54, 56, 58, 60..87 are Latin / standard percussion.
            return 'L';
    }
}

inline void addToFamilyMask(DrumKitProfile& profile, uint8_t midiNote, char family) noexcept {
    if (midiNote >= 128u) return;
    switch (family) {
        case 'B': profile.familyB.set(midiNote); break;
        case 'S': profile.familyS.set(midiNote); break;
        case 'H': profile.familyH.set(midiNote); break;
        case 'T': profile.familyT.set(midiNote); break;
        case 'C': profile.familyC.set(midiNote); break;
        case 'L': profile.familyL.set(midiNote); break;
        case 'E': profile.familyE.set(midiNote); break;
        case 'X': profile.familyX.set(midiNote); break;
        default: break;
    }
}

}  // namespace drumkit_profile_detail

// Builds a deterministic, read-only, strictly advisory `DrumKitProfile` from a parsed `DrumKitKeyMap`.
// Never mutates any graph state or affects Sound equivalence verdicts.
inline DrumKitProfile profileDrumKit(const DrumKitKeyMap& keyMap) {
    DrumKitProfile profile;
    profile.sound = keyMap.sound;
    profile.kindChunkId = keyMap.kindChunkId;
    profile.hasBank2Layers = keyMap.hasBank2Layers();

    // Pass 1: Per-key occupancy, velocity dynamics, choke groups, and Tiers 1..4 (non-positional evidence).
    for (uint32_t k = 0; k < 128u; ++k) {
        const DrumKitKeySummary& ks = keyMap.keys[k];
        DrumKitKeyProfile& kp = profile.keys[k];
        kp.midiNote = static_cast<uint8_t>(k);
        kp.occupancy = ks.occupancy;
        kp.populated = ks.isPopulated();
        kp.isRam = (ks.occupancy == DrumKeyOccupancy::RamPopulated);
        kp.isRomZeroPlaceholder = (ks.occupancy == DrumKeyOccupancy::RomIndexZeroPlaceholder);
        kp.layerCount = ks.layerCount;
        kp.effectiveDistinctLayers = ks.effectiveDistinctLayers();
        kp.effectiveMaxVel = ks.effectiveMaxVel();
        kp.hasDegenerateVelSplit = ks.hasDegenerateVelSplit();
        kp.hasSubMaxVelCeiling = kp.populated && kp.layerCount > 0u && kp.effectiveMaxVel < 127u;
        kp.chokeGroup = ks.advisoryExclusiveGroup;
        kp.pan = ks.advisoryPan;
        kp.velTop = ks.velTop;

        switch (ks.occupancy) {
            case DrumKeyOccupancy::RamPopulated:
                ++profile.ramPopulatedCount;
                ++profile.populatedKeyCount;
                profile.activeKeyMask.set(k);
                profile.ramKeyMask.set(k);
                break;
            case DrumKeyOccupancy::RomOrBank2Populated:
                ++profile.romOrBank2PopulatedCount;
                ++profile.populatedKeyCount;
                profile.activeKeyMask.set(k);
                break;
            case DrumKeyOccupancy::RomIndexZeroPlaceholder:
                ++profile.romPlaceholderCount;
                break;
            case DrumKeyOccupancy::EmptyOrUnknown:
                ++profile.emptyOrUnknownCount;
                break;
        }

        // Unpopulated keys (including ROM(idx==0) placeholders) are strictly excluded from profiling.
        if (!kp.populated) continue;

        // Velocity & choke-group aggregates across populated keys
        if (kp.effectiveDistinctLayers >= 2u) ++profile.multiLayerKeyCount;
        if (kp.effectiveDistinctLayers > profile.maxEffectiveLayers) {
            profile.maxEffectiveLayers = kp.effectiveDistinctLayers;
        }
        if (kp.hasDegenerateVelSplit) ++profile.degenerateVelKeyCount;
        if (kp.hasSubMaxVelCeiling) ++profile.subMaxVelCeilingKeyCount;
        if (kp.layerCount > 0u && kp.effectiveMaxVel < profile.minEffectiveMaxVel) {
            profile.minEffectiveMaxVel = kp.effectiveMaxVel;
        }
        if (kp.chokeGroup > 0u) {
            profile.chokeGroups[kp.chokeGroup].push_back(static_cast<uint8_t>(k));
            if (drumkit_profile_detail::isStandardGmChokeGroup1To6(static_cast<uint8_t>(k), kp.chokeGroup)) {
                profile.hasStandardGmChokeGroups = true;
            }
        }

        // Select top/primary resolved RAM layer sample name (highest velocity zone first)
        for (size_t idx = ks.layers.size(); idx > 0u; --idx) {
            const DrumKitLayerSummary& ls = ks.layers[idx - 1u];
            if (ls.isRam && ls.resolved && !ls.advisoryDrumSampleName.empty()) {
                kp.primarySampleName = ls.advisoryDrumSampleName;
                break;
            }
        }

        // Tier 1: High-precision core RAM.KMP +113 in {0..4} -> {'B','S','H','T','C'} (top resolved RAM layer first)
        char resolvedFamily = '\0';
        for (size_t idx = ks.layers.size(); idx > 0u; --idx) {
            const DrumKitLayerSummary& ls = ks.layers[idx - 1u];
            if (!ls.isRam || !ls.resolved) continue;
            const char f1 = drumkit_profile_detail::classifyCoreFamilyByte(ls.advisoryDrumFamilyByte);
            if (f1 != '\0') {
                resolvedFamily = f1;
                kp.familyTier = DrumFamilyTier::Tier1CoreFamilyByte;
                kp.familyConfidence = DrumFamilyConfidence::High;
                break;
            }
        }

        // Tier 2: Standard GM2/Korg choke-group structural hints (groups 1..7 on canonical keys)
        if (resolvedFamily == '\0') {
            const char f2 = drumkit_profile_detail::classifyChokeGroupHint(static_cast<uint8_t>(k), kp.chokeGroup);
            if (f2 != '\0') {
                resolvedFamily = f2;
                kp.familyTier = DrumFamilyTier::Tier2ChokeGroupHint;
                kp.familyConfidence = DrumFamilyConfidence::Medium;
            }
        }

        // Tier 3: Case-insensitive RAM DrumSample::name keyword classifier (top resolved RAM layer first)
        if (resolvedFamily == '\0') {
            for (size_t idx = ks.layers.size(); idx > 0u; --idx) {
                const DrumKitLayerSummary& ls = ks.layers[idx - 1u];
                if (!ls.isRam || !ls.resolved) continue;
                const char f3 = drumkit_profile_detail::classifySampleNameKeyword(ls.advisoryDrumSampleName);
                if (f3 != '\0') {
                    resolvedFamily = f3;
                    kp.familyTier = DrumFamilyTier::Tier3SampleNameKeyword;
                    kp.familyConfidence = DrumFamilyConfidence::Medium;
                    break;
                }
            }
        }

        // Tier 4: Percussion-tier RAM.KMP +113 fallback ({5 -> 'L', 6 -> 'E'})
        if (resolvedFamily == '\0') {
            for (size_t idx = ks.layers.size(); idx > 0u; --idx) {
                const DrumKitLayerSummary& ls = ks.layers[idx - 1u];
                if (!ls.isRam || !ls.resolved) continue;
                const char f4 = drumkit_profile_detail::classifyPercussionFamilyByte(ls.advisoryDrumFamilyByte);
                if (f4 != '\0') {
                    resolvedFamily = f4;
                    kp.familyTier = DrumFamilyTier::Tier4PercussionFamilyByte;
                    kp.familyConfidence = DrumFamilyConfidence::Low;
                    break;
                }
            }
        }

        kp.advisoryFamily = resolvedFamily;
    }

    profile.isRomOnly = keyMap.isRomOnly() ||
                        (profile.ramPopulatedCount == 0u && !profile.hasBank2Layers &&
                         keyMap.danglingRamLayers == 0u &&
                         (profile.romOrBank2PopulatedCount > 0u || profile.romPlaceholderCount > 0u));

    // Compute per-kit GM-alignment score over populated core anchor keys {35, 36, 38, 40, 42, 44, 46, 49, 51}.
    for (uint8_t anchor : drumkit_profile_detail::kGmCoreAnchorNotes) {
        const DrumKitKeyProfile& kp = profile.keys[anchor];
        if (!kp.populated) continue;
        ++profile.gmAnchorPopulatedCount;
        if (kp.advisoryFamily == drumkit_profile_detail::gmCoreAnchorFamily(anchor)) {
            ++profile.gmAnchorMatchedCount;
        }
    }
    if (profile.gmAnchorPopulatedCount > 0u) {
        profile.gmAlignmentScore =
            static_cast<double>(profile.gmAnchorMatchedCount) /
            static_cast<double>(profile.gmAnchorPopulatedCount);
    } else {
        profile.gmAlignmentScore = 0.0;
    }
    profile.isGmAligned = (profile.gmAlignmentScore >= drumkit_profile_detail::kGmAlignmentThreshold);

    // Pass 2: Tier 5 (GM-alignment-gated positional fallback) + per-family populated key bitmasks.
    const bool allowTier5 = profile.isGmAligned || (profile.isRomOnly && profile.hasStandardGmChokeGroups);
    for (uint32_t k = 0; k < 128u; ++k) {
        DrumKitKeyProfile& kp = profile.keys[k];
        if (!kp.populated) continue;
        if (kp.advisoryFamily == '\0' && allowTier5) {
            const char f5 = drumkit_profile_detail::gm2PositionalFamily(static_cast<uint8_t>(k));
            if (f5 != '\0') {
                kp.advisoryFamily = f5;
                kp.familyTier = DrumFamilyTier::Tier5GmPositionFallback;
                kp.familyConfidence = DrumFamilyConfidence::Low;
            }
        }
        if (kp.advisoryFamily != '\0') {
            drumkit_profile_detail::addToFamilyMask(profile, static_cast<uint8_t>(k), kp.advisoryFamily);
        }
    }

    return profile;
}

}  // namespace ai_arranger::media::graph

#endif  // AI_ARRANGER_MEDIA_SOUND_DRUMKIT_PROFILE_H
