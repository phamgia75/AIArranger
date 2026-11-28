// aiarranger-convert exit codes, end to end: 0 written+validated, 1 usage or
// input error, 2 conversion failed (and no output file), plus the sound-map
// template and --compare round-trip modes.

#include "exporters/korg/conversion_manifest.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/korg_style_smf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "../support/genos_corpus.h"

namespace fs = std::filesystem;
using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static int run(const std::string& args) {
    const std::string cmd = std::string("\"") + CONVERT_BIN + "\" " + args + (std::getenv("CLI_DEBUG") ? "" : " >/dev/null 2>&1");
    const int st = std::system(cmd.c_str());
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static std::string q(const fs::path& p) { return "\"" + p.string() + "\""; }

static void writeBytes(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

static std::string readTextFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

int main(int argc, char** argv) {
    bool corpusRun = false;
    if (const int rc = genos_corpus::gate(argc, argv, "test_aiarranger_convert_cli", corpusRun); rc >= 0) return rc;
    std::printf("Test: aiarranger-convert CLI exit codes\n");
    const fs::path dir = fs::temp_directory_path() / ("aiarr_cli_" + std::to_string(std::rand()) + std::to_string(::getpid()));
    fs::remove_all(dir);   // never inherit files from an earlier crashed run
    fs::create_directories(dir);

    // The same exit-code checks run on two different styles: CC0 synthetic ones publicly, and
    // POP_ACOUSTIC_2 / BALLAD_FOLK in the private corpus run (issue #27).
    fs::path pop = genos_corpus::synthetic("SYNTH_FULL_SFF2"), other = genos_corpus::synthetic("SYNTH_WALTZ_68");
    if (corpusRun) {
        pop.clear(); other.clear();
        for (const auto& f : fs::directory_iterator(CORPUS_DIR)) {
            const std::string fn = f.path().filename().string();
            if (f.path().extension() != ".sty") continue;
            if (fn.rfind("POP_ACOUSTIC_2", 0) == 0) pop = f.path();
            if (fn.rfind("BALLAD_FOLK", 0) == 0) other = f.path();
        }
    }
    TEST("input styles present", !pop.empty() && !other.empty() && fs::exists(pop) && fs::exists(other));

    // Break-only style (a Genos "Fill In BA" and nothing else): imports, but no
    // section has a Korg marker code -> exit 2 and no output file.
    {
        std::vector<uint8_t> trk = {0x00, 0xFF, 0x06, 10, 'F', 'i', 'l', 'l', ' ', 'I', 'n', ' ', 'B', 'A',
                                    0x00, 0xFF, 0x58, 4, 4, 2, 24, 8,
                                    0x00, 0x99, 38, 100, 0x83, 0x60, 0x89, 38, 0,
                                    0x8B, 0x20, 0xFF, 0x2F, 0x00};   // EOT at tick 1920
        std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                                  'M', 'T', 'r', 'k', 0, 0, 0, static_cast<uint8_t>(trk.size())};
        b.insert(b.end(), trk.begin(), trk.end());
        writeBytes(dir / "break_only.sty", b);
        TEST("conversion failure exits 2", run("--input " + q(dir / "break_only.sty") + " --target pa5x --output " +
                                               q(dir / "break.mid")) == 2);
        TEST("no output file on exit 2", !fs::exists(dir / "break.mid"));
    }

    const std::string base = "--input " + q(pop) + " --target pa5x ";
    TEST("missing arguments exit 1", run("--input " + q(pop)) == 1);
    TEST("unknown target exits 1", run("--input " + q(pop) + " --target pa4x --output " + q(dir / "x.mid")) == 1);
    TEST("missing input exits 1", run("--input " + q(dir / "nope.sty") + " --target pa5x --output " + q(dir / "x.mid")) == 1);
    TEST("non-numeric --ppqn exits 1", run(base + "--output " + q(dir / "x.mid") + " --ppqn 480abc") == 1);

    std::ofstream(dir / "bad.map") << "acc1 121 0\n";
    TEST("bad sound map exits 1", run(base + "--output " + q(dir / "x.mid") + " --sound-map " + q(dir / "bad.map")) == 1);
    TEST("no output file on exit 1", !fs::exists(dir / "x.mid"));

    TEST("valid conversion + template exits 0",
         run(base + "--output " + q(dir / "pop.mid") + " --sound-template " + q(dir / "pop.map")) == 0);
    TEST("output and template written", fs::exists(dir / "pop.mid") && fs::exists(dir / "pop.map"));
    TEST("generated template is accepted as a sound map",
         run(base + "--output " + q(dir / "pop2.mid") + " --sound-map " + q(dir / "pop.map")) == 0);
    {
        std::ifstream a(dir / "pop.mid", std::ios::binary), b(dir / "pop2.mid", std::ios::binary);
        const std::string x((std::istreambuf_iterator<char>(a)), {}), y((std::istreambuf_iterator<char>(b)), {});
        TEST("all-unresolved map changes nothing in the output", !x.empty() && x == y);
    }

    TEST("convert at 480 PPQN exits 0", run(base + "--output " + q(dir / "pop480.mid") + " --ppqn 480") == 0);
    TEST("--compare same file exits 0", run("--compare " + q(dir / "pop.mid") + " --with " + q(dir / "pop.mid")) == 0);
    TEST("--compare across PPQN (1920 vs 480) exits 0",
         run("--compare " + q(dir / "pop.mid") + " --with " + q(dir / "pop480.mid")) == 0);
    {
        run("--input " + q(other) + " --target pa5x --output " + q(dir / "ballad.mid"));
        TEST("--compare different styles exits 2", run("--compare " + q(dir / "pop.mid") + " --with " + q(dir / "ballad.mid")) == 2);
    }
    TEST("--compare unreadable file exits 1", run("--compare " + q(dir / "pop.mid") + " --with " + q(dir / "none.mid")) == 1);
    TEST("--compare without --with exits 1", run("--compare " + q(dir / "pop.mid")) == 1);

    // ── NIST SHA-256 vectors (conversion_manifest.h) ──
    TEST("SHA-256 empty NIST vector",
         manifest_detail::sha256Hex("") ==
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    TEST("SHA-256 'abc' NIST vector",
         manifest_detail::sha256Hex("abc") ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    TEST("SHA-256 multi-block NIST vector",
         manifest_detail::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // ── Fail-closed flag validation (batch, bundle, bake-rubato, soundmap) ──
    TEST("--batch-dir without --out-dir exits 1", run("--batch-dir " + q(dir)) == 1);
    TEST("--out-dir without --batch-dir exits 1", run("--out-dir " + q(dir)) == 1);
    TEST("--batch-dir combined with --input exits 1",
         run("--batch-dir " + q(dir) + " --out-dir " + q(dir / "b") + " --input " + q(pop)) == 1);
    TEST("--bake-rubato combined with --raw exits 1",
         run(base + "--output " + q(dir / "rb_bad.mid") + " --raw --bake-rubato") == 1);
    TEST("--soundmap missing file exits 1",
         run(base + "--output " + q(dir / "sm_bad.mid") + " --soundmap " + q(dir / "missing.soundmap")) == 1);

    // ── Single-file --out-bundle full artifact & SHA-256 manifest verification ──
    {
        const fs::path bundle = dir / "pop_bundle";
        const fs::path cvStyle = genos_corpus::synthetic("SYNTH_CHORD_MUTE_SPLIT");
        TEST("single-file --out-bundle exits 0",
             run("--input " + q(cvStyle) + " --out-bundle " + q(bundle) + " --ppqn 384 --break-policy cap") == 0);
        const std::vector<std::string> expectedFiles = {
            "converted.mid", "style.uasf", "soundmap.txt",
            "style_edit_sheet.md", "report.md", "unresolved_sounds.md", "manifest.json"
        };
        bool allExist = true;
        for (const auto& fn : expectedFiles) {
            if (!fs::exists(bundle / fn)) allExist = false;
        }
        TEST("--out-bundle emits all 7 structured artifacts", allExist);

        const std::string manifestTxt = readTextFile(bundle / "manifest.json");
        uint64_t inSz = 0;
        const std::string inSha = manifest_detail::sha256FileHex(cvStyle, inSz);
        uint64_t midSz = 0;
        const std::string midSha = manifest_detail::sha256FileHex(bundle / "converted.mid", midSz);
        uint64_t uasfSz = 0;
        const std::string uasfSha = manifest_detail::sha256FileHex(bundle / "style.uasf", uasfSz);
        TEST("manifest.json carries exact SHA-256 hashes & honest provenance",
             manifestTxt.find("\"hardwareVerified\": false") != std::string::npos &&
             manifestTxt.find("\"l3Status\": \"NOT_TESTED\"") != std::string::npos &&
             manifestTxt.find("\"outputFormat\": \"PA5X_MAPPED_SMF_IMPORT_ARTIFACT\"") != std::string::npos &&
             manifestTxt.find("\"nativeStyBinaryVerified\": false") != std::string::npos &&
             manifestTxt.find("\"sha256\": \"" + inSha + "\"") != std::string::npos &&
             manifestTxt.find("\"sha256\": \"" + midSha + "\"") != std::string::npos &&
             manifestTxt.find("\"sha256\": \"" + uasfSha + "\"") != std::string::npos);

        // P1-W1 Style Edit Parameter Sheet Section 0 verification
        const std::string sheetTxt = readTextFile(bundle / "style_edit_sheet.md");
        TEST("P1-W1 style_edit_sheet.md has Section 0 Import SMF, CV1..CV6, Key/Chord & Bass Inversion",
             sheetTxt.find("## 0. Pa5X Import SMF & CV1..CV6 Chord Table / Key-Chord Setup") != std::string::npos &&
             sheetTxt.find("Initialize") != std::string::npos &&
             sheetTxt.find("SHIFT + Execute") != std::string::npos &&
             sheetTxt.find("`v1cv1`") != std::string::npos &&
             sheetTxt.find("`v1cv2`") != std::string::npos &&
             sheetTxt.find("C Min") != std::string::npos &&
             sheetTxt.find("Bass Inversion") != std::string::npos);
    }

    // ── P1-W3: --bake-rubato opt-in on SYNTH_MIXED_METER_RUBATO.sty ──
    {
        const fs::path rubatoSty = genos_corpus::synthetic("SYNTH_MIXED_METER_RUBATO");
        const fs::path bOff = dir / "rubato_off";
        const fs::path bOn = dir / "rubato_on";
        TEST("rubato default (off) exits 0",
             run("--input " + q(rubatoSty) + " --out-bundle " + q(bOff) + " --ppqn 384 --break-policy cap") == 0);
        TEST("rubato --bake-rubato (on) exits 0",
             run("--input " + q(rubatoSty) + " --out-bundle " + q(bOn) + " --ppqn 384 --break-policy cap --bake-rubato") == 0);
        const std::string mOff = readTextFile(bOff / "manifest.json");
        const std::string mOn = readTextFile(bOn / "manifest.json");
        TEST("P1-W3 rubato default off leaves rubatoElementsBaked=0, --bake-rubato bakes >=1 element",
             mOff.find("\"bake_rubato\": false") != std::string::npos &&
             mOff.find("\"rubatoElementsBaked\": 0") != std::string::npos &&
             mOn.find("\"bake_rubato\": true") != std::string::npos &&
             mOn.find("\"rubatoElementsBaked\": 0") == std::string::npos);
    }

    // ── Batch --batch-dir on mixed valid + malformed inputs ──
    {
        const fs::path batchIn = dir / "batch_in";
        const fs::path batchOut = dir / "batch_out";
        fs::create_directories(batchIn / "sub");
        fs::copy_file(pop, batchIn / "01_pop.sty");
        fs::copy_file(other, batchIn / "sub" / "02_waltz.sty");
        writeBytes(batchIn / "03_corrupt.sty", {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0}); // truncated MThd
        std::ofstream(batchIn / "README.txt") << "non-style ignored\n";

        const int brc = run("--batch-dir " + q(batchIn) + " --out-dir " + q(batchOut) + " --ppqn 384 --break-policy cap");
        TEST("batch with 1 corrupt + 2 valid styles isolates failure and exits 2", brc == 2);
        const std::string bm = readTextFile(batchOut / "batch_manifest.json");
        const std::string bs = readTextFile(batchOut / "BATCH_SUMMARY.md");
        TEST("batch_manifest.json & BATCH_SUMMARY.md record 2/3 ok, 1 failed, sorted deterministically",
             fs::exists(batchOut / "01_pop" / "converted.mid") &&
             fs::exists(batchOut / "sub__02_waltz" / "converted.mid") &&
             !fs::exists(batchOut / "03_corrupt" / "converted.mid") &&
             bm.find("\"totalFiles\": 3") != std::string::npos &&
             bm.find("\"succeeded\": 2") != std::string::npos &&
             bm.find("\"failed\": 1") != std::string::npos &&
             bm.find("README.txt") == std::string::npos &&
             bs.find("01_pop.sty") != std::string::npos &&
             bs.find("03_corrupt.sty") != std::string::npos &&
             bs.find("FAILED") != std::string::npos);

        // Clean batch (valid only) exits 0
        fs::remove(batchIn / "03_corrupt.sty");
        const fs::path batchCleanOut = dir / "batch_clean_out";
        TEST("clean batch exits 0",
             run("--batch-dir " + q(batchIn) + " --out-dir " + q(batchCleanOut) + " --ppqn 384 --break-policy cap") == 0);
    }

    // ── Unresolved private-bank (EXP15-style) fail-closed + manual override resolution loop ──
    {
        // Build minimal SFF1-compatible SMF with Main A containing:
        // ch 11 (Acc1): vendor-private bank MSB=65 LSB=12 PC=45 + chord notes -> must stay UNRESOLVED_LOW_CONFIDENCE
        // ch 12 (Acc2): GM SFX MSB=0 LSB=0 PC=125 -> must stay UNSUPPORTED
        std::vector<uint8_t> trk = {
            0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,                                     // 120 BPM
            0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08,                               // 4/4
            0x00, 0xFF, 0x06, 0x06, 'M', 'a', 'i', 'n', ' ', 'A',                         // Main A
            0x00, 0xBB, 0x00, 65, 0x00, 0xBB, 0x20, 12, 0x00, 0xCB, 45,                   // Acc1 private bank 65/12/45
            0x00, 0xBC, 0x00, 0,  0x00, 0xBC, 0x20, 0,  0x00, 0xCC, 125,                  // Acc2 GM SFX 0/0/125
            0x00, 0x9B, 60, 80,   0x00, 0x9B, 64, 80,   0x00, 0x9B, 67, 80,               // Acc1 chord
            0x00, 0x9C, 72, 80,                                                           // Acc2 SFX note
            0x83, 0x60, 0x8B, 60, 0, 0x00, 0x8B, 64, 0, 0x00, 0x8B, 67, 0, 0x00, 0x8C, 72, 0, // tick 480
            0x8B, 0x20, 0xFF, 0x2F, 0x00                                                  // tick 1920 EOT
        };
        std::vector<uint8_t> sty = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                                    'M', 'T', 'r', 'k', 0, 0, 0, static_cast<uint8_t>(trk.size())};
        sty.insert(sty.end(), trk.begin(), trk.end());
        const fs::path exp15Like = dir / "private_bank.sty";
        writeBytes(exp15Like, sty);

        const fs::path unresBundle = dir / "unres_bundle";
        TEST("EXP15-style private-bank conversion exits 0 (fail-closed unresolved)",
             run("--input " + q(exp15Like) + " --out-bundle " + q(unresBundle) + " --ppqn 384") == 0);
        const std::string unresMd = readTextFile(unresBundle / "unresolved_sounds.md");
        const std::string unresManifest = readTextFile(unresBundle / "manifest.json");
        TEST("private-bank & GM SFX reported fail-closed in unresolved_sounds.md & manifest.json",
             unresMd.find("UNRESOLVED_LOW_CONFIDENCE") != std::string::npos &&
             unresMd.find("UNSUPPORTED") != std::string::npos &&
             unresMd.find("acc1 unresolved") != std::string::npos &&
             unresMd.find("acc2 unresolved") != std::string::npos &&
             unresManifest.find("\"status\": \"UNRESOLVED_LOW_CONFIDENCE\"") != std::string::npos &&
             unresManifest.find("\"status\": \"UNSUPPORTED\"") != std::string::npos);

        // Apply manual .soundmap override and confirm USER_OVERRIDE resolves both cleanly!
        const fs::path manualMap = dir / "manual.soundmap";
        std::ofstream(manualMap) << "acc1 121 0 1\nacc2 121 0 48\n";
        const fs::path fixedBundle = dir / "fixed_bundle";
        TEST("manual --soundmap override resolves private-bank tracks cleanly",
             run("--input " + q(exp15Like) + " --out-bundle " + q(fixedBundle) +
                 " --ppqn 384 --soundmap " + q(manualMap)) == 0);
        const std::string fixedMd = readTextFile(fixedBundle / "unresolved_sounds.md");
        const std::string fixedManifest = readTextFile(fixedBundle / "manifest.json");
        TEST("manual override transitions tracks to USER_OVERRIDE with 0 unresolved",
             fixedMd.find("All exported tracks resolved cleanly") != std::string::npos &&
             fixedManifest.find("\"status\": \"USER_OVERRIDE\"") != std::string::npos &&
             fixedManifest.find("\"manual_soundmap_used\": true") != std::string::npos &&
             fixedManifest.find("UNRESOLVED_LOW_CONFIDENCE") == std::string::npos);
    }

    // ── EXP12 stale-track-name kit-switch regression + P1-W2 dump/load round-trip ──
    {
        uasf::StyleDefinition st{};
        st.name = "EXP12_STALE_NAME_TEST";
        st.tempo_bpm = 120;
        st.resolution = 480;
        auto makeSec = [](uasf::SectionType type, const char* secName, uint8_t msb, uint8_t lsb, uint8_t pc) {
            uasf::SectionDefinition s{};
            s.type = type;
            s.name = secName;
            s.bars = 1;
            s.resolution = 480;
            s.beats_per_bar = 4;
            s.beat_note = 4;
            uasf::TrackDefinition tr{};
            tr.name = "SfxPerc"; // Style-wide CASM name containing "sfx"!
            tr.midi_channel = 8; // Rhythm1 -> Perc
            tr.role = uasf::TrackRole::Percussion;
            tr.is_drum = true;
            tr.events.push_back({uasf::MidiEventType::ControlChange, 8, 0, msb, 0});
            tr.events.push_back({uasf::MidiEventType::ControlChange, 8, 32, lsb, 0});
            tr.events.push_back({uasf::MidiEventType::ProgramChange, 8, pc, 0, 0});
            uint64_t tk = 0;
            for (uint8_t p : {uint8_t{88}, uint8_t{92}, uint8_t{96}, uint8_t{104}}) {
                tr.events.push_back({uasf::MidiEventType::NoteOn, 8, p, 90, tk});
                tr.events.push_back({uasf::MidiEventType::NoteOff, 8, p, 0, tk + 120});
                tk += 240;
            }
            s.tracks.push_back(std::move(tr));
            return s;
        };
        st.sections.push_back(makeSec(uasf::SectionType::Main1, "Main A", 126, 0, 0));  // SFX Kit -> 88/92/96/104 remapped onto populated SFX/Perc keys (P3-M4)
        st.sections.push_back(makeSec(uasf::SectionType::Main2, "Main B", 126, 0, 40)); // Ethnic switch -> must NOT be overridden by "SfxPerc"!
        st.sections.push_back(makeSec(uasf::SectionType::Main3, "Main C", 127, 0, 40)); // Brush switch -> must NOT be overridden by "SfxPerc"!

        MapOptions mopt = MapOptions::quality();
        mopt.ppqn = 384;
        mopt.capture_soundmap_dump = true;
        mopt.soundmap_style_name = "EXP12_STALE_NAME_TEST.sty";
        const auto mres = mapToKorg(st, mopt);
        auto firstNotePitch = [&](const char* marker) -> int {
            for (const auto& el : mres.style.elements) {
                if (el.marker != marker) continue;
                for (const auto& ev : el.events) {
                    if ((ev.status & 0xF0u) == 0x90u && ev.d2 > 0u) return ev.d1;
                }
            }
            return -1;
        };
        // Main A (Sfx): 88 -> 75 (Claves); Main B (Ethnic): 88 -> 60 (Darbuka/Bongo); Main C (Brush): 88 -> 40 (Brush Snare).
        TEST("EXP12 stale-track-name & P3-M4 SFX fix: Main A/B/C use distinct Sfx/Ethnic/Brush maps and keep all 12 hits (0 drops)",
             mres.ok && mres.report.drum_notes_unsupported == 0u &&
             (mres.report.drum_notes_remapped_exact + mres.report.drum_notes_remapped_approx) == 12u &&
             firstNotePitch("v1cv1") == 75 &&
             firstNotePitch("v2cv1") == 60 &&
             firstNotePitch("v3cv1") == 40);

        // P1-W2 round-trip: dumped soundmap contains per-element v2.perc / v3.perc lines,
        // and reloading with --no-auto-sound-map produces byte-identical SMF!
        std::istringstream smIn(mres.report.soundmap_dump);
        const auto parsedSm = parseSoundMap(smIn, "dump.soundmap");
        TEST("P1-W2 dumped soundmap includes parseable per-element v2/v3 switches",
             parsedSm.ok &&
             mres.report.soundmap_dump.find("v2.perc ") != std::string::npos &&
             mres.report.soundmap_dump.find("v3.perc ") != std::string::npos &&
             parsedSm.element_sounds.count({uasf::SectionType::Main2, kPercCh}) == 1 &&
             parsedSm.element_sounds.count({uasf::SectionType::Main3, kPercCh}) == 1);

        MapOptions rtOpt = MapOptions::quality();
        rtOpt.ppqn = 384;
        rtOpt.enable_sound_matching = false;
        rtOpt.sounds = parsedSm.sounds;
        rtOpt.element_sounds = parsedSm.element_sounds;
        const auto rtRes = mapToKorg(st, rtOpt);
        const auto w1 = write(mres.style);
        const auto w2 = write(rtRes.style);
        TEST("P1-W2 --dump-soundmap -> --sound-map --no-auto-sound-map round-trip is byte-identical",
             rtRes.ok && w1.ok && w2.ok && w1.bytes == w2.bytes);
    }

    // ── Phase 3 P3-M7/M8: --validation-bundle, --enhance, --analyze-musical-quality, --listening-record ──
    {
        // Fail-closed flag checks
        TEST("invalid --enhance value exits 1",
             run("--input " + q(pop) + " --validation-bundle " + q(dir / "vb_bad1") + " --enhance magic") == 1);
        TEST("--raw + --enhance recommended fails closed (exit 1)",
             run("--input " + q(pop) + " --validation-bundle " + q(dir / "vb_bad2") + " --raw --enhance recommended") == 1);
        TEST("P2-2: --validation-bundle empty string alongside --output fails closed (exit 1) with no output written",
             run("--input " + q(pop) + " --target pa5x --output " + q(dir / "vb_empty_out.mid") +
                 " --validation-bundle \"\"") == 1 &&
             !fs::exists(dir / "vb_empty_out.mid"));
        TEST("--listening-record missing file fails closed (exit 1)",
             run("--input " + q(pop) + " --validation-bundle " + q(dir / "vb_bad3") +
                 " --listening-record " + q(dir / "missing_listening.json")) == 1);

        const fs::path badListening = dir / "malformed_listening.json";
        std::ofstream(badListening) << R"({"evaluatorId":"m1","overallRating":9,"hardwareVerified":true})";
        TEST("malformed/out-of-range --listening-record fails closed (exit 1) without writing bundle",
             run("--input " + q(pop) + " --validation-bundle " + q(dir / "vb_bad4") +
                 " --listening-record " + q(badListening)) == 1 &&
             !fs::exists(dir / "vb_bad4" / "acceptance_report.json"));

        // Standalone --analyze-musical-quality prefix
        const fs::path mqPrefix = dir / "standalone_mq" / "pop_mq";
        TEST("standalone --analyze-musical-quality exits 0 and emits .json + .md",
             run("--input " + q(pop) + " --ppqn 384 --break-policy cap --analyze-musical-quality " + q(mqPrefix)) == 0 &&
             fs::exists(dir / "standalone_mq" / "pop_mq.json") &&
             fs::exists(dir / "standalone_mq" / "pop_mq.md"));
        const std::string mqStandaloneJson = readTextFile(dir / "standalone_mq" / "pop_mq.json");
        TEST("standalone musical quality JSON carries honest offline provenance",
             mqStandaloneJson.find("\"schema\": \"aiarranger-musical-quality/1\"") != std::string::npos &&
             mqStandaloneJson.find("\"hardwareVerified\": false") != std::string::npos);

        // --validation-bundle with default --enhance off (Mode A only -> Grade U!)
        const fs::path vbOff = dir / "vb_off";
        TEST("--validation-bundle default (--enhance off) exits 0",
             run("--input " + q(pop) + " --ppqn 384 --break-policy cap --validation-bundle " + q(vbOff)) == 0);
        const std::vector<std::string> expectedOffFiles = {
            "candidate_b_faithful/converted.mid",
            "candidate_b_faithful/style.uasf",
            "candidate_b_faithful/soundmap.txt",
            "candidate_b_faithful/style_edit_sheet.md",
            "candidate_b_faithful/report.md",
            "candidate_b_faithful/unresolved_sounds.md",
            "candidate_b_faithful/manifest.json",
            "musical_quality_faithful.json",
            "musical_quality_faithful.md",
            "acceptance_report.json",
            "acceptance_report.md",
            "hardware_session_staging/SHA256SUMS.txt",
            "hardware_session_staging/style_edit_sheet.md",
            "hardware_session_staging/OPERATOR_PREP_NOTES.md",
            "hardware_session_staging/LISTENING_RECORD_TEMPLATE.json",
            "VALIDATION_SUMMARY.md"
        };
        bool allOffExist = true;
        for (const auto& rel : expectedOffFiles) {
            if (!fs::exists(vbOff / rel)) allOffExist = false;
        }
        TEST("--validation-bundle default creates all Mode A + staging artifacts and NO candidate_c_enhanced",
             allOffExist && !fs::exists(vbOff / "candidate_c_enhanced") && !fs::exists(vbOff / "comparison_b_vs_c.md"));

        // Verify offline-only default yields Grade U (NEVER Grade A), PROPOSED_ACCEPTANCE_CRITERIA, and OFFLINE_STAGING_ONLY
        const std::string accOffJson = readTextFile(vbOff / "acceptance_report.json");
        const std::string prepNotes = readTextFile(vbOff / "hardware_session_staging" / "OPERATOR_PREP_NOTES.md");
        const std::string tmplJson = readTextFile(vbOff / "hardware_session_staging" / "LISTENING_RECORD_TEMPLATE.json");
        TEST("offline --validation-bundle yields Grade U (never Grade A) & stamps OFFLINE_STAGING_ONLY",
             accOffJson.find("\"criteriaStatus\": \"PROPOSED_ACCEPTANCE_CRITERIA\"") != std::string::npos &&
             accOffJson.find("\"grade\": \"U\"") != std::string::npos &&
             accOffJson.find("\"hardwareVerified\": false") != std::string::npos &&
             accOffJson.find("\"l3Status\": \"NOT_TESTED\"") != std::string::npos &&
             prepNotes.find("OFFLINE_STAGING_ONLY") != std::string::npos &&
             prepNotes.find("NO_HARDWARE_WRITES") != std::string::npos &&
             prepNotes.find("HARDWARE_AUTHORIZATION_REQUIRED") != std::string::npos &&
             tmplJson.find("\"is_template\": true") != std::string::npos &&
             tmplJson.find("\"source_class\": \"UNSPECIFIED_PENDING_PROVENANCE\"") != std::string::npos &&
             tmplJson.find("\"source_class\": \"REAL_GENOS_HUMAN\"") == std::string::npos &&
             tmplJson.find("\"overall_rating_1_to_5\": null") != std::string::npos);

        // DEF-P4-011: Even if is_template is flipped to false, leaving source_class as placeholder MUST fail closed!
        const fs::path unfilledSrcClass = dir / "unfilled_source_class.json";
        std::ofstream(unfilledSrcClass) << R"({
            "schema": "aiarranger-human-listening-record/1",
            "is_template": false,
            "source_class": "UNSPECIFIED_PENDING_PROVENANCE",
            "evaluator_id": "EVAL_01",
            "hardware_verified": true,
            "hardware_model": "Korg Pa5X",
            "overall_rating_1_to_5": 5
        })";
        TEST("P4-M6 DEF-P4-011: --listening-record rejects UNSPECIFIED_PENDING_PROVENANCE fail-closed (exit 1)",
             run("--input " + q(pop) + " --ppqn 384 --validation-bundle " + q(dir / "vb_src_rej") +
                 " --listening-record " + q(unfilledSrcClass)) == 1);

        // Template placeholder cannot be blindly fed back to --listening-record
        TEST("unfilled LISTENING_RECORD_TEMPLATE.json is rejected fail-closed by --listening-record",
             run("--input " + q(pop) + " --ppqn 384 --validation-bundle " + q(dir / "vb_tmpl_rej") +
                 " --listening-record " + q(vbOff / "hardware_session_staging" / "LISTENING_RECORD_TEMPLATE.json")) == 1);

        // Verify bit-exact parity of Mode A converted.mid between --out-bundle and --validation-bundle
        const fs::path refBundle = dir / "pop_ref_bundle";
        TEST("reference --out-bundle exits 0",
             run("--input " + q(pop) + " --ppqn 384 --break-policy cap --out-bundle " + q(refBundle)) == 0);
        TEST("--validation-bundle candidate_b_faithful/converted.mid is bit-identical to --out-bundle",
             readTextFile(vbOff / "candidate_b_faithful" / "converted.mid") ==
                 readTextFile(refBundle / "converted.mid"));

        // --validation-bundle with --enhance recommended: produces B (untouched!) AND C + comparison_b_vs_c.md
        const fs::path vbEnh = dir / "vb_enh";
        TEST("--validation-bundle --enhance recommended exits 0",
             run("--input " + q(pop) + " --ppqn 384 --break-policy cap --enhance recommended --validation-bundle " +
                 q(vbEnh)) == 0);
        TEST("--enhance recommended emits candidate_c_enhanced, musical_quality_enhanced, comparison_b_vs_c.md & leaves Mode A untouched",
             fs::exists(vbEnh / "candidate_c_enhanced" / "converted.mid") &&
             fs::exists(vbEnh / "candidate_c_enhanced" / "enhancement_actions.json") &&
             fs::exists(vbEnh / "candidate_c_enhanced" / "enhancement_actions.md") &&
             fs::exists(vbEnh / "candidate_c_enhanced" / "manifest.json") &&
             fs::exists(vbEnh / "musical_quality_enhanced.json") &&
             fs::exists(vbEnh / "musical_quality_enhanced.md") &&
             fs::exists(vbEnh / "comparison_b_vs_c.md") &&
             readTextFile(vbEnh / "candidate_b_faithful" / "converted.mid") ==
                 readTextFile(vbOff / "candidate_b_faithful" / "converted.mid"));

        // Valid hardware-verified --listening-record unlocks Grade A
        const fs::path goodListening = dir / "verified_listening.json";
        std::ofstream(goodListening) << R"({
            "schema": "aiarranger-human-listening-record/1",
            "templatePlaceholder": false,
            "evaluatorId": "musician-01",
            "overallRating": 5,
            "criticalDefectCount": 0,
            "minorDefectCount": 0,
            "hardwareVerified": true,
            "hardwareModel": "Korg Pa5X",
            "referenceType": "Genos_Direct_AB",
            "notes": "Verified across all variations and chord types."
        })";
        const fs::path vbGradeA = dir / "vb_grade_a";
        TEST("valid hardware-verified --listening-record yields Grade A",
             run("--input " + q(pop) + " --ppqn 384 --break-policy cap --validation-bundle " + q(vbGradeA) +
                 " --listening-record " + q(goodListening)) == 0);
        const std::string accAJson = readTextFile(vbGradeA / "acceptance_report.json");
        TEST("Grade A acceptance_report.json records grade A and hardwareVerified=true",
             accAJson.find("\"grade\": \"A\"") != std::string::npos &&
             accAJson.find("\"hardwareVerified\": true") != std::string::npos &&
             accAJson.find("\"l3Status\": \"VERIFIED\"") != std::string::npos);

        // EXP15-style unresolved private-bank style -> Grade C offline -> Grade U after --sound-map!
        const fs::path exp15Like = dir / "private_bank.sty";
        const fs::path vbExp15Unres = dir / "vb_exp15_unres";
        TEST("EXP15-style unresolved sound in --validation-bundle exits 0 and assigns Grade C",
             run("--input " + q(exp15Like) + " --ppqn 384 --validation-bundle " + q(vbExp15Unres)) == 0 &&
             readTextFile(vbExp15Unres / "acceptance_report.json").find("\"grade\": \"C\"") != std::string::npos);

        const fs::path manualMap = dir / "manual.soundmap";
        const fs::path vbExp15Fixed = dir / "vb_exp15_fixed";
        TEST("EXP15-style resolved via --sound-map moves from Grade C to Grade U (offline)",
             run("--input " + q(exp15Like) + " --ppqn 384 --sound-map " + q(manualMap) +
                 " --validation-bundle " + q(vbExp15Fixed)) == 0 &&
             readTextFile(vbExp15Fixed / "acceptance_report.json").find("\"grade\": \"U\"") != std::string::npos);
    }

    fs::remove_all(dir);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
