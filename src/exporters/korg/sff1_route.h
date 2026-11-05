#ifndef AI_ARRANGER_EXPORTERS_KORG_SFF1_ROUTE_H
#define AI_ARRANGER_EXPORTERS_KORG_SFF1_ROUTE_H

#include "importers/sff1/sff1_types.h"
#include <cstdint>
#include <map>

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

} // namespace ai_arranger::exporters::korg

#endif // AI_ARRANGER_EXPORTERS_KORG_SFF1_ROUTE_H
