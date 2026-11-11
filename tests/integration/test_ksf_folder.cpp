// .KSF samples without a .KMP -> Pa5X package (media/sound/ksf_folder_source.h). Synthetic audio only, in KSFs written
// from Korg's documented layout. Checks that roots are measured (and a name hint is only a hint), that zones are only
// proposed and the edited mapping is what gets built, that audio and loops arrive exactly, that unusable files are
// named with the reason, that the report separates recovered from approximated, and that the source is never changed.

#include "media/sound/ksf_folder_source.h"
#include "media/sound/package_verify.h"
#include "media/sound/package_writer.h"
#include "ui/sound/sound_conversion_model.h"
#include "../support/korg_fixture.h"
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
        std::ifstream f(e.path(), std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        all += e.path().string() + ":" + std::to_string(std::hash<std::string>{}(s)) + ":" +
               std::to_string(static_cast<long long>(e.last_write_time().time_since_epoch().count())) + "\n";
    }
    return all;
}
static double hz(int note, double cents = 0) { return 440 * std::pow(2.0, (note - 69 + cents / 100) / 12); }
static std::vector<int16_t> noise(size_t n) {
    std::vector<int16_t> v(n);
    uint32_t x = 12345;
    for (auto& s : v) { x = x * 1664525 + 1013904223; s = int16_t(int(x >> 16) - 32768) / 4; }
    return v;
}
static bool has(const std::string& s, const std::string& t) { return s.find(t) != std::string::npos; }

int main() {
    const fs::path root = fs::temp_directory_path() / ("ksf-src-" + std::to_string(getpid()));
    fs::remove_all(root);
    const fs::path src = root / "Test Strings";
    // C3 in tune, C4 20 cents flat (looped, Start 200), C5 named "61" (a wrong hint), an unpitched one named 84,
    // an unpitched one with no number, a compressed one, and a second C4.
    const auto c3 = tone(40000, hz(48), 9000, 1), c4 = tone(40000, hz(60, -20), 9000, 2), c5 = tone(40000, hz(72), 9000, 3);
    put(src / "MS000000.KSF", ksf({c3, 48000, 0, 0, 39999, 0x20}, nullptr, "TSTR48"));
    put(src / "MS000001.KSF", ksf({c4, 48000, 200, 10000, 37999, 0x20}, nullptr, "TSTR60"));
    put(src / "ms000002.ksf", ksf({c5, 48000, 0, 0, 39999, 0x80}, nullptr, "TSTR61"));
    put(src / "MS000003.KSF", ksf({noise(40000), 48000, 0, 0, 39999, 0x20}, nullptr, "NOISE84"));
    put(src / "MS000004.KSF", ksf({noise(40000), 48000, 0, 0, 39999, 0x20}, nullptr, "NOISE"));
    put(src / "MS000005.KSF", ksf({c3, 48000, 0, 0, 39999, 0x10}, nullptr, "PACKED48"));
    put(src / "MS000006.KSF", ksf({tone(40000, hz(60, 3), 8000, 4), 48000, 0, 0, 39999, 0x20}, nullptr, "TSTR60B"));
    put(src / "README.txt", Bytes{'x'});
    const std::string before = hashTree(src);

    std::printf("== folder kind and note names\n");
    TEST("a folder of KSFs without a KMP is a KSF folder", snd::isKsfFolder(src) && !snd::isKorgFolder(src));
    put(root / "withkmp" / "A.KMP", kmp("A", {{60, 127, 0, 0, "MS000000.KSF"}}));
    put(root / "withkmp" / "MS000000.KSF", ksf({c3, 48000, 0, 0, 39999, 0x20}));
    TEST("a folder with a KMP is not (it is a multisample folder)", !snd::isKsfFolder(root / "withkmp") && snd::isKorgFolder(root / "withkmp"));
    TEST("note names: C-1 = 0, C4 = 60, G#3 = 56, G9 = 127, numbers", snd::parseNote("C-1") == 0 && snd::parseNote("C4") == 60 &&
         snd::parseNote("G#3") == 56 && snd::parseNote("G9") == 127 && snd::parseNote("60") == 60);
    TEST("refused: G#9 (128), 128, H3, C10, c4", snd::parseNote("G#9") < 0 && snd::parseNote("128") < 0 && snd::parseNote("H3") < 0 &&
         snd::parseNote("C10") < 0 && snd::parseNote("c4") < 0);

    std::printf("== measured roots and the proposal\n");
    const auto k1 = snd::analyseKsf(src / "MS000001.KSF");
    TEST("C4 20 cents flat: measured C4, -20 +-2 cents", k1.detected == 60 && std::fabs(k1.cents + 20) < 2);
    TEST("the name's number is read as a hint (TSTR60 -> 60)", k1.hint == 60 && k1.name == "TSTR60");
    const auto kn = snd::analyseKsf(src / "MS000004.KSF");
    TEST("noise: no pitch, with the reason", kn.detected < 0 && !kn.why.empty());
    std::string map;
    const auto ep = snd::proposeKsfMapping(src, &map);
    TEST("a mapping is proposed: " + ep, ep.empty());
    std::printf("%s", map.c_str());
    TEST("C3 measured and agrees with its name", has(map, "MS000000.KSF     C3    C-1   F3    +0") && has(map, "TSTR48 ends in 48 (C3): agrees"));
    TEST("C4 zone F#3..F4 with tune +20 proposed from the measured -20 cents", has(map, "MS000001.KSF     C4    F#3   F4    +20"));
    TEST("a wrong name hint is flagged, the measurement wins", has(map, "ms000002.ksf     C5") && has(map, "ends in 61 (C#4): DISAGREES"));
    TEST("unpitched with a number: root from the name, marked NOT verified, tune 0",
         has(map, "MS000003.KSF     C6") && has(map, "root from the sample name only, NOT verified"));
    TEST("unpitched without a number: not used, with the reason", has(map, "# MS000004.KSF  NOT USED: no root"));
    TEST("compressed: not used, with the reason", has(map, "# MS000005.KSF  NOT USED: compressed sample data"));
    TEST("a second sample at C4 is not guessed into a velocity layer", has(map, "# MS000006.KSF  NOT USED: same root C4 as MS000001.KSF"));
    TEST("the proposal says it is not the original mapping", has(map, "Not the original mapping") && has(map, "velocity layers cannot be recovered"));

    std::printf("== build from the mapping\n");
    snd::Instrument in;
    const auto eb = snd::buildFromKsfMapping(src, "Test Strings", "Strings", map, &in);
    TEST("the proposal builds: " + eb, eb.empty());
    if (eb.empty()) {
        const auto& z = in.layers[0].zones;
        TEST("one layer 0-127, four zones tiling 0-127", in.layers.size() == 1 && in.layers[0].velLo == 0 && in.layers[0].velHi == 127 && z.size() == 4 &&
             z[0].lo == 0 && z[0].hi == 53 && z[1].lo == 54 && z[1].hi == 65 && z[2].lo == 66 && z[2].hi == 77 && z[3].lo == 78 && z[3].hi == 127);
        TEST("roots C3 C4 C5 C6, tune 0 +20 0 0", z[0].root == 48 && z[1].root == 60 && z[2].root == 72 && z[3].root == 84 && z[1].tuneCents == 20 &&
             z[0].tuneCents == 0 && z[3].tuneCents == 0);
        const auto& s1 = in.samples[size_t(z[1].sample)];
        TEST("audio exact from Start (200) to End, loop Loop Start..End inclusive", s1.frames() == 37800 && s1.pcm.front() == c4[200] &&
             s1.pcm.back() == c4[37999] && s1.looped && s1.loopStart == 9800 && s1.loopEnd == 37800 && s1.root == 60);
        TEST("Loop Off stays one-shot", !in.samples[size_t(z[2].sample)].looped);
    }
    // the musician's edit is what is built
    const std::string edited = "MS000000.KSF 48 0 59 # mine\nMS000001.KSF C4 C#4 G9 -5\n";
    snd::Instrument ed;
    TEST("an edited mapping builds as written", snd::buildFromKsfMapping(src, "Edited", "Strings", edited, &ed).empty() && ed.layers[0].zones.size() == 2 &&
         ed.layers[0].zones[0].hi == 59 && ed.layers[0].zones[1].lo == 61 && ed.layers[0].zones[1].tuneCents == -5);
    const auto refuse = [&](const std::string& m, const std::string& why) {
        snd::Instrument x;
        const auto e = snd::buildFromKsfMapping(src, "X", "Strings", m, &x);
        TEST("refused (" + why + "): " + e, has(e, why));
    };
    refuse("MS000000.KSF C3 C-1\n", "needs <file> <root> <low> <high> [tune]");
    refuse("MS000000.KSF C3 C-1 B9\n", "note names");
    refuse("MS000000.KSF C3 C4 C3\n", "low key above high key");
    refuse("MS000000.KSF C3 C-1 G9 +100\n", "whole cents from -99 to +99");
    refuse("../MS000000.KSF C3 C-1 G9\n", "a file name, not a path");
    refuse("MS000099.KSF C3 C-1 G9\n", "not in the folder");
    refuse("MS000005.KSF C3 C-1 G9\n", "compressed sample data");
    refuse("MS000000.KSF C3 C-1 C4\nMS000001.KSF C4 C4 G9\n", "overlap");
    refuse("# only comments\n", "no sample lines");

    std::printf("== Convert Sound model\n");
    ai_arranger::ui::SoundConversionModel m;
    std::string err;
    TEST("inspect: a KSF folder, one layer named after the folder, no splits", m.inspect(src, &err) && m.ksf() && !m.korg() &&
         m.proposedOrder() == "Test Strings" && m.proposedSplits().empty() && m.layers().front().files == 7);
    std::string mp;
    TEST("build with an empty mapping proposes one and builds it", m.build("Test Strings", "Test Strings", "", &err, &mp) && mp == map);
    TEST("two layers are refused (no evidence of velocity layers)",
         !ai_arranger::ui::SoundConversionModel::decode(src, "X", "a,b", "64", in, &err, &mp) && has(err, "one layer"));
    TEST("splits are refused", !ai_arranger::ui::SoundConversionModel::decode(src, "X", "a", "64", in, &err, &mp) && has(err, "splits"));

    std::printf("== package and report\n");
    const fs::path out = root / "out" / "Test Strings";
    const auto sum = ai_arranger::ui::SoundConversionModel::generate(m.instrument(), src, out, &err, mp);
    TEST("package written: " + err, !sum.empty());
    const auto v = snd::verifyPackage(out);
    TEST("the package verifies", v.ok);
    std::ifstream rf(out / "REPORT.txt");
    const std::string rep((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    TEST("REPORT: no KMP, recovered vs approximated vs not available", has(rep, "WITHOUT a .KMP") && has(rep, "RECOVERED exactly") &&
         has(rep, "APPROXIMATED (not in a KSF)") && has(rep, "NOT AVAILABLE without the KMP"));
    TEST("REPORT: per-sample measurement and the hint", has(rep, "MS000001.KSF     TSTR60           root C4   keys F#3-F4 tune +20  measured C4 -20") &&
         has(rep, "name says C#4") && has(rep, "not measured:"));
    TEST("REPORT: the mapping used is recorded", has(rep, "Mapping used:\n# KSF samples without a .KMP"));
    TEST("the source folder is unchanged", hashTree(src) == before);

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
