// Key/velocity mapping, end to end: the Sample Plugin form (note names) -> plan and pre-render map ->
// buildFromRenders (a synthetic renderer stands in for the plugin) -> writePackage -> the existing readers
// (RESOURCES.tsv through loadPackage, the SF2 reader sf2_check.h, the WAVs' smpl chunks through readWav).
// Every reader must describe the same map: keys 0-127 covered once per layer, roots at the sampled notes,
// the velocity ranges the preview shows, stereo as linked SF2 pairs, loops where asked. Edges at C-1 and G9.

#include "media/sf2_check.h"
#include "media/sound/package_writer.h"
#include "media/sound/wav_folder_source.h"
#include "ui/sound/plugin_sampler_model.h"
#include "ui/sound/sound_conversion_model.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
namespace snd = ai_arranger::media::sound;
using namespace ai_arranger::ui;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } } while (0)

// 0.05 s silence, a 3-harmonic tone held for `hold` s, then a 0.3 s decay; stereo = same tone, right channel at 60 %.
static snd::RenderFn synth(uint8_t channels, double hold) {
    return [=](uint8_t note, uint8_t vel) {
        const double f = 440.0 * std::pow(2.0, (note - 69) / 12.0), kPi = 3.14159265358979323846;
        const size_t n0 = 2400, n1 = n0 + static_cast<size_t>(hold * 48000), total = n1 + 14400 + 2400;
        std::vector<float> x(total * channels, 0.f);
        for (size_t n = n0; n < total - 2400; ++n) {
            const double t = (n - n0) / 48000.0;
            const double env = std::min(1.0, t / 0.005) * (n < n1 ? 1.0 : std::exp(-(t - hold) * 20));
            const double v = vel / 127.0 * 0.4 * env * (std::sin(2 * kPi * f * t) + 0.3 * std::sin(4 * kPi * f * t) + 0.1 * std::sin(6 * kPi * f * t));
            for (uint8_t c = 0; c < channels; ++c) x[n * channels + c] = static_cast<float>(c ? 0.6 * v : v);
        }
        return x;
    };
}

struct Case { const char* what; SamplerSettings form; };

static void run(const Case& c, const fs::path& root) {
    std::printf("Case: %s\n", c.what);
    SamplerPlan p;
    const auto problem = planRender(c.form, &p);
    TEST(std::string(c.what) + ": form accepted", problem.empty());
    if (!problem.empty()) { std::fprintf(stderr, "    %s\n", problem.c_str()); return; }
    const auto roots = snd::renderRoots(p.spec);
    const auto vel = snd::layerVelocityRanges(p.spec);
    const std::string map = mapText(p);

    // The preview names every sampled note when there are at most 8, and each velocity range.
    bool previewOk = map.find("every key C-1 to G9 plays") != std::string::npos;
    if (roots.size() <= 8) for (int r : roots) previewOk &= map.find(snd::txw::noteName(r)) != std::string::npos;
    if (vel.size() <= 6) for (const auto& [lo, hi] : vel) previewOk &= map.find(std::to_string(lo) + "-" + std::to_string(hi)) != std::string::npos;
    TEST(std::string(c.what) + ": preview names the sampled notes and velocity ranges", previewOk);

    // The engine's instrument.
    snd::RenderReport rep;
    const snd::Instrument in = snd::buildFromRenders(p.spec, synth(p.spec.channels, std::stod(c.form.hold)), &rep);
    bool built = in.layers.size() == vel.size();
    for (size_t l = 0; built && l < in.layers.size(); ++l) {
        const auto& L = in.layers[l];
        built &= L.velLo == vel[l].first && L.velHi == vel[l].second && L.zones.size() == roots.size();
        for (size_t z = 0; built && z < L.zones.size(); ++z) {
            const auto& Z = L.zones[z];
            const auto& S = in.samples[static_cast<size_t>(Z.sample)];
            built &= Z.root == roots[z] && S.root == roots[z] && S.channels == p.spec.channels && S.looped == (p.spec.loopHoldSec > 0) &&
                     Z.lo == (z == 0 ? 0 : L.zones[z - 1].hi + 1) && Z.lo <= Z.root && Z.root <= Z.hi && (z + 1 < L.zones.size() || Z.hi == 127);
        }
        built &= l == 0 ? L.velLo == 0 : L.velLo == in.layers[l - 1].velHi + 1;
    }
    built &= in.layers.empty() || in.layers.back().velHi == 127;
    TEST(std::string(c.what) + ": engine: keys 0-127 once per layer, roots at the sampled notes, preview's velocity ranges, channels, loops", built);

    // The written package, read back by the existing readers.
    std::string err;
    const fs::path out = root / p.spec.name;
    const std::string report = snd::writePackage(in, out, "", &err);
    TEST(std::string(c.what) + ": package written and checked", !report.empty());
    if (report.empty()) { std::fprintf(stderr, "    %s\n", err.c_str()); return; }

    ai_arranger::media::graph::Graph g;
    std::string pkg;
    TEST(std::string(c.what) + ": package loads back", snd::loadPackage(out, g, pkg, &err));
    // RESOURCES.tsv details: "oscillator N velocity a-b" and "zone z keys a-b root r", compared with the instrument.
    std::set<std::string> want, got;
    for (size_t l = 0; l < in.layers.size(); ++l) {
        want.insert("oscillator " + std::to_string(l + 1) + " velocity " + std::to_string(in.layers[l].velLo) + "-" + std::to_string(in.layers[l].velHi));
        for (size_t z = 0; z < in.layers[l].zones.size(); ++z) {
            const auto& Z = in.layers[l].zones[z];
            want.insert(std::to_string(l) + ":zone " + std::to_string(z) + " keys " + std::to_string(Z.lo) + "-" + std::to_string(Z.hi) + " root " + std::to_string(Z.root));
        }
    }
    for (const auto& e : g.edges()) {
        if (e.detail.rfind("oscillator ", 0) == 0) got.insert(e.detail);
        if (e.detail.rfind("zone ", 0) == 0) got.insert(std::to_string(e.from.number) + ":" + e.detail);
    }
    TEST(std::string(c.what) + ": RESOURCES.tsv records the same velocity ranges, key ranges and roots", got == want);

    // SF2 (read by sf2_check.h, not the writer): one instrument per layer, each zone's key range, stereo as linked L/R pairs.
    fs::path sf2Path;
    for (const auto& e : fs::directory_iterator(out)) if (e.path().extension() == ".sf2") sf2Path = e.path();
    const auto sf = ai_arranger::media::sf2::checkFile(sf2Path.string());
    bool sf2ok = sf.ok && sf.instruments.size() == in.layers.size() && sf.overlapping() == 0;
    for (size_t l = 0; sf2ok && l < in.layers.size(); ++l) {
        const auto& zs = sf.instruments[l].zones;
        sf2ok &= zs.size() == in.layers[l].zones.size() * p.spec.channels;
        for (size_t k = 0; sf2ok && k < zs.size(); ++k) {
            const auto& Z = in.layers[l].zones[k / p.spec.channels];
            const auto& S = sf.samples[static_cast<size_t>(zs[k].sample)];
            sf2ok &= zs[k].key.lo == Z.lo && zs[k].key.hi == Z.hi;
            if (p.spec.channels == 2) sf2ok &= (S.type == 2 || S.type == 4) && sf.samples[S.link].link == static_cast<uint16_t>(zs[k].sample);
            else sf2ok &= S.type == 1;
        }
    }
    TEST(std::string(c.what) + ": SF2 reader sees the same key ranges per layer" + (p.spec.channels == 2 ? ", stereo as linked left/right pairs" : ", mono samples"),
         sf2ok);

    // wav/<layer>_<root>_<lo>_<hi>.wav: names and smpl chunks (root, loop) as the instrument.
    bool wavOk = true;
    for (const auto& L : in.layers)
        for (const auto& Z : L.zones) {
            char name[200];
            std::snprintf(name, sizeof name, "%s_%03u_%03u_%03u.wav", snd::wavPrefix(L.name).c_str(), Z.root, Z.lo, Z.hi);
            std::ifstream f(out / "wav" / name, std::ios::binary);
            const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
            snd::Sample s;
            wavOk &= !b.empty() && snd::readWav(b, &s).empty() && s.root == Z.root && s.channels == p.spec.channels && s.looped == (p.spec.loopHoldSec > 0);
        }
    TEST(std::string(c.what) + ": every WAV is named by root/low/high and its smpl chunk carries the root and loop", wavOk);
}

int main() {
    std::printf("Test: key/velocity mapping, preview to package\n");
    for (int n = 0; n <= 127; ++n) {
        int back = -1;
        if (!noteNumber(snd::txw::noteName(n), &back) || back != n || SoundConversionModel::key(static_cast<uint8_t>(n)) != snd::txw::noteName(n)) {
            TEST("note name round trip " + std::to_string(n), false);
            return 1;
        }
    }
    TEST("all 128 notes: name -> number round trip, the two name functions agree (C4 = 60, C-1 = 0, G9 = 127)",
         snd::txw::noteName(60) == "C4" && snd::txw::noteName(0) == "C-1" && snd::txw::noteName(127) == "G9");

    const fs::path root = fs::temp_directory_path() / ("aia-mapping-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    auto form = [](const char* name, const char* lo, const char* hi, const char* step, const char* vels, const char* splits, bool stereo, bool loop) {
        SamplerSettings s;
        s.pluginId = "test"; s.name = name; s.low = lo; s.high = hi; s.step = step; s.velocities = vels; s.splits = splits;
        s.hold = loop ? "1.0" : "0.6"; s.release = "0.4"; s.stereo = stereo; s.loop = loop;
        return s;
    };
    const Case cases[] = {
        {"3 layers with splits, mono, names C2-C7", form("Map Three", "C2", "C7", "6", "40,80,120", "50,100", false, false)},
        {"full range C-1 to G9, stereo, looped", form("Map Full", "C-1", "G9", "12", "100", "", true, true)},
        {"one note at G9 (127), 2 layers halfway", form("Map Top", "G9", "g9", "1", "60,110", "", false, true)},
        {"one note at C-1 (0), stereo", form("Map Bottom", "c-1", "0", "1", "90", "", true, false)},
        {"names with sharps and flats, numbers mixed", form("Map Mixed", "Bb1", "f#4", "5", "30,70,100,127", "", true, false)},
    };
    for (const auto& c : cases) run(c, root);

    {   // a loop that can never be found is refused before rendering, not turned into one-shots afterwards
        SamplerSettings t = form("Map Loop", "C4", "C4", "1", "100", "", false, true);
        t.hold = "0.6";
        const auto e = planRender(t, nullptr);
        TEST("Loop with a hold too short to loop (0.6 s) is refused with the minimum", e.find("a loop needs a hold of at least 0.61 s") != std::string::npos);
        t.hold = "0.62";
        TEST("...and accepted just above the engine's minimum", planRender(t, nullptr).empty());
        {   // the boundary with the plugin's leading silence: accepted, rendered, and every sample one-shot -> said before Generate
            SamplerPlan p;
            planRender(t, &p);
            const auto in = snd::buildFromRenders(p.spec, synth(1, 0.62));   // 50 ms of silence before the note, trimmed by the engine
            TEST("a 0.62 s hold with 50 ms leading silence renders one-shot, and the status says so with what to do",
                 unloopedSamples(in, true) == in.samples.size() && renderedStatus(in, true) == "Rendered 1 samples; 1 could not be looped. See the preview." &&
                 loopWarning(in, true).find("1 of 1 samples could not be looped") != std::string::npos && loopWarning(in, true).find("Raise Hold") != std::string::npos);
            SamplerSettings longer = t;
            longer.hold = "1.0";
            planRender(longer, &p);
            const auto ok = snd::buildFromRenders(p.spec, synth(1, 1.0));
            TEST("...a 1.0 s hold loops, and the status has no loop warning", unloopedSamples(ok, true) == 0 &&
                 renderedStatus(ok, true) == "Rendered 1 samples. Check the map, then Generate Package..." && loopWarning(ok, true).empty());
        }
        t.hold = "0.6"; t.loop = false;
        TEST("...a 0.6 s hold without Loop is fine", planRender(t, nullptr).empty());
    }
    {   // G9 off the sampling step: the top sample is below it and the preview says so; key 127 still plays
        SamplerPlan p;
        planRender(form("Map Step", "C-1", "G9", "12", "100", "", false, false), &p);
        TEST("off-step G9: preview names the top sample", mapText(p).find("G9 is not on the sampling step; the top sample is C9.") != std::string::npos);
    }
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
