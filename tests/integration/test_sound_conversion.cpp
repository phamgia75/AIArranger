// Sound conversion S1-S3 (docs/SOUND_CONVERSION_ROADMAP.md): instrument model limits, the original
// additive source (loops, pitch), the SF2 writer (re-read by the independent-code sf2::check parser,
// determinism, malformed prefixes), the WAV writer. SOFTWARE evidence only: nothing here shows the
// Pa5X accepts the files (that is S6).

#include <csignal>
#include <sys/resource.h>
#include "media/sound/instrument.h"
#include "media/sound/additive_source.h"
#include "media/sound/sf2_writer.h"
#include "media/sound/wav_writer.h"
#include "media/sound/txprog_writer.h"
#include "media/sound/package_writer.h"
#include <chrono>
#include "media/sf2_check.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ai_arranger::media;
using namespace ai_arranger::media::sound;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static uint32_t rd32(const std::vector<uint8_t>& b, size_t o) { return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24; }
static size_t findChunk(const std::vector<uint8_t>& b, const char* id) {
    for (size_t o = 0; o + 4 <= b.size(); ++o) if (!std::memcmp(&b[o], id, 4)) return o;
    return std::string::npos;
}

// |DFT| of x at bin `bin` over N samples (Goertzel).
static double mag(const int16_t* x, size_t N, double bin) {
    const double w = 2 * 3.14159265358979323846 * bin / N, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t n = 0; n < N; ++n) { const double s0 = x[n] + c * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
}

int main() {
    std::printf("Test: Sound conversion S1-S5\n");
    double worstCents = 99;
    const Instrument org = buildDemoOrgan(&worstCents);

    // ── S1 model ──
    TEST("demo organ is valid against the Pa5X limits", validate(org).empty());
    {   // regression (Codex review, drum kit round): the same unescaped-control-character gap existed for Sounds
        Instrument c = org; c.name = "Org\x02";
        Instrument d = org; d.layers[0].name = "Soft\x1F";
        Instrument f = org; f.samples[0].name = "S\x80";
        TEST("control/non-ASCII characters in instrument, layer or sample names refused (XML-safe)",
             !validate(c).empty() && !validate(d).empty() && !validate(f).empty() && writeTxProg(c).empty());
    }
    TEST("2 velocity layers x 19 zones (C1..C7 every 4 semitones), 38 samples",
         org.layers.size() == 2 && org.layers[0].zones.size() == 19 && org.layers[1].zones.size() == 19 && org.samples.size() == 38);
    bool cover = true;
    for (const auto& L : org.layers) {
        int next = 0;
        for (const auto& z : L.zones) { cover &= z.lo == next; next = z.hi + 1; }
        cover &= next == 128;
    }
    TEST("each layer's zones tile keys 0-127 exactly (no gap, no overlap)", cover);
    {
        Instrument bad = org;
        bad.samples[0].rate = 8000;
        bad.samples[1].pcm.resize(1048577);
        bad.samples[2].loopEnd = bad.samples[2].loopStart + 10;
        bad.layers[0].zones[1].lo = bad.layers[0].zones[0].hi;   // overlap
        bad.layers.push_back(bad.layers[0]);                      // 3 layers
        const auto e = validate(bad);
        auto has = [&](const char* s) { for (const auto& m : e) if (m.find(s) != std::string::npos) return true; return false; };
        TEST("validate names each violation: rate, >1 048 576 frames, short loop, overlap, velocity zones not tiling",
             has("rate outside") && has("1 048 576") && has("shorter than 32") && has("overlap") && has("velocity zones must tile"));
        Instrument many = org;
        many.layers.clear();
        for (int l = 0; l < 25; ++l) { Layer L = org.layers[0]; L.velLo = uint8_t(l * 5); L.velHi = uint8_t(l == 24 ? 127 : l * 5 + 4); many.layers.push_back(L); }
        Instrument gap = org; gap.layers[1].velLo = 91;
        Instrument odd = org; odd.samples[3].channels = 2; odd.samples[3].pcm.pop_back();
        TEST("validate: > 24 layers, a velocity gap, stereo audio with half a frame",
             [&] { auto e = validate(many); for (auto& m : e) if (m.find("1-24") != std::string::npos) return true; return false; }() &&
             !validate(gap).empty() && !validate(odd).empty());
        TEST("the SF2 writer refuses an invalid instrument", writeSf2(bad).empty());
    }

    // ── S2 source: loops and pitch ──
    TEST("worst rendered pitch error < 0.1 cent", worstCents < 0.1);
    bool seamless = true, attackOk = true, inRange = true;
    for (const auto& s : org.samples) {
        for (size_t i = 0; i < 64 && s.loopEnd + i < s.pcm.size(); ++i) seamless &= s.pcm[s.loopEnd + i] == s.pcm[s.loopStart + i];
        attackOk &= s.pcm[0] == 0 && s.loopStart >= 16;
        inRange &= s.pcm.size() <= 1048576 && s.loopEnd + 8 <= s.pcm.size();
    }
    TEST("every loop is seamless: the frames after loopEnd repeat the loop start exactly", seamless);
    TEST("attack starts at 0 (no click) and the loop starts after it", attackOk);
    TEST("every sample fits 1 048 576 frames with valid frames after the loop", inRange);
    {   // the dominant spectral line over one loop is the requested note's fundamental
        bool fund = true;
        for (const auto& s : org.samples) {
            const size_t L = s.loopEnd - s.loopStart;
            const double k = noteHz(s.root) * L / s.rate;
            const int16_t* x = s.pcm.data() + s.loopStart;
            const double atK = mag(x, L, std::round(k));
            for (double b : {std::round(k) - 1, std::round(k) + 1, std::round(k / 2)})   // no energy off the harmonic grid
                if (b > 0 && b < L / 2.0) fund &= atK > 100 * mag(x, L, b);
            for (double b : {std::round(2 * k), std::round(3 * k)})                     // fundamental is the strongest line
                if (b < L / 2.0) fund &= atK > mag(x, L, b);
        }
        TEST("every sample: the fundamental (bin k) is the strongest line; neighbours and the octave below < 1%", fund);
    }

    // ── S3 SF2 writer ──
    const auto sf2bytes = writeSf2(org);
    TEST("SF2 written", !sf2bytes.empty());
    TEST("RIFF size field == file size - 8, form 'sfbk', ifil 2.04",
         sf2bytes.size() > 12 && rd32(sf2bytes, 4) == sf2bytes.size() - 8 && !std::memcmp(&sf2bytes[8], "sfbk", 4) &&
         [&] { size_t o = findChunk(sf2bytes, "ifil"); return o != std::string::npos && sf2bytes[o + 8] == 2 && sf2bytes[o + 10] == 4; }());
    const sf2::Report rep = sf2::check(sf2bytes);
    TEST("sf2::check reads it: 2 instruments, 38 samples, 0 overlapping instruments",
         rep.ok && rep.instruments.size() == 2 && rep.samples.size() == 38 && rep.overlapping() == 0);
    bool zonesMatch = rep.ok && rep.instruments.size() == 2;
    for (size_t l = 0; zonesMatch && l < 2; ++l) {
        zonesMatch &= rep.instruments[l].name == org.layers[l].name && rep.instruments[l].zones.size() == org.layers[l].zones.size();
        for (size_t z = 0; zonesMatch && z < org.layers[l].zones.size(); ++z) {
            const auto& a = rep.instruments[l].zones[z];
            const auto& b = org.layers[l].zones[z];
            zonesMatch &= a.key.lo == b.lo && a.key.hi == b.hi && a.sample == b.sample;
        }
    }
    TEST("re-read zones equal the model (names, key ranges, sample per zone)", zonesMatch);
    {   // shdr: start/end/loop of sample 0 and 1, rate, root
        const size_t o = findChunk(sf2bytes, "shdr") + 8;
        const Sample& s0 = org.samples[0];
        const size_t r1 = o + 46;
        const uint32_t start1 = static_cast<uint32_t>(s0.pcm.size()) + 46;
        TEST("shdr: sample 0 at 0, end = frames, loop = model loop; sample 1 after 46 pad frames; rate, root, mono",
             rd32(sf2bytes, o + 20) == 0 && rd32(sf2bytes, o + 24) == s0.pcm.size() && rd32(sf2bytes, o + 28) == s0.loopStart &&
             rd32(sf2bytes, o + 32) == s0.loopEnd && rd32(sf2bytes, o + 36) == 48000 && sf2bytes[o + 40] == s0.root &&
             sf2bytes[o + 44] == 1 && rd32(sf2bytes, r1 + 20) == start1);
    }
    TEST("deterministic: same instrument -> identical bytes", writeSf2(buildDemoOrgan()) == sf2bytes);
    {
        bool clean = true;
        std::vector<size_t> cuts;   // every byte of the headers, then 400 cuts spread over the rest
        for (size_t n = 0; n < 512; ++n) cuts.push_back(n);
        for (size_t n = 512; n < sf2bytes.size(); n += sf2bytes.size() / 400) cuts.push_back(n);
        for (size_t n = sf2bytes.size() - 2048; n < sf2bytes.size(); n += 7) cuts.push_back(n);   // pdta lives at the end
        for (size_t n : cuts) {
            const std::vector<uint8_t> pre(sf2bytes.begin(), sf2bytes.begin() + n);
            clean &= !sf2::check(pre).ok || n > sf2bytes.size() - 64;
        }
        TEST("truncated copies are refused by the reader (bounds-checked; ASan in CI)", clean);
    }

    // ── S5 TXPROG package ──
    {
        const auto pkg = writeTxProg(org);
        const std::string x = pkg.empty() ? "" : std::string(pkg[0].bytes.begin(), pkg[0].bytes.end());
        auto count = [&](const std::string& s) { size_t n = 0; for (size_t p = x.find(s); p != std::string::npos; p = x.find(s, p + 1)) ++n; return n; };
        TEST("TXPROG package: .txprog first + 38 WAVs in '<name> Samples/' under one folder",
             pkg.size() == 39 && pkg[0].path == "AIA Demo Organ/AIA Demo Organ.txprog" &&
             pkg[1].path == "AIA Demo Organ/AIA Demo Organ Samples/AIA OrgS C1.wav");
        TEST("UTF-8 BOM, program namespace, program name",
             x.rfind("\xEF\xBB\xBF<?xml", 0) == 0 && x.find("xmlns:tx=\"http://www.tx16wx.com/3.0/program\"") != std::string::npos &&
             x.find("tx:name=\"AIA Demo Organ\"") != std::string::npos);
        TEST("38 waves, 2 groups, 38 regions, 38 forward loops", count("<tx:wave ") == 38 && count("<tx:group ") == 2 &&
             count("<tx:region ") == 38 && count("tx:mode=\"Forward\"") == 38);
        const Sample& s0 = org.samples[0];
        TEST("wave 0: relative path, root C1, loop end exclusive (= model loopEnd)",
             x.find("tx:path=\"AIA Demo Organ Samples/AIA OrgS C1.wav\" tx:id=\"0\" tx:root=\"C1\"") != std::string::npos &&
             x.find("tx:name=\"1\" tx:end=\"" + std::to_string(s0.loopEnd) + "\" tx:mode=\"Forward\" tx:start=\"" +
                    std::to_string(s0.loopStart) + "\"") != std::string::npos);
        TEST("mirrors Korg's example: empty loop 2 + full-range bounds per wave, DFD regions, UUID-shaped soundshape id, format stamp created-by 30601, program bounds before the soundshape",
             count("tx:name=\"2\" tx:end=\"1\" tx:mode=\"None\"") == 38 && count("tx:low-key=\"C-1\" tx:high-key=\"G9\"") >= 38 &&
             count("tx:mode=\"DFD\"") == 38 && x.find("tx:soundshape=\"a1a0a1a0-0000-4000-8000-") != std::string::npos &&
             x.find("tx:created-by=\"30601\"") != std::string::npos &&
             x.find("</tx:wave>\n   <tx:bounds tx:high-vel=\"127\" tx:low-key=\"C-1\" tx:high-key=\"G9\" tx:low-vel=\"0\"/>\n   <tx:soundshape") != std::string::npos);
        TEST("key names span C-1..G9; Low group velocities 0-89, High 90-127",
             x.find("tx:low-key=\"C-1\" tx:high-key=\"C#1\" tx:low-vel=\"0\" tx:high-vel=\"89\"") != std::string::npos &&
             x.find("tx:low-key=\"A#6\" tx:high-key=\"G9\" tx:low-vel=\"90\" tx:high-vel=\"127\"") != std::string::npos &&
             txw::noteName(60) == "C4" && txw::noteName(127) == "G9");
        TEST("each packaged WAV equals writeWav of its sample", pkg.size() == 39 && pkg[38].bytes == writeWav(org.samples[37]));
        TEST("deterministic package", writeTxProg(buildDemoOrgan()).size() == 39 && writeTxProg(buildDemoOrgan())[0].bytes == pkg[0].bytes);
        Instrument r = org; r.layers[0].zones[0].root = 30;
        Instrument d = org; d.samples[1].name = d.samples[0].name;
        Instrument q = org; q.name = "A<&\"B"; q.layers[0].name = "x'y";
        const auto qp = writeTxProg(q);
        const std::string qx = qp.empty() ? "" : std::string(qp[0].bytes.begin(), qp[0].bytes.end());
        Instrument nn = org; nn.name = "";
        Instrument nl = org; nl.name = std::string(21, 'x');
        TEST("regression: empty or > 20-char instrument name is invalid (an empty name made the package path '/.txprog', absolute)",
             !validate(nn).empty() && writeTxProg(nn).empty() && writeSf2(nn).empty() && !validate(nl).empty());
        TEST("refuses: zone root != sample root, duplicate sample file names, invalid instrument",
             writeTxProg(r).empty() && writeTxProg(d).empty() && writeTxProg(Instrument{}).empty());
        TEST("names are XML-escaped and file-safe",
             qx.find("tx:name=\"A&lt;&amp;&quot;B\"") != std::string::npos && qx.find("tx:name=\"x&apos;y\"") != std::string::npos &&
             qp[0].path == "A___B/A___B.txprog");
    }

    // ── stereo + more than 2 layers ──
    {
        Instrument st = org;   // stereo copy of the organ: R = L at 0.5, so channels are told apart
        for (auto& sm : st.samples) {
            std::vector<int16_t> lr;
            for (int16_t v : sm.pcm) { lr.push_back(v); lr.push_back(static_cast<int16_t>(v / 2)); }
            sm.pcm = lr; sm.channels = 2;
        }
        Layer mid = st.layers[0]; mid.name = "AIA Organ Mid"; mid.velLo = 60; mid.velHi = 89;
        st.layers[0].velHi = 59;
        st.layers.insert(st.layers.begin() + 1, mid);
        TEST("stereo, 3 velocity layers (0-59, 60-89, 90-127): valid", validate(st).empty() && st.samples[0].frames() == org.samples[0].pcm.size());
        const auto b = writeSf2(st);
        const auto c = sf2::check(b);
        TEST("stereo SF2: 76 SF2 samples (L/R), 3 instruments, linked pairs not reported as overlaps",
             c.ok && c.samples.size() == 76 && c.instruments.size() == 3 && c.overlapping() == 0 && c.instruments[0].zones.size() == 38);
        {
            const size_t o = findChunk(b, "shdr") + 8;
            auto u16at = [&](size_t q) { return uint32_t(b[q] | b[q + 1] << 8); };
            const size_t r0 = o, r1 = o + 46;
            bool smplOk = true;   // left channel of sample 0 is the mono organ sample 0, right is half of it
            const size_t sd = findChunk(b, "smpl") + 8, startR = rd32(b, r1 + 20);
            for (size_t f = 0; f < org.samples[0].pcm.size(); f += 501)
                smplOk &= int16_t(u16at(sd + 2 * f)) == org.samples[0].pcm[f] && int16_t(u16at(sd + 2 * (startR + f))) == org.samples[0].pcm[f] / 2;
            TEST("stereo SF2: L/R names, link each other, types left(4)/right(2); data de-interleaved per channel",
                 std::string(reinterpret_cast<const char*>(&b[r0])) == "AIA OrgS C1-L" &&
                 std::string(reinterpret_cast<const char*>(&b[r1])) == "AIA OrgS C1-R" && u16at(r0 + 42) == 1 && u16at(r0 + 44) == 4 && u16at(r1 + 42) == 0 && u16at(r1 + 44) == 2 && smplOk);
        }
        const auto pkg = writeTxProg(st);
        const std::string x = pkg.empty() ? "" : std::string(pkg[0].bytes.begin(), pkg[0].bytes.end());
        TEST("stereo TXPROG: stereo WAVs (fmt channels 2, block align 4), 3 groups with velocity 0-59 / 60-89 / 90-127",
             pkg.size() == 39 && pkg[1].bytes[22] == 2 && pkg[1].bytes[32] == 4 &&
             x.find("tx:low-vel=\"60\" tx:high-vel=\"89\"") != std::string::npos && x.find("tx:low-vel=\"0\" tx:high-vel=\"59\"") != std::string::npos);
    }

    // ── S4/S8 package folder ──
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() /
            ("aiarr_pkg_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::string err;
        const std::string rep = writePackage(org, dir, "", &err);
        size_t files = 0;
        if (fs::is_directory(dir)) for (const auto& e : fs::recursive_directory_iterator(dir)) files += e.is_regular_file();
        TEST("package: .sf2 + .txprog + 38 tx WAVs + 38 wav/ + REPORT + RESOURCES = 80 files; report lists 4 GB use",
             !rep.empty() && files == 80 && fs::exists(dir / "AIA_Demo_Organ.sf2") &&
             fs::exists(dir / "wav" / "AIA_Organ_Soft_024_000_025.wav") && rep.find("of the 4 GB User Sample memory)\n\nLayer 0  velocity 0-89  AIA Organ Soft\n") != std::string::npos);
        TEST("REPORT states the 4/8 GB budget and irreversible compression", rep.find("8 GB") != std::string::npos && rep.find("cannot be undone") != std::string::npos);
        std::string err2;
        TEST("package refuses an existing folder and writes nothing", writePackage(org, dir, "", &err2).empty() &&
             err2.find("already exists") != std::string::npos);
        TEST("package published by rename: no INCOMPLETE folder left", !fs::exists(dir.string() + ".INCOMPLETE"));
        const fs::path d2 = dir.string() + " 2";
        fs::create_directories(d2.string() + ".INCOMPLETE/wav");
        TEST("an unfinished earlier write (NAME.INCOMPLETE) is refused and left alone",
             writePackage(org, d2, "", &err2).empty() && err2.find("did not finish") != std::string::npos && !fs::exists(d2) &&
             fs::is_empty(d2.string() + ".INCOMPLETE/wav") && outputFolderRefusal(d2).find(".INCOMPLETE already exists") != std::string::npos);
        std::error_code ec0;
        const fs::path pub = dir.string() + "-pub";
        fs::create_directories(pub / "work/x");
        fs::create_directories(pub / "taken");
        TEST("publishing never replaces a folder that appeared", !publishNewFolder(pub / "work", pub / "taken") &&
             fs::exists(pub / "work/x") && fs::is_empty(pub / "taken") && publishNewFolder(pub / "work", pub / "new") &&
             fs::exists(pub / "new/x"));
        const fs::path d3 = dir.string() + " 3";
        TEST("a destination given with a trailing slash is written beside it and published under its name",
             !writePackage(org, d3.string() + "/", "", &err2).empty() && fs::exists(d3 / "REPORT.txt") && !fs::exists(d3.string() + ".INCOMPLETE") &&
             !fs::exists(d3 / ".INCOMPLETE"));
        const fs::path d5 = dir.string() + " 5";
        TEST("...and one ending in /. too", !writePackage(org, d5.string() + "/.", "", &err2).empty() && fs::exists(d5 / "REPORT.txt") &&
             !fs::exists(d5.string() + ".INCOMPLETE") && !fs::exists(d5 / "..INCOMPLETE") && folderPath("/a/b/.") == "/a/b" && folderPath("/a/b/") == "/a/b");
        fs::remove_all(d5, ec0);
        {   // a full disk (here a file-size limit): the write fails, nothing is published under the package's name
            const fs::path d4 = dir.string() + " 4";
            struct rlimit old {};
            ::getrlimit(RLIMIT_FSIZE, &old);
            const auto prev = std::signal(SIGXFSZ, SIG_IGN);
            struct rlimit lim = old;
            lim.rlim_cur = 1000;   // smaller than REPORT.txt and every WAV
            ::setrlimit(RLIMIT_FSIZE, &lim);
            const std::string r4 = writePackage(org, d4, "", &err2);
            ::setrlimit(RLIMIT_FSIZE, &old);
            std::signal(SIGXFSZ, prev);
            TEST("a write that runs out of space fails and leaves only NAME.INCOMPLETE", r4.empty() && err2.find("write failed") != std::string::npos &&
                 !fs::exists(d4) && fs::is_directory(d4.string() + ".INCOMPLETE"));
            fs::remove_all(d4.string() + ".INCOMPLETE", ec0);
        }
        fs::remove_all(d3, ec0);
        {   // the pre-publication check: exact files, lengths and SHA-256, and the package must load back
            const fs::path d6 = dir.string() + " 6";
            pkgio::FileSet fsx{d6};
            fsx.save("a.txt", std::string("alpha"));
            fsx.save("sub/b.bin", std::string(3000, 'b'));
            TEST("check: files as written but no RESOURCES.tsv -> does not load back", fsx.ok && fsx.check().find("does not load back") != std::string::npos);
            std::ofstream(d6 / "a.txt") << "alphA";
            TEST("check: same length, other bytes -> caught", fsx.check() == "a.txt does not read back as written");
            std::ofstream(d6 / "a.txt") << "alph";
            TEST("check: other length -> caught", fsx.check() == "a.txt has 4 bytes, 5 written");
            std::ofstream(d6 / "a.txt") << "alpha";
            std::ofstream(d6 / "extra.txt") << "x";
            TEST("check: a file nobody wrote -> caught", fsx.check() == "unexpected file extra.txt");
            fs::remove(d6 / "extra.txt");
            fs::remove(d6 / "sub/b.bin");
            TEST("check: a missing file -> caught", fsx.check() == "sub/b.bin is missing");
            fs::remove_all(d6, ec0);
        }
        {   // a failure that shows only when the file is closed (the last buffered bytes): never published
            Instrument tiny; tiny.name = "Tiny";
            Sample s; s.name = "Tiny C4"; s.pcm.assign(64, 1000);
            tiny.samples.push_back(s);
            Layer l; l.name = "Tiny"; Zone z; z.sample = 0; l.zones.push_back(z); tiny.layers.push_back(l);
            const fs::path d7 = dir.string() + " 7", d8 = dir.string() + " 8";
            struct rlimit old {};
            ::getrlimit(RLIMIT_FSIZE, &old);
            const auto prev = std::signal(SIGXFSZ, SIG_IGN);
            struct rlimit lim = old;
            lim.rlim_cur = 1800;   // every file of this package is smaller except the .txprog (about 2 KB), which fits in the stream buffer
            ::setrlimit(RLIMIT_FSIZE, &lim);
            pkgio::FileSet one{d8};
            one.save("big.txt", std::string(2000, 'x'));
            const std::string r7 = writePackage(tiny, d7, "", &err2);
            ::setrlimit(RLIMIT_FSIZE, &old);
            std::signal(SIGXFSZ, prev);
            TEST("a write that fails only at close is reported (stream closed before it is checked)", !one.ok);
            TEST("...and a package whose .txprog fails at close is not published, only NAME.INCOMPLETE",
                 r7.empty() && err2.find("write failed") != std::string::npos && !fs::exists(d7) && fs::is_directory(d7.string() + ".INCOMPLETE"));
            TEST("the same tiny package writes and checks without the limit", !writePackage(tiny, d8.string() + "-ok", "", &err2).empty() &&
                 fs::exists(d8.string() + "-ok/REPORT.txt"));
            for (const auto& p : {d7.string() + ".INCOMPLETE", d8.string(), d8.string() + "-ok"}) fs::remove_all(p, ec0);
        }
        Instrument bad = org; bad.layers.clear();
        TEST("package refuses an invalid instrument before creating anything",
             writePackage(bad, dir / "x", "", &err2).empty() && !fs::exists(dir / "x"));
        std::error_code ec;
        fs::remove_all(dir, ec);   // our own unique temp folder
        fs::remove_all(pub, ec); fs::remove_all(d2, ec); fs::remove_all(d2.string() + ".INCOMPLETE", ec);
    }

    // ── WAV ──
    {
        const auto w = writeWav(org.samples[5]);
        const size_t sm = findChunk(w, "smpl");
        TEST("WAV: RIFF size, 48 kHz mono 16-bit, smpl unity note = root, loop end inclusive",
             rd32(w, 4) == w.size() - 8 && rd32(w, 24) == 48000 && w[22] == 1 && w[34] == 16 &&
             sm != std::string::npos && rd32(w, sm + 8 + 12) == org.samples[5].root &&
             rd32(w, sm + 8 + 28) == 1 && rd32(w, sm + 8 + 44) == org.samples[5].loopStart &&
             rd32(w, sm + 8 + 48) == org.samples[5].loopEnd - 1);
    }

    std::printf("    demo organ: %zu samples, %.1f MB of 16-bit audio (Pa5X User Sample memory 4 GB), worst %.4f cents\n",
                org.samples.size(), sampleBytes(org) / 1e6, worstCents);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
