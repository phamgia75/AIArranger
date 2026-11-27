#ifndef AI_ARRANGER_EXPORTERS_KORG_SFF1_ROUTE_H
#define AI_ARRANGER_EXPORTERS_KORG_SFF1_ROUTE_H

#include "engine/uasf/types.h"
#include "importers/sff1/sff1_types.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ai_arranger::exporters::korg {

// CASM source -> destination channel routing (Ctb2 byte 9). A Genos part on a
// non-part channel (POP_ACOUSTIC_2 "E.P  rt": ch 4 -> 15) plays through its
// destination part, so it must land on that part's Korg track. First config
// per source channel wins, as in Sff1ToUasfMapper.
inline std::map<uint8_t, uint8_t> routeFromCasm(const importers::sff1::ParseResult& p) {
    std::map<uint8_t, uint8_t> route;
    for (const auto& c : p.casm_configs)
        if (c.source_channel <= 15 && c.dest_channel <= 15) route.emplace(c.source_channel, c.dest_channel);
    return route;
}

// Extract per-section FF 51 Set Tempo maps (relative source ticks -> mpqn)
// aligned with Sff1ToUasfMapper's marker ranges for opt-in --bake-rubato (P1-W3).
inline std::map<uasf::SectionType, std::vector<std::pair<uint32_t, uint32_t>>>
sectionTemposFromSff1(const importers::sff1::ParseResult& p) {
    std::map<uasf::SectionType, std::vector<std::pair<uint32_t, uint32_t>>> out;
    if (p.tempos.size() < 2) return out;

    using T = uasf::SectionType;
    auto markerToType = [](const std::string& m, T& t) -> bool {
        static const struct { const char* name; T type; } kMap[] = {
            {"Intro A", T::Intro1}, {"Intro B", T::Intro2}, {"Intro C", T::Intro3},
            {"Main A", T::Main1}, {"Main B", T::Main2}, {"Main C", T::Main3}, {"Main D", T::Main4},
            {"Fill In AA", T::Fill1}, {"Fill In BB", T::Fill2},
            {"Fill In CC", T::Fill3}, {"Fill In DD", T::Fill4},
            {"Fill In BA", T::Break},
            {"Ending A", T::Ending1}, {"Ending B", T::Ending2}, {"Ending C", T::Ending3},
        };
        for (const auto& e : kMap) if (m == e.name) { t = e.type; return true; }
        return false;
    };

    uint32_t lastTick = p.end_of_track_tick;
    for (const auto& sec : p.sections) {
        for (const auto& tr : sec.tracks) {
            for (const auto& ev : tr.events) lastTick = std::max(lastTick, ev.tick);
        }
    }
    for (const auto& tm : p.tempos) lastTick = std::max(lastTick, tm.tick);

    auto tempos = p.tempos;
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const importers::sff1::SmfTempo& a, const importers::sff1::SmfTempo& b) {
                         return a.tick < b.tick;
                     });

    std::set<T> seen;
    for (size_t i = 0; i < p.markers.size(); ++i) {
        T st{};
        if (!markerToType(p.markers[i].name, st)) continue;
        uint32_t end = lastTick;
        for (size_t j = i + 1; j < p.markers.size(); ++j) {
            if (p.markers[j].tick > p.markers[i].tick) { end = p.markers[j].tick; break; }
        }
        const uint32_t start = p.markers[i].tick;
        if (end <= start) continue;
        if (!seen.insert(st).second) continue;

        uint32_t activeAtStart = tempos.front().mpqn;
        for (const auto& tm : tempos) {
            if (tm.tick <= start && tm.mpqn > 0) activeAtStart = tm.mpqn;
        }
        std::vector<std::pair<uint32_t, uint32_t>> secList;
        secList.emplace_back(0u, activeAtStart);
        for (const auto& tm : tempos) {
            if (tm.tick > start && tm.tick < end && tm.mpqn > 0) {
                secList.emplace_back(tm.tick - start, tm.mpqn);
            }
        }
        if (secList.size() >= 2) {
            out.emplace(st, std::move(secList));
        }
    }
    return out;
}

} // namespace ai_arranger::exporters::korg

#endif // AI_ARRANGER_EXPORTERS_KORG_SFF1_ROUTE_H
