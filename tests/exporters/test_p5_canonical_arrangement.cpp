// Phase 5 P5-M1 unit & regression tests for vendor-neutral TargetCapabilityProfile,
// CapabilityGapReport, and non-duplicating CanonicalStyleView (UASF + KorgStyle).
// 100% synthetic fixtures, portable C++20, zero private data or external dependencies.

#include "intelligence/target_capability.h"
#include "intelligence/canonical_arrangement.h"
#include "exporters/korg/genos_to_korg.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ai_arranger::intelligence;
using ai_arranger::arranger::ChordType;
using ai_arranger::exporters::korg::BreakPolicy;
using ai_arranger::exporters::korg::MapOptions;
using ai_arranger::exporters::korg::mapToKorg;
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

void addNote(TrackDefinition& tr, uint32_t on, uint32_t dur, uint8_t pitch, uint8_t vel = 90) {
    tr.events.push_back({MidiEventType::NoteOn, tr.midi_channel, pitch, vel, on});
    tr.events.push_back({MidiEventType::NoteOff, tr.midi_channel, pitch, 0, on + dur});
}

StyleDefinition buildSyntheticStyle() {
    StyleDefinition s{};
    s.name = "P5_Synthetic_Pop_Canonical";
    s.format_version = "1.2";
    s.tempo_bpm = 116;
    s.resolution = 480;

    // Main1 (2 bars, 4/4)
    SectionDefinition m1{};
    m1.type = SectionType::Main1;
    m1.name = "Main A";
    m1.bars = 2;
    m1.resolution = 480;
    m1.beats_per_bar = 4;
    m1.beat_note = 4;

    TrackDefinition drum{};
    drum.name = "Rhythm2";
    drum.midi_channel = 9;
    drum.role = TrackRole::Drum;
    drum.is_drum = true;
    drum.events.push_back({MidiEventType::ControlChange, 9, 0, 127, 0});
    drum.events.push_back({MidiEventType::ProgramChange, 9, 0, 0, 0});
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        addNote(drum, b + 0,    120, 36, 110);
        addNote(drum, b + 480,  120, 38, 102);
        addNote(drum, b + 960,  120, 36, 108);
        addNote(drum, b + 1440, 120, 38, 104);
        for (uint32_t e = 0; e < 8; ++e) {
            addNote(drum, b + e * 240, 80, 42, (e % 2 == 0) ? 85 : 68);
        }
    }

    TrackDefinition bass{};
    bass.name = "Bass";
    bass.midi_channel = 10;
    bass.role = TrackRole::Bass;
    bass.articulation.has_casm_meta = true;
    bass.articulation.dest_channel = 10;
    bass.articulation.source_chord_root = 0; // C
    bass.articulation.source_chord_type = 0; // Maj
    bass.events.push_back({MidiEventType::ControlChange, 10, 7, 108, 0});
    bass.events.push_back({MidiEventType::ControlChange, 10, 10, 64, 0});
    bass.events.push_back({MidiEventType::ProgramChange, 10, 33, 0, 0});
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        addNote(bass, b + 0,    420, 36, 102);
        addNote(bass, b + 960,  420, 43, 96);
        addNote(bass, b + 1440, 400, 40, 92);
    }

    TrackDefinition piano{};
    piano.name = "Chord1";
    piano.midi_channel = 11;
    piano.role = TrackRole::Chord;
    piano.events.push_back({MidiEventType::ProgramChange, 11, 0, 0, 0});
    piano.events.push_back({MidiEventType::ControlChange, 11, 64, 127, 0});
    piano.events.push_back({MidiEventType::ControlChange, 11, 64, 0, 1800});
    for (uint32_t bar = 0; bar < 2; ++bar) {
        const uint32_t b = bar * 1920;
        for (uint32_t beat = 0; beat < 4; ++beat) {
            const uint32_t t = b + beat * 480;
            addNote(piano, t, 420, 48, 82);
            addNote(piano, t, 420, 60, 86);
            addNote(piano, t, 420, 64, 84);
            addNote(piano, t, 420, 67, 85);
        }
    }

    m1.tracks.push_back(drum);
    m1.tracks.push_back(bass);
    m1.tracks.push_back(piano);
    s.sections.push_back(m1);

    // Fill1 (1 bar)
    SectionDefinition f1 = m1;
    f1.type = SectionType::Fill1;
    f1.name = "Fill In AA";
    f1.bars = 1;
    f1.tracks.clear();
    TrackDefinition fd{};
    fd.name = "Rhythm2";
    fd.midi_channel = 9;
    fd.role = TrackRole::Drum;
    fd.is_drum = true;
    for (uint32_t i = 0; i < 8; ++i) {
        addNote(fd, i * 240, 100, static_cast<uint8_t>(38 + (i % 4) * 2), static_cast<uint8_t>(85 + i * 4));
    }
    f1.tracks.push_back(fd);
    s.sections.push_back(f1);

    return s;
}

} // namespace

int main() {
    std::printf("Test: Phase 5 P5-M1 Target Capability & Canonical Arrangement\n");

    // ── 1. All 5 Arranger Platforms & Epistemic/HOLD Invariants ──
    {
        const auto platforms = supportedTargetPlatforms();
        TEST("exactly 5 supported arranger platforms", platforms.size() == 5);
        TEST("platform enum aliases resolve cleanly",
             ArrangerPlatform::KorgPaSeries == TargetPlatformId::KorgPa4XPa1000 &&
             ArrangerPlatform::RolandArranger == TargetPlatformId::RolandEA7BK);

        bool all_epistemic_locked = true;
        bool all_keys_roundtrip = true;
        for (auto pid : platforms) {
            const auto& p = targetCapabilityProfile(pid);
            if (p.hardware_verified || p.l3_status != "NOT_TESTED" ||
                p.articulation.native_binary_write_authorized ||
                p.articulation.native_binary_export_supported) {
                all_epistemic_locked = false;
            }
            const auto parsed = parseTargetPlatformId(p.key);
            if (!parsed.has_value() || *parsed != pid) {
                all_keys_roundtrip = false;
            }
        }
        TEST("all 5 platforms lock hardware_verified=false, l3=NOT_TESTED, native_binary=false",
             all_epistemic_locked);
        TEST("all 5 platform keys round-trip through parseTargetPlatformId", all_keys_roundtrip);
        TEST("parseTargetPlatformId handles aliases and rejects unknown tokens",
             parseTargetPlatformId("pa5x") == TargetPlatformId::KorgPa5X &&
             parseTargetPlatformId("genos2") == TargetPlatformId::YamahaGenos2 &&
             parseTargetPlatformId("korg_pa_series") == TargetPlatformId::KorgPa4XPa1000 &&
             parseTargetPlatformId("ketron") == TargetPlatformId::KetronEvent &&
             parseTargetPlatformId("roland_arranger") == TargetPlatformId::RolandEA7BK &&
             !parseTargetPlatformId("nonexistent_synth").has_value());
    }

    // ── 2. Platform-Specific Capability Queries ──
    {
        const auto& pa5x   = targetCapabilityProfile(TargetPlatformId::KorgPa5X);
        const auto& genos2 = targetCapabilityProfile(TargetPlatformId::YamahaGenos2);
        const auto& pa4x   = targetCapabilityProfile(TargetPlatformId::KorgPaSeries);
        const auto& ketron = targetCapabilityProfile(TargetPlatformId::KetronEvent);
        const auto& roland = targetCapabilityProfile(TargetPlatformId::RolandArranger);

        TEST("KorgPa5X exact capability specs (8 tracks, 6 CVs, 1-bar break, 384 PPQN, DNC, USERDK1..6)",
             pa5x.tracks.total_style_tracks == 8 &&
             pa5x.maxCvFor(SectionType::Main1) == 6 &&
             pa5x.maxCvFor(SectionType::Intro1) == 2 &&
             pa5x.maxCvFor(SectionType::Break) == 0 &&
             pa5x.sections.break_max_bars == 1 &&
             pa5x.preferred_ppqn == 384 &&
             pa5x.supportsArticulation(ArticulationFlag::DNC) &&
             pa5x.supportsArticulation(ArticulationFlag::RxNoise) &&
             !pa5x.supportsArticulation(ArticulationFlag::MegaVoice) &&
             pa5x.sound_addressing.drum_user_bank_max == 6 &&
             pa5x.tracks.chord_event_note_max == 11);

        TEST("KorgPa5X channel queries match Bass/Acc chord-driven & Drum/Perc non-chord-driven",
             pa5x.isStyleChannel(8) && pa5x.isChordDrivenChannel(8) &&
             pa5x.isStyleChannel(9) && !pa5x.isChordDrivenChannel(9) &&
             pa5x.isStyleChannel(10) && !pa5x.isChordDrivenChannel(10) &&
             pa5x.isStyleChannel(11) && pa5x.isChordDrivenChannel(11) &&
             pa5x.isStyleChannel(15) && pa5x.isChordDrivenChannel(15) &&
             !pa5x.isStyleChannel(0));

        TEST("YamahaGenos2 specs (8..16 tracks, 480 PPQN, MegaVoice/S.Art2, CASM mute)",
             genos2.tracks.total_style_tracks == 8 &&
             genos2.tracks.max_style_tracks == 16 &&
             genos2.preferred_ppqn == 480 &&
             genos2.chords.supports_casm_mute &&
             genos2.supportsArticulation(ArticulationFlag::MegaVoice) &&
             genos2.supportsArticulation(ArticulationFlag::SuperArticulation));

        TEST("KorgPaSeries legacy limits (128 polyphony, USER01..04, USERDK01..02)",
             pa4x.tracks.max_polyphony == 128 &&
             pa4x.sound_addressing.melodic_user_bank_max == 4 &&
             pa4x.sound_addressing.drum_user_bank_max == 2);

        TEST("KetronEvent Real Audio Drum + 12 tracks + 2-bar break capability",
             ketron.tracks.total_style_tracks == 12 &&
             ketron.articulation.supports_real_audio_drums &&
             ketron.supportsArticulation(ArticulationFlag::RealAudioDrum) &&
             ketron.sections.break_max_bars == 2);

        TEST("RolandArranger chord flags & polyphony",
             roland.tracks.max_polyphony == 128 &&
             roland.supportsChordType(ChordType::Major) &&
             roland.supportsChordType(ChordType::Min7) &&
             roland.supportsChordFlag(ChordCapabilityFlag::Slash) &&
             !roland.supportsChordType(ChordType::Aug) &&
             !roland.supportsChordType(ChordType::Dim7));

        const auto bass_rng = pitchRangeForMusicalRole(pa5x, MusicalRole::BassFoundation);
        TEST("Pa5X bass pitch range stays above chord-event 0..11 band",
             bass_rng.min_pitch >= 28 && bass_rng.contains(36) && !bass_rng.contains(11));
    }

    // ── 3. CapabilityGapReport Comparison Matrix ──
    {
        const auto g2_to_pa5x = compareTargetCapabilities(TargetPlatformId::YamahaGenos2,
                                                          TargetPlatformId::KorgPa5X);
        TEST("Genos2 -> Pa5X detects track compression, CASM->CV, MegaVoice/S.Art2 gap, PPQN rescale",
             g2_to_pa5x.max_severity == CapabilityGapSeverity::LossyAdaptationRequired &&
             g2_to_pa5x.track_count_delta == -8 &&
             g2_to_pa5x.ppqn_resample_recommended &&
             g2_to_pa5x.missing_articulations.size() >= 2 &&
             !g2_to_pa5x.items.empty() &&
             !g2_to_pa5x.hardware_verified);

        const auto pa5x_to_roland = compareTargetCapabilities(TargetPlatformId::KorgPa5X,
                                                              TargetPlatformId::RolandEA7BK);
        TEST("Pa5X -> Roland detects CV reduction, missing DNC, missing Aug/Dim7 chord flags",
             pa5x_to_roland.cv_count_delta < 0 &&
             !pa5x_to_roland.missing_chord_capabilities.empty() &&
             !pa5x_to_roland.missing_articulations.empty());

        const auto ketron_to_pa5x = compareTargetCapabilities(TargetPlatformId::KetronEvent,
                                                              TargetPlatformId::KorgPa5X);
        TEST("Ketron -> Pa5X detects 2-bar->1-bar Break cap & RealAudioDrum loss",
             ketron_to_pa5x.break_bar_delta == -1 &&
             ketron_to_pa5x.max_severity == CapabilityGapSeverity::LossyAdaptationRequired);

        const auto same = compareTargetCapabilities(TargetPlatformId::KorgPa5X,
                                                    TargetPlatformId::KorgPa5X);
        TEST("Pa5X -> Pa5X self-comparison has zero gaps",
             same.max_severity == CapabilityGapSeverity::None && same.items.empty());

        const std::string gap_json_1 = formatCapabilityGapReportJson(g2_to_pa5x);
        const std::string gap_json_2 = formatCapabilityGapReportJson(g2_to_pa5x);
        TEST("CapabilityGapReport JSON is deterministic and embeds schema + hardwareVerified=false",
             gap_json_1 == gap_json_2 &&
             gap_json_1.find("aiarranger-capability-gap/1") != std::string::npos &&
             gap_json_1.find("\"hardwareVerified\": false") != std::string::npos);
    }

    // ── 4. Zero-Duplication CanonicalStyleView Bridge (UASF + KorgStyle) ──
    {
        const StyleDefinition uasf_style = buildSyntheticStyle();
        auto mopt = MapOptions::quality();
        mopt.break_policy = BreakPolicy::Yamaha1BarCap;
        const auto mapped = mapToKorg(uasf_style, mopt);
        TEST("synthetic style maps cleanly to KorgStyle", mapped.ok);

        const auto view1 = buildCanonicalStyleView(uasf_style, &mapped.style, &mapped.report,
                                                   TargetPlatformId::KorgPa5X);
        const auto view2 = buildCanonicalStyleView(uasf_style, &mapped.style, &mapped.report,
                                                   TargetPlatformId::KorgPa5X);

        TEST("CanonicalStyleView holds non-owning zero-duplication pointers to UASF & KorgStyle",
             view1.source_uasf == &uasf_style &&
             view1.target_korg == &mapped.style &&
             view1.target_report == &mapped.report &&
             view1.target_capability == &targetCapabilityProfile(TargetPlatformId::KorgPa5X));

        TEST("CanonicalStyleView preserves structure, transitions, and provenance locks",
             view1.sections.size() == 2 &&
             view1.transitions.variation_count == 1 &&
             view1.transitions.fill_count == 1 &&
             view1.provenance.source_format == "UASF/1.2" &&
             !view1.hardware_verified &&
             view1.l3_status == "NOT_TESTED");

        // Verify extracted musical features on Main1 tracks
        const auto& m1 = view1.sections[0];
        TEST("Main1 bass features capture low register, chord-tone grounding, CC7/CC10",
             m1.tracks.size() == 3 &&
             m1.tracks[1].features.register_band == RegisterBand::Bass &&
             m1.tracks[1].features.chord_tone_ratio >= 0.95 &&
             m1.tracks[1].features.cc7_volume == 108 &&
             m1.tracks[1].features.cc10_pan == 64);

        TEST("Main1 piano features capture CC64 sustain, polyphony >= 4, two-hand split",
             m1.tracks[2].features.has_sustain_pedal &&
             m1.tracks[2].features.max_polyphony >= 4 &&
             m1.tracks[2].features.chordal_ratio >= 0.90 &&
             m1.tracks[2].features.has_two_hand_split);

        TEST("Inter-track kick-bass lock is detected (> 0.65)",
             m1.inter_track.kick_bass_lock_correlation >= 0.65 &&
             view1.inter_track_overall.kick_bass_lock_correlation >= 0.65);

        // Deterministic 2x canonical JSON & summary parity
        const std::string json1 = formatCanonicalStyleJson(view1);
        const std::string json2 = formatCanonicalStyleJson(view2);
        const std::string md1   = formatCanonicalStyleSummaryMd(view1);
        const std::string md2   = formatCanonicalStyleSummaryMd(view2);
        TEST("2x canonical JSON & Markdown outputs are byte-identical",
             json1 == json2 && md1 == md2 &&
             json1.find("\"zeroCopyUasfBound\": true") != std::string::npos &&
             json1.find("\"zeroCopyKorgBound\": true") != std::string::npos);

        // Also test building CanonicalStyleView directly from KorgStyle SMF0 alone
        const auto kview = buildCanonicalStyleViewFromKorg(mapped.style, "FromKorgSMF0", &mapped.report);
        TEST("buildCanonicalStyleViewFromKorg builds non-owning overlay over KorgStyle",
             kview.source_uasf == nullptr &&
             kview.target_korg == &mapped.style &&
             kview.provenance.source_format == "KorgSMF0" &&
             !kview.sections.empty() &&
             !kview.hardware_verified);
    }

    std::printf("Summary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
