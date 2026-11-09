// P1.9 KST inventory: counts per extension and folder, byte-identical
// duplicates (same size but different bytes is NOT a duplicate), symlinks not
// followed, errors reported, and the scanned tree is left untouched.

#include "media/kst_inventory.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

// Snapshot of names, sizes and mtimes (to prove the scan wrote nothing).
static std::string snapshot(const fs::path& root) {
    std::string s;
    std::vector<std::string> rows;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        std::error_code ec;
        const long long t = fs::symlink_status(e.path()).type() == fs::file_type::regular
                           ? static_cast<long long>(fs::last_write_time(e.path(), ec).time_since_epoch().count()) : 0;
        rows.push_back(e.path().string() + "|" + std::to_string(t) + "|" +
                       (e.is_regular_file() ? std::to_string(e.file_size()) : "-"));
    }
    std::sort(rows.begin(), rows.end());
    for (const auto& r : rows) s += r + "\n";
    return s;
}

int main() {
    std::printf("Test: KST inventory\n");
    const fs::path root = fs::temp_directory_path() / ("aiarr_kst_" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path kst = root / "MYSET.KST";
    put(kst / "STYLE/BANK01.STY", "style-A");
    put(kst / "STYLE/BANK02.sty", "style-A");          // duplicate of BANK01, lower-case ext
    put(kst / "STYLE/BANK03.STY", "style-B");          // same size, different bytes
    put(kst / "PAD/PAD01.PAD", std::string(70000, 'x'));   // > one read buffer
    put(kst / "PAD/PAD02.PAD", std::string(69999, 'x') + "y"); // differs in the last byte
    put(kst / "PAD/PAD03.PAD", std::string(70000, 'x'));   // duplicate of PAD01
    put(kst / "README", "");
    put(kst / "EMPTY.TXT", "");                         // two empty files are identical
    fs::create_directories(kst / "SOUND");              // empty folder: no files
    std::error_code ec;
    fs::create_symlink(kst / "STYLE/BANK01.STY", kst / "LINK.STY", ec);
    const bool haveLink = !ec;

    const std::string before = snapshot(kst);
    const Inventory inv = scan(kst);
    TEST("scan succeeds", inv.ok);
    TEST("8 regular files counted", inv.files == 8);
    TEST("extensions normalised to upper case",
         inv.by_ext.count(".STY") && inv.by_ext.at(".STY").files == 3 && !inv.by_ext.count(".sty"));
    TEST("no-extension files grouped", inv.by_ext.count("(none)") && inv.by_ext.at("(none)").files == 1);
    TEST("per-folder counts", inv.by_folder.count("PAD") && inv.by_folder.at("PAD").files == 3 &&
                              inv.by_folder.at(".").files == 2 && !inv.by_folder.count("SOUND"));
    TEST("bytes total", inv.bytes == 7 * 3 + 70000 * 3);
    const std::vector<std::vector<std::string>> want = {
        {"EMPTY.TXT", "README"},
        {"PAD/PAD01.PAD", "PAD/PAD03.PAD"},
        {"STYLE/BANK01.STY", "STYLE/BANK02.sty"},
    };
    TEST("exactly the byte-identical groups (same size, different bytes excluded)", inv.duplicates == want);
    if (haveLink)
        TEST("symlink skipped, not followed or counted", inv.skipped == std::vector<std::string>{"LINK.STY"});
    TEST("scan wrote nothing", snapshot(kst) == before);
    TEST("deterministic", format(scan(kst), "x") == format(inv, "x"));

    TEST("missing folder is an error", !scan(root / "nope").ok);
    TEST("a file is not a folder", !scan(kst / "README").ok);
    if (haveLink) {
        fs::create_directory_symlink(kst, root / "LINKDIR", ec);
        TEST("symlinked root is refused", ec || !scan(root / "LINKDIR").ok);
    }
    const std::string report = format(inv, "MYSET.KST");
    TEST("report says read-only and hardware pending",
         report.find("read-only") != std::string::npos &&
         report.find("HARDWARE VALIDATION PENDING") != std::string::npos);

    // Documented Pa5X types (manual v1.4 p.721) and the PCM folder flag (p.736).
    TEST("no PCM folder in the first tree", !inv.has_pcm_folder && report.find("PCM folder: no") != std::string::npos);
    TEST(".STY is not a documented Pa5X type (Pa5X Styles are .STG)",
         inv.by_type.count("(not a documented Pa5X type)") && !inv.by_type.count("Style"));
    TEST("EMPTY.TXT is a documented Text file", inv.by_type.count("Text") && inv.by_type.at("Text").files == 1);
    const fs::path k2 = root / "PA5X.KST";
    put(k2 / "STYLE/USER01/A.STG", "s");
    put(k2 / "STYLE/USER01/b.stg", "t");
    put(k2 / "KEYBOARD SET/K.KSC", "k");
    put(k2 / "SOUND/S.PCG", "p");
    put(k2 / "pcm/X.PCM", "xx");                       // lower-case folder name
    put(k2 / "MULTISAMPLE/M.KMP", "m");
    const Inventory i2 = scan(k2);
    TEST("documented types by extension, case-insensitive",
         i2.by_type.at("Style").files == 2 && i2.by_type.at("Keyboard Set").files == 1 &&
         i2.by_type.at("Sound").files == 1 && i2.by_type.at("Sample").files == 1 &&
         i2.by_type.at("Multisample").files == 1 && !i2.by_type.count("(not a documented Pa5X type)"));
    TEST("bank level: STYLE/USER01 holds 2 files; files directly in a type folder are not a bank",
         i2.by_bank.size() == 1 && i2.by_bank.at("STYLE/USER01").files == 2 && i2.by_bank.at("STYLE/USER01").bytes == 2);
    TEST("first tree: no file sits two levels down, so no banks", inv.by_bank.empty());
    TEST("PCM folder detected (any case) and warned about",
         i2.has_pcm_folder && format(i2, "x").find("deletes the User Samples") != std::string::npos);
    // P2.3 memory estimate: .PCM + .KMP disk bytes only (X.PCM 2 bytes + M.KMP 1 byte).
    const std::string est = formatMemoryEstimate(i2, 1000);
    TEST("estimate sums PCM and KMP disk bytes against the budget",
         est.find("Disk total 3 bytes = 0.3% of budget 1000 bytes") != std::string::npos &&
         est.find("not Pa5X sample RAM") != std::string::npos && est.find("OVER BUDGET") == std::string::npos);
    TEST("over budget is flagged", formatMemoryEstimate(i2, 2).find("OVER BUDGET") != std::string::npos);
    TEST("default budget is PA Manager's 4 GiB profile, labelled as an application figure",
         format(i2, "x").find("of budget 4294967296 bytes (PA Manager") != std::string::npos);
    TEST("no sample files: zero, no crash", formatMemoryEstimate(inv, 1).find("Disk total 0 bytes = 0.0%") != std::string::npos);
    // --budget-bytes parsing (regression: overflow used to be accepted as UINTMAX_MAX).
    uintmax_t b = 7;
    bool allRejected = true;
    for (const char* bad : {"", "0", "-1", "+1", " 1", "1x", "1.5", "0x10", "99999999999999999999999", "18446744073709551616"})
        allRejected = allRejected && !parseBudgetBytes(bad, b);
    TEST("malformed, zero, negative and overflowing budgets are rejected and leave the value unchanged", allRejected && b == 7 && !parseBudgetBytes(nullptr, b));
    TEST("valid budgets parse, including the maximum",
         parseBudgetBytes("1000", b) && b == 1000 && parseBudgetBytes("18446744073709551615", b) && b == UINTMAX_MAX);
    fs::remove_all(k2 / "pcm");
    put(k2 / "SOUND/PCM.PCG", "p");                     // a FILE named PCM.* is not a PCM folder
    TEST("a file named PCM is not the PCM folder", !scan(k2).has_pcm_folder);

    {   // regression: an unreadable folder was skipped silently (skip_permission_denied) and an unreadable
        // file left the scan "ok", so the duplicate count looked complete while it was not
        const bool canTest = ::geteuid() != 0;   // root ignores permissions
        const fs::path k3 = root / "K3";
        put(k3 / "A.PCM", "same");
        put(k3 / "LOCKED/B.PCM", "same");               // duplicate hidden behind an unreadable folder
        fs::permissions(k3 / "LOCKED", fs::perms::none);
        const Inventory d = scan(k3);
        TEST("unreadable folder fails the scan (no silent partial duplicate count)",
             !canTest || (!d.ok && d.error.find("cannot read") != std::string::npos));
        fs::permissions(k3 / "LOCKED", fs::perms::owner_all);
        put(k3 / "C.PCM", "same");
        fs::permissions(k3 / "C.PCM", fs::perms::none);  // same size as A and B: must be read to count duplicates
        const Inventory f = scan(k3);
        TEST("unreadable same-size file fails the scan", !canTest || (!f.ok && f.error.find("cannot read") != std::string::npos));
        TEST("failed scan reports the error, not duplicates",
             !canTest || (format(f, "x").find("ERROR") != std::string::npos && format(f, "x").find("duplicates") == std::string::npos));
        fs::permissions(k3 / "C.PCM", fs::perms::owner_all);
        TEST("readable again: A, B, C are one duplicate group", scan(k3).ok && scan(k3).duplicates.size() == 1 && scan(k3).duplicates[0].size() == 3);
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
