// Backup KST at scale, on a synthetic KST-like folder (original placeholder bytes, no Korg data):
// many small files, a few large ones, empty folders, a read-only file, spaces and non-ASCII names.
// Checks: SHA-256 integrity against an independent tool (shasum / sha256sum), byte equality, kept
// modes and times, source immutability, progress accuracy, cancel + recovery, a killed process
// (interrupted backup) + recovery, and throughput with many files.
// AIARR_BACKUP_SCALE=big: 200 000 files + 4 x 1 GB (local runs); default: 4 000 files + 3 x 8 MB (CI, incl. sanitizers).

#include "media/kst_backup.h"
#include "media/kst_diff.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger::media;
using Clock = std::chrono::steady_clock;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static double secs(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// Deterministic placeholder bytes.
static void fill(const fs::path& p, uint64_t size, uint64_t seed) {
    std::FILE* f = std::fopen(p.c_str(), "wb");
    static char buf[1 << 20];
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    for (uint64_t left = size; left > 0;) {
        const size_t n = size_t(std::min<uint64_t>(left, sizeof buf));
        for (size_t i = 0; i < n; i += 8) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; std::memcpy(buf + i, &x, std::min<size_t>(8, n - i)); }
        std::fwrite(buf, 1, n, f);
        left -= n;
    }
    std::fclose(f);
}

struct Meta { uintmax_t size; long long mtimeNs; unsigned mode; std::string sha; };
static long long mtimeNs(const struct stat& st) {
#ifdef __APPLE__
    return (long long)st.st_mtimespec.tv_sec * 1000000000ll + st.st_mtimespec.tv_nsec;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
}
// Everything except access times: path, size, mtime, permission bits, SHA-256; plus every folder.
static std::map<std::string, Meta> snapshot(const fs::path& root, std::vector<std::string>* dirs) {
    std::map<std::string, Meta> m;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        const std::string rel = e.path().lexically_relative(root).generic_string();
        if (e.is_directory()) { if (dirs) dirs->push_back(rel); continue; }
        struct stat st {};
        ::stat(e.path().c_str(), &st);
        bool c = false;
        std::string err;
        m[rel] = {uintmax_t(st.st_size), mtimeNs(st), unsigned(st.st_mode & 0777), detail::sha256File(e.path(), {}, &c, &err)};
    }
    if (dirs) std::sort(dirs->begin(), dirs->end());
    return m;
}
static bool sameFiles(const std::map<std::string, Meta>& a, const std::map<std::string, Meta>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [k, v] : a) {
        const auto it = b.find(k);
        if (it == b.end() || it->second.size != v.size || it->second.mtimeNs != v.mtimeNs || it->second.mode != v.mode || it->second.sha != v.sha) return false;
    }
    return true;
}
static std::string q(const fs::path& p) {   // single-quoted for sh
    std::string s = "'";
    for (char c : p.string()) s += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return s + "'";
}
static bool notPublished(const fs::path& d) { return !fs::exists(d) && !fs::exists(backupManifestPath(d)); }

int main() {
    const char* scale = std::getenv("AIARR_BACKUP_SCALE");
    const bool big = scale && std::string(scale) == "big";
    // CI size stays small enough for the sanitizer jobs; the big run is for local measurement.
    const size_t nFiles = big ? 200000 : 4000, nDirs = big ? 2000 : 40;
    const uint64_t bigSize = big ? (1ull << 30) : (8ull << 20);
    const int nBig = big ? 4 : 3;
    std::printf("Test: Backup KST at scale (%s: %zu small files in %zu folders + %d x %llu MB)\n", big ? "big" : "ci", nFiles, nDirs, nBig,
                (unsigned long long)(bigSize >> 20));

    const fs::path root = fs::temp_directory_path() / ("aia-kstscale-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path src = root / "SYNTH.SET", out = root / "out";
    fs::create_directories(out);
    auto t0 = Clock::now();
    uint64_t x = 42;
    for (size_t d = 0; d < nDirs; ++d) fs::create_directories(src / ("STYLE" + std::to_string(d / 50)) / ("BANK" + std::to_string(d)));
    for (size_t i = 0; i < nFiles; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        const size_t d = i % nDirs;
        fill(src / ("STYLE" + std::to_string(d / 50)) / ("BANK" + std::to_string(d)) / ("S" + std::to_string(i) + ".STY"), x % 8192, i);
    }
    fs::create_directories(src / "PCM");
    for (int i = 0; i < nBig; ++i) fill(src / "PCM" / ("RAM0" + std::to_string(i) + ".PCM"), bigSize, 1000 + i);
    fs::create_directories(src / "PAD/empty one");
    fs::create_directories(src / "SONGBOOK/empty two");
    fill(src / "SONGBOOK/Ballade \xE2\x80\x93 \xC3\xA9t\xC3\xA9.SBL", 777, 7);   // "Ballade – été.SBL"
    fill(src / "GLOBAL.RO", 300, 8);
    fs::permissions(src / "GLOBAL.RO", fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    std::vector<std::string> srcDirs;
    const auto before = snapshot(src, &srcDirs);
    uintmax_t total = 0;
    for (const auto& [k, v] : before) total += v.size;
    std::printf("  info: synthetic source: %zu files, %zu folders, %.1f MB, made in %.1f s\n", before.size(), srcDirs.size(), total / 1e6, secs(t0));

    // 1. Full backup, with progress recorded.
    struct Tick { double t; BackupProgress p; };
    std::vector<Tick> ticks;
    t0 = Clock::now();
    const fs::path dst = out / "SYNTH.SET backup";
    const auto r = backupKst(src, dst, [&](const BackupProgress& p) {
        if (ticks.empty() || p.workDone != ticks.back().p.workDone || p.phase != ticks.back().p.phase) ticks.push_back({secs(t0), p});
        return true;
    });
    const double took = secs(t0);
    std::printf("  info: backup took %.1f s: %.0f files/s, %.0f MB/s of source (each byte is read 4x and written once)\n", took,
                before.size() / took, total / 1e6 / took);
    TEST("backup of the synthetic KST succeeds", r.ok && r.files == before.size() && r.bytes == total);

    // 2. Integrity, checked by an independent SHA-256 tool, and byte equality.
    const char* tool = std::system("command -v shasum >/dev/null 2>&1") == 0 ? "shasum -a 256 -c --quiet" : "sha256sum -c --quiet";
    const std::string check = "cd " + q(dst) + " && " + tool + " " + q(backupManifestPath(dst)) + " >/dev/null 2>&1";
    TEST("the manifest passes an independent SHA-256 tool (shasum/sha256sum -c)", std::system(check.c_str()) == 0);
    TEST("backup is byte-identical to the source (kst-diff)", diff(src, dst).same());
    std::vector<std::string> dstDirs;
    const auto copy = snapshot(dst, &dstDirs);
    TEST("every file keeps its size, modification time, permission bits and SHA-256 (incl. read-only, non-ASCII)", sameFiles(before, copy));
    TEST("every folder is recreated, empty ones too", dstDirs == srcDirs);
    {   // the independent tool really checks: one flipped byte must fail it
        const fs::path victim = dst / "PCM/RAM00.PCM";
        std::FILE* f = std::fopen(victim.c_str(), "r+b");
        std::fseek(f, 12345, SEEK_SET);
        const int c = std::fgetc(f);
        std::fseek(f, 12345, SEEK_SET);
        std::fputc(c ^ 0xFF, f);
        std::fclose(f);
        const bool caught = std::system(check.c_str()) != 0 && verifyBackup(dst).find("changed: PCM/RAM00.PCM") != std::string::npos;
        f = std::fopen(victim.c_str(), "r+b");
        std::fseek(f, 12345, SEEK_SET);
        std::fputc(c, f);
        std::fclose(f);
        TEST("a single flipped byte in the backup is caught by both the independent tool and Verify", caught && verifyBackup(dst).empty());
    }
    TEST("source untouched (sizes, times, modes, SHA-256, folders)", [&] {
        std::vector<std::string> d;
        return sameFiles(before, snapshot(src, &d)) && d == srcDirs;
    }());

    // 3. Progress accuracy.
    bool monotone = true, phasesInOrder = true;
    for (size_t i = 1; i < ticks.size(); ++i) {
        monotone &= ticks[i].p.workDone >= ticks[i - 1].p.workDone && ticks[i].p.bytesDone >= ticks[i - 1].p.bytesDone;
        phasesInOrder &= ticks[i].p.phase >= ticks[i - 1].p.phase;
    }
    const auto& last = ticks.back().p;
    std::printf("  info: last progress: work %llu/%llu, bytes %llu/%llu, files %zu/%zu, phase %d\n", (unsigned long long)last.workDone,
                (unsigned long long)last.workTotal, (unsigned long long)last.bytesDone, (unsigned long long)last.bytesTotal, last.filesDone,
                last.filesTotal, int(last.phase));
    TEST("progress only moves forward, phases in order (scan, copy, source check, copy check)", monotone && phasesInOrder);
    TEST("progress ends exactly at 100% (work and copied bytes)", last.workDone == last.workTotal && last.bytesDone == last.bytesTotal &&
                                                                   last.filesDone == before.size());
    double worst = 0;
    for (const auto& t : ticks) worst = std::max(worst, std::abs(t.t / took - double(t.p.workDone) / double(t.p.workTotal)));
    std::printf("  info: progress vs elapsed time: worst gap %.0f%% of the run (%zu updates)\n", worst * 100, ticks.size());
    TEST("progress tracks elapsed time (the bar never sits at 100% for long: gap < 35%)", worst < 0.35);

    // 4. Cancel half way, then recover with a fresh backup.
    const fs::path dc = out / "cancelled";
    const auto c = backupKst(src, dc, [&](const BackupProgress& p) { return p.phase == BackupProgress::Phase::Scan || p.workDone < p.workTotal / 2; });
    std::printf("  info: cancel result: %s\n", c.message.c_str());
    TEST("cancel half way: reported, not published, the partial copy kept marked INCOMPLETE",
         !c.ok && c.cancelled && notPublished(dc) && fs::is_directory(dc.string() + ".INCOMPLETE"));
    TEST("an INCOMPLETE copy never verifies as a backup", !verifyBackup(dc.string() + ".INCOMPLETE").empty());
    const auto rec = backupKst(src, out / "after cancel", {});
    TEST("recovery after cancel: a fresh backup succeeds and verifies", rec.ok && verifyBackup(out / "after cancel").empty());

    // 5. Interrupted: the process is killed mid-copy (crash, power loss of the app), then recovery.
    const fs::path dk = out / "killed";
    const pid_t child = ::fork();
    if (child == 0) { backupKst(src, dk, {}); ::_exit(0); }
    const auto tk = Clock::now();
    while (secs(tk) < 30 && !fs::exists(dk.string() + ".INCOMPLETE/PCM")) ::usleep(2000);
    ::usleep(50000);
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    TEST("killed mid-copy: nothing published, no manifest, the partial copy is left marked INCOMPLETE",
         WIFSIGNALED(status) && notPublished(dk) && fs::is_directory(dk.string() + ".INCOMPLETE"));
    TEST("a second backup to the killed one's name refuses (its INCOMPLETE copy is still there)", !backupKst(src, dk, {}).ok);
    const auto rec2 = backupKst(src, out / "after kill", {});
    TEST("recovery after a kill: a fresh backup succeeds and verifies", rec2.ok && verifyBackup(out / "after kill").empty());
    TEST("source still untouched after cancel, kill and recoveries", sameFiles(before, snapshot(src, nullptr)));

    fs::permissions(src / "GLOBAL.RO", fs::perms::owner_all);
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
