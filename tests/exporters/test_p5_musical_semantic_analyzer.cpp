// Phase 5 P5-M2 unit, adversarial & P5-EXP-01 ground-truth tests for
// Musical Semantic Analyzer (src/intelligence/musical_semantic_analyzer.h).
//
// Proves:
//   * All 14 canonical semantic track roles are inferred accurately purely from
//     musical content with ZERO channel/program/name hints (stripped & misleading!).
//   * Honest fail-closed UnknownAbstain on empty, sparse, single-pitch ambiguous,
//     and random-noise tracks (NEVER fabricates certainty).
//   * Mixed-role detection (both cross-section V1 comping -> V4 arpeggio shift
//     AND intra-section stride LH bass + RH comping).
//   * Inter-track kick-bass lock correlation & Bass/Piano low-register collision detection.
//   * Section-dependent energy arc, swing detection, compound/mixed meter detection,
//     and 2x bit-exact JSON/Markdown determinism.

#include "intelligence/musical_semantic_analyzer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ai_arranger::intelligence;
using ai_arranger::uasf::MidiEvent;
using ai_arranger::uasf::MidiEventType;
using ai_arranger::uasf::SectionDefinition;
using ai_arranger::uasf::SectionType;
using ai_arranger::uasf::StyleDefinition;
using ai_arranger::uasf::TrackDefinition;
using ai_arranger::uasf::TrackRole;

namespace {

int failures = 0;
int passes   = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

void addNote(TrackDefinition& tr, uint32_t on, uint32_t dur, uint8_t pitch, uint8_t vel) {
    tr.events.push_back({MidiEventType::NoteOn, tr.midi_channel, pitch, vel, on});
    tr.events.push_back({MidiEventType::NoteOff, tr.midi_channel, pitch, 0, on + dur});
}

// Creates a completely hint-stripped track: neutral channel 3, empty name, no PC/MSB/LSB,
// generic Accompaniment role, is_drum=false — forcing 100% content-driven musical inference!
TrackDefinition makeStrippedTrack(uint8_t ch = 3) {
    TrackDefinition tr{};
    tr.midi_channel = ch;
    tr.name = "";
    tr.role = TrackRole::Accompaniment;
    tr.is_drum = false;
    return tr;
}

StyleDefinition wrapSingleTrack(const std::string& label, const TrackDefinition& tr,
                                uint32_t bars = 2, uint32_t bpm = 112,
                                SectionType st = SectionType::Main1) {
    StyleDefinition s{};
    s.name = label;
    s.tempo_bpm = bpm;
    s.resolution = 480;
    SectionDefinition sec{};
    sec.type = st;
    sec.name = "Main A";
    sec.bars = bars;
    sec.resolution = 480;
    sec.beats_per_bar = 4;
    sec.beat_note = 4;
    sec.tracks.push_back(tr);
    s.sections.push_back(sec);
    return s;
}

// ── Synthetic Ground-Truth Builders for ALL 14 Canonical Roles (ZERO hints!) ──

TrackDefinition gtKickSnareFoundation(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        addNote(tr, b + 0,    100, 36, 112); // Kick beat 1
        addNote(tr, b + 480,  100, 38, 104); // Snare beat 2
        addNote(tr, b + 960,  100, 36, 108); // Kick beat 3
        addNote(tr, b + 1440, 100, 38, 106); // Snare beat 4
        for (uint32_t e = 0; e < 8; ++e) {
            addNote(tr, b + e * 240, 75, 42, (e % 2 == 0) ? 88 : 66); // Closed HH 8ths
        }
    }
    return tr;
}

TrackDefinition gtDrumKitAuxPercussion(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Shaker (70) + Conga (62, 63, 64) + Claves (75) syncopated 16ths — zero kick/snare!
    const uint32_t pat[] = {0, 120, 360, 480, 720, 840, 1080, 1200, 1440, 1680};
    const uint8_t  key[] = {70, 62, 70,  63,  75,  70,  64,   70,   62,   75};
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (size_t i = 0; i < 10; ++i) {
            addNote(tr, b + pat[i], 70, key[i], static_cast<uint8_t>(75 + (i % 3) * 10));
        }
    }
    return tr;
}

TrackDefinition gtBassFoundation(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // C major walking/root-fifth electric bass in MIDI 36..48
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        addNote(tr, b + 0,    420, 36, 104); // C2
        addNote(tr, b + 480,  400, 40, 94);  // E2
        addNote(tr, b + 960,  420, 43, 98);  // G2
        addNote(tr, b + 1440, 400, 48, 92);  // C3
    }
    return tr;
}

TrackDefinition gtHarmonicAccompaniment(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Smooth, steady on-beat mid-register organ/accordion triad comp (1-beat sustained, compact span=7, even vel)
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (uint32_t beat = 0; beat < 4; ++beat) {
            const uint32_t t = b + beat * 480;
            addNote(tr, t, 460, 60, 80);
            addNote(tr, t, 460, 64, 79);
            addNote(tr, t, 460, 67, 81);
        }
    }
    return tr;
}

TrackDefinition gtChordComping(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Syncopated staccato funk/pop clav/EP chord chops (off-beat 8ths/16ths, short dur 120t)
    const uint32_t hits[] = {240, 600, 840, 1200, 1560, 1800};
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (uint32_t h : hits) {
            addNote(tr, b + h, 110, 60, 88);
            addNote(tr, b + h, 110, 64, 86);
            addNote(tr, b + h, 110, 67, 90);
        }
    }
    return tr;
}

TrackDefinition gtGuitarStrumming(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Acoustic 6-string strummed C major chord with 12-tick staggered down/up strums
    const uint32_t strums[] = {0, 480, 720, 1200, 1440, 1680};
    const uint8_t  voicing[] = {48, 52, 55, 60, 64};
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (size_t si = 0; si < 6; ++si) {
            const bool up = (si % 2 == 1);
            for (size_t k = 0; k < 5; ++k) {
                const uint8_t p = up ? voicing[4 - k] : voicing[k];
                addNote(tr, b + strums[si] + static_cast<uint32_t>(k * 12), 200, p, static_cast<uint8_t>(82 + k * 2));
            }
        }
    }
    return tr;
}

TrackDefinition gtArpeggio(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Broken-chord 8th-note picking pattern C4-E4-G4-C5-G4-E4-C4-G4 (pure chord tones, chord-leap intervals)
    const uint8_t seq[] = {60, 64, 67, 72, 67, 64, 60, 67};
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (size_t i = 0; i < 8; ++i) {
            addNote(tr, b + static_cast<uint32_t>(i * 240), 210, seq[i], static_cast<uint8_t>(78 + (i % 2) * 8));
        }
    }
    return tr;
}

TrackDefinition gtPianoAccompaniment(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Two-hand piano chords with sustain pedal (CC64) and wide span (C3=48 to G4=67 = 19 st)
    tr.events.push_back({MidiEventType::ControlChange, ch, 64, 127, 0});
    tr.events.push_back({MidiEventType::ControlChange, ch, 64, 0, 1880});
    tr.events.push_back({MidiEventType::ControlChange, ch, 64, 127, 1920});
    tr.events.push_back({MidiEventType::ControlChange, ch, 64, 0, 3800});
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (uint32_t beat = 0; beat < 4; ++beat) {
            const uint32_t t = b + beat * 480;
            addNote(tr, t, 440, 48, 84);
            addNote(tr, t, 440, 55, 80);
            addNote(tr, t, 440, 60, 88);
            addNote(tr, t, 440, 64, 86);
            addNote(tr, t, 440, 67, 90);
        }
    }
    return tr;
}

TrackDefinition gtPadSustainedHarmony(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Whole-bar sustained 4-beat triad pad (even velocity, no syncopation, duty ~1.0)
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        addNote(tr, b, 1880, 60, 72);
        addNote(tr, b, 1880, 64, 70);
        addNote(tr, b, 1880, 67, 71);
        addNote(tr, b, 1880, 72, 69);
    }
    return tr;
}

TrackDefinition gtCountermelody(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Mid-register (57..64, med=60) stepwise lyrical answer line, softer velocity (72..80)
    const uint32_t ts[] = {480, 720, 960, 1200, 1440};
    const uint8_t  ps[] = {60,  62,  64,  62,   60};
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (size_t i = 0; i < 5; ++i) {
            addNote(tr, b + ts[i], 220, ps[i], static_cast<uint8_t>(74 + (i % 3) * 3));
        }
    }
    return tr;
}

TrackDefinition gtBrassAccents(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Short, loud (vel 110..118), punchy horn stabs separated by large rests (> 2 beats)
    const uint32_t hits[] = {720, 1680};
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (uint32_t h : hits) {
            addNote(tr, b + h, 140, 64, 114);
            addNote(tr, b + h, 140, 67, 116);
            addNote(tr, b + h, 140, 72, 118);
        }
    }
    return tr;
}

TrackDefinition gtOrchestralAccents(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Dramatic dynamic swells with CC11 crescendo (35..120), wide velocity dynamic range (48..118),
    // sustained marcato duration (1.1 beats), strong downbeat + wide pitch span (24 st)
    tr.events.push_back({MidiEventType::ControlChange, ch, 11, 35,  0});
    tr.events.push_back({MidiEventType::ControlChange, ch, 11, 80,  480});
    tr.events.push_back({MidiEventType::ControlChange, ch, 11, 122, 960});
    tr.events.push_back({MidiEventType::ControlChange, ch, 11, 45,  1920});
    tr.events.push_back({MidiEventType::ControlChange, ch, 11, 118, 2880});

    addNote(tr, 0,    560, 48, 52);
    addNote(tr, 0,    560, 60, 56);
    addNote(tr, 0,    560, 72, 60);
    addNote(tr, 960,  560, 48, 112);
    addNote(tr, 960,  560, 64, 116);
    addNote(tr, 960,  560, 72, 118);
    addNote(tr, 1920, 560, 48, 54);
    addNote(tr, 1920, 560, 67, 108);
    addNote(tr, 2880, 560, 55, 115);
    addNote(tr, 2880, 560, 72, 118);
    return tr;
}

TrackDefinition gtTransitionalEffects(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Chromatic ascending riser/sweep into next bar with velocity crescendo + CC11 rise
    tr.events.push_back({MidiEventType::ControlChange, ch, 11, 30, 1920});
    tr.events.push_back({MidiEventType::ControlChange, ch, 11, 125, 3600});
    for (uint32_t i = 0; i < 10; ++i) {
        addNote(tr, 2400 + i * 120, 100, static_cast<uint8_t>(60 + i), static_cast<uint8_t>(45 + i * 8));
    }
    return tr;
}

TrackDefinition gtMelodicPhrase(uint8_t ch = 3) {
    auto tr = makeStrippedTrack(ch);
    // Prominent upper-register (72..81, med=76) stepwise melodic lead with expressive pitch bend & non-chord passing tones
    tr.events.push_back({MidiEventType::PitchBend, ch, 0, 76, 480});
    tr.events.push_back({MidiEventType::PitchBend, ch, 0, 64, 720});
    const uint32_t ts[] = {0,   240, 480, 840, 1080, 1440};
    const uint32_t ds[] = {220, 220, 340, 220, 320,  400};
    const uint8_t  ps[] = {72,  74,  76,  77,  79,   76};
    const uint8_t  vs[] = {96,  88,  104, 92,  108,  95};
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (size_t i = 0; i < 6; ++i) {
            addNote(tr, b + ts[i], ds[i], ps[i], vs[i]);
        }
    }
    return tr;
}

} // namespace

int main() {
    std::printf("Test: Phase 5 P5-M2 Musical Semantic Analyzer\n");

    // ── 1. All 14 Canonical Semantic Roles — ZERO Channel/Program/Name Hints! ──
    {
        struct Case {
            const char* name;
            MusicalRole expected;
            TrackDefinition track;
        };
        std::vector<Case> cases = {
            {"1. KickSnareFoundation (stripped)",   MusicalRole::KickSnareFoundation,   gtKickSnareFoundation()},
            {"2. DrumKitAuxPercussion (stripped)",  MusicalRole::DrumKitAuxPercussion,  gtDrumKitAuxPercussion()},
            {"3. BassFoundation (stripped)",        MusicalRole::BassFoundation,        gtBassFoundation()},
            {"4. HarmonicAccompaniment (stripped)", MusicalRole::HarmonicAccompaniment, gtHarmonicAccompaniment()},
            {"5. ChordComping (stripped)",          MusicalRole::ChordComping,          gtChordComping()},
            {"6. GuitarStrumming (stripped)",       MusicalRole::GuitarStrumming,       gtGuitarStrumming()},
            {"7. Arpeggio (stripped)",              MusicalRole::Arpeggio,              gtArpeggio()},
            {"8. PianoAccompaniment (stripped)",    MusicalRole::PianoAccompaniment,    gtPianoAccompaniment()},
            {"9. PadSustainedHarmony (stripped)",   MusicalRole::PadSustainedHarmony,   gtPadSustainedHarmony()},
            {"10. Countermelody (stripped)",        MusicalRole::Countermelody,         gtCountermelody()},
            {"11. BrassAccents (stripped)",         MusicalRole::BrassAccents,          gtBrassAccents()},
            {"12. OrchestralAccents (stripped)",    MusicalRole::OrchestralAccents,     gtOrchestralAccents()},
            {"13. TransitionalEffects (stripped)",  MusicalRole::TransitionalEffects,   gtTransitionalEffects()},
            {"14. MelodicPhrase (stripped)",        MusicalRole::MelodicPhrase,         gtMelodicPhrase()},
        };

        SemanticAnalyzeOptions strict_opt{};
        strict_opt.use_metadata_hints = false; // ZERO metadata hints!

        int exact_hits = 0;
        for (const auto& c : cases) {
            const auto st = wrapSingleTrack(c.name, c.track);
            const auto res = analyzeMusicalSemantics(st, nullptr, nullptr, strict_opt);
            const bool ok = (res.track_roles.size() == 1 &&
                             !res.track_roles[0].abstained &&
                             res.track_roles[0].role == c.expected &&
                             res.track_roles[0].confidence >= strict_opt.role_abstain_threshold);
            if (!ok && !res.track_roles.empty()) {
                std::fprintf(stderr, "    DEBUG %s -> got %s (conf=%.3f, H=%.3f, abs=%s)\n",
                             c.name,
                             musicalRoleName(res.track_roles[0].role),
                             res.track_roles[0].confidence,
                             res.track_roles[0].uncertainty_entropy,
                             res.track_roles[0].abstain_reason.c_str());
            } else if (ok) {
                ++exact_hits;
            }
            TEST(c.name, ok);
        }
        TEST("all 14/14 stripped ground-truth roles inferred purely from musical content", exact_hits == 14);
    }

    // ── 2. Adversarial Misleading Metadata (Wrong Channel + Wrong GM PC + Wrong Track Name!) ──
    {
        // Bassline put on channel 9 (standard drum channel!) with is_drum=false, name="FluteLead", GM PC=73 (Flute)
        auto misleading_bass = gtBassFoundation(9);
        misleading_bass.name = "FluteLead";
        misleading_bass.events.insert(misleading_bass.events.begin(),
                                      {MidiEventType::ProgramChange, 9, 73, 0, 0});
        const auto res_b = analyzeMusicalSemantics(wrapSingleTrack("AdversarialBass", misleading_bass));
        TEST("adversarial Bass on ch9 named 'FluteLead' with PC=73 still infers BassFoundation",
             res_b.track_roles.size() == 1 &&
             !res_b.track_roles[0].abstained &&
             res_b.track_roles[0].role == MusicalRole::BassFoundation);

        // Strummed guitar on channel 14 named "WarmPad" with GM PC=89 (Warm Pad)
        auto misleading_gtr = gtGuitarStrumming(14);
        misleading_gtr.name = "WarmPad";
        misleading_gtr.events.insert(misleading_gtr.events.begin(),
                                     {MidiEventType::ProgramChange, 14, 89, 0, 0});
        const auto res_g = analyzeMusicalSemantics(wrapSingleTrack("AdversarialGtr", misleading_gtr));
        TEST("adversarial GuitarStrumming named 'WarmPad' with PC=89 still infers GuitarStrumming",
             res_g.track_roles.size() == 1 &&
             !res_g.track_roles[0].abstained &&
             res_g.track_roles[0].role == MusicalRole::GuitarStrumming);

        // Sustained pad named "AcousticBass" with GM PC=32 (Acoustic Bass)
        auto misleading_pad = gtPadSustainedHarmony(10);
        misleading_pad.name = "AcousticBass";
        misleading_pad.events.insert(misleading_pad.events.begin(),
                                     {MidiEventType::ProgramChange, 10, 32, 0, 0});
        const auto res_p = analyzeMusicalSemantics(wrapSingleTrack("AdversarialPad", misleading_pad));
        TEST("adversarial PadSustainedHarmony on ch10 named 'AcousticBass' with PC=32 still infers PadSustainedHarmony",
             res_p.track_roles.size() == 1 &&
             !res_p.track_roles[0].abstained &&
             res_p.track_roles[0].role == MusicalRole::PadSustainedHarmony);
    }

    // ── 3. Honest Fail-Closed UnknownAbstain on Empty, Sparse, Ambiguous, and Random-Noise Tracks ──
    {
        // 3a. Empty track
        auto empty_tr = makeStrippedTrack(5);
        const auto r_empty = analyzeMusicalSemantics(wrapSingleTrack("Empty", empty_tr));
        TEST("empty track abstains cleanly as UnknownAbstain",
             r_empty.track_roles.size() == 1 &&
             r_empty.track_roles[0].abstained &&
             r_empty.track_roles[0].role == MusicalRole::UnknownAbstain &&
             r_empty.track_roles[0].abstain_reason == "empty_track_zero_notes" &&
             r_empty.abstained_track_count == 1);

        // 3b. Sparse 2-note mid-register blip
        auto sparse_tr = makeStrippedTrack(5);
        addNote(sparse_tr, 480, 200, 62, 75);
        addNote(sparse_tr, 1440, 200, 65, 72);
        const auto r_sparse = analyzeMusicalSemantics(wrapSingleTrack("Sparse", sparse_tr));
        TEST("sparse 2-note mid-register track abstains honestly",
             r_sparse.track_roles.size() == 1 &&
             r_sparse.track_roles[0].abstained &&
             r_sparse.track_roles[0].role == MusicalRole::UnknownAbstain);

        // 3c. Ambiguous single-pitch mid-register generic fragment
        auto single_p = makeStrippedTrack(5);
        single_p.name = "Chord1";
        addNote(single_p, 0, 300, 60, 80);
        addNote(single_p, 960, 300, 60, 80);
        addNote(single_p, 1920, 300, 60, 80);
        const auto r_sp = analyzeMusicalSemantics(wrapSingleTrack("SinglePitch", single_p));
        TEST("ambiguous single-pitch generic 'Chord1' abstains honestly",
             r_sp.track_roles.size() == 1 &&
             r_sp.track_roles[0].abstained &&
             r_sp.track_roles[0].role == MusicalRole::UnknownAbstain);

        // 3d. Chaotic random non-tonal noise across huge pitch span
        auto noise_tr = makeStrippedTrack(5);
        const uint8_t rnd_p[] = {29, 97, 41, 103, 35, 89, 53, 101, 31, 95, 47, 83};
        const uint32_t rnd_t[] = {70, 310, 520, 890, 1130, 1470, 1890, 2110, 2570, 2830, 3290, 3510};
        for (size_t i = 0; i < 12; ++i) {
            addNote(noise_tr, rnd_t[i], 260, rnd_p[i], static_cast<uint8_t>(60 + (i * 5) % 40));
        }
        const auto r_noise = analyzeMusicalSemantics(wrapSingleTrack("RandomNoise", noise_tr));
        TEST("unstructured random noise track abstains honestly with high entropy",
             r_noise.track_roles.size() == 1 &&
             r_noise.track_roles[0].abstained &&
             r_noise.track_roles[0].role == MusicalRole::UnknownAbstain &&
             r_noise.track_roles[0].uncertainty_entropy >= 0.70);
    }

    // ── 4. Mixed-Role Tracks (Cross-Section Shift V1->V4 AND Intra-Section Stride LH Bass + RH Comp) ──
    {
        // 4a. Cross-section shift: ChordComping in Main1 -> Arpeggio in Main4 on same channel 11
        StyleDefinition s{};
        s.name = "MixedRoleCrossSection";
        s.tempo_bpm = 115;
        s.resolution = 480;

        SectionDefinition v1{};
        v1.type = SectionType::Main1; v1.name = "Main A"; v1.bars = 2; v1.resolution = 480;
        v1.beats_per_bar = 4; v1.beat_note = 4;
        v1.tracks.push_back(gtChordComping(11));

        SectionDefinition v4{};
        v4.type = SectionType::Main4; v4.name = "Main D"; v4.bars = 2; v4.resolution = 480;
        v4.beats_per_bar = 4; v4.beat_note = 4;
        v4.tracks.push_back(gtArpeggio(11));

        s.sections.push_back(v1);
        s.sections.push_back(v4);

        const auto res_mix = analyzeMusicalSemantics(s);
        TEST("cross-section shift (V1 ChordComping -> V4 Arpeggio) flags MixedRole & preserves both roles",
             res_mix.track_roles.size() == 1 &&
             !res_mix.track_roles[0].abstained &&
             res_mix.track_roles[0].is_mixed_role &&
             res_mix.track_roles[0].section_role_shift_detected &&
             res_mix.track_roles[0].effectiveRoleOrMixed() == MusicalRole::MixedRole &&
             res_mix.mixed_role_track_count == 1 &&
             ((res_mix.track_roles[0].primary_role == MusicalRole::ChordComping &&
               res_mix.track_roles[0].secondary_role == MusicalRole::Arpeggio) ||
              (res_mix.track_roles[0].primary_role == MusicalRole::Arpeggio &&
               res_mix.track_roles[0].secondary_role == MusicalRole::ChordComping)));

        // 4b. Intra-section stride piano (alternating LH solo bass notes + RH chord comping + two-hand hits)
        auto stride = makeStrippedTrack(12);
        stride.events.push_back({MidiEventType::ControlChange, 12, 64, 127, 0});
        stride.events.push_back({MidiEventType::ControlChange, 12, 64, 0, 1880});
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            // Beat 1: solo LH bass C2 (36)
            addNote(stride, b + 0, 420, 36, 98);
            // Beat 2: RH chord + low note (two-hand split)
            addNote(stride, b + 480, 380, 43, 86);
            addNote(stride, b + 480, 380, 60, 88);
            addNote(stride, b + 480, 380, 64, 86);
            addNote(stride, b + 480, 380, 67, 88);
            // Beat 3: solo LH bass G2 (43)
            addNote(stride, b + 960, 420, 43, 96);
            // Beat 4: RH chord + low note (two-hand split)
            addNote(stride, b + 1440, 380, 40, 84);
            addNote(stride, b + 1440, 380, 60, 86);
            addNote(stride, b + 1440, 380, 64, 85);
            addNote(stride, b + 1440, 380, 67, 87);
        }
        const auto res_stride = analyzeMusicalSemantics(wrapSingleTrack("StridePiano", stride));
        TEST("intra-section stride piano (LH bass + RH chords) detects MixedRole with BassFoundation secondary",
             res_stride.track_roles.size() == 1 &&
             !res_stride.track_roles[0].abstained &&
             res_stride.track_roles[0].is_mixed_role &&
             res_stride.track_roles[0].secondary_role == MusicalRole::BassFoundation);
    }

    // ── 5. Inter-Track Kick-Bass Lock, Bass-Piano Low Collision, Energy Arc & Swing ──
    {
        StyleDefinition multi{};
        multi.name = "MultiTrack_Swing_Collision_Arc";
        multi.tempo_bpm = 128;
        multi.resolution = 480;

        // V1 (sparse) -> V2 (richer) with swing ~0.62 and low piano intrusion < C3 (48)
        SectionDefinition v1{};
        v1.type = SectionType::Main1; v1.name = "Main A"; v1.bars = 2; v1.resolution = 480;
        v1.beats_per_bar = 4; v1.beat_note = 4;

        // Swung drum kit (offbeat HH at 0.625 of beat = 300 ticks)
        TrackDefinition dr{};
        dr.midi_channel = 9; dr.name = "JazzKit"; dr.role = TrackRole::Drum; dr.is_drum = true;
        for (uint32_t bar = 0; bar < 2; ++bar) {
            const uint32_t b = bar * 1920;
            addNote(dr, b + 0,    100, 36, 98);
            addNote(dr, b + 480,  100, 38, 92);
            addNote(dr, b + 960,  100, 36, 96);
            addNote(dr, b + 1440, 100, 38, 94);
            for (uint32_t beat = 0; beat < 4; ++beat) {
                addNote(dr, b + beat * 480,       80, 51, 86);
                addNote(dr, b + beat * 480 + 300, 70, 51, 72); // 62.5% swing!
            }
        }
        TrackDefinition bs = gtBassFoundation(10);
        bs.role = TrackRole::Bass;
        // Piano intruding down to MIDI 36 & 40 concurrently with bass!
        TrackDefinition pn = gtPianoAccompaniment(11);
        for (uint32_t bar = 0; bar < 2; ++bar) {
            addNote(pn, bar * 1920 + 0,   420, 36, 88); // Low C2 collision with bass!
            addNote(pn, bar * 1920 + 960, 420, 43, 86); // Low G2 collision with bass!
        }

        v1.tracks.push_back(dr);
        v1.tracks.push_back(bs);
        v1.tracks.push_back(pn);

        SectionDefinition v2 = v1;
        v2.type = SectionType::Main2; v2.name = "Main B";
        v2.tracks.push_back(gtBrassAccents(13));
        v2.tracks.push_back(gtMelodicPhrase(14));

        multi.sections.push_back(v1);
        multi.sections.push_back(v2);

        const auto res1 = analyzeMusicalSemantics(multi);
        const auto res2 = analyzeMusicalSemantics(multi);

        TEST("swing ratio accurately estimated near 0.625 (+/- 0.03)",
             std::abs(res1.groove.estimated_swing_ratio - 0.625) <= 0.03);
        TEST("kick-bass onset lock correlation detected (> 0.60)",
             res1.inter_track.kick_bass_lock_correlation >= 0.60 &&
             res1.groove.kick_bass_lock_ratio >= 0.60);
        TEST("Bass/Piano low-register collision (< C3) flagged accurately",
             res1.inter_track.bass_piano_low_collision &&
             res1.inter_track.bass_piano_low_collision_notes >= 4);
        TEST("variation energy arc is monotonic (V2 > V1)",
             res1.variation_arc_monotonic &&
             res1.energy_arc.size() == 2 &&
             res1.energy_arc[1].energy_score > res1.energy_arc[0].energy_score);
        TEST("JazzSwing genre feel inferred from swung quartet",
             !res1.genre_abstained && res1.inferred_genre == GenreFeelClass::JazzSwing);

        // Determinism check (2x identical JSON & Markdown)
        const std::string j1 = formatSemanticAnalysisJson(res1);
        const std::string j2 = formatSemanticAnalysisJson(res2);
        const std::string m1 = formatSemanticAnalysisMd(res1);
        const std::string m2 = formatSemanticAnalysisMd(res2);
        TEST("2x semantic analysis JSON & Markdown outputs are byte-identical",
             j1 == j2 && m1 == m2 &&
             j1.find("\"hardwareVerified\": false") != std::string::npos &&
             j1.find("\"l3Status\": \"NOT_TESTED\"") != std::string::npos);
    }

    // ── 6. Compound 6/8 & Mixed Odd Meter Detection ──
    {
        StyleDefinition odd{};
        odd.name = "Compound_And_Odd_Meter";
        odd.tempo_bpm = 84;
        odd.resolution = 480;

        SectionDefinition s68{};
        s68.type = SectionType::Main1; s68.name = "Main A 6/8";
        s68.bars = 2; s68.resolution = 480; s68.beats_per_bar = 6; s68.beat_note = 8;
        s68.tracks.push_back(gtBassFoundation(10));

        SectionDefinition s54{};
        s54.type = SectionType::Main2; s54.name = "Main B 5/4";
        s54.bars = 2; s54.resolution = 480; s54.beats_per_bar = 5; s54.beat_note = 4;
        s54.tracks.push_back(gtBassFoundation(10));

        odd.sections.push_back(s68);
        odd.sections.push_back(s54);

        const auto r_odd = analyzeMusicalSemantics(odd);
        TEST("detects both compound 6/8 and mixed/odd 5/4 meter cleanly",
             r_odd.groove.compound_meter_detected && r_odd.groove.mixed_meter_detected);
    }

    // ── 7. Round-2 Audit Regressions (P1-1, P1-3, P1-4, P2-4) ──
    {
        // P1-3: Abstained sparse & single-pitch tracks MUST report primary_role=UnknownAbstain & entropy > 0.5 (1.0)!
        auto sparse2 = makeStrippedTrack(7);
        addNote(sparse2, 0, 200, 62, 80);
        addNote(sparse2, 960, 200, 65, 80);
        const auto r_p13 = analyzeMusicalSemantics(wrapSingleTrack("P1_3_Sparse", sparse2));
        TEST("P1-3: sparse abstained track sets primary_role=UnknownAbstain and entropy==1.0 (never 0.0000)",
             r_p13.track_roles.size() == 1 &&
             r_p13.track_roles[0].abstained &&
             r_p13.track_roles[0].role == MusicalRole::UnknownAbstain &&
             r_p13.track_roles[0].primary_role == MusicalRole::UnknownAbstain &&
             r_p13.track_roles[0].uncertainty_entropy >= 0.99);

        // P1-4: Genuine C-major walking bassline (is_drum=false) with drum bank CC0=62, CC32=100
        // MUST fail closed (metadata_content_conflict), NEVER fabricate 98.7% KickSnareFoundation!
        auto conflict_bass = gtBassFoundation(10);
        conflict_bass.is_drum = false;
        conflict_bass.events.insert(conflict_bass.events.begin(),
                                    {MidiEventType::ControlChange, 10, 32, 100, 0});
        conflict_bass.events.insert(conflict_bass.events.begin(),
                                    {MidiEventType::ControlChange, 10, 0, 62, 0});
        const auto r_p14_default = analyzeMusicalSemantics(wrapSingleTrack("P1_4_BankConflict", conflict_bass));
        SemanticAnalyzeOptions no_hints_opt{};
        no_hints_opt.use_metadata_hints = false;
        const auto r_p14_nohints = analyzeMusicalSemantics(
            wrapSingleTrack("P1_4_BankConflict", conflict_bass), nullptr, nullptr, no_hints_opt);
        TEST("P1-4: drum-bank metadata on walking bassline abstains (metadata_content_conflict), never fabricates Drum",
             r_p14_default.track_roles.size() == 1 &&
             r_p14_default.track_roles[0].abstained &&
             r_p14_default.track_roles[0].role == MusicalRole::UnknownAbstain &&
             r_p14_default.track_roles[0].primary_role == MusicalRole::UnknownAbstain &&
             r_p14_default.track_roles[0].abstain_reason == "metadata_content_conflict" &&
             r_p14_default.track_roles[0].uncertainty_entropy >= 0.99 &&
             r_p14_nohints.track_roles.size() == 1 &&
             !r_p14_nohints.track_roles[0].abstained &&
             r_p14_nohints.track_roles[0].role == MusicalRole::BassFoundation);

        // P2-4: Six simultaneous 0-duration notes (60..65) at tick 0 MUST abstain, NOT 97.4% DrumKitAuxPercussion!
        auto deg_cluster = makeStrippedTrack(6);
        for (uint8_t p = 60; p <= 65; ++p) {
            deg_cluster.events.push_back({MidiEventType::NoteOn, 6, p, 90, 0});
            deg_cluster.events.push_back({MidiEventType::NoteOff, 6, p, 0, 0});
        }
        StyleDefinition s_deg{};
        s_deg.name = "P2_4_ZeroDurCluster";
        SectionDefinition sec_deg{};
        sec_deg.type = SectionType::Main1; sec_deg.name = "Main A";
        sec_deg.bars = 0; sec_deg.resolution = 0; sec_deg.beats_per_bar = 4; sec_deg.beat_note = 4;
        sec_deg.tracks.push_back(deg_cluster);
        s_deg.sections.push_back(sec_deg);
        const auto r_p24 = analyzeMusicalSemantics(s_deg);
        TEST("P2-4: degenerate single-onset 0-duration chromatic cluster abstains honestly",
             r_p24.track_roles.size() == 1 &&
             r_p24.track_roles[0].abstained &&
             r_p24.track_roles[0].role == MusicalRole::UnknownAbstain &&
             r_p24.track_roles[0].primary_role == MusicalRole::UnknownAbstain &&
             r_p24.track_roles[0].uncertainty_entropy >= 0.99);

        // P1-1 end-to-end inside analyzeMusicalSemantics(): silent-in-Intro1 high flute + drums -> kbLock == 0.0!
        StyleDefinition s_no_bass{};
        s_no_bass.name = "P1_1_SemanticNoBass";
        s_no_bass.tempo_bpm = 120;
        s_no_bass.resolution = 480;
        SectionDefinition s0{};
        s0.type = SectionType::Intro1; s0.name = "Intro A"; s0.bars = 2; s0.resolution = 480;
        s0.beats_per_bar = 4; s0.beat_note = 4;
        auto dr0 = gtKickSnareFoundation(9); dr0.is_drum = true; dr0.role = TrackRole::Drum;
        auto fl0 = makeStrippedTrack(14); // silent in Intro1
        s0.tracks.push_back(dr0);
        s0.tracks.push_back(fl0);
        SectionDefinition s1 = s0;
        s1.type = SectionType::Main1; s1.name = "Main A";
        s1.tracks[1] = gtMelodicPhrase(14); // active in Main1
        s_no_bass.sections.push_back(s0);
        s_no_bass.sections.push_back(s1);
        const auto r_p11 = analyzeMusicalSemantics(s_no_bass);
        TEST("P1-1: analyzeMusicalSemantics() gives kick_bass_lock == 0.0 when no Bass exists (silent-intro flute)",
             r_p11.inter_track.kick_bass_lock_correlation == 0.0 &&
             r_p11.groove.kick_bass_lock_ratio == 0.0);
    }

    std::printf("Summary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
