// Synthetic unit & property tests for Track B Q2a CASM Chord-Mute / Note-Mute
// to Korg Pa5X CV1..CV6 splitter. 100% synthetic fixtures, zero private data.

// Dual-include order verification (Order B: cv_splitter.h -> megavoice_sanitizer.h).
#include "exporters/korg/cv_splitter.h"
#include "exporters/korg/megavoice_sanitizer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;
using ai_arranger::arranger::ChordType;
using ai_arranger::uasf::SectionType;

namespace {

int failures = 0;
int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

SectionTrackInput makeTrack(uint8_t src_ch, uint8_t korg_ch,
                            uint64_t chord_mask = kMaskAllChords,
                            uint16_t note_mask = kMaskAllNotes,
                            bool has_casm = true) {
    SectionTrackInput in{};
    in.track = nullptr;
    in.korg_ch = korg_ch;
    in.casm.src_ch = src_ch;
    in.casm.dest_ch = korg_ch;
    in.casm.chord_mute_mask = chord_mask;
    in.casm.note_mute_mask = note_mask;
    in.casm.has_casm = has_casm;
    return in;
}

bool bucketContainsSrc(const CvBucket& b, uint8_t src_ch) {
    for (const auto* t : b.tracks) {
        if (t && t->casm.src_ch == src_ch) return true;
    }
    return false;
}

bool hasDiag(const CvSplitPlan& plan, const char* code) {
    for (const auto& d : plan.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

// Verifies ALL hard korg::validate() & conflict-freedom invariants on a CvSplitPlan.
bool verifyPlanInvariants(const CvSplitPlan& plan) {
    if (plan.buckets.empty()) return false;
    std::set<std::string> seen_markers;
    int prev_cv = -1;
    for (const auto& b : plan.buckets) {
        if (!isValidMarker(b.marker)) return false;
        if (!seen_markers.insert(b.marker).second) return false;
        if (static_cast<int>(b.cv_index) <= prev_cv) return false;
        prev_cv = static_cast<int>(b.cv_index);

        // Zero intra-bucket mutually exclusive tracks on any shared korg_ch!
        for (size_t i = 0; i < b.tracks.size(); ++i) {
            for (size_t j = i + 1; j < b.tracks.size(); ++j) {
                const auto* a = b.tracks[i];
                const auto* c = b.tracks[j];
                if (a->korg_ch != c->korg_ch) continue;
                const uint64_t cm = (a->casm.chord_mute_mask & kMaskAllChords) &
                                    (c->casm.chord_mute_mask & kMaskAllChords);
                const uint16_t nm = static_cast<uint16_t>((a->casm.note_mute_mask & kMaskAllNotes) &
                                                          (c->casm.note_mute_mask & kMaskAllNotes));
                if (cm == 0 || nm == 0) return false;
            }
        }
    }
    return true;
}

} // namespace

int main() {
    std::printf("Test: CASM Chord-Mute / Note-Mute -> Pa5X CV1..CV6 splitter\n");

    // ── 1. Dual-include order & 34-bit mask constants ───────────────────
    {
        CasmTrackView v{};
        TEST("dual-include Order B exposes both headers cleanly",
             v.note_mute_mask == 0x0FFF &&
             static_cast<uint8_t>(MegaVoiceMode::Auto) == 0);

        TEST("34-bit chord masks partition kMaskAllChords with 34 bits total",
             (kMaskMajor | kMaskMinor | kMaskDom7 | kMaskDim | kMaskSus | kMaskOther) == kMaskAllChords &&
             std::popcount(kMaskAllChords) == 34 &&
             (kMaskMajor & (1ULL << 0x00)) != 0 && // Maj
             (kMaskMajor & (1ULL << 0x02)) != 0 && // Maj7
             (kMaskMinor & (1ULL << 0x08)) != 0 && // Min
             (kMaskMinor & (1ULL << 0x0A)) != 0 && // Min7
             (kMaskDim   & (1ULL << 0x11)) != 0 && // Dim
             (kMaskDim   & (1ULL << 0x12)) != 0 && // Dim7
             (kMaskDom7  & (1ULL << 0x13)) != 0 && // Dom7
             (kMaskOther & (1ULL << 0x1F)) != 0 && // 1+5
             (kMaskSus   & (1ULL << 0x20)) != 0 && // Sus4
             (kMaskSus   & (1ULL << 0x21)) != 0);  // Sus2 (1+2+5)
    }

    // ── 2. Exhaustive maxCvCountFor & korgMarkerForCv + isValidMarker ───
    {
        const SectionType all_sections[] = {
            SectionType::Intro1, SectionType::Intro2, SectionType::Intro3,
            SectionType::Main1,  SectionType::Main2,  SectionType::Main3, SectionType::Main4,
            SectionType::Fill1,  SectionType::Fill2,  SectionType::Fill3, SectionType::Fill4,
            SectionType::Ending1, SectionType::Ending2, SectionType::Ending3,
            SectionType::Break
        };

        bool marker_grammar_ok = true;
        for (SectionType st : all_sections) {
            const uint8_t max_cv = maxCvCountFor(st);
            if (st == SectionType::Break) {
                if (max_cv != 0) marker_grammar_ok = false;
                const std::string m0 = korgMarkerForCv(st, 0);
                if (m0 != "br" || !isValidMarker(m0)) marker_grammar_ok = false;
                for (uint8_t cv = 1; cv <= 8; ++cv) {
                    if (!korgMarkerForCv(st, cv).empty()) marker_grammar_ok = false;
                }
            } else {
                const uint8_t expected_max =
                    (st >= SectionType::Main1 && st <= SectionType::Main4) ? 6 : 2;
                if (max_cv != expected_max) marker_grammar_ok = false;
                if (!korgMarkerForCv(st, 0).empty()) marker_grammar_ok = false;
                for (uint8_t cv = 1; cv <= max_cv; ++cv) {
                    const std::string m = korgMarkerForCv(st, cv);
                    if (m.empty() || !isValidMarker(m)) marker_grammar_ok = false;
                }
                for (uint8_t cv = static_cast<uint8_t>(max_cv + 1); cv <= 8; ++cv) {
                    if (!korgMarkerForCv(st, cv).empty()) marker_grammar_ok = false;
                }
            }
        }
        TEST("maxCvCountFor & korgMarkerForCv match korg::isValidMarker across all 15 sections",
             marker_grammar_ok);
    }

    // ── 3. Legacy single-CV fast-path (no clashes, no Minor-only) ───────
    {
        const std::vector<SectionTrackInput> tracks = {
            makeTrack(9,  kDrumCh),
            makeTrack(8,  kPercCh),
            makeTrack(10, kBassCh),
            makeTrack(11, kAcc1Ch),
            makeTrack(12, kAcc1Ch + 1)
        };
        const auto plan = planSectionCvs(SectionType::Main1, "Main A", tracks);
        TEST("Legacy single-CV fast-path emits only v1cv1 with 0 diagnostics",
             verifyPlanInvariants(plan) &&
             plan.buckets.size() == 1 &&
             plan.buckets[0].cv_index == 1 &&
             plan.buckets[0].marker == "v1cv1" &&
             plan.buckets[0].reference_chord == ChordType::Major &&
             plan.buckets[0].tracks.size() == 5 &&
             plan.diagnostics.empty() &&
             plan.clashes_prevented == 0 &&
             plan.shadowed_tracks_dropped == 0);
    }

    // ── 4. Major + Minor 2-channel split on Acc4 & Acc5 with shared cloning ─
    {
        constexpr uint8_t kAcc4 = kAcc1Ch + 3; // ch 14
        constexpr uint8_t kAcc5 = kAcc1Ch + 4; // ch 15
        const std::vector<SectionTrackInput> tracks = {
            makeTrack(9,  kDrumCh, kMaskAllChords),
            makeTrack(10, kBassCh, kMaskAllChords),
            makeTrack(11, kAcc1Ch, kMaskAllChords),
            makeTrack(3,  kAcc4,   kMaskMajor),   // E.P Maj -> Acc4 cv1
            makeTrack(14, kAcc4,   kMaskMinor),   // E.P Min -> Acc4 cv2
            makeTrack(6,  kAcc5,   kMaskMajor),   // Tbn Maj -> Acc5 cv1
            makeTrack(15, kAcc5,   kMaskMinor)    // Tbn Min -> Acc5 cv2
        };
        const auto plan = planSectionCvs(SectionType::Main1, "Main A", tracks);
        TEST("Major+Minor split on Acc4 & Acc5 produces v1cv1 + v1cv2 with cloned Drum/Bass/Acc1",
             verifyPlanInvariants(plan) &&
             plan.buckets.size() == 2 &&
             plan.buckets[0].marker == "v1cv1" &&
             plan.buckets[0].reference_chord == ChordType::Major &&
             plan.buckets[1].marker == "v1cv2" &&
             plan.buckets[1].reference_chord == ChordType::Minor &&
             // Shared tracks cloned into BOTH cv1 and cv2:
             bucketContainsSrc(plan.buckets[0], 9) && bucketContainsSrc(plan.buckets[1], 9) &&
             bucketContainsSrc(plan.buckets[0], 10) && bucketContainsSrc(plan.buckets[1], 10) &&
             bucketContainsSrc(plan.buckets[0], 11) && bucketContainsSrc(plan.buckets[1], 11) &&
             // Mutually exclusive Acc4/Acc5 separated cleanly:
             bucketContainsSrc(plan.buckets[0], 3) && !bucketContainsSrc(plan.buckets[0], 14) &&
             bucketContainsSrc(plan.buckets[1], 14) && !bucketContainsSrc(plan.buckets[1], 3) &&
             bucketContainsSrc(plan.buckets[0], 6) && !bucketContainsSrc(plan.buckets[0], 15) &&
             bucketContainsSrc(plan.buckets[1], 15) && !bucketContainsSrc(plan.buckets[1], 6) &&
             plan.clashes_prevented == 2 &&
             plan.shadowed_tracks_dropped == 0 &&
             hasDiag(plan, "STYLE_QUAL_CHORD_MUTE_SPLIT"));
    }

    // ── 5. Full 6-CV Variation Split (v1cv1..v1cv6) ─────────────────────
    {
        constexpr uint8_t kAcc4 = kAcc1Ch + 3;
        const std::vector<SectionTrackInput> tracks = {
            makeTrack(9,  kDrumCh, kMaskAllChords),
            makeTrack(10, kBassCh, kMaskAllChords),
            makeTrack(0,  kAcc4,   kMaskMajor),
            makeTrack(1,  kAcc4,   kMaskMinor),
            makeTrack(2,  kAcc4,   kMaskDom7),
            makeTrack(3,  kAcc4,   kMaskDim),
            makeTrack(4,  kAcc4,   kMaskSus),
            makeTrack(5,  kAcc4,   kMaskOther)
        };
        const auto plan = planSectionCvs(SectionType::Main2, "Main B", tracks);
        bool six_ok = verifyPlanInvariants(plan) && plan.buckets.size() == 6;
        const char* expected_markers[6] = {"v2cv1", "v2cv2", "v2cv3", "v2cv4", "v2cv5", "v2cv6"};
        const ChordType expected_chords[6] = {
            ChordType::Major, ChordType::Minor, ChordType::Dom7,
            ChordType::Dim,   ChordType::Sus4,  ChordType::Power
        };
        for (uint8_t i = 0; six_ok && i < 6; ++i) {
            const auto& b = plan.buckets[i];
            if (b.cv_index != i + 1 ||
                b.marker != expected_markers[i] ||
                b.reference_chord != expected_chords[i] ||
                !bucketContainsSrc(b, 9) ||
                !bucketContainsSrc(b, 10) ||
                !bucketContainsSrc(b, i) ||
                b.tracks.size() != 3) {
                six_ok = false;
            }
        }
        TEST("Full 6-CV Variation split emits v2cv1..v2cv6 in ascending order with shared Drum/Bass",
             six_ok && plan.clashes_prevented == 5 && plan.shadowed_tracks_dropped == 0);
    }

    // ── 6. Intro / Fill / Ending 2-CV cap + 3rd variant overflow ────────
    {
        constexpr uint8_t kAcc4 = kAcc1Ch + 3;
        const std::vector<SectionTrackInput> tracks = {
            makeTrack(9,  kDrumCh, kMaskAllChords),
            makeTrack(0,  kAcc4,   kMaskMajor),
            makeTrack(1,  kAcc4,   kMaskMinor),
            makeTrack(2,  kAcc4,   kMaskDom7) // 3rd variant overflows maxCv==2!
        };

        const auto p_intro = planSectionCvs(SectionType::Intro1, "Intro A", tracks);
        TEST("Intro1 2-CV cap keeps Major->i1cv1, Minor->i1cv2, drops 3rd variant cleanly",
             verifyPlanInvariants(p_intro) &&
             p_intro.buckets.size() == 2 &&
             p_intro.buckets[0].marker == "i1cv1" && bucketContainsSrc(p_intro.buckets[0], 0) &&
             !bucketContainsSrc(p_intro.buckets[0], 2) &&
             p_intro.buckets[1].marker == "i1cv2" && bucketContainsSrc(p_intro.buckets[1], 1) &&
             !bucketContainsSrc(p_intro.buckets[1], 2) &&
             p_intro.shadowed_tracks_dropped == 1 &&
             hasDiag(p_intro, "STYLE_QUAL_CV_SLOTS_EXHAUSTED"));

        const auto p_fill = planSectionCvs(SectionType::Fill3, "Fill In CC", tracks);
        TEST("Fill3 2-CV cap produces f3cv1 + f3cv2 and drops 3rd variant",
             verifyPlanInvariants(p_fill) &&
             p_fill.buckets.size() == 2 &&
             p_fill.buckets[0].marker == "f3cv1" &&
             p_fill.buckets[1].marker == "f3cv2" &&
             p_fill.shadowed_tracks_dropped == 1);

        const auto p_end = planSectionCvs(SectionType::Ending2, "Ending B", tracks);
        TEST("Ending2 2-CV cap produces e2cv1 + e2cv2 and drops 3rd variant",
             verifyPlanInvariants(p_end) &&
             p_end.buckets.size() == 2 &&
             p_end.buckets[0].marker == "e2cv1" &&
             p_end.buckets[1].marker == "e2cv2" &&
             p_end.shadowed_tracks_dropped == 1);
    }

    // ── 7. Minor-only track without explicit Major peer ─────────────────
    {
        constexpr uint8_t kAcc5 = kAcc1Ch + 4;
        const std::vector<SectionTrackInput> tracks = {
            makeTrack(9,  kDrumCh, kMaskAllChords),
            makeTrack(10, kBassCh, kMaskAllChords),
            makeTrack(15, kAcc5,   kMaskMinor) // Minor-only pad
        };
        const auto plan = planSectionCvs(SectionType::Ending1, "Ending A", tracks);
        TEST("Minor-only track creates e1cv1 (no minor pad) + e1cv2 (minor pad + Drum/Bass)",
             verifyPlanInvariants(plan) &&
             plan.buckets.size() == 2 &&
             plan.buckets[0].marker == "e1cv1" && !bucketContainsSrc(plan.buckets[0], 15) &&
             plan.buckets[1].marker == "e1cv2" && bucketContainsSrc(plan.buckets[1], 15) &&
             bucketContainsSrc(plan.buckets[1], 9) && bucketContainsSrc(plan.buckets[1], 10));
    }

    // ── 8. Break "br" short-circuit & collision handling ────────────────
    {
        constexpr uint8_t kAcc4 = kAcc1Ch + 3;
        const std::vector<SectionTrackInput> tracks = {
            makeTrack(9,  kDrumCh, kMaskAllChords),
            makeTrack(1,  kAcc4,   kMaskMinor),  // put Minor first to verify Major-priority selection!
            makeTrack(0,  kAcc4,   kMaskMajor)
        };
        const auto plan = planSectionCvs(SectionType::Break, "Fill In BA", tracks);
        TEST("Break emits ONLY 'br' (never 'brcv2'), keeps Major-active track, drops Minor cleanly",
             verifyPlanInvariants(plan) &&
             plan.buckets.size() == 1 &&
             plan.buckets[0].cv_index == 0 &&
             plan.buckets[0].marker == "br" &&
             bucketContainsSrc(plan.buckets[0], 9) &&
             bucketContainsSrc(plan.buckets[0], 0) &&
             !bucketContainsSrc(plan.buckets[0], 1) &&
             plan.shadowed_tracks_dropped == 1 &&
             plan.clashes_prevented == 1 &&
             hasDiag(plan, "STYLE_QUAL_CV_SLOTS_EXHAUSTED"));
    }

    // ── 9. Zero-mask guard (chord_mute_mask == 0 or note_mute_mask == 0) ─
    {
        const std::vector<SectionTrackInput> tracks = {
            makeTrack(9,  kDrumCh, kMaskAllChords, kMaskAllNotes),
            makeTrack(11, kAcc1Ch, 0ULL,           kMaskAllNotes), // zero chord mask -> dropped!
            makeTrack(12, kAcc1Ch + 1, kMaskAllChords, 0U)         // zero note mask -> dropped!
        };
        const auto plan = planSectionCvs(SectionType::Main1, "Main A", tracks);
        TEST("Zero-mask guard drops chord_mute==0 and note_mute==0 tracks cleanly",
             verifyPlanInvariants(plan) &&
             plan.buckets.size() == 1 &&
             plan.buckets[0].tracks.size() == 1 &&
             bucketContainsSrc(plan.buckets[0], 9));
    }

    // ── 10. Note-mute exclusivity (disjoint 12-bit note_mute_mask) ──────
    {
        constexpr uint8_t kAcc4 = kAcc1Ch + 3;
        const std::vector<SectionTrackInput> tracks = {
            makeTrack(9,  kDrumCh, kMaskAllChords, kMaskAllNotes),
            makeTrack(4,  kAcc4,   kMaskAllChords, 0x003FU), // roots C..F
            makeTrack(14, kAcc4,   kMaskAllChords, 0x0FC0U)  // roots F#..B (disjoint!)
        };

        const auto p_main = planSectionCvs(SectionType::Main1, "Main A", tracks);
        TEST("Note-mute exclusivity on Main1 separates disjoint tracks (v1cv1 + v1cv6)",
             verifyPlanInvariants(p_main) &&
             p_main.buckets.size() == 2 &&
             p_main.buckets[0].marker == "v1cv1" &&
             p_main.buckets[1].marker == "v1cv6" &&
             bucketContainsSrc(p_main.buckets[0], 4) && !bucketContainsSrc(p_main.buckets[0], 14) &&
             bucketContainsSrc(p_main.buckets[1], 14) && !bucketContainsSrc(p_main.buckets[1], 4) &&
             bucketContainsSrc(p_main.buckets[1], 9) &&
             p_main.clashes_prevented == 1 &&
             p_main.shadowed_tracks_dropped == 0);

        const auto p_intro = planSectionCvs(SectionType::Intro2, "Intro B", tracks);
        TEST("Note-mute exclusivity on Intro2 separates into i2cv1 + i2cv2",
             verifyPlanInvariants(p_intro) &&
             p_intro.buckets.size() == 2 &&
             p_intro.buckets[0].marker == "i2cv1" &&
             p_intro.buckets[1].marker == "i2cv2" &&
             bucketContainsSrc(p_intro.buckets[0], 4) &&
             bucketContainsSrc(p_intro.buckets[1], 14));

        const auto p_br = planSectionCvs(SectionType::Break, "Fill In BA", tracks);
        TEST("Note-mute exclusivity on Break keeps one track in 'br' and drops peer",
             verifyPlanInvariants(p_br) &&
             p_br.buckets.size() == 1 &&
             p_br.buckets[0].marker == "br" &&
             p_br.shadowed_tracks_dropped == 1);
    }

    // ── 11. Empty input & invalid Korg channels ─────────────────────────
    {
        const std::vector<SectionTrackInput> invalid_ch = {
            makeTrack(0, 0xFF),
            makeTrack(1, 3),    // < kBassCh (8)
            makeTrack(2, 16)    // > 15
        };
        const auto p_empty = planSectionCvs(SectionType::Main4, "Main D", {});
        const auto p_inv = planSectionCvs(SectionType::Main4, "Main D", invalid_ch);
        const auto p_br_empty = planSectionCvs(SectionType::Break, "Fill In BA", {});
        TEST("Empty & invalid-channel inputs still produce valid empty single bucket",
             verifyPlanInvariants(p_empty) && p_empty.buckets.size() == 1 && p_empty.buckets[0].tracks.empty() &&
             verifyPlanInvariants(p_inv) && p_inv.buckets.size() == 1 && p_inv.buckets[0].tracks.empty() &&
             verifyPlanInvariants(p_br_empty) && p_br_empty.buckets.size() == 1 && p_br_empty.buckets[0].marker == "br");
    }

    // ── 12. Determinism check ───────────────────────────────────────────
    {
        constexpr uint8_t kAcc4 = kAcc1Ch + 3;
        const std::vector<SectionTrackInput> tracks = {
            makeTrack(9, kDrumCh), makeTrack(0, kAcc4, kMaskMajor),
            makeTrack(1, kAcc4, kMaskMinor), makeTrack(2, kAcc4, kMaskDom7)
        };
        const auto p1 = planSectionCvs(SectionType::Main1, "Main A", tracks);
        const auto p2 = planSectionCvs(SectionType::Main1, "Main A", tracks);
        bool same = (p1.buckets.size() == p2.buckets.size() &&
                     p1.clashes_prevented == p2.clashes_prevented &&
                     p1.shadowed_tracks_dropped == p2.shadowed_tracks_dropped &&
                     p1.diagnostics.size() == p2.diagnostics.size());
        for (size_t i = 0; same && i < p1.buckets.size(); ++i) {
            if (p1.buckets[i].marker != p2.buckets[i].marker ||
                p1.buckets[i].tracks.size() != p2.buckets[i].tracks.size()) {
                same = false;
            }
        }
        TEST("CV splitter output is 100% deterministic across repeated calls", same);
    }

    // ── 13. Audit Round 1 Regression Tests (P1-1, P3-2) ─────────────────
    {
        constexpr uint8_t kAcc4 = kAcc1Ch + 3; // ch 14
        constexpr uint8_t kAcc5 = kAcc1Ch + 4; // ch 15

        // P1-1 Repro 1: Main1 with non-colliding Acc5(Dom7) alongside colliding Acc4(Maj/Min)
        // MUST activate v1cv3 and preserve Acc5(Dom7) with 0 silent drops!
        const std::vector<SectionTrackInput> main_dom7 = {
            makeTrack(9,  kDrumCh, kMaskAllChords),
            makeTrack(3,  kAcc4,   kMaskMajor),
            makeTrack(14, kAcc4,   kMaskMinor),
            makeTrack(15, kAcc5,   kMaskDom7)
        };
        const auto p_main_dom7 = planSectionCvs(SectionType::Main1, "Main A", main_dom7);
        bool has_v1cv3_with_acc5 = false;
        for (const auto& b : p_main_dom7.buckets) {
            if (b.marker == "v1cv3" && b.reference_chord == ChordType::Dom7 &&
                bucketContainsSrc(b, 15) && bucketContainsSrc(b, 9)) {
                has_v1cv3_with_acc5 = true;
            }
        }
        TEST("P1-1 Repro 1: Main1 activates v1cv3 for non-colliding Acc5(Dom7) (0 silent drops)",
             verifyPlanInvariants(p_main_dom7) &&
             p_main_dom7.buckets.size() == 3 &&
             has_v1cv3_with_acc5 &&
             p_main_dom7.shadowed_tracks_dropped == 0);

        // P1-1 Repro 2: Intro1 (2-CV) with non-colliding Acc5(Dom7) alongside colliding Acc4(Maj/Min)
        // MUST preserve Acc5(Dom7) by folding to i1cv1 (never silently dropped!).
        const auto p_intro_dom7 = planSectionCvs(SectionType::Intro1, "Intro A", main_dom7);
        bool acc5_present_in_intro = false;
        for (const auto& b : p_intro_dom7.buckets) {
            if (bucketContainsSrc(b, 15)) acc5_present_in_intro = true;
        }
        TEST("P1-1 Repro 2: Intro1 (2-CV) preserves non-colliding Acc5(Dom7) via cv1 fold (0 silent drops)",
             verifyPlanInvariants(p_intro_dom7) &&
             p_intro_dom7.buckets.size() == 2 &&
             p_intro_dom7.buckets[0].marker == "i1cv1" &&
             p_intro_dom7.buckets[1].marker == "i1cv2" &&
             acc5_present_in_intro &&
             bucketContainsSrc(p_intro_dom7.buckets[0], 15) &&
             p_intro_dom7.shadowed_tracks_dropped == 0);

        // P1-1 bonus: Intro1 (2-CV) with non-colliding Acc5(Dim) folds Dim -> i1cv2 cleanly
        const std::vector<SectionTrackInput> intro_dim = {
            makeTrack(9,  kDrumCh, kMaskAllChords),
            makeTrack(15, kAcc5,   kMaskDim)
        };
        const auto p_intro_dim = planSectionCvs(SectionType::Intro1, "Intro A", intro_dim);
        TEST("P1-1: Intro1 (2-CV) folds non-colliding Acc5(Dim) into i1cv2 (0 silent drops)",
             verifyPlanInvariants(p_intro_dim) &&
             p_intro_dim.buckets.size() == 2 &&
             p_intro_dim.buckets[1].marker == "i1cv2" &&
             bucketContainsSrc(p_intro_dim.buckets[1], 15) &&
             p_intro_dim.shadowed_tracks_dropped == 0);

        // P3-2: Residual placement preserves compatible all-chords/all-notes layer on same korg_ch
        const std::vector<SectionTrackInput> res_compat = {
            makeTrack(9,  kDrumCh, kMaskAllChords, kMaskAllNotes),
            makeTrack(4,  kAcc4,   kMaskAllChords, 0x003FU),       // note-split A
            makeTrack(14, kAcc4,   kMaskAllChords, 0x0FC0U),       // note-split B (exclusive with A)
            makeTrack(6,  kAcc4,   kMaskAllChords, kMaskAllNotes)  // compatible shared layer on Acc4!
        };
        const auto p_res = planSectionCvs(SectionType::Main1, "Main A", res_compat);
        bool layer_preserved_in_cv6 = false;
        for (const auto& b : p_res.buckets) {
            if (b.marker == "v1cv6" && bucketContainsSrc(b, 14) &&
                bucketContainsSrc(b, 6) && !bucketContainsSrc(b, 4)) {
                layer_preserved_in_cv6 = true;
            }
        }
        TEST("P3-2: Residual note-mute placement preserves compatible all-chords/all-notes layer on same korg_ch",
             verifyPlanInvariants(p_res) &&
             p_res.buckets.size() == 2 &&
             bucketContainsSrc(p_res.buckets[0], 4) &&
             bucketContainsSrc(p_res.buckets[0], 6) &&
             layer_preserved_in_cv6 &&
             p_res.shadowed_tracks_dropped == 0);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
