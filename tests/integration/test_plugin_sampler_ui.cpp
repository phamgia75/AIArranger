// "Sample Plugin" tab model (ui/sound/plugin_sampler_model.h) and the shared engine rules it uses:
// form checks, estimate, sampler command line, sampler output parsing, output-folder refusal.
// No plugin is loaded here; SOFTWARE evidence.

#include "ui/sound/plugin_sampler_model.h"
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger::ui;
namespace snd = ai_arranger::media::sound;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool contains(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }

int main() {
    const auto list = parsePluginList("JUCE v8.0.4\nAudioUnit\tDLSMusicDevice\tApple\tAudioUnit:Synths/aumu,dls ,appl\n"
                                      "warning: x\nVST3\tBad\n");
    TEST("plugin list: tab lines parsed, banner and short lines ignored",
         list.size() == 1 && list[0].name == "DLSMusicDevice" && list[0].manufacturer == "Apple" && list[0].id == "AudioUnit:Synths/aumu,dls ,appl");

    SamplerSettings s;
    s.pluginId = "id"; s.name = "Piano"; s.low = "48"; s.high = "72"; s.step = "12"; s.velocities = "60, 110"; s.hold = "1.0"; s.release = "0.5";
    SamplerPlan p;
    TEST("valid form plans a render", planRender(s, &p).empty() && p.spec.velocities.size() == 2 && p.spec.lowRoot == 48);
    TEST("estimate: 3 notes x 2 layers, 9 s of audio, exact upper bounds",
         p.estimate.roots == 3 && p.estimate.samples == 6 && std::abs(p.estimate.audioSec - 9.0) < 1e-9 &&
         p.estimate.pcmBytesMax == uint64_t(6 * (1.5 * 48000 + 512) * 1 * 2) &&
         p.estimate.workingBytes == snd::kRenderBaselineBytes + uint64_t(6 * (1.5 * 48000 + 512) * 1 * snd::kRenderBytesPerFrameChannel));
    {   // the large DLSMusicDevice job measured on 2026-11-10: 183 stereo notes x 3 s peaked at 1024 MB
        snd::RenderSpec big; big.lowRoot = 36; big.highRoot = 96; big.step = 1; big.velocities = {40, 80, 120}; big.channels = 2;
        const auto e = snd::estimateRender(big, 2.5, 0.5);
        TEST("estimate matches the measured peak within 15%", e.samples == 183 && e.workingBytes > 870e6 && e.workingBytes < 1180e6);
    }
    TEST("estimate text names samples, audio, package size and working memory",
         contains(estimateText(p), "6 samples (3 notes x 2 layers), 9 s of audio") && contains(estimateText(p), "memory while rendering"));
    TEST("roots come from the engine's own rule", snd::renderRoots(p.spec) == std::vector<int>({48, 60, 72}));

    auto bad = [&](const char* what, auto change, const char* expect) {
        SamplerSettings t = s; change(t);
        const auto e = planRender(t, nullptr);
        TEST(what, contains(e, expect));
    };
    bad("no instrument chosen", [](SamplerSettings& t) { t.pluginId.clear(); }, "choose an instrument");
    bad("name too long", [](SamplerSettings& t) { t.name = std::string(21, 'x'); }, "1-20 characters");
    bad("low above high", [](SamplerSettings& t) { t.low = "80"; }, "lowest first");
    bad("note out of range", [](SamplerSettings& t) { t.high = "128"; }, "0-127");
    bad("non-numeric step", [](SamplerSettings& t) { t.step = "3x"; }, "semitones");
    bad("velocities not rising", [](SamplerSettings& t) { t.velocities = "110,60"; }, "must rise");
    bad("split outside its layers", [](SamplerSettings& t) { t.splits = "120"; }, "must rise");
    bad("velocity 0 refused", [](SamplerSettings& t) { t.velocities = "0,100"; }, "1-127");
    bad("hold + release over 21 s", [](SamplerSettings& t) { t.hold = "20"; t.release = "2"; }, "at most 21 s");
    bad("hold not a number", [](SamplerSettings& t) { t.hold = "two"; }, "hold must be");
    bad("program not a number", [](SamplerSettings& t) { t.program = "-1"; }, "program");
    bad("job over the working-memory cap refused before it starts",
        [](SamplerSettings& t) { t.low = "0"; t.high = "127"; t.step = "1"; t.velocities = "10,20,30,40,50,60,70,80,90,100,110,120";
                                 t.hold = "15"; t.release = "5"; t.stereo = true; }, "too large");

    SamplerSettings a = s; a.splits = "80"; a.program = "3"; a.loop = true; a.stereo = true;
    const auto args = samplerArgs(a, "/tmp/out");
    auto has = [&](const std::string& k, const std::string& v) {
        for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == k && args[i + 1] == v) return true;
        return false;
    };
    TEST("command line carries every setting, spaces removed from lists",
         args[0] == "render" && has("--plugin", "id") && has("--out", "/tmp/out") && has("--vel", "60,110") && has("--splits", "80") &&
         has("--program", "3") && has("--loop", "on") && has("--stereo", "on") && has("--hold", "1.0") && has("--low", "48"));
    TEST("optional flags left out when not set", !contains([&] { std::string j; for (auto& x : samplerArgs(s, "o")) j += x + " "; return j; }(), "--loop"));

    SamplerProgress pr;
    for (const char* l : {"JUCE v8.0.4", "progress\t2\t6", "AIArranger Sound package", "layers\tPiano_Lo,Piano_Hi", "splits\t86", "package\t/tmp/p"})
        readSamplerLine(l, pr);
    TEST("progress and result lines parsed", pr.done == 2 && pr.total == 6 && pr.layers == "Piano_Lo,Piano_Hi" && pr.splits == "86" && pr.package == "/tmp/p");
    readSamplerLine("progress\t9\t6", pr);
    readSamplerLine("cannot load X: boom", pr);
    TEST("impossible progress ignored; last other line kept as the error", pr.done == 2 && pr.lastLine == "cannot load X: boom");

    // Output-folder refusal shared by every writer
    const fs::path tmp = fs::temp_directory_path() / ("aia-psm-" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / "exists");
    TEST("refusal: new local folder allowed", snd::outputFolderRefusal(tmp / "new").empty());
    TEST("refusal: existing folder", contains(snd::outputFolderRefusal(tmp / "exists"), "already exists"));
    TEST("refusal: /Volumes", contains(snd::outputFolderRefusal("/Volumes/USB/x"), "mounted volume"));
#ifdef __APPLE__
    fs::create_directory_symlink("/Volumes", tmp / "link");
    TEST("refusal: /Volumes through a symlink", contains(snd::outputFolderRefusal(tmp / "link" / "USB" / "x"), "mounted volume"));
#endif
    TEST("wavPrefix: the package's WAV naming", snd::wavPrefix("Piano Lo") == "Piano_Lo" && snd::wavPrefix("a,b") == "a_b");
    fs::remove_all(tmp);

    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
