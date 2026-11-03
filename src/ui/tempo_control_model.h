#ifndef AI_ARRANGER_UI_TEMPO_CONTROL_MODEL_H
#define AI_ARRANGER_UI_TEMPO_CONTROL_MODEL_H

#include "engine/uasf/types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

// ── Tempo control model (Issue #7) ───────────────────────────────────
//
// UI-toolkit-agnostic state behind a tempo slider. The engine's tempo is the
// truth (a style load or the performer changes it); the slider shows it, and a
// user change is sent once and shown at once.
//
//  - syncFromEngine(bpm) on every UI poll: returns true when the slider should
//    be updated WITHOUT notifying (so syncing never sends a tempo back).
//  - userSet(value) from the slider's value-changed callback: clamps to the
//    engine range, sends it through the sink, and holds it on screen until the
//    engine reports it (setTempo applies on the engine's next tick) — at most
//    kHoldPolls polls, so a value the engine never adopts does not stick.

namespace ai_arranger::ui {

class TempoControlModel {
public:
    static constexpr uint32_t kMin = uasf::kMinTempoBpm;
    static constexpr uint32_t kMax = uasf::kMaxTempoBpm;
    static constexpr int kHoldPolls = 10;   // ~330 ms at the 30 Hz UI poll

    void setTempoSink(std::function<void(uint32_t)> sink) { sink_ = std::move(sink); }

    bool syncFromEngine(uint32_t engineBpm) {
        const uint32_t shown = clamp(engineBpm);
        if (hold_ > 0) {
            if (shown == displayed_) hold_ = 0;           // engine caught up
            else if (--hold_ > 0) return false;           // still applying: keep the user's value
        }
        if (shown == displayed_) return false;
        displayed_ = shown;
        return true;
    }

    void userSet(double value) {
        const uint32_t bpm = clamp(static_cast<uint32_t>(std::lround(std::max(0.0, value))));
        displayed_ = bpm;
        hold_ = kHoldPolls;
        if (sink_) sink_(bpm);
    }

    uint32_t displayed() const { return displayed_; }

private:
    static uint32_t clamp(uint32_t v) { return std::clamp(v, kMin, kMax); }

    std::function<void(uint32_t)> sink_;
    uint32_t displayed_ = 0;   // 0 = nothing shown yet
    int hold_ = 0;
};

} // namespace ai_arranger::ui

#endif // AI_ARRANGER_UI_TEMPO_CONTROL_MODEL_H
