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
#include "exporters/korg/conversion_manifest.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/pa5x_sound_matcher.h"
#include "exporters/korg/sff1_route.h"
#include "exporters/korg/sound_map.h"
#include "exporters/korg/target_kst_drum_adapter.h"
#include "exporters/korg/timbre_profiler.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
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
                 "       aiarranger-convert --input <file.sty> --out-bundle <dir> [options...]\n"
                 "       aiarranger-convert --batch-dir <in_dir> --out-dir <out_dir> [--strict-batch] [--flat] [options...]\n"
                 "       aiarranger-convert --compare <ours.mid> --with <pa5x_export.mid>\n"
                 "sound-map lines: <track> <msb> <lsb> <pc> | <section>.<track> <msb> <lsb> <pc> | <key> unresolved\n";
    return 1;
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
    bool quietStdout) {
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

    // Write -> re-parse with the independent reader -> validate BEFORE writing any output files.
    const auto written = write(mapped.style);
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

    outcome.exit_code = 0;
    return outcome;
}

} // namespace

int main(int argc, char** argv) {
    std::string input, output, target, soundMap, reportPath, templatePath, dumpSoundMapPath,
        styleEditSheetPath, targetKstPath, compareA, compareB;
    std::string outBundleDir, batchDir, outDir, manifestPath, unresolvedReportPath;
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
    if (!compareA.empty() || !compareB.empty()) {
        if (compareA.empty() || compareB.empty() || !input.empty() || !batchDir.empty() || !outBundleDir.empty()) {
            return usage();
        }
        return compareMode(compareA, compareB);
    }

    const bool isBatchMode = !batchDir.empty() || !outDir.empty();
    if (isBatchMode) {
        if (batchDir.empty() || outDir.empty()) {
            std::cerr << "invalid flags: --batch-dir and --out-dir must both be specified\n";
            return 1;
        }
        if (!input.empty() || !output.empty() || !outBundleDir.empty() ||
            !manifestPath.empty() || !unresolvedReportPath.empty() ||
            !reportPath.empty() || !templatePath.empty() || !dumpSoundMapPath.empty() ||
            !styleEditSheetPath.empty()) {
            std::cerr << "invalid flags: --batch-dir cannot be combined with single-file input/output flags\n";
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
        if (input.empty()) return usage();
        if (output.empty() && outBundleDir.empty()) return usage();
        if (outBundleDir.empty() && target.empty()) return usage();
        if (!target.empty() && target != "pa5x") {
            std::cerr << "unsupported target '" << target << "' (supported: pa5x)\n";
            return 1;
        }
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
            false);
        return res.exit_code;
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
            true);

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
