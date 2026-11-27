// Live Yamaha XG / GM / GS SysEx parsing in Sff1Reader -> Sff1ToUasfMapper -> UASF round-trip.
// Strictly synthetic & offline, zero private data, bounded fail-closed hardening.

#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "engine/uasf/serializer.h"
#include "engine/uasf/deserializer.h"
#include "engine/uasf/format.h"
#include "engine/uasf/validator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::importers::sff1;

namespace {

int passes = 0;
int failures = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } \
} while (0)

void appendVlq(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t tmp[4];
    int n = 0;
    tmp[n++] = static_cast<uint8_t>(v & 0x7Fu);
    v >>= 7;
    while (v > 0 && n < 4) {
        tmp[n++] = static_cast<uint8_t>((v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    for (int i = n - 1; i >= 0; --i) out.push_back(tmp[i]);
}

void addSysExF0(std::vector<uint8_t>& trk, uint32_t delta, const std::vector<uint8_t>& payload, bool addF7 = true) {
    appendVlq(trk, delta);
    trk.push_back(0xF0);
    appendVlq(trk, static_cast<uint32_t>(payload.size() + (addF7 ? 1u : 0u)));
    trk.insert(trk.end(), payload.begin(), payload.end());
    if (addF7) trk.push_back(0xF7);
}

void addSysExF7(std::vector<uint8_t>& trk, uint32_t delta, const std::vector<uint8_t>& bytes) {
    appendVlq(trk, delta);
    trk.push_back(0xF7);
    appendVlq(trk, static_cast<uint32_t>(bytes.size()));
    trk.insert(trk.end(), bytes.begin(), bytes.end());
}

void addMetaMarker(std::vector<uint8_t>& trk, uint32_t delta, const std::string& name) {
    appendVlq(trk, delta);
    trk.push_back(0xFF);
    trk.push_back(0x06);
    appendVlq(trk, static_cast<uint32_t>(name.size()));
    trk.insert(trk.end(), name.begin(), name.end());
}

void addMetaTempo(std::vector<uint8_t>& trk, uint32_t delta, uint32_t mpqn) {
    appendVlq(trk, delta);
    trk.push_back(0xFF);
    trk.push_back(0x51);
    trk.push_back(0x03);
    trk.push_back(static_cast<uint8_t>((mpqn >> 16) & 0xFF));
    trk.push_back(static_cast<uint8_t>((mpqn >> 8) & 0xFF));
    trk.push_back(static_cast<uint8_t>(mpqn & 0xFF));
}

void addMetaEoT(std::vector<uint8_t>& trk, uint32_t delta) {
    appendVlq(trk, delta);
    trk.push_back(0xFF);
    trk.push_back(0x2F);
    trk.push_back(0x00);
}

void addNote(std::vector<uint8_t>& trk, uint32_t delta, uint8_t ch, uint8_t pitch, uint8_t vel, uint32_t dur) {
    appendVlq(trk, delta);
    trk.push_back(static_cast<uint8_t>(0x90u | (ch & 0x0Fu)));
    trk.push_back(pitch);
    trk.push_back(vel);
    appendVlq(trk, dur);
    trk.push_back(static_cast<uint8_t>(0x80u | (ch & 0x0Fu)));
    trk.push_back(pitch);
    trk.push_back(0);
}

std::vector<uint8_t> wrapSmf(const std::vector<uint8_t>& trk, uint16_t ppqn = 480) {
    std::vector<uint8_t> b = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1,
        static_cast<uint8_t>((ppqn >> 8) & 0xFF), static_cast<uint8_t>(ppqn & 0xFF),
        'M', 'T', 'r', 'k'
    };
    const uint32_t n = static_cast<uint32_t>(trk.size());
    b.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(n & 0xFF));
    b.insert(b.end(), trk.begin(), trk.end());
    return b;
}

void putU32LE(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

} // namespace

int main() {
    std::printf("Test: Live XG SysEx parsing in Sff1Reader, Sff1ToUasfMapper & UASF\n");

    // ── 1. Full XG / GM1 / GM2 / GS SysEx Suite in Synthetic .STY ───────
    {
        std::vector<uint8_t> trk;
        addMetaTempo(trk, 0, 500000); // 120 BPM
        addMetaMarker(trk, 0, "SInt");

        // System mode flags
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00});                   // XG System On
        addSysExF0(trk, 0, {0x7E, 0x7F, 0x09, 0x01});                                     // GM1 System On
        addSysExF0(trk, 0, {0x7E, 0x7F, 0x09, 0x03});                                     // GM2 System On
        addSysExF0(trk, 0, {0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41});       // GS Reset

        // Master Volume, Reverb/Chorus/Variation Types, Master EQ
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x00, 0x00, 0x04, 0x6E});                   // Master Vol 110
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x02, 0x01, 0x00, 0x01, 0x11});             // Reverb 0x0111
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x02, 0x01, 0x20, 0x41, 0x02});             // Chorus 0x4102
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x02, 0x01, 0x40, 0x05, 0x00});             // Variation 0x0500
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x02, 0x40, 0x00, 0x02, 0x48});             // Master EQ Type 2 + Low +8 dB
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x02, 0x40, 0x09, 0x3E});                   // Master EQ Mid -2 dB
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x02, 0x40, 0x11, 0x46});                   // Master EQ High +6 dB

        // Part 8: DrumS1 mode + Revo! bank hint (62/100) + program + mix
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x08, 0x07, 0x02});                   // Part 8 Mode = DrumS1
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x08, 0x01, 62});                     // Bank MSB 62
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x08, 0x02, 100});                    // Bank LSB 100 -> Revo!
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x08, 0x03, 0});                      // Program 0

        // Part 11: MegaVoice bank hint (63/120) + full mix/send/filter/EQ
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x01, 63});                     // Bank MSB 63
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x02, 120});                    // Bank LSB 120 -> MegaVoice
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x03, 25});                     // Program 25
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x07, 0});                      // Mode Normal
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x0B, 104});                    // Vol 104
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x0E, 52});                     // Pan 52
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x11, 120});                    // Dry 120
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x12, 30});                     // Chorus 30
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x13, 45});                     // Reverb 45
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x14, 22});                     // Variation 22
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x18, 74});                     // Cutoff +10
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x19, 58});                     // Resonance -6
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x72, 60});                     // EQ Low -4 dB
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x73, 69});                     // EQ High +5 dB

        // Drum Setup 0 key 36 overrides + Yamaha Style DSP SysEx + unknown 7-bit SysEx
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x30, 36, 0x00, 61});                       // Pitch -3
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x30, 36, 0x01, 118});                      // Level 118
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x30, 36, 0x03, 64});                       // Pan 64
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x30, 36, 0x04, 16});                       // Reverb 16
        addSysExF0(trk, 0, {0x43, 0x73, 0x01, 0x51, 0x05, 0x00});                         // Yamaha DSP SysEx
        addSysExF0(trk, 0, {0x7D, 0x01, 0x02, 0x03});                                     // Non-XG valid 7-bit SysEx

        addMetaMarker(trk, 480, "Main A");
        addNote(trk, 0, 8, 36, 100, 240);
        addNote(trk, 0, 11, 60, 90, 240);
        addMetaEoT(trk, 1440);

        Sff1Reader reader;
        const auto pr = reader.parseBuffer(wrapSmf(trk), "xg_full.sty");
        TEST("full XG .sty parses successfully", pr.success);
        const auto& xg = pr.xg_sysex;
        TEST("counts: 35 seen, 34 recognized, 1 unrecognized, 0 malformed, 1 dsp",
             xg.total_sysex_seen == 35 && xg.recognized_xg_count == 34 &&
             xg.unrecognized_count == 1 && xg.malformed_count == 0 && xg.dsp_sysex_count == 1);
        TEST("system flags: XG/GM1/GM2/GS all detected",
             xg.xg_system_on && xg.gm1_system_on && xg.gm2_system_on && xg.gs_reset);
        TEST("master volume, FX types, and Master EQ parsed",
             xg.master_volume == 0x6E && xg.reverb_type == 0x0111u &&
             xg.chorus_type == 0x4102u && xg.variation_type == 0x0500u &&
             xg.master_eq_type == 2 && xg.master_eq_low_gain_db == 8 &&
             xg.master_eq_mid_gain_db == -2 && xg.master_eq_high_gain_db == 6);
        TEST("part 8 drum mode (DrumS1) + Revo! bank hint parsed",
             xg.parts.count(8) && xg.parts.at(8).part_mode == 2 && xg.parts.at(8).isDrumMode() &&
             xg.parts.at(8).bank_msb == 62 && xg.parts.at(8).bank_lsb == 100 &&
             xg.parts.at(8).revo_drum_hint && !xg.parts.at(8).megavoice_hint);
        TEST("part 11 MegaVoice hint + full mix/filter/EQ params parsed",
             xg.parts.count(11) && xg.parts.at(11).megavoice_hint && !xg.parts.at(11).isDrumMode() &&
             xg.parts.at(11).program == 25 && xg.parts.at(11).volume == 104 &&
             xg.parts.at(11).pan == 52 && xg.parts.at(11).dry_level == 120 &&
             xg.parts.at(11).chorus_send == 30 && xg.parts.at(11).reverb_send == 45 &&
             xg.parts.at(11).variation_send == 22 && xg.parts.at(11).filter_cutoff_offset == 10 &&
             xg.parts.at(11).filter_res_offset == -6 && xg.parts.at(11).eq_low_gain_db == -4 &&
             xg.parts.at(11).eq_high_gain_db == 5);
        TEST("drum override and DSP advisory preserved",
             xg.drum_overrides.size() == 1 && xg.drum_overrides[0].key == 36 &&
             xg.drum_overrides[0].pitch_coarse == -3 && xg.drum_overrides[0].level == 118 &&
             xg.advisory_codes.size() == 1 && xg.advisory_codes[0] == kDiagXgDspSysEx);

        // Round-trip: Sff1Reader -> Sff1ToUasfMapper -> UasfSerializer -> UasfDeserializer
        Sff1ToUasfMapper mapper;
        const auto mr = mapper.map(pr);
        TEST("mapper succeeds and propagates xg_sysex to UASF StyleDefinition",
             mr.success && !mr.style.xg_sysex.empty() &&
             mr.style.xg_sysex.xg_system_on && mr.style.xg_sysex.parts.size() == 2);

        // Verify no-CASM fallback honors XG Drum Mode (ch 8) and MegaVoice hint (ch 11)
        bool ch8Drum = false, ch11Mega = false;
        for (const auto& t : mr.style.sections[0].tracks) {
            if (t.midi_channel == 8 && t.role == uasf::TrackRole::Drum && t.is_drum) ch8Drum = true;
            if (t.midi_channel == 11 && t.articulation.profile == uasf::ArticulationProfile::MegaLike) ch11Mega = true;
        }
        TEST("mapper no-CASM fallback honors XG Drum Part Mode (ch 8) and MegaVoice hint (ch 11)",
             ch8Drum && ch11Mega);

        uasf::UasfSerializer ser;
        const auto s1 = ser.serialize(mr.style);
        const auto s2 = ser.serialize(mr.style);
        TEST("UASF serialization deterministic (byte-identical) and validates",
             s1.success && s2.success && s1.data == s2.data &&
             uasf::UasfValidator().validateBinary(s1.data).valid);

        uasf::UasfDeserializer des;
        const auto back = des.deserialize(s1.data);
        const auto& bx = back.style.xg_sysex;
        TEST("UASF round-trip preserves all XG SysEx counters, flags, FX, Master EQ, parts, drum overrides, advisories",
             back.success &&
             bx.total_sysex_seen == 35 && bx.recognized_xg_count == 34 &&
             bx.unrecognized_count == 1 && bx.malformed_count == 0 && bx.dsp_sysex_count == 1 &&
             bx.xg_system_on && bx.gm1_system_on && bx.gm2_system_on && bx.gs_reset &&
             bx.reverb_type == 0x0111u && bx.chorus_type == 0x4102u && bx.variation_type == 0x0500u &&
             bx.master_volume == 0x6E && bx.master_eq_type == 2 &&
             bx.master_eq_low_gain_db == 8 && bx.master_eq_mid_gain_db == -2 &&
             bx.master_eq_high_gain_db == 6 &&
             bx.parts.size() == 2 && bx.parts.at(8).isDrumMode() && bx.parts.at(8).revo_drum_hint &&
             bx.parts.at(11).megavoice_hint && bx.parts.at(11).volume == 104 &&
             bx.parts.at(11).filter_cutoff_offset == 10 && bx.parts.at(11).filter_res_offset == -6 &&
             bx.parts.at(11).eq_low_gain_db == -4 && bx.parts.at(11).eq_high_gain_db == 5 &&
             bx.drum_overrides.size() == 1 && bx.drum_overrides[0].pitch_coarse == -3 &&
             bx.drum_overrides[0].level == 118 && bx.drum_overrides[0].reverb_send == 16 &&
             bx.advisory_codes.size() == 1 && bx.advisory_codes[0] == kDiagXgDspSysEx);

        // Re-serializing the deserialized style must be byte-identical
        const auto s3 = ser.serialize(back.style);
        TEST("serialize(deserialize(x)) byte-identical with XGSX extension",
             s3.success && s3.data == s1.data);
    }

    // ── 2. Multi-Packet Divided 0xF7 Continuation & Escaped SysEx ───────
    {
        std::vector<uint8_t> trk;
        // 2-packet divided XG Part 10 Volume = 0x66
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08}, /*addF7=*/false);
        addSysExF7(trk, 0, {0x0A, 0x0B, 0x66, 0xF7});
        // 3-packet divided XG Reverb Type = 0x0109
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C}, /*addF7=*/false);
        addSysExF7(trk, 0, {0x02, 0x01, 0x00});
        addSysExF7(trk, 0, {0x01, 0x09, 0xF7});
        // Escaped complete SysEx inside 0xF7: F7 <len> F0 43 10 4C 00 00 04 55 F7
        addSysExF7(trk, 0, {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x04, 0x55, 0xF7});
        // Surrounding note event at tick 96
        addNote(trk, 96, 0, 64, 100, 120);
        addMetaEoT(trk, 0);

        Sff1Reader reader;
        const auto pr = reader.parseBuffer(wrapSmf(trk), "multipacket.sty");
        TEST("multi-packet divided 0xF7 & escaped SysEx parsed cleanly",
             pr.success &&
             pr.xg_sysex.total_sysex_seen == 3 &&
             pr.xg_sysex.recognized_xg_count == 3 &&
             pr.xg_sysex.malformed_count == 0 &&
             pr.xg_sysex.parts.count(10) && pr.xg_sysex.parts.at(10).volume == 0x66 &&
             pr.xg_sysex.reverb_type == 0x0109u &&
             pr.xg_sysex.master_volume == 0x55);
        TEST("surrounding note events preserved at exact ticks after multi-packet SysEx",
             !pr.sections.empty() && !pr.sections[0].tracks.empty() &&
             pr.sections[0].tracks[0].events.size() == 2 &&
             pr.sections[0].tracks[0].events[0].tick == 96 &&
             pr.sections[0].tracks[0].events[1].tick == 216);
    }

    // ── 3. Malformed / Oversized (>4KB) / Unterminated / Truncated Safety ─
    {
        std::vector<uint8_t> trk;
        // 3a. Oversized single-packet SysEx (5000 bytes + F7)
        std::vector<uint8_t> big(5000, 0x20);
        big[0] = 0x43; big[1] = 0x73;
        addSysExF0(trk, 0, big, /*addF7=*/true);
        // 3b. Oversized multi-packet SysEx (3 x 2000 bytes + F7 = 6000 bytes)
        std::vector<uint8_t> chunk(2000, 0x11);
        addSysExF0(trk, 0, chunk, /*addF7=*/false);
        addSysExF7(trk, 0, chunk);
        chunk.push_back(0xF7);
        addSysExF7(trk, 0, chunk);
        // 3c. Non-7-bit byte (0x81) inside payload
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x0B, 0x81});
        // 3d. Empty F0 00
        addSysExF0(trk, 0, {}, /*addF7=*/false);
        // 3e. Stray F7 continuation with no preceding F0
        addSysExF7(trk, 0, {0x01, 0x02, 0x03});
        // 3f. Unterminated F0 followed by valid XG System On
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C}, /*addF7=*/false);
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00});
        // 3g. Invalid part mode (6 > 5) & invalid part (16 > 15)
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x0A, 0x07, 0x06});
        addSysExF0(trk, 0, {0x43, 0x10, 0x4C, 0x08, 0x10, 0x0B, 0x64});
        // Valid MIDI note AFTER all malformed SysEx
        addNote(trk, 120, 1, 67, 88, 240);
        // 3h. Unterminated F0 right before EoT
        addSysExF0(trk, 0, {0x43, 0x10}, /*addF7=*/false);
        addMetaEoT(trk, 0);

        Sff1Reader reader;
        const auto pr = reader.parseBuffer(wrapSmf(trk), "malformed_sysex.sty");
        TEST("malformed/oversized SysEx does not abort Style import", pr.success);
        TEST("malformed count == 9, valid XG System On still parsed (1 recognized)",
             pr.xg_sysex.xg_system_on &&
             pr.xg_sysex.recognized_xg_count == 1 &&
             pr.xg_sysex.malformed_count == 9 &&
             pr.xg_sysex.unrecognized_count == 9 &&
             pr.xg_sysex.total_sysex_seen == 10);
        TEST("MIDI notes after malformed/oversized SysEx preserved intact (tick 120, 360)",
             !pr.sections.empty() && !pr.sections[0].tracks.empty() &&
             pr.sections[0].tracks[0].events.size() == 2 &&
             pr.sections[0].tracks[0].events[0].tick == 120 &&
             pr.sections[0].tracks[0].events[0].data1 == 67 &&
             pr.sections[0].tracks[0].events[1].tick == 360);

        // 3i. Truncated SysEx at end of track (declared length > remaining)
        std::vector<uint8_t> trunc_trk;
        addNote(trunc_trk, 0, 0, 60, 100, 100);
        trunc_trk.push_back(0x00);
        trunc_trk.push_back(0xF0);
        trunc_trk.push_back(0x40); // claims 64 bytes, only 2 follow
        trunc_trk.push_back(0x43);
        trunc_trk.push_back(0x10);
        const auto tr = reader.parseBuffer(wrapSmf(trunc_trk), "trunc_sysex.sty");
        TEST("truncated SysEx at EOF recorded as malformed, prior notes preserved",
             tr.success && tr.xg_sysex.malformed_count == 1 &&
             tr.xg_sysex.total_sysex_seen == 1 &&
             !tr.sections.empty() && !tr.sections[0].tracks.empty() &&
             tr.sections[0].tracks[0].events.size() == 2);
    }

    // ── 4. UASF Backward Compatibility & Malformed 'XGSX' Rejection ─────
    {
        // SysEx-free style writes ZERO 'XGSX' bytes
        std::vector<uint8_t> clean_trk;
        addMetaTempo(clean_trk, 0, 500000);
        addMetaMarker(clean_trk, 0, "Main A");
        addNote(clean_trk, 0, 0, 60, 100, 480);
        addMetaEoT(clean_trk, 1440);
        Sff1Reader reader;
        const auto pr = reader.parseBuffer(wrapSmf(clean_trk), "no_sysex.sty");
        const auto mr = Sff1ToUasfMapper().map(pr);
        const auto ser = uasf::UasfSerializer().serialize(mr.style);
        const std::string raw(reinterpret_cast<const char*>(ser.data.data()), ser.data.size());
        TEST("SysEx-free style has empty xg_sysex and emits zero XGSX bytes",
             mr.success && mr.style.xg_sysex.empty() && ser.success &&
             raw.find("XGSX") == std::string::npos);

        // Malformed XGSX block (bad version, truncated, duplicate) rejected fail-closed
        auto bad = ser.data;
        bad[8] |= uasf::format::kFlagExtensions;
        putU32LE(bad, uasf::format::kExtXgSysEx);
        putU32LE(bad, 2);
        bad.push_back(99); // bad version
        bad.push_back(0);
        TEST("malformed XGSX version rejected fail-closed",
             !uasf::UasfDeserializer().deserialize(bad).success);

        auto trunc = ser.data;
        trunc[8] |= uasf::format::kFlagExtensions;
        putU32LE(trunc, uasf::format::kExtXgSysEx);
        putU32LE(trunc, 5);
        trunc.insert(trunc.end(), {1, 0, 0, 0, 0}); // truncated counters
        TEST("truncated XGSX block rejected fail-closed",
             !uasf::UasfDeserializer().deserialize(trunc).success);
    }

    // ── 5. Real CC0 Fixtures: SYNTH_MIX_SYSEX_CC_HYGIENE & SYNTH_FULL_SFF2 ─
    {
        const std::string mix_path = std::string(REPO_DIR) + "/fixtures/synthetic/SYNTH_MIX_SYSEX_CC_HYGIENE.sty";
        Sff1Reader reader;
        const auto pr = reader.parseFile(mix_path);
        const auto& xg = pr.xg_sysex;
        TEST("SYNTH_MIX_SYSEX_CC_HYGIENE.sty: all 6 XG SysEx messages parsed live",
             pr.success &&
             xg.total_sysex_seen == 6 && xg.recognized_xg_count == 6 &&
             xg.unrecognized_count == 0 && xg.malformed_count == 0 &&
             xg.xg_system_on &&
             xg.reverb_type == 0x0100u && xg.chorus_type == 0x4100u &&
             xg.parts.count(10) && xg.parts.at(10).volume == 0x64 &&
             xg.parts.count(11) && xg.parts.at(11).pan == 0x48 &&
             xg.master_eq_type == 2 && xg.master_eq_low_gain_db == 8);

        const auto mr = Sff1ToUasfMapper().map(pr);
        const auto ser = uasf::UasfSerializer().serialize(mr.style);
        const auto back = uasf::UasfDeserializer().deserialize(ser.data);
        TEST("SYNTH_MIX_SYSEX_CC_HYGIENE.sty: UASF round-trip preserves all 6 XG SysEx settings",
             mr.success && ser.success && back.success &&
             back.style.xg_sysex.xg_system_on &&
             back.style.xg_sysex.reverb_type == 0x0100u &&
             back.style.xg_sysex.chorus_type == 0x4100u &&
             back.style.xg_sysex.parts.at(10).volume == 0x64 &&
             back.style.xg_sysex.parts.at(11).pan == 0x48 &&
             back.style.xg_sysex.master_eq_low_gain_db == 8);

        const std::string full_path = std::string(REPO_DIR) + "/fixtures/synthetic/SYNTH_FULL_SFF2.sty";
        const auto fpr = reader.parseFile(full_path);
        const auto fmr = Sff1ToUasfMapper().map(fpr);
        TEST("SYNTH_FULL_SFF2.sty: zero SysEx -> empty xg_sysex (golden parity)",
             fpr.success && fmr.success && fpr.xg_sysex.total_sysex_seen == 0 &&
             fmr.style.xg_sysex.empty());
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
