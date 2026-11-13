#ifndef AI_ARRANGER_JOBS_EXPORT_JOB_H
#define AI_ARRANGER_JOBS_EXPORT_JOB_H

// One export job model (roadmap E6, UI spec rev 2 §4.11 and §5.7) that every export runs through:
//   validate source -> check dependencies -> check destination (shared output guard) -> preview output (files, sizes)
//   -> estimate size and sample memory -> write (the EXISTING writer, inside the job's own NAME.INCOMPLETE folder)
//   -> publish (one rename that never replaces anything) -> SHA-256 of every published file -> verify (shared
//   verifiers; L3 never here, only from an owner record) -> report beside the output (NAME.export.md + .json).
// The report is written last so that it can state the level; it is not part of the output, so the output still
// checks against its own manifest. Synchronous: JobQueue (job_queue.h) runs it off the message thread.
//
// Cancellation is cooperative: checked before every phase, and inside a writer that takes a progress callback (KST
// backup). Until the publish rename nothing exists under the output's name, so a cancelled or failed job leaves only
// NAME.INCOMPLETE (holding the job's marker file), never a half-published output. A re-run refuses a leftover
// NAME.INCOMPLETE; with explicit consent (cleanOwnIncomplete) it removes it, but only when it carries an export job's
// marker. Once the publish rename has started a job can no longer be cancelled (the KST backup's rule too).

#include "media/kst_backup.h"      // detail::sha256File
#include "media/output_guard.h"
#include "media/sha256.h"
#include "media/sound/resource_graph.h"   // jsonEscape
#include "ui/studio/studio_models.h"      // Verification, badgeLine
#include <atomic>
#include <fstream>
#include <functional>

namespace ai_arranger::jobs {

namespace fs = std::filesystem;
using ui::studio::Check;
using ui::studio::Verification;

enum class Phase { Queued, Validate, Dependencies, Destination, Preview, Estimate, Run, Publish, Manifest, Verify, Report, Done, Failed, Cancelled };
inline const char* phaseName(Phase p) {
    static const char* n[] = {"queued",  "validate source", "check dependencies", "check destination", "preview output", "estimate size",
                              "write",   "publish",         "SHA-256 manifest",   "verify",            "report",         "done",
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
    fs::path dest;                     // the new output: a folder, or a single file
    bool cleanOwnIncomplete = false;   // explicit consent to remove a leftover NAME.INCOMPLETE made by an export job
    std::function<std::string()> validate;                                                            // "" = can export
    std::function<std::vector<std::string>(std::vector<std::string>& notes, uint64_t& sampleBytes)> dependencies;   // missing = blocking
    std::function<std::vector<OutFile>()> preview;
    std::function<WriteResult(const fs::path& out, const std::function<bool(double)>& progress)> write;
    std::function<std::pair<Verification, std::string>(const fs::path& out)> verify;
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
};

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
                    ",\n  \"reason\": " + q(s.reason) + ",\n  \"missing\": " + list(s.missing) + ",\n  \"notes\": " + list(s.notes) +
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
    if (!s.verifyText.empty()) o += "\n## Verification\n\n" + s.verifyText + "\n";
    o += "\n## Files (SHA-256, sha256sum format)\n\n```\n";
    for (const auto& f : s.files) o += f.sha256 + "  " + f.rel + "\n";
    o += "```\n\nNot verified: loading on a Pa5X. L3 comes only from an owner's recorded hardware test.\n";
    return o;
}

// Runs one job to its end. `cancelled` is polled; `update` gets a copy of the state at every phase and progress step.
inline void runExport(const ExportSpec& spec, JobState& st, const std::function<bool()>& cancelled, const std::function<void(const JobState&)>& update) {
    st.type = spec.type;
    st.source = spec.source;
    const fs::path dest = media::folderPath(fs::absolute(spec.dest)), work = media::incompletePath(dest), staged = work / dest.filename();
    st.dest = dest.string();
    const auto stop = [&](Phase p, std::string why) {
        st.stoppedAt = st.phase;
        st.phase = p;
        st.reason = std::move(why);
        st.reportMd = reportMd(st);   // in memory only: nothing is written beside an output that does not exist
        st.reportJson = reportJson(st);
        update(st);
    };
    const auto kept = [&] { return fs::exists(work) ? "; the unfinished output stays at " + work.string() + " (delete it, or re-run with consent to clean it)" : std::string(); };
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
    std::error_code ec;
    if (fs::exists(fs::symlink_status(work, ec))) {
        const bool ours = fs::is_directory(fs::symlink_status(work, ec)) && fs::is_regular_file(fs::symlink_status(work / kJobMarker, ec));
        if (!spec.cleanOwnIncomplete)
            return stop(Phase::Failed, "refusing: " + work.string() + " already exists (an earlier write that did not finish). " +
                                           (ours ? "Re-run with consent to remove it first." : "It was not made by an export job: delete it yourself."));
        if (!ours) return stop(Phase::Failed, "refusing: " + work.string() + " was not made by an export job; it is never removed here");
        fs::remove_all(work, ec);
        if (ec) return stop(Phase::Failed, "cannot remove " + work.string() + ": " + ec.message());
        st.warnings.push_back("removed the leftover " + work.string() + " of an earlier export job (with consent)");
    }
    if (const auto why = media::outputFolderRefusal(dest); !why.empty()) return stop(Phase::Failed, why);

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
    const auto wr = spec.write(staged, [&](double f) {
        const double now = base + share * std::clamp(f, 0.0, 1.0);
        if (now - st.progress >= 0.005) { st.progress = now; update(st); }   // throttled: a writer may call this per chunk
        return !cancelled();
    });
    st.writerText = wr.text;
    if (!wr.error.empty()) return stop(cancelled() ? Phase::Cancelled : Phase::Failed, wr.error + "; nothing was published" + kept());
    if (!fs::exists(fs::symlink_status(staged, ec))) return stop(Phase::Failed, "the writer reported success but wrote nothing" + kept());

    if (!enter(Phase::Publish)) return;
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
    fs::remove(work / kJobMarker, ec);
    if (!fs::remove(work, ec)) st.warnings.push_back("could not remove the empty job folder " + work.string());

    st.phase = Phase::Manifest;   // published: from here on the job runs to its end
    update(st);
    for (auto f : listFiles(dest)) {
        bool c = false;
        std::string err;
        f.sha256 = media::detail::sha256File(fs::is_directory(dest) ? dest / f.rel : dest, [](size_t) { return true; }, &c, &err);
        if (f.sha256.empty()) st.warnings.push_back("cannot hash " + f.rel + ": " + err);
        st.files.push_back(std::move(f));
    }

    st.phase = Phase::Verify;
    update(st);
    if (spec.verify) std::tie(st.level, st.verifyText) = spec.verify(dest);
    st.level.l3 = {Check::NotRun, "not tested on a Pa5X"};   // only an owner record gives L3 (Validation & Reports)

    st.phase = Phase::Report;
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

}  // namespace ai_arranger::jobs

#endif
