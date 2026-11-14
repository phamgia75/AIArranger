// AI Music Studio shell models (ui/studio/studio_models.h): read-only source recognition and the L1/L2/L3 badges.
// L3 must come only from an owner record naming this exact output while L1 passes. Original synthetic fixtures only.

#include "media/sound/additive_source.h"
#include "media/sound/package_writer.h"
#include "ui/studio/studio_models.h"
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::ui::studio;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static void put(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

static std::string listing(const fs::path& root) {   // names + sizes, to prove the probe wrote nothing
    std::string s;
    for (const auto& e : fs::recursive_directory_iterator(root)) s += e.path().string() + (e.is_regular_file() ? std::to_string(e.file_size()) : "") + "\n";
    return s;
}

int main() {
    std::printf("Test: studio models (source recognition, verification badges)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-studio-" + std::to_string(::getpid()));
    fs::remove_all(root);
    using K = SourceProbe::Kind;

    // ── recognition ──
    put(root / "Mine.KST/STYLE/USER01/Ballad.STG", "s");
    put(root / "Mine.KST/PAD/USER01/Hit.PDC", "pp");
    put(root / "Mine.KST/PCM/RAM01.PCM", std::string(100, 'p'));
    const auto before = listing(root);
    auto p = probeSource(root / "Mine.KST");
    TEST("KST folder: Pa5X user data by layout", p.kind == K::Pa5xUserData && p.layout && p.error.empty());
    TEST("probe wrote nothing", listing(root) == before);
    const auto view = libraryView(p);
    TEST("header says what was checked: layout yes, content never", view.find("layout \xE2\x97\x86 content \xE2\x97\x87") != std::string::npos &&
         view.find("Read-only source") != std::string::npos && view.find("Content: not checked") != std::string::npos);
    TEST("library view: our type names, zero rows stay visible", view.find("Styles               1") != std::string::npos &&
         view.find("Keyboard Sets     none in this source") != std::string::npos && view.find("STYLE/USER01: 1") != std::string::npos);
    TEST("library view: memory is an estimate", view.find("DISK SIZE, not Pa5X sample RAM") != std::string::npos);
    TEST("no output is called compatible", view.find("ompatible") == std::string::npos);

    put(root / "Old.SET/STYLE/BANK01.STY", "x");
    p = probeSource(root / "Old.SET");
    TEST("Pa4X SET: named as such, no Pa5X memory estimate, STY explained", p.kind == K::Pa4xSet &&
         libraryView(p).find("not Yamaha styles") != std::string::npos && libraryView(p).find("DISK SIZE") == std::string::npos);

    put(root / "kronos/EP000.KMP", "k");
    put(root / "kronos/EP000.KSF", "k");
    TEST("KMP/KSF folder: sampling data", probeSource(root / "kronos").kind == K::KorgSamplingData);
    put(root / "wavs/a_60_0_127.wav", "w");
    TEST("WAV folder", probeSource(root / "wavs").kind == K::WavFolder);
    put(root / "misc/readme.txt", "t");
    p = probeSource(root / "misc");
    TEST("unrecognised folder: not recognised, with what to choose instead", p.kind == K::NotRecognised && !p.layout &&
         p.error.find("Not a keyboard backup") != std::string::npos);
    TEST("a file is not a source", !probeSource(root / "misc/readme.txt").error.empty());
    put(root / "mixed/STYLE/B/A.stg", "s");
    put(root / "mixed/MULTISMP/ram.kmp", "k");
    p = probeSource(root / "mixed");
    TEST("suffix-less folder with Pa5X and Pa4X names: ambiguous, not opened as a KST or SET", p.kind == K::NotRecognised && !p.layout &&
         p.error.find("Ambiguous") != std::string::npos);

    std::string err;
    const fs::path pkg = root / "organ";
    TEST("package written", !media::sound::writePackage(media::sound::buildDemoOrgan(), pkg, "", &err).empty());
    TEST("package folder recognised", probeSource(pkg).kind == K::AiaPackage);

    // ── badges ──
    Verification none;
    TEST("nothing run: all hollow", badgeLine(none) == "\xE2\x97\x87 L1  \xE2\x97\x87 L2  \xE2\x97\x87 L3");
    TEST("spoken text says L3 not tested on a Pa5X", spokenBadges(none).find("Level 3 hardware test not recorded: not tested on a Pa5X") != std::string::npos);

    auto v = fromPackageCheck(media::sound::verifyPackage(pkg));
    TEST("good package: L1 and L2 passed, L3 hollow", v.l1.state == Check::Passed && v.l2.state == Check::Passed && v.l3.state == Check::NotRun &&
         badgeLine(v) == "\xE2\x97\x86 L1  \xE2\x97\x86\xE2\x97\x86 L2  \xE2\x97\x87 L3");

    // Files match but the package does not load: L1 passes, L2 fails (locks the verifier's message prefix).
    const fs::path broken = root / "broken";
    fs::copy(pkg, broken, fs::copy_options::recursive);
    {
        put(broken / "RESOURCES.tsv", "garbage\n");
        // re-hash the manifest so only the load fails
        std::ifstream m(broken / "PACKAGE.sha256");
        std::string man((std::istreambuf_iterator<char>(m)), {}), line, out;
        bool c = false;
        std::istringstream ls(man);
        const auto h = media::detail::sha256File(broken / "RESOURCES.tsv", [](size_t) { return true; }, &c, &err);
        while (std::getline(ls, line)) out += (line.find("RESOURCES.tsv") != std::string::npos ? h + line.substr(64) : line) + "\n";
        put(broken / "PACKAGE.sha256", out);
    }
    v = fromPackageCheck(media::sound::verifyPackage(broken));
    TEST("files match but no load: L1 passed, L2 FAILED", v.l1.state == Check::Passed && v.l2.state == Check::Failed &&
         badgeLine(v).find("\xE2\x9C\x95 L2") != std::string::npos);
    put(broken / "RESOURCES.tsv", "changed again\n");
    v = fromPackageCheck(media::sound::verifyPackage(broken));
    TEST("changed file: L1 FAILED, L2 not run", v.l1.state == Check::Failed && v.l2.state == Check::NotRun);
    v = fromPackageCheck(media::sound::verifyPackage(root / "misc"));
    TEST("no manifest: L1 FAILED with the reason", v.l1.state == Check::Failed && v.l1.detail == "no PACKAGE.sha256");

    // ── L3: only from an owner record naming this output ──
    bool c = false;
    const auto manHash = media::detail::sha256File(pkg / "PACKAGE.sha256", [](size_t) { return true; }, &c, &err);
    const auto good = fromPackageCheck(media::sound::verifyPackage(pkg));
    auto record = [&](const std::string& hash, const std::string& result, const std::string& device = "Pa5X") {
        put(root / "L3.txt", "aiarranger-l3-record\t1\nmanifest-sha256\t" + hash + "\ndevice\t" + device + "\ndate\t2026-11-20\ntester\towner\nresult\t" + result + "\n");
        return checkL3Record(root / "L3.txt", pkg, good.l1);
    };
    TEST("passing record for this output: L3 passed", record(manHash, "pass").state == Check::Passed);
    std::string upper = manHash;
    for (auto& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    TEST("hash case does not matter", record(upper, "pass").state == Check::Passed);
    TEST("failing record: L3 FAILED", record(manHash, "fail").state == Check::Failed);
    TEST("record for another output: no L3", record(std::string(64, '0'), "pass").state == Check::NotRun);
    TEST("record for another device: no L3", record(manHash, "pass", "Pa4X").state == Check::NotRun);
    TEST("unknown result word: no L3", record(manHash, "ok").state == Check::NotRun);
    {
        put(root / "L3.txt", "manifest-sha256 " + manHash + "\ndevice Pa5X\ndate 2026-11-20\ntester owner\nresult pass\n");
        TEST("no header: no L3", checkL3Record(root / "L3.txt", pkg, good.l1).state == Check::NotRun);
        put(root / "L3.txt", "aiarranger-l3-record 1\nmanifest-sha256 " + manHash + "\ndevice Pa5X\ndate 2026-11-20\nresult pass\n");
        TEST("no tester: no L3", checkL3Record(root / "L3.txt", pkg, good.l1).state == Check::NotRun);
    }
    record(manHash, "pass");
    TEST("L1 not run now: no L3 even with a matching record", checkL3Record(root / "L3.txt", pkg, Level{}).state == Check::NotRun);
    TEST("missing record file: no L3", checkL3Record(root / "nope.txt", pkg, good.l1).state == Check::NotRun);
    TEST("output without a manifest: no L3", checkL3Record(root / "L3.txt", root / "misc", good.l1).state == Check::NotRun);
    TEST("a good package alone never reaches L3", good.l3.state == Check::NotRun);

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
