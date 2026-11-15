// sf2-check <file.sf2>: read-only report of SF2 Instruments with overlapping
// zones (roadmap P3.5). Exit 0 = none, 2 = some found, 1 = usage/parse error.
#include "media/sf2_check.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: sf2-check <file.sf2>\n");
        return 1;
    }
    const auto r = ai_arranger::media::sf2::checkFile(argv[1]);
    std::fputs(ai_arranger::media::sf2::format(r).c_str(), r.ok ? stdout : stderr);
    return !r.ok ? 1 : r.overlapping() ? 2 : 0;
}
