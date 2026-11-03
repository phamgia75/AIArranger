#ifndef AI_ARRANGER_UASF_NOTE_CLOSURE_H
#define AI_ARRANGER_UASF_NOTE_CLOSURE_H

#include "engine/uasf/types.h"
#include <cstddef>
#include <cstdint>

namespace ai_arranger::uasf {

// Close every note a section cannot close by itself (Issue #3).
//
// The player dispatches a section's events in (0, length] per pass and never
// dispatches an event past the section end. A NoteOn whose NoteOff lies past
// `endTick` (or that has none) therefore stays sounding: a looping Main never
// re-closes it, and the dispatcher suppresses its later NoteOns as duplicates of
// an active note — one held note for as long as the Main loops.
//
// Adds one NoteOff at exactly `endTick` for each such NoteOn (same channel and
// note); the player dispatches a NoteOff at the section end as the close of the
// pass. Notes ending inside the section or exactly on `endTick` are untouched,
// events past `endTick` are left as they are (they are never dispatched), and a
// second call adds nothing. Returns the number of NoteOffs added.
size_t closeOpenNotes(TrackDefinition& track, uint64_t endTick);

// closeOpenNotes() on every track of `section`, with the section's own length
// (bars x its bar). Sections of unknown length (bars or grid 0) are skipped.
size_t closeOpenNotes(SectionDefinition& section);

} // namespace ai_arranger::uasf

#endif // AI_ARRANGER_UASF_NOTE_CLOSURE_H
