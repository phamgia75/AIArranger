// kst-inventory <folder> [--budget-bytes N]: read-only report on a KST folder
// (roadmap P1.9) with a sample-data disk-size estimate (P2.3).
// Run it on a COPY of the data. Exit 0 = report printed, 1 = usage/scan error.
#include "media/kst_inventory.h"
#include <cstdio>
#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    uintmax_t budget = ai_arranger::media::kPaManagerPa5xBudgetBytes;
    char* end = nullptr;
    if (argc == 4 && !std::strcmp(argv[2], "--budget-bytes") && argv[3][0] >= '0' && argv[3][0] <= '9')
        errno = 0, budget = std::strtoumax(argv[3], &end, 10);
    if (!(argc == 2 || (argc == 4 && end && !*end && errno != ERANGE && budget > 0))) {
        std::fprintf(stderr, "usage: kst-inventory <folder> [--budget-bytes N]\n");
        return 1;
    }
    const auto inv = ai_arranger::media::scan(argv[1]);
    std::fputs(ai_arranger::media::format(inv, argv[1], budget).c_str(), inv.ok ? stdout : stderr);
    return inv.ok ? 0 : 1;
}
