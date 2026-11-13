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

#include "media/kst_backup.h"      // detail::sha256File
#include "media/output_guard.h"
#include "media/sha256.h"
#include "media/sound/resource_graph.h"   // jsonEscape
#include "ui/studio/studio_models.h"      // Verification, badgeLine
#include <atomic>
#include <ctime>
#include <fstream>
#include <functional>
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
    std::function<std::pair<Verification, std::string>(const fs::path& out)> verify;   // runs on the STAGED output; L1 Failed = not published
    std::function<std::string(const fs::path& out)> beforePublish;   // right before the publishing renames: "" = publish, else why not
};

struct JobState {
    uint64_t id = 0;
    std::string type, source, dest;
    Phase phase = Phase::Queued, stoppedAt = Phase::Queued;   // stoppedAt: the phase a failed/cancelled job was in
    double progress = 0;
    std::string reason, writerText, verifyText;
    std::vector<std::string> missing, notes, warnings;
    std::vector<OutFile> planned, files;
    uint64_t plannedBytes = 0, sampleBytes = 0;
    Verification level;
    std::string reportMd, reportJson, reportPath;
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
                    ", \"L2\": " + lvl(s.level.l2) + ", \"L3\": " + lvl(s.level.l3) + "},\n  \"files\": [";
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
// D17: with consent, the leftover `work` is renamed aside in its own folder, never deleted. "" = moved (*to says where).
inline std::string moveAside(const fs::path& work, fs::path* to, const std::function<void()>& beforeRename = {}) {
    if (const auto link = symlinkIn(work); !link.empty())
        return "refusing to move the leftover " + work.string() + ": " + link + " is a symlink";
    struct stat w {}, parent {};
    if (::lstat(work.c_str(), &w) != 0 || !S_ISDIR(w.st_mode)) return "refusing: the leftover " + work.string() + " is not a folder";
    if (::lstat(work.parent_path().c_str(), &parent) != 0 || parent.st_dev != w.st_dev)
        return "refusing to move the leftover " + work.string() + ": it is on another device than its folder";
    *to = work.string() + ".stale-" + utcStamp();
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(*to, ec)) || fs::exists(*to, ec) || ec) return "refusing: " + to->string() + " already exists";
    if (beforeRename) beforeRename();   // test hook: a folder swapped in after the checks
    if (!media::publishNewFolder(work, *to)) return "cannot move " + work.string() + " to " + to->string();   // no-replace rename
    // shortcut: a parent folder swapped for a symlink, or the leftover swapped for another folder, between the checks
    // above and this rename is not prevented (same class as D16 C1: the guard stops accidents, not a concurrent hostile
    // local process). Mitigation (D18 C2): a moved folder that is not the inspected one is renamed back at once.
    struct stat moved {};
    if (::lstat(to->c_str(), &moved) != 0 || moved.st_ino != w.st_ino || moved.st_dev != w.st_dev) {
        const bool back = media::publishNewFolder(*to, work);
        return "refusing: the folder at " + work.string() + " changed between the check and the move, so it was not moved aside; " +
               (back ? "it was renamed back to " + work.string() : "it could NOT be renamed back: it is at " + to->string()) + ". Nothing was deleted.";
    }
    return "";
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
// "" when `work` holds this job's marker (an earlier run of the same export type to the same output), else why not.
inline std::string notOurLeftover(const fs::path& work, const std::string& type, const fs::path& dest) {
    std::error_code ec;
    const auto m = work / kJobMarker;
    if (!fs::is_regular_file(fs::symlink_status(m, ec)) || fs::file_size(m, ec) > 4096 || ec)
        return "refusing: " + work.string() + " has no export job marker, so it may hold someone else's files; delete or rename it by hand";
    std::ifstream f(m);
    std::string line;
    std::getline(f, line);
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

    if (!enter(Phase::Dependencies)) return;
    if (spec.dependencies) st.missing = spec.dependencies(st.notes, st.sampleBytes);
    if (!st.missing.empty()) return stop(Phase::Failed, std::to_string(st.missing.size()) + " missing resource(s): " + st.missing.front());

    if (!enter(Phase::Destination)) return;
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
        if (const auto e = detail::notOurLeftover(work, spec.type, dest); !e.empty()) return stop(Phase::Failed, e);
        fs::path to;
        if (const auto e = detail::moveAside(work, &to); !e.empty()) return stop(Phase::Failed, e);
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
    fs::create_directories(dest.parent_path(), ec);   // as the writers did themselves (the guard has refused mounted volumes)
    if (!fs::create_directory(work, ec)) return stop(Phase::Failed, "cannot create " + work.string() + (ec ? ": " + ec.message() : ""));
    if (!(std::ofstream(work / kJobMarker) << "export job " << st.id << " " << st.type << " -> " << st.dest << "\n"))
        return stop(Phase::Failed, "cannot write in " + work.string() + kept());
    const double base = st.progress, share = 1.0 / double(int(Phase::Done) - 1);
    st.writerThread = thisThread();
    const auto wr = spec.write(staged, [&](double f) {
        const double now = base + share * std::clamp(f, 0.0, 1.0);
        if (now - st.progress >= 0.005) { st.progress = now; update(st); }   // throttled: a writer may call this per chunk
        return !cancelled();
    });
    st.writerText = wr.text;
    if (!wr.error.empty()) return stop(cancelled() ? Phase::Cancelled : Phase::Failed, wr.error + "; nothing was published" + kept());
    if (!fs::exists(fs::symlink_status(staged, ec))) return stop(Phase::Failed, "the writer reported success but wrote nothing" + kept());

    // Checks on the STAGED output (D17): the files that will be published are the files that were checked.
    if (!enter(Phase::Manifest)) return;
    const bool folder = fs::is_directory(fs::symlink_status(staged, ec));
    for (auto f : listFiles(staged)) {
        bool c = false;
        std::string err;
        f.sha256 = media::detail::sha256File(folder ? staged / f.rel : staged, [](size_t) { return true; }, &c, &err);
        if (f.sha256.empty()) return stop(Phase::Failed, "cannot read back " + f.rel + " to hash it: " + err + "; nothing was published" + kept());
        st.files.push_back(std::move(f));
    }

    if (!enter(Phase::Verify)) return;
    if (spec.verify) std::tie(st.level, st.verifyText) = spec.verify(staged);
    for (auto* t : {&st.verifyText, &st.writerText})   // the texts name the published path, not the job's work folder
        for (size_t k; (k = t->find(staged.string())) != std::string::npos;) t->replace(k, staged.string().size(), dest.string());
    st.level.l3 = {Check::NotRun, "not tested on a Pa5X"};   // only an owner record gives L3 (Validation & Reports)
    if (st.level.l1.state == Check::Failed)
        return stop(Phase::Failed, "the written output failed its check (L1: " + st.level.l1.detail + "); nothing was published" + kept());

    if (!enter(Phase::Publish)) return;
    if (spec.beforePublish)
        if (const auto e = spec.beforePublish(staged); !e.empty()) return stop(Phase::Failed, "refusing to publish: " + e + "; nothing was published" + kept());
    // Side files the writer put next to its output (the KST backup's NAME.manifest.sha256) go first, the output last,
    // as backupKst publishes: until that last rename nothing is under the output's name.
    std::vector<fs::path> entries;   // listed first: the folder is not changed while it is being iterated
    for (const auto& e : fs::directory_iterator(work, ec)) entries.push_back(e.path());
    std::vector<std::pair<fs::path, fs::path>> moved;
    const std::string leaf = dest.filename().string();
    for (const auto& from : entries) {
        const std::string n = from.filename().string();
        if (n == kJobMarker || n == leaf) continue;
        if (n.rfind(leaf, 0) != 0) return stop(Phase::Failed, "unexpected " + from.string() + " in the job folder" + kept());
        const fs::path to = dest.parent_path() / (leaf + n.substr(leaf.size()));
        if (!media::publishNewFolder(from, to)) {
            for (const auto& [t, back] : moved) media::publishNewFolder(t, back);
            return stop(Phase::Failed, "refusing: " + to.string() + " appeared while writing" + kept());
        }
        moved.push_back({to, from});
    }
    if (!media::publishNewFolder(staged, dest)) {
        for (const auto& [to, back] : moved) media::publishNewFolder(to, back);
        return stop(Phase::Failed, "refusing: " + dest.string() + " appeared while writing" + kept());
    }
    // The job's own marker file and its now empty folder (rmdir: refused when anything else is in it)
    fs::remove(work / kJobMarker, ec);
    if (!fs::remove(work, ec)) st.warnings.push_back("could not remove the empty job folder " + work.string());

    st.phase = Phase::Report;   // published: from here on the job runs to its end
    update(st);
    st.phase = Phase::Done;
    st.progress = 1;
    st.reportMd = reportMd(st);
    st.reportJson = reportJson(st);
    const std::string side = dest.string() + ".export";
    const auto e1 = media::writeNewFile(side + ".md", {st.reportMd.begin(), st.reportMd.end()});
    const auto e2 = media::writeNewFile(side + ".json", {st.reportJson.begin(), st.reportJson.end()});
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
