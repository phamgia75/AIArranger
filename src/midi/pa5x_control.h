#ifndef AI_ARRANGER_MIDI_PA5X_CONTROL_H
#define AI_ARRANGER_MIDI_PA5X_CONTROL_H

#include "engine/uasf/types.h"
#include <cstdint>

// ── Korg Pa5X Control-channel commands (vendor data, no I/O) ────────────────
//
// Pa5X User Manual v1.4 p.613-614: Program Changes received on the Control
// channel drive the Pa5X's own arranger. Values are the raw 0-127 byte (the
// manual: "follow the 0-127 numbering system"). The Control channel itself is
// whatever the user assigned in Settings > MIDI > MIDI IN Channels; no factory
// preset value is assumed here, so the caller always passes it.
// HARDWARE VALIDATION PENDING: written from the manual, not yet seen on a Pa5X.

namespace ai_arranger::midi::pa5x {

enum class Command : uint8_t {
    KbdSet1 = 64, KbdSet2 = 65, KbdSet3 = 66, KbdSet4 = 67,
    Intro1 = 80, Intro2 = 81, Intro3CountIn = 82,
    Variation1 = 83, Variation2 = 84, Variation3 = 85, Variation4 = 86,
    Fill1 = 87, Fill2 = 88, Fill3 = 89, Fill4 = 90,
    Break = 91,
    Ending1 = 92, Ending2 = 93, Ending3 = 94,
    FadeInOut = 95, StyleToKbdSet = 96, AutoFill = 97, Memory = 98,
    BassInversion = 99, ManualBass = 100, TempoLock = 101,
    PlayStopPlayer1 = 104, PlayStopPlayer2 = 105,
    SynchroStart = 106, SynchroStop = 107,
};

// Program Change for `cmd` on `channel` (0-15, i.e. MIDI channel 1-16).
inline uasf::MidiEvent controlEvent(Command cmd, uint8_t channel) noexcept {
    return uasf::MidiEvent{uasf::MidiEventType::ProgramChange,
                           static_cast<uint8_t>(channel & 0x0F),
                           static_cast<uint8_t>(cmd), 0, 0};
}

} // namespace ai_arranger::midi::pa5x

#endif // AI_ARRANGER_MIDI_PA5X_CONTROL_H
