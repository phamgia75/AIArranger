#ifndef AI_ARRANGER_SFF1_TYPES_H
#define AI_ARRANGER_SFF1_TYPES_H

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

// ── CASM Track Configuration ──────────────────────────────────────
// One per Ctb2 source-channel block. Field offsets validated against
// 4 real Genos .S718 files — see docs/importers/casm-structure-notes.md.
struct CasmTrackConfig {
    std::string name;           // Track name (8-byte field, trimmed)
    uint8_t     source_channel; // MIDI source channel (0-15)
    uint8_t     dest_channel;   // MIDI destination channel (0-15)
    uint8_t     high_key;       // Note limit high
    uint8_t     low_key;        // Note limit low
    uint8_t     ntr;            // Note Transposition Rule value
    uint8_t     ntt;            // Note Transposition Table type (bits 0-6)
    bool        ntt_bass;       // NTT bit 7: bass-note conversion enabled
    bool        is_megavoice;   // Megavoice candidate flag
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
    uint8_t  time_sig_num = 4;
    uint8_t  time_sig_den = 4;
    uint32_t end_of_track_tick = 0;
};

// ── Supported Chunk IDs ───────────────────────────────────────────
constexpr const char* kKnownChunks[] = {
    "CASM", "OTS", "MDB", "STY", "SFF1", "SInt", "SAct", "SArt",
    "Midi", "Sect", "Trk", "MThd", "MTrk", "OTSc", "OTSm"
};

constexpr size_t kKnownChunkCount = sizeof(kKnownChunks) / sizeof(kKnownChunks[0]);

} // namespace ai_arranger::importers::sff1
#endif // AI_ARRANGER_SFF1_TYPES_H
