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
    style.tempo_bpm = 120;
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
