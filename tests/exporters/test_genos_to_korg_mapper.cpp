// Canonical (UASF) -> Korg Pa5X semantic model: section mapping, channel
// routing, voice-select policy, chord-event-range filter, boundary clamping,
// exact PPQN rescaling (triplets, 6/8), loss accounting, AND Wave-2B Q23-Wire
// E2E style conversion quality pipeline (CV1..CV6 split, XG/Revo! drum remap &
// Rhythm1/2 swap, MegaVoice/S.Art/Guitar sanitizer, Source-Chord normalizer).

#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/sff1_route.h"
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

    // ── Wave-2B Q23-Wire E2E tests on real CC0 synthetic fixtures ──
    runQ23WireE2eTests();

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
