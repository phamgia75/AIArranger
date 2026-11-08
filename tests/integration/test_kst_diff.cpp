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
    // ── merge plan (load SRC onto DST) ──
    {
        const fs::path dst = root / "dst", src = root / "src";
        put(dst / "STYLE/BANK01/Pop.STY", "pop-v1");
        put(dst / "STYLE/BANK01/Rock.STY", "rock");
        put(dst / "SOUND/BANK01/Piano.PCG", "piano");
        put(dst / "STYLE/BANK01/Jazz.STY", "jazz");
        put(src / "STYLE/BANK01/Pop.STY", "pop-v2");         // replace
        put(src / "STYLE/BANK01/Rock.STY", "rock");          // unchanged
        put(src / "STYLE/BANK02/Latin.STY", "latin");        // add
        put(src / "SOUND/BANK02/Piano2.PCG", "piano");       // duplicate of DST Piano.PCG
        put(src / "STYLE/BANK01/JAZZ.STY", "jazz-x");        // case-only name difference
        const std::string sd = snapshot(dst), ss = snapshot(src);
        const KstDiff md = diff(dst, src);
        const std::string plan = formatMergePlan(md, "src", "dst", hasPcmFolder(src));
        TEST("merge plan: 1 replace, 1 add, 1 unchanged, 1 duplicate, 1 case-only",
             plan.find("REPLACE    STYLE/BANK01/Pop.STY") != std::string::npos && plan.find("add        STYLE/BANK02/Latin.STY") != std::string::npos &&
             plan.find("unchanged (same name, same bytes): 1") != std::string::npos &&
             plan.find("duplicate  SOUND/BANK02/Piano2.PCG (same bytes as DST SOUND/BANK01/Piano.PCG)") != std::string::npos &&
             plan.find("CASE?      STYLE/BANK01/JAZZ.STY  vs existing STYLE/BANK01/Jazz.STY") != std::string::npos);
        TEST("merge plan: DST-only files counted as kept on Merge (case-only match not double-counted)",
             plan.find("deleted by Replace All of that type (DST only, p.734): 0") != std::string::npos);
        TEST("merge plan: replacement means a warning result, no PCM warning without a PCM folder",
             plan.find("RESULT: existing User data would be replaced") != std::string::npos && plan.find("WARNING") == std::string::npos);
        // regression guard: PCM files identical in both folders must still warn (loading the KST deletes all User Samples)
        put(dst / "PCM/RAM01.PCM", "samples");
        put(src / "PCM/RAM01.PCM", "samples");
        const std::string sd2 = snapshot(dst), ss2 = snapshot(src);
        const std::string p2 = formatMergePlan(diff(dst, src), "src", "dst", hasPcmFolder(src));
        TEST("PCM folder in SRC warns even when its files are identical to DST (manual p.736)",
             p2.find("deletes ALL User Samples") != std::string::npos);
        const fs::path clean = root / "clean";
        put(clean / "STYLE/BANK09/New.STY", "new");
        TEST("only new names: no existing data replaced",
             formatMergePlan(diff(dst, clean), "clean", "dst", hasPcmFolder(clean)).find("RESULT: no existing User data would be replaced") != std::string::npos);
        TEST("merge plan leaves both folders untouched (names, sizes, mtimes)", snapshot(dst) == sd2 && snapshot(src) == ss2 && !sd.empty() && !ss.empty());
    }
    TEST("missing folder is an error", !diff(root / "nope", b).ok && !diff(a, root / "nope").ok);
    TEST("both trees untouched", snapshot(a) == sa && snapshot(b) == sb);
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
