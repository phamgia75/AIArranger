// Synthetic unit & property tests for Track B Q2a MegaVoice/S.Art sanitizer
// and SFF2 Guitar (NTR=2) translator. 100% synthetic fixtures, zero private data.

// Dual-include order verification (Order A: megavoice_sanitizer.h -> cv_splitter.h).
#include "exporters/korg/megavoice_sanitizer.h"
#include "exporters/korg/cv_splitter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;
using ai_arranger::uasf::TrackRole;

namespace {

int failures = 0;
int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

bool hasCode(const std::vector<std::string>& codes, const char* needle) {
    for (const auto& c : codes) {
        if (c == needle) return true;
    }
    return false;
}

uint32_t maxSimultaneousVoices(const std::vector<NoteSpan>& notes) {
    struct Edge { uint32_t tick; int delta; };
    std::vector<Edge> edges;
    edges.reserve(notes.size() * 2);
    for (const auto& n : notes) {
        edges.push_back({n.on_tick, +1});
        edges.push_back({n.off_tick, -1});
    }
    // Process NoteOff (-1) before NoteOn (+1) at the same tick.
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.delta < b.delta;
    });
    int cur = 0;
    int best = 0;
    for (const auto& e : edges) {
        cur += e.delta;
        if (cur > best) best = cur;
    }
    return static_cast<uint32_t>(best);
}

// Helper: build a synthetic 4-zone stepped MegaVoice velocity pattern (16 notes).
std::vector<NoteSpan> makeSteppedFourZonePattern(uint8_t base_pitch = 52) {
    std::vector<NoteSpan> notes;
    // 8 normal notes in Zone 1 [1..64] (50% >= 40%)
    for (uint32_t i = 0; i < 8; ++i) {
        notes.push_back({i * 120, i * 120 + 200, static_cast<uint8_t>(base_pitch + (i % 5)),
                         static_cast<uint8_t>(40 + (i % 4) * 5), false});
    }
    // 4 muted notes in Zone 2 [65..90]
    for (uint32_t i = 0; i < 4; ++i) {
        notes.push_back({1000 + i * 120, 1000 + i * 120 + 360, base_pitch,
                         static_cast<uint8_t>(75 + i * 3), false});
    }
    // 2 harmonic notes in Zone 3 [91..100]
    notes.push_back({1600, 1840, static_cast<uint8_t>(base_pitch + 12), 95, false});
    notes.push_back({1840, 2080, static_cast<uint8_t>(base_pitch + 12), 98, false});
    // 2 noise notes in Zone 4 [101..127]
    notes.push_back({2100, 2340, base_pitch, 115, false});
    notes.push_back({2400, 2640, base_pitch, 124, false});
    return notes;
}

} // namespace

int main() {
    std::printf("Test: MegaVoice/S.Art sanitizer & SFF2 Guitar translator\n");

    // ── 1. Dual-include CasmTrackView sanity ────────────────────────────
    {
        CasmTrackView v{};
        TEST("CasmTrackView defaults match contract",
             v.high_key == 11 && v.note_low == 0 && v.note_high == 127 &&
             v.note_mute_mask == 0x0FFF && v.chord_mute_mask == 0x3FFFFFFFFULL && !v.has_casm);
        // Verify cv_splitter.h symbol is cleanly visible in same TU.
        TEST("dual-include Order A exposes cv_splitter symbols",
             maxCvCountFor(ai_arranger::uasf::SectionType::Main1) == 6);
    }

    // ── 2. Confidence Gate Signals S1..S6 ───────────────────────────────
    {
        const CasmTrackView plain_casm{};
        const std::vector<NoteSpan> normal_notes = {
            {0, 240, 52, 50, false}, {240, 480, 55, 55, false}, {480, 720, 59, 60, false}
        };

        // S1: MegaVoice banks (63/120, 63/60, 8/60) & name "Mega"
        const auto c_63_120 = classifyTrackArticulation(normal_notes, 63, 120, 21, "Chord1", TrackRole::Chord, plain_casm);
        TEST("S1 bank 63/120 triggers is_megavoice (+75)",
             c_63_120.is_megavoice && !c_63_120.is_sart && c_63_120.confidence == 75 &&
             c_63_120.family == InstrumentClass::GuitarPlucked);

        const auto c_63_60 = classifyTrackArticulation(normal_notes, 63, 60, 23, "Bass", TrackRole::Bass, plain_casm);
        TEST("S1 bank 63/60 triggers is_megavoice BassPlucked (+75)",
             c_63_60.is_megavoice && c_63_60.confidence == 75 &&
             c_63_60.family == InstrumentClass::BassPlucked);

        const auto c_8_60 = classifyTrackArticulation(normal_notes, 8, 60, 25, "SteelGtr", TrackRole::Chord, plain_casm);
        TEST("S1 bank 8/60 triggers is_megavoice (+75)",
             c_8_60.is_megavoice && c_8_60.confidence == 75);

        const auto c_name = classifyTrackArticulation(normal_notes, -1, -1, -1, "MeGaAcGt", TrackRole::Chord, plain_casm);
        TEST("S1 case-insensitive 'Mega' name triggers is_megavoice (+75)",
             c_name.is_megavoice && c_name.confidence == 75);

        // S2: S.Art / S.Art2 bank (8/64..127) and name ("S.Art" / "SA2")
        const std::vector<NoteSpan> sart_notes = {
            {0, 120, 20, 80, false},   // low key-switch (< 36 for Brass)
            {120, 240, 24, 80, false}, // low key-switch
            {240, 480, 60, 42, false}, // expressive soft note
            {480, 720, 64, 118, false} // expressive loud note
        };
        const auto c_sart_bank = classifyTrackArticulation(sart_notes, 8, 96, 56, "Trumpet", TrackRole::Phrase1, plain_casm);
        TEST("S2 bank 8/96 sets is_sart=true, is_megavoice=false",
             c_sart_bank.is_sart && !c_sart_bank.is_megavoice &&
             c_sart_bank.family == InstrumentClass::BrassWoodwind && c_sart_bank.confidence >= 55);

        const auto c_sart_name = classifyTrackArticulation(normal_notes, 0, 0, 65, "SA2 TenorSax", TrackRole::Phrase1, plain_casm);
        TEST("S2 name 'SA2' sets is_sart=true, is_megavoice=false",
             c_sart_name.is_sart && !c_sart_name.is_megavoice);

        // S.Art sanitization strips key-switches (< floor) WITHOUT altering natural velocities!
        const auto r_sart = sanitizeMegaVoiceTrack(sart_notes, 8, 96, 56, "Trumpet", TrackRole::Phrase1, plain_casm);
        TEST("S.Art strips low key-switches while preserving natural velocities untouched",
             r_sart.stats.fx_keys_stripped == 2 &&
             r_sart.stats.vel_reshaped_notes == 0 &&
             r_sart.notes.size() == 2 &&
             r_sart.notes[0].pitch == 60 && r_sart.notes[0].vel == 42 &&
             r_sart.notes[1].pitch == 64 && r_sart.notes[1].vel == 118 &&
             hasCode(r_sart.diagnostic_codes, "STYLE_QUAL_SART_ARTICULATION"));

        // S3 alone (+45 < 60) is not enough; S3 + S4 (+45 + 25 = 70 >= 60) triggers is_megavoice!
        std::vector<NoteSpan> s3_only = {
            {0, 240, 52, 80, false}, {240, 480, 98, 80, false}, {480, 720, 102, 80, false}
        };
        const auto c_s3 = classifyTrackArticulation(s3_only, -1, -1, -1, "Chord1", TrackRole::Chord, plain_casm);
        TEST("S3 alone (+45) below threshold 60",
             !c_s3.is_megavoice && c_s3.confidence == 45 && c_s3.high_fx_ct == 2);

        std::vector<NoteSpan> s3_s4 = s3_only;
        s3_s4.push_back({720, 840, 5, 80, false}); // chord-event range 0..11 -> S4 (+25)
        const auto c_s3_s4 = classifyTrackArticulation(s3_s4, -1, -1, -1, "Chord1", TrackRole::Chord, plain_casm);
        TEST("S3 (+45) + S4 (+25) = 70 triggers is_megavoice",
             c_s3_s4.is_megavoice && c_s3_s4.confidence == 70 && c_s3_s4.chord_range_ct == 1);

        // S5 alone (+25 < 60) NEVER triggers is_megavoice; S3 (+45) + S5 (+25) = 70 triggers!
        const auto stepped = makeSteppedFourZonePattern(52);
        const auto c_s5_only = classifyTrackArticulation(stepped, -1, -1, -1, "Guitar", TrackRole::Chord, plain_casm);
        TEST("S5 alone (+25) never sufficient alone (< 60)",
             !c_s5_only.is_megavoice && c_s5_only.confidence == 25 &&
             std::popcount(static_cast<unsigned>(c_s5_only.zone_mask)) == 4);

        auto s3_s5 = stepped;
        s3_s5.push_back({2700, 2800, 96, 80, false});
        s3_s5.push_back({2800, 2900, 100, 80, false});
        const auto c_s3_s5 = classifyTrackArticulation(s3_s5, -1, -1, -1, "Guitar", TrackRole::Chord, plain_casm);
        TEST("S3 (+45) + S5 (+25) = 70 triggers is_megavoice",
             c_s3_s5.is_megavoice && c_s3_s5.confidence == 70);

        // S6 (NTR == 2, +20) alone < 60; S3 (+45) + S6 (+20) = 65 >= 60!
        CasmTrackView gtr_casm{};
        gtr_casm.has_casm = true;
        gtr_casm.ntr = 2;
        gtr_casm.ntt = 1;
        const auto c_s6_only = classifyTrackArticulation(normal_notes, -1, -1, -1, "Chord1", TrackRole::Chord, gtr_casm);
        TEST("S6 alone (+20) below threshold 60 and infers GuitarPlucked",
             !c_s6_only.is_megavoice && c_s6_only.confidence == 20 &&
             c_s6_only.family == InstrumentClass::GuitarPlucked);

        const auto c_s3_s6 = classifyTrackArticulation(s3_only, -1, -1, -1, "Chord1", TrackRole::Chord, gtr_casm);
        TEST("S3 (+45) + S6 (+20) = 65 triggers is_megavoice",
             c_s3_s6.is_megavoice && c_s3_s6.confidence == 65);
    }

    // ── 3. Hard Non-MegaVoice Veto (Expressive Piano / Strings / Pad) ───
    {
        const CasmTrackView plain_casm{};

        // Expressive Grand Piano: smooth vel 25..124, wide 88-key runs up to C7 (96..104)
        std::vector<NoteSpan> piano_notes;
        for (uint32_t i = 0; i < 100; ++i) {
            const uint8_t vel = static_cast<uint8_t>(25 + i); // 25..124
            const uint8_t pitch = static_cast<uint8_t>(21 + (i % 84)); // 21..104 (includes >= 96!)
            piano_notes.push_back({i * 120, i * 120 + 240, pitch, vel, false});
        }
        const auto r_piano = sanitizeMegaVoiceTrack(piano_notes, 0, 0, 0, "Grand Piano", TrackRole::Chord, plain_casm);
        bool piano_untouched = (r_piano.notes.size() == piano_notes.size());
        for (size_t i = 0; piano_untouched && i < piano_notes.size(); ++i) {
            if (r_piano.notes[i].pitch != piano_notes[i].pitch ||
                r_piano.notes[i].vel != piano_notes[i].vel ||
                r_piano.notes[i].off_tick != piano_notes[i].off_tick) {
                piano_untouched = false;
            }
        }
        TEST("Hard Veto protects expressive Grand Piano (vetoed=true, 0 stripped, 0 vel changed)",
             r_piano.classification.vetoed && !r_piano.classification.is_megavoice &&
             r_piano.stats.fx_keys_stripped == 0 && r_piano.stats.vel_reshaped_notes == 0 &&
             piano_untouched);

        // Expressive StringsOrchestral (notes >= G1=31 up to C7=96, stepped velocities)
        std::vector<NoteSpan> str_notes = makeSteppedFourZonePattern(48);
        str_notes.push_back({3000, 3480, 96, 110, false});
        str_notes.push_back({3480, 3960, 100, 115, false});
        const auto r_str = sanitizeMegaVoiceTrack(str_notes, 0, 0, 48, "Strings Ensemble", TrackRole::Pad, plain_casm);
        TEST("Hard Veto protects expressive StringsOrchestral (vetoed=true, 0 vel reshaped)",
             r_str.classification.vetoed && !r_str.classification.is_megavoice &&
             r_str.stats.vel_reshaped_notes == 0 && r_str.notes.size() == str_notes.size());

        // Expressive SynthLeadPad
        std::vector<NoteSpan> pad_notes = makeSteppedFourZonePattern(48);
        const auto r_pad = sanitizeMegaVoiceTrack(pad_notes, 0, 0, 89, "Warm Pad", TrackRole::Pad, plain_casm);
        TEST("Hard Veto protects SynthLeadPad (vetoed=true, 0 vel reshaped)",
             r_pad.classification.vetoed && !r_pad.classification.is_megavoice &&
             r_pad.stats.vel_reshaped_notes == 0 && r_pad.notes.size() == pad_notes.size());

        // Explicit Sanitize mode overrides Auto veto
        SanitizeOptions force_opt{};
        force_opt.mode = MegaVoiceMode::Sanitize;
        const auto r_forced = sanitizeMegaVoiceTrack(piano_notes, 0, 0, 0, "Grand Piano", TrackRole::Chord, plain_casm, force_opt);
        TEST("MegaVoiceMode::Sanitize overrides veto when explicitly requested",
             r_forced.stats.vel_reshaped_notes > 0);
    }

    // ── 4. Exhaustive 1..127 Velocity-Zone Curve Math & M2.3 Zero-Inversion Proof ─
    {
        const CasmTrackView plain_casm{};
        std::vector<NoteSpan> all_vels;
        all_vels.reserve(127);
        for (uint32_t v = 1; v <= 127; ++v) {
            all_vels.push_back({v * 480, v * 480 + 480, 60, static_cast<uint8_t>(v), false});
        }

        // Default drop_noise_zone = true
        SanitizeOptions opt_drop{};
        opt_drop.ppqn = 480;
        opt_drop.drop_noise_zone = true;
        const auto r_drop = sanitizeMegaVoiceTrack(all_vels, 63, 120, 21, "MegaGtr", TrackRole::Chord, plain_casm, opt_drop);

        uint8_t min_z1 = 127, max_z1 = 0;
        uint8_t min_z2 = 127, max_z2 = 0;
        uint8_t min_z3 = 127, max_z3 = 0;
        bool z1_dur_ok = true, z2_dur_ok = true, z3_dur_ok = true;
        bool monotonic_ok = true;
        uint8_t prev_z1 = 0, prev_z2 = 0, prev_z3 = 0;

        for (const auto& n : r_drop.notes) {
            const uint32_t src_v = n.on_tick / 480;
            const uint32_t dur = n.off_tick - n.on_tick;
            if (src_v <= 64) {
                min_z1 = std::min(min_z1, n.vel);
                max_z1 = std::max(max_z1, n.vel);
                if (dur != 480) z1_dur_ok = false;
                if (n.vel < prev_z1) monotonic_ok = false;
                prev_z1 = n.vel;
            } else if (src_v <= 90) {
                min_z2 = std::min(min_z2, n.vel);
                max_z2 = std::max(max_z2, n.vel);
                if (dur != 120) z2_dur_ok = false; // clamped to ppqn/4 = 120
                if (n.vel < prev_z2) monotonic_ok = false;
                prev_z2 = n.vel;
            } else if (src_v <= 100) {
                min_z3 = std::min(min_z3, n.vel);
                max_z3 = std::max(max_z3, n.vel);
                if (dur != 480) z3_dur_ok = false;
                if (n.vel < prev_z3) monotonic_ok = false;
                prev_z3 = n.vel;
            } else {
                // Zone 4 must be stripped when drop_noise_zone == true!
                monotonic_ok = false;
            }
        }

        TEST("drop_noise_zone=true strips all 27 Zone 4 notes (101..127)",
             r_drop.notes.size() == 100 && r_drop.stats.noise_zone_stripped == 27 &&
             r_drop.stats.muted_gates_clamped == 26);
        TEST("Zone 1 [1..64] maps to exact [68..106] with unchanged duration",
             min_z1 == 68 && max_z1 == 106 && z1_dur_ok);
        TEST("Zone 2 [65..90] maps to exact [38..56] with gate clamped <= ppqn/4",
             min_z2 == 38 && max_z2 == 56 && z2_dur_ok);
        TEST("Zone 3 [91..100] maps to exact [50..66] with unchanged duration",
             min_z3 == 50 && max_z3 == 66 && z3_dur_ok);
        TEST("M2.3 Zero-Inversion Proof: max(65..127) <= 66 < 68 <= min(1..64) & monotonic",
             monotonic_ok && std::max(max_z2, max_z3) <= 66 && min_z1 >= 68 &&
             r_drop.stats.upper_zone_rms_vel <= 66.0 && r_drop.stats.normal_zone_rms_vel >= 68.0 &&
             r_drop.stats.upper_zone_rms_vel < r_drop.stats.normal_zone_rms_vel);

        // Ghost mode: drop_noise_zone = false
        SanitizeOptions opt_ghost{};
        opt_ghost.ppqn = 480;
        opt_ghost.drop_noise_zone = false;
        const auto r_ghost = sanitizeMegaVoiceTrack(all_vels, 63, 120, 21, "MegaGtr", TrackRole::Chord, plain_casm, opt_ghost);
        uint8_t min_z4 = 127, max_z4 = 0;
        bool z4_dur_ok = true;
        for (const auto& n : r_ghost.notes) {
            const uint32_t src_v = n.on_tick / 480;
            if (src_v >= 101) {
                min_z4 = std::min(min_z4, n.vel);
                max_z4 = std::max(max_z4, n.vel);
                if (n.off_tick - n.on_tick != 60) z4_dur_ok = false; // clamped to ppqn/8 = 60
            }
        }
        TEST("drop_noise_zone=false ghosts Zone 4 to [24..30] & duration <= ppqn/8",
             r_ghost.notes.size() == 127 && r_ghost.stats.noise_zone_stripped == 27 &&
             min_z4 == 24 && max_z4 == 30 && z4_dur_ok &&
             r_ghost.stats.upper_zone_rms_vel < r_ghost.stats.normal_zone_rms_vel);
    }

    // ── 5. Per-Family Thresholds, Bass Zone-3 Fold, Preserve Mode, Drum Bypass ─
    {
        const CasmTrackView plain_casm{};

        // MegaVoice Bass (63/60): floor 24, ceiling 84, Zone 3 pitch > 67 folded down 12 st
        const std::vector<NoteSpan> bass_notes = {
            {0, 120, 4, 60, false},     // 0..11 chord-event -> stripped
            {120, 240, 20, 60, false},  // < 24 low key-switch -> stripped
            {240, 480, 36, 50, false},  // valid normal bass note
            {480, 720, 72, 95, false},  // Zone 3 (> 67) -> folded down 12 st to 60!
            {720, 840, 84, 50, false},  // >= 84 high FX on Bass -> stripped
            {840, 960, 100, 50, false}  // >= 84 high FX on Bass -> stripped
        };
        const auto r_bass = sanitizeMegaVoiceTrack(bass_notes, 63, 60, 23, "MegaBass", TrackRole::Bass, plain_casm);
        TEST("MegaVoice Bass strips 0..11, <24, >=84 and folds Zone 3 pitch > 67 down 12 st",
             r_bass.stats.chord_range_stripped == 1 &&
             r_bass.stats.fx_keys_stripped == 3 &&
             r_bass.notes.size() == 2 &&
             r_bass.notes[0].pitch == 36 &&
             r_bass.notes[1].pitch == 60 &&
             hasCode(r_bass.diagnostic_codes, "STYLE_QUAL_MEGAVOICE_FX_KEYS") &&
             hasCode(r_bass.diagnostic_codes, "STYLE_QUAL_MEGAVOICE_VEL_ZONES"));

        // Preserve mode: strips ONLY mandatory 0..11, preserves >= 96 and raw velocities!
        SanitizeOptions opt_preserve{};
        opt_preserve.mode = MegaVoiceMode::Preserve;
        const auto r_pres = sanitizeMegaVoiceTrack(bass_notes, 63, 60, 23, "MegaBass", TrackRole::Bass, plain_casm, opt_preserve);
        TEST("Preserve mode strips ONLY 0..11 and keeps all other pitches/velocities untouched",
             r_pres.stats.chord_range_stripped == 1 &&
             r_pres.stats.fx_keys_stripped == 0 &&
             r_pres.stats.vel_reshaped_notes == 0 &&
             r_pres.notes.size() == 5 &&
             r_pres.notes[0].pitch == 20 && r_pres.notes[0].vel == 60 &&
             r_pres.notes[4].pitch == 100 && r_pres.notes[4].vel == 50);

        // Drum role: bypasses even 0..11 stripping and velocity shaping!
        const std::vector<NoteSpan> drum_notes = {
            {0, 120, 5, 110, false}, {120, 240, 36, 120, false}, {240, 360, 100, 40, false}
        };
        const auto r_drum = sanitizeMegaVoiceTrack(drum_notes, 63, 120, 0, "MegaDrum", TrackRole::Drum, plain_casm);
        TEST("Drum role bypasses 0..11 strip and MegaVoice sanitization",
             !r_drum.classification.is_megavoice &&
             r_drum.stats.chord_range_stripped == 0 &&
             r_drum.stats.vel_reshaped_notes == 0 &&
             r_drum.notes.size() == 3 && r_drum.notes[0].pitch == 5);
    }

    // ── 6. SFF2 Guitar Translator (NTR == 2) ────────────────────────────
    {
        // 6a. Collapsed 0-tick Stroke (NTT == 1) chord -> deterministic 12 ms down-strum stagger
        CasmTrackView gtr_stroke{};
        gtr_stroke.has_casm = true;
        gtr_stroke.ntr = 2;
        gtr_stroke.ntt = 1; // Stroke

        const std::vector<NoteSpan> collapsed_chord = {
            {0, 480, 64, 55, false},
            {0, 480, 40, 55, false},
            {0, 480, 55, 55, false},
            {0, 480, 47, 55, false},
            {0, 480, 59, 55, false},
            {0, 480, 52, 55, false}
        };
        SanitizeOptions gopt{};
        gopt.ppqn = 480;
        gopt.tempo_mpqn = 500000; // 120 BPM -> 12 ms = 11 ticks, 60 ms = 57 ticks
        const auto r_stroke = sanitizeMegaVoiceTrack(collapsed_chord, 63, 120, 21, "AcGuitar", TrackRole::Chord, gtr_stroke, gopt);

        bool monotonic_strum = (r_stroke.notes.size() == 6);
        for (size_t i = 1; monotonic_strum && i < r_stroke.notes.size(); ++i) {
            if (!(r_stroke.notes[i].on_tick > r_stroke.notes[i - 1].on_tick &&
                  r_stroke.notes[i].pitch > r_stroke.notes[i - 1].pitch &&
                  r_stroke.notes[i].off_tick > r_stroke.notes[i].on_tick &&
                  r_stroke.notes[i].from_guitar_ntr)) {
                monotonic_strum = false;
            }
        }
        const uint32_t spread = r_stroke.notes.empty() ? 0 : (r_stroke.notes.back().on_tick - r_stroke.notes.front().on_tick);
        TEST("SFF2 Guitar Stroke injects monotonic down-strum stagger within <=60 ms",
             monotonic_strum && spread >= 4 && spread <= 57 &&
             r_stroke.stats.guitar_mode_candidate &&
             hasCode(r_stroke.diagnostic_codes, "STYLE_QUAL_SFF2_GUITAR_MODE"));

        // 6b. Existing valid strum micro-timing (15..45 ms) is preserved untouched
        const std::vector<NoteSpan> natural_strum = {
            {0, 480, 40, 50, false},
            {12, 480, 47, 50, false},
            {24, 480, 52, 50, false},
            {36, 480, 56, 50, false}
        };
        const auto r_nat = sanitizeMegaVoiceTrack(natural_strum, 63, 120, 21, "AcGuitar", TrackRole::Chord, gtr_stroke, gopt);
        TEST("SFF2 Guitar preserves existing monotonic strum offsets (0,12,24,36)",
             r_nat.notes.size() == 4 &&
             r_nat.notes[0].on_tick == 0 && r_nat.notes[1].on_tick == 12 &&
             r_nat.notes[2].on_tick == 24 && r_nat.notes[3].on_tick == 36);

        // 6c. 8-voice chord trimmed to <= 6 voices (keeps lowest bass string + top 5 upper strings)
        const std::vector<NoteSpan> eight_voices = {
            {0, 480, 40, 50, false},
            {0, 480, 43, 50, false},
            {0, 480, 47, 50, false},
            {0, 480, 50, 50, false},
            {0, 480, 52, 50, false},
            {0, 480, 55, 50, false},
            {0, 480, 59, 50, false},
            {0, 480, 64, 50, false}
        };
        CasmTrackView gtr_all{};
        gtr_all.has_casm = true;
        gtr_all.ntr = 2;
        gtr_all.ntt = 0; // All-Purpose
        const auto r_trim = sanitizeMegaVoiceTrack(eight_voices, 63, 120, 21, "AcGuitar", TrackRole::Chord, gtr_all, gopt);
        std::set<uint8_t> kept_pitches;
        for (const auto& n : r_trim.notes) kept_pitches.insert(n.pitch);
        TEST("SFF2 Guitar trims 8-voice chord to 6 (lowest bass + top 5 upper strings)",
             r_trim.notes.size() == 6 &&
             r_trim.stats.guitar_voices_trimmed == 2 &&
             maxSimultaneousVoices(r_trim.notes) <= 6 &&
             kept_pitches == std::set<uint8_t>({40, 50, 52, 55, 59, 64}));

        // 6d. 36-semitone wide span (40..76) folded to <= 24 st inside [40, 88] without collision
        const std::vector<NoteSpan> wide_chord = {
            {0, 480, 40, 50, false}, // E2
            {0, 480, 55, 50, false}, // G3
            {0, 480, 59, 50, false}, // B3
            {0, 480, 76, 50, false}  // E5 (36 st above E2!)
        };
        const auto r_fold = sanitizeMegaVoiceTrack(wide_chord, 63, 120, 21, "AcGuitar", TrackRole::Chord, gtr_all, gopt);
        uint8_t lo_p = 127, hi_p = 0;
        std::set<uint8_t> uniq_p;
        for (const auto& n : r_fold.notes) {
            lo_p = std::min(lo_p, n.pitch);
            hi_p = std::max(hi_p, n.pitch);
            uniq_p.insert(n.pitch);
        }
        TEST("SFF2 Guitar folds 36-st span into <=24 st inside [40, 88] without collision",
             r_fold.notes.size() == 4 &&
             r_fold.stats.guitar_span_folded >= 1 &&
             lo_p >= 40 && hi_p <= 88 && (hi_p - lo_p) <= 24 &&
             uniq_p.size() == 4);
    }

    // ── 7. Boundary, Malformed Inputs & Determinism ─────────────────────
    {
        const CasmTrackView plain_casm{};
        SanitizeOptions zero_opt{};
        zero_opt.ppqn = 0;
        zero_opt.tempo_mpqn = 0;

        // Empty input
        const auto r_empty = sanitizeMegaVoiceTrack({}, 63, 120, 21, "MegaGtr", TrackRole::Chord, plain_casm, zero_opt);
        TEST("Empty input + ppqn=0 + tempo=0 handled cleanly without div-by-zero",
             r_empty.notes.empty() && r_empty.stats.normal_zone_rms_vel == 0.0 &&
             r_empty.stats.upper_zone_rms_vel == 0.0);

        // Malformed notes: vel=0, vel=200, pitch=128, off_tick <= on_tick, on_tick=UINT32_MAX
        const std::vector<NoteSpan> bad_notes = {
            {0, 100, 60, 0, false},                                             // vel == 0 -> drop
            {100, 200, 60, 200, false},                                         // vel > 127 -> drop
            {200, 300, 128, 60, false},                                         // pitch > 127 -> drop
            {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(), 60, 50, false}, // overflow -> drop
            {500, 500, 60, 50, false},                                          // off == on -> clamp off = on+1
            {600, 400, 64, 80, false}                                           // off < on -> clamp off = on+1
        };
        CasmTrackView gtr{};
        gtr.has_casm = true;
        gtr.ntr = 2;
        gtr.ntt = 1;
        const auto r_bad1 = sanitizeMegaVoiceTrack(bad_notes, 63, 120, 21, "MegaGtr", TrackRole::Chord, gtr, zero_opt);
        const auto r_bad2 = sanitizeMegaVoiceTrack(bad_notes, 63, 120, 21, "MegaGtr", TrackRole::Chord, gtr, zero_opt);
        bool all_valid = (r_bad1.notes.size() == 2);
        for (const auto& n : r_bad1.notes) {
            if (n.pitch > 127 || n.vel == 0 || n.vel > 127 || n.off_tick <= n.on_tick) all_valid = false;
        }
        bool deterministic = (r_bad1.notes.size() == r_bad2.notes.size() &&
                              r_bad1.classification.rationale == r_bad2.classification.rationale);
        for (size_t i = 0; deterministic && i < r_bad1.notes.size(); ++i) {
            if (r_bad1.notes[i].on_tick != r_bad2.notes[i].on_tick ||
                r_bad1.notes[i].off_tick != r_bad2.notes[i].off_tick ||
                r_bad1.notes[i].pitch != r_bad2.notes[i].pitch ||
                r_bad1.notes[i].vel != r_bad2.notes[i].vel) {
                deterministic = false;
            }
        }
        TEST("Malformed notes fail-safe (zero UB/overflow) and clamp inverted off_tick",
             all_valid);
        TEST("Sanitizer output is 100% deterministic across repeated calls",
             deterministic);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
