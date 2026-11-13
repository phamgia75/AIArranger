#ifndef AI_ARRANGER_STYLE_EDIT_STYLE_EDITOR_H
#define AI_ARRANGER_STYLE_EDIT_STYLE_EDITOR_H

#include "engine/uasf/types.h"
#include "engine/uasf/validator.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/korg_style_smf.h"
#include "style_edit/undo_history.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Style Studio editing core (Module 5): a non-real-time, undoable edit layer over
// the UASF style model. Never used on the audio/MIDI thread; an edited style
// reaches the player only through the normal style load path.
//
// Addressing: element = UASF section index, track = index in that section,
// event = index in TrackDefinition::events (kept sorted by tick). A note is its
// NoteOn plus the NoteOff that closes it (FIFO per channel and pitch): deleting,
// moving, copying, quantizing or transposing one end carries the other.
//
// Every op is a transaction: it edits a copy, the copy must pass the op's own
// checks and the UASF validator (no ERROR issue), and only then replaces the
// style and becomes one undo step. A rejected op changes nothing.

namespace ai_arranger::style_edit {

struct Selection {
    std::optional<size_t> section;   // nullopt = every section
    std::optional<size_t> track;     // nullopt = every track of the selected section(s)
    uint64_t from = 0;               // note-on / event tick range [from, to)
    uint64_t to = UINT64_MAX;
};

struct EditResult {
    bool ok = false;
    std::string error;                            // why it was rejected
    std::vector<uasf::ValidationIssue> issues;    // validator output for the edited style
    size_t changed = 0;                           // events changed (op specific)
    size_t skipped_tracks = 0;                    // transpose: drum/percussion tracks left alone
};

enum class VelocityMode { Set, Offset, ScalePercent };

// Events copied from one element, ticks relative to the copied range start.
struct Clip {
    uint32_t resolution = 0;
    std::vector<uasf::MidiEvent> events;
};

// Grid in ticks for "1/4", "1/8", "1/16", "1/32" (and the triplet forms with a
// trailing 't'). 0 = not a grid this parser knows or not a whole tick count.
uint32_t gridTicks(uint32_t resolution, const std::string& grid);

// Section length in ticks (bars x bar ticks); 0 when the meter is unset.
uint64_t sectionLengthTicks(const uasf::SectionDefinition& s);

// "" when UASF can store the Style exactly, else why not ("cannot be kept as UASF: ..."): the serializer's own refusals
// (tempo 20-400 BPM, bars/resolution <= 65535, ...) and the counts it would silently cut (32 elements, 16 tracks).
// Every edit, the Korg SMF import and every Studio open are held to it.
std::string saveError(const uasf::StyleDefinition& s);

// Pa Style element names ("Intro 1".."Intro 3", "Variation 1".."Variation 4", "Fill 1".."Fill 4", "Break",
// "Ending 1".."Ending 3"); the parser also takes "Main n" and ignores case and spaces.
const char* sectionTypeName(uasf::SectionType t) noexcept;
std::optional<uasf::SectionType> sectionTypeFromName(const std::string& name);
// UASF track role names, lower case ("drum", "percussion", "bass", "accompaniment", "chord", "pad", ...).
const char* trackRoleName(uasf::TrackRole r) noexcept;
std::optional<uasf::TrackRole> trackRoleFromName(const std::string& name);

class StyleEditor {
public:
    explicit StyleEditor(uasf::StyleDefinition style, size_t undoLimit = 64);

    const uasf::StyleDefinition& style() const noexcept { return style_; }

    // Note starts in the selection move toward the grid by strength% (0..100);
    // each NoteOff moves with its NoteOn (length kept). A start rounding onto the
    // element end goes to the previous grid line instead.
    EditResult quantize(const Selection& sel, uint32_t gridTicks, uint32_t strengthPercent);
    // Pitches of selected notes (and their poly pressure). Tracks with role
    // Drum/Percussion or is_drum are skipped. A result outside 0..127 rejects the op.
    EditResult transpose(const Selection& sel, int semitones);
    // Velocity of selected NoteOns, clamped to 1..127 (never 0, which is a NoteOff).
    EditResult velocity(const Selection& sel, VelocityMode mode, int value);
    EditResult setTempo(uint32_t bpm);
    // Bars are kept, so the element length changes; refused if events would end
    // up at or past the new end. Denominator must be a power of two (1..32).
    EditResult setMeter(size_t section, uint32_t num, uint32_t den);

    // Channel is forced to the track's. A NoteOn needs noteLength > 0 and gets
    // its NoteOff at tick + noteLength (clamped to the element end).
    EditResult insertEvent(size_t section, size_t track, uasf::MidiEvent ev, uint64_t noteLength = 0);
    EditResult deleteEvents(size_t section, size_t track, const std::vector<size_t>& indices);
    EditResult moveEvents(size_t section, size_t track, const std::vector<size_t>& indices, int64_t deltaTicks);

    // Copy notes starting (and other events) in [from, to); not an edit.
    std::optional<Clip> copy(size_t section, size_t track, uint64_t from, uint64_t to, std::string* error = nullptr) const;
    // Merge a clip at `at` (rescaled to the target resolution, channel = target track's).
    EditResult paste(size_t section, size_t track, uint64_t at, const Clip& clip);
    // Replace a track's events (SMF import uses this). Events are sorted; notes
    // left sounding are closed at the element end.
    EditResult replaceTrackEvents(size_t section, size_t track, std::vector<uasf::MidiEvent> events,
                                  const std::string& label);

    // ── Element (section) and track edits, Pa Style Edit element set ──
    // Copy element `from` over the element of type `to`, or append it as a new element named
    // sectionTypeName(to) when the Style has none (Pa "Copy"). The target keeps its type and name.
    EditResult copySection(size_t from, uasf::SectionType to);
    // Remove every event of the element; its length and meter stay.
    EditResult clearSection(size_t section);
    EditResult renameSection(size_t section, const std::string& name);
    // Length in whole bars (1..256); refused if events would end up at or past the new end.
    EditResult setSectionBars(size_t section, uint32_t bars);
    // Role of every track on `channel` in every element (style-wide, like a Pa track);
    // Drum/Percussion also set is_drum, any other role clears it.
    EditResult setChannelRole(uint8_t channel, uasf::TrackRole role);
    // Pa5X Style SMF export settings, kept in the Style (UASF 'KEXP'), so they undo and save like any edit.
    // Part: source channel -> the part channel it plays (the CASM route; korgChannelFor gives the Korg track).
    EditResult setPart(uint8_t sourceChannel, uint8_t partChannel);
    // Sound of a Korg track (8-15): in every element, or in `element` only; nullopt sound = unresolved (removed).
    EditResult setSound(uint8_t korgChannel, std::optional<uasf::KorgSound> sound, std::optional<uasf::SectionType> element);

    bool undo() { return history_.undo(style_); }
    bool redo() { return history_.redo(style_); }
    const UndoHistory<uasf::StyleDefinition>& history() const noexcept { return history_; }

private:
    using Mutator = std::function<std::string(uasf::StyleDefinition&, EditResult&)>;
    EditResult apply(const std::string& label, const Mutator& m);

    uasf::StyleDefinition style_;
    UndoHistory<uasf::StyleDefinition> history_;
};

// ── Korg element model: Chord Variations ──────────────────────────────
// UASF sections carry one chord variation (exported as cv1); Chord Variations
// exist only in the Korg Pa element model (marker "EnCVn"). This editor works
// on a parsed or exported Korg style, validated by the Korg exporter's validate().
class KorgStyleEditor {
public:
    explicit KorgStyleEditor(exporters::korg::Style style, size_t undoLimit = 64);
    const exporters::korg::Style& style() const noexcept { return style_; }

    // Copy element `from` (e.g. "v1cv1") to `to` (e.g. "v1cv2"), replacing `to`
    // if it exists, else appending it. Pa Style Edit "Copy" semantics.
    EditResult copyChordVariation(const std::string& from, const std::string& to);

    bool undo() { return history_.undo(style_); }
    bool redo() { return history_.redo(style_); }
    const UndoHistory<exporters::korg::Style>& history() const noexcept { return history_; }

private:
    exporters::korg::Style style_;
    UndoHistory<exporters::korg::Style> history_;
};

// ── Plain SMF import / Pa5X SMF export ────────────────────────────────

struct SmfEvent {
    uint64_t tick = 0;     // absolute, at the file's PPQN
    uint8_t status = 0;    // channel voice message 0x80..0xEF
    uint8_t d1 = 0, d2 = 0;
};
struct SmfFile {
    bool ok = false;
    std::string error;
    uint16_t ppqn = 0;
    std::vector<std::vector<SmfEvent>> tracks;   // channel voice events per MTrk
};
// Format 0 or 1, PPQN time base. Meta and SysEx events are skipped.
SmfFile readSmf(const std::vector<uint8_t>& bytes);

// Events of SMF track `smfTrack` (only `channel` 0..15 if given) as UASF events
// at `resolution`; events at or past `lengthTicks` are dropped (counted).
std::vector<uasf::MidiEvent> smfTrackToUasf(const SmfFile& smf, size_t smfTrack, std::optional<uint8_t> channel,
                                            uint32_t resolution, uint64_t lengthTicks, size_t* dropped = nullptr);

struct SmfExport {
    bool ok = false;
    std::string error;
    exporters::korg::Style korg;    // what was written
    std::vector<uint8_t> bytes;     // Pa5X "Style as SMF separated by markers"
    std::string report;             // mapper report
};
// UASF -> Korg mapper -> writer -> independent reader -> validator (as aiarranger-convert).
// The Style's korg_export settings (route, sounds, element sounds) drive the mapper.
SmfExport exportPa5xSmf(const uasf::StyleDefinition& style, uint16_t ppqn = 0);

// Korg Pa "Style as SMF separated by markers" -> UASF, the inverse of the exporter's element and
// track layout: marker EnCV1 -> section type, Korg track channel -> the source channel the
// exporter maps onto it (so re-export gives the same tracks). Bank/program (CC0/CC32/PC, only at
// element start in a valid file) -> style.korg_export: a track with the same sound in every element
// gets it style-wide, else each element keeps its own (re-export writes exactly those). Chord
// Variations other than cv1 and unknown markers are not imported (UASF holds one per element) and
// listed in `notes`. Refuses a file the Korg validator rejects.
// A selection that cannot be kept exactly (an incomplete CC0/CC32/PC triple, or a second one for
// the same track in one element) is a loss: SoundLoss::Refuse refuses the file and names it;
// KeepFirst (the user's explicit choice) keeps the first complete selection and lists every loss.
enum class SoundLoss { Refuse, KeepFirst };
struct KorgSmfImport {
    bool ok = false;
    std::string error;
    uasf::StyleDefinition style;
    std::vector<std::string> notes;
    std::vector<std::string> losses;   // bank/program not kept (empty unless KeepFirst)
};
KorgSmfImport importKorgSmf(const std::vector<uint8_t>& bytes, const std::string& name, SoundLoss policy = SoundLoss::Refuse);

} // namespace ai_arranger::style_edit

#endif // AI_ARRANGER_STYLE_EDIT_STYLE_EDITOR_H
