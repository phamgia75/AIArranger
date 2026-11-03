#ifndef AI_ARRANGER_SFF1_READER_H
#define AI_ARRANGER_SFF1_READER_H

#include "importers/sff1/sff1_types.h"
#include <vector>
#include <cstdint>

namespace ai_arranger::importers::sff1 {

/**
 * SFF1 binary reader.
 *
 * Reads Yamaha SFF1 (.sty) files, parses known chunks,
 * and extracts MIDI data from recognized sections.
 *
 * Architecture rule: This is importer-layer ONLY.
 * No runtime code touches SFF1 types.
 * No vendor logic leaks into engine.
 */

class Sff1Reader {
public:
    // Hostile-input bounds (a .sty is user-supplied). Real Genos files have 4
    // top-level chunks and one CSEG nesting level; these leave ample headroom
    // while bounding stack depth and memory for crafted files.
    static constexpr size_t kMaxTopLevelChunks = 1024;
    static constexpr int    kMaxCasmDepth      = 8;

    Sff1Reader();

    ParseResult parseFile(const std::string& path) noexcept;
    ParseResult parseBuffer(const std::vector<uint8_t>& buffer,
                            const std::string& filename) noexcept;

private:
    ParseResult parseBufferImpl(const std::vector<uint8_t>& buffer,
                                const std::string& filename);

    // Internal parse helpers may throw std::bad_alloc (they build strings and
    // vectors); the noexcept parseFile/parseBuffer convert that into a failed
    // ParseResult, so no exception ever leaves the importer.

    // ── Binary reading helpers ─────────────────────────────────────
    uint8_t  readU8() noexcept;
    uint16_t readU16LE() noexcept;
    uint32_t readU32LE() noexcept;
    uint32_t readU32BE() noexcept;
    std::string readString(size_t len);
    void skip(size_t bytes) noexcept;
    bool ensure(size_t bytes) noexcept;

    // ── Chunk detection ────────────────────────────────────────────
    SffChunk readChunk();
    bool isKnownChunk(const std::string& id) const noexcept;

    // ── Section parsing ────────────────────────────────────────────
    SffSection parseSection(const SffChunk& chunk);
    SffTrack parseTrack(const uint8_t* data, size_t size);

    // ── MIDI event parsing ─────────────────────────────────────────
    std::vector<SffMidiEvent> parseMidiEvents(const uint8_t* data,
                                              size_t size,
                                              uint32_t& offset);

    // ── SMF/SFF2 helpers ────────────────────────────────────────────
    bool parseMThd(const SffChunk& chunk);
    bool parseMTrk(const SffChunk& chunk);

    // ── CASM parsing (Claude-generated) ──────────────────────────────
    bool parseCasm(const uint8_t* data, size_t size, int depth = 0);
    void parseCtb2Block(const uint8_t* data, size_t size);

    // ── State ──────────────────────────────────────────────────────
    const uint8_t* data_{nullptr};
    size_t size_{0};
    size_t pos_{0};
    int    casm_section_idx_{-1};   // current CASM section during parseCasm
    ParseResult result_;
};

} // namespace ai_arranger::importers::sff1
#endif // AI_ARRANGER_SFF1_READER_H
