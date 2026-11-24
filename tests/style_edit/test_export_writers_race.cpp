// Every by-path export writer (jobs/export_specs.h) under a path race: the destination's parent is swapped for a symlink to
// a job source WHILE the writer runs. The write must stay below the pinned job folder (or the job must fail with nothing
// written in the source). One case per export type that still has a `write` callback. Synthetic data only.

#include "jobs/export_specs.h"
#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include "media/sound/wav_writer.h"
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
static std::vector<std::string> names(const fs::path& dir) {
    std::vector<std::string> r;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) r.push_back(e.path().filename().string());
    std::sort(r.begin(), r.end());
    return r;
}
static JobState run(const ExportSpec& s) {
    JobState st;
    st.id = 1;
    runExport(s, st, [] { return false; }, [](const JobState&) {});
    return st;
}

int main() {
    std::printf("Test: export writers under path races\n");
    const fs::path root = fs::weakly_canonical(fs::temp_directory_path()) / ("aia-export-wrace-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const auto organ = std::make_shared<const media::sound::Instrument>(media::sound::buildDemoOrgan());
    const auto kit = std::make_shared<const media::sound::DrumKit>(media::sound::buildDemoKit());
    media::sound::Sample smp;
    smp.rate = 44100;
    smp.pcm.assign(44100, 0);
    for (size_t i = 0; i < 4; ++i) for (size_t j = 0; j < 4000; ++j) smp.pcm[i * 11025 + j] = int16_t(9000 * std::sin(double(j) / 7));
    const auto wav = media::sound::writeWav(smp);
    put(root / "labin/a.wav", std::string(wav.begin(), wav.end()));
    put(root / "labin/b.wav", std::string(wav.begin(), wav.end()));
    put(root / "Mine.KST/STYLE/BANK01.STY", std::string(4000, 'x'));
    put(root / "Mine.KST/SOUND/BANK01.PCG", std::string(3000, 'y'));

    struct Case { const char* name; std::function<ExportSpec(const fs::path&)> make; };
    const std::vector<Case> cases = {
        {"Sound package", [&](const fs::path& d) { return soundPackage(organ, {}, "", d); }},
        {"Drum Kit package", [&](const fs::path& d) { return drumKit(kit, "synthetic", d); }},
        {"KMP/KSF folder", [&](const fs::path& d) { return kmpFolder(organ, d); }},
        {"Sample Lab batch", [&](const fs::path& d) { return labBatch(root / "labin", true, false, d); }},
        {"Sample Lab slice", [&](const fs::path& d) { return labSlice(root / "labin/a.wav", 1, 4, d); }},
        {"Sample Lab loop", [&](const fs::path& d) { return labLoop(root / "labin/a.wav", 1000, 20000, 0, 0, d); }},
        {"Style UASF (file)", [&](const fs::path& d) { return styleUasf("x", {'u', 'a', 's', 'f'}, d); }},
        {"Style SMF (file)", [&](const fs::path& d) { return styleSmf("x", style_edit::SmfExport{}, d); }},
        {"Drum Kit KMP", [&](const fs::path& d) { return drumKitKmp(kit, "synthetic", d); }},
        {"Rendered package copy", [&](const fs::path& d) {
             const fs::path staged = root / "staged-pkg";
             if (!fs::exists(staged)) run(soundPackage(organ, {}, "", staged));
             return stagedPackage(staged, d);
         }},
        {"KST backup", [&](const fs::path& d) { return kstBackup(root / "Mine.KST", d); }},
    };
    int n = 0;
    for (const auto& c : cases) {
        const fs::path base = root / ("w" + std::to_string(n++));
        put(base / "src/keep.txt", "source");
        fs::create_directories(base / "p");
        auto s = c.make(base / "p/out");
        s.sources.push_back(base / "src");
        const auto real = s.write;
        s.write = [&, real](const fs::path& out, const std::function<bool(double)>& pr) {
            fs::rename(base / "p", base / "p.moved");              // the attacker's moment: just before the writer touches disk
            fs::create_directory_symlink(base / "src", base / "p");
            return real(out, pr);
        };
        const auto st = run(s);
        TEST((std::string(c.name) + ": swapped parent during the writer: job fails").c_str(), st.phase == Phase::Failed);
        TEST((std::string(c.name) + ": nothing is created in the source").c_str(), names(base / "src") == std::vector<std::string>{"keep.txt"});
        TEST((std::string(c.name) + ": nothing is published").c_str(), !fs::exists(base / "p.moved/out") && !fs::exists(base / "p.moved/out.export.md"));
    }

    // A writer that reaches for an absolute path OUTSIDE its pinned job folder is refused (nothing is created there).
    {
        const fs::path base = root / "outside";
        fs::create_directories(base / "p");
        ExportSpec s;
        s.type = "Race test";
        s.source = "synthetic";
        s.dest = base / "p/out";
        s.write = [&](const fs::path& out, const std::function<bool(double)>&) {
            const auto a = media::writeNewFile(base / "elsewhere.txt", {'x'});   // outside the job folder
            const auto b = media::makeFolder(base / "elsewhere-dir");
            media::makeFolder(out);
            return WriteResult{(!a.empty() && !b) ? "" : "an outside write was allowed", ""};
        };
        const auto st = run(s);
        TEST("a pinned writer cannot create files or folders outside the job folder", !fs::exists(base / "elsewhere.txt") && !fs::exists(base / "elsewhere.txt.tmp") &&
             !fs::exists(base / "elsewhere-dir") && st.reason.find("outside write") == std::string::npos);
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
