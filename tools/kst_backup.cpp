// kst-backup SRC DST: verified backup of a KST folder into the NEW folder DST (media/kst_backup.h).
// kst-backup --verify DST: re-check DST against DST.manifest.sha256 (also reads tools/kst_copy.py manifests).
// Files are copied as opaque bytes; the source is only read. Exit 0 = done/verified, 1 = refused or failed.
#include "media/kst_backup.h"
#include <cstring>

int main(int argc, char** argv) {
    using namespace ai_arranger::media;
    if (argc == 3 && std::strcmp(argv[1], "--verify") == 0) {
        const auto bad = verifyBackup(argv[2]);
        if (!bad.empty()) { std::fputs(bad.c_str(), stderr); return 1; }
        std::printf("verified against %s\n", backupManifestPath(argv[2]).c_str());
        return 0;
    }
    if (argc != 3 || argv[1][0] == '-') { std::fprintf(stderr, "usage: kst-backup SRC DST | kst-backup --verify DST\n"); return 1; }
    const auto r = backupKst(argv[1], argv[2]);
    std::fputs(r.message.c_str(), r.ok ? stdout : stderr);
    if (!r.ok) std::fputc('\n', stderr);
    return r.ok ? 0 : 1;
}
