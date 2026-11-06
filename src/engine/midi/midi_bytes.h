#ifndef AI_ARRANGER_MIDI_MIDI_BYTES_H
#define AI_ARRANGER_MIDI_MIDI_BYTES_H

// Portable MidiEvent -> wire bytes (no platform headers). `out` holds 3 bytes.
// Returns the length, 0 for an unknown type.
#include "engine/uasf/types.h"
#include <cstddef>
#include <cstdint>

namespace ai_arranger::midi {

inline size_t encodeMidiEvent(const uasf::MidiEvent& ev, uint8_t* out) noexcept {
    const uint8_t status = static_cast<uint8_t>(ev.type) | (ev.channel & 0x0F);
    switch (ev.type) {
        case uasf::MidiEventType::NoteOff:
        case uasf::MidiEventType::NoteOn:
        case uasf::MidiEventType::PolyPressure:
        case uasf::MidiEventType::ControlChange:
        case uasf::MidiEventType::PitchBend:
            out[0] = status;
            out[1] = ev.data1 & 0x7F;
            out[2] = ev.data2 & 0x7F;
            return 3;
        case uasf::MidiEventType::ProgramChange:
        case uasf::MidiEventType::ChannelPressure:
            out[0] = status;
            out[1] = ev.data1 & 0x7F;
            return 2;
        case uasf::MidiEventType::TimingClock:
        case uasf::MidiEventType::Start:
        case uasf::MidiEventType::Stop:
            out[0] = static_cast<uint8_t>(ev.type);
            return 1;
        default:
            return 0;
    }
}

} // namespace ai_arranger::midi

#endif
