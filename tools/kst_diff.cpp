// kst-diff A B: read-only, byte-level comparison of two KST folders (or any folders): identical,
// changed, added, removed and moved files. File contents are not interpreted. Use it to check a
// backup against its source or to see what changed between two saves.
// Exit 0 = same content, 1 = different (or skipped entries not compared), 2 = usage/scan/read error.
#include "media/kst_diff.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: kst-diff <folderA> <folderB>\n"); return 2; }
    const auto d = ai_arranger::media::diff(argv[1], argv[2]);
    std::fputs(ai_arranger::media::format(d, argv[1], argv[2]).c_str(), d.ok ? stdout : stderr);
    return !d.ok ? 2 : d.same() ? 0 : 1;
}
