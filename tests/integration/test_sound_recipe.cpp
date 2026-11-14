// Sound Recipe (src/media/sound/sound_recipe.h): every non-transferred parameter carries its provenance;
// measured values match a synthetic render with known release and velocity scaling; NOT AVAILABLE
// entries carry no value; an estimate is never listed as measured. SOFTWARE evidence only.

#include "media/sound/sound_recipe.h"
#include <cstdio>

using namespace ai_arranger::media::sound;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static const RecipeParam* find(const SoundRecipe& r, const std::string& block, const std::string& prefix) {
    for (const auto& p : r.params) if (p.block == block && p.name.rfind(prefix, 0) == 0) return &p;
    return nullptr;
}

int main() {
    std::printf("Test: Sound Recipe\n");
    // tone held 1.5 s from 0.1 s, then exp(-10 t) release (-60 dB at 0.691 s); amplitude ~ velocity
    const RenderFn synth = [](uint8_t note, uint8_t vel) {
        const double f = 440.0 * std::pow(2.0, (note - 69) / 12.0), kPi = 3.14159265358979323846;
        std::vector<float> x(static_cast<size_t>(3.0 * 48000), 0.f);
        for (size_t n = 4800; n < x.size(); ++n) {
            const double t = (n - 4800) / 48000.0, env = std::min(1.0, t / 0.005) * (t < 1.5 ? 1.0 : std::exp(-(t - 1.5) * 10));
            x[n] = static_cast<float>(vel / 127.0 * 0.5 * env * std::sin(2 * kPi * f * t));
        }
        return x;
    };
    RenderSpec spec;
    spec.name = "Recipe Test";
    spec.lowRoot = 48; spec.highRoot = 72; spec.step = 12;
    spec.velocities = {64, 120};
    spec.loopHoldSec = 1.6;
    RenderReport rep;
    const Instrument in = buildFromRenders(spec, synth, &rep);
    TEST("render built", !in.samples.empty());
    const SoundRecipe r = recipeFromRender(spec, rep, {{"Cutoff", "1200 Hz"}, {"Resonance", ""}});
    TEST("recipe passes its own provenance rules", validate(r).empty());
    const RecipeParam* rel = find(r, "Amp EG", "Release");
    TEST("release MEASURED at 0.69 s", rel && rel->prov == Provenance::Measured && rel->value.rfind("0.69", 0) == 0);
    const RecipeParam* vel = find(r, "Velocity", "Level per layer");
    // 20 log10(64/120) = -5.46 dB
    TEST("velocity level per layer MEASURED (vel 64: -5.5 dB)", vel && vel->prov == Provenance::Measured &&
         vel->value.find("vel 64: -5.5 dB") != std::string::npos && vel->value.find("vel 120: +0.0 dB") != std::string::npos);
    const RecipeParam* att = find(r, "Amp EG", "Attack");
    TEST("attack is ESTIMATED with a stated rule, not measured", att && att->prov == Provenance::Estimated && att->basis.rfind("rule:", 0) == 0);
    bool na = true;
    for (const char* b : {"Filter", "Pitch", "LFO", "Effects"}) {
        bool seen = false;
        for (const auto& p : r.params) if (p.block == b) { seen = true; na &= p.prov == Provenance::NotAvailable && p.value.empty(); }
        na &= seen;
    }
    TEST("filter, pitch, LFO, effects: NOT AVAILABLE with no value", na);
    const RecipeParam* cut = find(r, "Source", "Cutoff");
    TEST("plugin parameters listed as EXTRACTED, unmapped (no Pa5X target)", cut && cut->prov == Provenance::Extracted &&
         cut->value == "1200 Hz" && cut->pa5x.empty() && find(r, "Source", "Resonance")->value == "(no display value)");
    const std::string txt = formatRecipe(r);
    const size_t m = txt.find("MEASURED:"), e = txt.find("ESTIMATED:"), x = txt.find("EXTRACTED:"), n = txt.find("NOT AVAILABLE:");
    TEST("report grouped MEASURED, ESTIMATED, EXTRACTED, NOT AVAILABLE in that order",
         m != std::string::npos && m < e && e < x && x < n && txt.find("Attack: fastest") > e && txt.find("Attack: fastest") < x);
    {
        RenderSpec one = spec; one.velocities = {100}; one.loopHoldSec = 0;
        RenderReport r1;
        buildFromRenders(one, synth, &r1);
        const SoundRecipe s1 = recipeFromRender(one, r1);
        TEST("one-shot, one velocity: release and layer level NOT AVAILABLE", validate(s1).empty() &&
             find(s1, "Amp EG", "Release")->prov == Provenance::NotAvailable && find(s1, "Velocity", "Level per layer")->prov == Provenance::NotAvailable);
    }
    {
        SoundRecipe bad;
        bad.sound = "x";
        bad.params.push_back({"Filter", "Cutoff", "800 Hz", Provenance::NotAvailable, "guess", ""});
        bad.params.push_back({"Amp EG", "Release", "", Provenance::Measured, "m", ""});
        bad.params.push_back({"LFO", "Rate", "5 Hz", Provenance::Estimated, "", ""});
        TEST("rules reject a value on NOT AVAILABLE, a measured entry without value, an estimate without basis",
             validate(bad).size() == 3);
    }
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
