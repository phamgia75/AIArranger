#ifndef AI_ARRANGER_ARRANGER_STYLE_PLAYER_H
#define AI_ARRANGER_ARRANGER_STYLE_PLAYER_H

#include "engine/uasf/types.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include "engine/arranger/chord_input.h"
#include "engine/arranger/section_sequencer.h"
#include "engine/articulation/renderer.h"
#include "engine/music/groove.h"
#include <vector>
#include <functional>
#include <cstdint>
#include <atomic>
#include <cstdint>

namespace ai_arranger::arranger {

/**
 * Minimal style playback engine for Gate 2.
 *
 * Loads a UASF style definition and plays it through:
 * - RealtimeClock for timing
 * - SectionSequencer for section switching
 * - MidiScheduler for event dispatch
 * - PanicHandler for all-notes-off
 *
 * Architecture rules:
 * - No Yamaha/Korg logic
 * - No internal sound engine
 * - No UI dependency
 * - All realtime paths are lock-free
 */

using PlaybackEventCallback = std::function<void(const char* event)>;

class StylePlayer {
public:
    StylePlayer(realtime::RealtimeClock& clock,
                midi::MidiScheduler& scheduler,
                midi::PanicHandler& panic);

    // ── Style loading (non-realtime) ───────────────────────────────
    void loadStyle(const uasf::StyleDefinition& style) noexcept;
    void clearStyle() noexcept;

    // ── Control ────────────────────────────────────────────────────
    bool start(int introSectionIndex = 0) noexcept;
    void stop() noexcept;
    void panic() noexcept;
    void intro() noexcept;
    void fill() noexcept;
    void breakSection() noexcept;
    void ending() noexcept;
    void switchSection(int index) noexcept;

    // ── Chord ──────────────────────────────────────────────────────
    void setChord(Chord chord) noexcept;
    Chord getCurrentChord() const noexcept;

    // ── Realtime tick — call from audio callback ─────────────────
    void tick() noexcept;

    // ── Status ─────────────────────────────────────────────────────
    bool isPlaying() const noexcept;
    int  getCurrentSection() const noexcept;
    const uasf::SectionDefinition* getCurrentSectionDef() const noexcept;
    SequencerState getState() const noexcept;

    // ── Event callback for demo/debug ──────────────────────────────
    void setEventCallback(PlaybackEventCallback cb) noexcept;

    // ── Articulation render strategy (Gate 10 Task C) ──────────────
    // Defaults to the naive pass-through renderer (backward-compatible). The
    // caller owns the renderer's lifetime; pass a long-lived instance.
    void setArticulationRenderer(const articulation::IArticulationRenderer& r) noexcept;

    // ── Swing / shuffle groove (Gate 10B Task B) ───────────────────
    // 50 = straight (default), ~67 = triplet shuffle, 75 = hard shuffle.
    // Realtime-safe to call while playing (atomic).
    void setSwing(uint8_t swingPercent) noexcept;
    uint8_t getSwing() const noexcept;

private:
    realtime::RealtimeClock& clock_;
    midi::MidiScheduler&     scheduler_;
    midi::PanicHandler&      panic_handler_;

    uasf::StyleDefinition    style_;
    bool                     style_loaded_{false};
    ChordInput               chord_input_;
    SectionSequencer         sequencer_;

    // Absolute tick at which the current section began playing.
    int64_t section_origin_tick_{0};

    // Last relative tick (within the current section) already dispatched.
    // -1 means "nothing dispatched yet" so tick-0 events still fire.
    int64_t section_rel_cursor_{-1};

    // Main section that Intro / Fill / Break hand back to when they end
    // (the most recently played Main; -1 = first Main in the style).
    int return_main_{-1};

    // End-of-section handling (arranger-engine-spec §5). Returns false if the
    // transport stopped (Ending finished).
    bool handleSectionEnd(int64_t currentTick) noexcept;
    int  defaultMainIndex() const noexcept;
    void applyStyleGrid() noexcept;

    // Event callback for demo/debug
    PlaybackEventCallback event_cb_{nullptr};

    // Articulation render strategy. Backward-compatible default = pass-through.
    const articulation::IArticulationRenderer* renderer_{
        &articulation::defaultRenderer()};

    // Swing percentage (50 = straight). Read on the dispatch path.
    std::atomic<uint8_t> swing_percent_{music::kStraightSwing};

    // Dispatch events from the current section that are <= currentTick
    // Events at section-relative ticks >= noteOffOnlyFrom are dispatched only
    // if they are note-offs (used to close a pass exactly at its end).
    void dispatchSectionEvents(const uasf::SectionDefinition& section,
                               int64_t currentTick,
                               Chord chord,
                               int64_t noteOffOnlyFrom = INT64_MAX) noexcept;

    // Transpose a MIDI event based on chord
    uint8_t transposeNote(uint8_t note, Chord chord, uasf::TrackRole role) const noexcept;
};

// ── Demo style builder ─────────────────────────────────────────────
// Builds a minimal hardcoded style for Gate 2 testing
uasf::StyleDefinition buildDemoStyle() noexcept;

} // namespace ai_arranger::arranger
#endif // AI_ARRANGER_ARRANGER_STYLE_PLAYER_H
