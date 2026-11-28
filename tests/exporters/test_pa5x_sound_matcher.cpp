// Track B Q4a unit tests for standalone Korg Pa5X Factory Catalog &
// Read-Only Target-KST Sound Matcher. 100% synthetic fixtures, zero private data.

// Dual-include order verification (Order B: pa5x_sound_matcher.h -> timbre_profiler.h + Track A read-only adapter).
#include "exporters/korg/pa5x_sound_matcher.h"
#include "exporters/korg/target_kst_drum_adapter.h"
#include "exporters/korg/timbre_profiler.h"
#include "exporters/korg/cv_splitter.h"
#include "exporters/korg/megavoice_sanitizer.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif

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

    // ── 10. Wave-3 Q4-Wire: Track A Read-Only Bridge (`target_kst_drum_adapter.h`) ──
    {
        using namespace ai_arranger::media;

        auto makeKeyMap = [](const std::string& bank, uint32_t num, bool addBank2) {
            graph::DrumKitKeyMap km{};
            km.sound = graph::ResourceId{false, bank, graph::Kind::Sound, num};
            km.kindChunkId = 0x10050038u;

            auto setRamKey = [&](uint8_t note, uint8_t famByte, const char* sampleName, uint8_t choke, uint8_t maxVel) {
                auto& ks = km.keys[note];
                ks.midiNote = note;
                ks.layerCount = 2;
                ks.occupancy = graph::DrumKeyOccupancy::RamPopulated;
                ks.velTop = {64, maxVel, 0, 0, 0, 0, 0, 0};
                ks.advisoryExclusiveGroup = choke;
                ks.advisoryPan = 64;

                graph::DrumKitLayerSummary l0{};
                l0.layerIndex = 0; l0.sourceBank = 1; l0.isRam = true; l0.resolved = true;
                l0.velLo = 1; l0.velHi = 64;
                l0.advisoryDrumSampleName = sampleName;
                l0.advisoryDrumFamilyByte = famByte;
                graph::DrumKitLayerSummary l1 = l0;
                l1.layerIndex = 1; l1.velLo = 65; l1.velHi = maxVel;
                ks.layers = {l0, l1};

                km.activeKeyMask.set(note);
                km.ramKeyMask.set(note);
                km.ramDrumLayers += 2;
                km.totalLayers += 2;
            };

            // Core GM anchors: Kick 35/36, Snare 38/40, HiHat 42/44/46, Crash 49, Ride 51
            setRamKey(35, 0, "Acoustic_Kick_Lo", 0, 127);
            setRamKey(36, 0, "Acoustic_Kick", 0, 127);
            setRamKey(38, 1, "Acoustic_Snare", 0, 127);
            setRamKey(40, 1, "Rim_Snare", 0, 127);
            setRamKey(42, 2, "Closed_Hat", 1, 120);
            setRamKey(44, 2, "Pedal_Hat", 1, 120);
            setRamKey(46, 2, "Open_Hat", 1, 120);
            setRamKey(49, 4, "Crash_Cym", 0, 127);
            setRamKey(51, 4, "Ride_Cym", 0, 127);

            // ROM(idx==0) placeholder on key 70 MUST be excluded by adapter!
            {
                auto& ph = km.keys[70];
                ph.midiNote = 70;
                ph.layerCount = 1;
                ph.occupancy = graph::DrumKeyOccupancy::RomIndexZeroPlaceholder;
                ph.velTop[0] = 127;
            }

            if (addBank2) {
                km.nonRamBank2LayerCount = 1;
                km.nonRamDrumLayers = 1;
            }
            return km;
        };

        selective::TargetOccupancyMap occMap = selective::TargetOccupancyMap::makeEmpty(true);

        // 1. In-spec melodic slot USER02/5
        {
            selective::OccupiedSlotEntry e{};
            e.address = selective::SlotAddress{false, 2, 5};
            e.displayName = "Synthetic Warm Rhodes EP";
            e.sourceContainer = "SOUND/USER02.PCG";
            occMap.addOccupiedSlot(std::move(e));
        }
        // 2. Comparable in-spec GM-aligned USERDK01/2
        const selective::SlotAddress dkGoodAddr{true, 1, 2};
        {
            selective::OccupiedSlotEntry e{};
            e.address = dkGoodAddr;
            e.displayName = "Synthetic Studio Pop Drum Kit";
            e.sourceContainer = "SOUND/USERDK01.PCG";
            e.equivalence.isDrumKit = true;
            occMap.addOccupiedSlot(std::move(e));
            occMap.drumKeyMaps.emplace(dkGoodAddr, makeKeyMap("USERDK01", 2, false));
        }
        // 3. Bank-2 (0x02) unverified USERDK01/4 — MUST fail closed!
        const selective::SlotAddress dkBank2Addr{true, 1, 4};
        {
            selective::OccupiedSlotEntry e{};
            e.address = dkBank2Addr;
            e.displayName = "Synthetic Bank2 Brush Kit";
            e.sourceContainer = "SOUND/USERDK01.PCG";
            e.equivalence.isDrumKit = true;
            e.equivalence.hasNonRamBank2Layers = true;
            occMap.addOccupiedSlot(std::move(e));
            occMap.drumKeyMaps.emplace(dkBank2Addr, makeKeyMap("USERDK01", 4, true));
        }
        // 4. Out-of-spec USERDK07/0 — MUST stay in_spec=false & never produce a UserdkDrumKitKeyMap!
        const selective::SlotAddress dkOosAddr{true, 7, 0};
        {
            selective::OccupiedSlotEntry e{};
            e.address = dkOosAddr;
            e.displayName = "Synthetic OutOfSpec Rock Kit";
            e.sourceContainer = "SOUND/USERDK07.PCG";
            e.equivalence.isDrumKit = true;
            occMap.addOccupiedSlot(std::move(e));
            occMap.drumKeyMaps.emplace(dkOosAddr, makeKeyMap("USERDK07", 0, false));
        }
        occMap.rebuildIndexesAndKey();

        const auto bridge = buildTargetKstBridge(occMap);
        const auto entries = buildTargetSoundEntries(occMap);
        TEST("Track A adapter: read-only bridge builds catalog & skips Bank-2 and OOS USERDK07 fail-closed",
             kTargetKstAdapterReadOnly &&
             bridge.valid &&
             entries.size() == 4 &&
             bridge.drum_key_maps.size() == 1 &&
             bridge.drum_kits_skipped_bank2 == 1 &&
             bridge.drum_kits_skipped_out_of_spec == 1);

        // Verify Bank-2 kit got NO fabricated gm_alignment_score or family_bitmask, while clean USERDK01/2 got gm=100 & DrumKit bitmask
        bool goodEnriched = false, bank2Unfabricated = false, oosMarkedFalse = false;
        for (const auto& te : bridge.catalog.entries) {
            if (te.msb == 120 && te.lsb == 64 && te.pc == 2) {
                goodEnriched = te.in_spec && te.gm_alignment_score.has_value() && *te.gm_alignment_score == 100 &&
                               te.family_bitmask.has_value() && (*te.family_bitmask & timbreFamilyBit(TimbreFamily::DrumKit)) != 0;
            }
            if (te.msb == 120 && te.lsb == 64 && te.pc == 4) {
                bank2Unfabricated = !te.gm_alignment_score.has_value() && !te.family_bitmask.has_value();
            }
            if (te.msb == 120 && te.lsb == 70 && te.pc == 0) {
                oosMarkedFalse = !te.in_spec;
            }
        }
        TEST("Track A adapter: clean USERDK01/2 enriched (gm=100), Bank-2 unfabricated, USERDK07 in_spec=false",
             goodEnriched && bank2Unfabricated && oosMarkedFalse);

        // Verify findComparableTargetDrumKeyMap & Q3 remapDrumTrack integration
        const auto goodKm = findComparableTargetDrumKeyMap(occMap, SoundRef{120, 64, 2});
        const auto bank2Km = findComparableTargetDrumKeyMap(occMap, SoundRef{120, 64, 4});
        const auto oosKm = findComparableTargetDrumKeyMap(occMap, SoundRef{120, 70, 0});
        TEST("Track A adapter: comparable USERDK01/2 returns UserdkDrumKitKeyMap excluding ROM(idx==0) key 70; Bank-2 & OOS return nullopt",
             goodKm.has_value() && goodKm->isUserdkTarget() &&
             goodKm->hasKey(36) && goodKm->familyOf(36) == 'B' &&
             goodKm->hasKey(42) && goodKm->keyInfo(42).choke_group == 1 && goodKm->keyInfo(42).effective_max_vel == 120 &&
             !goodKm->hasKey(70) && goodKm->keyInfo(70).is_rom_zero_placeholder &&
             !bank2Km.has_value() && !oosKm.has_value());

        // Feed the bridged UserdkDrumKitKeyMap directly into Q3 remapDrumTrack()
        if (goodKm.has_value()) {
            const std::vector<NoteSpan> testHits = {
                {0, 120, 36, 110, false},  // exact populated kick
                {240, 360, 13, 95, false}, // Revo!/XG low key -> remapped to populated key in goodKm!
                {480, 600, 42, 127, false} // populated hi-hat with sub-max ceiling 120
            };
            const auto dr = remapDrumTrack(testHits, DrumKitCategory::StandardPopRock, *goodKm, true, 480);
            bool allInTarget = !dr.notes.empty();
            for (const auto& n : dr.notes) {
                if (!goodKm->hasKey(n.pitch)) allInTarget = false;
            }
            TEST("Track A adapter -> Q3 remapDrumTrack: 100% surviving notes land on populated USERDK01/2 keys",
                 allInTarget && dr.stats.passthrough_notes >= 2);
        }

        // Read-only disk folder fail-closed check (missing folder & symlink refused, valid empty dir succeeds with 0 mutation)
        {
            namespace fs = std::filesystem;
            const fs::path tmpDir = fs::temp_directory_path() / ("aiarr_q4_adapter_" + std::to_string(::getpid()));
            fs::remove_all(tmpDir);
            fs::create_directories(tmpDir / "Valid.KST" / "SOUND");
            const auto missing = inspectTargetKstReadOnly(tmpDir / "Missing.KST");
            const auto okEmpty = inspectTargetKstReadOnly(tmpDir / "Valid.KST");
            bool symlinkRefused = true;
#if !defined(_WIN32)
            std::error_code ec;
            fs::create_directory_symlink(tmpDir / "Valid.KST", tmpDir / "Link.KST", ec);
            if (!ec) {
                const auto linkRes = inspectTargetKstReadOnly(tmpDir / "Link.KST");
                symlinkRefused = !linkRes.valid;
            }
#endif
            TEST("Track A adapter disk read-only: missing path & symlink fail closed, valid KST succeeds with zero mutation",
                 !missing.valid && okEmpty.valid && symlinkRefused &&
                 fs::is_empty(tmpDir / "Valid.KST" / "SOUND"));
            fs::remove_all(tmpDir);
        }

        // Audit P2-3: NaN / +Inf / -Inf gmAlignmentScore fail-closed clamping (0 UBSan issues)
        {
            auto baseProf = graph::profileDrumKit(makeKeyMap("USERDK01", 2, false));
            const double badVals[3] = {
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity()
            };
            bool allNonFiniteSafe = true;
            for (double bv : badVals) {
                auto p = baseProf;
                p.gmAlignmentScore = bv;
                const auto v = buildUserdkSummaryView(p, "SYNTH_NONFINITE");
                const auto km = buildComparableUserdkKeyMap(p, "SYNTH_NONFINITE");
                const uint8_t pct = target_adapter_detail::clampGmScorePercent(bv);
                if (!std::isfinite(v.gm_alignment_score) || v.gm_alignment_score != 0.0 ||
                    !km.has_value() || !std::isfinite(km->gmAlignmentScore()) || km->gmAlignmentScore() != 0.0 ||
                    pct != 0u) {
                    allNonFiniteSafe = false;
                }
            }
            TEST("P2-3: NaN/+Inf/-Inf gmAlignmentScore fails closed deterministically to 0.0 (0 UBSan issues)",
                 allNonFiniteSafe);
        }
    }

    // ── 11. Mission 4b Regression Tests (P0-3, P1-2, P1-6, P1-7) ──
    {
        SoundMatchOptions opt{};
        opt.enable_factory_catalog = true;

        // P1-7a: Orchestra Kit GM2 (120/0/48), SFX Perc Kit GM2 (120/0/56), Timpani GM2 (121/0/47)
        const auto orchProf = profileTrackTimbre(127, 0, 48, "OrchPerc", TrackRole::Drum, {}, {}, 9, kDrumCh);
        const auto mOrch = matchTrackSound(orchProf, {}, nullptr, opt);
        TEST("Mission4b P1-7: EXP09 OrchPerc (127/0/48) -> Orchestra Kit GM2 (120/0/48, DOCUMENTED_NOT_HW_VERIFIED)",
             mOrch.selected.has_value() &&
             mOrch.selected->msb == 120 && mOrch.selected->lsb == 0 && mOrch.selected->pc == 48 &&
             mOrch.top_candidates[0].display_name == "Orchestra Kit GM2" &&
             mOrch.selected_epistemic_tag == kEpistemicFactoryDocumented &&
             !mOrch.top_candidates[0].hardware_verified);

        const auto timpProf = profileTrackTimbre(0, 0, 47, "Timpani", TrackRole::Accompaniment, {}, {}, 15, 15);
        const auto mTimp = matchTrackSound(timpProf, {}, nullptr, opt);
        TEST("Mission4b P1-7: EXP09 Timpani (0/0/47) -> Timpani GM2 (121/0/47, not Vibraphone)",
             mTimp.selected.has_value() &&
             mTimp.selected->msb == 121 && mTimp.selected->lsb == 0 && mTimp.selected->pc == 47 &&
             mTimp.top_candidates[0].display_name == "Timpani GM2");

        // P1-7b: EXP12 SfxPerc (126/0/0) on kPercCh with Main B (126/0/40) & Main C (127/0/40) kit switches!
        auto sfxProf = profileTrackTimbre(126, 0, 0, "SfxPerc", TrackRole::Percussion, {}, {}, 9, kPercCh);
        sfxProf.element_yamaha_voices[SectionType::Main1] = {126, 0, 0};
        sfxProf.element_yamaha_voices[SectionType::Main2] = {126, 0, 40};
        sfxProf.element_yamaha_voices[SectionType::Main3] = {127, 0, 40};
        const auto mSfx = matchTrackSound(sfxProf, {}, nullptr, opt);
        TEST("Mission4b P1-7: EXP12 SfxPerc (126/0/0) -> SFX Perc Kit GM2 (120/0/56) & preserves Main B + Main C switches",
             mSfx.selected.has_value() &&
             mSfx.selected->msb == 120 && mSfx.selected->lsb == 0 && mSfx.selected->pc == 56 &&
             mSfx.element_selected.count(SectionType::Main2) == 1 &&
             mSfx.element_selected.at(SectionType::Main2).pc == 64 &&
             mSfx.element_selected.count(SectionType::Main3) == 1 &&
             mSfx.element_selected.at(SectionType::Main3).pc == 40);

        // Generic Percussion Kit without SFX token still maps to Standard Perc Kit (120/0/69), not SFX!
        const auto genPerc = makeProfile(TimbreFamily::PercussionKit, "Percussion Kit", kPercCh, 70);
        const auto mGenPerc = matchTrackSound(genPerc, {}, nullptr, opt);
        TEST("Mission4b P1-7: generic Percussion Kit still resolves to Standard Perc Kit (120/0/69, not SFX)",
             mGenPerc.selected.has_value() && mGenPerc.selected->pc == 69);

        // P1-2: EXP09 Contrabass (0/0/43) -> Acoustic Bass 1 (121/13/32, NEVER Solo Violin GM2!)
        const auto cbProf = profileTrackTimbre(0, 0, 43, "ContraBs", TrackRole::Bass, {}, {}, 10, kBassCh);
        const auto mCb = matchTrackSound(cbProf, {}, nullptr, opt);
        TEST("Mission4b P1-2: EXP09 Contrabass (0/0/43) maps to Acoustic Bass 1 (121/13/32, not Solo Violin)",
             mCb.selected.has_value() &&
             mCb.selected->msb == 121 && mCb.selected->lsb == 13 && mCb.selected->pc == 32 &&
             mCb.top_candidates[0].display_name == "Acoustic Bass 1");

        // P1-6: EXP11 Steel Guitar ("Guitar", 0/0/25) -> Steel Guitar1 DNC (121/47/25, NEVER Classic Gtr 1 DNC!)
        const auto steelProf = profileTrackTimbre(0, 0, 25, "Guitar", TrackRole::Chord, {}, {}, 12, 12);
        const auto mSteel = matchTrackSound(steelProf, {}, nullptr, opt);
        TEST("Mission4b P1-6: EXP11 'Guitar' (0/0/25) maps to Steel Guitar1 DNC (121/47/25, not Classic Gtr)",
             mSteel.selected.has_value() &&
             mSteel.selected->msb == 121 && mSteel.selected->lsb == 47 && mSteel.selected->pc == 25 &&
             mSteel.top_candidates[0].display_name == "Steel Guitar1 DNC");

        // P0-3: EXP15 unknown vendor-private banks (65/12/45, 66/7/122) fail closed UNRESOLVED!
        const std::vector<NoteSpan> triad = {
            {0, 480, 60, 82, false}, {0, 480, 64, 82, false}, {0, 480, 67, 82, false}
        };
        const std::vector<NoteSpan> padChord = {
            {0, 1920, 48, 68, false}, {0, 1920, 52, 68, false}, {0, 1920, 55, 68, false}
        };
        const auto priv1 = profileTrackTimbre(65, 12, 45, "Track 1", TrackRole::Chord, triad, {}, 11, 11);
        const auto priv2 = profileTrackTimbre(66, 7, 122, "Part 3", TrackRole::Pad, padChord, {}, 13, 13);
        const auto mPriv1 = matchTrackSound(priv1, {}, nullptr, opt);
        const auto mPriv2 = matchTrackSound(priv2, {}, nullptr, opt);
        TEST("Mission4b P0-3: EXP15 unknown private banks 65/12/45 & 66/7/122 fail closed UNRESOLVED (score <= 65)",
             !mPriv1.selected.has_value() && !mPriv2.selected.has_value() &&
             (mPriv1.top_candidates.empty() || mPriv1.top_candidates[0].score <= 65) &&
             (mPriv2.top_candidates.empty() || mPriv2.top_candidates[0].score <= 65));

        // Phase 4 P4-M6 DEF-P4-002: RockPerc -> Standard Perc Kit (NEVER SFX Perc Kit GM2);
        // MegaDist & OvdGtr/LeadGtr -> Distortion Guitar GM2 (NEVER Strat Cln / Jazz Guitar).
        const auto rockPercProf = profileTrackTimbre(127, 0, 0, "RockPerc", TrackRole::Percussion, {}, {}, 8, kPercCh);
        const auto mRockPerc = matchTrackSound(rockPercProf, {}, nullptr, opt);
        TEST("P4-M6 DEF-P4-002: RockPerc (127/0/0 on kPercCh) maps to Standard Perc Kit (120/0/69, NEVER SFX Perc Kit GM2)",
             mRockPerc.selected.has_value() &&
             mRockPerc.selected->msb == 120 && mRockPerc.selected->lsb == 0 && mRockPerc.selected->pc == 69 &&
             mRockPerc.top_candidates[0].display_name == "Standard Perc Kit");

        const auto customPerc = makeProfile(TimbreFamily::PercussionKit, "Custom Rock Perc", kPercCh, 90);
        const auto mCustomPerc = matchTrackSound(customPerc, {}, nullptr, opt);
        TEST("P4-M6 DEF-P4-002: non-SFX/non-Ethnic PercussionKit tie resolves to Standard Perc Kit (120/0/69, NEVER SFX 56 or Ethnic 64)",
             mCustomPerc.selected.has_value() &&
             mCustomPerc.selected->msb == 120 && mCustomPerc.selected->lsb == 0 && mCustomPerc.selected->pc == 69 &&
             mCustomPerc.top_candidates[0].display_name == "Standard Perc Kit");

        const auto revoRockPerc = profileTrackTimbre(62, 100, 0, "RockPerc", TrackRole::Percussion, {}, {}, 8, kPercCh);
        const auto mRevoRockPerc = matchTrackSound(revoRockPerc, {}, nullptr, opt);
        TEST("P4-M6 DEF-P4-002: Revo! 62/100/0 RockPerc on kPercCh resolves to Standard Perc Kit (120/0/69, NEVER SFX 56 or Ethnic 64)",
             revoRockPerc.inferred.family == TimbreFamily::PercussionKit &&
             mRevoRockPerc.selected.has_value() &&
             mRevoRockPerc.selected->msb == 120 && mRevoRockPerc.selected->lsb == 0 && mRevoRockPerc.selected->pc == 69 &&
             mRevoRockPerc.top_candidates[0].display_name == "Standard Perc Kit");

        const auto megaDistProf = profileTrackTimbre(63, 120, 30, "MegaDist", TrackRole::Chord, {}, {}, 12, 12);
        const auto mMegaDist = matchTrackSound(megaDistProf, {}, nullptr, opt);
        TEST("P4-M6 DEF-P4-002: MegaDist (63/120/30) maps to Distortion Guitar GM2 (121/0/30, NEVER Strat Cln DNC)",
             mMegaDist.selected.has_value() &&
             mMegaDist.selected->msb == 121 && mMegaDist.selected->lsb == 0 && mMegaDist.selected->pc == 30 &&
             mMegaDist.top_candidates[0].display_name == "Distortion Guitar GM2");

        const auto ovdGtrProf = profileTrackTimbre(0, 0, 29, "OvdGtr", TrackRole::Accompaniment, {}, {}, 14, 14);
        const auto mOvdGtr = matchTrackSound(ovdGtrProf, {}, nullptr, opt);
        TEST("P4-M6 DEF-P4-002: OvdGtr/LeadGtr (0/0/29) maps to Distortion Guitar GM2 (121/0/30, NEVER Jazz Guitar)",
             mOvdGtr.selected.has_value() &&
             mOvdGtr.selected->msb == 121 && mOvdGtr.selected->lsb == 0 && mOvdGtr.selected->pc == 30 &&
             mOvdGtr.top_candidates[0].display_name == "Distortion Guitar GM2");
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
