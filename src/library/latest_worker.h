// One background thread that runs one job at a time with at most ONE pending job: submit() replaces the pending job
// (latest wins) and marks the running one stale, so it can stop early. A job receives `stale()`; long loops check it.
// The destructor marks everything stale and joins the thread, so no job outlives the owner. Used by the Library
// Manager's view reports (E7); a job must not block on the thread that destroys the worker (the message thread).
#ifndef AI_ARRANGER_LIBRARY_LATEST_WORKER_H
#define AI_ARRANGER_LIBRARY_LATEST_WORKER_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace ai_arranger::library {

class LatestWorker {
public:
    using Job = std::function<void(const std::function<bool()>& stale)>;

    LatestWorker() {
        try { thread_ = std::thread([this] { run(); }); }
        catch (const std::system_error& e) { error_ = std::string("cannot start the background worker: ") + e.what(); }
    }
    ~LatestWorker() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
            pending_ = nullptr;
            ++gen_;   // the running job sees stale() and stops early
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    LatestWorker(const LatestWorker&) = delete;
    LatestWorker& operator=(const LatestWorker&) = delete;

    // Replaces any pending job; the running one becomes stale. False (with error()) when no thread could be started.
    bool submit(Job job) {
        if (!thread_.joinable()) return false;
        {
            std::lock_guard<std::mutex> l(m_);
            pending_ = std::move(job);
            ++gen_;
        }
        cv_.notify_all();
        return true;
    }
    const std::string& error() const { return error_; }

private:
    void run() {
        for (;;) {
            Job job;
            uint64_t mine = 0;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || pending_; });
                if (stop_) return;
                job = std::move(pending_);
                pending_ = nullptr;
                mine = gen_;
            }
            job([this, mine] { return gen_.load() != mine; });
        }
    }

    std::mutex m_;
    std::condition_variable cv_;
    Job pending_;
    bool stop_ = false;
    std::atomic<uint64_t> gen_{0};
    std::string error_;
    std::thread thread_;   // last: started once every other member exists
};

}  // namespace ai_arranger::library

#endif
