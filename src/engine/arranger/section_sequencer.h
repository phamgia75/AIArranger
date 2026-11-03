#ifndef AI_ARRANGER_ARRANGER_SECTION_SEQUENCER_H
#define AI_ARRANGER_ARRANGER_SECTION_SEQUENCER_H

#include "engine/uasf/types.h"
#include "engine/arranger/chord_input.h"
#include <cstdint>
#include <atomic>

namespace ai_arranger::arranger {

/**
 * Section sequencer — handles section switching on bar boundaries.
 *
 * Flow: Intro n → Main (loop) → Fill → Main → ... → Ending
 *
 * Section switching is queued (non-realtime) and executed
 * on the next bar boundary (realtime-safe).
 */

enum class SequencerState : uint8_t {
    Stopped,
    PlayingIntro,
    PlayingMain,
    PlayingFill,
    PlayingEnding,
    Panic,
};

class SectionSequencer {
public:
    SectionSequencer();

    // ── Control (non-realtime) ────────────────────────────────────
    void setSections(const uasf::SectionDefinition* sections, size_t count) noexcept;
    void queueSection(int index) noexcept;  // Queue switch at next bar
    // Switch now (engine thread) — used when the current section reaches its end
    // (Intro/Fill/Break -> Main). A different user-queued switch stays pending.
    void commitSection(int index) noexcept;
    // Tick of the bar boundary at which the last switch was committed by
    // advance() — the musical origin of the section that just started.
    int64_t lastBarStart() const noexcept { return last_bar_start_.load(std::memory_order_acquire); }
    void queueIntro() noexcept;
    void queueFill() noexcept;
    void queueBreak() noexcept;
    void queueEnding() noexcept;

    // ── Realtime query ────────────────────────────────────────────
    SequencerState getState() const noexcept;
    int getCurrentSectionIndex() const noexcept;
    int getQueuedSectionIndex() const noexcept;
    bool isQueued() const noexcept;

    // ── Realtime advance — call from scheduler thread ─────────────
    // Returns true if section switched this tick
    bool advance(int64_t currentTick, int64_t barSize) noexcept;

    // ── Chord handling ───────────────────────────────────────────
    void setCurrentChord(const Chord& chord) noexcept;
    Chord getCurrentChord() const noexcept;

    // ── Panic ─────────────────────────────────────────────────────
    void panic() noexcept;

private:
    // Activate a section now (set current index, clear queue, update state).
    void activateSection(int index) noexcept;

    std::atomic<SequencerState> state_{SequencerState::Stopped};
    std::atomic<int> current_section_{-1};
    std::atomic<int> queued_section_{-1};
    std::atomic<int> queued_section_callback_{-1}; // executed on bar boundary
    std::atomic<int64_t> last_bar_start_{0};

    // Current chord (atomic packed)
    std::atomic<uint32_t> current_chord_{0};

    // Non-atomic (set before play, stable during playback)
    const uasf::SectionDefinition* sections_{nullptr};
    size_t section_count_{0};
};

} // namespace ai_arranger::arranger
#endif // AI_ARRANGER_ARRANGER_SECTION_SEQUENCER_H
