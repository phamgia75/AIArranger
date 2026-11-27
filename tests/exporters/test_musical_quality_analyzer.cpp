// Unit & edge-case tests for Phase 2 Mission 4 standalone read-only
// 12-dimension track-by-track musical quality analyzer:
//   src/exporters/korg/musical_quality_analyzer.h
//
// Strictly self-contained (synthetic builders + repo-local CC0 fixtures/synthetic).
// Zero private data, zero external paths.

#include "exporters/korg/musical_quality_analyzer.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/korg_style_smf.h"
#include "exporters/korg/sff1_route.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;
using namespace ai_arranger::importers::sff1;

namespace {

int failures = 0;
int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

std::string synthFixture(const char* name) {
    return std::string(REPO_DIR) + "/fixtures/synthetic/" + name;
}

void addVoice(Element& el, uint8_t ch, uint8_t msb, uint8_t lsb, uint8_t pc) {
    el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | ch), 0u, msb});
    el.events.push_back(Event{0, static_cast<uint8_t>(0xB0u | ch), 32u, lsb});
    el.events.push_back(Event{0, static_cast<uint8_t>(0xC0u | ch), pc, 0u});
}

void addCc(Element& el, uint32_t tick, uint8_t ch, uint8_t cc, uint8_t val) {
    el.events.push_back(Event{tick, static_cast<uint8_t>(0xB0u | ch), cc, val});
}

void addPb(Element& el, uint32_t tick, uint8_t ch, uint16_t val14) {
    el.events.push_back(Event{tick, static_cast<uint8_t>(0xE0u | ch),
                              static_cast<uint8_t>(val14 & 0x7Fu),
                              static_cast<uint8_t>((val14 >> 7) & 0x7Fu)});
}

void addNote(Element& el, uint8_t ch, uint32_t on, uint32_t off, uint8_t pitch, uint8_t vel) {
    el.events.push_back(Event{on, static_cast<uint8_t>(0x90u | ch), pitch, vel});
    el.events.push_back(Event{off, static_cast<uint8_t>(0x80u | ch), pitch, 0u});
}

void sortEl(Element& el) {
    std::stable_sort(el.events.begin(), el.events.end(), [](const Event& a, const Event& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        auto prio = [](const Event& e) {
            const uint8_t hi = e.status & 0xF0u;
            if (e.tick == 0u) {
                if (hi == 0xB0u && e.d1 == 0u) return 0;
                if (hi == 0xB0u && e.d1 == 32u) return 1;
                if (hi == 0xC0u) return 2;
                if (hi == 0xB0u || hi == 0xE0u) return 3;
            }
            if (hi == 0x80u || (hi == 0x90u && e.d2 == 0u)) return 4;
            return 5;
        };
        return prio(a) < prio(b);
    });
}

bool hasFindingCode(const MusicalQualityReport& r, const std::string& code) {
    for (const auto& f : r.all_findings) {
        if (f.code == code) return true;
    }
    return false;
}

Style makeCleanReferenceStyle() {
    Style s;
    s.ppqn = 384;
    s.mpqn = 500000;  // 120 BPM

    auto makeVar = [&](const char* marker, uint8_t base_vel, uint8_t extra_hits) {
        Element el;
        el.marker = marker;
        el.num = 4;
        el.den = 4;
        el.length = 384 * 4 * 2;  // 2 bars = 3072 ticks

        // Voices + balanced genre mix
        addVoice(el, kDrumCh, 120, 0, 0);
        addVoice(el, kPercCh, 120, 0, 56);
        addVoice(el, kBassCh, 121, 0, 32);
        addVoice(el, kAcc1Ch, 121, 0, 0);   // Piano
        addVoice(el, kAcc1Ch + 1u, 121, 0, 25); // Steel Guitar

        addCc(el, 0, kDrumCh, 7, 110); addCc(el, 0, kDrumCh, 10, 64); addCc(el, 0, kDrumCh, 91, 20); addCc(el, 0, kDrumCh, 93, 0);
        addCc(el, 0, kPercCh, 7, 86);  addCc(el, 0, kPercCh, 10, 82); addCc(el, 0, kPercCh, 91, 24); addCc(el, 0, kPercCh, 93, 0);
        addCc(el, 0, kBassCh, 7, 104); addCc(el, 0, kBassCh, 10, 64); addCc(el, 0, kBassCh, 91, 10); addCc(el, 0, kBassCh, 93, 0);
        addCc(el, 0, kAcc1Ch, 7, 90);  addCc(el, 0, kAcc1Ch, 10, 48); addCc(el, 0, kAcc1Ch, 91, 32); addCc(el, 0, kAcc1Ch, 93, 12);
        addCc(el, 0, kAcc1Ch + 1u, 7, 88); addCc(el, 0, kAcc1Ch + 1u, 10, 78); addCc(el, 0, kAcc1Ch + 1u, 91, 28); addCc(el, 0, kAcc1Ch + 1u, 93, 14);

        for (uint32_t bar = 0; bar < 2u; ++bar) {
            const uint32_t b0 = bar * 1536u;
            // Dynamic drum groove with ghost snares
            addNote(el, kDrumCh, b0 + 0, b0 + 120, 36, static_cast<uint8_t>(base_vel + 18));
            addNote(el, kDrumCh, b0 + 768, b0 + 888, 36, static_cast<uint8_t>(base_vel + 14));
            addNote(el, kDrumCh, b0 + 384, b0 + 500, 38, static_cast<uint8_t>(base_vel + 10));
            addNote(el, kDrumCh, b0 + 672, b0 + 740, 38, 42);  // ghost snare (<55)
            addNote(el, kDrumCh, b0 + 1152, b0 + 1270, 38, static_cast<uint8_t>(base_vel + 12));
            addNote(el, kDrumCh, b0 + 1440, b0 + 1500, 38, 44); // ghost snare
            for (uint32_t e = 0; e < 8u; ++e) {
                const uint32_t t = b0 + e * 192u;
                const uint8_t hv = (e % 2u == 0u) ? static_cast<uint8_t>(base_vel) : static_cast<uint8_t>(base_vel - 14);
                addNote(el, kDrumCh, t, t + 100u, 42, hv);
            }
            // Isolated hand perc on kPercCh
            addNote(el, kPercCh, b0 + 192, b0 + 300, 62, static_cast<uint8_t>(base_vel - 8));
            addNote(el, kPercCh, b0 + 960, b0 + 1080, 63, static_cast<uint8_t>(base_vel - 4));

            // Legato root-fifth bass >= E1 (C2=36, G2=43)
            addNote(el, kBassCh, b0 + 0, b0 + 730, 36, static_cast<uint8_t>(base_vel + 6));
            addNote(el, kBassCh, b0 + 768, b0 + 1500, 43, static_cast<uint8_t>(base_vel + 2));

            // Clean open-voiced piano >= C3 (48)
            addNote(el, kAcc1Ch, b0 + 0, b0 + 1400, 60, static_cast<uint8_t>(base_vel - 6));
            addNote(el, kAcc1Ch, b0 + 0, b0 + 1400, 64, static_cast<uint8_t>(base_vel - 10));
            addNote(el, kAcc1Ch, b0 + 0, b0 + 1400, 67, static_cast<uint8_t>(base_vel - 4));

            // Staggered 4-string guitar strum (stagger ~8 ticks = ~10.4ms at 120 BPM, 384 PPQN)
            const uint8_t gtr_p[4] = {52, 55, 60, 64};
            for (uint8_t st = 0; st < 4u; ++st) {
                addNote(el, kAcc1Ch + 1u, b0 + st * 8u, b0 + 680u, gtr_p[st],
                        static_cast<uint8_t>(base_vel + st * 2u));
            }
            // Extra hits for escalating density on higher variations
            for (uint8_t x = 0; x < extra_hits; ++x) {
                addNote(el, kAcc1Ch, b0 + 768u + x * 192u, b0 + 880u + x * 192u,
                        static_cast<uint8_t>(60u + (x % 3u) * 4u), static_cast<uint8_t>(base_vel + 2u));
            }
        }
        sortEl(el);
        return el;
    };

    s.elements.push_back(makeVar("v1cv1", 72, 0));
    s.elements.push_back(makeVar("v2cv1", 78, 1));
    s.elements.push_back(makeVar("v3cv1", 84, 2));
    s.elements.push_back(makeVar("v4cv1", 90, 3));

    // Fill with crescendo
    {
        Element f;
        f.marker = "f1cv1";
        f.num = 4; f.den = 4; f.length = 1536;
        addVoice(f, kDrumCh, 120, 0, 0);
        addNote(f, kDrumCh, 0, 180, 38, 72);
        addNote(f, kDrumCh, 384, 560, 38, 78);
        addNote(f, kDrumCh, 768, 950, 45, 92);
        addNote(f, kDrumCh, 1152, 1340, 48, 108);
        sortEl(f);
        s.elements.push_back(std::move(f));
    }

    // 1-bar Break
    {
        Element br;
        br.marker = "br";
        br.num = 4; br.den = 4; br.length = 1536;
        addVoice(br, kDrumCh, 120, 0, 0);
        addNote(br, kDrumCh, 0, 200, 36, 110);
        addNote(br, kDrumCh, 0, 240, 49, 106);
        sortEl(br);
        s.elements.push_back(std::move(br));
    }

    // Intro (2 bars, harmonic arc, clean lead-in)
    {
        Element i1;
        i1.marker = "i1cv1";
        i1.num = 4; i1.den = 4; i1.length = 3072;
        addVoice(i1, kAcc1Ch, 121, 0, 0);
        addNote(i1, kAcc1Ch, 0, 1480, 60, 82);
        addNote(i1, kAcc1Ch, 0, 1480, 64, 80);
        addNote(i1, kAcc1Ch, 1536, 3000, 62, 86);
        addNote(i1, kAcc1Ch, 1536, 3000, 67, 88);
        sortEl(i1);
        s.elements.push_back(std::move(i1));
    }

    // Ending (2 bars, clean release room)
    {
        Element e1;
        e1.marker = "e1cv1";
        e1.num = 4; e1.den = 4; e1.length = 3072;
        addVoice(e1, kAcc1Ch, 121, 0, 0);
        addNote(e1, kAcc1Ch, 0, 1400, 65, 84);
        addNote(e1, kAcc1Ch, 1536, 2600, 60, 78);
        addNote(e1, kAcc1Ch, 1536, 2600, 64, 76);
        sortEl(e1);
        s.elements.push_back(std::move(e1));
    }
    return s;
}

} // namespace

int main() {
    std::printf("Test: Phase 2 M4 standalone 12-dimension musical quality analyzer\n");

    // ── 1. Clean reference style: Read-only immutability + all 12 dimensions ──
    {
        const Style ref = makeCleanReferenceStyle();
        const auto bytes_before = write(ref);
        TEST("clean reference style writes validly", bytes_before.ok);

        const auto rep1 = analyzeMusicalQuality(ref);
        const auto bytes_after = write(ref);
        TEST("analyzer is strictly read-only (SMF byte-exact)", bytes_before.bytes == bytes_after.bytes);
        TEST("metadata stamps hardwareVerified=false, l3Status=NOT_TESTED, independentFromFrozenQ1=true",
             !rep1.hardware_verified && rep1.l3_status == "NOT_TESTED" && rep1.independent_from_frozen_q1);
        TEST("all 12 dimensions populated and applicable on full reference style",
             rep1.dimensions.size() == 12u && rep1.overall_musical_score >= 92.0);
        TEST("D01 clean drums: 0 HH choke overlaps, >=4 ghost snares, high isolation",
             rep1.dimensions[0].metrics.at("hh_choke_overlaps") == 0.0 &&
             rep1.dimensions[0].metrics.at("ghost_snare_count") >= 4.0 &&
             rep1.dimensions[0].metrics.at("hand_perc_isolation_ratio") == 1.0);
        TEST("D02 clean bass: 0 below E1, legato duty >= 0.85",
             rep1.dimensions[1].metrics.at("notes_below_e1") == 0.0 &&
             rep1.dimensions[1].metrics.at("mean_duty_ratio") >= 0.85);
        TEST("D03 clean guitar: applicable, poly<=6, span<=24, stagger in [4..25]ms",
             rep1.dimensions[2].applicable &&
             rep1.dimensions[2].metrics.at("max_polyphony") == 4.0 &&
             rep1.dimensions[2].metrics.at("max_fretboard_span_st") == 12.0 &&
             rep1.dimensions[2].metrics.at("mean_strum_stagger_ms") >= 5.0 &&
             rep1.dimensions[2].metrics.at("mean_strum_stagger_ms") <= 20.0);
        TEST("D04 clean piano: 0 mud clusters <C3",
             rep1.dimensions[3].metrics.at("mud_clusters_lt_c3") == 0.0);
        TEST("D09 clean escalating V1..V4: 4 points, non-flat, 0 inversions",
             rep1.variation_curve.size() == 4u &&
             rep1.dimensions[8].metrics.at("flat_variation_curve") == 0.0 &&
             rep1.dimensions[8].metrics.at("velocity_inversions") == 0.0);
        TEST("D10 clean Fill+Break: positive crescendo, 1-bar Break valid",
             rep1.dimensions[9].metrics.at("fill_crescendo_delta") > 10.0 &&
             rep1.dimensions[9].metrics.at("break_1bar_valid") == 1.0);
        TEST("D11 clean Intro+Ending: harmonic arc >= 2, lead-in >= 0.90",
             rep1.dimensions[10].metrics.at("max_harmonic_arc_bars") >= 2.0 &&
             rep1.dimensions[10].metrics.at("intro_mean_leadin_ratio") >= 0.90);
        TEST("D12 clean mix: hierarchy ok, pan spread >= 20, bass/drum centered",
             rep1.dimensions[11].metrics.at("cc7_anchor_hierarchy_ok") == 1.0 &&
             rep1.dimensions[11].metrics.at("pan_spread") >= 20.0 &&
             rep1.dimensions[11].metrics.at("bass_drum_centered_ok") == 1.0);

        // Deterministic JSON & Markdown serialization
        const auto rep2 = analyzeMusicalQuality(ref);
        const std::string j1 = formatMusicalQualityJson(rep1);
        const std::string j2 = formatMusicalQualityJson(rep2);
        const std::string m1 = formatMusicalQualityMd(rep1);
        const std::string m2 = formatMusicalQualityMarkdown(rep2);
        TEST("JSON serializer deterministic & includes required metadata keys",
             j1 == j2 &&
             j1.find("\"schema\": \"aiarranger-musical-quality/1\"") != std::string::npos &&
             j1.find("\"hardwareVerified\": false") != std::string::npos &&
             j1.find("\"l3Status\": \"NOT_TESTED\"") != std::string::npos &&
             j1.find("\"independentFromFrozenQ1\": true") != std::string::npos);
        TEST("Markdown serializer deterministic & alias matches",
             m1 == m2 &&
             m1.find("hardwareVerified: false") != std::string::npos &&
             m1.find("independentFromFrozenQ1: true") != std::string::npos);
    }

    // ── 2. D01 negative: HH choke overlap & flat snare detection ────────
    {
        Style s;
        s.ppqn = 384; s.mpqn = 500000;
        Element el{"v1cv1", 4, 4, 3072, {}};
        for (uint32_t i = 0; i < 8u; ++i) {
            addNote(el, kDrumCh, i * 384u, i * 384u + 120u, 38, 96);  // flat snare std=0
        }
        // Open HH (46) overlaps closed HH (42) at tick 192
        addNote(el, kDrumCh, 0, 300, 46, 85);
        addNote(el, kDrumCh, 192, 260, 42, 80);
        sortEl(el);
        s.elements.push_back(std::move(el));

        const auto rep = analyzeMusicalQuality(s);
        TEST("D01 detects open/closed HH choke overlap",
             rep.dimensions[0].metrics.at("hh_choke_overlaps") == 1.0 &&
             hasFindingCode(rep, "MQ_D01_HH_CHOKE_OVERLAP"));
        TEST("D01 detects flat snare dynamics & suggests drum_ghost_dynamics",
             rep.dimensions[0].metrics.at("flat_snare_flag") == 1.0 &&
             hasFindingCode(rep, "MQ_D01_FLAT_SNARE_DYNAMICS"));
    }

    // ── 3. D02 negative: sub-E1/sub-B0 bass, staccato duty, walking bass risk ──
    {
        Style s;
        s.ppqn = 384; s.mpqn = 500000;
        Element el{"v1cv1", 4, 4, 3072, {}};
        // Sub-B0 (20) & sub-E1 (24) + walking bass degrees (C, D, E, G, A) with short staccato gates
        const uint8_t pitches[8] = {20, 24, 36, 38, 40, 43, 45, 40};
        for (uint32_t i = 0; i < 8u; ++i) {
            addNote(el, kBassCh, i * 384u, i * 384u + 100u, pitches[i], 95);
        }
        sortEl(el);
        s.elements.push_back(std::move(el));

        const auto rep = analyzeMusicalQuality(s);
        TEST("D02 detects sub-B0 and sub-E1 bass notes",
             rep.dimensions[1].metrics.at("notes_below_b0") == 1.0 &&
             rep.dimensions[1].metrics.at("notes_below_e1") == 2.0 &&
             hasFindingCode(rep, "MQ_D02_BASS_BELOW_B0") &&
             hasFindingCode(rep, "MQ_D02_BASS_BELOW_E1"));
        TEST("D02 measures staccato bass duty ratio (< 0.40)",
             rep.dimensions[1].metrics.at("mean_duty_ratio") < 0.40);
        TEST("D02 detects single-CV1 walking bass risk",
             rep.dimensions[1].metrics.at("single_cv1_walking_risk") == 1.0 &&
             hasFindingCode(rep, "MQ_D02_SINGLE_CV1_WALKING_BASS"));
    }

    // ── 4. D03 negative: >6 polyphony, >24 span, keyswitch & fret-noise leaks ──
    {
        Style s;
        s.ppqn = 384; s.mpqn = 500000;
        Element el{"v1cv1", 4, 4, 1536, {}};
        addVoice(el, kAcc1Ch, 121, 0, 24);  // Nylon Guitar PC 24
        const uint8_t chord7[7] = {40, 45, 50, 55, 59, 64, 72}; // 7 notes, 32 st span
        for (uint8_t p : chord7) addNote(el, kAcc1Ch, 0, 700, p, 90);
        addNote(el, kAcc1Ch, 768, 900, 100, 80); // high FX/keyswitch leak (>=96)
        addNote(el, kAcc1Ch, 960, 1100, 55, 14); // vel<=20 fret noise leak
        sortEl(el);
        s.elements.push_back(std::move(el));

        const auto rep = analyzeMusicalQuality(s);
        TEST("D03 flags guitar >6 polyphony, >24 st span, keyswitch & fret-noise leaks",
             rep.dimensions[2].metrics.at("max_polyphony") == 7.0 &&
             rep.dimensions[2].metrics.at("max_fretboard_span_st") == 32.0 &&
             rep.dimensions[2].metrics.at("keyswitch_leak_count") == 1.0 &&
             rep.dimensions[2].metrics.at("fret_noise_vel_leak_count") == 1.0 &&
             hasFindingCode(rep, "MQ_D03_GUITAR_POLY_EXCEEDS_6") &&
             hasFindingCode(rep, "MQ_D03_MEGAVOICE_FRET_NOISE_LEAK"));
    }

    // ── 5. D04 family-aware mud check: Piano mud FLAGGED, Timpani <C3 EXEMPT ──
    {
        Style s;
        s.ppqn = 384; s.mpqn = 500000;
        Element el{"v1cv1", 4, 4, 1536, {}};
        addVoice(el, kAcc1Ch, 121, 0, 0);       // Piano (NOT exempt)
        addVoice(el, kAcc1Ch + 1u, 121, 0, 47); // Timpani PC 47 (EXEMPT!)
        // Dense close-position C2-E2-G2 piano mud cluster
        addNote(el, kAcc1Ch, 0, 700, 36, 84);
        addNote(el, kAcc1Ch, 0, 700, 40, 82);
        addNote(el, kAcc1Ch, 0, 700, 43, 80);
        // Maj7 melody note B4 (71) in CV1-only section
        addNote(el, kAcc1Ch, 768, 1400, 71, 86);
        // Timpani low close interval (36 + 39) MUST NOT count as piano mud
        addNote(el, kAcc1Ch + 1u, 0, 700, 36, 98);
        addNote(el, kAcc1Ch + 1u, 0, 700, 39, 96);
        sortEl(el);
        s.elements.push_back(std::move(el));

        const auto rep = analyzeMusicalQuality(s);
        TEST("D04 flags Piano <C3 mud cluster while strictly exempting Timpani PC 47",
             rep.dimensions[3].metrics.at("mud_clusters_lt_c3") == 1.0 &&
             hasFindingCode(rep, "MQ_D04_LOW_REGISTER_MUD_CLUSTER"));
        TEST("D04 detects single-CV1 Maj7 (B) melody clash risk",
             rep.dimensions[3].metrics.at("single_cv1_maj7_melody_notes") == 1.0 &&
             hasFindingCode(rep, "MQ_D04_SINGLE_CV1_MAJ7_CLASH_RISK"));
    }

    // ── 6. D05 source-vs-export unintended velocity compression detection ──
    {
        uasf::StyleDefinition src;
        src.name = "dynamic-source";
        src.tempo_bpm = 120;
        uasf::SectionDefinition sec;
        sec.type = uasf::SectionType::Main1;
        sec.bars = 1; sec.resolution = 384; sec.beats_per_bar = 4; sec.beat_note = 4;
        uasf::TrackDefinition tr;
        tr.midi_channel = kAcc1Ch;
        const uint8_t dyn_v[8] = {45, 115, 50, 118, 48, 120, 52, 112};
        for (uint32_t i = 0; i < 8u; ++i) {
            tr.events.push_back(uasf::MidiEvent{uasf::MidiEventType::NoteOn, kAcc1Ch, 60, dyn_v[i], i * 192u});
            tr.events.push_back(uasf::MidiEvent{uasf::MidiEventType::NoteOff, kAcc1Ch, 60, 0, i * 192u + 120u});
        }
        sec.tracks.push_back(tr);
        src.sections.push_back(sec);

        Style compressed;
        compressed.ppqn = 384; compressed.mpqn = 500000;
        Element el{"v1cv1", 4, 4, 1536, {}};
        for (uint32_t i = 0; i < 8u; ++i) {
            addNote(el, kAcc1Ch, i * 192u, i * 192u + 120u, 60, 82);  // flattened!
        }
        sortEl(el);
        compressed.elements.push_back(std::move(el));

        const auto rep = analyzeMusicalQuality(compressed, nullptr, &src);
        TEST("D05 detects flat velocity track and unintended dynamic compression vs source UASF",
             rep.dimensions[4].metrics.at("flat_track_count") == 1.0 &&
             hasFindingCode(rep, "MQ_D05_UNINTENDED_DYNAMIC_COMPRESSION"));
    }

    // ── 7. D06 & D11: CC11 sidechain, Ending fade snap-back, open CC64/PB/NRPN/CC94 ──
    {
        Style s;
        s.ppqn = 384; s.mpqn = 500000;
        // Looping v1cv1 with EDM sidechain CC11 + abrupt loop jump + open CC64 + uncentered PB + NRPN + CC94
        Element v1{"v1cv1", 4, 4, 3072, {}};
        for (uint32_t i = 0; i < 16u; ++i) {
            const uint8_t v = (i % 2u == 0u) ? 24u : 116u;
            addCc(v1, i * 192u, kAcc1Ch, 11, v);
        }
        addCc(v1, 100, kAcc1Ch, 64, 127);  // left open!
        addPb(v1, 200, kBassCh, 9500);     // left uncentered!
        addCc(v1, 300, kAcc1Ch, 99, 10);   // NRPN leak
        addCc(v1, 320, kAcc1Ch, 94, 40);   // CC94 leak
        addCc(v1, 2000, kAcc1Ch + 1u, 11, 18);
        addCc(v1, 3071, kAcc1Ch + 1u, 11, 127); // abrupt +109 jump at len-1
        addNote(v1, kAcc1Ch, 0, 1400, 60, 90);
        sortEl(v1);
        s.elements.push_back(std::move(v1));

        // One-shot Ending e1cv1 fading 106 -> 68 -> 32 -> 14 then snapping back to 106 at len-1!
        Element e1{"e1cv1", 6, 8, 4608, {}};
        addCc(e1, 0, kAcc1Ch + 2u, 11, 106);
        addCc(e1, 1500, kAcc1Ch + 2u, 11, 68);
        addCc(e1, 2600, kAcc1Ch + 2u, 11, 32);
        addCc(e1, 3408, kAcc1Ch + 2u, 11, 14);
        addCc(e1, 4607, kAcc1Ch + 2u, 11, 106);  // snap-back pop!
        addNote(e1, kAcc1Ch + 2u, 0, 4200, 60, 82);
        sortEl(e1);
        s.elements.push_back(std::move(e1));

        const auto rep = analyzeMusicalQuality(s);
        TEST("D06 detects open CC64, uncentered PB, NRPN & CC94 leaks",
             rep.dimensions[5].metrics.at("open_cc64_at_end") == 1.0 &&
             rep.dimensions[5].metrics.at("uncentered_pb_at_end") == 1.0 &&
             rep.dimensions[5].metrics.at("nrpn_leaks") == 1.0 &&
             rep.dimensions[5].metrics.at("cc94_leaks") == 1.0);
        TEST("D06 detects CC11 sidechain preserved, abrupt loop jump & Ending CC11 snap-back",
             rep.dimensions[5].metrics.at("cc11_sidechain_preserved") == 1.0 &&
             rep.dimensions[5].metrics.at("abrupt_loop_cc11_jumps") >= 1.0 &&
             rep.dimensions[5].metrics.at("ending_cc11_snapback_count") == 1.0 &&
             hasFindingCode(rep, "MQ_D06_ENDING_CC11_SNAPBACK") &&
             hasFindingCode(rep, "MQ_D11_ENDING_FADE_POP_RISK"));
    }

    // ── 8. D07 Sound-Family: never rewards unresolved tracks ───────────
    {
        Style s;
        s.ppqn = 384; s.mpqn = 500000;
        Element el{"v1cv1", 4, 4, 1536, {}};
        addNote(el, kBassCh, 0, 700, 36, 90);
        addNote(el, kAcc1Ch, 0, 700, 60, 88);
        sortEl(el);
        s.elements.push_back(std::move(el));

        Report mock_rep;
        mock_rep.sound_match_stats.tracks_evaluated = 7;
        mock_rep.sound_match_stats.exact_matches = 2;
        mock_rep.sound_match_stats.unresolved = 5;

        const auto rep = analyzeMusicalQuality(s, &mock_rep);
        TEST("D07 reflects fail-closed UNRESOLVED tracks without inflating score",
             rep.dimensions[6].metrics.at("unresolved_count") == 5.0 &&
             rep.dimensions[6].score < 40.0 &&
             hasFindingCode(rep, "MQ_D07_UNRESOLVED_TRACKS"));
    }

    // ── 9. D08 Swing (66%) & Odd Meter (5/4, 7/8) ──────────────────────
    {
        Style s;
        s.ppqn = 384; s.mpqn = 441176;  // ~136 BPM
        Element v1{"v1cv1", 5, 4, 384 * 5, {}};
        // Swing 8th offbeats at 66.7% of quarter note (256 ticks into each 384-tick beat)
        for (uint32_t b = 0; b < 5u; ++b) {
            addNote(v1, kDrumCh, b * 384u, b * 384u + 100u, 51, 82);
            addNote(v1, kDrumCh, b * 384u + 256u, b * 384u + 340u, 51, 76);
        }
        sortEl(v1);
        s.elements.push_back(std::move(v1));

        Element v2{"v2cv1", 7, 8, 192 * 7, {}};
        addNote(v2, kDrumCh, 0, 100, 36, 96);
        sortEl(v2);
        s.elements.push_back(std::move(v2));

        const auto rep = analyzeMusicalQuality(s);
        TEST("D08 measures jazz triplet swing (~66.7%) and odd meters (5/4, 7/8)",
             rep.dimensions[7].metrics.at("swing_ratio_pct") >= 64.0 &&
             rep.dimensions[7].metrics.at("swing_ratio_pct") <= 69.0 &&
             rep.dimensions[7].metrics.at("meter_count") == 2.0 &&
             rep.dimensions[7].metrics.at("odd_meter_present") == 1.0);
    }

    // ── 10. D09 intentional sparse-fortissimo contrast vs flat curve ───
    {
        Style sparse_ff;
        sparse_ff.ppqn = 384; sparse_ff.mpqn = 500000;
        Element v1{"v1cv1", 4, 4, 1536, {}};
        for (uint32_t i = 0; i < 8u; ++i) addNote(v1, kAcc1Ch, i * 192u, i * 192u + 120u, 60, 80);
        sortEl(v1);
        Element v2{"v2cv1", 4, 4, 1536, {}};
        // Sparser (4 notes vs 8) but fortissimo (vel 92 vs 80 = +12!)
        for (uint32_t i = 0; i < 4u; ++i) addNote(v2, kAcc1Ch, i * 384u, i * 384u + 200u, 60, 92);
        sortEl(v2);
        sparse_ff.elements.push_back(std::move(v1));
        sparse_ff.elements.push_back(std::move(v2));

        const auto rep = analyzeMusicalQuality(sparse_ff);
        TEST("D09 recognizes intentional sparse-fortissimo contrast without density-dip penalty",
             rep.dimensions[8].metrics.at("intentional_sparse_ff") == 1.0 &&
             rep.dimensions[8].metrics.at("density_inversions") == 0.0 &&
             rep.dimensions[8].score == 100.0);
    }

    // ── 11. D12 dry/mono default mix & off-center bass/drum ────────────
    {
        Style dry;
        dry.ppqn = 384; dry.mpqn = 500000;
        Element el{"v1cv1", 4, 4, 1536, {}};
        addCc(el, 0, kBassCh, 7, 100);
        addCc(el, 0, kAcc1Ch, 7, 100);
        addNote(el, kBassCh, 0, 700, 36, 90);
        addNote(el, kAcc1Ch, 0, 700, 60, 90);
        sortEl(el);
        dry.elements.push_back(std::move(el));

        const auto rep = analyzeMusicalQuality(dry);
        TEST("D12 flags dry/unpanned default mix",
             rep.dimensions[11].metrics.at("dry_mono_mix_flag") == 1.0 &&
             hasFindingCode(rep, "MQ_D12_DRY_MONO_DEFAULT_MIX"));
    }

    // ── 12. Degenerate / boundary inputs: zero UB, zero NaN/Inf ────────
    {
        Style empty_style;
        const auto rep_empty = analyzeMusicalQuality(empty_style);
        const std::string j_empty = formatMusicalQualityJson(rep_empty);
        TEST("empty style (ppqn=0, 0 elements) produces finite JSON with 0 NaN/Inf",
             std::isfinite(rep_empty.overall_musical_score) &&
             j_empty.find("nan") == std::string::npos &&
             j_empty.find("NaN") == std::string::npos &&
             j_empty.find("inf") == std::string::npos &&
             j_empty.find("Inf") == std::string::npos);

        Style weird;
        weird.ppqn = 0;
        Element z{"v1cv1", 0, 0, 0, {}};
        z.events.push_back(Event{0, static_cast<uint8_t>(0x80u | kBassCh), 36, 0}); // orphan NoteOff
        z.events.push_back(Event{0, static_cast<uint8_t>(0x90u | kBassCh), 36, 90}); // unclosed NoteOn
        weird.elements.push_back(std::move(z));
        const auto rep_weird = analyzeMusicalQuality(weird);
        TEST("zero-length/zero-ppqn/orphan-NoteOff style handled safely without UB",
             std::isfinite(rep_weird.overall_musical_score) &&
             hasFindingCode(rep_weird, "MQ_D08_INVALID_PPQN"));
    }

    // ── 13. compareMusicalQuality delta verification ───────────────────
    {
        const Style clean = makeCleanReferenceStyle();
        Style degraded = clean;
        // Inject an Ending CC11 snap-back pop into degraded
        for (auto& el : degraded.elements) {
            if (el.marker == "e1cv1") {
                addCc(el, 0, kAcc1Ch, 11, 100);
                addCc(el, 2000, kAcc1Ch, 11, 16);
                addCc(el, el.length - 1u, kAcc1Ch, 11, 100);
                sortEl(el);
            }
        }
        const auto r_deg = analyzeMusicalQuality(degraded);
        const auto r_cln = analyzeMusicalQuality(clean);
        const auto delta = compareMusicalQuality(r_deg, r_cln);
        TEST("compareMusicalQuality shows positive overall delta and resolved snap-back",
             delta.overall_delta > 0.0 &&
             !delta.any_critical_regression &&
             delta.newly_introduced_warnings.empty() &&
             std::find(delta.resolved_finding_codes.begin(), delta.resolved_finding_codes.end(),
                       "MQ_D06_ENDING_CC11_SNAPBACK") != delta.resolved_finding_codes.end());
    }

    // ── 14. All 12 repo-local CC0 fixtures/synthetic/*.sty smoke & hygiene ──
    {
        static constexpr const char* kFixtures[12] = {
            "SYNTH_BREAK_TAIL.sty",
            "SYNTH_CHORD_MUTE_SPLIT.sty",
            "SYNTH_FULL_SFF2.sty",
            "SYNTH_MEGAVOICE_GUITAR_BASS.sty",
            "SYNTH_MIXED_METER_RUBATO.sty",
            "SYNTH_MIX_SYSEX_CC_HYGIENE.sty",
            "SYNTH_MULTI_ZONE_SART_OTS.sty",
            "SYNTH_POP_SFF2.sty",
            "SYNTH_REVO_XG_DRUMS.sty",
            "SYNTH_SFF1_CTAB_CNTT.sty",
            "SYNTH_SRC_CHORD_NON_C.sty",
            "SYNTH_WALTZ_68.sty",
        };
        uint32_t analyzed_ok = 0;
        for (const char* fx_name : kFixtures) {
            const auto pr = Sff1Reader().parseFile(synthFixture(fx_name));
            if (!pr.success) continue;
            const auto mr = Sff1ToUasfMapper().map(pr);
            if (!mr.success) continue;
            MapOptions mo = MapOptions::quality();
            mo.ppqn = 384;
            mo.break_policy = BreakPolicy::Yamaha1BarCap;
            mo.route = routeFromCasm(pr);
            const auto kr = mapToKorg(mr.style, mo);
            if (!kr.ok) continue;

            const auto qrep = analyzeMusicalQuality(kr.style, &kr.report, &mr.style);
            const std::string js = formatMusicalQualityJson(qrep);
            const bool clean_num = std::isfinite(qrep.overall_musical_score) &&
                                   qrep.overall_musical_score >= 60.0 &&
                                   js.find("NaN") == std::string::npos &&
                                   js.find("Inf") == std::string::npos &&
                                   !qrep.hardware_verified &&
                                   qrep.l3_status == "NOT_TESTED" &&
                                   qrep.independent_from_frozen_q1;
            if (clean_num) ++analyzed_ok;
        }
        TEST("all 12 repo-local CC0 fixtures analyze cleanly with finite scores and strict metadata",
             analyzed_ok == 12u);
    }

    std::printf("\nSummary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
