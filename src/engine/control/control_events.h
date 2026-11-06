#ifndef AI_ARRANGER_CONTROL_CONTROL_EVENTS_H
#define AI_ARRANGER_CONTROL_CONTROL_EVENTS_H

#include <cstdint>

// ── Control events (Gate 12 Task G) ──────────────────────────────────
//
// Vendor-neutral performer actions. Any front-end (MIDI, UI, test) emits the
// same ControlEvents; the engine reacts identically. No Korg/Yamaha button maps
// here — adapters translate device specifics into these.

namespace ai_arranger::control {

enum class ControlAction : uint8_t {
    None = 0,
    Start, Stop, SyncArm,
    Fill,
    VariationA, VariationB, VariationC, VariationD,
    Intro, Ending,
    Tap,
    Panic,
    // Chord intent from the UI: param = root MIDI note | (ChordType << 8).
    // Appended (not reordered) to keep existing values stable.
    Chord,
    // Raw held-note input from a MIDI keyboard, routed lock-free through the
    // same SPSC queue as every other control event so an async input thread
    // (e.g. the CoreMIDI read callback) never touches engine state directly.
    // NoteOn/NoteOff: param = MIDI note (0-127). Sustain: param = 1 (down)/0 (up).
    // NoteOn may carry kNoteBypassSplit: the note feeds chord detection whatever
    // the split point (a Pa5X-style Chord channel).
    // Appended (not reordered) to keep existing values stable.
    NoteOn,
    NoteOff,
    Sustain,
    // Break section (one-shot). Appended (not reordered) to keep values stable.
    Break,
};

inline constexpr int32_t kNoteBypassSplit = 0x100;   // NoteOn param flag

struct ControlEvent {
    ControlAction action = ControlAction::None;
    int32_t       param = 0;        // action-specific (e.g. fill target)
    uint64_t      timestamp = 0;    // tick or sample stamp (caller-defined)
};

} // namespace ai_arranger::control

#endif // AI_ARRANGER_CONTROL_CONTROL_EVENTS_H
