// The three W6 hardware-gate test KSTs (one Pad, one Keyboard Set, one Style). NOT HARDWARE-VERIFIED.
// Factory/GM sounds only (MSB 121 LSB 0 = GM2 melodic, MSB 120 LSB 0 = GM2 drum kits; program numbers per GM), no
// user Sounds, no samples (no PCM, so a Load cannot touch User Samples in memory beyond what MAN5 p.736 says a KST
// load does). All names are invented here.
#ifndef AI_ARRANGER_MEDIA_PANATIVE_KST_DEMO_H
#define AI_ARRANGER_MEDIA_PANATIVE_KST_DEMO_H

#include "media/panative/kst_assemble.h"
#include "media/panative/kst_builders.h"

namespace ai_arranger::media::panative::demo {

inline std::vector<build::Note> drumBar() {   // kick 36, snare 38, closed hat 42 (GM drum map)
    std::vector<build::Note> n;
    for (uint32_t i = 0; i < 8; ++i) n.push_back({i * 192, 96, 42, 80});
    n.push_back({0, 96, 36, 110}); n.push_back({768, 96, 36, 110});
    n.push_back({384, 96, 38, 100}); n.push_back({1152, 96, 38, 100});
    return n;
}
inline std::vector<build::Note> chord(uint8_t root) {
    return {{0, 1536, root, 90}, {0, 1536, uint8_t(root + 4), 90}, {0, 1536, uint8_t(root + 7), 90}};
}

struct Kst { std::string name; std::vector<assemble::Object> objects; };

inline std::vector<Kst> kits() {
    using build::SoundSelect;
    const std::array<SoundSelect, 4> parts = {SoundSelect{121, 0, 48}, SoundSelect{121, 0, 0}, SoundSelect{121, 0, 48}, SoundSelect{121, 0, 89}};
    build::StyleSpec s;
    s.name = "AIA Test Beat";
    s.drum = {120, 0, 0};
    s.drumPattern = drumBar();
    s.kbdSetNames = {"AIA Set 1", "AIA Set 2", "AIA Set 3", "AIA Set 4"};
    s.kbdSets = {parts, parts, parts, parts};
    s.pads = {build::StylePad{"AIA Pad 1", {121, 0, 0}, chord(60)}, build::StylePad{"AIA Pad 2", {121, 0, 0}, chord(65)},
              build::StylePad{"AIA Pad 3", {121, 0, 0}, chord(67)}, build::StylePad{"AIA Pad 4", {121, 0, 0}, chord(57)}};
    return {
        {"AIA_PAD_TEST", {{"PAD", "AIA", "AIA Pad Test", build::padClipTree({121, 0, 0}, chord(60))}}},
        {"AIA_KBD_TEST", {{"KEYBOARDSET", "AIA", "AIA Kbd Test", build::keyboardSetTree(parts)}}},
        {"AIA_STY_TEST", {{"STYLE", "AIA", "AIA Test Beat", build::styleTree(s)}}},
    };
}

}  // namespace ai_arranger::media::panative::demo

#endif
