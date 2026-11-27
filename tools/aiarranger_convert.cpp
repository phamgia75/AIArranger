// aiarranger-convert: Yamaha Genos .sty -> Korg Pa5X Style SMF (separated by markers).
//
//   aiarranger-convert --input Style.sty --target pa5x --output Style.mid
//                      [--ppqn N] [--sound-map FILE] [--sound-template FILE] [--report FILE]
//   aiarranger-convert --compare Style.mid --with Pa5X_Export.mid
//
// Load the output on Pa5X in Style Edit > Menu > Import/Export > Import SMF
// (Initialize checked, SHIFT + Execute imports every marker in one pass).
// --sound-template writes a fill-in sound map for this style (all unresolved).
// --compare checks a Pa5X re-export (Export SMF) against our file.
// Exit: 0 = written and validated / round trip equal, 1 = usage or input
// error, 2 = conversion or validation failed (no file written) / round trip
// differs.

#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/pa5x_sound_matcher.h"
#include "exporters/korg/sff1_route.h"
#include "exporters/korg/sound_map.h"
#include "exporters/korg/target_kst_drum_adapter.h"
#include "exporters/korg/timbre_profiler.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;

namespace {
int usage() {
    std::cerr << "usage: aiarranger-convert --input <file.sty> --target pa5x --output <file.mid>\n"
                 "                          [--ppqn N] [--sound-map FILE] [--sound-template FILE] [--report FILE]\n"
                 "                          [--break-policy <cap|withhold>] [--quality-mode <quality|raw>] [--raw]\n"
                 "                          [--megavoice-mode <auto|sanitize|preserve>] [--drum-remap <on|off>]\n"
                 "                          [--cv-split <on|off>] [--normalize-source-chord <on|off>]\n"
                 "                          [--auto-sound-map | --no-auto-sound-map]\n"
                 "                          [--target-kst <dir>] [--prefer-target-user-sounds] [--dump-soundmap FILE]\n"
                 "       aiarranger-convert --compare <ours.mid> --with <pa5x_export.mid>\n"
                 "sound-map lines: <bass|drum|perc|acc1..acc5> <msb> <lsb> <pc> | <track> unresolved   (# comments)\n";
    return 1;
}

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text;
    f.close();
    if (!f) std::cerr << "cannot write " << path << "\n";
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
} // namespace

int main(int argc, char** argv) {
    std::string input, output, target, soundMap, reportPath, templatePath, dumpSoundMapPath, targetKstPath, compareA, compareB;
    MapOptions opt;
    bool rawModeRequested = false;
    bool explicitAutoSoundMap = false;
    bool explicitNoAutoSoundMap = false;
    bool preferTargetUserSounds = false;

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

        if (i + 1 >= argc) return usage();
        const std::string v = argv[++i];
        if (a == "--input") input = v;
        else if (a == "--output") output = v;
        else if (a == "--target") target = v;
        else if (a == "--sound-map") soundMap = v;
        else if (a == "--report") reportPath = v;
        else if (a == "--sound-template") templatePath = v;
        else if (a == "--dump-soundmap") dumpSoundMapPath = v;
        else if (a == "--target-kst") targetKstPath = v;
        else if (a == "--compare") compareA = v;
        else if (a == "--with") compareB = v;
        else if (a == "--ppqn") {
            try {
                size_t used = 0;
                const long n = std::stol(v, &used);
                if (used != v.size() || n < 1 || n > 0x7FFF) return usage();
                opt.ppqn = static_cast<uint16_t>(n);
            }
            catch (...) { return usage(); }
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
            } else if (v == "quality") {
                rawModeRequested = false;
                opt.enable_cv_split = true;
                opt.enable_drum_remap = true;
                opt.megavoice_mode = quality::MegaVoiceMode::Auto;
                opt.enable_source_chord_normalization = true;
                opt.enable_sound_matching = true;
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
        } else return usage();
    }
    if (!compareA.empty() || !compareB.empty()) {
        if (compareA.empty() || compareB.empty() || !input.empty()) return usage();
        return compareMode(compareA, compareB);
    }
    if (input.empty() || output.empty() || target.empty()) return usage();
    if (target != "pa5x") { std::cerr << "unsupported target '" << target << "' (supported: pa5x)\n"; return 1; }

    // Fail-closed CLI flag-combination checks.
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

    if (!soundMap.empty()) {
        std::ifstream in(soundMap);
        if (!in) { std::cerr << "cannot open sound map " << soundMap << "\n"; return 1; }
        auto sm = parseSoundMap(in, soundMap);
        if (!sm.ok) { std::cerr << sm.error << "\n"; return 1; }
        opt.sounds = std::move(sm.sounds);
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

    importers::sff1::Sff1Reader reader;
    const auto parsed = reader.parseFile(input);
    if (!parsed.success) { std::cerr << "import failed: " << parsed.error << "\n"; return 1; }
    importers::sff1::Sff1ToUasfMapper toUasf;
    const auto canon = toUasf.map(parsed);
    if (!canon.success) { std::cerr << "import failed: " << canon.error << "\n"; return 1; }

    opt.route = routeFromCasm(parsed);
    const std::string baseStyleName = std::filesystem::path(input).filename().string();
    if (!dumpSoundMapPath.empty()) {
        opt.capture_soundmap_dump = true;
        opt.soundmap_style_name = baseStyleName;
    }

    const auto mapped = mapToKorg(canon.style, opt);
    if (!mapped.ok) { std::cerr << "conversion failed: " << mapped.error << "\n"; return 2; }
    if (!templatePath.empty() && !writeText(templatePath, soundMapTemplate(mapped.report, baseStyleName))) return 1;
    if (!dumpSoundMapPath.empty() && !writeText(dumpSoundMapPath, mapped.report.soundmap_dump)) return 1;

    // Write -> re-parse with the independent reader -> validate what was written.
    const auto written = write(mapped.style);
    if (!written.ok) { std::cerr << "write failed: " << written.error << "\n"; return 2; }
    const auto back = parse(written.bytes);
    std::vector<std::string> errs = back.ok ? validate(back.style) : std::vector<std::string>{back.error};

    std::string report = "Input: " + input + "\nTarget: Korg Pa5X Style SMF separated by markers (format 0)\n" +
                         formatReport(mapped.report, mapped.style) + "\nValidator: " +
                         (errs.empty() ? "PASS (" + std::to_string(back.style.elements.size()) + " elements)" : "FAIL") + "\n";
    for (const auto& e : errs) report += "  " + e + "\n";
    std::cout << report;
    if (!reportPath.empty() && !writeText(reportPath, report)) return 1;
    if (!errs.empty()) return 2;

    std::ofstream out(output, std::ios::binary);
    out.write(reinterpret_cast<const char*>(written.bytes.data()), static_cast<std::streamsize>(written.bytes.size()));
    out.close();
    if (!out) { std::cerr << "cannot write " << output << "\n"; return 1; }
    std::cout << "Wrote " << output << " (" << written.bytes.size() << " bytes)\n";
    return 0;
}
