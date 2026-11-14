#ifndef AI_ARRANGER_JOBS_JOB_QUEUE_H
#define AI_ARRANGER_JOBS_JOB_QUEUE_H

// A simple serial queue of export jobs (export_job.h): one worker thread runs them in the order they were added, so the
// message thread never waits for a write. Never the audio or MIDI thread. `changed` is called from the worker after
// every state change (the app posts it to the message thread). Destroying the queue cancels every job and returns at
// once: the worker stops at the job's next cancel check and exits; it never touches the queue's owner afterwards.
// An idle worker (no job running, e.g. after the app's quit waited for every job) is joined; a busy one is detached.
// workers() counts the worker threads still alive in this process (a direct check of that join in the tests).
// Every enqueue records its output folder in the one output record (setOutputRecord, set once by the app), so the
// leftover banner (leftoverReport) finds an unfinished write whichever window's queue made it. enqueue only notes the
// folder in memory (it runs on the message thread: no file I/O there, report export Codex r1 #2); a queue's worker
// writes every noted folder, in enqueue order, before it starts a job, so the record names a folder before its job can
// leave anything there. recordedFolders() applies pending notes in memory (never writes: a page opening runs it), so a
// reader never misses one.

#include "jobs/export_job.h"
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace ai_arranger::jobs {

// The output record: a text file, one output folder per line, most recent first, no duplicates, at most kRecordedFolders.
// Process-wide; empty (the default, e.g. in tests) records nothing.
inline constexpr size_t kRecordedFolders = 8;
namespace detail {
struct OutputRecord { std::mutex m; fs::path file; std::vector<std::string> pending; };   // pending: noted, not yet written
inline OutputRecord& outputRecord() { static OutputRecord r; return r; }
inline std::vector<std::string> readRecord(const fs::path& file) {
    std::vector<std::string> r;
    std::ifstream f(file);
    for (std::string line; std::getline(f, line);) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        if (!line.empty() && std::find(r.begin(), r.end(), line) == r.end()) r.push_back(line);
    }
    return r;
}
inline std::vector<std::string> withPending(std::vector<std::string> list, const std::vector<std::string>& pending) {
    for (const auto& folder : pending) {
        list.erase(std::remove(list.begin(), list.end(), folder), list.end());
        list.insert(list.begin(), folder);
    }
    if (list.size() > kRecordedFolders) list.resize(kRecordedFolders);
    return list;
}
inline void flushRecord(OutputRecord& r) {   // r.m held
    if (r.pending.empty() || r.file.empty()) { r.pending.clear(); return; }
    const auto before = readRecord(r.file);
    const auto list = withPending(before, r.pending);
    r.pending.clear();
    if (list == before) return;
    std::error_code ec;
    fs::create_directories(r.file.parent_path(), ec);
    std::ofstream o(r.file, std::ios::trunc);
    for (const auto& d : list) o << d << "\n";
}
}  // namespace detail
inline void setOutputRecord(const fs::path& file) {
    std::lock_guard<std::mutex> l(detail::outputRecord().m);
    detail::flushRecord(detail::outputRecord());   // notes made for the previous file go there
    detail::outputRecord().file = file;
}
// Memory only, no file I/O: the folder is written by the next flushOutputRecord (a queue worker) or recordedFolders.
inline void noteOutputFolder(const std::string& folder) {
    std::lock_guard<std::mutex> l(detail::outputRecord().m);
    if (!detail::outputRecord().file.empty()) detail::outputRecord().pending.push_back(folder);
}
inline void flushOutputRecord() {
    std::lock_guard<std::mutex> l(detail::outputRecord().m);
    detail::flushRecord(detail::outputRecord());
}
inline std::vector<std::string> recordedFolders() {   // read-only: pending notes applied in memory
    std::lock_guard<std::mutex> l(detail::outputRecord().m);
    if (detail::outputRecord().file.empty()) return {};
    return detail::withPending(detail::readRecord(detail::outputRecord().file), detail::outputRecord().pending);
}
inline void recordOutputFolder(const std::string& folder) {
    noteOutputFolder(folder);
    flushOutputRecord();
}

class JobQueue {
public:
    explicit JobQueue(std::function<void()> changed = {}) : s_(std::make_shared<Shared>()) {
        s_->changed = std::move(changed);
        {
            std::lock_guard<std::mutex> l(registry().m);
            registry().queues.push_back(s_);
        }
        ++workers();
        thread_ = std::thread([s = s_] {
            struct Exit { ~Exit() { --workers(); } } exit;
            work(s);
        });
    }
    ~JobQueue() {
        bool running;
        {
            std::lock_guard<std::mutex> l(s_->m);
            s_->changed = nullptr;
            s_->stop = true;
            for (auto& j : s_->jobs) j.cancel = true;
            running = s_->running;
            s_->cv.notify_all();
        }
        if (running) thread_.detach();   // never wait for a writer: it stops at its next cancel check (holds only Shared)
        else thread_.join();             // idle: it sees `stop` at once and exits
    }
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    uint64_t enqueue(ExportSpec spec) {
        {
            std::error_code ec;
            const auto parent = media::folderPath(fs::absolute(spec.dest, ec)).parent_path();
            if (!ec) noteOutputFolder(parent.string());   // written by the worker (flushOutputRecord), never here
        }
        uint64_t id;
        {
            std::lock_guard<std::mutex> l(s_->m);
            id = ++s_->next;
            Job j;
            j.spec = std::make_shared<ExportSpec>(std::move(spec));
            j.state.id = id;
            j.state.type = j.spec->type;
            j.state.source = j.spec->source;
            j.state.dest = j.spec->dest.string();
            s_->jobs.push_back(std::move(j));
            s_->cv.notify_all();
        }
        notify(s_);
        return id;
    }
    // A queued job is cancelled at once; a running one at its next cancel check (export_job.h).
    void cancel(uint64_t id) {
        {
            std::lock_guard<std::mutex> l(s_->m);
            for (auto& j : s_->jobs)
                if (j.state.id == id && !finished(j.state.phase)) {
                    j.cancel = true;
                    if (j.state.phase == Phase::Queued) { j.state.phase = Phase::Cancelled; j.state.reason = "cancelled before it started; nothing was written"; }
                }
        }
        notify(s_);
    }
    std::vector<JobState> snapshot() const {
        std::lock_guard<std::mutex> l(s_->m);
        std::vector<JobState> r;
        for (const auto& j : s_->jobs) r.push_back(j.state);
        return r;
    }
    static std::atomic<int>& workers() { static std::atomic<int> n{0}; return n; }
    // The NAME.INCOMPLETE work folders of every unfinished job of every queue in this process (a detached worker's too):
    // these are being written now, not left over.
    static std::set<std::string> unfinishedWork() {
        std::set<std::string> r;
        std::lock_guard<std::mutex> l(registry().m);
        auto& qs = registry().queues;
        qs.erase(std::remove_if(qs.begin(), qs.end(), [](const auto& w) { return w.expired(); }), qs.end());
        for (const auto& w : qs)
            if (const auto s = w.lock()) {
                std::lock_guard<std::mutex> sl(s->m);
                for (const auto& j : s->jobs)
                    if (!finished(j.state.phase)) {
                        std::error_code ec;
                        r.insert(media::incompletePath(media::folderPath(fs::absolute(j.spec->dest, ec))).string());
                    }
            }
        return r;
    }
    bool idle() const {
        std::lock_guard<std::mutex> l(s_->m);
        for (const auto& j : s_->jobs) if (!finished(j.state.phase)) return false;
        return true;
    }

private:
    struct Job { std::shared_ptr<ExportSpec> spec; JobState state; bool cancel = false; };
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        std::deque<Job> jobs;   // never erased: the job list shows them all; deque keeps references stable
        uint64_t next = 0;
        bool stop = false, running = false;   // running: the worker is inside a job
        std::function<void()> changed;
    };
    std::shared_ptr<Shared> s_;
    std::thread thread_;
    struct Registry { std::mutex m; std::vector<std::weak_ptr<Shared>> queues; };
    static Registry& registry() { static Registry r; return r; }

    static void notify(const std::shared_ptr<Shared>& s) {
        std::function<void()> f;
        { std::lock_guard<std::mutex> l(s->m); f = s->changed; }
        if (f) f();
    }
    static void work(std::shared_ptr<Shared> s) {
        for (;;) {
            Job* j = nullptr;
            std::shared_ptr<ExportSpec> spec;
            JobState st;
            {
                std::unique_lock<std::mutex> l(s->m);
                s->cv.wait(l, [&] {
                    if (s->stop) return true;
                    for (auto& x : s->jobs) if (x.state.phase == Phase::Queued) { j = &x; return true; }
                    return false;
                });
                if (s->stop) return;
                spec = j->spec;
                st = j->state;
                s->running = true;
            }
            flushOutputRecord();   // this job's folder (and every earlier note) is in the record before the job writes
            runExport(*spec, st, [&] { std::lock_guard<std::mutex> l(s->m); return j->cancel; },
                      [&](const JobState& now) {
                          { std::lock_guard<std::mutex> l(s->m); j->state = now; }
                          notify(s);
                      });
            std::lock_guard<std::mutex> l(s->m);
            s->running = false;
        }
    }
};

// The leftover banner's listing: each recorded output folder (most recent first) with its unfinished writes, minus the
// work folders of jobs still running in this process. Read-only; folders without leftovers are left out.
inline std::vector<std::pair<std::string, std::vector<std::string>>> leftoverReport() {
    std::vector<std::pair<std::string, std::vector<std::string>>> r;
    const auto running = JobQueue::unfinishedWork();
    for (const auto& dir : recordedFolders()) {
        auto names = leftovers(dir);
        names.erase(std::remove_if(names.begin(), names.end(), [&](const std::string& n) { return running.count((fs::path(dir) / n).string()) > 0; }),
                    names.end());
        if (!names.empty()) r.emplace_back(dir, std::move(names));
    }
    return r;
}

}  // namespace ai_arranger::jobs

#endif
