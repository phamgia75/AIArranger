// Strictly Read-Only Advisory DrumKitProfile Unit Tests (Track A Next — Slice A2).
// 100% synthetic fixtures (zero private owner names, zero owner bytes, zero local paths).
// Tests:
//  - All 5 deterministic advisory family cascade tiers (Tier 1..5)
//  - +113=7/0xFF catch-all noise override via Choke Group (Tier 2) & Sample Name keywords (Tier 3)
//  - Latin/Ethnic (+113=6) override via Choke Group & Sample Name keywords
//  - Alpha-bounded short keyword safety (e.g. "SYN_KBD_LOOP" does not false-match BD)
//  - Non-GM regional / custom kit (gmAlignmentScore < 0.60) does NOT fabricate GM roles
//  - ROM-only kit with standard GM choke groups 1..6 vs ROM-only kit without GM choke groups
//  - ROM(idx==0) placeholder exclusion from active/family bitmasks, choke groups & velocity stats
//  - Degenerate [127/127] velTop split detection & velTop < 127 velocity ceiling detection
//  - End-to-end buildNativeGraph() -> DrumKitKeyMap -> profileDrumKit() zero-graph-impact verification

#include "media/sound/drumkit_profile.h"
#include "media/sound/native_refs.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ai_arranger::media::graph;

static int passes = 0, failures = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static DrumKitKeyMap makeBlankPlaceholderKeyMap(uint32_t slot = 0) {
    DrumKitKeyMap km;
    km.sound = ResourceId{false, "USERDK01", Kind::Sound, slot};
    km.kindChunkId = 0x10050038u;
    km.totalLayers = 128;
    km.romDrumLayers = 128;
    for (uint32_t k = 0; k < 128u; ++k) {
        auto& ks = km.keys[k];
        ks.midiNote = static_cast<uint8_t>(k);
        ks.layerCount = 1;
        ks.occupancy = DrumKeyOccupancy::RomIndexZeroPlaceholder;
        ks.velTop.fill(127u);
        ks.advisoryPan = 64;
        ks.advisoryExclusiveGroup = 0;
        DrumKitLayerSummary ls;
        ls.layerIndex = 0;
        ls.sourceBank = 0;
        ls.tableIndex = 0;
        ls.velLo = 1;
        ls.velHi = 127;
        ls.isRam = false;
        ls.resolved = false;
        ks.layers = {ls};
    }
    return km;
}

static void setRamKey(DrumKitKeyMap& km, uint8_t note, uint8_t familyByte,
                      const std::string& sampleName, uint8_t choke = 0,
                      const std::vector<uint8_t>& velTops = {127}) {
    auto& ks = km.keys[note];
    ks.midiNote = note;
    ks.occupancy = DrumKeyOccupancy::RamPopulated;
    ks.layerCount = static_cast<uint8_t>(velTops.size());
    ks.velTop.fill(127u);
    for (size_t i = 0; i < velTops.size() && i < 8u; ++i) ks.velTop[i] = velTops[i];
    ks.advisoryExclusiveGroup = choke;
    ks.layers.clear();
    uint8_t prev = 0;
    for (size_t l = 0; l < velTops.size(); ++l) {
        DrumKitLayerSummary ls;
        ls.layerIndex = static_cast<uint8_t>(l);
        ls.sourceBank = 1;
        ls.tableIndex = static_cast<uint16_t>(note);
        ls.velHi = velTops[l];
        ls.velLo = std::min<uint8_t>(l == 0 ? 1u : static_cast<uint8_t>(std::min<uint32_t>(127u, uint32_t(prev) + 1u)), ls.velHi);
        prev = velTops[l];
        ls.isRam = true;
        ls.resolved = true;
        ls.drumSampleIndex = note;
        ls.advisoryDrumFamilyByte = familyByte;
        ls.advisoryDrumSampleName = sampleName;
        ks.layers.push_back(std::move(ls));
    }
    km.activeKeyMask.set(note);
    km.ramKeyMask.set(note);
    km.ramDrumLayers += velTops.size();
}

static void setRomPopulatedKey(DrumKitKeyMap& km, uint8_t note, uint16_t romIdx, uint8_t choke = 0) {
    auto& ks = km.keys[note];
    ks.midiNote = note;
    ks.occupancy = DrumKeyOccupancy::RomOrBank2Populated;
    ks.layerCount = 1;
    ks.velTop.fill(127u);
    ks.advisoryExclusiveGroup = choke;
    DrumKitLayerSummary ls;
    ls.layerIndex = 0;
    ls.sourceBank = 0;
    ls.tableIndex = romIdx;
    ls.velLo = 1;
    ls.velHi = 127;
    ls.isRam = false;
    ls.resolved = false;
    ks.layers = {ls};
    km.activeKeyMask.set(note);
}

static void testTier1CoreFamilyBytes() {
    std::printf("\n[Test 1] Tier 1 Core +113 (0..4) High-Confidence Classification & Placeholder Exclusion\n");
    DrumKitKeyMap km = makeBlankPlaceholderKeyMap(0);
    setRamKey(km, 36, 0, "SYN_GENERIC_A", 0); // 0 -> B
    setRamKey(km, 38, 1, "SYN_GENERIC_B", 0); // 1 -> S
    setRamKey(km, 42, 2, "SYN_GENERIC_C", 1); // 2 -> H (Tier 1 beats Tier 2)
    setRamKey(km, 45, 3, "SYN_GENERIC_D", 0); // 3 -> T
    setRamKey(km, 49, 4, "SYN_GENERIC_E", 0); // 4 -> C

    // Put a decoy choke group on a ROM(idx==0) placeholder key to ensure placeholders are strictly ignored!
    km.keys[71].advisoryExclusiveGroup = 2;
    km.keys[71].velTop[0] = 80; // placeholder velTop must not pollute minEffectiveMaxVel!

    const DrumKitProfile p = profileDrumKit(km);
    TEST("Tier 1 maps +113 {0..4} -> {B,S,H,T,C} with High confidence",
         p.keys[36].advisoryFamily == 'B' && p.keys[36].familyTier == DrumFamilyTier::Tier1CoreFamilyByte &&
         p.keys[36].familyConfidence == DrumFamilyConfidence::High &&
         p.keys[38].advisoryFamily == 'S' && p.keys[38].familyTier == DrumFamilyTier::Tier1CoreFamilyByte &&
         p.keys[42].advisoryFamily == 'H' && p.keys[42].familyTier == DrumFamilyTier::Tier1CoreFamilyByte &&
         p.keys[45].advisoryFamily == 'T' && p.keys[45].familyTier == DrumFamilyTier::Tier1CoreFamilyByte &&
         p.keys[49].advisoryFamily == 'C' && p.keys[49].familyTier == DrumFamilyTier::Tier1CoreFamilyByte);

    TEST("ROM(idx==0) placeholders excluded from activeKeyMask, family bitmasks, chokeGroups & minEffectiveMaxVel",
         p.populatedKeyCount == 5 && p.ramPopulatedCount == 5 &&
         p.romOrBank2PopulatedCount == 0 && p.romPlaceholderCount == 123 && p.emptyOrUnknownCount == 0 &&
         p.activeKeyMask.count() == 5 && p.ramKeyMask.count() == 5 &&
         !p.hasKey(0) && !p.hasKey(71) && p.familyOf(71) == '\0' &&
         p.familyB.count() == 1 && p.familyB.test(36) &&
         p.familyS.count() == 1 && p.familyS.test(38) &&
         p.familyH.count() == 1 && p.familyH.test(42) &&
         p.familyT.count() == 1 && p.familyT.test(45) &&
         p.familyC.count() == 1 && p.familyC.test(49) &&
         p.familyL.none() && p.familyE.none() && p.familyX.none() &&
         p.chokeGroups.size() == 1 && p.chokeGroups.count(1) == 1 && p.chokeGroups.count(2) == 0 &&
         p.minEffectiveMaxVel == 127 && p.subMaxVelCeilingKeyCount == 0);

    TEST("GM anchor score is 1.0 (4/4 populated anchors matched) -> isGmAligned == true",
         p.gmAnchorPopulatedCount == 4 && p.gmAnchorMatchedCount == 4 &&
         std::fabs(p.gmAlignmentScore - 1.0) < 1e-9 && p.isGmAligned);
}

static void testTier2ChokeGroupHintsAndNoiseOverride() {
    std::printf("\n[Test 2] Tier 2 Choke Group Hints Overriding +113=7/0xFF/6 Noise\n");
    DrumKitKeyMap km = makeBlankPlaceholderKeyMap(1);
    setRamKey(km, 42, 0xFF, "SYN_UNLABELLED_42", 1); // group 1 -> H
    setRamKey(km, 44, 7,    "SYN_UNLABELLED_44", 1); // group 1 -> H (overrides +113=7 noise!)
    setRamKey(km, 46, 0xFF, "SYN_UNLABELLED_46", 1); // group 1 -> H
    setRamKey(km, 71, 6,    "SYN_UNLABELLED_71", 2); // group 2 -> L (overrides +113=6!)
    setRamKey(km, 72, 0xFF, "SYN_UNLABELLED_72", 2); // group 2 -> L
    setRamKey(km, 73, 6,    "SYN_UNLABELLED_73", 3); // group 3 -> L
    setRamKey(km, 74, 7,    "SYN_UNLABELLED_74", 3); // group 3 -> L
    setRamKey(km, 78, 6,    "SYN_UNLABELLED_78", 4); // group 4 -> L
    setRamKey(km, 79, 6,    "SYN_UNLABELLED_79", 4); // group 4 -> L
    setRamKey(km, 80, 6,    "SYN_UNLABELLED_80", 5); // group 5 -> L
    setRamKey(km, 81, 0xFF, "SYN_UNLABELLED_81", 5); // group 5 -> L
    setRamKey(km, 86, 7,    "SYN_UNLABELLED_86", 6); // group 6 -> L
    setRamKey(km, 87, 0xFF, "SYN_UNLABELLED_87", 6); // group 6 -> L
    setRamKey(km, 25, 0xFF, "SYN_UNLABELLED_25", 7); // group 7 -> X
    setRamKey(km, 29, 0xFF, "SYN_UNLABELLED_29", 7); // group 7 -> X
    setRamKey(km, 30, 0xFF, "SYN_UNLABELLED_30", 7); // group 7 -> X
    // Non-canonical note 60 with choke group 1 must NOT trigger Tier 2!
    setRamKey(km, 60, 6,    "SYN_REGIONAL_60",   1); // should fall through to Tier 4 ('E')

    const DrumKitProfile p = profileDrumKit(km);
    TEST("Tier 2 classifies choke groups 1->H, 2..6->L, 7->X on canonical notes & overrides +113=7/0xFF/6",
         p.keys[42].advisoryFamily == 'H' && p.keys[42].familyTier == DrumFamilyTier::Tier2ChokeGroupHint &&
         p.keys[44].advisoryFamily == 'H' && p.keys[44].familyTier == DrumFamilyTier::Tier2ChokeGroupHint &&
         p.keys[46].advisoryFamily == 'H' && p.keys[46].familyTier == DrumFamilyTier::Tier2ChokeGroupHint &&
         p.keys[71].advisoryFamily == 'L' && p.keys[72].advisoryFamily == 'L' &&
         p.keys[73].advisoryFamily == 'L' && p.keys[74].advisoryFamily == 'L' &&
         p.keys[78].advisoryFamily == 'L' && p.keys[79].advisoryFamily == 'L' &&
         p.keys[80].advisoryFamily == 'L' && p.keys[81].advisoryFamily == 'L' &&
         p.keys[86].advisoryFamily == 'L' && p.keys[87].advisoryFamily == 'L' &&
         p.keys[25].advisoryFamily == 'X' && p.keys[29].advisoryFamily == 'X' && p.keys[30].advisoryFamily == 'X' &&
         p.keys[60].advisoryFamily == 'E' && p.keys[60].familyTier == DrumFamilyTier::Tier4PercussionFamilyByte &&
         p.chokeGroups.at(1).size() == 4 && p.chokeGroups.at(2).size() == 2 && p.chokeGroups.at(7).size() == 3);
}

static void testTier3SampleNameKeywordsAndTier4Fallback() {
    std::printf("\n[Test 3] Tier 3 Case-Insensitive Sample Name Classifier & Tier 4 Percussion Fallback\n");
    DrumKitKeyMap km = makeBlankPlaceholderKeyMap(2);
    // All keys carry noisy +113 = 7 or 0xFF or 6, with descriptive names (mixed case)
    setRamKey(km, 10, 7,    "syn_kick_deep");        // B
    setRamKey(km, 11, 0xFF, "808bd_punch");          // B (bounded BD)
    setRamKey(km, 12, 7,    "Acoustic_Snare_1");     // S
    setRamKey(km, 13, 0xFF, "syn_sd_tight");         // S (bounded SD)
    setRamKey(km, 14, 7,    "HandClap_Dry");         // S
    setRamKey(km, 15, 0xFF, "Brush_Swirl");          // S
    setRamKey(km, 16, 7,    "RimShot_Soft");         // S
    setRamKey(km, 17, 7,    "OpenHat_Bright");       // H
    setRamKey(km, 18, 0xFF, "syn_chh_01");           // H
    setRamKey(km, 19, 0xFF, "syn_hh_pedal");         // H (bounded HH)
    setRamKey(km, 20, 7,    "FloorTom_Low");         // T
    setRamKey(km, 21, 7,    "Crash_18");             // C
    setRamKey(km, 22, 0xFF, "ChinaCym_Fast");        // C
    setRamKey(km, 23, 6,    "syn_guiro_long");       // L (overrides +113=6!)
    setRamKey(km, 24, 6,    "HiConga_Mute");         // L
    setRamKey(km, 26, 7,    "Cowbell_808");          // L
    setRamKey(km, 27, 0xFF, "Triang_Open");          // L
    setRamKey(km, 28, 0xFF, "Scratch_Hit");          // X
    setRamKey(km, 31, 0xFF, "SYN_KBD_LOOP_01");      // X (must NOT false-match BD inside KBD!)
    setRamKey(km, 32, 0xFF, "SFX_Rise");             // X
    // Tier 4 fallbacks (no matching keyword, no choke group)
    setRamKey(km, 90, 5,    "SYN_PERC_ALPHA");       // Tier 4 -> L
    setRamKey(km, 91, 6,    "SYN_REGIONAL_BETA");    // Tier 4 -> E
    setRamKey(km, 92, 7,    "SYN_UNKNOWN_GAMMA");    // non-GM kit -> stays '\0' (Unresolved)

    const DrumKitProfile p = profileDrumKit(km);
    TEST("Tier 3 classifies all 7 families case-insensitively and avoids KBD->BD false positive",
         p.keys[10].advisoryFamily == 'B' && p.keys[10].familyTier == DrumFamilyTier::Tier3SampleNameKeyword &&
         p.keys[11].advisoryFamily == 'B' && p.keys[11].familyTier == DrumFamilyTier::Tier3SampleNameKeyword &&
         p.keys[12].advisoryFamily == 'S' && p.keys[13].advisoryFamily == 'S' &&
         p.keys[14].advisoryFamily == 'S' && p.keys[15].advisoryFamily == 'S' && p.keys[16].advisoryFamily == 'S' &&
         p.keys[17].advisoryFamily == 'H' && p.keys[18].advisoryFamily == 'H' && p.keys[19].advisoryFamily == 'H' &&
         p.keys[20].advisoryFamily == 'T' &&
         p.keys[21].advisoryFamily == 'C' && p.keys[22].advisoryFamily == 'C' &&
         p.keys[23].advisoryFamily == 'L' && p.keys[24].advisoryFamily == 'L' &&
         p.keys[26].advisoryFamily == 'L' && p.keys[27].advisoryFamily == 'L' &&
         p.keys[28].advisoryFamily == 'X' &&
         p.keys[31].advisoryFamily == 'X' && p.keys[31].familyTier == DrumFamilyTier::Tier3SampleNameKeyword &&
         p.keys[32].advisoryFamily == 'X');

    TEST("Tier 4 emits 'L' (+113=5) & 'E' (+113=6); unlabelled +113=7 stays Unresolved on non-GM kit",
         p.keys[90].advisoryFamily == 'L' && p.keys[90].familyTier == DrumFamilyTier::Tier4PercussionFamilyByte &&
         p.keys[91].advisoryFamily == 'E' && p.keys[91].familyTier == DrumFamilyTier::Tier4PercussionFamilyByte &&
         p.keys[92].advisoryFamily == '\0' && p.keys[92].familyTier == DrumFamilyTier::Unresolved &&
         !p.isGmAligned);
}

static void testTier5GmAlignedVsNonGmRegionalAndRomOnly() {
    std::printf("\n[Test 4] Tier 5 GM-Alignment Gate: GM Kit vs Non-GM Regional Kit vs ROM-Only Kits\n");

    // 4a. GM-aligned RAM kit (5/5 core anchors match -> gmAlignmentScore = 1.0 >= 0.60)
    {
        DrumKitKeyMap km = makeBlankPlaceholderKeyMap(3);
        setRamKey(km, 36, 0, "SYN_KICK");
        setRamKey(km, 38, 1, "SYN_SNARE");
        setRamKey(km, 42, 2, "SYN_HH_CL", 1);
        setRamKey(km, 46, 2, "SYN_HH_OP", 1);
        setRamKey(km, 49, 4, "SYN_CRASH");
        // Unlabelled populated keys with +113=0xFF & generic names:
        setRamKey(km, 48, 0xFF, "SYN_SLOT_48"); // GM2 Hi-Mid Tom -> T
        setRamKey(km, 56, 0xFF, "SYN_SLOT_56"); // GM2 Cowbell -> L
        setRamKey(km, 57, 0xFF, "SYN_SLOT_57"); // GM2 Crash 2 -> C
        setRamKey(km, 87, 0xFF, "SYN_SLOT_87"); // GM2 Open Surdo -> L
        setRamKey(km, 24, 0xFF, "SYN_SLOT_24"); // outside 35..87 -> stays '\0'
        setRamKey(km, 96, 0xFF, "SYN_SLOT_96"); // outside 35..87 -> stays '\0'

        const DrumKitProfile p = profileDrumKit(km);
        TEST("4a: GM-aligned RAM kit infers GM2 roles on 35..87 via Tier 5, leaves <35 & >87 unlabelled",
             p.isGmAligned && std::fabs(p.gmAlignmentScore - 1.0) < 1e-9 &&
             p.keys[48].advisoryFamily == 'T' && p.keys[48].familyTier == DrumFamilyTier::Tier5GmPositionFallback &&
             p.keys[56].advisoryFamily == 'L' && p.keys[56].familyTier == DrumFamilyTier::Tier5GmPositionFallback &&
             p.keys[57].advisoryFamily == 'C' && p.keys[57].familyTier == DrumFamilyTier::Tier5GmPositionFallback &&
             p.keys[87].advisoryFamily == 'L' && p.keys[87].familyTier == DrumFamilyTier::Tier5GmPositionFallback &&
             p.keys[24].advisoryFamily == '\0' && p.keys[96].advisoryFamily == '\0' &&
             p.familyT.test(48) && p.familyL.test(56) && p.familyC.test(57) && p.familyL.test(87));
    }

    // 4b. Non-GM regional kit (1/5 core anchors match -> gmAlignmentScore = 0.20 < 0.60 -> NEVER fabricate GM!)
    {
        DrumKitKeyMap km = makeBlankPlaceholderKeyMap(4);
        setRamKey(km, 36, 0,    "SYN_KICK");          // matches anchor 36 ('B')
        setRamKey(km, 38, 6,    "SYN_REG_PERC_A");    // Ethnic, NOT Snare
        setRamKey(km, 42, 6,    "SYN_REG_PERC_B");    // Ethnic, NOT Hi-Hat
        setRamKey(km, 46, 0xFF, "SYN_REG_PERC_C");    // Unlabelled, NOT Hi-Hat
        setRamKey(km, 49, 6,    "SYN_REG_PERC_D");    // Ethnic, NOT Crash
        setRamKey(km, 48, 0xFF, "SYN_REG_UNLABEL_48");// Unlabelled on GM Tom slot -> MUST NOT become 'T'!
        setRamKey(km, 51, 0xFF, "SYN_REG_UNLABEL_51");// Unlabelled on GM Ride slot -> MUST NOT become 'C'!

        const DrumKitProfile p = profileDrumKit(km);
        TEST("4b: non-GM regional kit (gmAlignmentScore < 0.60) does NOT fabricate GM roles on unlabelled keys",
             !p.isGmAligned && p.gmAnchorPopulatedCount == 6 && p.gmAnchorMatchedCount == 1 &&
             p.gmAlignmentScore < 0.60 &&
             p.keys[36].advisoryFamily == 'B' &&
             p.keys[38].advisoryFamily == 'E' &&
             p.keys[46].advisoryFamily == '\0' && p.keys[46].familyTier == DrumFamilyTier::Unresolved &&
             p.keys[48].advisoryFamily == '\0' && p.keys[48].familyTier == DrumFamilyTier::Unresolved &&
             p.keys[51].advisoryFamily == '\0' && p.keys[51].familyTier == DrumFamilyTier::Unresolved &&
             p.familyT.none() && p.familyC.none() && p.familyH.none());
    }

    // 4c. ROM-only kit WITH standard GM choke groups 1..6 vs ROM-only kit WITHOUT GM choke groups
    {
        DrumKitKeyMap kmRomGm = makeBlankPlaceholderKeyMap(5);
        setRomPopulatedKey(kmRomGm, 36, 101, 0);
        setRomPopulatedKey(kmRomGm, 38, 102, 0);
        setRomPopulatedKey(kmRomGm, 42, 103, 1); // standard GM Hi-Hat choke group 1
        setRomPopulatedKey(kmRomGm, 46, 104, 1);
        setRomPopulatedKey(kmRomGm, 49, 105, 0);

        DrumKitKeyMap kmRomCustom = makeBlankPlaceholderKeyMap(6);
        setRomPopulatedKey(kmRomCustom, 36, 201, 0);
        setRomPopulatedKey(kmRomCustom, 38, 202, 0);
        setRomPopulatedKey(kmRomCustom, 48, 203, 0);

        const DrumKitProfile pGm = profileDrumKit(kmRomGm);
        const DrumKitProfile pCustom = profileDrumKit(kmRomCustom);
        TEST("4c: ROM-only kit with standard GM choke group 1 enables Tier 5; ROM-only kit without GM choke does NOT fabricate GM",
             pGm.isRomOnly && pGm.hasStandardGmChokeGroups &&
             pGm.keys[42].advisoryFamily == 'H' && pGm.keys[42].familyTier == DrumFamilyTier::Tier2ChokeGroupHint &&
             pGm.keys[36].advisoryFamily == 'B' && pGm.keys[36].familyTier == DrumFamilyTier::Tier5GmPositionFallback &&
             pGm.keys[38].advisoryFamily == 'S' && pGm.keys[38].familyTier == DrumFamilyTier::Tier5GmPositionFallback &&
             pGm.keys[49].advisoryFamily == 'C' && pGm.keys[49].familyTier == DrumFamilyTier::Tier5GmPositionFallback &&
             pCustom.isRomOnly && !pCustom.hasStandardGmChokeGroups && !pCustom.isGmAligned &&
             pCustom.keys[36].advisoryFamily == '\0' && pCustom.keys[38].advisoryFamily == '\0' &&
             pCustom.keys[48].advisoryFamily == '\0');
    }
}

static void testVelocityDynamicsAndDegenerateSplits() {
    std::printf("\n[Test 5] Degenerate [127/127] velTop & Sub-Max Ceiling (<127) Profiling\n");
    DrumKitKeyMap km = makeBlankPlaceholderKeyMap(7);
    // Key 36: clean 3-layer split [50, 100, 127]
    setRamKey(km, 36, 0, "SYN_KICK_3L", 0, {50, 100, 127});
    // Key 38: degenerate [127, 127] -> effectiveDistinctLayers == 1, hasDegenerateVelSplit == true
    setRamKey(km, 38, 1, "SYN_SNARE_DEG", 0, {127, 127});
    // Key 42: sub-max ceiling [60, 109] -> effectiveMaxVel == 109, hasSubMaxVelCeiling == true
    setRamKey(km, 42, 2, "SYN_HH_CAP", 1, {60, 109});
    // Key 46: degenerate AND sub-max ceiling [80, 80, 122]
    setRamKey(km, 46, 2, "SYN_HH_OP_DEG", 1, {80, 80, 122});

    const DrumKitProfile p = profileDrumKit(km);
    TEST("Velocity profiling detects degenerate [127/127] splits, distinct bands & sub-max ceiling (<127)",
         p.keys[36].layerCount == 3 && p.keys[36].effectiveDistinctLayers == 3 &&
         !p.keys[36].hasDegenerateVelSplit && !p.keys[36].hasSubMaxVelCeiling && p.keys[36].effectiveMaxVel == 127 &&
         p.keys[38].layerCount == 2 && p.keys[38].effectiveDistinctLayers == 1 &&
         p.keys[38].hasDegenerateVelSplit && !p.keys[38].hasSubMaxVelCeiling && p.keys[38].effectiveMaxVel == 127 &&
         p.keys[42].layerCount == 2 && p.keys[42].effectiveDistinctLayers == 2 &&
         !p.keys[42].hasDegenerateVelSplit && p.keys[42].hasSubMaxVelCeiling && p.keys[42].effectiveMaxVel == 109 &&
         p.keys[46].layerCount == 3 && p.keys[46].effectiveDistinctLayers == 2 &&
         p.keys[46].hasDegenerateVelSplit && p.keys[46].hasSubMaxVelCeiling && p.keys[46].effectiveMaxVel == 122 &&
         p.multiLayerKeyCount == 3 && p.maxEffectiveLayers == 3 &&
         p.degenerateVelKeyCount == 2 && p.subMaxVelCeilingKeyCount == 2 &&
         p.minEffectiveMaxVel == 109);
}

static void testMultiLayerTopDownResolution() {
    std::printf("\n[Test 6] Multi-Layer Top-Down Primary Layer Selection\n");
    DrumKitKeyMap km = makeBlankPlaceholderKeyMap(8);
    auto& ks = km.keys[36];
    ks.occupancy = DrumKeyOccupancy::RamPopulated;
    ks.layerCount = 2;
    ks.velTop = {64, 127, 127, 127, 127, 127, 127, 127};
    DrumKitLayerSummary l0;
    l0.layerIndex = 0; l0.sourceBank = 1; l0.tableIndex = 0; l0.velLo = 1; l0.velHi = 64;
    l0.isRam = true; l0.resolved = true; l0.drumSampleIndex = 0;
    l0.advisoryDrumFamilyByte = 0; // 'B'
    l0.advisoryDrumSampleName = "SYN_KICK_SOFT";
    DrumKitLayerSummary l1;
    l1.layerIndex = 1; l1.sourceBank = 1; l1.tableIndex = 1; l1.velLo = 65; l1.velHi = 127;
    l1.isRam = true; l1.resolved = true; l1.drumSampleIndex = 1;
    l1.advisoryDrumFamilyByte = 0xFF; // noisy default on top accent layer!
    l1.advisoryDrumSampleName = "SYN_KICK_HARD";
    ks.layers = {l0, l1};
    km.activeKeyMask.set(36);
    km.ramKeyMask.set(36);
    km.ramDrumLayers = 2;

    const DrumKitProfile p = profileDrumKit(km);
    TEST("Primary sample name uses top resolved RAM layer while Tier 1 recovers core +113 from lower layer when top is 0xFF",
         p.keys[36].primarySampleName == "SYN_KICK_HARD" &&
         p.keys[36].advisoryFamily == 'B' &&
         p.keys[36].familyTier == DrumFamilyTier::Tier1CoreFamilyByte);
}

int main() {
    std::printf("Test: Strictly Read-Only Advisory DrumKitProfile (Slice A2)\n");
    testTier1CoreFamilyBytes();
    testTier2ChokeGroupHintsAndNoiseOverride();
    testTier3SampleNameKeywordsAndTier4Fallback();
    testTier5GmAlignedVsNonGmRegionalAndRomOnly();
    testVelocityDynamicsAndDegenerateSplits();
    testMultiLayerTopDownResolution();
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
