#ifndef AI_ARRANGER_EXPORTERS_KORG_SOUND_MAP_H
#define AI_ARRANGER_EXPORTERS_KORG_SOUND_MAP_H

#include "exporters/korg/genos_to_korg.h"

#include <istream>
#include <set>
#include <string>

// User-supplied Pa5X sound assignments (`aiarranger-convert --sound-map`).
//
//   # comment
//   <bass|drum|perc|acc1..acc5> <msb 0-127> <lsb 0-127> <pc 0-127>
//   <bass|drum|perc|acc1..acc5> unresolved
//   <section>.<track> <msb 0-127> <lsb 0-127> <pc 0-127>
//   <section>.<track> unresolved
//
// Supported <section> prefixes: v1..v4 (main_a..main_d), i1..i3 (intro_a..intro_c),
// f1..f4 (fill_aa..fill_dd), e1..e3 (ending_a..ending_c), br (break).
// One line per track or (section, track) at most. `unresolved` records that the
// track was looked at and has no verified Pa5X sound yet: nothing is emitted for it.
// Pa5X numbers are never inferred here; they come only from the user.

namespace ai_arranger::exporters::korg {

struct SoundMapResult {
    bool ok = false;
    std::string error;                    // "<name>:<line>: ..."
    std::map<uint8_t, SoundRef> sounds;   // Korg channel -> verified sound
    std::set<uint8_t> unresolved;         // Korg channels explicitly left open
    std::map<std::pair<uasf::SectionType, uint8_t>, SoundRef> element_sounds; // (section, Korg ch) override
    std::set<std::pair<uasf::SectionType, uint8_t>> element_unresolved;       // (section, Korg ch) left open
};

SoundMapResult parseSoundMap(std::istream& in, const std::string& name);

// Korg track keyword ("acc1") for a Korg channel, or nullptr.
const char* soundMapKey(uint8_t korgCh) noexcept;

// Canonical per-element section prefix ("v1".."v4", "i1".."i3", "f1".."f4", "e1".."e3", "br"), or nullptr.
const char* soundMapSectionKey(uasf::SectionType section) noexcept;

// Parse a per-element section prefix (e.g. "v2", "v2cv1", "main_b", "br", "break").
bool parseSoundMapSectionKey(const std::string& token, uasf::SectionType& out) noexcept;

// A fill-in template for one converted style: every Korg track `unresolved`,
// commented with the Genos parts that feed it (voice, family, MegaVoice flag).
std::string soundMapTemplate(const Report& r, const std::string& styleName);

} // namespace ai_arranger::exporters::korg

#endif // AI_ARRANGER_EXPORTERS_KORG_SOUND_MAP_H
