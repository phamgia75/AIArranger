#include "importers/sff1/sff1_reader.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <new>

namespace ai_arranger::importers::sff1 {

Sff1Reader::Sff1Reader() = default;

ParseResult Sff1Reader::parseFile(const std::string& path) noexcept try {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {false, "Cannot open file: " + path, path};
    }

    size_t fileSize = file.tellg();
    if (fileSize == 0 || fileSize > 10 * 1024 * 1024) {
        return {false, "File size out of range: " + std::to_string(fileSize), path};
    }

    std::vector<uint8_t> buffer(fileSize);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();

    return parseBuffer(buffer, path);
} catch (const std::bad_alloc&) {
    // noexcept entry point: an allocation failure is an import error, never
    // std::terminate (a crafted file must not be able to kill the host).
    ParseResult r{};
    r.success = false;
    r.error = "Out of memory while reading file";
    return r;
}

ParseResult Sff1Reader::parseBuffer(const std::vector<uint8_t>& buffer,
                                     const std::string& filename) noexcept {
    try {
        return parseBufferImpl(buffer, filename);
    } catch (const std::bad_alloc&) {
        result_ = ParseResult{};
        result_.success = false;
        result_.error = "Out of memory while parsing SFF1 data";
        data_ = nullptr; size_ = 0; pos_ = 0;
        return result_;
    }
}

ParseResult Sff1Reader::parseBufferImpl(const std::vector<uint8_t>& buffer,
                                        const std::string& filename) {
    data_ = buffer.data();
    size_ = buffer.size();
    pos_ = 0;
    casm_section_idx_ = -1;
    result_ = ParseResult{};
    result_.success = false;
    result_.file_path = filename;

    if (size_ < 4) {
        result_.error = "File too small to be SFF1";
        return result_;
    }

    // ── Read all top-level chunks ──────────────────────────────────
    while (pos_ < size_) {
        if (result_.chunks.size() >= kMaxTopLevelChunks) {
            result_.warnings.push_back("Top-level chunk limit (" +
                                       std::to_string(kMaxTopLevelChunks) +
                                       ") reached; remaining data ignored");
            break;
        }
        if (size_ - pos_ < 8) {
            result_.warnings.push_back("Trailing bytes at end of file (" +
                                       std::to_string(size_ - pos_) + " bytes)");
            break;
        }

        SffChunk chunk = readChunk();
        if (!chunk.chunk_id.empty()) {
            result_.chunks.push_back(chunk);

            if (!isKnownChunk(chunk.chunk_id)) {
                result_.unknown_chunks++;
            }

            // Try to parse known chunk types
            if (chunk.chunk_id == "SInt" || chunk.chunk_id == "Sect") {
                auto section = parseSection(chunk);
                result_.sections.push_back(std::move(section));
                result_.parsed_sections++;
            } else if (chunk.chunk_id == "MThd") {
                // SMF header — extract resolution
                parseMThd(chunk);
            } else if (chunk.chunk_id == "MTrk") {
                // SMF track — extract MIDI events
                parseMTrk(chunk);
            } else if (chunk.chunk_id == "CASM") {
                // CASM — semantic configuration data
                parseCasm(chunk.data.data(), chunk.data.size());
            }
        } else {
            // End of readable chunks
            break;
        }
    }

    // ── Build unsupported feature list ─────────────────────────────
    if (result_.unknown_chunks > 0) {
        result_.unsupported_features.push_back(
            "Unknown chunks: " + std::to_string(result_.unknown_chunks));
    }

    // Count total events
    for (const auto& section : result_.sections) {
        for (const auto& track : section.tracks) {
            result_.parsed_events += track.events.size();
        }
    }

    result_.total_chunks = result_.chunks.size();
    result_.success = result_.total_chunks > 0;

    if (!result_.success) {
        result_.error = "No recognizable SFF1 structure found";
    }

    return result_;
}

// ── Chunk Reading ─────────────────────────────────────────────────

SffChunk Sff1Reader::readChunk() {
    SffChunk chunk;

    // Try to read a 4-byte chunk ID
    if (!ensure(4)) return chunk;
    chunk.chunk_id = readString(4);

    // If the ID is not ASCII-printable, we've likely hit non-chunk data
    for (char c : chunk.chunk_id) {
        if (c < 32 || c > 126) {
            pos_ -= 4; // Go back (readString consumed exactly 4 bytes)
            return {};
        }
    }

    // Try to read chunk size (usually big-endian 4 bytes)
    if (!ensure(4)) return chunk;
    chunk.size = readU32BE();

    // Clamp size to remaining buffer
    if (chunk.size > size_ - pos_) {
        chunk.size = static_cast<uint32_t>(size_ - pos_);
    }

    // Read chunk data
    if (!ensure(chunk.size)) {
        chunk.size = static_cast<uint32_t>(size_ - pos_);
    }
    // assign() rather than resize()+memcpy: a zero-size chunk has data()==nullptr,
    // and memcpy with a null pointer is UB even for 0 bytes.
    chunk.data.assign(data_ + pos_, data_ + pos_ + chunk.size);
    skip(chunk.size);

    return chunk;
}

bool Sff1Reader::isKnownChunk(const std::string& id) const noexcept {
    for (size_t i = 0; i < kKnownChunkCount; ++i) {
        if (id == kKnownChunks[i]) return true;
    }
    return false;
}

// ── Section Parsing ───────────────────────────────────────────────

SffSection Sff1Reader::parseSection(const SffChunk& chunk) {
    SffSection section{};
    section.resolution = 480;
    section.bars = 4;
    section.name = chunk.chunk_id;
    section.casm_parsed = false;

    // Store original pos/ptr for nested parsing
    const uint8_t* saved_data = data_;
    size_t saved_size = size_;
    size_t saved_pos = pos_;

    data_ = chunk.data.data();
    size_ = chunk.data.size();
    pos_ = 0;

    // Try to parse tracks from the section data
    uint32_t trackCount = 0;
    if (ensure(1)) {
        trackCount = readU8(); // First byte might be track count
    }

    for (uint32_t t = 0; t < trackCount && t < 12 && pos_ < size_; ++t) {
        SffTrack track = parseTrack(data_, size_);
        section.tracks.push_back(std::move(track));
    }

    // Restore
    data_ = saved_data;
    size_ = saved_size;
    pos_ = saved_pos;

    return section;
}

SffTrack Sff1Reader::parseTrack(const uint8_t* data, size_t size) {
    SffTrack track{};
    track.midi_channel = 0;
    track.role = SffTrackRole::Phrase1;
    return track;
}

// ── MIDI Event Parsing ────────────────────────────────────────────

std::vector<SffMidiEvent> Sff1Reader::parseMidiEvents(const uint8_t* data,
                                                        size_t size,
                                                        uint32_t& offset) {
    std::vector<SffMidiEvent> events;
    if (offset >= size) return events;

    const uint8_t* d = data + offset;
    const size_t remaining = size - offset;
    uint8_t  running_status = 0;
    uint32_t absolute_tick = 0;
    size_t   pos = 0;
    bool     have_time_sig = false;

    // Standard MIDI File variable-length quantity (max 4 bytes).
    auto readVlq = [&](uint32_t& out) -> bool {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos >= remaining) return false;
            const uint8_t b = d[pos++];
            out = (out << 7) | (b & 0x7F);
            if ((b & 0x80) == 0) return true;
        }
        return false;   // > 4 bytes: malformed
    };

    // SMF 1.0 track decoding. Only channel messages become events. SysEx
    // (F0/F7 <len> <payload>) and meta (FF <type> <len> <payload>) events are
    // skipped by their declared length — real Genos styles carry dozens of
    // SysEx messages, whose payload must never be read as delta-times — and
    // both cancel running status. End of Track (FF 2F) stops decoding; a
    // truncated or unparseable message stops decoding without emitting it.
    for (;;) {
        uint32_t delta = 0;
        if (!readVlq(delta)) break;
        absolute_tick += delta;
        if (pos >= remaining) break;

        uint8_t status = d[pos];
        if (status & 0x80) {
            ++pos;
        } else if (running_status != 0) {
            status = running_status;   // data byte: reuse the running status
        } else {
            break;                     // data byte with no status: malformed
        }

        if (status == 0xFF) {
            if (pos >= remaining) break;
            const uint8_t metaType = d[pos++];
            uint32_t len = 0;
            if (!readVlq(len)) break;
            running_status = 0;
            if (metaType == 0x2F) {                    // End of Track
                result_.end_of_track_tick = absolute_tick;
                break;
            }
            if (len > remaining - pos) break;          // truncated meta
            // Section markers + time signature feed the mapper's section split.
            if (metaType == 0x06 && len > 0 && len <= 64 &&
                result_.markers.size() < kMaxTopLevelChunks) {
                std::string name(reinterpret_cast<const char*>(d + pos), len);
                while (!name.empty() && static_cast<uint8_t>(name.back()) <= 32) name.pop_back();
                result_.markers.push_back({absolute_tick, std::move(name)});
            } else if (metaType == 0x58 && len >= 2 && !have_time_sig) {
                result_.time_sig_num = d[pos];
                result_.time_sig_den = (d[pos + 1] < 8) ? static_cast<uint8_t>(1u << d[pos + 1]) : 4;
                have_time_sig = true;
            }
            pos += len;
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {
            uint32_t len = 0;
            if (!readVlq(len)) break;
            running_status = 0;
            if (len > remaining - pos) break;          // truncated SysEx
            pos += len;
            continue;
        }
        if (status >= 0xF0) break;                     // not valid inside an SMF track

        running_status = status;
        const uint8_t type = status & 0xF0;
        const size_t  need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
        if (need > remaining - pos) break;             // truncated channel message

        SffMidiEvent ev;
        ev.tick   = absolute_tick;
        ev.status = status;
        ev.data1  = d[pos];
        ev.data2  = (need == 2) ? d[pos + 1] : 0;
        pos += need;
        events.push_back(ev);
    }

    offset += pos;
    return events;
}

// ── Reader Helpers ────────────────────────────────────────────────

uint8_t Sff1Reader::readU8() noexcept {
    if (!ensure(1)) return 0;
    return data_[pos_++];
}

uint16_t Sff1Reader::readU16LE() noexcept {
    if (!ensure(2)) return 0;
    uint16_t v = data_[pos_] | (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
}

uint32_t Sff1Reader::readU32LE() noexcept {
    if (!ensure(4)) return 0;
    uint32_t v = data_[pos_] |
                 (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                 (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                 (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
}

uint32_t Sff1Reader::readU32BE() noexcept {
    if (!ensure(4)) return 0;
    uint32_t v = (static_cast<uint32_t>(data_[pos_]) << 24) |
                 (static_cast<uint32_t>(data_[pos_ + 1]) << 16) |
                 (static_cast<uint32_t>(data_[pos_ + 2]) << 8) |
                 static_cast<uint32_t>(data_[pos_ + 3]);
    pos_ += 4;
    return v;
}

std::string Sff1Reader::readString(size_t len) {
    if (!ensure(len)) return {};
    std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return s;
}

void Sff1Reader::skip(size_t bytes) noexcept {
    if (bytes > size_ - pos_) {
        pos_ = size_;
    } else {
        pos_ += bytes;
    }
}

bool Sff1Reader::ensure(size_t bytes) noexcept {
    return pos_ + bytes <= size_;
}

// ── SMF/SFF2 Parsing ─────────────────────────────────────────────

bool Sff1Reader::parseMThd(const SffChunk& chunk) {
    if (chunk.data.size() < 6) return false;

    // MThd division is big-endian per the Standard MIDI File spec
    // (bytes [4]=hi, [5]=lo). Reading it little-endian corrupts the PPQN
    // (e.g. 0x0780=1920 became 0x8007 -> SMPTE bit set -> 7), which made the
    // playback clock run the events at the wrong tempo (effectively silent).
    uint16_t division = (static_cast<uint16_t>(chunk.data[4]) << 8) |
                        static_cast<uint16_t>(chunk.data[5]);

    SffSection section;
    section.type = SffSectionType::Main1;
    section.name = "SMF (SFF2/Genos)";
    section.bars = 4;
    section.resolution = (division & 0x8000) ? (division & 0x7FFF) : division;
    section.casm_parsed = false;
    result_.sections.push_back(std::move(section));
    result_.parsed_sections++;

    return true;
}

bool Sff1Reader::parseMTrk(const SffChunk& chunk) {
    if (chunk.data.empty()) return false;

    uint32_t offset = 0;
    auto events = parseMidiEvents(chunk.data.data(), chunk.data.size(), offset);

    if (!events.empty() && !result_.sections.empty()) {
        auto& section = result_.sections.back();
        SffTrack track;
        track.role = SffTrackRole::Phrase1;
        track.midi_channel = 0;
        track.name = "MTrk";
        track.events = std::move(events);
        section.tracks.push_back(std::move(track));
        result_.parsed_events += track.events.size();
    }

    return true;
}

// ── CASM Parsing ───────────────────────────────────────────────────

bool Sff1Reader::parseCasm(const uint8_t* data, size_t size, int depth) {
    if (size < 8) return false;
    if (depth > kMaxCasmDepth) {
        // Real files nest CSEG once; a deeper chain is corrupt or crafted and
        // would otherwise recurse until the stack overflows.
        if (depth == kMaxCasmDepth + 1)
            result_.warnings.push_back("CASM nesting deeper than " +
                                       std::to_string(kMaxCasmDepth) +
                                       " levels ignored");
        return false;
    }

    size_t pos = 0;

    // Optional CSEG marker (present at top level, absent in inner CSEG)
    if (data[0] == 'C' && data[1] == 'S' && data[2] == 'E' && data[3] == 'G') {
        pos = 8; // Skip CSEG + 4-byte big-endian size
    }

    // Parse sub-chunks: id(4) + size_be(4) + data(size)
    while (pos + 8 <= size) {
        std::string sub_id(reinterpret_cast<const char*>(data + pos), 4);
        // CASM sizes are BIG-ENDIAN
        uint32_t sub_size = (static_cast<uint32_t>(data[pos + 4]) << 24) |
                            (static_cast<uint32_t>(data[pos + 5]) << 16) |
                            (static_cast<uint32_t>(data[pos + 6]) << 8) |
                             static_cast<uint32_t>(data[pos + 7]);
        pos += 8;

        if (pos + sub_size > size) break;

        if (sub_id == "Ctb2") {
            parseCtb2Block(data + pos, sub_size);
        } else if (sub_id == "CSEG") {
            // Inner CSEG — another section
            parseCasm(data + pos, sub_size, depth + 1);
        } else if (sub_id == "Sdec") {
            // Section definition — extract section name and open a new
            // CASM section. Subsequent Ctb2 blocks attach to it.
            if (sub_size > 0 && sub_size <= 64) {
                std::string sec_name(reinterpret_cast<const char*>(data + pos), sub_size);
                // Strip padding
                while (!sec_name.empty() &&
                       static_cast<uint8_t>(sec_name.back()) <= 32)
                    sec_name.pop_back();
                if (!sec_name.empty()) {
                    result_.sections_parsed.push_back(sec_name);
                    CasmSection cs;
                    cs.name = sec_name;
                    result_.casm_sections.push_back(std::move(cs));
                    casm_section_idx_ =
                        static_cast<int>(result_.casm_sections.size()) - 1;
                }
            }
        }

        pos += sub_size;
    }

    return true;
}

void Sff1Reader::parseCtb2Block(const uint8_t* data, size_t size) {
    // A Ctb2 sub-chunk holds one source-channel configuration. Layout
    // (offsets within the entry), validated against 4 real Genos files:
    //   [0]      Source Channel
    //   [1..8]   Name (8 bytes, space-padded)
    //   [9]      Destination Channel
    //   [10]     Editable flag
    //   [11..17] Note/chord mute bitfields
    //   [18..19] Source chord (root, type)
    //   [20]     Note Limit Low
    //   [21]     Note Limit High
    //   [22]     NTR (Note Transposition Rule)
    //   [23]     NTT (bit 7 = bass-note flag, bits 0-6 = table type)
    //   [24..]   Repeats for additional section variants (SFF2)
    constexpr size_t kMinEntry = 24;   // need through NTT at [23]
    if (size < kMinEntry) return;      // buffer too short — skip, no crash

    CasmTrackConfig cfg{};
    cfg.source_channel = data[0];

    // Name: bytes 1..8, strip trailing whitespace / non-printable
    cfg.name = std::string(reinterpret_cast<const char*>(data + 1), 8);
    while (!cfg.name.empty() &&
           (static_cast<uint8_t>(cfg.name.back()) <= 32 ||
            static_cast<uint8_t>(cfg.name.back()) == 127))
        cfg.name.pop_back();

    cfg.dest_channel = data[9];
    cfg.low_key  = data[20];
    cfg.high_key = data[21];
    cfg.ntr      = data[22];

    const uint8_t ntt_raw = data[23];
    cfg.ntt_bass = (ntt_raw & 0x80) != 0;
    cfg.ntt      = ntt_raw & 0x7F;

    // MegaVoice is detected from velocity data (see report generator),
    // not from CASM track config — do not guess here.
    cfg.is_megavoice = false;

    // Attach to the current CASM section (opened by the preceding Sdec),
    // then keep a flat copy for callers that don't need section grouping.
    if (casm_section_idx_ >= 0 &&
        casm_section_idx_ < static_cast<int>(result_.casm_sections.size())) {
        result_.casm_sections[casm_section_idx_].tracks.push_back(cfg);
    }
    result_.casm_configs.push_back(std::move(cfg));
}

} // namespace ai_arranger::importers::sff1
