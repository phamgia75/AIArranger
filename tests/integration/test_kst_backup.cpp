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
static bool nothingLeft(const fs::path& dst) {
    return !fs::exists(dst) && !fs::exists(dst.string() + ".INCOMPLETE") && !fs::exists(backupManifestPath(dst)) &&
           !fs::exists(backupManifestPath(dst).string() + ".INCOMPLETE");
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

    // Cancel inside the large file: nothing kept, nothing presented as a backup.
    const fs::path d3 = root / "backups/cancelled";
    const auto c = backupKst(src, d3, [](const BackupProgress& p) { return p.bytesDone < (1u << 20); });
    TEST("cancel: reported as cancelled, no backup, no manifest, no INCOMPLETE left", !c.ok && c.cancelled && nothingLeft(d3));
    size_t calls = 0, copyCalls = 0;
    backupKst(src, root / "count", [&](const BackupProgress&) { ++copyCalls; return true; });
    fs::remove_all(root / "count"); fs::remove(backupManifestPath(root / "count"));
    const auto late = backupKst(src, d3, [&](const BackupProgress&) { return ++calls < copyCalls - 1; });   // cancel in the last verification step
    TEST("cancel during the final verification: reported as cancelled, nothing kept", !late.ok && late.cancelled && nothingLeft(d3));
    // Source changing during the copy: discarded.
    const auto m = backupKst(src, d3, {}, [&] { put(src / "STYLE/BANK01.STY", "style-X"); });
    TEST("a source changed during the copy is detected and the copy discarded",
         !m.ok && m.message.find("source changed") != std::string::npos && nothingLeft(d3));
    put(src / "STYLE/BANK01.STY", "style-1");
    const auto add = backupKst(src, d3, {}, [&] { put(src / "STYLE/NEW.STY", "n"); });
    TEST("a file added to the source during the copy is detected", !add.ok && nothingLeft(d3));
    fs::remove(src / "STYLE/NEW.STY");

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
