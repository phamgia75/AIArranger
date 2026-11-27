// Track B Milestone Q1 Comprehensive Regression Tests (incl. Audit Round 1 P1/P2/P3 checks):
//   1. SFF1 Ctab / Cntt parsing, section-scoped Cntt (P3-2) & UASF round-trip
//   2. Full SFF2 Ctb2 (editable, 12-bit note mute, 34-bit chord mute, source chord,
//      Low/Mid/High zones, RTR, dest_channel) + explicit OTSc/FNRP/MDB detection
//   3. UASF additive 'CASM' v1 (34-byte records incl. editable + Low/High zones, P2-3),
//      malformed rejection, v1.0/v1.1 golden backward compatibility & 'SNAM' names
//   4. SInt mix CC propagation (CC7/10/11/91/93, excluding Bank/PC) to ALL sections
//   5. Mixed-meter FF 58 per-element preservation (6/8 + 4/4 + 2/4) & BreakPolicy::Yamaha1BarCap
//   6. Offline 13-chord benchmark matrix, fixed weights, determinism, path-scrubbing,
//      P1-1 (no side-channel UASF credit + slash-chord bass-inversion math),
//      P1-2 (honest M6 scoring + targetKstEvaluated),
//      P1-3 (M2.3 source-velocity bucketing: raw fails, Q2-sanitized passes!),
//      P1-4 (corpusSummary + activeComposite + corpusGrade),
//      P1-5 (M3.4 p99DriftMs detects +40 ms exported timing shift!),
//      P2-1 (M4.2/M4.4 drum family preservation & velocity correlation under Q3 remap),
//      P2-2 (active-channel CASM preservation = 1.0, zero false warnings),
//      P2-5 (toMarkdown emits INVALID label when !structuralValid)
//   7. Malformed / truncated / encrypted .sty fail-closed hardening

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

static std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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
    std::printf("1. SFF1 Ctab / Cntt parsing, section-scoped Cntt (P3-2) & UASF round-trip\n");
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

    // P3-2: Cntt in section 2 for a channel absent in section 2 must NOT clobber section 1's Ctab!
    {
        std::vector<uint8_t> ctab1(33, 0);
        ctab1[0] = 10;
        std::memcpy(&ctab1[1], "Bass    ", 8);
        ctab1[9] = 10; ctab1[20] = 0; ctab1[21] = 0x01; ctab1[22] = 5; ctab1[23] = 28; ctab1[24] = 55;
        std::vector<uint8_t> s1;
        auto sd1 = makeChunk("Sdec", {'M', 'a', 'i', 'n', ' ', 'A'});
        auto ct1 = makeChunk("Ctab", ctab1);
        s1.insert(s1.end(), sd1.begin(), sd1.end());
        s1.insert(s1.end(), ct1.begin(), ct1.end());

        std::vector<uint8_t> s2;
        auto sd2 = makeChunk("Sdec", {'M', 'a', 'i', 'n', ' ', 'B'});
        auto cn2 = makeChunk("Cntt", {10, 0x82}); // ch 10 not present in Main B!
        s2.insert(s2.end(), sd2.begin(), sd2.end());
        s2.insert(s2.end(), cn2.begin(), cn2.end());
        auto cseg2 = makeChunk("CSEG", s2);
        s1.insert(s1.end(), cseg2.begin(), cseg2.end());

        auto casm = makeChunk("CASM", makeChunk("CSEG", s1));
        const auto r = Sff1Reader().parseBuffer(casm, "cntt_scope.sty");
        TEST("P3-2: Cntt in section 2 does not clobber section 1's Ctab config",
             r.success && r.casm_configs.size() == 1 && !r.casm_configs[0].ntt_bass && r.casm_configs[0].ntt == 1);
    }

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
    std::printf("3. UASF 'CASM' v1 (34-byte incl. editable + Low/High zones, P2-3) & golden compat\n");
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
    // P2-3: Multi-zone editable + low_zone + high_zone full lossless UASF round-trip!
    {
        const auto pr = Sff1Reader().parseFile(fx("SYNTH_MULTI_ZONE_SART_OTS.sty"));
        const auto mr = Sff1ToUasfMapper().map(pr);
        const auto ser = uasf::UasfSerializer().serialize(mr.style);
        const auto des = uasf::UasfDeserializer().deserialize(ser.data);
        TEST("P2-3 multi-zone: serialize + deserialize succeeds", ser.success && des.success);
        bool zoneRoundTripOk = false;
        for (const auto& sec : des.style.sections) {
            for (const auto& tr : sec.tracks) {
                if (tr.midi_channel != 11) continue;
                const auto& a = tr.articulation;
                if (a.has_casm_meta && a.casm_editable == 1 && a.zone_low == 48 && a.zone_high == 72 &&
                    a.low_zone.ntr == 0 && a.low_zone.ntt == 1 && a.low_zone.ntt_bass &&
                    a.low_zone.high_key == 5 && a.low_zone.note_low == 28 && a.low_zone.note_high == 52 && a.low_zone.rtr == 1 &&
                    a.ntr == 0 && a.ntt == 2 && a.rtr == 3 &&
                    a.high_zone.ntr == 1 && a.high_zone.ntt == 1 && !a.high_zone.ntt_bass &&
                    a.high_zone.high_key == 11 && a.high_zone.note_low == 72 && a.high_zone.note_high == 96 && a.high_zone.rtr == 2) {
                    zoneRoundTripOk = true;
                }
            }
        }
        TEST("P2-3 multi-zone: editable + Low/Mid/High zones survive UASF round-trip 100% losslessly", zoneRoundTripOk);

        const auto ser2 = uasf::UasfSerializer().serialize(des.style);
        TEST("casm ext: serialize(deserialize(x)) == x (byte-identical)", ser2.success && ser2.data == ser.data);

        // Locate 'CASM' block and test strict validation mutations
        size_t casmAt = 0;
        for (size_t i = 0; i + 8 < ser.data.size(); ++i) {
            if (std::memcmp(ser.data.data() + i, "CASM", 4) == 0) { casmAt = i; break; }
        }
        TEST("casm ext: 'CASM' tag present in serialized bytes", casmAt > 0);
        if (casmAt > 0) {
            auto badVer = ser.data; badVer[casmAt + 8] = 2;
            TEST("casm ext: bad version rejected", !uasf::UasfDeserializer().deserialize(badVer).success);

            auto badRoot = ser.data; badRoot[casmAt + 8 + 1 + 6] = 12;
            TEST("casm ext: source_chord_root > 11 rejected", !uasf::UasfDeserializer().deserialize(badRoot).success);

            auto badDest = ser.data; badDest[casmAt + 8 + 1 + 17] = 16;
            TEST("casm ext: invalid dest_channel 16 rejected", !uasf::UasfDeserializer().deserialize(badDest).success);

            auto badNtt = ser.data; badNtt[casmAt + 8 + 1 + 3] = 0x80;
            TEST("casm ext: ntt > 127 rejected", !uasf::UasfDeserializer().deserialize(badNtt).success);

            auto badZone = ser.data; badZone[casmAt + 8 + 1 + 15] = 80; badZone[casmAt + 8 + 1 + 16] = 40;
            TEST("casm ext: zone_low > zone_high rejected", !uasf::UasfDeserializer().deserialize(badZone).success);

            auto badLowHk = ser.data; badLowHk[casmAt + 8 + 1 + 19 + 3] = 12; // low_zone.high_key > 11
            TEST("casm ext: low_zone high_key > 11 rejected", !uasf::UasfDeserializer().deserialize(badLowHk).success);

            auto badHighLoHi = ser.data; badHighLoHi[casmAt + 8 + 1 + 26 + 4] = 90; badHighLoHi[casmAt + 8 + 1 + 26 + 5] = 40;
            TEST("casm ext: high_zone note_low > note_high rejected", !uasf::UasfDeserializer().deserialize(badHighLoHi).success);
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
        TEST("mixed_meter: i1cv1=6/8 (2304), v1cv1=4/4 (3072), f1cv1=2/4 (768), br=4/4 (1536)",
             checkEl("i1cv1", 6, 8, 2 * 1152) &&
             checkEl("v1cv1", 4, 4, 2 * 1536) &&
             checkEl("f1cv1", 2, 4, 1 * 768) &&
             checkEl("br", 4, 4, 1 * 1536));
    }
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

static void testAuditCalibrationAndBenchmarkInvariants() {
    std::printf("6. Benchmark calibration (P1-1..P1-5, P2-1/P2-2/P2-5), fixed weights & determinism\n");

    // Immutable fixed weights invariant
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

    // P1-1: Bass inversion math on G7/B shifts root trigger C (36) -> B (47 or 35),
    // while 5th G (43) shifts by rootPc (+7 -> D = 50 or 38), NEVER by bassPc (+11 -> F#)!
    {
        bench::Pa5xTrackParams p{};
        p.hasOverride = true;
        p.bassInversion = true;
        p.wrapKey = 5;
        const auto& g7b = bench::kTestChords[12]; // G7/B (rootPc=7, bassPc=11)
        const uint8_t rootOut = bench::detail::simulatePa5xNote(36, korg::kBassCh, false, g7b, p);
        const uint8_t fifthOut = bench::detail::simulatePa5xNote(43, korg::kBassCh, false, g7b, p);
        TEST("P1-1 bass inversion math: G7/B maps C->B (pc 11) and G->D (pc 2, NOT F#)",
             (rootOut % 12) == 11 && (fifthOut % 12) == 2);
    }

    const char* kAll12[] = {
        "SYNTH_POP_SFF2.sty", "SYNTH_FULL_SFF2.sty", "SYNTH_WALTZ_68.sty", "SYNTH_BREAK_TAIL.sty",
        "SYNTH_SFF1_CTAB_CNTT.sty", "SYNTH_SRC_CHORD_NON_C.sty", "SYNTH_CHORD_MUTE_SPLIT.sty",
        "SYNTH_MEGAVOICE_GUITAR_BASS.sty", "SYNTH_REVO_XG_DRUMS.sty",
        "SYNTH_MIX_SYSEX_CC_HYGIENE.sty", "SYNTH_MIXED_METER_RUBATO.sty", "SYNTH_MULTI_ZONE_SART_OTS.sty",
    };

    bench::BenchOptions opt;
    opt.targetPpqn = 384;
    opt.breakYamaha1BarCap = true;

    std::vector<bench::BenchResult> allResults;
    std::vector<bench::BenchResult> rawResults;
    bool allOk = true, allDeterministic = true, allScrubbed = true, allEpistemic = true;
    bool allCasmActiveOk = true, allRawM6Honest = true, allLiveM6Lifted = true;
    for (const char* name : kAll12) {
        const std::string path = fx(name);
        const auto r1 = bench::evaluateStyleFile(path, opt);
        const auto r2 = bench::evaluateStyleFile(path, opt);
        if (!r1.ok || !r1.structuralValid || r1.matrixSummary.cellsEvaluated == 0) allOk = false;
        // P2-2: Active-channel CASM preservation must be 100% with zero false STYLE_QUAL_CASM_LOST!
        if (r1.m1.casmPreservationRate < 0.999) allCasmActiveOk = false;
        for (const auto& d : r1.diagnostics) if (d.code == "STYLE_QUAL_CASM_LOST") allCasmActiveOk = false;
        // Q4-Wire live M6 lift: real curated sound matching lifts M6 while keeping targetKstStatus=NOT_TESTED & l3Status=NOT_TESTED!
        if (r1.m6.score < 70.0 || r1.m6.targetKstEvaluated || r1.m6.targetKstValid ||
            r1.m6.targetKstStatus != bench::BenchStatus::NotTested || r1.l3Status != bench::BenchStatus::NotTested) {
            allLiveM6Lifted = false;
        }
        // P1-2 (raw NegCtrl): MapOptions::raw() with 0 resolved sounds MUST still keep M6 <= 15.01!
        {
            const auto rawBytes = readFileBytes(path);
            const auto pr = Sff1Reader().parseBuffer(rawBytes, name);
            const auto mr = Sff1ToUasfMapper().map(pr);
            korg::MapOptions rawOpt = korg::MapOptions::raw();
            rawOpt.ppqn = opt.targetPpqn;
            rawOpt.route = korg::routeFromCasm(pr);
            rawOpt.break_policy = korg::BreakPolicy::Yamaha1BarCap;
            const auto rawKr = korg::mapToKorg(mr.style, rawOpt);
            const auto rRaw = bench::evaluateExportedStyleAgainstSty(rawBytes, rawKr.style, rawKr.report, name, opt);
            if (!rRaw.ok || rRaw.m6.score > 15.01 || rRaw.m6.targetKstEvaluated || rRaw.m6.targetKstValid ||
                rRaw.m6.targetKstStatus != bench::BenchStatus::NotTested || rRaw.l3Status != bench::BenchStatus::NotTested) {
                allRawM6Honest = false;
            }
            rawResults.push_back(rRaw);
        }

        const std::string j1 = bench::toJson(r1);
        const std::string j2 = bench::toJson(r2);
        if (j1 != j2) allDeterministic = false;
        if (j1.find(REPO_DIR) != std::string::npos || j1.find("/Users/") != std::string::npos ||
            j1.find("/home/") != std::string::npos) allScrubbed = false;
        if (j1.find("\"hardwareVerified\": false") == std::string::npos ||
            j1.find("\"l3Status\": \"NOT_TESTED\"") == std::string::npos ||
            j1.find("STRUCTURALLY VALID — NOT VERIFIED ON A PA5X") == std::string::npos ||
            j1.find("aiarranger-style-quality-bench/1") == std::string::npos) allEpistemic = false;
        allResults.push_back(r1);
    }
    TEST("bench: all 12 CC0 torture fixtures evaluate & structurally validate", allOk);
    TEST("P2-2: active-channel CASM preservation = 1.0 across all 12 fixtures (zero false CASM_LOST)", allCasmActiveOk);
    TEST("P1-2 (raw NegCtrl): honest M6 <= 15.0 with 0 resolved sounds & targetKstStatus=NOT_TESTED, l3Status=NOT_TESTED", allRawM6Honest);
    TEST("Q4-Wire (live): M6 >= 70.0 across all 12 fixtures while keeping targetKstStatus=NOT_TESTED & l3Status=NOT_TESTED", allLiveM6Lifted);
    TEST("bench: 100% deterministic byte-identical JSON across repeated runs", allDeterministic);
    TEST("bench: JSON output is path-scrubbed (zero host paths leaked)", allScrubbed);
    TEST("bench: epistemic stamps present (hardwareVerified=false, l3Status=NOT_TESTED)", allEpistemic);

    // P1-1: Verify raw unsanitized SYNTH_MEGAVOICE_GUITAR_BASS still triggers benchmark penalties
    // (no side-channel UASF folding/Bass Inversion credit), AND live Q23-Wire default cleanly resolves them!
    {
        const std::string megaPath = fx("SYNTH_MEGAVOICE_GUITAR_BASS.sty");
        const auto rawBytes = readFileBytes(megaPath);
        const auto pr = Sff1Reader().parseBuffer(rawBytes, "SYNTH_MEGAVOICE_GUITAR_BASS.sty");
        const auto mr = Sff1ToUasfMapper().map(pr);
        korg::MapOptions rawOpt = korg::MapOptions::raw();
        rawOpt.ppqn = 384;
        rawOpt.route = korg::routeFromCasm(pr);
        rawOpt.break_policy = korg::BreakPolicy::Yamaha1BarCap;
        const auto rawKr = korg::mapToKorg(mr.style, rawOpt);
        const auto rawEval = bench::evaluateExportedStyleAgainstSty(
            rawBytes, rawKr.style, rawKr.report, "SYNTH_MEGAVOICE_GUITAR_BASS.sty", opt);
        TEST("P1-1 (raw NegCtrl): unsanitized SYNTH_MEGAVOICE_GUITAR_BASS keeps registerViolationRate > 0.15, bassSlash < 0.80 & velocityInversions == 2",
             rawEval.ok && rawEval.m1.registerViolationRate > 0.15 &&
             rawEval.m1.bassSlashAccuracy < 0.80 && rawEval.m2.velocityInversions == 2);

        const auto& megaRes = allResults[7];
        TEST("P1-1 (Q23-Wire live): default pipeline resolves SYNTH_MEGAVOICE_GUITAR_BASS (0 leaked FX, 0 velocity inversions, M2 PASS)",
             megaRes.fixtureName == "SYNTH_MEGAVOICE_GUITAR_BASS.sty" &&
             megaRes.m2.leakedChordRange == 0 && megaRes.m2.leakedFxNotesCount == 0 &&
             megaRes.m2.velocityInversions == 0 && megaRes.m1.registerViolationRate < 0.02 &&
             megaRes.m2.status == bench::BenchStatus::Pass);
    }

    // P1-2: Explicit --target-kst nonexistent sets targetKstEvaluated=true, targetKstValid=false, targetKstStatus=FAIL, m6.score=0
    {
        bench::BenchOptions kstOpt = opt;
        kstOpt.targetKstPath = "/nonexistent/path/Target.KST";
        const auto rk = bench::evaluateStyleFile(fx("SYNTH_POP_SFF2.sty"), kstOpt);
        TEST("P1-2: bad --target-kst reports targetKstEvaluated=true, targetKstValid=false, targetKstStatus=FAIL, m6=0",
             rk.ok && rk.m6.targetKstEvaluated && !rk.m6.targetKstValid &&
             rk.m6.targetKstStatus == bench::BenchStatus::Fail && rk.m6.score == 0.0);
    }

    // P1-3: M2.3 velocity-zone bucketing by SOURCE velocity (raw & re-inverted fail; Q23-Wire live passes!)
    {
        const std::string megaPath = fx("SYNTH_MEGAVOICE_GUITAR_BASS.sty");
        const auto rawBytes = readFileBytes(megaPath);
        const auto pr = Sff1Reader().parseBuffer(rawBytes, "SYNTH_MEGAVOICE_GUITAR_BASS.sty");
        const auto mr = Sff1ToUasfMapper().map(pr);
        korg::MapOptions rawOpt = korg::MapOptions::raw();
        rawOpt.ppqn = 384;
        rawOpt.route = korg::routeFromCasm(pr);
        rawOpt.break_policy = korg::BreakPolicy::Yamaha1BarCap;
        auto kr = korg::mapToKorg(mr.style, rawOpt);
        korg::Style sanitized = kr.style;
        korg::Style brokenInv = kr.style;
        for (auto& el : sanitized.elements) {
            std::vector<korg::Event> clean;
            for (auto ev : el.events) {
                uint8_t ch = ev.status & 0x0F;
                uint8_t hi = ev.status & 0xF0;
                if ((ch == korg::kBassCh || ch == 12) && (hi == 0x90 || hi == 0x80) && ev.d1 >= 96) continue;
                if ((ch == korg::kBassCh || ch == 12) && hi == 0x90 && ev.d2 > 0) {
                    if (ev.d2 <= 64) ev.d2 = 88;
                    else if (ev.d2 >= 91) ev.d2 = 42;
                }
                clean.push_back(ev);
            }
            el.events = std::move(clean);
        }
        for (auto& el : brokenInv.elements) {
            std::vector<korg::Event> inv;
            for (auto ev : el.events) {
                uint8_t ch = ev.status & 0x0F;
                uint8_t hi = ev.status & 0xF0;
                if ((ch == korg::kBassCh || ch == 12) && (hi == 0x90 || hi == 0x80) && ev.d1 >= 96) continue;
                if ((ch == korg::kBassCh || ch == 12) && hi == 0x90 && ev.d2 > 0) {
                    if (ev.d2 <= 64) ev.d2 = 45;
                    else if (ev.d2 >= 91) ev.d2 = 115;
                }
                inv.push_back(ev);
            }
            el.events = std::move(inv);
        }
        const auto q2Eval = bench::evaluateExportedStyleAgainstSty(
            rawBytes, sanitized, kr.report, "SYNTH_MEGAVOICE_GUITAR_BASS.sty", opt);
        const auto invEval = bench::evaluateExportedStyleAgainstSty(
            rawBytes, brokenInv, kr.report, "SYNTH_MEGAVOICE_GUITAR_BASS.sty", opt);
        TEST("P1-3: Q2-sanitized MegaVoice (src 1..64 -> 88, src 91..127 -> 42) passes with velocityInversions==0 & leakedFx==0",
             q2Eval.ok && q2Eval.structuralValid && q2Eval.m2.velocityInversions == 0 &&
             q2Eval.m2.leakedFxNotesCount == 0 && q2Eval.m2.velocityZoneStatus == bench::BenchStatus::Pass &&
             q2Eval.m2.status == bench::BenchStatus::Pass);
        TEST("P1-3 NegCtrl: deliberately inverted MegaVoice velocities flag velocityInversions==2 & FAIL even with 0 FX leaks",
             invEval.ok && invEval.m2.leakedFxNotesCount == 0 && invEval.m2.velocityInversions == 2 &&
             invEval.m2.velocityZoneStatus == bench::BenchStatus::Fail && invEval.m2.status == bench::BenchStatus::Fail &&
             invEval.m2.score < q2Eval.m2.score - 20.0);
    }

    // P1-4 + R2-P1-1/R2-P1-2: CorpusSummary coverage, N/A exclusions, unsupportedFeatures, notTestedItems & activeComposite
    {
        const auto rawCs = bench::computeCorpusSummary(rawResults);
        TEST("P1-4 + R2-P1-2 (raw NegCtrl): raw corpus keeps activeComposite < meanComposite < 85.0 & corpusGrade=FAIL",
             rawCs.fixtureCount == 12 && rawCs.activeComposite < rawCs.meanComposite &&
             rawCs.meanComposite < 85.0 && rawCs.hardInvariantViolations > 0 && rawCs.corpusGrade == bench::kStatusFail);

        const auto cs = bench::computeCorpusSummary(allResults);
        TEST("P1-4 + R2-P1-2 (Q5-Wire live): corpusSummary reports hardViol==0, M5/M6/composite lift, M2 N/A excluded (9), notTestedItems, corpusGrade=PASS",
             cs.fixtureCount == 12 && cs.activeComposite < cs.meanComposite &&
             cs.meanComposite >= 94.82 && cs.activeComposite >= 87.52 && cs.minComposite >= 86.57 &&
             cs.meanDimensions[4] >= 98.0 && cs.meanDimensions[5] >= 99.9 &&
             cs.hardInvariantViolations == 0 && cs.corpusGrade == bench::kStatusPass &&
             cs.evaluatedDimensionCounts[1] == 3 && cs.naExcludedCounts[1] == 9 &&
             cs.targetKstNotTestedCount == 12 && cs.l3Status == bench::BenchStatus::NotTested &&
             cs.notTestedItems.size() == 2 &&
             cs.unsupportedTotalOccurrences > 0 && cs.fixturesWithUnsupported > 0);
    }

    // P1-5: p99DriftMs detects +40 ms timing shift in actual exported SMF and drops M3 score
    {
        const std::string popPath = fx("SYNTH_POP_SFF2.sty");
        const auto rawBytes = readFileBytes(popPath);
        const auto pr = Sff1Reader().parseBuffer(rawBytes, "SYNTH_POP_SFF2.sty");
        const auto mr = Sff1ToUasfMapper().map(pr);
        korg::MapOptions kopt;
        kopt.ppqn = 384;
        kopt.route = korg::routeFromCasm(pr);
        kopt.break_policy = korg::BreakPolicy::Yamaha1BarCap;
        auto kr = korg::mapToKorg(mr.style, kopt);
        const auto baseEval = bench::evaluateExportedStyleAgainstSty(
            rawBytes, kr.style, kr.report, "SYNTH_POP_SFF2.sty", opt);
        korg::Style drifted = kr.style;
        for (auto& el : drifted.elements) {
            if (el.marker != "v1cv1") continue;
            for (auto& ev : el.events) {
                uint8_t hi = ev.status & 0xF0;
                if ((hi == 0x90 || hi == 0x80) && ev.tick + 31 < el.length - 1) ev.tick += 31;
            }
            std::stable_sort(el.events.begin(), el.events.end(),
                             [](const korg::Event& a, const korg::Event& b) { return a.tick < b.tick; });
        }
        const auto driftEval = bench::evaluateExportedStyleAgainstSty(
            rawBytes, drifted, kr.report, "SYNTH_POP_SFF2.sty", opt);
        TEST("P1-5: +40 ms shifted exported SMF reports p99DriftMs >= 39.0, timingDriftStatus=FAIL & drops M3 score",
             baseEval.ok && driftEval.ok && driftEval.m3.p99DriftMs >= 39.0 &&
             driftEval.m3.timingDriftStatus == bench::BenchStatus::Fail &&
             driftEval.m3.score < baseEval.m3.score - 10.0);
    }

    // P2-1: Q3-style family-preserving drum remap lifts familyPreservationRate to 1.0 & keeps velCorrelation >= 0.99
    {
        const std::string drumPath = fx("SYNTH_REVO_XG_DRUMS.sty");
        const auto rawBytes = readFileBytes(drumPath);
        const auto pr = Sff1Reader().parseBuffer(rawBytes, "SYNTH_REVO_XG_DRUMS.sty");
        const auto mr = Sff1ToUasfMapper().map(pr);
        korg::MapOptions kopt = korg::MapOptions::raw();
        kopt.ppqn = 384;
        kopt.route = korg::routeFromCasm(pr);
        kopt.break_policy = korg::BreakPolicy::Yamaha1BarCap;
        auto kr = korg::mapToKorg(mr.style, kopt);
        korg::Style remapped = kr.style;
        auto mapToGm = [](uint8_t p) -> uint8_t {
            if (p >= 35 && p <= 81) return p;
            char fam = bench::detail::sourceDrumFamilyOf(p);
            switch (fam) {
                case 'B': return 36;
                case 'S': return 38;
                case 'H': return 42;
                case 'T': return 45;
                case 'C': return 49;
                default:  return 70; // Latin/Perc
            }
        };
        for (auto& el : remapped.elements) {
            for (auto& ev : el.events) {
                uint8_t ch = ev.status & 0x0F;
                uint8_t hi = ev.status & 0xF0;
                if ((ch == korg::kDrumCh || ch == korg::kPercCh) && (hi == 0x90 || hi == 0x80)) {
                    ev.d1 = mapToGm(ev.d1);
                }
            }
        }
        const auto drumEval = bench::evaluateExportedStyleAgainstSty(
            rawBytes, remapped, kr.report, "SYNTH_REVO_XG_DRUMS.sty", opt);
        TEST("P2-1: Q3-style family-preserving drum remap lifts familyPreservationRate to 1.0 & keeps velCorrelation >= 0.99",
             drumEval.ok && drumEval.m4.familyPreservationRate >= 0.999 && drumEval.m4.velCorrelation >= 0.99);
    }

    // P2-5: toMarkdown emits STRUCTURALLY INVALID label when !structuralValid
    {
        bench::BenchResult invalidRes;
        invalidRes.ok = true;
        invalidRes.fixtureName = "BAD_STYLE.sty";
        invalidRes.structuralValid = false;
        const std::string md = bench::toMarkdown({invalidRes});
        TEST("P2-5: toMarkdown emits STRUCTURALLY INVALID label when !structuralValid",
             md.find(bench::kInvalidLabel) != std::string::npos &&
             md.find(bench::kValidLabel) == std::string::npos);
    }

    // CLI smoke test (verifying full R2-P1-2 corpusSummary in batchToJson!)
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
        TEST("bench CLI: exits 0, byte-identical JSON, emits 5-state taxonomy, coverage, naExclusions, unsupportedFeatures & notTestedItems",
             rc1 == 0 && rc2 == 0 && !s1.empty() && s1 == s2 && fs::exists(md) &&
             s1.find("\"statusTaxonomy\"") != std::string::npos &&
             s1.find("\"l3Status\": \"NOT_TESTED\"") != std::string::npos &&
             s1.find("\"corpusSummary\"") != std::string::npos &&
             s1.find("\"coverage\"") != std::string::npos &&
             s1.find("\"naExclusions\"") != std::string::npos &&
             s1.find("\"unsupportedFeatures\"") != std::string::npos &&
             s1.find("\"notTestedItems\"") != std::string::npos &&
             s1.find("\"L3_PA5X_HARDWARE_VALIDATION\"") != std::string::npos &&
             s1.find("\"M6_TARGET_KST_OCCUPANCY\"") != std::string::npos &&
             s1.find("\"activeComposite\"") != std::string::npos &&
             s1.find("\"hardInvariantViolations\": 0") != std::string::npos &&
             s1.find("\"corpusGrade\": \"PASS\"") != std::string::npos);
        fs::remove_all(tmpDir);
    }
#endif
}

// R2-P2-1: Self-contained negative controls across ALL 6 dimensions + 5-state taxonomy verification
static void testNegativeControlsAllDimensions() {
    std::printf("7. R2-P2-1 Negative Controls Across All 6 Dimensions & 5-State Taxonomy\n");
    bench::BenchOptions opt;
    opt.targetPpqn = 384;
    opt.breakYamaha1BarCap = true;

    const std::string popPath = fx("SYNTH_POP_SFF2.sty");
    const auto rawBytes = readFileBytes(popPath);
    const auto pr = Sff1Reader().parseBuffer(rawBytes, "SYNTH_POP_SFF2.sty");
    const auto mr = Sff1ToUasfMapper().map(pr);
    korg::MapOptions kopt;
    kopt.ppqn = 384;
    kopt.route = korg::routeFromCasm(pr);
    kopt.break_policy = korg::BreakPolicy::Yamaha1BarCap;
    const auto kr = korg::mapToKorg(mr.style, kopt);
    const auto base = bench::evaluateExportedStyleAgainstSty(rawBytes, kr.style, kr.report, "SYNTH_POP_SFF2.sty", opt);
    const auto baseCs = bench::computeCorpusSummary({base});

    // M1 Negative Control: inject clashing out-of-chord pitch (F# = 66 into C-major chord) -> clashRate rises & M1 drops!
    {
        korg::Style m1Bad = kr.style;
        for (auto& el : m1Bad.elements) {
            for (auto& ev : el.events) {
                uint8_t ch = ev.status & 0x0F;
                uint8_t hi = ev.status & 0xF0;
                if (ch == 11 && (hi == 0x90 || hi == 0x80)) ev.d1 = 66; // F#4 clashing tritone in C major
            }
        }
        const auto evM1 = bench::evaluateExportedStyleAgainstSty(rawBytes, m1Bad, kr.report, "SYNTH_POP_SFF2.sty", opt);
        TEST("R2-P2-1 M1: injecting F# (66) clash into C-major spikes clashRate & drops M1 score",
             evM1.ok && evM1.m1.clashRate > base.m1.clashRate + 0.05 && evM1.m1.score < base.m1.score - 5.0);
    }

    // M2.1 Negative Control: inject chord-range pitch (<= 11) on non-MegaVoice style -> FAIL, score=0, hardViol increments!
    {
        korg::Style m2Bad = kr.style;
        auto& el = m2Bad.elements.front();
        el.events.push_back({0, static_cast<uint8_t>(0x90 | 11), 4, 90});   // E-1 (pitch <= 11 chord-event range!)
        el.events.push_back({120, static_cast<uint8_t>(0x80 | 11), 4, 0});
        std::stable_sort(el.events.begin(), el.events.end(),
                         [](const korg::Event& a, const korg::Event& b) { return a.tick < b.tick; });
        const auto evM2 = bench::evaluateExportedStyleAgainstSty(rawBytes, m2Bad, kr.report, "SYNTH_POP_SFF2.sty", opt);
        const auto m2Cs = bench::computeCorpusSummary({evM2});
        TEST("R2-P2-1 M2.1: injecting chord-range pitch (<=11) triggers leakedChordRange>0, M2=FAIL (0.0) & increments hardViol",
             evM2.ok && evM2.m2.applicable && evM2.m2.leakedChordRange > 0 &&
             evM2.m2.status == bench::BenchStatus::Fail && evM2.m2.score == 0.0 &&
             m2Cs.hardInvariantViolations > baseCs.hardInvariantViolations);
    }

    // M3.1 & M3.3 Negative Controls: drop exported element -> coverageRate < 1.0; inject open CC64=127 & uncentered PB -> loopErrors >= 2!
    {
        korg::Style m3Drop = kr.style;
        m3Drop.elements.pop_back();
        const auto evDrop = bench::evaluateExportedStyleAgainstSty(rawBytes, m3Drop, kr.report, "SYNTH_POP_SFF2.sty", opt);
        TEST("R2-P2-1 M3.1: dropping exported element lowers coverageRate < 1.0 & M3 score",
             evDrop.ok && evDrop.m3.coverageRate < 1.0 && evDrop.m3.score < base.m3.score);

        korg::Style m3Loop = kr.style;
        auto& el = m3Loop.elements.front();
        el.events.push_back({10, static_cast<uint8_t>(0xB0 | 11), 64, 127}); // Open sustain pedal CC64=127
        el.events.push_back({20, static_cast<uint8_t>(0xE0 | 11), 0, 100});  // Uncentered Pitch Bend (!= 8192)
        std::stable_sort(el.events.begin(), el.events.end(),
                         [](const korg::Event& a, const korg::Event& b) { return a.tick < b.tick; });
        const auto evLoop = bench::evaluateExportedStyleAgainstSty(rawBytes, m3Loop, kr.report, "SYNTH_POP_SFF2.sty", opt);
        TEST("R2-P2-1 M3.3: open CC64=127 & uncentered Pitch Bend trigger loopBoundaryErrors >= 2 & M3=FAIL",
             evLoop.ok && evLoop.m3.loopBoundaryErrors >= 2 && evLoop.m3.status == bench::BenchStatus::Fail &&
             evLoop.m3.score < base.m3.score);
    }

    // M4.2 Negative Control: remap Kick (36) -> Triangle (81) (valid GM 35..81, wrong family!) -> familyPreservationRate drops sharply!
    {
        korg::Style m4Wrong = kr.style;
        for (auto& el : m4Wrong.elements) {
            for (auto& ev : el.events) {
                uint8_t ch = ev.status & 0x0F;
                uint8_t hi = ev.status & 0xF0;
                if ((ch == korg::kDrumCh || ch == korg::kPercCh) && (hi == 0x90 || hi == 0x80) && ev.d1 == 36) {
                    ev.d1 = 81; // Open Triangle ('L', valid GM 35..81, wrong family vs Kick 'B'!)
                }
            }
        }
        const auto evM4 = bench::evaluateExportedStyleAgainstSty(rawBytes, m4Wrong, kr.report, "SYNTH_POP_SFF2.sty", opt);
        TEST("R2-P2-1 M4.2: remapping Kick (36) -> Triangle (81) keeps unmappedHitRate==0 yet drops familyPreservationRate sharply",
             evM4.ok && evM4.m4.unmappedHitRate == 0.0 &&
             evM4.m4.familyPreservationRate < base.m4.familyPreservationRate - 0.10 &&
             evM4.m4.score < base.m4.score - 3.0);
    }

    // M5 Negative Controls: strip tick-0 mix CCs -> mixCcConsistency drops; inject CC98/99/94 -> unsafeCcLeaks > 0!
    {
        korg::Style m5Strip = kr.style;
        for (auto& el : m5Strip.elements) {
            std::vector<korg::Event> kept;
            for (const auto& ev : el.events) {
                if (ev.tick == 0 && (ev.status & 0xF0) == 0xB0 &&
                    (ev.d1 == 7 || ev.d1 == 10 || ev.d1 == 11 || ev.d1 == 91 || ev.d1 == 93)) {
                    continue;
                }
                kept.push_back(ev);
            }
            el.events = std::move(kept);
        }
        const auto evStrip = bench::evaluateExportedStyleAgainstSty(rawBytes, m5Strip, kr.report, "SYNTH_POP_SFF2.sty", opt);
        TEST("R2-P2-1 M5.1: stripping tick-0 mix CCs drops mixCcConsistency to 0 & drops M5 score",
             evStrip.ok && evStrip.m5.mixCcConsistency == 0.0 && evStrip.m5.score < base.m5.score - 40.0 &&
             evStrip.m5.status == bench::BenchStatus::Fail);

        korg::Style m5Unsafe = kr.style;
        auto& el = m5Unsafe.elements.front();
        el.events.push_back({0, static_cast<uint8_t>(0xB0 | 11), 99, 1});
        el.events.push_back({0, static_cast<uint8_t>(0xB0 | 11), 98, 2});
        el.events.push_back({0, static_cast<uint8_t>(0xB0 | 11), 94, 40});
        std::stable_sort(el.events.begin(), el.events.end(),
                         [](const korg::Event& a, const korg::Event& b) { return a.tick < b.tick; });
        const auto evUnsafe = bench::evaluateExportedStyleAgainstSty(rawBytes, m5Unsafe, kr.report, "SYNTH_POP_SFF2.sty", opt);
        TEST("R2-P2-1 M5.2: injecting CC98/99/94 flags unsafeCcLeaks >= 3, unsafeCcStatus=FAIL & M5=FAIL",
             evUnsafe.ok && evUnsafe.m5.unsafeCcLeaks >= 3 &&
             evUnsafe.m5.unsafeCcStatus == bench::BenchStatus::Fail &&
             evUnsafe.m5.status == bench::BenchStatus::Fail && evUnsafe.m5.score < base.m5.score);
    }

    // M6 Negative Control: omitting tracks from converter report cannot self-inflate M6 (denominator comes from raw .sty!)
    {
        korg::Report emptyReport = kr.report;
        emptyReport.tracks.clear();
        const auto evEmptyM6 = bench::evaluateExportedStyleAgainstSty(rawBytes, kr.style, emptyReport, "SYNTH_POP_SFF2.sty", opt);
        TEST("R2-P2-1 M6: omitting tracks from converter report yields familyMatchRate=0 & M6=0 (no self-inflation)",
             evEmptyM6.ok && evEmptyM6.m6.applicable && evEmptyM6.m6.familyMatchRate == 0.0 &&
             evEmptyM6.m6.score == 0.0 && evEmptyM6.m6.status == bench::BenchStatus::Fail);
    }

    // 5-State Taxonomy & True N/A Check across non-MegaVoice fixtures + OTSc/FNRP/MDB + BreakPolicy::Withhold
    {
        const auto sff1Res = bench::evaluateStyleFile(fx("SYNTH_SFF1_CTAB_CNTT.sty"), opt);
        const auto waltzRes = bench::evaluateStyleFile(fx("SYNTH_WALTZ_68.sty"), opt);
        const std::string popJson = bench::toJson(base);
        const std::string popMd = bench::toMarkdown({base});
        TEST("R2-P2-1 Taxonomy N/A & NOT_TESTED: Pop/SFF1/Waltz emit M2=N/A (score=null, weight excluded), l3Status=NOT_TESTED, targetKstStatus=NOT_TESTED",
             !base.m2.applicable && base.m2.status == bench::BenchStatus::NA &&
             !sff1Res.m2.applicable && sff1Res.m2.status == bench::BenchStatus::NA &&
             !waltzRes.m2.applicable && waltzRes.m2.status == bench::BenchStatus::NA &&
             std::abs(base.applicableWeightSum - 0.80) < 1e-6 &&
             base.l3Status == bench::BenchStatus::NotTested &&
             base.m6.targetKstStatus == bench::BenchStatus::NotTested &&
             popJson.find("\"articulationMegaVoice\": { \"status\": \"N/A\", \"score\": null") != std::string::npos &&
             popJson.find("\"l3Status\": \"NOT_TESTED\"") != std::string::npos &&
             popJson.find("\"targetKstStatus\": \"NOT_TESTED\"") != std::string::npos &&
             popMd.find("| N/A |") != std::string::npos);

        const auto multiRes = bench::evaluateStyleFile(fx("SYNTH_MULTI_ZONE_SART_OTS.sty"), opt);
        auto hasUnsup = [](const bench::BenchResult& r, const char* feat) {
            return std::any_of(r.unsupportedFeatures.begin(), r.unsupportedFeatures.end(),
                               [&](const bench::UnsupportedFeature& u) {
                                   return u.feature == feat && u.status == bench::BenchStatus::Unsupported;
                               });
        };
        bench::BenchOptions withholdOpt = opt;
        withholdOpt.breakYamaha1BarCap = false;
        const auto withheldRes = bench::evaluateStyleFile(fx("SYNTH_BREAK_TAIL.sty"), withholdOpt);
        TEST("R2-P2-1 Taxonomy UNSUPPORTED: OTSc, FNRP, MDB & BreakPolicy::Withhold explicitly tagged UNSUPPORTED",
             hasUnsup(multiRes, "OTSc") && hasUnsup(multiRes, "FNRP") && hasUnsup(multiRes, "MDB") &&
             withheldRes.ok && hasUnsup(withheldRes, "BREAK_WITHHELD_BY_POLICY"));
    }
}

static void testMalformedFailClosed() {
    std::printf("8. Malformed / truncated / encrypted .sty fail-closed checks\n");
    Sff1Reader reader;

    {
        auto buf = makeChunk("MThd", {0, 0, 0, 1});
        const auto r = reader.parseBuffer(buf, "trunc_mthd.sty");
        TEST("malformed: truncated MThd fails closed", !r.success);
    }
    {
        auto buf = makeChunk("MThd", {0, 0, 0, 1, 0, 0});
        const auto r = reader.parseBuffer(buf, "zero_div.sty");
        TEST("malformed: zero-division MThd fails closed", !r.success);
    }
    {
        auto mthd = makeChunk("MThd", {0, 0, 0, 1, 0x01, 0xE0});
        auto cryp = makeChunk("CRYP", {0xDE, 0xAD, 0xBE, 0xEF});
        mthd.insert(mthd.end(), cryp.begin(), cryp.end());
        const auto r = reader.parseBuffer(mthd, "drm.sty");
        TEST("encrypted: CRYP chunk fails closed with encrypted_or_drm=true",
             !r.success && r.encrypted_or_drm);
    }
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
    testAuditCalibrationAndBenchmarkInvariants();
    testNegativeControlsAllDimensions();
    testMalformedFailClosed();
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
