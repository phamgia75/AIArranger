// KRONOS .KMP/.KSF -> Pa5X package (media/sound/korg_kmp_source.h). The input files are written here from Korg's
// documented layout (KRONOS Parameter Guide En 3 p.1158-1159), with our own synthetic audio: no Korg content. Checks
// that keys, roots, tune, loops, Start/End and stereo pairs arrive exactly, that the package's WAVs read back to the
// same audio and map, that every undocumented or unsupported case is refused by name, that the report says what is
// lost, and that the source folder is never changed.

#include "media/sound/korg_kmp_source.h"
#include "media/sound/package_verify.h"
#include "media/sound/package_writer.h"
#include "media/sound/wav_folder_source.h"
#include "ui/sound/sound_conversion_model.h"
#include "../support/korg_fixture.h"
#include <cmath>
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;
namespace snd = ai_arranger::media::sound;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } } while (0)

using namespace korgfix;

static std::string hashTree(const fs::path& d) {
    std::string all;
    for (const auto& e : fs::recursive_directory_iterator(d)) {
        if (!e.is_regular_file()) continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        all += e.path().string() + ":" + std::to_string(std::hash<std::string>{}(s)) + ":" + std::to_string(static_cast<long long>(e.last_write_time().time_since_epoch().count())) + "\n";
    }
    return all;
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("kronos-src-" + std::to_string(getpid()));
    fs::remove_all(root);
    // A KRONOS "Save Sampling Data > One Multisample" folder: PIANO000.KMP plus PIANO000/ with its KSFs (p.856).
    const fs::path one = root / "one";
    const auto a = tone(30000, 261.63, 9000, 1), b = tone(26000, 523.25, 8000, 2), c = tone(20000, 1046.5, 7000, 3);
    put(one / "PIANO000.KMP", kmp("Soft Piano", {{48, 54, 0, 0, "MS000000.KSF"}, {60, 66, -12, 5, "MS000001.KSF"}, {72, 127, 7, 0, "MS000002.KSF"}}, true, 10));
    put(one / "PIANO000" / "MS000000.KSF", ksf({a, 48000, 100, 12000, 29999, 0x00, 16, 1, 3}));   // loop on, Start 100, loop tune
    put(one / "PIANO000" / "ms000001.ksf", ksf({b, 48000, 0, 0, 25999, 0x80}));                   // loop off; lower-case name on disk (FAT)
    put(one / "PIANO000" / "MS000002.KSF", ksf({c, 44100, 0, 5000, 18999, 0x01}));                // +12 dB, End before the last frame
    const std::string before = hashTree(one);

    std::printf("== KMP/KSF reading and mapping\n");
    snd::Instrument in;
    const auto e1 = snd::buildFromKorgFolder(one, "Soft Piano", {}, {}, &in);
    TEST("a One Multisample folder converts: " + e1, e1.empty());
    if (e1.empty()) {
        const auto& z = in.layers[0].zones;
        TEST("one layer, velocity 0-127, three zones", in.layers.size() == 1 && in.layers[0].velLo == 0 && in.layers[0].velHi == 127 && z.size() == 3);
        TEST("key zones: C-1..F#3, G3..F#4, G4..G9 (low = previous top + 1)", z[0].lo == 0 && z[0].hi == 54 && z[1].lo == 55 && z[1].hi == 66 && z[2].lo == 67 && z[2].hi == 127);
        TEST("original keys C3, C4, C5 and tune 0, -12, +7 cents", z[0].root == 48 && z[1].root == 60 && z[2].root == 72 && z[0].tuneCents == 0 && z[1].tuneCents == -12 && z[2].tuneCents == 7);
        const auto& s0 = in.samples[0], s1 = in.samples[1], s2 = in.samples[2];
        TEST("Start 100..End 29999 is kept, audio exact (big endian decoded)", s0.frames() == 29900 && s0.pcm[0] == a[100] && s0.pcm.back() == a[29999]);
        TEST("loop Loop Start..End inclusive -> [11900, 29900)", s0.looped && s0.loopStart == 11900 && s0.loopEnd == 29900);
        TEST("Loop Off -> one-shot, all audio", !s1.looped && s1.frames() == 26000 && s1.pcm == b);
        TEST("End before the last frame: the tail is not copied; 44.1 kHz kept", s2.frames() == 19000 && s2.rate == 44100 && s2.pcm.back() == c[18999]);
        TEST("mono samples", s0.channels == 1 && s1.channels == 1 && s2.channels == 1);
    }

    std::printf("== Package and read-back\n");
    const fs::path out = root / "out" / "Soft Piano";
    std::string err;
    const std::string rep = snd::writePackage(in, out, snd::korgFolderNotes(one, in), &err);
    TEST("package written: " + err, !rep.empty());
    snd::Instrument back;
    const auto eb = snd::buildFromWavFolder(out / "wav", "Soft Piano", {}, {}, &back);
    TEST("the package's WAVs read back through the WAV reader: " + eb, eb.empty());
    if (eb.empty() && in.layers.size() == 1) {
        bool same = back.samples.size() == in.samples.size() && back.layers[0].zones.size() == in.layers[0].zones.size();
        for (size_t i = 0; same && i < back.layers[0].zones.size(); ++i) {
            const auto &x = back.layers[0].zones[i], &y = in.layers[0].zones[i];
            const auto &sx = back.samples[size_t(x.sample)], &sy = in.samples[size_t(y.sample)];
            same = x.lo == y.lo && x.hi == y.hi && x.root == y.root && sx.pcm == sy.pcm && sx.rate == sy.rate && sx.looped == sy.looped &&
                   sx.loopStart == sy.loopStart && sx.loopEnd == sy.loopEnd;
        }
        TEST("read back: same keys, roots, audio, rates and loops", same);
    }
    TEST("the package verifies (PACKAGE.sha256 + loadPackage)", snd::verifyPackage(out).ok);
    std::ifstream rf(out / "REPORT.txt");
    const std::string report((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    TEST("REPORT names the source format and page", report.find("KRONOS Parameter Guide En 3 p.1158-1159") != std::string::npos);
    TEST("REPORT: Level, Loop Tune, mixed +12 dB and RLP2 are listed as LOST",
         report.find("1 region(s) have a Level offset: LOST") != std::string::npos && report.find("1 sample(s) have Loop Tune: LOST") != std::string::npos &&
         report.find("+12 dB and others not") != std::string::npos && report.find("(RLP2/RLP3) are set: LOST") != std::string::npos);
    TEST("REPORT: trimmed samples and the Program limits are stated",
         report.find("2 sample(s): audio outside Start..End") != std::string::npos && report.find("velocity zones, filters, envelopes") != std::string::npos);
    TEST("the source folder is unchanged (content and times)", hashTree(one) == before);

    std::printf("== Stereo pair and velocity layers\n");
    const fs::path st = root / "stereo";
    const auto l = tone(9000, 220, 6000, 4), r = tone(9000, 330, 5000, 5), h = tone(9000, 440, 9000, 6);
    put(st / "STRIN000.KMP", kmp("Strings -L", {{60, 127, 0, 0, "STRL0000.KSF"}}));
    put(st / "STRIN001.KMP", kmp("Strings -R", {{60, 127, 0, 0, "STRR0001.KSF"}}));
    put(st / "STRL0000.KSF", ksf({l, 48000, 0, 1000, 8999}));   // "Save All": KSFs next to the KMPs
    put(st / "STRR0001.KSF", ksf({r, 48000, 0, 1000, 8999}));
    put(st / "HARD0002.KMP", kmp("Hard", {{60, 127, 0, 0, "HARD0002.KSF"}}, false));   // MSP1 name only
    put(st / "HARD0002.KSF", ksf({h, 48000, 0, 1000, 8999}));
    std::vector<snd::KorgMultisample> ms;
    const auto es = snd::scanKorgFolder(st, ms);
    TEST("scan: the -L/-R pair is one stereo multisample, plus 'Hard': " + es, es.empty() && ms.size() == 2 && ms[1].name == "Strings" && !ms[1].kmpR.empty() && ms[0].name == "Hard");
    snd::Instrument s2;
    const auto e2 = snd::buildFromKorgFolder(st, "Strings", {"Strings", "Hard"}, {100}, &s2);
    TEST("two velocity layers build: " + e2, e2.empty());
    if (e2.empty()) {
        const auto& x = s2.samples[size_t(s2.layers[0].zones[0].sample)];
        bool inter = x.channels == 2 && x.frames() == 9000;
        for (size_t i = 0; inter && i < 9000; ++i) inter = x.pcm[2 * i] == l[i] && x.pcm[2 * i + 1] == r[i];
        TEST("stereo: L and R interleaved exactly, loop kept", inter && x.looped && x.loopStart == 1000 && x.loopEnd == 9000);
        TEST("velocity: Strings 0-99, Hard 100-127", s2.layers[0].velLo == 0 && s2.layers[0].velHi == 99 && s2.layers[1].velLo == 100 && s2.layers[1].velHi == 127);
    }

    std::printf("== GUI model: the Convert Sound tab takes a KRONOS folder\n");
    ai_arranger::ui::SoundConversionModel m;
    std::string me;
    TEST("inspect lists the multisamples as layers: " + me, m.inspect(st, &me) && m.layers().size() == 2 && m.korg());
    TEST("build with order and split", m.build("Strings", "Strings,Hard", "100", &me) && m.instrument().layers.size() == 2);
    const std::string gen = ai_arranger::ui::SoundConversionModel::generate(m.instrument(), st, root / "out" / "Strings", &me);
    TEST("generate writes a package whose REPORT is the KRONOS report: " + me, !gen.empty() && snd::verifyPackage(root / "out" / "Strings").ok &&
         gen.find("KRONOS Korg-format") != std::string::npos);

    std::printf("== Refusals (each names the file and the reason)\n");
    const auto refuse = [&](const std::string& what, const Bytes& k, const Bytes& s, const std::string& expect) {
        const fs::path d = root / ("bad" + std::to_string(std::hash<std::string>{}(what) % 100000));
        put(d / "BAD00000.KMP", k);
        if (!s.empty()) put(d / "BAD00000.KSF", s);
        snd::Instrument x;
        const auto e = snd::buildFromKorgFolder(d, "Bad", {}, {}, &x);
        TEST(what + " -> " + e, e.find(expect) != std::string::npos);
    };
    const auto K = kmp("Bad", {{60, 127, 0, 0, "BAD00000.KSF"}});
    const auto pcm = tone(4000, 300, 5000, 7);
    refuse("compressed KSF", K, ksf({pcm, 48000, 0, 0, 3999, 0x90}), "compressed sample data");
    refuse("Reverse on", K, ksf({pcm, 48000, 0, 0, 3999, 0x40}), "Reverse is on");
    refuse("8-bit", K, ksf({pcm, 48000, 0, 0, 3999, 0x80, 8}), "8-bit data");
    refuse("2 channels in one KSF", K, ksf({pcm, 48000, 0, 0, 3999, 0x80, 16, 2}), "2 channels");
    refuse("shared sample data (SMF1)", K, ksf({pcm, 48000, 0, 0, 3999, 0x80}, "SMF1"), "shared sample data");
    refuse("unknown chunk", K, ksf({pcm, 48000, 0, 0, 3999, 0x80}, "ZZZ1"), "not in the KRONOS documentation (chunk 'ZZZ1'");
    refuse("End past the audio", K, ksf({pcm, 48000, 0, 0, 4000, 0x80}), "Start/End outside the audio");
    refuse("missing KSF", K, {}, "BAD00000.KSF not found");
    refuse("INTERNAL sample", kmp("Bad", {{60, 127, 0, 0, "INTERNAL0001"}}), {}, "built into an older Korg");
    refuse("Non Transpose over several keys", kmp("Bad", {{60, 127, 0, 0, "BAD00000.KSF", true}}), ksf({pcm, 48000, 0, 0, 3999, 0x80}), "Non Transpose");
    refuse("top keys falling", kmp("Bad", {{60, 70, 0, 0, "BAD00000.KSF"}, {50, 60, 0, 0, "BAD00000.KSF"}}), ksf({pcm, 48000, 0, 0, 3999, 0x80}), "below the previous region");
    Bytes cut = K; cut.resize(cut.size() - 30);
    refuse("truncated KMP", cut, ksf({pcm, 48000, 0, 0, 3999, 0x80}), "runs past the end");
    {   // -L/-R halves that do not match
        const fs::path d = root / "badpair";
        put(d / "A.KMP", kmp("Pad -L", {{60, 127, 0, 0, "A.KSF"}}));
        put(d / "B.KMP", kmp("Pad -R", {{62, 127, 0, 0, "B.KSF"}}));
        put(d / "A.KSF", ksf({pcm, 48000, 0, 0, 3999, 0x80}));
        put(d / "B.KSF", ksf({pcm, 48000, 0, 0, 3999, 0x80}));
        snd::Instrument x;
        TEST("-L/-R halves with different roots are refused", snd::buildFromKorgFolder(d, "Pad", {}, {}, &x).find("halves differ") != std::string::npos);
    }
    {   // SKIPPEDSAMPL leaves those keys silent and the checks say so
        const fs::path d = root / "skip";
        put(d / "S.KMP", kmp("Gap", {{48, 59, 0, 0, "S.KSF"}, {60, 71, 0, 0, "SKIPPEDSAMPL"}, {72, 127, 0, 0, "S.KSF"}}));
        put(d / "S.KSF", ksf({pcm, 48000, 0, 0, 3999, 0x80}));
        snd::Instrument x;
        const auto e = snd::buildFromKorgFolder(d, "Gap", {}, {}, &x);
        bool silent = false;
        for (const auto& w : ai_arranger::ui::SoundConversionModel::checksOf(x)) silent |= w.find("C4-B4 are silent") != std::string::npos;
        TEST("SKIPPEDSAMPL: converts, one shared sample, C4-B4 reported silent: " + e, e.empty() && x.samples.size() == 1 && x.layers[0].zones.size() == 2 && silent);
    }

    fs::remove_all(root);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
