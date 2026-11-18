// Row 56: the staged Library Manager plan persists safely. Synthetic libraries only (placeholder bytes).
#include "library/library_history.h"
#include "library/library_plan_store.h"
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger::library;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static void put(const fs::path& p, const std::string& b) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << b; }
static std::string slurp(const fs::path& p) { std::ifstream i(p, std::ios::binary); return {std::istreambuf_iterator<char>(i), {}}; }
static bool has(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }
// Hash of every file under a folder, to prove the sources were not touched.
static std::string treeState(const fs::path& d) {
    std::string s;
    for (const auto& e : fs::recursive_directory_iterator(d)) if (e.is_regular_file()) s += e.path().string() + ":" + std::to_string(fs::file_size(e)) + ":" + slurp(e.path()) + "\n";
    return s;
}

int main() {
    std::printf("Test: Library Manager plan persistence (row 56)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-planstore-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path ka = root / "A.KST", kb = root / "B.KST", data = root / "appdata";
    put(ka / "STYLE/USER01/Ballad.STG", "AIARRANGER-SYNTHETIC ballad");
    put(ka / "STYLE/USER01/Swing.STG", "AIARRANGER-SYNTHETIC swing");
    put(kb / "STYLE/USER02/Waltz.STG", "AIARRANGER-SYNTHETIC waltz");
    const Library A = load(ka), B = load(kb);
    TEST("synthetic libraries open", A.ok && B.ok && A.items.size() == 2 && B.items.size() == 1);

    std::vector<Step> plan;
    { Step s; s.op = Op::Copy; s.side = 'A'; s.item = 1; s.bank = "STYLE/USER02"; s.slot = 3; plan.push_back(s); }
    { Step s; s.op = Op::Rename; s.side = 'B'; s.item = 0; s.text = "Wal\ttz %41\nnew"; s.wholeFile = true; plan.push_back(s); }
    { Step s; s.op = Op::BankSwap; s.side = 'A'; s.bank = "STYLE/USER01"; s.text = "STYLE/USER02"; plan.push_back(s); }

    const std::string before = treeState(ka) + treeState(kb);
    const std::string text = serializePlan(plan, A, B);
    const fs::path file = planFilePath(data, A, B);
    TEST("save writes the plan file", savePlan(file, text, A, B).empty() && fs::exists(file));
    TEST("no .INCOMPLETE left behind", !fs::exists(file.string() + ".INCOMPLETE"));
    TEST("sources untouched by save", treeState(ka) + treeState(kb) == before);

    std::string back;
    std::vector<Step> got;
    TEST("load reads it", loadPlanText(file, back).empty() && back == text);
    TEST("parse accepts it", parsePlan(back, A, B, got).empty() && got.size() == 3);
    bool same = got.size() == 3;
    for (size_t i = 0; same && i < 3; ++i)
        same = got[i].op == plan[i].op && got[i].side == plan[i].side && got[i].item == plan[i].item && got[i].bank == plan[i].bank &&
               got[i].slot == plan[i].slot && got[i].text == plan[i].text && got[i].wholeFile == plan[i].wholeFile;
    TEST("round trip is exact (tab, percent, newline in a name)", same);
    TEST("the restored plan evaluates like the original", evaluate(A, B, got).steps.size() == evaluate(A, B, plan).steps.size());

    // Rejections: each returns a reason and leaves no steps.
    auto refused = [&](const std::string& t, const char* why) { std::vector<Step> o{Step{}}; const std::string r = parsePlan(t, A, B, o); return !r.empty() && o.empty() && has(r, why); };
    std::string flipped = text; flipped[flipped.find("Wal")] = 'X';
    TEST("damaged file refused (checksum)", refused(flipped, "checksum"));
    TEST("truncated file refused", refused(text.substr(0, text.size() / 2), "checksum"));
    TEST("empty file refused", refused("", "checksum"));
    auto resign = [](std::string body) { return body + "sha256 " + detail::sha(body) + "\n"; };
    const std::string body = text.substr(0, text.rfind("sha256 "));
    std::string v2 = body; v2.replace(v2.find(" 1\n"), 3, " 2\n");
    TEST("unknown version refused even with a valid checksum", refused(resign(v2), "version"));
    std::string badop = body; badop.replace(badop.find("step\t0"), 6, "step\t9");
    TEST("unknown operation refused", refused(resign(badop), "operation"));
    std::string badidx = body; badidx.replace(badidx.find("step\t0\tA\t1"), 10, "step\t0\tA\t7");
    TEST("item index outside the library refused", refused(resign(badidx), "item index"));
    std::string badesc = body; badesc.replace(badesc.find("Wal"), 3, "W%Z");
    TEST("bad escape refused", refused(resign(badesc), "escape"));

    // Libraries changed since the save.
    put(kb / "STYLE/USER02/Extra.STG", "AIARRANGER-SYNTHETIC extra");
    const Library B2 = load(kb);
    std::vector<Step> o;
    const std::string r = parsePlan(text, A, B2, o);
    TEST("plan refused when a library changed", has(r, "different libraries") && o.empty());
    TEST("a changed pair gets its own plan file name", planFilePath(data, A, B2) != file);
    fs::remove(kb / "STYLE/USER02/Extra.STG");
    TEST("moving a library keeps the plan valid", [&] {
        fs::copy(ka, root / "A-moved.KST", fs::copy_options::recursive);
        const Library Am = load(root / "A-moved.KST");
        std::vector<Step> x;
        return parsePlan(text, Am, B, x).empty() && x.size() == 3;
    }());

    // Never inside a source library.
    const std::string inside = savePlan(ka / "STYLE/plan.aiaplan", text, A, B);
    TEST("save inside a source library refused, nothing written", has(inside, "inside a source library") && !fs::exists(ka / "STYLE/plan.aiaplan"));
    TEST("save into the library root itself refused", has(savePlan(kb / "plan.aiaplan", text, A, B), "inside a source library"));
    TEST("sources untouched by the refused saves", treeState(ka) + treeState(kb) == before);

    // Atomic replace: a second save replaces the first; a failed save keeps the old file.
    plan.pop_back();
    TEST("second save replaces the first", savePlan(file, serializePlan(plan, A, B), A, B).empty() && parsePlan(slurp(file), A, B, got).empty() && got.size() == 2);
    const std::string good = slurp(file);
    fs::path blocker = data / "blocked"; put(blocker, "x");
    TEST("a save that cannot be written reports why", !savePlan(blocker / "plan.aiaplan", text, A, B).empty());
    TEST("a failed save keeps the previous file", slurp(file) == good);
    TEST("missing file reports no saved plan", loadPlanText(data / "none.aiaplan", back) == "no saved plan");

    fs::remove_all(root);
    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
