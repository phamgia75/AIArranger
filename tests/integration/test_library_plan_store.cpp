// Row 56: the staged Library Manager plan persists safely. Synthetic libraries only (placeholder bytes).
#include "library/library_history.h"
#include "library/library_plan_store.h"
#include <cstdio>
#include <csignal>
#include <sys/wait.h>
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
    const std::string kA = libraryKey(A), kB = libraryKey(B);
    TEST("synthetic libraries open", A.ok && B.ok && A.items.size() == 2 && B.items.size() == 1);

    std::vector<Step> plan;
    { Step s; s.op = Op::Copy; s.side = 'A'; s.item = 1; s.bank = "STYLE/USER02"; plan.push_back(s); }
    { Step s; s.op = Op::Rename; s.side = 'B'; s.item = 0; s.text = "Wal\ttz %41\nnew"; s.wholeFile = true; plan.push_back(s); }
    { Step s; s.op = Op::BankSwap; s.side = 'A'; s.bank = "STYLE/USER01"; s.text = "STYLE/USER02"; plan.push_back(s); }

    const std::string before = treeState(ka) + treeState(kb);
    const std::string text = serializePlan(plan, kA, kB);
    const fs::path file = planFilePath(data, kA, kB);
    TEST("save writes the plan file", savePlan(file, text, A, B).empty() && fs::exists(file));
    auto leftovers = [&] { size_t n = 0; for (const auto& e : fs::directory_iterator(data)) if (e.path().extension() == ".INCOMPLETE") ++n; return n; };
    TEST("no .INCOMPLETE left behind", leftovers() == 0);
    TEST("sources untouched by save", treeState(ka) + treeState(kb) == before);

    std::string back;
    std::vector<Step> got;
    TEST("load reads it", loadPlanText(file, back).empty() && back == text);
    TEST("parse accepts it", parsePlan(back, kA, kB, A, B, got).empty() && got.size() == 3);
    bool same = got.size() == 3;
    for (size_t i = 0; same && i < 3; ++i)
        same = got[i].op == plan[i].op && got[i].side == plan[i].side && got[i].item == plan[i].item && got[i].bank == plan[i].bank &&
               got[i].slot == plan[i].slot && got[i].text == plan[i].text && got[i].wholeFile == plan[i].wholeFile;
    TEST("round trip is exact (tab, percent, newline in a name)", same);
    TEST("the restored plan evaluates like the original", evaluate(A, B, got).steps.size() == evaluate(A, B, plan).steps.size());

    // Rejections: each returns a reason and leaves no steps.
    auto refused = [&](const std::string& t, const char* why) { std::vector<Step> o{Step{}}; const std::string r = parsePlan(t, kA, kB, A, B, o); return !r.empty() && o.empty() && has(r, why); };
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

    // A hand-made file with a valid checksum still cannot get a hostile step past the Execute-time evaluation.
    { Step h; h.op = Op::BankSwap; h.side = 'A'; h.bank = "../escape"; h.text = "STYLE/USER01";
      std::vector<Step> hs{Step{}};
      TEST("a signed bank swap with a hostile bank is refused by the parser", !parsePlan(serializePlan({h}, kA, kB), kA, kB, A, B, hs).empty() && hs.empty());
      const auto ev = evaluate(A, B, {h});   // defence in depth: Execute evaluates again whatever the file said
      TEST("...and evaluate() blocks it too, so Execute would refuse", ev.blocked() && ev.steps[0].conflicts.size() == 1); }
    { Step h; h.op = Op::Rename; h.side = 'A'; h.item = 0; h.text = "bad/name";
      std::vector<Step> hs; parsePlan(serializePlan({h}, kA, kB), kA, kB, A, B, hs);
      TEST("a restored rename with a path separator is blocked by evaluate()", evaluate(A, B, hs).blocked()); }

    // Steps the UI cannot make are refused even with a valid checksum (addStep() never sets a slot; Rename has a name and no bank...).
    auto signedStep = [&](const std::string& line) { return resign(std::string(kPlanFileMagic) + "\nA " + kA + "\nB " + kB + "\n" + line + "\n"); };
    TEST("a step with a target slot is refused", refused(signedStep("step\t0\tA\t1\tSTYLE/USER02\t3\t\t0"), "no plan step can have"));
    TEST("a number with trailing junk is refused", refused(signedStep("step\t0\tA\t1x\tSTYLE/USER02\t-1\t\t0"), "malformed number"));
    TEST("a rename with a bank is refused", refused(signedStep("step\t2\tA\t1\tSTYLE/USER02\t-1\tNew\t0"), "no plan step can have"));
    TEST("a rename with no name is refused", refused(signedStep("step\t2\tA\t1\t\t-1\t\t0"), "no plan step can have"));
    TEST("a copy with text is refused", refused(signedStep("step\t0\tA\t1\t\t-1\tx\t0"), "no plan step can have"));
    TEST("a whole-file bank swap is refused", refused(signedStep("step\t3\tA\t1\tSTYLE/USER01\t-1\tSTYLE/USER02\t1"), "no plan step can have"));
    TEST("a bank swap with an item outside the library is refused", refused(signedStep("step\t3\tA\t9\tSTYLE/USER01\t-1\tSTYLE/USER02\t0"), "item index"));
    { std::vector<Step> ok; TEST("the steps addStep() makes are accepted", parsePlan(signedStep("step\t0\tA\t1\tSTYLE/USER02\t-1\t\t0"), kA, kB, A, B, ok).empty() && ok.size() == 1); }

    // A planted temp file (symlink to a source file, or to the previous plan) is never followed.
    { const fs::path victim = ka / "STYLE/USER01/Ballad.STG", plan2 = data / "plan2.aiaplan";
      const std::string vb = slurp(victim);
      savePlan(plan2, text, A, B);
      std::error_code lec;
      fs::create_symlink(victim, plan2.string() + ".INCOMPLETE", lec);
      TEST("save with a symlinked temp name succeeds", savePlan(plan2, serializePlan({}, kA, kB), A, B).empty());
      TEST("the symlink target (a source file) was not written", slurp(victim) == vb);
      fs::remove(plan2.string() + ".INCOMPLETE", lec);
      fs::create_symlink(plan2, plan2.string() + ".INCOMPLETE", lec);
      const std::string before2 = slurp(plan2);
      savePlan(plan2, text, A, B);
      TEST("a temp symlink to the previous plan did not corrupt it", parsePlan(slurp(plan2), kA, kB, A, B, got).empty() && got.size() == 3 && before2 != slurp(plan2));
      fs::create_symlink(plan2, data / "link.aiaplan", lec);
      std::string lt; TEST("a symlinked plan file is not read", loadPlanText(data / "link.aiaplan", lt) == "no saved plan");
      put(data / "huge.aiaplan", std::string((1u << 22) + 1, 'x'));
      TEST("an oversized plan file is refused before it is read", loadPlanText(data / "huge.aiaplan", lt) == "plan file too large" && lt.empty()); }

    // Libraries changed since the save.
    put(kb / "STYLE/USER02/Extra.STG", "AIARRANGER-SYNTHETIC extra");
    const Library B2 = load(kb);
    std::vector<Step> o;
    const std::string r = parsePlan(text, kA, libraryKey(B2), A, B2, o);
    TEST("plan refused when a library changed", has(r, "different libraries") && o.empty());
    TEST("a changed pair gets its own plan file name", planFilePath(data, kA, libraryKey(B2)) != file);
    fs::remove(kb / "STYLE/USER02/Extra.STG");
    TEST("moving a library keeps the plan valid", [&] {
        fs::copy(ka, root / "A-moved.KST", fs::copy_options::recursive);
        const Library Am = load(root / "A-moved.KST");
        std::vector<Step> x;
        return parsePlan(text, libraryKey(Am), kB, Am, B, x).empty() && x.size() == 3;
    }());

    // Never inside a source library.
    const std::string inside = savePlan(ka / "STYLE/plan.aiaplan", text, A, B);
    TEST("save inside a source library refused, nothing written", has(inside, "inside a source library") && !fs::exists(ka / "STYLE/plan.aiaplan"));
    TEST("save into the library root itself refused", has(savePlan(kb / "plan.aiaplan", text, A, B), "inside a source library"));
    TEST("sources untouched by the refused saves", treeState(ka) + treeState(kb) == before);

    // Atomic replace: a second save replaces the first; a failed save keeps the old file.
    plan.pop_back();
    TEST("second save replaces the first", savePlan(file, serializePlan(plan, kA, kB), A, B).empty() && parsePlan(slurp(file), kA, kB, A, B, got).empty() && got.size() == 2);
    const std::string good = slurp(file);
    fs::path blocker = data / "blocked"; put(blocker, "x");
    TEST("a save that cannot be written reports why", !savePlan(blocker / "plan.aiaplan", text, A, B).empty());
    TEST("a failed save keeps the previous file", slurp(file) == good);
    TEST("missing file reports no saved plan", loadPlanText(data / "none.aiaplan", back) == "no saved plan");

    // Interrupted writes: a stale .INCOMPLETE from a crash is ignored and replaced; a child killed (SIGKILL) at random moments
    // mid-save always leaves the previous or the new COMPLETE plan under the real name, never a partial one.
    put(file.string() + ".INCOMPLETE", "AIARRANGER-LIBRARY-PLAN 1\nA garbage");
    TEST("a stale .INCOMPLETE does not disturb loading", loadPlanText(file, back).empty() && back == good);
    TEST("the next save publishes its own file, not the stale one", savePlan(file, text, A, B).empty() && slurp(file) == text);
    std::vector<Step> big;
    for (int i = 0; i < 2000; ++i) { Step st; st.op = Op::Rename; st.side = 'A'; st.item = size_t(i % 2); st.text = "Name " + std::to_string(i); big.push_back(st); }
    const std::string bigText = serializePlan(big, kA, kB);
    int torn = 0, kills = 0;
    for (int round = 0; round < 60; ++round) {
        const pid_t pid = ::fork();
        if (pid == 0) { for (;;) { savePlan(file, round % 2 ? bigText : text, A, B); savePlan(file, round % 2 ? text : bigText, A, B); } }
        ::usleep(useconds_t(200 + (round * 937) % 4000));
        ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ++kills;
        std::vector<Step> chk;
        const std::string now = slurp(file);
        if (!parsePlan(now, kA, kB, A, B, chk).empty() || (now != text && now != bigText)) ++torn;
    }
    TEST("60 kills mid-save: the plan file was always one complete valid version", kills == 60 && torn == 0);
    // Two instances saving the same pair at once, both killed at random moments: still always one complete version.
    int torn2 = 0;
    for (int round = 0; round < 40; ++round) {
        pid_t kids[2];
        for (int k = 0; k < 2; ++k) { kids[k] = ::fork(); if (kids[k] == 0) { for (;;) savePlan(file, k ? bigText : text, A, B); } }
        ::usleep(useconds_t(300 + (round * 811) % 4000));
        for (pid_t k : kids) { ::kill(k, SIGKILL); ::waitpid(k, nullptr, 0); }
        std::vector<Step> chk; const std::string now = slurp(file);
        if (!parsePlan(now, kA, kB, A, B, chk).empty() || (now != text && now != bigText)) ++torn2;
    }
    TEST("two concurrent writers killed at random: never a partial plan", torn2 == 0);
    for (const auto& e : fs::directory_iterator(data)) if (e.path().extension() == ".INCOMPLETE") fs::remove(e.path());   // crash leftovers

    // Size limits are enforced at save, so a saved plan is always restorable; the old plan is kept.
    const std::string before3 = slurp(file);
    TEST("a plan too large to restore is refused at save, the old file kept", has(savePlan(file, std::string(kMaxPlanBytes + 1, 'x'), A, B), "too large") && slurp(file) == before3);
    { std::vector<Step> many(kMaxPlanSteps + 1, big[0]);
      std::vector<Step> o2; TEST("more steps than the cap are refused on load", has(parsePlan(serializePlan(many, kA, kB), kA, kB, A, B, o2), "too many")); }
    TEST("the capped plan size fits the load limit", serializePlan(std::vector<Step>(kMaxPlanSteps, big[0]), kA, kB).size() < kMaxPlanBytes);

    // Removing a cleared plan never deletes inside a source library.
    put(ka / "STYLE/plan-victim.aiaplan", "AIARRANGER-SYNTHETIC not a plan");
    TEST("removePlan refuses a path inside a source library", has(removePlan(ka / "STYLE/plan-victim.aiaplan", A, B), "inside a source library") && fs::exists(ka / "STYLE/plan-victim.aiaplan"));
    fs::remove(ka / "STYLE/plan-victim.aiaplan");
    TEST("removePlan removes a plan outside, and a missing one is fine", removePlan(file, A, B).empty() && !fs::exists(file) && removePlan(file, A, B).empty());

    // Banks must be what the UI would have taken from the selection.
    { std::vector<Step> ok2;
      TEST("a copy to a bank the other library has is accepted", parsePlan(signedStep("step\t0\tA\t1\tSTYLE/USER02\t-1\t\t0"), kA, kB, A, B, ok2).empty());
      TEST("a copy to an arbitrary bank is refused", refused(signedStep("step\t0\tA\t1\tSOUND/MADEUP\t-1\t\t0"), "target bank"));
      TEST("a bank swap that does not start at the selected item's bank is refused", refused(signedStep("step\t3\tA\t1\tSTYLE/USER02\t-1\tSTYLE/USER01\t0"), "selected item's bank"));
      TEST("a bank swap from the selected item's bank is accepted", parsePlan(signedStep("step\t3\tA\t1\tSTYLE/USER01\t-1\tSTYLE/USER02\t0"), kA, kB, A, B, ok2).empty()); }

    // Restoring N steps is one history entry, not N snapshots.
    { PlanHistory h; h.adopt(std::vector<Step>(kMaxPlanSteps, big[0]));
      TEST("adopt restores all steps as one undoable entry", h.steps().size() == kMaxPlanSteps && h.undo() && h.steps().empty() && !h.canUndo()); }

    fs::remove_all(root);
    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
