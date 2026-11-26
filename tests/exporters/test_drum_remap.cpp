// Track B Q3a — Unit tests for src/exporters/korg/drum_remap.h (+ Track A IDrumKitKeyMap seam).
// 100% synthetic/CC0 fixtures; zero private data; includes drum_remap.h BEFORE
// source_chord_normalizer.h to verify order-independent header inclusion.

#include "exporters/korg/drum_remap.h"
#include "exporters/korg/source_chord_normalizer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;

static int failures = 0;
static int passes   = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } \
} while (0)

static NoteSpan mkDrum(uint32_t on, uint32_t off, uint8_t pitch, uint8_t vel = 100) {
    NoteSpan s{};
    s.on_tick  = on;
    s.off_tick = off;
    s.pitch    = pitch;
    s.vel      = vel;
    return s;
}

static double pearsonR(const std::vector<double>& x, const std::vector<double>& y) {
    if (x.size() != y.size() || x.size() < 2) return 0.0;
    double mx = 0.0, my = 0.0;
    for (size_t i = 0; i < x.size(); ++i) { mx += x[i]; my += y[i]; }
    mx /= static_cast<double>(x.size());
    my /= static_cast<double>(y.size());
    double num = 0.0, dx2 = 0.0, dy2 = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double dx = x[i] - mx;
        const double dy = y[i] - my;
        num += dx * dy;
        dx2 += dx * dx;
        dy2 += dy * dy;
    }
    const double den = std::sqrt(dx2 * dy2);
    return den > 0.0 ? (num / den) : 0.0;
}

int main() {
    std::printf("Test: Track B Q3a — XG/Revo! Drum Remapper & IDrumKitKeyMap Seam\n");

    // ── 1. FactoryPa5xDrumKitKeyMap: populated range, families, and all 7 GM2/Pa5X choke groups ──
    {
        FactoryPa5xDrumKitKeyMap stdKit(DrumKitCategory::StandardPopRock);
        TEST("Factory Standard kit has GM1 35..81 and GM2 24..99",
             stdKit.hasKey(24) && stdKit.hasKey(36) && stdKit.hasKey(42) &&
             stdKit.hasKey(82) && stdKit.hasKey(99) && !stdKit.hasKey(13) && !stdKit.hasKey(120));
        TEST("Factory Standard families B/S/H/T/C/L/X",
             stdKit.familyOf(36) == 'B' && stdKit.familyOf(38) == 'S' &&
             stdKit.familyOf(42) == 'H' && stdKit.familyOf(48) == 'T' &&
             stdKit.familyOf(49) == 'C' && stdKit.familyOf(62) == 'L' &&
             stdKit.familyOf(27) == 'X');
        TEST("Factory Standard all 7 choke groups populated",
             stdKit.keyInfo(42).choke_group == 1 && stdKit.keyInfo(44).choke_group == 1 && stdKit.keyInfo(46).choke_group == 1 &&
             stdKit.keyInfo(71).choke_group == 2 && stdKit.keyInfo(72).choke_group == 2 &&
             stdKit.keyInfo(73).choke_group == 3 && stdKit.keyInfo(74).choke_group == 3 &&
             stdKit.keyInfo(78).choke_group == 4 && stdKit.keyInfo(79).choke_group == 4 &&
             stdKit.keyInfo(80).choke_group == 5 && stdKit.keyInfo(81).choke_group == 5 &&
             stdKit.keyInfo(86).choke_group == 6 && stdKit.keyInfo(87).choke_group == 6 &&
             stdKit.keyInfo(25).choke_group == 7 && stdKit.keyInfo(29).choke_group == 7 && stdKit.keyInfo(30).choke_group == 7);
    }

    // ── 2. All XG/Revo! extended keys 13..34 & 82..127 across Standard, Brush, Electronic, Latin, Ethnic, SFX ──
    {
        std::vector<NoteSpan> extNotes;
        uint32_t t = 0;
        for (uint8_t n = 13; n <= 34; ++n, t += 120) extNotes.push_back(mkDrum(t, t + 240, n, 100));
        for (uint8_t n = 82; n <= 127; ++n, t += 120) extNotes.push_back(mkDrum(t, t + 240, n, 100));

        // Standard XG
        FactoryPa5xDrumKitKeyMap stdKit(DrumKitCategory::StandardPopRock);
        const auto rStd = remapDrumTrack(extNotes, DrumKitCategory::StandardPopRock, stdKit, false, 480);
        bool allInKit = true, allOnLtOff = true;
        for (const auto& s : rStd.notes) {
            if (!stdKit.hasKey(s.pitch)) allInKit = false;
            if (!(s.on_tick < s.off_tick)) allOnLtOff = false;
        }
        // Unassigned 116..120 (5) + 122..127 (6) = 11 keys dropped cleanly on Standard Factory
        TEST("Standard XG: every surviving extended hit lands on a populated key & on<off",
             allInKit && allOnLtOff && rStd.stats.notes_dropped == 11 && rStd.notes.size() == extNotes.size() - 11);

        // Revo! 62/100: 13 -> 35 (Kick B), 14 -> 36 (Kick B), 22 -> 46 (short gate), 91 -> 49 (short gate)
        const std::vector<NoteSpan> revoChecks = {
            mkDrum(0, 480, 13, 100), mkDrum(480, 960, 14, 100),
            mkDrum(960, 1440, 22, 100), mkDrum(1440, 1920, 91, 100),
            mkDrum(1920, 2400, 121, 100)
        };
        const auto rRevo = remapDrumTrack(revoChecks, 62, 100, 0, "Revo!PopKit", uasf::TrackRole::Drum, stdKit, 480);
        TEST("Revo! 62/100: 13->35, 14->36, 22->46(short), 91->49(short), 121->49",
             rRevo.notes.size() == 5 &&
             rRevo.notes[0].pitch == 35 && rRevo.notes[1].pitch == 36 &&
             rRevo.notes[2].pitch == 46 && (rRevo.notes[2].off_tick - rRevo.notes[2].on_tick) <= 120 &&
             rRevo.notes[3].pitch == 49 && (rRevo.notes[3].off_tick - rRevo.notes[3].on_tick) <= 120 &&
             rRevo.notes[4].pitch == 49);

        // BrushJazz: 25->38, 26->40, 27->39, 16->38 (ghost vel ~60)
        FactoryPa5xDrumKitKeyMap brushKit(DrumKitCategory::BrushJazz);
        const std::vector<NoteSpan> brushNotes = {
            mkDrum(0, 240, 25, 100), mkDrum(240, 480, 26, 100),
            mkDrum(480, 720, 27, 100), mkDrum(720, 960, 16, 100)
        };
        const auto rBrush = remapDrumTrack(brushNotes, 127, 0, 40, "BrushKit", uasf::TrackRole::Drum, brushKit, 480);
        TEST("BrushJazz overrides preserve Brush Snare family (25->38, 26->40, 27->39, 16->38)",
             rBrush.category == DrumKitCategory::BrushJazz && rBrush.notes.size() == 4 &&
             rBrush.notes[0].pitch == 38 && rBrush.notes[1].pitch == 40 &&
             rBrush.notes[2].pitch == 39 && rBrush.notes[3].pitch == 38 && rBrush.notes[3].vel == 60);

        // Electronic: 13/14->36, 19->39, 20..22->42, 23->46, 88->75, 90->56
        FactoryPa5xDrumKitKeyMap elecKit(DrumKitCategory::Electronic);
        const std::vector<NoteSpan> elecNotes = {
            mkDrum(0, 120, 13, 100), mkDrum(120, 240, 19, 100),
            mkDrum(240, 360, 20, 100), mkDrum(360, 480, 23, 100),
            mkDrum(480, 600, 88, 100), mkDrum(600, 720, 90, 100)
        };
        const auto rElec = remapDrumTrack(elecNotes, 127, 0, 25, "Analog808", uasf::TrackRole::Drum, elecKit, 480);
        TEST("Electronic 808 overrides (13->36, 19->39, 20->42, 23->46, 88->75, 90->56)",
             rElec.category == DrumKitCategory::Electronic && rElec.notes.size() == 6 &&
             rElec.notes[0].pitch == 36 && rElec.notes[1].pitch == 39 &&
             rElec.notes[2].pitch == 42 && rElec.notes[3].pitch == 46 &&
             rElec.notes[4].pitch == 75 && rElec.notes[5].pitch == 56);

        // LatinPerc: 100% of 13..23 & 88..103 stay in Latin ('L') family
        FactoryPa5xDrumKitKeyMap latinKit(DrumKitCategory::LatinPerc);
        const std::vector<NoteSpan> latinNotes = {
            mkDrum(0, 120, 13, 100), mkDrum(120, 240, 16, 100), mkDrum(240, 360, 19, 100),
            mkDrum(360, 480, 88, 100), mkDrum(480, 600, 92, 100), mkDrum(600, 720, 96, 100)
        };
        const auto rLatin = remapDrumTrack(latinNotes, 127, 0, 48, "LatinPerc", uasf::TrackRole::Percussion, latinKit, 480);
        bool allLatin = (rLatin.notes.size() == latinNotes.size());
        for (const auto& s : rLatin.notes) {
            if (latinKit.familyOf(s.pitch) != 'L') allLatin = false;
        }
        TEST("LatinPerc keeps 100% of remapped hits inside 'L' family", allLatin);

        // EthnicArabic on Factory kit -> maps extended keys + emits STYLE_QUAL_EXTENDED_DRUM_MAP advisory
        FactoryPa5xDrumKitKeyMap ethKit(DrumKitCategory::EthnicArabic);
        const auto rEthFac = remapDrumTrack({mkDrum(0, 240, 18, 90), mkDrum(240, 480, 95, 90)},
                                            127, 0, 115, "ArabicKit", uasf::TrackRole::Drum, ethKit, 480);
        bool hasExtDiag = false;
        for (const auto& d : rEthFac.diagnostic_codes) if (d == "STYLE_QUAL_EXTENDED_DRUM_MAP") hasExtDiag = true;
        TEST("EthnicArabic on Factory maps to populated keys & emits advisory",
             rEthFac.notes.size() == 2 && ethKit.hasKey(rEthFac.notes[0].pitch) && hasExtDiag);

        // SFX: populated 27..34 stay 1:1; unpopulated 105 dropped cleanly (never turned into snare/tom)
        FactoryPa5xDrumKitKeyMap sfxKit(DrumKitCategory::Sfx);
        const auto rSfx = remapDrumTrack({mkDrum(0, 240, 27, 90), mkDrum(240, 480, 105, 90)},
                                         126, 0, 0, "SFX1", uasf::TrackRole::Drum, sfxKit, 480);
        TEST("SFX keeps populated 27 1:1 and drops unpopulated 105 cleanly",
             rSfx.notes.size() == 1 && rSfx.notes[0].pitch == 27 && rSfx.stats.notes_dropped == 1);
    }

    // ── 3. TxperfDrumKitKeyMap zero-copy adapter over media::sound::DrumKit ──
    {
        media::sound::DrumKit dk{};
        dk.name = "SyntheticTXKit";
        media::sound::DrumInstrument hh{};
        hh.name = "HiHatGrp";
        hh.choke = 1;
        media::sound::DrumKey k42{};
        k42.key = 42; k42.name = "CHH"; k42.family = 'H'; k42.panPct = -20;
        k42.layers.push_back({0, 0, 0});
        k42.layers.push_back({1, 70, 0});
        hh.keys.push_back(k42);
        dk.instruments.push_back(hh);

        TxperfDrumKitKeyMap txMap(dk);
        const DrumKeyInfo info42 = txMap.keyInfo(42);
        TEST("TxperfDrumKitKeyMap exposes key, family, layers, choke, label, pan",
             txMap.kitName() == "SyntheticTXKit" && txMap.hasKey(42) && !txMap.hasKey(36) &&
             txMap.familyOf(42) == 'H' && info42.choke_group == 1 &&
             info42.layer_count == 2 && info42.label == "CHH" && info42.pan < 64);
    }

    // ── 4. UserdkDrumKitKeyMap: sparse fallback preserving family+choke, ROM(idx==0) guard, vel ceiling, degenerate split ──
    {
        DrumKitKeySummaryView view{};
        view.kit_name = "USERDK01/0";
        view.gm_alignment_score = 0.85;

        // Key 36: Kick (B), multi-layer with velocity ceiling 109 < 127!
        auto& k36 = view.keys[36];
        k36.populated = true; k36.has_ram_layer = true; k36.layer_count = 2;
        k36.vel_tops[0] = 70; k36.vel_tops[1] = 109;
        k36.kmp_family_code = 0; // 'B'
        k36.primary_sample_name = "SynthKick";

        // Key 38: Snare (S), degenerate vel split {80, 127, 127}
        auto& k38 = view.keys[38];
        k38.populated = true; k38.has_ram_layer = true; k38.layer_count = 3;
        k38.vel_tops[0] = 80; k38.vel_tops[1] = 127; k38.vel_tops[2] = 127;
        k38.kmp_family_code = 1; // 'S'
        k38.primary_sample_name = "SynthSnare";

        // Key 42: Closed Hi-Hat (H, choke group 1) — ONLY hi-hat key populated (44 & 46 missing!)
        auto& k42 = view.keys[42];
        k42.populated = true; k42.has_ram_layer = true; k42.layer_count = 1;
        k42.vel_tops[0] = 127; k42.choke_group = 1; k42.kmp_family_code = 2; // 'H'
        k42.primary_sample_name = "SynthCHH";

        // Key 46: ROM(idx==0) unassigned placeholder -> MUST return hasKey(46) == false!
        auto& k46 = view.keys[46];
        k46.populated = true; // even if caller set populated=true, placeholder flag blocks it!
        k46.is_rom_zero_placeholder = true;
        k46.layer_count = 1; k46.kmp_family_code = 2;

        // Key 49: Unchoked Crash Cymbal (C, choke 0) — closer to 46 than 42? (|49-46|=3 < |42-46|=4!)
        // Proves choke-group + family matching beats mere pitch proximity!
        auto& k49 = view.keys[49];
        k49.populated = true; k49.has_ram_layer = true; k49.layer_count = 1;
        k49.vel_tops[0] = 127; k49.choke_group = 0; k49.kmp_family_code = 4; // 'C'
        k49.primary_sample_name = "SynthCrash";

        UserdkDrumKitKeyMap udk(view);
        TEST("UserdkDrumKitKeyMap ROM(idx==0) placeholder returns hasKey == false",
             !udk.hasKey(46) && udk.keyInfo(46).is_rom_zero_placeholder && !udk.keyInfo(46).populated);
        TEST("UserdkDrumKitKeyMap detects degenerate vel split (effective_layers==2)",
             udk.keyInfo(38).has_degenerate_vel && udk.keyInfo(38).effective_layers == 2);
        TEST("UserdkDrumKitKeyMap exposes effective_max_vel == 109 on key 36",
             udk.keyInfo(36).effective_max_vel == 109);

        // Remap Yamaha 22 (Half-Open HH -> wants 46, H, choke 1) + Kick 36 (vel 125)
        const auto rUdk = remapDrumTrack({mkDrum(0, 480, 22, 95), mkDrum(480, 960, 36, 125)},
                                         DrumKitCategory::StandardPopRock, udk, false, 480);
        bool hasVelCeilDiag = false;
        for (const auto& d : rUdk.diagnostic_codes) if (d == "STYLE_QUAL_TARGET_VEL_CEILING") hasVelCeilDiag = true;
        TEST("Sparse USERDK fallback maps HH 22 -> 42 (family 'H' + choke 1, NOT closer unchoked Crash 49!)",
             rUdk.notes.size() == 2 && rUdk.notes[0].pitch == 42 &&
             rUdk.stats.fallback_substituted_notes == 1 && rUdk.stats.choke_preserved_notes == 1);
        TEST("USERDK top-velocity ceiling guard clamps vel 125 -> 109 and emits diagnostic",
             rUdk.notes[1].pitch == 36 && rUdk.notes[1].vel == 109 &&
             rUdk.stats.vel_ceiling_clamped == 1 && hasVelCeilDiag);

        // Non-GM / EthnicArabic USERDK preserves native populated key (e.g. key 18 populated -> stays 18!)
        DrumKitKeySummaryView ethView{};
        ethView.kit_name = "USERDK02/0";
        ethView.gm_alignment_score = 0.25;
        ethView.keys[18].populated = true;
        ethView.keys[18].has_ram_layer = true;
        ethView.keys[18].layer_count = 1;
        ethView.keys[18].vel_tops[0] = 127;
        ethView.keys[18].resolved_family = 'E';
        ethView.keys[18].primary_sample_name = "DarbukaDum";
        UserdkDrumKitKeyMap ethUdk(ethView);
        const auto rEthUdk = remapDrumTrack({mkDrum(0, 240, 18, 100)},
                                            DrumKitCategory::EthnicArabic, ethUdk, false, 480);
        TEST("EthnicArabic / non-GM USERDK passes through populated native key 18 untouched",
             rEthUdk.notes.size() == 1 && rEthUdk.notes[0].pitch == 18 && rEthUdk.notes[0].vel == 100 &&
             rEthUdk.stats.passthrough_notes == 1);
    }

    // ── 5. Velocity contour correlation (M4.4: r >= 0.99 on extended remapped keys) ──
    {
        FactoryPa5xDrumKitKeyMap stdKit(DrumKitCategory::StandardPopRock);
        const std::vector<uint8_t> vels = {20, 38, 55, 72, 90, 108, 124};
        bool allCorrOk = true;
        for (uint8_t key : {uint8_t(16), uint8_t(20), uint8_t(22), uint8_t(88), uint8_t(91), uint8_t(100)}) {
            std::vector<NoteSpan> in;
            std::vector<double> xv, yv;
            uint32_t tk = 0;
            for (uint8_t v : vels) {
                in.push_back(mkDrum(tk, tk + 120, key, v));
                xv.push_back(static_cast<double>(v));
                tk += 120;
            }
            const auto out = remapDrumTrack(in, DrumKitCategory::StandardPopRock, stdKit, false, 480);
            if (out.notes.size() != in.size()) { allCorrOk = false; break; }
            for (const auto& s : out.notes) yv.push_back(static_cast<double>(s.vel));
            const double r = pearsonR(xv, yv);
            if (r < 0.99) allCorrOk = false;
        }
        TEST("Velocity contour Pearson correlation r >= 0.99 across extended remapped keys", allCorrOk);
    }

    // ── 6. detectRhythmTrackSwap() (M4.3) ──
    {
        std::vector<NoteSpan> mainKit;
        for (uint32_t i = 0; i < 12; ++i) {
            const uint8_t p = (i % 3 == 0) ? 36 : ((i % 3 == 1) ? 38 : 42);
            mainKit.push_back(mkDrum(i * 120, i * 120 + 60, p, 100));
        }
        std::vector<NoteSpan> handPerc;
        for (uint32_t i = 0; i < 12; ++i) {
            const uint8_t p = (i % 2 == 0) ? 62 : 70; // Conga / Maracas
            handPerc.push_back(mkDrum(i * 120, i * 120 + 60, p, 90));
        }

        const auto dSwapped = detectRhythmTrackSwap(mainKit, handPerc);
        TEST("detectRhythmTrackSwap: ch8=MainKit + ch9=HandPerc -> swap=true",
             dSwapped.swap_drum_and_perc && dSwapped.ch8_kit_score >= 0.99 && dSwapped.ch9_perc_score >= 0.99);

        const auto dEmpty9 = detectRhythmTrackSwap(mainKit, {});
        TEST("detectRhythmTrackSwap: ch8=MainKit + ch9=empty -> swap=true", dEmpty9.swap_drum_and_perc);

        const auto dNormal = detectRhythmTrackSwap(handPerc, mainKit);
        TEST("detectRhythmTrackSwap: ch8=HandPerc + ch9=MainKit -> swap=false", !dNormal.swap_drum_and_perc);

        const auto dLayered = detectRhythmTrackSwap(mainKit, mainKit);
        TEST("detectRhythmTrackSwap: layered dual kits -> swap=false (never swaps)", !dLayered.swap_drum_and_perc);

        std::vector<NoteSpan> sparseKit(mainKit.begin(), mainKit.begin() + 5);
        const auto dSparse = detectRhythmTrackSwap(sparseKit, handPerc);
        TEST("detectRhythmTrackSwap: ch8 < 8 hits -> swap=false", !dSparse.swap_drum_and_perc);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
