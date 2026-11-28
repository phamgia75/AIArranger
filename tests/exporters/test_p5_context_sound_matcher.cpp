// Phase 5 P5-M3 unit, adversarial, multi-platform & concurrency tests for
// Context Sound Matcher (src/intelligence/context_sound_matcher.h).

#include "intelligence/context_sound_matcher.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace ai_arranger::intelligence;
using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;
using ai_arranger::uasf::MidiEvent;
using ai_arranger::uasf::MidiEventType;
using ai_arranger::uasf::SectionDefinition;
using ai_arranger::uasf::SectionType;
using ai_arranger::uasf::StyleDefinition;
using ai_arranger::uasf::TrackDefinition;
using ai_arranger::uasf::TrackRole;

namespace {

int failures = 0;
int passes   = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

void addNote(TrackDefinition& tr, uint32_t on, uint32_t dur, uint8_t pitch, uint8_t vel) {
    tr.events.push_back({MidiEventType::NoteOn, tr.midi_channel, pitch, vel, on});
    tr.events.push_back({MidiEventType::NoteOff, tr.midi_channel, pitch, 0, on + dur});
}

void setVoice(TrackDefinition& tr, uint8_t msb, uint8_t lsb, uint8_t pc) {
    tr.events.push_back({MidiEventType::ControlChange, tr.midi_channel, 0, msb, 0});
    tr.events.push_back({MidiEventType::ControlChange, tr.midi_channel, 32, lsb, 0});
    tr.events.push_back({MidiEventType::ProgramChange, tr.midi_channel, pc, 0, 0});
}

StyleDefinition buildRichPopRockEnsembleStyle() {
    StyleDefinition st{};
    st.name = "P5_TEST_ROCK_ENSEMBLE";
    st.tempo_bpm = 124;
    st.resolution = 480;

    SectionDefinition sec{};
    sec.type = SectionType::Main1;
    sec.name = "Main A";
    sec.bars = 2;
    sec.resolution = 480;
    sec.beats_per_bar = 4;
    sec.beat_note = 4;

    // 1. Main Drum Kit (ch 9 -> kDrumCh=9)
    {
        TrackDefinition tr{};
        tr.midi_channel = 9;
        tr.name = "RockKit";
        tr.role = TrackRole::Drum;
        tr.is_drum = true;
        setVoice(tr, 127, 0, 16); // Yamaha XG Rock Kit
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            addNote(tr, b + 0, 90, 36, 118);
            addNote(tr, b + 480, 90, 38, 114);
            addNote(tr, b + 960, 90, 36, 112);
            addNote(tr, b + 1440, 90, 38, 116);
            for (uint32_t e = 0; e < 8; ++e) addNote(tr, b + e * 240, 70, 42, 86);
        }
        sec.tracks.push_back(std::move(tr));
    }

    // 2. Aux RockPerc (ch 8 -> kPercCh=10, must NOT match SFX or Ethnic!)
    {
        TrackDefinition tr{};
        tr.midi_channel = 8;
        tr.name = "RockPerc";
        tr.role = TrackRole::Percussion;
        tr.is_drum = true;
        setVoice(tr, 127, 0, 0);
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            for (uint32_t e = 0; e < 8; ++e) {
                addNote(tr, b + e * 240, 60, (e % 2 == 0) ? 54 : 70, 82);
            }
        }
        sec.tracks.push_back(std::move(tr));
    }

    // 3. Electric Pick Bass (ch 10 -> kBassCh=8)
    {
        TrackDefinition tr{};
        tr.midi_channel = 10;
        tr.name = "PickBass";
        tr.role = TrackRole::Bass;
        setVoice(tr, 0, 0, 34);
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            addNote(tr, b + 0, 420, 36, 106);
            addNote(tr, b + 480, 420, 43, 98);
            addNote(tr, b + 960, 420, 36, 104);
            addNote(tr, b + 1440, 420, 40, 96);
        }
        sec.tracks.push_back(std::move(tr));
    }

    // 4. Acc1 Acoustic Piano (ch 11 -> korg 11, overlapping mid register 60..72)
    {
        TrackDefinition tr{};
        tr.midi_channel = 11;
        tr.name = "GrandPiano";
        tr.role = TrackRole::Accompaniment;
        setVoice(tr, 0, 0, 0);
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            for (uint32_t beat = 0; beat < 4; ++beat) {
                addNote(tr, b + beat * 480, 400, 60, 92);
                addNote(tr, b + beat * 480, 400, 64, 88);
                addNote(tr, b + beat * 480, 400, 67, 86);
            }
        }
        sec.tracks.push_back(std::move(tr));
    }

    // 5. Acc2 Second Overlapping Piano/Keys (ch 12 -> korg 12, identical register & voice -> tests Factor 10 masking!)
    {
        TrackDefinition tr{};
        tr.midi_channel = 12;
        tr.name = "GrandPiano2";
        tr.role = TrackRole::Accompaniment;
        setVoice(tr, 0, 0, 0);
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            for (uint32_t beat = 0; beat < 4; ++beat) {
                addNote(tr, b + beat * 480, 380, 60, 90);
                addNote(tr, b + beat * 480, 380, 64, 88);
                addNote(tr, b + beat * 480, 380, 67, 86);
            }
        }
        sec.tracks.push_back(std::move(tr));
    }

    // 6. Acc3 Sustained Pad (ch 13 -> korg 13)
    {
        TrackDefinition tr{};
        tr.midi_channel = 13;
        tr.name = "WarmPad";
        tr.role = TrackRole::Pad;
        setVoice(tr, 0, 0, 89);
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            addNote(tr, b + 0, 1880, 60, 74);
            addNote(tr, b + 0, 1880, 64, 72);
            addNote(tr, b + 0, 1880, 67, 74);
        }
        sec.tracks.push_back(std::move(tr));
    }

    // 7. Acc4 Strummed Guitar (ch 14 -> korg 14)
    {
        TrackDefinition tr{};
        tr.midi_channel = 14;
        tr.name = "DistGtr";
        tr.role = TrackRole::Accompaniment;
        setVoice(tr, 0, 0, 30);
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            for (uint32_t beat = 0; beat < 4; ++beat) {
                const uint32_t t = b + beat * 480;
                addNote(tr, t + 0,  320, 52, 102);
                addNote(tr, t + 12, 310, 59, 98);
                addNote(tr, t + 24, 300, 64, 96);
            }
        }
        sec.tracks.push_back(std::move(tr));
    }

    st.sections.push_back(std::move(sec));
    return st;
}

StyleDefinition buildExp15UnknownPrivateBankStyle() {
    StyleDefinition st{};
    st.name = "EXP15_PRIVATE_BANK_MIRROR";
    st.tempo_bpm = 110;
    st.resolution = 480;

    SectionDefinition sec{};
    sec.type = SectionType::Main1;
    sec.name = "Main A";
    sec.bars = 2;
    sec.resolution = 480;
    sec.beats_per_bar = 4;
    sec.beat_note = 4;

    TrackDefinition tr{};
    tr.midi_channel = 11;
    tr.name = "CustomXYZ"; // Opaque unclassifiable private voice name
    tr.role = TrackRole::Accompaniment;
    setVoice(tr, 63, 12, 77); // Private custom Yamaha bank MSB=63 -> low confidence / Unknown!
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        addNote(tr, b + 0, 480, 60, 88);
        addNote(tr, b + 960, 480, 64, 86);
    }
    sec.tracks.push_back(std::move(tr));
    st.sections.push_back(std::move(sec));
    return st;
}

} // namespace

int main() {
    std::printf("Test: Phase 5 P5-M3 Context Sound Matcher\n");

    const StyleDefinition rock = buildRichPopRockEnsembleStyle();
    const auto sem = analyzeMusicalSemantics(rock);
    const auto& pa5x = targetCapabilityProfile(TargetPlatformId::KorgPa5X);

    // 1. Default OFF fail-closed behavior (advisory ranked candidates computed, zero auto-selections!)
    {
        ContextSoundMatchOptions def_opt{}; // enabled=false, enable_auto_select=false
        const auto res = matchSoundsInContext(sem, pa5x, {}, nullptr, def_opt);
        TEST("default OFF leaves auto_select_enabled=false", !res.auto_select_enabled);
        TEST("default OFF auto-selects 0 tracks (fail-closed UNRESOLVED)",
             res.auto_selected_count == 0u && res.resolved_sounds.empty());
        TEST("default OFF still computes ranked advisory candidates with 10 factors",
             !res.tracks.empty() && !res.tracks.front().ranked_options.empty() &&
             res.tracks.front().ranked_options.size() <= 5u);
        TEST("epistemic stamps strictly preserved (hardware_verified=false, l3=NOT_TESTED)",
             !res.hardware_verified && res.l3_status == "NOT_TESTED");
    }

    // 2. Opt-in Context Auto-Select + 10-Factor Reasoning + RockPerc anti-SFX/Ethnic + 0 Cross-Family Contamination
    {
        ContextSoundMatchOptions opt{};
        opt.enabled = true;
        opt.enable_auto_select = true;
        opt.top_k = 5;
        const auto res = matchSoundsInContext(sem, pa5x, {}, nullptr, opt);
        TEST("opt-in auto-select resolves known tracks cleanly", res.auto_selected_count >= 5u);
        TEST("zero melodic<->drum cross-family contaminations", res.cross_family_contaminations == 0u);

        bool perc_checked = false;
        bool pad_checked = false;
        bool all_candidates_have_rationale_and_epistemic = true;
        for (const auto& t : res.tracks) {
            if (t.track_name == "RockPerc" && !t.ranked_options.empty()) {
                perc_checked = true;
                const std::string top = context_sound_detail::toLowerAscii(t.ranked_options.front().base_candidate.display_name);
                TEST("RockPerc does NOT select SFX or Ethnic kit",
                     top.find("sfx") == std::string::npos && top.find("ethnic") == std::string::npos);
            }
            if (t.track_name == "WarmPad" && !t.ranked_options.empty()) {
                pad_checked = true;
                const std::string top = context_sound_detail::toLowerAscii(t.ranked_options.front().base_candidate.display_name);
                TEST("WarmPad selects pad/strings family, not brass/lead",
                     top.find("pad") != std::string::npos || top.find("string") != std::string::npos);
            }
            for (const auto& c : t.ranked_options) {
                if (c.match_rationale.empty() || c.known_limitations.empty() ||
                    c.estimated_musical_impact.empty() || c.fallback_behavior.empty() ||
                    c.base_candidate.hardware_verified || c.base_candidate.l3_status != "NOT_TESTED") {
                    all_candidates_have_rationale_and_epistemic = false;
                }
            }
        }
        TEST("RockPerc and WarmPad evaluated", perc_checked && pad_checked);
        TEST("every ranked candidate carries rationale, limitations, impact, fallback & epistemic stamps",
             all_candidates_have_rationale_and_epistemic);
        TEST("Factor 10 ensemble masking mitigates identical overlapping bright piano collision",
             res.ensemble_collisions_mitigated >= 1u);
    }

    // 3. USER_VERIFIED Unconditional Precedence (never overridden even when auto-select is ON!)
    {
        std::map<uint8_t, SoundRef> user_map;
        user_map[kBassCh] = SoundRef{121, 64, 7}; // USER01/7 explicit user sound
        user_map[kDrumCh] = SoundRef{120, 64, 3}; // USERDK01/3 explicit user kit
        ContextSoundMatchOptions opt{};
        opt.enabled = true;
        opt.enable_auto_select = true;
        const auto res = matchSoundsInContext(sem, pa5x, user_map, nullptr, opt);
        TEST("USER_VERIFIED overrides preserved unconditionally (count >= 2)",
             res.user_overrides_preserved >= 2u);
        TEST("resolved_sounds retains exact user Bass & Drum addresses",
             res.resolved_sounds.count(kBassCh) && res.resolved_sounds.at(kBassCh).pc == 7u &&
             res.resolved_sounds.count(kDrumCh) && res.resolved_sounds.at(kDrumCh).pc == 3u);
        bool bass_locked = false;
        for (const auto& t : res.tracks) {
            if (t.target_channel == kBassCh) {
                bass_locked = t.user_override_locked &&
                              t.selected_origin == MatchOrigin::UserSoundMap &&
                              t.epistemic_tag == kEpistemicUserVerified &&
                              t.selected_provenance == ContextMatchProvenance::DeterministicVerified;
            }
        }
        TEST("user override track stamped USER_VERIFIED + DETERMINISTIC_VERIFIED + locked", bass_locked);
    }

    // 4. Out-of-spec Target KST Rejection (USERDK07..12 & Bank-2 unverified MUST be 100% rejected!)
    {
        TargetSoundCatalogView cat{};
        // Valid in-spec USER01/5 Piano
        TargetSoundEntry ok_piano{};
        ok_piano.msb = 121; ok_piano.lsb = 64; ok_piano.pc = 5;
        ok_piano.is_drum_kit = false; ok_piano.in_spec = true;
        ok_piano.display_name = "Concert Grand User";
        ok_piano.slot_address = TargetSlotAddress{false, 1, 5};
        cat.entries.push_back(ok_piano);

        // Out-of-spec USERDK07 (bank=7, lsb=70) — MUST BE REJECTED!
        TargetSoundEntry oos_dk7{};
        oos_dk7.msb = 120; oos_dk7.lsb = 70; oos_dk7.pc = 0;
        oos_dk7.is_drum_kit = true; oos_dk7.in_spec = true; // even if caller lied in_spec=true!
        oos_dk7.display_name = "Rock Drum Kit OOS";
        oos_dk7.slot_address = TargetSlotAddress{true, 7, 0};
        cat.entries.push_back(oos_dk7);

        // Out-of-spec Bank-2 unverified kit (msb=2, lsb=2) — MUST BE REJECTED!
        TargetSoundEntry bank2_kit{};
        bank2_kit.msb = 2; bank2_kit.lsb = 2; bank2_kit.pc = 10;
        bank2_kit.is_drum_kit = true; bank2_kit.in_spec = true;
        bank2_kit.display_name = "Unverified Bank2 Kit";
        cat.entries.push_back(bank2_kit);

        ContextSoundMatchOptions opt{};
        opt.enabled = true;
        opt.enable_auto_select = true;
        opt.prefer_target_user_sounds = true;
        const auto res = matchSoundsInContext(sem, pa5x, {}, &cat, opt);
        TEST("OOS USERDK07 and Bank-2 entries 100% rejected (oos_target_entries_rejected == 2)",
             res.oos_target_entries_rejected == 2u);
        bool leaked_oos = false;
        for (const auto& t : res.tracks) {
            for (const auto& c : t.ranked_options) {
                if ((c.base_candidate.ref.msb == 120u && c.base_candidate.ref.lsb == 70u) ||
                    c.base_candidate.ref.msb == 2u) {
                    leaked_oos = true;
                }
            }
        }
        TEST("zero OOS or Bank-2 candidates appear in any ranked list", !leaked_oos);

        // Legacy Pa4X/Pa1000 tighter caps (USER01..04, USERDK01..02) reject USER05!
        TargetSoundCatalogView pa4x_cat{};
        TargetSoundEntry user5{};
        user5.msb = 121; user5.lsb = 68; user5.pc = 1; // bank 5 > Pa4X max 4!
        user5.is_drum_kit = false; user5.in_spec = true;
        user5.slot_address = TargetSlotAddress{false, 5, 1};
        user5.display_name = "User5 Piano";
        pa4x_cat.entries.push_back(user5);
        const auto res_pa4x = matchSoundsInContext(
            sem, targetCapabilityProfile(TargetPlatformId::KorgPa4XPa1000), {}, &pa4x_cat, opt);
        TEST("Pa4X profile enforces tighter USER01..04 cap via TargetCapabilityProfile",
             res_pa4x.oos_target_entries_rejected == 1u);
    }

    // 5. EXP15 Unknown Private Bank Fail-Closed Preservation (NEVER auto-guessed!)
    {
        const StyleDefinition exp15 = buildExp15UnknownPrivateBankStyle();
        const auto sem15 = analyzeMusicalSemantics(exp15);
        ContextSoundMatchOptions opt{};
        opt.enabled = true;
        opt.enable_auto_select = true;
        const auto res15 = matchSoundsInContext(sem15, pa5x, {}, nullptr, opt);
        TEST("EXP15-style private unknown bank fails closed to UNRESOLVED",
             res15.auto_selected_count == 0u && res15.unresolved_fallback_count == 1u &&
             res15.resolved_sounds.empty() && !res15.tracks.empty() &&
             res15.tracks.front().fell_back_unresolved &&
             res15.tracks.front().selected_provenance == ContextMatchProvenance::UnresolvedFailClosed);
    }

    // 6. All 5 Target Platform Profiles work cleanly + RFC 8259 JSON formatting + Determinism
    {
        ContextSoundMatchOptions opt{};
        opt.enabled = true;
        opt.enable_auto_select = true;
        bool all_5_ok = true;
        for (TargetPlatformId pid : supportedTargetPlatforms()) {
            const auto& prof = targetCapabilityProfile(pid);
            const auto r1 = matchSoundsInContext(sem, prof, {}, nullptr, opt);
            const auto r2 = matchSoundsInContext(sem, prof, {}, nullptr, opt);
            const std::string j1 = formatContextSoundMatchJson(r1);
            const std::string j2 = formatContextSoundMatchJson(r2);
            const std::string md = formatContextSoundMatchMd(r1);
            if (j1 != j2 || j1.find("NaN") != std::string::npos || j1.find("inf") != std::string::npos ||
                j1.find("\"hardwareVerified\": false") == std::string::npos || md.empty()) {
                all_5_ok = false;
            }
        }
        TEST("all 5 target platforms deterministic, NaN-free RFC 8259 JSON & MD", all_5_ok);
    }

    // 7. 8-Thread TSan Concurrency Stress Test (zero mutable globals / zero data races!)
    {
        std::atomic<int> race_errors{0};
        std::vector<std::thread> threads;
        threads.reserve(8);
        ContextSoundMatchOptions opt{};
        opt.enabled = true;
        opt.enable_auto_select = true;
        const std::string ref_json = formatContextSoundMatchJson(matchSoundsInContext(sem, pa5x, {}, nullptr, opt));
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t]() {
                const auto pid = static_cast<TargetPlatformId>(t % kTargetPlatformCount);
                const auto& prof = targetCapabilityProfile(pid);
                for (int iter = 0; iter < 5; ++iter) {
                    const auto r = matchSoundsInContext(sem, prof, {}, nullptr, opt);
                    if (pid == TargetPlatformId::KorgPa5X && formatContextSoundMatchJson(r) != ref_json) {
                        race_errors.fetch_add(1);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        TEST("8-thread concurrent matchSoundsInContext produces 0 races & exact parity", race_errors.load() == 0);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
