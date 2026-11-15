#include "engine/uasf/deserializer.h"
#include <fstream>
#include <cstring>

namespace ai_arranger::uasf {

DeserializeResult UasfDeserializer::deserialize(const std::vector<uint8_t>& data) noexcept {
    data_ = data.data();
    size_ = data.size();
    pos_ = 0;
    error_ = false;
    error_msg_ = "";

    DeserializeResult result;
    result.success = false;

    // Check minimum size
    if (size_ < sizeof(format::FileHeader)) {
        result.error = "File too small: not a valid UASF file";
        return result;
    }

    // Read file header
    format::FileHeader header;
    readBytes(&header, sizeof(header));
    if (error_) { result.error = error_msg_; return result; }

    if (header.magic != format::kMagic) {
        result.error = "Invalid magic: not a UASF file (expected UASF)";
        return result;
    }

    if (header.version != format::kVersion) {
        result.error = "Unsupported version: " + std::to_string(header.version);
        return result;
    }

    // Structural limits (format.h). The serializer clamps to them, so a larger
    // count means a corrupt or crafted file; rejecting it also bounds memory.
    if (header.section_count > format::kMaxSections) {
        result.error = "Too many sections: " + std::to_string(header.section_count) +
                       " (max " + std::to_string(format::kMaxSections) + ")";
        return result;
    }

    auto& style = result.style;
    style.format_version = "1.0";
    style.tempo_bpm = kDefaultTempoBpm;   // unless a tempo block says otherwise
    style.resolution = 480;

    // Read sections
    for (uint32_t s = 0; s < header.section_count; ++s) {
        if (!ensure(sizeof(format::SectionHeader))) {
            result.error = "Truncated section header";
            return result;
        }

        format::SectionHeader sect_hdr;
        readBytes(&sect_hdr, sizeof(sect_hdr));

        SectionDefinition section;
        section.type = static_cast<SectionType>(sect_hdr.type);
        section.name = "Section " + std::to_string(s);
        section.bars = sect_hdr.bars;
        section.resolution = sect_hdr.resolution;
        section.beats_per_bar = sect_hdr.beats_per_bar;
        section.beat_note = sect_hdr.beat_note;

        if (sect_hdr.track_count > format::kMaxTracksPerSection) {
            result.error = "Too many tracks in section " + std::to_string(s) + ": " +
                           std::to_string(sect_hdr.track_count) +
                           " (max " + std::to_string(format::kMaxTracksPerSection) + ")";
            return result;
        }

        // Read tracks
        for (uint8_t t = 0; t < sect_hdr.track_count; ++t) {
            if (!ensure(sizeof(format::TrackHeader))) {
                result.error = "Truncated track header";
                return result;
            }

            format::TrackHeader trk_hdr;
            readBytes(&trk_hdr, sizeof(trk_hdr));

            TrackDefinition track;
            track.midi_channel = trk_hdr.midi_channel;
            track.role = static_cast<TrackRole>(trk_hdr.role);
            track.is_drum = trk_hdr.is_drum != 0;

            if (trk_hdr.name_length > format::kMaxTrackNameLength) {
                result.error = "Track name too long: " + std::to_string(trk_hdr.name_length);
                return result;
            }
            if (trk_hdr.event_count > format::kMaxEvents) {
                result.error = "Too many events in track: " + std::to_string(trk_hdr.event_count) +
                               " (max " + std::to_string(format::kMaxEvents) + ")";
                return result;
            }

            // Track name
            if (trk_hdr.name_length > 0) {
                if (!ensure(trk_hdr.name_length)) {
                    result.error = "Truncated track name";
                    return result;
                }
                track.name = readString(trk_hdr.name_length);
            }

            // Articulation metadata
            track.articulation.profile = static_cast<ArticulationProfile>(trk_hdr.articulation_profile);
            track.articulation.fidelity = static_cast<FidelityRequirement>(trk_hdr.fidelity);
            track.articulation.recommended.external_yamaha = (trk_hdr.playback_flags & format::PF_EXTERNAL_YAMAHA) != 0;
            track.articulation.recommended.external_midi   = (trk_hdr.playback_flags & format::PF_EXTERNAL_MIDI) != 0;
            track.articulation.recommended.premium_library = (trk_hdr.playback_flags & format::PF_PREMIUM_LIBRARY) != 0;
            track.articulation.recommended.user_soundfont  = (trk_hdr.playback_flags & format::PF_USER_SOUNDFONT) != 0;
            track.articulation.recommended.degradation_allowed = (trk_hdr.playback_flags & format::PF_DEGRADATION_OK) != 0;

            // Events
            uint64_t accumulated_tick = 0;
            for (uint32_t e = 0; e < trk_hdr.event_count; ++e) {
                if (!ensure(sizeof(format::MidiEventSerialized))) {
                    result.error = "Truncated event data";
                    return result;
                }

                format::MidiEventSerialized ev;
                readBytes(&ev, sizeof(ev));

                MidiEvent midi_event;
                accumulated_tick += ev.tick_delta;
                midi_event.tick = accumulated_tick;
                midi_event.type = static_cast<MidiEventType>(ev.type_channel & 0xF0);
                midi_event.channel = ev.type_channel & 0x0F;
                midi_event.data1 = ev.data1;
                midi_event.data2 = ev.data2;

                track.events.push_back(midi_event);
            }

            section.tracks.push_back(std::move(track));
        }

        style.sections.push_back(std::move(section));
    }

    // ── Extension blocks (v1.1): only when flagged; v1.0 files never set the
    // flag, and trailing bytes without it are ignored as before. ─────────────
    if (header.flags & format::kFlagExtensions) {
        bool haveTempo = false, haveTrns = false, haveKexp = false, haveSnam = false;
        while (pos_ < size_) {
            if (!ensure(sizeof(format::ExtensionHeader))) {
                result.error = "Truncated extension header";
                return result;
            }
            format::ExtensionHeader ext;
            readBytes(&ext, sizeof(ext));
            if (ext.length > size_ - pos_) {
                result.error = "Truncated extension block";
                return result;
            }
            if (ext.tag == format::kExtTranspose) {
                if (haveTrns || ext.length % 5 != 0) {
                    result.error = haveTrns ? "Duplicate transposition block" : "Malformed transposition block";
                    return result;
                }
                for (uint32_t k = 0; k < ext.length / 5; ++k) {
                    const uint8_t si = readU8(), ti = readU8();
                    const uint8_t hk = readU8(), lo = readU8(), hi = readU8();
                    if (si >= style.sections.size() || ti >= style.sections[si].tracks.size() ||
                        !ArticulationMetadata::registerLimitsValid(hk, lo, hi)) {
                        result.error = "Invalid transposition entry " + std::to_string(k);
                        return result;
                    }
                    auto& a = style.sections[si].tracks[ti].articulation;
                    a.high_key = hk; a.note_low = lo; a.note_high = hi;
                }
                haveTrns = true;
                continue;
            }
            if (ext.tag == format::kExtSectionNames) {
                const size_t end = pos_ + ext.length;
                int last = -1;
                if (haveSnam) { result.error = "Duplicate section name block"; return result; }
                while (pos_ < end) {
                    if (end - pos_ < 2) { result.error = "Malformed section name block"; return result; }
                    const uint8_t si = readU8(), len = readU8();
                    if (si <= last || si >= style.sections.size() || len < 1 || len > 64 || len > end - pos_) {
                        result.error = "Invalid section name entry";
                        return result;
                    }
                    style.sections[si].name = readString(len);
                    style.sections[si].keep_name = true;
                    last = si;
                }
                haveSnam = true;
                continue;
            }
            if (ext.tag == format::kExtKorgExport) {
                const size_t end = pos_ + ext.length;
                auto bad = [&](const char* why) { result.error = std::string("Malformed Korg export block: ") + why; return result; };
                if (haveKexp) return bad("duplicate");
                auto& k = style.korg_export;
                auto u8 = [&](uint8_t& v) { if (pos_ >= end) return false; v = readU8(); return true; };
                uint8_t ver = 0, n = 0;
                if (!u8(ver) || ver != 1) return bad("version");
                if (!u8(n)) return bad("truncated");
                for (uint8_t i = 0; i < n; ++i) {
                    uint8_t a, b;
                    if (!u8(a) || !u8(b)) return bad("truncated");
                    if (a > 15 || b > 15 || !k.route.emplace(a, b).second) return bad("route entry");
                }
                if (!u8(n)) return bad("truncated");
                for (uint8_t i = 0; i < n; ++i) {
                    uint8_t ch, m, l, p;
                    if (!u8(ch) || !u8(m) || !u8(l) || !u8(p)) return bad("truncated");
                    if (ch < 8 || ch > 15 || m > 127 || l > 127 || p > 127 || !k.sounds.emplace(ch, KorgSound{m, l, p}).second) return bad("sound entry");
                }
                uint8_t lo, hi;
                if (!u8(lo) || !u8(hi)) return bad("truncated");
                for (uint32_t i = 0, e = lo | uint32_t{hi} << 8; i < e; ++i) {
                    uint8_t t, ch, m, l, p;
                    if (!u8(t) || !u8(ch) || !u8(m) || !u8(l) || !u8(p)) return bad("truncated");
                    if (t > static_cast<uint8_t>(SectionType::Break) || ch < 8 || ch > 15 || m > 127 || l > 127 || p > 127 ||
                        !k.element_sounds.emplace(std::make_pair(static_cast<SectionType>(t), ch), KorgSound{m, l, p}).second)
                        return bad("element sound entry");
                }
                if (pos_ != end) return bad("trailing bytes");
                haveKexp = true;
                continue;
            }
            if (ext.tag != format::kExtTempo) {   // unknown: keep verbatim for the writer
                const uint8_t* blk = data_ + pos_ - sizeof(ext);
                style.unknown_extensions.insert(style.unknown_extensions.end(), blk,
                                                blk + sizeof(ext) + ext.length);
                pos_ += ext.length;
                continue;
            }
            if (haveTempo || ext.length != 4) {
                result.error = haveTempo ? "Duplicate tempo block" : "Malformed tempo block";
                return result;
            }
            const uint32_t bpm = readU32();
            if (bpm < kMinTempoBpm || bpm > kMaxTempoBpm) {
                result.error = "Tempo out of range: " + std::to_string(bpm);
                return result;
            }
            style.tempo_bpm = bpm;
            haveTempo = true;
        }
    }

    // The file header carries no style-level resolution, so recover it from
    // the (round-tripped) per-section resolution. Without this the playback
    // clock would default to 480 PPQN and play higher-PPQN styles (e.g. 1920)
    // at the wrong tempo — effectively silent.
    for (const auto& sec : style.sections) {
        if (sec.resolution > 0) { style.resolution = sec.resolution; break; }
    }

    result.success = true;
    return result;
}

DeserializeResult UasfDeserializer::deserializeFromFile(const std::string& path) noexcept {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {false, {}, "Cannot open file: " + path};
    }

    size_t fileSize = file.tellg();
    if (fileSize > format::kMaxFileSize || fileSize < format::kMinFileSize) {
        return {false, {}, "File size out of range"};
    }

    std::vector<uint8_t> data(fileSize);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();

    return deserialize(data);
}

// ── Reader helpers ─────────────────────────────────────────────────

bool UasfDeserializer::ensure(size_t bytes) noexcept {
    if (error_) return false;
    if (pos_ + bytes > size_) {
        error_ = true;
        error_msg_ = "Read past end of buffer";
        return false;
    }
    return true;
}

uint8_t UasfDeserializer::readU8() noexcept {
    if (!ensure(1)) return 0;
    return data_[pos_++];
}

uint16_t UasfDeserializer::readU16() noexcept {
    if (!ensure(2)) return 0;
    uint16_t v = data_[pos_] | (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
}

uint32_t UasfDeserializer::readU32() noexcept {
    if (!ensure(4)) return 0;
    uint32_t v = data_[pos_] |
                 (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                 (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                 (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
}

uint64_t UasfDeserializer::readU64() noexcept {
    uint64_t v = 0;
    if (!ensure(8)) return 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
    }
    pos_ += 8;
    return v;
}

void UasfDeserializer::readBytes(void* dest, size_t size) noexcept {
    if (!ensure(size)) return;
    std::memcpy(dest, data_ + pos_, size);
    pos_ += size;
}

std::string UasfDeserializer::readString(uint8_t length) noexcept {
    if (length == 0 || !ensure(length)) return {};
    std::string s(reinterpret_cast<const char*>(data_ + pos_), length);
    pos_ += length;
    return s;
}

} // namespace ai_arranger::uasf
