// kst-inventory <folder> [--budget-bytes N]: read-only report on a KST folder
// (roadmap P1.9) with a sample-data disk-size estimate (P2.3).
// Run it on a COPY of the data. Exit 0 = report printed, 1 = usage/scan error.
#include "media/kst_inventory.h"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    uintmax_t budget = ai_arranger::media::kPaManagerPa5xBudgetBytes;
    if (!(argc == 2 || (argc == 4 && !std::strcmp(argv[2], "--budget-bytes") &&
                        ai_arranger::media::parseBudgetBytes(argv[3], budget)))) {
        std::fprintf(stderr, "usage: kst-inventory <folder> [--budget-bytes N]\n");
        return 1;
    }
    const auto inv = ai_arranger::media::scan(argv[1]);
    std::fputs(ai_arranger::media::format(inv, argv[1], budget).c_str(), inv.ok ? stdout : stderr);
    return inv.ok ? 0 : 1;
}
