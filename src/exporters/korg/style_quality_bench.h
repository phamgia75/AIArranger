#ifndef AI_ARRANGER_EXPORTERS_KORG_STYLE_QUALITY_BENCH_H
#define AI_ARRANGER_EXPORTERS_KORG_STYLE_QUALITY_BENCH_H

// Offline Musical Quality Benchmark Harness (Track B Milestone Q1).
// Implements the 3-stage deterministic offline evaluation pipeline specified in:
//   STYLE_CONVERSION_QUALITY_BENCHMARK_SPEC_2026-11-25.md
//
// Epistemic boundary:
//   100% offline, deterministic, software-verified only.
//   hardwareVerified = false, l3Status = "NotRun",
//   label = "STRUCTURALLY VALID — NOT VERIFIED ON A PA5X".
//
// Immutable benchmark weights (sum = 100%):
//   M1 Harmonic & Voice-Leading           = 25%
//   M2 Articulation & MegaVoice Hygiene   = 20%
//   M3 Section & Structural Completeness  = 15%
//   M4 Drum & Percussion Accuracy         = 15%
//   M5 Mix & Dynamic Balance              = 12%
//   M6 Timbre & Target-Resource Coherence = 13%

#include "engine/uasf/deserializer.h"
#include "engine/uasf/serializer.h"
#include "engine/uasf/types.h"
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
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace ai_arranger::exporters::korg::bench {

// ── Immutable Fixed Benchmark Weights (DO NOT MODIFY) ────────────────
inline constexpr double kWeightM1 = 0.25;
inline constexpr double kWeightM2 = 0.20;
inline constexpr double kWeightM3 = 0.15;
inline constexpr double kWeightM4 = 0.15;
inline constexpr double kWeightM5 = 0.12;
inline constexpr double kWeightM6 = 0.13;

inline constexpr const char* kSchemaId = "aiarranger-style-quality-bench/1";
inline constexpr const char* kValidLabel = "STRUCTURALLY VALID — NOT VERIFIED ON A PA5X";
inline constexpr const char* kInvalidLabel = "STRUCTURALLY INVALID — NOT VERIFIED ON A PA5X";

// ── Standardized 13-Chord Multi-Chord Torture Suite ──────────────────
enum class TestChordKind : uint8_t {
    Major = 0,
    Minor = 1,
    Dom7 = 2,
    Maj7 = 3,
    Min7b5 = 4,
    Dim7 = 5,
    Sus4 = 6,
    Add9 = 7,
};

struct TestChord {
    const char* symbol;
    uint16_t chordMask;    // 12-bit mask of chord pitch classes (0=C .. 11=B)
    uint16_t allowedMask;  // 12-bit mask of chord tones + non-clashing tensions
    uint8_t rootPc;        // 0..11
    uint8_t bassPc;        // 0..11
    TestChordKind kind;
};

constexpr uint16_t pcMask(std::initializer_list<int> pcs) noexcept {
    uint16_t m = 0;
    for (int p : pcs) m = static_cast<uint16_t>(m | (1u << (p % 12)));
    return m;
}

inline constexpr std::array<TestChord, 13> kTestChords = {{
    {"Cmaj",  pcMask({0, 4, 7}),     pcMask({0, 2, 4, 7, 9, 11}),   0, 0,  TestChordKind::Major},
    {"Cmin",  pcMask({0, 3, 7}),     pcMask({0, 2, 3, 5, 7, 10}),   0, 0,  TestChordKind::Minor},
    {"C7",    pcMask({0, 4, 7, 10}), pcMask({0, 2, 4, 7, 9, 10}),   0, 0,  TestChordKind::Dom7},
    {"Cmaj7", pcMask({0, 4, 7, 11}), pcMask({0, 2, 4, 7, 9, 11}),   0, 0,  TestChordKind::Maj7},
    {"Cm7b5", pcMask({0, 3, 6, 10}), pcMask({0, 3, 5, 6, 8, 10}),   0, 0,  TestChordKind::Min7b5},
    {"Cdim7", pcMask({0, 3, 6, 9}),  pcMask({0, 2, 3, 5, 6, 8, 9, 11}), 0, 0, TestChordKind::Dim7},
    {"Csus4", pcMask({0, 5, 7}),     pcMask({0, 2, 5, 7, 9, 10}),   0, 0,  TestChordKind::Sus4},
    {"Cadd9", pcMask({0, 2, 4, 7}),  pcMask({0, 2, 4, 7, 9, 11}),   0, 0,  TestChordKind::Add9},
    {"C/E",   pcMask({0, 4, 7}),     pcMask({0, 2, 4, 7, 9, 11}),   0, 4,  TestChordKind::Major},
    {"C/G",   pcMask({0, 4, 7}),     pcMask({0, 2, 4, 7, 9, 11}),   0, 7,  TestChordKind::Major},
    {"F#maj", pcMask({6, 10, 1}),    pcMask({6, 8, 10, 1, 3, 5}),   6, 6,  TestChordKind::Major},
    {"Bbmin", pcMask({10, 1, 5}),    pcMask({10, 0, 1, 3, 5, 8}),  10, 10, TestChordKind::Minor},
    {"G7/B",  pcMask({7, 11, 2, 5}), pcMask({7, 9, 11, 2, 4, 5}),   7, 11, TestChordKind::Dom7},
}};

// ── Benchmark Options & Structured Output Models ─────────────────────
struct BenchOptions {
    uint16_t targetPpqn = 384;          // Pa5X native internal resolution (or 0 = keep source)
    bool breakYamaha1BarCap = true;     // Used when BreakPolicy is available in MapOptions
    std::string targetKstPath;          // Optional read-only Target KST path (never written)
};

struct Diagnostic {
    std::string code;
    std::string severity;   // "ERROR", "WARN", "INFO"
    std::string section;
    std::string track;
    uint32_t count = 0;
    std::string fallback;
};

struct MatrixCell {
    std::string section;
    std::string track;
    std::string chord;
    uint32_t noteCount = 0;
    uint32_t clashCount = 0;
    double bassRootHitRate = 1.0;
    uint32_t registerViolations = 0;
    double voiceLeadingAvgSemis = 0.0;
    uint32_t leakedFxNotes = 0;
    bool velocityInversionFlag = false;
    uint32_t loopErrors = 0;
    double cellScore = 100.0;
};

struct M1Scores {
    double score = 0.0;
    double clashRate = 0.0;
    double bassSlashAccuracy = 1.0;
    double voiceLeadingAvgSemis = 0.0;
    double registerViolationRate = 0.0;
    double voicingCollapseRate = 0.0;
    double casmPreservationRate = 0.0;
};

struct M2Scores {
    double score = 0.0;
    uint32_t leakedChordRange = 0;
    double leakedFxKeyRate = 0.0;
    uint32_t velocityInversions = 0;
    double guitarStrumCoherence = 1.0;
};

struct M3Scores {
    double score = 0.0;
    double coverageRate = 0.0;
    double meterBarAccuracy = 0.0;
    uint32_t loopBoundaryErrors = 0;
    double p99DriftMs = 0.0;
};

struct M4Scores {
    double score = 0.0;
    double unmappedHitRate = 0.0;
    double familyPreservationRate = 1.0;
    double trackSeparationAccuracy = 1.0;
    double velCorrelation = 1.0;
};

struct M5Scores {
    double score = 0.0;
    double mixCcConsistency = 1.0;
    uint32_t unsafeCcLeaks = 0;
    double balanceHealthRate = 1.0;
    double xgSysexCoverage = 1.0;
};

struct M6Scores {
    double score = 0.0;
    double familyMatchRate = 0.0;
    double elementVoicePreservation = 1.0;
    bool targetKstValid = true;
};

struct MatrixSummary {
    uint32_t cellsEvaluated = 0;
    uint32_t cellsPass = 0;
    uint32_t cellsWarn = 0;
    uint32_t cellsFail = 0;
};

struct BenchResult {
    bool ok = false;
    std::string error;
    std::string fixtureName;    // path-scrubbed basename only!
    std::string format = "SFF2";
    uint16_t sourcePpqn = 0;
    uint16_t targetPpqn = 0;
    uint32_t sectionsSource = 0;
    uint32_t sectionsExported = 0;
    bool structuralValid = false;
    std::vector<std::string> validationErrors;
    double compositeScore = 0.0;
    std::string grade = "FAIL";
    M1Scores m1;
    M2Scores m2;
    M3Scores m3;
    M4Scores m4;
    M5Scores m5;
    M6Scores m6;
    std::vector<Diagnostic> diagnostics;
    MatrixSummary matrixSummary;
    std::vector<MatrixCell> cells;
};

namespace detail {

inline double clamp01(double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
inline double clamp100(double v) noexcept { return v < 0.0 ? 0.0 : (v > 100.0 ? 100.0 : v); }

inline std::string basenameOnly(const std::string& p) {
    if (p.empty()) return "unnamed.sty";
    return std::filesystem::path(p).filename().string();
}

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

inline std::string fmtNum(double v, int prec = 4) {
    if (!std::isfinite(v)) v = 0.0;
    if (std::abs(v) < 1e-12) v = 0.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(prec) << v;
    return oss.str();
}

inline char drumFamilyOf(uint8_t note) noexcept {
    if (note == 35 || note == 36) return 'B';                                 // Bass drum
    if (note == 37 || note == 38 || note == 39 || note == 40) return 'S';     // Snare / Rim / Clap
    if (note == 42 || note == 44 || note == 46) return 'H';                   // Hi-Hat
    if (note == 41 || note == 43 || note == 45 || note == 47 || note == 48 || note == 50) return 'T'; // Toms
    if (note == 49 || note == 51 || note == 52 || note == 53 || note == 55 || note == 57 || note == 59) return 'C'; // Cymbals
    if (note >= 54 && note <= 81) return 'L';                                 // Latin / Perc
    return 0;                                                                 // Outside GM 35..81
}

inline bool isPrimaryKitHit(uint8_t note) noexcept {
    return note == 35 || note == 36 || note == 38 || note == 40 || note == 42 || note == 44 || note == 46;
}

inline bool isMixCc(uint8_t cc) noexcept {
    return cc == 7 || cc == 10 || cc == 11 || cc == 91 || cc == 93;
}

inline bool markerToSectionType(const std::string& m, uasf::SectionType& out) noexcept {
    using T = uasf::SectionType;
    static const struct { const char* name; T type; } kMap[] = {
        {"Intro A", T::Intro1}, {"Intro B", T::Intro2}, {"Intro C", T::Intro3},
        {"Main A", T::Main1}, {"Main B", T::Main2}, {"Main C", T::Main3}, {"Main D", T::Main4},
        {"Fill In AA", T::Fill1}, {"Fill In BB", T::Fill2},
        {"Fill In CC", T::Fill3}, {"Fill In DD", T::Fill4},
        {"Fill In BA", T::Break},
        {"Ending A", T::Ending1}, {"Ending B", T::Ending2}, {"Ending C", T::Ending3},
    };
    for (const auto& e : kMap) {
        if (m == e.name) { out = e.type; return true; }
    }
    return false;
}

inline std::string baseMarkerForType(uasf::SectionType t) {
    using T = uasf::SectionType;
    switch (t) {
        case T::Intro1:  return "i1";
        case T::Intro2:  return "i2";
        case T::Intro3:  return "i3";
        case T::Main1:   return "v1";
        case T::Main2:   return "v2";
        case T::Main3:   return "v3";
        case T::Main4:   return "v4";
        case T::Fill1:   return "f1";
        case T::Fill2:   return "f2";
        case T::Fill3:   return "f3";
        case T::Fill4:   return "f4";
        case T::Ending1: return "e1";
        case T::Ending2: return "e2";
        case T::Ending3: return "e3";
        case T::Break:   return "br";
    }
    return "";
}

inline std::string baseElementOfMarker(const std::string& m) {
    if (m == "br") return "br";
    if (m.size() == 5 && m[2] == 'c' && m[3] == 'v') return m.substr(0, 2);
    return m;
}

inline int cvIndexOfMarker(const std::string& m) noexcept {
    if (m == "br") return 1;
    if (m.size() == 5 && m[2] == 'c' && m[3] == 'v' && m[4] >= '1' && m[4] <= '6')
        return m[4] - '0';
    return 1;
}

// ── Stage 1: Independent Reference Analyzer (raw .sty bytes) ─────────
struct RefCasmEntry {
    std::string sectionName;
    std::string trackName;
    uint8_t srcCh = 0;
    uint8_t destCh = 0;
    uint8_t editable = 0;
    uint16_t noteMute = 0x0FFF;
    uint64_t chordMute = 0x3FFFFFFFFull;
    uint8_t srcRoot = 0;
    uint8_t srcType = 0;
    uint8_t zoneLow = 0;
    uint8_t zoneHigh = 127;
    uint8_t ntr = 0;
    uint8_t ntt = 0;
    bool nttBass = false;
    uint8_t highKey = 11;
    uint8_t noteLow = 0;
    uint8_t noteHigh = 127;
    uint8_t rtr = 0;
    bool fromSff1Ctab = false;
};

struct RefNote {
    uint32_t on = 0;
    uint32_t off = 0;
    uint8_t ch = 0;
    uint8_t pitch = 0;
    uint8_t vel = 0;
};

struct RefSection {
    uasf::SectionType type = uasf::SectionType::Main1;
    std::string name;
    uint32_t startTick = 0;
    uint32_t endTick = 0;
    uint32_t musicalEndTick = 0;   // With Yamaha 1-bar cap on Fill In BA
    uint8_t num = 4;
    uint8_t den = 4;
    uint32_t expectedBars = 1;
    bool breakHasTail = false;
    std::vector<RefNote> notes;
    std::map<uint8_t, std::tuple<int, int, int>> sectionVoices; // ch -> (msb, lsb, pc) changed in section
};

struct RefAnalysis {
    bool ok = false;
    std::string error;
    std::string format = "SFF2";
    uint16_t ppqn = 0;
    uint32_t initialMpqn = 500000;
    std::vector<std::pair<uint32_t, uint32_t>> tempos;
    std::vector<std::tuple<uint32_t, uint8_t, uint8_t>> timeSigs;
    uint32_t xgSysexCount = 0;
    std::vector<std::string> extraChunks; // OTSc, OTSm, OTS, FNRP, MDB
    std::map<uint8_t, std::map<uint8_t, uint8_t>> sintMixCc;     // ch -> {cc -> val}
    std::map<uint8_t, std::tuple<int, int, int>> sintVoice;      // ch -> (msb, lsb, pc)
    std::vector<RefSection> sections;
    std::vector<RefCasmEntry> casmEntries;
    std::map<uint8_t, RefCasmEntry> firstCasmByCh;
    std::map<uint8_t, uint8_t> casmRoute;
    std::set<uint8_t> megaVoiceSrcChannels;
    std::set<uint8_t> sartSrcChannels;
    std::set<uint8_t> guitarNtrSrcChannels;
    bool hasChordMutePairs = false;
    uint32_t midStyleVoiceChanges = 0;
};

inline uint32_t readBE32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline void parseRefCasmRec(const uint8_t* data, size_t size, int depth,
                            std::string& curSec, RefAnalysis& out) {
    if (size < 8 || depth > 8) return;
    size_t pos = 0;
    if (std::memcmp(data, "CSEG", 4) == 0) pos = 8;
    while (pos + 8 <= size) {
        std::string id(reinterpret_cast<const char*>(data + pos), 4);
        uint32_t sz = readBE32(data + pos + 4);
        pos += 8;
        if (sz > size - pos) break;
        const uint8_t* d = data + pos;
        if (id == "CSEG") {
            parseRefCasmRec(d, sz, depth + 1, curSec, out);
        } else if (id == "Sdec" && sz > 0 && sz <= 64) {
            curSec.assign(reinterpret_cast<const char*>(d), sz);
            while (!curSec.empty() && static_cast<uint8_t>(curSec.back()) <= 32) curSec.pop_back();
        } else if (id == "Ctb2" && sz >= 34) {
            RefCasmEntry e;
            e.sectionName = curSec;
            e.srcCh = d[0];
            e.trackName.assign(reinterpret_cast<const char*>(d + 1), 8);
            while (!e.trackName.empty() && static_cast<uint8_t>(e.trackName.back()) <= 32) e.trackName.pop_back();
            e.destCh = d[9];
            e.editable = d[10];
            e.noteMute = static_cast<uint16_t>(((d[11] & 0x0F) << 8) | d[12]);
            e.chordMute = (uint64_t(d[13] & 0x03) << 32) | (uint64_t(d[14]) << 24) |
                          (uint64_t(d[15]) << 16) | (uint64_t(d[16]) << 8) | uint64_t(d[17]);
            e.srcRoot = d[18];
            e.srcType = d[19];
            e.zoneLow = d[20];
            e.zoneHigh = d[21];
            e.ntr = d[28];
            e.nttBass = (d[29] & 0x80) != 0;
            e.ntt = d[29] & 0x7F;
            e.highKey = d[30];
            e.noteLow = d[31];
            e.noteHigh = d[32];
            e.rtr = d[33];
            out.casmEntries.push_back(e);
            if (e.srcCh <= 15) {
                out.firstCasmByCh.emplace(e.srcCh, e);
                if (e.destCh <= 15) out.casmRoute.emplace(e.srcCh, e.destCh);
                if (e.ntr == 2) out.guitarNtrSrcChannels.insert(e.srcCh);
            }
        } else if (id == "Ctab" && sz >= 26) {
            RefCasmEntry e;
            e.sectionName = curSec;
            e.srcCh = d[0];
            e.trackName.assign(reinterpret_cast<const char*>(d + 1), 8);
            while (!e.trackName.empty() && static_cast<uint8_t>(e.trackName.back()) <= 32) e.trackName.pop_back();
            e.destCh = d[9];
            e.editable = d[10];
            e.noteMute = static_cast<uint16_t>(((d[11] & 0x0F) << 8) | d[12]);
            e.chordMute = (uint64_t(d[13] & 0x03) << 32) | (uint64_t(d[14]) << 24) |
                          (uint64_t(d[15]) << 16) | (uint64_t(d[16]) << 8) | uint64_t(d[17]);
            e.srcRoot = d[18];
            e.srcType = d[19];
            e.ntr = d[20];
            e.nttBass = (d[21] & 0x80) != 0;
            e.ntt = d[21] & 0x7F;
            e.highKey = d[22];
            e.noteLow = d[23];
            e.noteHigh = d[24];
            e.rtr = d[25];
            e.fromSff1Ctab = true;
            out.casmEntries.push_back(e);
            if (e.srcCh <= 15) {
                out.firstCasmByCh.emplace(e.srcCh, e);
                if (e.destCh <= 15) out.casmRoute.emplace(e.srcCh, e.destCh);
                if (e.ntr == 2) out.guitarNtrSrcChannels.insert(e.srcCh);
            }
        } else if (id == "Cntt" && sz >= 2) {
            const uint8_t srcCh = d[0];
            const bool bass = (d[1] & 0x80) != 0;
            const uint8_t ntt = d[1] & 0x7F;
            // Apply to matching entry in current section
            for (auto it = out.casmEntries.rbegin(); it != out.casmEntries.rend(); ++it) {
                if (it->sectionName == curSec && it->srcCh == srcCh) {
                    it->nttBass = bass;
                    it->ntt = ntt;
                    break;
                }
            }
            auto fit = out.firstCasmByCh.find(srcCh);
            if (fit != out.firstCasmByCh.end() && fit->second.sectionName == curSec) {
                fit->second.nttBass = bass;
                fit->second.ntt = ntt;
            }
        }
        pos += sz;
    }
}

inline RefAnalysis analyzeReferenceSty(const std::vector<uint8_t>& buf) {
    RefAnalysis out;
    if (buf.size() < 14) { out.error = "file too small"; return out; }

    struct RawEv { uint32_t tick; uint8_t status; uint8_t d1; uint8_t d2; };
    struct RawMark { uint32_t tick; std::string name; };
    std::vector<RawEv> events;
    std::vector<RawMark> markers;
    uint32_t eotTick = 0;

    size_t pos = 0;
    while (pos + 8 <= buf.size()) {
        std::string id(reinterpret_cast<const char*>(buf.data() + pos), 4);
        bool printable = true;
        for (char c : id) if (c < 32 || c > 126) printable = false;
        if (!printable) break;
        uint32_t sz = readBE32(buf.data() + pos + 4);
        pos += 8;
        if (sz > buf.size() - pos) sz = static_cast<uint32_t>(buf.size() - pos);
        const uint8_t* d = buf.data() + pos;

        if (id == "MThd" && sz >= 6) {
            uint16_t div = static_cast<uint16_t>((d[4] << 8) | d[5]);
            out.ppqn = (div & 0x8000) ? static_cast<uint16_t>(div & 0x7FFF) : div;
        } else if (id == "MTrk") {
            size_t p = 0;
            uint64_t abs = 0;
            uint8_t rs = 0;
            auto readVlq = [&](uint32_t& v) -> bool {
                v = 0;
                for (int i = 0; i < 4; ++i) {
                    if (p >= sz) return false;
                    uint8_t b = d[p++];
                    v = (v << 7) | (b & 0x7F);
                    if (!(b & 0x80)) return true;
                }
                return false;
            };
            while (p < sz) {
                uint32_t dt = 0;
                if (!readVlq(dt)) break;
                abs += dt;
                if (abs > UINT32_MAX || p >= sz) break;
                uint8_t st = d[p];
                if (st & 0x80) ++p;
                else if (rs) st = rs;
                else break;
                if (st == 0xFF) {
                    rs = 0;
                    if (p >= sz) break;
                    uint8_t mt = d[p++];
                    uint32_t ml = 0;
                    if (!readVlq(ml) || ml > sz - p) break;
                    if (mt == 0x2F) { eotTick = static_cast<uint32_t>(abs); break; }
                    if (mt == 0x06 && ml > 0 && ml <= 64) {
                        std::string nm(reinterpret_cast<const char*>(d + p), ml);
                        while (!nm.empty() && static_cast<uint8_t>(nm.back()) <= 32) nm.pop_back();
                        if (nm == "SFF1") out.format = "SFF1";
                        else if (nm == "SFF2") out.format = "SFF2";
                        markers.push_back({static_cast<uint32_t>(abs), std::move(nm)});
                    } else if (mt == 0x58 && ml >= 2) {
                        uint8_t num = d[p];
                        uint8_t den = (d[p + 1] < 8) ? static_cast<uint8_t>(1u << d[p + 1]) : 4;
                        if (num > 0) out.timeSigs.emplace_back(static_cast<uint32_t>(abs), num, den);
                    } else if (mt == 0x51 && ml == 3) {
                        uint32_t mpqn = (uint32_t(d[p]) << 16) | (uint32_t(d[p + 1]) << 8) | d[p + 2];
                        if (mpqn > 0) out.tempos.emplace_back(static_cast<uint32_t>(abs), mpqn);
                    }
                    p += ml;
                } else if (st == 0xF0 || st == 0xF7) {
                    rs = 0;
                    uint32_t sl = 0;
                    if (!readVlq(sl) || sl > sz - p) break;
                    if (st == 0xF0 && sl >= 1 && d[p] == 0x43) ++out.xgSysexCount;
                    p += sl;
                } else if (st >= 0xF1) {
                    break;
                } else {
                    rs = st;
                    uint8_t hi = st & 0xF0;
                    size_t need = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
                    if (need > sz - p) break;
                    events.push_back({static_cast<uint32_t>(abs), st, d[p], need == 2 ? d[p + 1] : uint8_t{0}});
                    p += need;
                }
            }
        } else if (id == "CASM") {
            std::string curSec;
            parseRefCasmRec(d, sz, 0, curSec, out);
        } else if (id == "OTSc" || id == "OTSm" || id == "OTS " || id == "FNRP" || id == "MDB " || id == "MDB") {
            out.extraChunks.push_back(id);
        }
        pos += sz;
    }

    if (out.ppqn == 0) { out.error = "missing MThd/PPQN"; return out; }
    if (!out.tempos.empty()) {
        auto it = std::min_element(out.tempos.begin(), out.tempos.end(),
                                   [](const auto& a, const auto& b) { return a.first < b.first; });
        out.initialMpqn = it->second;
    }

    std::stable_sort(markers.begin(), markers.end(),
                     [](const RawMark& a, const RawMark& b) { return a.tick < b.tick; });
    uint32_t lastTick = eotTick;
    for (const auto& e : events) lastTick = std::max(lastTick, e.tick);

    auto meterAt = [&](uint32_t tick) -> std::pair<uint8_t, uint8_t> {
        uint8_t num = 4, den = 4;
        for (const auto& [t, n, dn] : out.timeSigs) {
            if (t <= tick) { num = n; den = dn; }
        }
        return {num, den};
    };

    bool haveSInt = false;
    uint32_t sintStart = 0, sintEnd = 0;
    std::set<uasf::SectionType> seenTypes;
    for (size_t i = 0; i < markers.size(); ++i) {
        uint32_t end = lastTick;
        for (size_t j = i + 1; j < markers.size(); ++j) {
            if (markers[j].tick > markers[i].tick) { end = markers[j].tick; break; }
        }
        if (markers[i].name == "SInt") {
            haveSInt = true; sintStart = markers[i].tick; sintEnd = end;
            continue;
        }
        uasf::SectionType st;
        if (!markerToSectionType(markers[i].name, st)) continue;
        if (end <= markers[i].tick || !seenTypes.insert(st).second) continue;

        RefSection sec;
        sec.type = st;
        sec.name = markers[i].name;
        sec.startTick = markers[i].tick;
        sec.endTick = end;
        auto [num, den] = meterAt(sec.startTick);
        sec.num = num;
        sec.den = den;
        uint64_t barTicks = std::max<uint64_t>(1, uint64_t{out.ppqn} * 4 * num / den);
        uint32_t rawLen = end - sec.startTick;
        uint32_t rawBars = static_cast<uint32_t>(std::max<uint64_t>(1, (rawLen + barTicks - 1) / barTicks));
        if (st == uasf::SectionType::Break && rawBars > 1) {
            sec.breakHasTail = true;
            sec.expectedBars = 1;
            sec.musicalEndTick = sec.startTick + static_cast<uint32_t>(barTicks);
        } else {
            sec.expectedBars = rawBars;
            sec.musicalEndTick = end;
        }
        out.sections.push_back(std::move(sec));
    }

    // Extract SInt mix CCs & voices
    if (haveSInt) {
        for (const auto& e : events) {
            if (e.tick < sintStart || e.tick >= sintEnd) continue;
            uint8_t hi = e.status & 0xF0, ch = e.status & 0x0F;
            if (hi == 0xB0 && isMixCc(e.d1)) out.sintMixCc[ch][e.d1] = e.d2;
            if (hi == 0xB0 && e.d1 == 0) {
                auto& v = out.sintVoice.try_emplace(ch, -1, -1, -1).first->second;
                std::get<0>(v) = e.d2;
            } else if (hi == 0xB0 && e.d1 == 32) {
                auto& v = out.sintVoice.try_emplace(ch, -1, -1, -1).first->second;
                std::get<1>(v) = e.d2;
            } else if (hi == 0xC0) {
                auto& v = out.sintVoice.try_emplace(ch, -1, -1, -1).first->second;
                std::get<2>(v) = e.d1;
            }
        }
    }

    // Collect notes and mid-style voice changes per section
    std::map<uint8_t, uint32_t> highNotesByCh;
    std::map<uint8_t, uint32_t> lowSartByCh;
    std::map<uint8_t, uint8_t> velZoneMaskByCh;
    for (auto& sec : out.sections) {
        std::map<uint16_t, std::deque<size_t>> open;
        for (const auto& e : events) {
            uint8_t hi = e.status & 0xF0, ch = e.status & 0x0F;
            bool isOff = (hi == 0x80) || (hi == 0x90 && e.d2 == 0);
            if (!((e.tick >= sec.startTick && e.tick < sec.musicalEndTick) ||
                  (isOff && e.tick == sec.musicalEndTick))) {
                continue;
            }
            uint32_t rel = e.tick - sec.startTick;
            if (hi == 0x90 && e.d2 > 0) {
                uint16_t k = static_cast<uint16_t>((ch << 8) | e.d1);
                open[k].push_back(sec.notes.size());
                sec.notes.push_back({rel, sec.musicalEndTick - sec.startTick, ch, e.d1, e.d2});
                if (ch != 8 && ch != 9) {
                    if (e.d1 >= 96) ++highNotesByCh[ch];
                    if (e.d1 >= 12 && e.d1 <= 23) ++lowSartByCh[ch];
                    uint8_t zm = e.d2 <= 64 ? 1 : e.d2 <= 90 ? 2 : e.d2 <= 100 ? 4 : 8;
                    velZoneMaskByCh[ch] |= zm;
                }
            } else if (isOff) {
                uint16_t k = static_cast<uint16_t>((ch << 8) | e.d1);
                auto& q = open[k];
                if (!q.empty()) {
                    sec.notes[q.front()].off = rel;
                    q.pop_front();
                }
            } else if (hi == 0xB0 && (e.d1 == 0 || e.d1 == 32)) {
                auto& v = sec.sectionVoices.try_emplace(ch, -1, -1, -1).first->second;
                if (e.d1 == 0) std::get<0>(v) = e.d2;
                else std::get<1>(v) = e.d2;
            } else if (hi == 0xC0) {
                auto& v = sec.sectionVoices.try_emplace(ch, -1, -1, -1).first->second;
                std::get<2>(v) = e.d1;
            }
        }
        for (const auto& [ch, v] : sec.sectionVoices) {
            auto sit = out.sintVoice.find(ch);
            if (sit == out.sintVoice.end() || sit->second != v) ++out.midStyleVoiceChanges;
        }
    }

    // Detect MegaVoice & S.Art channels from Bank/PC + pitch/vel signals
    for (const auto& [ch, v] : out.sintVoice) {
        int msb = std::get<0>(v), lsb = std::get<1>(v);
        if (msb == 63 && (lsb == 120 || lsb == 60)) out.megaVoiceSrcChannels.insert(ch);
        if (msb == 8 && lsb >= 64) out.sartSrcChannels.insert(ch);
    }
    for (const auto& [ch, cnt] : highNotesByCh) {
        uint8_t zm = velZoneMaskByCh[ch];
        int pop = 0;
        for (uint8_t z = zm; z; z &= static_cast<uint8_t>(z - 1)) ++pop;
        if (cnt > 0 && pop >= 3) out.megaVoiceSrcChannels.insert(ch);
    }
    for (const auto& [ch, cnt] : lowSartByCh) {
        if (cnt > 0) out.sartSrcChannels.insert(ch);
    }

    // Detect mutually exclusive CASM chord-mute pairs targeting the same Korg channel
    std::map<std::string, std::map<uint8_t, std::vector<RefCasmEntry>>> bySecKorg;
    for (const auto& e : out.casmEntries) {
        uint8_t kch = korgChannelFor(e.destCh);
        if (kch != 0xFF) bySecKorg[e.sectionName][kch].push_back(e);
    }
    for (const auto& [secName, kmap] : bySecKorg) {
        (void)secName;
        for (const auto& [kch, vec] : kmap) {
            (void)kch;
            if (vec.size() >= 2) {
                for (size_t i = 0; i < vec.size(); ++i) {
                    for (size_t j = i + 1; j < vec.size(); ++j) {
                        if (vec[i].chordMute != vec[j].chordMute &&
                            (vec[i].chordMute & vec[j].chordMute) == 0) {
                            out.hasChordMutePairs = true;
                        }
                    }
                }
            }
        }
    }

    out.ok = !out.sections.empty();
    if (!out.ok) out.error = "no recognized Yamaha section markers";
    return out;
}

// ── Feature-detection helpers so Stage 1 & Stage 2 share IDENTICAL code ─
template <typename OptT>
inline void applyBreakPolicy(OptT& opt, bool yamaha1Bar) noexcept {
    if constexpr (requires { opt.break_policy; }) {
        using BP = std::remove_cvref_t<decltype(opt.break_policy)>;
        opt.break_policy = yamaha1Bar ? BP::Yamaha1BarCap : BP::Withhold;
    } else {
        (void)opt;
        (void)yamaha1Bar;
    }
}

struct RoundTrippedTrackMeta {
    bool hasCasm = false;
    uint8_t ntr = 0;
    uint8_t ntt = 0;
    bool nttBass = false;
    uint8_t highKey = 11;
    uint8_t noteLow = 0;
    uint8_t noteHigh = 127;
    uint8_t rtr = 0;
    uint8_t srcRoot = 0;
    uint8_t srcType = 0;
    uint16_t noteMute = 0x0FFF;
    uint64_t chordMute = 0x3FFFFFFFFull;
    uint8_t destCh = 0xFF;
};

template <typename ArtT>
inline RoundTrippedTrackMeta extractUasfMeta(const ArtT& a) noexcept {
    RoundTrippedTrackMeta m;
    m.ntr = a.ntr;
    m.ntt = a.ntt;
    m.highKey = a.high_key;
    m.noteLow = a.note_low;
    m.noteHigh = a.note_high;
    if constexpr (requires { a.ntt_bass; a.rtr; a.source_chord_root; a.source_chord_type; a.chord_mute_mask; a.dest_channel; a.has_casm_meta; }) {
        m.hasCasm = a.has_casm_meta;
        m.nttBass = a.ntt_bass;
        m.rtr = a.rtr;
        m.srcRoot = a.source_chord_root;
        m.srcType = a.source_chord_type;
        m.noteMute = a.note_mute_mask;
        m.chordMute = a.chord_mute_mask;
        m.destCh = a.dest_channel;
    }
    return m;
}

// ── Offline Pa5X Playback Simulator (MAN5 documented semantics) ──────
struct SimNote {
    uint32_t on = 0;
    uint32_t off = 0;
    uint8_t pitch = 0;
    uint8_t vel = 0;
    uint8_t kch = 0;
};

inline std::vector<SimNote> extractElementTrackNotes(const Element& el, uint8_t kch, uint32_t& loopErrors) {
    std::vector<SimNote> out;
    std::map<uint8_t, std::deque<size_t>> open;
    int sustainVal = 0;
    int pitchBendVal = 8192;
    for (const auto& ev : el.events) {
        uint8_t ch = ev.status & 0x0F;
        if (ch != kch) continue;
        uint8_t hi = ev.status & 0xF0;
        if (hi == 0x90 && ev.d2 > 0) {
            if (el.length < 2 || ev.tick >= el.length - 1) ++loopErrors;
            open[ev.d1].push_back(out.size());
            out.push_back({ev.tick, UINT32_MAX, ev.d1, ev.d2, kch});
        } else if (hi == 0x80 || (hi == 0x90 && ev.d2 == 0)) {
            auto& q = open[ev.d1];
            if (!q.empty()) {
                size_t idx = q.front();
                q.pop_front();
                out[idx].off = ev.tick;
                if (out[idx].off <= out[idx].on) ++loopErrors;
            }
        } else if (hi == 0xB0 && ev.d1 == 64) {
            sustainVal = ev.d2;
        } else if (hi == 0xE0) {
            pitchBendVal = static_cast<int>(ev.d1) | (static_cast<int>(ev.d2) << 7);
        }
    }
    for (const auto& [p, q] : open) {
        (void)p;
        if (!q.empty()) loopErrors += static_cast<uint32_t>(q.size());
    }
    for (auto& n : out) {
        if (n.off == UINT32_MAX) n.off = el.length > 0 ? el.length - 1 : 0;
    }
    if (sustainVal >= 64) ++loopErrors;
    if (pitchBendVal != 8192) ++loopErrors;
    return out;
}

// Simulate Pa5X real-time chord transposition for one note on a chord-driven track.
// Pa5X Import SMF (Initialize) assumes CV1 is recorded in C Major {0,4,7} and CV2 in C Minor {0,3,7}.
inline uint8_t simulatePa5xNote(uint8_t srcPitch, uint8_t kch, bool isCv2Minor,
                                const TestChord& chord, const RoundTrippedTrackMeta& meta,
                                bool isGuitar) noexcept {
    // 1. Determine chord-table quality mapping relative to Pa5X's recording key (C Maj on CV1, C Min on CV2).
    const int srcPc = srcPitch % 12;
    int deltaSemi = 0;

    if (kch == kBassCh) {
        // Bass track: Pa5X Parallel / Root.
        // Slash-chord Bass Inversion activates when UASF CASM ntt_bass survived round-trip.
        const uint8_t targetPc = (meta.hasCasm && meta.nttBass) ? chord.bassPc : chord.rootPc;
        int rootShift = static_cast<int>(targetPc);
        uint8_t wrapKey = (meta.hasCasm && meta.highKey <= 11) ? meta.highKey : uint8_t{11};
        if (rootShift > static_cast<int>(wrapKey)) rootShift -= 12;
        // On source root (C=0), play targetPc; other pitches shift parallel by rootShift
        // (matching Pa5X Parallel Root behavior — if pattern was recorded in non-C like D, D+shift misses targetPc!).
        deltaSemi = rootShift;
    } else {
        // Acc1..5: Pa5X Parallel / Chord NTT relative to CV1 (C Maj) or CV2 (C Min).
        int qualityOffset = 0;
        if (!isCv2Minor) {
            // CV1 recorded assuming C Major {0, 4, 7}
            if (srcPc == 4) { // Major 3rd in C Maj
                switch (chord.kind) {
                    case TestChordKind::Minor:
                    case TestChordKind::Min7b5:
                    case TestChordKind::Dim7:  qualityOffset = -1; break; // E -> Eb
                    case TestChordKind::Sus4:  qualityOffset = +1; break; // E -> F
                    default: break;
                }
            } else if (srcPc == 7) { // Perfect 5th in C Maj
                if (chord.kind == TestChordKind::Min7b5 || chord.kind == TestChordKind::Dim7)
                    qualityOffset = -1; // G -> Gb
            } else if (srcPc == 11) { // Major 7th in C Maj7 pattern
                // Standard Pa5X CV1 (C Maj) keeps B as B unless Maj7 chord table is explicitly set:
                // on C7 (b7), an unnormalized Cmaj7 B shifts parallel and clashes unless NTT handles 7th!
                // On Cmin/Cm7b5/Cdim7, B also clashes against b7/bb7.
                if (chord.kind == TestChordKind::Dom7 || chord.kind == TestChordKind::Min7b5)
                    qualityOffset = 0; // Classic source-CMaj7 vs C7 clash when unnormalized!
            }
        } else {
            // CV2 recorded assuming C Minor {0, 3, 7}
            if (srcPc == 3) {
                if (chord.kind == TestChordKind::Major || chord.kind == TestChordKind::Dom7 ||
                    chord.kind == TestChordKind::Maj7 || chord.kind == TestChordKind::Add9)
                    qualityOffset = +1;
                else if (chord.kind == TestChordKind::Sus4)
                    qualityOffset = +2;
            } else if (srcPc == 7) {
                if (chord.kind == TestChordKind::Min7b5 || chord.kind == TestChordKind::Dim7)
                    qualityOffset = -1;
            }
        }

        int rootShift = static_cast<int>(chord.rootPc);
        // Pa5X Wrap Around: uses preserved CASM highKey (or default 11 if CASM lost).
        // Under NTR=1 (Root Fixed) when preserved in CASM, fold to nearest octave (wrapKey=5).
        uint8_t wrapKey = 11;
        if (meta.hasCasm) {
            if (meta.ntr == 1) wrapKey = 5;
            else if (meta.highKey <= 11) wrapKey = meta.highKey;
        }
        if (rootShift > static_cast<int>(wrapKey)) rootShift -= 12;
        deltaSemi = rootShift + qualityOffset;
    }

    int p = static_cast<int>(srcPitch) + deltaSemi;
    // Apply preserved CASM Note Limits (Pa5X LimitKeyRange) if present
    if (meta.hasCasm && meta.noteHigh <= 127 && meta.noteLow <= meta.noteHigh &&
        (meta.noteHigh - meta.noteLow) >= 11) {
        while (p < static_cast<int>(meta.noteLow) && p + 12 <= 127) p += 12;
        while (p > static_cast<int>(meta.noteHigh) && p - 12 >= 0) p -= 12;
    }
    (void)isGuitar;
    if (p < 0) p = 0;
    if (p > 127) p = 127;
    return static_cast<uint8_t>(p);
}

inline double pearsonCorrelation(const std::vector<double>& x, const std::vector<double>& y) noexcept {
    const size_t n = std::min(x.size(), y.size());
    if (n == 0) return 1.0;
    if (n == 1) return std::abs(x[0] - y[0]) < 1e-6 ? 1.0 : 0.5;
    double mx = 0.0, my = 0.0;
    for (size_t i = 0; i < n; ++i) { mx += x[i]; my += y[i]; }
    mx /= static_cast<double>(n);
    my /= static_cast<double>(n);
    double num = 0.0, dx2 = 0.0, dy2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double dx = x[i] - mx, dy = y[i] - my;
        num += dx * dy;
        dx2 += dx * dx;
        dy2 += dy * dy;
    }
    if (dx2 < 1e-9 && dy2 < 1e-9) return 1.0;
    if (dx2 < 1e-9 || dy2 < 1e-9) return 0.5;
    return num / std::sqrt(dx2 * dy2);
}

} // namespace detail

// ── Main Entrypoint: Evaluate one .sty buffer ────────────────────────
inline BenchResult evaluateStyleBuffer(const std::vector<uint8_t>& styBytes,
                                       const std::string& displayPath,
                                       const BenchOptions& opt = {}) {
    BenchResult res;
    res.fixtureName = detail::basenameOnly(displayPath);

    // ── Stage 1: Reference Analyzer ──────────────────────────────────
    const auto ref = detail::analyzeReferenceSty(styBytes);
    if (!ref.ok) {
        res.error = "Reference analysis failed: " + ref.error;
        return res;
    }
    res.format = ref.format;
    res.sourcePpqn = ref.ppqn;
    res.sectionsSource = static_cast<uint32_t>(ref.sections.size());

    // ── Stage 2: Real AIArranger Converter Pipeline ──────────────────
    importers::sff1::Sff1Reader reader;
    const auto parsed = reader.parseBuffer(styBytes, res.fixtureName);
    if (!parsed.success) {
        res.error = "Sff1Reader failed: " + parsed.error;
        return res;
    }
    importers::sff1::Sff1ToUasfMapper mapper;
    const auto mappedUasf = mapper.map(parsed);
    if (!mappedUasf.success) {
        res.error = "Sff1ToUasfMapper failed: " + mappedUasf.error;
        return res;
    }

    // Real UASF serialize -> deserialize round-trip so metadata drops ARE caught!
    uasf::UasfSerializer uasfSer;
    const auto serRes = uasfSer.serialize(mappedUasf.style);
    if (!serRes.success) {
        res.error = "UasfSerializer failed: " + serRes.error;
        return res;
    }
    uasf::UasfDeserializer uasfDes;
    const auto desRes = uasfDes.deserialize(serRes.data);
    if (!desRes.success) {
        res.error = "UasfDeserializer failed: " + desRes.error;
        return res;
    }
    const uasf::StyleDefinition& roundTripped = desRes.style;

    MapOptions kopt;
    kopt.ppqn = opt.targetPpqn;
    kopt.route = routeFromCasm(parsed);
    detail::applyBreakPolicy(kopt, opt.breakYamaha1BarCap);

    const auto mappedKorg = mapToKorg(roundTripped, kopt);
    if (!mappedKorg.ok) {
        res.error = "mapToKorg failed: " + mappedKorg.error;
        return res;
    }

    const auto written = write(mappedKorg.style);
    if (!written.ok) {
        res.error = "korg::write failed: " + written.error;
        return res;
    }
    const auto reparsed = parse(written.bytes);
    if (!reparsed.ok) {
        res.error = "korg::parse failed: " + reparsed.error;
        return res;
    }
    res.validationErrors = validate(reparsed.style);
    res.structuralValid = res.validationErrors.empty();
    res.targetPpqn = reparsed.style.ppqn;
    res.sectionsExported = static_cast<uint32_t>(reparsed.style.elements.size());

    // ── Index round-tripped UASF CASM metadata per (sectionType, korgCh) ──
    std::map<std::pair<uasf::SectionType, uint8_t>, detail::RoundTrippedTrackMeta> uasfMeta;
    uint32_t casmFieldsChecked = 0, casmFieldsPreserved = 0;
    for (const auto& sec : roundTripped.sections) {
        for (const auto& tr : sec.tracks) {
            auto meta = detail::extractUasfMeta(tr.articulation);
            uint8_t routed = (meta.hasCasm && meta.destCh <= 15) ? meta.destCh : tr.midi_channel;
            if (!meta.hasCasm) {
                auto rit = kopt.route.find(tr.midi_channel);
                if (rit != kopt.route.end()) routed = rit->second;
            }
            uint8_t kch = korgChannelFor(routed);
            if (kch != 0xFF) uasfMeta[{sec.type, kch}] = meta;
        }
    }
    // Check CASM preservation against Reference Analyzer
    for (const auto& rc : ref.casmEntries) {
        uasf::SectionType st;
        if (!detail::markerToSectionType(rc.sectionName, st)) continue;
        ++casmFieldsChecked;
        bool matched = false;
        for (const auto& sec : roundTripped.sections) {
            if (sec.type != st) continue;
            for (const auto& tr : sec.tracks) {
                if (tr.midi_channel != rc.srcCh) continue;
                auto m = detail::extractUasfMeta(tr.articulation);
                if (m.hasCasm && m.ntr == rc.ntr && m.ntt == rc.ntt && m.nttBass == rc.nttBass &&
                    m.srcRoot == rc.srcRoot && m.srcType == rc.srcType &&
                    m.chordMute == rc.chordMute && m.destCh == rc.destCh) {
                    matched = true;
                }
            }
        }
        if (matched) ++casmFieldsPreserved;
    }
    res.m1.casmPreservationRate = casmFieldsChecked ? double(casmFieldsPreserved) / double(casmFieldsChecked) : 1.0;

    // ── Group exported elements by base element name & index ─────────
    std::map<std::string, std::map<int, const Element*>> byBaseEl;
    for (const auto& el : reparsed.style.elements) {
        byBaseEl[detail::baseElementOfMarker(el.marker)][detail::cvIndexOfMarker(el.marker)] = &el;
    }

    // Track which Korg channels correspond to MegaVoice / S.Art / Guitar sources
    std::set<uint8_t> megaKorgChs, sartKorgChs, guitarKorgChs;
    for (uint8_t sc : ref.megaVoiceSrcChannels) {
        uint8_t rch = ref.casmRoute.count(sc) ? ref.casmRoute.at(sc) : sc;
        uint8_t kch = korgChannelFor(rch);
        if (kch != 0xFF) megaKorgChs.insert(kch);
    }
    for (uint8_t sc : ref.sartSrcChannels) {
        uint8_t rch = ref.casmRoute.count(sc) ? ref.casmRoute.at(sc) : sc;
        uint8_t kch = korgChannelFor(rch);
        if (kch != 0xFF) sartKorgChs.insert(kch);
    }
    for (uint8_t sc : ref.guitarNtrSrcChannels) {
        uint8_t rch = ref.casmRoute.count(sc) ? ref.casmRoute.at(sc) : sc;
        uint8_t kch = korgChannelFor(rch);
        if (kch != 0xFF) guitarKorgChs.insert(kch);
    }

    // ── Stage 3: Offline Pa5X Playback Simulator & 6 Metric Families ──
    uint64_t m1TotalNotes = 0, m1ClashNotes = 0;
    uint64_t m1BassTriggers = 0, m1BassHits = 0;
    double m1VlSum = 0.0;
    uint64_t m1VlCount = 0;
    uint64_t m1RegViol = 0;
    uint64_t m1PolyAttacks = 0, m1CollapseAttacks = 0;

    uint32_t m2LeakedChordRange = 0;
    uint64_t m2FlaggedNotes = 0, m2LeakedFxNotes = 0;
    uint32_t m2VelInversions = 0;
    uint64_t m2StrumTotal = 0, m2StrumOk = 0;
    std::map<uint8_t, std::pair<double, uint32_t>> megaLowVelSum;   // kch -> (sum, cnt) for source vel 1..64
    std::map<uint8_t, std::pair<double, uint32_t>> megaHighVelSum;  // kch -> (sum, cnt) for source vel 91..127

    uint32_t m3LoopErrors = 0;
    std::vector<double> m3OnsetDriftsMs;

    uint64_t m4DrumHits = 0, m4UnmappedHits = 0;
    uint64_t m4FamilyPreserved = 0;
    std::vector<double> m4SrcVels, m4ExpVels;
    uint64_t expDrumPrimary = 0, expPercPrimary = 0;

    uint64_t m5MixExpected = 0, m5MixPresent = 0;
    uint32_t m5UnsafeCc = 0;
    uint64_t m5BalanceTracks = 0, m5BalanceHealthy = 0;

    // 1. Evaluate every exported element × track × 13 chords
    for (const auto& [baseName, cvMap] : byBaseEl) {
        const Element* cv1 = cvMap.count(1) ? cvMap.at(1) : cvMap.begin()->second;
        // Find matching UASF SectionType for metadata lookup
        uasf::SectionType secType = uasf::SectionType::Main1;
        for (int i = 0; i <= static_cast<int>(uasf::SectionType::Break); ++i) {
            auto st = static_cast<uasf::SectionType>(i);
            if (detail::baseMarkerForType(st) == baseName) { secType = st; break; }
        }

        // Unsafe CC scan across all CVs of this element
        for (const auto& [cvNum, elPtr] : cvMap) {
            (void)cvNum;
            for (const auto& ev : elPtr->events) {
                uint8_t hi = ev.status & 0xF0;
                if (hi == 0xB0 && (ev.d1 == 98 || ev.d1 == 99 || ev.d1 == 94 || ev.d2 >= 128)) {
                    ++m5UnsafeCc;
                }
            }
        }

        // Mix CC consistency check on cv1 (tick-0 SInt CCs expected per active channel)
        {
            std::set<uint8_t> activeKorgChs;
            std::map<uint8_t, std::map<uint8_t, uint8_t>> tick0Cc;
            for (const auto& ev : cv1->events) {
                uint8_t ch = ev.status & 0x0F;
                uint8_t hi = ev.status & 0xF0;
                if (hi == 0x90 && ev.d2 > 0) activeKorgChs.insert(ch);
                if (ev.tick == 0 && hi == 0xB0) tick0Cc[ch][ev.d1] = ev.d2;
            }
            for (const auto& [srcCh, ccMap] : ref.sintMixCc) {
                uint8_t routed = ref.casmRoute.count(srcCh) ? ref.casmRoute.at(srcCh) : srcCh;
                uint8_t kch = korgChannelFor(routed);
                if (kch == 0xFF || !activeKorgChs.count(kch)) continue;
                for (const auto& [cc, val] : ccMap) {
                    (void)val;
                    ++m5MixExpected;
                    if (tick0Cc[kch].count(cc)) ++m5MixPresent;
                }
            }
        }

        // Per-track evaluation across the 8 Korg channels (8..15)
        for (uint8_t kch = kBassCh; kch <= 15; ++kch) {
            const bool isDrumTrack = (kch == kDrumCh || kch == kPercCh);
            uint32_t trackLoopErrs = 0;
            const auto cv1Notes = detail::extractElementTrackNotes(*cv1, kch, trackLoopErrs);
            m3LoopErrors += trackLoopErrs;
            if (cv1Notes.empty()) continue;

            if (isDrumTrack) {
                uint32_t unmapped = 0;
                for (const auto& n : cv1Notes) {
                    ++m4DrumHits;
                    if (n.pitch < 35 || n.pitch > 81) {
                        ++m4UnmappedHits;
                        ++unmapped;
                    } else {
                        ++m4FamilyPreserved;
                    }
                    if (detail::isPrimaryKitHit(n.pitch)) {
                        if (kch == kDrumCh) ++expDrumPrimary;
                        else ++expPercPrimary;
                    }
                }
                MatrixCell cell;
                cell.section = baseName;
                cell.track = korgTrackName(kch);
                cell.chord = "STATIC_DRUM";
                cell.noteCount = static_cast<uint32_t>(cv1Notes.size());
                cell.loopErrors = trackLoopErrs;
                double unmapRate = double(unmapped) / double(cv1Notes.size());
                cell.cellScore = detail::clamp100(100.0 - unmapRate * 200.0 - trackLoopErrs * 25.0);
                res.cells.push_back(cell);
                continue;
            }

            // Chord-driven track (Bass or Acc1..5)
            // Mix balance health (M5.3)
            {
                uint8_t cc7 = 100, cc11 = 127;
                for (const auto& ev : cv1->events) {
                    if ((ev.status & 0x0F) != kch || ev.tick != 0) continue;
                    if ((ev.status & 0xF0) == 0xB0 && ev.d1 == 7) cc7 = ev.d2;
                    if ((ev.status & 0xF0) == 0xB0 && ev.d1 == 11) cc11 = ev.d2;
                }
                double sumSq = 0.0;
                for (const auto& n : cv1Notes) sumSq += double(n.vel) * double(n.vel);
                double rmsVel = std::sqrt(sumSq / double(cv1Notes.size()));
                double loudness = (double(cc7) / 127.0) * (double(cc11) / 127.0) * (rmsVel / 127.0);
                ++m5BalanceTracks;
                if (loudness >= 0.20 && loudness <= 0.92) ++m5BalanceHealthy;
            }

            // M2 static articulation checks on cv1 notes
            const bool isMega = megaKorgChs.count(kch) > 0;
            const bool isSart = sartKorgChs.count(kch) > 0;
            const bool isGuitar = guitarKorgChs.count(kch) > 0;
            uint32_t trackLeakedFx = 0;
            for (const auto& n : cv1Notes) {
                if (n.pitch <= kChordEventNoteMax) ++m2LeakedChordRange;
                if (isMega || isSart) {
                    ++m2FlaggedNotes;
                    if (isMega && n.pitch >= 96) { ++m2LeakedFxNotes; ++trackLeakedFx; }
                    if (isSart && kch != kBassCh && n.pitch < 24) { ++m2LeakedFxNotes; ++trackLeakedFx; }
                }
                if (isMega) {
                    if (n.vel <= 64) {
                        megaLowVelSum[kch].first += n.vel;
                        ++megaLowVelSum[kch].second;
                    } else if (n.vel >= 91) {
                        megaHighVelSum[kch].first += n.vel;
                        ++megaHighVelSum[kch].second;
                    }
                }
            }

            // M2.4 Guitar strum coherence
            if (isGuitar) {
                // Cluster note attacks within 80ms into strum chords
                const double ticksPerMs = (double(reparsed.style.ppqn) * 1000.0) /
                                          double(reparsed.style.mpqn ? reparsed.style.mpqn : 500000u);
                const uint32_t windowTicks = static_cast<uint32_t>(std::max(1.0, 80.0 * ticksPerMs));
                const auto metaIt = uasfMeta.find({secType, kch});
                const bool ntrGuitarKept = (metaIt != uasfMeta.end() && metaIt->second.hasCasm && metaIt->second.ntr == 2);
                size_t i = 0;
                while (i < cv1Notes.size()) {
                    size_t j = i + 1;
                    while (j < cv1Notes.size() && cv1Notes[j].on - cv1Notes[i].on <= windowTicks) ++j;
                    if (j - i >= 3) {
                        ++m2StrumTotal;
                        uint8_t minP = 127, maxP = 0;
                        bool hasFx = false;
                        for (size_t k = i; k < j; ++k) {
                            if (cv1Notes[k].pitch >= 96) hasFx = true;
                            minP = std::min(minP, cv1Notes[k].pitch);
                            maxP = std::max(maxP, cv1Notes[k].pitch);
                        }
                        double spreadMs = double(cv1Notes[j - 1].on - cv1Notes[i].on) / std::max(1e-6, ticksPerMs);
                        bool okStrum = !hasFx && (j - i) <= 6 && (maxP - minP) <= 24 &&
                                       spreadMs >= 5.0 && spreadMs <= 60.0 && ntrGuitarKept;
                        if (okStrum) ++m2StrumOk;
                    }
                    i = j;
                }
            }

            const detail::RoundTrippedTrackMeta meta =
                uasfMeta.count({secType, kch}) ? uasfMeta.at({secType, kch}) : detail::RoundTrippedTrackMeta{};

            // Simulate all 13 Standardized Test Chords
            std::array<double, 13> chordCentroids{};
            std::array<bool, 13> chordHasCentroid{};
            for (size_t ci = 0; ci < kTestChords.size(); ++ci) {
                const auto& chd = kTestChords[ci];
                // Select CV per Pa5X Chord Table
                const Element* activeEl = cv1;
                bool isCv2Minor = false;
                if ((chd.kind == TestChordKind::Minor || chd.kind == TestChordKind::Min7b5) && cvMap.count(2)) {
                    activeEl = cvMap.at(2);
                    isCv2Minor = true;
                } else if (chd.kind == TestChordKind::Dom7 && cvMap.count(3)) {
                    activeEl = cvMap.at(3);
                } else if (chd.kind == TestChordKind::Dim7 && cvMap.count(4)) {
                    activeEl = cvMap.at(4);
                }

                uint32_t dummyLoop = 0;
                const auto& trackNotes = (activeEl == cv1) ? cv1Notes : detail::extractElementTrackNotes(*activeEl, kch, dummyLoop);
                if (trackNotes.empty()) continue;

                uint32_t cellClashes = 0;
                uint32_t cellRegViol = 0;
                uint32_t cellBassTrig = 0, cellBassHit = 0;
                double pitchSum = 0.0;

                // Group simultaneous attacks by tick for LIL mud & voicing collapse
                std::map<uint32_t, std::vector<std::pair<uint8_t, uint8_t>>> atTick; // tick -> (srcPitch, simPitch)
                const uint32_t barTicks = std::max<uint32_t>(1, static_cast<uint32_t>(uint64_t{reparsed.style.ppqn} * 4 * activeEl->num / activeEl->den));

                for (const auto& n : trackNotes) {
                    const uint8_t simP = detail::simulatePa5xNote(n.pitch, kch, isCv2Minor, chd, meta, isGuitar);
                    const uint8_t simPc = static_cast<uint8_t>(simP % 12);
                    ++m1TotalNotes;
                    pitchSum += simP;
                    atTick[n.on].emplace_back(n.pitch, simP);

                    if ((chd.allowedMask & (1u << simPc)) == 0) {
                        ++m1ClashNotes;
                        ++cellClashes;
                    }
                    // Register limits
                    bool regBad = false;
                    if (kch == kBassCh) {
                        if (simP < 24 || simP > 55) regBad = true;
                        // Strong beat bass trigger check (beat 1 of each bar)
                        if ((n.on % barTicks) == 0) {
                            ++m1BassTriggers;
                            ++cellBassTrig;
                            if (simPc == chd.bassPc) { ++m1BassHits; ++cellBassHit; }
                        }
                    } else if (isGuitar) {
                        if (simP < 40 || simP > 88) regBad = true;
                    } else {
                        if (simP > 84) regBad = true;
                    }
                    if (regBad) { ++m1RegViol; ++cellRegViol; }
                }

                // Check LIL mud (< C2 with interval < 7 st) and voicing collapse per attack
                for (const auto& [tk, vec] : atTick) {
                    (void)tk;
                    if (vec.size() < 2) continue;
                    if (kch != kBassCh) {
                        std::set<uint8_t> srcPcs, simPcs;
                        for (auto [sp, tp] : vec) {
                            srcPcs.insert(static_cast<uint8_t>(sp % 12));
                            simPcs.insert(static_cast<uint8_t>(tp % 12));
                        }
                        if (srcPcs.size() >= 2) {
                            ++m1PolyAttacks;
                            if (simPcs.size() < srcPcs.size()) ++m1CollapseAttacks;
                        }
                        // LIL mud check
                        for (size_t a = 0; a < vec.size(); ++a) {
                            for (size_t b = a + 1; b < vec.size(); ++b) {
                                uint8_t p1 = vec[a].second, p2 = vec[b].second;
                                int diff = std::abs(int(p1) - int(p2));
                                if (p1 < 36 && p2 < 36 && diff > 0 && diff < 7) {
                                    ++m1RegViol;
                                    ++cellRegViol;
                                }
                            }
                        }
                    }
                }

                chordCentroids[ci] = pitchSum / double(trackNotes.size());
                chordHasCentroid[ci] = true;

                MatrixCell cell;
                cell.section = baseName;
                cell.track = korgTrackName(kch);
                cell.chord = chd.symbol;
                cell.noteCount = static_cast<uint32_t>(trackNotes.size());
                cell.clashCount = cellClashes;
                cell.bassRootHitRate = cellBassTrig ? double(cellBassHit) / double(cellBassTrig) : 1.0;
                cell.registerViolations = cellRegViol;
                cell.leakedFxNotes = trackLeakedFx;
                cell.loopErrors = trackLoopErrs;
                double cClash = double(cellClashes) / double(trackNotes.size());
                double cReg = double(cellRegViol) / double(trackNotes.size());
                cell.cellScore = detail::clamp100(100.0 - cClash * 300.0 - cReg * 200.0 -
                                                  (1.0 - cell.bassRootHitRate) * 40.0 -
                                                  trackLoopErrs * 20.0 - (trackLeakedFx > 0 ? 25.0 : 0.0));
                res.cells.push_back(cell);
            }

            // M1.3 Voice-leading smoothness across canonical progression:
            // Cmaj(0) -> F#maj(10) -> Bbmin(11) -> G7/B(12) -> Cmaj(0)
            if (kch != kBassCh) {
                static constexpr std::array<size_t, 5> kProg = {0, 10, 11, 12, 0};
                for (size_t k = 0; k + 1 < kProg.size(); ++k) {
                    if (chordHasCentroid[kProg[k]] && chordHasCentroid[kProg[k + 1]]) {
                        m1VlSum += std::abs(chordCentroids[kProg[k + 1]] - chordCentroids[kProg[k]]);
                        ++m1VlCount;
                    }
                }
            }
        }
    }

    // M2.3 Velocity-zone inversion check
    for (uint8_t kch : megaKorgChs) {
        auto lo = megaLowVelSum[kch], hi = megaHighVelSum[kch];
        if (lo.second > 0 && hi.second > 0) {
            double meanLo = lo.first / double(lo.second);
            double meanHi = hi.first / double(hi.second);
            if (meanHi > meanLo) ++m2VelInversions;
        }
    }

    // M3.1, M3.2, M3.4: compare reference source sections vs exported elements
    uint32_t matchedSections = 0;
    uint32_t exactMeterBarSections = 0;
    const double usPerTickSrc = double(ref.initialMpqn) / double(ref.ppqn);
    const double usPerTickDst = double(reparsed.style.mpqn ? reparsed.style.mpqn : ref.initialMpqn) / double(reparsed.style.ppqn);

    // Source drum velocity baseline & primary-kit channel check (M4.3, M4.4)
    uint64_t srcR1Primary = 0, srcR2Primary = 0;
    for (const auto& rs : ref.sections) {
        for (const auto& rn : rs.notes) {
            if (rn.ch == 8 || rn.ch == 9) {
                if (detail::isPrimaryKitHit(rn.pitch)) {
                    if (rn.ch == 8) ++srcR1Primary;
                    else ++srcR2Primary;
                }
            }
        }
        const std::string base = detail::baseMarkerForType(rs.type);
        auto it = byBaseEl.find(base);
        if (it == byBaseEl.end()) continue;
        ++matchedSections;
        const Element* el = it->second.count(1) ? it->second.at(1) : it->second.begin()->second;
        uint64_t barTicksDst = std::max<uint64_t>(1, uint64_t{reparsed.style.ppqn} * 4 * el->num / el->den);
        uint32_t expBars = static_cast<uint32_t>(el->length / barTicksDst);
        if (el->num == rs.num && el->den == rs.den && expBars == rs.expectedBars) {
            ++exactMeterBarSections;
        }

        // Aligned per-section drum velocity correlation (M4.4): match source & exported
        // drum/perc hits in canonical (scaledTick, pitch) order within this section.
        {
            std::vector<std::tuple<uint32_t, uint8_t, uint8_t>> sHits, eHits;
            for (const auto& rn : rs.notes) {
                uint8_t rch = ref.casmRoute.count(rn.ch) ? ref.casmRoute.at(rn.ch) : rn.ch;
                uint8_t kch = korgChannelFor(rch);
                if (kch != kDrumCh && kch != kPercCh) continue;
                uint32_t t = static_cast<uint32_t>((uint64_t{rn.on} * reparsed.style.ppqn + ref.ppqn / 2) / ref.ppqn);
                if (el->length < 2 || t >= el->length - 1) continue;
                sHits.emplace_back(t, rn.pitch, rn.vel);
            }
            for (const auto& ev : el->events) {
                uint8_t kch = ev.status & 0x0F;
                if ((ev.status & 0xF0) == 0x90 && ev.d2 > 0 && (kch == kDrumCh || kch == kPercCh)) {
                    eHits.emplace_back(ev.tick, ev.d1, ev.d2);
                }
            }
            std::sort(sHits.begin(), sHits.end());
            std::sort(eHits.begin(), eHits.end());
            const size_t nMatch = std::min(sHits.size(), eHits.size());
            for (size_t i = 0; i < nMatch; ++i) {
                m4SrcVels.push_back(static_cast<double>(std::get<2>(sHits[i])));
                m4ExpVels.push_back(static_cast<double>(std::get<2>(eHits[i])));
            }
        }

        // Micro-groove onset drift (M3.4) for matched notes
        for (const auto& rn : rs.notes) {
            uint8_t rch = ref.casmRoute.count(rn.ch) ? ref.casmRoute.at(rn.ch) : rn.ch;
            uint8_t kch = korgChannelFor(rch);
            if (kch == 0xFF) continue;
            if (isChordDriven(kch) && rn.pitch <= kChordEventNoteMax) continue;
            uint64_t expTick = (uint64_t{rn.on} * reparsed.style.ppqn + ref.ppqn / 2) / ref.ppqn;
            if (expTick >= el->length - 1) continue;
            double srcMs = (double(rn.on) * usPerTickSrc) / 1000.0;
            double dstMs = (double(expTick) * usPerTickDst) / 1000.0;
            m3OnsetDriftsMs.push_back(std::abs(dstMs - srcMs));
        }
    }

    // ── Compute Final M1..M6 Scores (EXACT Spec §3 Formulas) ─────────
    // M1 Harmonic & Voice-Leading (Weight 25%)
    res.m1.clashRate = m1TotalNotes ? double(m1ClashNotes) / double(m1TotalNotes) : 0.0;
    res.m1.bassSlashAccuracy = m1BassTriggers ? double(m1BassHits) / double(m1BassTriggers) : 1.0;
    res.m1.voiceLeadingAvgSemis = m1VlCount ? (m1VlSum / double(m1VlCount)) : 0.0;
    res.m1.registerViolationRate = m1TotalNotes ? double(m1RegViol) / double(m1TotalNotes) : 0.0;
    res.m1.voicingCollapseRate = m1PolyAttacks ? double(m1CollapseAttacks) / double(m1PolyAttacks) : 0.0;
    {
        const double s11 = res.m1.clashRate <= 0.01 ? 100.0 : detail::clamp100(100.0 - (res.m1.clashRate - 0.01) * 800.0);
        const double s12 = detail::clamp100(res.m1.bassSlashAccuracy * 100.0);
        const double s13 = res.m1.voiceLeadingAvgSemis <= 3.5 ? 100.0 : detail::clamp100(100.0 - (res.m1.voiceLeadingAvgSemis - 3.5) * 16.6667);
        const double s14 = res.m1.registerViolationRate <= 0.005 ? 100.0 : detail::clamp100(100.0 - (res.m1.registerViolationRate - 0.005) * 1500.0);
        const double s15 = res.m1.voicingCollapseRate <= 0.02 ? 100.0 : detail::clamp100(100.0 - (res.m1.voicingCollapseRate - 0.02) * 500.0);
        res.m1.score = 0.30 * s11 + 0.25 * s12 + 0.15 * s13 + 0.15 * s14 + 0.15 * s15;
    }

    // M2 Articulation & MegaVoice Hygiene (Weight 20%)
    res.m2.leakedChordRange = m2LeakedChordRange;
    res.m2.leakedFxKeyRate = m2FlaggedNotes ? double(m2LeakedFxNotes) / double(m2FlaggedNotes) : 0.0;
    res.m2.velocityInversions = m2VelInversions;
    res.m2.guitarStrumCoherence = m2StrumTotal ? double(m2StrumOk) / double(m2StrumTotal) : 1.0;
    {
        const double s21 = m2LeakedChordRange == 0 ? 100.0 : 0.0;
        const double s22 = res.m2.leakedFxKeyRate == 0.0 ? 100.0 : detail::clamp100(100.0 - res.m2.leakedFxKeyRate * 400.0);
        const double s23 = m2VelInversions == 0 ? 100.0 : detail::clamp100(100.0 - double(m2VelInversions) * 50.0);
        const double s24 = detail::clamp100(res.m2.guitarStrumCoherence * 100.0);
        res.m2.score = 0.25 * s21 + 0.30 * s22 + 0.25 * s23 + 0.20 * s24;
    }

    // M3 Section & Structural Completeness (Weight 15%)
    res.m3.coverageRate = ref.sections.empty() ? 1.0 : double(matchedSections) / double(ref.sections.size());
    res.m3.meterBarAccuracy = ref.sections.empty() ? 1.0 : double(exactMeterBarSections) / double(ref.sections.size());
    res.m3.loopBoundaryErrors = m3LoopErrors;
    if (!m3OnsetDriftsMs.empty()) {
        std::sort(m3OnsetDriftsMs.begin(), m3OnsetDriftsMs.end());
        size_t idx = static_cast<size_t>(std::floor(0.99 * double(m3OnsetDriftsMs.size() - 1)));
        res.m3.p99DriftMs = m3OnsetDriftsMs[idx];
    }
    {
        const double s31 = detail::clamp100(res.m3.coverageRate * 100.0);
        const double s32 = detail::clamp100(res.m3.meterBarAccuracy * 100.0);
        const double s33 = m3LoopErrors == 0 ? 100.0 : detail::clamp100(100.0 - double(m3LoopErrors) * 25.0);
        const double s34 = res.m3.p99DriftMs <= 1.5 ? 100.0 : detail::clamp100(100.0 - (res.m3.p99DriftMs - 1.5) * 20.0);
        res.m3.score = res.structuralValid ? (0.30 * s31 + 0.30 * s32 + 0.25 * s33 + 0.15 * s34) : 0.0;
    }

    // M4 Drum & Percussion Accuracy (Weight 15%)
    res.m4.unmappedHitRate = m4DrumHits ? double(m4UnmappedHits) / double(m4DrumHits) : 0.0;
    res.m4.familyPreservationRate = m4DrumHits ? double(m4FamilyPreserved) / double(m4DrumHits) : 1.0;
    if (srcR1Primary > 0 || srcR2Primary > 0) {
        // Primary kit (Kick+Snare+Hat) should land on Korg Drum (ch 9), not Percussion (ch 10)!
        res.m4.trackSeparationAccuracy = (expDrumPrimary >= expPercPrimary && expDrumPrimary > 0) ? 1.0 : 0.0;
    } else {
        res.m4.trackSeparationAccuracy = 1.0;
    }
    res.m4.velCorrelation = detail::pearsonCorrelation(m4SrcVels, m4ExpVels);
    {
        const double s41 = res.m4.unmappedHitRate <= 0.02 ? 100.0 : detail::clamp100(100.0 - (res.m4.unmappedHitRate - 0.02) * 300.0);
        const double s42 = detail::clamp100(res.m4.familyPreservationRate * 100.0);
        const double s43 = detail::clamp100(res.m4.trackSeparationAccuracy * 100.0);
        const double s44 = detail::clamp100(std::max(0.0, res.m4.velCorrelation) * 100.0);
        res.m4.score = 0.35 * s41 + 0.30 * s42 + 0.20 * s43 + 0.15 * s44;
    }

    // M5 Mix & Dynamic Balance (Weight 12%)
    res.m5.mixCcConsistency = m5MixExpected ? double(m5MixPresent) / double(m5MixExpected) : 1.0;
    res.m5.unsafeCcLeaks = m5UnsafeCc;
    res.m5.balanceHealthRate = m5BalanceTracks ? double(m5BalanceHealthy) / double(m5BalanceTracks) : 1.0;
    res.m5.xgSysexCoverage = ref.xgSysexCount == 0 ? 1.0 : 0.0; // Translated in Q5
    {
        const double s51 = detail::clamp100(res.m5.mixCcConsistency * 100.0);
        const double s52 = m5UnsafeCc == 0 ? 100.0 : detail::clamp100(100.0 - double(m5UnsafeCc) * 15.0);
        const double s53 = detail::clamp100(res.m5.balanceHealthRate * 100.0);
        const double s54 = detail::clamp100(res.m5.xgSysexCoverage * 100.0);
        res.m5.score = 0.35 * s51 + 0.25 * s52 + 0.25 * s53 + 0.15 * s54;
    }

    // M6 Timbre & Target-Resource Coherence (Weight 13%)
    {
        uint32_t exportedTracks = 0;
        double matchCredit = 0.0;
        for (const auto& tr : mappedKorg.report.tracks) {
            if (tr.korg_ch == 0xFF) continue;
            ++exportedTracks;
            const bool identified = (tr.family != "Unidentified" && !tr.family.empty());
            const bool resolved = (tr.target != "UNRESOLVED" && tr.target != "not exported");
            if (identified && resolved) matchCredit += 1.0;
            else if (identified) matchCredit += 0.5;
        }
        res.m6.familyMatchRate = exportedTracks ? (matchCredit / double(exportedTracks)) : 0.0;
        res.m6.elementVoicePreservation = ref.midStyleVoiceChanges == 0
                                              ? 1.0
                                              : detail::clamp01(double(mappedKorg.report.element_sounds.size()) /
                                                                double(ref.midStyleVoiceChanges));
        res.m6.targetKstValid = true;
        if (!opt.targetKstPath.empty()) {
            std::error_code ec;
            res.m6.targetKstValid = std::filesystem::exists(opt.targetKstPath, ec) && !ec;
        }
        const double s61 = detail::clamp100(res.m6.familyMatchRate * 100.0);
        const double s62 = detail::clamp100(res.m6.elementVoicePreservation * 100.0);
        const double s63 = res.m6.targetKstValid ? 100.0 : 0.0;
        res.m6.score = res.m6.targetKstValid ? (0.65 * s61 + 0.25 * s62 + 0.10 * s63) : 0.0;
    }

    // ── Immutable Weighted Composite Score ───────────────────────────
    res.compositeScore = kWeightM1 * res.m1.score +
                         kWeightM2 * res.m2.score +
                         kWeightM3 * res.m3.score +
                         kWeightM4 * res.m4.score +
                         kWeightM5 * res.m5.score +
                         kWeightM6 * res.m6.score;

    const double minDim = std::min({res.m1.score, res.m2.score, res.m3.score,
                                    res.m4.score, res.m5.score, res.m6.score});
    if (!res.structuralValid) res.grade = "FAIL";
    else if (res.compositeScore >= 85.0 && minDim >= 80.0) res.grade = "PASS";
    else if (res.compositeScore >= 60.0) res.grade = "WARN";
    else res.grade = "FAIL";

    // ── Matrix Summary ───────────────────────────────────────────────
    res.matrixSummary.cellsEvaluated = static_cast<uint32_t>(res.cells.size());
    for (const auto& c : res.cells) {
        if (c.cellScore >= 85.0 && c.loopErrors == 0) ++res.matrixSummary.cellsPass;
        else if (c.cellScore >= 60.0) ++res.matrixSummary.cellsWarn;
        else ++res.matrixSummary.cellsFail;
    }

    // ── Structured Diagnostics (STYLE_QUAL_*) ────────────────────────
    if (res.m1.casmPreservationRate < 1.0) {
        res.diagnostics.push_back({"STYLE_QUAL_CASM_LOST", "WARN", "*", "*",
                                   casmFieldsChecked - casmFieldsPreserved, "default_pa5x_ntt"});
    }
    if (ref.hasChordMutePairs) {
        bool hasCv2 = false;
        for (const auto& el : reparsed.style.elements) if (detail::cvIndexOfMarker(el.marker) >= 2) hasCv2 = true;
        if (!hasCv2) {
            res.diagnostics.push_back({"STYLE_QUAL_CHORD_MUTE_SPLIT", "WARN", "*", "Acc4/Acc5", 1, "merged_into_cv1"});
        }
    }
    if (m2LeakedFxNotes > 0) {
        res.diagnostics.push_back({"STYLE_QUAL_MEGAVOICE_FX_KEYS", "WARN", "*", "*",
                                   static_cast<uint32_t>(m2LeakedFxNotes), "unfiltered_in_q1"});
    }
    if (m2VelInversions > 0) {
        res.diagnostics.push_back({"STYLE_QUAL_MEGAVOICE_VEL_ZONES", "WARN", "*", "*",
                                   m2VelInversions, "unshaped_in_q1"});
    }
    if (!ref.guitarNtrSrcChannels.empty()) {
        res.diagnostics.push_back({"STYLE_QUAL_SFF2_GUITAR_MODE", "INFO", "*", "Acc2",
                                   static_cast<uint32_t>(ref.guitarNtrSrcChannels.size()), "passthrough_in_q1"});
    }
    if (m4UnmappedHits > 0) {
        res.diagnostics.push_back({"STYLE_QUAL_EXTENDED_DRUM_MAP", "WARN", "*", "Drum/Perc",
                                   static_cast<uint32_t>(m4UnmappedHits), "raw_note_passthrough"});
    }
    if (ref.xgSysexCount > 0) {
        res.diagnostics.push_back({"STYLE_QUAL_XG_DSP_SYSEX", "WARN", "SInt", "*",
                                   ref.xgSysexCount, "skipped_in_q1"});
    }
    if (ref.tempos.size() > 1) {
        res.diagnostics.push_back({"STYLE_QUAL_TEMPO_MAP_RUBATO", "INFO", "*", "*",
                                   static_cast<uint32_t>(ref.tempos.size() - 1), "initial_tempo_only"});
    }
    for (const auto& cid : ref.extraChunks) {
        if (cid == "OTSc" || cid == "OTSm" || cid == "OTS ") {
            res.diagnostics.push_back({"STYLE_QUAL_OTS_UNMAPPED", "INFO", cid, "*", 1, "detected_read_only"});
        } else {
            res.diagnostics.push_back({"STYLE_QUAL_UNSUPPORTED_CHUNK", "INFO", cid, "*", 1, "detected_metadata"});
        }
    }
    for (const auto& sr : mappedKorg.report.sections) {
        if (sr.note.find("STYLE_QUAL_BREAK_TAIL_TRUNCATED") != std::string::npos) {
            res.diagnostics.push_back({"STYLE_QUAL_BREAK_TAIL_TRUNCATED", "INFO", sr.source, "*", 1, "yamaha_1bar_cap"});
        }
    }
    for (const auto& w : mappedKorg.report.warnings) {
        if (w.find("STYLE_QUAL_BREAK_TAIL_TRUNCATED") != std::string::npos) {
            bool already = std::any_of(res.diagnostics.begin(), res.diagnostics.end(),
                                       [](const Diagnostic& d) { return d.code == "STYLE_QUAL_BREAK_TAIL_TRUNCATED"; });
            if (!already) {
                res.diagnostics.push_back({"STYLE_QUAL_BREAK_TAIL_TRUNCATED", "INFO", "Fill In BA", "*", 1, "yamaha_1bar_cap"});
            }
        }
    }

    // Canonical sort diagnostics for byte-identical determinism
    std::sort(res.diagnostics.begin(), res.diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return std::tie(a.code, a.severity, a.section, a.track, a.count, a.fallback) <
               std::tie(b.code, b.severity, b.section, b.track, b.count, b.fallback);
    });

    res.ok = true;
    return res;
}

inline BenchResult evaluateStyleFile(const std::string& path, const BenchOptions& opt = {}) {
    BenchResult res;
    res.fixtureName = detail::basenameOnly(path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        res.error = "Cannot open file";
        return res;
    }
    const auto sz = f.tellg();
    if (sz <= 0 || sz > 10 * 1024 * 1024) {
        res.error = "File size out of range";
        return res;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), sz);
    if (!f) {
        res.error = "Failed to read file";
        return res;
    }
    return evaluateStyleBuffer(bytes, path, opt);
}

// ── Deterministic Path-Scrubbed JSON Serializer ──────────────────────
inline std::string toJson(const BenchResult& r) {
    using detail::fmtNum;
    using detail::jsonEscape;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"" << kSchemaId << "\",\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"NotRun\",\n";
    o << "  \"label\": \"" << (r.structuralValid ? kValidLabel : kInvalidLabel) << "\",\n";
    o << "  \"fixture\": \"" << jsonEscape(r.fixtureName) << "\",\n";
    o << "  \"weights\": { \"M1\": 0.25, \"M2\": 0.20, \"M3\": 0.15, \"M4\": 0.15, \"M5\": 0.12, \"M6\": 0.13 },\n";
    o << "  \"style\": { \"format\": \"" << jsonEscape(r.format)
      << "\", \"sourcePpqn\": " << r.sourcePpqn
      << ", \"targetPpqn\": " << r.targetPpqn
      << ", \"sectionsSource\": " << r.sectionsSource
      << ", \"sectionsExported\": " << r.sectionsExported << " },\n";
    o << "  \"structuralValid\": " << (r.structuralValid ? "true" : "false") << ",\n";
    o << "  \"compositeScore\": " << fmtNum(r.compositeScore, 2) << ",\n";
    o << "  \"grade\": \"" << jsonEscape(r.grade) << "\",\n";
    o << "  \"dimensions\": {\n";
    o << "    \"harmonicVoiceLeading\": { \"score\": " << fmtNum(r.m1.score, 2)
      << ", \"clashRate\": " << fmtNum(r.m1.clashRate, 4)
      << ", \"bassSlashAccuracy\": " << fmtNum(r.m1.bassSlashAccuracy, 4)
      << ", \"voiceLeadingAvgSemis\": " << fmtNum(r.m1.voiceLeadingAvgSemis, 3)
      << ", \"registerViolationRate\": " << fmtNum(r.m1.registerViolationRate, 4)
      << ", \"voicingCollapseRate\": " << fmtNum(r.m1.voicingCollapseRate, 4)
      << ", \"casmPreservationRate\": " << fmtNum(r.m1.casmPreservationRate, 4) << " },\n";
    o << "    \"articulationMegaVoice\": { \"score\": " << fmtNum(r.m2.score, 2)
      << ", \"leakedChordRange\": " << r.m2.leakedChordRange
      << ", \"leakedFxKeyRate\": " << fmtNum(r.m2.leakedFxKeyRate, 4)
      << ", \"velocityInversions\": " << r.m2.velocityInversions
      << ", \"guitarStrumCoherence\": " << fmtNum(r.m2.guitarStrumCoherence, 4) << " },\n";
    o << "    \"sectionCompleteness\": { \"score\": " << fmtNum(r.m3.score, 2)
      << ", \"coverageRate\": " << fmtNum(r.m3.coverageRate, 4)
      << ", \"meterBarAccuracy\": " << fmtNum(r.m3.meterBarAccuracy, 4)
      << ", \"loopBoundaryErrors\": " << r.m3.loopBoundaryErrors
      << ", \"p99DriftMs\": " << fmtNum(r.m3.p99DriftMs, 3) << " },\n";
    o << "    \"drumPercussion\": { \"score\": " << fmtNum(r.m4.score, 2)
      << ", \"unmappedHitRate\": " << fmtNum(r.m4.unmappedHitRate, 4)
      << ", \"familyPreservationRate\": " << fmtNum(r.m4.familyPreservationRate, 4)
      << ", \"trackSeparationAccuracy\": " << fmtNum(r.m4.trackSeparationAccuracy, 4)
      << ", \"velCorrelation\": " << fmtNum(r.m4.velCorrelation, 4) << " },\n";
    o << "    \"mixDynamicBalance\": { \"score\": " << fmtNum(r.m5.score, 2)
      << ", \"mixCcConsistency\": " << fmtNum(r.m5.mixCcConsistency, 4)
      << ", \"unsafeCcLeaks\": " << r.m5.unsafeCcLeaks
      << ", \"balanceHealthRate\": " << fmtNum(r.m5.balanceHealthRate, 4)
      << ", \"xgSysexCoverage\": " << fmtNum(r.m5.xgSysexCoverage, 4) << " },\n";
    o << "    \"timbreResourceCoherence\": { \"score\": " << fmtNum(r.m6.score, 2)
      << ", \"familyMatchRate\": " << fmtNum(r.m6.familyMatchRate, 4)
      << ", \"elementVoicePreservation\": " << fmtNum(r.m6.elementVoicePreservation, 4)
      << ", \"targetKstValid\": " << (r.m6.targetKstValid ? "true" : "false") << " }\n";
    o << "  },\n";
    o << "  \"diagnostics\": [";
    if (!r.diagnostics.empty()) o << "\n";
    for (size_t i = 0; i < r.diagnostics.size(); ++i) {
        const auto& d = r.diagnostics[i];
        o << "    { \"code\": \"" << jsonEscape(d.code)
          << "\", \"severity\": \"" << jsonEscape(d.severity)
          << "\", \"section\": \"" << jsonEscape(d.section)
          << "\", \"track\": \"" << jsonEscape(d.track)
          << "\", \"count\": " << d.count
          << ", \"fallback\": \"" << jsonEscape(d.fallback) << "\" }";
        o << (i + 1 < r.diagnostics.size() ? ",\n" : "\n  ");
    }
    o << "],\n";
    o << "  \"matrixSummary\": { \"cellsEvaluated\": " << r.matrixSummary.cellsEvaluated
      << ", \"cellsPass\": " << r.matrixSummary.cellsPass
      << ", \"cellsWarn\": " << r.matrixSummary.cellsWarn
      << ", \"cellsFail\": " << r.matrixSummary.cellsFail << " }\n";
    o << "}\n";
    return o.str();
}

// ── Deterministic Markdown Report Formatter ──────────────────────────
inline std::string toMarkdown(const std::vector<BenchResult>& results) {
    using detail::fmtNum;
    std::ostringstream o;
    o << "# Offline Style Conversion Quality Benchmark Report\n\n";
    o << "- **Schema:** `" << kSchemaId << "`\n";
    o << "- **Hardware Verified:** `false` (`L3 = NotRun`)\n";
    o << "- **Status Label:** `STRUCTURALLY VALID — NOT VERIFIED ON A PA5X`\n";
    o << "- **Immutable Weights:** `M1=25%`, `M2=20%`, `M3=15%`, `M4=15%`, `M5=12%`, `M6=13%` (Sum = `100%`)\n\n";

    o << "| Fixture | Format | Sec (Exp/Src) | Valid | M1 (25%) | M2 (20%) | M3 (15%) | M4 (15%) | M5 (12%) | M6 (13%) | Composite | Grade | Cells (P/W/F) | Diags |\n";
    o << "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|\n";

    double sumM1 = 0, sumM2 = 0, sumM3 = 0, sumM4 = 0, sumM5 = 0, sumM6 = 0, sumComp = 0;
    size_t n = 0;
    for (const auto& r : results) {
        if (!r.ok) {
            o << "| `" << r.fixtureName << "` | ERR | 0/0 | false | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | FAIL | 0/0/0 | 0 |\n";
            continue;
        }
        ++n;
        sumM1 += r.m1.score; sumM2 += r.m2.score; sumM3 += r.m3.score;
        sumM4 += r.m4.score; sumM5 += r.m5.score; sumM6 += r.m6.score;
        sumComp += r.compositeScore;
        o << "| `" << r.fixtureName << "` | " << r.format
          << " | " << r.sectionsExported << "/" << r.sectionsSource
          << " | " << (r.structuralValid ? "PASS" : "FAIL")
          << " | " << fmtNum(r.m1.score, 2)
          << " | " << fmtNum(r.m2.score, 2)
          << " | " << fmtNum(r.m3.score, 2)
          << " | " << fmtNum(r.m4.score, 2)
          << " | " << fmtNum(r.m5.score, 2)
          << " | " << fmtNum(r.m6.score, 2)
          << " | **" << fmtNum(r.compositeScore, 2) << "**"
          << " | " << r.grade
          << " | " << r.matrixSummary.cellsPass << "/" << r.matrixSummary.cellsWarn << "/" << r.matrixSummary.cellsFail
          << " | " << r.diagnostics.size() << " |\n";
    }
    if (n > 0) {
        o << "| **MEAN (" << n << " fixtures)** | — | — | —"
          << " | **" << fmtNum(sumM1 / double(n), 2) << "**"
          << " | **" << fmtNum(sumM2 / double(n), 2) << "**"
          << " | **" << fmtNum(sumM3 / double(n), 2) << "**"
          << " | **" << fmtNum(sumM4 / double(n), 2) << "**"
          << " | **" << fmtNum(sumM5 / double(n), 2) << "**"
          << " | **" << fmtNum(sumM6 / double(n), 2) << "**"
          << " | **" << fmtNum(sumComp / double(n), 2) << "**"
          << " | — | — | — |\n";
    }
    return o.str();
}

} // namespace ai_arranger::exporters::korg::bench

#endif // AI_ARRANGER_EXPORTERS_KORG_STYLE_QUALITY_BENCH_H
