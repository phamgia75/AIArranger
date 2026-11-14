#ifndef AI_ARRANGER_MIDI_MIDI_INPUT_PARSER_H
#define AI_ARRANGER_MIDI_MIDI_INPUT_PARSER_H

#include <cstddef>
#include <cstdint>

// ── Pure MIDI input parser (symmetric to midiEventToBytes on the output side) ──
//
// Turns a buffer of raw MIDI bytes (as delivered by a CoreMIDI packet, one or
// more complete messages, possibly using running status) into typed channel-voice
// messages. No CoreMIDI dependency, no allocation, no locks — fully unit-testable
// on CI. NoteOn with velocity 0 is normalized to NoteOff (the common keyboard
// convention). System-common / SysEx / realtime bytes are ignored for the chord
// input path.

namespace ai_arranger::midi {

enum class MidiInMsgType : uint8_t {
    NoteOn,
    NoteOff,
    ControlChange,
    Other,          // program change, pressure, pitch bend, etc. (not chord-relevant)
};

struct MidiInputMessage {
    MidiInMsgType type = MidiInMsgType::Other;
    uint8_t       channel = 0;   // 0-15
    uint8_t       data1 = 0;     // note number or CC number
    uint8_t       data2 = 0;     // velocity or CC value
};

// Parse `len` bytes into up to `maxOut` messages. Returns the count written.
// Robust to running status and partial trailing bytes (an incomplete message at
// the end is dropped, not misread).
size_t parseMidiInput(const uint8_t* bytes, size_t len,
                      MidiInputMessage* out, size_t maxOut) noexcept;

// Same parse, every message handed to `fn` (no output cap: a packet carrying more
// than a fixed buffer's worth of messages loses none). Returns the count delivered.
template <class Fn>
size_t forEachMidiInput(const uint8_t* bytes, size_t len, Fn&& fn) {
    if (!bytes) return 0;
    size_t  n = 0;
    size_t  i = 0;
    uint8_t status = 0;   // running status (channel-voice only)
    auto emit = [&](MidiInputMessage m) { fn(m); ++n; };

    while (i < len) {
        const uint8_t b = bytes[i];

        if (b & 0x80) {                 // ── status byte ──
            if (b >= 0xF8) { ++i; continue; }              // realtime: ignore, keep running status
            if (b == 0xF0) {                                // SysEx: skip to EOX (0xF7)
                ++i;
                while (i < len && bytes[i] != 0xF7) ++i;
                if (i < len) ++i;                            // consume the 0xF7
                status = 0;
                continue;
            }
            if (b >= 0xF1 && b <= 0xF7) { status = 0; ++i; continue; } // system common: drop
            status = b;                                      // channel-voice status
            ++i;
            continue;
        }

        if (status == 0) { ++i; continue; }                 // stray data, no running status
        const uint8_t hi = status & 0xF0;
        const uint8_t ch = status & 0x0F;

        if (hi == 0xC0 || hi == 0xD0) {                     // 1 data byte (PC / channel pressure)
            emit({MidiInMsgType::Other, ch, b, 0});
            ++i;
            continue;
        }

        if (i + 1 >= len) break;                            // incomplete trailing message
        const uint8_t d1 = b;
        const uint8_t d2 = bytes[i + 1];
        i += 2;

        MidiInputMessage m;
        m.channel = ch; m.data1 = d1; m.data2 = d2;
        if (hi == 0x90)      m.type = (d2 == 0) ? MidiInMsgType::NoteOff : MidiInMsgType::NoteOn;
        else if (hi == 0x80) m.type = MidiInMsgType::NoteOff;
        else if (hi == 0xB0) m.type = MidiInMsgType::ControlChange;
        else                 m.type = MidiInMsgType::Other; // poly pressure / pitch bend
        emit(m);
    }
    return n;
}

} // namespace ai_arranger::midi

#endif // AI_ARRANGER_MIDI_MIDI_INPUT_PARSER_H
