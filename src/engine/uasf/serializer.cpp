#include "engine/uasf/serializer.h"
#include <cstring>
#include <algorithm>
#include <string>

namespace ai_arranger::uasf {

SerializeResult UasfSerializer::serialize(const StyleDefinition& style) noexcept {
    buffer_.clear();
    buffer_.reserve(65536); // Pre-allocate

    // A tempo the reader would reject must not be written (0 = no tempo stored).
    if (style.tempo_bpm != 0 &&
        (style.tempo_bpm < kMinTempoBpm || style.tempo_bpm > kMaxTempoBpm)) {
        return {false, {}, "Tempo " + std::to_string(style.tempo_bpm) + " BPM out of range (" +
                               std::to_string(kMinTempoBpm) + "-" + std::to_string(kMaxTempoBpm) + ")"};
    }

    // SectionHeader stores bars and resolution as uint16: refuse rather than
    // truncate (a truncated length plays a different, shorter section).
    for (size_t i = 0; i < style.sections.size() && i < format::kMaxSections; ++i) {
        const auto& sec = style.sections[i];
        if (sec.bars > 0xFFFF || sec.resolution > 0xFFFF) {
            return {false, {}, "Section " + std::to_string(i) + ": bars/resolution exceed 65535"};
        }
    }

    // ── File header ────────────────────────────────────────────────
    format::FileHeader header;
    header.magic = format::kMagic;
    header.version = format::kVersion;
    std::vector<uint8_t> trns;   // 'TRNS' payload, written sections/tracks only
    for (size_t si = 0; si < style.sections.size() && si < format::kMaxSections; ++si) {
        const auto& tracks = style.sections[si].tracks;
        for (size_t ti = 0; ti < tracks.size() && ti < format::kMaxTracksPerSection; ++ti) {
            const auto& a = tracks[ti].articulation;
            if (!a.hasRegisterLimits()) continue;
            if (!ArticulationMetadata::registerLimitsValid(a.high_key, a.note_low, a.note_high))
                return {false, {}, "Section " + std::to_string(si) + " track " + std::to_string(ti) +
                                       ": invalid register limits (High Key 0-11, note range >= 1 octave)"};
            trns.insert(trns.end(), {static_cast<uint8_t>(si), static_cast<uint8_t>(ti),
                                     a.high_key, a.note_low, a.note_high});
        }
    }
    std::vector<uint8_t> snam;   // 'SNAM' payload: names the reader would not restore by itself
    for (size_t si = 0; si < style.sections.size() && si < format::kMaxSections; ++si) {
        const auto& n = style.sections[si].name;
        if (!style.sections[si].keep_name || n.empty()) continue;   // only keep_name sections are written into 'SNAM'
        if (n.size() > 64) return {false, {}, "Section " + std::to_string(si) + ": name longer than 64 bytes"};
        snam.push_back(static_cast<uint8_t>(si));
        snam.push_back(static_cast<uint8_t>(n.size()));
        snam.insert(snam.end(), n.begin(), n.end());
    }
    std::vector<uint8_t> kexp;   // 'KEXP' payload
    if (!style.korg_export.empty()) {
        const auto& k = style.korg_export;
        if (k.route.size() > 16 || k.sounds.size() > 8 || k.element_sounds.size() > 15 * 8)
            return {false, {}, "Korg export settings: too many entries"};
        auto ok7 = [](const KorgSound& s) { return s.msb <= 127 && s.lsb <= 127 && s.pc <= 127; };
        kexp.push_back(1);
        kexp.push_back(static_cast<uint8_t>(k.route.size()));
        for (const auto& [src, dst] : k.route) {
            if (src > 15 || dst > 15) return {false, {}, "Korg export settings: route channel out of range"};
            kexp.insert(kexp.end(), {src, dst});
        }
        kexp.push_back(static_cast<uint8_t>(k.sounds.size()));
        for (const auto& [ch, s] : k.sounds) {
            if (ch < 8 || ch > 15 || !ok7(s)) return {false, {}, "Korg export settings: invalid sound"};
            kexp.insert(kexp.end(), {ch, s.msb, s.lsb, s.pc});
        }
        kexp.push_back(static_cast<uint8_t>(k.element_sounds.size() & 0xFF));
        kexp.push_back(static_cast<uint8_t>(k.element_sounds.size() >> 8));
        for (const auto& [key, s] : k.element_sounds) {
            if (static_cast<uint8_t>(key.first) > static_cast<uint8_t>(SectionType::Break) || key.second < 8 || key.second > 15 || !ok7(s))
                return {false, {}, "Korg export settings: invalid element sound"};
            kexp.insert(kexp.end(), {static_cast<uint8_t>(key.first), key.second, s.msb, s.lsb, s.pc});
        }
    }
    std::vector<uint8_t> casm;   // 'CASM' payload (v1.2 additive extension, 34 bytes/record)
    auto zoneValid = [](const CasmZoneParams& z) {
        return z.ntt <= 127 && z.high_key <= 11 && z.note_low <= 127 &&
               z.note_high <= 127 && z.note_low <= z.note_high;
    };
    for (size_t si = 0; si < style.sections.size() && si < format::kMaxSections; ++si) {
        const auto& tracks = style.sections[si].tracks;
        for (size_t ti = 0; ti < tracks.size() && ti < format::kMaxTracksPerSection; ++ti) {
            const auto& a = tracks[ti].articulation;
            if (!a.hasCasmMetadata()) continue;
            if (a.ntt > 127 || a.source_chord_root > 11 || a.note_mute_mask > 0x0FFF ||
                (a.chord_mute_mask >> 34) != 0 || a.zone_low > 127 || a.zone_high > 127 ||
                a.zone_low > a.zone_high || (a.dest_channel > 15 && a.dest_channel != 0xFF) ||
                !zoneValid(a.low_zone) || !zoneValid(a.high_zone)) {
                return {false, {}, "Section " + std::to_string(si) + " track " + std::to_string(ti) +
                                       ": invalid CASM metadata"};
            }
            if (casm.empty()) casm.push_back(1);  // version = 1
            casm.push_back(static_cast<uint8_t>(si));
            casm.push_back(static_cast<uint8_t>(ti));
            casm.push_back(a.ntr);
            casm.push_back(a.ntt);
            casm.push_back(a.ntt_bass ? 1 : 0);
            casm.push_back(a.rtr);
            casm.push_back(a.source_chord_root);
            casm.push_back(a.source_chord_type);
            casm.push_back(static_cast<uint8_t>(a.note_mute_mask & 0xFF));
            casm.push_back(static_cast<uint8_t>((a.note_mute_mask >> 8) & 0xFF));
            for (int b = 0; b < 5; ++b) {
                casm.push_back(static_cast<uint8_t>((a.chord_mute_mask >> (8 * b)) & 0xFF));
            }
            casm.push_back(a.zone_low);
            casm.push_back(a.zone_high);
            casm.push_back(a.dest_channel);
            casm.push_back(a.casm_editable);
            auto writeZone = [&](const CasmZoneParams& z) {
                casm.push_back(z.ntr);
                casm.push_back(z.ntt);
                casm.push_back(z.ntt_bass ? 1 : 0);
                casm.push_back(z.high_key);
                casm.push_back(z.note_low);
                casm.push_back(z.note_high);
                casm.push_back(z.rtr);
            };
            writeZone(a.low_zone);
            writeZone(a.high_zone);
            casm.push_back(0);  // reserved
        }
    }
    std::vector<uint8_t> xgsx;   // 'XGSX' payload (additive XG SysEx metadata)
    if (!style.xg_sysex.empty()) {
        const auto& xg = style.xg_sysex;
        auto pushU16 = [&](uint16_t v) {
            xgsx.push_back(static_cast<uint8_t>(v & 0xFF));
            xgsx.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        };
        auto pushU32 = [&](uint32_t v) {
            xgsx.push_back(static_cast<uint8_t>(v & 0xFF));
            xgsx.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            xgsx.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            xgsx.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        };
        auto eqOk = [](int8_t g) { return g >= -12 && g <= 12; };
        auto offOk = [](int8_t o) { return o >= -64 && o <= 63; };

        if (xg.parts.size() > 16 || xg.drum_overrides.size() > 512 || xg.advisory_codes.size() > 32) {
            return {false, {}, "XG SysEx metadata: too many entries"};
        }
        if ((xg.master_volume && *xg.master_volume > 127) ||
            (xg.master_eq_type && *xg.master_eq_type > 127) ||
            (xg.master_eq_low_gain_db && !eqOk(*xg.master_eq_low_gain_db)) ||
            (xg.master_eq_mid_gain_db && !eqOk(*xg.master_eq_mid_gain_db)) ||
            (xg.master_eq_high_gain_db && !eqOk(*xg.master_eq_high_gain_db))) {
            return {false, {}, "XG SysEx metadata: invalid master parameter"};
        }

        xgsx.push_back(1);  // version = 1
        pushU32(xg.total_sysex_seen);
        pushU32(xg.recognized_xg_count);
        pushU32(xg.unrecognized_count);
        pushU32(xg.malformed_count);
        pushU32(xg.dsp_sysex_count);

        uint8_t mode_flags = 0;
        if (xg.xg_system_on)  mode_flags |= (1u << 0);
        if (xg.gm1_system_on) mode_flags |= (1u << 1);
        if (xg.gm2_system_on) mode_flags |= (1u << 2);
        if (xg.gs_reset)      mode_flags |= (1u << 3);
        xgsx.push_back(mode_flags);

        uint8_t opt_flags = 0;
        if (xg.reverb_type)            opt_flags |= (1u << 0);
        if (xg.chorus_type)            opt_flags |= (1u << 1);
        if (xg.variation_type)         opt_flags |= (1u << 2);
        if (xg.master_volume)          opt_flags |= (1u << 3);
        if (xg.master_eq_type)         opt_flags |= (1u << 4);
        if (xg.master_eq_low_gain_db)  opt_flags |= (1u << 5);
        if (xg.master_eq_mid_gain_db)  opt_flags |= (1u << 6);
        if (xg.master_eq_high_gain_db) opt_flags |= (1u << 7);
        xgsx.push_back(opt_flags);

        if (xg.reverb_type)            pushU16(*xg.reverb_type);
        if (xg.chorus_type)            pushU16(*xg.chorus_type);
        if (xg.variation_type)         pushU16(*xg.variation_type);
        if (xg.master_volume)          xgsx.push_back(*xg.master_volume);
        if (xg.master_eq_type)         xgsx.push_back(*xg.master_eq_type);
        if (xg.master_eq_low_gain_db)  xgsx.push_back(static_cast<uint8_t>(*xg.master_eq_low_gain_db));
        if (xg.master_eq_mid_gain_db)  xgsx.push_back(static_cast<uint8_t>(*xg.master_eq_mid_gain_db));
        if (xg.master_eq_high_gain_db) xgsx.push_back(static_cast<uint8_t>(*xg.master_eq_high_gain_db));

        xgsx.push_back(static_cast<uint8_t>(xg.parts.size()));
        for (const auto& [part, p] : xg.parts) {
            if (part > 15 ||
                (p.bank_msb && *p.bank_msb > 127) ||
                (p.bank_lsb && *p.bank_lsb > 127) ||
                (p.program && *p.program > 127) ||
                (p.part_mode && *p.part_mode > 5) ||
                (p.volume && *p.volume > 127) ||
                (p.pan && *p.pan > 127) ||
                (p.dry_level && *p.dry_level > 127) ||
                (p.chorus_send && *p.chorus_send > 127) ||
                (p.reverb_send && *p.reverb_send > 127) ||
                (p.variation_send && *p.variation_send > 127) ||
                (p.filter_cutoff_offset && !offOk(*p.filter_cutoff_offset)) ||
                (p.filter_res_offset && !offOk(*p.filter_res_offset)) ||
                (p.eq_low_gain_db && !eqOk(*p.eq_low_gain_db)) ||
                (p.eq_mid_gain_db && !eqOk(*p.eq_mid_gain_db)) ||
                (p.eq_high_gain_db && !eqOk(*p.eq_high_gain_db))) {
                return {false, {}, "XG SysEx metadata: invalid part parameter"};
            }
            uint16_t mask = 0;
            if (p.bank_msb)             mask |= (1u << 0);
            if (p.bank_lsb)             mask |= (1u << 1);
            if (p.program)              mask |= (1u << 2);
            if (p.part_mode)            mask |= (1u << 3);
            if (p.volume)               mask |= (1u << 4);
            if (p.pan)                  mask |= (1u << 5);
            if (p.dry_level)            mask |= (1u << 6);
            if (p.chorus_send)          mask |= (1u << 7);
            if (p.reverb_send)          mask |= (1u << 8);
            if (p.variation_send)       mask |= (1u << 9);
            if (p.filter_cutoff_offset) mask |= (1u << 10);
            if (p.filter_res_offset)    mask |= (1u << 11);
            if (p.eq_low_gain_db)       mask |= (1u << 12);
            if (p.eq_mid_gain_db)       mask |= (1u << 13);
            if (p.eq_high_gain_db)      mask |= (1u << 14);
            uint8_t hints = 0;
            if (p.megavoice_hint) hints |= (1u << 0);
            if (p.revo_drum_hint) hints |= (1u << 1);

            xgsx.push_back(part);
            pushU16(mask);
            xgsx.push_back(hints);
            if (p.bank_msb)             xgsx.push_back(*p.bank_msb);
            if (p.bank_lsb)             xgsx.push_back(*p.bank_lsb);
            if (p.program)              xgsx.push_back(*p.program);
            if (p.part_mode)            xgsx.push_back(*p.part_mode);
            if (p.volume)               xgsx.push_back(*p.volume);
            if (p.pan)                  xgsx.push_back(*p.pan);
            if (p.dry_level)            xgsx.push_back(*p.dry_level);
            if (p.chorus_send)          xgsx.push_back(*p.chorus_send);
            if (p.reverb_send)          xgsx.push_back(*p.reverb_send);
            if (p.variation_send)       xgsx.push_back(*p.variation_send);
            if (p.filter_cutoff_offset) xgsx.push_back(static_cast<uint8_t>(*p.filter_cutoff_offset));
            if (p.filter_res_offset)    xgsx.push_back(static_cast<uint8_t>(*p.filter_res_offset));
            if (p.eq_low_gain_db)       xgsx.push_back(static_cast<uint8_t>(*p.eq_low_gain_db));
            if (p.eq_mid_gain_db)       xgsx.push_back(static_cast<uint8_t>(*p.eq_mid_gain_db));
            if (p.eq_high_gain_db)      xgsx.push_back(static_cast<uint8_t>(*p.eq_high_gain_db));
        }

        pushU16(static_cast<uint16_t>(xg.drum_overrides.size()));
        for (const auto& d : xg.drum_overrides) {
            if (d.drum_setup > 3 || d.key > 127 ||
                (d.level && *d.level > 127) ||
                (d.pan && *d.pan > 127) ||
                (d.reverb_send && *d.reverb_send > 127) ||
                (d.pitch_coarse && !offOk(*d.pitch_coarse))) {
                return {false, {}, "XG SysEx metadata: invalid drum override"};
            }
            uint8_t dmask = 0;
            if (d.level)        dmask |= (1u << 0);
            if (d.pan)          dmask |= (1u << 1);
            if (d.reverb_send)  dmask |= (1u << 2);
            if (d.pitch_coarse) dmask |= (1u << 3);
            xgsx.push_back(d.drum_setup);
            xgsx.push_back(d.key);
            xgsx.push_back(dmask);
            if (d.level)        xgsx.push_back(*d.level);
            if (d.pan)          xgsx.push_back(*d.pan);
            if (d.reverb_send)  xgsx.push_back(*d.reverb_send);
            if (d.pitch_coarse) xgsx.push_back(static_cast<uint8_t>(*d.pitch_coarse));
        }

        xgsx.push_back(static_cast<uint8_t>(xg.advisory_codes.size()));
        for (const auto& code : xg.advisory_codes) {
            if (code.empty() || code.size() > 64) {
                return {false, {}, "XG SysEx metadata: invalid advisory code length"};
            }
            xgsx.push_back(static_cast<uint8_t>(code.size()));
            xgsx.insert(xgsx.end(), code.begin(), code.end());
        }
    }
    header.flags = (style.tempo_bpm != 0 || !trns.empty() || !kexp.empty() || !snam.empty() ||
                    !casm.empty() || !xgsx.empty() || !style.unknown_extensions.empty())
                       ? format::kFlagExtensions : 0;
    header.section_count = static_cast<uint32_t>(std::min(style.sections.size(),
                                                         size_t(format::kMaxSections)));

    writeBytes(&header, sizeof(header));

    // ── Sections ───────────────────────────────────────────────────
    uint64_t global_tick_offset = 0;

    // Write exactly the counted sections/tracks: anything past the header count
    // would desynchronise the reader (and land where it expects extensions).
    for (uint32_t si = 0; si < header.section_count; ++si) {
        const auto& section = style.sections[si];
        // Section header
        format::SectionHeader sect_hdr;
        sect_hdr.type = static_cast<uint8_t>(section.type);
        sect_hdr.bars = static_cast<uint16_t>(section.bars);
        sect_hdr.resolution = static_cast<uint16_t>(section.resolution);
        sect_hdr.beats_per_bar = static_cast<uint8_t>(section.beats_per_bar);
        sect_hdr.beat_note = static_cast<uint8_t>(section.beat_note);
        sect_hdr.track_count = static_cast<uint8_t>(std::min(section.tracks.size(),
                                                             size_t(format::kMaxTracksPerSection)));
        sect_hdr.padding = 0;

        writeBytes(&sect_hdr, sizeof(sect_hdr));

        // Tracks
        for (uint32_t ti = 0; ti < sect_hdr.track_count; ++ti) {
            const auto& track = section.tracks[ti];
            format::TrackHeader trk_hdr;
            trk_hdr.name_length = static_cast<uint8_t>(std::min(track.name.size(),
                                                                size_t(format::kMaxTrackNameLength)));
            trk_hdr.midi_channel = track.midi_channel;
            trk_hdr.role = static_cast<uint8_t>(track.role);
            trk_hdr.is_drum = track.is_drum ? 1 : 0;
            trk_hdr.articulation_profile = static_cast<uint8_t>(track.articulation.profile);
            trk_hdr.fidelity = static_cast<uint8_t>(track.articulation.fidelity);
            trk_hdr.playback_flags = 0;
            if (track.articulation.recommended.external_yamaha) trk_hdr.playback_flags |= format::PF_EXTERNAL_YAMAHA;
            if (track.articulation.recommended.external_midi)   trk_hdr.playback_flags |= format::PF_EXTERNAL_MIDI;
            if (track.articulation.recommended.premium_library) trk_hdr.playback_flags |= format::PF_PREMIUM_LIBRARY;
            if (track.articulation.recommended.user_soundfont)  trk_hdr.playback_flags |= format::PF_USER_SOUNDFONT;
            if (track.articulation.recommended.degradation_allowed) trk_hdr.playback_flags |= format::PF_DEGRADATION_OK;

            trk_hdr.event_count = static_cast<uint32_t>(std::min(track.events.size(),
                                                                  size_t(format::kMaxEvents)));

            writeBytes(&trk_hdr, sizeof(trk_hdr));

            // Track name
            if (trk_hdr.name_length > 0) {
                writeBytes(track.name.data(), trk_hdr.name_length);
            }

            // Events (delta-encoded). tick_delta is UNSIGNED, so events must be
            // written in non-decreasing tick order: a track stored out of time
            // order (e.g. the demo drums, grouped per instrument) would otherwise
            // underflow and be clamped, shifting every later event by ~2^32
            // ticks on load. Stable sort keeps same-tick order (NoteOff before a
            // retriggered NoteOn). Exactly event_count events are written — the
            // header count is capped at kMaxEvents, and writing more would
            // desynchronise the rest of the file.
            std::vector<const MidiEvent*> ordered;
            ordered.reserve(trk_hdr.event_count);
            for (uint32_t i = 0; i < trk_hdr.event_count; ++i)
                ordered.push_back(&track.events[i]);
            std::stable_sort(ordered.begin(), ordered.end(),
                             [](const MidiEvent* x, const MidiEvent* y) { return x->tick < y->tick; });

            uint64_t last_tick = global_tick_offset;
            for (const MidiEvent* evp : ordered) {
                const MidiEvent& event = *evp;
                format::MidiEventSerialized ev;
                ev.tick_delta = deltaEncode(event.tick, last_tick);
                ev.type_channel = (static_cast<uint8_t>(event.type) & 0xF0) | (event.channel & 0x0F);
                ev.data1 = event.data1;
                ev.data2 = event.data2;
                ev.padding = 0;
                writeBytes(&ev, sizeof(ev));
            }
        }
    }

    // ── Extension blocks (v1.1) ────────────────────────────────────
    if (style.tempo_bpm != 0) {
        writeU32(format::kExtTempo);
        writeU32(4);
        writeU32(style.tempo_bpm);
    }
    if (!trns.empty()) {
        writeU32(format::kExtTranspose);
        writeU32(static_cast<uint32_t>(trns.size()));
        writeBytes(trns.data(), trns.size());
    }
    if (!snam.empty()) {
        writeU32(format::kExtSectionNames);
        writeU32(static_cast<uint32_t>(snam.size()));
        writeBytes(snam.data(), snam.size());
    }
    if (!kexp.empty()) {
        writeU32(format::kExtKorgExport);
        writeU32(static_cast<uint32_t>(kexp.size()));
        writeBytes(kexp.data(), kexp.size());
    }
    if (!casm.empty()) {
        writeU32(format::kExtCasm);
        writeU32(static_cast<uint32_t>(casm.size()));
        writeBytes(casm.data(), casm.size());
    }
    if (!xgsx.empty()) {
        writeU32(format::kExtXgSysEx);
        writeU32(static_cast<uint32_t>(xgsx.size()));
        writeBytes(xgsx.data(), xgsx.size());
    }
    // Blocks a reader kept but did not understand, re-emitted verbatim.
    if (!style.unknown_extensions.empty())
        writeBytes(style.unknown_extensions.data(), style.unknown_extensions.size());

    return {true, std::move(buffer_), ""};
}

void UasfSerializer::writeU8(uint8_t v) noexcept {
    buffer_.push_back(v);
}

void UasfSerializer::writeU16(uint16_t v) noexcept {
    buffer_.push_back(static_cast<uint8_t>(v & 0xFF));
    buffer_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void UasfSerializer::writeU32(uint32_t v) noexcept {
    buffer_.push_back(static_cast<uint8_t>(v & 0xFF));
    buffer_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buffer_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buffer_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void UasfSerializer::writeU64(uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        buffer_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

void UasfSerializer::writeBytes(const void* data, size_t size) noexcept {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void UasfSerializer::writeString(const std::string& s) noexcept {
    uint8_t len = static_cast<uint8_t>(std::min(s.size(), size_t(255)));
    writeU8(len);
    if (len > 0) writeBytes(s.data(), len);
}

uint32_t UasfSerializer::deltaEncode(uint64_t tick, uint64_t& last_tick) noexcept {
    uint64_t delta = tick - last_tick;
    last_tick = tick;
    return static_cast<uint32_t>(std::min(delta, uint64_t(0xFFFFFFFF)));
}

} // namespace ai_arranger::uasf
