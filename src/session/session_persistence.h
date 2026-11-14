#ifndef AI_ARRANGER_SESSION_SESSION_PERSISTENCE_H
#define AI_ARRANGER_SESSION_SESSION_PERSISTENCE_H

#include <cstdint>
#include <string>

// ── Performer session persistence (Gate 15 Task G) ───────────────────
//
// Superset of the Gate 14 snapshot, persisted to a JSON file on disk: style,
// variation, tempo, split, groove profile, MIDI routing (device name), UI
// layout, and theme. Validated on load (version + ranges); a corrupt/missing
// file yields false (caller falls back to defaults). No audio.

namespace ai_arranger::session {

struct PerformerSession {
    static constexpr uint16_t kVersion = 1;
    uint16_t version = kVersion;

    // Core performer state.
    std::string style_name;
    uint8_t  variation = 0;          // 0..3
    uint32_t tempo_bpm = 120;        // 20..400
    uint8_t  split_point = 60;       // 0..127
    bool     manual_bass = false;
    bool     sync_armed = false;
    uint8_t  chord_scan_mode = 0;    // 0..3

    // Gate 15 additions.
    uint8_t     groove_profile = 0;  // 0..4
    std::string midi_output_name;    // routing (device name; "" = none)
    uint8_t     ui_layout = 0;       // 0=desktop, 1=stage
    uint8_t     theme = 0;           // theme index
    // Output routing (midi/output_routing.h text, e.g. "drum=10,bass=9,..."); "" = none
    // (identity output). Optional on load: files written before it load unchanged.
    std::string output_routing;
};

class SessionPersistence {
public:
    static bool isValid(const PerformerSession& s) noexcept;

    static std::string serialize(const PerformerSession& s);
    static bool deserialize(const std::string& json, PerformerSession& out);

    static bool save(const PerformerSession& s, const std::string& path);
    static bool load(const std::string& path, PerformerSession& out);
};

} // namespace ai_arranger::session

#endif // AI_ARRANGER_SESSION_SESSION_PERSISTENCE_H
