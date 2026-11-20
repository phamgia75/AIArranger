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
        const auto why = detail::moveAside(parent.v, "out.INCOMPLETE", base / "out.INCOMPLETE", &to, [&] {
            fs::rename(base / "out.INCOMPLETE", base / "out.INCOMPLETE.gone");
            fs::rename(base / "decoy", base / "out.INCOMPLETE");
        });
        TEST("R8 leftover swapped before the rename: refused and renamed back", has(why, "changed between") && fs::is_regular_file(base / "out.INCOMPLETE/precious.txt") &&
             fs::is_regular_file(base / "out.INCOMPLETE.gone/mine.txt"));
    }
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
