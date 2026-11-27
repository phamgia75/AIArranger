#ifndef AI_ARRANGER_SFF1_TYPES_H
#define AI_ARRANGER_SFF1_TYPES_H

#include "importers/sff1/xg_sysex_parser.h"
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace ai_arranger::importers::sff1 {

// ── SFF1 Section Types ────────────────────────────────────────────
enum class SffSectionType : uint8_t {
    Intro1   = 0,
    Intro2   = 1,
    Intro3   = 2,
    Main1    = 3,
    Main2    = 4,
    Main3    = 5,
    Main4    = 6,
    Fill1    = 7,
    Fill2    = 8,
    Fill3    = 9,
    Fill4    = 10,
    Ending1  = 11,
    Ending2  = 12,
    Ending3  = 13,
    Break    = 14,
};

// ── SFF1 Track Roles ──────────────────────────────────────────────
enum class SffTrackRole : uint8_t {
    Rhythm1   = 0,
    Rhythm2   = 1,
    Bass      = 2,
    Chord1    = 3,
    Chord2    = 4,
    Pad       = 5,
    Phrase1   = 6,
    Phrase2   = 7,
    // SFF2 extras
    Extra1    = 8,
    Extra2    = 9,
    Extra3    = 10,
    Extra4    = 11,
    Extra5    = 12,
};

// ── NTT / NTR Enums ───────────────────────────────────────────────
enum class NtrType : uint8_t {
    Root        = 0,
    Fifth       = 1,
    Chord       = 2,
    Melodic     = 3,
    Scale       = 4,
    Bypass      = 5,
    Fixed       = 6,
    Unknown     = 0xFF,
};

enum class NttType : uint8_t {
    Melodic    = 0,
    Bypass     = 1,
    Guitar     = 2,
    Bass       = 3,
    Chord      = 4,
    Scale      = 5,
    Unknown    = 0xFF,
};

// ── CASM Data ─────────────────────────────────────────────────────
struct CasmSectionInfo {
    NtrType ntr;
    NttType ntt;
    uint8_t high_key;
    uint8_t low_key;
    uint8_t original_key;
    bool    is_megavoice;
    uint8_t megavoice_low;   // Velocity range low
    uint8_t megavoice_high;  // Velocity range high
};

// ── CASM Zone Configuration (Low / Middle / High) ────────────────
struct CasmZoneConfig {
    uint8_t ntr = 0;            // Note Transposition Rule (0 Root Trans, 1 Root Fixed, 2 Guitar)
    uint8_t ntt = 0;            // Note Transposition Table (bits 0-6)
    bool    ntt_bass = false;   // NTT bit 7: bass-note conversion enabled
    uint8_t high_key = 11;      // High Key 0-11 (C..B)
    uint8_t note_low = 0;       // Note Limit low (MIDI note)
    uint8_t note_high = 127;    // Note Limit high (MIDI note)
    uint8_t rtr = 0;            // Retrigger Rule
};

// ── CASM Track Configuration ──────────────────────────────────────
// One per SFF2 Ctb2 block (47 bytes) or legacy SFF1 Ctab (+ optional Cntt)
// block (33 bytes + 2 bytes). Layout: Wierzba/Bedesem "Style Files" v2.1
// Tables 5-16, checked against 4 Genos files (docs/importers/casm-structure-notes.md).
struct CasmTrackConfig {
    std::string    name;                           // Track name (8-byte field, trimmed)
    uint8_t        source_channel = 0;             // MIDI source channel (0-15)
    uint8_t        dest_channel = 0;               // MIDI destination channel (0-15)
    uint8_t        editable = 0;                   // [10] Editable flag
    uint16_t       note_mute_mask = 0x0FFF;        // [11..12] 12-bit Note Mute bitfield
    uint64_t       chord_mute_mask = 0x3FFFFFFFFull;// [13..17] 34-bit Chord Mute bitfield
    uint8_t        source_chord_root = 0;          // [18] Source Chord Root (0=C..11=B)
    uint8_t        source_chord_type = 0;          // [19] Source Chord Type (Yamaha table)
    uint8_t        zone_low = 0;                   // [20] Middle zone lowest note; 0 = no low zone
    uint8_t        zone_high = 127;                // [21] Middle zone highest note; 127 = no high zone
    uint8_t        ntr = 0;                        // Middle zone NTR (0 Root Trans, 1 Root Fixed, 2 Guitar)
    uint8_t        ntt = 0;                        // Middle zone NTT (bits 0-6)
    bool           ntt_bass = false;               // Middle zone NTT bit 7: bass-note conversion
    uint8_t        high_key = 11;                  // Middle zone High Key 0-11 (C..B)
    uint8_t        note_low = 0;                   // Middle zone Note Limit low
    uint8_t        note_high = 127;                // Middle zone Note Limit high
    uint8_t        rtr = 0;                        // Middle zone Retrigger Rule
    CasmZoneConfig low_zone{};                     // [22..27] SFF2 Low transposition zone
    CasmZoneConfig high_zone{};                    // [34..39] SFF2 High transposition zone
    bool           from_sff1_ctab = false;         // True if parsed from SFF1 Ctab/Cntt
    bool           is_megavoice = false;           // Megavoice candidate flag
};

// ── CASM Section (from Sdec declaration) ──────────────────────────
// One per Sdec sub-chunk. Groups the Ctb2 track configs that follow it
// within the same CSEG. Section names are the canonical Yamaha labels
// ("Main A", "Fill In AA", "Intro B", "Ending A", ...).
struct CasmSection {
    std::string name;
    std::vector<CasmTrackConfig> tracks;
};

// ── MIDI Event (raw SFF1) ─────────────────────────────────────────
struct SffMidiEvent {
    uint32_t tick;
    uint8_t  status;
    uint8_t  data1;
    uint8_t  data2;
};

// ── Section Track ─────────────────────────────────────────────────
struct SffTrack {
    SffTrackRole role;
    uint8_t      midi_channel;
    std::string  name;
    std::vector<SffMidiEvent> events;
};

// ── Style Section ─────────────────────────────────────────────────
struct SffSection {
    SffSectionType type;
    std::string    name;
    uint32_t       bars;
    uint32_t       resolution;
    bool           casm_parsed;
    CasmSectionInfo casm;
    std::vector<SffTrack> tracks;
};

// ── SMF marker (FF 06) — Yamaha section boundaries ("SInt", "Main A", ...) ──
struct SmfMarker {
    uint32_t    tick;
    std::string name;
};

// ── SMF Set Tempo (FF 51 03 tt tt tt) — raw microseconds per quarter note ──
struct SmfTempo {
    uint32_t tick;   // absolute tick within its MTrk
    uint32_t mpqn;   // microseconds per quarter note (1..0xFFFFFF)
};

// ── SMF Time Signature (FF 58 04 nn dd cc bb) — per-section meter changes ──
struct SmfTimeSignature {
    uint32_t tick = 0;
    uint8_t  num = 4;
    uint8_t  den = 4;
};

// ── SFF1 Chunk ────────────────────────────────────────────────────
struct SffChunk {
    std::string chunk_id;     // 4-char ID
    uint32_t    size;
    std::vector<uint8_t> data;
};

// ── Parse Result ──────────────────────────────────────────────────
struct ParseResult {
    bool success;
    std::string error;
    std::string file_path;
    std::vector<SffChunk> chunks;
    std::vector<SffSection> sections;
    std::vector<std::string> unsupported_features;
    std::vector<std::string> warnings;
    uint32_t total_chunks;
    uint32_t unknown_chunks;
    uint32_t parsed_sections;
    uint32_t parsed_events;
    std::vector<CasmTrackConfig> casm_configs;
    std::vector<std::string> sections_parsed;
    std::vector<CasmSection> casm_sections;
    // SMF meta data from the MTrk (section splitting): markers in file order,
    // the first time signature (FF 58), and the End-of-Track tick (0 = none).
    std::vector<SmfMarker> markers;
    // Every well-formed Set Tempo, in file order (MTrk by MTrk); the mapper
    // picks the earliest by tick. Raw MPQN: no rounding at the reader layer.
    std::vector<SmfTempo> tempos;
    // Every well-formed Time Signature (FF 58), in tick order (per-section meter).
    std::vector<SmfTimeSignature> time_signatures;
    // Explicitly detected non-note Yamaha metadata chunks (OTSc/OTSm/OTS/FNRP/MDB).
    std::vector<std::string> detected_metadata_chunks;
    bool     encrypted_or_drm = false;
    uint8_t  time_sig_num = 4;
    uint8_t  time_sig_den = 4;
    uint32_t end_of_track_tick = 0;
    // Live-parsed Yamaha XG / GM / GS SysEx summary from MTrk 0xF0/0xF7 events.
    XgSysExSummary xg_sysex;
};

// ── Supported Chunk IDs ───────────────────────────────────────────
constexpr const char* kKnownChunks[] = {
    "CASM", "OTS", "OTS ", "MDB", "MDB ", "FNRP", "STY", "SFF1", "SInt", "SAct", "SArt",
    "Midi", "Sect", "Trk", "MThd", "MTrk", "OTSc", "OTSm"
};

constexpr size_t kKnownChunkCount = sizeof(kKnownChunks) / sizeof(kKnownChunks[0]);

} // namespace ai_arranger::importers::sff1
#endif // AI_ARRANGER_SFF1_TYPES_H
