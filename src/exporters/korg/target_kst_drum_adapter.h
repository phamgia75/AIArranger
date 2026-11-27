#pragma once
#ifndef AI_ARRANGER_EXPORTERS_KORG_TARGET_KST_DRUM_ADAPTER_H
#define AI_ARRANGER_EXPORTERS_KORG_TARGET_KST_DRUM_ADAPTER_H

// Track B Wave-3 Q4-Wire: Thin, Strictly Read-Only Track A -> Track B Bridge.
//
// Bridges Track A read-only native target inspection models:
//   * `ai_arranger::media::selective::TargetOccupancyMap` (`media/sound/slot_conflicts.h`)
//   * `ai_arranger::media::graph::DrumKitKeyMap` (`media/sound/native_refs.h`)
//   * `ai_arranger::media::graph::DrumKitProfile` (`media/sound/drumkit_profile.h`)
// into Track B Q3/Q4 read-only consumer contracts:
//   * `ai_arranger::exporters::korg::quality::TargetSoundCatalogView` / `TargetSoundEntry`
//   * `ai_arranger::exporters::korg::quality::UserdkDrumKitKeyMap` (implements `IDrumKitKeyMap`)
//
// Boundaries, Zero-Circularity & HOLD Gates:
//   * Direction is STRICTLY one-way: `src/exporters/` & `tools/` -> `src/media/sound/` (read-only).
//     `src/media/` MUST NEVER include `src/exporters/`.
//   * NATIVE_SOUND_WRITE_HOLD: 100% read-only inspection only; zero writes or mutations to any
//     `.KST`, `.PCG`, `.KMP`, `.PCM`, or `USERDK` artifact.
//   * Bank-2 (0x02) Fail-Closed Guard: USERDK kits containing unverified Bank-2 (`sourceBank == 2`)
//     layers, dangling RAM layers, or unresolved non-RAM layers are NEVER exposed as comparable
//     `UserdkDrumKitKeyMap` targets and NEVER given fabricated GM-alignment or family bitmasks.
//   * Out-of-spec `USERDK07..12` banks (`bank > 6`) are marked `in_spec = false` for auditing and
//     never converted to `UserdkDrumKitKeyMap` targets.
//   * ROM(idx == 0) unassigned placeholders are strictly excluded from populated keys.
//   * Pure portable C++20 header-only, ODR-safe (`namespace target_adapter_detail`).

#include "exporters/korg/drum_remap.h"
#include "exporters/korg/pa5x_sound_matcher.h"
#include "media/sound/drumkit_profile.h"
#include "media/sound/slot_conflicts.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ai_arranger::exporters::korg::quality {

inline constexpr bool kTargetKstAdapterReadOnly = true;
inline constexpr const char* kNativeSoundWriteHoldNotice =
    "NATIVE_SOUND_WRITE_HOLD: read-only target KST inspection only; zero PCG/KMP/PCM/USERDK mutations.";

namespace target_adapter_detail {

inline uint8_t topResolvedRamFamilyByte(const media::graph::DrumKitKeySummary& ks) noexcept {
    for (size_t idx = ks.layers.size(); idx > 0u; --idx) {
        const auto& ls = ks.layers[idx - 1u];
        if (ls.isRam && ls.resolved && ls.advisoryDrumFamilyByte < 8u) {
            return ls.advisoryDrumFamilyByte;
        }
    }
    return 0xFFu;
}

inline std::string defaultKitName(const media::graph::ResourceId& sound) {
    if (!sound.scope.empty()) {
        return sound.scope + "/" + std::to_string(sound.number);
    }
    return "USERDK/" + std::to_string(sound.number);
}

inline uint8_t clampGmScorePercent(double score01) noexcept {
    if (!std::isfinite(score01) || score01 <= 0.0) return 0u;
    if (score01 >= 1.0) return 100u;
    return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(score01 * 100.0)), 0, 100));
}

}  // namespace target_adapter_detail

// Fail-closed comparability gate for a parsed Track A USERDK DrumKitKeyMap.
// Rejects Bank-2 (0x02) unverified layers, dangling RAM layers, unresolved non-RAM layers,
// and kits with zero populated keys.
inline bool isDrumKitComparableForTargetRemap(
    const media::graph::DrumKitKeyMap& key_map,
    const media::selective::SoundEquivalenceProfile* equivalence = nullptr) noexcept {
    if (key_map.hasBank2Layers()) return false;
    if (key_map.danglingRamLayers > 0u || key_map.unresolvedNonRamLayerCount > 0u) return false;
    if (equivalence != nullptr && equivalence->hasNonRamBank2Layers) return false;
    if (key_map.activeKeyMask.none()) return false;
    return true;
}

// Fail-closed comparability gate for an already-computed Track A DrumKitProfile.
inline bool isDrumKitProfileComparable(const media::graph::DrumKitProfile& profile) noexcept {
    if (profile.hasBank2Layers) return false;
    if (profile.populatedKeyCount == 0u || profile.activeKeyMask.none()) return false;
    return true;
}

// Pure read-only conversion: Track A `DrumKitProfile` (+ optional `DrumKitKeyMap`) -> Q3 `DrumKitKeySummaryView`.
// Strictly excludes `RomIndexZeroPlaceholder` keys and non-populated keys.
inline DrumKitKeySummaryView buildUserdkSummaryView(
    const media::graph::DrumKitProfile& profile,
    const std::string& kit_name = {},
    const media::graph::DrumKitKeyMap* raw_key_map = nullptr) {
    DrumKitKeySummaryView view{};
    view.kit_name = kit_name.empty() ? target_adapter_detail::defaultKitName(profile.sound) : kit_name;
    view.gm_alignment_score = std::clamp(profile.gmAlignmentScore, 0.0, 1.0);

    for (uint32_t k = 0; k < 128u; ++k) {
        const media::graph::DrumKitKeyProfile& kp = profile.keys[k];
        UserdkKeySlotSummary& slot = view.keys[k];
        slot.is_rom_zero_placeholder = kp.isRomZeroPlaceholder;
        slot.populated = kp.populated && !kp.isRomZeroPlaceholder;
        slot.has_ram_layer = slot.populated && kp.isRam;
        slot.layer_count = slot.populated ? kp.layerCount : 0u;
        for (size_t i = 0; i < 8u; ++i) {
            slot.vel_tops[i] = slot.populated ? kp.velTop[i] : 0u;
        }
        slot.choke_group = slot.populated ? kp.chokeGroup : 0u;
        slot.pan = kp.pan;
        slot.resolved_family = slot.populated ? kp.advisoryFamily : '\0';
        slot.primary_sample_name = slot.populated ? kp.primarySampleName : std::string{};
        slot.kmp_family_code = (slot.populated && raw_key_map != nullptr)
                                   ? target_adapter_detail::topResolvedRamFamilyByte(raw_key_map->keys[k])
                                   : uint8_t{0xFFu};
    }
    return view;
}

// Fail-closed builder converting a Track A `DrumKitKeyMap` into Q3's `UserdkDrumKitKeyMap`.
// Returns `std::nullopt` whenever Bank-2 (0x02) layers, dangling/unresolved layers, or 0 populated keys exist.
inline std::optional<UserdkDrumKitKeyMap> buildComparableUserdkKeyMap(
    const media::graph::DrumKitKeyMap& key_map,
    const std::string& kit_name = {},
    const media::selective::SoundEquivalenceProfile* equivalence = nullptr) {
    if (!isDrumKitComparableForTargetRemap(key_map, equivalence)) {
        return std::nullopt;
    }
    const media::graph::DrumKitProfile profile = media::graph::profileDrumKit(key_map);
    if (!isDrumKitProfileComparable(profile)) {
        return std::nullopt;
    }
    return UserdkDrumKitKeyMap(buildUserdkSummaryView(profile, kit_name, &key_map));
}

// Fail-closed builder converting an already-profiled Track A `DrumKitProfile` into Q3's `UserdkDrumKitKeyMap`.
inline std::optional<UserdkDrumKitKeyMap> buildComparableUserdkKeyMap(
    const media::graph::DrumKitProfile& profile,
    const std::string& kit_name = {}) {
    if (!isDrumKitProfileComparable(profile)) {
        return std::nullopt;
    }
    return UserdkDrumKitKeyMap(buildUserdkSummaryView(profile, kit_name, nullptr));
}

// Looks up a specific Pa5X MIDI SoundRef (MSB 120, LSB 64..69, PC 0..127) inside a read-only
// `TargetOccupancyMap` and returns a comparable `UserdkDrumKitKeyMap` (or `std::nullopt` fail-closed).
inline std::optional<UserdkDrumKitKeyMap> findComparableTargetDrumKeyMap(
    const media::selective::TargetOccupancyMap& target_map,
    const SoundRef& ref) {
    const auto addr = media::selective::SlotAddress::fromMidi(ref.msb, ref.lsb, ref.pc);
    if (!addr.has_value() || !addr->isDrumKit || !addr->isInSpec()) {
        return std::nullopt;
    }
    const media::graph::DrumKitKeyMap* km = target_map.findDrumKitKeyMap(*addr);
    if (km == nullptr) {
        return std::nullopt;
    }
    const media::selective::OccupiedSlotEntry* occ = target_map.findOccupied(*addr);
    const std::string label = (occ != nullptr && !occ->displayName.empty())
                                  ? occ->displayName
                                  : addr->shortLabel();
    return buildComparableUserdkKeyMap(*km, label, occ ? &occ->equivalence : nullptr);
}

// Pure read-only conversion: Track A `TargetOccupancyMap` -> Q4 `TargetSoundCatalogView`.
// Enriches comparable, in-spec, non-Bank-2 USERDK entries with read-only `DrumKitProfile`
// GM-alignment scores (0..100) and unambiguous structural family hints while preserving
// out-of-spec (`USERDK07..12`) entries with `in_spec = false` for matcher fail-closed auditing.
inline TargetSoundCatalogView buildTargetSoundCatalogView(
    const media::selective::TargetOccupancyMap& target_map) {
    TargetSoundCatalogView view{};
    view.is_partial_inspect = target_map.partialInspect.isPartial;
    view.partial_notice = target_map.partialInspect.notice;
    view.entries.reserve(target_map.occupiedSlots.size());

    for (const auto& [addr, occ] : target_map.occupiedSlots) {
        TargetSoundEntry e{};
        e.msb = addr.msb();
        e.lsb = addr.lsb();
        e.pc = addr.pc();
        e.is_drum_kit = addr.isDrumKit;
        e.in_spec = addr.isInSpec();
        e.display_name = occ.displayName;
        e.slot_address = TargetSlotAddress{addr.isDrumKit, addr.bank, addr.slot};

        if (addr.isDrumKit && addr.isInSpec() && !occ.equivalence.hasNonRamBank2Layers) {
            if (const media::graph::DrumKitKeyMap* km = target_map.findDrumKitKeyMap(addr);
                km != nullptr && isDrumKitComparableForTargetRemap(*km, &occ.equivalence)) {
                const media::graph::DrumKitProfile prof = media::graph::profileDrumKit(*km);
                if (isDrumKitProfileComparable(prof)) {
                    e.gm_alignment_score = target_adapter_detail::clampGmScorePercent(prof.gmAlignmentScore);
                    if (prof.isGmAligned) {
                        e.family_bitmask = timbreFamilyBit(TimbreFamily::DrumKit);
                    } else if ((prof.familyL.count() + prof.familyE.count()) > 0u &&
                               (prof.familyB.count() + prof.familyS.count() + prof.familyH.count()) == 0u) {
                        e.family_bitmask = timbreFamilyBit(TimbreFamily::PercussionKit);
                    }
                }
            }
        }

        view.entries.push_back(std::move(e));
    }
    return view;
}

// Convenience wrapper returning just the read-only `TargetSoundEntry` vector for `Pa5XSoundMatcher`.
inline std::vector<TargetSoundEntry> buildTargetSoundEntries(
    const media::selective::TargetOccupancyMap& target_map) {
    return buildTargetSoundCatalogView(target_map).entries;
}

// Combined read-only Target KST bridge holding both Q4 `TargetSoundCatalogView` and
// Q3 comparable `UserdkDrumKitKeyMap` instances indexed by `(msb, lsb, pc)`.
struct TargetKstReadOnlyBridge {
    bool valid = false;
    bool fully_inspectable = false;
    std::string error;
    TargetSoundCatalogView catalog;
    std::map<std::tuple<uint8_t, uint8_t, uint8_t>, UserdkDrumKitKeyMap> drum_key_maps;
    uint32_t drum_kits_skipped_bank2 = 0;
    uint32_t drum_kits_skipped_uncomparable = 0;
    uint32_t drum_kits_skipped_out_of_spec = 0;

    const IDrumKitKeyMap* findDrumKeyMap(const SoundRef& ref) const noexcept {
        const auto it = drum_key_maps.find(std::make_tuple(ref.msb, ref.lsb, ref.pc));
        return it == drum_key_maps.end() ? nullptr : static_cast<const IDrumKitKeyMap*>(&it->second);
    }
};

// Pure read-only conversion from an already-inspected Track A `TargetOccupancyMap`.
inline TargetKstReadOnlyBridge buildTargetKstBridge(
    const media::selective::TargetOccupancyMap& target_map) {
    TargetKstReadOnlyBridge bridge{};
    bridge.valid = target_map.valid;
    bridge.fully_inspectable = target_map.fullyInspectable;
    if (!target_map.valid) {
        bridge.error = target_map.uninspectableReason.empty()
                           ? "target KST occupancy map is invalid"
                           : target_map.uninspectableReason;
        return bridge;
    }

    bridge.catalog = buildTargetSoundCatalogView(target_map);

    for (const auto& [addr, km] : target_map.drumKeyMaps) {
        if (!addr.isDrumKit || !addr.isInSpec()) {
            ++bridge.drum_kits_skipped_out_of_spec;
            continue;
        }
        const media::selective::OccupiedSlotEntry* occ = target_map.findOccupied(addr);
        if (km.hasBank2Layers() || (occ != nullptr && occ->equivalence.hasNonRamBank2Layers)) {
            ++bridge.drum_kits_skipped_bank2;
            continue;
        }
        const std::string label = (occ != nullptr && !occ->displayName.empty())
                                      ? occ->displayName
                                      : addr.shortLabel();
        auto mapped = buildComparableUserdkKeyMap(km, label, occ ? &occ->equivalence : nullptr);
        if (!mapped.has_value()) {
            ++bridge.drum_kits_skipped_uncomparable;
            continue;
        }
        bridge.drum_key_maps.emplace(std::make_tuple(addr.msb(), addr.lsb(), addr.pc()), std::move(*mapped));
    }

    return bridge;
}

// Strictly read-only disk folder inspector (O_RDONLY | O_DIRECTORY | O_NOFOLLOW via Track A
// `inspectTargetKstFolder`). Refuses missing paths, non-directories, and symlinks fail-closed;
// NEVER writes to or mutates the target KST directory (NATIVE_SOUND_WRITE_HOLD).
inline TargetKstReadOnlyBridge inspectTargetKstReadOnly(
    const std::filesystem::path& kst_dir,
    const media::selective::TargetInspectOptions& opts = {}) {
    media::selective::TargetInspectOptions ro_opts = opts;
    ro_opts.hashObjectFiles = false;  // Sound/USERDK catalog read needs no Style/KSC object hashing
    const media::selective::TargetOccupancyMap occ =
        media::selective::inspectTargetKstFolder(kst_dir, ro_opts);
    return buildTargetKstBridge(occ);
}

}  // namespace ai_arranger::exporters::korg::quality

#endif  // AI_ARRANGER_EXPORTERS_KORG_TARGET_KST_DRUM_ADAPTER_H
