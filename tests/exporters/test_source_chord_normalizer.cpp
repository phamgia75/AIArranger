// Track B Q3a — Unit tests for src/exporters/korg/source_chord_normalizer.h.
// 100% synthetic/CC0 fixtures; zero private data; includes ALL 4 quality headers
// (megavoice_sanitizer.h -> cv_splitter.h -> source_chord_normalizer.h -> drum_remap.h)
// to verify order-independent inclusion with zero ODR conflicts.

#include "exporters/korg/megavoice_sanitizer.h"
#include "exporters/korg/cv_splitter.h"
#include "exporters/korg/source_chord_normalizer.h"
#include "exporters/korg/drum_remap.h"

#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;
using arranger::ChordType;

static int failures = 0;
static int passes   = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } \
} while (0)

static NoteSpan mkNote(uint32_t on, uint32_t off, uint8_t pitch, uint8_t vel = 90) {
    NoteSpan s{};
    s.on_tick  = on;
    s.off_tick = off;
    s.pitch    = pitch;
    s.vel      = vel;
    return s;
}

int main() {
    std::printf("Test: Track B Q3a — Source-Chord Normalizer & Register Audit\n");

    // ── 1. Yamaha Ctb2[18..19] full Table 15 decoder (0x00..0x22 + out-of-range) ──
    {
        bool allValidNames = true;
        for (uint8_t code = 0x00; code <= 0x22; ++code) {
            const auto d = decodeYamahaSourceChord(0, code);
            if (!d.name || d.name[0] == '\0') allValidNames = false;
        }
        TEST("decodeYamahaSourceChord handles all 35 codes 0x00..0x22", allValidNames);
        TEST("decode 0x02 Maj7", decodeYamahaSourceChord(0, 0x02).chord_type == ChordType::Maj7 &&
                                 decodeYamahaSourceChord(0, 0x02).has_maj7);
        TEST("decode 0x0A Min7", decodeYamahaSourceChord(2, 0x0A).root == 2 &&
                                 decodeYamahaSourceChord(2, 0x0A).chord_type == ChordType::Min7 &&
                                 decodeYamahaSourceChord(2, 0x0A).is_minor_family);
        TEST("decode 0x13 Dom7", decodeYamahaSourceChord(7, 0x13).root == 7 &&
                                 decodeYamahaSourceChord(7, 0x13).chord_type == ChordType::Dom7 &&
                                 decodeYamahaSourceChord(7, 0x13).is_dominant_family);
        TEST("decode 0x14 7sus4", decodeYamahaSourceChord(7, 0x14).chord_type == ChordType::Dom7Sus4);
        TEST("decode 0x22 Cancel", decodeYamahaSourceChord(0, 0x22).is_cancel);
        TEST("unset root 0x7F defaults to C (0)", decodeYamahaSourceChord(0x7F, 0x00).root == 0);
        TEST("unknown code 0xFF falls back to Maj", decodeYamahaSourceChord(0, 0xFF).chord_type == ChordType::Major);
    }

    // ── 2. Non-C root minimal signed shift to C ──
    {
        CasmTrackView casm{};
        casm.has_casm = true;
        casm.ntr = 0; casm.ntt = 2; // RootTrans + Chord
        casm.source_chord_root = 2; // D -> delta -2
        casm.source_chord_type = 0x00; // Maj
        const std::vector<NoteSpan> dMaj = {mkNote(0, 480, 62), mkNote(0, 480, 66), mkNote(0, 480, 69)};
        const auto res = normalizeTrackToPa5xReference(dMaj, kAcc1Ch, uasf::TrackRole::Chord, casm);
        TEST("D Maj -> C Maj minimal shift (-2 st)",
             res.normalized_from_non_c && res.notes_transposed == 3 &&
             res.notes[0].pitch == 60 && res.notes[1].pitch == 64 && res.notes[2].pitch == 67);

        // G Maj (root 7 > 6 -> minimal signed shift +5 st up to C, not -7 down!)
        casm.source_chord_root = 7;
        const std::vector<NoteSpan> gMaj = {mkNote(0, 480, 55), mkNote(0, 480, 59), mkNote(0, 480, 62)};
        const auto resG = normalizeTrackToPa5xReference(gMaj, kAcc1Ch, uasf::TrackRole::Chord, casm);
        TEST("G Maj -> C Maj minimal shift (+5 st)",
             resG.normalized_from_non_c && resG.notes_transposed == 3 &&
             resG.notes[0].pitch == 60 && resG.notes[1].pitch == 64 && resG.notes[2].pitch == 67);
    }

    // ── 3. Quality normalization: CMaj7, Cm7, C7 -> C Maj (cv1) & C Maj -> C Min (cv2) ──
    {
        // CMaj7 (0x02) on Chord track -> B (71) snaps to C (72) to prevent C7 clash
        CasmTrackView casmChord{};
        casmChord.has_casm = true; casmChord.ntr = 0; casmChord.ntt = 2;
        casmChord.source_chord_root = 0; casmChord.source_chord_type = 0x02;
        const std::vector<NoteSpan> cmaj7 = {mkNote(0, 480, 60), mkNote(0, 480, 64), mkNote(0, 480, 67), mkNote(0, 480, 71)};
        const auto rChord = normalizeTrackToPa5xReference(cmaj7, kAcc1Ch, uasf::TrackRole::Chord, casmChord, ChordType::Major);
        TEST("CMaj7 Chord -> C Maj snaps B(71) -> C(72)",
             rChord.quality_remapped == 1 && rChord.notes[3].pitch == 72);

        // CMaj7 (0x02) on Melody track -> B (71) preserved untouched!
        CasmTrackView casmMel = casmChord;
        casmMel.ntt = 1; // Melody
        const auto rMel = normalizeTrackToPa5xReference(cmaj7, kAcc1Ch + 1, uasf::TrackRole::Phrase1, casmMel, ChordType::Major);
        TEST("CMaj7 Melody -> C Maj preserves B(71)",
             rMel.quality_remapped == 0 && rMel.notes[3].pitch == 71);

        // Cm7 (0x0A) -> C Maj: Eb(63)->E(64), Bb(70)->C(72) or G(67)
        CasmTrackView casmMin7 = casmChord;
        casmMin7.source_chord_type = 0x0A;
        const std::vector<NoteSpan> cm7 = {mkNote(0, 480, 60), mkNote(0, 480, 63), mkNote(0, 480, 67), mkNote(0, 480, 70)};
        const auto rMin7 = normalizeTrackToPa5xReference(cm7, kAcc1Ch, uasf::TrackRole::Chord, casmMin7, ChordType::Major);
        TEST("Cm7 -> C Maj remaps Eb->E and Bb->C (no Eb/Bb left)",
             rMin7.quality_remapped == 2 && rMin7.notes[1].pitch == 64 &&
             (rMin7.notes[3].pitch == 72 || rMin7.notes[3].pitch == 67));

        // C7 (0x13) -> C Maj: Bb(70) -> C(72) (or G(67) if C(72) already occupied!)
        CasmTrackView casmDom7 = casmChord;
        casmDom7.source_chord_type = 0x13;
        const std::vector<NoteSpan> c7Busy = {mkNote(0, 480, 60), mkNote(0, 480, 64), mkNote(0, 480, 70), mkNote(0, 480, 72)};
        const auto rDom7 = normalizeTrackToPa5xReference(c7Busy, kAcc1Ch, uasf::TrackRole::Chord, casmDom7, ChordType::Major);
        TEST("C7 -> C Maj avoids collision on occupied C5 (72) by mapping Bb4 (70) -> G4 (67)",
             rDom7.quality_remapped == 1 && rDom7.notes[2].pitch == 67);

        // C Maj (0x00) -> C Min (cv2): E(64) -> Eb(63)
        CasmTrackView casmMaj = casmChord;
        casmMaj.source_chord_type = 0x00;
        const std::vector<NoteSpan> cmaj = {mkNote(0, 480, 60), mkNote(0, 480, 64), mkNote(0, 480, 67)};
        const auto rToMin = normalizeTrackToPa5xReference(cmaj, kAcc1Ch, uasf::TrackRole::Chord, casmMaj, ChordType::Minor);
        TEST("C Maj -> C Min (cv2) maps E(64) -> Eb(63)",
             rToMin.quality_remapped == 1 && rToMin.notes[1].pitch == 63);
    }

    // ── 4. Skip guard: Drum/Perc and CASM Bypass (NTR=1 & NTT=0) untouched ──
    {
        CasmTrackView casmBypass{};
        casmBypass.has_casm = true; casmBypass.ntr = 1; casmBypass.ntt = 0;
        casmBypass.source_chord_root = 5; casmBypass.source_chord_type = 0x0A;
        const std::vector<NoteSpan> notes = {mkNote(0, 240, 36), mkNote(240, 480, 42)};

        const auto rDrum = normalizeTrackToPa5xReference(notes, kDrumCh, uasf::TrackRole::Drum, casmBypass);
        TEST("Drum track skipped untouched",
             rDrum.notes[0].pitch == 36 && rDrum.notes[1].pitch == 42 &&
             rDrum.advice.pa5x_ntt_table == "No Transpose" && !rDrum.normalized_from_non_c);

        const auto rByp = normalizeTrackToPa5xReference(notes, kAcc1Ch, uasf::TrackRole::Chord, casmBypass);
        TEST("CASM NTR=1 & NTT=0 Bypass skipped untouched",
             rByp.notes[0].pitch == 36 && rByp.notes[1].pitch == 42 &&
             rByp.advice.pa5x_ntt_type == "No Transpose" && rByp.notes_transposed == 0);
    }

    // ── 5. 12-Root Register Safety Audit (Bass, Chord/Pad + LIL, Guitar) ──
    {
        // High bass (C4=60, G4=67) -> centered down into Bass [24..55] with 0 violations across all 12 roots
        CasmTrackView casmBass{};
        casmBass.has_casm = true; casmBass.ntr = 0; casmBass.ntt = 1; casmBass.ntt_bass = true;
        casmBass.high_key = 11;
        const std::vector<NoteSpan> hiBass = {mkNote(0, 480, 60), mkNote(480, 960, 67)};
        const auto rHiBass = normalizeTrackToPa5xReference(hiBass, kBassCh, uasf::TrackRole::Bass, casmBass);
        const uint32_t violHi = auditRegisterSafetyAcross12Roots(rHiBass.notes, rHiBass.advice.wrap_around, 24, 55);
        TEST("Too-high Bass centered to 0 violations across all 12 roots",
             violHi == 0 && rHiBass.register_violations_fixed > 0 &&
             rHiBass.advice.pa5x_ntt_table == "Root" &&
             rHiBass.advice.wrap_around >= 1 && rHiBass.advice.wrap_around <= 12);

        // Too-low bass (C0=12, G0=19) -> centered up into Bass [24..55] with 0 violations
        const std::vector<NoteSpan> loBass = {mkNote(0, 480, 12), mkNote(480, 960, 19)};
        const auto rLoBass = normalizeTrackToPa5xReference(loBass, kBassCh, uasf::TrackRole::Bass, casmBass);
        const uint32_t violLo = auditRegisterSafetyAcross12Roots(rLoBass.notes, rLoBass.advice.wrap_around, 24, 55);
        TEST("Too-low Bass centered to 0 violations across all 12 roots",
             violLo == 0 && rLoBass.register_violations_fixed > 0);

        // Chord track with muddy low interval (< 48, interval < 7 st)
        CasmTrackView casmChord{};
        casmChord.has_casm = true; casmChord.ntr = 0; casmChord.ntt = 2;
        const std::vector<NoteSpan> muddy = {mkNote(0, 480, 40), mkNote(0, 480, 44), mkNote(0, 480, 47)};
        const auto rMud = normalizeTrackToPa5xReference(muddy, kAcc1Ch, uasf::TrackRole::Chord, casmChord);
        const uint32_t violChord = auditRegisterSafetyAcross12Roots(rMud.notes, rMud.advice.wrap_around, 36, 84);
        TEST("Chord/Pad LIL mud guard & 0 register violations across 12 roots",
             violChord == 0 && rMud.register_violations_fixed > 0);

        // Guitar NTR=2 -> Guitar Mode advice + [40..88] safe
        CasmTrackView casmGtr{};
        casmGtr.has_casm = true; casmGtr.ntr = 2; casmGtr.ntt = 1;
        const std::vector<NoteSpan> gtr = {mkNote(0, 480, 52), mkNote(10, 480, 59), mkNote(20, 480, 64)};
        const auto rGtr = normalizeTrackToPa5xReference(gtr, kAcc1Ch + 2, uasf::TrackRole::Accompaniment, casmGtr);
        const uint32_t violGtr = auditRegisterSafetyAcross12Roots(rGtr.notes, rGtr.advice.wrap_around, 40, 88);
        TEST("Guitar NTR=2 emits Guitar Mode advice and 0 register violations",
             violGtr == 0 && rGtr.advice.pa5x_ntt_type == "Guitar Mode" &&
             rGtr.advice.pa5x_ntt_table == "Guitar");
    }

    // ── 6. Yamaha RTR 0..5 -> Pa5X Retrigger Mode ──
    {
        TEST("RTR 0 -> Off",       mapYamahaRtrToPa5x(0) == "Off");
        TEST("RTR 1 -> Repitch",   mapYamahaRtrToPa5x(1) == "Repitch");
        TEST("RTR 2 -> Repitch",   mapYamahaRtrToPa5x(2) == "Repitch");
        TEST("RTR 3 -> Retrigger", mapYamahaRtrToPa5x(3) == "Retrigger");
        TEST("RTR 4 -> Retrigger", mapYamahaRtrToPa5x(4) == "Retrigger");
        TEST("RTR 5 -> Retrigger", mapYamahaRtrToPa5x(5) == "Retrigger");
        TEST("RTR 99 -> Off",      mapYamahaRtrToPa5x(99) == "Off");
    }

    // ── 7. Extreme / malformed edge cases (zero UB, strict on < off, pitch in [0..127]) ──
    {
        CasmTrackView casm{};
        casm.has_casm = true; casm.ntr = 0; casm.ntt = 2;
        casm.source_chord_root = 6; casm.source_chord_type = 0x12;

        // Empty track
        const auto rEmpty = normalizeTrackToPa5xReference({}, kAcc1Ch, uasf::TrackRole::Chord, casm);
        TEST("Empty input track handled cleanly", rEmpty.notes.empty());

        // Malformed spans: off_tick <= on_tick, vel == 0, pitch > 127, on_tick == UINT32_MAX
        NoteSpan bad1{500, 500, 200, 0, false};
        NoteSpan bad2{std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(), 0, 255, false};
        const auto rBad = normalizeTrackToPa5xReference({bad1, bad2}, kAcc1Ch, uasf::TrackRole::Chord, casm);
        bool invOk = (rBad.notes.size() == 2);
        for (const auto& s : rBad.notes) {
            if (!(s.on_tick < s.off_tick && s.pitch <= 127 && s.vel >= 1 && s.vel <= 127)) invOk = false;
        }
        TEST("Malformed spans sanitized (strict on_tick < off_tick, pitch/vel clamped)", invOk);
    }

    // ── 8. Audit Round 1 P2-1: Coherent key_range_low < key_range_high <= 127 ──
    {
        const std::vector<NoteSpan> triad = {mkNote(0, 480, 60), mkNote(0, 480, 64), mkNote(0, 480, 67)};

        CasmTrackView cHi{};
        cHi.has_casm = true; cHi.ntr = 0; cHi.ntt = 2; cHi.note_low = 90; cHi.note_high = 127;
        const auto rHi = normalizeTrackToPa5xReference(triad, kAcc1Ch, uasf::TrackRole::Chord, cHi);
        TEST("P2-1: partial CASM note_low=90, note_high=127 yields coherent [90, 127] (never inverted)",
             rHi.advice.key_range_low == 90 && rHi.advice.key_range_high == 127 &&
             rHi.advice.key_range_low < rHi.advice.key_range_high);

        CasmTrackView cLo{};
        cLo.has_casm = true; cLo.ntr = 0; cLo.ntt = 2; cLo.note_low = 0; cLo.note_high = 30;
        const auto rLo = normalizeTrackToPa5xReference(triad, kAcc1Ch, uasf::TrackRole::Chord, cLo);
        TEST("P2-1: partial CASM note_low=0, note_high=30 yields coherent [0, 30] (never inverted)",
             rLo.advice.key_range_low == 0 && rLo.advice.key_range_high == 30 &&
             rLo.advice.key_range_low < rLo.advice.key_range_high);

        CasmTrackView cOob{};
        cOob.has_casm = true; cOob.ntr = 0; cOob.ntt = 2; cOob.note_low = 150; cOob.note_high = 200;
        const auto rOob = normalizeTrackToPa5xReference(triad, kAcc1Ch, uasf::TrackRole::Chord, cOob);
        TEST("P2-1: out-of-range CASM note_low=150, note_high=200 falls back to valid [36, 84]",
             rOob.advice.key_range_low == 36 && rOob.advice.key_range_high == 84 &&
             rOob.advice.key_range_high <= 127);
    }

    // ── 9. Audit Round 1 P2-3: Zero duplicate simultaneous (on_tick, pitch) spans ──
    {
        auto hasDup = [](const std::vector<NoteSpan>& ns) {
            std::set<std::pair<uint32_t, uint8_t>> seen;
            for (const auto& s : ns) {
                if (!seen.insert({s.on_tick, s.pitch}).second) return true;
            }
            return false;
        };

        CasmTrackView casmMaj7{};
        casmMaj7.has_casm = true; casmMaj7.ntr = 0; casmMaj7.ntt = 2;
        casmMaj7.source_chord_root = 0; casmMaj7.source_chord_type = 0x02; // CMaj7
        const std::vector<NoteSpan> maj7Dense = {
            mkNote(0, 480, 60), mkNote(0, 480, 64), mkNote(0, 480, 67),
            mkNote(0, 480, 71), mkNote(0, 480, 72)
        };
        const auto rMaj7 = normalizeTrackToPa5xReference(maj7Dense, kAcc1Ch, uasf::TrackRole::Chord, casmMaj7, ChordType::Major);
        TEST("P2-3: CMaj7 {60,64,67,71,72} produces zero duplicate simultaneous pitches",
             !hasDup(rMaj7.notes) && !rMaj7.notes.empty());

        CasmTrackView casmDom7 = casmMaj7;
        casmDom7.source_chord_type = 0x13; // C7
        const std::vector<NoteSpan> dom7Dense = {
            mkNote(0, 480, 60), mkNote(0, 480, 64), mkNote(0, 480, 67),
            mkNote(0, 480, 70), mkNote(0, 480, 72)
        };
        const auto rDom7 = normalizeTrackToPa5xReference(dom7Dense, kAcc1Ch, uasf::TrackRole::Chord, casmDom7, ChordType::Major);
        TEST("P2-3: C7 {60,64,67,70,72} produces zero duplicate simultaneous pitches",
             !hasDup(rDom7.notes) && !rDom7.notes.empty());

        CasmTrackView casmLil = casmMaj7;
        casmLil.source_chord_type = 0x00; // C Maj
        const std::vector<NoteSpan> lilCollide = {
            mkNote(0, 480, 40), mkNote(0, 480, 44), mkNote(0, 480, 56)
        };
        const auto rLil = normalizeTrackToPa5xReference(lilCollide, kAcc1Ch, uasf::TrackRole::Chord, casmLil, ChordType::Major);
        bool stillMuddy = false;
        for (size_t i = 0; i < rLil.notes.size(); ++i) {
            for (size_t j = i + 1; j < rLil.notes.size(); ++j) {
                const int lo = std::min(rLil.notes[i].pitch, rLil.notes[j].pitch);
                const int hi = std::max(rLil.notes[i].pitch, rLil.notes[j].pitch);
                if (lo < 48 && hi > lo && (hi - lo) < 7) stillMuddy = true;
            }
        }
        TEST("P2-3: LIL lift on {40,44,56} avoids colliding onto 56 (3 unique pitches, no mud)",
             !hasDup(rLil.notes) && rLil.notes.size() == 3 && !stillMuddy);
    }

    // ── 10. Mission 4b Regression Tests (P0-1 CV3..CV6 & P1-1 SFF2 Guitar NTR=2, NTT=1) ──
    {
        CasmTrackView casm{};
        casm.has_casm = true;
        casm.ntr = 0;
        casm.ntt = 2;
        casm.source_chord_root = 0;

        // P0-1a: CV3 (Dom7) preserves b7 (pc 10: Bb=70)
        casm.source_chord_type = 0x13; // Dom7
        const std::vector<NoteSpan> dom7 = {mkNote(0, 480, 60), mkNote(0, 480, 64), mkNote(0, 480, 67), mkNote(0, 480, 70)};
        const auto rCv3 = normalizeTrackToPa5xReference(dom7, kAcc1Ch, uasf::TrackRole::Chord, casm, ChordType::Dom7);
        TEST("Mission4b P0-1: CV3 (Dom7) preserves b7 (Bb=70) untouched",
             rCv3.quality_remapped == 0 && rCv3.notes.size() == 4 &&
             rCv3.notes[0].pitch == 60 && rCv3.notes[1].pitch == 64 &&
             rCv3.notes[2].pitch == 67 && rCv3.notes[3].pitch == 70);

        // P0-1b: CV4 (Dim/Dim7) preserves b3 (Eb=63), b5 (Gb=66), bb7 (A=69)
        casm.source_chord_type = 0x12; // Dim7
        const std::vector<NoteSpan> dim7 = {mkNote(0, 480, 60), mkNote(0, 480, 63), mkNote(0, 480, 66), mkNote(0, 480, 69)};
        const auto rCv4 = normalizeTrackToPa5xReference(dim7, kAcc1Ch, uasf::TrackRole::Chord, casm, ChordType::Dim);
        TEST("Mission4b P0-1: CV4 (Dim7) preserves b3(63), b5(66), bb7(69) untouched",
             rCv4.quality_remapped == 0 && rCv4.notes.size() == 4 &&
             rCv4.notes[0].pitch == 60 && rCv4.notes[1].pitch == 63 &&
             rCv4.notes[2].pitch == 66 && rCv4.notes[3].pitch == 69);

        // P0-1c: CV5 (Sus4) preserves 4th (F=65)
        casm.source_chord_type = 0x20; // Sus4
        const std::vector<NoteSpan> sus4 = {mkNote(0, 480, 60), mkNote(0, 480, 65), mkNote(0, 480, 67)};
        const auto rCv5 = normalizeTrackToPa5xReference(sus4, kAcc1Ch, uasf::TrackRole::Chord, casm, ChordType::Sus4);
        TEST("Mission4b P0-1: CV5 (Sus4) preserves 4th (F=65) untouched",
             rCv5.quality_remapped == 0 && rCv5.notes.size() == 3 &&
             rCv5.notes[0].pitch == 60 && rCv5.notes[1].pitch == 65 && rCv5.notes[2].pitch == 67);

        // P0-1d: CV6 (Power) preserves {0, 7} (C/G) and normalizes Major 3rd onto {0, 7} without duplicates
        casm.source_chord_type = 0x1F; // 1+5 Power
        const std::vector<NoteSpan> pwr = {mkNote(0, 480, 60), mkNote(0, 480, 67), mkNote(0, 480, 72)};
        const auto rCv6 = normalizeTrackToPa5xReference(pwr, kAcc1Ch, uasf::TrackRole::Chord, casm, ChordType::Power);
        TEST("Mission4b P0-1: CV6 (Power) preserves {0, 7} (60,67,72) untouched",
             rCv6.quality_remapped == 0 && rCv6.notes.size() == 3 &&
             rCv6.notes[0].pitch == 60 && rCv6.notes[1].pitch == 67 && rCv6.notes[2].pitch == 72);

        casm.source_chord_type = 0x00; // C Major triad normalized into Power CV6 -> only {0, 7} pitch classes
        const std::vector<NoteSpan> majToPwr = {mkNote(0, 480, 60), mkNote(0, 480, 64), mkNote(0, 480, 67)};
        const auto rCv6Maj = normalizeTrackToPa5xReference(majToPwr, kAcc1Ch, uasf::TrackRole::Chord, casm, ChordType::Power);
        bool allPwrPcs = !rCv6Maj.notes.empty();
        for (const auto& s : rCv6Maj.notes) {
            if ((s.pitch % 12) != 0 && (s.pitch % 12) != 7) allPwrPcs = false;
        }
        TEST("Mission4b P0-1: Major triad normalized to CV6 (Power) yields only {0, 7} pitch classes",
             rCv6Maj.quality_remapped == 1 && allPwrPcs);

        // P1-1: SFF2 Guitar Stroke (NTR=2, NTT=1) in Maj7 is NOT treated as Melody -> snaps B (pc 11) in CV1!
        CasmTrackView casmGtrStroke{};
        casmGtrStroke.has_casm = true;
        casmGtrStroke.ntr = 2;
        casmGtrStroke.ntt = 1; // Stroke (NOT Melody!)
        casmGtrStroke.source_chord_root = 0;
        casmGtrStroke.source_chord_type = 0x02; // CMaj7
        const std::vector<NoteSpan> gtrMaj7 = {
            mkNote(0, 480, 48), mkNote(28, 480, 52), mkNote(56, 480, 55), mkNote(84, 480, 59)
        };
        const auto rGtrMaj7 = normalizeTrackToPa5xReference(
            gtrMaj7, kAcc1Ch + 1, uasf::TrackRole::Chord, casmGtrStroke, ChordType::Major);
        TEST("Mission4b P1-1: SFF2 Guitar Stroke (NTR=2, NTT=1) snaps Maj7 B(59) -> C(60) in CV1",
             rGtrMaj7.quality_remapped == 1 && rGtrMaj7.notes.size() == 4 &&
             rGtrMaj7.notes[3].pitch == 60);

        // Non-C Fmaj7 Guitar Stroke (root=5, type=0x02): E(64) -> shifted -5 to B(59) -> snapped to C(60)!
        casmGtrStroke.source_chord_root = 5;
        const std::vector<NoteSpan> fMaj7Gtr = {
            mkNote(0, 480, 53), mkNote(28, 480, 57), mkNote(56, 480, 60), mkNote(84, 480, 64)
        };
        const auto rFMaj7Gtr = normalizeTrackToPa5xReference(
            fMaj7Gtr, kAcc1Ch + 1, uasf::TrackRole::Chord, casmGtrStroke, ChordType::Major);
        TEST("Mission4b P1-1: Non-C Fmaj7 Guitar Stroke (NTR=2, NTT=1) transposes & snaps Maj7 -> C(60)",
             rFMaj7Gtr.normalized_from_non_c && rFMaj7Gtr.quality_remapped == 1 &&
             rFMaj7Gtr.notes[3].pitch == 60);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
