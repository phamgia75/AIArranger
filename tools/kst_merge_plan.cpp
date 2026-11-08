// kst-merge-plan SRC DST: read-only prediction of what loading KST folder SRC onto the User data saved
// in KST folder DST would replace, add, duplicate or delete, under the Pa5X's documented load rules
// (manual v1.4 pp.734-736). Nothing is copied, loaded or modified.
// Exit 0 = no existing data would be replaced, 1 = something would be replaced/deleted, 2 = error.
#include "media/kst_diff.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: kst-merge-plan <SRC folder to load> <DST folder: saved current User data>\n"); return 2; }
    const auto d = ai_arranger::media::diff(argv[2], argv[1]);   // A = existing (DST), B = incoming (SRC)
    const bool pcm = ai_arranger::media::hasPcmFolder(argv[1]);
    const std::string out = ai_arranger::media::formatMergePlan(d, argv[1], argv[2], pcm);
    std::fputs(out.c_str(), d.ok ? stdout : stderr);
    return !d.ok ? 2 : out.find("RESULT: no existing") != std::string::npos ? 0 : 1;
}
