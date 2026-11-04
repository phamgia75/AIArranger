#ifndef AI_ARRANGER_SESSION_ENGINE_SESSION_H
#define AI_ARRANGER_SESSION_ENGINE_SESSION_H

#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include "engine/arranger/style_player.h"
#include "engine/integration/performer_adapter.h"
#include "engine/uasf/types.h"

// ── Engine session lifecycle (Gate 14 Task B) ────────────────────────
//
// Owns the engine graph (clock, scheduler, panic, StylePlayer, performer
// adapter) and manages boot / shutdown / reset for the app shell. The UI never
// touches the engine directly — it goes through this session + the bridge.
// boot() is idempotent; shutdown() leaves zero hanging notes; reset() is a clean
// shutdown+boot. No audio is produced here.

namespace ai_arranger::session {

class EngineSession {
public:
    EngineSession() noexcept;

    // Prepare the engine and make it ready to play. Idempotent (a second boot is
    // a no-op returning true). Loads a default demo style if none is loaded.
    bool boot() noexcept;

    // Stop playback, flush all notes, mark not booted. Idempotent.
    void shutdown() noexcept;

    // Clean shutdown followed by boot.
    void reset() noexcept;

    bool isBooted() const noexcept { return booted_; }

    // False (nothing changed) while the transport is playing; see StylePlayer.
    bool loadStyle(const uasf::StyleDefinition& style) noexcept;

    // Drive one engine step (drains control events, advances the player).
    void tick() noexcept { adapter_.tick(); }

    // Accessors for the app shell / tests.
    realtime::RealtimeClock&        clock() noexcept { return clock_; }
    midi::MidiScheduler&            scheduler() noexcept { return scheduler_; }
    midi::PanicHandler&             panic() noexcept { return panic_; }
    arranger::StylePlayer&          player() noexcept { return player_; }
    const arranger::StylePlayer&    player() const noexcept { return player_; }
    integration::PerformerAdapter&  adapter() noexcept { return adapter_; }

private:
    realtime::RealtimeClock clock_;
    midi::MidiScheduler     scheduler_;
    midi::PanicHandler      panic_;
    arranger::StylePlayer   player_{clock_, scheduler_, panic_};
    integration::PerformerAdapter adapter_{clock_, player_};
    bool booted_{false};
    bool style_loaded_{false};
};

} // namespace ai_arranger::session

#endif // AI_ARRANGER_SESSION_ENGINE_SESSION_H
