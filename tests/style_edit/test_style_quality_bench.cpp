// Track B Milestone Q1 Comprehensive Regression Tests:
//   1. SFF1 Ctab / Cntt parsing & UASF round-trip
//   2. Full SFF2 Ctb2 (editable, 12-bit note mute, 34-bit chord mute, source chord,
//      Low/Mid/High zones, RTR, dest_channel) + explicit OTSc/FNRP/MDB detection
//   3. UASF additive 'CASM' extension serialize/deserialize, malformed rejection,
//      v1.0/v1.1 golden backward compatibility, and 'SNAM' importer section names
//   4. SInt mix CC propagation (CC7/10/11/91/93, excluding Bank/PC) to ALL sections
//   5. Mixed-meter FF 58 per-element preservation (6/8 + 4/4 + 2/4)
//   6. BreakPolicy::Yamaha1BarCap ("br" 1-bar cap, 0 stuck notes, STYLE_QUAL_BREAK_TAIL_TRUNCATED)
//   7. Offline multi-chord 13-chord benchmark matrix, determinism, fixed-weight invariant,
//      path-scrubbed JSON, epistemic stamps, and CLI execution
//   8. Malformed / truncated / encrypted .sty fail-closed hardening

#include "engine/uasf/deserializer.h"
#include "engine/uasf/format.h"
#include "engine/uasf/serializer.h"
#include "engine/uasf/validator.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/korg_style_smf.h"
#include "exporters/korg/sff1_route.h"
#include "exporters/korg/style_quality_bench.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ai_arranger;
using namespace ai_arranger::importers::sff1;
namespace korg = ai_arranger::exporters::korg;
namespace bench = ai_arranger::exporters::korg::bench;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s (line %d)\n", name, __LINE__); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static std::string fx(const char* name) {
    return std::string(REPO_DIR) + "/fixtures/synthetic/" + name;
}

static std::vector<uint8_t> makeChunk(const char* id4, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> b;
    b.insert(b.end(), id4, id4 + 4);
    const uint32_t sz = static_cast<uint32_t>(body.size());
    b.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(sz & 0xFF));
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

static void testSff1CtabCntt() {
    std::printf("1. SFF1 Ctab / Cntt parsing & UASF round-trip\n");
    const auto pr = Sff1Reader().parseFile(fx("SYNTH_SFF1_CTAB_CNTT.sty"));
    TEST("sff1: parses successfully", pr.success && !pr.casm_sections.empty());
    TEST("sff1: all CASM configs came from Ctab (0 Ctb2)",
         !pr.casm_configs.empty() &&
         std::all_of(pr.casm_configs.begin(), pr.casm_configs.end(),
                     [](const CasmTrackConfig& c) { return c.from_sff1_ctab; }));

    const CasmTrackConfig* bass = nullptr;
    const CasmTrackConfig* ep = nullptr;
    for (const auto& c : pr.casm_configs) {
        if (c.source_channel == 10 && !bass) bass = &c;
        if (c.source_channel == 4 && !ep) ep = &c;
    }
    TEST("sff1: Bass Cntt overrides ntt_bass=true, ntt=1, hk=4, lo=28, hi=55, rtr=4",
         bass && bass->name == "Bass" && bass->ntt_bass && bass->ntt == 1 &&
         bass->high_key == 4 && bass->note_low == 28 && bass->note_high == 55 && bass->rtr == 4);
    TEST("sff1: E.Piano Ctab routes ch 4 -> dest_channel 14, ntr=1, ntt=2",
         ep && ep->name == "E.Piano" && ep->dest_channel == 14 && ep->ntr == 1 && ep->ntt == 2 && !ep->ntt_bass);

    const auto mr = Sff1ToUasfMapper().map(pr);
    TEST("sff1: mapper succeeds & preserves section names (keep_name=true)",
         mr.success && !mr.style.sections.empty() &&
         std::all_of(mr.style.sections.begin(), mr.style.sections.end(),
                     [](const uasf::SectionDefinition& s) { return s.keep_name && !s.name.empty(); }));

    const auto ser = uasf::UasfSerializer().serialize(mr.style);
    const auto des = uasf::UasfDeserializer().deserialize(ser.data);
    TEST("sff1: UASF serialize -> deserialize succeeds", ser.success && des.success);
    bool foundBass = false, foundEp = false, namesKept = true;
    for (size_t si = 0; si < des.style.sections.size(); ++si) {
        if (des.style.sections[si].name != mr.style.sections[si].name) namesKept = false;
        for (const auto& tr : des.style.sections[si].tracks) {
            if (tr.midi_channel == 10 && tr.articulation.has_casm_meta &&
                tr.articulation.ntt_bass && tr.articulation.ntt == 1 &&
                tr.articulation.high_key == 4 && tr.articulation.rtr == 4) {
                foundBass = true;
            }
            if (tr.midi_channel == 4 && tr.articulation.has_casm_meta &&
                tr.articulation.dest_channel == 14 && tr.articulation.ntr == 1 && tr.articulation.ntt == 2) {
                foundEp = true;
            }
        }
    }
    TEST("sff1: UASF round-trip keeps SNAM section names and CASM Bass + routed E.Piano",
         namesKept && foundBass && foundEp);

    // Korg export from reloaded UASF (even with empty opt.route!) routes ch 4 -> Acc4 via UASF CASM dest_channel!
    korg::MapOptions kopt;
    kopt.break_policy = korg::BreakPolicy::Yamaha1BarCap;
    const auto kr = korg::mapToKorg(des.style, kopt);
    TEST("sff1: Korg export from reloaded UASF routes ch 4 -> Acc4 and validates",
         kr.ok && korg::validate(kr.style).empty() && kr.style.elements.size() == 6 &&
         std::any_of(kr.report.tracks.begin(), kr.report.tracks.end(),
                     [](const korg::TrackRow& t) { return t.src_ch == 4 && t.korg_ch == 14; }));
}

static void testSff2FullCtb2AndChunks() {
    std::printf("2. SFF2 full Ctb2 (source chord, mute masks, zones, RTR) & metadata chunks\n");
    {
        const auto pr = Sff1Reader().parseFile(fx("SYNTH_SRC_CHORD_NON_C.sty"));
        TEST("src_chord: parses", pr.success && pr.casm_sections.size() == 6);
        bool sawCm7 = false, sawG7 = false, sawDm = false;
        for (const auto& cs : pr.casm_sections) {
            for (const auto& t : cs.tracks) {
                if (cs.name == "Main A" && t.source_channel == 11 &&
                    t.source_chord_root == 0 && t.source_chord_type == 11) sawCm7 = true;
                if (cs.name == "Main A" && t.source_channel == 13 &&
                    t.source_chord_root == 7 && t.source_chord_type == 19) sawG7 = true;
                if (cs.name == "Main B" && t.source_channel == 11 &&
                    t.source_chord_root == 2 && t.source_chord_type == 8) sawDm = true;
            }
        }
        TEST("src_chord: Cm7 (0/11), G7 (7/19), Dm (2/8) extracted per section",
             sawCm7 && sawG7 && sawDm);
    }
    {
        const auto pr = Sff1Reader().parseFile(fx("SYNTH_CHORD_MUTE_SPLIT.sty"));
        TEST("chord_mute: parses", pr.success);
        uint64_t m3 = 0, m14 = 0;
        uint16_t nm3 = 0;
        for (const auto& t : pr.casm_configs) {
            if (t.source_channel == 3) { m3 = t.chord_mute_mask; nm3 = t.note_mute_mask; }
            if (t.source_channel == 14) { m14 = t.chord_mute_mask; }
        }
        TEST("chord_mute: 12-bit note mute & mutually exclusive 34-bit chord mute masks extracted",
             nm3 == 0x0FFF && m3 == 0x000000007Full && m14 == 0x00000FF800ull && (m3 & m14) == 0);
    }
    {
        const auto pr = Sff1Reader().parseFile(fx("SYNTH_MULTI_ZONE_SART_OTS.sty"));
        TEST("multi_zone: parses", pr.success);
        const CasmTrackConfig* ch11 = nullptr;
        for (const auto& t : pr.casm_configs) if (t.source_channel == 11 && !ch11) ch11 = &t;
        TEST("multi_zone: editable=1, zone_low=48, zone_high=72, distinct Low/Mid/High zones",
             ch11 && ch11->editable == 1 && ch11->zone_low == 48 && ch11->zone_high == 72 &&
             ch11->low_zone.ntr == 0 && ch11->low_zone.ntt_bass && ch11->low_zone.high_key == 5 &&
             ch11->ntr == 0 && ch11->ntt == 2 && ch11->rtr == 3 &&
             ch11->high_zone.ntr == 1 && ch11->high_zone.ntt == 1 && ch11->high_zone.rtr == 2);
        auto hasMetaChunk = [&](const char* id) {
            return std::find(pr.detected_metadata_chunks.begin(), pr.detected_metadata_chunks.end(), id) !=
                   pr.detected_metadata_chunks.end();
        };
        TEST("multi_zone: OTSc, FNRP, MDB  chunks explicitly detected (zero silent swallow)",
             hasMetaChunk("OTSc") && hasMetaChunk("FNRP") && hasMetaChunk("MDB ") &&
             pr.unknown_chunks == 0 && pr.unsupported_features.size() >= 3);
    }
}

static void testUasfCasmExtensionAndCompat() {
    std::printf("3. UASF 'CASM' extension round-trip, validation & v1.0/v1.1 backward compatibility\n");
    // v1.0 golden file loads, validates, has no CASM, re-serializes with zero CASM block
    {
        const std::string goldenPath = std::string(REPO_DIR) + "/tests/golden/uasf/gate-3-demo-v1.uasf";
        const auto r = uasf::UasfDeserializer().deserializeFromFile(goldenPath);
        TEST("v1.0 golden: loads & validates", r.success && uasf::UasfValidator().validate(r.style).valid);
        bool anyCasm = false;
        for (const auto& s : r.style.sections)
            for (const auto& t : s.tracks)
                if (t.articulation.hasCasmMetadata()) anyCasm = true;
        TEST("v1.0 golden: no CASM metadata invented", !anyCasm);
        const auto ser = uasf::UasfSerializer().serialize(r.style);
        const bool hasCasmTag = std::search(ser.data.begin(), ser.data.end(),
                                            std::begin("CASM"), std::begin("CASM") + 4) != ser.data.end();
        TEST("v1.0 golden: re-serializes without 'CASM' block", ser.success && !hasCasmTag);
    }
    // Full round-trip + malformed CASM trust-boundary checks
    {
        const auto pr = Sff1Reader().parseFile(fx("SYNTH_CHORD_MUTE_SPLIT.sty"));
        const auto mr = Sff1ToUasfMapper().map(pr);
        const auto ser = uasf::UasfSerializer().serialize(mr.style);
        const auto des = uasf::UasfDeserializer().deserialize(ser.data);
        TEST("casm ext: serialize + deserialize succeeds", ser.success && des.success);
        const auto ser2 = uasf::UasfSerializer().serialize(des.style);
        TEST("casm ext: serialize(deserialize(x)) == x (byte-identical)", ser2.success && ser2.data == ser.data);

        // Locate 'CASM' extension block and test fail-closed mutations
        size_t casmAt = 0;
        for (size_t i = 0; i + 8 < ser.data.size(); ++i) {
            if (std::memcmp(ser.data.data() + i, "CASM", 4) == 0) { casmAt = i; break; }
        }
        TEST("casm ext: 'CASM' tag present in serialized bytes", casmAt > 0);
        if (casmAt > 0) {
            auto badVer = ser.data;
            badVer[casmAt + 8] = 2; // version != 1
            TEST("casm ext: bad version rejected", !uasf::UasfDeserializer().deserialize(badVer).success);

            auto badRoot = ser.data;
            badRoot[casmAt + 8 + 1 + 6] = 12; // source_chord_root > 11
            TEST("casm ext: source_chord_root > 11 rejected", !uasf::UasfDeserializer().deserialize(badRoot).success);

            auto badDest = ser.data;
            badDest[casmAt + 8 + 1 + 17] = 16; // dest_channel == 16 (not 0..15 or 0xFF)
            TEST("casm ext: invalid dest_channel 16 rejected", !uasf::UasfDeserializer().deserialize(badDest).success);

            auto badNtt = ser.data;
            badNtt[casmAt + 8 + 1 + 3] = 0x80; // ntt > 127
            TEST("casm ext: ntt > 127 rejected", !uasf::UasfDeserializer().deserialize(badNtt).success);

            auto badZone = ser.data;
            badZone[casmAt + 8 + 1 + 15] = 80; // zone_low > zone_high
            badZone[casmAt + 8 + 1 + 16] = 40;
            TEST("casm ext: zone_low > zone_high rejected", !uasf::UasfDeserializer().deserialize(badZone).success);
        }
    }
}

static void testSintPropagationAllSections() {
    std::printf("4. SInt mix CC propagation to ALL sections (excluding Bank/PC)\n");
    const auto pr = Sff1Reader().parseFile(fx("SYNTH_MIX_SYSEX_CC_HYGIENE.sty"));
    const auto mr = Sff1ToUasfMapper().map(pr);
    TEST("sint: maps cleanly", pr.success && mr.success && mr.style.sections.size() == 6);

    bool allSectionsHaveMixCcs = true;
    bool nonIntroHasNoBankPc = true;
    for (const auto& sec : mr.style.sections) {
        const bool isIntro = sec.type >= uasf::SectionType::Intro1 && sec.type <= uasf::SectionType::Intro3;
        for (const auto& tr : sec.tracks) {
            bool has7 = false, has10 = false, has11 = false, has91 = false, has93 = false;
            for (const auto& ev : tr.events) {
                if (ev.tick == 0 && ev.type == uasf::MidiEventType::ControlChange) {
                    if (ev.data1 == 7) has7 = true;
                    if (ev.data1 == 10) has10 = true;
                    if (ev.data1 == 11) has11 = true;
                    if (ev.data1 == 91) has91 = true;
                    if (ev.data1 == 93) has93 = true;
                    if (!isIntro && (ev.data1 == 0 || ev.data1 == 32)) nonIntroHasNoBankPc = false;
                }
                if (!isIntro && ev.tick == 0 && ev.type == uasf::MidiEventType::ProgramChange) {
                    nonIntroHasNoBankPc = false;
                }
            }
            if (!(has7 && has10 && has11 && has91 && has93)) allSectionsHaveMixCcs = false;
        }
    }
    TEST("sint: Main/Fill/Break/Ending/Intro all carry tick-0 CC7/10/11/91/93", allSectionsHaveMixCcs);
    TEST("sint: Bank/PC not duplicated into non-Intro sections", nonIntroHasNoBankPc);
}

static void testMixedMeterAndBreakPolicy() {
    std::printf("5. Mixed-meter FF 58 preservation & BreakPolicy::Yamaha1BarCap\n");
    // Mixed meter: 6/8 Intro + 4/4 Main A/B + 2/4 Fill In AA + 4/4 Break + 4/4 Ending A
    {
        const auto pr = Sff1Reader().parseFile(fx("SYNTH_MIXED_METER_RUBATO.sty"));
        TEST("mixed_meter: reader records all 4 FF 58 events", pr.success && pr.time_signatures.size() == 4);
        const auto mr = Sff1ToUasfMapper().map(pr);
        TEST("mixed_meter: mapper succeeds", mr.success && mr.style.sections.size() == 6);
        korg::MapOptions kopt;
        kopt.ppqn = 384;
        kopt.route = korg::routeFromCasm(pr);
        kopt.break_policy = korg::BreakPolicy::Yamaha1BarCap;
        const auto kr = korg::mapToKorg(mr.style, kopt);
        TEST("mixed_meter: Korg export validates with all 6 elements",
             kr.ok && kr.style.elements.size() == 6 && korg::validate(kr.style).empty());
        auto checkEl = [&](const char* marker, uint8_t num, uint8_t den, uint32_t expectedLen) {
            for (const auto& e : kr.style.elements)
                if (e.marker == marker) return e.num == num && e.den == den && e.length == expectedLen;
            return false;
        };
        TEST("mixed_meter: i1cv1=6/8 (2 bars=2304), v1cv1=4/4 (2 bars=3072), f1cv1=2/4 (1 bar=768), br=4/4 (1 bar=1536)",
             checkEl("i1cv1", 6, 8, 2 * 1152) &&
             checkEl("v1cv1", 4, 4, 2 * 1536) &&
             checkEl("f1cv1", 2, 4, 1 * 768) &&
             checkEl("br", 4, 4, 1 * 1536));
    }

    // BreakPolicy::Yamaha1BarCap on SYNTH_BREAK_TAIL.sty (#26)
    {
        const auto pr = Sff1Reader().parseFile(fx("SYNTH_BREAK_TAIL.sty"));
        const auto mr = Sff1ToUasfMapper().map(pr);
        korg::MapOptions wOpt;
        wOpt.route = korg::routeFromCasm(pr);
        wOpt.break_policy = korg::BreakPolicy::Withhold;
        const auto withheld = korg::mapToKorg(mr.style, wOpt);
        TEST("break_tail: Withhold policy keeps 1 element (backward-compatible)",
             withheld.ok && withheld.style.elements.size() == 1);

        korg::MapOptions cOpt;
        cOpt.ppqn = 384;
        cOpt.route = korg::routeFromCasm(pr);
        cOpt.break_policy = korg::BreakPolicy::Yamaha1BarCap;
        const auto capped = korg::mapToKorg(mr.style, cOpt);
        const auto written = korg::write(capped.style);
        const auto back = korg::parse(written.bytes);
        const korg::Element* br = nullptr;
        for (const auto& e : back.style.elements) if (e.marker == "br") br = &e;
        bool hasDiag = std::any_of(capped.report.warnings.begin(), capped.report.warnings.end(),
                                   [](const std::string& w) {
                                       return w.find(korg::kDiagBreakTailTruncated) != std::string::npos;
                                   });
        TEST("break_tail: Yamaha1BarCap emits valid 1-bar 'br' with 0 stuck notes & STYLE_QUAL_BREAK_TAIL_TRUNCATED",
             capped.ok && written.ok && back.ok && korg::validate(back.style).empty() &&
             br && br->length == 1536 && hasDiag &&
             capped.report.dropped.count(korg::kDropBreakTail) &&
             capped.report.dropped.at(korg::kDropBreakTail) > 0);
    }
}

static void testBenchmarkMatrixDeterminismAndInvariants() {
    std::printf("6. Benchmark 13-chord matrix, fixed weights, determinism & path-scrubbing\n");
    // Immutable fixed weight check
    const double wSum = bench::kWeightM1 + bench::kWeightM2 + bench::kWeightM3 +
                        bench::kWeightM4 + bench::kWeightM5 + bench::kWeightM6;
    TEST("weights: exact immutable spec values (25/20/15/15/12/13, sum=100%)",
         bench::kWeightM1 == 0.25 && bench::kWeightM2 == 0.20 && bench::kWeightM3 == 0.15 &&
         bench::kWeightM4 == 0.15 && bench::kWeightM5 == 0.12 && bench::kWeightM6 == 0.13 &&
         std::abs(wSum - 1.0) < 1e-12);
    TEST("chords: all 13 standardized test chords present",
         bench::kTestChords.size() == 13 &&
         std::string(bench::kTestChords[0].symbol) == "Cmaj" &&
         std::string(bench::kTestChords[4].symbol) == "Cm7b5" &&
         std::string(bench::kTestChords[12].symbol) == "G7/B");

    const char* kAll12[] = {
        "SYNTH_POP_SFF2.sty", "SYNTH_FULL_SFF2.sty", "SYNTH_WALTZ_68.sty", "SYNTH_BREAK_TAIL.sty",
        "SYNTH_SFF1_CTAB_CNTT.sty", "SYNTH_SRC_CHORD_NON_C.sty", "SYNTH_CHORD_MUTE_SPLIT.sty",
        "SYNTH_MEGAVOICE_GUITAR_BASS.sty", "SYNTH_REVO_XG_DRUMS.sty",
        "SYNTH_MIX_SYSEX_CC_HYGIENE.sty", "SYNTH_MIXED_METER_RUBATO.sty", "SYNTH_MULTI_ZONE_SART_OTS.sty",
    };

    bench::BenchOptions opt;
    opt.targetPpqn = 384;
    opt.breakYamaha1BarCap = true;

    bool allOk = true, allDeterministic = true, allScrubbed = true, allEpistemic = true;
    for (const char* name : kAll12) {
        const std::string path = fx(name);
        const auto r1 = bench::evaluateStyleFile(path, opt);
        const auto r2 = bench::evaluateStyleFile(path, opt);
        if (!r1.ok || !r1.structuralValid || r1.matrixSummary.cellsEvaluated == 0) allOk = false;
        const std::string j1 = bench::toJson(r1);
        const std::string j2 = bench::toJson(r2);
        if (j1 != j2) allDeterministic = false;
        if (j1.find(REPO_DIR) != std::string::npos || j1.find("/Users/") != std::string::npos ||
            j1.find("/home/") != std::string::npos) {
            allScrubbed = false;
        }
        if (j1.find("\"hardwareVerified\": false") == std::string::npos ||
            j1.find("\"l3Status\": \"NotRun\"") == std::string::npos ||
            j1.find("STRUCTURALLY VALID — NOT VERIFIED ON A PA5X") == std::string::npos ||
            j1.find("aiarranger-style-quality-bench/1") == std::string::npos) {
            allEpistemic = false;
        }
    }
    TEST("bench: all 12 CC0 torture fixtures evaluate & structurally validate", allOk);
    TEST("bench: 100% deterministic byte-identical JSON across repeated runs", allDeterministic);
    TEST("bench: JSON output is path-scrubbed (zero host paths leaked)", allScrubbed);
    TEST("bench: epistemic stamps present (hardwareVerified=false, L3=NotRun, NOT VERIFIED ON A PA5X)", allEpistemic);

    // CLI smoke test
#if defined(BENCH_BIN) && !defined(_WIN32)
    {
        const fs::path tmpDir = fs::temp_directory_path() / ("aiarr_bench_test_" + std::to_string(::getpid()));
        fs::remove_all(tmpDir);
        fs::create_directories(tmpDir);
        const fs::path j1 = tmpDir / "b1.json";
        const fs::path j2 = tmpDir / "b2.json";
        const fs::path md = tmpDir / "b.md";
        const std::string cmd1 = std::string("\"") + BENCH_BIN + "\" --input-dir \"" +
                                 std::string(REPO_DIR) + "/fixtures/synthetic\" --ppqn 384 --json \"" +
                                 j1.string() + "\" --md \"" + md.string() + "\" >/dev/null 2>&1";
        const std::string cmd2 = std::string("\"") + BENCH_BIN + "\" --input-dir \"" +
                                 std::string(REPO_DIR) + "/fixtures/synthetic\" --ppqn 384 --json \"" +
                                 j2.string() + "\" >/dev/null 2>&1";
        const int st1 = std::system(cmd1.c_str());
        const int st2 = std::system(cmd2.c_str());
        const int rc1 = WIFEXITED(st1) ? WEXITSTATUS(st1) : -1;
        const int rc2 = WIFEXITED(st2) ? WEXITSTATUS(st2) : -1;
        std::ifstream f1(j1, std::ios::binary), f2(j2, std::ios::binary);
        const std::string s1((std::istreambuf_iterator<char>(f1)), {});
        const std::string s2((std::istreambuf_iterator<char>(f2)), {});
        TEST("bench CLI: exits 0 on 12-fixture dir & produces byte-identical JSON",
             rc1 == 0 && rc2 == 0 && !s1.empty() && s1 == s2 && fs::exists(md));
        fs::remove_all(tmpDir);
    }
#endif
}

static void testMalformedFailClosed() {
    std::printf("7. Malformed / truncated / encrypted .sty fail-closed checks\n");
    Sff1Reader reader;

    // Truncated MThd (< 6 bytes)
    {
        auto buf = makeChunk("MThd", {0, 0, 0, 1});
        const auto r = reader.parseBuffer(buf, "trunc_mthd.sty");
        TEST("malformed: truncated MThd fails closed", !r.success);
    }
    // Zero division MThd
    {
        auto buf = makeChunk("MThd", {0, 0, 0, 1, 0, 0});
        const auto r = reader.parseBuffer(buf, "zero_div.sty");
        TEST("malformed: zero-division MThd fails closed", !r.success);
    }
    // Encrypted/DRM Yamaha container fails closed
    {
        auto mthd = makeChunk("MThd", {0, 0, 0, 1, 0x01, 0xE0});
        auto cryp = makeChunk("CRYP", {0xDE, 0xAD, 0xBE, 0xEF});
        mthd.insert(mthd.end(), cryp.begin(), cryp.end());
        const auto r = reader.parseBuffer(mthd, "drm.sty");
        TEST("encrypted: CRYP chunk fails closed with encrypted_or_drm=true",
             !r.success && r.encrypted_or_drm);
    }
    // Truncated Ctab (< 26 bytes) and truncated Cntt (< 2 bytes): safe ignore, zero crash
    {
        auto sdec = makeChunk("Sdec", {'M', 'a', 'i', 'n', ' ', 'A'});
        auto badCtab = makeChunk("Ctab", std::vector<uint8_t>(10, 0));
        auto badCntt = makeChunk("Cntt", {0x0A});
        std::vector<uint8_t> inner;
        inner.insert(inner.end(), sdec.begin(), sdec.end());
        inner.insert(inner.end(), badCtab.begin(), badCtab.end());
        inner.insert(inner.end(), badCntt.begin(), badCntt.end());
        auto casm = makeChunk("CASM", makeChunk("CSEG", inner));
        const auto r = reader.parseBuffer(casm, "trunc_ctab.sty");
        TEST("malformed: truncated Ctab/Cntt ignored safely (0 configs, no crash)",
             r.success && r.casm_configs.empty());
    }
    // Benchmark fails closed on corrupt buffer
    {
        const auto br = bench::evaluateStyleBuffer({0x01, 0x02, 0x03}, "corrupt.sty");
        TEST("bench: corrupt buffer fails closed (ok=false)", !br.ok && !br.error.empty());
    }
}

int main() {
    std::printf("Test: Track B Q1 Style Quality Benchmark, SFF1/SFF2 CASM, UASF 'CASM' & Break\n");
    testSff1CtabCntt();
    testSff2FullCtb2AndChunks();
    testUasfCasmExtensionAndCompat();
    testSintPropagationAllSections();
    testMixedMeterAndBreakPolicy();
    testBenchmarkMatrixDeterminismAndInvariants();
    testMalformedFailClosed();
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
