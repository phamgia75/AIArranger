#ifndef AI_ARRANGER_EXPORTERS_GENOS_TO_KORG_H
#define AI_ARRANGER_EXPORTERS_GENOS_TO_KORG_H

#include "engine/uasf/types.h"
#include "exporters/korg/korg_style_smf.h"

#include <map>
#include <string>
#include <vector>

// Canonical (UASF) style -> Korg Pa5X style semantic model. Input is the
// vendor-neutral UASF model, never raw Genos bytes. Every lossy decision is
// counted in the Report; nothing is dropped silently.

namespace ai_arranger::exporters::korg {

struct SoundRef { uint8_t msb = 0, lsb = 0, pc = 0; };

struct MapOptions {
    uint16_t ppqn = 0;                    // 0 = keep the source resolution
    std::map<uint8_t, uint8_t> route;     // source channel -> CASM destination channel (0-based)
    std::map<uint8_t, SoundRef> sounds;   // Korg channel (0-based) -> user-supplied Pa5X sound
};

struct SectionRow {
    std::string source;     // UASF section name ("Main A", "Fill In BA", ...)
    std::string marker;     // Korg marker, empty = not exported
    uint32_t bars = 0;
    std::string note;       // why it was not exported
};

struct TrackRow {
    uint8_t src_ch = 0;
    uint8_t korg_ch = 0xFF;   // 0xFF = not exported
    std::string name;
    uasf::TrackRole role = uasf::TrackRole::Accompaniment;
    uint8_t ntr = 0, ntt = 0;
    int msb = -1, lsb = -1, pc = -1;   // first Yamaha voice select seen
    std::string family;                // instrument family hint
    std::string target;                // Pa5X sound, or UNRESOLVED
};

struct Report {
    std::vector<SectionRow> sections;
    std::vector<TrackRow> tracks;
    std::map<std::string, uint32_t> dropped;       // reason -> event count
    std::map<uint8_t, uint32_t> non_gm_drum_notes;  // drum/perc note outside GM 35..81 -> count
    std::vector<std::string> warnings;
};

struct MapResult {
    bool ok = false;
    std::string error;
    Style style;
    Report report;
};

const char* korgTrackName(uint8_t korgCh) noexcept;
// Genos style part channel (0-based, after CASM routing) -> Korg track channel; 0xFF = none.
uint8_t korgChannelFor(uint8_t genosCh) noexcept;
// Korg marker for a canonical section, or nullptr when the Korg spec has no code for it.
const char* korgMarkerFor(uasf::SectionType t) noexcept;

MapResult mapToKorg(const uasf::StyleDefinition& style, const MapOptions& opt);

// Human-readable conversion report (section map, tracks, loss).
std::string formatReport(const Report& r, const Style& s);

// Drop-reason keys (stable strings; tests and the report use them).
inline constexpr const char* kDropSection = "section has no Korg marker code";
inline constexpr const char* kDropChannel = "channel outside the 8 Korg style tracks";
inline constexpr const char* kDropVoiceSelect = "Yamaha bank select / program change";
inline constexpr const char* kDropChordRange = "note C-1..B-1 on a chord-driven track";
inline constexpr const char* kDropLastTick = "note starts in the last tick of the section";
inline constexpr const char* kDropPastEnd = "event at or after the section end";
inline constexpr const char* kDropOrphanOff = "NoteOff without a NoteOn";

} // namespace ai_arranger::exporters::korg

#endif // AI_ARRANGER_EXPORTERS_GENOS_TO_KORG_H
