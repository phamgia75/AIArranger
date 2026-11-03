#include "engine/uasf/note_closure.h"
#include <algorithm>
#include <map>
#include <vector>

namespace ai_arranger::uasf {

size_t closeOpenNotes(TrackDefinition& track, uint64_t endTick) {
    // Walk the in-section events in time order (stable: same-tick file order).
    std::vector<const MidiEvent*> evs;
    for (const auto& e : track.events)
        if (e.tick <= endTick) evs.push_back(&e);
    std::stable_sort(evs.begin(), evs.end(),
                     [](const MidiEvent* a, const MidiEvent* b) { return a->tick < b->tick; });

    std::map<int, int> open;   // (channel << 8 | note) -> NoteOns not yet closed
    for (const MidiEvent* e : evs) {
        const int key = (e->channel << 8) | e->data1;
        if (e->type == MidiEventType::NoteOn && e->data2 > 0) {
            ++open[key];
        } else if (e->type == MidiEventType::NoteOff ||
                   (e->type == MidiEventType::NoteOn && e->data2 == 0)) {
            if (open[key] > 0) --open[key];
        }
    }

    size_t added = 0;
    for (const auto& [key, n] : open) {   // std::map: deterministic order
        for (int i = 0; i < n; ++i) {
            MidiEvent off{};
            off.type = MidiEventType::NoteOff;
            off.channel = static_cast<uint8_t>(key >> 8);
            off.data1 = static_cast<uint8_t>(key & 0xFF);
            off.data2 = 0;
            off.tick = endTick;
            track.events.push_back(off);
            ++added;
        }
    }
    return added;
}

size_t closeOpenNotes(SectionDefinition& section) {
    if (section.bars == 0 || section.resolution == 0 || section.beats_per_bar == 0 ||
        section.beat_note == 0)
        return 0;
    const uint64_t bar = static_cast<uint64_t>(section.resolution) * 4 *
                         section.beats_per_bar / section.beat_note;
    if (bar == 0) return 0;
    size_t added = 0;
    for (auto& t : section.tracks) added += closeOpenNotes(t, bar * section.bars);
    return added;
}

} // namespace ai_arranger::uasf
