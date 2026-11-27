// Canonical (UASF) -> Korg Pa5X semantic model: section mapping, channel
// routing, voice-select policy, chord-event-range filter, boundary clamping,
// exact PPQN rescaling (triplets, 6/8), loss accounting, AND Wave-2B Q23-Wire
// E2E style conversion quality pipeline (CV1..CV6 split, XG/Revo! drum remap &
// Rhythm1/2 swap, MegaVoice/S.Art/Guitar sanitizer, Source-Chord normalizer).

#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/pa5x_sound_matcher.h"
#include "exporters/korg/sff1_route.h"
#include "exporters/korg/sound_map.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;
using uasf::MidiEventType;
using uasf::SectionType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s (line %d)\n", name, __LINE__); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static uasf::MidiEvent ev(MidiEventType t, uint8_t ch, uint8_t d1, uint8_t d2, uint64_t tick) {
    return {t, ch, d1, d2, tick};
}
static uasf::TrackDefinition track(uint8_t ch, std::vector<uasf::MidiEvent> evs, uasf::TrackRole role = uasf::TrackRole::Chord) {
    uasf::TrackDefinition t;
    t.midi_channel = ch;
    t.name = "T" + std::to_string(ch + 1);
    t.role = role;
    t.events = std::move(evs);
    return t;
}
static uasf::SectionDefinition section(SectionType type, std::string name, uint32_t bars,
                                       std::vector<uasf::TrackDefinition> tracks, uint32_t num = 4, uint32_t den = 4) {
    uasf::SectionDefinition s;
    s.type = type; s.name = std::move(name); s.bars = bars;
    s.resolution = 1920; s.beats_per_bar = num; s.beat_note = den;
    s.tracks = std::move(tracks);
    return s;
}
static uasf::StyleDefinition styleOf(std::vector<uasf::SectionDefinition> secs) {
    uasf::StyleDefinition s;
    s.tempo_bpm = 100; s.resolution = 1920; s.sections = std::move(secs);
    return s;
}
static const Element* find(const Style& s, const std::string& m) {
    for (const auto& e : s.elements) if (e.marker == m) return &e;
    return nullptr;
}
static uint32_t count(const Element& e, uint8_t status, int d1 = -1) {
    uint32_t n = 0;
    for (const auto& x : e.events) if (x.status == status && (d1 < 0 || x.d1 == d1)) ++n;
    return n;
}

static std::string fixturePath(const char* name) {
    return std::string(REPO_DIR) + "/fixtures/synthetic/" + name;
}

static bool loadAndMapFixture(const char* name,
                              const MapOptions& optIn,
                              importers::sff1::ParseResult& prOut,
                              importers::sff1::SffToUasfResult& mrOut,
                              MapResult& krOut) {
    prOut = importers::sff1::Sff1Reader().parseFile(fixturePath(name));
    if (!prOut.success) return false;
    mrOut = importers::sff1::Sff1ToUasfMapper().map(prOut);
    if (!mrOut.success) return false;
    MapOptions opt = optIn;
    opt.route = routeFromCasm(prOut);
    krOut = mapToKorg(mrOut.style, opt);
    return krOut.ok;
}

static void runQ23WireE2eTests() {
    std::printf("\n── Wave-2B Q23-Wire E2E Quality Pipeline Tests ──\n");

    // 1. SYNTH_MEGAVOICE_GUITAR_BASS.sty: MegaVoice sanitization + SFF2 Guitar + ArticulationProfile::MegaLike
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult kr;
        MapOptions opt;
        opt.ppqn = 384;
        opt.break_policy = BreakPolicy::Yamaha1BarCap;
        const bool ok = loadAndMapFixture("SYNTH_MEGAVOICE_GUITAR_BASS.sty", opt, pr, mr, kr);
        const auto w = write(kr.style);
        const auto back = parse(w.bytes);
        TEST("Q23 MegaVoice: loads, maps, writes & validates cleanly",
             ok && w.ok && back.ok && validate(back.style).empty());

        // Verify sff1_mapper tagged ArticulationProfile::MegaLike on ch10 (63/60) & ch12 (63/120),
        // and left ch11 (Piano) & ch13 (Pad) as Generic.
        bool megaBassTagged = false, megaGtrTagged = false, nonMegaClean = true;
        for (const auto& sec : mr.style.sections) {
            for (const auto& tr : sec.tracks) {
                if (tr.midi_channel == 10 && tr.articulation.profile == uasf::ArticulationProfile::MegaLike)
                    megaBassTagged = true;
                if (tr.midi_channel == 12 && tr.articulation.profile == uasf::ArticulationProfile::MegaLike)
                    megaGtrTagged = true;
                if ((tr.midi_channel == 11 || tr.midi_channel == 13 || tr.midi_channel == 9) &&
                    tr.articulation.profile == uasf::ArticulationProfile::MegaLike)
                    nonMegaClean = false;
            }
        }
        TEST("Q23 MegaVoice: sff1_mapper tags ArticulationProfile::MegaLike only on MegaVoice tracks",
             megaBassTagged && megaGtrTagged && nonMegaClean);

        // Verify 0 leaked >= C7 FX notes & 0 leaked 0..11 chord-range notes across ALL elements.
        uint32_t leakedFx = 0, leakedChord = 0;
        uint32_t minBassVel = 127, maxBassVel = 0;
        for (const auto& el : back.style.elements) {
            for (const auto& e : el.events) {
                const uint8_t hi = e.status & 0xF0;
                const uint8_t ch = e.status & 0x0F;
                if (hi != 0x90 || e.d2 == 0) continue;
                if (isChordDriven(ch) && e.d1 <= kChordEventNoteMax) ++leakedChord;
                if ((ch == kBassCh || ch == 12) && e.d1 >= 96) ++leakedFx;
                if (ch == kBassCh) {
                    minBassVel = std::min<uint32_t>(minBassVel, e.d2);
                    maxBassVel = std::max<uint32_t>(maxBassVel, e.d2);
                }
            }
        }
        TEST("Q23 MegaVoice: 0 leaked >=C7 FX notes, 0 leaked 0..11 notes, velocities reshaped",
             leakedFx == 0 && leakedChord == 0 &&
             kr.report.megavoice_tracks_sanitized > 0 &&
             kr.report.megavoice_fx_notes_stripped >= 40 &&
             kr.report.megavoice_vel_reshaped > 0 &&
             minBassVel >= 38 && maxBassVel <= 106);

        // Verify SFF2 Guitar (Acc2 / ch12) maintains <= 6-string strum coherence in Main A.
        const Element* v1 = find(back.style, "v1cv1");
        bool guitarOk = false;
        if (v1) {
            uint32_t gtrNotes = 0;
            uint8_t minP = 127, maxP = 0;
            for (const auto& e : v1->events) {
                if (e.status == (0x90 | 12) && e.d2 > 0) {
                    ++gtrNotes;
                    minP = std::min(minP, e.d1);
                    maxP = std::max(maxP, e.d1);
                }
            }
            guitarOk = (gtrNotes > 0 && minP >= 40 && maxP <= 88 && (maxP - minP) <= 24);
        }
        TEST("Q23 MegaVoice: SFF2 Guitar fretboard span <=24 st inside [40..88]", guitarOk);
    }

    // 2. SYNTH_CHORD_MUTE_SPLIT.sty: mutually exclusive channels split cleanly into cv1 + cv2
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult kr;
        MapOptions opt;
        opt.ppqn = 384;
        opt.break_policy = BreakPolicy::Yamaha1BarCap;
        const bool ok = loadAndMapFixture("SYNTH_CHORD_MUTE_SPLIT.sty", opt, pr, mr, kr);
        const auto w = write(kr.style);
        const auto back = parse(w.bytes);
        TEST("Q23 CV-Split: loads, maps, writes & validates cleanly",
             ok && w.ok && back.ok && validate(back.style).empty());

        const Element* v1cv1 = find(back.style, "v1cv1");
        const Element* v1cv2 = find(back.style, "v1cv2");
        const Element* br = find(back.style, "br");
        TEST("Q23 CV-Split: creates v1cv1 & v1cv2, keeps 'br' suffix-free, 0 shadowed drops",
             v1cv1 != nullptr && v1cv2 != nullptr && br != nullptr &&
             !find(back.style, "brcv2") &&
             kr.report.cv_splits_created == 5 &&
             kr.report.shadowed_tracks_dropped == 0);

        // Verify Acc4 (ch14) in v1cv1 has Major 3rd (E4=64) and NO Minor 3rd (Eb4=63),
        // while v1cv2 has Minor 3rd (Eb4=63) and NO Major 3rd (E4=64), AND Drum+Bass are cloned in both!
        const bool cv1PureMaj = v1cv1 && count(*v1cv1, 0x90 | 14, 64) > 0 && count(*v1cv1, 0x90 | 14, 63) == 0 &&
                                count(*v1cv1, 0x90 | kDrumCh) > 0 && count(*v1cv1, 0x90 | kBassCh) > 0;
        const bool cv2PureMin = v1cv2 && count(*v1cv2, 0x90 | 14, 63) > 0 && count(*v1cv2, 0x90 | 14, 64) == 0 &&
                                count(*v1cv2, 0x90 | kDrumCh) > 0 && count(*v1cv2, 0x90 | kBassCh) > 0;
        TEST("Q23 CV-Split: v1cv1=pure Major, v1cv2=pure Minor, shared Drum+Bass cloned in both",
             cv1PureMaj && cv2PureMin);
    }

    // 3. SYNTH_SRC_CHORD_NON_C.sty: non-C / non-Major source chord normalized ONCE to C Major + Pa5xTrackEditAdvice
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult kr;
        MapOptions opt;
        opt.ppqn = 384;
        opt.break_policy = BreakPolicy::Yamaha1BarCap;
        const bool ok = loadAndMapFixture("SYNTH_SRC_CHORD_NON_C.sty", opt, pr, mr, kr);
        const auto w = write(kr.style);
        const auto back = parse(w.bytes);
        TEST("Q23 SourceChord: loads, maps, writes & validates cleanly",
             ok && w.ok && back.ok && validate(back.style).empty());

        const Element* v1 = find(back.style, "v1cv1"); // Main A: Chord1 was Cm7, Pad was G7
        const Element* v2 = find(back.style, "v2cv1"); // Main B: Bass/Chord1/Pad were Dm (root=2, type=8)
        auto hasOnlyCmajTriadPcs = [](const Element* el, uint8_t ch) {
            if (!el) return false;
            uint32_t n = 0;
            for (const auto& e : el->events) {
                if (e.status == (0x90 | ch) && e.d2 > 0) {
                    ++n;
                    const uint8_t pc = static_cast<uint8_t>(e.d1 % 12);
                    if (pc != 0 && pc != 4 && pc != 7) return false;
                }
            }
            return n > 0;
        };
        const bool v1Norm = hasOnlyCmajTriadPcs(v1, 11) && hasOnlyCmajTriadPcs(v1, 13);
        const bool v2Norm = hasOnlyCmajTriadPcs(v2, kBassCh) && hasOnlyCmajTriadPcs(v2, 11) && hasOnlyCmajTriadPcs(v2, 13);
        // Zero double-transposition check on Main B Bass: source D2(38)/A2(45) -> C2(36)/G2(43) (NOT Bb1=34!)
        const bool noDoubleShift = v2 && count(*v2, 0x90 | kBassCh, 36) > 0 && count(*v2, 0x90 | kBassCh, 34) == 0;
        const bool hasAdvice = !kr.report.track_edit_advice.empty() &&
                               std::any_of(kr.report.track_edit_advice.begin(), kr.report.track_edit_advice.end(),
                                           [](const quality::Pa5xTrackEditAdvice& a) {
                                               return a.korg_ch == kBassCh && a.pa5x_ntt_table == "Root" &&
                                                      a.wrap_around >= 1 && a.wrap_around <= 12 &&
                                                      !a.retrigger_mode.empty();
                                           });
        TEST("Q23 SourceChord: Cm7/G7/Dm normalized cleanly to C Maj with 0 double-transposition & valid advice",
             v1Norm && v2Norm && noDoubleShift && kr.report.tracks_source_chord_normalized > 0 && hasAdvice);
    }

    // 4. SYNTH_REVO_XG_DRUMS.sty: Rhythm1/Rhythm2 swap + extended XG/Revo! drum remap + custom IDrumKitKeyMap injection
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult kr;
        MapOptions opt;
        opt.ppqn = 384;
        opt.break_policy = BreakPolicy::Yamaha1BarCap;
        const bool ok = loadAndMapFixture("SYNTH_REVO_XG_DRUMS.sty", opt, pr, mr, kr);
        const auto w = write(kr.style);
        const auto back = parse(w.bytes);
        TEST("Q23 DrumRemap: loads, maps, writes & validates cleanly",
             ok && w.ok && back.ok && validate(back.style).empty());

        const Element* v1 = find(back.style, "v1cv1");
        uint32_t drumCore = 0, percCore = 0, lowUnremapped = 0, highUnremapped = 0;
        if (v1) {
            for (const auto& e : v1->events) {
                const uint8_t hi = e.status & 0xF0;
                const uint8_t ch = e.status & 0x0F;
                if (hi != 0x90 || e.d2 == 0) continue;
                if (ch == kDrumCh) {
                    if (e.d1 == 35 || e.d1 == 36 || e.d1 == 38 || e.d1 == 40 || e.d1 == 42) ++drumCore;
                    if (e.d1 < 24) ++lowUnremapped;
                } else if (ch == kPercCh) {
                    if (e.d1 == 35 || e.d1 == 36 || e.d1 == 38 || e.d1 == 40 || e.d1 == 42) ++percCore;
                    if (e.d1 >= 88) ++highUnremapped;
                }
            }
        }
        TEST("Q23 DrumRemap: Rhythm1/2 swapped (main kit on Drum), low <24 & high >=88 remapped, exact/approx counted",
             kr.report.rhythm_tracks_swapped &&
             drumCore > 0 && drumCore > percCore &&
             lowUnremapped == 0 && highUnremapped == 0 &&
             kr.report.drum_notes_remapped_exact > 0 &&
             kr.report.drum_notes_remapped_approx > 0);

        // Verify caller-injected custom IDrumKitKeyMap* is honored (zero silent substitution):
        // A sparse GM1-only USERDK kit lacking GM2 29/34/82/83/84 forces fallback inside GM1 35..81!
        quality::DrumKitKeySummaryView sparseSummary;
        sparseSummary.kit_name = "TEST_SPARSE_GM1_KIT";
        sparseSummary.gm_alignment_score = 1.0;
        for (uint8_t n : {uint8_t(35), uint8_t(36), uint8_t(37), uint8_t(38), uint8_t(39), uint8_t(40),
                          uint8_t(42), uint8_t(44), uint8_t(46), uint8_t(48), uint8_t(49), uint8_t(51),
                          uint8_t(54), uint8_t(56), uint8_t(64), uint8_t(70), uint8_t(73), uint8_t(76)}) {
            auto& slot = sparseSummary.keys[n];
            slot.populated = true;
            slot.layer_count = 1;
            slot.vel_tops[0] = 127;
            slot.resolved_family = quality::drum_detail::gm2DefaultFamily(n);
            slot.choke_group = quality::drum_detail::gm2DefaultChokeGroup(n);
        }
        quality::UserdkDrumKitKeyMap sparseKit(sparseSummary);
        MapOptions customOpt = opt;
        customOpt.target_drum_key_map = &sparseKit;
        customOpt.target_perc_key_map = &sparseKit;
        MapResult customKr;
        const bool customOk = loadAndMapFixture("SYNTH_REVO_XG_DRUMS.sty", customOpt, pr, mr, customKr);
        bool allInSparseKit = customOk;
        for (const auto& el : customKr.style.elements) {
            for (const auto& e : el.events) {
                const uint8_t hi = e.status & 0xF0;
                const uint8_t ch = e.status & 0x0F;
                if (hi == 0x90 && e.d2 > 0 && (ch == kDrumCh || ch == kPercCh)) {
                    if (!sparseKit.hasKey(e.d1)) allInSparseKit = false;
                }
            }
        }
        TEST("Q23 DrumRemap: caller-injected IDrumKitKeyMap* honored (100% surviving hits populated in target kit)",
             allInSparseKit && validate(customKr.style).empty());
    }

    // 5. SYNTH_MULTI_ZONE_SART_OTS.sty: S.Art key-switches stripped, melodic velocities 100% preserved!
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult kr;
        MapOptions opt;
        opt.ppqn = 384;
        opt.break_policy = BreakPolicy::Yamaha1BarCap;
        const bool ok = loadAndMapFixture("SYNTH_MULTI_ZONE_SART_OTS.sty", opt, pr, mr, kr);
        const auto w = write(kr.style);
        const auto back = parse(w.bytes);
        TEST("Q23 S.Art: loads, maps, writes & validates cleanly",
             ok && w.ok && back.ok && validate(back.style).empty());

        const Element* v1 = find(back.style, "v1cv1");
        uint32_t leakedKs = 0;
        std::set<uint8_t> saxVels;
        if (v1) {
            for (const auto& e : v1->events) {
                if (e.status == (0x90 | 14) && e.d2 > 0) {
                    if (e.d1 < 36) ++leakedKs;
                    else saxVels.insert(e.d2);
                }
            }
        }
        TEST("Q23 S.Art: low key-switches (12,15,19) stripped while melodic velocities (86,88,92) untouched",
             leakedKs == 0 && saxVels.count(86) == 1 && saxVels.count(88) == 1 && saxVels.count(92) == 1 &&
             kr.report.megavoice_fx_notes_stripped >= 24 && kr.report.megavoice_vel_reshaped == 0);
    }

    // 6. Control fixtures with legacy all-zero Ctb2[11..17] masks: ZERO silent track drops & ZERO false MegaVoice flattening!
    {
        const char* kControls[] = {
            "SYNTH_WALTZ_68.sty", "SYNTH_SFF1_CTAB_CNTT.sty", "SYNTH_FULL_SFF2.sty", "SYNTH_POP_SFF2.sty"
        };
        bool allValid = true, zeroDrops = true, zeroFalseMega = true;
        for (const char* fxName : kControls) {
            importers::sff1::ParseResult pr;
            importers::sff1::SffToUasfResult mr;
            MapResult kr;
            MapOptions opt;
            opt.ppqn = 384;
            opt.break_policy = BreakPolicy::Yamaha1BarCap;
            if (!loadAndMapFixture(fxName, opt, pr, mr, kr) || !validate(kr.style).empty()) {
                allValid = false;
                continue;
            }
            if (kr.report.shadowed_tracks_dropped != 0) zeroDrops = false;
            if (kr.report.megavoice_vel_reshaped != 0 || kr.report.megavoice_fx_notes_stripped != 0) {
                zeroFalseMega = false;
            }
            // Verify Main A still carries Drum, Bass, Chord1, Pad notes (zero silent drop from all-zero Ctb2[11..17]!).
            const Element* v1 = find(kr.style, "v1cv1");
            if (!v1 || count(*v1, 0x90 | kDrumCh) == 0 || count(*v1, 0x90 | kBassCh) == 0 ||
                count(*v1, 0x90 | 11) == 0 || count(*v1, 0x90 | 13) == 0) {
                zeroDrops = false;
            }
        }
        TEST("Q23 Controls (Waltz/SFF1/Full/Pop): 0 silent track drops on legacy all-zero Ctb2, 0 false MegaVoice flattening",
             allValid && zeroDrops && zeroFalseMega);
    }

    // 7. MapOptions::raw() disables Q2-Q3 transforms cleanly
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult krSplit, krMega, krDrum, krSrc;
        MapOptions raw = MapOptions::raw();
        raw.ppqn = 384;
        raw.break_policy = BreakPolicy::Yamaha1BarCap;

        const bool ok1 = loadAndMapFixture("SYNTH_CHORD_MUTE_SPLIT.sty", raw, pr, mr, krSplit);
        const bool ok2 = loadAndMapFixture("SYNTH_MEGAVOICE_GUITAR_BASS.sty", raw, pr, mr, krMega);
        const bool ok3 = loadAndMapFixture("SYNTH_REVO_XG_DRUMS.sty", raw, pr, mr, krDrum);
        const bool ok4 = loadAndMapFixture("SYNTH_SRC_CHORD_NON_C.sty", raw, pr, mr, krSrc);

        const Element* rawMegaV1 = find(krMega.style, "v1cv1");
        bool hasRawFx = false;
        if (rawMegaV1) {
            for (const auto& e : rawMegaV1->events) {
                if (e.status == (0x90 | 12) && e.d2 > 0 && e.d1 >= 96) hasRawFx = true;
            }
        }
        TEST("Q23 raw(): disables CV split, MegaVoice sanitize, drum remap/swap & source-chord normalization",
             ok1 && ok2 && ok3 && ok4 &&
             krSplit.report.cv_splits_created == 0 && !find(krSplit.style, "v1cv2") &&
             hasRawFx && krMega.report.megavoice_vel_reshaped == 0 &&
             !krDrum.report.rhythm_tracks_swapped && krDrum.report.drum_notes_remapped_approx == 0 &&
             krSrc.report.tracks_source_chord_normalized == 0);
    }

    // 8. CLI additive flags smoke test on aiarranger-convert
#if defined(CONVERT_BIN) && !defined(_WIN32)
    {
        namespace fs = std::filesystem;
        const fs::path tmpDir = fs::temp_directory_path() / ("aiarr_q23_cli_" + std::to_string(::getpid()));
        fs::remove_all(tmpDir);
        fs::create_directories(tmpDir);
        const fs::path qualMid = tmpDir / "split_qual.mid";
        const fs::path rawMid = tmpDir / "split_raw.mid";

        auto runCli = [](const std::string& args) {
            const std::string cmd = std::string("\"") + CONVERT_BIN + "\" " + args + " >/dev/null 2>&1";
            const int st = std::system(cmd.c_str());
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        };

        const std::string inSplit = "\"" + fixturePath("SYNTH_CHORD_MUTE_SPLIT.sty") + "\"";
        const int rcQual = runCli("--input " + inSplit + " --target pa5x --output \"" + qualMid.string() +
                                  "\" --break-policy cap --quality-mode quality --megavoice-mode auto "
                                  "--drum-remap on --cv-split on --normalize-source-chord on");
        const int rcRaw = runCli("--input " + inSplit + " --target pa5x --output \"" + rawMid.string() +
                                 "\" --break-policy withhold --quality-mode raw");
        const int rcBad = runCli("--input " + inSplit + " --target pa5x --output \"" + rawMid.string() +
                                 "\" --quality-mode invalid_mode");

        auto readBytes = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
        };
        const auto pQual = parse(readBytes(qualMid));
        const auto pRaw = parse(readBytes(rawMid));
        TEST("Q23 CLI: additive flags work, quality emits v1cv2+br, raw emits single-CV & invalid exits 1",
             rcQual == 0 && rcRaw == 0 && rcBad == 1 &&
             pQual.ok && validate(pQual.style).empty() &&
             find(pQual.style, "v1cv2") != nullptr && find(pQual.style, "br") != nullptr &&
             pRaw.ok && validate(pRaw.style).empty() &&
             find(pRaw.style, "v1cv2") == nullptr && find(pRaw.style, "br") == nullptr);

        fs::remove_all(tmpDir);
    }
#endif
}

static void runQ4WireE2eTests() {
    std::printf("\n── Wave-3 Q4-Wire Timbre Profiler, Pa5X Sound Matcher & CLI E2E Tests ──\n");

    // 1. Curated Pa5X sound matching across melodic, bass, guitar, sax, pad, strings, drum/perc tracks
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult kr;
        MapOptions opt = MapOptions::quality();
        opt.ppqn = 384;
        opt.break_policy = BreakPolicy::Yamaha1BarCap;

        const bool ok = loadAndMapFixture("SYNTH_MULTI_ZONE_SART_OTS.sty", opt, pr, mr, kr);
        const auto w = write(kr.style);
        const auto back = parse(w.bytes);
        TEST("Q4-Wire S.Art/Mid-Style: maps, writes & validates cleanly with curated Pa5X sounds",
             ok && w.ok && back.ok && validate(back.style).empty());

        const auto& st = kr.report.sound_match_stats;
        TEST("Q4-Wire S.Art/Mid-Style: all 5 tracks resolved, 0 cross-contamination, 0 low-conf auto-selects, hwVerified=false",
             st.tracks_evaluated == 5 &&
             (st.exact_matches + st.approx_matches) == 5 &&
             st.unresolved == 0 &&
             st.cross_family_contaminations == 0 &&
             st.low_confidence_auto_selects == 0 &&
             !st.hardware_verified && st.l3_status == "NOT_TESTED");

        // Verify mid-style voice switch preserved: Intro A Acc1 = Austrian Grand (121/20/0), Main A Acc1 = MK I Stage (121/46/4).
        const Element* i1 = find(back.style, "i1cv1");
        const Element* v1 = find(back.style, "v1cv1");
        auto hasTick0Sound = [](const Element* el, uint8_t kch, uint8_t msb, uint8_t lsb, uint8_t pc) {
            if (!el) return false;
            bool m = false, l = false, p = false;
            for (const auto& e : el->events) {
                if (e.tick != 0) break;
                if (e.status == (0xB0 | kch) && e.d1 == 0 && e.d2 == msb) m = true;
                if (e.status == (0xB0 | kch) && e.d1 == 32 && e.d2 == lsb) l = true;
                if (e.status == (0xC0 | kch) && e.d1 == pc) p = true;
            }
            return m && l && p;
        };
        TEST("Q4-Wire S.Art/Mid-Style: Intro A Acc1=Piano(121/20/0), Main A Acc1=EP(121/46/4), Sax Acc4=SaxReed(121/0/65)",
             hasTick0Sound(i1, 11, 121, 20, 0) &&
             hasTick0Sound(v1, 11, 121, 46, 4) &&
             hasTick0Sound(v1, 14, 121, 0, 65) &&
             st.element_voice_switches >= 1 &&
             !kr.report.element_sounds.empty());
    }

    // 2. Post-swap Drum/Perc sound assignment on SYNTH_REVO_XG_DRUMS.sty (0 drum<->melodic cross-contamination)
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult kr;
        MapOptions opt = MapOptions::quality();
        opt.ppqn = 384;
        opt.break_policy = BreakPolicy::Yamaha1BarCap;

        const bool ok = loadAndMapFixture("SYNTH_REVO_XG_DRUMS.sty", opt, pr, mr, kr);
        const auto w = write(kr.style);
        const auto back = parse(w.bytes);
        TEST("Q4-Wire Revo! Swap: swapped Rhythm1->Drum gets Pop Kit 1 RR (120/0/100), Rhythm2->Perc gets Perc Kit (msb 120) + Main B switch (120/0/69)",
             ok && w.ok && back.ok && validate(back.style).empty() &&
             kr.report.rhythm_tracks_swapped &&
             kr.report.resolved_sounds.count(kDrumCh) == 1 &&
             kr.report.resolved_sounds.at(kDrumCh).msb == 120 &&
             kr.report.resolved_sounds.at(kDrumCh).pc == 100 &&
             kr.report.resolved_sounds.count(kPercCh) == 1 &&
             kr.report.resolved_sounds.at(kPercCh).msb == 120 &&
             kr.report.resolved_element_sounds.count({SectionType::Main2, kPercCh}) == 1 &&
             kr.report.resolved_element_sounds.at({SectionType::Main2, kPercCh}).msb == 120 &&
             kr.report.resolved_element_sounds.at({SectionType::Main2, kPercCh}).pc == 69 &&
             kr.report.sound_match_stats.element_voice_switches == 1 &&
             kr.report.sound_match_stats.cross_family_contaminations == 0 &&
             kr.report.sound_match_stats.low_confidence_auto_selects == 0);
    }

    // 3. Explicit user --sound-map & element_sounds precedence ALWAYS wins over auto-matching & Target KST
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult kr;
        std::vector<quality::TargetSoundEntry> targetEntries;
        {
            quality::TargetSoundEntry e{};
            e.msb = 121; e.lsb = 64; e.pc = 12;
            e.is_drum_kit = false; e.in_spec = true;
            e.display_name = "Synthetic Target Grand Piano";
            targetEntries.push_back(e);
        }
        {
            quality::TargetSoundEntry oos{};
            oos.msb = 120; oos.lsb = 70; oos.pc = 0; // Out-of-spec USERDK07!
            oos.is_drum_kit = true; oos.in_spec = true; // tampered flag — matcher MUST reject!
            oos.display_name = "Synthetic OOS Rock Drum Kit";
            targetEntries.push_back(oos);
        }

        MapOptions opt = MapOptions::quality();
        opt.ppqn = 384;
        opt.break_policy = BreakPolicy::Yamaha1BarCap;
        opt.prefer_target_user_sounds = true;
        opt.target_sounds = &targetEntries;
        opt.sounds[kBassCh] = SoundRef{121, 9, 35};   // Explicit user Bass override
        opt.element_sounds[{SectionType::Main1, 11}] = SoundRef{121, 7, 19}; // Explicit user element override

        const bool ok = loadAndMapFixture("SYNTH_POP_SFF2.sty", opt, pr, mr, kr);
        const Element* v1 = find(kr.style, "v1cv1");
        bool userBassWritten = false, userElAcc1Written = false;
        if (v1) {
            for (const auto& e : v1->events) {
                if (e.tick == 0 && e.status == (0xC0 | kBassCh) && e.d1 == 35) userBassWritten = true;
                if (e.tick == 0 && e.status == (0xC0 | 11) && e.d1 == 19) userElAcc1Written = true;
            }
        }
        TEST("Q4-Wire Precedence & Target KST: user sounds win unconditionally, OOS USERDK07 skipped & counted",
             ok && validate(kr.style).empty() &&
             userBassWritten && userElAcc1Written &&
             kr.report.resolved_sounds.at(kBassCh).lsb == 9 &&
             kr.report.resolved_sounds.at(kBassCh).pc == 35 &&
             kr.report.sound_match_stats.user_overrides == 1 &&
             kr.report.sound_match_stats.unsupported >= 1 &&
             std::find(kr.report.quality_diagnostics.begin(), kr.report.quality_diagnostics.end(),
                       "STYLE_QUAL_TARGET_KST_OUT_OF_SPEC_SKIPPED") != kr.report.quality_diagnostics.end());
    }

    // 4. Low-confidence (< 40) / Unidentified / GM SFX fail-closed rejection (NEVER auto-selects junk sound)
    {
        auto notePair = [](uint8_t ch, uint8_t p, uint64_t on, uint64_t off) {
            return std::vector<uasf::MidiEvent>{
                ev(MidiEventType::NoteOn, ch, p, 85, on),
                ev(MidiEventType::NoteOff, ch, p, 0, off),
            };
        };
        // Track A (ch 11, generic "Chord1"): single mid-high mono note, NO Bank/PC -> Unknown (conf 0) -> UNRESOLVED!
        auto unkTr = track(11, notePair(11, 72, 0, 480), uasf::TrackRole::Chord);
        unkTr.name = "Chord1";
        // Track B (ch 12, generic "Chord2"): GM SFX PC 125 (Helicopter) -> Unknown / unsupported -> UNRESOLVED!
        std::vector<uasf::MidiEvent> sfxEvs = {
            ev(MidiEventType::ControlChange, 12, 0, 0, 0),
            ev(MidiEventType::ControlChange, 12, 32, 0, 0),
            ev(MidiEventType::ProgramChange, 12, 125, 0, 0),
            ev(MidiEventType::NoteOn, 12, 60, 80, 0),
            ev(MidiEventType::NoteOff, 12, 60, 0, 480),
        };
        auto sfxTr = track(12, std::move(sfxEvs), uasf::TrackRole::Chord);
        sfxTr.name = "Chord2";
        // Track C (ch 10, Bass): clear Yamaha Electric Bass 0/0/33 -> resolved cleanly!
        std::vector<uasf::MidiEvent> bassEvs = {
            ev(MidiEventType::ControlChange, 10, 0, 0, 0),
            ev(MidiEventType::ControlChange, 10, 32, 0, 0),
            ev(MidiEventType::ProgramChange, 10, 33, 0, 0),
            ev(MidiEventType::NoteOn, 10, 36, 95, 0),
            ev(MidiEventType::NoteOff, 10, 36, 0, 960),
        };
        auto bassTr = track(10, std::move(bassEvs), uasf::TrackRole::Bass);
        bassTr.name = "Bass";

        auto st = styleOf({section(SectionType::Main1, "Main A", 1, {bassTr, unkTr, sfxTr})});
        const auto kr = mapToKorg(st, MapOptions::quality());
        const Element* v1 = find(kr.style, "v1cv1");
        TEST("Q4-Wire Fail-Closed: Unidentified & GM SFX stay UNRESOLVED (0 auto PC emitted), Bass resolves cleanly",
             kr.ok && validate(kr.style).empty() && v1 &&
             count(*v1, 0xC0 | kBassCh) == 1 &&
             count(*v1, 0xC0 | 11) == 0 &&
             count(*v1, 0xC0 | 12) == 0 &&
             kr.report.sound_match_stats.unresolved == 2 &&
             kr.report.sound_match_stats.low_confidence_fallbacks == 1 &&
             kr.report.sound_match_stats.unsupported == 1 &&
             kr.report.sound_match_stats.low_confidence_auto_selects == 0 &&
             kr.report.sound_match_stats.cross_family_contaminations == 0);
    }

    // 5. MapOptions::raw() completely disables sound matching (0 auto Bank/PC, all UNRESOLVED)
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult krRaw;
        MapOptions raw = MapOptions::raw();
        raw.ppqn = 384;
        raw.break_policy = BreakPolicy::Yamaha1BarCap;

        const bool ok = loadAndMapFixture("SYNTH_MULTI_ZONE_SART_OTS.sty", raw, pr, mr, krRaw);
        uint32_t pcEvents = 0;
        for (const auto& el : krRaw.style.elements) {
            for (const auto& e : el.events) {
                if ((e.status & 0xF0) == 0xC0) ++pcEvents;
            }
        }
        TEST("Q4-Wire raw(): disables sound matching completely (0 PC events, 0 exact/approx, all UNRESOLVED)",
             ok && pcEvents == 0 &&
             krRaw.report.resolved_sounds.empty() &&
             krRaw.report.sound_match_stats.exact_matches == 0 &&
             krRaw.report.sound_match_stats.approx_matches == 0 &&
             krRaw.report.sound_match_stats.unresolved == krRaw.report.sound_match_stats.tracks_evaluated);
    }

    // 6. CLI Q4-Wire flags + fail-closed validation on aiarranger-convert
#if defined(CONVERT_BIN) && !defined(_WIN32)
    {
        namespace fs = std::filesystem;
        const fs::path tmpDir = fs::temp_directory_path() / ("aiarr_q4_cli_" + std::to_string(::getpid()));
        fs::remove_all(tmpDir);
        fs::create_directories(tmpDir);

        const fs::path autoMid = tmpDir / "auto.mid";
        const fs::path noAutoMid = tmpDir / "noauto.mid";
        const fs::path dumpMap = tmpDir / "dumped.soundmap";
        const fs::path failMid = tmpDir / "should_not_exist.mid";
        const fs::path synthKst = tmpDir / "Target.KST";
        fs::create_directories(synthKst / "SOUND");

        auto runCli = [](const std::string& args) {
            const std::string cmd = std::string("\"") + CONVERT_BIN + "\" " + args + " >/dev/null 2>&1";
            const int st = std::system(cmd.c_str());
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        };

        const std::string inPop = "\"" + fixturePath("SYNTH_POP_SFF2.sty") + "\"";
        const int rcAuto = runCli("--input " + inPop + " --target pa5x --output \"" + autoMid.string() +
                                  "\" --auto-sound-map --dump-soundmap \"" + dumpMap.string() + "\"");
        const int rcNoAuto = runCli("--input " + inPop + " --target pa5x --output \"" + noAutoMid.string() +
                                    "\" --no-auto-sound-map");
        const int rcKstOk = runCli("--input " + inPop + " --target pa5x --output \"" + (tmpDir / "kst.mid").string() +
                                   "\" --target-kst \"" + synthKst.string() + "\" --prefer-target-user-sounds");

        // Fail-closed CLI rejections:
        const int rcPrefWithoutKst = runCli("--input " + inPop + " --target pa5x --output \"" + failMid.string() +
                                            "\" --prefer-target-user-sounds");
        const int rcMissingKst = runCli("--input " + inPop + " --target pa5x --output \"" + failMid.string() +
                                        "\" --target-kst \"" + (tmpDir / "missing.KST").string() + "\"");
        const int rcRawPlusAuto = runCli("--input " + inPop + " --target pa5x --output \"" + failMid.string() +
                                         "\" --raw --auto-sound-map");
        const int rcQRawPlusAuto = runCli("--input " + inPop + " --target pa5x --output \"" + failMid.string() +
                                          "\" --quality-mode raw --auto-sound-map");
        const int rcBothAutoFlags = runCli("--input " + inPop + " --target pa5x --output \"" + failMid.string() +
                                           "\" --auto-sound-map --no-auto-sound-map");
        const int rcNoAutoPlusPref = runCli("--input " + inPop + " --target pa5x --output \"" + failMid.string() +
                                            "\" --no-auto-sound-map --target-kst \"" + synthKst.string() +
                                            "\" --prefer-target-user-sounds");

        auto readBytes = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
        };
        const auto pAuto = parse(readBytes(autoMid));
        const auto pNoAuto = parse(readBytes(noAutoMid));
        uint32_t autoPcs = 0, noAutoPcs = 0;
        if (pAuto.ok) {
            for (const auto& e : pAuto.style.elements[0].events) if ((e.status & 0xF0) == 0xC0) ++autoPcs;
        }
        if (pNoAuto.ok) {
            for (const auto& e : pNoAuto.style.elements[0].events) if ((e.status & 0xF0) == 0xC0) ++noAutoPcs;
        }
        std::ifstream dumpIn(dumpMap);
        const auto parsedDump = parseSoundMap(dumpIn, "dumped.soundmap");

        TEST("Q4-Wire CLI: --auto-sound-map emits sounds, --no-auto-sound-map omits them, --dump-soundmap parses cleanly, --target-kst succeeds read-only",
             rcAuto == 0 && rcNoAuto == 0 && rcKstOk == 0 &&
             pAuto.ok && validate(pAuto.style).empty() && autoPcs >= 5 &&
             pNoAuto.ok && validate(pNoAuto.style).empty() && noAutoPcs == 0 &&
             parsedDump.ok && parsedDump.sounds.size() >= 5);

        TEST("Q4-Wire CLI Fail-Closed: invalid flag combos & missing --target-kst exit 1 and write no output",
             rcPrefWithoutKst == 1 && rcMissingKst == 1 && rcRawPlusAuto == 1 &&
             rcQRawPlusAuto == 1 && rcBothAutoFlags == 1 && rcNoAutoPlusPref == 1 &&
             !fs::exists(failMid));

        // Audit P1-1: Verify --dump-soundmap matches SMF tick-0 Bank/PC 100% identically on BOTH
        // normal SFF2 (SYNTH_POP_SFF2, where Rhythm2 has role=Percussion) AND Rhythm1/2 swapped (SYNTH_REVO_XG_DRUMS)!
        {
            auto extractTick0Sounds = [](const Element& el) {
                std::map<uint8_t, SoundRef> out;
                std::map<uint8_t, int> msb, lsb, pc;
                for (const auto& e : el.events) {
                    if (e.tick != 0) break;
                    const uint8_t hi = e.status & 0xF0;
                    const uint8_t ch = e.status & 0x0F;
                    if (hi == 0xB0 && e.d1 == 0) msb[ch] = e.d2;
                    else if (hi == 0xB0 && e.d1 == 32) lsb[ch] = e.d2;
                    else if (hi == 0xC0) pc[ch] = e.d1;
                }
                for (const auto& [ch, p] : pc) {
                    if (msb.count(ch) && lsb.count(ch)) {
                        out[ch] = SoundRef{static_cast<uint8_t>(msb[ch]), static_cast<uint8_t>(lsb[ch]), static_cast<uint8_t>(p)};
                    }
                }
                return out;
            };

            const auto smfPopSounds = extractTick0Sounds(pAuto.style.elements[0]);
            bool popExactMatch = (smfPopSounds.size() == parsedDump.sounds.size() &&
                                  parsedDump.sounds.count(kDrumCh) == 1 &&
                                  parsedDump.sounds.at(kDrumCh).msb == 120 &&
                                  parsedDump.sounds.at(kDrumCh).lsb == 0 &&
                                  parsedDump.sounds.at(kDrumCh).pc == 100);
            for (const auto& [ch, ref] : smfPopSounds) {
                auto it = parsedDump.sounds.find(ch);
                if (it == parsedDump.sounds.end() ||
                    it->second.msb != ref.msb || it->second.lsb != ref.lsb || it->second.pc != ref.pc) {
                    popExactMatch = false;
                }
            }

            const fs::path revoMid = tmpDir / "revo.mid";
            const fs::path revoDump = tmpDir / "revo.soundmap";
            const std::string inRevo = "\"" + fixturePath("SYNTH_REVO_XG_DRUMS.sty") + "\"";
            const int rcRevo = runCli("--input " + inRevo + " --target pa5x --output \"" + revoMid.string() +
                                      "\" --break-policy cap --auto-sound-map --dump-soundmap \"" + revoDump.string() + "\"");
            const auto pRevo = parse(readBytes(revoMid));
            std::ifstream revoDumpIn(revoDump);
            const auto parsedRevoDump = parseSoundMap(revoDumpIn, "revo.soundmap");
            bool revoExactMatch = (rcRevo == 0 && pRevo.ok && parsedRevoDump.ok &&
                                   parsedRevoDump.sounds.count(kDrumCh) == 1 &&
                                   parsedRevoDump.sounds.at(kDrumCh).msb == 120 &&
                                   parsedRevoDump.sounds.at(kDrumCh).pc == 100);
            if (pRevo.ok && !pRevo.style.elements.empty()) {
                const auto smfRevoSounds = extractTick0Sounds(pRevo.style.elements[0]); // i1cv1 has style-wide sounds
                if (smfRevoSounds.size() != parsedRevoDump.sounds.size()) revoExactMatch = false;
                for (const auto& [ch, ref] : smfRevoSounds) {
                    auto it = parsedRevoDump.sounds.find(ch);
                    if (it == parsedRevoDump.sounds.end() ||
                        it->second.msb != ref.msb || it->second.lsb != ref.lsb || it->second.pc != ref.pc) {
                        revoExactMatch = false;
                    }
                }
            }
            TEST("P1-1 CLI Parity: --dump-soundmap matches SMF Bank/PC 100% identically on SYNTH_POP_SFF2 (drum=120/0/100) & swapped SYNTH_REVO_XG_DRUMS",
                 popExactMatch && revoExactMatch);
        }

        fs::remove_all(tmpDir);
    }
#endif

    // 7. Audit P1-1 & P1-2: Target-KST DrumKit binding on normal SFF2 (Rhythm2 role=Percussion) AND strict kDrumCh vs kPercCh isolation
    {
        // P1-2 direct isolation test: target_drum_key_map set to sparse {36, 38, 42} ONLY;
        // kPercCh has Factory Perc notes 60 (Hi Bongo) & 70 (Maracas) and MUST remain 100% unmangled!
        quality::DrumKitKeySummaryView sparseDrum;
        sparseDrum.kit_name = "USERDK01/2";
        sparseDrum.gm_alignment_score = 1.0;
        for (uint8_t n : {uint8_t(36), uint8_t(38), uint8_t(42)}) {
            auto& slot = sparseDrum.keys[n];
            slot.populated = true;
            slot.layer_count = 1;
            slot.vel_tops[0] = 127;
            slot.resolved_family = quality::drum_detail::gm2DefaultFamily(n);
            slot.choke_group = quality::drum_detail::gm2DefaultChokeGroup(n);
        }
        quality::UserdkDrumKitKeyMap sparseUserdk(sparseDrum);

        auto mkNote = [](uint8_t ch, uint8_t p, uint64_t on, uint64_t off) {
            return std::vector<uasf::MidiEvent>{
                ev(MidiEventType::NoteOn, ch, p, 95, on),
                ev(MidiEventType::NoteOff, ch, p, 0, off),
            };
        };
        std::vector<uasf::MidiEvent> drumEvs;
        for (auto e : mkNote(9, 36, 0, 240)) drumEvs.push_back(e);
        for (auto e : mkNote(9, 13, 480, 720)) drumEvs.push_back(e); // extended key -> remaps into sparseUserdk {36,38,42}
        std::vector<uasf::MidiEvent> percEvs;
        for (auto e : mkNote(8, 60, 0, 240)) percEvs.push_back(e);   // Hi Bongo (Factory Perc!)
        for (auto e : mkNote(8, 70, 480, 720)) percEvs.push_back(e); // Maracas (Factory Perc!)

        auto trDrum = track(9, std::move(drumEvs), uasf::TrackRole::Drum);
        trDrum.name = "Rhythm2";
        auto trPerc = track(8, std::move(percEvs), uasf::TrackRole::Percussion);
        trPerc.name = "Rhythm1";
        auto stIso = styleOf({section(SectionType::Main1, "Main A", 1, {trDrum, trPerc})});

        MapOptions isoOpt = MapOptions::quality();
        isoOpt.target_drum_key_map = &sparseUserdk;
        isoOpt.target_perc_key_map = nullptr; // kPercCh stays on Factory Percussion!
        const auto krIso = mapToKorg(stIso, isoOpt);
        const Element* v1Iso = find(krIso.style, "v1cv1");
        bool drumAllInSparse = (v1Iso != nullptr);
        if (v1Iso) {
            for (const auto& e : v1Iso->events) {
                if (e.status == (0x90 | kDrumCh) && e.d2 > 0 && !sparseUserdk.hasKey(e.d1)) {
                    drumAllInSparse = false;
                }
            }
        }
        TEST("P1-2 Isolation: target_drum_key_map applies ONLY to kDrumCh; Factory kPercCh notes 60/70 stay 100% unmangled (not mapped to 42)",
             krIso.ok && validate(krIso.style).empty() && v1Iso &&
             drumAllInSparse &&
             count(*v1Iso, 0x90 | kPercCh, 60) == 1 &&
             count(*v1Iso, 0x90 | kPercCh, 70) == 1 &&
             count(*v1Iso, 0x90 | kPercCh, 42) == 0);

        // P1-1 Target-KST DrumKit binding via target_drum_key_maps on SYNTH_POP_SFF2 (normal SFF2 where sff1_mapper sets role=Percussion on Rhythm2)
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult krBind;
        quality::TargetSoundCatalogView targetCat{};
        {
            quality::TargetSoundEntry te{};
            te.msb = 120; te.lsb = 64; te.pc = 2; // USERDK01/2
            te.is_drum_kit = true; te.in_spec = true;
            te.display_name = "Synthetic Pop Studio Drum Kit";
            te.slot_address = quality::TargetSlotAddress{true, 1, 2};
            te.gm_alignment_score = 100;
            te.family_bitmask = quality::timbreFamilyBit(quality::TimbreFamily::DrumKit);
            targetCat.entries.push_back(te);
        }
        std::map<std::tuple<uint8_t, uint8_t, uint8_t>, quality::UserdkDrumKitKeyMap> bridgeKits;
        bridgeKits.emplace(std::make_tuple(uint8_t{120}, uint8_t{64}, uint8_t{2}), sparseUserdk);

        MapOptions bindOpt = MapOptions::quality();
        bindOpt.ppqn = 384;
        bindOpt.break_policy = BreakPolicy::Yamaha1BarCap;
        bindOpt.prefer_target_user_sounds = true;
        bindOpt.target_sound_catalog = &targetCat;
        bindOpt.target_drum_key_maps = &bridgeKits;
        const bool bindOk = loadAndMapFixture("SYNTH_POP_SFF2.sty", bindOpt, pr, mr, krBind);
        bool popDrumBoundToUserdk = bindOk && krBind.report.resolved_sounds.count(kDrumCh) == 1 &&
                                    krBind.report.resolved_sounds.at(kDrumCh).msb == 120 &&
                                    krBind.report.resolved_sounds.at(kDrumCh).lsb == 64 &&
                                    krBind.report.resolved_sounds.at(kDrumCh).pc == 2;
        bool popPercStayedFactory = krBind.report.resolved_sounds.count(kPercCh) == 1 &&
                                    krBind.report.resolved_sounds.at(kPercCh).msb == 120 &&
                                    krBind.report.resolved_sounds.at(kPercCh).lsb == 0 &&
                                    krBind.report.resolved_sounds.at(kPercCh).pc == 69;
        for (const auto& el : krBind.style.elements) {
            for (const auto& e : el.events) {
                if (e.status == (0x90 | kDrumCh) && e.d2 > 0 && !sparseUserdk.hasKey(e.d1)) {
                    popDrumBoundToUserdk = false;
                }
            }
        }
        TEST("P1-1+P1-2 Target-KST Bridge: SYNTH_POP_SFF2 binds USERDK01/2 strictly to kDrumCh while kPercCh stays on Factory Perc (120/0/69)",
             popDrumBoundToUserdk && popPercStayedFactory && validate(krBind.style).empty());
    }

    // 8. Audit P2-1 & P2-2: Duplicate SectionType first-wins protection + accurate "(auto match)" vs "(user map)" report headers
    {
        // P2-1: Duplicate SectionType::Main1 (dropped by exporter) must NOT overwrite the first Main1's Piano voice with Brass!
        std::vector<uasf::MidiEvent> pnoEvs = {
            ev(MidiEventType::ControlChange, 11, 0, 0, 0),
            ev(MidiEventType::ControlChange, 11, 32, 0, 0),
            ev(MidiEventType::ProgramChange, 11, 0, 0, 0), // Piano
            ev(MidiEventType::NoteOn, 11, 60, 90, 0),
            ev(MidiEventType::NoteOff, 11, 60, 0, 480),
        };
        std::vector<uasf::MidiEvent> brsEvs = {
            ev(MidiEventType::ControlChange, 11, 0, 0, 0),
            ev(MidiEventType::ControlChange, 11, 32, 0, 0),
            ev(MidiEventType::ProgramChange, 11, 61, 0, 0), // Brass (in dropped duplicate section!)
            ev(MidiEventType::NoteOn, 11, 60, 90, 0),
            ev(MidiEventType::NoteOff, 11, 60, 0, 480),
        };
        auto t1 = track(11, std::move(pnoEvs), uasf::TrackRole::Chord); t1.name = "Chord1";
        auto t2 = track(11, std::move(brsEvs), uasf::TrackRole::Chord); t2.name = "Chord1";
        auto stDup = styleOf({
            section(SectionType::Main1, "Main A", 1, {t1}),
            section(SectionType::Main1, "Main A Dup", 1, {t2}),
        });
        const auto krDup = mapToKorg(stDup, MapOptions::quality());
        TEST("P2-1 Duplicate SectionType: first Main1 Piano (121/20/0) preserved; dropped duplicate Brass ignored",
             krDup.ok && krDup.report.resolved_sounds.count(11) == 1 &&
             krDup.report.resolved_sounds.at(11).msb == 121 &&
             krDup.report.resolved_sounds.at(11).lsb == 20 &&
             krDup.report.resolved_sounds.at(11).pc == 0);

        // P2-2: Auto-matched element switches say "(auto match)", explicit user element switches say "(user map)"
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult krAutoEl;
        MapOptions autoElOpt = MapOptions::quality();
        autoElOpt.ppqn = 384;
        autoElOpt.break_policy = BreakPolicy::Yamaha1BarCap;
        const bool okAutoEl = loadAndMapFixture("SYNTH_MULTI_ZONE_SART_OTS.sty", autoElOpt, pr, mr, krAutoEl);
        const std::string autoRepTxt = formatReport(krAutoEl.report, krAutoEl.style);

        MapOptions userElOpt = MapOptions::raw();
        userElOpt.element_sounds[{SectionType::Main1, 11}] = SoundRef{121, 5, 10};
        const auto krUserEl = mapToKorg(stDup, userElOpt);
        const std::string userRepTxt = formatReport(krUserEl.report, krUserEl.style);

        TEST("P2-2 Element-Sound Header: auto-matched reports '(auto match)' and user-only reports '(user map)'",
             okAutoEl &&
             autoRepTxt.find("Element-specific Pa5X sounds (auto match, in that element only):") != std::string::npos &&
             autoRepTxt.find("(MK I Stage, auto match)") != std::string::npos &&
             userRepTxt.find("Element-specific Pa5X sounds (user map, in that element only):") != std::string::npos);
    }
}

static void runQ5WireE2eTests() {
    std::printf("\n── Wave-4b Q5-Wire Mix/Loop-Boundary/Groove, XG SysEx & CLI E2E Tests ──\n");

    auto hasSamePitchOverlap = [](const Element& el) {
        std::map<uint16_t, int> open;
        for (const auto& e : el.events) {
            const uint8_t hi = e.status & 0xF0;
            const uint8_t ch = e.status & 0x0F;
            const uint16_t k = static_cast<uint16_t>((ch << 8) | e.d1);
            if (hi == 0x90 && e.d2 > 0) {
                if (open[k] > 0) return true;
                ++open[k];
            } else if (hi == 0x80 || (hi == 0x90 && e.d2 == 0)) {
                if (open[k] > 0) --open[k];
            }
        }
        return false;
    };

    auto hasOpenSustainOrPb = [](const Element& el) {
        std::map<uint8_t, uint8_t> cc64;
        std::map<uint8_t, uint16_t> pb;
        for (const auto& e : el.events) {
            const uint8_t hi = e.status & 0xF0;
            const uint8_t ch = e.status & 0x0F;
            if (hi == 0xB0 && e.d1 == 64) cc64[ch] = e.d2;
            else if (hi == 0xE0) pb[ch] = static_cast<uint16_t>(e.d1 | (static_cast<uint16_t>(e.d2) << 7));
        }
        for (const auto& [ch, v] : cc64) if (v >= 64) return true;
        for (const auto& [ch, v] : pb) if (v != 8192) return true;
        return false;
    };

    // 1. SYNTH_MIX_SYSEX_CC_HYGIENE.sty: 0 loop errors, 0 NRPN/CC94 leaks, live XG SysEx wired, Style Edit Sheet
    {
        importers::sff1::ParseResult pr;
        importers::sff1::SffToUasfResult mr;
        MapResult kr;
        MapOptions opt = MapOptions::quality();
        opt.ppqn = 384;
        opt.break_policy = BreakPolicy::Yamaha1BarCap;
        opt.capture_style_edit_sheet = true;
        opt.soundmap_style_name = "SYNTH_MIX_SYSEX_CC_HYGIENE.sty";

        const bool ok = loadAndMapFixture("SYNTH_MIX_SYSEX_CC_HYGIENE.sty", opt, pr, mr, kr);
        const auto w = write(kr.style);
        const auto back = parse(w.bytes);

        bool zeroLoopErr = true, zeroUnsafeCc = true, zeroOverlap = true;
        if (back.ok) {
            for (const auto& el : back.style.elements) {
                if (hasOpenSustainOrPb(el)) zeroLoopErr = false;
                if (hasSamePitchOverlap(el)) zeroOverlap = false;
                for (const auto& e : el.events) {
                    if ((e.status & 0xF0) == 0xB0 && (e.d1 == 98 || e.d1 == 99 || e.d1 == 94)) zeroUnsafeCc = false;
                }
            }
        }
        TEST("Q5-Wire SYNTH_MIX_SYSEX_CC_HYGIENE: 0 loop errors, 0 NRPN/CC94 leaks, 0 overlaps, XG SysEx tracked",
             ok && w.ok && back.ok && validate(back.style).empty() &&
             zeroLoopErr && zeroUnsafeCc && zeroOverlap &&
             kr.report.sustain_resets_added >= 2 &&
             kr.report.pitchbend_resets_added >= 2 &&
             kr.report.proprietary_ccs_stripped >= 6 &&
             kr.report.xg_sysex_seen == 6 &&
             kr.report.xg_sysex_recognized == 6 &&
             kr.report.xg_sysex_malformed == 0);

        TEST("Q5-Wire Style Edit Parameter Sheet: deterministic Markdown with epistemic tags, XG FX & EQ",
             !kr.report.style_edit_sheet_md.empty() &&
             kr.report.style_edit_sheet_md.find("hardwareVerified: false") != std::string::npos &&
             kr.report.style_edit_sheet_md.find("L3 = NOT_TESTED") != std::string::npos &&
             kr.report.style_edit_sheet_md.find("SYNTH_MIX_SYSEX_CC_HYGIENE.sty") != std::string::npos &&
             kr.report.style_edit_sheet_md.find("0x0100") != std::string::npos &&
             kr.report.style_edit_sheet_md.find("seen=6, recognized=6, unrecognized=0, malformed=0") != std::string::npos);
    }

    // 2. Same-tick RPN vs NRPN protocol order preservation + Var/Fill/Intro/Ending CC64/PB/CC11 resets
    {
        std::vector<uasf::MidiEvent> bassEvs = {
            // Valid RPN Pitch Bend Sensitivity = +/- 2 semitones at tick 0 (MUST stay in exact order!)
            ev(MidiEventType::ControlChange, 10, 101, 0, 0),
            ev(MidiEventType::ControlChange, 10, 100, 0, 0),
            ev(MidiEventType::ControlChange, 10, 6, 2, 0),
            ev(MidiEventType::ControlChange, 10, 101, 127, 0),
            ev(MidiEventType::ControlChange, 10, 100, 127, 0),
            ev(MidiEventType::PitchBend, 10, 0x00, 0x60, 480), // uncentered PB -> reset at len-1
            ev(MidiEventType::NoteOn, 10, 36, 96, 0),
            ev(MidiEventType::NoteOff, 10, 36, 0, 960),
        };
        std::vector<uasf::MidiEvent> accEvs = {
            // Yamaha NRPN (99, 98, 6, 38, 96, 97) + CC94=50 at tick 0 -> ALL stripped, CC94->CC93=15!
            ev(MidiEventType::ControlChange, 11, 99, 24, 0),
            ev(MidiEventType::ControlChange, 11, 98, 36, 0),
            ev(MidiEventType::ControlChange, 11, 6, 70, 0),
            ev(MidiEventType::ControlChange, 11, 38, 12, 0),
            ev(MidiEventType::ControlChange, 11, 96, 1, 0),
            ev(MidiEventType::ControlChange, 11, 97, 1, 0),
            ev(MidiEventType::ControlChange, 11, 94, 50, 0),
            ev(MidiEventType::ControlChange, 11, 11, 104, 0),
            ev(MidiEventType::ControlChange, 11, 64, 127, 240), // open CC64 -> reset at len-1
            ev(MidiEventType::ControlChange, 11, 11, 16, 7000), // fade below 32 -> restored to 104 at len-1
            ev(MidiEventType::NoteOn, 11, 60, 90, 0),
            ev(MidiEventType::NoteOff, 11, 60, 0, 4000),
        };
        auto trB = track(10, bassEvs, uasf::TrackRole::Bass); trB.name = "Bass";
        auto trA = track(11, accEvs, uasf::TrackRole::Chord); trA.name = "Piano";

        auto st = styleOf({
            section(SectionType::Intro1, "Intro A", 1, {trB, trA}),
            section(SectionType::Main1, "Main A", 1, {trB, trA}),
            section(SectionType::Fill1, "Fill In AA", 1, {trB, trA}),
            section(SectionType::Ending1, "Ending A", 1, {trB, trA}),
        });
        const auto kr = mapToKorg(st, MapOptions::quality());
        bool allFourClean = (kr.ok && validate(kr.style).empty() && kr.style.elements.size() == 4);
        bool rpnOrderExact = true;
        bool nrpnFullyStripped = true;
        bool cc93Blended = true;
        bool cc11Restored = true;
        for (const auto& el : kr.style.elements) {
            if (hasOpenSustainOrPb(el)) allFourClean = false;
            std::vector<std::pair<uint8_t, uint8_t>> bassRpn;
            int accCc93 = -1;
            int accLastCc11 = -1;
            for (const auto& e : el.events) {
                if ((e.status & 0xF0) != 0xB0) continue;
                const uint8_t ch = e.status & 0x0F;
                if (ch == kBassCh && (e.d1 == 101 || e.d1 == 100 || e.d1 == 6)) {
                    bassRpn.emplace_back(e.d1, e.d2);
                }
                if (ch == 11) {
                    if (e.d1 == 99 || e.d1 == 98 || e.d1 == 6 || e.d1 == 38 || e.d1 == 96 || e.d1 == 97 || e.d1 == 94) {
                        nrpnFullyStripped = false;
                    }
                    if (e.tick == 0 && e.d1 == 93) accCc93 = e.d2;
                    if (e.d1 == 11) accLastCc11 = e.d2;
                }
            }
            const std::vector<std::pair<uint8_t, uint8_t>> expectedRpn = {
                {101, 0}, {100, 0}, {6, 2}, {101, 127}, {100, 127}
            };
            if (bassRpn != expectedRpn) rpnOrderExact = false;
            if (accCc93 != 15) cc93Blended = false;
            if (accLastCc11 != 104) cc11Restored = false;
        }
        TEST("Q5-Wire RPN/NRPN & Var/Fill/Intro/Ending resets: valid RPN order kept, NRPN 99/98/6/38/96/97 stripped, CC64/PB/CC11 reset across all 4 element types",
             allFourClean && rpnOrderExact && nrpnFullyStripped && cc93Blended && cc11Restored &&
             kr.report.sustain_resets_added == 4 &&
             kr.report.pitchbend_resets_added == 4 &&
             kr.report.expression_resets_added == 4 &&
             kr.report.proprietary_ccs_stripped == 28);
    }

    // 3. XG SysEx -> tick-0 CC7/10/91/93 synthesis respecting Rhythm1/Rhythm2 swap + active-span CC11 EDM guard
    {
        // Put primary kit (36/38/42) on ch 8 (Rhythm1) and hand perc (60/70) on ch 9 (Rhythm2) -> triggers swap!
        std::vector<uasf::MidiEvent> r1Evs;
        for (int b = 0; b < 4; ++b) {
            r1Evs.push_back(ev(MidiEventType::NoteOn, 8, (b % 2 == 0) ? 36 : 38, 100, b * 1920));
            r1Evs.push_back(ev(MidiEventType::NoteOff, 8, (b % 2 == 0) ? 36 : 38, 0, b * 1920 + 480));
            r1Evs.push_back(ev(MidiEventType::NoteOn, 8, 42, 80, b * 1920 + 960));
            r1Evs.push_back(ev(MidiEventType::NoteOff, 8, 42, 0, b * 1920 + 1200));
        }
        std::vector<uasf::MidiEvent> r2Evs = {
            ev(MidiEventType::NoteOn, 9, 60, 75, 0), ev(MidiEventType::NoteOff, 9, 60, 0, 480),
            ev(MidiEventType::NoteOn, 9, 70, 72, 1920), ev(MidiEventType::NoteOff, 9, 70, 0, 2400),
        };
        // Sidechain-ducked Pad on ch 13 (tick-0 CC11=24 -> offbeat CC11=116, XG SysEx Vol=82, Pan=74, Rev=44, Var=40)
        std::vector<uasf::MidiEvent> padEvs = {
            ev(MidiEventType::ControlChange, 13, 11, 24, 0),
            ev(MidiEventType::NoteOn, 13, 48, 78, 0),
            ev(MidiEventType::NoteOn, 13, 52, 78, 0),
            ev(MidiEventType::ControlChange, 13, 11, 116, 960),
            ev(MidiEventType::NoteOff, 13, 48, 0, 7000),
            ev(MidiEventType::NoteOff, 13, 52, 0, 7000),
        };
        auto trR1 = track(8, r1Evs, uasf::TrackRole::Drum); trR1.name = "Rhythm1";
        auto trR2 = track(9, r2Evs, uasf::TrackRole::Percussion); trR2.name = "Rhythm2";
        auto trPad = track(13, padEvs, uasf::TrackRole::Pad); trPad.name = "ScPad";

        auto st = styleOf({section(SectionType::Main1, "Main A", 1, {trR1, trR2, trPad})});
        st.xg_sysex.total_sysex_seen = 6;
        st.xg_sysex.recognized_xg_count = 6;
        st.xg_sysex.parts[8].volume = 112;      // Rhythm1 -> swapped to kDrumCh (9)!
        st.xg_sysex.parts[9].volume = 86;       // Rhythm2 -> swapped to kPercCh (10)!
        st.xg_sysex.parts[13].volume = 82;
        st.xg_sysex.parts[13].pan = 74;
        st.xg_sysex.parts[13].reverb_send = 44;
        st.xg_sysex.parts[13].variation_send = 40; // blends 30% -> CC93=12

        const auto kr = mapToKorg(st, MapOptions::quality());
        const Element* v1 = find(kr.style, "v1cv1");
        auto t0cc = [&](uint8_t ch, uint8_t cc) -> int {
            int val = -1;
            if (!v1) return val;
            for (const auto& e : v1->events) {
                if (e.tick > 0) break;
                if (e.status == (0xB0 | ch) && e.d1 == cc) val = e.d2;
            }
            return val;
        };
        TEST("Q5-Wire XG SysEx + Swap + EDM Sidechain: swapped ch8/9 volumes follow kDrumCh/kPercCh, Pad gets XG CCs without sidechain over-boost",
             kr.ok && validate(kr.style).empty() && kr.report.rhythm_tracks_swapped &&
             t0cc(kDrumCh, 7) == 112 &&
             t0cc(kPercCh, 7) == 86 &&
             t0cc(13, 7) == 82 && t0cc(13, 10) == 74 && t0cc(13, 91) == 44 && t0cc(13, 93) == 12 &&
             t0cc(13, 11) == 24 &&
             kr.report.tracks_gain_adjusted == 0 &&
             kr.report.xg_sysex_params_translated == 6);
    }

    // 4. Groove: same-pitch de-overlap, swing/microtiming preservation (non-robotic!), zero-dur fix, raw() bypass
    {
        std::vector<uasf::MidiEvent> gEvs = {
            // Overlapping same-pitch notes at humanized/swing ticks (+17, +31, -9)
            ev(MidiEventType::NoteOn, 11, 60, 90, 17),
            ev(MidiEventType::NoteOn, 11, 60, 88, 1951),  // +31 swing offset, overlaps first note!
            ev(MidiEventType::NoteOff, 11, 60, 0, 2200),
            ev(MidiEventType::NoteOff, 11, 60, 0, 3400),
            // Zero-duration note at humanized tick 3831 (-9 ticks)
            ev(MidiEventType::NoteOn, 11, 64, 84, 3831),
            ev(MidiEventType::NoteOff, 11, 64, 0, 3831),
            // Final-tick anticipation at 7679 (len = 7680)
            ev(MidiEventType::NoteOn, 11, 67, 86, 7679),
            ev(MidiEventType::NoteOff, 11, 67, 0, 7720),
        };
        auto trG = track(11, gEvs, uasf::TrackRole::Chord); trG.name = "E.Piano";
        auto st = styleOf({section(SectionType::Main1, "Main A", 1, {trG})});

        const auto krQ = mapToKorg(st, MapOptions::quality());
        const auto krR = mapToKorg(st, MapOptions::raw());
        const Element* v1q = find(krQ.style, "v1cv1");
        const Element* v1r = find(krR.style, "v1cv1");

        bool keptTick17 = false, keptTick1951 = false, keptTick3831 = false, snappedAntic0 = false;
        if (v1q) {
            for (const auto& e : v1q->events) {
                if (e.status == (0x90 | 11) && e.d2 > 0) {
                    if (e.d1 == 60 && e.tick == 17) keptTick17 = true;
                    if (e.d1 == 60 && e.tick == 1951) keptTick1951 = true;
                    if (e.d1 == 64 && e.tick == 3831) keptTick3831 = true;
                    if (e.d1 == 67 && e.tick == 0) snappedAntic0 = true;
                }
            }
        }
        TEST("Q5-Wire Groove: de-overlaps same pitch, preserves exact swing/microtiming ticks, fixes 0-dur & preserves anticipation",
             krQ.ok && validate(krQ.style).empty() && v1q &&
             !hasSamePitchOverlap(*v1q) &&
             keptTick17 && keptTick1951 && keptTick3831 && snappedAntic0 &&
             krQ.report.same_pitch_overlaps_fixed >= 1 &&
             krQ.report.zero_length_notes_fixed >= 1 &&
             krQ.report.anticipations_preserved >= 1);

        TEST("Q5-Wire raw(): completely bypasses groove & mix normalization (0 overlaps fixed, 0 CCs stripped, 0 resets)",
             krR.ok && validate(krR.style).empty() && v1r &&
             hasSamePitchOverlap(*v1r) &&
             krR.report.same_pitch_overlaps_fixed == 0 &&
             krR.report.proprietary_ccs_stripped == 0 &&
             krR.report.sustain_resets_added == 0 &&
             krR.report.pitchbend_resets_added == 0);
    }

    // 5. CLI Q5 flags, --style-edit-sheet & fail-closed validation on aiarranger-convert
#if defined(CONVERT_BIN) && !defined(_WIN32)
    {
        namespace fs = std::filesystem;
        const fs::path tmpDir = fs::temp_directory_path() / ("aiarr_q5_cli_" + std::to_string(::getpid()));
        fs::remove_all(tmpDir);
        fs::create_directories(tmpDir);

        const fs::path okMid = tmpDir / "q5_ok.mid";
        const fs::path noMixMid = tmpDir / "q5_nomix.mid";
        const fs::path sheetPath = tmpDir / "sheet.md";
        const fs::path reportPath = tmpDir / "report.txt";
        const fs::path failMid = tmpDir / "fail_q5.mid";

        auto runCli = [](const std::string& args) {
            const std::string cmd = std::string("\"") + CONVERT_BIN + "\" " + args + " >/dev/null 2>&1";
            const int st = std::system(cmd.c_str());
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        };

        const std::string inMix = "\"" + fixturePath("SYNTH_MIX_SYSEX_CC_HYGIENE.sty") + "\"";
        const int rcOk = runCli("--input " + inMix + " --target pa5x --output \"" + okMid.string() +
                                "\" --break-policy cap --mix-normalize on --groove-normalize on "
                                "--xg-sysex-mix on --headroom-normalize on --style-edit-sheet \"" +
                                sheetPath.string() + "\" --report \"" + reportPath.string() + "\"");
        const int rcNoMix = runCli("--input " + inMix + " --target pa5x --output \"" + noMixMid.string() +
                                   "\" --break-policy cap --mix-normalize off --groove-normalize off");
        const int rcBadRawMix = runCli("--input " + inMix + " --target pa5x --output \"" + failMid.string() +
                                       "\" --raw --mix-normalize on");
        const int rcBadVal = runCli("--input " + inMix + " --target pa5x --output \"" + failMid.string() +
                                    "\" --mix-normalize maybe");

        auto readText = [](const fs::path& p) {
            std::ifstream f(p);
            return std::string((std::istreambuf_iterator<char>(f)), {});
        };
        auto readBytes = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
        };
        const auto pOk = parse(readBytes(okMid));
        const auto pNoMix = parse(readBytes(noMixMid));
        const std::string sheetTxt = readText(sheetPath);
        const std::string repTxt = readText(reportPath);

        bool okHasLoopErr = false, noMixHasLoopErr = false;
        if (pOk.ok) for (const auto& el : pOk.style.elements) if (hasOpenSustainOrPb(el)) okHasLoopErr = true;
        if (pNoMix.ok) for (const auto& el : pNoMix.style.elements) if (hasOpenSustainOrPb(el)) noMixHasLoopErr = true;

        TEST("Q5-Wire CLI: Q5 knobs, --style-edit-sheet, report Stages 1-6 & fail-closed raw+mix rejection",
             rcOk == 0 && rcNoMix == 0 && rcBadRawMix == 1 && rcBadVal == 1 &&
             !fs::exists(failMid) &&
             pOk.ok && validate(pOk.style).empty() && !okHasLoopErr &&
             pNoMix.ok && validate(pNoMix.style).empty() && noMixHasLoopErr &&
             sheetTxt.find("# Korg Pa5X Style Edit Parameter Sheet") != std::string::npos &&
             sheetTxt.find("hardwareVerified: false") != std::string::npos &&
             repTxt.find("Groove & timing (Stage 6a):") != std::string::npos &&
             repTxt.find("Mix, loop-boundary & XG SysEx (Stage 6b):") != std::string::npos);

        fs::remove_all(tmpDir);
    }
#endif
}

int main() {
    std::printf("Test: Genos (UASF) -> Korg Pa5X mapper\n");
    using T = SectionType;

    // ── Channel table: Genos parts 9..16 -> Korg tracks ──
    TEST("Rhythm1 (ch9) -> Percussion", korgChannelFor(8) == kPercCh);
    TEST("Rhythm2 (ch10) -> Drum", korgChannelFor(9) == kDrumCh);
    TEST("Bass (ch11) -> Bass", korgChannelFor(10) == kBassCh);
    TEST("Chord1..Phrase2 (ch12-16) -> Acc1..Acc5", korgChannelFor(11) == 11 && korgChannelFor(15) == 15);
    TEST("ch1-8 have no Korg track", korgChannelFor(0) == 0xFF && korgChannelFor(7) == 0xFF);

    // ── Section table ──
    TEST("Main A-D -> v1..v4", std::string(korgMarkerFor(T::Main1)) == "v1cv1" && std::string(korgMarkerFor(T::Main4)) == "v4cv1");
    TEST("Intro A/B -> i1/i2", std::string(korgMarkerFor(T::Intro1)) == "i1cv1" && std::string(korgMarkerFor(T::Intro2)) == "i2cv1");
    TEST("Fill AA/BB -> f1/f2", std::string(korgMarkerFor(T::Fill1)) == "f1cv1" && std::string(korgMarkerFor(T::Fill2)) == "f2cv1");
    TEST("Ending A/B -> e1/e2", std::string(korgMarkerFor(T::Ending1)) == "e1cv1" && std::string(korgMarkerFor(T::Ending2)) == "e2cv1");
    TEST("Intro C / Fill CC,DD / Ending C -> i3 / f3,f4 / e3 (manual p.1035)",
         std::string(korgMarkerFor(T::Intro3)) == "i3cv1" && std::string(korgMarkerFor(T::Fill3)) == "f3cv1" &&
         std::string(korgMarkerFor(T::Fill4)) == "f4cv1" && std::string(korgMarkerFor(T::Ending3)) == "e3cv1");
    TEST("Break: held back (no code emitted)", !korgMarkerFor(T::Break));

    // ── Full mapping on a synthetic style (legacy raw passthrough contract) ──
    const auto note = [](uint8_t ch, uint8_t p, uint64_t on, uint64_t off, uint8_t v = 100) {
        return std::vector<uasf::MidiEvent>{ev(MidiEventType::NoteOn, ch, p, v, on), ev(MidiEventType::NoteOff, ch, p, 0, off)};
    };
    auto cat = [](std::vector<uasf::MidiEvent> a, const std::vector<uasf::MidiEvent>& b) { a.insert(a.end(), b.begin(), b.end()); return a; };

    std::vector<uasf::MidiEvent> acc = {ev(MidiEventType::ControlChange, 11, 0, 63, 0),
                                        ev(MidiEventType::ControlChange, 11, 32, 120, 0),
                                        ev(MidiEventType::ProgramChange, 11, 21, 0, 0),
                                        ev(MidiEventType::ControlChange, 11, 11, 100, 0)};
    acc = cat(acc, note(11, 60, 0, 960));
    acc = cat(acc, note(11, 5, 100, 200));          // MegaVoice-style noise in C-1..B-1
    acc = cat(acc, note(11, 64, 7000, 9000));       // crosses the 1-bar (7680) end
    acc = cat(acc, note(11, 67, 7679, 7680));       // starts on the last tick
    auto drums = cat(note(9, 36, 0, 100), note(9, 13, 480, 500));   // Revo-style low note kept in raw mode
    auto lowCh = note(3, 60, 0, 100);               // ch4 with no route: no Korg track
    auto routed = note(3, 62, 0, 100);              // ch4 routed by CASM to ch15

    auto st = styleOf({
        section(T::Main1, "Main A", 1, {track(11, acc), track(9, drums, uasf::TrackRole::Drum), track(3, lowCh)}),
        section(T::Main2, "Main B", 1, {track(14, note(14, 70, 0, 100)), track(3, routed)}),
        section(T::Fill3, "Fill In CC", 1, {track(9, note(9, 38, 0, 10))}),
        section(T::Break, "Fill In BA", 10, {track(9, note(9, 38, 0, 10))}),
        section(T::Fill1, "Fill In AA", 0, {}),
    });

    MapOptions opt = MapOptions::raw();
    auto r = mapToKorg(st, opt);
    TEST("maps", r.ok);
    TEST("tempo 100 BPM -> 600000 us/q", r.style.mpqn == 600000);
    TEST("source PPQN kept", r.style.ppqn == 1920);
    TEST("mapped output validates", validate(r.style).empty());
    const Element* v1 = find(r.style, "v1cv1");
    TEST("Main A exported as v1cv1, 1 bar 4/4", v1 && v1->length == 7680 && v1->num == 4 && v1->den == 4);
    const Element* f3 = find(r.style, "f3cv1");
    TEST("Fill CC exported as f3cv1", f3 && f3->length == 7680);
    TEST("Break and zero-length Fill AA not exported", !find(r.style, "f1cv1") && !find(r.style, "br") &&
         r.style.elements.size() == 3);
    TEST("Break row explains why", r.report.sections.size() == 5 &&
         r.report.sections[3].marker.empty() && r.report.sections[3].note.find("Break") != std::string::npos);
    TEST("zero-length section reported", r.report.sections[4].note == "zero-length section");
    TEST("dropped sections counted (Break's 2 events)", r.report.dropped[kDropSection] == 2);

    TEST("Yamaha bank/PC dropped (3 events)", r.report.dropped[kDropVoiceSelect] == 3 &&
         count(*v1, 0xC0 | 11) == 0 && count(*v1, 0xB0 | 11, 0) == 0);
    TEST("Yamaha voice recorded in the report", r.report.tracks[0].src_ch == 3 &&
         [&] { for (auto& t : r.report.tracks) if (t.src_ch == 11) return t.msb == 63 && t.lsb == 120 && t.pc == 21 && t.target == "UNRESOLVED"; return false; }());
    TEST("CC11 expression kept at tick 0", count(*v1, 0xB0 | 11, 11) == 1);
    TEST("note 5 on Acc1 dropped (chord-event range)", r.report.dropped[kDropChordRange] == 1 && count(*v1, 0x90 | 11, 5) == 0);
    TEST("note 13 on Drum kept, reported non-GM", count(*v1, 0x90 | kDrumCh, 13) == 1 && r.report.non_gm_drum_notes[13] == 1);
    TEST("note crossing section end clamped to last tick", [&] {
        for (auto& e : v1->events) if (e.status == (0x80 | 11) && e.d1 == 64) return e.tick == 7679; return false; }());
    TEST("note starting on the last tick dropped", r.report.dropped[kDropLastTick] == 1 && count(*v1, 0x90 | 11, 67) == 0);
    TEST("ch4 without route dropped", r.report.dropped[kDropChannel] == 4 && count(*v1, 0x90 | 3) == 0);

    // Same synthetic style under default quality mode: remaps note 13 -> 86 (XG Surdo Mute) & sanitizes 63/120 MegaVoice.
    {
        auto rq = mapToKorg(st, MapOptions{});
        const Element* v1q = find(rq.style, "v1cv1");
        TEST("default quality mode remaps drum note 13 & sanitizes 63/120 MegaVoice",
             rq.ok && validate(rq.style).empty() && v1q &&
             count(*v1q, 0x90 | kDrumCh, 13) == 0 && count(*v1q, 0x90 | kDrumCh, 86) == 1 &&
             rq.report.drum_notes_remapped_approx == 1 && rq.report.megavoice_vel_reshaped > 0);
    }

    // CASM routing: ch4 -> ch15 lands on Korg Acc4 and merges with Phrase1.
    opt.route = {{3, 14}};
    r = mapToKorg(st, opt);
    const Element* v2 = find(r.style, "v2cv1");
    TEST("routed ch4 plays on Acc4", r.ok && v2 && count(*v2, 0x90 | 14, 62) == 1 && count(*v2, 0x90 | 14, 70) == 1);
    TEST("merge warning emitted", !r.report.warnings.empty() &&
         r.report.warnings[0].find("both play Korg Acc4") != std::string::npos);

    // User sound map: Bank/PC emitted at tick 0 of every element.
    opt.route.clear();
    opt.sounds[kAcc1Ch] = {121, 3, 40};
    r = mapToKorg(st, opt);
    TEST("user sound map emits bank+PC in every element", r.ok &&
         count(r.style.elements[0], 0xC0 | 11, 40) == 1 && count(r.style.elements[1], 0xC0 | 11, 40) == 1 &&
         r.style.elements[0].events[0].tick == 0 && validate(r.style).empty());

    // ── PPQN 1920 -> 480: triplets and 6/8, no drift ──
    std::vector<uasf::MidiEvent> trip;
    for (int i = 0; i < 18; ++i) {   // eighth-note triplets over 6/8 bars: 640 ticks each at 1920
        auto n = note(11, 60, i * 640, i * 640 + 320);
        trip.insert(trip.end(), n.begin(), n.end());
    }
    auto s68 = styleOf({section(T::Main1, "Main A", 2, {track(11, trip)}, 6, 8)});
    MapOptions o480;
    o480.ppqn = 480;
    r = mapToKorg(s68, o480);
    TEST("6/8 at 480 PPQN: 2 bars = 2880 ticks", r.ok && r.style.ppqn == 480 && r.style.elements[0].length == 2880 &&
         r.style.elements[0].num == 6 && r.style.elements[0].den == 8);
    TEST("triplet onsets land exactly on 160-tick grid", [&] {
        int k = 0;
        for (auto& e : r.style.elements[0].events)
            if ((e.status & 0xF0) == 0x90) { if (e.tick != static_cast<uint32_t>(k) * 160) return false; ++k; }
        return k == 18; }());
    { MapOptions o; o.ppqn = 7;   // 7*4*6/8 = 21 ticks per bar: representable
      TEST("odd PPQN with whole-tick 6/8 bar maps", mapToKorg(s68, o).ok); }
    { MapOptions o; o.ppqn = 5;   // 5*4*6/8 = 15: fine; 5*4*7/8 not
      auto s78 = styleOf({section(T::Main1, "Main A", 1, {}, 7, 8)});
      TEST("meter without a whole-tick bar is rejected", !mapToKorg(s78, o).ok); }

    // ── Report text ──
    r = mapToKorg(st, MapOptions{});
    const std::string txt = formatReport(r.report, r.style);
    TEST("report lists section map, tracks, loss", txt.find("Main A (1 bars) -> v1cv1") != std::string::npos &&
         txt.find("NOT EXPORTED") != std::string::npos && txt.find(kDropChordRange) != std::string::npos &&
         txt.find("UNRESOLVED") != std::string::npos);

    // ── No section maps -> error ──
    TEST("style with only Break fails", !mapToKorg(styleOf({section(T::Break, "Fill In BA", 1, {})}), {}).ok);

    // ── P1 regression: Drum/Percussion-role track routed to chord-driven Acc1 (kch=11) with pitch 0..11 ──
    {
        auto drumToAcc = cat(note(11, 5, 0, 240, 90), note(11, 60, 480, 960, 90));
        auto sDrumAcc = styleOf({section(T::Main1, "Main A", 1, {
            track(11, drumToAcc, uasf::TrackRole::Drum),
        })});
        auto rQual = mapToKorg(sDrumAcc, MapOptions{});
        auto rRaw = mapToKorg(sDrumAcc, MapOptions::raw());
        const Element* v1Qual = find(rQual.style, "v1cv1");
        const Element* v1Raw = find(rRaw.style, "v1cv1");
        TEST("P1 regression (quality): Drum-role track routed to Acc1 strips pitch 5, counts drop & validates",
             rQual.ok && validate(rQual.style).empty() && v1Qual &&
             count(*v1Qual, 0x90 | 11, 5) == 0 && count(*v1Qual, 0x90 | 11, 60) == 1 &&
             rQual.report.dropped[kDropChordRange] == 1 &&
             rQual.report.tracks.size() == 1 && rQual.report.tracks[0].chord_range_dropped == 1);
        TEST("P1 regression (raw): Drum-role track routed to Acc1 strips pitch 5, counts drop & validates",
             rRaw.ok && validate(rRaw.style).empty() && v1Raw &&
             count(*v1Raw, 0x90 | 11, 5) == 0 && count(*v1Raw, 0x90 | 11, 60) == 1 &&
             rRaw.report.dropped[kDropChordRange] == 1 &&
             rRaw.report.tracks.size() == 1 && rRaw.report.tracks[0].chord_range_dropped == 1);
    }

    // ── Wave-2B Q23-Wire, Wave-3 Q4-Wire & Wave-4b Q5-Wire E2E tests on real CC0 synthetic fixtures ──
    runQ23WireE2eTests();
    runQ4WireE2eTests();
    runQ5WireE2eTests();

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
