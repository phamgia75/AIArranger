#include "engine/midi/scheduler.h"

namespace ai_arranger::midi {

MidiScheduler::MidiScheduler() = default;

void MidiScheduler::setOutputCallback(MidiEventCallback callback) noexcept {
    callback_ = std::move(callback);
}

bool MidiScheduler::scheduleEvent(const uasf::MidiEvent& event) noexcept {
    return queue_.push(event);
}

void MidiScheduler::scheduleEvents(const uasf::MidiEvent* events, size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) {
        if (!queue_.push(events[i])) {
            break; // Queue full, drop remaining
        }
    }
}

void MidiScheduler::advanceTo(int64_t currentTick) noexcept {
    if (!callback_) return;

    // Visit exactly the events queued on entry: dispatch every due one, re-queue
    // future ones in their original order. Stopping at the first future event
    // (as before) left every due event behind it waiting for a later block —
    // with swing, off-beats are scheduled ahead, so grooves ran up to ~17 ms
    // late (Issue #16). Allocation-free; same thread as scheduleEvent().
    uasf::MidiEvent event;
    for (size_t n = queue_.size(); n > 0 && queue_.pop(event); --n) {
        if (static_cast<int64_t>(event.tick) <= currentTick) callback_(event);
        else queue_.push(event);
    }
    last_dispatched_tick_.store(currentTick, std::memory_order_release);
}

void MidiScheduler::clear() noexcept {
    queue_.clear();
}

size_t MidiScheduler::pendingCount() const noexcept {
    return queue_.size();
}

bool MidiScheduler::isFull() noexcept {
    uasf::MidiEvent dummy{};
    return !queue_.push(dummy); // Try push, pop if succeeded
}

} // namespace ai_arranger::midi
