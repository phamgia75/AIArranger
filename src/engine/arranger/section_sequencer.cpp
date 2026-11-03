#include "engine/arranger/section_sequencer.h"

namespace ai_arranger::arranger {

SectionSequencer::SectionSequencer() = default;

void SectionSequencer::setSections(const uasf::SectionDefinition* sections, size_t count) noexcept {
    sections_ = sections;
    section_count_ = count;
}

void SectionSequencer::queueSection(int index) noexcept {
    queued_section_.store(index, std::memory_order_release);
}

void SectionSequencer::commitSection(int index) noexcept {
    if (!sections_ || index < 0 || index >= static_cast<int>(section_count_)) return;
    const int pending = queued_section_.load(std::memory_order_acquire);
    activateSection(index);
    if (pending >= 0 && pending != index)
        queued_section_.store(pending, std::memory_order_release);
}

void SectionSequencer::queueIntro() noexcept {
    // Find the first Intro section (symmetric with queueFill/queueEnding).
    for (size_t i = 0; i < section_count_; ++i) {
        if (sections_ && sections_[i].type >= uasf::SectionType::Intro1 &&
            sections_[i].type <= uasf::SectionType::Intro3) {
            queued_section_.store(static_cast<int>(i), std::memory_order_release);
            return;
        }
    }
}

void SectionSequencer::queueFill() noexcept {
    // Fill for the Main that is playing (Main A -> Fill In AA, ... Main D ->
    // Fill In DD, arranger-engine-spec §5.3); otherwise the first Fill.
    const int cur = current_section_.load(std::memory_order_acquire);
    if (sections_ && cur >= 0 && cur < static_cast<int>(section_count_)) {
        const auto t = sections_[cur].type;
        if (t >= uasf::SectionType::Main1 && t <= uasf::SectionType::Main4) {
            const auto want = static_cast<uasf::SectionType>(
                static_cast<int>(uasf::SectionType::Fill1) +
                (static_cast<int>(t) - static_cast<int>(uasf::SectionType::Main1)));
            for (size_t i = 0; i < section_count_; ++i) {
                if (sections_[i].type == want) {
                    queued_section_.store(static_cast<int>(i), std::memory_order_release);
                    return;
                }
            }
        }
    }
    for (size_t i = 0; i < section_count_; ++i) {
        if (sections_ && sections_[i].type >= uasf::SectionType::Fill1 &&
            sections_[i].type <= uasf::SectionType::Fill4) {
            queued_section_.store(static_cast<int>(i), std::memory_order_release);
            return;
        }
    }
}

void SectionSequencer::queueBreak() noexcept {
    // Find the first Break section.
    for (size_t i = 0; i < section_count_; ++i) {
        if (sections_ && sections_[i].type == uasf::SectionType::Break) {
            queued_section_.store(static_cast<int>(i), std::memory_order_release);
            return;
        }
    }
}

void SectionSequencer::queueEnding() noexcept {
    // Find the first Ending section
    for (size_t i = 0; i < section_count_; ++i) {
        if (sections_ && sections_[i].type >= uasf::SectionType::Ending1 &&
            sections_[i].type <= uasf::SectionType::Ending3) {
            queued_section_.store(static_cast<int>(i), std::memory_order_release);
            return;
        }
    }
}

SequencerState SectionSequencer::getState() const noexcept {
    return state_.load(std::memory_order_acquire);
}

int SectionSequencer::getCurrentSectionIndex() const noexcept {
    return current_section_.load(std::memory_order_acquire);
}

int SectionSequencer::getQueuedSectionIndex() const noexcept {
    return queued_section_.load(std::memory_order_acquire);
}

bool SectionSequencer::isQueued() const noexcept {
    return queued_section_.load(std::memory_order_acquire) >= 0;
}

bool SectionSequencer::advance(int64_t currentTick, int64_t barSize) noexcept {
    if (!sections_ || section_count_ == 0 || barSize <= 0) return false;

    // ── First-start immediate activation (Gate 10B Task C) ────────────
    // On START the intro is queued but no section is playing yet. Requiring a
    // bar boundary here meant the intro did not begin until the clock crossed
    // into bar 2 — a full bar of silence (the G9 "intro one-bar delay"). Yamaha
    // begins the intro at bar 1 (tick 0), so when nothing is playing yet we
    // activate the queued section immediately. Subsequent section changes still
    // wait for the next bar boundary (musical quantise) via the path below.
    if (current_section_.load(std::memory_order_acquire) < 0) {
        int queued = queued_section_.load(std::memory_order_acquire);
        if (queued >= 0 && queued < static_cast<int>(section_count_)) {
            last_bar_start_.store((currentTick / barSize) * barSize,
                                  std::memory_order_release);
            activateSection(queued);
            return true;
        }
    }

    // Check if we've crossed a bar boundary
    int64_t currentBar = currentTick / barSize;
    int64_t lastBar = last_bar_start_.load(std::memory_order_acquire) / barSize;

    if (currentBar > lastBar) {
        last_bar_start_.store(currentBar * barSize, std::memory_order_release);

        // Execute queued section switch on bar boundary
        int queued = queued_section_.load(std::memory_order_acquire);
        if (queued >= 0 && queued < static_cast<int>(section_count_)) {
            activateSection(queued);
            return true; // Section switched
        }
    }

    return false;
}

void SectionSequencer::activateSection(int index) noexcept {
    current_section_.store(index, std::memory_order_release);
    queued_section_.store(-1, std::memory_order_release);

    // Update state based on section type
    auto st = sections_[index].type;
    if (st >= uasf::SectionType::Ending1 && st <= uasf::SectionType::Ending3) {
        state_.store(SequencerState::PlayingEnding, std::memory_order_release);
    } else if (st >= uasf::SectionType::Fill1 && st <= uasf::SectionType::Fill4) {
        state_.store(SequencerState::PlayingFill, std::memory_order_release);
    } else if (st >= uasf::SectionType::Main1 && st <= uasf::SectionType::Main4) {
        state_.store(SequencerState::PlayingMain, std::memory_order_release);
    } else if (st >= uasf::SectionType::Intro1 && st <= uasf::SectionType::Intro3) {
        state_.store(SequencerState::PlayingIntro, std::memory_order_release);
    }
}

void SectionSequencer::setCurrentChord(const Chord& chord) noexcept {
    current_chord_.store(
        (static_cast<uint32_t>(chord.type) & 0xFF) |
        ((static_cast<uint32_t>(chord.root) & 0xFF) << 8),
        std::memory_order_release
    );
}

Chord SectionSequencer::getCurrentChord() const noexcept {
    uint32_t data = current_chord_.load(std::memory_order_acquire);
    Chord c;
    c.type = static_cast<ChordType>(data & 0xFF);
    c.root = static_cast<uint8_t>((data >> 8) & 0xFF);
    c.bass = 0;
    c.octave = 0;
    return c;
}

void SectionSequencer::panic() noexcept {
    state_.store(SequencerState::Panic, std::memory_order_release);
    current_section_.store(-1, std::memory_order_release);
    queued_section_.store(-1, std::memory_order_release);
}

} // namespace ai_arranger::arranger
