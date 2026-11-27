#include "importers/sff1/sff1_reader.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <new>
#include <cstdint>

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

            // Fail-closed on encrypted/DRM Yamaha containers.
            if (chunk.chunk_id == "CRYP" || chunk.chunk_id == "ENCR" || chunk.chunk_id == "DRM ") {
                result_.encrypted_or_drm = true;
                result_.success = false;
                result_.error = "Encrypted/DRM Yamaha style chunk refused (fail-closed): " + chunk.chunk_id;
                return result_;
            }

            // Explicitly record non-note Yamaha metadata chunks (never silently swallowed).
            if (chunk.chunk_id == "OTSc" || chunk.chunk_id == "OTSm" ||
                chunk.chunk_id == "OTS"  || chunk.chunk_id == "OTS " ||
                chunk.chunk_id == "FNRP" || chunk.chunk_id == "MDB"  || chunk.chunk_id == "MDB ") {
                result_.detected_metadata_chunks.push_back(chunk.chunk_id);
                result_.unsupported_features.push_back(
                    "Detected Yamaha metadata chunk '" + chunk.chunk_id + "' (read-only, not mapped)");
            }

            // Try to parse known chunk types
            if (chunk.chunk_id == "SInt" || chunk.chunk_id == "Sect") {
                auto section = parseSection(chunk);
                result_.sections.push_back(std::move(section));
                result_.parsed_sections++;
            } else if (chunk.chunk_id == "MThd") {
                // SMF header — extract resolution (fail-closed on malformed MThd)
                if (!parseMThd(chunk)) {
                    result_.success = false;
                    result_.error = "Malformed MThd chunk (truncated or zero time division)";
                    return result_;
                }
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

    // Markers and time signatures are collected MTrk by MTrk, each with its own
    // tick base of 0; the mapper's section ranges need them in time order.
    std::stable_sort(result_.markers.begin(), result_.markers.end(),
                     [](const SmfMarker& a, const SmfMarker& b) { return a.tick < b.tick; });
    std::stable_sort(result_.time_signatures.begin(), result_.time_signatures.end(),
                     [](const SmfTimeSignature& a, const SmfTimeSignature& b) { return a.tick < b.tick; });

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
    uint64_t absolute_tick = 0;   // wider than SmfMarker/SffMidiEvent ticks: checked below
    size_t   pos = 0;
    bool     have_time_sig = false;

    // Bounded multi-packet SMF SysEx reassembly state (0xF0 ... 0xF7 continuations).
    // Strictly capped at kMaxSysExPayloadBytes so crafted files never allocate unbounded RAM.
    bool                 sysex_in_progress = false;
    bool                 sysex_overflowed  = false;
    std::vector<uint8_t> sysex_buf;

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
    // (F0/F7 <len> <payload>) are decoded live into result_.xg_sysex with strict
    // bounds (single-packet and multi-packet 0xF7 continuation), then skipped by
    // their declared length — real Genos styles carry dozens of SysEx messages,
    // whose payload must never be read as delta-times — and both SysEx and meta
    // (FF <type> <len> <payload>) cancel running status. End of Track (FF 2F)
    // stops decoding; a truncated or unparseable message stops decoding without
    // emitting it.
    for (;;) {
        uint32_t delta = 0;
        if (!readVlq(delta)) break;
        absolute_tick += delta;
        if (absolute_tick > UINT32_MAX) {   // ticks are stored as uint32: never wrap
            result_.warnings.push_back("MTrk too long (tick exceeds 2^32-1); rest of the track ignored");
            break;
        }
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
                result_.end_of_track_tick = static_cast<uint32_t>(absolute_tick);
                break;
            }
            if (len > remaining - pos) break;          // truncated meta
            // Section markers + time signature feed the mapper's section split.
            if (metaType == 0x06 && len > 0 && len <= 64 &&
                result_.markers.size() < kMaxTopLevelChunks) {
                std::string name(reinterpret_cast<const char*>(d + pos), len);
                while (!name.empty() && static_cast<uint8_t>(name.back()) <= 32) name.pop_back();
                result_.markers.push_back({static_cast<uint32_t>(absolute_tick), std::move(name)});
            } else if (metaType == 0x58 && len >= 2) {
                const uint8_t num = d[pos];
                const uint8_t den = (d[pos + 1] < 8) ? static_cast<uint8_t>(1u << d[pos + 1]) : 4;
                if (num > 0 && result_.time_signatures.size() < kMaxTopLevelChunks) {
                    result_.time_signatures.push_back({static_cast<uint32_t>(absolute_tick), num, den});
                }
                if (!have_time_sig) {
                    result_.time_sig_num = num;
                    result_.time_sig_den = den;
                    have_time_sig = true;
                }
            } else if (metaType == 0x51) {             // Set Tempo: 3-byte MPQN
                const uint32_t mpqn = (len == 3) ? (uint32_t(d[pos]) << 16) |
                                                   (uint32_t(d[pos + 1]) << 8) | d[pos + 2]
                                                 : 0;
                if (mpqn == 0) {
                    result_.warnings.push_back("Malformed Set Tempo at tick " +
                                               std::to_string(absolute_tick) + " ignored");
                } else if (result_.tempos.size() < kMaxTopLevelChunks) {
                    result_.tempos.push_back({static_cast<uint32_t>(absolute_tick), mpqn});
                }
            }
            pos += len;
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {
            uint32_t len = 0;
            running_status = 0;
            if (!readVlq(len) || len > remaining - pos) {
                if (sysex_in_progress) {
                    recordMalformedSysEx(result_.xg_sysex);
                    sysex_in_progress = false;
                    sysex_overflowed = false;
                    sysex_buf.clear();
                }
                recordMalformedSysEx(result_.xg_sysex);
                break;                                 // truncated or invalid SysEx
            }

            const uint8_t* pkt = d + pos;
            if (status == 0xF0) {
                if (sysex_in_progress) {
                    // Previous divided SysEx was never terminated by 0xF7.
                    recordMalformedSysEx(result_.xg_sysex);
                    sysex_in_progress = false;
                    sysex_overflowed = false;
                    sysex_buf.clear();
                }
                if (len == 0) {
                    recordMalformedSysEx(result_.xg_sysex);
                } else if (pkt[len - 1] == 0xF7u) {
                    // Complete single-packet SMF SysEx (strip trailing 0xF7; zero allocation).
                    parseXgSysExMessage(pkt, static_cast<size_t>(len - 1), result_.xg_sysex);
                } else {
                    // Start of divided / multi-packet SMF SysEx; continue on 0xF7 packets.
                    sysex_in_progress = true;
                    sysex_overflowed = (len > kMaxSysExPayloadBytes);
                    sysex_buf.clear();
                    if (!sysex_overflowed) {
                        sysex_buf.assign(pkt, pkt + len);
                    }
                }
            } else { // status == 0xF7
                if (sysex_in_progress) {
                    const bool closes = (len > 0 && pkt[len - 1] == 0xF7u);
                    const size_t chunk_len = closes ? static_cast<size_t>(len - 1)
                                                    : static_cast<size_t>(len);
                    if (!sysex_overflowed) {
                        if (chunk_len > kMaxSysExPayloadBytes - sysex_buf.size()) {
                            sysex_overflowed = true;
                            sysex_buf.clear();
                        } else if (chunk_len > 0) {
                            sysex_buf.insert(sysex_buf.end(), pkt, pkt + chunk_len);
                        }
                    }
                    if (closes) {
                        if (sysex_overflowed) {
                            recordMalformedSysEx(result_.xg_sysex);
                        } else {
                            parseXgSysExMessage(sysex_buf.data(), sysex_buf.size(), result_.xg_sysex);
                        }
                        sysex_in_progress = false;
                        sysex_overflowed = false;
                        sysex_buf.clear();
                    }
                } else if (len >= 2 && pkt[0] == 0xF0u && pkt[len - 1] == 0xF7u) {
                    // Escaped complete SysEx packet: F7 <len> F0 <payload...> F7.
                    parseXgSysExMessage(pkt + 1, static_cast<size_t>(len - 2), result_.xg_sysex);
                } else {
                    // Stray 0xF7 continuation or non-SysEx escape with no preceding 0xF0.
                    recordMalformedSysEx(result_.xg_sysex);
                }
            }

            pos += len;
            continue;
        }
        if (status >= 0xF0) break;                     // not valid inside an SMF track

        running_status = status;
        const uint8_t type = status & 0xF0;
        const size_t  need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
        if (need > remaining - pos) break;             // truncated channel message

        SffMidiEvent ev;
        ev.tick   = static_cast<uint32_t>(absolute_tick);
        ev.status = status;
        ev.data1  = d[pos];
        ev.data2  = (need == 2) ? d[pos + 1] : 0;
        pos += need;
        events.push_back(ev);
    }

    if (sysex_in_progress) {
        recordMalformedSysEx(result_.xg_sysex);
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
    const uint16_t res = (division & 0x8000) ? static_cast<uint16_t>(division & 0x7FFF) : division;
    if (res == 0) return false;

    SffSection section;
    section.type = SffSectionType::Main1;
    section.name = "SMF (SFF2/Genos)";
    section.bars = 4;
    section.resolution = res;
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

namespace {

CasmZoneConfig readZone(const uint8_t* z) noexcept {
    CasmZoneConfig out{};
    out.ntr = z[0];
    out.ntt_bass = (z[1] & 0x80) != 0;
    out.ntt = z[1] & 0x7F;
    out.high_key = z[2];
    out.note_low = z[3];
    out.note_high = z[4];
    out.rtr = z[5];
    return out;
}

std::string trimCasmName(const uint8_t* ptr, size_t len) {
    std::string s(reinterpret_cast<const char*>(ptr), len);
    while (!s.empty() &&
           (static_cast<uint8_t>(s.back()) <= 32 ||
            static_cast<uint8_t>(s.back()) == 127)) {
        s.pop_back();
    }
    return s;
}

// Parse legacy SFF1 Ctab block (Wierzba/Bedesem v2.1 Tables 5-10, 33 bytes standard, min 26).
void parseCtabBlock(const uint8_t* data, size_t size, ParseResult& result, int casm_section_idx) {
    constexpr size_t kMinCtab = 26;
    if (size < kMinCtab) return;

    CasmTrackConfig cfg{};
    cfg.source_channel = data[0];
    cfg.name = trimCasmName(data + 1, 8);
    cfg.dest_channel = data[9];
    cfg.editable = data[10];
    cfg.note_mute_mask = static_cast<uint16_t>(((data[11] & 0x0F) << 8) | data[12]);
    cfg.chord_mute_mask = (static_cast<uint64_t>(data[13] & 0x03) << 32) |
                          (static_cast<uint64_t>(data[14]) << 24) |
                          (static_cast<uint64_t>(data[15]) << 16) |
                          (static_cast<uint64_t>(data[16]) << 8) |
                           static_cast<uint64_t>(data[17]);
    cfg.source_chord_root = data[18];
    cfg.source_chord_type = data[19];
    cfg.zone_low = 0;
    cfg.zone_high = 127;
    cfg.ntr = data[20];
    cfg.ntt_bass = (data[21] & 0x80) != 0;
    cfg.ntt = data[21] & 0x7F;
    cfg.high_key = data[22];
    cfg.note_low = data[23];
    cfg.note_high = data[24];
    cfg.rtr = data[25];
    cfg.from_sff1_ctab = true;
    cfg.is_megavoice = false;

    if (casm_section_idx >= 0 &&
        casm_section_idx < static_cast<int>(result.casm_sections.size())) {
        result.casm_sections[casm_section_idx].tracks.push_back(cfg);
    }
    result.casm_configs.push_back(std::move(cfg));
}

// Parse legacy SFF1 Cntt companion block (Wierzba/Bedesem Table 11: [0]=srcCh, [1]=ntt with bit7=bass).
void parseCnttBlock(const uint8_t* data, size_t size, ParseResult& result, int casm_section_idx) {
    if (size < 2) return;
    const uint8_t srcCh = data[0];
    const bool bass = (data[1] & 0x80) != 0;
    const uint8_t ntt = data[1] & 0x7F;

    if (casm_section_idx >= 0 &&
        casm_section_idx < static_cast<int>(result.casm_sections.size())) {
        auto& tracks = result.casm_sections[casm_section_idx].tracks;
        bool matchedInSection = false;
        for (auto it = tracks.rbegin(); it != tracks.rend(); ++it) {
            if (it->source_channel == srcCh) {
                it->ntt_bass = bass;
                it->ntt = ntt;
                matchedInSection = true;
                break;
            }
        }
        // Only update the matching entry belonging to this section in flat casm_configs
        // (never clobbering an earlier section's track config).
        if (matchedInSection) {
            const size_t limit = std::min(tracks.size(), result.casm_configs.size());
            for (size_t k = 0; k < limit; ++k) {
                auto& cfg = result.casm_configs[result.casm_configs.size() - 1 - k];
                if (cfg.source_channel == srcCh) {
                    cfg.ntt_bass = bass;
                    cfg.ntt = ntt;
                    break;
                }
            }
        }
    } else {
        for (auto it = result.casm_configs.rbegin(); it != result.casm_configs.rend(); ++it) {
            if (it->source_channel == srcCh) {
                it->ntt_bass = bass;
                it->ntt = ntt;
                break;
            }
        }
    }
}

} // namespace

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

        if (sub_size > size - pos) break;

        if (sub_id == "Ctb2") {
            parseCtb2Block(data + pos, sub_size);
        } else if (sub_id == "Ctab") {
            parseCtabBlock(data + pos, sub_size, result_, casm_section_idx_);
        } else if (sub_id == "Cntt") {
            parseCnttBlock(data + pos, sub_size, result_, casm_section_idx_);
        } else if (sub_id == "CSEG") {
            // Inner CSEG — another section
            parseCasm(data + pos, sub_size, depth + 1);
        } else if (sub_id == "Sdec") {
            // Section definition — extract section name and open a new
            // CASM section. Subsequent Ctb2 / Ctab / Cntt blocks attach to it.
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
    // A Ctb2 sub-chunk holds one source-channel configuration (Wierzba/Bedesem
    // "Style Files" v2.1, Tables 12-16):
    //   [0]      Source Channel        [1..8] Name   [9] Destination Channel
    //   [10]     Editable flag         [11..17] Note/chord mute bitfields
    //   [18..19] Source chord (root, type)
    //   [20]     Middle zone lowest note  (0   = low zone unused)
    //   [21]     Middle zone highest note (127 = high zone unused)
    //   [22..27] Low zone    {NTR, NTT, High Key, Note Low, Note High, RTR}
    //   [28..33] Middle zone (same layout)
    //   [34..39] High zone   (same layout)
    //   [40..46] Undocumented
    constexpr size_t kMinEntry = 34;   // need the middle zone [28..33]
    if (size < kMinEntry) return;      // buffer too short — skip, no crash

    CasmTrackConfig cfg{};
    cfg.source_channel = data[0];
    cfg.name = trimCasmName(data + 1, 8);
    cfg.dest_channel = data[9];
    cfg.editable = data[10];
    cfg.note_mute_mask = static_cast<uint16_t>(((data[11] & 0x0F) << 8) | data[12]);
    cfg.chord_mute_mask = (static_cast<uint64_t>(data[13] & 0x03) << 32) |
                          (static_cast<uint64_t>(data[14]) << 24) |
                          (static_cast<uint64_t>(data[15]) << 16) |
                          (static_cast<uint64_t>(data[16]) << 8) |
                           static_cast<uint64_t>(data[17]);
    cfg.source_chord_root = data[18];
    cfg.source_chord_type = data[19];
    cfg.zone_low  = data[20];
    cfg.zone_high = data[21];
    cfg.low_zone  = readZone(data + 22);
    const CasmZoneConfig mid = readZone(data + 28);
    cfg.ntr       = mid.ntr;
    cfg.ntt_bass  = mid.ntt_bass;
    cfg.ntt       = mid.ntt;
    cfg.high_key  = mid.high_key;
    cfg.note_low  = mid.note_low;
    cfg.note_high = mid.note_high;
    cfg.rtr       = mid.rtr;
    if (size >= 40) {
        cfg.high_zone = readZone(data + 34);
    }
    cfg.from_sff1_ctab = false;

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
