// Shared export runner (jobs/export_job.h): path-check-to-open races (HIGH). The destination's parent (or one of its
// ancestors) is swapped for a symlink AFTER the runner's by-path guards and BEFORE it opens the folder; staging, the
// marker, the publish rename and the report must still land only in the authorized destination, or the job must fail
// with nothing published. A legitimate symlinked ancestor (macOS /var -> /private/var) stays allowed.
// Synthetic data only.

#include "jobs/export_job.h"
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

static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
static std::vector<std::string> names(const fs::path& dir) {
    std::vector<std::string> r;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) r.push_back(e.path().filename().string());
    std::sort(r.begin(), r.end());
    return r;
}

// One tiny export: a folder `dest` holding one file. `byFd` picks writeAt (pinned) or the legacy by-path writer.
static ExportSpec spec(const fs::path& dest, bool byFd, std::vector<fs::path> sources = {}) {
    ExportSpec s;
    s.type = "Race test";
    s.source = "synthetic";
    s.sources = std::move(sources);
    s.dest = dest;
    s.preview = [] { return std::vector<OutFile>{{"a.txt", 5, "", true}}; };
    if (byFd)
        s.writeAt = [](int outFd, const fs::path&, const std::function<bool(double)>&) {
            const int fd = ::openat(outFd, "a.txt", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
            const bool ok = fd >= 0 && ::write(fd, "hello", 5) == 5;
            if (fd >= 0) ::close(fd);
            return WriteResult{ok ? "" : "cannot write", ""};
        };
    else
        s.write = [](const fs::path& out, const std::function<bool(double)>&) {
            fs::create_directories(out);
            put(out / "a.txt", "hello");
            return WriteResult{"", ""};
        };
    return s;
}
// Runs `s`; `onEnter(phase)` is called each time the job reports a phase, once per phase (the attacker's moment).
static JobState run(const ExportSpec& s, const std::function<void(Phase)>& onEnter) {
    JobState st;
    st.id = 1;
    Phase last = Phase::Queued;
    runExport(s, st, [] { return false; }, [&](const JobState& now) { if (now.phase != last) { last = now.phase; onEnter(now.phase); } });
    return st;
}
// Replaces folder `victim` with a symlink to `target` (the real folder is renamed to `victim`.moved).
static void swapForLink(const fs::path& victim, const fs::path& target) {
    fs::rename(victim, victim.string() + ".moved");
    fs::create_directory_symlink(target, victim);
}

int main() {
    std::printf("Test: export runner path races\n");
    const fs::path root = fs::weakly_canonical(fs::temp_directory_path()) / ("aia-export-race-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    for (const bool byFd : {true, false}) {
        const std::string how = byFd ? "writeAt" : "write";
        // R1: the destination's parent becomes a symlink to another folder after the guards
        {
            const fs::path base = root / ("r1-" + how);
            fs::create_directories(base / "p");
            fs::create_directories(base / "elsewhere");
            const auto st = run(spec(base / "p/out", byFd), [&](Phase ph) { if (ph == Phase::Preview) swapForLink(base / "p", base / "elsewhere"); });
            TEST(("R1 " + how + ": parent swapped for a symlink: job fails").c_str(), st.phase == Phase::Failed);
            TEST(("R1 " + how + ": nothing is written through the link").c_str(), names(base / "elsewhere").empty());
            TEST(("R1 " + how + ": nothing is published in the real folder").c_str(), !fs::exists(base / "p.moved/out") && !fs::exists(base / "p.moved/out.export.md"));
        }
        // R2: the swap redirects into a folder the job reads (a source)
        {
            const fs::path base = root / ("r2-" + how);
            put(base / "src/keep.txt", "source");
            fs::create_directories(base / "p");
            const auto st = run(spec(base / "p/out", byFd, {base / "src"}), [&](Phase ph) { if (ph == Phase::Preview) swapForLink(base / "p", base / "src"); });
            TEST(("R2 " + how + ": parent swapped to a link INTO a source: job fails").c_str(), st.phase == Phase::Failed);
            TEST(("R2 " + how + ": the source gains no file or folder").c_str(), names(base / "src") == std::vector<std::string>{"keep.txt"});
        }
        // R3: an ANCESTOR (not the parent) is swapped
        {
            const fs::path base = root / ("r3-" + how);
            fs::create_directories(base / "a/b");
            fs::create_directories(base / "elsewhere/b");
            const auto st = run(spec(base / "a/b/out", byFd), [&](Phase ph) { if (ph == Phase::Preview) swapForLink(base / "a", base / "elsewhere"); });
            TEST(("R3 " + how + ": ancestor swapped for a symlink: job fails").c_str(), st.phase == Phase::Failed);
            TEST(("R3 " + how + ": nothing is written through the link").c_str(), names(base / "elsewhere/b").empty());
        }
        // R4: the parent is swapped for ANOTHER REAL folder (identity changes, no link)
        {
            const fs::path base = root / ("r4-" + how);
            fs::create_directories(base / "p");
            fs::create_directories(base / "other");
            const auto st = run(spec(base / "p/out", byFd), [&](Phase ph) { if (ph == Phase::Preview) { fs::rename(base / "p", base / "p.moved"); fs::rename(base / "other", base / "p"); } });
            TEST(("R4 " + how + ": parent replaced by another folder: job fails, nothing written there").c_str(), st.phase == Phase::Failed && names(base / "p").empty());
        }
        // R5: a legitimate symlinked ancestor is resolved once and honoured: output lands in the real folder
        {
            const fs::path base = root / ("r5-" + how);
            fs::create_directories(base / "real");
            fs::create_directory_symlink(base / "real", base / "link");
            const auto st = run(spec(base / "link/out", byFd), [](Phase) {});
            TEST(("R5 " + how + ": symlinked ancestor: published in the real folder").c_str(), st.phase == Phase::Done && fs::is_regular_file(base / "real/out/a.txt") &&
                 fs::is_regular_file(base / "real/out.export.md") && !fs::exists(base / "real/out.INCOMPLETE"));
        }
    }

    // R6: after publish, the parent is swapped before the report is written: the report is not written through the link
    {
        const fs::path base = root / "r6";
        fs::create_directories(base / "p");
        fs::create_directories(base / "elsewhere");
        const auto st = run(spec(base / "p/out", true), [&](Phase ph) { if (ph == Phase::Report) swapForLink(base / "p", base / "elsewhere"); });
        TEST("R6 report after publish: not written through a swapped parent", names(base / "elsewhere").empty() && fs::is_directory(base / "p.moved/out"));
        TEST("R6 report after publish: the report went beside the published output", fs::is_regular_file(base / "p.moved/out.export.md") || has(st.reason, "report") || !st.warnings.empty());
    }
    // R7: swap during a by-path writer (after the pin): the job must not publish; the staged output stays
    {
        const fs::path base = root / "r7";
        fs::create_directories(base / "p");
        fs::create_directories(base / "elsewhere");
        auto s = spec(base / "p/out", false);
        const auto real = s.write;
        s.write = [&, real](const fs::path& out, const std::function<bool(double)>& pr) {
            const auto r = real(out, pr);
            swapForLink(base / "p", base / "elsewhere");
            return r;
        };
        const auto st = run(s, [](Phase) {});
        TEST("R7 swap during the writer: job fails, nothing published", st.phase == Phase::Failed && !fs::exists(base / "p.moved/out") && names(base / "elsewhere").empty());
        TEST("R7 swap during the writer: only NAME.INCOMPLETE remains in the real folder", names(base / "p.moved") == std::vector<std::string>{"out.INCOMPLETE"});
    }
    // R8: leftover move-aside: the leftover is swapped for another folder between the checks and the rename -> renamed back
    {
        const fs::path base = root / "r8";
        put(base / "out.INCOMPLETE" / kJobMarker, "export job 1 Race test -> " + (base / "out").string() + "\n");
        put(base / "out.INCOMPLETE/mine.txt", "mine");
        put(base / "decoy/precious.txt", "precious");
        media::detail::Fd parent(::open(base.c_str(), O_RDONLY | O_DIRECTORY));
        fs::path to;
        const auto why = detail::moveAside(parent.v, "out.INCOMPLETE", base / "out.INCOMPLETE", &to, nullptr, [&] {
            fs::rename(base / "out.INCOMPLETE", base / "out.INCOMPLETE.gone");
            fs::rename(base / "decoy", base / "out.INCOMPLETE");
        });
        TEST("R8 leftover swapped before the rename: refused and renamed back", has(why, "changed between") && fs::is_regular_file(base / "out.INCOMPLETE/precious.txt") &&
             fs::is_regular_file(base / "out.INCOMPLETE.gone/mine.txt"));
    }
    // R10: the parent does not exist yet; an ancestor is swapped for a link into a source before the missing folder is made
    {
        const fs::path base = root / "r10";
        put(base / "src/keep.txt", "source");
        fs::create_directories(base / "a");
        const auto st = run(spec(base / "a/new/out", true, {base / "src"}), [&](Phase ph) { if (ph == Phase::Preview) swapForLink(base / "a", base / "src"); });
        TEST("R10 missing parent below a swapped ancestor: job fails, no folder made in the source", st.phase == Phase::Failed && names(base / "src") == std::vector<std::string>{"keep.txt"});
    }
    // R11: the pinned parent is renamed INTO a source while the job runs (the handle still points at it)
    {
        const fs::path base = root / "r11";
        put(base / "src/keep.txt", "source");
        fs::create_directories(base / "p");
        auto s = spec(base / "p/out", true, {base / "src"});
        const auto real = s.writeAt;
        s.writeAt = [&, real](int fd, const fs::path& out, const std::function<bool(double)>& pr) {
            const auto r = real(fd, out, pr);
            fs::rename(base / "p", base / "src/p");
            return r;
        };
        const auto st = run(s, [](Phase) {});
        TEST("R11 parent moved into a source during the job: fails, nothing published, no report", st.phase == Phase::Failed && !fs::exists(base / "src/p/out") &&
             !fs::exists(base / "src/p/out.export.md") && st.reportPath.empty());
    }

    // R12: the leftover whose marker was checked is swapped for another folder before it is moved: refused, nothing moved
    {
        const fs::path base = root / "r12";
        put(base / "out.INCOMPLETE" / kJobMarker, "export job 1 Race test -> " + (base / "out").string() + "\n");
        put(base / "decoy/precious.txt", "precious");
        media::detail::Fd parent(::open(base.c_str(), O_RDONLY | O_DIRECTORY));
        struct stat id {};
        const auto ours = detail::notOurLeftover(parent.v, "out.INCOMPLETE", base / "out.INCOMPLETE", "Race test", base / "out", &id);
        fs::rename(base / "out.INCOMPLETE", base / "out.INCOMPLETE.gone");
        fs::rename(base / "decoy", base / "out.INCOMPLETE");
        fs::path to;
        const auto why = detail::moveAside(parent.v, "out.INCOMPLETE", base / "out.INCOMPLETE", &to, &id);
        TEST("R12 leftover swapped after its marker was checked: refused, the other folder is not moved", ours.empty() && has(why, "not the folder whose marker") &&
             fs::is_regular_file(base / "out.INCOMPLETE/precious.txt"));
    }

    // ── Final gate: cleanup removes only what the job created; publication is never reported complete when it is not ──
    {   // C1: the marker is swapped for another file right before cleanup: the substitute is never removed
        const fs::path base = root / "c1";
        fs::create_directories(base);
        detail::testHook() = [&](const char* w) {
            if (std::string(w) != "beforeMarkerCleanup") return;
            fs::rename(base / "out.INCOMPLETE" / kJobMarker, base / "marker.orig");
            put(base / "out.INCOMPLETE" / kJobMarker, "substitute");
        };
        const auto st = run(spec(base / "out", true), [](Phase) {});
        detail::testHook() = nullptr;
        TEST("C1 swapped marker: the output is published, the substitute is kept (never unlinked) and reported", st.phase == Phase::Done && fs::is_directory(base / "out") &&
             fs::is_regular_file(base / "out.INCOMPLETE" / kJobMarker) && fs::exists(base / "marker.orig") && !st.warnings.empty() &&
             has(st.warnings.front(), "marker") && has(st.warnings.front(), "not the entry this job created"));
    }
    {   // C2: the job folder is swapped for another (empty) folder before its removal: kept, reported
        const fs::path base = root / "c2";
        fs::create_directories(base);
        detail::testHook() = [&](const char* w) {
            if (std::string(w) != "beforeWorkRemoval") return;
            fs::rename(base / "out.INCOMPLETE", base / "work.orig");
            fs::create_directory(base / "out.INCOMPLETE");
        };
        const auto st = run(spec(base / "out", true), [](Phase) {});
        detail::testHook() = nullptr;
        bool warned = false;
        for (const auto& w : st.warnings) warned |= has(w, "job folder") && has(w, "not removed");
        TEST("C2 swapped job folder: kept and reported, never removed", fs::is_directory(base / "out.INCOMPLETE") && fs::is_directory(base / "work.orig") && warned);
    }
    {   // C3: a side file cannot be moved back: Failed, the side file is named as still published, the evidence is kept
        const fs::path base = root / "c3";
        fs::create_directories(base);
        auto s = spec(base / "out", false);
        const auto real = s.write;
        s.write = [&, real](const fs::path& out, const std::function<bool(double)>& pr) {
            const auto r = real(out, pr);
            put(out.string() + ".side", "side file");
            return r;
        };
        detail::testHook() = [&](const char* w) {
            if (std::string(w) != "afterSideMoves") return;
            put(base / "out.INCOMPLETE/out.side", "taken meanwhile");   // the rollback name is taken
            fs::create_directory(base / "out");                       // the final rename will be refused
        };
        const auto st = run(s, [](Phase) {});
        detail::testHook() = nullptr;
        TEST("C3 partial publication: Failed (never success), the side file is named as still published", st.phase == Phase::Failed && has(st.reason, "could NOT be moved back") &&
             has(st.reason, "out.side") && st.reportPath.empty());
        TEST("C3 partial publication: recovery evidence is preserved (side file, staged output, marker)", fs::is_regular_file(base / "out.side") &&
             fs::is_regular_file(base / "out.INCOMPLETE/out.side") && fs::is_directory(base / "out.INCOMPLETE/out") && fs::is_regular_file(base / "out.INCOMPLETE" / kJobMarker));
    }
    {   // C4: removeOwned itself
        const fs::path base = root / "c4";
        put(base / "f.txt", "mine");
        media::detail::Fd d(::open(base.c_str(), O_RDONLY | O_DIRECTORY));
        struct stat mine {};
        ::stat((base / "f.txt").c_str(), &mine);
        fs::rename(base / "f.txt", base / "f.orig");
        put(base / "f.txt", "someone else's");
        const auto kept = media::removeOwned(d.v, "f.txt", mine, false);
        TEST("C4 removeOwned: a substituted file is kept, reported", has(kept, "not the entry this job created") && fs::is_regular_file(base / "f.txt"));
        fs::remove(base / "f.txt");
        ::stat((base / "f.orig").c_str(), &mine);
        TEST("C4 removeOwned: the proven file is removed", media::removeOwned(d.v, "f.orig", mine, false).empty() && !fs::exists(base / "f.orig"));
        fs::create_directories(base / "dir/inner");
        struct stat dm {};
        ::stat((base / "dir").c_str(), &dm);
        const auto full = media::removeOwned(d.v, "dir", dm, true);
        TEST("C4 removeOwned: a non-empty folder is put back, not removed", has(full, "not empty") && fs::is_directory(base / "dir/inner"));
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
