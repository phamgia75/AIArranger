// aiarranger-convert: Yamaha Genos .sty -> Korg Pa5X Style SMF (separated by markers).
//
//   aiarranger-convert --input Style.sty --target pa5x --output Style.mid
//                      [--ppqn N] [--sound-map FILE] [--report FILE]
//
// Load the output on Pa5X in Style Edit > Menu > Import/Export > Import SMF
// (Initialize checked, SHIFT + Execute imports every marker in one pass).
// Exit: 0 = written and validated, 1 = usage/input error, 2 = conversion or
// validation failed (no file written).

#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/sff1_route.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;

namespace {
int usage() {
    std::cerr << "usage: aiarranger-convert --input <file.sty> --target pa5x --output <file.mid>\n"
                 "                          [--ppqn N] [--sound-map FILE] [--report FILE]\n"
                 "sound-map lines: <bass|drum|perc|acc1..acc5> <msb> <lsb> <pc>   (# comments)\n";
    return 1;
}

bool parseSoundMap(const std::string& path, std::map<uint8_t, SoundRef>& out, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open sound map " + path; return false; }
    static const std::map<std::string, uint8_t> kTracks = {
        {"bass", kBassCh}, {"drum", kDrumCh}, {"perc", kPercCh},
        {"acc1", 11}, {"acc2", 12}, {"acc3", 13}, {"acc4", 14}, {"acc5", 15}};
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::string track;
        int msb, lsb, pc;
        if (!(ss >> track)) continue;
        std::string extra;
        auto it = kTracks.find(track);
        if (it == kTracks.end() || !(ss >> msb >> lsb >> pc) || (ss >> extra) ||
            msb < 0 || msb > 127 || lsb < 0 || lsb > 127 || pc < 0 || pc > 127) {
            err = path + ":" + std::to_string(n) + ": expected '<track> <msb 0-127> <lsb 0-127> <pc 0-127>'";
            return false;
        }
        out[it->second] = {static_cast<uint8_t>(msb), static_cast<uint8_t>(lsb), static_cast<uint8_t>(pc)};
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    std::string input, output, target, soundMap, reportPath;
    MapOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) return usage();
        const std::string v = argv[++i];
        if (a == "--input") input = v;
        else if (a == "--output") output = v;
        else if (a == "--target") target = v;
        else if (a == "--sound-map") soundMap = v;
        else if (a == "--report") reportPath = v;
        else if (a == "--ppqn") {
            try {
                size_t used = 0;
                const long n = std::stol(v, &used);
                if (used != v.size() || n < 1 || n > 0x7FFF) return usage();
                opt.ppqn = static_cast<uint16_t>(n);
            }
            catch (...) { return usage(); }
        } else return usage();
    }
    if (input.empty() || output.empty() || target.empty()) return usage();
    if (target != "pa5x") { std::cerr << "unsupported target '" << target << "' (supported: pa5x)\n"; return 1; }

    std::string err;
    if (!soundMap.empty() && !parseSoundMap(soundMap, opt.sounds, err)) { std::cerr << err << "\n"; return 1; }

    importers::sff1::Sff1Reader reader;
    const auto parsed = reader.parseFile(input);
    if (!parsed.success) { std::cerr << "import failed: " << parsed.error << "\n"; return 1; }
    importers::sff1::Sff1ToUasfMapper toUasf;
    const auto canon = toUasf.map(parsed);
    if (!canon.success) { std::cerr << "import failed: " << canon.error << "\n"; return 1; }

    opt.route = routeFromCasm(parsed);
    const auto mapped = mapToKorg(canon.style, opt);
    if (!mapped.ok) { std::cerr << "conversion failed: " << mapped.error << "\n"; return 2; }

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
    if (!reportPath.empty()) {
        std::ofstream rf(reportPath);
        rf << report;
        rf.close();
        if (!rf) { std::cerr << "cannot write report " << reportPath << "\n"; return 1; }
    }
    if (!errs.empty()) return 2;

    std::ofstream out(output, std::ios::binary);
    out.write(reinterpret_cast<const char*>(written.bytes.data()), static_cast<std::streamsize>(written.bytes.size()));
    out.close();
    if (!out) { std::cerr << "cannot write " << output << "\n"; return 1; }
    std::cout << "Wrote " << output << " (" << written.bytes.size() << " bytes)\n";
    return 0;
}
