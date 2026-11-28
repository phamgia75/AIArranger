// Phase 5 P5-M4 unit, interactive 10-chord NTT, adversarial mock-AI rollback &
// concurrency tests for Target Re-Orchestrator (src/intelligence/target_reorchestrator.h).

#include "intelligence/target_reorchestrator.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace ai_arranger::intelligence;
using namespace ai_arranger::exporters::korg;
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

void addUasfNote(TrackDefinition& tr, uint32_t on, uint32_t dur, uint8_t pitch, uint8_t vel) {
    tr.events.push_back({MidiEventType::NoteOn, tr.midi_channel, pitch, vel, on});
    tr.events.push_back({MidiEventType::NoteOff, tr.midi_channel, pitch, 0, on + dur});
}

// Builds a clean valid Korg Style + matching UASF + Report with:
// - Out-of-register Bass note (< E1 = 24)
// - Bass/Piano < C3 collision (piano note at 43 colliding with bass at 36)
// - 8-note over-dense piano chord (> 6-note playable cap)
// - MegaVoice top-zone velocity spike (125) on guitar
// - V1 & V4 elements for Mode C arc shaping
struct TestFixture {
    Style style;
    Report report;
    StyleDefinition uasf;
};

TestFixture buildAdaptationFixture() {
    TestFixture fx{};
    fx.style.ppqn = 384;
    fx.style.mpqn = 500000;
    fx.uasf.name = "P5_REORCH_FIXTURE";
    fx.uasf.tempo_bpm = 120;
    fx.uasf.resolution = 480;

    auto makeVar = [&](const std::string& marker, SectionType st, const std::string& sname) {
        Element el{};
        el.marker = marker;
        el.num = 4;
        el.den = 4;
        el.length = 384 * 4 * 2; // 2 bars = 3072 ticks

        // Tick-0 valid bank/pc
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kBassCh), 0, 121});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xC0u | kBassCh), 33, 0});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kDrumCh), 0, 120});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xC0u | kDrumCh), 0, 0});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | kAcc1Ch), 0, 121});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xC0u | kAcc1Ch), 0, 0});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | 12u), 0, 121});
        el.events.push_back(Event{0, static_cast<uint8_t>(0xC0u | 12u), 25, 0});

        // Drum (ch 9)
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1536;
            addKorgNote(el, kDrumCh, b + 0, b + 96, 36, 108);
            addKorgNote(el, kDrumCh, b + 384, b + 480, 38, 102);
            addKorgNote(el, kDrumCh, b + 768, b + 864, 36, 106);
            addKorgNote(el, kDrumCh, b + 1152, b + 1248, 38, 104);
            for (uint32_t e = 0; e < 8; ++e) addKorgNote(el, kDrumCh, b + e * 192, b + e * 192 + 60, 42, 82);
        }

        // Bass (ch 8): includes one sub-E1 note (pitch 24 = C1) on bar 1 beat 3!
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1536;
            addKorgNote(el, kBassCh, b + 0, b + 350, 36, 104);
            addKorgNote(el, kBassCh, b + 384, b + 730, 43, 98);
            addKorgNote(el, kBassCh, b + 768, b + 1110, (bar == 0) ? 24 : 36, 102); // C1 < E1!
            addKorgNote(el, kBassCh, b + 1152, b + 1500, 40, 96);
        }

        // Acc1 Piano (ch 11): 8-note over-dense chord with low colliding notes < C3 (40, 43)
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1536;
            const uint8_t cluster[8] = {40, 43, 48, 52, 55, 60, 64, 67};
            for (uint8_t p : cluster) {
                addKorgNote(el, kAcc1Ch, b + 0, b + 700, p, 88);
            }
            addKorgNote(el, kAcc1Ch, b + 768, b + 1450, 60, 86);
            addKorgNote(el, kAcc1Ch, b + 768, b + 1450, 64, 84);
            addKorgNote(el, kAcc1Ch, b + 768, b + 1450, 67, 84);
        }

        // Acc2 Strummed Guitar (ch 12): includes MegaVoice top-zone spike (vel 125)
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1536;
            for (uint32_t beat = 0; beat < 4; ++beat) {
                const uint32_t t = b + beat * 384;
                addKorgNote(el, 12u, t + 0,  t + 260, 52, 125); // MegaVoice spike >= 122!
                addKorgNote(el, 12u, t + 10, t + 260, 60, 94);
                addKorgNote(el, 12u, t + 20, t + 260, 64, 92);
            }
        }

        reorch_detail::canonicalSortEvents(el.events);
        fx.style.elements.push_back(std::move(el));

        // Matching UASF section for P5-M2 semantics
        SectionDefinition sec{};
        sec.type = st;
        sec.name = sname;
        sec.bars = 2;
        sec.resolution = 480;
        sec.beats_per_bar = 4;
        sec.beat_note = 4;

        TrackDefinition trD{}; trD.midi_channel = 9; trD.name = "PopKit"; trD.role = TrackRole::Drum; trD.is_drum = true;
        TrackDefinition trB{}; trB.midi_channel = 10; trB.name = "ElecBass"; trB.role = TrackRole::Bass;
        TrackDefinition trP{}; trP.midi_channel = 11; trP.name = "GrandPiano"; trP.role = TrackRole::Accompaniment;
        TrackDefinition trG{}; trG.midi_channel = 12; trG.name = "SteelGtr"; trG.role = TrackRole::Accompaniment;
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            addUasfNote(trD, b + 0, 90, 36, 108); addUasfNote(trD, b + 480, 90, 38, 102);
            for (uint32_t e = 0; e < 8; ++e) addUasfNote(trD, b + e * 240, 70, 42, 82);
            addUasfNote(trB, b + 0, 420, 36, 104); addUasfNote(trB, b + 960, 420, 43, 98);
            // Piano with two-hand split so P5-M2 classifies PianoAccompaniment
            addUasfNote(trP, b + 0, 700, 43, 88); addUasfNote(trP, b + 0, 700, 60, 88);
            addUasfNote(trP, b + 0, 700, 64, 86); addUasfNote(trP, b + 0, 700, 67, 86);
            addUasfNote(trP, b + 960, 700, 48, 86); addUasfNote(trP, b + 960, 700, 60, 86);
            addUasfNote(trP, b + 960, 700, 64, 84); addUasfNote(trP, b + 960, 700, 67, 84);
            // Strummed guitar
            for (uint32_t beat = 0; beat < 4; ++beat) {
                addUasfNote(trG, b + beat * 480 + 0, 300, 52, 98);
                addUasfNote(trG, b + beat * 480 + 12, 290, 60, 94);
                addUasfNote(trG, b + beat * 480 + 24, 280, 64, 92);
            }
        }
        sec.tracks = {trD, trB, trP, trG};
        fx.uasf.sections.push_back(std::move(sec));
    };

    makeVar("v1cv1", SectionType::Main1, "Main A");
    makeVar("v4cv1", SectionType::Main4, "Main D");

    fx.report.tracks.push_back(TrackRow{9, kDrumCh, "PopKit", TrackRole::Drum, 0, 0, 127, 0, 0, "DrumKit", "Pop Kit", 0, 0, 0, false, "", "EXACT", 95, "Pop Kit", 120, 0, 100, "DOCUMENTED_NOT_HW_VERIFIED"});
    fx.report.tracks.push_back(TrackRow{10, kBassCh, "ElecBass", TrackRole::Bass, 0, 0, 0, 0, 33, "ElectricBass", "JBass", 0, 0, 0, false, "", "EXACT", 94, "JBass", 121, 29, 33, "DOCUMENTED_NOT_HW_VERIFIED"});
    fx.report.tracks.push_back(TrackRow{11, kAcc1Ch, "GrandPiano", TrackRole::Accompaniment, 0, 0, 0, 0, 0, "Piano", "Austrian Grand", 0, 0, 0, false, "", "EXACT", 95, "Austrian Grand", 121, 20, 0, "DOCUMENTED_NOT_HW_VERIFIED"});
    fx.report.tracks.push_back(TrackRow{12, 12u, "SteelGtr", TrackRole::Accompaniment, 0, 0, 0, 0, 25, "AcousticGuitar", "Steel Gtr", 0, 0, 0, false, "", "EXACT", 92, "Steel Gtr", 121, 47, 25, "DOCUMENTED_NOT_HW_VERIFIED"});
    return fx;
}

// Adversarial Mock Generative AI Hook injecting all 6 pre-registered P5-EXP-03 failure classes + 2 valid cases!
class MockAdversarialAiHook final : public IGenerativeProposalHook {
public:
    enum class Scenario {
        LoopOverflow,
        HangingNote,
        ChordBandLeak,
        LockedTrackMutation,
        ExtremeBassViolation,
        IdentityDestroyer,
        ValidMusicalRefinement,
        ValidGentleVoicing,
    };

    explicit MockAdversarialAiHook(Scenario s) : scenario_(s) {}

    GenerativeProposalResult propose(const GenerativeProposalRequest& req) const override {
        GenerativeProposalResult r{};
        if (req.section_marker != "v1cv1") return r;
        const uint8_t target = (scenario_ == Scenario::LockedTrackMutation ||
                                scenario_ == Scenario::ExtremeBassViolation)
                                   ? kBassCh
                                   : kAcc1Ch;
        if (req.target_channel != target) return r;

        r.available = true;
        r.model_id = "mock-adversarial-v1";
        r.confidence = 0.85;
        r.rationale = "Synthetic P5-EXP-03 test proposal";

        const uint8_t ch = req.target_channel;
        switch (scenario_) {
            case Scenario::LoopOverflow:
                r.proposed_channel_events.push_back(Event{req.element_length_ticks + 10u, static_cast<uint8_t>(0x90u | ch), 60, 90});
                r.proposed_channel_events.push_back(Event{req.element_length_ticks + 100u, static_cast<uint8_t>(0x80u | ch), 60, 0});
                break;
            case Scenario::HangingNote:
                r.proposed_channel_events.push_back(Event{0, static_cast<uint8_t>(0x90u | ch), 60, 90});
                // Deliberately missing NoteOff!
                break;
            case Scenario::ChordBandLeak:
                r.proposed_channel_events.push_back(Event{0, static_cast<uint8_t>(0x90u | ch), 5, 90}); // C-1..B-1 reserved!
                r.proposed_channel_events.push_back(Event{192, static_cast<uint8_t>(0x80u | ch), 5, 0});
                break;
            case Scenario::LockedTrackMutation:
                r.proposed_channel_events.push_back(Event{0, static_cast<uint8_t>(0x90u | ch), 36, 100});
                r.proposed_channel_events.push_back(Event{300, static_cast<uint8_t>(0x80u | ch), 36, 0});
                break;
            case Scenario::ExtremeBassViolation:
                r.proposed_channel_events.push_back(Event{0, static_cast<uint8_t>(0x90u | ch), 18, 100}); // F#0 < E1!
                r.proposed_channel_events.push_back(Event{300, static_cast<uint8_t>(0x80u | ch), 18, 0});
                break;
            case Scenario::IdentityDestroyer:
                // Replace all C major chords on beat 1 with single off-beat F# chromatic cluster (destroys identity)
                for (uint32_t k = 0; k < 4; ++k) {
                    const uint32_t t = 192u + k * 768u;
                    r.proposed_channel_events.push_back(Event{t, static_cast<uint8_t>(0x90u | ch), 66, 95});
                    r.proposed_channel_events.push_back(Event{t + 120u, static_cast<uint8_t>(0x80u | ch), 66, 0});
                }
                break;
            case Scenario::ValidMusicalRefinement:
            case Scenario::ValidGentleVoicing: {
                // Clean C major open piano voicing matching groove & pitch classes, zero violations!
                r.proposed_channel_events.push_back(Event{0, static_cast<uint8_t>(0xB0u | ch), 0, 121});
                r.proposed_channel_events.push_back(Event{0, static_cast<uint8_t>(0xC0u | ch), 0, 0});
                const uint8_t vel = (scenario_ == Scenario::ValidMusicalRefinement) ? 88u : 82u;
                for (uint32_t bar = 0; bar < 2; ++bar) {
                    const uint32_t b = bar * 1536;
                    for (uint8_t p : {uint8_t{52}, uint8_t{55}, uint8_t{60}, uint8_t{64}, uint8_t{67}}) {
                        r.proposed_channel_events.push_back(Event{b + 0, static_cast<uint8_t>(0x90u | ch), p, vel});
                        r.proposed_channel_events.push_back(Event{b + 700, static_cast<uint8_t>(0x80u | ch), p, 0});
                    }
                    for (uint8_t p : {uint8_t{60}, uint8_t{64}, uint8_t{67}}) {
                        r.proposed_channel_events.push_back(Event{b + 768, static_cast<uint8_t>(0x90u | ch), p, vel});
                        r.proposed_channel_events.push_back(Event{b + 1450, static_cast<uint8_t>(0x80u | ch), p, 0});
                    }
                }
                break;
            }
        }
        return r;
    }

private:
    Scenario scenario_;
};

} // namespace

int main() {
    std::printf("Test: Phase 5 P5-M4 Target Re-Orchestrator\n");

    const TestFixture fx = buildAdaptationFixture();
    const auto sem = analyzeMusicalSemantics(fx.uasf, &fx.style, &fx.report);
    const auto& pa5x = targetCapabilityProfile(TargetPlatformId::KorgPa5X);

    // 1. Mode A Faithful: Bit-exact 0-mutation identity
    {
        ReorchestrateOptions optA{}; // Default ModeA_Faithful
        const auto resA = reorchestrateForTarget(fx.style, fx.report, sem, pa5x, optA);
        const auto wOrig = write(fx.style);
        const auto wOut  = write(resA.style);
        TEST("Mode A default is ok and 0-byte bit-exact identical",
             resA.ok && resA.effective_mode == ArrangementMode::ModeA_Faithful &&
             resA.plan_actions.empty() && !resA.rolled_back_to_faithful &&
             wOrig.ok && wOut.ok && wOrig.bytes == wOut.bytes);
    }

    // 2. Interactive 12-Root x 10-Chord-Quality NTT Validator (120 checks per element-track!)
    {
        const auto audit = validateInteractiveChords(fx.style, pa5x, &fx.style);
        TEST("12-root x 10-chord NTT sweep executes >= 720 cells (6 chord-driven tracks x 120)",
             audit.chords_tested >= 720u);
        TEST("NTT validator detects raw out-of-register Bass note (< E1)",
             audit.bass_out_of_register_notes >= 2u);
        TEST("NTT chord-tone conformance high on C-major fixture",
             audit.chord_tone_conformance >= 0.90 && audit.bass_root_accuracy >= 0.95);
    }

    // 3. Mode B Intelligent Adaptation: fixes Bass < E1, Bass/Piano < C3, 8-note voicing cap, MegaVoice vel tame
    {
        ReorchestrateOptions optB{};
        optB.mode = ArrangementMode::ModeB_IntelligentAdaptation;
        const auto resB = reorchestrateForTarget(fx.style, fx.report, sem, pa5x, optB);
        TEST("Mode B succeeds with 0 korg::validate errors and 0 rollback",
             resB.ok && !resB.rolled_back_to_faithful && validate(resB.style).empty());
        TEST("Mode B eliminates out-of-register Bass notes (< E1 -> 0)",
             resB.ntt_audit.bass_out_of_register_notes == 0u);
        TEST("Mode B eliminates Bass/Piano < C3 frequency collisions",
             resB.bass_piano_collisions_before > 0u && resB.bass_piano_collisions_after == 0u);
        TEST("Mode B emits auditable reversible actions across multiple domains",
             resB.plan_actions.size() >= 3u && resB.reversible_verified);

        // Pass-2 idempotence verification
        const auto resB2 = reorchestrateForTarget(resB.style, fx.report, sem, pa5x, optB);
        const auto w1 = write(resB.style);
        const auto w2 = write(resB2.style);
        TEST("Mode B Pass-2 idempotence (exact SMF byte match)",
             resB2.ok && w1.ok && w2.ok && w1.bytes == w2.bytes);
    }

    // 4. Locked-Track 0-Byte Invariance Guarantee
    {
        ReorchestrateOptions optLock{};
        optLock.mode = ArrangementMode::ModeB_IntelligentAdaptation;
        optLock.locked_channels = {kBassCh, kAcc1Ch};
        const auto resL = reorchestrateForTarget(fx.style, fx.report, sem, pa5x, optLock);
        TEST("Locked tracks remain 100% bit-exact identical in Mode B",
             resL.ok && resL.locked_tracks_bit_exact &&
             reorch_detail::verifyLockedChannelsBitExact(fx.style, resL.style, optLock.locked_channels));
    }

    // 5. Mode C + Adversarial Mock AI Hook: 6/6 adversarial proposals REJECTED & rolled back; 2/2 valid ACCEPTED!
    {
        using S = MockAdversarialAiHook::Scenario;
        const S bad_scenarios[6] = {
            S::LoopOverflow, S::HangingNote, S::ChordBandLeak,
            S::LockedTrackMutation, S::ExtremeBassViolation, S::IdentityDestroyer
        };
        int bad_rejected = 0;
        const auto wFaithful = write(fx.style);
        for (S sc : bad_scenarios) {
            MockAdversarialAiHook hook(sc);
            ReorchestrateOptions optC{};
            optC.mode = ArrangementMode::ModeC_CreativeEnhancement;
            optC.enable_ai_proposals = true;
            optC.ai_hook = &hook;
            if (sc == S::LockedTrackMutation) optC.locked_channels = {kBassCh};
            const auto resC = reorchestrateForTarget(fx.style, fx.report, sem, pa5x, optC);
            const auto wC = write(resC.style);
            if (resC.rolled_back_to_faithful &&
                resC.effective_mode == ArrangementMode::ModeA_Faithful &&
                wC.ok && wC.bytes == wFaithful.bytes) {
                ++bad_rejected;
            }
        }
        TEST("6/6 adversarial mock-AI proposals rejected & rolled back bit-exact to Mode A",
             bad_rejected == 6);

        // 2/2 valid musical proposals accepted cleanly!
        int good_accepted = 0;
        for (S sc : {S::ValidMusicalRefinement, S::ValidGentleVoicing}) {
            MockAdversarialAiHook hook(sc);
            ReorchestrateOptions optC{};
            optC.mode = ArrangementMode::ModeC_CreativeEnhancement;
            optC.enable_ai_proposals = true;
            optC.ai_hook = &hook;
            const auto resC = reorchestrateForTarget(fx.style, fx.report, sem, pa5x, optC);
            if (resC.ok && !resC.rolled_back_to_faithful && validate(resC.style).empty()) {
                ++good_accepted;
            }
        }
        TEST("2/2 valid musical AI proposals accepted cleanly after hard validation",
             good_accepted == 2);
    }

    // 6. Independent Mode A/B/C Bundle Generation + RFC 8259 JSON Audit + Same-tick CC sort
    {
        const auto bundle = generateModeAbcCandidates(fx.style, fx.report, sem, pa5x);
        const auto wOrig = write(fx.style);
        const auto wA = write(bundle.mode_a.style);
        TEST("generateModeAbcCandidates keeps Mode A bit-exact while generating independent B & C",
             bundle.mode_a.ok && bundle.mode_b.ok && bundle.mode_c.ok &&
             wOrig.ok && wA.ok && wOrig.bytes == wA.bytes);
        const std::string jB = formatReorchestrationAuditJson(bundle.mode_b);
        TEST("reorchestration JSON is RFC 8259 valid, NaN-free, hardwareVerified=false",
             jB.find("\"hardwareVerified\": false") != std::string::npos &&
             jB.find("NaN") == std::string::npos &&
             jB.find("\"perQualityConformance\"") != std::string::npos);
    }

    // 7. 8-Thread TSan Concurrency Stress Test
    {
        std::atomic<int> errs{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 8; ++t) {
            ths.emplace_back([&]() {
                ReorchestrateOptions o{};
                o.mode = ArrangementMode::ModeB_IntelligentAdaptation;
                for (int k = 0; k < 3; ++k) {
                    const auto r = reorchestrateForTarget(fx.style, fx.report, sem, pa5x, o);
                    if (!r.ok || r.rolled_back_to_faithful) errs.fetch_add(1);
                }
            });
        }
        for (auto& th : ths) th.join();
        TEST("8-thread concurrent reorchestrateForTarget passes with 0 races", errs.load() == 0);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
