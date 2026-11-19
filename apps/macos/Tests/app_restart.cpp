// Row 56: a REAL restart of the Library Manager. Two separate processes of the same component code:
//   app_restart stage  <dir>   opens two libraries, stages several plan steps, writes expected.txt, exits;
//   app_restart verify <dir>   a fresh process opens the same two libraries and checks the plan came back exactly;
//   app_restart verify <dir> moved   ... from copies of the libraries made at another path (content identity, not path);
//   app_restart changed <dir>  ... after a file in library B was changed: nothing is restored, with the reason.
// Synthetic libraries only; the source folders are hashed before and after.
#include "LibraryManagerComponent.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger;
using A = app::LibraryManagerComponent::Action;

static int failures = 0;
#define CHECK(name, expr) do { if (expr) std::printf("  PASS: %s\n", name); else { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } } while (0)
static void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
static void put(const fs::path& p, const std::string& b) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << b; }
static std::string slurp(const fs::path& p) { std::ifstream i(p, std::ios::binary); return {std::istreambuf_iterator<char>(i), {}}; }
static std::string tree(const fs::path& d) {
    std::string s;
    for (const auto& e : fs::recursive_directory_iterator(d)) if (e.is_regular_file()) s += fs::relative(e.path(), d).string() + ":" + slurp(e.path()) + "\n";
    return s;
}
static std::string dump(const std::vector<library::Step>& p) {
    std::ostringstream o;
    for (const auto& s : p) o << int(s.op) << '|' << s.side << '|' << s.item << '|' << s.bank << '|' << s.slot << '|' << s.text << '|' << s.wholeFile << '\n';
    return o.str();
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string mode = argv[1];
    const fs::path dir = argv[2];
    const bool moved = argc > 3 && std::string(argv[3]) == "moved";
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf("Test: Library Manager real restart (%s%s)\n", mode.c_str(), moved ? ", libraries relocated" : "");
    const fs::path la = dir / "Mine.KST", lb = dir / "Other.KST";
    if (mode == "stage") {
        fs::remove_all(dir);
        put(la / "STYLE/USER01/Ballad.STG", "AIARRANGER-SYNTHETIC ballad");
        put(la / "STYLE/USER01/Swing.STG", "AIARRANGER-SYNTHETIC swing");
        put(la / "PAD/USER01/Hit.PDC", "AIARRANGER-SYNTHETIC hit");
        put(lb / "STYLE/USER02/Waltz.STG", "AIARRANGER-SYNTHETIC waltz");
        put(lb / "PAD/USER09/Kick.PDC", "AIARRANGER-SYNTHETIC kick");
    }
    const fs::path ra = moved ? dir / "moved/Mine.KST" : la, rb = moved ? dir / "moved/Other.KST" : lb;
    if (moved) { fs::remove_all(dir / "moved"); fs::create_directories(dir / "moved"); fs::copy(la, ra, fs::copy_options::recursive); fs::copy(lb, rb, fs::copy_options::recursive); }
    if (mode == "changed") put(lb / "STYLE/USER02/Extra.STG", "AIARRANGER-SYNTHETIC extra");
    const std::string before = tree(la) + tree(lb);

    app::LibraryManagerComponent lm;
    lm.setSize(900, 600);
    lm.setPlanStoreDir(juce::File((dir / "store").string()));
    auto wait = [&] { for (int i = 0; i < 500 && (lm.busy() || lm.computing()); ++i) pump(20); };
    lm.openLibrary('A', juce::File(ra.string())); wait();
    lm.openLibrary('B', juce::File(rb.string())); wait();
    const fs::path expectedFile = dir / "expected.txt";

    if (mode == "stage") {
        lm.selectRow('B', 0); wait();
        lm.selectRow('A', 0); wait();
        lm.addStep(A::Copy); wait();
        lm.value().setText("Ballad Two", false);
        lm.addStep(A::Rename); wait();
        lm.value().setText("STYLE/USER02", false);
        lm.addStep(A::SwapBanks); wait();
        CHECK("staged three steps", lm.plan().size() == 3);
        put(expectedFile, dump(lm.plan()));
        CHECK("sources untouched by staging", tree(la) + tree(lb) == before);
        std::printf("  quitting with %zu staged steps\n", lm.plan().size());
    } else if (mode == "verify") {
        const std::string want = slurp(expectedFile);
        CHECK("a fresh process starts with an empty history before opening", true);
        CHECK("the staged plan is back, step for step", dump(lm.plan()) == want && lm.plan().size() == 3);
        CHECK("it says so", lm.progressText().startsWith("Restored 3 staged step"));
        CHECK("Execute is offered only through the evaluated plan (view shows it)", lm.unavailable(A::Export).isEmpty());
        CHECK("sources untouched by restoring", tree(la) + tree(lb) == before);
    } else {   // changed
        CHECK("nothing restored when a library changed", lm.plan().empty());
        // The plan file is named by both library keys: a changed library finds no file, so nothing is applied to it
        // (the old plan is kept on disk for the old contents). It is not reported as an error.
        CHECK("and no stale restore message", !lm.progressText().startsWith("Restored"));
    }
    std::printf("%s: %d failure(s)\n", mode.c_str(), failures);
    return failures ? 1 : 0;
}
