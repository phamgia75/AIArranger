// kst-merge-plan SRC DST: read-only, FILE-LEVEL prediction of what loading KST folder SRC onto the User
// data saved in KST folder DST would replace, add, duplicate or delete, under the Pa5X's documented
// load rules (manual v1.4 pp.734-736). Sound/sample dependencies are not analysed. Nothing is
// copied, loaded or modified.
// Exit 0 = neither Merge nor Replace All would replace or delete existing data,
//      1 = something would be replaced or deleted (or entries were skipped), 2 = error.
#include "media/kst_diff.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: kst-merge-plan <SRC folder to load> <DST folder: saved current User data>\n"); return 2; }
    const auto p = ai_arranger::media::planMerge(argv[1], argv[2]);
    std::fputs(ai_arranger::media::format(p, argv[1], argv[2]).c_str(), p.ok ? stdout : stderr);
    return !p.ok ? 2 : p.replaceAllSafe() ? 0 : 1;
}
