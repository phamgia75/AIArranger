// Phase 5 P5-M5 unit, 4-layer evaluation, 6-defect negative control,
// AI-Rejection Gate & concurrency tests for Quality Intelligence
// (src/intelligence/quality_intelligence.h).

#include "intelligence/quality_intelligence.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace ai_arranger::intelligence;
using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;
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

void addKorgNote(Element& el, uint8_t ch, uint32_t on, uint32_t off, uint8_t pitch, uint8_t vel) {
    el.events.push_back(Event{on, static_cast<uint8_t>(0x90u | ch), pitch, vel});
    el.events.push_back(Event{off, static_cast<uint8_t>(0x80u | ch), pitch, 0u});
}

struct CleanBaseline {
    Style style;
    Report report;
    StyleDefinition uasf;
};

CleanBaseline buildCleanStyle() {
    CleanBaseline b{};
    b.style.ppqn = 384;
    b.style.mpqn = 500000;
    b.uasf.name = "P5_QI_CLEAN_STYLE";
    b.uasf.tempo_bpm = 120;
    b.uasf.resolution = 480;

    auto addVar = [&](const std::string& marker, SectionType st, uint8_t vel_base) {
        Element el{};
        el.marker = marker;
        el.num = 4;
        el.den = 4;
        el.length = 384 * 4 * 2;

        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kBassCh), 0, 121});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xC0u | kBassCh), 33, 0});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kBassCh), 7, 100});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kBassCh), 10, 64});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kDrumCh), 0, 120});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xC0u | kDrumCh), 100, 0});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kAcc1Ch), 0, 121});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xC0u | kAcc1Ch), 0, 0});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kAcc1Ch), 10, 48});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kAcc1Ch), 91, 40});

        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t base = bar * 1536;
            addKorgNote(el, kDrumCh, base + 0, base + 90, 36, static_cast<uint8_t>(vel_base + 14));
            addKorgNote(el, kDrumCh, base + 384, base + 474, 38, static_cast<uint8_t>(vel_base + 8));
            addKorgNote(el, kDrumCh, base + 768, base + 858, 36, static_cast<uint8_t>(vel_base + 12));
            addKorgNote(el, kDrumCh, base + 1152, base + 1242, 38, static_cast<uint8_t>(vel_base + 10));
            for (uint32_t e = 0; e < 8; ++e) {
                addKorgNote(el, kDrumCh, base + e * 192, base + e * 192 + 60, 42,
                            static_cast<uint8_t>(vel_base - (e % 2 == 0 ? 4 : 18)));
            }
            addKorgNote(el, kBassCh, base + 0, base + 350, 36, static_cast<uint8_t>(vel_base + 12));
            addKorgNote(el, kBassCh, base + 384, base + 730, 43, static_cast<uint8_t>(vel_base + 4));
            addKorgNote(el, kBassCh, base + 768, base + 1110, 36, static_cast<uint8_t>(vel_base + 10));
            addKorgNote(el, kBassCh, base + 1152, base + 1500, 40, static_cast<uint8_t>(vel_base + 2));

            addKorgNote(el, kAcc1Ch, base + 0, base + 700, 60, vel_base);
            addKorgNote(el, kAcc1Ch, base + 0, base + 700, 64, static_cast<uint8_t>(vel_base - 4));
            addKorgNote(el, kAcc1Ch, base + 0, base + 700, 67, static_cast<uint8_t>(vel_base - 6));
            addKorgNote(el, kAcc1Ch, base + 768, base + 1450, 60, static_cast<uint8_t>(vel_base - 2));
            addKorgNote(el, kAcc1Ch, base + 768, base + 1450, 64, static_cast<uint8_t>(vel_base - 5));
            addKorgNote(el, kAcc1Ch, base + 768, base + 1450, 67, static_cast<uint8_t>(vel_base - 8));
        }

        reorch_detail::canonicalSortEvents(el.events);
        b.style.elements.push_back(std::move(el));

        SectionDefinition sec{};
        sec.type = st;
        sec.name = marker;
        sec.bars = 2;
        sec.resolution = 480;
        sec.beats_per_bar = 4;
        sec.beat_note = 4;
        b.uasf.sections.push_back(std::move(sec));
    };

    addVar("v1cv1", SectionType::Main1, 78);
    addVar("v2cv1", SectionType::Main2, 86);
    addVar("v3cv1", SectionType::Main3, 94);
    addVar("v4cv1", SectionType::Main4, 102);

    b.report.tracks.push_back(TrackRow{9, kDrumCh, "PopKit", TrackRole::Drum, 0, 0, 127, 0, 0, "DrumKit", "Pop Kit", 0, 0, 0, false, "", "EXACT", 96, "Pop Kit", 120, 0, 100, "DOCUMENTED_NOT_HW_VERIFIED"});
    b.report.tracks.push_back(TrackRow{10, kBassCh, "ElecBass", TrackRole::Bass, 0, 0, 0, 0, 33, "ElectricBass", "JBass", 0, 0, 0, false, "", "EXACT", 95, "JBass", 121, 29, 33, "DOCUMENTED_NOT_HW_VERIFIED"});
    b.report.tracks.push_back(TrackRow{11, kAcc1Ch, "GrandPiano", TrackRole::Accompaniment, 0, 0, 0, 0, 0, "Piano", "Austrian Grand", 0, 0, 0, false, "", "EXACT", 95, "Austrian Grand", 121, 20, 0, "DOCUMENTED_NOT_HW_VERIFIED"});
    return b;
}

} // namespace

int main() {
    std::printf("Test: Phase 5 P5-M5 4-Layer Quality Intelligence & AI-Rejection Gate\n");

    const CleanBaseline clean = buildCleanStyle();
    const auto& pa5x = targetCapabilityProfile(TargetPlatformId::KorgPa5X);

    // 1. Clean 4-Layer Evaluation + Epistemic Lock (Layer 4 STRICTLY NOT_TESTED!)
    {
        const auto qi = evaluateQualityIntelligence(clean.style, clean.report, pa5x, ArrangementMode::ModeA_Faithful);
        TEST("Layer 1 Technical Integrity PASS on clean style",
             qi.layer1_technical.passed && qi.layer1_technical.score_0_100 == 100.0);
        TEST("Layer 2 Musical Integrity decompressed score in valid range (not ceiling-pinned)",
             qi.layer2_musical.decompressed_musical_score > 60.0 &&
             qi.layer2_musical.decompressed_musical_score <= 100.0);
        TEST("Layer 3 Target Suitability populated for Pa5X",
             qi.layer3_target.score_0_100 > 70.0 && qi.layer3_target.capability_cap_violations == 0u);
        TEST("Layer 4 Human Preference strictly NOT_TESTED, hardwareVerified=false, Grade U",
             !qi.layer4_human.hardware_verified &&
             qi.layer4_human.l3_status == "NOT_TESTED" &&
             !qi.layer4_human.audible_proof_available &&
             qi.layer4_human.preference_hooks.status == "NOT_TESTED" &&
             qi.layer4_human.grade == AcceptanceGrade::U);
        TEST("anti-overfitting epistemic warning present in JSON & MD",
             qi.offline_score_is_not_audible_proof &&
             formatQualityIntelligenceJson(qi).find("\"offlineScoreIsNotAudibleProof\": true") != std::string::npos &&
             formatQualityIntelligenceMd(qi).find("NOT_TESTED") != std::string::npos);
    }

    // 2. 6/6 Negative Control Defect Probes MUST BE REJECTED by Layer 1 / Rejection Gate!
    {
        int defects_rejected = 0;

        // Probe 1 (DEF-P4-010): Same-tick CC11 placed AFTER NoteOn at tick 0 (while keeping ticks sorted!)
        {
            ReorchestrateResult bad{};
            bad.ok = true;
            bad.requested_mode = ArrangementMode::ModeB_IntelligentAdaptation;
            bad.effective_mode = ArrangementMode::ModeB_IntelligentAdaptation;
            bad.style = clean.style;
            auto& evs = bad.style.elements[0].events;
            size_t insert_pos = 0;
            while (insert_pos < evs.size() && evs[insert_pos].tick == 0u) ++insert_pos;
            evs.insert(evs.begin() + static_cast<ptrdiff_t>(insert_pos),
                       Event{0, static_cast<uint8_t>(0xB0u | kAcc1Ch), 11, 90});
            const auto dec = evaluateCandidateAcceptance(clean.style, clean.report, bad, pa5x);
            if (!dec.accepted && dec.fallback_to_faithful &&
                dec.candidate_report.layer1_technical.same_tick_cc_after_note_on > 0u) {
                ++defects_rejected;
            }
        }

        // Probe 2 (AGMI-R12): Loop overflow NoteOff at tick == element.length
        {
            ReorchestrateResult bad{};
            bad.ok = true;
            bad.requested_mode = ArrangementMode::ModeC_CreativeEnhancement;
            bad.effective_mode = ArrangementMode::ModeC_CreativeEnhancement;
            bad.style = clean.style;
            const uint32_t len = bad.style.elements[0].length;
            bad.style.elements[0].events.push_back(Event{len - 10u, static_cast<uint8_t>(0x90u | kAcc1Ch), 72, 90});
            bad.style.elements[0].events.push_back(Event{len, static_cast<uint8_t>(0x80u | kAcc1Ch), 72, 0});
            const auto dec = evaluateCandidateAcceptance(clean.style, clean.report, bad, pa5x);
            if (!dec.accepted && dec.fallback_to_faithful && !dec.candidate_report.layer1_technical.passed) {
                ++defects_rejected;
            }
        }

        // Probe 3: Hanging unclosed NoteOn
        {
            ReorchestrateResult bad{};
            bad.ok = true;
            bad.requested_mode = ArrangementMode::ModeC_CreativeEnhancement;
            bad.effective_mode = ArrangementMode::ModeC_CreativeEnhancement;
            bad.style = clean.style;
            bad.style.elements[0].events.push_back(Event{100u, static_cast<uint8_t>(0x90u | kAcc1Ch), 74, 88});
            const auto dec = evaluateCandidateAcceptance(clean.style, clean.report, bad, pa5x);
            if (!dec.accepted && dec.fallback_to_faithful &&
                dec.candidate_report.layer1_technical.hanging_note_violations > 0u) {
                ++defects_rejected;
            }
        }

        // Probe 4: Chord-event reserved range leak (pitch <= 11 on chord-driven track)
        {
            ReorchestrateResult bad{};
            bad.ok = true;
            bad.requested_mode = ArrangementMode::ModeC_CreativeEnhancement;
            bad.effective_mode = ArrangementMode::ModeC_CreativeEnhancement;
            bad.style = clean.style;
            addKorgNote(bad.style.elements[0], kAcc1Ch, 0, 192, 6, 90);
            reorch_detail::canonicalSortEvents(bad.style.elements[0].events);
            const auto dec = evaluateCandidateAcceptance(clean.style, clean.report, bad, pa5x);
            if (!dec.accepted && dec.fallback_to_faithful &&
                dec.candidate_report.layer1_technical.chord_band_leaks > 0u) {
                ++defects_rejected;
            }
        }

        // Probe 5: Locked-track mutation (Bass channel mutated while Bass is locked)
        {
            ReorchestrateResult bad{};
            bad.ok = true;
            bad.requested_mode = ArrangementMode::ModeB_IntelligentAdaptation;
            bad.effective_mode = ArrangementMode::ModeB_IntelligentAdaptation;
            bad.style = clean.style;
            addKorgNote(bad.style.elements[0], kBassCh, 192, 320, 48, 92);
            reorch_detail::canonicalSortEvents(bad.style.elements[0].events);
            const auto dec = evaluateCandidateAcceptance(
                clean.style, clean.report, bad, pa5x, {}, nullptr, {kBassCh});
            if (!dec.accepted && dec.fallback_to_faithful &&
                dec.candidate_report.layer1_technical.locked_track_mutations > 0u) {
                ++defects_rejected;
            }
        }

        // Probe 6: Source musical identity destroyed (Mode C candidate swaps entire harmonic content)
        {
            ReorchestrateResult bad{};
            bad.ok = true;
            bad.requested_mode = ArrangementMode::ModeC_CreativeEnhancement;
            bad.effective_mode = ArrangementMode::ModeC_CreativeEnhancement;
            bad.style = clean.style;
            for (auto& el : bad.style.elements) {
                el.events.erase(std::remove_if(el.events.begin(), el.events.end(), [](const Event& e) {
                    const uint8_t ch = e.status & 0x0Fu;
                    const uint8_t hi = e.status & 0xF0u;
                    return ch == kAcc1Ch && (hi == 0x80u || hi == 0x90u);
                }), el.events.end());
                addKorgNote(el, kAcc1Ch, 192, 320, 66, 90); // Single off-beat F# replacing full chords
                reorch_detail::canonicalSortEvents(el.events);
            }
            const auto dec = evaluateCandidateAcceptance(clean.style, clean.report, bad, pa5x);
            if (!dec.accepted && dec.fallback_to_faithful && !dec.rejection_reasons.empty()) {
                ++defects_rejected;
            }
        }

        TEST("6/6 negative control defect probes rejected by Layer 1 / AI-Rejection Gate",
             defects_rejected == 6);
    }

    // 3. Deterministic Mode A wins over no-op / noisy Mode B & C via selectBestCandidate()!
    {
        ReorchestrateResult noopB{};
        noopB.ok = true;
        noopB.requested_mode = ArrangementMode::ModeB_IntelligentAdaptation;
        noopB.effective_mode = ArrangementMode::ModeB_IntelligentAdaptation;
        noopB.style = clean.style;

        ReorchestrateResult noopC{};
        noopC.ok = true;
        noopC.requested_mode = ArrangementMode::ModeC_CreativeEnhancement;
        noopC.effective_mode = ArrangementMode::ModeC_CreativeEnhancement;
        noopC.style = clean.style;

        const auto sel = selectBestCandidate(clean.style, clean.report, noopB, noopC, pa5x);
        TEST("AI-Rejection Gate retains Deterministic Mode A when B/C produce 0 net gain",
             sel.deterministic_a_won &&
             sel.selected_mode == ArrangementMode::ModeA_Faithful &&
             !sel.mode_b_decision.accepted && !sel.mode_c_decision.accepted);
    }

    // 4. Mode B beats Mode A cleanly when resolving real register & collision defects!
    {
        CleanBaseline defective = clean;
        // Inject real sub-E1 Bass defect (pitch 24 = C1) into baseline
        for (auto& el : defective.style.elements) {
            for (auto& ev : el.events) {
                if ((ev.status & 0x0Fu) == kBassCh &&
                    ((ev.status & 0xF0u) == 0x90u || (ev.status & 0xF0u) == 0x80u) &&
                    ev.d1 == 36u) {
                    ev.d1 = 24u; // C1 < E1!
                }
            }
        }
        const auto sem = analyzeMusicalSemantics(defective.uasf, &defective.style, &defective.report);
        const auto bundle = generateModeAbcCandidates(defective.style, defective.report, sem, pa5x);
        const auto sel = selectBestCandidate(defective.style, defective.report, bundle.mode_b, bundle.mode_c, pa5x);
        TEST("Mode B accepted over defective Mode A when fixing real sub-E1 Bass register defect",
             sel.mode_b_decision.accepted && sel.mode_b_decision.musical_delta > 0.0 &&
             !sel.deterministic_a_won);
    }

    // 5. All 5 Target Profiles evaluated cleanly + 8-Thread TSan Stress Test
    {
        bool all_5_ok = true;
        for (TargetPlatformId pid : supportedTargetPlatforms()) {
            const auto qi = evaluateQualityIntelligence(clean.style, clean.report, targetCapabilityProfile(pid));
            const std::string j = formatQualityIntelligenceJson(qi);
            if (!qi.layer1_technical.passed || j.find("NaN") != std::string::npos ||
                j.find("\"hardwareVerified\": false") == std::string::npos) {
                all_5_ok = false;
            }
        }
        TEST("all 5 target platforms produce valid 4-Layer JSON with zero NaN", all_5_ok);

        std::atomic<int> errs{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 8; ++t) {
            ths.emplace_back([&]() {
                for (int k = 0; k < 4; ++k) {
                    const auto qi = evaluateQualityIntelligence(clean.style, clean.report, pa5x);
                    if (!qi.layer1_technical.passed || qi.layer4_human.hardware_verified) {
                        errs.fetch_add(1);
                    }
                }
            });
        }
        for (auto& th : ths) th.join();
        TEST("8-thread concurrent evaluateQualityIntelligence passes with 0 races", errs.load() == 0);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
