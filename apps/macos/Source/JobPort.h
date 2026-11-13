#pragma once

// How a page hands a write to an export job (jobs/, E6: D17 never delete, D18 no overlap with a source). A page uses a
// JobPort; by default the port of its own JobRunner (a standalone window), and inside the AI Music Studio the studio's
// one queue (so every write is in its job list, counted by its busy() and cancellable there).

#include <juce_events/juce_events.h>
#include "jobs/job_queue.h"
#include <chrono>
#include <map>
#include <optional>

namespace ai_arranger::app {

struct JobPort {
    // `done` runs once on the message thread when the job has finished (done, failed or cancelled).
    std::function<uint64_t(jobs::ExportSpec, std::function<void(const jobs::JobState&)> done)> enqueue;
    std::function<void(uint64_t)> cancel;
    std::function<std::optional<jobs::JobState>(uint64_t)> state;   // for progress
};

// A message-thread front of a JobQueue: lands each finished job once on its `done`.
class JobRunner {
public:
    JobRunner() = default;
    ~JobRunner() { *alive_ = false; }
    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    JobPort port() {
        return {[this](jobs::ExportSpec s, std::function<void(const jobs::JobState&)> done) {
                    const auto id = queue_.enqueue(std::move(s));
                    done_[id] = std::move(done);
                    return id;
                },
                [this](uint64_t id) { queue_.cancel(id); },
                [this](uint64_t id) -> std::optional<jobs::JobState> {
                    for (const auto& j : queue_.snapshot()) if (j.id == id) return j;
                    return std::nullopt;
                }};
    }
    // Blocks until no job is queued or running (no message loop needed): a window that must not close mid-copy.
    void waitIdle() const { while (!queue_.idle()) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }

private:
    void land() {   // message thread
        for (const auto& j : queue_.snapshot()) {
            const auto d = done_.find(j.id);
            if (!jobs::finished(j.phase) || d == done_.end()) continue;
            auto f = std::move(d->second);
            done_.erase(d);
            if (f) f(j);
        }
    }

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::shared_ptr<std::atomic<bool>> posted_ = std::make_shared<std::atomic<bool>>(false);
    std::map<uint64_t, std::function<void(const jobs::JobState&)>> done_;   // message thread only
    jobs::JobQueue queue_{[this, alive = std::weak_ptr<bool>(alive_), posted = posted_] {   // last: destroyed first
        if (posted->exchange(true)) return;   // one landing in flight reads the latest state
        juce::MessageManager::callAsync([this, alive, posted] {
            posted->store(false);
            if (!alive.expired()) land();
        });
    }};
};

// The jobs a page started and waits for: busy() until each has landed. Every start gets its own token, captured by that
// job's `done`, so overlapping starts each count once and land once (Codex L2 P1: one stored id lost the first of two),
// and a re-run from the studio's job list (the same `done` again) counts nothing.
class PendingJobs {
public:
    bool waiting() const { return n_ > 0; }
    std::shared_ptr<bool> start() {
        ++n_;
        return std::make_shared<bool>(true);
    }
    bool land(const std::shared_ptr<bool>& token) {   // true once per token
        if (!std::exchange(*token, false)) return false;
        --n_;
        return true;
    }

private:
    int n_ = 0;   // message thread only
};

}  // namespace ai_arranger::app
