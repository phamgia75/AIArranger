#ifndef AI_ARRANGER_IMPORTERS_SFF1_XG_SYSEX_PARSER_H
#define AI_ARRANGER_IMPORTERS_SFF1_XG_SYSEX_PARSER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Bounded, strictly read-only Yamaha XG / Style DSP / GM Master Volume SysEx
// payload extractor. Operates on raw SysEx payload bytes (excluding leading
// 0xF0 and trailing 0xF7). Zero JUCE/GUI dependency; 100% deterministic,
// offline, and fail-closed on malformed, truncated, oversized, or non-7-bit
// payloads.

namespace ai_arranger::importers::sff1 {

inline constexpr size_t   kMaxSysExPayloadBytes = 4096;
inline constexpr uint32_t kMaxSysExMessages     = 1024;

inline constexpr const char* kDiagXgDspSysEx = "STYLE_QUAL_XG_DSP_SYSEX";

struct XgPartMixParams {
    std::optional<uint8_t> volume;          // Multi Part 08 pp 0B (0..127)
    std::optional<uint8_t> pan;             // Multi Part 08 pp 0E (0=Rnd, 1..127, 64=C)
    std::optional<uint8_t> dry_level;       // Multi Part 08 pp 11 (0..127)
    std::optional<uint8_t> chorus_send;     // Multi Part 08 pp 12 (0..127)
    std::optional<uint8_t> reverb_send;     // Multi Part 08 pp 13 (0..127)
    std::optional<uint8_t> variation_send;  // Multi Part 08 pp 14 (0..127)
    std::optional<int8_t>  eq_low_gain_db;  // Multi Part 08 pp 72 (value - 64, clamped -12..+12 dB)
    std::optional<int8_t>  eq_high_gain_db; // Multi Part 08 pp 73 (value - 64, clamped -12..+12 dB)
    std::optional<int8_t>  eq_mid_gain_db;  // Master/Part EQ mid gain (-12..+12 dB)
};

struct XgDrumKeyOverride {
    uint8_t drum_setup = 0;                 // 0..3 (XG address high 0x30..0x33)
    uint8_t key        = 0;                 // MIDI note 0..127
    std::optional<uint8_t> level;           // param 0x01
    std::optional<uint8_t> pan;             // param 0x03
    std::optional<uint8_t> reverb_send;     // param 0x04
    std::optional<int8_t>  pitch_coarse;    // param 0x00 (value - 64)
};

struct XgSysExSummary {
    uint32_t total_sysex_seen    = 0;
    uint32_t recognized_xg_count = 0;
    uint32_t unrecognized_count  = 0;
    uint32_t dsp_sysex_count     = 0;       // 43 73 ... Yamaha Style/DSP SysEx count
    std::optional<uint16_t> reverb_type;    // 02 01 00 (MSB<<8 | LSB)
    std::optional<uint16_t> chorus_type;    // 02 01 20 (MSB<<8 | LSB)
    std::optional<uint16_t> variation_type; // 02 01 40 (MSB<<8 | LSB)
    std::optional<uint8_t>  master_volume;  // GM 7F 7F 04 01 ll mm or XG 00 00 04
    std::map<uint8_t, XgPartMixParams> parts; // part 0..15 -> extracted params
    std::vector<XgDrumKeyOverride>     drum_overrides;
    std::vector<std::string>           advisory_codes; // e.g., STYLE_QUAL_XG_DSP_SYSEX
};

namespace xg_sysex_detail {

inline void addUniqueAdvisory(std::vector<std::string>& codes, const char* code) {
    for (const auto& existing : codes) {
        if (existing == code) return;
    }
    codes.emplace_back(code);
}

inline XgDrumKeyOverride& findOrAddDrumOverride(std::vector<XgDrumKeyOverride>& list,
                                                uint8_t drum_setup,
                                                uint8_t key) {
    for (auto& ov : list) {
        if (ov.drum_setup == drum_setup && ov.key == key) return ov;
    }
    XgDrumKeyOverride fresh;
    fresh.drum_setup = drum_setup;
    fresh.key = key;
    list.push_back(fresh);
    return list.back();
}

} // namespace xg_sysex_detail

// Pure read-only parser for a single SysEx payload (excluding leading 0xF0 and
// trailing 0xF7). Rejects null/empty/oversized payloads, payloads containing
// non-7-bit bytes (>= 0x80), and truncated parameter messages fail-closed.
// Stops processing once out.total_sysex_seen reaches kMaxSysExMessages.
inline bool parseXgSysExMessage(const uint8_t* payload,
                                size_t len,
                                XgSysExSummary& out) noexcept {
    if (out.total_sysex_seen >= kMaxSysExMessages) {
        return false;
    }
    ++out.total_sysex_seen;

    if (payload == nullptr || len == 0 || len > kMaxSysExPayloadBytes) {
        ++out.unrecognized_count;
        return false;
    }

    // Fail-closed 7-bit data hygiene check across the entire payload.
    for (size_t i = 0; i < len; ++i) {
        if (payload[i] >= 0x80u) {
            ++out.unrecognized_count;
            return false;
        }
    }

    try {
        // 1. GM Universal Realtime Master Volume: 7F 7F 04 01 ll mm
        if (len == 6 &&
            payload[0] == 0x7Fu &&
            payload[1] == 0x7Fu &&
            payload[2] == 0x04u &&
            payload[3] == 0x01u) {
            out.master_volume = std::min<uint8_t>(payload[5], 127u);
            ++out.recognized_xg_count;
            return true;
        }

        // 2. Yamaha Style / DSP SysEx: 43 73 ...
        if (len >= 3 && payload[0] == 0x43u && payload[1] == 0x73u) {
            ++out.recognized_xg_count;
            ++out.dsp_sysex_count;
            xg_sysex_detail::addUniqueAdvisory(out.advisory_codes, kDiagXgDspSysEx);
            return true;
        }

        // 3. Yamaha XG Parameter Change: 43 1n 4C hh mm ll dd...
        if (len >= 7 &&
            payload[0] == 0x43u &&
            (payload[1] & 0xF0u) == 0x10u &&
            payload[2] == 0x4Cu) {
            const uint8_t hh = payload[3];
            const uint8_t mm = payload[4];
            const uint8_t ll = payload[5];
            const uint8_t dd = payload[6];

            // 3a. XG Master Volume: 00 00 04 dd
            if (hh == 0x00u && mm == 0x00u && ll == 0x04u) {
                out.master_volume = std::min<uint8_t>(dd, 127u);
                ++out.recognized_xg_count;
                return true;
            }

            // 3b. XG Effect Types (Reverb / Chorus / Variation): 02 01 {00|20|40} msb lsb
            if (hh == 0x02u && mm == 0x01u &&
                (ll == 0x00u || ll == 0x20u || ll == 0x40u)) {
                if (len < 8) {
                    ++out.unrecognized_count;
                    return false;
                }
                const uint16_t eff_type = static_cast<uint16_t>(
                    (static_cast<uint16_t>(payload[6]) << 8) |
                    static_cast<uint16_t>(payload[7]));
                if (ll == 0x00u) {
                    out.reverb_type = eff_type;
                } else if (ll == 0x20u) {
                    out.chorus_type = eff_type;
                } else {
                    out.variation_type = eff_type;
                }
                ++out.recognized_xg_count;
                return true;
            }

            // 3c. XG Multi Part parameters: 08 pp ll dd (pp in 0x00..0x0F)
            if (hh == 0x08u && mm <= 0x0Fu) {
                const uint8_t part = mm;
                switch (ll) {
                    case 0x0Bu: // Part Volume
                        out.parts[part].volume = dd;
                        ++out.recognized_xg_count;
                        return true;
                    case 0x0Eu: // Part Pan
                        out.parts[part].pan = dd;
                        ++out.recognized_xg_count;
                        return true;
                    case 0x11u: // Dry Level
                        out.parts[part].dry_level = dd;
                        ++out.recognized_xg_count;
                        return true;
                    case 0x12u: // Chorus Send
                        out.parts[part].chorus_send = dd;
                        ++out.recognized_xg_count;
                        return true;
                    case 0x13u: // Reverb Send
                        out.parts[part].reverb_send = dd;
                        ++out.recognized_xg_count;
                        return true;
                    case 0x14u: // Variation Send
                        out.parts[part].variation_send = dd;
                        ++out.recognized_xg_count;
                        return true;
                    case 0x72u: { // EQ Bass Gain (val - 64, clamped -12..+12 dB)
                        const int gain = std::clamp(static_cast<int>(dd) - 64, -12, 12);
                        out.parts[part].eq_low_gain_db = static_cast<int8_t>(gain);
                        ++out.recognized_xg_count;
                        return true;
                    }
                    case 0x73u: { // EQ Treble Gain (val - 64, clamped -12..+12 dB)
                        const int gain = std::clamp(static_cast<int>(dd) - 64, -12, 12);
                        out.parts[part].eq_high_gain_db = static_cast<int8_t>(gain);
                        ++out.recognized_xg_count;
                        return true;
                    }
                    default:
                        break;
                }
            }

            // 3d. XG Drum Setup per-key overrides: 30..33 kk ll dd (kk in 0x00..0x7F)
            if (hh >= 0x30u && hh <= 0x33u && mm <= 0x7Fu) {
                if (ll == 0x00u || ll == 0x01u || ll == 0x03u || ll == 0x04u) {
                    const uint8_t drum_setup = static_cast<uint8_t>(hh - 0x30u);
                    const uint8_t key = mm;
                    auto& ov = xg_sysex_detail::findOrAddDrumOverride(out.drum_overrides, drum_setup, key);
                    if (ll == 0x00u) {
                        ov.pitch_coarse = static_cast<int8_t>(static_cast<int>(dd) - 64);
                    } else if (ll == 0x01u) {
                        ov.level = dd;
                    } else if (ll == 0x03u) {
                        ov.pan = dd;
                    } else {
                        ov.reverb_send = dd;
                    }
                    ++out.recognized_xg_count;
                    return true;
                }
            }
        }
    } catch (...) {
        ++out.unrecognized_count;
        return false;
    }

    ++out.unrecognized_count;
    return false;
}

} // namespace ai_arranger::importers::sff1

#endif // AI_ARRANGER_IMPORTERS_SFF1_XG_SYSEX_PARSER_H
