// Instrument -> Korg-format .KMP/.KSF folder (media/sound/korg_kmp_writer.h, EXPERIMENTAL, L1). Synthetic audio only.
// Every folder is read back through the existing reader (korg_kmp_source.h): zones, roots, tune, loops, rate, stereo
// -L/-R pairs and audio must arrive exactly; names must fit the documented fields (KRONOS Parameter Guide En 3
// p.1158-1159: KSF file name 12 bytes incl. period and extension, multisample name 16/24, sample name 16/24); inputs
// the documentation cannot hold are refused by name, and a refused write leaves nothing behind.

#include "media/sound/korg_kmp_writer.h"
#include "../support/korg_fixture.h"
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;
namespace snd = ai_arranger::media::sound;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } } while (0)

static snd::Sample sample(const std::string& name, uint32_t rate, uint8_t ch, size_t frames, double f, bool looped, int salt) {
    snd::Sample s;
    s.name = name; s.rate = rate; s.channels = ch;
    const auto l = korgfix::tone(frames, f, 9000, salt), r = korgfix::tone(frames, f * 1.01, 7000, salt + 1);
    for (size_t i = 0; i < frames; ++i) { s.pcm.push_back(l[i]); if (ch == 2) s.pcm.push_back(r[i]); }
    if (looped) { s.looped = true; s.loopStart = uint32_t(frames / 2); s.loopEnd = uint32_t(frames - 100); }   // audio after the loop
    return s;
}

static std::vector<uint8_t> bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("kmp-writer-" + std::to_string(getpid()));
    fs::remove_all(root);

    // Two mono layers (gaps between zones, tune at both limits, a level offset, two rates, one shared sample) and a
    // stereo layer with a 20-character name.
    snd::Instrument in;
    in.name = "Test Keys";
    in.samples = {sample("Low C", 44100, 1, 9000, 130.8, true, 1), sample("Mid C", 48000, 1, 8000, 261.6, false, 2),
                  sample("High C", 48000, 1, 7000, 523.3, true, 3), sample("Pad St", 32000, 2, 6000, 220.0, true, 4)};
    in.layers = {{"Soft Keys", {{36, 47, 0, 48, -99, 0}, {60, 72, 1, 60, 99, 0}, {73, 90, 2, 72, 5, 30}}, 0, 63},
                 {"Hard Keys", {{0, 59, 0, 48, 0, 0}, {60, 127, 2, 72, -3, 0}}, 64, 99},
                 {"Twenty Char Pad Name", {{40, 80, 3, 57, 12, 0}}, 100, 127}};

    std::printf("== Write and read back through the existing reader\n");
    const fs::path out = root / "Keys";
    std::string err;
    const std::string rep = snd::writeKorgFolder(in, out, "Source: synthetic test\n", &err);
    TEST("written: " + err, !rep.empty() && fs::is_directory(out) && !fs::exists(ai_arranger::media::incompletePath(out)));
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(out)) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    const std::vector<std::string> kmps = {"HARD01.KMP", "SOFT00.KMP", "TWEN02_L.KMP", "TWEN02_R.KMP"};
    TEST("one KMP per mono layer, an _L/_R pair for the stereo layer",
         std::all_of(kmps.begin(), kmps.end(), [&](const std::string& k) { return std::count(names.begin(), names.end(), k); }));
    bool fit = true;
    size_t ksfs = 0;
    for (const auto& n : names) {
        const auto dot = n.find('.');
        if (n.size() >= 4 && (n.substr(n.size() - 4) == ".KSF" || n.substr(n.size() - 4) == ".KMP")) {
            fit &= n.size() <= 12 && dot != std::string::npos && dot <= 8 && n.size() - dot == 4;
            ksfs += n.substr(n.size() - 4) == ".KSF";
        }
    }
    TEST("every KMP/KSF name is 8.3 (the RLP1 KSF file name field is 12 bytes incl. period and extension)", fit);
    TEST("one KSF per mono sample, two (L, R) per stereo sample: 5", ksfs == 5);
    TEST("REPORT.txt and PACKAGE.sha256 written", std::count(names.begin(), names.end(), "REPORT.txt") && std::count(names.begin(), names.end(), "PACKAGE.sha256"));

    snd::Instrument back;
    const auto rb = snd::buildFromKorgFolder(out, in.name, {"Soft Keys", "Hard Keys", "Twenty Char Pad Name"}, {64, 100}, &back);
    TEST("the reader reads it with the layer order and splits from the REPORT: " + rb, rb.empty());
    TEST("zones, roots, tune, loops, rates, channels and audio arrive exactly", rb.empty() && snd::korgReadBackDiffers(in, back).empty());
    if (rb.empty()) {
        const auto& p = back.layers[2];
        const auto& s = back.samples[size_t(p.zones[0].sample)];
        TEST("the stereo layer comes back stereo at 32 kHz, keys 40-80, root 57, +12 cents",
             s.channels == 2 && s.rate == 32000 && p.zones[0].lo == 40 && p.zones[0].hi == 80 && p.zones[0].root == 57 && p.zones[0].tuneCents == 12);
        const auto& z = back.layers[0].zones;
        TEST("gaps stay silent: the soft layer keeps 3 zones (36-47, 60-72, 73-90), tune -99 and +99",
             z.size() == 3 && z[0].lo == 36 && z[0].hi == 47 && z[1].lo == 60 && z[2].hi == 90 && z[0].tuneCents == -99 && z[1].tuneCents == 99);
        bool same = true;
        for (size_t l = 0; l < in.layers.size(); ++l)
            for (size_t i = 0; i < in.layers[l].zones.size(); ++i) {
                std::vector<snd::Zone> zs = in.layers[l].zones;
                std::sort(zs.begin(), zs.end(), [](const snd::Zone& a, const snd::Zone& b) { return a.lo < b.lo; });
                same &= in.samples[size_t(zs[i].sample)].name == back.samples[size_t(back.layers[l].zones[i].sample)].name;
            }
        TEST("every sample name comes back (KSF NAME chunk), zone by zone", same);
        TEST("a looped sample plays to its loop end (End = last looped frame); the 100 frames after it are not played",
             back.samples[size_t(z[0].sample)].frames() == 8900 && back.samples[size_t(z[0].sample)].loopEnd == 8900);
    }

    std::printf("== Bytes, from the documented layout\n");
    {
        const auto k = bytes(out / "SOFT00.KMP");
        snd::Kmp m;
        TEST("SOFT00.KMP: MSP1 18 bytes, NAME 24, RLP1 18 per region", snd::readKmp(k, &m).empty() && !std::memcmp(k.data(), "MSP1", 4) &&
             snd::korg::be32(k, 4) == 18 && !std::memcmp(&k[26], "NAME", 4) && snd::korg::be32(k, 30) == 24);
        // 0-35 skipped, 36-47, 48-59 skipped, 60-72, 73-90, 91-127 skipped
        TEST("keys outside every zone are SKIPPEDSAMPL regions (each region starts at the previous top key + 1): 6 regions",
             m.regions.size() == 6 && m.regions[0].ksf == "SKIPPEDSAMPL" && m.regions[0].hi == 35 && m.regions[2].ksf == "SKIPPEDSAMPL" &&
             m.regions[5].ksf == "SKIPPEDSAMPL" && m.regions[5].hi == 127 && k[24] == 6);
        TEST("the level offset is written 0 (unit not documented for the Pa5X) and the REPORT says so",
             m.regions[4].level == 0 && rep.find("1 zone(s) have a level offset: NOT carried") != std::string::npos);
        const auto p = bytes(out / "TWEN02_R.KMP");
        snd::Kmp r;
        TEST("the stereo halves are named '<layer> -L' / '-R' inside (NAME 24 bytes holds 20 + 3), MSP1 truncated to 16",
             snd::readKmp(p, &r).empty() && r.name == "Twenty Char Pad Name -R" && snd::korg::text(p, 8, 16) == "Twenty Char Pad" && p[8 + 15] == ' ');
        const auto s = bytes(out / r.regions[1].ksf);
        snd::KsfHead h;
        TEST("its KSF: SMP1 32 bytes, SMD1 mono 16-bit, Not Use 2nd Start, loop on", snd::readKsf(s, &h, nullptr).empty() &&
             !std::memcmp(s.data(), "SMP1", 4) && snd::korg::be32(s, 4) == 32 && !h.use2ndStart && h.loopOn && !h.plus12dB && s[48 + 6] == 1 && s[48 + 7] == 16);
    }
    {   // REPORT: labels, read-back command
        TEST("REPORT: EXPERIMENTAL, L1 only, not labelled Pa5X-compatible",
             rep.find("EXPERIMENTAL. Verification level L1 only") != std::string::npos && rep.find("not labelled Pa5X-compatible") != std::string::npos);
        TEST("REPORT: audio after the loop end counted once per sample (stereo too): Low C, High C, Pad St = 3",
             rep.find("  3 sample(s) have audio after the loop end") != std::string::npos);
        TEST("REPORT: the read-back command carries the layer order and splits",
             rep.find("--layers 'Soft Keys,Hard Keys,Twenty Char Pad Name' --splits 64,100") != std::string::npos);
        std::ifstream m(out / "PACKAGE.sha256");
        const std::string man((std::istreambuf_iterator<char>(m)), std::istreambuf_iterator<char>());
        TEST("PACKAGE.sha256 lists all 10 files (4 KMP, 5 KSF, REPORT)", man.find("# 10 files") != std::string::npos);
    }

    std::printf("== Refusals (nothing is left behind)\n");
    const auto refuse = [&](const std::string& what, const snd::Instrument& x, const std::string& expect) {
        const fs::path d = root / ("bad" + std::to_string(std::hash<std::string>{}(what) % 100000));
        std::string e;
        const bool wrote = !snd::writeKorgFolder(x, d, "", &e).empty();
        TEST(what + " -> " + e, !wrote && e.find(expect) != std::string::npos && !fs::exists(d) && !fs::exists(ai_arranger::media::incompletePath(d)));
    };
    snd::Instrument one;
    one.name = "One";
    one.samples = {sample("S", 48000, 1, 4000, 300, false, 9)};
    one.layers = {{"Lead", {{0, 127, 0, 60, 0, 0}}, 0, 127}};
    auto x = one; x.layers[0].zones[0].tuneCents = 100;
    refuse("zone tune +100 cents", x, "outside -99..+99 (p.1158)");
    x = one; x.layers[0].zones[0].tuneCents = -100;
    refuse("zone tune -100 cents", x, "outside -99..+99");
    x = one; x.layers[0].name = "Lead -L";
    refuse("a mono layer named like a stereo half", x, "cannot end in -L or -R");
    x = one; x.samples[0].channels = 2; x.layers[0].name = "Lead -R";
    refuse("a stereo layer named like a stereo half", x, "cannot end in -L or -R");
    x = one; x.layers[0].name = "Lead,1";
    refuse("a comma in the multisample name (the reader cannot give it back)", x, "contain a comma");
    x = one; x.layers[0].name = "Lead ";
    refuse("a trailing space in the multisample name", x, "start or end with a space");
    x = one; x.samples.push_back(sample("T", 48000, 2, 4000, 300, false, 9)); x.layers[0].zones = {{0, 59, 0, 60, 0, 0}, {60, 127, 1, 60, 0, 0}};
    refuse("a layer mixing mono and stereo samples", x, "mixes mono and stereo");
    x = one; x.layers = {{"Lead", {{0, 127, 0, 60, 0, 0}}, 0, 63}, {"Lead", {{0, 127, 0, 60, 0, 0}}, 64, 127}};
    refuse("two layers with the same name", x, "same name");
    x = one; x.samples[0].rate = 8000;
    refuse("a sample rate the Pa5X model refuses (validate)", x, "rate outside 11025-48000");
    x = one; x.samples[0].name = "A twenty-one-char nam";
    refuse("a sample name longer than 20 (NAME holds 24; the model allows 20)", x, "1-20 printable");
    x = one; x.layers[0].zones = {{0, 59, 0, 48, 0, 0}, {60, 127, 0, 60, 0, 0}};
    refuse("one sample under two original keys (the reader splits it; its name could not come back)", x, "under two original keys");
    x = one; x.samples[0].name = "S ";
    refuse("a sample name ending with a space", x, "cannot end with a space");
    x = one; x.samples.push_back(sample("A/B", 48000, 1, 4000, 300, false, 9)); x.samples[0].name = "A_B";
    x.layers[0].zones = {{0, 59, 0, 60, 0, 0}, {60, 127, 1, 60, 0, 0}};
    refuse("two sample names with the same file name (A_B, A/B)", x, "same file name");
    x.samples[1].name = "a_b";
    refuse("two sample names differing only in case (FAT)", x, "same file name");
    std::printf("== Mounted volumes are refused before anything is created (media/output_guard.h)\n");
    {
        const fs::path vol = "/Volumes/AIA-E1-TEST-" + std::to_string(getpid());
        std::string e;
        TEST("--out under /Volumes is refused: " + e,
             snd::writeKorgFolder(one, vol / "Keys", "", &e).empty() && e.find("mounted volume") != std::string::npos && !fs::exists(vol));
        std::error_code ec;
        fs::create_symlink("/Volumes", root / "vol", ec);
        e.clear();
        const bool made = !ec && !snd::writeKorgFolder(one, root / "vol" / vol.filename() / "Keys", "", &e).empty();
        if (fs::is_directory("/Volumes"))   // macOS: the mount folder exists, the link resolves into it, and the guard refuses
            TEST("--out through a symlink into /Volumes is refused: " + e, !ec && !made && e.find("mounted volume") != std::string::npos && !fs::exists(vol));
        else   // no /Volumes (Linux): the link leads nowhere. Not mounted-volume coverage: Linux mount points have no guard (D44)
            TEST("no /Volumes here: a write through a link to it fails and creates nothing (no mounted-volume rule on this OS): " + e,
                 !ec && !made && !e.empty() && !fs::exists(vol) && fs::is_symlink(fs::symlink_status(root / "vol")) && !fs::exists(root / "vol"));
    }
    {   // an apostrophe in a name: the printed read-back command must still be one shell word per argument
        x = one; x.name = "Bob's Lead";
        std::string e;
        const auto r = snd::writeKorgFolder(x, root / "Quote", "", &e);
        TEST("REPORT quotes an apostrophe for the shell: " + e, r.find("--name 'Bob'\\''s Lead'") != std::string::npos);
    }
    {
        std::string e;
        TEST("an existing folder is never written into", snd::writeKorgFolder(one, out, "", &e).empty() && e.find("already exists") != std::string::npos);
        fs::create_directories(ai_arranger::media::incompletePath(root / "Half"));
        TEST("an unfinished earlier write is not reused", snd::writeKorgFolder(one, root / "Half", "", &e).empty() && e.find(".INCOMPLETE already exists") != std::string::npos);
    }

    fs::remove_all(root);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
