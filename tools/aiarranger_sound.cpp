// aiarranger-sound (roadmap S4): writes the USB-ready package folder (media/sound/package_writer.h:
// .sf2, tx/ TXPROG, wav/, REPORT.txt). Refuses to write into an existing DIR.
//   demo --out DIR                                    the original demo organ
//   drums --out DIR                                   the original demo Drum Kit (.txperf)
//   wav --in SRC --name NAME [--layers a,b --splits 90] --out DIR
//       a folder of <layer>_<OrgKey>_<LowKey>_<HighKey>.wav (route C, media/sound/wav_folder_source.h);
//       SRC is only read.
// Exit 0 = written, 1 = usage/IO/validation error.
#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include "media/sound/package_writer.h"
#include "media/sound/wav_folder_source.h"
#include <cstdio>
#include <map>
#include <sstream>
#include <string>

using namespace ai_arranger::media::sound;

static int usage() {
    std::fprintf(stderr, "usage: aiarranger-sound demo --out DIR\n"
                         "       aiarranger-sound drums --out DIR\n"
                         "       aiarranger-sound wav --in SRC --name NAME [--layers a,b --splits 90] --out DIR\n"
                         "       (DIR must not exist)\n");
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 2 || argc % 2) return usage();
    const std::string cmd = argv[1];
    std::map<std::string, std::string> opt;
    for (int i = 2; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
    if (!opt.count("--out")) return usage();
    Instrument in;
    std::string notes;
    if (cmd == "drums" && opt.size() == 1) {
        std::string err;
        const std::string rep = writeDrumPackage(buildDemoKit(), opt["--out"], "Source: original synthesized drums (no recorded audio)\n", &err);
        if (rep.empty()) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        std::fputs(rep.c_str(), stdout);
        return 0;
    }
    if (cmd == "demo" && opt.size() == 1) {
        double worst = 0;
        in = buildDemoOrgan(&worst);
        char buf[96];
        std::snprintf(buf, sizeof buf, "Source: original additive synthesis, looped; worst pitch error %.4f cents\n", worst);
        notes = buf;
    } else if (cmd == "wav" && opt.count("--in") && opt.count("--name")) {
        std::vector<std::string> layers;
        std::vector<uint8_t> splits;
        std::stringstream ls(opt["--layers"]), ss(opt["--splits"]);
        for (std::string t; std::getline(ls, t, ',');) layers.push_back(t);
        for (std::string t; std::getline(ss, t, ',');) {
            if (t.empty() || t.size() > 3 || t.find_first_not_of("0123456789") != std::string::npos || std::stoi(t) > 127) return usage();
            splits.push_back(static_cast<uint8_t>(std::stoi(t)));
        }
        if (const auto e = buildFromWavFolder(opt["--in"], opt["--name"], layers, splits, &in); !e.empty()) {
            std::fprintf(stderr, "%s\n", e.c_str());
            return 1;
        }
        notes = "Source: WAV folder " + opt["--in"] + " (root and loop from each WAV's smpl chunk)\n";
    } else {
        return usage();
    }
    std::string err;
    const std::string rep = writePackage(in, opt["--out"], notes, &err);
    if (rep.empty()) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::fputs(rep.c_str(), stdout);
    return 0;
}
