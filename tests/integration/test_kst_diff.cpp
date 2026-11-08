// KST folder comparison: identical / changed / added / removed / moved, byte equality (same size,
// different bytes is "changed"), symlinks skipped, both trees left untouched.

#include "media/kst_diff.h"
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
static std::string snapshot(const fs::path& root) {
    std::vector<std::string> rows;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        std::error_code ec;
        const bool reg = fs::symlink_status(e.path()).type() == fs::file_type::regular;
        rows.push_back(e.path().string() + "|" + (reg ? std::to_string(static_cast<long long>(fs::last_write_time(e.path(), ec).time_since_epoch().count())) + "|" +
                                                         std::to_string(e.file_size()) : "-"));
    }
    std::sort(rows.begin(), rows.end());
    std::string s;
    for (const auto& r : rows) s += r + "\n";
    return s;
}

int main() {
    std::printf("Test: KST folder diff\n");
    const fs::path root = fs::temp_directory_path() / ("aia-kstdiff-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path a = root / "A", b = root / "B";
    put(a / "STYLE/BANK01.STY", "style-1");
    put(a / "SOUND/BANK01.PCG", "sound-aaaa");
    put(a / "PCM/RAM01.PCM", "pcm-data-1");
    put(a / "PAD/BANK01.PAD", "pad");
    put(a / "OLD.TXT", "gone");
    put(b / "STYLE/BANK01.STY", "style-1");          // identical
    put(b / "SOUND/BANK01.PCG", "sound-bbbb");       // same size, different bytes -> changed
    put(b / "PCM/RAM02.PCM", "pcm-data-1");          // moved
    put(b / "PAD/BANK01.PAD", "pad-longer");         // changed (size)
    put(b / "NEW.TXT", "new");                       // added
    std::error_code ec;
    fs::create_symlink(b / "NEW.TXT", b / "LINK.TXT", ec);
    const std::string sa = snapshot(a), sb = snapshot(b);

    const KstDiff d = diff(a, b);
    TEST("diff ok", d.ok);
    TEST("1 identical", d.identical == 1);
    TEST("same size, different bytes is changed; size change is changed",
         d.changed == std::vector<std::string>({"PAD/BANK01.PAD", "SOUND/BANK01.PCG"}));
    TEST("byte-identical file under a new path is a move",
         d.moved.size() == 1 && d.moved[0].first == "PCM/RAM01.PCM" && d.moved[0].second == "PCM/RAM02.PCM");
    TEST("removed and added", d.removed == std::vector<std::string>({"OLD.TXT"}) && d.added == std::vector<std::string>({"NEW.TXT"}));
    TEST("symlink skipped, not followed", ec || (d.skipped.size() == 1 && d.skipped[0] == "B: LINK.TXT"));
    TEST("result is different", !d.same() && format(d, "A", "B").find("RESULT: different") != std::string::npos);
    TEST("a folder against itself is the same", diff(a, a).same() && diff(a, a).identical == 5);
    {   // a copy whose one byte differs is caught although sizes match (backup verification)
        put(root / "C/STYLE/BANK01.STY", "style-2");
        put(root / "D/STYLE/BANK01.STY", "style-1");
        const KstDiff cd = diff(root / "C", root / "D");
        TEST("one-byte difference with equal size detected", cd.changed.size() == 1 && !cd.same());
    }
    {   // two identical removed files and one added copy: only one move, the other stays removed
        put(root / "E/X1.PCM", "same");
        put(root / "E/X2.PCM", "same");
        put(root / "F/Y.PCM", "same");
        const KstDiff m = diff(root / "E", root / "F");
        TEST("each added file pairs with one removed file only", m.moved.size() == 1 && m.removed.size() == 1 && m.added.empty());
    }
    TEST("missing folder is an error", !diff(root / "nope", b).ok && !diff(a, root / "nope").ok);
    TEST("both trees untouched", snapshot(a) == sa && snapshot(b) == sb);
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
