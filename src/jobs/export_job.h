#ifndef AI_ARRANGER_JOBS_EXPORT_JOB_H
#define AI_ARRANGER_JOBS_EXPORT_JOB_H

// One export job model (roadmap E6, UI spec rev 2 §4.11 and §5.7) that every export runs through:
//   validate source -> check dependencies -> check destination (shared output guard) -> preview output (files, sizes)
//   -> estimate size and sample memory -> write (the EXISTING writer, inside the job's own NAME.INCOMPLETE folder)
//   -> SHA-256 of every staged file -> verify the STAGED output (shared verifiers; L3 never here, only from an owner
//   record) -> publish (renames that never replace anything) -> report beside the output (NAME.export.md + .json).
// Nothing is published unless every check passed on the staged files: a hash that cannot be read or an L1 that fails
// fails the job. The report is not part of the output, so the output still checks against its own manifest.
// Synchronous: JobQueue (job_queue.h) runs it off the message thread.
//
// D17: an export job NEVER deletes. Cancellation is cooperative: checked before every phase up to publish, and inside a
// writer that takes a progress callback (KST backup). A cancelled or failed job leaves only NAME.INCOMPLETE, never a
// half-published output. A re-run refuses a leftover NAME.INCOMPLETE; with explicit consent (moveLeftoverAside), and
// only after the output guard has passed, it RENAMES the leftover within the same folder to
// NAME.INCOMPLETE.stale-<UTC time> (refused when a path component is a symlink, the target exists, or the leftover is
// on another device) and the report names that folder for the user to delete by hand. Any exception inside a job fails
// that job (runExport is an exception boundary). Once the publish rename has started a job can no longer be cancelled.
// Path races (HIGH): the destination's parent is pinned by handle (detail::pinParent) and everything is done through it.
// shortcut: (1) legacy `write`/`verify`/`beforePublish` callbacks still take a pathname (the real, link-free path of the
// staged output, re-identified after each); an ancestor swapped only while one runs can redirect that callback's own writes, and
// it fails the job afterwards. Upgrade: move each writer to `writeAt`. (2) macOS has no rename-by-fd or conditional unlink, so
// a swap inside the identity-check-to-rename/unlink window is found afterwards. (3) A hostile process that may write inside
// the authorized folder itself, or rename that folder itself, is detected where possible, not prevented (same class as D16 C1).

#include "media/kst_backup.h"      // detail::sha256File
#include "media/output_guard.h"
#include "media/sha256.h"
#include "media/sound/resource_graph.h"   // jsonEscape
#include "ui/studio/studio_models.h"      // Verification, badgeLine
#include <atomic>
#include <climits>
#include <dirent.h>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <map>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace ai_arranger::jobs {

namespace fs = std::filesystem;
using ui::studio::Check;
using ui::studio::Verification;

enum class Phase { Queued, Validate, Dependencies, Destination, Preview, Estimate, Run, Manifest, Verify, Publish, Report, Done, Failed, Cancelled };
inline const char* phaseName(Phase p) {
    static const char* n[] = {"queued",  "validate source",  "check dependencies", "check destination", "preview output", "estimate size",
                              "write",   "SHA-256 manifest", "verify",             "publish",           "report",         "done",
                              "failed",  "cancelled"};
    return n[int(p)];
}
inline bool finished(Phase p) { return p >= Phase::Done; }

struct OutFile { std::string rel; uint64_t bytes = 0; std::string sha256; bool exact = true; };   // a preview has no sha256
struct WriteResult { std::string error, text; };                                                      // text: the writer's own report

// What one export provides. Every function but write is read-only. write creates the output at exactly `out` (a new
// folder or file inside the job's NAME.INCOMPLETE) with the existing writer; progress(0..1) returns false to cancel.
struct ExportSpec {
    std::string type, source;
    std::vector<fs::path> sources;     // every path the job reads: the output and its NAME.INCOMPLETE may not overlap any (D18)
    fs::path dest;                     // the new output: a folder, or a single file
    bool moveLeftoverAside = false;    // explicit consent to rename a leftover NAME.INCOMPLETE to NAME.INCOMPLETE.stale-<time>
    std::function<std::string()> validate;                                                            // "" = can export
    std::function<std::vector<std::string>(std::vector<std::string>& notes, uint64_t& sampleBytes)> dependencies;   // missing = blocking
    std::function<std::vector<OutFile>()> preview;
    std::function<WriteResult(const fs::path& out, const std::function<bool(double)>& progress)> write;
    // Instead of write, for a folder output (E9 Codex r3): the runner makes `out` below its pinned job folder and hands
    // the writer that folder open (O_DIRECTORY|O_NOFOLLOW); the writer writes through the fd only, never re-resolving `out`.
    std::function<WriteResult(int outFd, const fs::path& out, const std::function<bool(double)>& progress)> writeAt;
    std::function<std::pair<Verification, std::string>(const fs::path& out)> verify;   // runs on the STAGED output; L1 Failed = not published
    std::function<std::map<std::string, std::string>()> verifiedHashes;   // after verify: rel -> SHA-256 it checked; replaces the Manifest step's (D55 N3)
    std::function<std::string(const fs::path& out)> beforePublish;   // right before the publishing renames: "" = publish, else why not
    std::function<void()> finished;   // once the job has ended, in any terminal state (done, failed, cancelled, exception): release what it held
    // P1 conversion report: what the output carries and how sure it is: {"EXACT"|"MEASURED"|"ESTIMATED"|"MISSING", what}
    std::function<std::vector<std::pair<std::string, std::string>>()> conversion;
};

struct JobState {
    uint64_t id = 0;
    std::string type, source, dest;
    Phase phase = Phase::Queued, stoppedAt = Phase::Queued;   // stoppedAt: the phase a failed/cancelled job was in
    double progress = 0;
    std::string reason, writerText, verifyText;
    std::vector<std::string> missing, notes, warnings;
    std::vector<std::pair<std::string, std::string>> conversion;   // ExportSpec::conversion
    std::vector<OutFile> planned, files;
    uint64_t plannedBytes = 0, sampleBytes = 0;
    Verification level;
    std::string reportMd, reportJson, reportPath;
    bool packageManifest = false;   // Done: the output holds PACKAGE.sha256 (checked by the worker, not the message thread)
    size_t writerThread = 0;   // hash of the thread that ran the writer: evidence that a UI's write never runs on its message thread
};
inline size_t thisThread() { return std::hash<std::thread::id>{}(std::this_thread::get_id()); }

inline constexpr const char* kJobMarker = "AIARRANGER-EXPORT-JOB";   // inside NAME.INCOMPLETE: made by an export job
// Above this the Sound can never fit, even compressed (MAN p.1181 "up to a half"; resource_graph.h budget).
inline constexpr uint64_t kBlockingSampleBytes = media::graph::kPa5xCompressedEquivalentDecimal;

inline std::string fmtBytes(uint64_t b) {
    char s[32];
    if (b >= 1000000) std::snprintf(s, sizeof s, "%.2f MB", double(b) / 1e6);
    else std::snprintf(s, sizeof s, "%llu bytes", (unsigned long long)b);
    return s;
}

// Every regular file below `root` (or `root` itself when it is a file), relative paths, sorted. Read-only.
inline std::vector<OutFile> listFiles(const fs::path& root) {
    std::vector<OutFile> r;
    std::error_code ec;
    if (fs::is_regular_file(fs::symlink_status(root, ec))) return {{root.filename().string(), fs::file_size(root, ec)}};
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) r.push_back({fs::relative(it->path(), root, ec).generic_string(), it->file_size(ec)});
    std::sort(r.begin(), r.end(), [](const OutFile& a, const OutFile& b) { return a.rel < b.rel; });
    return r;
}

// Unfinished writes in `dir`: NAME.INCOMPLETE, and leftovers moved aside as NAME.INCOMPLETE.stale-<time>. Names only,
// sorted; read-only (nothing is opened, moved or deleted).
inline std::vector<std::string> leftovers(const fs::path& dir) {
    std::vector<std::string> r;
    std::error_code ec;
    static const std::string inc = ".INCOMPLETE";
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto n = it->path().filename().string();
        if ((n.size() > inc.size() && n.compare(n.size() - inc.size(), inc.size(), inc) == 0) || n.find(inc + ".stale-") != std::string::npos) r.push_back(n);
    }
    std::sort(r.begin(), r.end());
    return r;
}

inline constexpr const char* kConversionStatus = "structurally valid, not verified on a Pa5X";
inline const char* conversionStatus(Phase p) { return p == Phase::Done ? kConversionStatus : "not published"; }

// Machine report (aiarranger-export-report/1) and its human-readable twin.
inline std::string reportJson(const JobState& s) {
    auto q = [](const std::string& v) { return "\"" + media::graph::jsonEscape(v) + "\""; };
    auto list = [&](const std::vector<std::string>& v) {
        std::string o = "[";
        for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + q(v[i]);
        return o + "]";
    };
    auto lvl = [&](const ui::studio::Level& l) {
        return "{\"state\": " + q(l.state == Check::Passed ? "passed" : l.state == Check::Failed ? "failed" : "not-run") + ", \"detail\": " + q(l.detail) + "}";
    };
    std::string o = "{\n  \"schema\": \"aiarranger-export-report/1\",\n  \"job\": " + std::to_string(s.id) + ",\n  \"type\": " + q(s.type) +
                    ",\n  \"source\": " + q(s.source) + ",\n  \"output\": " + q(s.dest) + ",\n  \"result\": " + q(phaseName(s.phase)) +
                    ",\n  \"reason\": " + q(s.reason) + ",\n  \"writer\": " + q(s.writerText) + ",\n  \"verifyText\": " + q(s.verifyText) + ",\n  \"missing\": " + list(s.missing) + ",\n  \"notes\": " + list(s.notes) +
                    ",\n  \"warnings\": " + list(s.warnings) + ",\n  \"plannedBytes\": " + std::to_string(s.plannedBytes) +
                    ",\n  \"sampleBytes\": " + std::to_string(s.sampleBytes) + ",\n  \"verification\": {\"L1\": " + lvl(s.level.l1) +
                    ", \"L2\": " + lvl(s.level.l2) + ", \"L3\": " + lvl(s.level.l3) + "},\n";
    if (!s.conversion.empty()) {
        o += "  \"status\": " + q(conversionStatus(s.phase)) + ",\n  \"conversion\": [";
        for (size_t i = 0; i < s.conversion.size(); ++i)
            o += std::string(i ? "," : "") + "\n    {\"class\": " + q(s.conversion[i].first) + ", \"item\": " + q(s.conversion[i].second) + "}";
        o += "\n  ],\n";
    }
    o += "  \"files\": [";
    for (size_t i = 0; i < s.files.size(); ++i)
        o += std::string(i ? "," : "") + "\n    {\"path\": " + q(s.files[i].rel) + ", \"bytes\": " + std::to_string(s.files[i].bytes) +
             ", \"sha256\": " + q(s.files[i].sha256) + "}";
    return o + (s.files.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

inline std::string reportMd(const JobState& s) {
    std::string o = "# Export report: " + s.type + "\n\n- Job: " + std::to_string(s.id) + "\n- Result: " + phaseName(s.phase) +
                    (s.reason.empty() ? "" : " (" + s.reason + ")") + "\n- Source (only read): " + s.source + "\n- Output: " + s.dest +
                    "\n- Verification: " + ui::studio::badgeLine(s.level) + "\n  " + ui::studio::spokenBadges(s.level) +
                    "\n- Planned size: " + fmtBytes(s.plannedBytes) + (s.sampleBytes ? "; sample memory (PCM): " + fmtBytes(s.sampleBytes) : "") + "\n";
    auto section = [&](const char* title, const std::vector<std::string>& v) {
        if (v.empty()) return;
        o += std::string("\n## ") + title + "\n\n";
        for (const auto& x : v) o += "- " + x + "\n";
    };
    section("Missing resources", s.missing);
    section("Warnings", s.warnings);
    section("Notes", s.notes);
    if (!s.conversion.empty()) {
        o += std::string("\n## Conversion (EXACT / MEASURED / ESTIMATED / MISSING)\n\nStatus: ") + conversionStatus(s.phase) + ".\n\n";
        for (const char* c : {"EXACT", "MEASURED", "ESTIMATED", "MISSING"})
            for (const auto& [k, v] : s.conversion) if (k == c) o += "- **" + k + "** " + v + "\n";
    }
    if (!s.writerText.empty()) o += "\n## Writer report\n\n```\n" + s.writerText + (s.writerText.back() == '\n' ? "" : "\n") + "```\n";
    if (!s.verifyText.empty()) o += "\n## Verification\n\n" + s.verifyText + "\n";
    o += "\n## Files (SHA-256, sha256sum format)\n\n```\n";
    for (const auto& f : s.files) o += f.sha256 + "  " + f.rel + "\n";
    o += "```\n\nNot verified: loading on a Pa5X. L3 comes only from an owner's recorded hardware test.\n";
    return o;
}

namespace detail {
// The first symlink among the components of `p` below its first two ("/" and e.g. "Users"; macOS's own /var, /tmp and
// /etc are links at that level), or "" when there is none. Read-only.
inline std::string symlinkIn(const fs::path& p) {
    fs::path acc;
    int depth = 0;
    std::error_code ec;
    for (const auto& c : p) {
        acc /= c;
        if (++depth > 2 && fs::is_symlink(fs::symlink_status(acc, ec))) return acc.string();
    }
    return "";
}
inline std::string utcStamp() {
    char b[32];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(b, sizeof b, "%Y%m%dT%H%M%SZ", &tm);
    return b;
}
// True when `p` is on a case-insensitive volume (macOS default APFS/HFS+): its nearest existing folder decides.
inline bool caseInsensitive(fs::path p) {
#ifdef _PC_CASE_SENSITIVE
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec) && p.has_parent_path() && p.parent_path() != p) p = p.parent_path();
    return ::pathconf(p.c_str(), _PC_CASE_SENSITIVE) == 0;
#else
    (void)p;
    return false;
#endif
}
// True when `a` and `b` are the same path or one lies inside the other (symlinks in the existing part resolved; case
// folded on a case-insensitive volume). A path that cannot be resolved counts as overlapping (callers refuse).
inline bool overlaps(const fs::path& a, const fs::path& b) {
    std::error_code ea, eb;
    std::string x = fs::weakly_canonical(fs::absolute(a, ea), ea).string(), y = fs::weakly_canonical(fs::absolute(b, eb), eb).string();
    if (ea || eb) return true;
    if (caseInsensitive(a) || caseInsensitive(b))
        for (auto* s : {&x, &y}) for (auto& c : *s) c = char(std::tolower(static_cast<unsigned char>(c)));
    while (x.size() > 1 && x.back() == '/') x.pop_back();
    while (y.size() > 1 && y.back() == '/') y.pop_back();
    const auto under = [](const std::string& in, const std::string& top) { return in.size() > top.size() && in.compare(0, top.size(), top) == 0 && in[top.size()] == '/'; };
    return x == y || under(x, y) || under(y, x);
}
inline bool sameFile(const struct stat& a, const struct stat& b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
// Renames `a` (in `from`) to `b` (in `to`) and never replaces an entry.
inline bool moveNoReplace(int from, const std::string& a, int to, const std::string& b) { return media::renameAtNoReplace(from, a, to, b); }
// Test seam (never set in production): called at named points of a job so a test can play the hostile process.
inline std::function<void(const char*)>& testHook() { static std::function<void(const char*)> h; return h; }
inline void hook(const char* where) { if (testHook()) testHook()(where); }
// The first source that the open folder `fd` is, or lies inside (device + inode, walking ".."): independent of any path, so a
// folder that is renamed into a source after it was opened is still found. "" = none.
inline std::string insideSource(int fd, const std::vector<fs::path>& sources) {
    for (const auto& src : sources) {
        struct stat s {};
        if (::stat(src.c_str(), &s) == 0 && media::detail::isSameOrBelow(fd, s)) return src.string();
    }
    return "";
}
// Path-check-to-open race (HIGH): the destination's parent is opened ONCE and everything after goes through that handle.
// `anchor` is the nearest folder that existed when the guards ran and `want` its identity then; the OS resolves its
// ancestors at this single open (a legitimate symlinked ancestor, macOS /var -> /private/var, is followed). Missing
// folders below it (`rest`) are made one component at a time through the handle, never following a link. The opened
// folder must be that anchor, must not be or lie inside a source, and its REAL location must overlap no source either
// (fail closed when the OS cannot report it). Anything else refuses before anything is written.
inline std::string pinParent(const fs::path& anchor, const struct stat& want, const std::vector<std::string>& rest, const std::vector<fs::path>& sources,
                             const std::string& leaf, const std::string& workName, media::detail::Fd& out, fs::path* real) {
    media::detail::Fd cur(::open(anchor.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (cur.v < 0) return "cannot open " + anchor.string() + ": " + std::strerror(errno);
    struct stat now {};
    if (::fstat(cur.v, &now) != 0) return "cannot examine " + anchor.string();
    if (!sameFile(want, now)) return "refusing: " + anchor.string() + " is not the folder that was checked (it was replaced after the check); nothing was written";
    if (const auto src = insideSource(cur.v, sources); !src.empty())
        return "refusing: the output folder " + anchor.string() + " is, or lies inside, the job's source " + src + "; nothing was written";
    for (const auto& name : rest) {   // missing folders: made below the handle, each opened without following a link
        if (::mkdirat(cur.v, name.c_str(), 0755) != 0 && errno != EEXIST) return "cannot create " + name + " below " + anchor.string() + ": " + std::strerror(errno);
        media::detail::Fd next(::openat(cur.v, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (next.v < 0) return "refusing: " + name + " below " + anchor.string() + " is not a plain folder; nothing was written";
        std::swap(cur.v, next.v);
    }
    const std::string p = media::detail::fdPath(cur.v);
    if (p.empty()) return "refusing: cannot confirm where " + anchor.string() + " really is; nothing was written";
    if (const auto src = insideSource(cur.v, sources); !src.empty())
        return "refusing: the output folder " + p + " lies inside the job's source " + src + "; nothing was written";
    for (const auto& src : sources)
        if (overlaps(fs::path(p) / leaf, src) || overlaps(fs::path(p) / workName, src))
            return "refusing: the output below " + p + " overlaps the job's source " + src.string() + "; nothing was written";
    std::swap(out.v, cur.v);
    *real = p;
    return "";
}
// Writes `bytes` as a new file `name` below `dirFd`: temp file (O_EXCL|O_NOFOLLOW), then a no-replace rename. The temp
// file is removed on failure only while it is still the file this call created.
inline std::string writeNewAt(int dirFd, const std::string& name, const std::string& bytes) {
    const std::string tmp = name + ".tmp";
    const int fd = ::openat(dirFd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return "refusing: cannot create " + tmp + " (it may already exist)";
    struct stat mine {};
    const bool ok = ::fstat(fd, &mine) == 0 && ::write(fd, bytes.data(), bytes.size()) == ssize_t(bytes.size());
    const bool closed = ::close(fd) == 0;
    const auto dropIfMine = [&]() -> std::string { const auto why = media::removeOwned(dirFd, tmp, mine, false); return why.empty() ? "" : " (" + why + ")"; };
    if (!closed || !ok) return "cannot write " + tmp + dropIfMine();
    if (!moveNoReplace(dirFd, tmp, dirFd, name)) return "refusing: " + name + " appeared while writing" + dropIfMine();
    return "";
}
// Names in the folder `dirFd` (a fresh open of it: the caller's offset is not shared), sorted. Read-only.
inline bool namesAt(int dirFd, std::vector<std::string>& out) {
    media::detail::Fd d(::openat(dirFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    DIR* dir = d.v < 0 ? nullptr : ::fdopendir(d.v);
    if (!dir) return false;
    d.v = -1;   // owned by `dir` now
    for (const dirent* e; (e = ::readdir(dir));) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") out.push_back(n);
    }
    ::closedir(dir);
    std::sort(out.begin(), out.end());
    return true;
}
// Every regular file below the open folder (links not followed, no path re-resolved), relative paths, sorted. false on any error.
inline bool listFilesAt(int dirFd, std::vector<OutFile>& out, const std::string& rel = "", int depth = 0) {
    std::vector<std::string> names;
    if (depth > 64 || !namesAt(dirFd, names)) return false;
    for (const auto& n : names) {
        struct stat s {};
        if (::fstatat(dirFd, n.c_str(), &s, AT_SYMLINK_NOFOLLOW) != 0) return false;
        if (S_ISREG(s.st_mode)) out.push_back({rel + n, uint64_t(s.st_size)});
        else if (S_ISDIR(s.st_mode)) {
            media::detail::Fd sub(::openat(dirFd, n.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (sub.v < 0 || !listFilesAt(sub.v, out, rel + n + "/", depth + 1)) return false;
        }
    }
    if (depth == 0) std::sort(out.begin(), out.end(), [](const OutFile& a, const OutFile& b) { return a.rel < b.rel; });
    return true;
}
// SHA-256 of `rel` below the open folder, reached one component at a time without following a link. "" + *error on failure.
inline std::string sha256At(int dirFd, const std::string& rel, std::string* error) {
    const size_t slash = rel.rfind('/');
    const int d = slash == std::string::npos ? ::dup(dirFd) : media::detail::openDirBelow(dirFd, rel.substr(0, slash));
    media::detail::Fd dir(d);
    std::FILE* in = d < 0 ? nullptr : media::detail::openRegular(d, rel.substr(slash == std::string::npos ? 0 : slash + 1).c_str());
    if (!in) { *error = "cannot read " + rel + " (missing, or no longer a regular file)"; return ""; }
    bool cancelled = false;
    const auto h = media::detail::sha256Stream(in, nullptr, nullptr, &cancelled, error, rel);
    std::fclose(in);
    return h;
}
// D17: with consent, the leftover `workName` below the pinned folder `parentFd` is renamed aside in that folder, never
// deleted. "" = moved (*to says where). Everything is relative to the handle, so a swapped path cannot redirect it.
inline std::string moveAside(int parentFd, const std::string& workName, const fs::path& work, fs::path* to, const struct stat* validated = nullptr,
                             const std::function<void()>& beforeRename = {}) {
    struct stat w {}, parent {};
    if (::fstatat(parentFd, workName.c_str(), &w, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(w.st_mode))
        return "refusing: the leftover " + work.string() + " is not a folder (or is a symlink)";
    if (validated && !sameFile(*validated, w)) return "refusing: the leftover " + work.string() + " is not the folder whose marker was checked; nothing was moved";
    if (::fstat(parentFd, &parent) != 0 || parent.st_dev != w.st_dev)
        return "refusing to move the leftover " + work.string() + ": it is on another device than its folder";
    const std::string toName = workName + ".stale-" + utcStamp();
    *to = work.parent_path() / toName;
    struct stat ex {};
    if (::fstatat(parentFd, toName.c_str(), &ex, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) return "refusing: " + to->string() + " already exists";
    if (beforeRename) beforeRename();   // test hook: a folder swapped in after the checks
    if (!moveNoReplace(parentFd, workName, parentFd, toName)) return "cannot move " + work.string() + " to " + to->string();
    // The rename is relative to the pinned folder, but the entry itself can still be swapped between the lstat above and
    // the rename (no rename-by-inode); a moved folder that is not the inspected one is renamed back at once.
    struct stat moved {};
    if (::fstatat(parentFd, toName.c_str(), &moved, AT_SYMLINK_NOFOLLOW) != 0 || !sameFile(moved, w)) {
        const bool back = moveNoReplace(parentFd, toName, parentFd, workName);
        return "refusing: the folder at " + work.string() + " changed between the check and the move, so it was not moved aside; " +
               (back ? "it was renamed back to " + work.string() : "it could NOT be renamed back: it is at " + to->string()) + ". Nothing was deleted.";
    }
    return "";
}

// "" when the leftover `workName` below the pinned folder `parentFd` holds this job's marker (an earlier run of the same
// export type to the same output), else why not. Read through the handle, no link followed.
inline std::string notOurLeftover(int parentFd, const std::string& workName, const fs::path& work, const std::string& type, const fs::path& dest, struct stat* id = nullptr) {
    const std::string none = "refusing: " + work.string() + " has no export job marker, so it may hold someone else's files; delete or rename it by hand";
    media::detail::Fd w(::openat(parentFd, workName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (w.v < 0 || (id && ::fstat(w.v, id) != 0)) return none;
    media::detail::Fd m(::openat(w.v, kJobMarker, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat ms {};
    if (m.v < 0 || ::fstat(m.v, &ms) != 0 || !S_ISREG(ms.st_mode) || ms.st_size > 4096) return none;
    std::string text(size_t(ms.st_size), '\0');
    if (::read(m.v, text.data(), text.size()) != ssize_t(text.size())) return none;
    const std::string line = text.substr(0, text.find('\n'));
    const std::string want = " " + type + " -> " + dest.string();
    if (line.rfind("export job ", 0) != 0 || line.size() < want.size() || line.compare(line.size() - want.size(), want.size(), want) != 0)
        return "refusing: the export job marker in " + work.string() + " is not for this export (" + type + " -> " + dest.string() +
               "); delete or rename the folder by hand";
    return "";
}
}  // namespace detail

// Runs one job to its end. `cancelled` is polled; `update` gets a copy of the state at every phase and progress step.
inline void runExportSteps(const ExportSpec& spec, JobState& st, const std::function<bool()>& cancelled, const std::function<void(const JobState&)>& update) {
    st.type = spec.type;
    st.source = spec.source;
    std::error_code ec;
    const fs::path dest = media::folderPath(fs::absolute(spec.dest, ec)), work = media::incompletePath(dest), staged = work / dest.filename();
    st.dest = dest.string();
    const auto stop = [&](Phase p, std::string why) {
        st.stoppedAt = st.phase;
        st.phase = p;
        st.reason = std::move(why);
        st.reportMd = reportMd(st);   // in memory only: nothing is written beside an output that does not exist
        st.reportJson = reportJson(st);
        update(st);
    };
    const auto kept = [&] {
        std::error_code e;
        return fs::exists(fs::symlink_status(work, e)) ? "; the unfinished output stays at " + work.string() + " (delete it by hand, or re-run with consent to move it aside)" : std::string();
    };
    // Enter phase p (progress = its share); false once cancelled. From Publish on, cancel is no longer possible.
    const auto enter = [&](Phase p) {
        st.phase = p;
        if (p <= Phase::Publish && cancelled()) { stop(Phase::Cancelled, "cancelled by the user at " + std::string(phaseName(p)) + "; nothing was published" + kept()); return false; }
        st.progress = double(int(p) - 1) / double(int(Phase::Done) - 1);
        update(st);
        return true;
    };

    if (!enter(Phase::Validate)) return;
    if (const auto e = spec.validate ? spec.validate() : ""; !e.empty()) return stop(Phase::Failed, e);
    if (spec.conversion) st.conversion = spec.conversion();

    if (!enter(Phase::Dependencies)) return;
    if (spec.dependencies) st.missing = spec.dependencies(st.notes, st.sampleBytes);
    if (!st.missing.empty()) return stop(Phase::Failed, std::to_string(st.missing.size()) + " missing resource(s): " + st.missing.front());

    if (!enter(Phase::Destination)) return;
    // Path-check-to-open race: the folder the guards below judge is the folder this job may write in. Its identity is taken
    // first (when it exists) and compared with the folder the runner finally opens (detail::pinParent).
    // The anchor is the nearest folder that exists now; folders missing below it are made later through its handle.
    struct stat authorized {};
    fs::path anchor = dest.parent_path();
    std::vector<std::string> rest;   // missing components below the anchor, outermost last
    while (::stat(anchor.c_str(), &authorized) != 0 || !S_ISDIR(authorized.st_mode)) {
        if (!anchor.has_parent_path() || anchor.parent_path() == anchor) return stop(Phase::Failed, "refusing: no folder above " + dest.string() + " exists");
        rest.push_back(anchor.filename().string());
        anchor = anchor.parent_path();
    }
    std::reverse(rest.begin(), rest.end());
    const std::string workName = work.filename().string(), leaf = dest.filename().string();
    // D18: the output and its NAME.INCOMPLETE may not be, contain or lie inside anything the job reads, before any rename
    for (const auto& src : spec.sources)
        if (detail::overlaps(dest, src) || detail::overlaps(work, src))
            return stop(Phase::Failed, "refusing: the output " + dest.string() + " (work folder " + work.string() + ") overlaps the job's source " +
                                           src.string() + "; choose a destination outside the source");
    // The shared output guard first (mounted volume, existing output); only when nothing but a leftover NAME.INCOMPLETE is
    // in the way may it be moved aside, and only with consent. A job never deletes (D17).
    if (const auto why = media::outputFolderRefusal(dest); !why.empty()) {
        if (why != media::incompleteRefusal(dest)) return stop(Phase::Failed, why);
        if (!spec.moveLeftoverAside)
            return stop(Phase::Failed, "refusing: " + work.string() + " already exists (an earlier write that did not finish). "
                                       "Delete it by hand, or Re-run with consent to move it aside (it is never deleted).");
        // A leftover is moved only along a path with no symlink in it (D17, kept as before); the pin below then makes sure the
        // folder moved is the one that was looked at.
        if (const auto link = detail::symlinkIn(work); !link.empty())
            return stop(Phase::Failed, "refusing to move the leftover " + work.string() + ": " + link + " is a symlink");
        media::detail::Fd leftoverParent;
        fs::path realParent;
        if (!rest.empty()) return stop(Phase::Failed, "refusing: the leftover " + work.string() + " is gone from its folder");
        if (const auto e = detail::pinParent(anchor, authorized, {}, spec.sources, leaf, workName, leftoverParent, &realParent); !e.empty())
            return stop(Phase::Failed, e);
        struct stat leftoverId {};   // the folder whose marker was read is the folder that is moved
        if (const auto e = detail::notOurLeftover(leftoverParent.v, workName, work, spec.type, dest, &leftoverId); !e.empty()) return stop(Phase::Failed, e);
        fs::path to;
        if (const auto e = detail::moveAside(leftoverParent.v, workName, work, &to, &leftoverId); !e.empty()) return stop(Phase::Failed, e);
        st.warnings.push_back("moved the leftover " + work.string() + " aside to " + to.string() + " (with consent); delete it by hand when you no longer need it");
        if (const auto again = media::outputFolderRefusal(dest); !again.empty()) return stop(Phase::Failed, again);
    }

    if (!enter(Phase::Preview)) return;
    if (spec.preview) st.planned = spec.preview();
    for (const auto& f : st.planned) st.plannedBytes += f.bytes;

    if (!enter(Phase::Estimate)) return;
    if (st.sampleBytes > kBlockingSampleBytes)
        return stop(Phase::Failed, "sample memory " + fmtBytes(st.sampleBytes) + " exceeds even the 8 GB compressed capacity of the Pa5X");
    if (st.sampleBytes > media::graph::kPa5xUserSampleBytesDecimal)
        st.warnings.push_back("sample memory " + fmtBytes(st.sampleBytes) + " is above 4 GB: fits only if compressed on import (irreversible)");
    const auto space = fs::space(dest.parent_path(), ec);
    if (!ec && space.available < st.plannedBytes + st.plannedBytes / 10)
        return stop(Phase::Failed, "not enough free space: " + fmtBytes(space.available) + " available, " + fmtBytes(st.plannedBytes) + " + 10% needed");

    if (!enter(Phase::Run)) return;
    // E9 Codex r3: the job folder is made and opened below the pinned output folder, and published from there by fd, so a
    // NAME.INCOMPLETE or a staged output swapped by path is never written through or published.
    // HIGH (path-check-to-open): the parent is opened once, compared with the authorized folder and checked again at its REAL
    // location (overlap with a source, output refusals); only then is anything created, always through the handle.
    using media::detail::Fd;
    Fd parentFd;
    fs::path realParent;
    if (const auto e = detail::pinParent(anchor, authorized, rest, spec.sources, leaf, workName, parentFd, &realParent); !e.empty())   // also makes missing folders
        return stop(Phase::Failed, e);
    if (const auto why = media::outputFolderRefusal(realParent / leaf); !why.empty()) return stop(Phase::Failed, why);
    if (::mkdirat(parentFd.v, workName.c_str(), 0755) != 0)
        return stop(Phase::Failed, "cannot create " + work.string() + ": " + std::strerror(errno));
    // By-path readers and writers (the legacy `write`, hashing, verify) get the REAL location of the staged output, with no
    // symlink in it; `byPathIsStaged` then proves that path still names the staged output (see the shortcut at Publish).
    const fs::path stagedReal = realParent / workName / leaf;
    const Fd workFd(::openat(parentFd.v, workName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat markerId {};
    {
        const std::string m = "export job " + std::to_string(st.id) + " " + st.type + " -> " + st.dest + "\n";
        const int fd = workFd.v < 0 ? -1 : ::openat(workFd.v, kJobMarker, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        const bool good = fd >= 0 && ::fstat(fd, &markerId) == 0 && ::write(fd, m.data(), m.size()) == ssize_t(m.size());
        if (fd < 0 || !(good & (::close(fd) == 0))) return stop(Phase::Failed, "cannot write in " + work.string() + kept());
    }
    const double base = st.progress, share = 1.0 / double(int(Phase::Done) - 1);
    st.writerThread = thisThread();
    const auto progress = [&](double f) {
        const double now = base + share * std::clamp(f, 0.0, 1.0);
        if (now - st.progress >= 0.005) { st.progress = now; update(st); }   // throttled: a writer may call this per chunk
        return !cancelled();
    };
    // The pinned folder is still where it was and still outside every source (a folder renamed after the pin is found by
    // identity, not by path); checked after the write, before publishing and before the report.
    const auto locationMoved = [&]() -> std::string {
        if (const auto src = detail::insideSource(parentFd.v, spec.sources); !src.empty()) return "the output folder was moved into the job's source " + src;
        return media::detail::fdPath(parentFd.v) == realParent.string() ? "" : "the output folder was moved or renamed while the job ran";
    };
    if (const auto m = locationMoved(); !m.empty()) return stop(Phase::Failed, "refusing: " + m + "; nothing was published" + kept());
    struct stat staging {};   // the identity of the staged output: only that one is published
    Fd outDir;                // the staged folder, open for the checks below (no path is re-resolved)
    WriteResult wr;
    if (spec.writeAt) {
        if (::mkdirat(workFd.v, leaf.c_str(), 0755) != 0) return stop(Phase::Failed, "cannot create " + staged.string() + kept());
        outDir.v = ::openat(workFd.v, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (outDir.v < 0 || ::fstat(outDir.v, &staging) != 0) return stop(Phase::Failed, "cannot open " + staged.string() + kept());
        wr = spec.writeAt(outDir.v, stagedReal, progress);
    } else {
        // The legacy writer takes a pathname: it gets the pinned job folder (thread-local), so everything it creates, renames or
        // writes below `stagedReal` goes through the open folder and any path outside it is refused (media::pinned).
        const media::pinned::Root pinnedWork{workFd.v, (realParent / workName).string()};
        const media::pinned::Scope scope(&pinnedWork);
        wr = spec.write(stagedReal, progress);
        if (wr.error.empty() && ::fstatat(workFd.v, leaf.c_str(), &staging, AT_SYMLINK_NOFOLLOW) != 0)
            return stop(Phase::Failed, "the writer reported success but wrote nothing" + kept());
    }
    st.writerText = wr.text;
    if (wr.error.empty() && !spec.writeAt && S_ISDIR(staging.st_mode)) {   // the legacy writer made a folder: open it by handle for the checks
        outDir.v = ::openat(workFd.v, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat o {};
        if (outDir.v < 0 || ::fstat(outDir.v, &o) != 0 || !detail::sameFile(o, staging))
            return stop(Phase::Failed, "refusing: the staged output was replaced while it was written; nothing was published" + kept());
    }
    const auto byPathIsStaged = [&] {   // the real path still leads to the staged output (no ancestor swapped since the pin)
        struct stat now {};
        return ::lstat(stagedReal.c_str(), &now) == 0 && detail::sameFile(now, staging);
    };
    if (wr.error.empty())
        if (const auto m = locationMoved(); !m.empty()) return stop(Phase::Failed, "refusing: " + m + "; nothing was published" + kept());
    if (wr.error.empty() && !byPathIsStaged())
        return stop(Phase::Failed, "refusing: a folder on the way to " + stagedReal.string() + " was replaced while it was written; nothing was published" + kept());
    if (!wr.error.empty()) return stop(cancelled() ? Phase::Cancelled : Phase::Failed, wr.error + "; nothing was published" + kept());

    // Checks on the STAGED output (D17): the files that will be published are the files that were checked.
    if (!enter(Phase::Manifest)) return;
    // Listed and hashed through the staged folder's handle: the names, sizes and bytes are those of the folder that is published.
    const auto stagedFiles = [&](std::vector<OutFile>& out) {
        if (!S_ISDIR(staging.st_mode)) {   // a single staged file
            struct stat f {};
            if (::fstatat(workFd.v, leaf.c_str(), &f, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(f.st_mode)) return false;
            out.push_back({leaf, uint64_t(f.st_size)});
            return true;
        }
        return outDir.v >= 0 && detail::listFilesAt(outDir.v, out);
    };
    std::vector<OutFile> listed;
    if (!stagedFiles(listed)) return stop(Phase::Failed, "cannot list the staged output; nothing was published" + kept());
    for (auto f : listed) {
        std::string err;
        f.sha256 = S_ISDIR(staging.st_mode) ? detail::sha256At(outDir.v, f.rel, &err) : detail::sha256At(workFd.v, leaf, &err);
        if (f.sha256.empty()) return stop(Phase::Failed, "cannot read back " + f.rel + " to hash it: " + err + "; nothing was published" + kept());
        st.files.push_back(std::move(f));
    }

    if (!enter(Phase::Verify)) return;
    if (spec.verify) std::tie(st.level, st.verifyText) = spec.verify(stagedReal);
    if (!byPathIsStaged()) return stop(Phase::Failed, "refusing: a folder on the way to " + stagedReal.string() + " was replaced while it was verified; nothing was published" + kept());
    for (auto* t : {&st.verifyText, &st.writerText})   // the texts name the published path, not the job's work folder
        for (const auto* name : {&stagedReal, &staged})
            for (size_t k; (k = t->find(name->string())) != std::string::npos;) t->replace(k, name->string().size(), dest.string());
    st.level.l3 = {Check::NotRun, "not tested on a Pa5X"};   // only an owner record gives L3 (Validation & Reports)
    if (st.level.l1.state == Check::Failed)
        return stop(Phase::Failed, "the written output failed its check (L1: " + st.level.l1.detail + "); nothing was published" + kept());
    if (spec.verifiedHashes) {   // the report then names the bytes verify checked, not a pathname read before it
        const auto h = spec.verifiedHashes();
        for (auto& f : st.files) {
            const auto it = h.find(f.rel);
            if (it == h.end() || it->second.empty()) return stop(Phase::Failed, "no verified hash for " + f.rel + "; nothing was published" + kept());
            f.sha256 = it->second;
        }
    }

    if (!enter(Phase::Publish)) return;
    if (spec.beforePublish)
        if (const auto e = spec.beforePublish(stagedReal); !e.empty()) return stop(Phase::Failed, "refusing to publish: " + e + "; nothing was published" + kept());
    // Only what was verified is published: the job folder and the staged output are still the ones made and checked
    // (device + inode, no link followed), and the staged files still have the listed names and sizes.
    const auto same = detail::sameFile;
    const auto identity = [&] {   // the job folder and the staged output by device + inode, no link followed; the pinned folder still in place
        struct stat w {}, wn {}, o {};
        return locationMoved().empty() && ::fstat(workFd.v, &w) == 0 && ::fstatat(parentFd.v, workName.c_str(), &wn, AT_SYMLINK_NOFOLLOW) == 0 && same(w, wn) &&
               ::fstatat(workFd.v, leaf.c_str(), &o, AT_SYMLINK_NOFOLLOW) == 0 && same(o, staging) && byPathIsStaged();
    };
    const auto unchanged = [&] {
        if (!identity()) return false;
        std::vector<OutFile> now;
        if (!stagedFiles(now) || now.size() != st.files.size()) return false;
        for (size_t i = 0; i < now.size(); ++i)
            if (now[i].rel != st.files[i].rel || now[i].bytes != st.files[i].bytes) return false;
        return true;
    };
    if (!unchanged()) return stop(Phase::Failed, "refusing: the staged output changed after it was verified; nothing was published" + kept());
    const auto move = detail::moveNoReplace;   // never replaces an entry
    // Side files the writer put next to its output (the KST backup's NAME.manifest.sha256) go first, the output last,
    // as backupKst publishes: until that last rename nothing is under the output's name.
    std::vector<std::string> entries;   // listed first (through the handle): the folder is not changed while it is being iterated
    if (!detail::namesAt(workFd.v, entries)) return stop(Phase::Failed, "cannot list " + work.string() + "; nothing was published" + kept());
    std::vector<std::string> moved;
    // A side file that cannot be moved back (the name was taken meanwhile) stays published: the failure says so, never "nothing was published".
    const auto failPublish = [&](const std::string& why) {
        std::string stuck;
        for (const auto& n : moved)
            if (!move(parentFd.v, n, workFd.v, n)) stuck += (stuck.empty() ? "" : ", ") + (dest.parent_path() / n).string();
        return stop(Phase::Failed, why + (stuck.empty() ? "; nothing was published" : "; these files could NOT be moved back and stay published: " + stuck) + kept());
    };
    for (const auto& n : entries) {
        if (n == kJobMarker || n == leaf) continue;
        if (n.rfind(leaf, 0) != 0) return failPublish("unexpected " + (work / n).string() + " in the job folder");
        if (!move(workFd.v, n, parentFd.v, n)) return failPublish("refusing: " + (dest.parent_path() / n).string() + " appeared while writing");
        moved.push_back(n);
    }
    detail::hook("afterSideMoves");
    // shortcut: the identity is checked right before the rename, but macOS has no rename-by-fd, so a swap in the window
    // between this check and renameat is caught only after it (the published entry is compared again and moved back).
    if (!identity()) return failPublish("refusing: the staged output changed after it was verified");
    if (!move(workFd.v, leaf, parentFd.v, leaf)) return failPublish("refusing: " + dest.string() + " appeared while writing");
    struct stat pub {};
    if (::fstatat(parentFd.v, leaf.c_str(), &pub, AT_SYMLINK_NOFOLLOW) != 0 || !same(pub, staging)) {   // swapped during the rename
        const bool back = move(parentFd.v, leaf, workFd.v, leaf);
        return failPublish("refusing: the output at " + dest.string() + " is not the verified one" + (back ? "; it was moved back" : "; it could NOT be moved back and stays at that name"));
    }
    // The job's own marker file and its now empty folder (rmdir: refused when anything else is in it)
    // Cleanup removes only entries proven to be this job's (media::removeOwned): a substituted marker or folder is never
    // unlinked; it is kept and reported, and the leftover stays for the user.
    detail::hook("beforeMarkerCleanup");
    if (const auto why = media::removeOwned(workFd.v, kJobMarker, markerId, false); !why.empty())
        st.warnings.push_back("the job marker in " + work.string() + " was not removed: " + why);
    detail::hook("beforeWorkRemoval");
    struct stat workId {};
    if (::fstat(workFd.v, &workId) != 0) st.warnings.push_back("could not identify the job folder " + work.string() + "; it was kept");
    else if (const auto why = media::removeOwned(parentFd.v, workName, workId, true); !why.empty())
        st.warnings.push_back("the job folder " + work.string() + " was not removed: " + why);

    st.phase = Phase::Report;   // published: from here on the job runs to its end
    {
        struct stat pm {};
        st.packageManifest = ::fstatat(parentFd.v, (leaf + "/" + media::sound::pkgio::FileSet::kPackageManifest).c_str(), &pm, 0) == 0 && S_ISREG(pm.st_mode);   // as the studio checked it before
    }
    update(st);
    st.phase = Phase::Done;
    st.progress = 1;
    st.reportMd = reportMd(st);
    st.reportJson = reportJson(st);
    const std::string side = dest.string() + ".export";
    const auto moveNote = locationMoved();   // the report goes only to the folder that was authorized
    const auto e1 = !moveNote.empty() ? moveNote : detail::writeNewAt(parentFd.v, leaf + ".export.md", st.reportMd);   // through the pinned folder, like the output
    const auto e2 = !moveNote.empty() ? moveNote : detail::writeNewAt(parentFd.v, leaf + ".export.json", st.reportJson);
    if (e1.empty()) st.reportPath = side + ".md";
    if (!e1.empty() || !e2.empty()) {
        st.warnings.push_back("export report not written beside the output: " + (e1.empty() ? e2 : e1));
        st.reportMd = reportMd(st);
        st.reportJson = reportJson(st);
    }
    update(st);
}

// The exception boundary (D17): anything a validator, writer or filesystem call throws fails this job, nothing else.
inline void runExport(const ExportSpec& spec, JobState& st, const std::function<bool()>& cancelled, const std::function<void(const JobState&)>& update) {
    struct Finished { const ExportSpec& s; ~Finished() { if (s.finished) s.finished(); } } finished{spec};   // every exit path, exceptions too
    std::string why;
    try {
        runExportSteps(spec, st, cancelled, update);
        return;
    } catch (const std::exception& e) {
        why = e.what();
    } catch (...) {
        why = "unknown exception";
    }
    st.stoppedAt = st.phase;
    st.phase = Phase::Failed;
    std::error_code ec;
    const bool left = fs::exists(fs::symlink_status(media::incompletePath(media::folderPath(fs::absolute(spec.dest, ec))), ec));
    st.reason = "internal error at " + std::string(phaseName(st.stoppedAt)) + ": " + why +
                (st.stoppedAt >= Phase::Report ? "; the output was published at " + st.dest + " but its report was not written" : "; nothing was published") +
                (left ? "; the unfinished output stays at " + media::incompletePath(media::folderPath(fs::absolute(spec.dest, ec))).string() : "");
    try {
        st.reportMd = reportMd(st);
        st.reportJson = reportJson(st);
        update(st);
    } catch (...) {}   // the queue reads the state it already has
}

}  // namespace ai_arranger::jobs

#endif
