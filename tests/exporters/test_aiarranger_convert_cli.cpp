// aiarranger-convert exit codes, end to end: 0 written+validated, 1 usage or
// input error, 2 conversion failed (and no output file), plus the sound-map
// template and --compare round-trip modes.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

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

int main() {
    std::printf("Test: aiarranger-convert CLI exit codes\n");
    const fs::path dir = fs::temp_directory_path() / ("aiarr_cli_" + std::to_string(std::rand()) + std::to_string(::getpid()));
    fs::remove_all(dir);   // never inherit files from an earlier crashed run
    fs::create_directories(dir);

    fs::path pop;
    for (const auto& f : fs::directory_iterator(CORPUS_DIR))
        if (f.path().extension() == ".sty" && f.path().filename().string().rfind("POP_ACOUSTIC_2", 0) == 0) pop = f.path();
    TEST("corpus style present", !pop.empty());

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
        fs::path other;
        for (const auto& f : fs::directory_iterator(CORPUS_DIR))
            if (f.path().extension() == ".sty" && f.path().filename().string().rfind("BALLAD_FOLK", 0) == 0) other = f.path();
        run("--input " + q(other) + " --target pa5x --output " + q(dir / "ballad.mid"));
        TEST("--compare different styles exits 2", run("--compare " + q(dir / "pop.mid") + " --with " + q(dir / "ballad.mid")) == 2);
    }
    TEST("--compare unreadable file exits 1", run("--compare " + q(dir / "pop.mid") + " --with " + q(dir / "none.mid")) == 1);
    TEST("--compare without --with exits 1", run("--compare " + q(dir / "pop.mid")) == 1);

    fs::remove_all(dir);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
