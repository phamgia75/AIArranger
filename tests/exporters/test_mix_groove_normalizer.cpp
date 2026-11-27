// Exhaustive synthetic unit & property tests for Track B Q5a:
//   - src/importers/sff1/xg_sysex_parser.h
//   - src/exporters/korg/mix_normalizer.h
//   - src/exporters/korg/groove_normalizer.h
// 100% synthetic fixtures, strictly offline, zero private data.

// Dual-include ODR verification (groove_normalizer.h + mix_normalizer.h BEFORE
// megavoice_sanitizer.h + cv_splitter.h).
#include "exporters/korg/groove_normalizer.h"
#include "exporters/korg/mix_normalizer.h"
#include "exporters/korg/megavoice_sanitizer.h"
#include "exporters/korg/cv_splitter.h"
#include "importers/sff1/xg_sysex_parser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;
using namespace ai_arranger::importers::sff1;

namespace {

int failures = 0;
int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

double computeEffectiveLoudness(const Element& el, uint8_t ch) {
    uint8_t cc7 = 100, cc11 = 127;
    double sum_sq = 0.0;
    uint32_t count = 0;
    for (const auto& ev : el.events) {
        if ((ev.status & 0x0Fu) != ch) continue;
        const uint8_t hi = ev.status & 0xF0u;
        if (ev.tick == 0 && hi == 0xB0u) {
            if (ev.d1 == 7u)  cc7 = ev.d2;
            if (ev.d1 == 11u) cc11 = ev.d2;
        } else if (hi == 0x90u && ev.d2 > 0u) {
            sum_sq += static_cast<double>(ev.d2) * static_cast<double>(ev.d2);
            ++count;
        }
    }
    if (count == 0) return 0.0;
    const double rms = std::sqrt(sum_sq / static_cast<double>(count));
    return (static_cast<double>(cc7) / 127.0) *
           (static_cast<double>(cc11) / 127.0) *
           (rms / 127.0);
}

int findLastTick0Cc(const Element& el, uint8_t ch, uint8_t cc) {
    int val = -1;
    for (const auto& ev : el.events) {
        if (ev.tick > 0) break;
        if ((ev.status & 0xF0u) == 0xB0u && (ev.status & 0x0Fu) == ch && ev.d1 == cc) {
            val = static_cast<int>(ev.d2);
        }
    }
    return val;
}

bool hasLoopBoundaryIssues(const Element& el) {
    std::array<uint8_t, 16> cc64{};
    std::array<uint16_t, 16> pb{};
    pb.fill(8192u);
    for (const auto& ev : el.events) {
        const uint8_t hi = ev.status & 0xF0u;
        const uint8_t ch = ev.status & 0x0Fu;
        if (hi == 0xB0u && ev.d1 == 64u) cc64[ch] = ev.d2;
        if (hi == 0xE0u) {
            pb[ch] = static_cast<uint16_t>(ev.d1 | (static_cast<uint16_t>(ev.d2) << 7));
        }
    }
    for (size_t ch = 0; ch < 16; ++ch) {
        if (cc64[ch] >= 64u || pb[ch] != 8192u) return true;
    }
    return false;
}

bool verifyStrictSamePitchNonOverlap(const std::vector<NoteSpan>& notes, uint32_t len) {
    std::array<std::vector<NoteSpan>, 128> by_pitch{};
    for (const auto& n : notes) {
        if (n.pitch > 127u || n.vel == 0u || n.vel > 127u) return false;
        if (n.on_tick >= n.off_tick) return false;
        if (n.off_tick >= len) return false;
        by_pitch[n.pitch].push_back(n);
    }
    for (const auto& seq : by_pitch) {
        for (size_t i = 1; i < seq.size(); ++i) {
            if (seq[i - 1].off_tick > seq[i].on_tick) return false;
        }
    }
    return true;
}

void emitNoteSpansToElement(Element& el, uint8_t ch, const std::vector<NoteSpan>& notes) {
    for (const auto& n : notes) {
        el.events.push_back(Event{n.on_tick, static_cast<uint8_t>(0x90u | ch), n.pitch, n.vel});
        el.events.push_back(Event{n.off_tick, static_cast<uint8_t>(0x80u | ch), n.pitch, 0u});
    }
    std::stable_sort(el.events.begin(), el.events.end(),
                     [](const Event& a, const Event& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         // Emit NoteOff before NoteOn at identical tick for clean FIFO pairing.
                         const bool a_off = ((a.status & 0xF0u) == 0x80u);
                         const bool b_off = ((b.status & 0xF0u) == 0x80u);
                         if (a_off != b_off) return a_off;
                         return false;
                     });
}

} // namespace

int main() {
    std::printf("Test: Track B Q5a XG SysEx parser, Mix/Loop-Boundary normalizer & Groove normalizer\n");

    // ── 1. Dual-Include ODR Safety ──────────────────────────────────────
    {
#ifdef AI_ARRANGER_EXPORTERS_KORG_QUALITY_NOTE_SPAN_DEFINED
        const bool guard_defined = true;
#else
        const bool guard_defined = false;
#endif
        NoteSpan ns{10, 120, 60, 90, false};
        CasmTrackView cv{};
        TEST("dual-include ODR guard defined and NoteSpan/CasmTrackView/cv_splitter coexist",
             guard_defined && ns.pitch == 60 && cv.high_key == 11 &&
             maxCvCountFor(ai_arranger::uasf::SectionType::Main1) == 6);
    }

    // ── 2. XG SysEx Parser: All Documented Signatures ───────────────────
    {
        XgSysExSummary sum;

        // XG Master Volume: 43 10 4C 00 00 04 6E
        const uint8_t xg_mv[] = {0x43, 0x10, 0x4C, 0x00, 0x00, 0x04, 0x6E};
        TEST("XG Master Volume parsed",
             parseXgSysExMessage(xg_mv, sizeof(xg_mv), sum) &&
             sum.master_volume.has_value() && *sum.master_volume == 0x6E);

        // GM Realtime Master Volume: 7F 7F 04 01 ll mm (overwrites with mm=0x5A)
        const uint8_t gm_mv[] = {0x7F, 0x7F, 0x04, 0x01, 0x00, 0x5A};
        TEST("GM Realtime Master Volume parsed",
             parseXgSysExMessage(gm_mv, sizeof(gm_mv), sum) &&
             sum.master_volume.has_value() && *sum.master_volume == 0x5A);

        // Reverb / Chorus / Variation Effect Types (2 data bytes MSB/LSB)
        const uint8_t rev_type[] = {0x43, 0x10, 0x4C, 0x02, 0x01, 0x00, 0x01, 0x11};
        const uint8_t cho_type[] = {0x43, 0x10, 0x4C, 0x02, 0x01, 0x20, 0x41, 0x02};
        const uint8_t var_type[] = {0x43, 0x1F, 0x4C, 0x02, 0x01, 0x40, 0x05, 0x00};
        TEST("XG Reverb/Chorus/Variation Effect Types parsed (incl device nibble 1F)",
             parseXgSysExMessage(rev_type, sizeof(rev_type), sum) &&
             parseXgSysExMessage(cho_type, sizeof(cho_type), sum) &&
             parseXgSysExMessage(var_type, sizeof(var_type), sum) &&
             sum.reverb_type == 0x0111u &&
             sum.chorus_type == 0x4102u &&
             sum.variation_type == 0x0500u);

        // Multi Part 08 0B (part 11) all documented parameters
        const uint8_t mp_vol[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x0B, 108};
        const uint8_t mp_pan[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x0E, 48};
        const uint8_t mp_dry[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x11, 120};
        const uint8_t mp_cho[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x12, 32};
        const uint8_t mp_rev[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x13, 44};
        const uint8_t mp_var[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x14, 24};
        const uint8_t mp_eql[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x72, 60}; // -4 dB
        const uint8_t mp_eqh[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x73, 70}; // +6 dB
        TEST("XG Multi Part 0B all 8 params parsed",
             parseXgSysExMessage(mp_vol, sizeof(mp_vol), sum) &&
             parseXgSysExMessage(mp_pan, sizeof(mp_pan), sum) &&
             parseXgSysExMessage(mp_dry, sizeof(mp_dry), sum) &&
             parseXgSysExMessage(mp_cho, sizeof(mp_cho), sum) &&
             parseXgSysExMessage(mp_rev, sizeof(mp_rev), sum) &&
             parseXgSysExMessage(mp_var, sizeof(mp_var), sum) &&
             parseXgSysExMessage(mp_eql, sizeof(mp_eql), sum) &&
             parseXgSysExMessage(mp_eqh, sizeof(mp_eqh), sum) &&
             sum.parts.at(11).volume == 108 &&
             sum.parts.at(11).pan == 48 &&
             sum.parts.at(11).dry_level == 120 &&
             sum.parts.at(11).chorus_send == 32 &&
             sum.parts.at(11).reverb_send == 44 &&
             sum.parts.at(11).variation_send == 24 &&
             sum.parts.at(11).eq_low_gain_db == -4 &&
             sum.parts.at(11).eq_high_gain_db == 6);

        // EQ Bass/Treble clamping to [-12, +12] dB on extreme values (0 -> -12, 127 -> +12)
        const uint8_t eq_min[] = {0x43, 0x10, 0x4C, 0x08, 0x00, 0x72, 0};
        const uint8_t eq_max[] = {0x43, 0x10, 0x4C, 0x08, 0x00, 0x73, 127};
        TEST("XG Multi Part EQ Bass/Treble clamped to [-12, +12] dB",
             parseXgSysExMessage(eq_min, sizeof(eq_min), sum) &&
             parseXgSysExMessage(eq_max, sizeof(eq_max), sum) &&
             sum.parts.at(0).eq_low_gain_db == -12 &&
             sum.parts.at(0).eq_high_gain_db == 12);

        // Drum Setup 30..33 (setup 0 key 36, setup 3 key 38)
        const uint8_t d0_pitch[] = {0x43, 0x10, 0x4C, 0x30, 36, 0x00, 61}; // -3
        const uint8_t d0_lvl[]   = {0x43, 0x10, 0x4C, 0x30, 36, 0x01, 115};
        const uint8_t d0_pan[]   = {0x43, 0x10, 0x4C, 0x30, 36, 0x03, 64};
        const uint8_t d0_rev[]   = {0x43, 0x10, 0x4C, 0x30, 36, 0x04, 18};
        const uint8_t d3_lvl[]   = {0x43, 0x10, 0x4C, 0x33, 38, 0x01, 102};
        TEST("XG Drum Setup 30..33 overrides parsed and merged per (setup, key)",
             parseXgSysExMessage(d0_pitch, sizeof(d0_pitch), sum) &&
             parseXgSysExMessage(d0_lvl, sizeof(d0_lvl), sum) &&
             parseXgSysExMessage(d0_pan, sizeof(d0_pan), sum) &&
             parseXgSysExMessage(d0_rev, sizeof(d0_rev), sum) &&
             parseXgSysExMessage(d3_lvl, sizeof(d3_lvl), sum) &&
             sum.drum_overrides.size() == 2 &&
             sum.drum_overrides[0].drum_setup == 0 &&
             sum.drum_overrides[0].key == 36 &&
             sum.drum_overrides[0].pitch_coarse == -3 &&
             sum.drum_overrides[0].level == 115 &&
             sum.drum_overrides[0].pan == 64 &&
             sum.drum_overrides[0].reverb_send == 18 &&
             sum.drum_overrides[1].drum_setup == 3 &&
             sum.drum_overrides[1].key == 38 &&
             sum.drum_overrides[1].level == 102);

        // Yamaha Style / DSP SysEx (43 73 ...)
        const uint8_t dsp1[] = {0x43, 0x73, 0x01, 0x51, 0x05, 0x00, 0x01, 0x08};
        const uint8_t dsp2[] = {0x43, 0x73, 0x7F, 0x00, 0x11};
        TEST("Yamaha Style/DSP SysEx (43 73 ...) counted & advisory recorded uniquely",
             parseXgSysExMessage(dsp1, sizeof(dsp1), sum) &&
             parseXgSysExMessage(dsp2, sizeof(dsp2), sum) &&
             sum.dsp_sysex_count == 2 &&
             sum.advisory_codes.size() == 1 &&
             sum.advisory_codes[0] == kDiagXgDspSysEx &&
             sum.recognized_xg_count == 22 &&
             sum.unrecognized_count == 0);
    }

    // ── 3. XG SysEx Parser: Fail-Closed Malformed / Bounded Checks ──────
    {
        XgSysExSummary sum;

        TEST("null payload rejected fail-closed",
             !parseXgSysExMessage(nullptr, 8, sum));
        const uint8_t dummy[] = {0x43};
        TEST("empty payload rejected fail-closed",
             !parseXgSysExMessage(dummy, 0, sum));

        std::vector<uint8_t> oversized(kMaxSysExPayloadBytes + 1, 0x00);
        oversized[0] = 0x43;
        oversized[1] = 0x73;
        TEST("oversized (> 4096 B) payload rejected fail-closed",
             !parseXgSysExMessage(oversized.data(), oversized.size(), sum));

        // High-bit (>= 0x80) byte in payload rejected fail-closed
        const uint8_t high_bit_xg[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x0B, 0x80};
        const uint8_t trailing_f7[] = {0x7F, 0x7F, 0x04, 0x01, 0x00, 0x64, 0xF7};
        TEST("non-7-bit (>= 0x80) payloads rejected fail-closed",
             !parseXgSysExMessage(high_bit_xg, sizeof(high_bit_xg), sum) &&
             !parseXgSysExMessage(trailing_f7, sizeof(trailing_f7), sum) &&
             sum.parts.empty() && !sum.master_volume.has_value());

        // Truncated payloads
        const uint8_t trunc_rev[] = {0x43, 0x10, 0x4C, 0x02, 0x01, 0x00, 0x01}; // missing LSB
        const uint8_t trunc_mp[]  = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x0B};       // missing dd
        const uint8_t trunc_gm[]  = {0x7F, 0x7F, 0x04, 0x01, 0x00};             // missing mm
        const uint8_t trunc_dsp[] = {0x43, 0x73};                                // len < 3
        const uint8_t bad_part[]  = {0x43, 0x10, 0x4C, 0x08, 0x10, 0x0B, 100};  // part 16 invalid
        TEST("truncated and invalid-part payloads rejected fail-closed",
             !parseXgSysExMessage(trunc_rev, sizeof(trunc_rev), sum) &&
             !parseXgSysExMessage(trunc_mp, sizeof(trunc_mp), sum) &&
             !parseXgSysExMessage(trunc_gm, sizeof(trunc_gm), sum) &&
             !parseXgSysExMessage(trunc_dsp, sizeof(trunc_dsp), sum) &&
             !parseXgSysExMessage(bad_part, sizeof(bad_part), sum) &&
             sum.recognized_xg_count == 0 &&
             sum.unrecognized_count == 10);

        // Message cap (kMaxSysExMessages = 1024)
        XgSysExSummary capped;
        const uint8_t valid_mv[] = {0x43, 0x10, 0x4C, 0x00, 0x00, 0x04, 100};
        uint32_t ok_count = 0;
        for (uint32_t i = 0; i < kMaxSysExMessages + 25; ++i) {
            if (parseXgSysExMessage(valid_mv, sizeof(valid_mv), capped)) ++ok_count;
        }
        TEST("kMaxSysExMessages=1024 cap enforced cleanly",
             ok_count == kMaxSysExMessages &&
             capped.total_sysex_seen == kMaxSysExMessages &&
             capped.recognized_xg_count == kMaxSysExMessages);
    }

    // ── 4. Mix Normalizer: CC Hygiene (NRPN vs RPN, CC94->CC93, >127) ───
    {
        Element el;
        el.marker = "v1cv1";
        el.num = 4;
        el.den = 4;
        el.length = 1920;

        // Channel 11 (Acc1): NRPN sequence (99, 98, 6, 38) followed by RPN Pitch Bend Range (101=0, 100=0, 6=12)
        // plus null-RPN (101=127, 100=127), CC94=100 (no CC93 -> blends 30), safe CCs, invalid >127 CC.
        el.events = {
            {0,   0xB0 | 11, 99, 1},     // NRPN MSB -> drop
            {0,   0xB0 | 11, 98, 32},    // NRPN LSB -> drop
            {0,   0xB0 | 11, 6,  80},    // NRPN Data MSB -> drop
            {0,   0xB0 | 11, 38, 10},    // NRPN Data LSB -> drop
            {0,   0xB0 | 11, 101, 0},    // RPN MSB -> keep & reset NRPN state
            {0,   0xB0 | 11, 100, 0},    // RPN LSB -> keep & reset NRPN state
            {0,   0xB0 | 11, 6,  12},    // RPN Pitch Bend Range MSB -> MUST KEEP!
            {0,   0xB0 | 11, 101, 127},  // RPN null -> keep
            {0,   0xB0 | 11, 100, 127},  // RPN null -> keep
            {0,   0xB0 | 11, 94, 100},   // XG Variation -> drop & blend 30 into CC93
            {0,   0xB0 | 11, 7,  100},   // safe CC7
            {0,   0xB0 | 11, 10, 64},    // safe CC10
            {0,   0xB0 | 11, 91, 40},    // safe CC91
            {10,  0xB0 | 11, 1,  50},    // safe CC1
            {10,  0xB0 | 11, 11, 120},   // safe CC11
            {10,  0xB0 | 11, 66, 127},   // safe CC66
            {10,  0xB0 | 11, 67, 64},    // safe CC67
            {20,  0xB0 | 11, 74, 200},   // invalid data byte >127 -> drop
            {20,  0xB0 | 11, 130, 50},   // invalid controller >127 -> drop
            // Channel 12 (Acc2): already has CC93=55 AND CC94=100 -> CC94 dropped, CC93 stays 55!
            {0,   0xB0 | 12, 93, 55},
            {0,   0xB0 | 12, 94, 100},
            {0,   0x90 | 11, 60, 90},
            {480, 0x80 | 11, 60, 0},
        };

        MixNormalizeOptions opt;
        opt.normalize_headroom = false;
        const auto stats = normalizeElementMix(el, {}, opt);

        bool saw_nrpn_or_cc94_or_invalid = false;
        bool saw_rpn_pb_range_cc6 = false;
        bool saw_nrpn_cc6_80 = false;
        bool saw_safe_cc1 = false, saw_safe_cc66 = false, saw_safe_cc67 = false;
        for (const auto& ev : el.events) {
            if ((ev.status & 0xF0u) != 0xB0u) continue;
            if (ev.d1 == 98 || ev.d1 == 99 || ev.d1 == 94 || ev.d1 > 127 || ev.d2 > 127) {
                saw_nrpn_or_cc94_or_invalid = true;
            }
            if ((ev.status & 0x0Fu) == 11 && ev.d1 == 6 && ev.d2 == 12) saw_rpn_pb_range_cc6 = true;
            if ((ev.status & 0x0Fu) == 11 && ev.d1 == 6 && ev.d2 == 80) saw_nrpn_cc6_80 = true;
            if ((ev.status & 0x0Fu) == 11 && ev.d1 == 1 && ev.d2 == 50) saw_safe_cc1 = true;
            if ((ev.status & 0x0Fu) == 11 && ev.d1 == 66 && ev.d2 == 127) saw_safe_cc66 = true;
            if ((ev.status & 0x0Fu) == 11 && ev.d1 == 67 && ev.d2 == 64) saw_safe_cc67 = true;
        }

        TEST("NRPN CC98/99+6/38 & >127 stripped while RPN PB Range CC100/101+6 preserved",
             !saw_nrpn_or_cc94_or_invalid &&
             !saw_nrpn_cc6_80 &&
             saw_rpn_pb_range_cc6 &&
             saw_safe_cc1 && saw_safe_cc66 && saw_safe_cc67 &&
             stats.proprietary_ccs_stripped == 8);

        TEST("CC94 blends 30% into tick-0 CC93 only when CC93 absent",
             findLastTick0Cc(el, 11, 93) == 30 &&
             findLastTick0Cc(el, 12, 93) == 55 &&
             stats.xg_params_translated == 1);
    }

    // ── 5. Mix Normalizer: Loop-Boundary Resets + korg::validate() ──────
    {
        Style st;
        st.ppqn = 480;
        st.mpqn = 500000;

        Element el;
        el.marker = "v1cv1";
        el.num = 4;
        el.den = 4;
        el.length = 1920;
        el.events = {
            {0,    0x90 | 8,  36, 95},
            {0,    0xB0 | 11, 64, 127}, // left open at end!
            {240,  0xE0 | 11, 0x00, 0x60}, // bent up (12288 != 8192) and never centered!
            {0,    0xB0 | 12, 64, 127}, // opened then closed -> NO reset needed
            {480,  0xB0 | 12, 64, 0},
            {120,  0xE0 | 12, 0x20, 0x30},
            {600,  0xE0 | 12, 0x00, 0x40}, // centered -> NO reset needed
            {960,  0x80 | 8,  36, 0},
            {0,    0x90 | 11, 60, 88},
            {960,  0x80 | 11, 60, 0},
        };

        const auto stats = normalizeElementMix(el, {});
        st.elements.push_back(el);

        const auto errs = validate(st);
        const auto wr = write(st);
        const auto pr = parse(wr.bytes);

        TEST("open CC64=127 & off-center PitchBend reset at len-1 without redundant resets",
             stats.sustain_resets_added == 1 &&
             stats.pitchbend_resets_added == 1 &&
             !hasLoopBoundaryIssues(el));
        TEST("loop-reset element passes korg::validate() and write->parse round-trip",
             errs.empty() && wr.ok && pr.ok && pr.style.elements.size() == 1 &&
             !hasLoopBoundaryIssues(pr.style.elements[0]));

        // Zero-length element guard
        Element zero_el;
        zero_el.length = 0;
        zero_el.events = {{0, 0xB0 | 11, 64, 127}};
        const auto zstats = normalizeElementMix(zero_el, {});
        TEST("zero-length element safely skips loop-boundary append",
             zstats.sustain_resets_added == 0 && zstats.pitchbend_resets_added == 0);
    }

    // ── 6. Mix Normalizer: XG SysEx Tick-0 Mix CC Injection ─────────────
    {
        Element el;
        el.marker = "v1cv1";
        el.num = 4;
        el.den = 4;
        el.length = 1920;
        // Pre-existing tick-0 Bank/PC + CC7=92 on ch 11 (must NOT be clobbered!).
        el.events = {
            {0,   0xB0 | 11, 0,  121},
            {0,   0xB0 | 11, 32, 0},
            {0,   0xC0 | 11, 24, 0},
            {0,   0xB0 | 11, 7,  92},
            {0,   0x90 | 11, 60, 85},
            {480, 0x80 | 11, 60, 0},
        };

        std::map<uint8_t, XgPartMixParams> xg_map;
        XgPartMixParams p11;
        p11.volume = 110;       // existing tick-0 CC7=92 -> must NOT overwrite!
        p11.pan = 42;           // missing -> inject
        p11.reverb_send = 50;   // missing -> inject
        p11.chorus_send = 22;   // missing -> inject
        xg_map[11] = p11;

        XgPartMixParams p3;     // channel 3 (< kBassCh) -> ignored so korg::validate() stays clean
        p3.volume = 100;
        xg_map[3] = p3;

        MixNormalizeOptions opt;
        opt.normalize_headroom = false;
        const auto stats = normalizeElementMix(el, xg_map, opt);

        Style st{480, 500000, {el}};
        const auto errs = validate(st);
        TEST("XG SysEx injects missing tick-0 CC10/91/93 without clobbering existing CC7",
             findLastTick0Cc(el, 11, 7) == 92 &&
             findLastTick0Cc(el, 11, 10) == 42 &&
             findLastTick0Cc(el, 11, 91) == 50 &&
             findLastTick0Cc(el, 11, 93) == 22 &&
             stats.xg_params_translated == 3 &&
             errs.empty());
    }

    // ── 7. Mix Normalizer: Effective Loudness Proxy Headroom (M5.3) ─────
    {
        Element el;
        el.marker = "v1cv1";
        el.num = 4;
        el.den = 4;
        el.length = 1920;

        // ch 8 (Bass): very quiet (CC7=40, vel=18,24 -> L ~ 0.052 < 0.20) -> boost CC7<=120 & scale vel
        el.events.push_back({0,   0xB0 | 8, 7, 40});
        el.events.push_back({0,   0x90 | 8, 36, 18});
        el.events.push_back({240, 0x80 | 8, 36, 0});
        el.events.push_back({480, 0x90 | 8, 43, 24});
        el.events.push_back({720, 0x80 | 8, 43, 0});

        // ch 11 (Acc1): clipping loud (CC7=127, CC11=127, vel=125 -> L = 0.984 > 0.92) -> attenuate CC7 <= 0.86
        el.events.push_back({0,   0xB0 | 11, 7,  127});
        el.events.push_back({0,   0xB0 | 11, 11, 127});
        el.events.push_back({0,   0x90 | 11, 60, 125});
        el.events.push_back({480, 0x80 | 11, 60, 0});

        // ch 12 (Acc2): already healthy (CC7=100, CC11=127, vel=85 -> L = 0.527) -> untouched
        el.events.push_back({0,   0xB0 | 12, 7,  100});
        el.events.push_back({0,   0x90 | 12, 64, 85});
        el.events.push_back({480, 0x80 | 12, 64, 0});

        // ch 9 (Drum) & ch 10 (Perc): extreme quiet/loud -> MUST NOT be gain-adjusted!
        el.events.push_back({0,   0xB0 | 9,  7,  20});
        el.events.push_back({0,   0x90 | 9,  36, 10});
        el.events.push_back({120, 0x80 | 9,  36, 0});
        el.events.push_back({0,   0xB0 | 10, 7,  127});
        el.events.push_back({0,   0x90 | 10, 60, 127});
        el.events.push_back({120, 0x80 | 10, 60, 0});

        const auto stats = normalizeElementMix(el, {});

        const double l_bass = computeEffectiveLoudness(el, 8);
        const double l_acc1 = computeEffectiveLoudness(el, 11);
        const double l_acc2 = computeEffectiveLoudness(el, 12);

        bool any_vel_zero_noteon = false;
        uint8_t acc1_vel = 0;
        uint8_t drum_vel = 0;
        for (const auto& ev : el.events) {
            if ((ev.status & 0xF0u) == 0x90u) {
                if (ev.d2 == 0) any_vel_zero_noteon = true;
                if ((ev.status & 0x0Fu) == 11) acc1_vel = ev.d2;
                if ((ev.status & 0x0Fu) == 9)  drum_vel = ev.d2;
            }
        }

        Style st{480, 500000, {el}};
        TEST("headroom boost brings quiet Bass L >= 0.24 with zero vel-0 notes",
             l_bass >= 0.24 && l_bass <= 0.92 &&
             findLastTick0Cc(el, 8, 7) <= 120 && !any_vel_zero_noteon);
        TEST("headroom attenuation brings loud Acc1 L <= 0.86 preserving note velocity",
             l_acc1 >= 0.20 && l_acc1 <= 0.86 && acc1_vel == 125);
        TEST("healthy Acc2 and Drum/Perc channels left completely untouched",
             l_acc2 > 0.50 && l_acc2 < 0.56 &&
             findLastTick0Cc(el, 9, 7) == 20 && drum_vel == 10 &&
             findLastTick0Cc(el, 10, 7) == 127 &&
             stats.tracks_gain_adjusted == 2 &&
             validate(st).empty());
    }

    // ── 8. Groove Normalizer: Anticipated Downbeat Preservation ─────────
    {
        const uint32_t bar = 1920;
        const uint32_t len = 1920;

        // Non-looping Fill: note at len - 1 moved to [len - 2, len - 1]
        std::vector<NoteSpan> fill_notes = {
            {0, 480, 36, 100, false},
            {len - 1, len + 20, 36, 110, false},      // exact len - 1 anticipation
            {len - 10, len + 5, 42, 85, false},       // inside final 64th window [1890..1919]
        };
        GrooveNormalizeOptions fill_opt;
        fill_opt.is_looping_main = false;
        const auto fstats = normalizeTrackGroove(fill_notes, len, bar, {}, fill_opt);
        TEST("non-looping Fill preserves len-1 & final-64th anticipations cleanly",
             fill_notes.size() == 3 &&
             fstats.anticipations_preserved == 2 &&
             verifyStrictSamePitchNonOverlap(fill_notes, len) &&
             fill_notes[1].on_tick == len - 10 && fill_notes[1].off_tick == len - 1 &&
             fill_notes[2].on_tick == len - 2 && fill_notes[2].off_tick == len - 1);

        // Looping Main: note at len - 1 snapped to tick 0!
        std::vector<NoteSpan> main_notes = {
            {len - 1, len + 59, 60, 96, false}, // orig_dur = 60
        };
        GrooveNormalizeOptions main_opt;
        main_opt.is_looping_main = true;
        const auto mstats = normalizeTrackGroove(main_notes, len, bar, {}, main_opt);
        TEST("looping Main snaps len-1 anticipation cleanly to tick 0",
             main_notes.size() == 1 &&
             mstats.anticipations_preserved == 1 &&
             main_notes[0].on_tick == 0 &&
             main_notes[0].off_tick == 60 &&
             verifyStrictSamePitchNonOverlap(main_notes, len));

        // Degenerate element lengths (0, 1, 2)
        std::vector<NoteSpan> deg = {{0, 1, 60, 80, false}};
        const auto d0 = normalizeTrackGroove(deg, 0, 1920);
        TEST("len=0 clears safely with zero UB", deg.empty() && d0.new_element_length_ticks == 0);
        deg = {{1, 2, 60, 80, false}};
        const auto d2 = normalizeTrackGroove(deg, 2, 1920);
        TEST("len=2 preserves anticipation at [0, 1]",
             deg.size() == 1 && deg[0].on_tick == 0 && deg[0].off_tick == 1 &&
             d2.new_element_length_ticks == 2);
    }

    // ── 9. Groove Normalizer: Same-Pitch De-Overlap After PPQN Rescale ──
    {
        const uint32_t len = 1920;
        const uint32_t bar = 1920;

        // Simulate 1920->480 PPQN rescale artifacts:
        //  - Overlapping pitch 60 notes: [0, 520] & [480, 960]
        //  - 3-note rapid flam collapsed to identical tick 1000 on pitch 64
        //  - Multi-pitch chord at tick 0 (pitches 48, 52, 55) must remain simultaneous!
        //  - Boundary flam collapsed at len - 2 (1918) on pitch 67
        std::vector<NoteSpan> notes = {
            {0,    480, 48, 80, false},
            {0,    480, 52, 82, false},
            {0,    480, 55, 84, false},
            {0,    520, 60, 90, false},
            {480,  960, 60, 95, false},
            {1000, 1100, 64, 70, false},
            {1000, 1001, 64, 80, false},
            {1000, 1200, 64, 90, false},
            {1918, 1919, 67, 75, false},
            {1918, 1919, 67, 85, false},
        };

        const auto gstats = normalizeTrackGroove(notes, len, bar);

        // Verify all 10 notes survived and strict on < off <= next.on < len holds!
        uint32_t chord_at_0 = 0;
        for (const auto& n : notes) {
            if (n.on_tick == 0 && (n.pitch == 48 || n.pitch == 52 || n.pitch == 55)) {
                ++chord_at_0;
            }
        }

        Element el{"v1cv1", 4, 4, len, {}};
        emitNoteSpansToElement(el, 11, notes);
        Style st{480, 500000, {el}};

        TEST("same-pitch overlap & collapsed flams resolved with zero dropped notes",
             notes.size() == 10 &&
             gstats.same_pitch_overlaps_fixed >= 4 &&
             chord_at_0 == 3 &&
             verifyStrictSamePitchNonOverlap(notes, len));
        TEST("de-overlapped notes pass korg::validate() cleanly",
             validate(st).empty());
    }

    // ── 10. Groove Normalizer: --bake-rubato Microsecond Integration ────
    {
        const uint32_t ppqn = 480;
        const uint32_t bar  = 1920; // 4/4
        const uint32_t len  = 3840; // 2 bars

        // Exact ritardando: bar 1 @ 500000 us/q (120 BPM), bar 2 @ 1000000 us/q (60 BPM)
        // Bar 1 = 1920 warped ticks; Bar 2 = 3840 warped ticks -> exact 5760 ticks (3 bars)!
        std::vector<NoteSpan> notes = {
            {0,    480,  60, 90, false}, // -> [0, 480]
            {1920, 2400, 64, 92, false}, // -> [1920, 2880] (duration doubled!)
            {2880, 3840, 67, 95, false}, // -> [3840, 5759] (clamped to new_len - 1)
        };
        const std::vector<std::pair<uint32_t, uint32_t>> tempos = {
            {0, 500000},
            {1920, 1000000},
        };
        GrooveNormalizeOptions opt;
        opt.bake_rubato = true;
        opt.base_mpqn = 500000;
        opt.ppqn = ppqn;

        const auto rstats = normalizeTrackGroove(notes, len, bar, tempos, opt);
        TEST("--bake-rubato exact ritardando integration (2 bars -> 3 bars = 5760 ticks)",
             rstats.rubato_baked &&
             rstats.new_element_length_ticks == 5760 &&
             rstats.max_timing_drift_ms < 0.001 &&
             notes.size() == 3 &&
             notes[0].on_tick == 0 && notes[0].off_tick == 480 &&
             notes[1].on_tick == 1920 && notes[1].off_tick == 2880 &&
             notes[2].on_tick == 3840 && notes[2].off_tick == 5759 &&
             verifyStrictSamePitchNonOverlap(notes, rstats.new_element_length_ticks));

        // Fractional bar length rounds UP to whole bars (2.5 bars = 4800 -> 3 bars = 5760)
        std::vector<NoteSpan> frac_notes = {{1920, 3840, 60, 88, false}};
        const std::vector<std::pair<uint32_t, uint32_t>> frac_tempos = {
            {0, 500000},
            {1920, 750000}, // bar 2 takes 1.5x -> 1920 + 2880 = 4800 -> ceil to 5760!
        };
        const auto frstats = normalizeTrackGroove(frac_notes, len, bar, frac_tempos, opt);
        Element rel{"e1cv1", 4, 4, frstats.new_element_length_ticks, {}};
        emitNoteSpansToElement(rel, 11, frac_notes);
        Style rst{480, 500000, {rel}};
        TEST("--bake-rubato rounds fractional warped total up to whole bars passing korg::validate()",
             frstats.rubato_baked &&
             frstats.new_element_length_ticks == 5760 &&
             frstats.new_element_length_ticks % bar == 0 &&
             frac_notes[0].on_tick == 1920 && frac_notes[0].off_tick == 4800 &&
             validate(rst).empty());

        // Single tempo does NOT trigger rubato bake
        std::vector<NoteSpan> single_notes = {{0, 480, 60, 80, false}};
        const auto s1 = normalizeTrackGroove(single_notes, len, bar, {{0, 500000}}, opt);
        TEST("--bake-rubato skipped when < 2 distinct tempos",
             !s1.rubato_baked && s1.new_element_length_ticks == len);

        // Zero guards (base_mpqn=0, bar_ticks=0, ppqn=0) and uint64 overflow guard
        GrooveNormalizeOptions bad_opt = opt;
        bad_opt.base_mpqn = 0;
        auto g_mpqn0 = normalizeTrackGroove(single_notes, len, bar, tempos, bad_opt);
        bad_opt.base_mpqn = 500000;
        bad_opt.ppqn = 0;
        auto g_bar0 = normalizeTrackGroove(single_notes, len, 0, tempos, bad_opt);
        std::vector<NoteSpan> ppqn0_notes = {{0, 480, 60, 80, false}};
        auto g_ppqn0 = normalizeTrackGroove(ppqn0_notes, len, bar, tempos, bad_opt);
        // Overflow guard: huge tick * huge mpqn
        std::vector<NoteSpan> ov_notes = {{0, 100, 60, 80, false}};
        GrooveNormalizeOptions ov_opt = opt;
        ov_opt.base_mpqn = 1;
        const auto g_ov = normalizeTrackGroove(
            ov_notes,
            std::numeric_limits<uint32_t>::max(),
            bar,
            {{0, std::numeric_limits<uint32_t>::max()}, {10, std::numeric_limits<uint32_t>::max() - 1}},
            ov_opt);
        TEST("--bake-rubato guards mpqn==0, bar_ticks==0, ppqn==0, and uint64 overflow",
             !g_mpqn0.rubato_baked &&
             !g_bar0.rubato_baked &&
             g_ppqn0.rubato_baked &&
             !g_ov.rubato_baked);
    }

    // ── 11. formatPa5xStyleEditSheetMd() ────────────────────────────────
    {
        XgSysExSummary xg;
        const uint8_t mv[]  = {0x43, 0x10, 0x4C, 0x00, 0x00, 0x04, 112};
        const uint8_t rev[] = {0x43, 0x10, 0x4C, 0x02, 0x01, 0x00, 0x01, 0x10};
        const uint8_t vol[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x0B, 105};
        const uint8_t eql[] = {0x43, 0x10, 0x4C, 0x08, 0x0B, 0x72, 68}; // +4 dB
        const uint8_t drm[] = {0x43, 0x10, 0x4C, 0x30, 36, 0x01, 118};
        const uint8_t dsp[] = {0x43, 0x73, 0x01, 0x51, 0x00};
        parseXgSysExMessage(mv, sizeof(mv), xg);
        parseXgSysExMessage(rev, sizeof(rev), xg);
        parseXgSysExMessage(vol, sizeof(vol), xg);
        parseXgSysExMessage(eql, sizeof(eql), xg);
        parseXgSysExMessage(drm, sizeof(drm), xg);
        parseXgSysExMessage(dsp, sizeof(dsp), xg);

        StyleEditSheetTrackView tv;
        tv.korg_ch = kAcc1Ch;
        tv.track_name = "Acc1";
        tv.pa5x_ntt_type = "Parallel";
        tv.pa5x_ntt_table = "Chord";
        tv.wrap_around = 6;
        tv.key_range_low = 40;
        tv.key_range_high = 88;
        tv.retrigger_mode = "Retrigger";
        tv.guitar_mode_candidate = true;
        tv.eq_low_gain_db = 4;

        StyleEditSheetSoundView sv;
        sv.korg_ch = kAcc1Ch;
        sv.track_name = "Acc1";
        sv.inferred_family = "AcousticGuitar";
        sv.selected_name = "Steel Guitar DN1";
        sv.msb = 121;
        sv.lsb = 4;
        sv.pc = 25;
        sv.score = 88;
        sv.origin = "Pa5xFactoryCatalog";
        sv.epistemic_tag = "DOCUMENTED_NOT_HW_VERIFIED";

        const std::string md1 = formatPa5xStyleEditSheetMd({tv}, xg, {sv}, "SyntheticPopBallad");
        const std::string md2 = formatPa5xStyleEditSheetMd({tv}, xg, {sv}, "SyntheticPopBallad");

        TEST("formatPa5xStyleEditSheetMd contains required headers, epistemic gates & XG details",
             md1 == md2 &&
             md1.find("hardwareVerified: false") != std::string::npos &&
             md1.find("L3 = NOT_TESTED") != std::string::npos &&
             md1.find("SyntheticPopBallad") != std::string::npos &&
             md1.find("Steel Guitar DN1") != std::string::npos &&
             md1.find("121/4/25") != std::string::npos &&
             md1.find("0x0110") != std::string::npos &&
             md1.find(kDiagXgDspSysEx) != std::string::npos &&
             md1.find("+4 dB") != std::string::npos &&
             md1.find("Candidate") != std::string::npos);

        // P2-3 regression: escape \r/\n and | in headings and Markdown table cells
        StyleEditSheetTrackView evil_tv = tv;
        evil_tv.track_name = "Acc1\r\n|Lead|";
        StyleEditSheetSoundView evil_sv = sv;
        evil_sv.track_name = "Acc1\n|Track|";
        evil_sv.selected_name = "Evil\n|Name|";
        const std::string evil_md = formatPa5xStyleEditSheetMd({evil_tv}, xg, {evil_sv}, "Evil\n|Name|");
        const size_t first_nl = evil_md.find('\n');
        const std::string heading_line = (first_nl != std::string::npos) ? evil_md.substr(0, first_nl) : evil_md;
        TEST("P2-3 formatPa5xStyleEditSheetMd escapes \\r/\\n and | in heading & table cells",
             heading_line.find("# Korg Pa5X Style Edit Parameter Sheet") == 0 &&
             heading_line.find("Evil \\|Name\\|") != std::string::npos &&
             evil_md.find("Evil\n") == std::string::npos &&
             evil_md.find("Acc1  \\|Lead\\|") != std::string::npos &&
             evil_md.find("Acc1 \\|Track\\|") != std::string::npos &&
             evil_md.find("| Evil \\|Name\\| |") != std::string::npos);
    }

    // ── 12. Audit Round 1 Regressions (P1 std::clamp UB, P2-1 CC94 scope) ─
    {
        // P1 regression: CC7 in [121..127] with soft vel=15 (L < 0.20) must NOT UB/abort,
        // preserves CC7=127, lifts Note-On vel so L >= 0.24, leaves running-status NoteOff (0x90 vel=0) untouched.
        Element el;
        el.marker = "v1cv1";
        el.num = 4;
        el.den = 4;
        el.length = 1920;
        el.events = {
            {0,   0xB0 | 11, 7,  127},
            {0,   0xB0 | 11, 11, 127},
            {0,   0x90 | 11, 60, 15},
            {480, 0x90 | 11, 60, 0},   // 0x90 vel=0 Note-Off: MUST stay vel=0!
            {0,   0xB0 | 12, 7,  121},
            {0,   0x90 | 12, 64, 15},
            {480, 0x80 | 12, 64, 0},
        };
        const auto p1_stats = normalizeElementMix(el, {});
        const double l_ch11 = computeEffectiveLoudness(el, 11);
        const double l_ch12 = computeEffectiveLoudness(el, 12);
        bool noteoff_zero_preserved = false;
        for (const auto& ev : el.events) {
            if (ev.tick == 480 && ev.status == (0x90 | 11) && ev.d1 == 60 && ev.d2 == 0) {
                noteoff_zero_preserved = true;
            }
        }
        Style st_p1{480, 500000, {el}};
        TEST("P1 regression: CC7=127 & CC7=121 with vel=15 avoid std::clamp UB and meet L >= 0.24",
             findLastTick0Cc(el, 11, 7) == 127 &&
             findLastTick0Cc(el, 12, 7) == 121 &&
             l_ch11 >= 0.24 && l_ch11 <= 0.92 &&
             l_ch12 >= 0.24 && l_ch12 <= 0.92 &&
             noteoff_zero_preserved &&
             p1_stats.tracks_gain_adjusted == 2 &&
             validate(st_p1).empty());

        // P2-1 regression: stray CC94=100 on ch=0 (< kBassCh) and CC94=2 (blended=0) on ch=11
        // must NOT inject tick-0 CC93 on ch=0 or ch=11, and element passes korg::validate().
        Element el_cc94;
        el_cc94.marker = "v1cv1";
        el_cc94.num = 4;
        el_cc94.den = 4;
        el_cc94.length = 1920;
        el_cc94.events = {
            {0,   0xB0 | 0,  94, 100}, // non-Korg ch=0 -> strip, do NOT inject CC93 on ch=0
            {0,   0xB0 | 11, 94, 2},   // blended = (2*30)/100 = 0 -> strip, skip CC93=0
            {0,   0x90 | 11, 60, 88},
            {480, 0x80 | 11, 60, 0},
        };
        MixNormalizeOptions opt_p21;
        opt_p21.normalize_headroom = false;
        const auto p21_stats = normalizeElementMix(el_cc94, {}, opt_p21);
        Style st_p21{480, 500000, {el_cc94}};
        TEST("P2-1 regression: CC94->CC93 skips ch<8 and blended==0, passing korg::validate()",
             findLastTick0Cc(el_cc94, 0, 93) == -1 &&
             findLastTick0Cc(el_cc94, 11, 93) == -1 &&
             p21_stats.proprietary_ccs_stripped == 2 &&
             p21_stats.xg_params_translated == 0 &&
             validate(st_p21).empty());
    }

    std::printf("\nSummary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
