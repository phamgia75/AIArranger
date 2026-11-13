// #55 acceptance: runtime proof that the staged plan history (library_history.h) never touches the sources, that
// Execute Plan runs exactly the plan shown after every undo / redo / new step, and that a failed, cancelled or
// interrupted run leaves the sources, the plan and its history intact, with only NAME.INCOMPLETE left (E6) and a
// re-run published under a new name. Synthetic KST libraries in a temp folder only; no owner data.

#include "jobs/job_queue.h"
#include "jobs/export_specs.h"
#include "library/library_history.h"
#include "../support/panative_fixture.h"
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;
using library::Op;
using library::Step;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } } while (0)

static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
static std::string blob(const std::string& tag) { return "synthetic test bytes, not a Korg file: " + tag + "\n"; }
// Every entry below `root` (and root itself): kind, a file's SHA-256, size, mtime (ns), mode.
static std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> m;
    auto add = [&](const fs::path& p) {
        struct stat s{};
        ::lstat(p.c_str(), &s);
        bool cancelled = false;
        std::string err;
        const std::string what = S_ISREG(s.st_mode) ? "file " + media::detail::sha256File(p, [](size_t) { return true; }, &cancelled, &err) + err
                                 : S_ISDIR(s.st_mode) ? "dir" : "other";
        m[p == root ? "." : fs::relative(p, root).generic_string()] = what + " size " + std::to_string(s.st_size) + " mtime " +
            std::to_string(s.st_mtimespec.tv_sec) + "." + std::to_string(s.st_mtimespec.tv_nsec) + " mode " + std::to_string(s.st_mode);
    };
    add(root);
    for (const auto& e : fs::recursive_directory_iterator(root)) add(e.path());
    return m;
}
static std::vector<std::string> named(const fs::path& dir, const std::string& stem) {
    std::vector<std::string> r;
    for (const auto& e : fs::directory_iterator(dir)) if (e.path().filename().string().rfind(stem, 0) == 0) r.push_back(e.path().filename().string());
    std::sort(r.begin(), r.end());
    return r;
}
using Lib = std::shared_ptr<const library::Library>;
static Lib open(const fs::path& p) { return std::make_shared<const library::Library>(library::load(p)); }
static size_t item(const Lib& L, const std::string& name) {
    for (size_t i = 0; i < L->items.size(); ++i) if (library::trimmed(L->items[i].name) == name) return i;
    return size_t(-1);
}
static bool sameSteps(const std::vector<Step>& a, const std::vector<Step>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].op != b[i].op || a[i].side != b[i].side || a[i].item != b[i].item || a[i].bank != b[i].bank || a[i].slot != b[i].slot ||
            a[i].text != b[i].text || a[i].wholeFile != b[i].wholeFile) return false;
    return true;
}
// "N. <summary>" of every step, as the Plan view shows it (planReport) ...
static std::vector<std::string> shownSteps(const std::string& report) {
    std::vector<std::string> r;
    std::istringstream in(report);
    for (std::string l; std::getline(in, l);)
        if (!l.empty() && std::isdigit((unsigned char)l[0]) && has(l, ". ")) r.push_back(l);
    return r;
}
// ... equal to the executed library's report, step for step (applyPlan: "N. <summary>: <what was done>")
static bool executedAsShown(const std::string& writerText, const std::vector<std::string>& shown) {
    const auto done = shownSteps(writerText);
    if (done.size() != shown.size()) return false;
    for (size_t i = 0; i < shown.size(); ++i) if (done[i].rfind(shown[i] + ": ", 0) != 0) return false;
    return true;
}

int main() {
    std::printf("Test: #55 staged plan history and Execute Plan at runtime (synthetic KST)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-libhist-exec-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path ka = root / "A.KST", kb = root / "B.KST", out = root / "out";
    put(ka / "STYLE/USER01/Ballad.STG", blob("ballad"));
    put(ka / "PAD/USER01/Hit.PDC", blob("hit"));
    put(kb / "STYLE/USER01/Waltz.STG", blob("waltz"));
    put(kb / "STYLE/USER03/Tango.STG", blob("tango"));
    fs::create_directories(out);
    const auto A = open(ka), B = open(kb);
    TEST("synthetic KST libraries open", A->ok && B->ok && item(A, "Ballad") != size_t(-1) && item(B, "Waltz") != size_t(-1));
    const auto snapA = snapshot(ka), snapB = snapshot(kb);
    const auto untouched = [&] { return snapshot(ka) == snapA && snapshot(kb) == snapB; };

    // every step targets B, so each prefix of the plan is executable on its own
    const Step s1{Op::Copy, 'A', item(A, "Ballad"), "STYLE/USER03", -1, ""}, s2{Op::Rename, 'B', item(B, "Waltz"), "", -1, "Old Waltz"},
               s3{Op::Copy, 'A', item(A, "Hit"), "", -1, ""}, s4{Op::BankSwap, 'B', 0, "STYLE/USER01", -1, "STYLE/USER05"};
    library::PlanHistory h;
    const auto add = [&](const Step& s) { h.add(s, library::stepLabel(s.side == 'A' ? *A : *B, s)); };

    // ── 1. staged edits never touch the sources (bytes, listing, mtimes, modes) ──
    {
        add(s1); add(s2); add(s3); h.undo(); h.undo(); h.redo(); add(s4); h.removeLast(); h.undo(); h.clear(); h.undo(); h.redo(); h.undo();
        for (int i = 0; i < 3; ++i) (void)library::planReport(*A, *B, h.steps(), library::evaluate(*A, *B, h.steps()));   // what the Plan view computes
        TEST("1: after add/remove/clear/undo/redo/branch, every source file, folder, mtime and mode is unchanged", untouched());
        TEST("1: and nothing was written beside them", fs::is_empty(out));
        h.reset();
    }

    // ── 2. Execute Plan runs exactly the plan shown, after every undo / redo / new step ──
    int run = 0;
    const auto execute = [&](const std::vector<Step>& plan, const std::string& what) {
        const fs::path dest = out / ("state " + std::to_string(++run) + ".KST");
        JobState st;
        runExport(libraryApply(A, B, plan, dest), st, [] { return false; }, [](const JobState&) {});
        const auto shown = shownSteps(library::planReport(*A, *B, h.steps(), library::evaluate(*A, *B, h.steps())));
        if (h.empty()) {
            TEST("2: " + what + ": an empty plan is refused, nothing published", st.phase == Phase::Failed && has(st.reason, "no steps") && !fs::exists(dest));
            return;
        }
        TEST("2: " + what + ": the executed plan is the displayed plan (" + std::to_string(shown.size()) + " steps)",
             st.phase == Phase::Done && sameSteps(plan, h.steps()) && executedAsShown(st.writerText, shown) && fs::is_directory(dest));
    };
    {
        struct Case { std::function<void()> act; std::vector<Step> expect; std::string what; };
        const std::vector<Case> cases = {
            {[&] { add(s1); add(s2); add(s3); }, {s1, s2, s3}, "three steps added"},
            {[&] { h.undo(); }, {s1, s2}, "undo"},
            {[&] { h.undo(); }, {s1}, "second undo"},
            {[&] { h.redo(); }, {s1, s2}, "redo"},
            {[&] { add(s4); }, {s1, s2, s4}, "new step after undo (branch)"},
            {[&] { TEST("2: the branch dropped the redo of step 3", !h.redo()); }, {s1, s2, s4}, "redo refused after the branch"},
            {[&] { h.removeLast(); }, {s1, s2}, "remove last"},
            {[&] { h.undo(); }, {s1, s2, s4}, "undo of remove"},
            {[&] { h.clear(); }, {}, "clear"},
            {[&] { h.undo(); }, {s1, s2, s4}, "undo of clear"},
        };
        for (const auto& c : cases) {
            c.act();
            TEST("2: " + c.what + ": the history holds the expected steps", sameSteps(h.steps(), c.expect));
            execute(h.steps(), c.what);
        }
        const fs::path last = out / ("state " + std::to_string(run) + ".KST");
        TEST("2: the branched plan's effects, and not the undone step's, are in the output",
             fs::is_regular_file(last / "STYLE/USER03/Ballad.STG") && fs::is_regular_file(last / "STYLE/USER05/Old Waltz.STG") &&
             !fs::exists(last / "PAD/USER01/Hit.PDC"));
        TEST("2: sources unchanged after every execution", untouched());
    }
    const auto planBefore = h.steps();
    const auto labelsBefore = std::make_pair(h.undoLabel(), h.redoLabel());
    const auto historyIntact = [&] { return sameSteps(h.steps(), planBefore) && std::make_pair(h.undoLabel(), h.redoLabel()) == labelsBefore && h.canUndo(); };

    // ── 3. a failed Execute Plan: nothing published, sources identical, plan and history intact ──
    {
        const fs::path dest = out / "fail verify.KST";
        auto spec = libraryApply(A, B, h.steps(), dest);
        spec.beforePublish = [](const fs::path&) { return std::string("injected failure right before publishing"); };
        JobState st;
        runExport(spec, st, [] { return false; }, [](const JobState&) {});
        TEST("3: failure before publish: Failed, nothing published", st.phase == Phase::Failed && has(st.reason, "injected failure") &&
             has(st.reason, "nothing was published") && !fs::exists(dest));
        TEST("3: only NAME.INCOMPLETE is left", named(out, "fail verify") == std::vector<std::string>{"fail verify.KST.INCOMPLETE"});
        TEST("3: sources byte-identical (and mtimes)", untouched());
        TEST("3: staged plan and history intact", historyIntact());

        const fs::path ro = out / "readonly";
        fs::create_directories(ro);
        ::chmod(ro.c_str(), 0555);
        JobState st2;
        runExport(libraryApply(A, B, h.steps(), ro / "B edited.KST"), st2, [] { return false; }, [](const JobState&) {});
        ::chmod(ro.c_str(), 0755);
        TEST("3: unwritable output folder: Failed, nothing created there", st2.phase == Phase::Failed && fs::is_empty(ro));
        TEST("3: sources and history intact after the unwritable-folder failure", untouched() && historyIntact());
    }

    // ── 4. cancelled mid-run through the job runner: only NAME.INCOMPLETE, nothing published ──
    const fs::path record = root / "record/outputs.txt";
    setOutputRecord(record);   // the leftover detection reads it in step 5
    const fs::path cancelled = out / "B edited T1.KST";
    {
        JobQueue q;
        std::atomic<bool> release{false};
        auto spec = libraryApply(A, B, h.steps(), cancelled);
        spec.verify = [&release, verify = spec.verify](const fs::path& o) {   // hold the job at verify: the staged copy exists
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return verify(o);
        };
        const auto id = q.enqueue(spec);
        const auto phase = [&] { for (const auto& j : q.snapshot()) if (j.id == id) return j.phase; return Phase::Queued; };
        for (int i = 0; i < 5000 && phase() != Phase::Verify; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        TEST("4: the job is mid-run (verifying its staged copy)", phase() == Phase::Verify && fs::is_directory(media::incompletePath(cancelled)));
        q.cancel(id);
        release = true;
        for (int i = 0; i < 5000 && !q.idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        JobState st;
        for (const auto& j : q.snapshot()) if (j.id == id) st = j;
        TEST("4: cancelled by the runner, nothing published", st.phase == Phase::Cancelled && has(st.reason, "nothing was published") && !fs::exists(cancelled));
        TEST("4: only NAME.INCOMPLETE is left", named(out, "B edited T1") == std::vector<std::string>{"B edited T1.KST.INCOMPLETE"});
        TEST("4: sources byte-identical, plan and history intact", untouched() && historyIntact());
    }   // the queue (the app session's runner) ends here

    // ── 5. restart: a fresh session starts empty; the leftover is reported, never published; a re-run gets a new name ──
    {
        library::PlanHistory fresh;   // history is session-only (documented limit): nothing is restored
        TEST("5: a fresh session starts with an empty plan and no history", fresh.empty() && !fresh.canUndo() && !fresh.canRedo());
        const auto report = leftoverReport();
        bool listed = false;
        for (const auto& [dir, names] : report)
            if (fs::path(dir) == out) listed = std::find(names.begin(), names.end(), "B edited T1.KST.INCOMPLETE") != names.end();
        TEST("5: the leftover detection reports the cancelled run's NAME.INCOMPLETE", listed);
        TEST("5: the leftover is not a published library", !fs::exists(cancelled) && !fs::exists(media::backupManifestPath(cancelled)));
        JobState same;
        runExport(libraryApply(A, B, planBefore, cancelled), same, [] { return false; }, [](const JobState&) {});
        TEST("5: a re-run under the same name is refused while the leftover is there", same.phase == Phase::Failed && !fs::exists(cancelled) &&
             fs::is_directory(media::incompletePath(cancelled)));
        const fs::path again = out / library::outputName(*B, "T2");
        JobState st;
        runExport(libraryApply(A, B, planBefore, again), st, [] { return false; }, [](const JobState&) {});
        TEST("5: the re-run publishes under a new name", st.phase == Phase::Done && again != cancelled && fs::is_directory(again) &&
             fs::is_regular_file(again / "STYLE/USER05/Old Waltz.STG"));
        TEST("5: the leftover is left as it was (never deleted, never published)", fs::is_directory(media::incompletePath(cancelled)) && !fs::exists(cancelled));
        TEST("5: sources byte-identical after everything", untouched());
    }

    // ── 1 (owner-gated): the same staged sequence with a local COPY of an owner SET as source A; prints counts only ──
    if (const char* own = std::getenv("AIARR_LIBRARY_OWNER_SET")) {
        const fs::path sb = root / "SetB";
        put(sb / "GLOBAL/LIMITER.AUD", "x");   // Pa700 marker
        put(sb / "STYLE/USER01.STY", build({{0x06000030, 2, 1, 0, "B Waltz", "w"}}, 1));
        const auto O = open(own), SB = open(sb);
        const auto snapO = snapshot(own);
        std::vector<size_t> styles;
        for (size_t i = 0; i < O->items.size(); ++i) if (O->items[i].type == "Style" && O->items[i].slot >= 0) styles.push_back(i);
        TEST("1 owner: the owner SET copy and a synthetic SET open", O->ok && O->format == library::Format::PaSet && SB->ok && styles.size() >= 2);
        if (styles.size() >= 2) {
            const auto& x = O->items[styles.front()];
            std::string other;
            for (size_t i : styles) if (O->items[i].bank != x.bank) { other = O->items[i].bank; break; }
            const Step o1{Op::Copy, 'A', styles.front(), "STYLE/USER07", -1, "", true}, o2{Op::Rename, 'B', 0, "", -1, "B Waltz 2"},
                       o3{Op::BankSwap, 'A', 0, x.bank, -1, other.empty() ? "STYLE/USER99" : other}, o4{Op::Copy, 'A', styles.back(), "STYLE/USER01", -1, ""};
            library::PlanHistory oh;
            const auto addO = [&](const Step& s) { oh.add(s, library::stepLabel(s.side == 'A' ? *O : *SB, s)); };
            const auto view = [&] { (void)library::planReport(*O, *SB, oh.steps(), library::evaluate(*O, *SB, oh.steps())); };
            addO(o1); view(); addO(o2); view(); addO(o3); view(); oh.undo(); view(); oh.undo(); view(); oh.redo(); view(); addO(o4); view();
            const bool branched = !oh.redo() && oh.steps().size() == 3;
            oh.removeLast(); view(); oh.clear(); view(); oh.undo(); view(); oh.redo(); view(); oh.undo(); view();
            TEST("1 owner: the staged sequence ran (branch dropped redo, last undo restored the 2 cleared steps)", branched && oh.steps().size() == 2);
            const auto after = snapshot(own);
            std::printf("  info: owner SET: %zu entries checked (SHA-256, listing, mtime, mode)\n", after.size());
            TEST("1 owner: every owner file's SHA-256, the full listing and every mtime are unchanged", after == snapO);
        }
    } else {
        std::printf("  info: AIARR_LIBRARY_OWNER_SET not set: owner check skipped\n");
    }

    setOutputRecord({});
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
