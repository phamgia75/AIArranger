#ifndef AI_ARRANGER_EXPORTERS_KORG_ACCEPTANCE_GATE_H
#define AI_ARRANGER_EXPORTERS_KORG_ACCEPTANCE_GATE_H

#include "exporters/korg/conversion_manifest.h"
#include "exporters/korg/musical_quality_analyzer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Proposed Production Acceptance Gate (Phase 3 P3-M7): Grade A / B / C / D / U.
//
// Architectural & Epistemic Invariants:
//   - Explicitly tagged "criteriaStatus": "PROPOSED_ACCEPTANCE_CRITERIA".
//   - Strictly enforces hardware verification boundary: offline structural/analyzer
//     PASS alone ALWAYS yields Grade U (Unverified) — NEVER Grade A or Grade B.
//   - Grade A (Performance Ready) requires BOTH hardware_verified == true AND
//     valid human listening evidence (overall_rating >= 4, 0 critical defects,
//     0 minor defects, 0 hard invariant violations, 0 unresolved sounds).
//   - Strict fail-closed JSON ingestion for external human listening records.
//   - Completely independent of frozen Q1 benchmark (style_quality_bench.h).
//   - Portable C++20 header-only, zero JUCE/Apple dependencies, ODR-safe
//     (`namespace acceptance_gate_detail`).

namespace ai_arranger::exporters::korg::quality {

enum class AcceptanceGrade : uint8_t {
    A = 0, // Performance Ready (requires hardware_verified + human listening >= 4, 0 defects)
    B = 1, // Usable With Minor Refinement (requires hardware_verified + human listening)
    C = 2, // Manual Correction Required
    D = 3, // Rejected
    U = 4, // Unverified (offline PASS alone ALWAYS lands here)
};

inline const char* acceptanceGradeCode(AcceptanceGrade g) noexcept {
    switch (g) {
        case AcceptanceGrade::A: return "A";
        case AcceptanceGrade::B: return "B";
        case AcceptanceGrade::C: return "C";
        case AcceptanceGrade::D: return "D";
        case AcceptanceGrade::U: return "U";
    }
    return "U";
}

inline const char* acceptanceGradeLabel(AcceptanceGrade g) noexcept {
    switch (g) {
        case AcceptanceGrade::A: return "Performance Ready";
        case AcceptanceGrade::B: return "Usable With Minor Refinement";
        case AcceptanceGrade::C: return "Manual Correction Required";
        case AcceptanceGrade::D: return "Rejected";
        case AcceptanceGrade::U: return "Unverified";
    }
    return "Unverified";
}

inline std::string acceptanceGradeDisplay(AcceptanceGrade g) {
    return std::string("Grade ") + acceptanceGradeCode(g) + " (" + acceptanceGradeLabel(g) + ")";
}

struct HumanListeningRecord {
    bool                 present = false;
    std::string          evaluator_id;               // Non-PII evaluator tag (e.g. "musician-01")
    int                  overall_rating_1_to_5 = 0;  // Required integer 1..5 when present
    std::array<int, 12>  per_dimension_ratings{};    // Optional 1..5 (0 = omitted)
    std::array<bool, 12> per_dimension_present{};
    uint32_t             critical_defect_count = 0;
    uint32_t             minor_defect_count = 0;
    bool                 hardware_verified = false;  // Default false — never assumed offline
    std::string          hardware_model;             // Required non-empty when hardware_verified == true in JSON
    std::string          reference_type;             // e.g. "Genos_Direct_AB", "Standalone_Pa5X_Audition"
    bool                 major_correction_needed = false; // Distinguishes rating==3 Grade C vs Grade B
    bool                 required_sections_verified = true;
    bool                 required_chords_verified = true;
    std::string          notes;                      // Free-form musician notes (strictly JSON-escaped)
};

struct HumanListeningParseResult {
    bool                 ok = false;
    std::string          error;
    HumanListeningRecord record;
};

struct OfflineAcceptanceEvidence {
    std::string              style_name;
    std::string              candidate_label = "Candidate B (Mode A Faithful)";
    bool                     is_enhanced_candidate = false;
    bool                     has_fatal_conversion_error = false;
    std::string              fatal_error_message;
    uint32_t                 hard_invariant_violations = 0;
    uint32_t                 unresolved_critical_sounds = 0;
    uint32_t                 unresolved_total_sounds = 0;
    std::vector<std::string> unresolved_critical_tracks;
    std::vector<std::string> unresolved_non_critical_tracks;
    bool                     has_analyzer_summary = false;
    double                   overall_musical_score = 0.0;
    uint32_t                 analyzer_critical_findings = 0;
    uint32_t                 analyzer_warning_findings = 0;
    uint32_t                 analyzer_advisory_findings = 0;
    std::array<double, 12>   dimension_scores{};
    std::array<bool, 12>     dimension_applicable{};
};

struct AcceptanceReport {
    std::string               schema = "aiarranger-acceptance-report/1";
    std::string               criteria_status = "PROPOSED_ACCEPTANCE_CRITERIA";
    std::string               style_name;
    std::string               candidate_label;
    bool                      is_enhanced_candidate = false;
    AcceptanceGrade           grade = AcceptanceGrade::U;
    std::string               grade_code = "U";
    std::string               grade_label = "Unverified";
    bool                      hardware_verified = false;
    std::string               l3_status = "NOT_TESTED";
    bool                      independent_from_frozen_q1 = true;
    OfflineAcceptanceEvidence offline;
    HumanListeningRecord      listening;
    std::vector<std::string>  gate_reasons;
    std::vector<std::string>  required_next_actions;
};

namespace acceptance_gate_detail {

inline double sanitizeScore(double v) noexcept {
    if (!std::isfinite(v)) return 0.0;
    if (v < 0.0) return 0.0;
    if (v > 100.0) return 100.0;
    return std::round(v * 100.0) / 100.0;
}

inline std::string formatDouble2(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << (std::isfinite(v) ? v : 0.0);
    return oss.str();
}

inline std::string jsonEscape(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uc < 0x20u || uc == 0x7Fu) {
                    out += "\\u00";
                    out.push_back(kHex[(uc >> 4) & 0x0Fu]);
                    out.push_back(kHex[uc & 0x0Fu]);
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

inline std::string mdEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (ch == '\r' || ch == '\n' || uc < 0x20u || uc == 0x7Fu) {
            out.push_back(' ');
        } else if (ch == '|') {
            out += "\\|";
        } else if (ch == '`') {
            out += "'";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

inline bool isBlankAscii(std::string_view s) noexcept {
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

inline bool hasControlChar(std::string_view s) noexcept {
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20u || uc == 0x7Fu) return true;
    }
    return false;
}

inline bool isPlaceholderText(std::string_view s) noexcept {
    if (isBlankAscii(s)) return true;
    std::string up;
    up.reserve(s.size());
    for (char c : s) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return up.find("REPLACE_WITH") != std::string::npos ||
           up.find("REPLACE_ME") != std::string::npos ||
           up.find("TEMPLATE ONLY") != std::string::npos ||
           up.find(" | ") != std::string::npos;
}

inline std::string stripStyleExtension(std::string_view filename) {
    std::string base(filename);
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1u);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0u) {
        std::string ext = base.substr(dot);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".sty" || ext == ".prs" || ext == ".sst" || ext == ".bcs" || ext == ".pcs" || ext == ".fps") {
            base = base.substr(0u, dot);
        }
    }
    return base.empty() ? std::string("UNKNOWN_STYLE") : base;
}

inline std::string canonicalCandidateLabel(std::string_view raw) {
    std::string up;
    up.reserve(raw.size());
    for (char c : raw) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "A_REF" || up.find("CANDIDATE A") != std::string::npos || up.find("REFERENCE") != std::string::npos) {
        return "A_REF";
    }
    if (up == "C_ENHANCED" || up.find("CANDIDATE C") != std::string::npos || up.find("MODE B") != std::string::npos ||
        up.find("ENHANCED") != std::string::npos) {
        return "C_ENHANCED";
    }
    return "B_FAITHFUL";
}

inline std::string inferTemplateSourceClass(std::string_view style_id) {
    std::string up;
    up.reserve(style_id.size());
    for (char c : style_id) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up.rfind("SYNTH_", 0u) == 0u || up.rfind("EXP", 0u) == 0u || up.rfind("MAL", 0u) == 0u) {
        return "SYNTHETIC_CC0";
    }
    return "REAL_GENOS_HUMAN";
}

inline bool isCriticalTrackKey(std::string_view key) noexcept {
    return key == "drum" || key == "bass" || key == "acc1";
}

inline int parseDimensionIndexKey(std::string_view k) noexcept {
    if (k.size() < 3) return -1;
    if ((k[0] != 'D' && k[0] != 'd') || !std::isdigit(static_cast<unsigned char>(k[1])) ||
        !std::isdigit(static_cast<unsigned char>(k[2]))) {
        return -1;
    }
    const int num = (k[1] - '0') * 10 + (k[2] - '0');
    if (num < 1 || num > 12) return -1;
    if (k.size() == 3) return num - 1;
    if (k[3] == '_') {
        const char* expected = musicalDimensionKey(static_cast<MusicalDimensionId>(num));
        if (expected && k == expected) return num - 1;
    }
    return -1;
}

class StrictJsonReader {
public:
    explicit StrictJsonReader(std::string_view text) noexcept : text_(text) {}

    void skipWs() noexcept {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool eof() const noexcept { return pos_ >= text_.size(); }

    char peek() const noexcept { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    bool consume(char expected) noexcept {
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool parseString(std::string& out, std::string& err) {
        skipWs();
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            err = "expected JSON string";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            const unsigned char uc = static_cast<unsigned char>(c);
            if (c == '"') return true;
            if (uc < 0x20u) {
                err = "unescaped control character in JSON string";
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                err = "unterminated escape sequence in JSON string";
                return false;
            }
            const char esc = text_[pos_++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4u > text_.size()) {
                        err = "truncated \\uXXXX escape in JSON string";
                        return false;
                    }
                    uint32_t cp = 0;
                    for (size_t i = 0; i < 4u; ++i) {
                        const char h = text_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(10 + (h - 'a'));
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(10 + (h - 'A'));
                        else {
                            err = "invalid hex digit in \\uXXXX escape";
                            return false;
                        }
                    }
                    if (cp < 0x80u) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800u) {
                        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                    } else {
                        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                    }
                    break;
                }
                default:
                    err = "invalid escape character in JSON string";
                    return false;
            }
        }
        err = "unterminated JSON string";
        return false;
    }

    bool parseBool(bool& out, std::string& err) {
        skipWs();
        if (text_.substr(pos_, 4u) == "true") {
            pos_ += 4u;
            if (!atDelimiter()) { err = "invalid boolean literal"; return false; }
            out = true;
            return true;
        }
        if (text_.substr(pos_, 5u) == "false") {
            pos_ += 5u;
            if (!atDelimiter()) { err = "invalid boolean literal"; return false; }
            out = false;
            return true;
        }
        err = "expected JSON boolean (true/false)";
        return false;
    }

    bool parseNull() noexcept {
        skipWs();
        if (text_.substr(pos_, 4u) == "null") {
            pos_ += 4u;
            if (atDelimiter()) return true;
            pos_ -= 4u;
        }
        return false;
    }

    bool parseInt64(int64_t& out, std::string& err) {
        skipWs();
        if (pos_ >= text_.size()) {
            err = "expected JSON integer";
            return false;
        }
        const size_t start = pos_;
        bool neg = false;
        if (text_[pos_] == '-') {
            neg = true;
            ++pos_;
        }
        if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            pos_ = start;
            err = "expected JSON integer";
            return false;
        }
        if (text_[pos_] == '0' && pos_ + 1u < text_.size() &&
            std::isdigit(static_cast<unsigned char>(text_[pos_ + 1u]))) {
            err = "leading zero in JSON integer";
            return false;
        }
        uint64_t acc = 0;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            const uint64_t digit = static_cast<uint64_t>(text_[pos_] - '0');
            if (acc > (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - digit) / 10u) {
                err = "integer overflow in JSON";
                return false;
            }
            acc = acc * 10u + digit;
            ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
            err = "expected integer value, got floating-point number";
            return false;
        }
        if (!atDelimiter()) {
            err = "invalid trailing characters in JSON integer";
            return false;
        }
        out = neg ? -static_cast<int64_t>(acc) : static_cast<int64_t>(acc);
        return true;
    }

    bool parseNumberToken(std::string& err) {
        skipWs();
        if (pos_ >= text_.size()) { err = "expected JSON number"; return false; }
        if (text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            err = "expected JSON number";
            return false;
        }
        if (text_[pos_] == '0') {
            ++pos_;
            if (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                err = "leading zero in JSON number";
                return false;
            }
        } else {
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                err = "expected digit after decimal point in JSON number";
                return false;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                err = "expected digit in JSON number exponent";
                return false;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (!atDelimiter()) {
            err = "invalid trailing characters in JSON number";
            return false;
        }
        return true;
    }

    bool skipValue(std::string& err, int depth = 0) {
        if (depth > 16) {
            err = "JSON nesting depth exceeds safety limit";
            return false;
        }
        skipWs();
        if (pos_ >= text_.size()) { err = "unexpected end of JSON value"; return false; }
        const char c = text_[pos_];
        if (c == '"') {
            std::string tmp;
            return parseString(tmp, err);
        }
        if (c == 't' || c == 'f') {
            bool b = false;
            return parseBool(b, err);
        }
        if (c == 'n') {
            if (parseNull()) return true;
            err = "invalid literal in JSON";
            return false;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parseNumberToken(err);
        }
        if (c == '[') {
            ++pos_;
            skipWs();
            if (consume(']')) return true;
            while (true) {
                if (!skipValue(err, depth + 1)) return false;
                if (consume(',')) continue;
                if (consume(']')) return true;
                err = "expected ',' or ']' in JSON array";
                return false;
            }
        }
        if (c == '{') {
            ++pos_;
            skipWs();
            if (consume('}')) return true;
            std::set<std::string> seen_obj_keys;
            while (true) {
                std::string k;
                if (!parseString(k, err)) return false;
                if (!seen_obj_keys.insert(k).second) {
                    err = "duplicate key '" + k + "' in nested JSON object";
                    return false;
                }
                if (!consume(':')) { err = "expected ':' in nested JSON object"; return false; }
                if (!skipValue(err, depth + 1)) return false;
                if (consume(',')) continue;
                if (consume('}')) return true;
                err = "expected ',' or '}' in nested JSON object";
                return false;
            }
        }
        err = "unexpected character in JSON value";
        return false;
    }

private:
    bool atDelimiter() const noexcept {
        if (pos_ >= text_.size()) return true;
        const char c = text_[pos_];
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
               c == ',' || c == '}' || c == ']';
    }

    std::string_view text_;
    size_t pos_ = 0;
};

} // namespace acceptance_gate_detail

inline HumanListeningParseResult parseHumanListeningRecordJson(std::string_view json) {
    using namespace acceptance_gate_detail;
    HumanListeningParseResult res{};

    try {
        if (json.size() > 262144u) {
            res.error = "human listening record JSON exceeds 256 KiB safety limit";
            return res;
        }

        StrictJsonReader r(json);
        if (!r.consume('{')) {
            res.error = "human listening record must be a JSON object";
            return res;
        }

        HumanListeningRecord rec{};
        std::set<std::string> seen_fields;
        bool has_evaluator = false;
        bool has_overall_rating = false;
        bool is_template_placeholder = false;
        bool is_synthetic_self_test = false;
        std::string listening_environment;

        r.skipWs();
        if (r.peek() == '}') {
            res.error = "human listening record JSON object is empty";
            return res;
        }

        while (true) {
            std::string key;
            std::string err;
            if (!r.parseString(key, err)) {
                res.error = "listening record key parse error: " + err;
                return res;
            }
            if (!r.consume(':')) {
                res.error = "expected ':' after key '" + key + "'";
                return res;
            }

            // Normalize canonical camelCase & P3-M5 snake_case field aliases.
            // Mapping both spellings to one canonical key guarantees duplicate/conflicting
            // camelCase + snake_case combinations are rejected fail-closed.
            std::string canonical = key;
            if (key == "evaluator_id") canonical = "evaluatorId";
            else if (key == "overall_rating_1_to_5") canonical = "overallRating";
            else if (key == "per_dimension_ratings") canonical = "perDimensionRatings";
            else if (key == "critical_defect_count") canonical = "criticalDefectCount";
            else if (key == "minor_defect_count") canonical = "minorDefectCount";
            else if (key == "hardware_verified") canonical = "hardwareVerified";
            else if (key == "hardware_model") canonical = "hardwareModel";
            else if (key == "reference_type") canonical = "referenceType";
            else if (key == "major_correction_needed") canonical = "majorCorrectionNeeded";
            else if (key == "required_sections_verified") canonical = "requiredSectionsVerified";
            else if (key == "required_chords_verified") canonical = "requiredChordsVerified";
            else if (key == "template_placeholder" || key == "is_template" || key == "isTemplate") {
                canonical = "templatePlaceholder";
            } else if (key == "evaluator_comments" || key == "evaluatorComments") {
                canonical = "notes";
            } else if (key == "style_filename" || key == "styleFilename") {
                canonical = "styleName";
            } else if (key == "candidate_label" || key == "candidateLabel") {
                canonical = "candidate";
            } else if (key == "style_sha256" || key == "styleSha256" || key == "converted_mid_sha256") {
                canonical = "convertedMidSha256";
            } else if (key == "session_date" || key == "sessionDate") {
                canonical = "date";
            } else if (key == "record_id" || key == "recordId") {
                canonical = "recordId";
            } else if (key == "style_id" || key == "styleId") {
                canonical = "styleId";
            } else if (key == "source_class" || key == "sourceClass") {
                canonical = "sourceClass";
            } else if (key == "blind_session" || key == "blindSession") {
                canonical = "blindSession";
            } else if (key == "listening_environment" || key == "listeningEnvironment") {
                canonical = "listeningEnvironment";
            } else if (key == "analyzer_version" || key == "analyzerVersion") {
                canonical = "analyzerVersion";
            } else if (key == "defect_annotations" || key == "defectAnnotations") {
                canonical = "defectAnnotations";
            } else if (key == "synthetic_self_test" || key == "syntheticSelfTest") {
                canonical = "syntheticSelfTest";
            }

            if (!seen_fields.insert(canonical).second) {
                res.error = "duplicate field '" + canonical + "' in human listening record";
                return res;
            }

            if (canonical == "schema") {
                std::string s;
                if (!r.parseString(s, err)) { res.error = "field 'schema': " + err; return res; }
                if (s != "aiarranger-human-listening-record/1") {
                    res.error = "unsupported listening record schema '" + s +
                                "' (expected 'aiarranger-human-listening-record/1')";
                    return res;
                }
            } else if (canonical == "styleName" || canonical == "candidate" ||
                       canonical == "convertedMidSha256" || canonical == "date" ||
                       canonical == "recordId" || canonical == "styleId" ||
                       canonical == "genre" || canonical == "sourceClass" ||
                       canonical == "analyzerVersion") {
                if (!r.parseNull()) {
                    std::string ignored_meta;
                    if (!r.parseString(ignored_meta, err)) {
                        res.error = "field '" + key + "': " + err;
                        return res;
                    }
                }
            } else if (canonical == "listeningEnvironment") {
                if (!r.parseNull()) {
                    if (!r.parseString(listening_environment, err)) {
                        res.error = "field '" + key + "': " + err;
                        return res;
                    }
                    if (hasControlChar(listening_environment) || listening_environment.size() > 256u) {
                        res.error = "field '" + key + "' must be single-line (<= 256 chars)";
                        return res;
                    }
                }
            } else if (canonical == "blindSession") {
                if (!r.parseNull()) {
                    bool ignored_bool = false;
                    if (!r.parseBool(ignored_bool, err)) {
                        res.error = "field '" + key + "': " + err;
                        return res;
                    }
                }
            } else if (canonical == "syntheticSelfTest") {
                if (!r.parseBool(is_synthetic_self_test, err)) {
                    res.error = "field '" + key + "': " + err;
                    return res;
                }
            } else if (canonical == "defectAnnotations") {
                r.skipWs();
                if (r.parseNull()) {
                    // Explicitly omitted.
                } else if (r.consume('[')) {
                    r.skipWs();
                    if (!r.consume(']')) {
                        while (true) {
                            r.skipWs();
                            if (r.peek() != '{') {
                                res.error = "field '" + key + "' entries must be JSON objects";
                                return res;
                            }
                            if (!r.skipValue(err, 1)) {
                                res.error = "field '" + key + "': " + err;
                                return res;
                            }
                            if (r.consume(',')) continue;
                            if (r.consume(']')) break;
                            res.error = "expected ',' or ']' in '" + key + "' array";
                            return res;
                        }
                    }
                } else {
                    res.error = "field '" + key + "' must be null or a JSON array";
                    return res;
                }
            } else if (canonical == "templatePlaceholder") {
                if (!r.parseBool(is_template_placeholder, err)) {
                    res.error = "field '" + key + "': " + err;
                    return res;
                }
            } else if (canonical == "evaluatorId") {
                if (!r.parseString(rec.evaluator_id, err)) {
                    res.error = "field 'evaluatorId': " + err;
                    return res;
                }
                if (isBlankAscii(rec.evaluator_id) || hasControlChar(rec.evaluator_id) ||
                    rec.evaluator_id.size() > 128u) {
                    res.error = "field 'evaluatorId' must be a non-empty single-line tag (<= 128 chars)";
                    return res;
                }
                has_evaluator = true;
            } else if (canonical == "overallRating") {
                if (!r.parseNull()) {
                    int64_t val = 0;
                    if (!r.parseInt64(val, err)) {
                        res.error = "field 'overallRating': " + err;
                        return res;
                    }
                    if (val < 1 || val > 5) {
                        res.error = "field 'overallRating' out of range (must be integer 1..5)";
                        return res;
                    }
                    rec.overall_rating_1_to_5 = static_cast<int>(val);
                    has_overall_rating = true;
                }
            } else if (canonical == "criticalDefectCount") {
                int64_t val = 0;
                if (!r.parseInt64(val, err)) {
                    res.error = "field 'criticalDefectCount': " + err;
                    return res;
                }
                if (val < 0 || val > 100000) {
                    res.error = "field 'criticalDefectCount' out of range (must be 0..100000)";
                    return res;
                }
                rec.critical_defect_count = static_cast<uint32_t>(val);
            } else if (canonical == "minorDefectCount") {
                int64_t val = 0;
                if (!r.parseInt64(val, err)) {
                    res.error = "field 'minorDefectCount': " + err;
                    return res;
                }
                if (val < 0 || val > 100000) {
                    res.error = "field 'minorDefectCount' out of range (must be 0..100000)";
                    return res;
                }
                rec.minor_defect_count = static_cast<uint32_t>(val);
            } else if (canonical == "hardwareVerified") {
                if (!r.parseBool(rec.hardware_verified, err)) {
                    res.error = "field 'hardwareVerified': " + err;
                    return res;
                }
            } else if (canonical == "hardwareModel") {
                if (!r.parseString(rec.hardware_model, err)) {
                    res.error = "field 'hardwareModel': " + err;
                    return res;
                }
                if (hasControlChar(rec.hardware_model) || rec.hardware_model.size() > 128u) {
                    res.error = "field 'hardwareModel' must be single-line (<= 128 chars)";
                    return res;
                }
            } else if (canonical == "referenceType") {
                if (!r.parseString(rec.reference_type, err)) {
                    res.error = "field 'referenceType': " + err;
                    return res;
                }
                if (hasControlChar(rec.reference_type) || rec.reference_type.size() > 128u) {
                    res.error = "field 'referenceType' must be single-line (<= 128 chars)";
                    return res;
                }
            } else if (canonical == "majorCorrectionNeeded") {
                if (!r.parseBool(rec.major_correction_needed, err)) {
                    res.error = "field 'majorCorrectionNeeded': " + err;
                    return res;
                }
            } else if (canonical == "requiredSectionsVerified") {
                if (!r.parseBool(rec.required_sections_verified, err)) {
                    res.error = "field 'requiredSectionsVerified': " + err;
                    return res;
                }
            } else if (canonical == "requiredChordsVerified") {
                if (!r.parseBool(rec.required_chords_verified, err)) {
                    res.error = "field 'requiredChordsVerified': " + err;
                    return res;
                }
            } else if (canonical == "notes") {
                if (!r.parseString(rec.notes, err)) {
                    res.error = "field 'notes': " + err;
                    return res;
                }
                if (rec.notes.size() > 8192u) {
                    res.error = "field 'notes' exceeds 8192-byte limit";
                    return res;
                }
            } else if (canonical == "perDimensionRatings") {
                r.skipWs();
                if (r.parseNull()) {
                    // Explicitly omitted.
                } else if (r.consume('[')) {
                    size_t idx = 0;
                    r.skipWs();
                    if (r.peek() == ']') {
                        res.error = "field 'perDimensionRatings' array must have exactly 12 entries";
                        return res;
                    }
                    while (true) {
                        if (idx >= 12u) {
                            res.error = "field 'perDimensionRatings' array exceeds 12 entries";
                            return res;
                        }
                        if (r.parseNull()) {
                            rec.per_dimension_present[idx] = false;
                            rec.per_dimension_ratings[idx] = 0;
                        } else {
                            int64_t dv = 0;
                            if (!r.parseInt64(dv, err)) {
                                res.error = "field 'perDimensionRatings[" + std::to_string(idx) + "]': " + err;
                                return res;
                            }
                            if (dv < 0 || dv > 5) {
                                res.error = "field 'perDimensionRatings[" + std::to_string(idx) +
                                            "]' out of range (must be 0/null or 1..5)";
                                return res;
                            }
                            rec.per_dimension_present[idx] = (dv > 0);
                            rec.per_dimension_ratings[idx] = static_cast<int>(dv);
                        }
                        ++idx;
                        if (r.consume(',')) continue;
                        if (r.consume(']')) break;
                        res.error = "expected ',' or ']' in 'perDimensionRatings' array";
                        return res;
                    }
                    if (idx != 12u) {
                        res.error = "field 'perDimensionRatings' array must have exactly 12 entries (got " +
                                    std::to_string(idx) + ")";
                        return res;
                    }
                } else if (r.consume('{')) {
                    r.skipWs();
                    if (!r.consume('}')) {
                        std::array<bool, 12> seen_dim{};
                        while (true) {
                            std::string dk;
                            if (!r.parseString(dk, err)) {
                                res.error = "field 'perDimensionRatings' key error: " + err;
                                return res;
                            }
                            const int dim_idx = parseDimensionIndexKey(dk);
                            if (dim_idx < 0 || dim_idx >= 12) {
                                res.error = "unknown dimension key '" + dk + "' in 'perDimensionRatings'";
                                return res;
                            }
                            if (seen_dim[static_cast<size_t>(dim_idx)]) {
                                res.error = "duplicate dimension key '" + dk +
                                            "' in 'perDimensionRatings'";
                                return res;
                            }
                            seen_dim[static_cast<size_t>(dim_idx)] = true;
                            if (!r.consume(':')) {
                                res.error = "expected ':' after dimension key '" + dk + "'";
                                return res;
                            }
                            if (r.parseNull()) {
                                rec.per_dimension_present[static_cast<size_t>(dim_idx)] = false;
                                rec.per_dimension_ratings[static_cast<size_t>(dim_idx)] = 0;
                            } else {
                                int64_t dv = 0;
                                if (!r.parseInt64(dv, err)) {
                                    res.error = "field 'perDimensionRatings." + dk + "': " + err;
                                    return res;
                                }
                                if (dv < 0 || dv > 5) {
                                    res.error = "field 'perDimensionRatings." + dk +
                                                "' out of range (must be 0/null or 1..5)";
                                    return res;
                                }
                                rec.per_dimension_present[static_cast<size_t>(dim_idx)] = (dv > 0);
                                rec.per_dimension_ratings[static_cast<size_t>(dim_idx)] = static_cast<int>(dv);
                            }
                            if (r.consume(',')) continue;
                            if (r.consume('}')) break;
                            res.error = "expected ',' or '}' in 'perDimensionRatings' object";
                            return res;
                        }
                    }
                } else {
                    res.error = "field 'perDimensionRatings' must be null, a 12-element array, or an object";
                    return res;
                }
            } else {
                res.error = "unknown field '" + key + "' in human listening record";
                return res;
            }

            if (r.consume(',')) continue;
            if (r.consume('}')) break;
            res.error = "expected ',' or '}' in human listening record JSON";
            return res;
        }

        r.skipWs();
        if (!r.eof()) {
            res.error = "unexpected trailing characters after JSON object";
            return res;
        }
        if (is_template_placeholder) {
            res.error = "listening record is still an unfilled template "
                        "(complete authorized audition and set is_template to false)";
            return res;
        }
        if (is_synthetic_self_test) {
            res.error = "rejected synthetic_self_test record (not real human listening data)";
            return res;
        }
        if (!has_evaluator) {
            res.error = "missing required field 'evaluatorId'";
            return res;
        }
        if (!has_overall_rating) {
            res.error = "missing required field 'overallRating' (integer 1..5)";
            return res;
        }
        if (rec.hardware_verified && isBlankAscii(rec.hardware_model)) {
            if (!isPlaceholderText(listening_environment)) {
                rec.hardware_model = listening_environment;
            } else {
                res.error = "hardwareVerified=true requires non-empty 'hardwareModel' (or non-placeholder 'listening_environment')";
                return res;
            }
        }

        rec.present = true;
        res.record = std::move(rec);
        res.ok = true;
        return res;
    } catch (...) {
        res.ok = false;
        res.record = HumanListeningRecord{};
        res.error = "unexpected error while parsing human listening record JSON";
        return res;
    }
}

inline OfflineAcceptanceEvidence buildOfflineAcceptanceEvidence(
    const std::string& style_name,
    const ConversionManifest& manifest,
    const MusicalQualityReport* analyzer_report = nullptr,
    bool is_enhanced_candidate = false,
    uint32_t extra_hard_invariant_violations = 0) {
    using namespace acceptance_gate_detail;
    OfflineAcceptanceEvidence ev{};
    ev.style_name = style_name;
    ev.is_enhanced_candidate = is_enhanced_candidate;
    ev.candidate_label = is_enhanced_candidate
                             ? "Candidate C (Mode B Recommended Enhancement)"
                             : "Candidate B (Mode A Faithful)";
    ev.has_fatal_conversion_error = !manifest.ok;
    ev.fatal_error_message = manifest.error;
    ev.hard_invariant_violations = extra_hard_invariant_violations;

    for (const auto& tr : manifest.track_resolutions) {
        const bool resolved =
            (tr.status == "USER_OVERRIDE" || tr.status == "EXACT" || tr.status == "APPROX");
        if (!resolved) {
            ++ev.unresolved_total_sounds;
            if (isCriticalTrackKey(tr.track_key)) {
                ++ev.unresolved_critical_sounds;
                ev.unresolved_critical_tracks.push_back(tr.track_key + " (" + tr.status + ")");
            } else {
                ev.unresolved_non_critical_tracks.push_back(tr.track_key + " (" + tr.status + ")");
            }
        }
    }

    if (analyzer_report != nullptr) {
        ev.has_analyzer_summary = true;
        ev.overall_musical_score = sanitizeScore(analyzer_report->overall_musical_score);
        for (size_t i = 0; i < 12u; ++i) {
            ev.dimension_scores[i] = sanitizeScore(analyzer_report->dimensions[i].score);
            ev.dimension_applicable[i] = analyzer_report->dimensions[i].applicable;
        }
        for (const auto& f : analyzer_report->all_findings) {
            if (f.severity == MusicalFindingSeverity::Critical) ++ev.analyzer_critical_findings;
            else if (f.severity == MusicalFindingSeverity::Warning) ++ev.analyzer_warning_findings;
            else if (f.severity == MusicalFindingSeverity::Advisory) ++ev.analyzer_advisory_findings;
        }
    }
    return ev;
}

inline AcceptanceReport evaluateAcceptanceGate(
    const OfflineAcceptanceEvidence& offline_in,
    const HumanListeningRecord& listening_in = HumanListeningRecord{}) noexcept {
    AcceptanceReport report{};
    try {
        using namespace acceptance_gate_detail;

        OfflineAcceptanceEvidence offline = offline_in;
        offline.overall_musical_score = sanitizeScore(offline.overall_musical_score);
        for (size_t i = 0; i < 12u; ++i) {
            offline.dimension_scores[i] = sanitizeScore(offline.dimension_scores[i]);
        }

        HumanListeningRecord listening = listening_in;
        const bool valid_listening_rating =
            listening.present &&
            listening.overall_rating_1_to_5 >= 1 &&
            listening.overall_rating_1_to_5 <= 5;
        const bool invalid_listening_supplied = listening.present && !valid_listening_rating;
        const bool hw_verified = valid_listening_rating && listening.hardware_verified;

        report.style_name = offline.style_name;
        report.candidate_label = offline.candidate_label.empty()
                                     ? (offline.is_enhanced_candidate
                                            ? "Candidate C (Mode B Recommended Enhancement)"
                                            : "Candidate B (Mode A Faithful)")
                                     : offline.candidate_label;
        report.is_enhanced_candidate = offline.is_enhanced_candidate;
        report.offline = offline;
        report.listening = listening;
        report.hardware_verified = hw_verified;
        report.l3_status = hw_verified ? "VERIFIED" : "NOT_TESTED";
        report.independent_from_frozen_q1 = true;

        // ── 1. Grade D (Rejected) ───────────────────────────────────────
        bool grade_d = false;
        if (offline.has_fatal_conversion_error) {
            grade_d = true;
            report.gate_reasons.push_back(
                offline.fatal_error_message.empty()
                    ? "Fatal conversion or SMF validation error."
                    : ("Fatal conversion or SMF validation error: " + offline.fatal_error_message));
        }
        if (offline.hard_invariant_violations > 0u) {
            grade_d = true;
            report.gate_reasons.push_back(
                "Hard structural/safety invariant violations detected (" +
                std::to_string(offline.hard_invariant_violations) + " > 0).");
        }
        if (valid_listening_rating && listening.overall_rating_1_to_5 == 1) {
            grade_d = true;
            report.gate_reasons.push_back(
                "Human listening evaluation rated overall musical quality 1/5 (Rejected).");
        }
        if (valid_listening_rating && listening.critical_defect_count > 0u) {
            grade_d = true;
            report.gate_reasons.push_back(
                "Human listening evaluation recorded " +
                std::to_string(listening.critical_defect_count) + " critical musical defect(s).");
        }

        if (grade_d) {
            report.grade = AcceptanceGrade::D;
            report.required_next_actions.push_back(
                "Resolve all fatal conversion errors, hard invariant violations, and critical listening defects before re-testing.");
        }
        // ── 2. Grade C (Manual Correction Required) ─────────────────────
        else if (offline.unresolved_critical_sounds > 0u ||
                 (valid_listening_rating &&
                  (listening.overall_rating_1_to_5 == 2 ||
                   (listening.overall_rating_1_to_5 == 3 && listening.major_correction_needed)))) {
            report.grade = AcceptanceGrade::C;
            if (offline.unresolved_critical_sounds > 0u) {
                report.gate_reasons.push_back(
                    std::to_string(offline.unresolved_critical_sounds) +
                    " critical sound track(s) remain unresolved/unsupported (drum/bass/acc1).");
                report.required_next_actions.push_back(
                    "Inspect unresolved_sounds.md, fill verified Pa5X Bank/PC entries in a .soundmap file, and re-run with --sound-map.");
            }
            if (valid_listening_rating && listening.overall_rating_1_to_5 == 2) {
                report.gate_reasons.push_back(
                    "Human listening evaluation rated overall musical quality 2/5 (Manual Correction Required).");
            }
            if (valid_listening_rating && listening.overall_rating_1_to_5 == 3 &&
                listening.major_correction_needed) {
                report.gate_reasons.push_back(
                    "Human listening evaluation rated 3/5 with majorCorrectionNeeded=true.");
            }
            if (!hw_verified) {
                report.required_next_actions.push_back(
                    "After manual correction, complete owner-authorized Pa5X hardware listening and attach --listening-record.");
            }
        }
        // ── 3. Grade U (Unverified) — STRICT EPISTEMIC BOUNDARY ─────────
        else if (!valid_listening_rating || !listening.hardware_verified) {
            report.grade = AcceptanceGrade::U;
            if (invalid_listening_supplied) {
                report.gate_reasons.push_back(
                    "Supplied human listening record has invalid overall_rating_1_to_5 (must be 1..5); cannot verify.");
            } else if (!listening.present) {
                report.gate_reasons.push_back(
                    "Offline structural checks passed with 0 hard violations and 0 critical unresolved sounds, "
                    "but no human listening record was supplied (offline PASS alone is NEVER Grade A).");
            } else {
                report.gate_reasons.push_back(
                    "Human listening record present, but hardwareVerified=false "
                    "(Pa5X hardware verification is mandatory for Grade A/B).");
            }
            if (offline.unresolved_total_sounds > 0u) {
                report.gate_reasons.push_back(
                    std::to_string(offline.unresolved_total_sounds) +
                    " non-critical sound track(s) remain unresolved/unsupported.");
            }
            report.required_next_actions.push_back(
                "Review style_edit_sheet.md and hardware_session_staging/OPERATOR_PREP_NOTES.md.");
            report.required_next_actions.push_back(
                "Perform owner-authorized Pa5X hardware listening, complete LISTENING_RECORD_TEMPLATE.json, "
                "and re-run with --listening-record <path.json>.");
        }
        // ── 4. Grade B (Usable With Minor Refinement) ───────────────────
        else if (listening.overall_rating_1_to_5 == 3 ||
                 listening.minor_defect_count > 0u ||
                 offline.unresolved_total_sounds > 0u ||
                 !listening.required_sections_verified ||
                 !listening.required_chords_verified) {
            report.grade = AcceptanceGrade::B;
            if (listening.overall_rating_1_to_5 == 3) {
                report.gate_reasons.push_back(
                    "Hardware-verified listening passed with overall rating 3/5 and 0 critical defects.");
            }
            if (listening.minor_defect_count > 0u) {
                report.gate_reasons.push_back(
                    "Hardware-verified listening recorded " +
                    std::to_string(listening.minor_defect_count) + " minor defect(s).");
            }
            if (offline.unresolved_total_sounds > 0u) {
                report.gate_reasons.push_back(
                    std::to_string(offline.unresolved_total_sounds) +
                    " non-critical sound track(s) remain unresolved/unsupported.");
            }
            if (!listening.required_sections_verified) {
                report.gate_reasons.push_back(
                    "Required style sections (Intros/Variations/Fills/Endings) were not all marked verified.");
            }
            if (!listening.required_chords_verified) {
                report.gate_reasons.push_back(
                    "Required chord-type audition matrix (Maj/Min/7th/Dim/Sus4) was not marked verified.");
            }
            report.required_next_actions.push_back(
                "Apply minor sound/EQ/voicing refinements in Pa5X Style Edit or via .soundmap / Mode B, then re-audition for Grade A.");
        }
        // ── 5. Grade A (Performance Ready) ──────────────────────────────
        else {
            report.grade = AcceptanceGrade::A;
            report.gate_reasons.push_back(
                "Hardware-verified on " +
                (isBlankAscii(listening.hardware_model) ? std::string("verified Pa5X hardware") : listening.hardware_model) +
                " with overall rating " + std::to_string(listening.overall_rating_1_to_5) +
                "/5, 0 critical defects, 0 minor defects, 0 hard invariant violations, and 0 unresolved sounds.");
            report.required_next_actions.push_back(
                "Ready for musician performance archive (preserve manifest.json, style_edit_sheet.md, and listening record for provenance).");
        }

        if (offline.has_analyzer_summary &&
            (offline.analyzer_critical_findings > 0u || offline.analyzer_warning_findings > 0u)) {
            report.gate_reasons.push_back(
                "Offline 12-dimension analyzer summary: score=" +
                formatDouble2(offline.overall_musical_score) +
                ", criticalFindings=" + std::to_string(offline.analyzer_critical_findings) +
                ", warningFindings=" + std::to_string(offline.analyzer_warning_findings) +
                ", advisoryFindings=" + std::to_string(offline.analyzer_advisory_findings) + ".");
        }

        report.grade_code = acceptanceGradeCode(report.grade);
        report.grade_label = acceptanceGradeLabel(report.grade);
    } catch (...) {
        report.grade = AcceptanceGrade::D;
        report.grade_code = "D";
        report.grade_label = "Rejected";
        report.hardware_verified = false;
        report.l3_status = "NOT_TESTED";
        report.gate_reasons = {"Unexpected error during acceptance gate evaluation."};
    }
    return report;
}

inline std::string formatAcceptanceReportJson(const AcceptanceReport& r) {
    using namespace acceptance_gate_detail;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"" << jsonEscape(r.schema) << "\",\n";
    o << "  \"criteriaStatus\": \"" << jsonEscape(r.criteria_status) << "\",\n";
    o << "  \"styleName\": \"" << jsonEscape(r.style_name) << "\",\n";
    o << "  \"candidateLabel\": \"" << jsonEscape(r.candidate_label) << "\",\n";
    o << "  \"isEnhancedCandidate\": " << (r.is_enhanced_candidate ? "true" : "false") << ",\n";
    o << "  \"grade\": \"" << jsonEscape(r.grade_code) << "\",\n";
    o << "  \"gradeLabel\": \"" << jsonEscape(r.grade_label) << "\",\n";
    o << "  \"gradeDisplay\": \"" << jsonEscape(acceptanceGradeDisplay(r.grade)) << "\",\n";
    o << "  \"hardwareVerified\": " << (r.hardware_verified ? "true" : "false") << ",\n";
    o << "  \"l3Status\": \"" << jsonEscape(r.l3_status) << "\",\n";
    o << "  \"independentFromFrozenQ1\": " << (r.independent_from_frozen_q1 ? "true" : "false") << ",\n";

    o << "  \"offlineEvidence\": {\n";
    o << "    \"hasFatalConversionError\": " << (r.offline.has_fatal_conversion_error ? "true" : "false") << ",\n";
    o << "    \"fatalErrorMessage\": \"" << jsonEscape(r.offline.fatal_error_message) << "\",\n";
    o << "    \"hardInvariantViolations\": " << r.offline.hard_invariant_violations << ",\n";
    o << "    \"unresolvedCriticalSounds\": " << r.offline.unresolved_critical_sounds << ",\n";
    o << "    \"unresolvedTotalSounds\": " << r.offline.unresolved_total_sounds << ",\n";
    o << "    \"unresolvedCriticalTracks\": [";
    for (size_t i = 0; i < r.offline.unresolved_critical_tracks.size(); ++i) {
        if (i > 0u) o << ", ";
        o << "\"" << jsonEscape(r.offline.unresolved_critical_tracks[i]) << "\"";
    }
    o << "],\n";
    o << "    \"unresolvedNonCriticalTracks\": [";
    for (size_t i = 0; i < r.offline.unresolved_non_critical_tracks.size(); ++i) {
        if (i > 0u) o << ", ";
        o << "\"" << jsonEscape(r.offline.unresolved_non_critical_tracks[i]) << "\"";
    }
    o << "],\n";
    o << "    \"hasAnalyzerSummary\": " << (r.offline.has_analyzer_summary ? "true" : "false") << ",\n";
    o << "    \"overallMusicalScore\": " << formatDouble2(r.offline.overall_musical_score) << ",\n";
    o << "    \"analyzerCriticalFindings\": " << r.offline.analyzer_critical_findings << ",\n";
    o << "    \"analyzerWarningFindings\": " << r.offline.analyzer_warning_findings << ",\n";
    o << "    \"analyzerAdvisoryFindings\": " << r.offline.analyzer_advisory_findings << "\n";
    o << "  },\n";

    o << "  \"humanListening\": {\n";
    o << "    \"present\": " << (r.listening.present ? "true" : "false") << ",\n";
    o << "    \"evaluatorId\": \"" << jsonEscape(r.listening.evaluator_id) << "\",\n";
    o << "    \"overallRating1To5\": " << r.listening.overall_rating_1_to_5 << ",\n";
    o << "    \"perDimensionRatings\": [";
    for (size_t i = 0; i < 12u; ++i) {
        if (i > 0u) o << ", ";
        if (r.listening.per_dimension_present[i]) o << r.listening.per_dimension_ratings[i];
        else o << "null";
    }
    o << "],\n";
    o << "    \"criticalDefectCount\": " << r.listening.critical_defect_count << ",\n";
    o << "    \"minorDefectCount\": " << r.listening.minor_defect_count << ",\n";
    o << "    \"hardwareVerified\": " << (r.listening.hardware_verified ? "true" : "false") << ",\n";
    o << "    \"hardwareModel\": \"" << jsonEscape(r.listening.hardware_model) << "\",\n";
    o << "    \"referenceType\": \"" << jsonEscape(r.listening.reference_type) << "\",\n";
    o << "    \"majorCorrectionNeeded\": " << (r.listening.major_correction_needed ? "true" : "false") << ",\n";
    o << "    \"requiredSectionsVerified\": " << (r.listening.required_sections_verified ? "true" : "false") << ",\n";
    o << "    \"requiredChordsVerified\": " << (r.listening.required_chords_verified ? "true" : "false") << ",\n";
    o << "    \"notes\": \"" << jsonEscape(r.listening.notes) << "\"\n";
    o << "  },\n";

    o << "  \"gateReasons\": [";
    if (!r.gate_reasons.empty()) o << "\n";
    for (size_t i = 0; i < r.gate_reasons.size(); ++i) {
        o << "    \"" << jsonEscape(r.gate_reasons[i]) << "\""
          << (i + 1u < r.gate_reasons.size() ? "," : "") << "\n";
    }
    o << (r.gate_reasons.empty() ? "],\n" : "  ],\n");

    o << "  \"requiredNextActions\": [";
    if (!r.required_next_actions.empty()) o << "\n";
    for (size_t i = 0; i < r.required_next_actions.size(); ++i) {
        o << "    \"" << jsonEscape(r.required_next_actions[i]) << "\""
          << (i + 1u < r.required_next_actions.size() ? "," : "") << "\n";
    }
    o << (r.required_next_actions.empty() ? "]\n" : "  ]\n");
    o << "}\n";
    return o.str();
}

inline std::string formatAcceptanceReportMd(const AcceptanceReport& r) {
    using namespace acceptance_gate_detail;
    std::ostringstream o;
    o << "# Proposed Production Acceptance Report: " << mdEscape(r.style_name) << "\n\n";
    o << "- **Criteria Status:** `" << mdEscape(r.criteria_status) << "`\n";
    o << "- **Candidate:** `" << mdEscape(r.candidate_label) << "`\n";
    o << "- **Assigned Grade:** **`" << acceptanceGradeDisplay(r.grade) << "`**\n";
    o << "- **Epistemic Boundary:** `hardwareVerified: " << (r.hardware_verified ? "true" : "false")
      << "` | `l3Status: " << mdEscape(r.l3_status)
      << "` | `independentFromFrozenQ1: true`\n";
    o << "- **Rule:** Offline structural/analyzer PASS alone **NEVER** yields `Grade A`; hardware listening verification is mandatory for `Grade A` or `Grade B`.\n\n";

    o << "## Offline Conversion & Analyzer Evidence\n\n";
    o << "| Metric | Value |\n";
    o << "|---|---|\n";
    o << "| Fatal Conversion / SMF Error | `" << (r.offline.has_fatal_conversion_error ? "YES" : "NO") << "` |\n";
    o << "| Hard Invariant Violations | `" << r.offline.hard_invariant_violations << "` |\n";
    o << "| Unresolved Critical Sounds (`drum`/`bass`/`acc1`) | `" << r.offline.unresolved_critical_sounds << "` |\n";
    o << "| Unresolved Total Sounds | `" << r.offline.unresolved_total_sounds << "` |\n";
    if (r.offline.has_analyzer_summary) {
        o << "| 12-Dimension Musical Quality Score (Offline Diagnostic) | `"
          << formatDouble2(r.offline.overall_musical_score) << " / 100.00` |\n";
        o << "| Analyzer Findings (Critical / Warning / Advisory) | `"
          << r.offline.analyzer_critical_findings << " / "
          << r.offline.analyzer_warning_findings << " / "
          << r.offline.analyzer_advisory_findings << "` |\n";
    }
    o << "\n";

    o << "## Human Listening Evidence\n\n";
    if (!r.listening.present) {
        o << "_Not provided (`present: false`, `hardwareVerified: false`). Complete authorized Pa5X hardware audition and pass `--listening-record <path.json>` to unlock Grade A/B evaluation._\n\n";
    } else {
        o << "| Field | Value |\n";
        o << "|---|---|\n";
        o << "| Evaluator ID | `" << mdEscape(r.listening.evaluator_id) << "` |\n";
        o << "| Overall Rating (1..5) | `" << r.listening.overall_rating_1_to_5 << " / 5` |\n";
        o << "| Hardware Verified | `" << (r.listening.hardware_verified ? "true" : "false") << "` |\n";
        o << "| Hardware Model | `" << mdEscape(r.listening.hardware_model.empty() ? "-" : r.listening.hardware_model) << "` |\n";
        o << "| Reference Type | `" << mdEscape(r.listening.reference_type.empty() ? "-" : r.listening.reference_type) << "` |\n";
        o << "| Critical / Minor Defects | `" << r.listening.critical_defect_count << " / "
          << r.listening.minor_defect_count << "` |\n";
        o << "| Required Sections / Chords Verified | `"
          << (r.listening.required_sections_verified ? "yes" : "no") << " / "
          << (r.listening.required_chords_verified ? "yes" : "no") << "` |\n";
        if (!r.listening.notes.empty()) {
            o << "| Notes | " << mdEscape(r.listening.notes) << " |\n";
        }
        o << "\n";
    }

    o << "## Gate Decision Reasons\n\n";
    for (const auto& reason : r.gate_reasons) {
        o << "- " << mdEscape(reason) << "\n";
    }
    o << "\n## Required Next Actions\n\n";
    for (const auto& action : r.required_next_actions) {
        o << "- " << mdEscape(action) << "\n";
    }
    o << "\n## Proposed Grade Definitions (`" << mdEscape(r.criteria_status) << "`)\n\n";
    o << "| Grade | Label | Requirement Summary |\n";
    o << "|---|---|---|\n";
    o << "| `A` | Performance Ready | `hardwareVerified=true`, human rating `>= 4/5`, `0` critical/minor defects, `0` hard violations, `0` unresolved sounds, required sections & chords verified |\n";
    o << "| `B` | Usable With Minor Refinement | `hardwareVerified=true`, `0` critical defects, rating `3/5` (no major correction) or `>= 4/5` with minor defects/non-critical unresolved sounds |\n";
    o << "| `C` | Manual Correction Required | Unresolved critical sounds (`drum`/`bass`/`acc1`) OR listening rating `2/5` (or `3/5` with `majorCorrectionNeeded=true`) |\n";
    o << "| `D` | Rejected | Fatal conversion error, `hardInvariantViolations > 0`, listening rating `1/5`, or `criticalDefectCount > 0` |\n";
    o << "| `U` | Unverified | No Grade D/C blockers, but missing valid human listening record or `hardwareVerified=false` (offline PASS alone) |\n";
    return o.str();
}

inline std::string formatListeningRecordTemplateJson(
    const std::string& style_name,
    const std::string& candidate_label,
    const std::string& converted_mid_sha256) {
    using namespace acceptance_gate_detail;
    const std::string style_id = stripStyleExtension(style_name);
    const std::string cand = canonicalCandidateLabel(candidate_label);
    const std::string src_class = inferTemplateSourceClass(style_id);
    const std::string record_id = "REC_" + style_id + "_" + cand;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"aiarranger-human-listening-record/1\",\n";
    o << "  \"is_template\": true,\n";
    o << "  \"record_id\": \"" << jsonEscape(record_id) << "\",\n";
    o << "  \"style_id\": \"" << jsonEscape(style_id) << "\",\n";
    o << "  \"style_filename\": \"" << jsonEscape(style_name) << "\",\n";
    o << "  \"style_sha256\": \"" << jsonEscape(converted_mid_sha256) << "\",\n";
    o << "  \"genre\": \"Other/Unclassified\",\n";
    o << "  \"source_class\": \"" << jsonEscape(src_class) << "\",\n";
    o << "  \"candidate_label\": \"" << jsonEscape(cand) << "\",\n";
    o << "  \"evaluator_id\": \"EVAL_01\",\n";
    o << "  \"hardware_verified\": false,\n";
    o << "  \"hardware_model\": \"Korg Pa5X\",\n";
    o << "  \"reference_type\": \"Genos_Direct_AB\",\n";
    o << "  \"session_date\": \"YYYY-MM-DD\",\n";
    o << "  \"blind_session\": true,\n";
    o << "  \"listening_environment\": \"Korg Pa5X direct monitoring\",\n";
    o << "  \"analyzer_version\": \"phase2-m4-v1\",\n";
    o << "  \"overall_rating_1_to_5\": null,\n";
    o << "  \"per_dimension_ratings\": {\n";
    o << "    \"D01\": null,\n";
    o << "    \"D02\": null,\n";
    o << "    \"D03\": null,\n";
    o << "    \"D04\": null,\n";
    o << "    \"D05\": null,\n";
    o << "    \"D06\": null,\n";
    o << "    \"D07\": null,\n";
    o << "    \"D08\": null,\n";
    o << "    \"D09\": null,\n";
    o << "    \"D10\": null,\n";
    o << "    \"D11\": null,\n";
    o << "    \"D12\": null\n";
    o << "  },\n";
    o << "  \"critical_defect_count\": 0,\n";
    o << "  \"minor_defect_count\": 0,\n";
    o << "  \"defect_annotations\": [],\n";
    o << "  \"major_correction_needed\": false,\n";
    o << "  \"required_sections_verified\": true,\n";
    o << "  \"required_chords_verified\": true,\n";
    o << "  \"evaluator_comments\": \"TEMPLATE ONLY — fill after owner-authorized Pa5X audition, then set is_template=false, hardware_verified=true, and overall_rating_1_to_5 (1..5).\"\n";
    o << "}\n";
    return o.str();
}

} // namespace ai_arranger::exporters::korg::quality

#endif // AI_ARRANGER_EXPORTERS_KORG_ACCEPTANCE_GATE_H
