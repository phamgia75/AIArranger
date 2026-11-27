// Unit, A/B invariance, explainability, reversibility & guardrail tests for
// Phase 2 Mission 4 opt-in Style Enhancer (`Mode A` vs `Mode B`):
//   src/exporters/korg/style_enhancer.h
//
// Strictly self-contained (synthetic builders + repo-local CC0 fixtures/synthetic).
// Zero private data, zero external paths.

#include "exporters/korg/style_enhancer.h"
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
#include <map>
#include <set>
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

void addNote(Element& el, uint8_t ch, uint32_t on, uint32_t off, uint8_t pitch, uint8_t vel) {
    el.events.push_back(Event{on, static_cast<uint8_t>(0x90u | ch), pitch, vel});
    el.events.push_back(Event{off, static_cast<uint8_t>(0x80u | ch), pitch, 0u});
}

void sortEl(Element& el) {
    style_enhancer_detail::canonicalSortElement(el);
}

bool hasMarker(const Style& s, const std::string& m) {
    for (const auto& el : s.elements) {
        if (el.marker == m) return true;
    }
    return false;
}

const Element* findMarker(const Style& s, const std::string& m) {
    for (const auto& el : s.elements) {
        if (el.marker == m) return &el;
    }
    return nullptr;
}

bool allActionsExplainable(const EnhanceResult& res, const std::string& expected_rule = "") {
    for (const auto& a : res.actions) {
        if (a.rule_id.empty() || a.section.empty() || a.track.empty()) return false;
        if (a.before_summary.empty() || a.after_summary.empty()) return false;
        if (a.before_summary == a.after_summary) return false;
        if (a.musical_reason.empty() || !a.reversible) return false;
        if (!expected_rule.empty() && a.rule_id != expected_rule) return false;
    }
    return true;
}

double elementMeanVel(const Element& el) {
    std::vector<uint8_t> v;
    for (const auto& ev : el.events) {
        if ((ev.status & 0xF0u) == 0x90u && ev.d2 > 0u) v.push_back(ev.d2);
    }
    return style_enhancer_detail::computeMeanVel(v);
}

} // namespace

int main() {
    std::printf("Test: Phase 2 M4 opt-in reversible Style Enhancer (Mode A vs Mode B)\n");

    // ── 1. Default EnhanceOptions invariants ────────────────────────────
    {
        const EnhanceOptions def{};
        const EnhanceOptions faith = EnhanceOptions::faithful();
        const EnhanceOptions off = EnhanceOptions::off();
        const EnhanceOptions rec = EnhanceOptions::recommended();

        TEST("default EnhanceOptions is Mode A Faithful with all 6 rules false",
             def.mode == ConversionMode::Faithful &&
             !def.anyRuleEnabled() && !def.anyEnabled() &&
             faith.mode == ConversionMode::Faithful && !faith.anyEnabled() &&
             off.mode == ConversionMode::Faithful && !off.anyEnabled());
        TEST("recommended() enables IntelligentEnhance and all 6 rules",
             rec.mode == ConversionMode::IntelligentEnhance &&
             rec.enhance_variation_energy_curve &&
             rec.enhance_drum_ghost_dynamics &&
             rec.enhance_bass_register_guard &&
             rec.enhance_piano_mud_voicing &&
             rec.enhance_ending_fade_pop_guard &&
             rec.enhance_jazz_cv_melody_split &&
             rec.anyEnabled());
    }

    // ── 2. Mode A Bit-Exact Bypass Guarantee (even if flags set under Faithful!) ──
    {
        Style s;
        s.ppqn = 384; s.mpqn = 500000;
        Element v1{"v1cv1", 4, 4, 1536, {}};
        addVoice(v1, kBassCh, 121, 0, 32);
        addNote(v1, kBassCh, 0, 700, 24, 90);  // sub-E1 bait!
        addNote(v1, kAcc1Ch, 0, 700, 36, 80);  // mud cluster bait!
        addNote(v1, kAcc1Ch, 0, 700, 40, 80);
        addNote(v1, kAcc1Ch, 0, 700, 43, 80);
        sortEl(v1);
        s.elements.push_back(std::move(v1));

        const auto mode_a_bytes = write(s);
        TEST("synthetic bait style writes validly", mode_a_bytes.ok);

        // (a) Faithful default
        const auto r_def = enhanceStyle(s, EnhanceOptions::faithful());
        TEST("Mode A faithful(): ok, 0 actions, bypass=true, byte-exact SMF",
             r_def.ok && r_def.actions.empty() && r_def.stats.mode_a_bit_exact_bypass &&
             write(r_def.style).bytes == mode_a_bytes.bytes);

        // (b) Mode == Faithful even with rule flags accidentally true!
        EnhanceOptions guarded = EnhanceOptions::recommended();
        guarded.mode = ConversionMode::Faithful;
        const auto r_guard = enhanceStyle(s, guarded);
        TEST("Mode A Faithful overrides rule flags -> 0 mutations, byte-exact SMF",
             r_guard.ok && r_guard.actions.empty() && r_guard.stats.mode_a_bit_exact_bypass &&
             write(r_guard.style).bytes == mode_a_bytes.bytes);

        // (c) Mode == IntelligentEnhance with all rule flags false -> 0 mutations!
        EnhanceOptions empty_b;
        empty_b.mode = ConversionMode::IntelligentEnhance;
        const auto r_empty_b = enhanceStyle(s, empty_b);
        TEST("Mode B with all rule flags false -> 0 mutations, byte-exact SMF",
             r_empty_b.ok && r_empty_b.actions.empty() && r_empty_b.stats.mode_a_bit_exact_bypass &&
             write(r_empty_b.style).bytes == mode_a_bytes.bytes);
    }

    // ── 3. Rule 1: variation_energy_curve (isolated, bounded, exempts sparse-ff) ──
    {
        Style flat_vars;
        flat_vars.ppqn = 384; flat_vars.mpqn = 500000;
        const char* markers[4] = {"v1cv1", "v2cv1", "v3cv1", "v4cv1"};
        for (const char* m : markers) {
            Element el{m, 4, 4, 1536, {}};
            addVoice(el, kAcc1Ch, 121, 0, 0);
            // Same mean velocity (80) with subtle micro-dynamics (74, 86)
            for (uint32_t i = 0; i < 4u; ++i) {
                addNote(el, kAcc1Ch, i * 384u, i * 384u + 300u, 60, (i % 2u == 0u) ? 74u : 86u);
            }
            sortEl(el);
            flat_vars.elements.push_back(std::move(el));
        }

        EnhanceOptions opt;
        opt.mode = ConversionMode::IntelligentEnhance;
        opt.enhance_variation_energy_curve = true;
        const auto res = enhanceStyle(flat_vars, opt);
        TEST("Rule 1 (variation_energy_curve): ok, emits explainable actions only for rule 1",
             res.ok && !res.actions.empty() && allActionsExplainable(res, "variation_energy_curve"));

        const double v1m = elementMeanVel(*findMarker(res.style, "v1cv1"));
        const double v2m = elementMeanVel(*findMarker(res.style, "v2cv1"));
        const double v3m = elementMeanVel(*findMarker(res.style, "v3cv1"));
        const double v4m = elementMeanVel(*findMarker(res.style, "v4cv1"));
        TEST("Rule 1 creates monotonic V1 < V2 < V3 < V4 within +/-12 vel cap",
             v1m < v2m && v2m < v3m && v3m < v4m && (v4m - v1m) <= 24.0 && (v4m - v1m) >= 4.0);

        // Idempotency on pass 2
        const auto res2 = enhanceStyle(res.style, opt);
        TEST("Rule 1 is idempotent on 2nd pass (0 actions, byte-exact)",
             res2.ok && res2.actions.empty() && write(res2.style).bytes == write(res.style).bytes);

        // Protective guard: intentional sparse-fortissimo V2 must NOT be flattened!
        Style orch;
        orch.ppqn = 384; orch.mpqn = 500000;
        Element ov1{"v1cv1", 4, 4, 1536, {}};
        for (uint32_t i = 0; i < 8u; ++i) addNote(ov1, kAcc1Ch, i * 192u, i * 192u + 140u, 60, 80);
        sortEl(ov1);
        Element ov2{"v2cv1", 4, 4, 1536, {}};
        for (uint32_t i = 0; i < 4u; ++i) addNote(ov2, kAcc1Ch, i * 384u, i * 384u + 260u, 60, 92);
        sortEl(ov2);
        orch.elements.push_back(std::move(ov1));
        orch.elements.push_back(std::move(ov2));

        const auto res_orch = enhanceStyle(orch, opt);
        TEST("Rule 1 preserves intentional sparse-fortissimo orchestral V2 untouched",
             res_orch.ok && res_orch.actions.empty() &&
             write(res_orch.style).bytes == write(orch).bytes);
    }

    // ── 4. Rule 2: drum_ghost_dynamics (flat drum + fill lift; skips dynamic drums) ──
    {
        Style flat_drum;
        flat_drum.ppqn = 384; flat_drum.mpqn = 500000;
        Element v1{"v1cv1", 4, 4, 3072, {}};
        addVoice(v1, kDrumCh, 120, 0, 0);
        for (uint32_t bar = 0; bar < 2u; ++bar) {
            const uint32_t b0 = bar * 1536u;
            addNote(v1, kDrumCh, b0 + 384, b0 + 480, 38, 100);  // beat 2 flat
            addNote(v1, kDrumCh, b0 + 1152, b0 + 1248, 38, 100); // beat 4 flat
            addNote(v1, kDrumCh, b0 + 672, b0 + 730, 38, 100);  // offbeat flat
            for (uint32_t e = 0; e < 8u; ++e) {
                addNote(v1, kDrumCh, b0 + e * 192u, b0 + e * 192u + 90u, 42, 80); // flat hat
            }
        }
        sortEl(v1);
        flat_drum.elements.push_back(std::move(v1));

        // Flat/decrescendo Fill (snare vel 100 matching v1cv1 so pre-enhance snare std == 0.0)
        Element f1{"f1cv1", 4, 4, 1536, {}};
        addVoice(f1, kDrumCh, 120, 0, 0);
        addNote(f1, kDrumCh, 0, 180, 38, 100);
        addNote(f1, kDrumCh, 384, 560, 38, 100);
        addNote(f1, kDrumCh, 768, 950, 45, 98);
        addNote(f1, kDrumCh, 1152, 1340, 48, 96);
        sortEl(f1);
        flat_drum.elements.push_back(std::move(f1));

        EnhanceOptions opt;
        opt.mode = ConversionMode::IntelligentEnhance;
        opt.enhance_drum_ghost_dynamics = true;
        const auto res = enhanceStyle(flat_drum, opt);
        TEST("Rule 2 (drum_ghost_dynamics): shapes flat snare, flat hat & fill crescendo",
             res.ok && res.actions.size() >= 3u && allActionsExplainable(res, "drum_ghost_dynamics"));

        const auto qa_before = analyzeMusicalQuality(flat_drum);
        const auto qa_after = analyzeMusicalQuality(res.style);
        TEST("Rule 2 lifts snare vel stddev >= 3.0 and improves fill crescendo",
             qa_before.dimensions[0].metrics.at("snare_vel_stddev") < 1.0 &&
             qa_after.dimensions[0].metrics.at("snare_vel_stddev") >= 3.0 &&
             qa_after.dimensions[9].metrics.at("fill_crescendo_delta") >
             qa_before.dimensions[9].metrics.at("fill_crescendo_delta"));

        // Protective guard: already-dynamic funk drums with ghost notes untouched!
        Style dyn_drum;
        dyn_drum.ppqn = 384; dyn_drum.mpqn = 500000;
        Element dv1{"v1cv1", 4, 4, 1536, {}};
        addVoice(dv1, kDrumCh, 120, 0, 0);
        addNote(dv1, kDrumCh, 384, 480, 38, 108);
        addNote(dv1, kDrumCh, 672, 740, 38, 38);  // ghost snare < 55
        addNote(dv1, kDrumCh, 1152, 1248, 38, 104);
        addNote(dv1, kDrumCh, 1440, 1500, 38, 42);
        sortEl(dv1);
        dyn_drum.elements.push_back(std::move(dv1));

        const auto res_dyn = enhanceStyle(dyn_drum, opt);
        TEST("Rule 2 STRICTLY skips already-dynamic funk drums (0 actions, byte-exact)",
             res_dyn.ok && res_dyn.actions.empty() &&
             write(res_dyn.style).bytes == write(dyn_drum).bytes);
    }

    // ── 5. Rule 3: bass_register_guard (sub-E1 fold; no-op when >= 28) ─
    {
        Style sub_bass;
        sub_bass.ppqn = 384; sub_bass.mpqn = 500000;
        Element v1{"v1cv1", 4, 4, 1536, {}};
        addVoice(v1, kBassCh, 121, 0, 32);
        addNote(v1, kBassCh, 0, 350, 24, 96);    // C1 (<28) -> C2 (36)
        addNote(v1, kBassCh, 384, 720, 22, 92);  // Bb0 (<28) -> Bb1 (34)
        addNote(v1, kBassCh, 768, 1100, 28, 94); // E1 (==28) -> untouched!
        addNote(v1, kBassCh, 1152, 1500, 36, 95); // C2 (>28) -> untouched!
        sortEl(v1);
        sub_bass.elements.push_back(std::move(v1));

        EnhanceOptions opt;
        opt.mode = ConversionMode::IntelligentEnhance;
        opt.enhance_bass_register_guard = true;
        const auto res = enhanceStyle(sub_bass, opt);
        TEST("Rule 3 (bass_register_guard): folds strictly <E1 notes up an octave",
             res.ok && res.actions.size() == 2u && res.stats.notes_pitch_adjusted == 2u &&
             allActionsExplainable(res, "bass_register_guard"));

        const auto qa_after = analyzeMusicalQuality(res.style);
        TEST("Rule 3 eliminates all sub-E1 notes (min_pitch >= 28)",
             qa_after.dimensions[1].metrics.at("notes_below_e1") == 0.0 &&
             qa_after.dimensions[1].metrics.at("min_pitch") == 28.0);

        // No-op when already >= 28 (idempotent!)
        const auto res2 = enhanceStyle(res.style, opt);
        TEST("Rule 3 is a 0-action no-op when all bass notes >= E1",
             res2.ok && res2.actions.empty() && write(res2.style).bytes == write(res.style).bytes);
    }

    // ── 6. Rule 4: piano_mud_voicing (open-voices <C3; exempts Timpani & Bass) ──
    {
        Style muddy;
        muddy.ppqn = 384; muddy.mpqn = 500000;
        Element v1{"v1cv1", 4, 4, 1536, {}};
        addVoice(v1, kAcc1Ch, 121, 0, 0);       // Piano (eligible)
        addVoice(v1, kAcc1Ch + 1u, 121, 0, 47); // Timpani PC 47 (EXEMPT!)
        addVoice(v1, kBassCh, 121, 0, 32);      // Bass (EXEMPT!)

        // Piano close-position C2-E2-G2 [36, 40, 43] -> keeps root 36, lifts 40->52 -> [36, 43, 52]
        addNote(v1, kAcc1Ch, 0, 700, 36, 84);
        addNote(v1, kAcc1Ch, 0, 700, 40, 82);
        addNote(v1, kAcc1Ch, 0, 700, 43, 80);

        // Timpani low close pitches [36, 39] MUST stay untouched
        addNote(v1, kAcc1Ch + 1u, 0, 700, 36, 98);
        addNote(v1, kAcc1Ch + 1u, 0, 700, 39, 96);

        // Bass low chord/dyad [36, 40] MUST stay untouched
        addNote(v1, kBassCh, 0, 700, 36, 100);
        sortEl(v1);
        muddy.elements.push_back(std::move(v1));

        EnhanceOptions opt;
        opt.mode = ConversionMode::IntelligentEnhance;
        opt.enhance_piano_mud_voicing = true;
        const auto res = enhanceStyle(muddy, opt);
        TEST("Rule 4 (piano_mud_voicing): open-voices Piano <C3 cluster while exempting Timpani",
             res.ok && res.actions.size() == 1u &&
             res.actions[0].korg_ch == kAcc1Ch &&
             allActionsExplainable(res, "piano_mud_voicing"));

        // Verify Piano pitches are [36, 43, 52] and Timpani pitches remain [36, 39]
        const auto piano_notes = style_enhancer_detail::extractChannelNotes(res.style.elements[0], kAcc1Ch);
        const auto timp_notes  = style_enhancer_detail::extractChannelNotes(res.style.elements[0], kAcc1Ch + 1u);
        std::vector<uint8_t> pp, tp;
        for (const auto& n : piano_notes) pp.push_back(n.pitch);
        for (const auto& n : timp_notes) tp.push_back(n.pitch);
        std::sort(pp.begin(), pp.end());
        std::sort(tp.begin(), tp.end());
        TEST("Rule 4 keeps Piano bass root 36, moves inner 3rd 40->52, leaves Timpani [36,39] intact",
             pp == std::vector<uint8_t>({36, 43, 52}) &&
             tp == std::vector<uint8_t>({36, 39}));

        const auto qa_after = analyzeMusicalQuality(res.style);
        TEST("Rule 4 reduces mud_clusters_lt_c3 to 0 and is idempotent",
             qa_after.dimensions[3].metrics.at("mud_clusters_lt_c3") == 0.0 &&
             enhanceStyle(res.style, opt).actions.empty());
    }

    // ── 7. Rule 5: ending_fade_pop_guard (Ending snap-back removed; Looping V1 ramped) ──
    {
        Style cc_style;
        cc_style.ppqn = 384; cc_style.mpqn = 500000;

        // One-shot Ending e1cv1: fades 106 -> 68 -> 32 -> 14, then snaps 106 at 4607
        Element e1{"e1cv1", 6, 8, 4608, {}};
        addVoice(e1, kAcc1Ch + 2u, 121, 0, 48);
        addCc(e1, 0, kAcc1Ch + 2u, 11, 106);
        addCc(e1, 1500, kAcc1Ch + 2u, 11, 68);
        addCc(e1, 2600, kAcc1Ch + 2u, 11, 32);
        addCc(e1, 3408, kAcc1Ch + 2u, 11, 14);
        addCc(e1, 4607, kAcc1Ch + 2u, 11, 106);
        addNote(e1, kAcc1Ch + 2u, 0, 4200, 60, 82);
        sortEl(e1);
        cc_style.elements.push_back(std::move(e1));

        // Looping v1cv1: fades to 18 then resets to 127 at 3071 (MUST keep reset 127, add ramp!)
        Element v1{"v1cv1", 4, 4, 3072, {}};
        addVoice(v1, kAcc1Ch + 2u, 121, 0, 48);
        addCc(v1, 0, kAcc1Ch + 2u, 11, 120);
        addCc(v1, 1152, kAcc1Ch + 2u, 11, 18);
        addCc(v1, 3071, kAcc1Ch + 2u, 11, 127);
        addNote(v1, kAcc1Ch + 2u, 0, 2900, 60, 82);
        sortEl(v1);
        cc_style.elements.push_back(std::move(v1));

        EnhanceOptions opt;
        opt.mode = ConversionMode::IntelligentEnhance;
        opt.enhance_ending_fade_pop_guard = true;
        const auto res = enhanceStyle(cc_style, opt);
        TEST("Rule 5 (ending_fade_pop_guard): removes Ending snap-back AND ramps looping V1 reset",
             res.ok && res.actions.size() == 2u &&
             res.stats.cc_events_removed == 1u &&
             res.stats.cc_events_added >= 2u &&
             allActionsExplainable(res, "ending_fade_pop_guard"));

        // Verify e1cv1 ends at CC11=14 (no 106 snap-back), while v1cv1 still ends at CC11=127!
        int e1_last_cc11 = -1, v1_last_cc11 = -1;
        for (const auto& ev : findMarker(res.style, "e1cv1")->events) {
            if ((ev.status & 0xF0u) == 0xB0u && ev.d1 == 11u) e1_last_cc11 = ev.d2;
        }
        for (const auto& ev : findMarker(res.style, "v1cv1")->events) {
            if ((ev.status & 0xF0u) == 0xB0u && ev.d1 == 11u) v1_last_cc11 = ev.d2;
        }
        TEST("Rule 5 leaves e1cv1 fade cleanly at CC11=14 and preserves v1cv1 loop reset CC11=127",
             e1_last_cc11 == 14 && v1_last_cc11 == 127);

        const auto qa_after = analyzeMusicalQuality(res.style);
        TEST("Rule 5 resolves Ending snap-back & abrupt loop jump findings (and is idempotent)",
             qa_after.dimensions[5].metrics.at("ending_cc11_snapback_count") == 0.0 &&
             qa_after.dimensions[5].metrics.at("abrupt_loop_cc11_jumps") == 0.0 &&
             enhanceStyle(res.style, opt).actions.empty());
    }

    // ── 8. Rule 6: jazz_cv_melody_split (synthesizes CV2/CV3/CV4; preserves CV1, br, existing multi-CV) ──
    {
        Style jazz;
        jazz.ppqn = 384; jazz.mpqn = 441176;

        // Single-CV1 Main A with walking bass (C-D-E-G-A) + Maj7 Sax melody (B4=71)
        Element v1{"v1cv1", 4, 4, 3072, {}};
        addVoice(v1, kBassCh, 121, 13, 32);
        addVoice(v1, kAcc1Ch + 4u, 121, 0, 66);
        const uint8_t wb[8] = {36, 38, 40, 43, 45, 43, 40, 38};
        for (uint32_t i = 0; i < 8u; ++i) {
            addNote(v1, kBassCh, i * 384u, i * 384u + 340u, wb[i], 96);
        }
        addNote(v1, kAcc1Ch + 4u, 0, 700, 71, 88);    // Maj7 B4
        addNote(v1, kAcc1Ch + 4u, 768, 1400, 64, 86); // E4
        addNote(v1, kAcc1Ch + 4u, 1536, 2200, 71, 90);
        sortEl(v1);
        jazz.elements.push_back(v1);

        // Existing multi-CV Main B (v2cv1 + v2cv2) MUST NOT be overwritten or duplicated
        Element v2c1{"v2cv1", 4, 4, 1536, {}};
        addNote(v2c1, kAcc1Ch + 4u, 0, 700, 71, 88);
        sortEl(v2c1);
        Element v2c2{"v2cv2", 4, 4, 1536, {}};
        addNote(v2c2, kAcc1Ch + 4u, 0, 700, 70, 88);
        sortEl(v2c2);
        jazz.elements.push_back(std::move(v2c1));
        jazz.elements.push_back(std::move(v2c2));

        // Break ("br") MUST NEVER be split
        Element br{"br", 4, 4, 1536, {}};
        addNote(br, kAcc1Ch + 4u, 0, 700, 71, 88);
        sortEl(br);
        jazz.elements.push_back(std::move(br));

        EnhanceOptions opt;
        opt.mode = ConversionMode::IntelligentEnhance;
        opt.enhance_jazz_cv_melody_split = true;
        const auto res = enhanceStyle(jazz, opt);
        TEST("Rule 6 (jazz_cv_melody_split): synthesizes v1cv2, v1cv3, v1cv4 validly",
             res.ok && res.stats.cv_elements_synthesized == 3u &&
             hasMarker(res.style, "v1cv2") && hasMarker(res.style, "v1cv3") && hasMarker(res.style, "v1cv4") &&
             allActionsExplainable(res, "jazz_cv_melody_split"));

        // Verify v1cv1 untouched (still has B4=71), v1cv2 adapts E->Eb & B->Bb, v1cv3 keeps E & adapts B->Bb
        const auto v1_sax = style_enhancer_detail::extractChannelNotes(*findMarker(res.style, "v1cv1"), kAcc1Ch + 4u);
        const auto v2_sax = style_enhancer_detail::extractChannelNotes(*findMarker(res.style, "v1cv2"), kAcc1Ch + 4u);
        const auto v3_sax = style_enhancer_detail::extractChannelNotes(*findMarker(res.style, "v1cv3"), kAcc1Ch + 4u);
        TEST("Rule 6 preserves authentic Maj7 B4 in v1cv1, adapts v1cv2 (Eb/Bb) & v1cv3 (E/Bb)",
             v1_sax[0].pitch == 71u && v1_sax[1].pitch == 64u &&
             v2_sax[0].pitch == 70u && v2_sax[1].pitch == 63u &&
             v3_sax[0].pitch == 70u && v3_sax[1].pitch == 64u);

        // Verify v2 multi-CV untouched (no v2cv3 synthesized) and br untouched
        TEST("Rule 6 never splits Break or existing multi-CV sections",
             !hasMarker(res.style, "v2cv3") && !hasMarker(res.style, "brcv2"));

        // Independent SMF write -> parse round-trip + idempotency
        const auto wr = write(res.style);
        const auto pr = parse(wr.bytes);
        TEST("Rule 6 output passes korg::validate(), write(), independent parse(), and is idempotent",
             wr.ok && pr.ok && pr.style.elements.size() == res.style.elements.size() &&
             enhanceStyle(res.style, opt).actions.empty());
    }

    // ── 9. Audit JSON & Markdown serializers + determinism ─────────────
    {
        Style s;
        s.ppqn = 384; s.mpqn = 500000;
        Element v1{"v1cv1", 4, 4, 1536, {}};
        addVoice(v1, kBassCh, 121, 0, 32);
        addNote(v1, kBassCh, 0, 700, 24, 90);
        sortEl(v1);
        s.elements.push_back(std::move(v1));

        const auto r1 = enhanceStyle(s, EnhanceOptions::recommended());
        const auto r2 = enhanceStyle(s, EnhanceOptions::recommended());
        const std::string j1 = formatEnhancementReportJson(r1);
        const std::string j2 = formatEnhancementAuditJson(r2);
        const std::string m1 = formatEnhancementReportMd(r1);
        const std::string m2 = formatEnhancementAuditMarkdown(r2);
        TEST("Enhancement JSON/Markdown serializers are deterministic & stamp epistemic metadata",
             j1 == j2 && m1 == m2 &&
             write(r1.style).bytes == write(r2.style).bytes &&
             j1.find("\"hardwareVerified\": false") != std::string::npos &&
             j1.find("\"l3Status\": \"NOT_TESTED\"") != std::string::npos &&
             j1.find("\"independentFromFrozenQ1\": true") != std::string::npos &&
             j1.find("\"reversible\": true") != std::string::npos);
    }

    // ── 10. Fail-closed guardrail rollback on malformed input ──────────
    {
        Style bad;
        bad.ppqn = 0;  // invalid!
        const auto rb = enhanceStyle(bad, EnhanceOptions::recommended());
        TEST("malformed input fails closed and rolls back safely",
             !rb.ok && rb.stats.rolled_back_on_guardrail && rb.actions.empty() &&
             !rb.validation_errors.empty());
    }

    // ── 11. All 12 repo-local CC0 fixtures: Mode A invariance + Mode B anti-degradation ──
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
        uint32_t mode_a_exact = 0;
        uint32_t mode_b_clean = 0;
        uint32_t mode_b_converged = 0;

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

            const auto bytes_a = write(kr.style);
            const auto res_a = enhanceStyle(kr.style, EnhanceOptions::faithful(), &kr.report, &mr.style);
            if (res_a.ok && res_a.actions.empty() && res_a.stats.mode_a_bit_exact_bypass &&
                write(res_a.style).bytes == bytes_a.bytes) {
                ++mode_a_exact;
            }

            const auto res_b = enhanceStyle(kr.style, EnhanceOptions::recommended(), &kr.report, &mr.style);
            const auto bytes_b = write(res_b.style);
            const auto parsed_b = parse(bytes_b.bytes);
            const auto qa_a = analyzeMusicalQuality(kr.style, &kr.report, &mr.style);
            const auto qa_b = analyzeMusicalQuality(res_b.style, &kr.report, &mr.style);
            const auto delta = compareMusicalQuality(qa_a, qa_b);

            if (res_b.ok && validate(res_b.style).empty() && bytes_b.ok && parsed_b.ok &&
                allActionsExplainable(res_b) && !delta.any_critical_regression &&
                delta.newly_introduced_warnings.empty()) {
                ++mode_b_clean;
            }

            // Bounded convergence check on pass 2 (0 runaway drift)
            const auto res_b2 = enhanceStyle(res_b.style, EnhanceOptions::recommended(), &kr.report, &mr.style);
            if (res_b2.ok && res_b2.actions.empty() && write(res_b2.style).bytes == bytes_b.bytes) {
                ++mode_b_converged;
            }
        }

        TEST("all 12 CC0 fixtures satisfy Mode A bit-exact invariance", mode_a_exact == 12u);
        TEST("all 12 CC0 fixtures pass Mode B recommended() with 0 hard violations & 0 regressions",
             mode_b_clean == 12u);
        TEST("all 12 CC0 fixtures converge idempotently on 2nd Mode B pass (0 actions)",
             mode_b_converged == 12u);
    }

    std::printf("\nSummary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
