// KST folder comparison: identical / changed / added / removed / moved, byte equality (same size,
// different bytes is "changed"), symlinks skipped, both trees left untouched.

#include "media/kst_diff.h"
#include <cstdio>
#include <chrono>
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
    TEST("result is not 'same' (and not verified: a symlink was skipped)", !d.same() &&
         format(d, "A", "B").find(ec ? "RESULT: different" : "RESULT: not verified") != std::string::npos);
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
    auto has = [](const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    {
        const fs::path dst = root / "dst", src = root / "src";
        put(dst / "STYLE/BANK01/Pop.STY", "pop-v1");
        put(dst / "STYLE/BANK01/Rock.STY", "rock");
        put(dst / "SOUND/BANK01/Piano.PCG", "piano");
        put(dst / "STYLE/BANK01/Jazz.STY", "jazz");
        put(dst / "PAD/BANK01/Pad.PAD", "pad");
        put(src / "STYLE/BANK01/Pop.STY", "pop-v2");         // replace
        put(src / "STYLE/BANK01/Rock.STY", "rock");          // unchanged
        put(src / "STYLE/BANK02/Latin.STY", "latin");        // add
        put(src / "SOUND/BANK02/Piano2.PCG", "piano");       // add, duplicate of DST Piano.PCG
        put(src / "STYLE/BANK01/JAZZ.STY", "jazz-x");        // case-only name difference
        const std::string sd = snapshot(dst), ss = snapshot(src);
        const MergePlan mp = planMerge(src, dst);
        TEST("merge plan: replace, add, unchanged, duplicate, case-only classified",
             mp.ok && mp.replace == std::vector<std::string>({"STYLE/BANK01/Pop.STY"}) && has(mp.add, "STYLE/BANK02/Latin.STY") &&
             mp.unchanged.size() == 1 && mp.duplicate.size() == 1 && mp.duplicate[0].second == "SOUND/BANK01/Piano.PCG" &&
             mp.caseOnly.size() == 1 && mp.caseOnly[0].second == "STYLE/BANK01/Jazz.STY");
        TEST("merge plan: PAD not loaded is kept and not deleted by Replace All; SOUND Piano kept but deleted by Replace All",
             has(mp.kept, "PAD/BANK01/Pad.PAD") && !has(mp.replaceAllDeletes, "PAD/BANK01/Pad.PAD") && has(mp.replaceAllDeletes, "SOUND/BANK01/Piano.PCG"));
        TEST("merge plan: replacement is unsafe for both modes; no PCM warning without a PCM folder",
             !mp.mergeSafe() && !mp.replaceAllSafe() && format(mp, "src", "dst").find("WARNING") == std::string::npos);
        put(dst / "PCM/RAM01.PCM", "samples");
        put(src / "PCM/RAM01.PCM", "samples");
        const std::string sd2 = snapshot(dst), ss2 = snapshot(src);
        const MergePlan p2 = planMerge(src, dst);
        TEST("PCM folder in SRC warns even when its files are identical to DST (manual p.736)",
             p2.srcPcm && format(p2, "src", "dst").find("deletes ALL User Samples") != std::string::npos);
        TEST("merge plan leaves both folders untouched (names, sizes, mtimes)", snapshot(dst) == sd2 && snapshot(src) == ss2 && !sd.empty() && !ss.empty());
    }
    // regressions from the independent (Codex) review of the merge plan
    {   // #1 case-only name with identical bytes used to vanish as a "move" and report safe
        put(root / "c1d/STYLE/Jazz.STY", "jazz");
        put(root / "c1s/STYLE/JAZZ.STY", "jazz");
        const MergePlan c = planMerge(root / "c1s", root / "c1d");
        TEST("case-only name with identical bytes is CASE?, never safe", c.ok && c.caseOnly.size() == 1 && !c.mergeSafe());
        put(root / "c2d/STYLE/Jazz.STY", "a");
        put(root / "c2d/STYLE/JAZZ.STY", "b");
        put(root / "c2s/STYLE/Jazz.STY", "a");
        const bool caseSensitiveFs = std::distance(fs::directory_iterator(root / "c2d/STYLE"), fs::directory_iterator{}) == 2;
        const MergePlan c2 = planMerge(root / "c2s", root / "c2d");
        TEST("exact match plus a case variant in DST is still flagged (case-sensitive file systems only)",
             !caseSensitiveFs || (c2.ok && c2.unchanged.size() == 1 && c2.caseOnly.size() == 1 && !c2.mergeSafe()));
    }
    {   // #2 Replace All deletions used to give a safe result
        put(root / "r2d/STYLE/Old.STG", "old");
        put(root / "r2s/STYLE/New.STG", "new");
        const MergePlan r = planMerge(root / "r2s", root / "r2d");
        TEST("Replace All deletion: Merge safe, Replace All not, and the report says so",
             r.ok && r.mergeSafe() && !r.replaceAllSafe() && r.replaceAllDeletes.size() == 1 &&
             format(r, "s", "d").find("RESULT (Replace All): existing User data would be replaced or deleted") != std::string::npos);
    }
    {   // #3 an unreadable folder used to be skipped silently; #4 an unreadable file counted as "changed"
        put(root / "p3a/STYLE/A.STY", "a");
        put(root / "p3b/STYLE/A.STY", "a");
        put(root / "p3b/LOCKED/X.STY", "x");
        fs::permissions(root / "p3b/LOCKED", fs::perms::none);
        const bool canTest = ::geteuid() != 0;   // root ignores permissions
        const KstDiff pd = diff(root / "p3a", root / "p3b");
        const MergePlan pm = planMerge(root / "p3b", root / "p3a");
        TEST("unreadable folder is an error for diff and merge plan", !canTest || (!pd.ok && !pm.ok && !pd.same()));
        fs::permissions(root / "p3b/LOCKED", fs::perms::owner_all);
        put(root / "p4a/S.STY", "same");
        put(root / "p4b/S.STY", "same");
        fs::permissions(root / "p4b/S.STY", fs::perms::none);
        const KstDiff fd = diff(root / "p4a", root / "p4b");
        TEST("unreadable file is an error, not 'changed'", !canTest || (!fd.ok && fd.error.find("cannot read") != std::string::npos));
        fs::permissions(root / "p4b/S.STY", fs::perms::owner_all);
    }
    {   // #5 duplicates against the whole DST, and kept files counted correctly
        put(root / "d5d/SOUND/A.PCG", "x");
        put(root / "d5s/SOUND/A.PCG", "x");
        put(root / "d5s/SOUND/B.PCG", "x");
        const MergePlan d5 = planMerge(root / "d5s", root / "d5d");
        TEST("added file identical to a DST file that SRC also names is a duplicate", d5.ok && d5.duplicate.size() == 1 && d5.duplicate[0].first == "SOUND/B.PCG");
        put(root / "d6d/SOUND/A.PCG", "x");
        put(root / "d6s/SOUND/B.PCG", "x");
        const MergePlan d6 = planMerge(root / "d6s", root / "d6d");
        TEST("renamed copy: one duplicate AND the DST original counted as kept", d6.ok && d6.duplicate.size() == 1 && d6.kept.size() == 1);
    }
    {   // #7 equal-size, different-content files on both sides: hash pre-filter, not O(pairs x bytes)
        const std::string body(16384, 'k');
        for (int i = 0; i < 400; ++i) {
            put(root / "big/a" / ("A" + std::to_string(i) + ".PCM"), body + "a" + std::to_string(1000 + i));
            put(root / "big/b" / ("B" + std::to_string(i) + ".PCM"), body + "b" + std::to_string(1000 + i));
        }
        const auto t0 = std::chrono::steady_clock::now();
        const KstDiff bd = diff(root / "big/a", root / "big/b");
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        TEST("400 x 400 equal-size non-matching files compared in under 10 s (no pairwise rereads)",
             bd.ok && bd.removed.size() == 400 && bd.added.size() == 400 && bd.moved.empty() && sec < 10);
        std::printf("    (%.2f s)\n", sec);
    }
    {   // symlinks are not compared: the folders are not reported as the same
        put(root / "s1/A.STY", "a");
        put(root / "s2/A.STY", "a");
        std::error_code e2;
        fs::create_symlink(root / "s2/A.STY", root / "s2/L.STY", e2);
        TEST("a skipped symlink means 'not verified', never 'same'", e2 || (!diff(root / "s1", root / "s2").same() &&
             format(diff(root / "s1", root / "s2"), "", "").find("not verified") != std::string::npos));
    }
    TEST("missing folder is an error", !diff(root / "nope", b).ok && !diff(a, root / "nope").ok);
    TEST("both trees untouched", snapshot(a) == sa && snapshot(b) == sb);
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
