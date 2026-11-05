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
//
// One line per Korg track at most. `unresolved` records that the track was
// looked at and has no verified Pa5X sound yet: nothing is emitted for it.
// Pa5X numbers are never inferred here; they come only from the user.

namespace ai_arranger::exporters::korg {

struct SoundMapResult {
    bool ok = false;
    std::string error;                    // "<name>:<line>: ..."
    std::map<uint8_t, SoundRef> sounds;   // Korg channel -> verified sound
    std::set<uint8_t> unresolved;         // Korg channels explicitly left open
};

SoundMapResult parseSoundMap(std::istream& in, const std::string& name);

// Korg track keyword ("acc1") for a Korg channel, or nullptr.
const char* soundMapKey(uint8_t korgCh) noexcept;

// A fill-in template for one converted style: every Korg track `unresolved`,
// commented with the Genos parts that feed it (voice, family, MegaVoice flag).
std::string soundMapTemplate(const Report& r, const std::string& styleName);

} // namespace ai_arranger::exporters::korg

#endif // AI_ARRANGER_EXPORTERS_KORG_SOUND_MAP_H
