#ifndef AI_ARRANGER_MIDI_MIDI_INPUT_ROUTER_H
#define AI_ARRANGER_MIDI_MIDI_INPUT_ROUTER_H

#include "midi/midi_input_parser.h"
#include "engine/control/control_events.h"
#include <cstddef>
#include <cstdint>

// ── MIDI input router (vendor-neutral) ───────────────────────────────────────
//
// Translates parsed MIDI input into ControlEvents posted to a lock-free sink
// (typically PerformerAdapter::postEvent). Only chord-relevant messages are
// forwarded:
//   NoteOn / NoteOff -> held-note input  (ControlAction::NoteOn / NoteOff)
//   CC64  (sustain)  -> ControlAction::Sustain (param = down/up)
//   CC123 (all off)  -> ControlAction::Panic
// Header-only and templated on the sink so it is testable without the engine and
// callable from a real-time read callback (postEvent is lock-free / non-blocking).

namespace ai_arranger::midi {

inline constexpr uint8_t kCcSustain     = 64;   // CC64  — sustain pedal (hold)
inline constexpr uint8_t kCcAllNotesOff = 123;  // CC123 — all notes off (panic)

// What each input MIDI channel means to chord recognition (modelled on the Pa5X
// MIDI IN channel types, User Manual v1.4 p.571-572):
//   Global - behaves like the local keyboard: notes go through the split.
//   Chord  - notes always feed chord detection, ignoring the split.
//   Ignore - nothing from this channel is routed.
enum class ChannelRole : uint8_t { Global = 0, Chord = 1, Ignore = 2 };

// Default: every channel Global, i.e. the behaviour before channel roles existed.
struct ChannelRoles {
    ChannelRole role[16] = {};
    ChannelRole of(uint8_t ch) const noexcept { return role[ch & 0x0F]; }
};

// Route a single parsed message. `Sink` must expose
// `bool postEvent(const control::ControlEvent&)`.
template <class Sink>
inline void routeMidiInput(const MidiInputMessage& m, Sink& sink,
                           const ChannelRoles& roles = {}) {
    using control::ControlAction;
    using control::ControlEvent;
    const ChannelRole role = roles.of(m.channel);
    if (role == ChannelRole::Ignore) return;
    switch (m.type) {
        case MidiInMsgType::NoteOn:
            sink.postEvent(ControlEvent{ControlAction::NoteOn,
                m.data1 | (role == ChannelRole::Chord ? control::kNoteBypassSplit : 0), 0});
            break;
        case MidiInMsgType::NoteOff:
            sink.postEvent(ControlEvent{ControlAction::NoteOff, m.data1, 0});
            break;
        case MidiInMsgType::ControlChange:
            if (m.data1 == kCcSustain)
                sink.postEvent(ControlEvent{ControlAction::Sustain, m.data2 >= 64 ? 1 : 0, 0});
            else if (m.data1 == kCcAllNotesOff)
                sink.postEvent(ControlEvent{ControlAction::Panic, 0, 0});
            break;
        case MidiInMsgType::Other:
            break; // PC / pressure / pitch bend — not chord-relevant
    }
}

// Convenience: parse a raw MIDI byte buffer and route every message.
// Returns the number of parsed messages.
template <class Sink>
inline size_t routeMidiBytes(const uint8_t* bytes, size_t len, Sink& sink,
                             const ChannelRoles& roles = {}) {
    return forEachMidiInput(bytes, len, [&](const MidiInputMessage& m) { routeMidiInput(m, sink, roles); });
}

} // namespace ai_arranger::midi

#endif // AI_ARRANGER_MIDI_MIDI_INPUT_ROUTER_H
