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
#include <set>
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
        TEST("sample names: the KSF's own name (SMP1 'SYNTH') when the package can carry it, else '<layer> N' for the KSFs sharing it",
             in.samples[0].name == "SYNTH" && in.samples[1].name == "Soft Piano 2" && in.samples[2].name == "Soft Piano 3");
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
        TEST("SKIPPEDSAMPL: converts (the KSF under roots C3 and C5 = 2 samples), C4-B4 reported silent: " + e, e.empty() && x.samples.size() == 2 && x.layers[0].zones.size() == 2 && silent);
    }

    std::printf("== Codex review of 7dc072f: regressions\n");
    const auto ok = ksf({pcm, 48000, 0, 0, 3999, 0x80});
    refuse("Non Transpose, one key, original key elsewhere", kmp("Bad", {{48, 60, 0, 0, "BAD00000.KSF", true}, {48, 127, 0, 0, "BAD00000.KSF"}}), ok, "region 1");
    {   // the first region spans 0-60 there; a real one-key case: skipped 0-59, then a fixed-pitch key 60 rooted at C3
        const fs::path d = root / "fixed";
        put(d / "F.KMP", kmp("Fix", {{48, 59, 0, 0, "SKIPPEDSAMPL"}, {48, 60, 0, 0, "F.KSF", true}, {72, 127, 0, 0, "F.KSF"}}));
        put(d / "F.KSF", ok);
        snd::Instrument x;
        TEST("Non Transpose on one key whose original key differs is refused (it would play transposed)",
             snd::buildFromKorgFolder(d, "Fix", {}, {}, &x).find("Non Transpose (fixed pitch)") != std::string::npos);
        put(d / "F.KMP", kmp("Fix", {{48, 59, 0, 0, "SKIPPEDSAMPL"}, {60, 60, 0, 0, "F.KSF", true}, {72, 127, 0, 0, "F.KSF"}}));
        TEST("Non Transpose on one key rooted at that key converts (plays untransposed either way)", snd::buildFromKorgFolder(d, "Fix", {}, {}, &x).empty());
    }
    const auto pair = [&](const std::string& what, const Bytes& kl, const Bytes& kr, const Bytes& sl, const Bytes& sr) {
        const fs::path d = root / ("pair" + std::to_string(std::hash<std::string>{}(what) % 100000));
        put(d / "A.KMP", kl); put(d / "B.KMP", kr); put(d / "A.KSF", sl); put(d / "B.KSF", sr);
        snd::Instrument x;
        TEST(what, snd::buildFromKorgFolder(d, "Pad", {}, {}, &x).find("halves differ") != std::string::npos);
    };
    const auto KL = kmp("Pad -L", {{60, 127, 0, 0, "A.KSF"}}), KR = kmp("Pad -R", {{60, 127, 0, 0, "B.KSF"}});
    pair("-R Non Transpose, -L not: refused", KL, kmp("Pad -R", {{60, 127, 0, 0, "B.KSF", true}}), ok, ok);
    pair("-R +12 dB, -L not: refused", KL, KR, ok, ksf({pcm, 48000, 0, 0, 3999, 0x81}));
    pair("-R Level differs: refused", KL, kmp("Pad -R", {{60, 127, 0, 9, "B.KSF"}}), ok, ok);
    pair("-R RLP2 differs: refused", KL, kmp("Pad -R", {{60, 127, 0, 0, "B.KSF"}}, true, 5), ok, ok);
    refuse("Level byte 0x80 (-128)", kmp("Bad", {{60, 127, 0, int8_t(-128), "BAD00000.KSF"}}), ok, "level outside -99..+99");
    refuse("undocumented attribute bits on uncompressed data (0x8E)", K, ksf({pcm, 48000, 0, 0, 3999, 0x8E}), "attribute bits 3-1");
    {   // 2nd Start in use and uniform +12 dB are reported
        const fs::path d = root / "start2";
        Ksf k2{pcm, 48000, 0, 0, 3999, 0x01};   // bit 5 clear: 2nd start used
        Bytes b2 = ksf(k2);
        b2[8 + 16 + 4 + 3] = 50;   // SMP1 2nd start address (offset 20 in the body) = 50
        put(d / "S.KMP", kmp("Two", {{60, 127, 0, 0, "S.KSF"}}));
        put(d / "S.KSF", b2);
        snd::Instrument x;
        TEST("2nd Start in use: converts from Start", snd::buildFromKorgFolder(d, "Two", {}, {}, &x).empty() && x.samples[0].frames() == 4000);
        const auto n = snd::korgFolderNotes(d, x);
        TEST("REPORT: 2nd Start and +12 dB on all samples are stated",
             n.find("1 sample(s) have a 2nd Start in use: not carried") != std::string::npos && n.find("have +12 dB (all of them)") != std::string::npos);
    }
    {   // the scan reads the first 60 bytes; the conversion reads (and checks) the whole file
        const fs::path d = root / "head";
        Bytes b3 = ok;
        Bytes junk(12, 0); chunk(b3, "ZZZ1", junk);
        put(d / "H.KMP", kmp("Head", {{60, 127, 0, 0, "H.KSF"}}));
        put(d / "H.KSF", b3);
        std::vector<snd::KorgMultisample> m3;
        snd::Instrument x;
        TEST("scan (header only) accepts what it does not read; the conversion refuses the undocumented chunk",
             snd::scanKorgFolder(d, m3).empty() && snd::buildFromKorgFolder(d, "Head", {}, {}, &x).find("ZZZ1") != std::string::npos);
    }
    {   // A.KSF and a.ksf: only possible on a case-sensitive disk (Linux CI); macOS APFS default is case-insensitive
        const fs::path d = root / "case";
        put(d / "C.KMP", kmp("Case", {{60, 127, 0, 0, "A.KSF"}}));
        put(d / "A.KSF", ok);
        put(d / "a.ksf", ksf({tone(4000, 900, 5000, 8), 48000, 0, 0, 3999, 0x80}));
        size_t n = 0;
        for (const auto& e : fs::directory_iterator(d)) n += e.path().extension() != ".KMP";
        snd::Instrument x;
        if (n == 2) TEST("A.KSF and a.ksf both present: refused, not picked", snd::buildFromKorgFolder(d, "Case", {}, {}, &x).find("differing only in case") != std::string::npos);
        else std::printf("  (case-insensitive disk: A.KSF/a.ksf ambiguity cannot exist here; covered on Linux CI)\n");
    }

    std::printf("== Codex review of 166f3c5: regressions\n");
    {   // a second SMP1 after the audio would override the Loop Start the scan approved
        Bytes rdup = ksf({pcm, 48000, 0, 100, 3999});
        Bytes smp; str(smp, "X", 16); be32(smp, 0); be32(smp, 0); be32(smp, 900); be32(smp, 3999);
        chunk(rdup, "SMP1", smp);
        {
            const fs::path dp = root / "duppair";
            put(dp / "A.KMP", KL); put(dp / "B.KMP", KR); put(dp / "A.KSF", ksf({pcm, 48000, 0, 100, 3999})); put(dp / "B.KSF", rdup);
            snd::Instrument x;
            TEST("a duplicate SMP1 in the -R KSF (overriding Loop Start) is refused, not converted",
                 snd::buildFromKorgFolder(dp, "Pad", {}, {}, &x).find("B.KSF: chunk 'SMP1'") != std::string::npos);
        }
        const fs::path d = root / "dup";
        put(d / "D.KMP", kmp("Dup", {{60, 127, 0, 0, "D.KSF"}}));
        put(d / "D.KSF", rdup);
        snd::Instrument x;
        TEST("a duplicate chunk is refused by name", snd::buildFromKorgFolder(d, "Dup", {}, {}, &x).find("'SMP1' at") != std::string::npos);
        Bytes kdup = KL; Bytes mno; be32(mno, 1); chunk(kdup, "MNO1", mno);
        refuse("a duplicate chunk in a KMP", kdup, ok, "appears twice");
    }
    {   // -L without 2nd Start, -R using 2nd Start 50
        Bytes r2 = ksf({pcm, 48000, 0, 0, 3999, 0x80});   // bit 5 clear: 2nd start used
        r2[31] = 50;
        pair("-R uses a 2nd Start, -L does not: refused", KL, KR, ksf({pcm, 48000, 0, 0, 3999, 0xA0}), r2);
    }
    {   // a KSF that does not start SMP1 + SMD1 is refused by the scan without reading its audio
        const fs::path d = root / "layout";
        Bytes odd; Bytes sno; be32(sno, 0); chunk(odd, "SNO1", sno);
        const Bytes rest = ok; odd.insert(odd.end(), rest.begin(), rest.end());
        put(d / "O.KMP", kmp("Odd", {{60, 127, 0, 0, "O.KSF"}}));
        put(d / "O.KSF", odd);
        std::vector<snd::KorgMultisample> m4;
        TEST("scan refuses a KSF not starting SMP1 then SMD1", snd::scanKorgFolder(d, m4).find("does not start with SMP1") != std::string::npos);
    }

    std::printf("== Codex review of fd2eb4e: regressions\n");
    {   // only -L present
        const fs::path d = root / "half";
        put(d / "A.KMP", KL); put(d / "A.KSF", ok);
        snd::Instrument x;
        TEST("a -L half without its -R is refused, not converted as mono",
             snd::buildFromKorgFolder(d, "Pad", {}, {}, &x).find("-R half is not in the folder") != std::string::npos);
    }
    {   // _L/_R names are two mono multisamples (only -L/-R is the documented stereo naming)
        const fs::path d = root / "under";
        put(d / "A.KMP", kmp("Pad_L", {{60, 127, 0, 0, "A.KSF"}})); put(d / "B.KMP", kmp("Pad_R", {{60, 127, 0, 0, "B.KSF"}}));
        put(d / "A.KSF", ok); put(d / "B.KSF", ok);
        std::vector<snd::KorgMultisample> m5;
        TEST("Pad_L and Pad_R stay two mono multisamples", snd::scanKorgFolder(d, m5).empty() && m5.size() == 2 && m5[0].kmpR.empty() && m5[1].kmpR.empty());
    }
    {   // one KSF in two regions with different original keys -> two samples, each with its root; the package writes
        const fs::path d = root / "reuse";
        put(d / "R.KMP", kmp("Reuse", {{48, 59, 0, 0, "R.KSF"}, {60, 127, 0, 0, "R.KSF"}}));
        put(d / "R.KSF", ok);
        snd::Instrument x;
        const auto e = snd::buildFromKorgFolder(d, "Reuse", {}, {}, &x);
        TEST("a KSF reused under two original keys becomes two samples with those roots: " + e,
             e.empty() && x.samples.size() == 2 && x.samples[0].root == 48 && x.samples[1].root == 60);
        std::string werr;
        TEST("... and the package is written: " + werr, !snd::writePackage(x, root / "out" / "Reuse", snd::korgFolderNotes(d, x), &werr).empty());
        put(d / "R.KMP", kmp("Reuse", {{48, 59, 0, 0, "R.KSF"}, {48, 127, 0, 0, "R.KSF"}}));
        TEST("a KSF reused under the same original key stays one sample", snd::buildFromKorgFolder(d, "Reuse", {}, {}, &x).empty() && x.samples.size() == 1);
    }
    {   // 2nd Start in use but outside the audio
        Bytes bad2 = ksf({pcm, 48000, 0, 0, 3999, 0x80});   // bit 5 clear: in use
        bad2[28] = bad2[29] = bad2[30] = bad2[31] = 0xFF;
        refuse("2nd Start 0xFFFFFFFF in use", K, bad2, "2nd Start outside the audio");
    }

    std::printf("== SMF1 shared sample data (p.1159): SMD1 holds only its header, the audio is in the KSF that SMF1 names\n");
    {
        const auto src = tone(6000, 440, 9000, 11);
        const Ksf sh{src, 48000, 200, 1000, 4999, 0x00};   // its own Start/Loop/End over the shared audio, loop on
        const auto dir = [&](const std::string& n, const Bytes& shared, const std::string& kmpRef = "B.KSF") {
            const fs::path d = root / ("smf1-" + n);
            put(d / "S.KMP", kmp("Shared", {{60, 127, 0, 0, kmpRef}}));
            put(d / "A.KSF", ksf({src, 48000, 0, 0, 5999, 0x80}));
            put(d / "B.KSF", shared);
            return d;
        };
        snd::Instrument x;
        const fs::path d = dir("ok", ksfShared(sh, "a.ksf"));   // FAT: the name matches without case
        const auto e = snd::buildFromKorgFolder(d, "Shared", {}, {}, &x);
        TEST("a shared KSF converts with the named file's audio and its own Start/Loop/End: " + e,
             e.empty() && x.samples.size() == 1 && x.samples[0].frames() == 4800 && x.samples[0].pcm.front() == src[200] &&
             x.samples[0].pcm.back() == src[4999] && x.samples[0].looped && x.samples[0].loopStart == 800 && x.samples[0].loopEnd == 4800);
        const auto smf = [&](const std::string& what, const Bytes& shared, const std::string& expect) {
            snd::Instrument y;
            const auto r = snd::buildFromKorgFolder(dir(std::to_string(std::hash<std::string>{}(what) % 100000), shared), "Shared", {}, {}, &y);
            TEST(what + " -> " + r, r.find(expect) != std::string::npos && r.find("B.KSF") != std::string::npos);
        };
        smf("SMF1 with a parent path", ksfShared(sh, "../A.KSF"), "not a plain file name");
        smf("SMF1 with a sub-folder path", ksfShared(sh, "sub/A.KSF"), "not a plain file name");
        smf("SMF1 with a backslash path", ksfShared(sh, "..\\A.KSF"), "not a plain file name");
        smf("SMF1 naming a missing file", ksfShared(sh, "GONE.KSF"), "GONE.KSF not found");
        smf("SMF1 naming the file itself", ksfShared(sh, "B.KSF"), "names itself");
        smf("SMF1 naming SKIPPEDSAMPL", ksfShared(sh, "SKIPPEDSAMPL"), "skipped sample");
        smf("SMF1 naming an INTERNAL sample", ksfShared(sh, "INTERNAL0001"), "built into an older Korg");
        smf("SMF1 with a different rate", ksfShared({src, 44100, 200, 1000, 4999, 0x00}, "A.KSF"), "rate or sample count differs");
        smf("SMF1 with a different sample count", ksfShared({tone(5000, 440, 9000, 11), 48000, 200, 1000, 4999, 0x00}, "A.KSF"), "rate or sample count differs");
        smf("SMF1 with an End past the shared audio", ksfShared({src, 48000, 200, 1000, 6000, 0x00}, "A.KSF"), "Start/End outside the audio");
        {   // SMF1 plus audio in SMD1: p.1159 says the data is empty then
            Bytes both = ksf({src, 48000, 0, 0, 5999, 0x80});
            Bytes nm; str(nm, "A.KSF", 12); chunk(both, "SMF1", nm);
            smf("SMF1 and audio in SMD1", both, "SMD1 also holds audio");
        }
        {   // SMD1 without audio and no SMF1
            Bytes empty;
            Bytes smp; str(smp, "X", 16); be32(smp, 0); be32(smp, 0); be32(smp, 0); be32(smp, 99);
            chunk(empty, "SMP1", smp);
            Bytes smd; be32(smd, 48000); smd.push_back(0x80); smd.push_back(0); smd.push_back(1); smd.push_back(16); be32(smd, 100);
            chunk(empty, "SMD1", smd);
            smf("SMD1 without audio and no SMF1", empty, "no SMF1 chunk naming the file");
        }
        {   // a chain: B shares C's data, C shares A's
            const fs::path c = dir("chain", ksfShared(sh, "C.KSF"));
            put(c / "C.KSF", ksfShared(sh, "A.KSF"));
            snd::Instrument y;
            const auto r = snd::buildFromKorgFolder(c, "Shared", {}, {}, &y);
            TEST("SMF1 naming another shared KSF (a chain) is refused: " + r, r.find("SMF1 names 'C.KSF', which is refused: shared sample data") != std::string::npos);
        }
        {   // a link named like the KSF, pointing outside the folder
            const fs::path l = dir("link", ksfShared(sh, "L.KSF"));
            std::error_code ec;
            fs::create_symlink(root / "smf1-ok" / "A.KSF", l / "L.KSF", ec);
            snd::Instrument y;
            const auto r = snd::buildFromKorgFolder(l, "Shared", {}, {}, &y);
            TEST("SMF1 naming a symbolic link is refused: " + r, !ec && r.find("it is a link") != std::string::npos);
        }
        {   // SMP1 SNO1 SMF1 SMD1: chunks in another order than p.1159 lists; the header scan keeps refusing that start
            std::vector<snd::KorgMultisample> m;
            TEST("a shared KSF in another chunk order is refused by the scan (documented order only)",
                 snd::scanKorgFolder(dir("order", ksfShared(sh, "A.KSF", true)), m).find("does not start with SMP1") != std::string::npos);
        }
        {   // the KSF-only route reads each file on its own: it names the chunk and the file it points at
            snd::KsfHead h;
            snd::Sample s;
            const auto r = snd::readKsf(ksfShared(sh, "A.KSF"), &h, &s);
            TEST("without the file's folder, SMF1 is refused naming the chunk and its target: " + r,
                 r.find("shared sample data (chunk SMF1 names A.KSF)") != std::string::npos);
        }
    }

    std::printf("== Fallback sample names always fit 20 characters and never loop (Codex re-check, P1)\n");
    {   // 8 layers x 128 regions of one KSF under 128 original keys = 1024 samples; layer names of 16+ characters, so the
        // old "<16 chars> N" fallback reached 21 characters at N = 1000 and retried forever
        const fs::path d = root / "many";
        std::vector<std::string> names;
        std::vector<uint8_t> splits;
        for (int l = 0; l < 8; ++l) {
            std::vector<Reg> rs;
            for (int k = 0; k < 128; ++k) rs.push_back({uint8_t(k), uint8_t(k), 0, 0, "M" + std::to_string(l) + ".KSF"});
            names.push_back("Long Layer Name " + std::string(1, char('A' + l)));
            if (l) splits.push_back(uint8_t(16 * l));
            put(d / ("M" + std::to_string(l) + ".KMP"), kmp(names.back(), rs));
            put(d / ("M" + std::to_string(l) + ".KSF"), ksf({tone(64, 440, 5000, l), 48000, 0, 0, 63, 0x80}));
        }
        snd::Instrument x;
        const auto e = snd::buildFromKorgFolder(d, "Many", names, splits, &x);
        bool fit = true;
        std::set<std::string> seen;
        for (const auto& s : x.samples) fit &= snd::nameOk(s.name) && seen.insert(s.name).second;
        TEST("1024 samples from 16+ character layer names convert, every name 1-20 characters and unique: " + e,
             e.empty() && x.samples.size() == 1024 && fit && x.samples[1000].name == "Long Layer Name 1001");
        const auto none = [](const std::string&) { return true; };
        TEST("no free name: the helper gives up after its bound instead of looping",
             snd::korg::sampleName("", "Long Layer Name A", 1000, 5, none).empty());
        const auto first5 = [](const std::string& n) { return n.size() > 2 && n.substr(n.size() - 2) < " 6" && n.substr(n.size() - 2, 1) == " "; };
        TEST("taken candidates are skipped: ' 1'..' 5' taken gives 'Long Layer Name A 6'",
             snd::korg::sampleName("", "Long Layer Name A", 1, 10, first5) == "Long Layer Name A 6");
    }

    fs::remove_all(root);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
