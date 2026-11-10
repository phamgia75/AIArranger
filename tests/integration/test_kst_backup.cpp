// Verified KST backup (media/kst_backup.h): SHA-256 vectors, a byte-identical copy with its manifest beside it,
// the source untouched, every refusal, cancel, a source changing mid-copy, and verify spotting damage.
// Never an incomplete folder presented as a backup.

#include "media/kst_backup.h"
#include "media/kst_diff.h"
#include <chrono>
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger::media;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static void put(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}
static std::string sha(const std::string& s) { Sha256 h; h.update(s.data(), s.size()); return h.hex(); }
// Path, size, mtime and content hash of every entry: proves a folder was not touched.
static std::string snapshot(const fs::path& root) {
    std::vector<std::string> rows;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        std::error_code ec;
        std::string row = e.path().string();
        if (fs::symlink_status(e.path()).type() == fs::file_type::regular) {
            std::ifstream in(e.path(), std::ios::binary);
            row += "|" + std::to_string(e.file_size()) + "|" +
                   std::to_string(static_cast<long long>(fs::last_write_time(e.path(), ec).time_since_epoch().count())) + "|" +
                   sha(std::string(std::istreambuf_iterator<char>(in), {}));
        }
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end());
    std::string s;
    for (const auto& r : rows) s += r + "\n";
    return s;
}
// A cancelled or failed backup is never published and never deleted: it stays, marked, as DST.INCOMPLETE.
static bool keptIncomplete(const fs::path& dst) {
    return !fs::exists(dst) && !fs::exists(backupManifestPath(dst)) && fs::is_directory(dst.string() + ".INCOMPLETE");
}

int main() {
    std::printf("Test: verified KST backup\n");
    TEST("sha256: empty, abc, 448-bit and 1M x 'a' (FIPS 180-4 vectors)",
         sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
         sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
         sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" &&
         sha(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    const fs::path root = fs::temp_directory_path() / ("aia-kstbackup-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path src = root / "MY.SET";
    std::string big(3 << 20, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char((i * 2654435761u) >> 13);
    put(src / "STYLE/BANK01.STY", "style-1");
    put(src / "PCM/RAM01.PCM", big);   // several 1 MB chunks: progress inside one file
    put(src / "SOUND/USER 01.PCG", "");
    fs::create_directories(src / "PAD/empty");
    const std::string before = snapshot(src);

    const fs::path dst = root / "backups/MY.SET backup";
    fs::create_directories(dst.parent_path());
    std::vector<BackupProgress> seen;
    const auto r = backupKst(src, dst, [&](const BackupProgress& p) { seen.push_back(p); return true; });
    TEST("backup succeeds and reports files and bytes", r.ok && !r.cancelled && r.files == 3 && r.bytes == big.size() + 7 &&
                                                        r.message.rfind("Backup complete and verified", 0) == 0);
    TEST("backup is byte-identical to the source, empty folders kept", diff(src, dst).same() && fs::is_directory(dst / "PAD/empty"));
    TEST("source untouched (paths, sizes, times, bytes)", snapshot(src) == before);
    TEST("manifest beside the backup, not inside it; no INCOMPLETE left",
         fs::is_regular_file(r.manifest) && r.manifest == backupManifestPath(dst) && !fs::exists(dst / "MY.SET backup.manifest.sha256") &&
         !fs::exists(dst.string() + ".INCOMPLETE") && !fs::exists(r.manifest.string() + ".INCOMPLETE"));
    {
        std::ifstream m(r.manifest);
        std::string all((std::istreambuf_iterator<char>(m)), {});
        TEST("manifest lines are sha256sum format", all.find(sha("style-1") + "  STYLE/BANK01.STY\n") != std::string::npos &&
                                                    all.find(sha("") + "  SOUND/USER 01.PCG\n") != std::string::npos);
    }
    TEST("progress rises to the total, inside large files too",
         seen.size() >= 5 && seen.back().bytesDone == seen.back().bytesTotal && seen.back().filesDone == 3 &&
         std::is_sorted(seen.begin(), seen.end(), [](auto& a, auto& b) { return a.bytesDone < b.bytesDone; }));
    TEST("verify passes on the fresh backup", verifyBackup(dst).empty());

    // Refusals: nothing created, source untouched.
    auto refused = [&](const BackupResult& x, const char* word) { return !x.ok && x.message.find(word) != std::string::npos; };
    TEST("refuses an existing destination (never overwrites)", refused(backupKst(src, dst), "already exists") && verifyBackup(dst).empty());
    const fs::path d2 = root / "backups/second";
    put(backupManifestPath(d2), "keep");
    TEST("refuses when the manifest name is taken", refused(backupKst(src, d2), "already exists") && !fs::exists(d2));
    fs::remove(backupManifestPath(d2));
    fs::create_directories(d2.string() + ".INCOMPLETE");
    TEST("refuses when an INCOMPLETE folder from an earlier run exists", refused(backupKst(src, d2), "already exists") && !fs::exists(d2));
    fs::remove(d2.string() + ".INCOMPLETE");
    TEST("refuses a destination inside the source", refused(backupKst(src, src / "STYLE/inner"), "inside") && !fs::exists(src / "STYLE/inner"));
    TEST("refuses a missing destination parent", refused(backupKst(src, root / "nope/x"), "does not exist"));
    TEST("refuses a source that is not a folder", refused(backupKst(src / "STYLE/BANK01.STY", d2), "not a folder") && !fs::exists(d2));
#ifdef __APPLE__
    TEST("refuses a mounted volume", refused(backupKst(src, "/Volumes/AIARR-NO-SUCH/x"), "mounted volume"));
    fs::create_directory_symlink("/Volumes", root / "vol");
    TEST("refuses a mounted volume reached through a symlink", refused(backupKst(src, root / "vol/AIARR-NO-SUCH-2"), "mounted volume"));
#endif
    const fs::path linked = root / "LINKED";
    put(linked / "A.STY", "a");
    fs::create_symlink(linked / "A.STY", linked / "LINK");
    TEST("refuses a source holding a symbolic link", refused(backupKst(linked, d2), "symbolic link") && !fs::exists(d2));
    TEST("source still untouched after the refusals", snapshot(src) == before);

    // Cancel inside the large file: not published, nothing deleted, marked INCOMPLETE.
    const auto c = backupKst(src, root / "backups/c1", [](const BackupProgress& p) { return p.bytesDone < (1u << 20); });
    TEST("cancel: reported, not published, the partial copy kept as INCOMPLETE",
         !c.ok && c.cancelled && keptIncomplete(root / "backups/c1") && c.message.find("Nothing was deleted") != std::string::npos);
    TEST("a second backup to the same name refuses while the INCOMPLETE copy is there",
         refused(backupKst(src, root / "backups/c1"), "already exists") && keptIncomplete(root / "backups/c1"));
    size_t calls = 0, copyCalls = 0;
    backupKst(src, root / "backups/count", [&](const BackupProgress&) { ++copyCalls; return true; });
    const auto late = backupKst(src, root / "backups/c2", [&](const BackupProgress&) { return ++calls < copyCalls; });   // cancel at the very last check
    TEST("cancel at the final check before publishing: not published", !late.ok && late.cancelled && keptIncomplete(root / "backups/c2"));
    fs::create_directories(root / "EMPTY.SET");
    const auto empty = backupKst(root / "EMPTY.SET", root / "backups/c3", [](const BackupProgress&) { return false; });
    TEST("cancel also stops a backup with no files", !empty.ok && empty.cancelled && keptIncomplete(root / "backups/c3"));
    TEST("source untouched after the cancelled backups", snapshot(src) == before);
    // The source changing during the copy fails the backup.
    const auto m = backupKst(src, root / "backups/m1", {}, [&] { put(src / "STYLE/BANK01.STY", "style-X"); });
    TEST("a source file changed during the copy fails the backup", !m.ok && m.message.find("source changed") != std::string::npos &&
                                                                    keptIncomplete(root / "backups/m1"));
    put(src / "STYLE/BANK01.STY", "style-1");
    const auto add = backupKst(src, root / "backups/m2", {}, [&] { put(src / "STYLE/NEW.STY", "n"); });
    TEST("a file added to the source during the copy fails the backup", !add.ok && keptIncomplete(root / "backups/m2"));
    fs::remove(src / "STYLE/NEW.STY");
    const auto dirAdded = backupKst(src, root / "backups/m3", {}, [&] { fs::create_directories(src / "NEWDIR"); });
    TEST("a folder added to the source during the copy fails the backup", !dirAdded.ok && keptIncomplete(root / "backups/m3"));
    fs::remove(src / "NEWDIR");
    // Nothing is ever replaced, even when names appear while the backup runs.
    const fs::path d4 = root / "backups/raced";
    const auto manRace = backupKst(src, d4, {}, [&] { put(backupManifestPath(d4), "someone else's"); });
    std::ifstream mk(backupManifestPath(d4));
    TEST("a manifest that appears during the backup is never replaced",
         !manRace.ok && std::string(std::istreambuf_iterator<char>(mk), {}) == "someone else's");
    const fs::path d5 = root / "backups/raced-dir";
    const auto dirRace = backupKst(src, d5, {}, [&] { put(d5 / "THEIRS.TXT", "theirs"); });
    TEST("a destination folder that appears during the backup is never replaced",
         !dirRace.ok && fs::exists(d5 / "THEIRS.TXT") && !fs::exists(d5 / "STYLE") && fs::is_directory(d5.string() + ".INCOMPLETE"));
    // The destination's parent is pinned: swapping it for a symlink mid-backup cannot redirect the writes.
    const fs::path pinned = root / "pinned", elsewhere = root / "elsewhere";
    fs::create_directories(pinned);
    fs::create_directories(elsewhere);
    const auto swap = backupKst(src, pinned / "b", {}, [&] { fs::rename(pinned, root / "pinned.moved"); fs::create_directory_symlink(elsewhere, pinned); });
    TEST("a parent folder swapped for a symlink mid-backup: writes stay in the folder that was checked",
         swap.ok && fs::is_empty(elsewhere) && diff(src, root / "pinned.moved/b").same() && fs::exists(root / "pinned.moved/b.manifest.sha256"));
    // Names the manifest cannot hold are refused up front.
    const fs::path nl = root / "NL.SET";
    put(nl / "A\nB.STY", "x");
    TEST("refuses a file name with a line break (the manifest is one line per file)",
         refused(backupKst(nl, root / "backups/nl"), "line break") && !fs::exists(root / "backups/nl.INCOMPLETE"));

    // Verify reports damage to a backup.
    put(dst / "STYLE/BANK01.STY", "style-2");
    fs::remove(dst / "SOUND/USER 01.PCG");
    put(dst / "EXTRA.TXT", "x");
    const auto bad = verifyBackup(dst);
    TEST("verify reports changed, missing and extra files",
         bad.find("changed: STYLE/BANK01.STY") != std::string::npos && bad.find("missing: SOUND/USER 01.PCG") != std::string::npos &&
         bad.find("extra:   EXTRA.TXT") != std::string::npos);
    TEST("verify without a manifest says so", verifyBackup(root / "LINKED").rfind("no manifest", 0) == 0);

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
