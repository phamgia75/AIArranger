// Track B Q4a unit tests for standalone Korg Pa5X Factory Catalog &
// Read-Only Target-KST Sound Matcher. 100% synthetic fixtures, zero private data.

// Dual-include order verification (Order B: pa5x_sound_matcher.h -> timbre_profiler.h).
#include "exporters/korg/pa5x_sound_matcher.h"
#include "exporters/korg/timbre_profiler.h"
#include "exporters/korg/cv_splitter.h"
#include "exporters/korg/megavoice_sanitizer.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;
using ai_arranger::uasf::SectionType;
using ai_arranger::uasf::TrackRole;

namespace {

int failures = 0;
int passes   = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

TrackTimbreProfile makeProfile(TimbreFamily fam, const char* sub, uint8_t korg_ch,
                               uint8_t conf = 90, bool prefers_dnc = false,
                               int msb = 0, int lsb = 0, int pc = 0) {
    TrackTimbreProfile p{};
    p.src_ch = korg_ch;
    p.korg_ch = korg_ch;
    p.yamaha_msb = msb;
    p.yamaha_lsb = lsb;
    p.yamaha_pc = pc;
    p.inferred = {fam, sub, prefers_dnc};
    p.confidence = conf;
    return p;
}

// Structural mock matching `media::selective::TargetOccupancyMap` layout
// (verifies zero-dependency template adapter without including src/media/).
struct MockSlotAddress {
    bool     isDrumKit = false;
    uint32_t bank      = 1;
    uint32_t slot      = 0;
    bool operator<(const MockSlotAddress& o) const {
        return std::tie(isDrumKit, bank, slot) < std::tie(o.isDrumKit, o.bank, o.slot);
    }
    uint8_t msb() const { return isDrumKit ? 120u : 121u; }
    uint8_t lsb() const { return static_cast<uint8_t>(63u + bank); }
    uint8_t pc()  const { return static_cast<uint8_t>(slot); }
    bool isInSpec() const {
        const uint32_t max_b = isDrumKit ? 6u : 12u;
        return bank >= 1u && bank <= max_b && slot < 128u;
    }
};

struct MockOccupiedSlotEntry {
    MockSlotAddress address;
    std::string displayName;
};

struct MockPartialInspect {
    bool isPartial = false;
    std::string notice;
};

struct MockTargetOccupancyMap {
    std::map<MockSlotAddress, MockOccupiedSlotEntry> occupiedSlots;
    MockPartialInspect partialInspect;
};

} // namespace

int main() {
    std::printf("Test: Track B Q4a Pa5X Sound Matcher\n");

    // ── 1. Factory Catalog Epistemic Audit & Full 22-Family Coverage ──
    {
        const auto& cat = pa5xFactoryCatalog();
        TEST("factory catalog is non-empty", cat.size() >= 35);

        bool all_epistemic_ok = true;
        bool all_midi_ok      = true;
        std::set<uint8_t> covered_families;
        for (const auto& e : cat) {
            if (std::string(e.epistemic) != kEpistemicFactoryDocumented ||
                e.hardware_verified != false ||
                std::string(e.l3_status) != kL3StatusNotTested) {
                all_epistemic_ok = false;
            }
            const bool expected_msb_ok = e.is_drum_kit ? (e.ref.msb == 120) : (e.ref.msb == 121);
            if (!expected_msb_ok || e.ref.lsb > 127 || e.ref.pc > 127 ||
                !e.display_name || e.display_name[0] == '\0' || !e.man5_ref || e.man5_ref[0] == '\0') {
                all_midi_ok = false;
            }
            covered_families.insert(static_cast<uint8_t>(e.family));
        }

        TEST("every factory catalog entry carries DOCUMENTED_NOT_HW_VERIFIED, hwVerified=false, L3=NOT_TESTED",
             all_epistemic_ok);
        TEST("every factory catalog entry has valid 7-bit MIDI and strict 120/121 drum/melodic MSB",
             all_midi_ok);
        TEST("factory catalog covers all 21 concrete TimbreFamilies (excluding Unknown)",
             covered_families.size() == (kTimbreFamilyCount - 1u) &&
             covered_families.count(static_cast<uint8_t>(TimbreFamily::Unknown)) == 0);

        const std::string cav(kPa5xK8K9Caveat);
        TEST("K8/K9 caveat documents 0-based vs 1-based PC & hardwareVerified=false / L3=NOT_TESTED",
             cav.find("K8/K9") != std::string::npos &&
             cav.find("0-based") != std::string::npos &&
             cav.find("1-based") != std::string::npos &&
             cav.find("hardwareVerified=false") != std::string::npos &&
             cav.find("L3=NOT_TESTED") != std::string::npos);
    }

    // ── 2. Default Opt-In OFF Preserves UNRESOLVED Behavior ──
    {
        SoundMatchOptions def_opt{};
        TEST("default SoundMatchOptions has enable_factory_catalog=false, prefer_target_user_sounds=false, min=70, bonus=10",
             !def_opt.enable_factory_catalog && !def_opt.prefer_target_user_sounds &&
             def_opt.min_auto_select_score == 70 && def_opt.target_user_bonus == 10);

        const auto pno = makeProfile(TimbreFamily::Piano, "Acoustic Grand Piano", 11);
        const auto m = matchTrackSound(pno, {}, nullptr, def_opt);
        TEST("default OFF: selected == nullopt (preserves UNRESOLVED)", !m.selected.has_value());
        TEST("default OFF: advisory top_candidates still populated with DOCUMENTED_NOT_HW_VERIFIED",
             !m.top_candidates.empty() &&
             m.top_candidates[0].origin == MatchOrigin::Pa5xFactoryCatalog &&
             m.top_candidates[0].epistemic_tag == kEpistemicFactoryDocumented &&
             !m.top_candidates[0].hardware_verified &&
             m.top_candidates[0].score >= 85);
    }

    // ── 3. User --sound-map Override ALWAYS Wins Unconditionally ──
    {
        TargetSoundCatalogView target{};
        TargetSoundEntry te{};
        te.msb = 121; te.lsb = 64; te.pc = 10;
        te.is_drum_kit = false; te.in_spec = true;
        te.display_name = "Synthetic Rhodes Stage EP";
        target.entries.push_back(te);

        SoundMatchOptions opt{};
        opt.enable_factory_catalog = true;
        opt.prefer_target_user_sounds = true;

        std::map<uint8_t, SoundRef> user_map;
        user_map[12] = SoundRef{121, 5, 42};

        const auto ep = makeProfile(TimbreFamily::ElectricPiano, "Rhodes EP", 12, 95, true);
        const auto m = matchTrackSound(ep, user_map, &target, opt);
        TEST("user --sound-map ALWAYS wins unconditionally over Factory & Target KST",
             m.selected.has_value() &&
             m.selected->msb == 121 && m.selected->lsb == 5 && m.selected->pc == 42 &&
             m.selected_origin == MatchOrigin::UserSoundMap &&
             m.selected_epistemic_tag == kEpistemicUserVerified &&
             !m.top_candidates.empty() &&
             m.top_candidates[0].origin == MatchOrigin::UserSoundMap &&
             m.top_candidates[0].score == 100);
    }

    // ── 4. Factory Catalog Matching Across All 21 Concrete Families & Key Sub-Families ──
    {
        SoundMatchOptions opt{};
        opt.enable_factory_catalog = true;

        bool all_21_match = true;
        for (uint8_t i = 1; i < kTimbreFamilyCount; ++i) {
            const auto fam = static_cast<TimbreFamily>(i);
            const uint8_t kch = (fam == TimbreFamily::DrumKit)       ? kDrumCh
                              : (fam == TimbreFamily::PercussionKit) ? kPercCh
                              : (fam == TimbreFamily::AcousticBass || fam == TimbreFamily::ElectricBass || fam == TimbreFamily::SynthBass)
                                    ? kBassCh
                                    : uint8_t{11};
            const auto prof = makeProfile(fam, timbreFamilyName(fam), kch, 90);
            const auto m = matchTrackSound(prof, {}, nullptr, opt);
            const uint8_t expect_msb = isPercussiveTimbreFamily(fam) ? 120u : 121u;
            if (!m.selected.has_value() ||
                m.selected->msb != expect_msb ||
                m.selected_origin != MatchOrigin::Pa5xFactoryCatalog ||
                m.selected_epistemic_tag != kEpistemicFactoryDocumented ||
                m.top_candidates.empty() ||
                m.top_candidates[0].hardware_verified ||
                m.top_candidates[0].l3_status != kL3StatusNotTested ||
                m.top_candidates[0].caveat.empty()) {
                std::fprintf(stderr, "    Factory match failed for family %u (%s)\n",
                             unsigned(i), timbreFamilyName(fam));
                all_21_match = false;
                break;
            }
        }
        TEST("opt-in Factory catalog matches all 21 concrete TimbreFamilies with strict 120/121 MSB & epistemic tags",
             all_21_match);

        // Sub-family specificity verification
        auto topName = [&](TimbreFamily fam, const char* sub, uint8_t kch, bool dnc = false) -> std::string {
            const auto m = matchTrackSound(makeProfile(fam, sub, kch, 92, dnc), {}, nullptr, opt);
            return m.top_candidates.empty() ? "" : m.top_candidates[0].display_name;
        };
        TEST("sub-family: Brush Kit -> Brush Drum Kit GM2",
             topName(TimbreFamily::DrumKit, "Brush Drum Kit", kDrumCh) == "Brush Drum Kit GM2");
        TEST("sub-family: Rock Kit -> Rock / Power Kit GM2",
             topName(TimbreFamily::DrumKit, "Rock / Power Drum Kit", kDrumCh) == "Rock / Power Kit GM2");
        TEST("sub-family: Electronic Kit -> Electronic / Analog Kit GM2",
             topName(TimbreFamily::DrumKit, "Electronic / Analog Drum Kit", kDrumCh) == "Electronic / Analog Kit GM2");
        TEST("sub-family: Revo! Pop Kit -> Pop Kit 1 RR",
             topName(TimbreFamily::DrumKit, "Revo! Standard Pop Drum Kit", kDrumCh, true) == "Pop Kit 1 RR");
        TEST("sub-family: Nylon Acoustic Guitar -> Classic Gtr 1 DNC",
             topName(TimbreFamily::AcousticGuitar, "Nylon Acoustic Guitar", 11, true) == "Classic Gtr 1 DNC");
        TEST("sub-family: Steel Acoustic Guitar -> Steel Guitar1 DNC",
             topName(TimbreFamily::AcousticGuitar, "Steel Acoustic Guitar", 11, true) == "Steel Guitar1 DNC");
        TEST("sub-family: Clean Electric Guitar -> Strat N Cln DNC",
             topName(TimbreFamily::ElectricGuitar, "Clean Electric Guitar", 11, true) == "Strat N Cln DNC");
        TEST("sub-family: Jazz Electric Guitar -> Jazz Electric Guitar",
             topName(TimbreFamily::ElectricGuitar, "Jazz Electric Guitar", 11) == "Jazz Electric Guitar");
        TEST("sub-family: Distortion Electric Guitar -> Distortion Guitar GM2",
             topName(TimbreFamily::ElectricGuitar, "Distortion Electric Guitar", 11) == "Distortion Guitar GM2");
        TEST("sub-family: Rhodes EP -> MK I Stage",
             topName(TimbreFamily::ElectricPiano, "Rhodes EP", 12) == "MK I Stage");
        TEST("sub-family: Wurlitzer EP -> Wurlitzer EP",
             topName(TimbreFamily::ElectricPiano, "Wurlitzer EP", 12) == "Wurlitzer EP");
        TEST("sub-family: Clavinet -> Clavinet",
             topName(TimbreFamily::ElectricPiano, "Clavinet", 12) == "Clavinet");
        TEST("sub-family: Harpsichord -> Harpsichord",
             topName(TimbreFamily::Piano, "Harpsichord", 11) == "Harpsichord");
        TEST("sub-family: Acoustic Upright Bass -> Acoustic Bass 1",
             topName(TimbreFamily::AcousticBass, "Acoustic Upright Bass", kBassCh) == "Acoustic Bass 1");
        TEST("sub-family: Finger Electric Bass -> JBass Fing DN1",
             topName(TimbreFamily::ElectricBass, "Finger Electric Bass", kBassCh, true) == "JBass Fing DN1");
    }

    // ── 5. Fail-Closed (< 70 & Unknown) Stays UNRESOLVED ──
    {
        SoundMatchOptions opt{};
        opt.enable_factory_catalog = true;
        opt.prefer_target_user_sounds = true;

        const auto unk = makeProfile(TimbreFamily::Unknown, "Unidentified", 11, 0);
        const auto m_unk = matchTrackSound(unk, {}, nullptr, opt);
        TEST("fail-closed: Unknown family produces 0 candidates and stays UNRESOLVED",
             m_unk.top_candidates.empty() && !m_unk.selected.has_value());

        const auto low_conf = makeProfile(TimbreFamily::Piano, "Generic", 11, 25);
        const auto m_low = matchTrackSound(low_conf, {}, nullptr, opt);
        TEST("fail-closed: low-confidence (<40 -> score <70) stays UNRESOLVED",
             !m_low.top_candidates.empty() && m_low.top_candidates[0].score < 70 && !m_low.selected.has_value());

        SoundMatchOptions strict_opt = opt;
        strict_opt.min_auto_select_score = 99;
        const auto m_strict = matchTrackSound(makeProfile(TimbreFamily::Piano, "Acoustic Grand Piano", 11, 95),
                                              {}, nullptr, strict_opt);
        TEST("fail-closed: score < min_auto_select_score stays UNRESOLVED",
             !m_strict.top_candidates.empty() && m_strict.top_candidates[0].score < 99 &&
             !m_strict.selected.has_value());
    }

    // ── 6. Target-KST Read-Only Synthetic Matching, USERDK07..12 Rejection & Drum/Melodic Separation ──
    {
        // Build via zero-dependency template adapter mirroring media::selective::TargetOccupancyMap
        MockTargetOccupancyMap mock_occ{};
        mock_occ.partialInspect.isPartial = true;
        mock_occ.partialInspect.notice = "Synthetic partial inspect notice (OC31 RAM.KMP skipped)";

        // In-spec melodic: USER03/12 (MSB 121, LSB 66, PC 12)
        MockSlotAddress s_ep{false, 3, 12};
        mock_occ.occupiedSlots[s_ep] = {s_ep, "Synthetic Stage Rhodes EP"};

        // In-spec melodic with deceptive drum name (must NEVER match Drum channel!)
        MockSlotAddress s_mel_deceptive{false, 1, 5};
        mock_occ.occupiedSlots[s_mel_deceptive] = {s_mel_deceptive, "Synthetic Pop Brush Drum Kit"};

        // In-spec drum kit: USERDK01/3 (MSB 120, LSB 64, PC 3)
        MockSlotAddress s_dk_ok{true, 1, 3};
        mock_occ.occupiedSlots[s_dk_ok] = {s_dk_ok, "Synthetic Jazz Brush Kit"};

        // In-spec drum kit with deceptive piano name (must NEVER match Piano channel!)
        MockSlotAddress s_dk_deceptive{true, 2, 1};
        mock_occ.occupiedSlots[s_dk_deceptive] = {s_dk_deceptive, "Synthetic Grand Piano"};

        // Out-of-spec drum kit: USERDK07/0 (MSB 120, LSB 70, PC 0) — MUST BE REJECTED!
        MockSlotAddress s_dk_oos7{true, 7, 0};
        mock_occ.occupiedSlots[s_dk_oos7] = {s_dk_oos7, "Synthetic Rock Power Drum Kit"};

        // Out-of-spec drum kit: USERDK12/9 (MSB 120, LSB 75, PC 9) — MUST BE REJECTED!
        MockSlotAddress s_dk_oos12{true, 12, 9};
        mock_occ.occupiedSlots[s_dk_oos12] = {s_dk_oos12, "Synthetic Brush Drum Kit"};

        auto target_view = buildTargetSoundCatalogFromOccupancy(mock_occ);

        // Also inject a tampered entry with USERDK07 (lsb=70) falsely claiming in_spec=true!
        TargetSoundEntry tampered{};
        tampered.msb = 120; tampered.lsb = 70; tampered.pc = 4;
        tampered.is_drum_kit = true;
        tampered.in_spec = true; // tampered flag — defense-in-depth MUST still reject!
        tampered.display_name = "Synthetic Pop Studio Drum Kit";
        target_view.entries.push_back(tampered);

        // Verify defense-in-depth helper directly
        TEST("isTargetSoundEntryInSpec rejects tampered USERDK07 (lsb=70) even if in_spec=true",
             !isTargetSoundEntryInSpec(tampered));

        SoundMatchOptions opt{};
        opt.enable_factory_catalog = true;
        opt.prefer_target_user_sounds = true;
        opt.target_user_bonus = 10;

        std::vector<TrackTimbreProfile> style_profs = {
            makeProfile(TimbreFamily::ElectricPiano, "Rhodes EP", 12, 92),
            makeProfile(TimbreFamily::DrumKit, "Brush Drum Kit", kDrumCh, 92),
            makeProfile(TimbreFamily::DrumKit, "Rock / Power Drum Kit", kDrumCh, 92),
            makeProfile(TimbreFamily::Piano, "Acoustic Grand Piano", 11, 92)
        };

        const auto style_res = matchStyleSounds(style_profs, {}, &target_view, opt);
        TEST("Target-KST skips all 3 out-of-spec USERDK07..12 entries (4 in-spec considered)",
             style_res.target_entries_considered == 4 &&
             style_res.target_entries_skipped_oos == 3 &&
             style_res.target_partial_inspect &&
             style_res.target_partial_notice.find("Synthetic partial inspect") != std::string::npos);

        // EP track: USER03/12 wins with target_user_bonus
        const auto& m_ep = style_res.tracks[0];
        TEST("Target-KST prefer_target_user_sounds selects in-spec USER03/12 Rhodes EP with bonus",
             m_ep.selected.has_value() &&
             m_ep.selected->msb == 121 && m_ep.selected->lsb == 66 && m_ep.selected->pc == 12 &&
             m_ep.selected_origin == MatchOrigin::TargetKstUserSound &&
             m_ep.selected_epistemic_tag == kEpistemicTargetKstOccupied &&
             m_ep.top_candidates[0].target_slot.has_value() &&
             m_ep.top_candidates[0].target_slot->shortLabel() == "USER03/12");

        // Brush Drum track: selects in-spec USERDK01/3, NEVER USERDK12/9 or melodic USER01/5
        const auto& m_brush = style_res.tracks[1];
        TEST("Target-KST Brush Drum selects in-spec USERDK01/3 (never USERDK12/9 or melodic USER01/5)",
             m_brush.selected.has_value() &&
             m_brush.selected->msb == 120 && m_brush.selected->lsb == 64 && m_brush.selected->pc == 3 &&
             m_brush.selected_origin == MatchOrigin::TargetKstUserSound &&
             m_brush.top_candidates[0].target_slot->shortLabel() == "USERDK01/3");

        // Rock Drum track: USERDK07/0 was rejected -> falls back cleanly to Factory Rock/Power Kit GM2
        const auto& m_rock = style_res.tracks[2];
        bool any_oos_in_rock = false;
        for (const auto& c : m_rock.top_candidates) {
            if (c.origin == MatchOrigin::TargetKstUserSound && c.ref.lsb >= 70) any_oos_in_rock = true;
            if (c.ref.msb != 120) any_oos_in_rock = true;
        }
        TEST("Target-KST Rock Drum rejects USERDK07/0 & melodic slots, falls back to Factory Rock Kit",
             !any_oos_in_rock &&
             m_rock.selected.has_value() &&
             m_rock.selected_origin == MatchOrigin::Pa5xFactoryCatalog &&
             m_rock.top_candidates[0].display_name == "Rock / Power Kit GM2");

        // Piano track: deceptive USERDK02/1 'Synthetic Grand Piano' MUST NEVER match melodic Acc1
        const auto& m_pno = style_res.tracks[3];
        bool any_dk_on_piano = false;
        for (const auto& c : m_pno.top_candidates) {
            if (c.ref.msb == 120) any_dk_on_piano = true;
        }
        TEST("strict drum/melodic separation: USERDK02/1 never matches melodic Piano track",
             !any_dk_on_piano &&
             m_pno.selected.has_value() &&
             m_pno.selected_origin == MatchOrigin::Pa5xFactoryCatalog &&
             m_pno.selected->msb == 121 && m_pno.selected->lsb == 20 && m_pno.selected->pc == 0);
    }

    // ── 7. Per-Element Mid-Style Voice Matching ──
    {
        TrackTimbreProfile prof = makeProfile(TimbreFamily::ElectricPiano, "Rhodes EP", 12, 90, false, 104, 7, 4);
        prof.element_yamaha_voices[SectionType::Intro1] = {0, 0, 0};      // Acoustic Grand Piano
        prof.element_yamaha_voices[SectionType::Main1]  = {104, 7, 4};    // Dominant Rhodes EP
        prof.element_yamaha_voices[SectionType::Main4]  = {104, 5, 61};   // Brass Section

        // Default OFF: advisory candidates populated, element_selected empty
        const auto m_off = matchTrackSound(prof, {}, nullptr, SoundMatchOptions{});
        TEST("per-element default OFF: element_candidates advisory present, element_selected empty",
             m_off.element_candidates.size() == 2 && m_off.element_selected.empty());

        // Opt-in ON: Intro1 -> Austrian Grand (121/20/0), Main4 -> Big Band Brass 1 (121/32/61)
        SoundMatchOptions opt_on{};
        opt_on.enable_factory_catalog = true;
        const auto res_on = matchStyleSounds({prof}, {}, nullptr, opt_on);
        TEST("per-element opt-in ON: populates element_sounds for Intro1 (Piano) & Main4 (Brass)",
             res_on.resolved_sounds.count(12) == 1 &&
             res_on.resolved_sounds.at(12).pc == 4 &&
             res_on.element_sounds.size() == 2 &&
             res_on.element_sounds.at({SectionType::Intro1, 12}).msb == 121 &&
             res_on.element_sounds.at({SectionType::Intro1, 12}).lsb == 20 &&
             res_on.element_sounds.at({SectionType::Intro1, 12}).pc == 0 &&
             res_on.element_sounds.at({SectionType::Main4, 12}).msb == 121 &&
             res_on.element_sounds.at({SectionType::Main4, 12}).lsb == 32 &&
             res_on.element_sounds.at({SectionType::Main4, 12}).pc == 61);

        // User element override always wins even when default OFF
        std::map<std::pair<SectionType, uint8_t>, SoundRef> user_el;
        user_el[{SectionType::Intro1, 12}] = SoundRef{121, 9, 1};
        const auto res_uel = matchStyleSounds({prof}, {}, nullptr, SoundMatchOptions{}, user_el);
        TEST("explicit user element_sounds override wins unconditionally",
             res_uel.element_sounds.count({SectionType::Intro1, 12}) == 1 &&
             res_uel.element_sounds.at({SectionType::Intro1, 12}).lsb == 9 &&
             res_uel.element_sounds.at({SectionType::Intro1, 12}).pc == 1);
    }

    // ── 8. parseSoundMap() Round-Trip Compatibility & Determinism ──
    {
        Report rep{};
        TrackRow row_drum{};
        row_drum.src_ch = 9; row_drum.korg_ch = kDrumCh; row_drum.name = "Rhythm2";
        row_drum.msb = 62; row_drum.lsb = 100; row_drum.pc = 1; row_drum.family = "Drum Kit";
        rep.tracks.push_back(row_drum);

        std::vector<TrackTimbreProfile> profs = {
            makeProfile(TimbreFamily::DrumKit, "Revo! Standard Pop Drum Kit", kDrumCh, 95, true, 62, 100, 1),
            makeProfile(TimbreFamily::ElectricBass, "Finger Electric Bass", kBassCh, 92, true, 63, 60, 23),
            makeProfile(TimbreFamily::AcousticGuitar, "Steel Acoustic Guitar", 11, 92, true, 63, 120, 21),
            makeProfile(TimbreFamily::Unknown, "Unidentified", 15, 0, false, -1, -1, -1)
        };

        // Case A: default OFF (--sound-template without --auto-sound-map)
        const SoundMatchOptions opt_off{};
        const auto m_off = matchStyleSounds(profs, {}, nullptr, opt_off);
        const std::string tpl_off = formatSoundMapTemplateWithRecommendations(rep, m_off, "SYNTH_POP.sty", opt_off);
        std::istringstream ss_off(tpl_off);
        const auto parsed_off = parseSoundMap(ss_off, "tpl_off.soundmap");
        TEST("template (default OFF) parses cleanly via parseSoundMap(): 0 sounds, 8 unresolved",
             parsed_off.ok && parsed_off.sounds.empty() && parsed_off.unresolved.size() == 8);
        TEST("template (default OFF) includes ranked # Suggested comments, epistemic tag & K8/K9 caveat",
             tpl_off.find("# Suggested #1:") != std::string::npos &&
             tpl_off.find("DOCUMENTED_NOT_HW_VERIFIED") != std::string::npos &&
             tpl_off.find("K8/K9 caveat") != std::string::npos &&
             tpl_off.find("hardwareVerified=false") != std::string::npos);

        // Case B: opt-in ON (--sound-template --auto-sound-map)
        SoundMatchOptions opt_on{};
        opt_on.enable_factory_catalog = true;
        const auto m_on = matchStyleSounds(profs, {}, nullptr, opt_on);
        const std::string tpl_on = formatSoundMapTemplateWithRecommendations(rep, m_on, "SYNTH_POP.sty", opt_on);
        std::istringstream ss_on(tpl_on);
        const auto parsed_on = parseSoundMap(ss_on, "tpl_on.soundmap");
        TEST("template (opt-in ON) parses cleanly via parseSoundMap(): 3 resolved, 5 unresolved",
             parsed_on.ok && parsed_on.sounds.size() == 3 && parsed_on.unresolved.size() == 5 &&
             parsed_on.sounds.at(kDrumCh).msb == 120 && parsed_on.sounds.at(kDrumCh).pc == 100 &&
             parsed_on.sounds.at(kBassCh).msb == 121 && parsed_on.sounds.at(kBassCh).pc == 33 &&
             parsed_on.sounds.at(11).msb == 121 && parsed_on.sounds.at(11).pc == 25 &&
             parsed_on.unresolved.count(15) == 1);

        // Determinism check: second invocation produces byte-identical template
        const std::string tpl_on_2 = formatSoundMapTemplateWithRecommendations(
            rep, matchStyleSounds(profs, {}, nullptr, opt_on), "SYNTH_POP.sty", opt_on);
        TEST("matcher & template output are 100% deterministic", tpl_on == tpl_on_2);
    }

    // ── 9. Audit Round 1 Regression Tests (P0-1, P1-3, P1-4, P1-5, P2-1) ──
    {
        // P0-1: Comment-framing newline/CR/NUL/control injection sanitization in template output
        {
            Report evil_rep{};
            TrackRow evil_row{};
            evil_row.src_ch = 11;
            evil_row.korg_ch = 11;
            evil_row.name = "Track\r\nacc1 0 0 0";
            evil_row.family = "Piano\nacc2 1 2 3";
            evil_row.megavoice = true;
            evil_row.megavoice_why = "zones\nacc3 4 5 6";
            evil_rep.tracks.push_back(evil_row);

            TargetSoundCatalogView evil_target{};
            evil_target.is_partial_inspect = true;
            evil_target.partial_notice = "Notice\nmalicious";
            TargetSoundEntry evil_entry{};
            evil_entry.msb = 121; evil_entry.lsb = 64; evil_entry.pc = 7;
            evil_entry.is_drum_kit = false; evil_entry.in_spec = true;
            evil_entry.display_name = std::string("Evil Grand Piano\nacc5 121 0 33\n# tail") + '\0' + "nulltail";
            evil_target.entries.push_back(evil_entry);

            TrackTimbreProfile pno_prof = makeProfile(TimbreFamily::Piano, "Acoustic Grand Piano\nacc4 9 9 9", 11, 92);
            pno_prof.rationale = "test\r\nperc 120 0 1";

            SoundMatchOptions opt_off{};
            const auto evil_matches = matchStyleSounds({pno_prof}, {}, &evil_target, opt_off);
            const std::string evil_tpl = formatSoundMapTemplateWithRecommendations(
                evil_rep, evil_matches, "Evil\nbass 121 0 33", opt_off);

            std::istringstream ss(evil_tpl);
            const auto parsed = parseSoundMap(ss, "evil.soundmap");
            TEST("P0-1: newline/CR/NUL payloads in display_name/style_name/notice/row.name cannot hijack .soundmap",
                 parsed.ok && parsed.sounds.empty() && parsed.unresolved.size() == 8);
        }

        // P1-3: Strict fail-closed ceiling <= 65 whenever profile.confidence < 40 (Factory & Target KST)
        {
            SoundMatchOptions opt{};
            opt.enable_factory_catalog = true;
            opt.prefer_target_user_sounds = true;
            opt.target_user_bonus = 10;

            const auto low_gtr = makeProfile(TimbreFamily::AcousticGuitar, "Nylon Acoustic Guitar", 11, 10, true);
            const auto m_low_gtr = matchTrackSound(low_gtr, {}, nullptr, opt);
            TEST("P1-3: Factory low-confidence (conf=10, Nylon + DNC) strictly <= 65 and UNRESOLVED",
                 !m_low_gtr.top_candidates.empty() &&
                 m_low_gtr.top_candidates[0].score <= 65 &&
                 !m_low_gtr.selected.has_value());

            TargetSoundCatalogView target{};
            TargetSoundEntry tp{};
            tp.msb = 121; tp.lsb = 64; tp.pc = 0;
            tp.is_drum_kit = false; tp.in_spec = true;
            tp.display_name = "Synthetic Acoustic Grand Piano";
            tp.gm_alignment_score = 90;
            target.entries.push_back(tp);

            const auto low_pno = makeProfile(TimbreFamily::Piano, "Acoustic Grand Piano", 11, 10, false);
            const auto m_low_pno = matchTrackSound(low_pno, {}, &target, opt);
            bool all_under_65 = !m_low_pno.top_candidates.empty();
            for (const auto& c : m_low_pno.top_candidates) {
                if (c.score > 65) all_under_65 = false;
            }
            TEST("P1-3: Target KST low-confidence (conf=10, gm_alignment=90, prefer_target=true) strictly <= 65 and UNRESOLVED",
                 all_under_65 && !m_low_pno.selected.has_value());
        }

        // P1-4: Target-only opt-in (enable_factory_catalog=false, prefer_target_user_sounds=true)
        {
            TargetSoundCatalogView target{};
            TargetSoundEntry th{};
            th.msb = 121; th.lsb = 64; th.pc = 6;
            th.is_drum_kit = false; th.in_spec = true;
            th.display_name = "Harpsichord";
            target.entries.push_back(th);

            SoundMatchOptions opt{};
            opt.enable_factory_catalog = false;
            opt.prefer_target_user_sounds = true;
            opt.target_user_bonus = 0; // Ensure Factory "Austrian Grand" outscores Target "Harpsichord"

            const auto pno = makeProfile(TimbreFamily::Piano, "Acoustic Grand Piano", 11, 92);
            const auto m = matchTrackSound(pno, {}, &target, opt);
            bool sel_visible = false;
            for (const auto& c : m.top_candidates) {
                if (c.origin == MatchOrigin::TargetKstUserSound &&
                    c.ref.msb == 121 && c.ref.lsb == 64 && c.ref.pc == 6) {
                    sel_visible = true;
                }
            }
            TEST("P1-4: Target-only opt-in selects allowed Target KST candidate even when Factory scores higher",
                 m.selected.has_value() &&
                 m.selected_origin == MatchOrigin::TargetKstUserSound &&
                 m.selected->msb == 121 && m.selected->lsb == 64 && m.selected->pc == 6 &&
                 sel_visible);
        }

        // P1-5: Objective Target KST drum classification (no query-channel kPercCh bias)
        {
            TargetSoundCatalogView target{};
            TargetSoundEntry dk{};
            dk.msb = 120; dk.lsb = 64; dk.pc = 0;
            dk.is_drum_kit = true; dk.in_spec = true;
            dk.display_name = "Pop Studio Kit";
            target.entries.push_back(dk);

            SoundMatchOptions opt{};
            opt.enable_factory_catalog = false;
            opt.prefer_target_user_sounds = true;

            const auto perc = makeProfile(TimbreFamily::PercussionKit, "Standard Percussion Kit", kPercCh, 92);
            const auto m_perc = matchTrackSound(perc, {}, &target, opt);
            TEST("P1-5: kPercCh query MUST NOT auto-select DrumKit 'Pop Studio Kit' onto Percussion",
                 !m_perc.selected.has_value());

            SoundMatchOptions opt_both{};
            opt_both.enable_factory_catalog = true;
            opt_both.prefer_target_user_sounds = true;
            const auto m_perc_both = matchTrackSound(perc, {}, &target, opt_both);
            TEST("P1-5: kPercCh with factory+target picks Factory Standard Perc Kit, never 'Pop Studio Kit'",
                 m_perc_both.selected.has_value() &&
                 m_perc_both.selected_origin == MatchOrigin::Pa5xFactoryCatalog &&
                 m_perc_both.selected->pc == 69);
        }

        // P2-1: Template emission prefers resolved merged track & falls back to user_sounds for unprofiled channels
        {
            Report empty_rep{};
            std::vector<TrackTimbreProfile> merged_profs = {
                makeProfile(TimbreFamily::Unknown, "Unidentified", 11, 0),
                makeProfile(TimbreFamily::Piano, "Acoustic Grand Piano", 11, 92)
            };
            std::map<uint8_t, SoundRef> user_map;
            user_map[15] = SoundRef{121, 13, 49}; // Acc5 has no TrackTimbreProfile in merged_profs

            SoundMatchOptions opt{};
            opt.enable_factory_catalog = true;
            const auto res = matchStyleSounds(merged_profs, user_map, nullptr, opt);
            const std::string tpl = formatSoundMapTemplateWithRecommendations(empty_rep, res, "MERGED.sty", opt);
            std::istringstream ss(tpl);
            const auto parsed = parseSoundMap(ss, "merged.soundmap");
            TEST("P2-1: merged channel emits resolved track (Acc1) & unprofiled user_sounds fallback (Acc5)",
                 parsed.ok &&
                 parsed.sounds.count(11) == 1 && parsed.sounds.at(11).pc == 0 &&
                 parsed.sounds.count(15) == 1 && parsed.sounds.at(15).msb == 121 &&
                 parsed.sounds.at(15).lsb == 13 && parsed.sounds.at(15).pc == 49);
        }
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
