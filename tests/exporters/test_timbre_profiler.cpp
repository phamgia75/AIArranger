// Track B Q4a unit tests for standalone Yamaha multi-signal timbre profiler &
// per-element voice-change extractor. 100% synthetic fixtures, zero private data.

// Dual-include order verification (Order A: timbre_profiler.h -> pa5x_sound_matcher.h).
#include "exporters/korg/timbre_profiler.h"
#include "exporters/korg/pa5x_sound_matcher.h"
#include "exporters/korg/cv_splitter.h"
#include "exporters/korg/megavoice_sanitizer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;
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

NoteSpan makeNote(uint32_t on, uint32_t off, uint8_t pitch, uint8_t vel = 85) {
    NoteSpan n{};
    n.on_tick  = on;
    n.off_tick = off;
    n.pitch    = pitch;
    n.vel      = vel;
    return n;
}

TrackDefinition makeVoiceTrack(uint8_t ch, const char* name, TrackRole role,
                               int msb, int lsb, int pc,
                               const std::vector<std::pair<uint32_t, uint8_t>>& notes) {
    TrackDefinition tr{};
    tr.midi_channel = ch;
    tr.name = name;
    tr.role = role;
    tr.is_drum = (role == TrackRole::Drum || role == TrackRole::Percussion);
    if (msb >= 0) tr.events.push_back({MidiEventType::ControlChange, ch, 0, static_cast<uint8_t>(msb), 0});
    if (lsb >= 0) tr.events.push_back({MidiEventType::ControlChange, ch, 32, static_cast<uint8_t>(lsb), 0});
    if (pc >= 0)  tr.events.push_back({MidiEventType::ProgramChange, ch, static_cast<uint8_t>(pc), 0, 0});
    for (const auto& [tick, pitch] : notes) {
        tr.events.push_back({MidiEventType::NoteOn, ch, pitch, 90, tick});
        tr.events.push_back({MidiEventType::NoteOff, ch, pitch, 0, tick + 240});
    }
    return tr;
}

} // namespace

int main() {
    std::printf("Test: Track B Q4a Timbre Profiler\n");

    // ── 1. All 22 TimbreFamily enumerators have non-empty names ──
    {
        bool all_named = true;
        for (uint8_t i = 0; i < kTimbreFamilyCount; ++i) {
            const auto f = static_cast<TimbreFamily>(i);
            const char* nm = timbreFamilyName(f);
            if (!nm || nm[0] == '\0') all_named = false;
        }
        TEST("all 22 TimbreFamily values have non-empty names", all_named && kTimbreFamilyCount == 22);
        TEST("percussive family predicate matches DrumKit & PercussionKit only",
             isPercussiveTimbreFamily(TimbreFamily::DrumKit) &&
             isPercussiveTimbreFamily(TimbreFamily::PercussionKit) &&
             !isPercussiveTimbreFamily(TimbreFamily::MalletPerc) &&
             !isPercussiveTimbreFamily(TimbreFamily::Piano));
    }

    // ── 2. Yamaha Drum & Percussion Banks (127/0, 62/100, 126/0) ──
    {
        const auto xg_std   = profileTrackTimbre(127, 0, 0,   "Rhythm2", TrackRole::Drum, {}, {}, 9, kDrumCh);
        const auto xg_room  = profileTrackTimbre(127, 0, 8,   "Rhythm2", TrackRole::Drum, {}, {}, 9, kDrumCh);
        const auto xg_rock  = profileTrackTimbre(127, 0, 16,  "Rhythm2", TrackRole::Drum, {}, {}, 9, kDrumCh);
        const auto xg_elec  = profileTrackTimbre(127, 0, 24,  "Rhythm2", TrackRole::Drum, {}, {}, 9, kDrumCh);
        const auto xg_jazz  = profileTrackTimbre(127, 0, 32,  "Rhythm2", TrackRole::Drum, {}, {}, 9, kDrumCh);
        const auto xg_brush = profileTrackTimbre(127, 0, 40,  "Rhythm2", TrackRole::Drum, {}, {}, 9, kDrumCh);
        const auto xg_orch  = profileTrackTimbre(127, 0, 48,  "Rhythm1", TrackRole::Percussion, {}, {}, 8, kPercCh);
        const auto xg_ethn  = profileTrackTimbre(127, 0, 112, "Rhythm2", TrackRole::Drum, {}, {}, 9, kDrumCh);

        TEST("XG 127/0 PC 0 -> Standard DrumKit",
             xg_std.inferred.family == TimbreFamily::DrumKit &&
             xg_std.inferred.sub_family.find("Standard") != std::string::npos);
        TEST("XG 127/0 PC 8 -> Room DrumKit",
             xg_room.inferred.family == TimbreFamily::DrumKit &&
             xg_room.inferred.sub_family.find("Room") != std::string::npos);
        TEST("XG 127/0 PC 16 -> Rock DrumKit",
             xg_rock.inferred.family == TimbreFamily::DrumKit &&
             xg_rock.inferred.sub_family.find("Rock") != std::string::npos);
        TEST("XG 127/0 PC 24 -> Electronic DrumKit",
             xg_elec.inferred.family == TimbreFamily::DrumKit &&
             xg_elec.inferred.sub_family.find("Electronic") != std::string::npos);
        TEST("XG 127/0 PC 32 -> Jazz DrumKit",
             xg_jazz.inferred.family == TimbreFamily::DrumKit &&
             xg_jazz.inferred.sub_family.find("Jazz") != std::string::npos);
        TEST("XG 127/0 PC 40 -> Brush DrumKit",
             xg_brush.inferred.family == TimbreFamily::DrumKit &&
             xg_brush.inferred.sub_family.find("Brush") != std::string::npos);
        TEST("XG 127/0 PC 48 on Perc -> PercussionKit Orchestra/Latin",
             xg_orch.inferred.family == TimbreFamily::PercussionKit &&
             xg_orch.inferred.sub_family.find("Latin") != std::string::npos);
        TEST("XG 127/0 PC 112 -> Ethnic DrumKit",
             xg_ethn.inferred.family == TimbreFamily::DrumKit &&
             xg_ethn.inferred.sub_family.find("Ethnic") != std::string::npos);

        const auto revo_pop = profileTrackTimbre(62, 100, 1, "Rhythm2", TrackRole::Drum, {}, {}, 9, kDrumCh);
        const auto revo_brush = profileTrackTimbre(62, 100, 40, "BrushKit", TrackRole::Drum, {}, {}, 9, kDrumCh);
        const auto revo_perc = profileTrackTimbre(62, 100, 0, "Rhythm1", TrackRole::Percussion, {}, {}, 8, kPercCh);
        TEST("Genos Revo! 62/100 -> DrumKit + prefers_dnc",
             revo_pop.inferred.family == TimbreFamily::DrumKit && revo_pop.inferred.prefers_dnc &&
             revo_pop.confidence >= 90);
        TEST("Genos Revo! 62/100 Brush -> Brush sub-family + prefers_dnc",
             revo_brush.inferred.family == TimbreFamily::DrumKit && revo_brush.inferred.prefers_dnc &&
             revo_brush.inferred.sub_family.find("Brush") != std::string::npos);
        TEST("Genos Revo! 62/100 on Rhythm1 Perc -> PercussionKit + prefers_dnc",
             revo_perc.inferred.family == TimbreFamily::PercussionKit && revo_perc.inferred.prefers_dnc);

        const auto sfx_kit = profileTrackTimbre(126, 0, 0, "SFX", TrackRole::Percussion, {}, {}, 8, kPercCh);
        const auto orient_kit = profileTrackTimbre(126, 0, 24, "Rhythm1", TrackRole::Percussion, {}, {}, 8, kPercCh);
        TEST("Yamaha 126/0 PC 0 -> PercussionKit SFX",
             sfx_kit.inferred.family == TimbreFamily::PercussionKit &&
             sfx_kit.inferred.sub_family.find("SFX") != std::string::npos);
        TEST("Yamaha 126/0 PC 24 -> PercussionKit Ethnic/Oriental",
             orient_kit.inferred.family == TimbreFamily::PercussionKit &&
             orient_kit.inferred.sub_family.find("Ethnic") != std::string::npos);
    }

    // ── 3. MegaVoice Guitars (63/120), MegaVoice Basses (63/60), Bank 8/60 ──
    {
        const auto mv_nylon = profileTrackTimbre(63, 120, 4,  "Chord1", TrackRole::Chord, {}, {}, 11, 11);
        const auto mv_steel = profileTrackTimbre(63, 120, 21, "Chord1", TrackRole::Chord, {}, {}, 11, 11);
        const auto mv_clean = profileTrackTimbre(63, 120, 27, "Chord2", TrackRole::Chord, {}, {}, 12, 12);
        const auto mv_dist  = profileTrackTimbre(63, 120, 48, "Part14", TrackRole::Chord, {}, {}, 15, 15);
        const auto mv_name_el = profileTrackTimbre(63, 120, 10, "Strat Mega", TrackRole::Chord, {}, {}, 11, 11);

        TEST("MegaVoice 63/120 PC 4 -> Nylon AcousticGuitar + prefers_dnc",
             mv_nylon.inferred.family == TimbreFamily::AcousticGuitar && mv_nylon.inferred.prefers_dnc &&
             mv_nylon.inferred.sub_family.find("Nylon") != std::string::npos);
        TEST("MegaVoice 63/120 PC 21 (generic Chord1) -> Steel AcousticGuitar + prefers_dnc",
             mv_steel.inferred.family == TimbreFamily::AcousticGuitar && mv_steel.inferred.prefers_dnc &&
             mv_steel.inferred.sub_family.find("Steel") != std::string::npos);
        TEST("MegaVoice 63/120 PC 27 -> Clean ElectricGuitar + prefers_dnc",
             mv_clean.inferred.family == TimbreFamily::ElectricGuitar && mv_clean.inferred.prefers_dnc);
        TEST("MegaVoice 63/120 PC 48 -> Overdrive/Distortion ElectricGuitar + prefers_dnc",
             mv_dist.inferred.family == TimbreFamily::ElectricGuitar && mv_dist.inferred.prefers_dnc);
        TEST("MegaVoice 63/120 explicit 'Strat' name overrides to ElectricGuitar",
             mv_name_el.inferred.family == TimbreFamily::ElectricGuitar && mv_name_el.inferred.prefers_dnc);

        const auto mv_acbs = profileTrackTimbre(63, 60, 0,  "Bass", TrackRole::Bass, {}, {}, 10, kBassCh);
        const auto mv_elbs = profileTrackTimbre(63, 60, 23, "Bass", TrackRole::Bass, {}, {}, 10, kBassCh);
        const auto mv_synbs = profileTrackTimbre(63, 60, 38, "Bass", TrackRole::Bass, {}, {}, 10, kBassCh);
        TEST("MegaVoice 63/60 PC 0 -> AcousticBass + prefers_dnc",
             mv_acbs.inferred.family == TimbreFamily::AcousticBass && mv_acbs.inferred.prefers_dnc);
        TEST("MegaVoice 63/60 PC 23 -> ElectricBass + prefers_dnc",
             mv_elbs.inferred.family == TimbreFamily::ElectricBass && mv_elbs.inferred.prefers_dnc);
        TEST("MegaVoice 63/60 PC 38 -> SynthBass + prefers_dnc",
             mv_synbs.inferred.family == TimbreFamily::SynthBass && mv_synbs.inferred.prefers_dnc);
    }

    // ── 4. Full GM/XG/Panel/S.Art/Live-Cool-Sweet PC 0..127 Sweep (msb ∈ {0, 8, 104}) ──
    {
        auto expectGm = [](int pc) -> TimbreFamily {
            if (pc <= 3 || pc == 6) return TimbreFamily::Piano;
            if (pc == 4 || pc == 5 || pc == 7) return TimbreFamily::ElectricPiano;
            if (pc <= 15) return TimbreFamily::MalletPerc;
            if (pc <= 20) return TimbreFamily::Organ;
            if (pc <= 23) return TimbreFamily::Accordion;
            if (pc <= 25) return TimbreFamily::AcousticGuitar;
            if (pc <= 31) return TimbreFamily::ElectricGuitar;
            if (pc == 32 || pc == 43) return TimbreFamily::AcousticBass;
            if (pc <= 37) return TimbreFamily::ElectricBass;
            if (pc <= 39) return TimbreFamily::SynthBass;
            if (pc == 46) return TimbreFamily::PluckedStrings;
            if (pc == 47) return TimbreFamily::MalletPerc;
            if (pc <= 51) return TimbreFamily::Strings;
            if (pc <= 55) return TimbreFamily::ChoirPad;
            if (pc <= 63) return TimbreFamily::Brass;
            if (pc <= 71) return TimbreFamily::SaxReed;
            if (pc <= 79) return TimbreFamily::FlutePipe;
            if (pc <= 87) return TimbreFamily::SynthLead;
            if (pc == 91) return TimbreFamily::ChoirPad;
            if (pc <= 103) return TimbreFamily::SynthPad;
            if (pc <= 111) return TimbreFamily::EthnicMelodic;
            if (pc <= 114) return TimbreFamily::MalletPerc;
            if (pc <= 119) return TimbreFamily::PercussionKit;
            return TimbreFamily::Unknown;
        };

        bool all_gm_ok = true;
        for (int pc = 0; pc <= 127; ++pc) {
            const auto p0   = profileTrackTimbre(0, 0, pc, "Chord1", TrackRole::Chord, {}, {}, 11, 11);
            const auto p8   = profileTrackTimbre(8, 1, pc, "Chord2", TrackRole::Chord, {}, {}, 12, 12);
            const auto p104 = profileTrackTimbre(104, 7, pc, "Phrase1", TrackRole::Phrase1, {}, {}, 14, 14);
            const auto want = expectGm(pc);
            if (p0.inferred.family != want || p8.inferred.family != want || p104.inferred.family != want) {
                std::fprintf(stderr, "    GM PC %d mismatch: got %d/%d/%d want %d\n",
                             pc, int(p0.inferred.family), int(p8.inferred.family), int(p104.inferred.family), int(want));
                all_gm_ok = false;
                break;
            }
        }
        TEST("full GM/XG/Panel/Live-Cool-Sweet PC 0..127 maps every range across msb 0, 8, 104", all_gm_ok);

        const auto sart_tpt = profileTrackTimbre(8, 64, 56, "Phrase2", TrackRole::Phrase2, {}, {}, 15, 15);
        const auto sart2_sax = profileTrackTimbre(8, 120, 66, "Phrase1", TrackRole::Phrase1, {}, {}, 14, 14);
        const auto panel_pno = profileTrackTimbre(8, 1, 0, "Chord1", TrackRole::Chord, {}, {}, 11, 11);
        TEST("S.Art (msb=8, lsb=64) Trumpet sets Brass + prefers_dnc=true",
             sart_tpt.inferred.family == TimbreFamily::Brass && sart_tpt.inferred.prefers_dnc);
        TEST("S.Art2 (msb=8, lsb=120) Tenor Sax sets SaxReed + prefers_dnc=true",
             sart2_sax.inferred.family == TimbreFamily::SaxReed && sart2_sax.inferred.prefers_dnc);
        TEST("Panel (msb=8, lsb=1) Piano keeps prefers_dnc=false",
             panel_pno.inferred.family == TimbreFamily::Piano && !panel_pno.inferred.prefers_dnc);

        // Panel Bass role guard (e.g. 8/37/17 on Bass track)
        const auto panel_bass = profileTrackTimbre(8, 37, 17, "Bass", TrackRole::Bass, {}, {}, 10, kBassCh);
        TEST("Yamaha Panel 8/37/17 on Bass role resolves to ElectricBass (not Organ)",
             panel_bass.inferred.family == TimbreFamily::ElectricBass);

        // Generic track names rescued by Bank/PC
        const auto rescue_ep   = profileTrackTimbre(104, 7, 4,   "Chord1",  TrackRole::Chord,   {}, {}, 11, 11);
        const auto rescue_pad  = profileTrackTimbre(104, 11, 89, "Phrase1", TrackRole::Phrase1, {}, {}, 14, 14);
        const auto rescue_brs  = profileTrackTimbre(104, 5, 61,  "Phrase2", TrackRole::Phrase2, {}, {}, 15, 15);
        const auto rescue_lead = profileTrackTimbre(104, 9, 81,  "Phrase2", TrackRole::Phrase2, {}, {}, 15, 15);
        TEST("Bank/PC rescues generic Chord1/Phrase1/Phrase2 (EP, WarmPad, Brass, SynthLead)",
             rescue_ep.inferred.family == TimbreFamily::ElectricPiano &&
             rescue_pad.inferred.family == TimbreFamily::SynthPad &&
             rescue_brs.inferred.family == TimbreFamily::Brass &&
             rescue_lead.inferred.family == TimbreFamily::SynthLead);
    }

    // ── 5. Expanded Multi-Lingual / Yamaha Abbreviation Keywords + Substring Trap Guards ──
    {
        auto kwProf = [](const char* name, TrackRole role = TrackRole::Chord, uint8_t kch = 11) {
            return profileTrackTimbre(-1, -1, -1, name, role, {}, {}, kch, kch);
        };

        TEST("keyword: 'Pno' -> Piano", kwProf("Pno").inferred.family == TimbreFamily::Piano);
        TEST("keyword: 'Klavier' (DE) -> Piano", kwProf("Klavier").inferred.family == TimbreFamily::Piano);
        TEST("keyword: 'E.P rt' -> ElectricPiano", kwProf("E.P rt").inferred.family == TimbreFamily::ElectricPiano);
        TEST("keyword: 'Rhodes' -> ElectricPiano Rhodes",
             kwProf("Rhodes").inferred.family == TimbreFamily::ElectricPiano &&
             kwProf("Rhodes").inferred.sub_family.find("Rhodes") != std::string::npos);
        TEST("keyword: 'Wurlitzer' -> ElectricPiano Wurlitzer",
             kwProf("Wurlitzer").inferred.family == TimbreFamily::ElectricPiano &&
             kwProf("Wurlitzer").inferred.sub_family.find("Wurlitzer") != std::string::npos);
        TEST("keyword: 'DX7' -> ElectricPiano", kwProf("DX7").inferred.family == TimbreFamily::ElectricPiano);
        TEST("keyword: 'Clav' -> ElectricPiano Clavinet",
             kwProf("Clav").inferred.family == TimbreFamily::ElectricPiano &&
             kwProf("Clav").inferred.sub_family.find("Clav") != std::string::npos);
        TEST("keyword: 'B3 Rotary' -> Organ", kwProf("B3 Rotary").inferred.family == TimbreFamily::Organ);
        TEST("keyword: 'Akkordeon' / 'Musette' -> Accordion",
             kwProf("Akkordeon").inferred.family == TimbreFamily::Accordion &&
             kwProf("Musette").inferred.family == TimbreFamily::Accordion);
        TEST("keyword: 'AcGt' / 'Nyl' / 'Stl' -> AcousticGuitar",
             kwProf("AcGt").inferred.family == TimbreFamily::AcousticGuitar &&
             kwProf("Nyl").inferred.family == TimbreFamily::AcousticGuitar &&
             kwProf("Stl").inferred.family == TimbreFamily::AcousticGuitar);
        TEST("keyword: 'ClnGt' / 'Strat' / 'DistGt' -> ElectricGuitar",
             kwProf("ClnGt").inferred.family == TimbreFamily::ElectricGuitar &&
             kwProf("Strat").inferred.family == TimbreFamily::ElectricGuitar &&
             kwProf("DistGt").inferred.family == TimbreFamily::ElectricGuitar);
        TEST("keyword: 'FingBs' / 'PickBs' / 'SlapBs' -> ElectricBass",
             kwProf("FingBs").inferred.family == TimbreFamily::ElectricBass &&
             kwProf("PickBs").inferred.family == TimbreFamily::ElectricBass &&
             kwProf("SlapBs").inferred.family == TimbreFamily::ElectricBass);
        TEST("keyword: 'AcBs' -> AcousticBass", kwProf("AcBs").inferred.family == TimbreFamily::AcousticBass);
        TEST("keyword: 'SynBs' -> SynthBass", kwProf("SynBs").inferred.family == TimbreFamily::SynthBass);
        TEST("keyword: 'Tbn Bs' -> Brass (Trombone wins over 'bs'!)",
             kwProf("Tbn Bs").inferred.family == TimbreFamily::Brass);
        TEST("keyword: 'Trp' / 'Tpt' / 'Horn' -> Brass",
             kwProf("Trp").inferred.family == TimbreFamily::Brass &&
             kwProf("Tpt").inferred.family == TimbreFamily::Brass &&
             kwProf("Horn").inferred.family == TimbreFamily::Brass);
        TEST("keyword: 'Sax' / 'Oboe' / 'Cl' -> SaxReed",
             kwProf("Sax").inferred.family == TimbreFamily::SaxReed &&
             kwProf("Oboe").inferred.family == TimbreFamily::SaxReed &&
             kwProf("Cl").inferred.family == TimbreFamily::SaxReed);
        TEST("keyword: 'Fl' / 'PanFlute' -> FlutePipe",
             kwProf("Fl").inferred.family == TimbreFamily::FlutePipe &&
             kwProf("PanFlute").inferred.family == TimbreFamily::FlutePipe);
        TEST("keyword: 'Str' / 'Orch' / 'Vln' / 'Cel' / 'Streicher' -> Strings",
             kwProf("Str").inferred.family == TimbreFamily::Strings &&
             kwProf("Orch").inferred.family == TimbreFamily::Strings &&
             kwProf("Vln").inferred.family == TimbreFamily::Strings &&
             kwProf("Cel").inferred.family == TimbreFamily::Strings &&
             kwProf("Streicher").inferred.family == TimbreFamily::Strings);
        TEST("keyword: 'Choir' / 'Vox' -> ChoirPad",
             kwProf("Choir").inferred.family == TimbreFamily::ChoirPad &&
             kwProf("Vox").inferred.family == TimbreFamily::ChoirPad);
        TEST("keyword: 'Warm' / 'OB Pad' -> SynthPad",
             kwProf("Warm").inferred.family == TimbreFamily::SynthPad &&
             kwProf("OB Pad").inferred.family == TimbreFamily::SynthPad);
        TEST("keyword: 'Lead' / 'Arp' / 'Syn' -> SynthLead",
             kwProf("Lead").inferred.family == TimbreFamily::SynthLead &&
             kwProf("Arp").inferred.family == TimbreFamily::SynthLead &&
             kwProf("Syn").inferred.family == TimbreFamily::SynthLead);
        TEST("keyword: 'Vibes' / 'Marimba' -> MalletPerc",
             kwProf("Vibes").inferred.family == TimbreFamily::MalletPerc &&
             kwProf("Marimba").inferred.family == TimbreFamily::MalletPerc);
        TEST("keyword: 'Sitar' / 'Oud' -> EthnicMelodic",
             kwProf("Sitar").inferred.family == TimbreFamily::EthnicMelodic &&
             kwProf("Oud").inferred.family == TimbreFamily::EthnicMelodic);

        // ── CRITICAL SUBSTRING TRAP GUARDS ──
        const auto trap_bottom = kwProf("Bottom");
        const auto trap_bottom_end = kwProf("BottomEnd");
        const auto trap_harpsi = kwProf("Harpsichord");
        const auto trap_harpsi_short = kwProf("Harpsi");
        const auto true_harp = kwProf("Harp");
        const auto generic_acc1 = kwProf("Acc1");
        const auto generic_chord1 = kwProf("Chord1");
        const auto generic_phrase1 = kwProf("Phrase1");

        TEST("substring guard: 'Bottom' & 'BottomEnd' do NOT falsely trigger drum/tom",
             trap_bottom.inferred.family == TimbreFamily::Unknown &&
             trap_bottom_end.inferred.family == TimbreFamily::Unknown);
        TEST("substring guard: 'Harpsichord' & 'Harpsi' map to Piano (NOT Harp/PluckedStrings!)",
             trap_harpsi.inferred.family == TimbreFamily::Piano &&
             trap_harpsi.inferred.sub_family == "Harpsichord" &&
             trap_harpsi_short.inferred.family == TimbreFamily::Piano &&
             trap_harpsi_short.inferred.sub_family == "Harpsichord");
        TEST("substring guard: 'Harp' still maps cleanly to PluckedStrings",
             true_harp.inferred.family == TimbreFamily::PluckedStrings &&
             true_harp.inferred.sub_family == "Orchestral Harp");
        TEST("substring guard: 'Acc1', 'Chord1', 'Phrase1' do NOT falsely trigger Accordion/keywords",
             generic_acc1.inferred.family == TimbreFamily::Unknown &&
             generic_chord1.inferred.family == TimbreFamily::Unknown &&
             generic_phrase1.inferred.family == TimbreFamily::Unknown);
    }

    // ── 6. Musical Signal Disambiguator (no Bank/PC, generic track names) ──
    {
        // Monophonic low notes (contiguous legato: off_tick == next on_tick -> polyphony MUST be 1!)
        std::vector<NoteSpan> bass_notes = {
            makeNote(0, 480, 36), makeNote(480, 960, 40),
            makeNote(960, 1440, 43), makeNote(1440, 1920, 38)
        };
        const auto sig_bass = profileTrackTimbre(-1, -1, -1, "Phrase1", TrackRole::Accompaniment, bass_notes, {}, 14, 14);
        TEST("signal disambiguator: mono low legato notes -> ElectricBass, poly=1, median<=48",
             sig_bass.inferred.family == TimbreFamily::ElectricBass &&
             sig_bass.max_polyphony == 1 &&
             sig_bass.pitch_min == 36 && sig_bass.pitch_max == 43 && sig_bass.pitch_median <= 48 &&
             std::fabs(sig_bass.mean_duration_beats - 1.0) < 0.01 &&
             std::fabs(sig_bass.notes_per_bar - 4.0) < 0.01);

        // Sustained 4-voice chord (>= 1.8 beats) -> Strings (or SynthPad on Pad role)
        std::vector<NoteSpan> sus_notes = {
            makeNote(0, 1920, 60), makeNote(0, 1920, 64),
            makeNote(0, 1920, 67), makeNote(0, 1920, 72)
        };
        const auto sig_str = profileTrackTimbre(-1, -1, -1, "Chord1", TrackRole::Chord, sus_notes, {}, 11, 11);
        const auto sig_pad = profileTrackTimbre(-1, -1, -1, "Chord2", TrackRole::Pad,   sus_notes, {}, 13, 13);
        TEST("signal disambiguator: sustained 4-voice chord -> Strings (Chord) / SynthPad (Pad)",
             sig_str.inferred.family == TimbreFamily::Strings && sig_str.max_polyphony == 4 &&
             sig_str.mean_duration_beats >= 3.9 &&
             sig_pad.inferred.family == TimbreFamily::SynthPad && sig_pad.max_polyphony == 4);

        // Short staccato simultaneous 3-voice chords -> Piano
        std::vector<NoteSpan> stacc_notes = {
            makeNote(0, 240, 60), makeNote(0, 240, 64), makeNote(0, 240, 67),
            makeNote(480, 720, 60), makeNote(480, 720, 64), makeNote(480, 720, 67)
        };
        const auto sig_pno = profileTrackTimbre(-1, -1, -1, "Chord1", TrackRole::Chord, stacc_notes, {}, 11, 11);
        TEST("signal disambiguator: short simultaneous 3-voice comp -> Piano",
             sig_pno.inferred.family == TimbreFamily::Piano &&
             sig_pno.max_polyphony == 3 && sig_pno.mean_duration_beats < 0.75 && !sig_pno.has_strum_offsets);

        // Short staggered monotonic 4-string strum (~15 ms = ~14 ticks at 480 PPQN / 120 BPM) -> AcousticGuitar
        std::vector<NoteSpan> strum_notes = {
            makeNote(0,  220, 48), makeNote(14, 220, 55), makeNote(28, 220, 60), makeNote(42, 220, 64),
            makeNote(480, 700, 48), makeNote(494, 700, 55), makeNote(508, 700, 60), makeNote(522, 700, 64)
        };
        const auto sig_gtr = profileTrackTimbre(-1, -1, -1, "Chord1", TrackRole::Chord, strum_notes, {}, 11, 11);
        TEST("signal disambiguator: short staggered strum -> AcousticGuitar with has_strum_offsets=true",
             sig_gtr.inferred.family == TimbreFamily::AcousticGuitar &&
             sig_gtr.max_polyphony == 4 && sig_gtr.has_strum_offsets);
    }

    // ── 7. Per-Element Mid-Style Voice Change Extraction (`profileStyleTracks`) ──
    {
        StyleDefinition style{};
        style.name = "SYNTH_VOICE_SWITCH";
        style.tempo_bpm = 110;
        style.resolution = 480;

        auto makeSec = [&](SectionType st, const char* nm, int msb, int lsb, int pc) {
            SectionDefinition sec{};
            sec.type = st;
            sec.name = nm;
            sec.bars = 2;
            sec.resolution = 480;
            sec.beats_per_bar = 4;
            sec.beat_note = 4;
            sec.tracks.push_back(makeVoiceTrack(12, "Chord2", TrackRole::Chord, msb, lsb, pc,
                                                {{0, 60}, {480, 64}, {960, 67}}));
            return sec;
        };

        // Intro1 = Acoustic Piano (0/0/0), Main1 & Main2 = Rhodes EP (104/7/4), Main4 = Brass (104/5/61)
        style.sections.push_back(makeSec(SectionType::Intro1, "Intro A", 0,   0,  0));
        style.sections.push_back(makeSec(SectionType::Main1,  "Main A",  104, 7,  4));
        style.sections.push_back(makeSec(SectionType::Main2,  "Main B",  104, 7,  4));
        style.sections.push_back(makeSec(SectionType::Main4,  "Main D",  104, 5, 61));

        const auto profiles = profileStyleTracks(style);
        TEST("profileStyleTracks captures 1 track profile", profiles.size() == 1);
        if (!profiles.empty()) {
            const auto& p = profiles[0];
            TEST("dominant voice is most-frequent Rhodes EP (104/7/4)",
                 p.yamaha_msb == 104 && p.yamaha_lsb == 7 && p.yamaha_pc == 4 &&
                 p.inferred.family == TimbreFamily::ElectricPiano);
            TEST("element_yamaha_voices preserves all 4 section voices including mid-style switches",
                 p.element_yamaha_voices.size() == 4 &&
                 p.element_yamaha_voices.at(SectionType::Intro1) == std::make_tuple(0, 0, 0) &&
                 p.element_yamaha_voices.at(SectionType::Main1)  == std::make_tuple(104, 7, 4) &&
                 p.element_yamaha_voices.at(SectionType::Main4)  == std::make_tuple(104, 5, 61));
        }
    }

    // ── 8. Malformed / Empty Edge Cases ──
    {
        const auto empty_p = profileTrackTimbre(-1, -1, -1, "", TrackRole::Accompaniment, {});
        TEST("empty input returns Unknown with note_count=0, max_polyphony=0",
             empty_p.inferred.family == TimbreFamily::Unknown &&
             empty_p.note_count == 0 && empty_p.max_polyphony == 0 && empty_p.confidence == 0);

        // Out-of-range Bank/PC and zero/inverted NoteSpan + ppqn=0/tempo=0
        std::vector<NoteSpan> weird_notes = {
            makeNote(100, 100, 200, 0),
            makeNote(200, 50, 40, 127)
        };
        TimbreProfileOptions bad_opt{};
        bad_opt.ppqn = 0;
        bad_opt.tempo_mpqn = 0;
        const auto oob_p = profileTrackTimbre(999, -5, 300, "!!!", TrackRole::Accompaniment,
                                              weird_notes, {}, 11, 11, bad_opt);
        TEST("malformed Bank/PC, inverted NoteSpan, ppqn=0 handled safely without UB",
             oob_p.note_count == 2 && oob_p.pitch_max <= 127 && oob_p.max_polyphony >= 1);
    }

    // ── 9. Audit Round 1 Regression Tests (P1-1, P1-2, P2-2) ──
    {
        // P1-1: UBSan div-by-zero, float-cast-overflow, and uint32_t tick wraparound
        {
            TimbreProfileOptions opt_div0{};
            opt_div0.ppqn = 0x40000000u; // 0x40000000u * 4u == 0 in uint32_t!
            opt_div0.bar_ticks = 0;
            const auto p_div0 = profileTrackTimbre(-1, -1, -1, "Chord1", TrackRole::Chord,
                                                   {makeNote(0, 100, 60)}, {}, 11, 11, opt_div0);
            TEST("P1-1: ppqn=0x40000000u, bar_ticks=0 avoids uint32_t div-by-zero",
                 p_div0.note_count == 1 && p_div0.notes_per_bar > 0.0);

            TimbreProfileOptions opt_fco{};
            opt_fco.ppqn = 1000000u;
            opt_fco.tempo_mpqn = 1u; // 60 * 1e9 overflows uint32_t without clamp!
            const auto p_fco = profileTrackTimbre(-1, -1, -1, "Chord1", TrackRole::Chord,
                                                  {makeNote(0, 100, 60), makeNote(10, 110, 64), makeNote(20, 120, 67)},
                                                  {}, 11, 11, opt_fco);
            TEST("P1-1: ppqn=1000000u, tempo_mpqn=1u avoids float-cast-overflow UB",
                 p_fco.note_count == 3 && p_fco.max_polyphony == 3);

            const auto p_max = profileTrackTimbre(-1, -1, -1, "Chord1", TrackRole::Chord,
                                                  {makeNote(UINT32_MAX, UINT32_MAX, 60)}, {}, 11, 11);
            TEST("P1-1: NoteSpan{UINT32_MAX, UINT32_MAX} preserves max_polyphony == 1 without wraparound",
                 p_max.note_count == 1 && p_max.max_polyphony == 1 && p_max.notes_per_bar > 0.0);

            const auto p_near = profileTrackTimbre(-1, -1, -1, "Chord1", TrackRole::Chord,
                                                   {makeNote(0xFFFFFE00u, 0xFFFFFF00u, 60)}, {}, 11, 11);
            TEST("P1-1: NoteSpan{0xFFFFFE00u, 0xFFFFFF00u} computes bars in uint64_t without wraparound",
                 p_near.note_count == 1 && p_near.max_polyphony == 1 && p_near.notes_per_bar > 0.0);
        }

        // P1-2: Keyword precedence fixes
        {
            auto kwProf = [](const char* name, TrackRole role = TrackRole::Chord, uint8_t kch = 11) {
                return profileTrackTimbre(-1, -1, -1, name, role, {}, {}, kch, kch);
            };
            TEST("P1-2: 'Organ_Reed' and 'Reed Organ' classify as Organ (not SaxReed)",
                 kwProf("Organ_Reed").inferred.family == TimbreFamily::Organ &&
                 kwProf("Reed Organ").inferred.family == TimbreFamily::Organ);
            TEST("P1-2: 'SteelDrum' and 'Steel Drums' classify as MalletPerc (not DrumKit)",
                 kwProf("SteelDrum").inferred.family == TimbreFamily::MalletPerc &&
                 kwProf("Steel Drums").inferred.family == TimbreFamily::MalletPerc);
            TEST("P1-2: 'English Horn' classifies as SaxReed (not Brass)",
                 kwProf("English Horn").inferred.family == TimbreFamily::SaxReed &&
                 kwProf("English Horn").inferred.sub_family == "English Horn");
            TEST("P1-2: 'Overdrive_Lead' classifies as SynthLead (not ElectricGuitar)",
                 kwProf("Overdrive_Lead").inferred.family == TimbreFamily::SynthLead);
        }

        // P2-2: Numbered instrument names [A-Za-z]+[0-9]+ emit alphabetic prefix safely
        {
            auto kwProf = [](const char* name) {
                return profileTrackTimbre(-1, -1, -1, name, TrackRole::Chord, {}, {}, 11, 11);
            };
            TEST("P2-2: 'Piano1' -> Piano, 'Organ1' -> Organ, 'Guitar1' -> AcousticGuitar",
                 kwProf("Piano1").inferred.family == TimbreFamily::Piano &&
                 kwProf("Organ1").inferred.family == TimbreFamily::Organ &&
                 kwProf("Guitar1").inferred.family == TimbreFamily::AcousticGuitar);
            TEST("P2-2: 'Chord1', 'Phrase1', 'Acc1', 'Part14' remain generic Unknown",
                 kwProf("Chord1").inferred.family == TimbreFamily::Unknown &&
                 kwProf("Phrase1").inferred.family == TimbreFamily::Unknown &&
                 kwProf("Acc1").inferred.family == TimbreFamily::Unknown &&
                 kwProf("Part14").inferred.family == TimbreFamily::Unknown);
            TEST("P2-2: 'DX7' and 'B3' still resolve to ElectricPiano and Organ",
                 kwProf("DX7").inferred.family == TimbreFamily::ElectricPiano &&
                 kwProf("B3").inferred.family == TimbreFamily::Organ);
        }
    }

    // ── 10. Mission 4b Regression Tests (P0-3 Private Banks, P1-2 Contrabass, P1-6 Generic Keyword Guard, P1-7 Timpani/Brush) ──
    {
        // P0-3: Unrecognized vendor-private banks (EXP15 65/12/45 "Track 1", 66/7/122 "Part 3") cap confidence <= 35!
        const std::vector<NoteSpan> triad = {
            makeNote(0, 480, 60), makeNote(0, 480, 64), makeNote(0, 480, 67)
        };
        const std::vector<NoteSpan> padSustained = {
            makeNote(0, 1920, 48), makeNote(0, 1920, 52), makeNote(0, 1920, 55)
        };
        const auto priv1 = profileTrackTimbre(65, 12, 45, "Track 1", TrackRole::Chord, triad, {}, 11, 11);
        const auto priv2 = profileTrackTimbre(66, 7, 122, "Part 3", TrackRole::Pad, padSustained, {}, 13, 13);
        TEST("Mission4b P0-3: unknown private bank 65/12/45 caps signal-only confidence <= 35",
             priv1.confidence <= 35 && priv1.rationale.find("private-bank-cap") != std::string::npos);
        TEST("Mission4b P0-3: unknown private bank 66/7/122 caps sustained-pad confidence <= 35",
             priv2.confidence <= 35 && priv2.rationale.find("private-bank-cap") != std::string::npos);

        // Verify standard GM (MSB=0) SFX PC 125 even with polyphony stays Unknown (conf=0)
        const auto gmSfxPoly = profileTrackTimbre(0, 0, 125, "FX Ring", TrackRole::Chord, triad, {}, 14, 14);
        TEST("Mission4b P0-3: GM SFX 0/0/125 with polyphonic notes still fails closed (Unknown, conf=0)",
             gmSfxPoly.inferred.family == TimbreFamily::Unknown && gmSfxPoly.confidence == 0);

        // Verify no-bank (-1/-1/-1) Signal-3 still works at normal confidence (>= 60)
        const auto noBankTriad = profileTrackTimbre(-1, -1, -1, "Track 1", TrackRole::Chord, triad, {}, 11, 11);
        TEST("Mission4b P0-3: no-bank (-1/-1/-1) signal heuristic retains confidence >= 60",
             noBankTriad.inferred.family == TimbreFamily::Piano && noBankTriad.confidence >= 60);

        // P1-2: GM PC 43 Contrabass -> AcousticBass ("Contrabass")
        const auto cb = profileTrackTimbre(0, 0, 43, "ContraBs", TrackRole::Bass, {}, {}, 10, kBassCh);
        TEST("Mission4b P1-2: GM PC 43 'ContraBs' profiles as AcousticBass (not Strings)",
             cb.inferred.family == TimbreFamily::AcousticBass && cb.confidence >= 90);

        // P1-6: Generic keyword ("Guitar", "Bass", "Piano") must NOT overwrite specific GM PC sub-family!
        const auto steelGtr = profileTrackTimbre(0, 0, 25, "Guitar", TrackRole::Chord, {}, {}, 12, 12);
        const auto slapBass = profileTrackTimbre(0, 0, 36, "Bass", TrackRole::Bass, {}, {}, 10, kBassCh);
        const auto brightPno = profileTrackTimbre(0, 0, 1, "Piano", TrackRole::Chord, {}, {}, 11, 11);
        const auto wurliRefined = profileTrackTimbre(0, 0, 4, "Wurlitzer", TrackRole::Chord, {}, {}, 11, 11);
        TEST("Mission4b P1-6: generic 'Guitar' preserves GM PC 25 'Steel Acoustic Guitar'",
             steelGtr.inferred.family == TimbreFamily::AcousticGuitar &&
             steelGtr.inferred.sub_family == "Steel Acoustic Guitar");
        TEST("Mission4b P1-6: generic 'Bass' preserves GM PC 36 'Slap Bass 1'",
             slapBass.inferred.family == TimbreFamily::ElectricBass &&
             slapBass.inferred.sub_family == "Slap Bass 1");
        TEST("Mission4b P1-6: generic 'Piano' preserves GM PC 1 'Bright Acoustic Piano'",
             brightPno.inferred.family == TimbreFamily::Piano &&
             brightPno.inferred.sub_family == "Bright Acoustic Piano");
        TEST("Mission4b P1-6: specific 'Wurlitzer' still refines GM PC 4 Rhodes -> 'Wurlitzer EP'",
             wurliRefined.inferred.family == TimbreFamily::ElectricPiano &&
             wurliRefined.inferred.sub_family == "Wurlitzer EP");

        // P1-7 helper: Timpani keyword retains "Timpani" sub-family; XG 127/0/40 on kPercCh stays Brush DrumKit
        const auto timp = profileTrackTimbre(0, 0, 47, "Timpani", TrackRole::Accompaniment, {}, {}, 15, 15);
        const auto brushOnPerc = profileTrackTimbre(127, 0, 40, "", TrackRole::Percussion, {}, {}, 9, kPercCh);
        TEST("Mission4b P1-7: 'Timpani' (0/0/47) retains sub_family 'Timpani' (not Vibraphone)",
             timp.inferred.family == TimbreFamily::MalletPerc && timp.inferred.sub_family == "Timpani");
        TEST("Mission4b P1-7: XG 127/0/40 on kPercCh profiles as Brush DrumKit for exact GM2 match",
             brushOnPerc.inferred.family == TimbreFamily::DrumKit &&
             brushOnPerc.inferred.sub_family == "Brush Drum Kit");
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
