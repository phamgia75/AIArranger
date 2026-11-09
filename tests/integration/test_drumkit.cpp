// Drum Kit model + TX16Wx Performance writer (docs/research/TXPERF_DRUMKIT_NOTES.md): validation
// limits from Korg's guide, file structure mirroring Korg's "Real-Drums DrumKit" example, determinism,
// path safety, refusals. SOFTWARE evidence only: no file here has been imported on a Pa5X.

#include "media/sound/drum_source.h"
#include "media/sound/package_writer.h"
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger::media::sound;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static std::string text(const PackageFile& f) { return std::string(f.bytes.begin(), f.bytes.end()); }
static const PackageFile* find(const std::vector<PackageFile>& v, const std::string& suffix) {
    for (const auto& f : v) if (f.path.size() >= suffix.size() && f.path.compare(f.path.size() - suffix.size(), suffix.size(), suffix) == 0) return &f;
    return nullptr;
}
static bool rejects(DrumKit k, const char* expect) {
    for (const auto& e : validate(k)) if (e.find(expect) != std::string::npos) return writeTxPerf(k).empty();
    std::fprintf(stderr, "    expected a message containing: %s\n", expect);
    return false;
}

int main() {
    std::printf("Test: Drum Kit (TXPERF)\n");
    const DrumKit kit = buildDemoKit();
    const auto ve = validate(kit);
    TEST("demo kit is valid", ve.empty());
    for (const auto& e : ve) std::fprintf(stderr, "    %s\n", e.c_str());
    const auto files = writeTxPerf(kit);
    TEST("writer: .txperf, .txprog, 7 matrices, 9 WAVs", files.size() == 2 + 7 + 9 &&
         files[0].path == "AIA Demo Kit/AIA Demo Kit.txperf" && files[1].path == "AIA Demo Kit/AIA Demo Kit.txprog");
    TEST("deterministic (byte-identical second run)", [&] {
        const auto again = writeTxPerf(buildDemoKit());
        if (again.size() != files.size()) return false;
        for (size_t i = 0; i < files.size(); ++i) if (again[i].path != files[i].path || again[i].bytes != files[i].bytes) return false;
        return true;
    }());
    const std::string perf = text(files[0]), prog = text(files[1]);
    TEST("txperf: BOM, created-by 30601, one slot pointing at the .txprog",
         perf.rfind("\xEF\xBB\xBF<?xml", 0) == 0 && perf.find("tx:created-by=\"30601\"") != std::string::npos &&
         perf.find("tx:program=\"AIA Demo Kit.txprog\"") != std::string::npos && perf.find("<tx:slot") == perf.rfind("<tx:slot"));
    TEST("txprog: hi-hat group has choke-group 1, others 0, one-shot groups",
         prog.find("tx:name=\"Hi-hat\"") != std::string::npos &&
         prog.find("tx:choke-group=\"1\"", prog.find("tx:name=\"Hi-hat\"")) < prog.find("<tx:region", prog.find("tx:name=\"Hi-hat\"")) &&
         prog.find("tx:choke-group=\"1\"") == prog.rfind("tx:choke-group=\"1\"") && prog.find("tx:playmode=\"Normal\"") == std::string::npos);
    TEST("txprog: each key a single-key region over velocity 0-127 with pan/shift/tune",
         prog.find("tx:low-key=\"F#2\" tx:low-vel=\"0\" tx:high-key=\"F#2\"") != std::string::npos &&
         prog.find("tx:pan=\"30%\"") != std::string::npos && prog.find("tx:pan=\"-25%\"") != std::string::npos &&
         prog.find("tx:shift=\"0\"") != std::string::npos);
    const PackageFile* kick = find(files, "/B_AIA Kick.txmatrix");
    TEST("kick matrix: family prefix B_, Y = velocity, rows 0 and 90, root C4 on each wave",
         kick && text(*kick).find("tx:name=\"B_AIA Kick\"") != std::string::npos &&
         text(*kick).find("<tx:yaxis tx:source=\"Vel\">") != std::string::npos &&
         text(*kick).find("<tx:row tx:value=\"0\">") != std::string::npos && text(*kick).find("<tx:row tx:value=\"90\">") != std::string::npos &&
         text(*kick).find("tx:root=\"C4\"") != std::string::npos && text(*kick).find("AIA Demo Kit Samples/AIA Kick Hard.wav") != std::string::npos);
    const PackageFile* snare = find(files, "/S_AIA Snare.txmatrix");
    TEST("cell level written like Korg's example (-1.0 dB)", snare && text(*snare).find("tx:attenuation=\"-1.0 dB\"") != std::string::npos);
    const PackageFile* open = find(files, "/AIA HH Open.wav");
    TEST("stereo sample kept (open hi-hat: 2 channels)", open && open->bytes[22] == 2);
    bool noDots = true;
    for (const auto& f : files) noDots &= f.path.find("..") == std::string::npos && f.path[0] != '/';
    TEST("no absolute or parent paths in the package", noDots);

    // loop preserved
    {
        DrumKit k = kit;
        Sample& s = k.samples[0];
        s.looped = true; s.loopStart = 1000; s.loopEnd = 9000;
        const auto f = writeTxPerf(k);
        const PackageFile* m = find(f, "/B_AIA Kick.txmatrix");
        TEST("looped sample: matrix wave carries the Forward loop (end exclusive)",
             m && text(*m).find("<tx:loop tx:end=\"9000\" tx:start=\"1000\" tx:mode=\"Forward\" tx:name=\"1\"/>") != std::string::npos);
    }
    // escaping and path safety
    {
        DrumKit k = kit;
        k.name = "A&B \"Kit\" <1>";
        k.instruments[0].keys[0].name = "../../etc";
        const auto f = writeTxPerf(k);
        bool safe = !f.empty();
        for (const auto& x : f) safe &= x.path.find("..") == std::string::npos && x.path.find("/etc") == std::string::npos;
        TEST("names are XML-escaped and file names cannot leave the package",
             safe && text(f[1]).find("tx:name=\"A&amp;B &quot;Kit&quot; &lt;1&gt;\"") != std::string::npos);
    }

    // ── refusals ──
    { DrumKit k = kit; k.instruments[1].keys[0].key = 36; TEST("duplicate key refused", rejects(k, "already assigned")); }
    { DrumKit k = kit; auto& L = k.instruments[0].keys[0].layers; L.clear();
      for (int i = 0; i < 17; ++i) L.push_back({0, uint8_t(i * 7), 0});
      TEST("17 velocity layers refused", rejects(k, "1-16 velocity layers")); }
    { DrumKit k = kit; k.instruments[0].keys[0].layers[0].velLo = 1; TEST("first layer not starting at 0 refused", rejects(k, "start at 0")); }
    { DrumKit k = kit; k.instruments[0].keys[0].layers[1].velLo = 0; TEST("non-ascending layers refused", rejects(k, "ascend")); }
    { DrumKit k = kit; k.instruments[2].choke = 9; TEST("exclusive group 9 refused", rejects(k, "exclusive group")); }
    { DrumKit k = kit; k.instruments[0].keys[0].layers[0].gainDb10 = 61; TEST("level above +6 dB refused", rejects(k, "+6 dB")); }
    { DrumKit k = kit; k.instruments[0].keys[0].transpose = 13; TEST("transpose beyond +-12 refused", rejects(k, "transpose")); }
    { DrumKit k = kit; k.instruments[0].keys[0].tuneCents = 51; TEST("tune beyond +-50 cents refused", rejects(k, "tune")); }
    { DrumKit k = kit; k.instruments[0].keys[0].panPct = 101; TEST("pan beyond 100% refused", rejects(k, "pan")); }
    { DrumKit k = kit; k.instruments[0].keys[0].family = 'Z'; TEST("unknown family refused", rejects(k, "family")); }
    { DrumKit k = kit; k.instruments[0].keys[0].name = "Nineteen chars long"; TEST("name over 20 with prefix refused", rejects(k, "1-20")); }
    // regression (Codex review): control characters passed validation and were written into the XML
    { DrumKit k = kit; k.name = "Kit\x01"; TEST("control character in the kit name refused", rejects(k, "printable ASCII")); }
    { DrumKit k = kit; k.instruments[0].name = "Kick\x7F"; TEST("DEL in an instrument name refused", rejects(k, "printable ASCII")); }
    { DrumKit k = kit; k.instruments[0].keys[0].name = "Kick\xC3"; TEST("non-ASCII byte in a key name refused", rejects(k, "printable ASCII")); }
    { DrumKit k = kit; k.samples.push_back(k.samples[0]); k.samples.back().name = "Orphan"; TEST("unused sample refused", rejects(k, "orphan")); }
    { DrumKit k = kit; k.instruments[0].keys[0].layers[0].sample = 99; TEST("missing sample refused", rejects(k, "no sample")); }
    { DrumKit k = kit; k.instruments.clear(); k.samples.clear(); TEST("empty kit refused", rejects(k, "at least one")); }
    { DrumKit k = kit; k.instruments[3].keys[1].name = "AIA Tom/Hi"; k.instruments[3].keys[0].name = "AIA Tom_Hi";
      TEST("two matrices mapping to one file name refused", validate(k).empty() && writeTxPerf(k).empty()); }
    { DrumKit k = kit; k.samples[1].name = "aia kick soft";
      TEST("WAV names differing only in case refused (FAT USB sticks)", validate(k).empty() && writeTxPerf(k).empty()); }

    // package
    const fs::path root = fs::temp_directory_path() / ("aia-drumkit-" + std::to_string(::getpid()));
    fs::remove_all(root);
    std::string err;
    const std::string rep = writeDrumPackage(kit, root / "pkg", "", &err);
    TEST("package written with REPORT separating imported and manual settings",
         !rep.empty() && fs::is_regular_file(root / "pkg/tx/AIA Demo Kit/AIA Demo Kit.txperf") &&
         rep.find("Imported by the Pa5X") != std::string::npos && rep.find("NOT imported") != std::string::npos &&
         rep.find("H_AIA HH Open") != std::string::npos);
    TEST("REPORT states the 4/8 GB budget and irreversible compression", rep.find("8 GB") != std::string::npos && rep.find("cannot be undone") != std::string::npos);
    TEST("package refuses an existing folder", writeDrumPackage(kit, root / "pkg", "", &err).empty() && err.find("exists") != std::string::npos);
    fs::remove_all(root);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
