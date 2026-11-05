#ifndef AI_ARRANGER_EXPORTERS_KORG_STYLE_SMF_H
#define AI_ARRANGER_EXPORTERS_KORG_STYLE_SMF_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Korg Pa "Style as SMF separated by Markers": the documented interchange
// artifact a Pa5X loads in Style Edit > Menu > Import/Export > Import SMF.
// Format evidence (public Korg documents, see docs/exporters/pa5x-format-evidence.md):
//   - "Importing/exporting Styles as SMF separated by Markers (Pa2X/Pa800)"
//   - "Pa5X Operating System Version 1.2 - New Features", pp.39-46
// The native Pa5X container (KST folder) is undocumented and is NOT written.

namespace ai_arranger::exporters::korg {

// Korg standard Style track channels (0-based; SMF channels 9..16).
inline constexpr uint8_t kBassCh = 8;
inline constexpr uint8_t kDrumCh = 9;
inline constexpr uint8_t kPercCh = 10;
inline constexpr uint8_t kAcc1Ch = 11;   // Acc1..Acc5 = 11..15

// Bass and Acc1..5 follow the chords; Drum and Percussion do not.
inline bool isChordDriven(uint8_t ch) noexcept { return ch == kBassCh || ch >= kAcc1Ch; }

// Notes C-1..B-1 (0..11) on a chord-driven track are imported by Pa5X as Chord
// events, not as notes (Pa5X OS 1.2 New Features p.46).
inline constexpr uint8_t kChordEventNoteMax = 11;

struct Event {
    uint32_t tick = 0;    // element-relative
    uint8_t status = 0;   // channel voice message (0x80..0xEF)
    uint8_t d1 = 0;
    uint8_t d2 = 0;       // unused for 0xC0 / 0xD0
};

struct Element {
    std::string marker;          // "EnCVn", e.g. "v1cv1", "i2cv1"
    uint8_t num = 4;             // time signature, mandatory at tick 0
    uint8_t den = 4;             // 0 = missing (parser)
    uint32_t length = 0;         // ticks, whole bars
    std::vector<Event> events;   // sorted by tick
};

struct Style {
    uint16_t ppqn = 0;
    uint32_t mpqn = 0;           // microseconds per quarter; 0 = no tempo event
    std::vector<Element> elements;
};

bool isValidMarker(std::string_view m) noexcept;

// Every rule the Korg import documents, plus SMF sanity. Empty = valid.
std::vector<std::string> validate(const Style& s);

struct WriteResult {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> bytes;
};
// SMF format 0, one MTrk. Refuses a style validate() rejects. Deterministic.
WriteResult write(const Style& s);

struct ParseResult {
    bool ok = false;
    std::string error;
    Style style;
    std::vector<std::string> notes;   // skipped chunks / SysEx / stray events
};
// Independent reader (does not share code with write) used to verify output.
ParseResult parse(const std::vector<uint8_t>& bytes);

} // namespace ai_arranger::exporters::korg

#endif // AI_ARRANGER_EXPORTERS_KORG_STYLE_SMF_H
