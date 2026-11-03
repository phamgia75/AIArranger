#include "engine/uasf/validator.h"
#include "engine/arranger/style_player.h"
#include <cstdio>

using namespace ai_arranger::uasf;
using namespace ai_arranger::arranger;

static int failures = 0;
static int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  ❌ FAIL: %s\n", name); failures++; } \
    else { std::printf("  ✅ PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: UASF Validator\n");

    UasfValidator validator;

    // ── Valid demo style ───────────────────────────────────────────
    auto style = buildDemoStyle();
    style.tempo_bpm = 120;
    style.resolution = 480;
    style.name = "Test";

    auto r1 = validator.validate(style);
    TEST("Valid style passes", r1.valid);

    // ── No name ────────────────────────────────────────────────────
    auto noName = style;
    noName.name = "";
    auto r2 = validator.validate(noName);
    TEST("No name — still valid (warning only)", r2.valid);

    // ── Zero tempo ─────────────────────────────────────────────────
    auto badTempo = style;
    badTempo.tempo_bpm = 0;
    auto r3 = validator.validate(badTempo);
    TEST("Zero tempo — invalid", !r3.valid);

    // ── Zero resolution ────────────────────────────────────────────
    auto badRes = style;
    badRes.resolution = 0;
    auto r4 = validator.validate(badRes);
    TEST("Zero resolution — invalid", !r4.valid);

    // ── Real Genos resolution (1920) is valid; above it is not ─────
    {
        auto genos = style; genos.resolution = 1920;
        TEST("1920 PPQN (Genos source resolution) — valid", validator.validate(genos).valid);
        auto tooHigh = style; tooHigh.resolution = 1921;
        TEST("1921 PPQN — invalid", !validator.validate(tooHigh).valid);
    }

    // ── Empty sections ─────────────────────────────────────────────
    auto emptyStyle = style;
    emptyStyle.sections.clear();
    auto r5 = validator.validate(emptyStyle);
    TEST("No sections — invalid", !r5.valid);

    // ── MegaVoice low fidelity warning ─────────────────────────────
    auto mvStyle = style;
    if (!mvStyle.sections.empty() && !mvStyle.sections[0].tracks.empty()) {
        auto& at = mvStyle.sections[0].tracks[0].articulation;
        at.profile = ArticulationProfile::MegaLike;
        at.fidelity = FidelityRequirement::Low;
        auto r6 = validator.validate(mvStyle);
        bool hasMegaWarning = false;
        for (const auto& issue : r6.issues) {
            if (issue.message.find("MegaLike") != std::string::npos) {
                hasMegaWarning = true;
                break;
            }
        }
        TEST("MegaVoice low fidelity triggers warning", hasMegaWarning);
    }

    // ── Invalid MIDI channel ──────────────────────────────────────
    auto badChan = style;
    if (!badChan.sections.empty() && !badChan.sections[0].tracks.empty()) {
        badChan.sections[0].tracks[0].midi_channel = 16;
        auto r7 = validator.validate(badChan);
        bool hasChanError = false;
        for (const auto& issue : r7.issues) {
            if (issue.message.find("MIDI channel") != std::string::npos) {
                hasChanError = true;
                break;
            }
        }
        TEST("Invalid channel — error", hasChanError);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
