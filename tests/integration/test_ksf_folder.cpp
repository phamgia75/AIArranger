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
    refuse("MS000000.KSF C3 C-1\n", "needs <file> <root> <low> <high> [tune with sign]");
    refuse("MS000000.KSF C3 C-1 B9\n", "note names");
    refuse("MS000000.KSF C3 C4 C3\n", "low key above high key");
    refuse("MS000000.KSF C3 C-1 G9 +100\n", "whole cents from -99 to +99");
    refuse("../MS000000.KSF C3 C-1 G9\n", "a file name, not a path");
    refuse("MS000099.KSF C3 C-1 G9\n", "not in the folder");
    refuse("MS000005.KSF C3 C-1 G9\n", "compressed sample data");
    refuse("MS000000.KSF C3 C-1 C4\nMS000001.KSF C4 C4 G9\n", "overlap");
    refuse("# only comments\n", "no sample lines");
    refuse("MS000001.KSF C4 C4 G9 5\n", "a tune needs its sign");

    std::printf("== review round 1 regressions\n");
    {   // high and low notes: YIN searched only periods >= 24 frames (2 kHz), so MIDI 108 read as 89
        const fs::path hi = root / "range";
        for (int note : {28, 100, 108})
            put(hi / ("N" + std::to_string(note) + ".KSF"), ksf({tone(40000, hz(note), 9000, note), 48000, 0, 0, 39999, 0x20}, nullptr, "N"));
        for (int note : {28, 100, 108}) {
            const auto k = snd::analyseKsf(hi / ("N" + std::to_string(note) + ".KSF"));
            TEST("MIDI " + std::to_string(note) + " measured as itself (got " + std::to_string(k.detected) + ")", k.detected == note && std::fabs(k.cents) < 2);
        }
    }
    {   // file names with spaces: the columns are read from the right
        const fs::path sp = root / "spaces";
        put(sp / "Grand Piano.KSF", ksf({c3, 48000, 0, 0, 39999, 0x20}, nullptr, "GP48"));
        put(sp / "Grand  Piano 2.KSF", ksf({c5, 48000, 0, 0, 39999, 0x20}, nullptr, "GP72"));
        put(sp / "odd #1.KSF", ksf({c4, 48000, 0, 0, 39999, 0x20}, nullptr, "ODD60"));
        std::string pm;
        snd::Instrument x;
        TEST("names with spaces are proposed and build", snd::proposeKsfMapping(sp, &pm).empty() && has(pm, "Grand  Piano 2.KSF") &&
             snd::buildFromKsfMapping(sp, "GP", "GP", pm, &x).empty() && x.samples.size() == 2);
        TEST("a name a mapping line cannot hold is listed, not mangled", has(pm, "# odd #1.KSF  NOT USED: this file name cannot be written"));
        std::vector<snd::KsfLine> ls;
        TEST("parser: file 'Grand  Piano 2.KSF', root C5, tune -3",
             snd::parseKsfMapping("Grand  Piano 2.KSF  C5 C-1 G9 -3  # x\n", ls).empty() && ls.size() == 1 && ls[0].file == "Grand  Piano 2.KSF" &&
             ls[0].root == 72 && ls[0].tune == -3);
    }
    std::printf("== review round 2 regressions\n");
    {   // a long file name must not truncate its row (and swallow the next one); a leading '#' is not a comment row
        const fs::path ln = root / "long";
        const std::string longName = std::string(220, 'A') + ".KSF";
        put(ln / longName, ksf({c3, 48000, 0, 0, 39999, 0x20}, nullptr, "L48"));
        put(ln / "B.KSF", ksf({c5, 48000, 0, 0, 39999, 0x20}, nullptr, "B72"));
        put(ln / "#C.KSF", ksf({c4, 48000, 0, 0, 39999, 0x20}, nullptr, "C60"));
        std::string pm;
        snd::Instrument x;
        TEST("a 224-character name and the next sample both build", snd::proposeKsfMapping(ln, &pm).empty() &&
             snd::buildFromKsfMapping(ln, "L", "L", pm, &x).empty() && x.samples.size() == 2 && has(pm, longName + " C3"));
        TEST("a name starting with '#' is listed NOT USED, not silently commented", has(pm, "# #C.KSF  NOT USED: this file name cannot be written"));
    }
    {   // the KSF notes are used even when the source folder no longer looks like a KSF folder at Generate
        const fs::path gone = root / "gone";
        put(gone / "A.KSF", ksf({c4, 48000, 0, 0, 39999, 0x20}, nullptr, "A60"));
        std::string pm, nt, ge;
        snd::Instrument x;
        const bool ok = snd::proposeKsfMapping(gone, &pm).empty() && snd::buildFromKsfMapping(gone, "A", "A", pm, &x, &nt).empty();
        fs::remove(gone / "A.KSF");
        const auto r = ai_arranger::ui::SoundConversionModel::generate(x, gone, root / "out" / "gone", &ge, nt);
        std::ifstream f(root / "out" / "gone" / "REPORT.txt");
        const std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        TEST("REPORT keeps the KSF measurements after the source files are removed", ok && !r.empty() && has(t, "WITHOUT a .KMP") && has(t, "measured C4"));
    }
    std::printf("== review round 3 regressions\n");
    {   // control characters in names must not start new mapping lines; samples over the Pa5X limits are not proposed
        const fs::path cc = root / "ctrl";
        put(cc / "bad\nBROKEN.KSF", ksf({c3, 48000, 0, 0, 39999, 0x20}, nullptr, "B48"));
        put(cc / "OK.KSF", ksf({c4, 48000, 0, 0, 39999, 0x20}, nullptr, "X\nBAD60"));
        put(cc / "LONG.KSF", ksf({tone(1048577, hz(72), 9000, 5), 48000, 0, 0, 1048576, 0x20}, nullptr, "LONG72"));
        put(cc / "SLOW.KSF", ksf({tone(40000, hz(84), 9000, 6), 8000, 0, 0, 39999, 0x20}, nullptr, "SLOW84"));
        put(cc / "TINY.KSF", ksf({c5, 48000, 0, 39990, 39999, 0x00}, nullptr, "TINY72"));
        std::string pm;
        snd::Instrument x;
        const auto e = snd::proposeKsfMapping(cc, &pm);
        std::printf("%s", pm.c_str());
        TEST("the folder still previews: only OK.KSF is used", e.empty() && snd::buildFromKsfMapping(cc, "C", "C", pm, &x).empty() && x.samples.size() == 1);
        TEST("a newline in a file name is shown as '?', and listed NOT USED", has(pm, "# bad?BROKEN.KSF  NOT USED: this file name") && !has(pm, "\nBROKEN"));
        TEST("a newline in a sample name is shown as '?'", has(pm, "name X?BAD60 ends in 60"));
        TEST("over 1 048 576 frames: NOT USED with the Pa5X limit", has(pm, "# LONG.KSF  NOT USED: longer than 1 048 576 frames"));
        TEST("8 kHz: NOT USED with the Pa5X limit", has(pm, "# SLOW.KSF  NOT USED: rate outside 11025-48000 Hz"));
        TEST("a 10-frame loop: NOT USED", has(pm, "# TINY.KSF  NOT USED: loop shorter than 32 frames"));
    }
    std::printf("== vibrato, overrides and quality\n");
    {   // a vibrato tone with loudness tied to the swing: the centre (-15 cents) is measured, not the loud half
        const fs::path vb = root / "vibrato";
        const auto vibrato = [&](int note, double centre, double depth, double am, double gain, size_t n) {
            std::vector<int16_t> v(n);
            double ph = 0;
            for (size_t i = 0; i < n; ++i) {
                const double t = double(i) / 48000, sw = std::sin(2 * 3.14159265358979 * 5.5 * t);
                ph += 2 * 3.14159265358979 * hz(note, centre + depth * sw) / 48000;
                v[i] = int16_t(gain * (1 + am * sw) * (std::sin(ph) + 0.4 * std::sin(2 * ph) + 0.2 * std::sin(3 * ph)));
            }
            return v;
        };
        put(vb / "V64.KSF", ksf({vibrato(64, -15, 25, 0.5, 6000, 144000), 48000, 0, 48000, 143999, 0x20}, nullptr, "VIB64"));
        const auto k = snd::analyseKsf(vb / "V64.KSF");
        TEST("vibrato: E4, centre -15 +-1.5 cents (got " + std::to_string(k.cents) + ")", k.detected == 64 && std::fabs(k.cents + 15) < 1.5 && k.vibrato);
        TEST("vibrato: swing about -40..+10 and 5.5 Hz reported", k.moveLo < -30 && k.moveHi > 0 && std::fabs(k.vibHz - 5.5) < 0.6 && k.confidence < 4);
        TEST("a steady tone is not called vibrato", !snd::analyseKsf(src / "MS000001.KSF").vibrato);
        std::string pm, nt;
        snd::Instrument x;
        TEST("the proposal corrects the centre: tune +15", snd::proposeKsfMapping(vb, &pm).empty() && has(pm, "V64.KSF          E4    C-1   G9    +15") &&
             has(pm, "(vibrato centre)"));
        // the musician overrides root and tune: the report says so
        TEST("overrides build; root and tune both named", snd::buildFromKsfMapping(vb, "V", "V", "V64.KSF F4 C-1 G9 +3\n", &x, &nt).empty() && has(nt, "ROOT SET BY YOU") &&
             has(nt, "TUNE SET BY YOU (measured correction +15)") &&
             has(nt, "vibrato centre; swings"));
        TEST("a tune override is named, with the measured correction", snd::buildFromKsfMapping(vb, "V", "V", "V64.KSF E4 C-1 G9 +3\n", &x, &nt).empty() &&
             has(nt, "root as measured, TUNE SET BY YOU (measured correction +15)") && has(nt, "plays -12."));
    }
    std::printf("== quality review round 1 regressions\n");
    {   // a 199 Hz sample rate made the 5 ms step (rate / 200) zero: the analysis never ended
        const fs::path r = root / "rate199";
        std::vector<int16_t> v(4000);
        for (size_t i = 0; i < v.size(); ++i) v[i] = int16_t(8000 * std::sin(2 * 3.14159265358979 * i / 4.0));
        put(r / "R.KSF", ksf({v, 199, 0, 0, 3999, 0x20}, nullptr, "R60"));
        std::string pm;
        TEST("a 199 Hz rate is refused before the analysis", snd::analyseKsf(r / "R.KSF").refused == "rate outside 11025-48000 Hz" &&
             !snd::proposeKsfMapping(r, &pm).empty());
    }
    {   // a steady tone whose loop is not a whole number of periods: seam windows must not make it "vibrato" or detune it
        const fs::path sm = root / "seam";
        put(sm / "S.KSF", ksf({tone(48000, hz(60), 9000, 3), 48000, 0, 45600, 47999, 0x20}, nullptr, "S60"));
        const auto k = snd::analyseKsf(sm / "S.KSF");
        TEST("steady C4 with a rough seam: steady, 0 +-1 cents (got " + std::to_string(k.cents) + ")", k.detected == 60 && !k.vibrato && std::fabs(k.cents) < 1);
        TEST("its loop alone also reads 0 +-1.5", std::fabs(k.loopCents) < 1.5);
    }
    {   // vibrato around 0 for the first second, then a steady +20 cents loop: the loop's own centre is reported
        const fs::path ls = root / "steadyloop";
        std::vector<int16_t> v(72000);
        double ph = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            const double t = i / 48000.0, c = t < 1.0 ? 30 * std::sin(2 * 3.14159265358979 * 5 * t) : 20;
            ph += 2 * 3.14159265358979 * hz(64, c) / 48000;
            v[i] = int16_t(7000 * std::sin(ph));
        }
        put(ls / "L.KSF", ksf({v, 48000, 0, 52000, 71999, 0x20}, nullptr, "L64"));
        const auto k = snd::analyseKsf(ls / "L.KSF");
        TEST("a steady +20 loop after a moving sustain: loop reads +20 +-2 (got " + std::to_string(k.loopCents) + ")", std::fabs(k.loopCents - 20) < 2);
    }
    {   // review round 2: C1 at 11025 Hz, +-40 cents vibrato, a one-cycle loop mostly too short to track: the true centre
        // (0) must lie inside the reported range, and the report must say why the range is wide
        const fs::path sl = root / "shortloop";
        std::vector<int16_t> v(11025);
        double ph = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            ph += 2 * 3.14159265358979 * hz(24, 40 * std::cos(2 * 3.14159265358979 * 5 * (i / 11025.0))) / 11025;
            v[i] = int16_t(8000 * std::sin(ph));
        }
        put(sl / "C1.KSF", ksf({v, 11025, 0, 8820, 11024, 0x20}, nullptr, "C1"));
        const auto k = snd::analyseKsf(sl / "C1.KSF");
        TEST("short loop: true centre 0 within the range (got " + std::to_string(k.cents) + " +-" + std::to_string(k.confidence) + ")",
             k.detected == 24 && std::fabs(k.cents) <= k.confidence && k.sparse);
        std::string pm, nt;
        snd::Instrument x;
        TEST("short loop: the report says the loop is too short to track", snd::proposeKsfMapping(sl, &pm).empty() &&
             snd::buildFromKsfMapping(sl, "S", "S", pm, &x, &nt).empty() && has(nt, "loop too short to track"));
    }
    {   // quality: a 6 dB quieter neighbour, a 5x brighter one, a rough loop seam, a one-cycle vibrato loop
        const fs::path ql = root / "quality";
        put(ql / "A.KSF", ksf({tone(48000, hz(60), 8000, 1), 48000, 0, 24000, 47999, 0x20}, nullptr, "A60"));
        put(ql / "B.KSF", ksf({tone(48000, hz(64), 4000, 2), 48000, 0, 24000, 47999, 0x20}, nullptr, "B64"));   // -6 dB
        std::vector<int16_t> bright(48000);
        for (size_t i = 0; i < bright.size(); ++i)
            bright[i] = int16_t(4000 * (std::sin(2 * 3.14159265358979 * hz(67) * i / 48000) + 0.9 * std::sin(2 * 3.14159265358979 * 9 * hz(67) * i / 48000)));
        put(ql / "C.KSF", ksf({bright, 48000, 0, 24000, 47999, 0x20}, nullptr, "C67"));
        put(ql / "D.KSF", ksf({tone(48000, hz(72), 4000, 4), 48000, 0, 24000, 47999, 0x20}, nullptr, "D72"));   // loop not a whole number of periods
        std::string pm, nt;
        snd::Instrument x;
        TEST("quality build", snd::proposeKsfMapping(ql, &pm).empty() && snd::buildFromKsfMapping(ql, "Q", "Q", pm, &x, &nt).empty());
        TEST("quality: a 6 dB quieter zone is named", has(nt, "(B.KSF) are 6.0 dB quieter than the keys below"));
        TEST("quality: a brighter zone is named", has(nt, "(C.KSF) sound ") && has(nt, "dB brighter than the keys below"));
        TEST("quality: steady tones raise no vibrato-loop note", !has(nt, "vibrato cycle"));
        const fs::path q2 = root / "quality2";
        std::vector<int16_t> one(30000);
        double ph = 0;
        for (size_t i = 0; i < one.size(); ++i) { ph += 2 * 3.14159265358979 * hz(64, 25 * std::sin(2 * 3.14159265358979 * 5 * i / 48000.0)) / 48000; one[i] = int16_t(6000 * std::sin(ph)); }
        put(q2 / "E.KSF", ksf({one, 48000, 0, 20400, 29999, 0x20}, nullptr, "E64"));   // a 0.2 s loop: one 5 Hz cycle, seam off-phase
        TEST("quality: a one-cycle vibrato loop is named", snd::proposeKsfMapping(q2, &pm).empty() &&
             snd::buildFromKsfMapping(q2, "Q", "Q", pm, &x, &nt).empty() && has(nt, "the loop holds about 1.0 vibrato cycle(s)"));
        TEST("quality: that loop's seam is smooth, so no seam note", !has(nt, "the loop seam matches"));
        // a loop of 100.5 periods: playback jumps to the opposite phase at each pass
        const fs::path q3 = root / "quality3";
        const double per = 48000 / hz(72);
        put(q3 / "F.KSF", ksf({tone(48000, hz(72), 8000, 7), 48000, 0, uint32_t(47999 - std::lround(100.5 * per)), 47999, 0x20}, nullptr, "F72"));
        TEST("quality: a rough loop seam is named", snd::proposeKsfMapping(q3, &pm).empty() && snd::buildFromKsfMapping(q3, "Q", "Q", pm, &x, &nt).empty() &&
             has(nt, "(F.KSF): the loop seam matches -"));
    }
    {   // the report describes the audio that was built, even if the file changes before Generate
        const fs::path ch = root / "changes";
        put(ch / "A.KSF", ksf({c4, 48000, 0, 0, 39999, 0x20}, nullptr, "A60"));
        std::string pm, nt;
        snd::Instrument x;
        const bool ok = snd::proposeKsfMapping(ch, &pm).empty() && snd::buildFromKsfMapping(ch, "A", "A", pm, &x, &nt).empty();
        put(ch / "A.KSF", ksf({c5, 48000, 0, 0, 39999, 0x20}, nullptr, "A72"));   // replaced after Preview
        std::string ge;
        const auto r = ai_arranger::ui::SoundConversionModel::generate(x, ch, root / "out" / "changes", &ge, nt);
        std::ifstream f(root / "out" / "changes" / "REPORT.txt");
        const std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        TEST("REPORT states the measurement of the packaged audio (C4), not of the replaced file", ok && !r.empty() &&
             has(t, "A60") && has(t, "measured C4 -20") && !has(t, "A72"));
    }

    std::printf("== Convert Sound model\n");
    ai_arranger::ui::SoundConversionModel m;
    std::string err;
    TEST("inspect: a KSF folder, one layer named after the folder, no splits", m.inspect(src, &err) && m.ksf() && !m.korg() &&
         m.proposedOrder() == "Test Strings" && m.proposedSplits().empty() && m.layers().front().files == 7);
    std::string mp, notes;
    TEST("build with an empty mapping proposes one and builds it", m.build("Test Strings", "Test Strings", "", &err, &mp, &notes) && mp == map);
    TEST("two layers are refused (no evidence of velocity layers)",
         !ai_arranger::ui::SoundConversionModel::decode(src, "X", "a,b", "64", in, &err, &mp) && has(err, "one layer"));
    TEST("splits are refused", !ai_arranger::ui::SoundConversionModel::decode(src, "X", "a", "64", in, &err, &mp) && has(err, "splits"));

    std::printf("== package and report\n");
    const fs::path out = root / "out" / "Test Strings";
    TEST("generate without the build's notes is refused", ai_arranger::ui::SoundConversionModel::generate(m.instrument(), src, root / "out" / "x", &err).empty() &&
         has(err, "press Preview again"));
    const auto sum = ai_arranger::ui::SoundConversionModel::generate(m.instrument(), src, out, &err, notes);
    TEST("package written: " + err, !sum.empty());
    const auto v = snd::verifyPackage(out);
    TEST("the package verifies", v.ok);
    std::ifstream rf(out / "REPORT.txt");
    const std::string rep((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    TEST("REPORT: EXACT / MEASURED / ESTIMATED / MISSING, in that order", has(rep, "WITHOUT a .KMP") && rep.find("EXACT (copied") < rep.find("MEASURED (from the audio") &&
         rep.find("MEASURED (from the audio") < rep.find("ESTIMATED (proposed") && rep.find("ESTIMATED (proposed") < rep.find("MISSING (needs the original KMP"));
    TEST("REPORT: per-sample row: keys, root, tune, measurement, what plays", has(rep, "MS000001.KSF     TSTR60           keys F#3-F4    root C4   tune +20") &&
         has(rep, "measured C4 -20.") && has(rep, "(steady); root as measured, tune = the measured correction; plays +0.") && has(rep, "name says C#4") &&
         has(rep, "pitch not measured ("));
    TEST("REPORT: a QUALITY section", has(rep, "QUALITY (measured; listen to these on the Pa5X):"));
    TEST("REPORT: the mapping used is recorded", has(rep, "Mapping used:\n# KSF samples without a .KMP"));
    TEST("the source folder is unchanged", hashTree(src) == before);

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
