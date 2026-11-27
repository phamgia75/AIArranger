#ifndef AI_ARRANGER_UASF_FORMAT_H
#define AI_ARRANGER_UASF_FORMAT_H

#include <cstdint>
#include <cstddef>

namespace ai_arranger::uasf::format {

// ── Binary format constants ────────────────────────────────────────

constexpr uint32_t kMagic = 0x46534155; // "UASF" little-endian
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxSections = 32;
constexpr uint32_t kMaxTracksPerSection = 16;
constexpr uint32_t kMaxTrackNameLength = 64;
constexpr uint32_t kMaxEvents = 65536;

// ── File header (16 bytes) ─────────────────────────────────────────

#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic;           // kMagic
    uint32_t version;         // kVersion
    uint32_t flags;           // reserved
    uint32_t section_count;   // number of sections
};

// ── Section header ─────────────────────────────────────────────────

struct SectionHeader {
    uint8_t  type;            // SectionType enum
    uint16_t bars;
    uint16_t resolution;      // ticks per quarter note
    uint8_t  beats_per_bar;
    uint8_t  beat_note;
    uint8_t  track_count;
    uint8_t  padding;         // reserved
};

// ── Track header (variable-length due to name) ─────────────────────

struct TrackHeader {
    uint8_t  name_length;     // bytes in name (0-63)
    uint8_t  midi_channel;
    uint8_t  role;            // TrackRole enum
    uint8_t  is_drum;
    uint8_t  articulation_profile;  // ArticulationProfile enum
    uint8_t  fidelity;             // FidelityRequirement enum
    uint16_t playback_flags;       // PlaybackRecommendation bitmask
    uint32_t event_count;
    // Followed by: char name[name_length]
    // Followed by: MidiEventSerialized[event_count]
};

// ── MIDI event (8 bytes, delta-encoded ticks) ──────────────────────

struct MidiEventSerialized {
    uint32_t tick_delta;      // ticks from previous event
    uint8_t  type_channel;    // high nibble = type, low nibble = channel
    uint8_t  data1;
    uint8_t  data2;
    uint8_t  padding;
};
// ── Extension blocks (v1.1, optional) ──────────────────────────────
// When FileHeader.flags has kFlagExtensions, tagged blocks follow the last
// section until end of file: ExtensionHeader + `length` payload bytes. Readers
// keep unknown tags verbatim (StyleDefinition::unknown_extensions); writers
// re-emit them. `version` stays 1: v1.0 readers ignore `flags`
// and trailing bytes, so they still load these files (at their defaults), and
// files without the flag (every file written before v1.1) are unchanged.

struct ExtensionHeader {
    uint32_t tag;             // four ASCII bytes, little-endian
    uint32_t length;          // payload bytes that follow
};
#pragma pack(pop)

constexpr uint32_t kFlagExtensions = 1u << 0;
// 'TMPO': style tempo, payload = uint32 whole BPM in [kMinTempoBpm, kMaxTempoBpm]
// (types.h). Absent = no stored tempo (the reader uses 120 BPM, as v1.0 did).
constexpr uint32_t kExtTempo = 0x4F504D54;
// 'TRNS': per-track register limits (ArticulationMetadata high_key/note_low/
// note_high), payload = N x {u8 section, u8 track, u8 high_key 0-11, u8 note_low,
// u8 note_high}, note_high - note_low >= 11. Written only for tracks that differ
// from the defaults (11, 0, 127); absent = defaults, as before.
constexpr uint32_t kExtTranspose = 0x534E5254;
// 'KEXP': Korg Pa5X Style SMF export settings (StyleDefinition::korg_export), payload =
// u8 version 1, u8 R + R x {u8 source ch 0-15, u8 part ch 0-15},
// u8 S + S x {u8 Korg ch 8-15, u8 msb, u8 lsb, u8 pc},
// u16 E + E x {u8 section type 0-14, u8 Korg ch 8-15, u8 msb, u8 lsb, u8 pc}; values 0-127, keys unique,
// sorted. Written only when not empty; absent = no settings, as before.
constexpr uint32_t kExtKorgExport = 0x5058454B;
// 'SNAM': persisted section names (SectionDefinition::keep_name), payload =
// N x {u8 section index (ascending, unique), u8 length 1-64, name bytes}. Absent = defaults, as before.
constexpr uint32_t kExtSectionNames = 0x4D414E53;
// 'CASM': per-section, per-track Yamaha CASM metadata (ArticulationMetadata::has_casm_meta),
// payload = u8 version (1) + N x 34-byte records:
//   [0..17]  {u8 si, u8 ti, u8 ntr, u8 ntt (0-127), u8 ntt_bass (0/1), u8 rtr,
//             u8 src_root (0-11), u8 src_type, u16 LE note_mute (0-0x0FFF),
//             u8[5] LE chord_mute (34-bit), u8 zone_low (0-127), u8 zone_high (0-127),
//             u8 dest_channel (0-15 or 0xFF)}
//   [18]     u8 casm_editable
//   [19..25] low_zone  {u8 ntr, u8 ntt (0-127), u8 ntt_bass (0/1), u8 high_key (0-11),
//                       u8 note_low (0-127), u8 note_high (0-127), u8 rtr}
//   [26..32] high_zone (same 7-byte layout)
//   [33]     u8 reserved (0)
// Written only when at least one track has has_casm_meta == true; absent in v1.0/v1.1 files.
constexpr uint32_t kExtCasm = 0x4D534143;
// 'XGSX': style-level Yamaha XG / GM / GS SysEx summary metadata (StyleDefinition::xg_sysex),
// payload = u8 version (1) + 5 x u32 LE counters + u8 mode_flags + u8 opt_flags + optional
// scalar values + u8 part_count (<=16) + per-part records + u16 LE drum_override_count (<=512)
// + per-drum records + u8 advisory_count (<=32) + length-prefixed advisory strings (1..64 B).
// Written only when !style.xg_sysex.empty(); absent in legacy and SysEx-free files.
constexpr uint32_t kExtXgSysEx = 0x58534758;

// ── Validation constants ───────────────────────────────────────────

constexpr uint32_t kMaxFileSize = 10 * 1024 * 1024; // 10 MB
constexpr uint32_t kMinFileSize = sizeof(FileHeader);

// ── Playback recommendation bitmask ────────────────────────────────

enum PlaybackFlag : uint16_t {
    PF_EXTERNAL_YAMAHA    = 1 << 0,
    PF_EXTERNAL_MIDI      = 1 << 1,
    PF_PREMIUM_LIBRARY    = 1 << 2,
    PF_USER_SOUNDFONT     = 1 << 3,
    PF_DEGRADATION_OK     = 1 << 4,
};

} // namespace ai_arranger::uasf::format
#endif // AI_ARRANGER_UASF_FORMAT_H
