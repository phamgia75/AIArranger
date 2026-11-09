// Route C (docs/research/KRONOS_KMP_KSF_IMPORT.md): Instrument from a folder of
// <layer>_<OrgKey>_<LowKey>_<HighKey>.wav files. Round trip through the package writer's own wav/
// output, plus refusals for malformed or unsupported input. SOFTWARE evidence only.

#include "media/sound/additive_source.h"
#include "media/sound/package_writer.h"
#include "media/sound/wav_folder_source.h"
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger::media::sound;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static void put(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

int main() {
    std::printf("Test: WAV folder source (route C)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-wavfolder-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    // ── round trip: demo package wav/ → Instrument ──
    const Instrument org = buildDemoOrgan(nullptr);
    std::string err;
    TEST("demo package written", !writePackage(org, root / "pkg", "", &err).empty());
    Instrument back;
    err = buildFromWavFolder(root / "pkg" / "wav", "AIA Demo Organ", {"AIA_Organ_Soft", "AIA_Organ_Bright"}, {90}, &back);
    TEST("demo wav/ folder reads back", err.empty());
    bool same = back.layers.size() == org.layers.size() && back.samples.size() == org.samples.size();
    for (size_t l = 0; same && l < org.layers.size(); ++l) {
        const Layer &a = org.layers[l], &b = back.layers[l];
        same &= a.name == b.name && a.velLo == b.velLo && a.velHi == b.velHi && a.zones.size() == b.zones.size();
        for (size_t z = 0; same && z < a.zones.size(); ++z) {
            const Zone &za = a.zones[z], &zb = b.zones[z];
            const Sample &sa = org.samples[size_t(za.sample)], &sb = back.samples[size_t(zb.sample)];
            same &= za.lo == zb.lo && za.hi == zb.hi && za.root == zb.root && sa.pcm == sb.pcm && sa.rate == sb.rate &&
                    sa.root == sb.root && sa.looped == sb.looped && sa.loopStart == sb.loopStart && sa.loopEnd == sb.loopEnd &&
                    sa.channels == sb.channels && sb.name.size() <= 20;
        }
    }
    TEST("layers, velocities, zones, roots, loops and PCM identical after the round trip", same);
    TEST("read-back instrument packages again", !writePackage(back, root / "pkg2", "", &err).empty());

    // stereo sample round trip
    Sample st; st.name = "St"; st.channels = 2; st.root = 64; st.pcm = {1, -1, 2, -2, 3, -3};
    for (int i = 0; i < 200; ++i) st.pcm.push_back(int16_t(i)), st.pcm.push_back(int16_t(-i));
    st.looped = true; st.loopStart = 10; st.loopEnd = 100;
    Sample st2;
    TEST("stereo WAV with loop reads back", readWav(writeWav(st), &st2).empty() && st2.pcm == st.pcm && st2.channels == 2 &&
                                            st2.root == 64 && st2.looped && st2.loopStart == 10 && st2.loopEnd == 100);

    // single layer, no layer names needed
    fs::create_directories(root / "one");
    Sample m = st; m.channels = 1; m.root = 60;
    put(root / "one" / "Pad_060_000_127.wav", writeWav(m));
    put(root / "one" / "readme.txt", {'x'});
    Instrument one;
    err = buildFromWavFolder(root / "one", "Pad", {}, {}, &one);
    TEST("single-prefix folder needs no layer list; non-WAV files ignored", err.empty() && one.layers.size() == 1 &&
                                                                             one.layers[0].name == "Pad" && one.layers[0].zones[0].root == 60);

    // ── refusals ──
    auto refuses = [&](const char* label, const std::string& file, const std::vector<uint8_t>& bytes, const char* expect) {
        const fs::path d = root / label;
        fs::create_directories(d);
        put(d / file, bytes);
        Instrument x;
        const auto e = buildFromWavFolder(d, "X", {}, {}, &x);
        TEST(label, e.find(expect) != std::string::npos);
        if (e.find(expect) == std::string::npos) std::fprintf(stderr, "    got: %s\n", e.c_str());
    };
    refuses("bad name refused", "Pad_60_127.wav", writeWav(m), "file name is not");
    refuses("prefix-less name refused", "_060_000_127.wav", writeWav(m), "file name is not");
    refuses("key over 127 refused", "Pad_060_000_128.wav", writeWav(m), "file name is not");
    refuses("root outside range refused", "Pad_070_000_064.wav", writeWav(m), "root outside");
    refuses("smpl root mismatch refused", "Pad_061_000_127.wav", writeWav(m), "differs from the file name");
    auto w = writeWav(m);
    w[20] = 3;   // format tag float
    refuses("non-PCM refused", "Pad_060_000_127.wav", w, "uncompressed PCM");
    w = writeWav(m); w[34] = 24;
    refuses("24-bit refused", "Pad_060_000_127.wav", w, "16-bit");
    w = writeWav(m); w.resize(w.size() - 60);   // drop the smpl chunk tail
    refuses("truncated chunk refused", "Pad_060_000_127.wav", w, "overruns");
    Sample nl = m; nl.looped = false;
    w = writeWav(nl); w.resize(w.size() - 44);   // remove the smpl chunk entirely
    {   // no smpl (plain DAW export): root from the file name, one-shot; readWav alone stays strict
        Sample x;
        TEST("readWav without smpl is refused when the caller needs the root", readWav(w, &x).find("no smpl") != std::string::npos);
        const auto d = fs::temp_directory_path() / ("aia-nosmpl-" + std::to_string(::getpid()));
        fs::remove_all(d); fs::create_directories(d);
        std::ofstream(d / "Pad_060_000_127.wav", std::ios::binary).write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
        Instrument in; const auto e = buildFromWavFolder(d.string(), "Pad", {}, {}, &in);
        TEST("WAV without smpl: root from the file name, no loop (GUI validation 2026-11-09)",
             e.empty() && in.samples.size() == 1 && in.samples[0].root == 60 && !in.samples[0].looped);
        fs::remove_all(d);
    }
    w = writeWav(m); w[w.size() - 24 + 4] = 1;   // loop type ping-pong
    refuses("non-forward loop refused", "Pad_060_000_127.wav", w, "forward loops");
    // regressions from the independent (Codex) review
    w = writeWav(m); w[22] = 1; w[23] = 1;   // 257 channels: narrowed to 1 by the old code
    refuses("257 channels refused (not narrowed to mono)", "Pad_060_000_127.wav", w, "mono or stereo");
    w = writeWav(m); w[w.size() - 24 - 8] = 2;   // numSampleLoops = 2
    refuses("two loops refused", "Pad_060_000_127.wav", w, "one loop");
    w = writeWav(m); w[w.size() - 4] = 1;   // play count 1
    refuses("finite play count refused", "Pad_060_000_127.wav", w, "play count");
    w = writeWav(m); w[w.size() - 8] = 1;   // fraction
    refuses("fractional loop refused", "Pad_060_000_127.wav", w, "fractional");
    {   // a 65-byte mono data chunk (+ pad byte) before a valid smpl chunk
        Sample o = m; o.looped = false; o.pcm.resize(33);
        auto v = writeWav(o);
        const size_t d = 12 + 8 + 16;   // 'data' header offset
        v[d + 4] = 65;                  // declare 65 bytes; byte 66 becomes the RIFF pad
        refuses("odd-sized data chunk refused", "Pad_060_000_127.wav", v, "whole 16-bit frames");
    }
    {
        Instrument x;
        TEST("two prefixes without a layer order refused",
             buildFromWavFolder(root / "pkg" / "wav", "X", {}, {}, &x).find("velocity order") != std::string::npos);
        TEST("wrong split count refused",
             buildFromWavFolder(root / "pkg" / "wav", "X", {"AIA_Organ_Soft", "AIA_Organ_Bright"}, {}, &x).find("split") != std::string::npos);
        TEST("split 0 refused by validate",
             !buildFromWavFolder(root / "pkg" / "wav", "X", {"AIA_Organ_Soft", "AIA_Organ_Bright"}, {0}, &x).empty());
    }
    // every prefix of a valid WAV is refused or parsed, never read out of bounds (ASan in CI)
    {
        const auto full = writeWav(st);
        bool none = true;
        for (size_t n = 0; n < full.size(); ++n) {
            Sample x;
            const std::vector<uint8_t> cut(full.begin(), full.begin() + long(n));
            none &= !readWav(cut, &x).empty();
        }
        TEST("every truncated prefix of a WAV is refused", none);
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
