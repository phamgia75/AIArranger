# UASF Field Naming Conventions

> **Frozen as of Gate 3 (2026-06-13).** No field renames without MAJOR version bump.

## Naming Rules

- `snake_case` for all fields
- No abbreviations except: `midi`, `bpm`, `uasf`, `id`
- Boolean fields: prefix `is_` or `has_` (e.g., `is_drum`, `has_keyswitch`)
- Enum values: `UPPER_SNAKE_CASE`
- Constants: `k` prefix + PascalCase (`kMaxSections`, `kDefaultSampleRate`)

## Frozen Core Fields

### FileHeader

| Field | Type | Frozen |
|-------|------|--------|
| magic | uint32 | ✅ v1.0 |
| version | uint32 | ✅ v1.0 |
| flags | uint32 | ✅ v1.0 |
| section_count | uint32 | ✅ v1.0 |

### SectionHeader

| Field | Type | Frozen |
|-------|------|--------|
| type | uint8 | ✅ v1.0 |
| bars | uint16 | ✅ v1.0 |
| resolution | uint16 | ✅ v1.0 |
| beats_per_bar | uint8 | ✅ v1.0 |
| beat_note | uint8 | ✅ v1.0 |
| track_count | uint8 | ✅ v1.0 |

### TrackHeader

| Field | Type | Frozen |
|-------|------|--------|
| name_length | uint8 | ✅ v1.0 |
| midi_channel | uint8 | ✅ v1.0 |
| role | uint8 | ✅ v1.0 |
| is_drum | uint8 | ✅ v1.0 |
| articulation_profile | uint8 | ✅ v1.0 |
| fidelity | uint8 | ✅ v1.0 |
| playback_flags | uint16 | ✅ v1.0 |
| event_count | uint32 | ✅ v1.0 |

### MidiEventSerialized

| Field | Type | Frozen |
|-------|------|--------|
| tick_delta | uint32 | ✅ v1.0 |
| type_channel | uint8 | ✅ v1.0 |
| data1 | uint8 | ✅ v1.0 |
| data2 | uint8 | ✅ v1.0 |

## v1.1 Extension Blocks (optional)

`FileHeader.flags` bit 0 (`kFlagExtensions`) means tagged blocks follow the last
section, until end of file. `version` stays **1**, so v1.0 readers (which ignore
`flags` and trailing bytes) still load v1.1 files at their defaults.

| Field | Type | Since |
|-------|------|-------|
| tag | uint32 (4 ASCII bytes, LE) | v1.1 |
| length | uint32 (payload bytes) | v1.1 |
| payload | `length` bytes | v1.1 |

Readers skip unknown tags by `length`; a truncated block is an error.

| Tag | Payload | Meaning |
|-----|---------|---------|
| `TMPO` | uint32 `tempo_bpm`, 20..400 | Style tempo (whole BPM). Absent → 120 BPM, the v1.0 default. At most one. |

Writers emit `TMPO` only for a set tempo (`tempo_bpm != 0`); a style without a
tempo is written with `flags = 0` and no trailer, byte-identical to v1.0.
