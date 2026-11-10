// "Convert Sound" tab model (ui/sound/sound_conversion_model.h) and the WAV-source Sound Recipe
// (recipeFromSamples): inspect, order/splits, mapping preview, checks, generate. Original synthetic
// WAVs only; SOFTWARE evidence.

#include "media/kst_diff.h"
#include "media/sound/wav_writer.h"
#include "ui/sound/sound_conversion_model.h"
#include <cmath>
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger::media::sound;
using ai_arranger::ui::SoundConversionModel;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool contains(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }
static bool any(const std::vector<std::string>& v, const std::string& x) { for (const auto& s : v) if (contains(s, x)) return true; return false; }

static void wav(const fs::path& p, uint8_t root, double amp, uint32_t frames, uint32_t loopStart, uint32_t loopEnd, uint8_t ch = 1) {
    Sample s;
    s.root = root; s.rate = 48000; s.channels = ch;
    for (uint32_t i = 0; i < frames; ++i)
        for (uint8_t c = 0; c < ch; ++c) s.pcm.push_back(int16_t(amp * std::sin(2 * M_PI * 440 * std::pow(2, (root - 69) / 12.0) * i / 48000)));
    if (loopEnd) { s.looped = true; s.loopStart = loopStart; s.loopEnd = loopEnd; }
    const auto b = writeWav(s);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

int main() {
    std::printf("Test: Convert Sound model + WAV Sound Recipe\n");
    std::string err;
    const fs::path root = fs::temp_directory_path() / ("aia-convui-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "src");
    // Two layers; Soft has a gap (keys 60-63 missing) and a far-transposed zone; Hard covers 48-71.
    wav(root / "src/Soft_48_48_59.wav", 48, 4000, 9600, 4800, 9600);
    wav(root / "src/Soft_64_64_95.wav", 64, 4000, 9600, 4800, 9600);
    wav(root / "src/Hard_48_48_59.wav", 48, 16000, 9600, 4800, 9600);
    wav(root / "src/Hard_60_60_71.wav", 60, 16000, 9600, 50, 150);    // 2 ms loop starting at 1 ms

    SoundConversionModel m;
    TEST("build before inspect is refused", !m.build("X", "", "", &err) && contains(err, "choose a WAV folder"));
    TEST("inspect an empty folder: clear message", !m.inspect(root, &err) && contains(err, "no <layer>"));
    TEST("inspect reads names only: 2 layers", m.inspect(root / "src", &err) && m.layers().size() == 2 && m.layers()[0].prefix == "Hard" &&
         m.layers()[0].files == 2 && m.layers()[1].lo == 48 && m.layers()[1].hi == 95);
    TEST("proposals: softest first from dynamic names, even split (GUI validation 2026-11-09)", m.proposedOrder() == "Soft,Hard" && m.proposedSplits() == "64");
    {
        const std::string w = ai_arranger::ui::wrapLines("  basis: RMS of the first 200 ms of each sample\nshort\n\nxxxxxxxxxxxxxxxxxxxxxxxxx", 20);
        bool fits = true; std::istringstream is(w); std::string l; int n = 0;
        while (std::getline(is, l)) { fits &= l.size() <= 20; ++n; }
        TEST("wrapLines: every line fits, words kept, indent continued, blank line kept", fits && n >= 7 &&
             w.find("  basis: RMS of the\n    first 200 ms of") == 0 && w.find("\nshort\n\n") != std::string::npos);
    }
    TEST("order: dynamics ranked, case-insensitive", SoundConversionModel::loudness("FF") > SoundConversionModel::loudness("mf") &&
         SoundConversionModel::loudness("mf") > SoundConversionModel::loudness("pp") && SoundConversionModel::loudness("v10") > SoundConversionModel::loudness("v2") &&
         SoundConversionModel::loudness("Bright") < 0);
    TEST("bad split refused", !m.build("Test", "Soft,Hard", "200", &err) && contains(err, "1 to 127") && !m.built());
    TEST("falling splits refused", !m.build("Test", "Soft,Hard", "64,30", &err) && contains(err, "must rise") && !m.built());
    TEST("unknown layer name refused by the engine", !m.build("Test", "Soft,Loud", "64", &err) && !m.built());
    TEST("build with musician's order (softest first) and spaces", m.build("Test Piano", "Soft, Hard", " 70", &err) && m.built());
    TEST("velocity zones follow the split", m.instrument().layers[0].velHi == 69 && m.instrument().layers[1].velLo == 70);
    const auto c = m.checks();
    TEST("check: silent key gap inside a layer", any(c, "Soft: keys C4-D#4 are silent"));
    TEST("check: far transposition", any(c, "transposed up to 31 semitones"));
    TEST("check: very short loop", any(c, "loop is shorter than 10 ms"));
    TEST("check: loop starting at the attack", any(c, "loop starts within 2 ms"));
    TEST("check: layer range limited", any(c, "Hard: plays only C3-B4"));
    const std::string pv = m.preview();
    TEST("preview: layers, zones with note names, exact bytes", contains(pv, "Layer 'Soft'  velocity 0-69") && contains(pv, "C3  ") &&
         contains(pv, "76800 bytes uncompressed PCM (exact"));
    const std::string rc = m.recipeText();
    TEST("recipe: per-layer level MEASURED, release NOT AVAILABLE, no invented filter value",
         contains(rc, "MEASURED") && contains(rc, "Soft (vel 0-69): -") && contains(rc, "Release time\n      basis: WAV files carry no release") &&
         validate(recipeFromSamples(m.instrument())).empty());
    TEST("generate onto a mounted volume is refused", SoundConversionModel::generate(m.instrument(), root / "src", "/Volumes/USB/x", &err).empty() &&
         contains(err, "copy it to the USB stick"));
#ifdef __APPLE__
    fs::create_directory_symlink("/Volumes", root / "looks-local");
    TEST("a symlink leading onto /Volumes is refused too (Codex P1)",
         SoundConversionModel::generate(m.instrument(), root / "src", root / "looks-local/USB/x", &err).empty() && contains(err, "mounted volume"));
#endif
    {   // silent layers: no fabricated measured level (Codex P2)
        Instrument q = m.instrument();
        for (auto& s : q.samples) std::fill(s.pcm.begin(), s.pcm.end(), int16_t(0));
        const std::string all = formatRecipe(recipeFromSamples(q));
        TEST("all layers silent: level per layer NOT AVAILABLE, no dB value", contains(all, "fewer than two layers have an audible median zone") && !contains(all, " dB,") && validate(recipeFromSamples(q)).empty());
        for (const auto& z : q.layers[1].zones) { auto& s = q.samples[size_t(z.sample)]; for (size_t i = 0; i < s.pcm.size(); ++i) s.pcm[i] = int16_t(i % 2 ? 3000 : -3000); }
        Instrument q3 = q;
        q3.layers.push_back(q.layers[1]);   // a third, audible layer so two are measured
        q3.layers[1].velHi = 90; q3.layers[2].velLo = 91;
        const std::string one = formatRecipe(recipeFromSamples(q3));
        TEST("one silent layer is named silent, the others measured", contains(one, "Soft (vel 0-69): silent (median zone)") && contains(one, "+0.0 dB") && !contains(one, "e+"));
    }
    {   // Codex round 2: silent zones count in the median (2 of 3 silent -> the layer's median is silent)
        Instrument q;
        q.name = "Med";
        for (int i = 0; i < 6; ++i) { Sample s; s.root = uint8_t(48 + 12 * (i % 3)); s.pcm.assign(9600, int16_t(i == 0 || i >= 3 ? 3000 : 0)); s.name = "S" + std::to_string(i); q.samples.push_back(s); }
        for (int l = 0; l < 2; ++l) {
            Layer L; L.name = l ? "Hard" : "Soft"; L.velLo = l ? 64 : 0; L.velHi = l ? 127 : 63;
            for (int z = 0; z < 3; ++z) L.zones.push_back({uint8_t(48 + 12 * z), uint8_t(59 + 12 * z), l * 3 + z, uint8_t(48 + 12 * z), 0, 0});
            q.layers.push_back(L);
        }
        const std::string t = formatRecipe(recipeFromSamples(q));
        TEST("partly silent layer: median zone silent is reported, not +0.0 dB (Codex round 2)",
             contains(t, "fewer than two layers have an audible median zone") && !contains(t, "Soft (vel 0-63): +0.0 dB"));
    }
    {   // Codex review of bfe83b5: adopt hands the previous Sound to the caller as its only owner
        const auto before = m.snapshot().get();
        SoundConversionModel m2 = m;
        auto old = m2.adopt(m.instrument());
        TEST("adopt returns the previous Sound, no longer held by the model", old.get() == before && m2.snapshot().get() != before &&
             old.use_count() == 2 /* m still holds it; m2 does not */);
    }
    TEST("Generate shares the decoded Sound instead of copying its audio (Codex round 2)",
         m.snapshot() && m.snapshot().get() == &m.instrument());
    m.invalidate();
    TEST("invalidate: a changed setting means nothing is built", !m.built());
    TEST("rebuild", m.build("Test Piano", "Soft, Hard", " 70", &err));
    fs::copy(root / "src", root / "src-before", fs::copy_options::recursive);
    const auto srcTime = fs::last_write_time(root / "src");
    const std::string out = SoundConversionModel::generate(m.instrument(), root / "src", root / "out", &err);
    TEST("generate: package written, reloads, budget and recipe in the summary",
         contains(out, "Package written") && contains(out, "0 unresolved") && contains(out, "verdict: fits") &&
         fs::exists(root / "out/Test_Piano.sf2") && fs::exists(root / "out/RESOURCES.tsv") && fs::exists(root / "out/REPORT.txt"));
    TEST("REPORT.txt carries the Sound Recipe", [&] { std::ifstream f(root / "out/REPORT.txt"); std::string s((std::istreambuf_iterator<char>(f)), {}); return contains(s, "POST-IMPORT CONFIGURATION"); }());
    TEST("generate never overwrites", SoundConversionModel::generate(m.instrument(), root / "src", root / "out", &err).empty() && contains(err, "already exists"));
    TEST("source folder unchanged: same 4 files, same bytes, folder not modified",
         std::distance(fs::directory_iterator(root / "src"), fs::directory_iterator{}) == 4 && ai_arranger::media::diff(root / "src-before", root / "src").same() &&
         fs::last_write_time(root / "src") == srcTime);
    TEST("note names", SoundConversionModel::key(60) == "C4" && SoundConversionModel::key(0) == "C-1" && SoundConversionModel::key(127) == "G9");

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
