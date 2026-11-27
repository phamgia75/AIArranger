// aiarranger-convert: Yamaha Genos .sty -> Korg Pa5X Style SMF (separated by markers),
// with structured single-file bundle output (--out-bundle), reproducible SHA-256
// manifests (--manifest), unresolved instrument reports (--unresolved-report),
// opt-in rubato baking (--bake-rubato), and batch directory conversion (--batch-dir).
//
// Single-file legacy / direct mode:
//   aiarranger-convert --input Style.sty --target pa5x --output Style.mid
//                      [--ppqn N] [--sound-map FILE] [--sound-template FILE] [--report FILE]
//                      [--manifest FILE] [--unresolved-report FILE] [--out-bundle DIR]
// Single-file bundle mode:
//   aiarranger-convert --input Style.sty --out-bundle OutDir [--target pa5x] [...]
// Batch mode:
//   aiarranger-convert --batch-dir InDir --out-dir OutDir [--strict-batch] [--flat] [...]
// Compare mode:
//   aiarranger-convert --compare Style.mid --with Pa5X_Export.mid
//
// Exit: 0 = written and validated / round trip equal / all batch styles ok,
//       1 = usage, invalid flags, or unreadable input/config error,
//       2 = conversion or validation failed (no corrupt SMF written) / round trip differs / batch had failures.

#include "engine/uasf/serializer.h"
#include "exporters/korg/acceptance_gate.h"
#include "exporters/korg/conversion_manifest.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/musical_quality_analyzer.h"
#include "exporters/korg/pa5x_sound_matcher.h"
#include "exporters/korg/sff1_route.h"
#include "exporters/korg/sound_map.h"
#include "exporters/korg/style_enhancer.h"
#include "exporters/korg/target_kst_drum_adapter.h"
#include "exporters/korg/timbre_profiler.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::cerr << "usage: aiarranger-convert --input <file.sty> --target pa5x --output <file.mid>\n"
                 "                          [--ppqn N] [--sound-map|--soundmap FILE] [--sound-template FILE] [--report FILE]\n"
                 "                          [--out-bundle DIR] [--manifest FILE] [--unresolved-report FILE]\n"
                 "                          [--break-policy <cap|withhold>] [--quality-mode <quality|raw>] [--raw]\n"
                 "                          [--megavoice-mode <auto|sanitize|preserve>] [--drum-remap <on|off>]\n"
                 "                          [--cv-split <on|off>] [--normalize-source-chord <on|off>]\n"
                 "                          [--mix-normalize <on|off>] [--groove-normalize <on|off>]\n"
                 "                          [--xg-sysex-mix <on|off>] [--headroom-normalize <on|off>]\n"
                 "                          [--bake-rubato [on|off]] [--style-edit-sheet FILE]\n"
                 "                          [--auto-sound-map | --no-auto-sound-map]\n"
                 "                          [--target-kst <dir>] [--prefer-target-user-sounds] [--dump-soundmap FILE]\n"
                 "                          [--enhance <off|recommended>] [--analyze-musical-quality <dir_or_prefix>]\n"
                 "                          [--listening-record <path.json>] [--validation-bundle <dir>]\n"
                 "       aiarranger-convert --input <file.sty> --out-bundle <dir> [options...]\n"
                 "       aiarranger-convert --input <file.sty> --validation-bundle <dir> [--enhance off|recommended] [--listening-record FILE] [options...]\n"
                 "       aiarranger-convert --batch-dir <in_dir> --out-dir <out_dir> [--strict-batch] [--flat] [options...]\n"
                 "       aiarranger-convert --compare <ours.mid> --with <pa5x_export.mid>\n"
                 "sound-map lines: <track> <msb> <lsb> <pc> | <section>.<track> <msb> <lsb> <pc> | <key> unresolved\n";
    return 1;
}

bool readTextFile(const fs::path& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "cannot open " << path.string() << "\n";
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) {
        std::cerr << "error reading " << path.string() << "\n";
        return false;
    }
    return true;
}

bool ensureParentDir(const fs::path& path) {
    const fs::path parent = path.parent_path();
    if (parent.empty()) return true;
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        std::cerr << "cannot create directory " << parent.string() << ": " << ec.message() << "\n";
        return false;
    }
    return true;
}

bool resolveAnalyzeOutputPaths(const std::string& arg, fs::path& outJson, fs::path& outMd) {
    if (arg.empty()) return false;
    std::error_code ec;
    const bool endsWithSep = (arg.back() == '/' || arg.back() == '\\');
    const fs::path p(arg);
    const bool existsAsDir = fs::exists(p, ec) && fs::is_directory(p, ec);
    if (endsWithSep || existsAsDir) {
        if (fs::exists(p, ec) && !fs::is_directory(p, ec)) {
            std::cerr << "analyze output target exists and is not a directory: " << arg << "\n";
            return false;
        }
        fs::create_directories(p, ec);
        if (ec) {
            std::cerr << "cannot create analyze output directory " << arg << ": " << ec.message() << "\n";
            return false;
        }
        outJson = p / "musical_quality.json";
        outMd = p / "musical_quality.md";
        return true;
    }
    outJson = fs::path(arg + ".json");
    outMd = fs::path(arg + ".md");
    return ensureParentDir(outJson);
}

std::string fmtSigned2(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << (v >= 0.0 ? "+" : "") << v;
    return o.str();
}

std::string fmtUnsigned2(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << v;
    return o.str();
}

std::string formatComparisonBvsCMd(
    const std::string& styleName,
    const quality::MusicalQualityReport& mqB,
    const quality::MusicalQualityReport& mqC,
    const quality::MusicalQualityDelta& delta,
    const quality::EnhanceResult& enh,
    const std::string& shaB,
    const std::string& shaC) {
    std::ostringstream o;
    o << "# Candidate B (Mode A Faithful) vs. Candidate C (Mode B Enhanced): " << styleName << "\n\n";
    o << "- **Epistemic Boundary:** `hardwareVerified: false` | `l3Status: NOT_TESTED` | `independentFromFrozenQ1: true`\n";
    o << "- **Mode A Integrity:** Candidate B (`candidate_b_faithful/converted.mid`) was **NOT mutated** by Mode B.\n";
    o << "- **Candidate B SHA-256:** `" << shaB << "`\n";
    o << "- **Candidate C SHA-256:** `" << shaC << "` ("
      << (shaB == shaC ? "identical — 0 enhancement edits needed" : "modified by opt-in Mode B rules") << ")\n";
    o << "- **Overall Musical Quality Score:** `" << fmtUnsigned2(mqB.overall_musical_score)
      << "` -> `" << fmtUnsigned2(mqC.overall_musical_score)
      << "` (`" << fmtSigned2(delta.overall_delta) << "`)\n";
    o << "- **Critical Regressions Introduced:** `" << (delta.any_critical_regression ? "YES" : "NO (0)") << "`\n";
    o << "- **Enhancement Actions Applied:** `" << enh.stats.actions_applied
      << "` (`vel=" << enh.stats.notes_velocity_adjusted
      << "`, `pitch=" << enh.stats.notes_pitch_adjusted
      << "`, `cc+=" << enh.stats.cc_events_added
      << "`, `cc-=" << enh.stats.cc_events_removed
      << "`, `cv+=" << enh.stats.cv_elements_synthesized << "`)\n\n";

    o << "## 12-Dimension Side-by-Side Comparison\n\n";
    o << "| # | Dimension | Mode A (Faithful) | Mode B (Enhanced) | Delta | Status (A -> B) |\n";
    o << "|---:|---|---:|---:|---:|---|\n";
    for (size_t i = 0; i < 12u; ++i) {
        const auto& db = mqB.dimensions[i];
        const auto& dc = mqC.dimensions[i];
        o << "| " << static_cast<unsigned>(db.id)
          << " | " << db.name
          << " | " << fmtUnsigned2(db.score)
          << " | " << fmtUnsigned2(dc.score)
          << " | `" << fmtSigned2(delta.dimension_deltas[i]) << "`"
          << " | `" << db.status << "` -> `" << dc.status << "` |\n";
    }

    o << "\n## Finding Delta\n\n";
    o << "- **Resolved Findings (" << delta.resolved_finding_codes.size() << "):** ";
    if (delta.resolved_finding_codes.empty()) {
        o << "_none_\n";
    } else {
        for (size_t i = 0; i < delta.resolved_finding_codes.size(); ++i) {
            if (i > 0u) o << ", ";
            o << "`" << delta.resolved_finding_codes[i] << "`";
        }
        o << "\n";
    }
    o << "- **Newly Introduced Warnings (" << delta.newly_introduced_warnings.size() << "):** ";
    if (delta.newly_introduced_warnings.empty()) {
        o << "_none_\n";
    } else {
        for (size_t i = 0; i < delta.newly_introduced_warnings.size(); ++i) {
            if (i > 0u) o << ", ";
            o << "`" << delta.newly_introduced_warnings[i] << "`";
        }
        o << "\n";
    }
    o << "\nSee `candidate_c_enhanced/enhancement_actions.md` for every reversible edit record.\n";
    return o.str();
}

std::string formatEnhancedManifestJson(
    const std::string& styleName,
    const std::string& inputSha256,
    const std::string& faithfulMidSha256,
    const quality::EnhanceResult& enh,
    const std::vector<ArtifactEntry>& artifacts) {
    using namespace manifest_detail;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"aiarranger-enhanced-candidate-manifest/1\",\n";
    o << "  \"inputFilename\": " << jsonString(styleName) << ",\n";
    o << "  \"inputSha256\": " << jsonString(inputSha256) << ",\n";
    o << "  \"faithfulCandidateMidSha256\": " << jsonString(faithfulMidSha256) << ",\n";
    o << "  \"conversionMode\": " << jsonString(quality::conversionModeName(enh.mode)) << ",\n";
    o << "  \"ok\": " << (enh.ok ? "true" : "false") << ",\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"NOT_TESTED\",\n";
    o << "  \"outputFormat\": \"PA5X_MAPPED_SMF_IMPORT_ARTIFACT\",\n";
    o << "  \"nativeStyBinaryVerified\": false,\n";
    o << "  \"independentFromFrozenQ1\": true,\n";
    o << "  \"enhancementStats\": {\n";
    o << "    \"actionsApplied\": " << enh.stats.actions_applied << ",\n";
    o << "    \"notesVelocityAdjusted\": " << enh.stats.notes_velocity_adjusted << ",\n";
    o << "    \"notesPitchAdjusted\": " << enh.stats.notes_pitch_adjusted << ",\n";
    o << "    \"ccEventsAdded\": " << enh.stats.cc_events_added << ",\n";
    o << "    \"ccEventsRemoved\": " << enh.stats.cc_events_removed << ",\n";
    o << "    \"cvElementsSynthesized\": " << enh.stats.cv_elements_synthesized << ",\n";
    o << "    \"rolledBackOnGuardrail\": " << (enh.stats.rolled_back_on_guardrail ? "true" : "false") << "\n";
    o << "  },\n";
    o << "  \"artifacts\": [\n";
    for (size_t i = 0; i < artifacts.size(); ++i) {
        const auto& a = artifacts[i];
        o << "    {\n";
        o << "      \"role\": " << jsonString(a.role) << ",\n";
        o << "      \"relativePath\": " << jsonString(a.relative_path) << ",\n";
        o << "      \"sizeBytes\": " << a.size_bytes << ",\n";
        o << "      \"sha256\": " << jsonString(a.sha256) << "\n";
        o << "    }" << (i + 1u < artifacts.size() ? "," : "") << "\n";
    }
    o << "  ]\n";
    o << "}\n";
    return o.str();
}

std::string formatOperatorPrepNotesMd(
    const std::string& styleName,
    bool hasEnhanced,
    const std::string& shaB,
    const std::string& shaC) {
    std::ostringstream o;
    o << "# Hardware Session Staging — Operator Preparation Notes\n\n";
    o << "**STATUS STAMP:** `OFFLINE_STAGING_ONLY — NO_HARDWARE_WRITES — HARDWARE_AUTHORIZATION_REQUIRED`\n\n";
    o << "- **Style:** `" << styleName << "`\n";
    o << "- **Hardware Access Performed by CLI:** `NONE` (zero CoreMIDI, zero USB, zero device writes)\n";
    o << "- **Candidate B (Mode A Faithful) SMF SHA-256:** `" << shaB << "` (`../candidate_b_faithful/converted.mid`)\n";
    if (hasEnhanced) {
        o << "- **Candidate C (Mode B Enhanced) SMF SHA-256:** `" << shaC
          << "` (`../candidate_c_enhanced/converted.mid`)\n";
    }
    o << "\n## Pre-Flight Checklist (Before Authorized Pa5X Hardware Audition)\n\n";
    o << "1. Verify SHA-256 digests in `SHA256SUMS.txt` before copying any `.mid` to a verified FAT32 USB drive.\n";
    o << "2. Confirm full verified backup of any user Pa5X library before loading test styles into a temporary User bank slot.\n";
    o << "3. Review `style_edit_sheet.md` (Section 0 Import SMF & CV1..CV6 Chord Table/Key-Chord setup, Section 1 NTT/Bass Inversion/Wrap/Key Range/EQ, Section 2 Sound assignments, Section 3 XG SysEx advisories).\n";
    o << "4. On Pa5X: **Style Edit > Menu > Import/Export > Import SMF**, check **Initialize**, press **SHIFT + Execute**.\n";
    o << "5. Audition Intros, Variations `V1..V4`, Fills `F1..F4`, Break, and Endings across `Maj / Min / 7th / Dim / Sus4` chords";
    if (hasEnhanced) {
        o << " — perform blind A/B listening comparing Candidate B (Mode A Faithful) vs. Candidate C (Mode B Enhanced)";
    }
    o << ".\n";
    o << "6. Record honest musician ratings and defects in `LISTENING_RECORD_TEMPLATE.json`, set `\"is_template\": false`, `\"hardware_verified\": true`, and `\"overall_rating_1_to_5\"` (`1..5`), save to a new file (e.g., `listening_record_completed.json`), and re-run `aiarranger-convert --validation-bundle ... --listening-record listening_record_completed.json` to compute the hardware-verified Acceptance Gate grade (the completed JSON is also directly compatible with `calibrate_analyzer_human.py`).\n";
    return o.str();
}

std::string formatValidationSummaryMd(
    const std::string& styleName,
    const ConversionManifest& manifestB,
    const quality::MusicalQualityReport& mqB,
    bool hasEnhanced,
    const quality::MusicalQualityReport* mqC,
    const quality::MusicalQualityDelta* delta,
    const quality::EnhanceResult* enh,
    const quality::AcceptanceReport& acceptance,
    const std::string& shaB,
    const std::string& shaC) {
    std::ostringstream o;
    o << "# Practical Musician Validation Workflow Summary: " << styleName << "\n\n";
    o << "- **Workflow Version:** Phase 3 P3-M7/M8 (10-Step Offline Validation Bundle)\n";
    o << "- **Proposed Acceptance Verdict:** **`" << quality::acceptanceGradeDisplay(acceptance.grade)
      << "`** (`criteriaStatus: " << acceptance.criteria_status << "`)\n";
    o << "- **Epistemic Boundary:** `hardwareVerified: " << (acceptance.hardware_verified ? "true" : "false")
      << "` | `l3Status: " << acceptance.l3_status
      << "` | `independentFromFrozenQ1: true`\n";
    o << "- **Mode B Opt-In Enhancement:** `" << (hasEnhanced ? "recommended (Candidate C generated alongside untouched Candidate B)" : "off (Mode A Faithful only)") << "`\n\n";

    o << "## 10-Step Musician Validation Checklist\n\n";
    o << "| Step | Stage | Status | Artifact |\n";
    o << "|---:|---|---|---|\n";
    o << "| 1 | Ingest & Hash Source Style (`SHA-256`) | DONE | `candidate_b_faithful/manifest.json` |\n";
    o << "| 2 | Mode A Faithful Conversion (`mapToKorg`) | DONE (`" << manifestB.elements_exported << "` elements) | `candidate_b_faithful/converted.mid` |\n";
    o << "| 3 | Independent SMF Re-Parse & Validator Check | PASS (`0` hard violations) | `candidate_b_faithful/report.md` |\n";
    o << "| 4 | Sound Resolution & Unresolved Audit | `" << acceptance.offline.unresolved_total_sounds
      << "` unresolved (`" << acceptance.offline.unresolved_critical_sounds << "` critical) | `candidate_b_faithful/unresolved_sounds.md` |\n";
    o << "| 5 | Pa5X Style Edit Parameter Sheet (`Section 0..3`) | DONE | `candidate_b_faithful/style_edit_sheet.md` |\n";
    o << "| 6 | 12-Dimension Read-Only Musical Quality Critique (Mode A) | `"
      << fmtUnsigned2(mqB.overall_musical_score) << " / 100.00` | `musical_quality_faithful.md` |\n";
    if (hasEnhanced && mqC && delta && enh) {
        o << "| 7 | Opt-In Mode B Reversible Enhancement (`--enhance recommended`) | DONE (`"
          << enh->stats.actions_applied << "` actions, Mode A untouched) | `candidate_c_enhanced/enhancement_actions.md` |\n";
        o << "| 8 | Side-by-Side Candidate B vs. Candidate C Diff | `"
          << fmtUnsigned2(mqC->overall_musical_score) << "` (`" << fmtSigned2(delta->overall_delta)
          << "`) | `comparison_b_vs_c.md` |\n";
    } else {
        o << "| 7 | Opt-In Mode B Reversible Enhancement (`--enhance off`) | SKIPPED (default Mode A) | _(re-run with `--enhance recommended` to generate Candidate C)_ |\n";
        o << "| 8 | Side-by-Side Candidate B vs. Candidate C Diff | N/A | _(generated when `--enhance recommended` is passed)_ |\n";
    }
    o << "| 9 | Proposed Production Acceptance Gate (`Grade A-U`) | **`"
      << quality::acceptanceGradeDisplay(acceptance.grade) << "`** | `acceptance_report.md` / `acceptance_report.json` |\n";
    o << "| 10 | Offline Hardware Session Staging (Zero HW Writes) | STAGED (`OFFLINE_STAGING_ONLY`) | `hardware_session_staging/OPERATOR_PREP_NOTES.md` |\n\n";

    o << "## Key Digests\n\n";
    o << "- **Input Style SHA-256:** `" << manifestB.input_sha256 << "`\n";
    o << "- **Candidate B (`converted.mid`) SHA-256:** `" << shaB << "`\n";
    if (hasEnhanced) {
        o << "- **Candidate C (`converted.mid`) SHA-256:** `" << shaC << "`\n";
    }

    o << "\n## Gate Reasons & Next Actions\n\n";
    for (const auto& r : acceptance.gate_reasons) o << "- **Reason:** " << r << "\n";
    for (const auto& a : acceptance.required_next_actions) o << "- **Next Action:** " << a << "\n";
    return o.str();
}

bool writeText(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    f.close();
    if (!f) std::cerr << "cannot write " << path.string() << "\n";
    return static_cast<bool>(f);
}

bool writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    f.close();
    if (!f) std::cerr << "cannot write " << path.string() << "\n";
    return static_cast<bool>(f);
}

bool readStyle(const std::string& path, Style& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << path << "\n"; return false; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto p = parse(bytes);
    if (!p.ok) { std::cerr << path << ": " << p.error << "\n"; return false; }
    for (const auto& n : p.notes) std::cout << path << ": " << n << "\n";
    out = p.style;
    return true;
}

int compareMode(const std::string& ours, const std::string& theirs) {
    Style a, b;
    if (!readStyle(ours, a) || !readStyle(theirs, b)) return 1;
    const auto c = compareStyles(a, b);
    for (const auto& i : c.info) std::cout << "info: " << i << "\n";
    for (const auto& d : c.differences) std::cout << "DIFF: " << d << "\n";
    std::cout << "Round trip: " << (c.differences.empty() ? "EQUAL" : "DIFFERS") << " (" << a.elements.size()
              << " elements checked)\n";
    return c.differences.empty() ? 0 : 2;
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isYamahaStyleExtension(const fs::path& p) {
    const std::string ext = toLowerAscii(p.extension().string());
    return ext == ".sty" || ext == ".prs" || ext == ".sst" || ext == ".bcs" || ext == ".pcs" || ext == ".fps";
}

// Reject path traversal and produce a clean, deterministic single-component directory stem.
bool sanitizeBatchStem(const fs::path& relPath, std::string& outStem) {
    for (const auto& part : relPath) {
        if (part == "..") return false;
    }
    fs::path withoutExt = relPath;
    withoutExt.replace_extension("");
    const std::string raw = withoutExt.generic_string();
    std::string clean;
    clean.reserve(raw.size() + 4);
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '/' || c == '\\') {
            clean += "__";
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '-' || c == '.') {
            clean.push_back(static_cast<char>(c));
        } else {
            clean.push_back('_');
        }
    }
    while (!clean.empty() && (clean.front() == '.' || clean.front() == '_')) clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == '.') clean.pop_back();
    if (clean.empty() || clean == "." || clean == ".." || clean.find("..") != std::string::npos) {
        return false;
    }
    outStem = std::move(clean);
    return true;
}

struct SingleConvertOutcome {
    int exit_code = 0; // 0 = ok, 1 = input/io error, 2 = conversion/validation failure
    ConversionManifest manifest;
    std::string report_text;
    // Populated only when captureValidationData == true (avoids extra copies in batch mode).
    bool has_validation_data = false;
    Style korg_style;
    Report map_report;
    uasf::StyleDefinition source_uasf;
    std::vector<uint8_t> written_mid_bytes;
    std::string written_mid_sha256;
};

SingleConvertOutcome convertOneStyle(
    const fs::path& inputPath,
    const MapOptions& baseOpt,
    const ConversionOptionsRecord& optRecord,
    const std::optional<fs::path>& directOutputMid,
    const std::optional<fs::path>& outBundleDir,
    const std::optional<fs::path>& reportPath,
    const std::optional<fs::path>& templatePath,
    const std::optional<fs::path>& dumpSoundMapPath,
    const std::optional<fs::path>& styleEditSheetPath,
    const std::optional<fs::path>& manifestPath,
    const std::optional<fs::path>& unresolvedReportPath,
    bool quietStdout,
    bool applyEnhanceRecommended = false,
    bool captureValidationData = false) {
    SingleConvertOutcome outcome{};
    const std::string baseStyleName = inputPath.filename().string();

    uint64_t inputSize = 0;
    const std::string inputSha = manifest_detail::sha256FileHex(inputPath, inputSize);
    outcome.manifest.input_filename = baseStyleName;
    outcome.manifest.input_size_bytes = inputSize;
    outcome.manifest.input_sha256 = inputSha;
    outcome.manifest.options = optRecord;

    importers::sff1::Sff1Reader reader;
    const auto parsed = reader.parseFile(inputPath.string());
    if (!parsed.success) {
        outcome.exit_code = 1;
        outcome.manifest.ok = false;
        outcome.manifest.error = "import failed: " + parsed.error;
        outcome.manifest.reader_warnings = parsed.warnings;
        outcome.manifest.xg_sysex_malformed = parsed.xg_sysex.malformed_count;
        if (!quietStdout) std::cerr << outcome.manifest.error << "\n";
        return outcome;
    }

    importers::sff1::Sff1ToUasfMapper toUasf;
    const auto canon = toUasf.map(parsed);
    if (!canon.success) {
        outcome.exit_code = 1;
        outcome.manifest.ok = false;
        outcome.manifest.error = "import failed: " + canon.error;
        outcome.manifest.reader_warnings = parsed.warnings;
        outcome.manifest.xg_sysex_malformed = parsed.xg_sysex.malformed_count;
        if (!quietStdout) std::cerr << outcome.manifest.error << "\n";
        return outcome;
    }

    MapOptions opt = baseOpt;
    opt.route = routeFromCasm(parsed);
    if (opt.enable_bake_rubato) {
        opt.section_tempos = sectionTemposFromSff1(parsed);
    }
    const bool needSoundmapDump = dumpSoundMapPath.has_value() || outBundleDir.has_value();
    const bool needEditSheet = styleEditSheetPath.has_value() || outBundleDir.has_value();
    if (needSoundmapDump) {
        opt.capture_soundmap_dump = true;
        opt.soundmap_style_name = baseStyleName;
    }
    if (needEditSheet) {
        opt.capture_style_edit_sheet = true;
        if (opt.soundmap_style_name.empty()) opt.soundmap_style_name = baseStyleName;
    }

    const auto mapped = mapToKorg(canon.style, opt);
    if (!mapped.ok) {
        outcome.exit_code = 2;
        outcome.manifest.ok = false;
        outcome.manifest.error = "conversion failed: " + mapped.error;
        outcome.manifest.reader_warnings = parsed.warnings;
        outcome.manifest.xg_sysex_malformed = parsed.xg_sysex.malformed_count;
        if (!quietStdout) std::cerr << outcome.manifest.error << "\n";
        return outcome;
    }

    // Opt-in Mode B enhancement is invoked ONLY when explicitly requested (`--enhance recommended`).
    // When applyEnhanceRecommended == false (default Mode A), style_enhancer.h is never called,
    // guaranteeing bit-for-bit identical bytes to Mode A.
    const Style* styleToWrite = &mapped.style;
    std::optional<quality::EnhanceResult> enhancedResult;
    if (applyEnhanceRecommended) {
        enhancedResult = quality::enhanceStyle(
            mapped.style, quality::EnhanceOptions::recommended(), &mapped.report, &canon.style);
        if (!enhancedResult->ok) {
            outcome.exit_code = 2;
            outcome.manifest.ok = false;
            outcome.manifest.error = "enhancement failed (rolled back): " + enhancedResult->error;
            if (!quietStdout) std::cerr << outcome.manifest.error << "\n";
            return outcome;
        }
        styleToWrite = &enhancedResult->style;
    }

    // Write -> re-parse with the independent reader -> validate BEFORE writing any output files.
    const auto written = write(*styleToWrite);
    if (!written.ok) {
        outcome.exit_code = 2;
        outcome.manifest.ok = false;
        outcome.manifest.error = "write failed: " + written.error;
        if (!quietStdout) std::cerr << outcome.manifest.error << "\n";
        return outcome;
    }
    const auto back = parse(written.bytes);
    const std::vector<std::string> errs = back.ok ? validate(back.style) : std::vector<std::string>{back.error};

    std::string report = "Input: " + baseStyleName + "\nTarget: Korg Pa5X Style SMF separated by markers (format 0)\n" +
                         formatReport(mapped.report, mapped.style);
    if (!parsed.warnings.empty() || parsed.xg_sysex.malformed_count > 0 || !parsed.detected_metadata_chunks.empty()) {
        report += "\nImporter diagnostics (Sff1Reader):\n";
        if (parsed.xg_sysex.malformed_count > 0) {
            report += "  malformed XG/MIDI SysEx count: " + std::to_string(parsed.xg_sysex.malformed_count) + "\n";
        }
        if (!parsed.detected_metadata_chunks.empty()) {
            report += "  metadata chunks (read-only):";
            for (const auto& cid : parsed.detected_metadata_chunks) report += " " + cid;
            report += "\n";
        }
        for (const auto& w : parsed.warnings) report += "  " + w + "\n";
    }
    report += "\nValidator: " +
              (errs.empty() ? "PASS (" + std::to_string(back.style.elements.size()) + " elements)" : "FAIL") + "\n";
    for (const auto& e : errs) report += "  " + e + "\n";
    outcome.report_text = report;

    if (!quietStdout) std::cout << report;

    // Legacy behavior: write standalone sidecars requested prior to validation failure check only when no bundle.
    if (templatePath.has_value() && !writeText(*templatePath, soundMapTemplate(mapped.report, baseStyleName))) {
        outcome.exit_code = 1;
        outcome.manifest.ok = false;
        outcome.manifest.error = "failed to write sound template";
        return outcome;
    }
    if (dumpSoundMapPath.has_value() && !writeText(*dumpSoundMapPath, mapped.report.soundmap_dump)) {
        outcome.exit_code = 1;
        outcome.manifest.ok = false;
        outcome.manifest.error = "failed to write soundmap dump";
        return outcome;
    }
    if (styleEditSheetPath.has_value() && !writeText(*styleEditSheetPath, mapped.report.style_edit_sheet_md)) {
        outcome.exit_code = 1;
        outcome.manifest.ok = false;
        outcome.manifest.error = "failed to write style edit sheet";
        return outcome;
    }
    if (reportPath.has_value() && !writeText(*reportPath, report)) {
        outcome.exit_code = 1;
        outcome.manifest.ok = false;
        outcome.manifest.error = "failed to write report";
        return outcome;
    }

    if (!errs.empty()) {
        outcome.exit_code = 2;
        outcome.manifest.ok = false;
        outcome.manifest.error = "SMF validation failed: " + errs.front();
        return outcome;
    }

    std::vector<ArtifactEntry> artifacts;
    auto recordMemArtifact = [&](const std::string& role, const std::string& rel, const uint8_t* data, size_t sz) {
        artifacts.push_back({role, rel, static_cast<uint64_t>(sz), manifest_detail::sha256Hex(data, sz)});
    };
    auto recordTextArtifact = [&](const std::string& role, const std::string& rel, const std::string& text) {
        artifacts.push_back({role, rel, static_cast<uint64_t>(text.size()), manifest_detail::sha256Hex(text)});
    };

    if (directOutputMid.has_value()) {
        if (!writeBytes(*directOutputMid, written.bytes)) {
            outcome.exit_code = 1;
            outcome.manifest.ok = false;
            outcome.manifest.error = "cannot write " + directOutputMid->string();
            return outcome;
        }
        if (!quietStdout) {
            std::cout << "Wrote " << directOutputMid->string() << " (" << written.bytes.size() << " bytes)\n";
        }
        if (!outBundleDir.has_value()) {
            recordMemArtifact("smf", directOutputMid->filename().string(), written.bytes.data(), written.bytes.size());
        }
    }

    // Build preliminary manifest (before final artifact list) to generate unresolved_sounds.md.
    ConversionManifest prelim = buildConversionManifest(
        baseStyleName, inputSize, inputSha, optRecord, mapped.report, mapped.style,
        parsed.warnings, parsed.xg_sysex.malformed_count, {});
    const std::string unresolvedMd = formatUnresolvedSoundsMd(prelim);

    if (outBundleDir.has_value()) {
        std::error_code ec;
        if (fs::exists(*outBundleDir, ec) && !fs::is_directory(*outBundleDir, ec)) {
            outcome.exit_code = 1;
            outcome.manifest.ok = false;
            outcome.manifest.error = "bundle target exists and is not a directory: " + outBundleDir->string();
            if (!quietStdout) std::cerr << outcome.manifest.error << "\n";
            return outcome;
        }
        fs::create_directories(*outBundleDir, ec);
        if (ec) {
            outcome.exit_code = 1;
            outcome.manifest.ok = false;
            outcome.manifest.error = "cannot create bundle directory " + outBundleDir->string() + ": " + ec.message();
            if (!quietStdout) std::cerr << outcome.manifest.error << "\n";
            return outcome;
        }

        const fs::path midPath = *outBundleDir / "converted.mid";
        if (!writeBytes(midPath, written.bytes)) {
            outcome.exit_code = 1;
            outcome.manifest.ok = false;
            outcome.manifest.error = "cannot write bundle converted.mid";
            return outcome;
        }
        recordMemArtifact("smf", "converted.mid", written.bytes.data(), written.bytes.size());

        uasf::UasfSerializer uasfSerializer;
        const auto uasfSer = uasfSerializer.serialize(canon.style);
        if (uasfSer.success) {
            const fs::path uasfPath = *outBundleDir / "style.uasf";
            if (!writeBytes(uasfPath, uasfSer.data)) {
                outcome.exit_code = 1;
                outcome.manifest.ok = false;
                outcome.manifest.error = "cannot write bundle style.uasf";
                return outcome;
            }
            recordMemArtifact("uasf", "style.uasf", uasfSer.data.data(), uasfSer.data.size());
        }

        const std::string soundmapText = mapped.report.soundmap_dump.empty()
                                             ? soundMapTemplate(mapped.report, baseStyleName)
                                             : mapped.report.soundmap_dump;
        if (!writeText(*outBundleDir / "soundmap.txt", soundmapText)) {
            outcome.exit_code = 1;
            outcome.manifest.ok = false;
            outcome.manifest.error = "cannot write bundle soundmap.txt";
            return outcome;
        }
        recordTextArtifact("soundmap", "soundmap.txt", soundmapText);

        if (!writeText(*outBundleDir / "style_edit_sheet.md", mapped.report.style_edit_sheet_md)) {
            outcome.exit_code = 1;
            outcome.manifest.ok = false;
            outcome.manifest.error = "cannot write bundle style_edit_sheet.md";
            return outcome;
        }
        recordTextArtifact("style_edit_sheet", "style_edit_sheet.md", mapped.report.style_edit_sheet_md);

        if (!writeText(*outBundleDir / "report.md", report)) {
            outcome.exit_code = 1;
            outcome.manifest.ok = false;
            outcome.manifest.error = "cannot write bundle report.md";
            return outcome;
        }
        recordTextArtifact("report", "report.md", report);

        if (!writeText(*outBundleDir / "unresolved_sounds.md", unresolvedMd)) {
            outcome.exit_code = 1;
            outcome.manifest.ok = false;
            outcome.manifest.error = "cannot write bundle unresolved_sounds.md";
            return outcome;
        }
        recordTextArtifact("unresolved_sounds", "unresolved_sounds.md", unresolvedMd);

        if (enhancedResult.has_value()) {
            const std::string enhJson = quality::formatEnhancementAuditJson(*enhancedResult);
            const std::string enhMd = quality::formatEnhancementAuditMarkdown(*enhancedResult);
            if (!writeText(*outBundleDir / "enhancement_actions.json", enhJson) ||
                !writeText(*outBundleDir / "enhancement_actions.md", enhMd)) {
                outcome.exit_code = 1;
                outcome.manifest.ok = false;
                outcome.manifest.error = "cannot write bundle enhancement_actions";
                return outcome;
            }
            recordTextArtifact("enhancement_actions_json", "enhancement_actions.json", enhJson);
            recordTextArtifact("enhancement_actions_md", "enhancement_actions.md", enhMd);
        }
    } else {
        if (dumpSoundMapPath.has_value()) {
            recordTextArtifact("soundmap", dumpSoundMapPath->filename().string(), mapped.report.soundmap_dump);
        }
        if (styleEditSheetPath.has_value()) {
            recordTextArtifact("style_edit_sheet", styleEditSheetPath->filename().string(), mapped.report.style_edit_sheet_md);
        }
        if (reportPath.has_value()) {
            recordTextArtifact("report", reportPath->filename().string(), report);
        }
        if (unresolvedReportPath.has_value()) {
            recordTextArtifact("unresolved_sounds", unresolvedReportPath->filename().string(), unresolvedMd);
        }
    }

    if (unresolvedReportPath.has_value() && !writeText(*unresolvedReportPath, unresolvedMd)) {
        outcome.exit_code = 1;
        outcome.manifest.ok = false;
        outcome.manifest.error = "cannot write unresolved report";
        return outcome;
    }

    outcome.manifest = buildConversionManifest(
        baseStyleName, inputSize, inputSha, optRecord, mapped.report, mapped.style,
        parsed.warnings, parsed.xg_sysex.malformed_count, artifacts);

    const std::string manifestJson = formatConversionManifestJson(outcome.manifest);
    if (outBundleDir.has_value()) {
        if (!writeText(*outBundleDir / "manifest.json", manifestJson)) {
            outcome.exit_code = 1;
            outcome.manifest.ok = false;
            outcome.manifest.error = "cannot write bundle manifest.json";
            return outcome;
        }
        if (!quietStdout) {
            std::cout << "Wrote bundle " << outBundleDir->string() << " (" << artifacts.size()
                      << " hashed artifacts + manifest.json)\n";
        }
    }
    if (manifestPath.has_value() && !writeText(*manifestPath, manifestJson)) {
        outcome.exit_code = 1;
        outcome.manifest.ok = false;
        outcome.manifest.error = "cannot write manifest";
        return outcome;
    }

    if (captureValidationData) {
        outcome.has_validation_data = true;
        outcome.korg_style = *styleToWrite;
        outcome.map_report = mapped.report;
        outcome.source_uasf = canon.style;
        outcome.written_mid_bytes = written.bytes;
        outcome.written_mid_sha256 = manifest_detail::sha256Hex(written.bytes.data(), written.bytes.size());
    }

    outcome.exit_code = 0;
    return outcome;
}

} // namespace

int main(int argc, char** argv) {
    std::string input, output, target, soundMap, reportPath, templatePath, dumpSoundMapPath,
        styleEditSheetPath, targetKstPath, compareA, compareB;
    std::string outBundleDir, batchDir, outDir, manifestPath, unresolvedReportPath;
    std::string enhanceMode = "off";
    std::string analyzeQualityArg, listeningRecordPath, validationBundleDir;
    bool hasValidationBundle = false;
    MapOptions opt;
    bool rawModeRequested = false;
    bool explicitAutoSoundMap = false;
    bool explicitNoAutoSoundMap = false;
    bool preferTargetUserSounds = false;
    bool explicitQ5Enable = false;
    bool explicitBakeRubatoOn = false;
    bool strictBatch = false;
    bool flatBatch = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        // Boolean flags (no trailing value argument)
        if (a == "--raw") {
            rawModeRequested = true;
            opt.enable_cv_split = false;
            opt.enable_drum_remap = false;
            opt.megavoice_mode = quality::MegaVoiceMode::Preserve;
            opt.enable_source_chord_normalization = false;
            opt.enable_sound_matching = false;
            opt.prefer_target_user_sounds = false;
            opt.enable_mix_groove_normalization = false;
            opt.enable_groove_normalization = false;
            opt.enable_mix_normalization = false;
            opt.enable_xg_sysex_mix_injection = false;
            opt.enable_headroom_normalization = false;
            opt.enable_bake_rubato = false;
            continue;
        }
        if (a == "--auto-sound-map") {
            explicitAutoSoundMap = true;
            continue;
        }
        if (a == "--no-auto-sound-map") {
            explicitNoAutoSoundMap = true;
            continue;
        }
        if (a == "--prefer-target-user-sounds") {
            preferTargetUserSounds = true;
            continue;
        }
        if (a == "--strict-batch") {
            strictBatch = true;
            continue;
        }
        if (a == "--flat") {
            flatBatch = true;
            continue;
        }
        if (a == "--bake-rubato") {
            if (i + 1 < argc && (std::string(argv[i + 1]) == "on" || std::string(argv[i + 1]) == "off")) {
                const std::string v = argv[++i];
                if (v == "on") {
                    opt.enable_bake_rubato = true;
                    explicitBakeRubatoOn = true;
                } else {
                    opt.enable_bake_rubato = false;
                    explicitBakeRubatoOn = false;
                }
            } else {
                opt.enable_bake_rubato = true;
                explicitBakeRubatoOn = true;
            }
            continue;
        }

        if (i + 1 >= argc) return usage();
        const std::string v = argv[++i];
        if (a == "--input") input = v;
        else if (a == "--output") output = v;
        else if (a == "--target") target = v;
        else if (a == "--sound-map" || a == "--soundmap") soundMap = v;
        else if (a == "--report") reportPath = v;
        else if (a == "--sound-template") templatePath = v;
        else if (a == "--dump-soundmap") dumpSoundMapPath = v;
        else if (a == "--style-edit-sheet") styleEditSheetPath = v;
        else if (a == "--target-kst") targetKstPath = v;
        else if (a == "--out-bundle") outBundleDir = v;
        else if (a == "--batch-dir") batchDir = v;
        else if (a == "--out-dir") outDir = v;
        else if (a == "--manifest") manifestPath = v;
        else if (a == "--unresolved-report") unresolvedReportPath = v;
        else if (a == "--compare") compareA = v;
        else if (a == "--with") compareB = v;
        else if (a == "--enhance") {
            if (v == "off" || v == "recommended") enhanceMode = v;
            else {
                std::cerr << "invalid flags: --enhance must be 'off' or 'recommended' (got '" << v << "')\n";
                return 1;
            }
        } else if (a == "--analyze-musical-quality") {
            if (v.empty()) {
                std::cerr << "invalid flags: --analyze-musical-quality path must be non-empty\n";
                return 1;
            }
            analyzeQualityArg = v;
        } else if (a == "--listening-record") {
            if (v.empty()) {
                std::cerr << "invalid flags: --listening-record path must be non-empty\n";
                return 1;
            }
            listeningRecordPath = v;
        } else if (a == "--validation-bundle") {
            hasValidationBundle = true;
            validationBundleDir = v;
        }
        else if (a == "--ppqn") {
            try {
                size_t used = 0;
                const long n = std::stol(v, &used);
                if (used != v.size() || n < 1 || n > 0x7FFF) return usage();
                opt.ppqn = static_cast<uint16_t>(n);
            } catch (...) { return usage(); }
        } else if (a == "--break-policy") {
            if (v == "cap") opt.break_policy = BreakPolicy::Yamaha1BarCap;
            else if (v == "withhold") opt.break_policy = BreakPolicy::Withhold;
            else return usage();
        } else if (a == "--quality-mode") {
            if (v == "raw") {
                rawModeRequested = true;
                opt.enable_cv_split = false;
                opt.enable_drum_remap = false;
                opt.megavoice_mode = quality::MegaVoiceMode::Preserve;
                opt.enable_source_chord_normalization = false;
                opt.enable_sound_matching = false;
                opt.prefer_target_user_sounds = false;
                opt.enable_mix_groove_normalization = false;
                opt.enable_groove_normalization = false;
                opt.enable_mix_normalization = false;
                opt.enable_xg_sysex_mix_injection = false;
                opt.enable_headroom_normalization = false;
                opt.enable_bake_rubato = false;
            } else if (v == "quality") {
                rawModeRequested = false;
                opt.enable_cv_split = true;
                opt.enable_drum_remap = true;
                opt.megavoice_mode = quality::MegaVoiceMode::Auto;
                opt.enable_source_chord_normalization = true;
                opt.enable_sound_matching = true;
                opt.enable_mix_groove_normalization = true;
                opt.enable_groove_normalization = true;
                opt.enable_mix_normalization = true;
                opt.enable_xg_sysex_mix_injection = true;
                opt.enable_headroom_normalization = true;
            } else return usage();
        } else if (a == "--megavoice-mode") {
            if (v == "auto") opt.megavoice_mode = quality::MegaVoiceMode::Auto;
            else if (v == "sanitize") opt.megavoice_mode = quality::MegaVoiceMode::Sanitize;
            else if (v == "preserve") opt.megavoice_mode = quality::MegaVoiceMode::Preserve;
            else return usage();
        } else if (a == "--drum-remap") {
            if (v == "on") opt.enable_drum_remap = true;
            else if (v == "off") opt.enable_drum_remap = false;
            else return usage();
        } else if (a == "--cv-split") {
            if (v == "on") opt.enable_cv_split = true;
            else if (v == "off") opt.enable_cv_split = false;
            else return usage();
        } else if (a == "--normalize-source-chord") {
            if (v == "on") opt.enable_source_chord_normalization = true;
            else if (v == "off") opt.enable_source_chord_normalization = false;
            else return usage();
        } else if (a == "--mix-normalize") {
            if (v == "on") { opt.enable_mix_normalization = true; explicitQ5Enable = true; }
            else if (v == "off") opt.enable_mix_normalization = false;
            else return usage();
        } else if (a == "--groove-normalize") {
            if (v == "on") { opt.enable_groove_normalization = true; explicitQ5Enable = true; }
            else if (v == "off") opt.enable_groove_normalization = false;
            else return usage();
        } else if (a == "--xg-sysex-mix") {
            if (v == "on") { opt.enable_xg_sysex_mix_injection = true; explicitQ5Enable = true; }
            else if (v == "off") opt.enable_xg_sysex_mix_injection = false;
            else return usage();
        } else if (a == "--headroom-normalize") {
            if (v == "on") { opt.enable_headroom_normalization = true; explicitQ5Enable = true; }
            else if (v == "off") opt.enable_headroom_normalization = false;
            else return usage();
        } else return usage();
    }

    // ── Mode dispatch & fail-closed flag combination validation ──
    if (hasValidationBundle && validationBundleDir.empty()) {
        std::cerr << "invalid flags: --validation-bundle directory must be non-empty\n";
        return 1;
    }

    if (!compareA.empty() || !compareB.empty()) {
        if (compareA.empty() || compareB.empty() || !input.empty() || !batchDir.empty() ||
            !outBundleDir.empty() || hasValidationBundle ||
            !analyzeQualityArg.empty() || !listeningRecordPath.empty()) {
            return usage();
        }
        return compareMode(compareA, compareB);
    }

    const bool enhanceRecommended = (enhanceMode == "recommended");
    const bool isBatchMode = !batchDir.empty() || !outDir.empty();
    if (isBatchMode) {
        if (batchDir.empty() || outDir.empty()) {
            std::cerr << "invalid flags: --batch-dir and --out-dir must both be specified\n";
            return 1;
        }
        if (!input.empty() || !output.empty() || !outBundleDir.empty() || hasValidationBundle ||
            !analyzeQualityArg.empty() || !listeningRecordPath.empty() ||
            !manifestPath.empty() || !unresolvedReportPath.empty() ||
            !reportPath.empty() || !templatePath.empty() || !dumpSoundMapPath.empty() ||
            !styleEditSheetPath.empty()) {
            std::cerr << "invalid flags: --batch-dir cannot be combined with single-file input/output/validation flags\n";
            return 1;
        }
        if (!target.empty() && target != "pa5x") {
            std::cerr << "unsupported target '" << target << "' (supported: pa5x)\n";
            return 1;
        }
    } else {
        if (strictBatch || flatBatch) {
            std::cerr << "invalid flags: --strict-batch and --flat require --batch-dir\n";
            return 1;
        }
        if (hasValidationBundle && !outBundleDir.empty()) {
            std::cerr << "invalid flags: --validation-bundle cannot be combined with --out-bundle\n";
            return 1;
        }
        if (input.empty()) return usage();
        if (output.empty() && outBundleDir.empty() && !hasValidationBundle && analyzeQualityArg.empty()) {
            return usage();
        }
        if (outBundleDir.empty() && !hasValidationBundle && analyzeQualityArg.empty() && target.empty()) {
            return usage();
        }
        if (!target.empty() && target != "pa5x") {
            std::cerr << "unsupported target '" << target << "' (supported: pa5x)\n";
            return 1;
        }
    }

    if (rawModeRequested && enhanceRecommended) {
        std::cerr << "invalid flags: --raw / --quality-mode raw cannot be combined with --enhance recommended\n";
        return 1;
    }
    if (!listeningRecordPath.empty() && !hasValidationBundle) {
        std::cerr << "invalid flags: --listening-record requires --validation-bundle <dir>\n";
        return 1;
    }

    // Fail-closed pre-validation of optional external human listening record JSON.
    quality::HumanListeningRecord listeningRecord{};
    if (!listeningRecordPath.empty()) {
        std::string rawJson;
        if (!readTextFile(fs::path(listeningRecordPath), rawJson)) {
            return 1;
        }
        const auto parsedListening = quality::parseHumanListeningRecordJson(rawJson);
        if (!parsedListening.ok) {
            std::cerr << "invalid --listening-record (" << listeningRecordPath << "): "
                      << parsedListening.error << "\n";
            return 1;
        }
        listeningRecord = parsedListening.record;
    }

    if (explicitAutoSoundMap && explicitNoAutoSoundMap) {
        std::cerr << "invalid flags: --auto-sound-map and --no-auto-sound-map are mutually exclusive\n";
        return 1;
    }
    if (rawModeRequested && explicitAutoSoundMap) {
        std::cerr << "invalid flags: --raw / --quality-mode raw cannot be combined with --auto-sound-map\n";
        return 1;
    }
    if (rawModeRequested && preferTargetUserSounds) {
        std::cerr << "invalid flags: --raw / --quality-mode raw cannot be combined with --prefer-target-user-sounds\n";
        return 1;
    }
    if (rawModeRequested && explicitQ5Enable) {
        std::cerr << "invalid flags: --raw / --quality-mode raw cannot be combined with Q5 normalization enable flags\n";
        return 1;
    }
    if (rawModeRequested && explicitBakeRubatoOn) {
        std::cerr << "invalid flags: --raw / --quality-mode raw cannot be combined with --bake-rubato\n";
        return 1;
    }
    if (preferTargetUserSounds && targetKstPath.empty()) {
        std::cerr << "invalid flags: --prefer-target-user-sounds requires --target-kst <dir>\n";
        return 1;
    }
    if (explicitNoAutoSoundMap && preferTargetUserSounds) {
        std::cerr << "invalid flags: --no-auto-sound-map cannot be combined with --prefer-target-user-sounds\n";
        return 1;
    }

    if (explicitAutoSoundMap) opt.enable_sound_matching = true;
    if (explicitNoAutoSoundMap) opt.enable_sound_matching = false;
    opt.prefer_target_user_sounds = preferTargetUserSounds;
    if (explicitBakeRubatoOn) opt.enable_bake_rubato = true;

    if (!soundMap.empty()) {
        std::ifstream in(soundMap);
        if (!in) { std::cerr << "cannot open sound map " << soundMap << "\n"; return 1; }
        auto sm = parseSoundMap(in, soundMap);
        if (!sm.ok) { std::cerr << sm.error << "\n"; return 1; }
        opt.sounds = std::move(sm.sounds);
        opt.element_sounds = std::move(sm.element_sounds);
    }

    // Optional strictly read-only Target KST inspection (Track A -> Track B adapter; NEVER writes to target KST).
    std::optional<quality::TargetKstReadOnlyBridge> targetBridge;
    if (!targetKstPath.empty()) {
        targetBridge = quality::inspectTargetKstReadOnly(targetKstPath);
        if (!targetBridge->valid) {
            std::cerr << "target KST inspection failed (read-only fail-closed): " << targetBridge->error << "\n";
            return 1;
        }
        opt.target_sound_catalog = &targetBridge->catalog;
        if (!targetBridge->drum_key_maps.empty()) {
            opt.target_drum_key_maps = &targetBridge->drum_key_maps;
        }
    }

    ConversionOptionsRecord optRecord{};
    optRecord.mode = rawModeRequested ? "raw" : "quality";
    optRecord.ppqn = opt.ppqn;
    optRecord.break_policy = (opt.break_policy == BreakPolicy::Yamaha1BarCap) ? "cap" : "withhold";
    optRecord.auto_sound_map = opt.enable_sound_matching;
    optRecord.prefer_target_user_sounds = opt.prefer_target_user_sounds;
    optRecord.bake_rubato = opt.enable_bake_rubato;
    optRecord.target_kst_used = !targetKstPath.empty();
    optRecord.manual_soundmap_used = !soundMap.empty();

    if (!isBatchMode) {
        if (!validationBundleDir.empty()) {
            std::error_code vec;
            const fs::path vRoot(validationBundleDir);
            if (fs::exists(vRoot, vec) && !fs::is_directory(vRoot, vec)) {
                std::cerr << "validation bundle target exists and is not a directory: " << validationBundleDir << "\n";
                return 1;
            }
            fs::create_directories(vRoot, vec);
            if (vec) {
                std::cerr << "cannot create validation bundle directory " << validationBundleDir
                          << ": " << vec.message() << "\n";
                return 1;
            }

            // Step 1-5: Always convert Mode A (Faithful) into candidate_b_faithful/ (NEVER mutated by Mode B).
            const fs::path candBDir = vRoot / "candidate_b_faithful";
            const auto resB = convertOneStyle(
                fs::path(input), opt, optRecord,
                output.empty() ? std::nullopt : std::optional<fs::path>(output),
                candBDir,
                reportPath.empty() ? std::nullopt : std::optional<fs::path>(reportPath),
                templatePath.empty() ? std::nullopt : std::optional<fs::path>(templatePath),
                dumpSoundMapPath.empty() ? std::nullopt : std::optional<fs::path>(dumpSoundMapPath),
                styleEditSheetPath.empty() ? std::nullopt : std::optional<fs::path>(styleEditSheetPath),
                manifestPath.empty() ? std::nullopt : std::optional<fs::path>(manifestPath),
                unresolvedReportPath.empty() ? std::nullopt : std::optional<fs::path>(unresolvedReportPath),
                false,
                /*applyEnhanceRecommended=*/false,
                /*captureValidationData=*/true);
            if (resB.exit_code != 0) return resB.exit_code;

            const std::string baseStyleName = fs::path(input).filename().string();

            // Step 6: 12-dimension read-only musical quality analysis on Candidate B (Mode A Faithful).
            const auto mqB = quality::analyzeMusicalQuality(
                resB.korg_style, &resB.map_report, &resB.source_uasf);
            if (!writeText(vRoot / "musical_quality_faithful.json", quality::formatMusicalQualityJson(mqB)) ||
                !writeText(vRoot / "musical_quality_faithful.md", quality::formatMusicalQualityMarkdown(mqB))) {
                return 1;
            }

            // Optional standalone --analyze-musical-quality sidecar if also requested.
            if (!analyzeQualityArg.empty()) {
                fs::path aJson, aMd;
                if (!resolveAnalyzeOutputPaths(analyzeQualityArg, aJson, aMd)) return 1;
                if (!writeText(aJson, quality::formatMusicalQualityJson(mqB)) ||
                    !writeText(aMd, quality::formatMusicalQualityMarkdown(mqB))) {
                    return 1;
                }
            }

            // Step 7-8: Opt-in Mode B Enhancement (ONLY when --enhance recommended is explicitly passed).
            std::optional<quality::EnhanceResult> enhC;
            std::optional<quality::MusicalQualityReport> mqC;
            std::optional<quality::MusicalQualityDelta> deltaBC;
            std::string shaC;
            uint32_t candidateHardViolations = 0;

            if (enhanceRecommended) {
                enhC = quality::enhanceStyle(
                    resB.korg_style, quality::EnhanceOptions::recommended(),
                    &resB.map_report, &resB.source_uasf);
                if (!enhC->ok) {
                    std::cerr << "Mode B enhancement failed (rolled back): " << enhC->error << "\n";
                    return 2;
                }
                const auto wC = write(enhC->style);
                if (!wC.ok) {
                    std::cerr << "Mode B SMF write failed: " << wC.error << "\n";
                    return 2;
                }
                const auto pC = parse(wC.bytes);
                const std::vector<std::string> errsC =
                    pC.ok ? validate(pC.style) : std::vector<std::string>{pC.error};
                if (!errsC.empty()) {
                    std::cerr << "Mode B SMF validation failed: " << errsC.front() << "\n";
                    return 2;
                }

                const fs::path candCDir = vRoot / "candidate_c_enhanced";
                fs::create_directories(candCDir, vec);
                if (vec) {
                    std::cerr << "cannot create candidate_c_enhanced directory: " << vec.message() << "\n";
                    return 1;
                }

                if (!writeBytes(candCDir / "converted.mid", wC.bytes)) return 1;
                shaC = manifest_detail::sha256Hex(wC.bytes.data(), wC.bytes.size());

                const std::string enhJson = quality::formatEnhancementAuditJson(*enhC);
                const std::string enhMd = quality::formatEnhancementAuditMarkdown(*enhC);
                if (!writeText(candCDir / "enhancement_actions.json", enhJson) ||
                    !writeText(candCDir / "enhancement_actions.md", enhMd)) {
                    return 1;
                }

                std::vector<ArtifactEntry> cArtifacts = {
                    {"smf", "converted.mid", static_cast<uint64_t>(wC.bytes.size()), shaC},
                    {"enhancement_actions_json", "enhancement_actions.json",
                     static_cast<uint64_t>(enhJson.size()), manifest_detail::sha256Hex(enhJson)},
                    {"enhancement_actions_md", "enhancement_actions.md",
                     static_cast<uint64_t>(enhMd.size()), manifest_detail::sha256Hex(enhMd)}
                };
                const std::string cManifest = formatEnhancedManifestJson(
                    baseStyleName, resB.manifest.input_sha256, resB.written_mid_sha256, *enhC, cArtifacts);
                if (!writeText(candCDir / "manifest.json", cManifest)) return 1;

                mqC = quality::analyzeMusicalQuality(enhC->style, &resB.map_report, &resB.source_uasf);
                if (!writeText(vRoot / "musical_quality_enhanced.json", quality::formatMusicalQualityJson(*mqC)) ||
                    !writeText(vRoot / "musical_quality_enhanced.md", quality::formatMusicalQualityMarkdown(*mqC))) {
                    return 1;
                }

                deltaBC = quality::compareMusicalQuality(mqB, *mqC);
                if (!writeText(vRoot / "comparison_b_vs_c.md",
                               formatComparisonBvsCMd(baseStyleName, mqB, *mqC, *deltaBC, *enhC,
                                                      resB.written_mid_sha256, shaC))) {
                    return 1;
                }
            }

            // Step 9: Proposed Production Acceptance Gate (Grade A / B / C / D / U).
            const quality::MusicalQualityReport* targetMq = enhanceRecommended && mqC.has_value() ? &(*mqC) : &mqB;
            const auto offlineEv = quality::buildOfflineAcceptanceEvidence(
                baseStyleName, resB.manifest, targetMq, enhanceRecommended, candidateHardViolations);
            const auto acceptance = quality::evaluateAcceptanceGate(offlineEv, listeningRecord);
            if (!writeText(vRoot / "acceptance_report.json", quality::formatAcceptanceReportJson(acceptance)) ||
                !writeText(vRoot / "acceptance_report.md", quality::formatAcceptanceReportMd(acceptance))) {
                return 1;
            }

            // Step 10: Offline-Only Hardware Session Staging (ZERO hardware writes, ZERO CoreMIDI/USB).
            const fs::path stageDir = vRoot / "hardware_session_staging";
            fs::create_directories(stageDir, vec);
            if (vec) {
                std::cerr << "cannot create hardware_session_staging directory: " << vec.message() << "\n";
                return 1;
            }

            const std::string editSheetSha =
                manifest_detail::sha256Hex(resB.map_report.style_edit_sheet_md);
            std::ostringstream shaSums;
            shaSums << resB.written_mid_sha256 << "  ../candidate_b_faithful/converted.mid\n";
            shaSums << editSheetSha << "  style_edit_sheet.md\n";
            if (enhanceRecommended) {
                shaSums << shaC << "  ../candidate_c_enhanced/converted.mid\n";
            }
            if (!writeText(stageDir / "SHA256SUMS.txt", shaSums.str()) ||
                !writeText(stageDir / "style_edit_sheet.md", resB.map_report.style_edit_sheet_md) ||
                !writeText(stageDir / "OPERATOR_PREP_NOTES.md",
                           formatOperatorPrepNotesMd(baseStyleName, enhanceRecommended,
                                                     resB.written_mid_sha256, shaC)) ||
                !writeText(stageDir / "LISTENING_RECORD_TEMPLATE.json",
                           quality::formatListeningRecordTemplateJson(
                               baseStyleName, acceptance.candidate_label,
                               enhanceRecommended ? shaC : resB.written_mid_sha256))) {
                return 1;
            }

            // Top-level 10-step summary tying all artifacts together.
            if (!writeText(vRoot / "VALIDATION_SUMMARY.md",
                           formatValidationSummaryMd(
                               baseStyleName, resB.manifest, mqB, enhanceRecommended,
                               mqC.has_value() ? &(*mqC) : nullptr,
                               deltaBC.has_value() ? &(*deltaBC) : nullptr,
                               enhC.has_value() ? &(*enhC) : nullptr,
                               acceptance, resB.written_mid_sha256, shaC))) {
                return 1;
            }

            std::cout << "Wrote validation bundle " << vRoot.string()
                      << " | Grade: " << quality::acceptanceGradeDisplay(acceptance.grade)
                      << " (" << acceptance.criteria_status << ")"
                      << " | hardwareVerified=" << (acceptance.hardware_verified ? "true" : "false") << "\n";
            return 0;
        }

        const bool needValidationCapture = !analyzeQualityArg.empty();
        const auto res = convertOneStyle(
            fs::path(input), opt, optRecord,
            output.empty() ? std::nullopt : std::optional<fs::path>(output),
            outBundleDir.empty() ? std::nullopt : std::optional<fs::path>(outBundleDir),
            reportPath.empty() ? std::nullopt : std::optional<fs::path>(reportPath),
            templatePath.empty() ? std::nullopt : std::optional<fs::path>(templatePath),
            dumpSoundMapPath.empty() ? std::nullopt : std::optional<fs::path>(dumpSoundMapPath),
            styleEditSheetPath.empty() ? std::nullopt : std::optional<fs::path>(styleEditSheetPath),
            manifestPath.empty() ? std::nullopt : std::optional<fs::path>(manifestPath),
            unresolvedReportPath.empty() ? std::nullopt : std::optional<fs::path>(unresolvedReportPath),
            false,
            enhanceRecommended,
            needValidationCapture);
        if (res.exit_code != 0) return res.exit_code;

        if (!analyzeQualityArg.empty() && res.has_validation_data) {
            fs::path aJson, aMd;
            if (!resolveAnalyzeOutputPaths(analyzeQualityArg, aJson, aMd)) return 1;
            const auto mq = quality::analyzeMusicalQuality(
                res.korg_style, &res.map_report, &res.source_uasf);
            if (!writeText(aJson, quality::formatMusicalQualityJson(mq)) ||
                !writeText(aMd, quality::formatMusicalQualityMarkdown(mq))) {
                return 1;
            }
            std::cout << "Wrote musical quality report " << aJson.string()
                      << " & " << aMd.string() << "\n";
        }
        return 0;
    }

    // ── Batch Directory Conversion Mode ──
    std::error_code ec;
    const fs::path inRoot(batchDir);
    const fs::path outRoot(outDir);
    if (!fs::exists(inRoot, ec) || !fs::is_directory(inRoot, ec)) {
        std::cerr << "batch input directory does not exist or is not a directory: " << batchDir << "\n";
        return 1;
    }
    if (fs::exists(outRoot, ec) && !fs::is_directory(outRoot, ec)) {
        std::cerr << "batch output path exists and is not a directory: " << outDir << "\n";
        return 1;
    }
    fs::create_directories(outRoot, ec);
    if (ec) {
        std::cerr << "cannot create batch output directory " << outDir << ": " << ec.message() << "\n";
        return 1;
    }

    std::vector<std::pair<std::string, fs::path>> discovered;
    auto collectEntry = [&](const fs::directory_entry& de) {
        std::error_code eec;
        if (de.is_symlink(eec)) return;
        if (!de.is_regular_file(eec)) return;
        if (!isYamahaStyleExtension(de.path())) return;
        const fs::path rel = fs::relative(de.path(), inRoot, eec);
        if (eec) return;
        discovered.emplace_back(rel.generic_string(), de.path());
    };
    if (flatBatch) {
        for (const auto& de : fs::directory_iterator(inRoot, ec)) collectEntry(de);
    } else {
        for (const auto& de : fs::recursive_directory_iterator(inRoot, fs::directory_options::skip_permission_denied, ec)) {
            collectEntry(de);
        }
    }
    if (ec) {
        std::cerr << "error scanning batch input directory " << batchDir << ": " << ec.message() << "\n";
        return 1;
    }
    std::sort(discovered.begin(), discovered.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (discovered.empty()) {
        std::cerr << "no Yamaha style files (.sty/.prs/.sst/.bcs/.pcs/.fps) found in " << batchDir << "\n";
        return 1;
    }

    BatchManifest batch{};
    batch.options = optRecord;
    batch.total_files = static_cast<uint32_t>(discovered.size());

    std::set<std::string> usedStems;
    for (const auto& [relStr, fullPath] : discovered) {
        std::string stem;
        if (!sanitizeBatchStem(fs::path(relStr), stem)) {
            BatchItemSummary item{};
            item.input_relative_path = relStr;
            item.ok = false;
            item.error = "rejected unsafe relative path stem";
            ++batch.failed;
            batch.items.push_back(std::move(item));
            continue;
        }
        std::string uniqueStem = stem;
        for (int suffix = 2; !usedStems.insert(uniqueStem).second; ++suffix) {
            uniqueStem = stem + "_" + std::to_string(suffix);
        }

        const fs::path itemDir = outRoot / uniqueStem;
        const auto res = convertOneStyle(
            fullPath, opt, optRecord,
            std::nullopt,
            itemDir,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            true,
            enhanceRecommended,
            false);

        BatchItemSummary item{};
        item.input_relative_path = relStr;
        item.output_subdir = uniqueStem;
        item.input_size_bytes = res.manifest.input_size_bytes;
        item.input_sha256 = res.manifest.input_sha256;
        item.ok = (res.exit_code == 0 && res.manifest.ok);
        item.error = res.manifest.error;
        item.elements_exported = res.manifest.elements_exported;
        item.warning_count = static_cast<uint32_t>(res.manifest.warnings.size() + res.manifest.reader_warnings.size());

        for (const auto& tr : res.manifest.track_resolutions) {
            if (tr.status == "USER_OVERRIDE" || tr.status == "EXACT" || tr.status == "APPROX") {
                ++item.resolved_tracks;
            } else if (tr.status == "UNSUPPORTED") {
                ++item.unsupported_tracks;
            } else {
                ++item.unresolved_tracks;
            }
        }
        batch.total_unresolved_tracks += item.unresolved_tracks + item.unsupported_tracks;

        if (item.ok) {
            ++batch.succeeded;
            item.manifest_relative_path = uniqueStem + "/manifest.json";
        } else {
            ++batch.failed;
            // Record failure manifest cleanly inside subdir so every batch entry is auditable.
            std::error_code dec;
            fs::create_directories(itemDir, dec);
            if (!dec && writeText(itemDir / "manifest.json", formatConversionManifestJson(res.manifest))) {
                item.manifest_relative_path = uniqueStem + "/manifest.json";
            }
        }
        batch.items.push_back(std::move(item));
    }

    if (!writeText(outRoot / "batch_manifest.json", formatBatchManifestJson(batch))) return 1;
    if (!writeText(outRoot / "BATCH_SUMMARY.md", formatBatchSummaryMd(batch))) return 1;

    std::cout << "Batch conversion complete: " << batch.succeeded << "/" << batch.total_files
              << " succeeded, " << batch.failed << " failed, "
              << batch.total_unresolved_tracks << " unresolved/unsupported tracks\n"
              << "Summary: " << (outRoot / "BATCH_SUMMARY.md").string() << "\n"
              << "Manifest: " << (outRoot / "batch_manifest.json").string() << "\n";

    if (batch.failed > 0) return 2;
    if (strictBatch && batch.total_unresolved_tracks > 0) return 2;
    return 0;
}
