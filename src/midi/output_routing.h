#ifndef AI_ARRANGER_MIDI_OUTPUT_ROUTING_H
#define AI_ARRANGER_MIDI_OUTPUT_ROUTING_H

// ── Output routing: logical Style parts -> physical MIDI channels (no I/O) ──
//
// Two independent tables, combined into the bridge's OutputChannelMap:
//   PartLayout      engine (style) channel -> logical part. Depends on the style:
//                   sffPartLayout() for Yamaha SFF1 imports, partLayoutFromRoles()
//                   for native UASF styles (e.g. the demo).
//   RoutingProfile  logical part -> physical channel, with the evidence level of
//                   each assignment. Depends on the device and its MIDI preset.
// Vendor-neutral: device presets live in their own profile headers (pa5x_profile.h).
// The bridge maps by engine channel, so a part's Note On, Note Off and cleanup always
// leave on the same physical channel; change the map only while stopped (the facade
// refuses it while playing).

#include "control/midi_output_bridge.h"   // OutputChannelMap
#include "engine/uasf/types.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ai_arranger::midi {

enum class StylePart : uint8_t { Drum, Perc, Bass, Acc1, Acc2, Acc3, Acc4, Acc5 };
inline constexpr size_t kStyleParts = 8;
inline constexpr uint8_t kNoPart = 0xFF;
inline constexpr uint8_t kUnassigned = 0xFF;   // RoutingProfile: part not sent

// How an assignment is known. Only DeviceConfirmed means "observed on the device".
enum class RouteEvidence : uint8_t { None, Documented, OwnerPhoto, UserSet, DeviceConfirmed };

inline constexpr std::array<std::string_view, kStyleParts> kPartNames = {
    "drum", "perc", "bass", "acc1", "acc2", "acc3", "acc4", "acc5"};

struct PartLayout {
    std::array<uint8_t, 16> part_of_channel{};   // engine channel 0-15 -> StylePart or kNoPart
    PartLayout() { part_of_channel.fill(kNoPart); }
};

struct RoutingProfile {
    std::array<uint8_t, kStyleParts> channel{};          // StylePart -> physical channel 0-15 or kUnassigned
    std::array<RouteEvidence, kStyleParts> evidence{};
    RoutingProfile() { channel.fill(kUnassigned); }
};

// Yamaha SFF1 part channels 9-16 (Rhythm1, Rhythm2, Bass, Chord1, Chord2, Pad, Phrase1,
// Phrase2), same part assignment as the Korg exporter (genos_to_korg korgChannelFor):
// Rhythm1 -> Perc, Rhythm2 -> Drum ("Rhythm2 carries the main kit in every corpus style").
inline PartLayout sffPartLayout() noexcept {
    PartLayout l;
    l.part_of_channel[8] = static_cast<uint8_t>(StylePart::Perc);
    l.part_of_channel[9] = static_cast<uint8_t>(StylePart::Drum);
    l.part_of_channel[10] = static_cast<uint8_t>(StylePart::Bass);
    for (uint8_t ch = 11; ch < 16; ++ch)
        l.part_of_channel[ch] = static_cast<uint8_t>(static_cast<uint8_t>(StylePart::Acc1) + (ch - 11));
    return l;
}

// Native styles: the first track role seen on a channel decides its part. Drum, Percussion
// and Bass take their parts; every other role takes the next free Acc part in channel
// order. A channel past Acc5, or a second channel claiming Drum/Perc/Bass, gets no part.
inline PartLayout partLayoutFromRoles(const uasf::StyleDefinition& style) {
    std::array<int, 16> role{};
    role.fill(-1);
    for (const auto& sec : style.sections)
        for (const auto& t : sec.tracks)
            if (t.midi_channel < 16 && role[t.midi_channel] < 0) role[t.midi_channel] = static_cast<int>(t.role);
    PartLayout l;
    std::array<bool, kStyleParts> taken{};
    uint8_t nextAcc = static_cast<uint8_t>(StylePart::Acc1);
    for (uint8_t ch = 0; ch < 16; ++ch) {
        if (role[ch] < 0) continue;
        const auto r = static_cast<uasf::TrackRole>(role[ch]);
        uint8_t part = r == uasf::TrackRole::Drum ? static_cast<uint8_t>(StylePart::Drum)
                     : r == uasf::TrackRole::Percussion ? static_cast<uint8_t>(StylePart::Perc)
                     : r == uasf::TrackRole::Bass ? static_cast<uint8_t>(StylePart::Bass)
                     : (nextAcc < kStyleParts ? nextAcc++ : kNoPart);
        if (part == kNoPart || taken[part]) continue;
        taken[part] = true;
        l.part_of_channel[ch] = part;
    }
    return l;
}

// Engine channels without a part, and parts without a channel, are dropped. Voice select
// (Bank Select / Program Change) is dropped when asked: the style's sounds are another
// instrument's numbers.
inline control::OutputChannelMap buildChannelMap(const PartLayout& layout, const RoutingProfile& profile,
                                                 bool dropVoiceSelect) noexcept {
    control::OutputChannelMap m;
    for (uint8_t ch = 0; ch < 16; ++ch) {
        const uint8_t part = layout.part_of_channel[ch];
        m.dst[ch] = part < kStyleParts && profile.channel[part] < 16 ? profile.channel[part]
                                                                     : control::OutputChannelMap::kDrop;
    }
    m.dropVoiceSelect = dropVoiceSelect;
    return m;
}

// "drum=10,perc=11,bass=9,acc1=12,..." with 1-based channels or "off". Parts not named
// stay unassigned. Refused (nullopt): unknown or repeated part, channel outside 1-16, two
// parts on one channel (a Note Off of one part would end the other's note), empty text.
inline std::optional<RoutingProfile> parseRoutingProfile(std::string_view text) {
    RoutingProfile p;
    std::array<bool, kStyleParts> seen{};
    std::array<bool, 16> used{};
    if (text.empty()) return std::nullopt;
    while (!text.empty()) {
        const size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        const size_t eq = item.find('=');
        if (eq == std::string_view::npos) return std::nullopt;
        const std::string_view key = item.substr(0, eq), val = item.substr(eq + 1);
        size_t part = kStyleParts;
        for (size_t i = 0; i < kStyleParts; ++i) if (kPartNames[i] == key) part = i;
        if (part == kStyleParts || seen[part]) return std::nullopt;
        seen[part] = true;
        if (val == "off") continue;
        if (val.empty() || val.size() > 2 || (val.size() == 2 && val[0] == '0')) return std::nullopt;
        int ch = 0;
        for (char c : val) { if (c < '0' || c > '9') return std::nullopt; ch = ch * 10 + (c - '0'); }
        if (ch < 1 || ch > 16 || used[ch - 1]) return std::nullopt;
        used[ch - 1] = true;
        p.channel[part] = static_cast<uint8_t>(ch - 1);
        p.evidence[part] = RouteEvidence::UserSet;
    }
    return p;
}

inline std::string formatRoutingProfile(const RoutingProfile& p) {
    std::string s;
    for (size_t i = 0; i < kStyleParts; ++i) {
        if (!s.empty()) s += ',';
        s += kPartNames[i];
        s += '=';
        s += p.channel[i] < 16 ? std::to_string(p.channel[i] + 1) : std::string("off");
    }
    return s;
}

} // namespace ai_arranger::midi

#endif // AI_ARRANGER_MIDI_OUTPUT_ROUTING_H
