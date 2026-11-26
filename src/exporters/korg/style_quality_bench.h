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
// Immutable benchmark weights (sum = 100% — NEVER MODIFY):
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

// ── Explicit 5-State Benchmark Status Taxonomy ───────────────────────
enum class BenchStatus : uint8_t {
    Pass = 0,
    Fail = 1,
    Unsupported = 2,
    NA = 3,
    NotTested = 4,
};

inline constexpr const char* kStatusPass = "PASS";
inline constexpr const char* kStatusFail = "FAIL";
inline constexpr const char* kStatusUnsupported = "UNSUPPORTED";
inline constexpr const char* kStatusNA = "N/A";
inline constexpr const char* kStatusNotTested = "NOT_TESTED";

inline constexpr const char* benchStatusToString(BenchStatus s) noexcept {
    switch (s) {
        case BenchStatus::Pass:        return kStatusPass;
        case BenchStatus::Fail:        return kStatusFail;
        case BenchStatus::Unsupported: return kStatusUnsupported;
        case BenchStatus::NA:          return kStatusNA;
        case BenchStatus::NotTested:   return kStatusNotTested;
    }
    return kStatusFail;
}

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
    {"Cmaj",  pcMask({0, 4, 7}),     pcMask({0, 2, 4, 7, 9, 11}),       0, 0,  TestChordKind::Major},
    {"Cmin",  pcMask({0, 3, 7}),     pcMask({0, 2, 3, 5, 7, 10}),       0, 0,  TestChordKind::Minor},
    {"C7",    pcMask({0, 4, 7, 10}), pcMask({0, 2, 4, 7, 9, 10}),       0, 0,  TestChordKind::Dom7},
    {"Cmaj7", pcMask({0, 4, 7, 11}), pcMask({0, 2, 4, 7, 9, 11}),       0, 0,  TestChordKind::Maj7},
    {"Cm7b5", pcMask({0, 3, 6, 10}), pcMask({0, 3, 5, 6, 8, 10}),       0, 0,  TestChordKind::Min7b5},
    {"Cdim7", pcMask({0, 3, 6, 9}),  pcMask({0, 2, 3, 5, 6, 8, 9, 11}), 0, 0,  TestChordKind::Dim7},
    {"Csus4", pcMask({0, 5, 7}),     pcMask({0, 2, 5, 7, 9, 10}),       0, 0,  TestChordKind::Sus4},
    {"Cadd9", pcMask({0, 2, 4, 7}),  pcMask({0, 2, 4, 7, 9, 11}),       0, 0,  TestChordKind::Add9},
    {"C/E",   pcMask({0, 4, 7}),     pcMask({0, 2, 4, 7, 9, 11}),       0, 4,  TestChordKind::Major},
    {"C/G",   pcMask({0, 4, 7}),     pcMask({0, 2, 4, 7, 9, 11}),       0, 7,  TestChordKind::Major},
    {"F#maj", pcMask({6, 10, 1}),    pcMask({6, 8, 10, 1, 3, 5}),       6, 6,  TestChordKind::Major},
    {"Bbmin", pcMask({10, 1, 5}),    pcMask({10, 0, 1, 3, 5, 8}),      10, 10, TestChordKind::Minor},
    {"G7/B",  pcMask({7, 11, 2, 5}), pcMask({7, 9, 11, 2, 4, 5}),       7, 11, TestChordKind::Dom7},
}};

// ── Explicit Optional Converter-Emitted Pa5X Style Edit Parameter Sheet ──
// When empty (default in Q1 — because SMF carries MIDI notes/CCs only and Q1
// emits no Style Edit Parameter Sheet), the simulator uses Pa5X Import SMF
// (Initialize) defaults: Parallel NTT, no Bass Inversion, wrapKey = 11,
// no Note Limits, no Guitar Mode. NEVER populated side-channel from UASF!
struct Pa5xTrackParams {
    bool hasOverride = false;
    bool bassInversion = false;
    bool fixedNtr = false;
    bool guitarMode = false;
    uint8_t wrapKey = 11;
    uint8_t limitLow = 0;
    uint8_t limitHigh = 127;
};

struct Pa5xParameterSheet {
    // Key: (baseElement e.g. "v1", korgCh 8..15)
    std::map<std::pair<std::string, uint8_t>, Pa5xTrackParams> tracks;
};

// ── Benchmark Options & Structured Output Models ─────────────────────
struct BenchOptions {
    uint16_t targetPpqn = 384;          // Pa5X native internal resolution (or 0 = keep source)
    bool breakYamaha1BarCap = true;     // Used when BreakPolicy is available in MapOptions
    std::string targetKstPath;          // Optional read-only Target KST path (never written)
    Pa5xParameterSheet parameterSheet;  // Optional explicit converter-emitted Parameter Sheet (empty in Q1)
};

struct Diagnostic {
    std::string code;
    std::string severity;   // "ERROR", "WARN", "INFO"
    std::string section;
    std::string track;
    uint32_t count = 0;
    std::string fallback;
};

struct UnsupportedFeature {
    std::string feature;
    BenchStatus status = BenchStatus::Unsupported;
    uint32_t count = 0;
    std::string reason;
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
    bool applicable = false;
    BenchStatus status = BenchStatus::NA;
    double score = 0.0;
    double clashRate = 0.0;
    double bassSlashAccuracy = 1.0;
    BenchStatus bassSlashStatus = BenchStatus::NA;
    double voiceLeadingAvgSemis = 0.0;
    BenchStatus voiceLeadingStatus = BenchStatus::NA;
    double registerViolationRate = 0.0;
    double voicingCollapseRate = 0.0;
    BenchStatus voicingCollapseStatus = BenchStatus::NA;
    // Independent UASF-layer diagnostic ONLY (never fed into Pa5X SMF playback simulation!)
    double casmPreservationRate = 0.0;
};

struct M2Scores {
    bool applicable = false;
    BenchStatus status = BenchStatus::NA;
    double score = 0.0;
    uint32_t leakedChordRange = 0;
    double leakedFxKeyRate = 0.0;
    uint32_t leakedFxNotesCount = 0;
    BenchStatus fxKeyStatus = BenchStatus::NA;
    uint32_t velocityInversions = 0;
    BenchStatus velocityZoneStatus = BenchStatus::NA;
    double guitarStrumCoherence = 1.0;
    BenchStatus guitarStrumStatus = BenchStatus::NA;
};

struct M3Scores {
    bool applicable = false;
    BenchStatus status = BenchStatus::NA;
    double score = 0.0;
    double coverageRate = 0.0;
    double meterBarAccuracy = 0.0;
    uint32_t loopBoundaryErrors = 0;
    double p99DriftMs = 0.0;
    BenchStatus timingDriftStatus = BenchStatus::NA;
};

struct M4Scores {
    bool applicable = false;
    BenchStatus status = BenchStatus::NA;
    double score = 0.0;
    double unmappedHitRate = 0.0;
    double familyPreservationRate = 1.0;
    double trackSeparationAccuracy = 1.0;
    double velCorrelation = 1.0;
};

struct M5Scores {
    bool applicable = false;
    BenchStatus status = BenchStatus::NA;
    double score = 0.0;
    double mixCcConsistency = 1.0;
    BenchStatus mixCcStatus = BenchStatus::NA;
    uint32_t unsafeCcLeaks = 0;
    BenchStatus unsafeCcStatus = BenchStatus::NA;
    double balanceHealthRate = 1.0;
    BenchStatus balanceStatus = BenchStatus::NA;
    double xgSysexCoverage = 1.0;
    BenchStatus xgSysexStatus = BenchStatus::NA;
};

struct M6Scores {
    bool applicable = false;
    BenchStatus status = BenchStatus::NA;
    double score = 0.0;
    double familyMatchRate = 0.0;
    BenchStatus familyMatchStatus = BenchStatus::NA;
    double elementVoicePreservation = 0.0;
    BenchStatus elementVoiceStatus = BenchStatus::NA;
    bool hasMidStyleVoiceChanges = false;
    bool targetKstEvaluated = false;
    bool targetKstValid = false;
    BenchStatus targetKstStatus = BenchStatus::NotTested;
};

struct ApplicabilityFlags {
    bool m1Harmonic = false;            // Chord-driven tracks present
    bool m2Articulation = false;        // MegaVoice / S.Art / Guitar NTR=2 present
    bool m3SectionStress = false;       // Break / mixed meter / tempo map / loop errors present
    bool m4DrumStress = false;          // Extended non-GM drums or swapped Rhythm1/2 present
    bool m5MixStress = false;           // XG SysEx or proprietary NRPN/CC94 present
    bool m6Timbre = false;              // Exported tracks present
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
    BenchStatus structuralStatus = BenchStatus::Fail;
    BenchStatus l3Status = BenchStatus::NotTested;
    std::vector<std::string> validationErrors;
    double applicableWeightSum = 0.0;
    double compositeScore = 0.0;
    std::string grade = kStatusFail;
    ApplicabilityFlags applicability;
    M1Scores m1;
    M2Scores m2;
    M3Scores m3;
    M4Scores m4;
    M5Scores m5;
    M6Scores m6;
    std::vector<UnsupportedFeature> unsupportedFeatures;
    std::vector<Diagnostic> diagnostics;
    MatrixSummary matrixSummary;
    std::vector<MatrixCell> cells;
};

struct CorpusSummary {
    uint32_t fixtureCount = 0;
    uint32_t passCount = 0;
    uint32_t warnCount = 0;
    uint32_t failCount = 0;
    bool allStructuralValid = false;
    BenchStatus l3Status = BenchStatus::NotTested;
    uint32_t targetKstPassCount = 0;
    uint32_t targetKstFailCount = 0;
    uint32_t targetKstNotTestedCount = 0;
    double meanComposite = 0.0;
    double minComposite = 0.0;
    std::array<double, 6> meanDimensions{};
    std::array<double, 6> minDimensionScores{};
    std::array<double, 6> activeDimensionMeans{};
    std::array<uint32_t, 6> evaluatedDimensionCounts{};
    std::array<uint32_t, 6> activeDimensionCounts{};
    std::array<uint32_t, 6> naExcludedCounts{};
    std::array<uint32_t, 6> dimPassCounts{};
    std::array<uint32_t, 6> dimFailCounts{};
    double activeComposite = 0.0;
    uint32_t hardInvariantViolations = 0;
    uint32_t unsupportedTotalOccurrences = 0;
    uint32_t fixturesWithUnsupported = 0;
    std::map<std::string, uint32_t> unsupportedByFeature;
    std::vector<std::string> notTestedItems;
    std::string corpusGrade = kStatusFail;
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

// Target Pa5X standard GM2 Drum Family (populated keys 35..81; 0 = unmapped).
inline char targetPa5xDrumFamilyOf(uint8_t note) noexcept {
    if (note == 35 || note == 36) return 'B';                                 // Bass drum
    if (note == 37 || note == 38 || note == 39 || note == 40) return 'S';     // Snare / Rim / Clap
    if (note == 42 || note == 44 || note == 46) return 'H';                   // Hi-Hat
    if (note == 41 || note == 43 || note == 45 || note == 47 || note == 48 || note == 50) return 'T'; // Toms
    if (note == 49 || note == 51 || note == 52 || note == 53 || note == 55 || note == 57 || note == 59) return 'C'; // Cymbals
    if (note >= 54 && note <= 81) return 'L';                                 // Latin / Perc
    return 0;                                                                 // Outside GM 35..81
}

// Source Yamaha XG / Revo! Drum Family (GM 35..81 plus documented XG extended keys 13..34, 82..105).
inline char sourceDrumFamilyOf(uint8_t note) noexcept {
    if (const char gm = targetPa5xDrumFamilyOf(note); gm != 0) return gm;
    if (note >= 13 && note <= 16) return 'S';  // XG Surdo/Rim/Snare roll / Brush tap
    if (note >= 17 && note <= 24) return 'B';  // XG Kick / Low drum variants
    if (note >= 25 && note <= 28) return 'S';  // XG Snare Roll / Finger Snap / Brush Swirl-Slap
    if (note >= 29 && note <= 32) return 'L';  // XG Scratch / Perc / Metronome
    if (note == 33 || note == 34) return 'B';  // XG Kick / Rim-Kick low variants
    if (note >= 82 && note <= 105) return 'L'; // XG Shaker / Jingle / Bell Tree / Castanets / Surdo / Ethnic
    return 'X';                                // Other extended SFX
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
struct RefCasmZone {
    uint8_t ntr = 0;
    uint8_t ntt = 0;
    bool nttBass = false;
    uint8_t highKey = 11;
    uint8_t noteLow = 0;
    uint8_t noteHigh = 127;
    uint8_t rtr = 0;
};

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
    RefCasmZone lowZone{};
    RefCasmZone highZone{};
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
    std::set<uint8_t> activeChannels; // channels with any channel event in section
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
    uint32_t srcUnsafeCcCount = 0;
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
    bool hasExtendedDrumNotes = false;
    uint32_t midStyleVoiceChanges = 0;
};

inline uint32_t readBE32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline RefCasmZone readRefZone(const uint8_t* z) noexcept {
    RefCasmZone out{};
    out.ntr = z[0];
    out.nttBass = (z[1] & 0x80) != 0;
    out.ntt = z[1] & 0x7F;
    out.highKey = z[2];
    out.noteLow = z[3];
    out.noteHigh = z[4];
    out.rtr = z[5];
    return out;
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
            e.lowZone = readRefZone(d + 22);
            const auto mid = readRefZone(d + 28);
            e.ntr = mid.ntr;
            e.nttBass = mid.nttBass;
            e.ntt = mid.ntt;
            e.highKey = mid.highKey;
            e.noteLow = mid.noteLow;
            e.noteHigh = mid.noteHigh;
            e.rtr = mid.rtr;
            if (sz >= 40) e.highZone = readRefZone(d + 34);
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
                    uint8_t d1 = d[p];
                    uint8_t d2 = need == 2 ? d[p + 1] : uint8_t{0};
                    if (hi == 0xB0 && (d1 == 98 || d1 == 99 || d1 == 94)) ++out.srcUnsafeCcCount;
                    events.push_back({static_cast<uint32_t>(abs), st, d1, d2});
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
            sec.activeChannels.insert(ch);
            uint32_t rel = e.tick - sec.startTick;
            if (hi == 0x90 && e.d2 > 0) {
                uint16_t k = static_cast<uint16_t>((ch << 8) | e.d1);
                open[k].push_back(sec.notes.size());
                sec.notes.push_back({rel, sec.musicalEndTick - sec.startTick, ch, e.d1, e.d2});
                if (ch == 8 || ch == 9) {
                    if (e.d1 < 35 || e.d1 > 81) out.hasExtendedDrumNotes = true;
                } else {
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

struct RoundTrippedUasfCasmCheck {
    bool hasCasm = false;
    uint8_t ntr = 0;
    uint8_t ntt = 0;
    bool nttBass = false;
    uint8_t rtr = 0;
    uint8_t srcRoot = 0;
    uint8_t srcType = 0;
    uint16_t noteMute = 0x0FFF;
    uint64_t chordMute = 0x3FFFFFFFFull;
    uint8_t destCh = 0xFF;
    bool hasZonesAndEditable = false;
    uint8_t editable = 0;
    RefCasmZone lowZone{};
    RefCasmZone highZone{};
};

template <typename ArtT>
inline RoundTrippedUasfCasmCheck extractUasfCasmDiag(const ArtT& a) noexcept {
    RoundTrippedUasfCasmCheck m;
    m.ntr = a.ntr;
    m.ntt = a.ntt;
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
    if constexpr (requires { a.casm_editable; a.low_zone; a.high_zone; }) {
        m.hasZonesAndEditable = true;
        m.editable = a.casm_editable;
        m.lowZone = {a.low_zone.ntr, a.low_zone.ntt, a.low_zone.ntt_bass,
                     a.low_zone.high_key, a.low_zone.note_low, a.low_zone.note_high, a.low_zone.rtr};
        m.highZone = {a.high_zone.ntr, a.high_zone.ntt, a.high_zone.ntt_bass,
                      a.high_zone.high_key, a.high_zone.note_low, a.high_zone.note_high, a.high_zone.rtr};
    }
    return m;
}

// ── Offline Pa5X Playback Simulator (MAN5 documented semantics) ──────
// Strictly evaluates the exported Pa5X Marker SMF + optional explicit
// Pa5xParameterSheet (NEVER reads UASF CASM side-channel!).
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
                                const TestChord& chord, const Pa5xTrackParams& params) noexcept {
    const int srcPc = srcPitch % 12;
    int deltaSemi = 0;

    if (kch == kBassCh) {
        // Pa5X Bass track (Parallel / Root).
        // When Bass Inversion is explicitly enabled via Parameter Sheet (Q3+):
        //   - Root triggers (srcPc == 0) move to chord.bassPc
        //   - Non-root bass notes (e.g. 5th G, 3rd E) shift by chord.rootPc (NEVER by bassPc!)
        const uint8_t wrapKey = (params.hasOverride && params.wrapKey <= 11) ? params.wrapKey : uint8_t{11};
        const bool useBassInv = params.hasOverride && params.bassInversion && (srcPc == 0);
        int rootShift = static_cast<int>(useBassInv ? chord.bassPc : chord.rootPc);
        if (rootShift > static_cast<int>(wrapKey)) rootShift -= 12;
        deltaSemi = rootShift;
    } else {
        // Acc1..5: Pa5X Parallel / Chord NTT relative to CV1 (C Maj) or CV2 (C Min).
        int qualityOffset = 0;
        if (!isCv2Minor) {
            if (srcPc == 4) {
                switch (chord.kind) {
                    case TestChordKind::Minor:
                    case TestChordKind::Min7b5:
                    case TestChordKind::Dim7:  qualityOffset = -1; break;
                    case TestChordKind::Sus4:  qualityOffset = +1; break;
                    default: break;
                }
            } else if (srcPc == 7) {
                if (chord.kind == TestChordKind::Min7b5 || chord.kind == TestChordKind::Dim7)
                    qualityOffset = -1;
            }
        } else {
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
        uint8_t wrapKey = 11;
        if (params.hasOverride) {
            if (params.fixedNtr) wrapKey = 5;
            else if (params.wrapKey <= 11) wrapKey = params.wrapKey;
        }
        if (rootShift > static_cast<int>(wrapKey)) rootShift -= 12;
        deltaSemi = rootShift + qualityOffset;
    }

    int p = static_cast<int>(srcPitch) + deltaSemi;
    // Apply explicit Parameter Sheet LimitKeyRange ONLY when emitted by converter (never side-channel!).
    if (params.hasOverride && params.limitHigh <= 127 && params.limitLow <= params.limitHigh &&
        (params.limitHigh - params.limitLow) >= 11) {
        while (p < static_cast<int>(params.limitLow) && p + 12 <= 127) p += 12;
        while (p > static_cast<int>(params.limitHigh) && p - 12 >= 0) p -= 12;
    }
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

// Stage 3 core evaluator: evaluates an exported & re-parsed Pa5X Marker SMF against
// the independent Reference Analyzer output (also callable directly by unit tests for
// verifying M2.3 Q2 velocity-zone sanitization and M3.4 timing-drift sensitivity!).
inline BenchResult evaluateReparsedSmf(const RefAnalysis& ref,
                                       const Style& reparsedStyle,
                                       const Report& mapReport,
                                       const uasf::StyleDefinition* roundTrippedUasf,
                                       const std::string& fixtureName,
                                       const BenchOptions& opt) {
    BenchResult res;
    res.fixtureName = fixtureName;
    res.format = ref.format;
    res.sourcePpqn = ref.ppqn;
    res.sectionsSource = static_cast<uint32_t>(ref.sections.size());
    res.validationErrors = validate(reparsedStyle);
    res.structuralValid = res.validationErrors.empty();
    res.structuralStatus = res.structuralValid ? BenchStatus::Pass : BenchStatus::Fail;
    res.l3Status = BenchStatus::NotTested;
    res.targetPpqn = reparsedStyle.ppqn;
    res.sectionsExported = static_cast<uint32_t>(reparsedStyle.elements.size());

    // ── Independent UASF-layer CASM round-trip diagnostic (P1-1 / P2-2) ──
    // Strictly filtered to CASM entries whose (section, srcCh) has active MIDI events.
    uint32_t casmFieldsChecked = 0, casmFieldsPreserved = 0;
    if (roundTrippedUasf != nullptr) {
        std::map<std::string, std::set<uint8_t>> activeBySec;
        for (const auto& rs : ref.sections) activeBySec[rs.name] = rs.activeChannels;
        for (const auto& rc : ref.casmEntries) {
            uasf::SectionType st;
            if (!markerToSectionType(rc.sectionName, st)) continue;
            auto ait = activeBySec.find(rc.sectionName);
            if (ait == activeBySec.end() || !ait->second.count(rc.srcCh)) continue; // P2-2: skip silent channels
            ++casmFieldsChecked;
            bool matched = false;
            for (const auto& sec : roundTrippedUasf->sections) {
                if (sec.type != st) continue;
                for (const auto& tr : sec.tracks) {
                    if (tr.midi_channel != rc.srcCh) continue;
                    const auto m = extractUasfCasmDiag(tr.articulation);
                    bool zonesOk = true;
                    if (m.hasZonesAndEditable && !rc.fromSff1Ctab) {
                        zonesOk = (m.editable == rc.editable) &&
                                  (m.lowZone.ntr == rc.lowZone.ntr && m.lowZone.ntt == rc.lowZone.ntt &&
                                   m.lowZone.nttBass == rc.lowZone.nttBass && m.lowZone.rtr == rc.lowZone.rtr) &&
                                  (m.highZone.ntr == rc.highZone.ntr && m.highZone.ntt == rc.highZone.ntt &&
                                   m.highZone.nttBass == rc.highZone.nttBass && m.highZone.rtr == rc.highZone.rtr);
                    }
                    if (m.hasCasm && m.ntr == rc.ntr && m.ntt == rc.ntt && m.nttBass == rc.nttBass &&
                        m.srcRoot == rc.srcRoot && m.srcType == rc.srcType &&
                        m.chordMute == rc.chordMute && m.destCh == rc.destCh && zonesOk) {
                        matched = true;
                    }
                }
            }
            if (matched) ++casmFieldsPreserved;
        }
    }
    res.m1.casmPreservationRate = casmFieldsChecked ? double(casmFieldsPreserved) / double(casmFieldsChecked) : 1.0;

    // ── Group exported elements by base element name & CV index ──────
    std::map<std::string, std::map<int, const Element*>> byBaseEl;
    for (const auto& el : reparsedStyle.elements) {
        byBaseEl[baseElementOfMarker(el.marker)][cvIndexOfMarker(el.marker)] = &el;
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

    // ── Metric Accumulators ──────────────────────────────────────────
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
    // P1-3: bucket exported velocities by SOURCE note velocity (1..64 vs 91..127)
    std::map<uint8_t, std::pair<double, uint32_t>> megaSrcLowExpVel;
    std::map<uint8_t, std::pair<double, uint32_t>> megaSrcHighExpVel;

    uint32_t m3LoopErrors = 0;
    std::vector<double> m3OnsetDriftsMs;

    uint64_t m4DrumHits = 0, m4UnmappedHits = 0;
    uint64_t m4FamilyChecked = 0, m4FamilyPreserved = 0;
    std::vector<double> m4SrcVels, m4ExpVels;
    uint64_t expDrumPrimary = 0, expPercPrimary = 0;

    uint64_t m5MixExpected = 0, m5MixPresent = 0;
    uint32_t m5UnsafeCc = 0;
    uint64_t m5BalanceTracks = 0, m5BalanceHealthy = 0;

    // 1. Evaluate every exported element × track × 13 chords
    for (const auto& [baseName, cvMap] : byBaseEl) {
        const Element* cv1 = cvMap.count(1) ? cvMap.at(1) : cvMap.begin()->second;

        // Unsafe CC scan across ALL CVs of this element
        for (const auto& [cvNum, elPtr] : cvMap) {
            (void)cvNum;
            for (const auto& ev : elPtr->events) {
                uint8_t hi = ev.status & 0xF0;
                if (hi == 0xB0 && (ev.d1 == 98 || ev.d1 == 99 || ev.d1 == 94 || ev.d2 >= 128)) {
                    ++m5UnsafeCc;
                }
            }
        }

        // Mix CC consistency check on cv1
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

        // Pre-extract notes & loop-boundary errors for EVERY CV (P3-3: no discarded dummyLoop!)
        std::map<int, std::map<uint8_t, std::pair<std::vector<SimNote>, uint32_t>>> cvTrackNotes;
        for (const auto& [cvNum, elPtr] : cvMap) {
            for (uint8_t kch = kBassCh; kch <= 15; ++kch) {
                uint32_t errs = 0;
                auto notes = extractElementTrackNotes(*elPtr, kch, errs);
                m3LoopErrors += errs;
                cvTrackNotes[cvNum][kch] = {std::move(notes), errs};
            }
        }

        // Per-track evaluation across the 8 Korg channels (8..15)
        for (uint8_t kch = kBassCh; kch <= 15; ++kch) {
            const bool isDrumTrack = (kch == kDrumCh || kch == kPercCh);
            const auto& [cv1Notes, trackLoopErrs] = cvTrackNotes[cvIndexOfMarker(cv1->marker)][kch];
            if (cv1Notes.empty()) continue;

            if (isDrumTrack) {
                uint32_t unmapped = 0;
                for (const auto& n : cv1Notes) {
                    ++m4DrumHits;
                    if (targetPa5xDrumFamilyOf(n.pitch) == 0) {
                        ++m4UnmappedHits;
                        ++unmapped;
                    }
                    if (isPrimaryKitHit(n.pitch)) {
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
                cell.cellScore = clamp100(100.0 - unmapRate * 200.0 - trackLoopErrs * 25.0);
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

            // M2 static articulation checks on exported cv1 notes
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
            }

            // M2.4 Guitar strum coherence — evaluated strictly on exported SMF notes (P1-1: no UASF side-channel!)
            if (isGuitar) {
                const double ticksPerMs = (double(reparsedStyle.ppqn) * 1000.0) /
                                          double(reparsedStyle.mpqn ? reparsedStyle.mpqn : 500000u);
                const uint32_t windowTicks = static_cast<uint32_t>(std::max(1.0, 80.0 * ticksPerMs));
                size_t i = 0;
                while (i < cv1Notes.size()) {
                    size_t j = i + 1;
                    while (j < cv1Notes.size() && cv1Notes[j].on - cv1Notes[i].on <= windowTicks) ++j;
                    if (j - i >= 3) {
                        ++m2StrumTotal;
                        uint8_t minP = 127, maxP = 0;
                        bool hasFx = false;
                        bool monotonic = true;
                        for (size_t k = i; k < j; ++k) {
                            if (cv1Notes[k].pitch >= 96) hasFx = true;
                            if (k > i && cv1Notes[k].on < cv1Notes[k - 1].on) monotonic = false;
                            minP = std::min(minP, cv1Notes[k].pitch);
                            maxP = std::max(maxP, cv1Notes[k].pitch);
                        }
                        double spreadMs = double(cv1Notes[j - 1].on - cv1Notes[i].on) / std::max(1e-6, ticksPerMs);
                        bool okStrum = monotonic && !hasFx && (j - i) <= 6 && (maxP - minP) <= 24 &&
                                       spreadMs >= 5.0 && spreadMs <= 60.0;
                        if (okStrum) ++m2StrumOk;
                    }
                    i = j;
                }
            }

            // Strictly use explicit converter-emitted Parameter Sheet (empty/default in Q1!)
            Pa5xTrackParams params{};
            if (auto pit = opt.parameterSheet.tracks.find({baseName, kch}); pit != opt.parameterSheet.tracks.end()) {
                params = pit->second;
            }

            // Simulate all 13 Standardized Test Chords
            std::array<double, 13> chordCentroids{};
            std::array<bool, 13> chordHasCentroid{};
            for (size_t ci = 0; ci < kTestChords.size(); ++ci) {
                const auto& chd = kTestChords[ci];
                int activeCv = cvIndexOfMarker(cv1->marker);
                bool isCv2Minor = false;
                if ((chd.kind == TestChordKind::Minor || chd.kind == TestChordKind::Min7b5) && cvMap.count(2)) {
                    activeCv = 2;
                    isCv2Minor = true;
                } else if (chd.kind == TestChordKind::Dom7 && cvMap.count(3)) {
                    activeCv = 3;
                } else if (chd.kind == TestChordKind::Dim7 && cvMap.count(4)) {
                    activeCv = 4;
                }

                const Element* activeEl = cvMap.at(activeCv);
                const auto& [trackNotes, activeLoopErrs] = cvTrackNotes[activeCv][kch];
                if (trackNotes.empty()) continue;

                uint32_t cellClashes = 0;
                uint32_t cellRegViol = 0;
                uint32_t cellBassTrig = 0, cellBassHit = 0;
                double pitchSum = 0.0;

                std::map<uint32_t, std::vector<std::pair<uint8_t, uint8_t>>> atTick;
                const uint32_t barTicks = std::max<uint32_t>(
                    1, static_cast<uint32_t>(uint64_t{reparsedStyle.ppqn} * 4 * activeEl->num / activeEl->den));

                for (const auto& n : trackNotes) {
                    const uint8_t simP = simulatePa5xNote(n.pitch, kch, isCv2Minor, chd, params);
                    const uint8_t simPc = static_cast<uint8_t>(simP % 12);
                    ++m1TotalNotes;
                    pitchSum += simP;
                    atTick[n.on].emplace_back(n.pitch, simP);

                    if ((chd.allowedMask & (1u << simPc)) == 0) {
                        ++m1ClashNotes;
                        ++cellClashes;
                    }
                    bool regBad = false;
                    if (kch == kBassCh) {
                        if (simP < 24 || simP > 55) regBad = true;
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
                cell.loopErrors = activeLoopErrs;
                double cClash = double(cellClashes) / double(trackNotes.size());
                double cReg = double(cellRegViol) / double(trackNotes.size());
                cell.cellScore = clamp100(100.0 - cClash * 300.0 - cReg * 200.0 -
                                          (1.0 - cell.bassRootHitRate) * 40.0 -
                                          activeLoopErrs * 20.0 - (trackLeakedFx > 0 ? 25.0 : 0.0));
                res.cells.push_back(cell);
            }

            // M1.3 Voice-leading smoothness across Cmaj -> F#maj -> Bbmin -> G7/B -> Cmaj
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

    // 2. Cross-reference source sections vs ACTUAL exported SMF events:
    //    - M2.3 MegaVoice velocity-zone inversion (bucketed by SOURCE velocity! — P1-3)
    //    - M3.1 Section coverage, M3.2 Meter & bar accuracy
    //    - M3.4 Micro-groove timing drift (measured from ACTUAL exported NoteOn ticks! — P1-5)
    //    - M4.2 Drum family preservation & M4.4 Drum dynamic contour correlation (P2-1)
    uint32_t matchedSections = 0;
    uint32_t exactMeterBarSections = 0;
    const double usPerTickSrc = double(ref.initialMpqn) / double(ref.ppqn);
    const double usPerTickDst = double(reparsedStyle.mpqn ? reparsedStyle.mpqn : ref.initialMpqn) / double(reparsedStyle.ppqn);

    uint64_t srcR1Primary = 0, srcR2Primary = 0;
    bool hasBreakSection = false;
    for (const auto& rs : ref.sections) {
        if (rs.type == uasf::SectionType::Break) hasBreakSection = true;
        for (const auto& rn : rs.notes) {
            if (rn.ch == 8 || rn.ch == 9) {
                if (isPrimaryKitHit(rn.pitch)) {
                    if (rn.ch == 8) ++srcR1Primary;
                    else ++srcR2Primary;
                }
            }
        }
        const std::string base = baseMarkerForType(rs.type);
        auto it = byBaseEl.find(base);
        if (it == byBaseEl.end()) continue;
        ++matchedSections;
        const Element* el = it->second.count(1) ? it->second.at(1) : it->second.begin()->second;
        uint64_t barTicksDst = std::max<uint64_t>(1, uint64_t{reparsedStyle.ppqn} * 4 * el->num / el->den);
        uint32_t expBars = static_cast<uint32_t>(el->length / barTicksDst);
        if (el->num == rs.num && el->den == rs.den && expBars == rs.expectedBars) {
            ++exactMeterBarSections;
        }

        // Collect actual exported Note-On events in this element
        struct ExpNoteOn { uint32_t tick; uint8_t kch; uint8_t pitch; uint8_t vel; bool usedDrift; bool usedMega; bool usedDrum; };
        std::vector<ExpNoteOn> expOns;
        for (const auto& ev : el->events) {
            if ((ev.status & 0xF0) == 0x90 && ev.d2 > 0) {
                expOns.push_back({ev.tick, static_cast<uint8_t>(ev.status & 0x0F), ev.d1, ev.d2, false, false, false});
            }
        }

        // P1-5: Measure M3.4 onset timing drift against ACTUAL exported Note-On ticks!
        for (const auto& rn : rs.notes) {
            uint8_t rch = ref.casmRoute.count(rn.ch) ? ref.casmRoute.at(rn.ch) : rn.ch;
            uint8_t kch = korgChannelFor(rch);
            if (kch == 0xFF) continue;
            if (isChordDriven(kch) && rn.pitch <= kChordEventNoteMax) continue;
            uint64_t expectedTick = (uint64_t{rn.on} * reparsedStyle.ppqn + ref.ppqn / 2) / ref.ppqn;
            if (el->length < 2 || expectedTick >= el->length - 1) continue;

            // Find nearest unmatched actual exported note on same (kch, pitch)
            size_t bestIdx = expOns.size();
            uint64_t bestDist = UINT64_MAX;
            for (size_t idx = 0; idx < expOns.size(); ++idx) {
                if (expOns[idx].usedDrift || expOns[idx].kch != kch || expOns[idx].pitch != rn.pitch) continue;
                uint64_t d = expOns[idx].tick > expectedTick ? (expOns[idx].tick - expectedTick)
                                                             : (expectedTick - expOns[idx].tick);
                if (d < bestDist) { bestDist = d; bestIdx = idx; }
            }
            if (bestIdx < expOns.size()) {
                expOns[bestIdx].usedDrift = true;
                double srcMs = (double(rn.on) * usPerTickSrc) / 1000.0;
                double dstMs = (double(expOns[bestIdx].tick) * usPerTickDst) / 1000.0;
                m3OnsetDriftsMs.push_back(std::abs(dstMs - srcMs));
            }
        }

        // P1-3: Match MegaVoice source notes to actual exported notes, bucketing by SOURCE velocity!
        for (const auto& rn : rs.notes) {
            uint8_t rch = ref.casmRoute.count(rn.ch) ? ref.casmRoute.at(rn.ch) : rn.ch;
            uint8_t kch = korgChannelFor(rch);
            if (!megaKorgChs.count(kch)) continue;
            if (rn.pitch <= kChordEventNoteMax) continue;
            if (rn.vel > 64 && rn.vel < 91) continue; // Only compare source normal (1..64) vs high/FX (91..127)
            uint64_t expectedTick = (uint64_t{rn.on} * reparsedStyle.ppqn + ref.ppqn / 2) / ref.ppqn;
            if (el->length < 2 || expectedTick >= el->length - 1) continue;

            // Match nearest unused exported note on same kch within <= 2 ticks (prefer exact pitch, else pitch-class)
            size_t bestExact = expOns.size(), bestPc = expOns.size();
            uint64_t distExact = 3, distPc = 3;
            for (size_t idx = 0; idx < expOns.size(); ++idx) {
                if (expOns[idx].usedMega || expOns[idx].kch != kch) continue;
                uint64_t d = expOns[idx].tick > expectedTick ? (expOns[idx].tick - expectedTick)
                                                             : (expectedTick - expOns[idx].tick);
                if (d > 2) continue;
                if (expOns[idx].pitch == rn.pitch && d < distExact) { distExact = d; bestExact = idx; }
                else if ((expOns[idx].pitch % 12) == (rn.pitch % 12) && d < distPc) { distPc = d; bestPc = idx; }
            }
            const size_t pick = (bestExact < expOns.size()) ? bestExact : bestPc;
            if (pick < expOns.size()) {
                expOns[pick].usedMega = true;
                if (rn.vel <= 64) {
                    megaSrcLowExpVel[kch].first += expOns[pick].vel;
                    ++megaSrcLowExpVel[kch].second;
                } else {
                    megaSrcHighExpVel[kch].first += expOns[pick].vel;
                    ++megaSrcHighExpVel[kch].second;
                }
            }
        }

        // P2-1: M4.2 Drum family preservation + M4.4 family-paired velocity correlation
        {
            std::vector<std::tuple<uint32_t, char, uint8_t>> sDrumHits;
            for (const auto& rn : rs.notes) {
                uint8_t rch = ref.casmRoute.count(rn.ch) ? ref.casmRoute.at(rn.ch) : rn.ch;
                uint8_t kch = korgChannelFor(rch);
                if (kch != kDrumCh && kch != kPercCh) continue;
                uint32_t t = static_cast<uint32_t>((uint64_t{rn.on} * reparsedStyle.ppqn + ref.ppqn / 2) / ref.ppqn);
                if (el->length < 2 || t >= el->length - 1) continue;
                sDrumHits.emplace_back(t, sourceDrumFamilyOf(rn.pitch), rn.vel);
            }
            std::sort(sDrumHits.begin(), sDrumHits.end());

            for (const auto& [st, sFam, sVel] : sDrumHits) {
                ++m4FamilyChecked;
                // Prefer nearest unmatched exported drum/perc hit within <= 2 ticks with matching target family;
                // else nearest unmatched drum/perc hit within <= 2 ticks (so unmapped raw hits still pair for M4.4!).
                size_t bestSameFam = expOns.size(), bestAny = expOns.size();
                uint32_t dSame = 3, dAny = 3;
                for (size_t idx = 0; idx < expOns.size(); ++idx) {
                    if (expOns[idx].usedDrum) continue;
                    if (expOns[idx].kch != kDrumCh && expOns[idx].kch != kPercCh) continue;
                    uint32_t d = expOns[idx].tick > st ? (expOns[idx].tick - st) : (st - expOns[idx].tick);
                    if (d > 2) continue;
                    char eFam = targetPa5xDrumFamilyOf(expOns[idx].pitch);
                    if (eFam != 0 && eFam == sFam && d < dSame) { dSame = d; bestSameFam = idx; }
                    if (d < dAny) { dAny = d; bestAny = idx; }
                }
                const size_t pick = (bestSameFam < expOns.size()) ? bestSameFam : bestAny;
                if (pick < expOns.size()) {
                    expOns[pick].usedDrum = true;
                    char eFam = targetPa5xDrumFamilyOf(expOns[pick].pitch);
                    if (eFam != 0 && eFam == sFam) ++m4FamilyPreserved;
                    m4SrcVels.push_back(static_cast<double>(sVel));
                    m4ExpVels.push_back(static_cast<double>(expOns[pick].vel));
                }
            }
        }
    }

    // M2.3 Velocity-zone inversion count (from SOURCE-velocity buckets!)
    uint32_t m2VelCheckedChannels = 0;
    for (uint8_t kch : megaKorgChs) {
        auto lo = megaSrcLowExpVel[kch], hi = megaSrcHighExpVel[kch];
        if (lo.second > 0 && hi.second > 0) {
            ++m2VelCheckedChannels;
            double meanLo = lo.first / double(lo.second);
            double meanHi = hi.first / double(hi.second);
            if (meanHi > meanLo) ++m2VelInversions;
        }
    }

    // Independent expected active Korg channels from raw source .sty (never supplied by converter!)
    std::set<uint8_t> expectedKorgChsFromRef;
    for (const auto& rs : ref.sections) {
        for (const auto& rn : rs.notes) {
            uint8_t rch = ref.casmRoute.count(rn.ch) ? ref.casmRoute.at(rn.ch) : rn.ch;
            uint8_t kch = korgChannelFor(rch);
            if (kch != 0xFF) expectedKorgChsFromRef.insert(kch);
        }
    }

    // ── Applicability Flags (P1-4: Stress/Torture Corpus Tracking) ───
    res.applicability.m1Harmonic = (m1TotalNotes > 0);
    res.applicability.m2Articulation = !ref.megaVoiceSrcChannels.empty() || !ref.sartSrcChannels.empty() || !ref.guitarNtrSrcChannels.empty();
    res.applicability.m3SectionStress = hasBreakSection || ref.timeSigs.size() > 1 || ref.tempos.size() > 1 || m3LoopErrors > 0;
    res.applicability.m4DrumStress = ref.hasExtendedDrumNotes || (srcR1Primary > srcR2Primary);
    res.applicability.m5MixStress = (ref.xgSysexCount > 0) || (ref.srcUnsafeCcCount > 0) || (m5UnsafeCc > 0);
    res.applicability.m6Timbre = !expectedKorgChsFromRef.empty() || !mapReport.tracks.empty();

    // ── Compute Final M1..M6 Scores (EXACT Spec §3 Formulas; N/A Excluded — Never Fake 100.0!) ──
    // M1 Harmonic & Voice-Leading (Weight 25%)
    res.m1.applicable = (m1TotalNotes > 0);
    res.m1.clashRate = m1TotalNotes ? double(m1ClashNotes) / double(m1TotalNotes) : 0.0;
    res.m1.bassSlashAccuracy = m1BassTriggers ? double(m1BassHits) / double(m1BassTriggers) : 0.0;
    res.m1.bassSlashStatus = m1BassTriggers == 0 ? BenchStatus::NA : (res.m1.bassSlashAccuracy >= 0.80 ? BenchStatus::Pass : BenchStatus::Fail);
    res.m1.voiceLeadingAvgSemis = m1VlCount ? (m1VlSum / double(m1VlCount)) : 0.0;
    res.m1.voiceLeadingStatus = m1VlCount == 0 ? BenchStatus::NA : (res.m1.voiceLeadingAvgSemis <= 3.5 ? BenchStatus::Pass : BenchStatus::Fail);
    res.m1.registerViolationRate = m1TotalNotes ? double(m1RegViol) / double(m1TotalNotes) : 0.0;
    res.m1.voicingCollapseRate = m1PolyAttacks ? double(m1CollapseAttacks) / double(m1PolyAttacks) : 0.0;
    res.m1.voicingCollapseStatus = m1PolyAttacks == 0 ? BenchStatus::NA : (res.m1.voicingCollapseRate <= 0.02 ? BenchStatus::Pass : BenchStatus::Fail);
    if (!res.m1.applicable) {
        res.m1.score = 0.0;
        res.m1.status = BenchStatus::NA;
    } else {
        const double s11 = res.m1.clashRate <= 0.01 ? 100.0 : clamp100(100.0 - (res.m1.clashRate - 0.01) * 800.0);
        const double s12 = clamp100(res.m1.bassSlashAccuracy * 100.0);
        const double s13 = res.m1.voiceLeadingAvgSemis <= 3.5 ? 100.0 : clamp100(100.0 - (res.m1.voiceLeadingAvgSemis - 3.5) * 16.6667);
        const double s14 = res.m1.registerViolationRate <= 0.005 ? 100.0 : clamp100(100.0 - (res.m1.registerViolationRate - 0.005) * 1500.0);
        const double s15 = res.m1.voicingCollapseRate <= 0.02 ? 100.0 : clamp100(100.0 - (res.m1.voicingCollapseRate - 0.02) * 500.0);
        const double w11 = 0.30;
        const double w12 = m1BassTriggers > 0 ? 0.25 : 0.0;
        const double w13 = m1VlCount > 0 ? 0.15 : 0.0;
        const double w14 = 0.15;
        const double w15 = m1PolyAttacks > 0 ? 0.15 : 0.0;
        const double wSum = w11 + w12 + w13 + w14 + w15;
        res.m1.score = (w11 * s11 + w12 * s12 + w13 * s13 + w14 * s14 + w15 * s15) / wSum;
        res.m1.status = res.m1.score >= 80.0 ? BenchStatus::Pass : BenchStatus::Fail;
    }

    // M2 Articulation & MegaVoice Hygiene (Weight 20%)
    // N/A when fixture has zero MegaVoice/S.Art/Guitar sources AND zero chord-range leaks (never fake 100.0!).
    res.m2.leakedChordRange = m2LeakedChordRange;
    res.m2.leakedFxKeyRate = m2FlaggedNotes ? double(m2LeakedFxNotes) / double(m2FlaggedNotes) : 0.0;
    res.m2.leakedFxNotesCount = static_cast<uint32_t>(m2LeakedFxNotes);
    res.m2.fxKeyStatus = m2FlaggedNotes == 0 ? BenchStatus::NA : (m2LeakedFxNotes == 0 ? BenchStatus::Pass : BenchStatus::Fail);
    res.m2.velocityInversions = m2VelInversions;
    res.m2.velocityZoneStatus = m2VelCheckedChannels == 0 ? BenchStatus::NA : (m2VelInversions == 0 ? BenchStatus::Pass : BenchStatus::Fail);
    res.m2.guitarStrumCoherence = m2StrumTotal ? double(m2StrumOk) / double(m2StrumTotal) : 0.0;
    res.m2.guitarStrumStatus = m2StrumTotal == 0 ? BenchStatus::NA : (res.m2.guitarStrumCoherence >= 0.80 ? BenchStatus::Pass : BenchStatus::Fail);
    res.m2.applicable = res.applicability.m2Articulation || (m2LeakedChordRange > 0);
    if (!res.m2.applicable) {
        res.m2.score = 0.0;
        res.m2.status = BenchStatus::NA;
    } else if (!res.applicability.m2Articulation && m2LeakedChordRange > 0) {
        // Hard-fail chord-range leak even on a non-MegaVoice style!
        res.m2.score = 0.0;
        res.m2.status = BenchStatus::Fail;
    } else {
        const double s21 = m2LeakedChordRange == 0 ? 100.0 : 0.0;
        const double s22 = res.m2.leakedFxKeyRate == 0.0 ? 100.0 : clamp100(100.0 - res.m2.leakedFxKeyRate * 400.0);
        const double s23 = m2VelInversions == 0 ? 100.0 : clamp100(100.0 - double(m2VelInversions) * 50.0);
        const double s24 = clamp100(res.m2.guitarStrumCoherence * 100.0);
        const double w21 = (m1TotalNotes > 0 || m2LeakedChordRange > 0) ? 0.25 : 0.0;
        const double w22 = m2FlaggedNotes > 0 ? 0.30 : 0.0;
        const double w23 = m2VelCheckedChannels > 0 ? 0.25 : 0.0;
        const double w24 = m2StrumTotal > 0 ? 0.20 : 0.0;
        const double wSum = w21 + w22 + w23 + w24;
        res.m2.score = wSum > 0.0 ? (w21 * s21 + w22 * s22 + w23 * s23 + w24 * s24) / wSum : 0.0;
        res.m2.status = (res.m2.score >= 80.0 && m2LeakedChordRange == 0 && m2LeakedFxNotes == 0 && m2VelInversions == 0)
                            ? BenchStatus::Pass
                            : BenchStatus::Fail;
    }

    // M3 Section & Structural Completeness (Weight 15%)
    res.m3.applicable = !ref.sections.empty();
    res.m3.coverageRate = ref.sections.empty() ? 0.0 : double(matchedSections) / double(ref.sections.size());
    res.m3.meterBarAccuracy = ref.sections.empty() ? 0.0 : double(exactMeterBarSections) / double(ref.sections.size());
    res.m3.loopBoundaryErrors = m3LoopErrors;
    if (!m3OnsetDriftsMs.empty()) {
        std::sort(m3OnsetDriftsMs.begin(), m3OnsetDriftsMs.end());
        size_t idx = static_cast<size_t>(std::floor(0.99 * double(m3OnsetDriftsMs.size() - 1)));
        res.m3.p99DriftMs = m3OnsetDriftsMs[idx];
    }
    res.m3.timingDriftStatus = m3OnsetDriftsMs.empty() ? BenchStatus::NA : (res.m3.p99DriftMs <= 1.5 ? BenchStatus::Pass : BenchStatus::Fail);
    if (!res.m3.applicable) {
        res.m3.score = 0.0;
        res.m3.status = BenchStatus::NA;
    } else {
        const double s31 = clamp100(res.m3.coverageRate * 100.0);
        const double s32 = clamp100(res.m3.meterBarAccuracy * 100.0);
        const double s33 = m3LoopErrors == 0 ? 100.0 : clamp100(100.0 - double(m3LoopErrors) * 25.0);
        const double s34 = res.m3.p99DriftMs <= 1.5 ? 100.0 : clamp100(100.0 - (res.m3.p99DriftMs - 1.5) * 20.0);
        const double w31 = 0.30, w32 = 0.30, w33 = 0.25;
        const double w34 = m3OnsetDriftsMs.empty() ? 0.0 : 0.15;
        const double wSum = w31 + w32 + w33 + w34;
        res.m3.score = res.structuralValid ? (w31 * s31 + w32 * s32 + w33 * s33 + w34 * s34) / wSum : 0.0;
        res.m3.status = (res.structuralValid && res.m3.score >= 80.0 && m3LoopErrors == 0) ? BenchStatus::Pass : BenchStatus::Fail;
    }

    // M4 Drum & Percussion Accuracy (Weight 15%)
    res.m4.applicable = (m4DrumHits > 0 || m4FamilyChecked > 0);
    res.m4.unmappedHitRate = m4DrumHits ? double(m4UnmappedHits) / double(m4DrumHits) : 0.0;
    res.m4.familyPreservationRate = m4FamilyChecked ? double(m4FamilyPreserved) / double(m4FamilyChecked) : 0.0;
    if (srcR1Primary > 0 || srcR2Primary > 0) {
        res.m4.trackSeparationAccuracy = (expDrumPrimary >= expPercPrimary && expDrumPrimary > 0) ? 1.0 : 0.0;
    } else {
        res.m4.trackSeparationAccuracy = 0.0;
    }
    res.m4.velCorrelation = m4SrcVels.empty() ? 0.0 : pearsonCorrelation(m4SrcVels, m4ExpVels);
    if (!res.m4.applicable) {
        res.m4.score = 0.0;
        res.m4.status = BenchStatus::NA;
    } else {
        const double s41 = res.m4.unmappedHitRate <= 0.02 ? 100.0 : clamp100(100.0 - (res.m4.unmappedHitRate - 0.02) * 300.0);
        const double s42 = clamp100(res.m4.familyPreservationRate * 100.0);
        const double s43 = clamp100(res.m4.trackSeparationAccuracy * 100.0);
        const double s44 = clamp100(std::max(0.0, res.m4.velCorrelation) * 100.0);
        const double w41 = m4DrumHits > 0 ? 0.35 : 0.0;
        const double w42 = m4FamilyChecked > 0 ? 0.30 : 0.0;
        const double w43 = (srcR1Primary > 0 || srcR2Primary > 0) ? 0.20 : 0.0;
        const double w44 = !m4SrcVels.empty() ? 0.15 : 0.0;
        const double wSum = w41 + w42 + w43 + w44;
        res.m4.score = wSum > 0.0 ? (w41 * s41 + w42 * s42 + w43 * s43 + w44 * s44) / wSum : 0.0;
        res.m4.status = res.m4.score >= 80.0 ? BenchStatus::Pass : BenchStatus::Fail;
    }

    // M5 Mix & Dynamic Balance (Weight 12% — R2-P1-1: renormalize s52 & s54 when N/A, never free 100.0!)
    res.m5.mixCcConsistency = m5MixExpected ? double(m5MixPresent) / double(m5MixExpected) : 0.0;
    res.m5.mixCcStatus = m5MixExpected == 0 ? BenchStatus::NA : (res.m5.mixCcConsistency >= 0.80 ? BenchStatus::Pass : BenchStatus::Fail);
    res.m5.unsafeCcLeaks = m5UnsafeCc;
    const bool s52Applicable = (ref.srcUnsafeCcCount > 0 || m5UnsafeCc > 0);
    res.m5.unsafeCcStatus = !s52Applicable ? BenchStatus::NA : (m5UnsafeCc == 0 ? BenchStatus::Pass : BenchStatus::Fail);
    res.m5.balanceHealthRate = m5BalanceTracks ? double(m5BalanceHealthy) / double(m5BalanceTracks) : 0.0;
    res.m5.balanceStatus = m5BalanceTracks == 0 ? BenchStatus::NA : (res.m5.balanceHealthRate >= 0.80 ? BenchStatus::Pass : BenchStatus::Fail);
    res.m5.xgSysexCoverage = 0.0;
    res.m5.xgSysexStatus = ref.xgSysexCount == 0 ? BenchStatus::NA : (res.m5.xgSysexCoverage >= 0.80 ? BenchStatus::Pass : BenchStatus::Unsupported);
    {
        const double s51 = clamp100(res.m5.mixCcConsistency * 100.0);
        const double s52 = m5UnsafeCc == 0 ? 100.0 : clamp100(100.0 - double(m5UnsafeCc) * 15.0);
        const double s53 = clamp100(res.m5.balanceHealthRate * 100.0);
        const double s54 = clamp100(res.m5.xgSysexCoverage * 100.0);
        const double w51 = m5MixExpected > 0 ? 0.35 : 0.0;
        const double w52 = s52Applicable ? 0.25 : 0.0;
        const double w53 = m5BalanceTracks > 0 ? 0.25 : 0.0;
        const double w54 = ref.xgSysexCount > 0 ? 0.15 : 0.0;
        const double wSum = w51 + w52 + w53 + w54;
        res.m5.applicable = (wSum > 0.0);
        if (!res.m5.applicable) {
            res.m5.score = 0.0;
            res.m5.status = BenchStatus::NA;
        } else {
            res.m5.score = (w51 * s51 + w52 * s52 + w53 * s53 + w54 * s54) / wSum;
            res.m5.status = (res.m5.score >= 80.0 && m5UnsafeCc == 0) ? BenchStatus::Pass : BenchStatus::Fail;
        }
    }

    // M6 Timbre & Target-Resource Coherence (Weight 13% — P1-2 honest scoring + independent denominator!)
    {
        uint32_t exportedTracks = 0;
        uint32_t resolvedTracks = 0;
        double matchCredit = 0.0;
        for (const auto& tr : mapReport.tracks) {
            if (tr.korg_ch == 0xFF) continue;
            ++exportedTracks;
            const bool identified = (tr.family != "Unidentified" && !tr.family.empty());
            const bool resolved = (tr.target != "UNRESOLVED" && tr.target != "not exported");
            if (resolved) ++resolvedTracks;
            if (identified && resolved) matchCredit += 1.0;
            else if (identified) matchCredit += 0.15; // Tiny role-hint credit only when UNRESOLVED (0.0 if Unidentified)
        }
        // Denominator never smaller than independent active Korg channels derived from raw source .sty!
        const uint32_t m6Denom = std::max<uint32_t>(exportedTracks, static_cast<uint32_t>(expectedKorgChsFromRef.size()));
        res.m6.familyMatchRate = m6Denom ? clamp01(matchCredit / double(m6Denom)) : 0.0;
        res.m6.familyMatchStatus = m6Denom == 0 ? BenchStatus::NA : (res.m6.familyMatchRate >= 0.80 ? BenchStatus::Pass : BenchStatus::Fail);
        res.m6.hasMidStyleVoiceChanges = (ref.midStyleVoiceChanges > 0);
        res.m6.elementVoicePreservation = res.m6.hasMidStyleVoiceChanges
                                              ? clamp01(double(mapReport.element_sounds.size()) /
                                                        double(ref.midStyleVoiceChanges))
                                              : 0.0;
        res.m6.elementVoiceStatus = !res.m6.hasMidStyleVoiceChanges
                                        ? BenchStatus::NA
                                        : (res.m6.elementVoicePreservation >= 0.80 ? BenchStatus::Pass : BenchStatus::Fail);
        res.m6.targetKstEvaluated = !opt.targetKstPath.empty();
        if (res.m6.targetKstEvaluated) {
            std::error_code ec;
            const bool exists = std::filesystem::exists(opt.targetKstPath, ec) && !ec;
            // Require real target path AND at least one resolved target sound slot (Q4)
            res.m6.targetKstValid = exists && (resolvedTracks > 0);
            res.m6.targetKstStatus = res.m6.targetKstValid ? BenchStatus::Pass : BenchStatus::Fail;
        } else {
            res.m6.targetKstValid = false; // Never claim valid when not evaluated!
            res.m6.targetKstStatus = BenchStatus::NotTested;
        }

        const double s61 = clamp100(res.m6.familyMatchRate * 100.0);
        const double s62 = clamp100(res.m6.elementVoicePreservation * 100.0);
        const double s63 = res.m6.targetKstValid ? 100.0 : 0.0;
        const double w61 = m6Denom > 0 ? 0.65 : 0.0;
        const double w62 = res.m6.hasMidStyleVoiceChanges ? 0.25 : 0.0;
        const double w63 = res.m6.targetKstEvaluated ? 0.10 : 0.0;
        const double wSum = w61 + w62 + w63;
        res.m6.applicable = (wSum > 0.0);
        if (!res.m6.applicable) {
            res.m6.score = 0.0;
            res.m6.status = BenchStatus::NA;
        } else if (res.m6.targetKstEvaluated && !res.m6.targetKstValid) {
            res.m6.score = 0.0;
            res.m6.status = BenchStatus::Fail;
        } else {
            res.m6.score = (w61 * s61 + w62 * s62 + w63 * s63) / wSum;
            res.m6.status = res.m6.score >= 80.0 ? BenchStatus::Pass : BenchStatus::Fail;
        }
    }

    // ── Immutable Weighted Composite Score (Renormalized Over Applicable Dimensions ONLY — Zero Fake N/A=100!) ──
    const std::array<double, 6> kWeights = {kWeightM1, kWeightM2, kWeightM3, kWeightM4, kWeightM5, kWeightM6};
    const std::array<double, 6> dimScores = {res.m1.score, res.m2.score, res.m3.score,
                                             res.m4.score, res.m5.score, res.m6.score};
    const std::array<bool, 6> dimApplicable = {res.m1.applicable, res.m2.applicable, res.m3.applicable,
                                               res.m4.applicable, res.m5.applicable, res.m6.applicable};
    double compNum = 0.0, compDen = 0.0;
    double minDim = 100.0;
    for (size_t i = 0; i < 6; ++i) {
        if (dimApplicable[i]) {
            compNum += kWeights[i] * dimScores[i];
            compDen += kWeights[i];
            minDim = std::min(minDim, dimScores[i]);
        }
    }
    res.applicableWeightSum = compDen;
    res.compositeScore = compDen > 0.0 ? (compNum / compDen) : 0.0;

    if (!res.structuralValid) res.grade = kStatusFail;
    else if (res.compositeScore >= 85.0 && minDim >= 80.0 && m3LoopErrors == 0 &&
             m2LeakedChordRange == 0 && m2LeakedFxNotes == 0 && m2VelInversions == 0)
        res.grade = kStatusPass;
    else if (res.compositeScore >= 60.0) res.grade = "WARN";
    else res.grade = kStatusFail;

    // ── Matrix Summary ───────────────────────────────────────────────
    res.matrixSummary.cellsEvaluated = static_cast<uint32_t>(res.cells.size());
    for (const auto& c : res.cells) {
        if (c.cellScore >= 85.0 && c.loopErrors == 0) ++res.matrixSummary.cellsPass;
        else if (c.cellScore >= 60.0) ++res.matrixSummary.cellsWarn;
        else ++res.matrixSummary.cellsFail;
    }

    // ── Explicit UNSUPPORTED Source & Target Features (5-State Taxonomy) ──
    bool hasCv2 = false;
    for (const auto& el : reparsedStyle.elements) if (cvIndexOfMarker(el.marker) >= 2) hasCv2 = true;
    bool hasGuitarParamSheet = false;
    for (const auto& [key, tp] : opt.parameterSheet.tracks) {
        (void)key;
        if (tp.hasOverride && tp.guitarMode) hasGuitarParamSheet = true;
    }

    if (hasBreakSection && !opt.breakYamaha1BarCap) {
        res.unsupportedFeatures.push_back({"BREAK_WITHHELD_BY_POLICY", BenchStatus::Unsupported, 1, "BreakPolicy::Withhold"});
    }
    if (ref.hasChordMutePairs && !hasCv2) {
        res.unsupportedFeatures.push_back({"CHORD_MUTE_CV_SPLIT_Q2", BenchStatus::Unsupported, 1, "merged_into_cv1_until_q2"});
    }
    if (!ref.megaVoiceSrcChannels.empty() && (m2LeakedFxNotes > 0 || m2VelInversions > 0)) {
        res.unsupportedFeatures.push_back({"MEGAVOICE_SANITIZATION_Q2", BenchStatus::Unsupported,
                                           static_cast<uint32_t>(ref.megaVoiceSrcChannels.size()), "unshaped_until_q2"});
    }
    if (!ref.sartSrcChannels.empty() && m2LeakedFxNotes > 0) {
        res.unsupportedFeatures.push_back({"SART_KEYSWITCH_FILTER_Q2", BenchStatus::Unsupported,
                                           static_cast<uint32_t>(ref.sartSrcChannels.size()), "unfiltered_until_q2"});
    }
    if (!ref.guitarNtrSrcChannels.empty() && !hasGuitarParamSheet) {
        res.unsupportedFeatures.push_back({"SFF2_GUITAR_MODE_Q2", BenchStatus::Unsupported,
                                           static_cast<uint32_t>(ref.guitarNtrSrcChannels.size()), "passthrough_until_q2"});
    }
    if (ref.hasExtendedDrumNotes && m4UnmappedHits > 0) {
        res.unsupportedFeatures.push_back({"EXTENDED_XG_REVO_DRUM_MAP_Q3", BenchStatus::Unsupported,
                                           static_cast<uint32_t>(m4UnmappedHits), "raw_note_until_q3"});
    }
    if (ref.midStyleVoiceChanges > 0 && mapReport.element_sounds.empty()) {
        res.unsupportedFeatures.push_back({"MID_STYLE_ELEMENT_VOICES_Q4", BenchStatus::Unsupported,
                                           ref.midStyleVoiceChanges, "unmapped_until_q4"});
    }
    if (ref.xgSysexCount > 0 && res.m5.xgSysexCoverage < 1.0) {
        res.unsupportedFeatures.push_back({"XG_DSP_SYSEX_TRANSLATION_Q5", BenchStatus::Unsupported,
                                           ref.xgSysexCount, "skipped_until_q5"});
    }
    if (ref.tempos.size() > 1) {
        res.unsupportedFeatures.push_back({"MID_STYLE_TEMPO_MAP_RUBATO", BenchStatus::Unsupported,
                                           static_cast<uint32_t>(ref.tempos.size() - 1), "pa5x_single_style_tempo"});
    }
    for (const auto& cid : ref.extraChunks) {
        if (cid == "OTSc" || cid == "OTSm" || cid == "OTS ") {
            res.unsupportedFeatures.push_back({"OTSc", BenchStatus::Unsupported, 1, cid});
        } else if (cid == "FNRP") {
            res.unsupportedFeatures.push_back({"FNRP", BenchStatus::Unsupported, 1, cid});
        } else {
            res.unsupportedFeatures.push_back({"MDB", BenchStatus::Unsupported, 1, cid});
        }
    }
    std::sort(res.unsupportedFeatures.begin(), res.unsupportedFeatures.end(),
              [](const UnsupportedFeature& a, const UnsupportedFeature& b) {
                  return std::tie(a.feature, a.reason, a.count) < std::tie(b.feature, b.reason, b.count);
              });

    // ── Structured Diagnostics (STYLE_QUAL_*) ────────────────────────
    if (res.m1.casmPreservationRate < 1.0) {
        res.diagnostics.push_back({"STYLE_QUAL_CASM_LOST", "WARN", "*", "*",
                                   casmFieldsChecked - casmFieldsPreserved, "default_pa5x_ntt"});
    }
    if (ref.hasChordMutePairs && !hasCv2) {
        res.diagnostics.push_back({"STYLE_QUAL_CHORD_MUTE_SPLIT", "WARN", "*", "Acc4/Acc5", 1, "merged_into_cv1"});
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
    for (const auto& sr : mapReport.sections) {
        if (sr.note.find("STYLE_QUAL_BREAK_TAIL_TRUNCATED") != std::string::npos) {
            res.diagnostics.push_back({"STYLE_QUAL_BREAK_TAIL_TRUNCATED", "INFO", sr.source, "*", 1, "yamaha_1bar_cap"});
        }
    }
    for (const auto& w : mapReport.warnings) {
        if (w.find("STYLE_QUAL_BREAK_TAIL_TRUNCATED") != std::string::npos) {
            bool already = std::any_of(res.diagnostics.begin(), res.diagnostics.end(),
                                       [](const Diagnostic& d) { return d.code == "STYLE_QUAL_BREAK_TAIL_TRUNCATED"; });
            if (!already) {
                res.diagnostics.push_back({"STYLE_QUAL_BREAK_TAIL_TRUNCATED", "INFO", "Fill In BA", "*", 1, "yamaha_1bar_cap"});
            }
        }
    }

    std::sort(res.diagnostics.begin(), res.diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return std::tie(a.code, a.severity, a.section, a.track, a.count, a.fallback) <
               std::tie(b.code, b.severity, b.section, b.track, b.count, b.fallback);
    });

    res.ok = true;
    return res;
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

    MapOptions kopt;
    kopt.ppqn = opt.targetPpqn;
    kopt.route = routeFromCasm(parsed);
    detail::applyBreakPolicy(kopt, opt.breakYamaha1BarCap);

    const auto mappedKorg = mapToKorg(desRes.style, kopt);
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

    return detail::evaluateReparsedSmf(ref, reparsed.style, mappedKorg.report, &desRes.style, res.fixtureName, opt);
}

// Evaluate an already-exported/modified Korg SMF against raw source .sty bytes (for regression testing
// Q2 velocity sanitization, M3.4 timing-drift detection, Parameter Sheet simulation, etc.).
inline BenchResult evaluateExportedStyleAgainstSty(const std::vector<uint8_t>& styBytes,
                                                   const Style& exportedKorgStyle,
                                                   const Report& mapReport,
                                                   const std::string& displayPath,
                                                   const BenchOptions& opt = {}) {
    BenchResult res;
    res.fixtureName = detail::basenameOnly(displayPath);
    const auto ref = detail::analyzeReferenceSty(styBytes);
    if (!ref.ok) {
        res.error = "Reference analysis failed: " + ref.error;
        return res;
    }
    return detail::evaluateReparsedSmf(ref, exportedKorgStyle, mapReport, nullptr, res.fixtureName, opt);
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

// ── Corpus Summary Aggregation (5-State Taxonomy + Coverage/Exclusions/Unsupported/NotTested) ──
inline CorpusSummary computeCorpusSummary(const std::vector<BenchResult>& results) {
    CorpusSummary cs;
    cs.fixtureCount = static_cast<uint32_t>(results.size());
    cs.allStructuralValid = !results.empty();
    cs.l3Status = BenchStatus::NotTested;
    cs.minComposite = results.empty() ? 0.0 : 100.0;
    cs.minDimensionScores.fill(results.empty() ? 0.0 : 100.0);

    std::array<double, 6> sumDim{};
    std::array<double, 6> activeSum{};
    double sumComp = 0.0;
    uint32_t okCount = 0;

    for (const auto& r : results) {
        if (!r.ok || !r.structuralValid) {
            cs.allStructuralValid = false;
            ++cs.hardInvariantViolations;
        }
        if (!r.ok) {
            ++cs.failCount;
            ++cs.targetKstNotTestedCount;
            cs.minComposite = 0.0;
            continue;
        }
        ++okCount;
        if (r.grade == kStatusPass) ++cs.passCount;
        else if (r.grade == "WARN") ++cs.warnCount;
        else ++cs.failCount;

        if (r.m6.targetKstStatus == BenchStatus::Pass) ++cs.targetKstPassCount;
        else if (r.m6.targetKstStatus == BenchStatus::Fail) ++cs.targetKstFailCount;
        else ++cs.targetKstNotTestedCount;

        if (!r.unsupportedFeatures.empty()) {
            ++cs.fixturesWithUnsupported;
            for (const auto& uf : r.unsupportedFeatures) {
                cs.unsupportedTotalOccurrences += uf.count;
                cs.unsupportedByFeature[uf.feature] += uf.count;
            }
        }

        cs.hardInvariantViolations += r.m3.loopBoundaryErrors + r.m2.leakedChordRange +
                                      r.m2.leakedFxNotesCount + r.m2.velocityInversions;

        const std::array<double, 6> dims = {r.m1.score, r.m2.score, r.m3.score,
                                            r.m4.score, r.m5.score, r.m6.score};
        const std::array<bool, 6> evalApp = {r.m1.applicable, r.m2.applicable, r.m3.applicable,
                                             r.m4.applicable, r.m5.applicable, r.m6.applicable};
        const std::array<BenchStatus, 6> dimStatus = {r.m1.status, r.m2.status, r.m3.status,
                                                      r.m4.status, r.m5.status, r.m6.status};
        const std::array<bool, 6> stressApp = {r.applicability.m1Harmonic, r.applicability.m2Articulation,
                                               r.applicability.m3SectionStress, r.applicability.m4DrumStress,
                                               r.applicability.m5MixStress, r.applicability.m6Timbre};
        sumComp += r.compositeScore;
        cs.minComposite = std::min(cs.minComposite, r.compositeScore);
        for (size_t i = 0; i < 6; ++i) {
            if (evalApp[i]) {
                sumDim[i] += dims[i];
                ++cs.evaluatedDimensionCounts[i];
                cs.minDimensionScores[i] = std::min(cs.minDimensionScores[i], dims[i]);
                if (dimStatus[i] == BenchStatus::Pass) ++cs.dimPassCounts[i];
                else ++cs.dimFailCounts[i];
            } else {
                ++cs.naExcludedCounts[i];
            }
            if (stressApp[i] && evalApp[i]) {
                activeSum[i] += dims[i];
                ++cs.activeDimensionCounts[i];
            }
        }
    }

    cs.notTestedItems.push_back("L3_PA5X_HARDWARE_VALIDATION");
    if (cs.targetKstNotTestedCount > 0) {
        cs.notTestedItems.push_back("M6_TARGET_KST_OCCUPANCY");
    }

    const double denom = okCount ? double(okCount) : 1.0;
    cs.meanComposite = sumComp / denom;
    for (size_t i = 0; i < 6; ++i) {
        if (cs.evaluatedDimensionCounts[i] > 0) {
            cs.meanDimensions[i] = sumDim[i] / double(cs.evaluatedDimensionCounts[i]);
        } else {
            cs.meanDimensions[i] = 0.0;
            cs.minDimensionScores[i] = 0.0;
        }
        cs.activeDimensionMeans[i] = cs.activeDimensionCounts[i]
                                         ? (activeSum[i] / double(cs.activeDimensionCounts[i]))
                                         : cs.meanDimensions[i];
    }
    // Exact immutable weights on active-dimension means (eliminates N/A=100 dilution!)
    const std::array<double, 6> kWeights = {kWeightM1, kWeightM2, kWeightM3, kWeightM4, kWeightM5, kWeightM6};
    double actNum = 0.0, actDen = 0.0;
    for (size_t i = 0; i < 6; ++i) {
        if (cs.activeDimensionCounts[i] > 0 || cs.evaluatedDimensionCounts[i] > 0) {
            actNum += kWeights[i] * cs.activeDimensionMeans[i];
            actDen += kWeights[i];
        }
    }
    cs.activeComposite = actDen > 0.0 ? (actNum / actDen) : 0.0;

    if (cs.allStructuralValid && cs.meanComposite >= 85.0 && cs.activeComposite >= 85.0 &&
        cs.minComposite >= 80.0 && cs.hardInvariantViolations == 0 && cs.failCount == 0) {
        cs.corpusGrade = kStatusPass;
    } else if (!cs.allStructuralValid || cs.hardInvariantViolations > 0 ||
               cs.failCount > 0 || cs.activeComposite < 60.0) {
        cs.corpusGrade = kStatusFail;
    } else {
        cs.corpusGrade = "WARN";
    }
    return cs;
}

// ── Deterministic Path-Scrubbed JSON Serializer ──────────────────────
inline std::string toJson(const BenchResult& r) {
    using detail::fmtNum;
    using detail::jsonEscape;
    auto optScore = [](bool applicable, double score) -> std::string {
        return applicable ? fmtNum(score, 2) : "null";
    };
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"" << kSchemaId << "\",\n";
    o << "  \"statusTaxonomy\": [\"PASS\", \"FAIL\", \"UNSUPPORTED\", \"N/A\", \"NOT_TESTED\"],\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"" << benchStatusToString(r.l3Status) << "\",\n";
    o << "  \"label\": \"" << ((r.ok && r.structuralValid) ? kValidLabel : kInvalidLabel) << "\",\n";
    o << "  \"fixture\": \"" << jsonEscape(r.fixtureName) << "\",\n";
    if (!r.ok) {
        o << "  \"ok\": false,\n";
        o << "  \"error\": \"" << jsonEscape(r.error) << "\",\n";
    }
    o << "  \"weights\": { \"M1\": 0.25, \"M2\": 0.20, \"M3\": 0.15, \"M4\": 0.15, \"M5\": 0.12, \"M6\": 0.13 },\n";
    o << "  \"style\": { \"format\": \"" << jsonEscape(r.format)
      << "\", \"sourcePpqn\": " << r.sourcePpqn
      << ", \"targetPpqn\": " << r.targetPpqn
      << ", \"sectionsSource\": " << r.sectionsSource
      << ", \"sectionsExported\": " << r.sectionsExported << " },\n";
    o << "  \"structuralValid\": " << (r.structuralValid ? "true" : "false") << ",\n";
    o << "  \"structuralStatus\": \"" << benchStatusToString(r.structuralStatus) << "\",\n";
    o << "  \"applicableWeightSum\": " << fmtNum(r.applicableWeightSum, 2) << ",\n";
    o << "  \"compositeScore\": " << fmtNum(r.compositeScore, 2) << ",\n";
    o << "  \"grade\": \"" << jsonEscape(r.grade) << "\",\n";
    o << "  \"applicability\": { \"m1\": " << (r.applicability.m1Harmonic ? "true" : "false")
      << ", \"m2\": " << (r.applicability.m2Articulation ? "true" : "false")
      << ", \"m3\": " << (r.applicability.m3SectionStress ? "true" : "false")
      << ", \"m4\": " << (r.applicability.m4DrumStress ? "true" : "false")
      << ", \"m5\": " << (r.applicability.m5MixStress ? "true" : "false")
      << ", \"m6\": " << (r.applicability.m6Timbre ? "true" : "false") << " },\n";
    o << "  \"dimensions\": {\n";
    o << "    \"harmonicVoiceLeading\": { \"status\": \"" << benchStatusToString(r.m1.status)
      << "\", \"score\": " << optScore(r.m1.applicable, r.m1.score)
      << ", \"clashRate\": " << fmtNum(r.m1.clashRate, 4)
      << ", \"bassSlashAccuracy\": " << fmtNum(r.m1.bassSlashAccuracy, 4)
      << ", \"bassSlashStatus\": \"" << benchStatusToString(r.m1.bassSlashStatus) << "\""
      << ", \"voiceLeadingAvgSemis\": " << fmtNum(r.m1.voiceLeadingAvgSemis, 3)
      << ", \"voiceLeadingStatus\": \"" << benchStatusToString(r.m1.voiceLeadingStatus) << "\""
      << ", \"registerViolationRate\": " << fmtNum(r.m1.registerViolationRate, 4)
      << ", \"voicingCollapseRate\": " << fmtNum(r.m1.voicingCollapseRate, 4)
      << ", \"voicingCollapseStatus\": \"" << benchStatusToString(r.m1.voicingCollapseStatus) << "\""
      << ", \"casmPreservationRate\": " << fmtNum(r.m1.casmPreservationRate, 4) << " },\n";
    o << "    \"articulationMegaVoice\": { \"status\": \"" << benchStatusToString(r.m2.status)
      << "\", \"score\": " << optScore(r.m2.applicable, r.m2.score)
      << ", \"leakedChordRange\": " << r.m2.leakedChordRange
      << ", \"leakedFxKeyRate\": " << fmtNum(r.m2.leakedFxKeyRate, 4)
      << ", \"leakedFxNotesCount\": " << r.m2.leakedFxNotesCount
      << ", \"fxKeyStatus\": \"" << benchStatusToString(r.m2.fxKeyStatus) << "\""
      << ", \"velocityInversions\": " << r.m2.velocityInversions
      << ", \"velocityZoneStatus\": \"" << benchStatusToString(r.m2.velocityZoneStatus) << "\""
      << ", \"guitarStrumCoherence\": " << fmtNum(r.m2.guitarStrumCoherence, 4)
      << ", \"guitarStrumStatus\": \"" << benchStatusToString(r.m2.guitarStrumStatus) << "\" },\n";
    o << "    \"sectionCompleteness\": { \"status\": \"" << benchStatusToString(r.m3.status)
      << "\", \"score\": " << optScore(r.m3.applicable, r.m3.score)
      << ", \"coverageRate\": " << fmtNum(r.m3.coverageRate, 4)
      << ", \"meterBarAccuracy\": " << fmtNum(r.m3.meterBarAccuracy, 4)
      << ", \"loopBoundaryErrors\": " << r.m3.loopBoundaryErrors
      << ", \"p99DriftMs\": " << fmtNum(r.m3.p99DriftMs, 3)
      << ", \"timingDriftStatus\": \"" << benchStatusToString(r.m3.timingDriftStatus) << "\" },\n";
    o << "    \"drumPercussion\": { \"status\": \"" << benchStatusToString(r.m4.status)
      << "\", \"score\": " << optScore(r.m4.applicable, r.m4.score)
      << ", \"unmappedHitRate\": " << fmtNum(r.m4.unmappedHitRate, 4)
      << ", \"familyPreservationRate\": " << fmtNum(r.m4.familyPreservationRate, 4)
      << ", \"trackSeparationAccuracy\": " << fmtNum(r.m4.trackSeparationAccuracy, 4)
      << ", \"velCorrelation\": " << fmtNum(r.m4.velCorrelation, 4) << " },\n";
    o << "    \"mixDynamicBalance\": { \"status\": \"" << benchStatusToString(r.m5.status)
      << "\", \"score\": " << optScore(r.m5.applicable, r.m5.score)
      << ", \"mixCcConsistency\": " << fmtNum(r.m5.mixCcConsistency, 4)
      << ", \"mixCcStatus\": \"" << benchStatusToString(r.m5.mixCcStatus) << "\""
      << ", \"unsafeCcLeaks\": " << r.m5.unsafeCcLeaks
      << ", \"unsafeCcStatus\": \"" << benchStatusToString(r.m5.unsafeCcStatus) << "\""
      << ", \"balanceHealthRate\": " << fmtNum(r.m5.balanceHealthRate, 4)
      << ", \"balanceStatus\": \"" << benchStatusToString(r.m5.balanceStatus) << "\""
      << ", \"xgSysexCoverage\": " << fmtNum(r.m5.xgSysexCoverage, 4)
      << ", \"xgSysexStatus\": \"" << benchStatusToString(r.m5.xgSysexStatus) << "\" },\n";
    o << "    \"timbreResourceCoherence\": { \"status\": \"" << benchStatusToString(r.m6.status)
      << "\", \"score\": " << optScore(r.m6.applicable, r.m6.score)
      << ", \"familyMatchRate\": " << fmtNum(r.m6.familyMatchRate, 4)
      << ", \"familyMatchStatus\": \"" << benchStatusToString(r.m6.familyMatchStatus) << "\""
      << ", \"hasMidStyleVoiceChanges\": " << (r.m6.hasMidStyleVoiceChanges ? "true" : "false")
      << ", \"elementVoicePreservation\": " << fmtNum(r.m6.elementVoicePreservation, 4)
      << ", \"elementVoiceStatus\": \"" << benchStatusToString(r.m6.elementVoiceStatus) << "\""
      << ", \"targetKstEvaluated\": " << (r.m6.targetKstEvaluated ? "true" : "false")
      << ", \"targetKstValid\": " << (r.m6.targetKstValid ? "true" : "false")
      << ", \"targetKstStatus\": \"" << benchStatusToString(r.m6.targetKstStatus) << "\" }\n";
    o << "  },\n";
    o << "  \"unsupportedFeatures\": [";
    if (!r.unsupportedFeatures.empty()) o << "\n";
    for (size_t i = 0; i < r.unsupportedFeatures.size(); ++i) {
        const auto& u = r.unsupportedFeatures[i];
        o << "    { \"feature\": \"" << jsonEscape(u.feature)
          << "\", \"status\": \"" << benchStatusToString(u.status)
          << "\", \"count\": " << u.count
          << ", \"reason\": \"" << jsonEscape(u.reason) << "\" }";
        o << (i + 1 < r.unsupportedFeatures.size() ? ",\n" : "\n  ");
    }
    o << "],\n";
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

// ── Deterministic Markdown Report Formatter (5-State Taxonomy + Coverage/NotTested) ──
inline std::string toMarkdown(const std::vector<BenchResult>& results) {
    using detail::fmtNum;
    const CorpusSummary cs = computeCorpusSummary(results);
    auto fmtDimCell = [](bool applicable, double score) -> std::string {
        return applicable ? fmtNum(score, 2) : std::string(kStatusNA);
    };
    auto fmtAggDim = [](uint32_t count, double score) -> std::string {
        return count > 0 ? fmtNum(score, 2) : std::string(kStatusNA);
    };
    std::ostringstream o;
    o << "# Offline Style Conversion Quality Benchmark Report\n\n";
    o << "- **Schema:** `" << kSchemaId << "`\n";
    o << "- **Status Taxonomy:** `PASS` | `FAIL` | `UNSUPPORTED` | `N/A` | `NOT_TESTED`\n";
    o << "- **Hardware Verified (`L3`):** `false` (`l3Status = " << benchStatusToString(cs.l3Status) << "`)\n";
    o << "- **Target KST Gate:** `PASS=" << cs.targetKstPassCount
      << "`, `FAIL=" << cs.targetKstFailCount
      << "`, `NOT_TESTED=" << cs.targetKstNotTestedCount << "`\n";
    o << "- **Not Tested Items (`NOT_TESTED`):** ";
    for (size_t i = 0; i < cs.notTestedItems.size(); ++i) {
        if (i > 0) o << ", ";
        o << "`" << cs.notTestedItems[i] << "`";
    }
    o << "\n";
    o << "- **Status Label:** `" << (cs.allStructuralValid ? kValidLabel : kInvalidLabel) << "`\n";
    o << "- **Immutable Weights:** `M1=25%`, `M2=20%`, `M3=15%`, `M4=15%`, `M5=12%`, `M6=13%` (Sum = `100%`; `N/A` dimensions excluded — never awarded fake `100.0`)\n\n";

    o << "| Fixture | Format | Sec (Exp/Src) | Valid | M1 (25%) | M2 (20%) | M3 (15%) | M4 (15%) | M5 (12%) | M6 (13%) | Composite | Grade | Unsup | Cells (P/W/F) |\n";
    o << "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|\n";

    for (const auto& r : results) {
        if (!r.ok) {
            o << "| `" << r.fixtureName << "` | ERR | 0/0 | FAIL | N/A | N/A | N/A | N/A | N/A | N/A | **0.00** | FAIL | 0 | 0/0/0 |\n";
            continue;
        }
        o << "| `" << r.fixtureName << "` | " << r.format
          << " | " << r.sectionsExported << "/" << r.sectionsSource
          << " | " << benchStatusToString(r.structuralStatus)
          << " | " << fmtDimCell(r.m1.applicable, r.m1.score)
          << " | " << fmtDimCell(r.m2.applicable, r.m2.score)
          << " | " << fmtDimCell(r.m3.applicable, r.m3.score)
          << " | " << fmtDimCell(r.m4.applicable, r.m4.score)
          << " | " << fmtDimCell(r.m5.applicable, r.m5.score)
          << " | " << fmtDimCell(r.m6.applicable, r.m6.score)
          << " | **" << fmtNum(r.compositeScore, 2) << "**"
          << " | " << r.grade
          << " | " << r.unsupportedFeatures.size()
          << " | " << r.matrixSummary.cellsPass << "/" << r.matrixSummary.cellsWarn << "/" << r.matrixSummary.cellsFail << " |\n";
    }
    if (cs.fixtureCount > 0) {
        o << "| **EVALUATED MEAN (excl. N/A)** | — | — | " << (cs.allStructuralValid ? kStatusPass : kStatusFail)
          << " | **" << fmtAggDim(cs.evaluatedDimensionCounts[0], cs.meanDimensions[0]) << "** (" << cs.evaluatedDimensionCounts[0] << ")"
          << " | **" << fmtAggDim(cs.evaluatedDimensionCounts[1], cs.meanDimensions[1]) << "** (" << cs.evaluatedDimensionCounts[1] << ")"
          << " | **" << fmtAggDim(cs.evaluatedDimensionCounts[2], cs.meanDimensions[2]) << "** (" << cs.evaluatedDimensionCounts[2] << ")"
          << " | **" << fmtAggDim(cs.evaluatedDimensionCounts[3], cs.meanDimensions[3]) << "** (" << cs.evaluatedDimensionCounts[3] << ")"
          << " | **" << fmtAggDim(cs.evaluatedDimensionCounts[4], cs.meanDimensions[4]) << "** (" << cs.evaluatedDimensionCounts[4] << ")"
          << " | **" << fmtAggDim(cs.evaluatedDimensionCounts[5], cs.meanDimensions[5]) << "** (" << cs.evaluatedDimensionCounts[5] << ")"
          << " | **" << fmtNum(cs.meanComposite, 2) << "**"
          << " | **" << cs.corpusGrade << "** | " << cs.fixturesWithUnsupported << "/" << cs.fixtureCount << " | — |\n";
        o << "| **ACTIVE-TORTURE MEAN** | — | — | —"
          << " | **" << fmtAggDim(cs.activeDimensionCounts[0], cs.activeDimensionMeans[0]) << "** (" << cs.activeDimensionCounts[0] << ")"
          << " | **" << fmtAggDim(cs.activeDimensionCounts[1], cs.activeDimensionMeans[1]) << "** (" << cs.activeDimensionCounts[1] << ")"
          << " | **" << fmtAggDim(cs.activeDimensionCounts[2], cs.activeDimensionMeans[2]) << "** (" << cs.activeDimensionCounts[2] << ")"
          << " | **" << fmtAggDim(cs.activeDimensionCounts[3], cs.activeDimensionMeans[3]) << "** (" << cs.activeDimensionCounts[3] << ")"
          << " | **" << fmtAggDim(cs.activeDimensionCounts[4], cs.activeDimensionMeans[4]) << "** (" << cs.activeDimensionCounts[4] << ")"
          << " | **" << fmtAggDim(cs.activeDimensionCounts[5], cs.activeDimensionMeans[5]) << "** (" << cs.activeDimensionCounts[5] << ")"
          << " | **" << fmtNum(cs.activeComposite, 2) << "**"
          << " | **" << cs.corpusGrade << "** | " << cs.unsupportedTotalOccurrences << " | — |\n";
        o << "| **MIN (evaluated)** | — | — | —"
          << " | " << fmtAggDim(cs.evaluatedDimensionCounts[0], cs.minDimensionScores[0])
          << " | " << fmtAggDim(cs.evaluatedDimensionCounts[1], cs.minDimensionScores[1])
          << " | " << fmtAggDim(cs.evaluatedDimensionCounts[2], cs.minDimensionScores[2])
          << " | " << fmtAggDim(cs.evaluatedDimensionCounts[3], cs.minDimensionScores[3])
          << " | " << fmtAggDim(cs.evaluatedDimensionCounts[4], cs.minDimensionScores[4])
          << " | " << fmtAggDim(cs.evaluatedDimensionCounts[5], cs.minDimensionScores[5])
          << " | " << fmtNum(cs.minComposite, 2)
          << " | P:" << cs.passCount << " W:" << cs.warnCount << " F:" << cs.failCount
          << " | HardViol:" << cs.hardInvariantViolations << " | — |\n\n";

        o << "## Corpus Coverage, N/A Exclusions, Unsupported & Not-Tested Summary\n\n";
        o << "| Dimension | Evaluated | Active Torture | N/A Excluded | Dim PASS | Dim FAIL |\n";
        o << "|---|---:|---:|---:|---:|---:|\n";
        static constexpr const char* kDimNames[6] = {"M1 (25%)", "M2 (20%)", "M3 (15%)", "M4 (15%)", "M5 (12%)", "M6 (13%)"};
        for (size_t i = 0; i < 6; ++i) {
            o << "| " << kDimNames[i]
              << " | " << cs.evaluatedDimensionCounts[i]
              << " | " << cs.activeDimensionCounts[i]
              << " | " << cs.naExcludedCounts[i]
              << " | " << cs.dimPassCounts[i]
              << " | " << cs.dimFailCounts[i] << " |\n";
        }
        if (!cs.unsupportedByFeature.empty()) {
            o << "\n| Unsupported Feature (`UNSUPPORTED`) | Occurrences |\n|---|---:|\n";
            for (const auto& [feat, cnt] : cs.unsupportedByFeature) {
                o << "| `" << feat << "` | " << cnt << " |\n";
            }
        }
    }
    return o.str();
}

} // namespace ai_arranger::exporters::korg::bench

#endif // AI_ARRANGER_EXPORTERS_KORG_STYLE_QUALITY_BENCH_H
