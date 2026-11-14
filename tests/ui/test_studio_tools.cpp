// AI Music Studio phase-2 jobs (ui/studio/studio_tools.h): KMP/KSF folder (EXPERIMENTAL, L1 only), Sample Lab tools,
// Pa-native SET recognition and inspect, and the real level of each new output (L2 only for a package; never L3).
// Synthetic audio generated in-test only.

#include "media/sound/additive_source.h"
#include "media/sound/package_writer.h"
#include "ui/studio/studio_tools.h"
#include <cmath>
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::ui::studio;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static void put(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// One bar of 4 decaying 440 Hz hits at 120 BPM, 44.1 kHz, with 0.25 s of silence before and after.
static media::sound::Sample loop4() {
    media::sound::Sample s;
    s.rate = 44100;
    const size_t beat = 22050, pad = 11025;
    s.pcm.assign(pad + 4 * beat + pad, 0);
    for (size_t b = 0; b < 4; ++b)
        for (size_t i = 0; i < 8000; ++i)
            s.pcm[pad + b * beat + i] = int16_t(12000 * std::exp(-double(i) / 2000) * std::sin(2 * M_PI * 440 * double(i) / s.rate));
    return s;
}

int main() {
    std::printf("Test: studio tools (KMP/KSF, Sample Lab, Pa-native inspect, output levels)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-studio-tools-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    // ── KMP/KSF writer: L1 only, never L2/L3, nothing called compatible ──
    const auto organ = media::sound::buildDemoOrgan();
    auto k = writeKmpFolder(organ, root / "organ-kmp");
    TEST("KMP folder written", fs::is_regular_file(root / "organ-kmp/PACKAGE.sha256") && k.output == (root / "organ-kmp").string());
    TEST("KMP folder: L1 passed, L2 not claimed, no L3", k.level && k.level->l1.state == Check::Passed && k.level->l2.state == Check::NotRun &&
         k.level->l3.state == Check::NotRun && has(k.level->l2.detail, "L1 only"));
    TEST("KMP report says EXPERIMENTAL and L1", has(k.text, "EXPERIMENTAL") && has(k.text, "level L1"));
    {
        std::string t = k.text;   // the only allowed mention is the negation
        for (size_t i; (i = t.find("not labelled Pa5X-compatible")) != std::string::npos;) t.erase(i, 28);
        TEST("KMP report never claims compatibility", !has(t, "ompatible"));
    }
    TEST("KMP badge line: L1 only", badgeLine(*k.level) == "\xE2\x97\x86 L1  \xE2\x97\x87 L2  \xE2\x97\x87 L3");
    TEST("existing KMP folder refused, nothing changed", !writeKmpFolder(organ, root / "organ-kmp").level);
    TEST("KMP onto /Volumes refused", has(writeKmpFolder(organ, "/Volumes/AIA-STUDIO-TEST/x").text, "mounted volume"));

    // ── a Sound package keeps L1 + L2 ──
    std::string err;
    TEST("package written", !media::sound::writePackage(organ, root / "organ-pkg", "", &err).empty());
    const auto pk = verifyOutput(root / "organ-pkg");
    TEST("package: L1 + L2", pk.level->l1.state == Check::Passed && pk.level->l2.state == Check::Passed && pk.level->l3.state == Check::NotRun);

    // ── a changed KMP file fails L1 ──
    for (const auto& e : fs::directory_iterator(root / "organ-kmp"))
        if (e.path().extension() == ".KSF") { std::ofstream(e.path(), std::ios::app) << "x"; break; }
    TEST("changed KMP folder: L1 failed", verifyOutput(root / "organ-kmp").level->l1.state == Check::Failed);

    // ── Sample Lab ──
    const auto loop = loop4();
    put(root / "in/Loop.wav", media::sound::writeWav(loop));
    const auto a = labAnalyze(root / "in/Loop.wav");
    TEST("analyze: levels, 4 onsets, previews, nothing written", has(a.text, "peak ") && has(a.text, "Onsets (4)") &&
         has(a.text, "Normalize preview") && has(a.text, "Trim preview") && !a.level && a.output.empty());
    TEST("analyze: missing file is an error", has(labAnalyze(root / "in/none.wav").text, "not a regular file"));

    const auto sl = labSlice(root / "in/Loop.wav", 1, 4, root / "slices");
    TEST("Time Slice: lead-in + 4 hits = 5 slices written", has(sl.text, "Slices: 5") && fs::is_regular_file(root / "slices/SLICES.tsv"));
    TEST("Time Slice folder: L1 only (Sample Lab working files)", sl.level && sl.level->l1.state == Check::Passed &&
         sl.level->l2.state == Check::NotRun && has(sl.level->l2.detail, "Sample Lab"));
    TEST("Time Slice: bad bars refused, nothing written", !labSlice(root / "in/Loop.wav", 0, 4, root / "slices0").level && !fs::exists(root / "slices0"));

    const auto lp = labLoop(root / "in/Loop.wav", 11025, 11025 + 4 * 22050, 64, 256, root / "looped");
    TEST("loop helper written, L1", lp.level && lp.level->l1.state == Check::Passed && fs::exists(root / "looped/wav/Loop.wav"));
    TEST("loop outside the sample refused", !labLoop(root / "in/Loop.wav", 0, 999999999, 0, 0, root / "looped2").level && !fs::exists(root / "looped2"));

    put(root / "in/Copy.wav", media::sound::writeWav(loop));
    const auto pre = labBatch(root / "in", true, true, {});
    TEST("batch preview: duplicates found, nothing written", has(pre.text, "Batch preview") && has(pre.text, "EXACT") && !pre.level);
    const auto bt = labBatch(root / "in", true, true, root / "batch");
    TEST("batch written, L1", bt.level && bt.level->l1.state == Check::Passed && fs::is_regular_file(root / "batch/PACKAGE.sha256"));
    TEST("batch into its own input refused", !labBatch(root / "in", false, false, root / "in/out").level);

    // ── Pa-native SET recognition (E5 layout) and inspect ──
    put(root / "Pa700copy/GLOBAL/LIMITER.AUD", {1, 2});
    put(root / "Pa700copy/SOUND/USERDK01.PCG", {0x01, 0x00, 0x01, 0x14, 0, 0});   // magic only: an invalid container
    const auto p = probeSource(root / "Pa700copy");
    TEST("SET without a .SET suffix recognised by its Pa700 markers", p.kind == SourceProbe::Kind::Pa4xSet && has(p.detected, "Pa700") && canInspectPaNative(p));
    TEST("a .PCG in a SET is not taken for Pa5X user data", p.kind != SourceProbe::Kind::Pa5xUserData);
    const auto in = paNativeInspect(root / "Pa700copy");
    TEST("inspect: read-only, lists every file, flags the invalid container", has(in.text, "READ-ONLY") && has(in.text, "Files: 2") &&
         has(in.text, "INVALID Pa-native container") && has(in.text, "no Pa-native wrapper") && !in.level);
    put(root / "Mine.KST/STYLE/USER01/Ballad.STG", {1});
    TEST("Pa5X KST: no Pa-native inspect", !canInspectPaNative(probeSource(root / "Mine.KST")));

    fs::remove_all(root);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
