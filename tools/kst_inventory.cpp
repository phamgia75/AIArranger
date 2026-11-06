// kst-inventory <folder>: read-only report on a KST folder (roadmap P1.9).
// Run it on a COPY of the data. Exit 0 = report printed, 1 = usage/scan error.
#include "media/kst_inventory.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: kst-inventory <folder>\n");
        return 1;
    }
    const auto inv = ai_arranger::media::scan(argv[1]);
    std::fputs(ai_arranger::media::format(inv, argv[1]).c_str(), inv.ok ? stdout : stderr);
    return inv.ok ? 0 : 1;
}
