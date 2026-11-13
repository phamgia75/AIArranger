#ifndef AI_ARRANGER_JOBS_JOB_QUEUE_H
#define AI_ARRANGER_JOBS_JOB_QUEUE_H

// A simple serial queue of export jobs (export_job.h): one worker thread runs them in the order they were added, so the
// message thread never waits for a write. Never the audio or MIDI thread. `changed` is called from the worker after
// every state change (the app posts it to the message thread). Destroying the queue cancels every job and returns at
// once: the worker stops at the job's next cancel check and exits; it never touches the queue's owner afterwards.

#include "jobs/export_job.h"
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace ai_arranger::jobs {

class JobQueue {
public:
    explicit JobQueue(std::function<void()> changed = {}) : s_(std::make_shared<Shared>()) {
        s_->changed = std::move(changed);
        std::thread([s = s_] { work(s); }).detach();
    }
    ~JobQueue() {
        std::lock_guard<std::mutex> l(s_->m);
        s_->changed = nullptr;
        s_->stop = true;
        for (auto& j : s_->jobs) j.cancel = true;
        s_->cv.notify_all();
    }
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    uint64_t enqueue(ExportSpec spec) {
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
        bool stop = false;
        std::function<void()> changed;
    };
    std::shared_ptr<Shared> s_;

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
            }
            runExport(*spec, st, [&] { std::lock_guard<std::mutex> l(s->m); return j->cancel; },
                      [&](const JobState& now) {
                          { std::lock_guard<std::mutex> l(s->m); j->state = now; }
                          notify(s);
                      });
        }
    }
};

}  // namespace ai_arranger::jobs

#endif
