// P0 Keyboard Compatibility card (library/library_compat.h): synthetic folders only (placeholder-free bytes, Pa-native
// containers from tests/support/panative_fixture.h); no owner data. Each case checks one line the card derives from a
// sourced rule; every folder is checked unchanged afterwards.

#include "library/library_compat.h"
#include "../support/panative_fixture.h"
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger::library;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

using Tree = std::map<std::string, std::string>;
static fs::path make(const fs::path& p, const Tree& t) {
    for (const auto& [rel, b] : t) {
        fs::create_directories((p / rel).parent_path());
        std::ofstream(p / rel, std::ios::binary) << b;
    }
    return p;
}
static std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> m;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file()) { std::ifstream f(e.path(), std::ios::binary); m[e.path().string()] = std::string(std::istreambuf_iterator<char>(f), {}); }
    return m;
}
static const Item& itemAt(const Library& L, const std::string& file, int slot = -1) {
    for (const auto& it : L.items) if (it.file == file && (slot < 0 || it.slot == slot)) return it;
    std::fprintf(stderr, "no item %s\n", file.c_str());
    std::exit(1);
}
// The line of `kbd` with this rule and status (nullptr = none).
static const CompatLine* line(const std::vector<CompatTarget>& card, const std::string& kbd, const std::string& rule, Compat s) {
    for (const auto& c : card)
        if (c.keyboard == kbd)
            for (const auto& l : c.lines) if (l.rule == rule && l.status == s) return &l;
    return nullptr;
}
static bool has(const CompatLine* l, const std::string& text) { return l && (l->text + " [" + l->source + "]").find(text) != std::string::npos; }

int main() {
    std::printf("Test: Keyboard Compatibility card (sourced rule tables only)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-compat-" + std::to_string(::getpid()));
    fs::remove_all(root);

    // ── Pa5X KST: one clean item, one loose item (R-05) ──
    const fs::path kst = make(root / "MINE.KST", {{"STYLE/BANK01/Ballad.STG", "x"}, {"STYLE/Loose.STG", "x"}, {"SOUND/USER01.PCG", "x"}});
    const auto before = snapshot(kst);
    const auto L = load(kst);
    const auto v = validate(kst);
    const auto card = compatCard(L, itemAt(L, "STYLE/BANK01/Ballad.STG"), v);
    TEST("three targets, Pa5X first", card.size() == 3 && card[0].keyboard == "Pa5X" && card[1].keyboard == "Pa4X family" && card[2].keyboard == "Pa700/Pa1000 family");
    TEST("Pa5X: STYLE/.STG is DOCUMENTED with its R-04 source (MAN5 p.721)", has(line(card, "Pa5X", "R-04", Compat::Documented), "MAN5 p.721"));
    TEST("Pa5X: FAT32 file size DOCUMENTED within the limit, cites MAN p.715", has(line(card, "Pa5X", "FAT32", Compat::Documented), "MAN p.715"));
    TEST("Pa5X: a clean item has no EXCEEDS line", std::none_of(card[0].lines.begin(), card[0].lines.end(), [](const CompatLine& l) { return l.status == Compat::Exceeds; }));
    TEST("Pa4X: .STG is not in MAN4's table: UNKNOWN, not EXCEEDS", has(line(card, "Pa4X family", "R-04", Compat::Unknown), "MAN4 PDF p.973-975") &&
         !line(card, "Pa4X family", "R-04", Compat::Exceeds));
    TEST("Pa4X: no source says it reads a Pa5X KST: UNKNOWN", has(line(card, "Pa4X family", "-", Compat::Unknown), "reads a Pa5X KST"));
    TEST("Pa4X: a KST item has no known slot: R-16 UNKNOWN", line(card, "Pa4X family", "R-16", Compat::Unknown) != nullptr);
    TEST("every target: L3 not measured", std::all_of(card.begin(), card.end(), [](const CompatTarget& c) {
             return std::any_of(c.lines.begin(), c.lines.end(), [](const CompatLine& l) { return l.rule == "L3" && l.text.find("not measured") != std::string::npos; }); }));
    const std::string text = compatCardText(itemAt(L, "STYLE/BANK01/Ballad.STG"), card);
    TEST("text: never a compatibility claim, every target NOT VERIFIED", text.find("Pa5X: NOT VERIFIED ON A PA5X") != std::string::npos &&
         text.find("NOT VERIFIED ON A PA4X FAMILY KEYBOARD") != std::string::npos && text.find(" compatible") == std::string::npos);
    const auto loose = compatCard(L, itemAt(L, "STYLE/Loose.STG"), v);
    TEST("Pa5X: the validator's R-05 finding on this item is EXCEEDS with its source", has(line(loose, "Pa5X", "R-05", Compat::Exceeds), "MAN5 p.731-732"));
    TEST("validator findings only on the item's own layout table", !line(loose, "Pa4X family", "R-05", Compat::Exceeds));

    // ── Pa700 SET: a Style bank file with 49 slots (R-16), the one-way Data Conversion for the Pa5X (R-13) ──
    std::vector<Synth> full;
    for (int s = 0; s < 49; ++s) full.push_back({0x06000030, 2, 17, uint8_t(s), "F", "f"});
    const std::string pnAny = build({{0x0A0A0010, 1, 0, 0, "G", "g"}}, 1);
    const fs::path set = make(root / "set700", {{"GLOBAL/LIMITER.AUD", pnAny}, {"STYLE/USER01.STY", build(full, 1)},
                                                {"STYLE/USER02.STY", build({{0x06000030, 2, 17, 0, "S", "p"}}, 1)}});
    const auto setBefore = snapshot(set);
    const auto S = load(set);
    const auto sv = validate(set);
    const auto last = compatCard(S, itemAt(S, "STYLE/USER01.STY", 48), sv), first = compatCard(S, itemAt(S, "STYLE/USER01.STY", 0), sv);
    TEST("SET: slot 49 EXCEEDS the 48 slots (R-16, MAN4 p.975) on both SET tables", has(line(last, "Pa700/Pa1000 family", "R-16", Compat::Exceeds), "MAN4 PDF p.975") &&
         line(last, "Pa4X family", "R-16", Compat::Exceeds));
    // Codex r1 #2: the overflow is the bank FILE's (48 per bank file), so it shows on every SET target whatever slot is selected
    TEST("SET: slot 1 of a 49-slot bank file is EXCEEDS on both SET targets, never DOCUMENTED",
         has(line(first, "Pa4X family", "R-16", Compat::Exceeds), "MAN4 PDF p.975") && line(first, "Pa700/Pa1000 family", "R-16", Compat::Exceeds) &&
         !line(first, "Pa4X family", "R-16", Compat::Documented) && !line(first, "Pa700/Pa1000 family", "R-16", Compat::Documented));
    const auto ok16 = compatCard(S, itemAt(S, "STYLE/USER02.STY", 0), sv);
    TEST("SET: an item of a bank file within 48 slots is DOCUMENTED on both SET targets",
         line(ok16, "Pa4X family", "R-16", Compat::Documented) && line(ok16, "Pa700/Pa1000 family", "R-16", Compat::Documented) &&
         !line(ok16, "Pa4X family", "R-16", Compat::Exceeds));
    TEST("Pa5X: a SET item only through the one-way Data Conversion (R-13, MAN5 p.737)", has(line(first, "Pa5X", "R-13", Compat::Documented), "MAN5 p.737"));
    TEST("Pa5X: .STY is not in the Pa5X table: UNKNOWN", line(first, "Pa5X", "R-04", Compat::Unknown) != nullptr);

    // ── limits from the tables on a synthetic model (no 4 GB file written) ──
    {
        Library big;
        big.format = Format::Pa5xKst;
        big.layout = ai_arranger::media::kLayoutKst;
        big.sampleDiskBytes = (uintmax_t(4) << 30) + 1;
        Item pcm{"PCM/RAM01.PCM", "Sample", "PCM", "RAM01", -1, -1, 0, "file name"};
        big.files["PCM/RAM01.PCM"].size = uintmax_t(1) << 32;
        Validation none;   // ok = false
        none.error = "cancelled";
        const auto c = compatCard(big, pcm, none);
        // Codex r1 #3: disk bytes are not sample memory and "4 GB" is unit-ambiguous: the card's own R-08 line is never DOCUMENTED
        TEST("Pa5X: R-08 from disk bytes is UNKNOWN with both 4 GB readings, never DOCUMENTED or EXCEEDS by the card itself",
             has(line(c, "Pa5X", "R-08", Compat::Unknown), "4000000000") && has(line(c, "Pa5X", "R-08", Compat::Unknown), "4294967296") &&
             !line(c, "Pa5X", "R-08", Compat::Documented) && !line(c, "Pa5X", "R-08", Compat::Exceeds));
        big.sampleDiskBytes = 4100000000u;   // between the two readings
        const auto amb = compatCard(big, pcm, none);
        TEST("Pa5X: 4,100,000,000 disk bytes: R-08 UNKNOWN, not DOCUMENTED", line(amb, "Pa5X", "R-08", Compat::Unknown) && !line(amb, "Pa5X", "R-08", Compat::Documented));
        big.sampleDiskBytes = 10;
        const auto small = compatCard(big, pcm, none);
        TEST("Pa5X: even 10 disk bytes: R-08 UNKNOWN, not DOCUMENTED", line(small, "Pa5X", "R-08", Compat::Unknown) && !line(small, "Pa5X", "R-08", Compat::Documented));
        Validation v8;   // the validator's own R-08 WARN (binary 4 GiB, its rule table's source) stays EXCEEDS
        v8.ok = true;
        v8.findings.push_back({Severity::Warn, "R-08", ".", "PCM+KMP disk bytes exceed 4 GB", "MAN5 p.1174/p.1936: 4 GB User Sample memory (disk size here, not RAM)"});
        TEST("Pa5X: the validator's R-08 finding stays EXCEEDS with its source", has(line(compatCard(big, pcm, v8), "Pa5X", "R-08", Compat::Exceeds), "MAN5 p.1174"));
        TEST("Pa5X: a 4 GiB file EXCEEDS the FAT32 limit", line(c, "Pa5X", "FAT32", Compat::Exceeds) != nullptr);
        TEST("an incomplete validation is UNKNOWN, never a pass", has(line(c, "Pa5X", "-", Compat::Unknown), "validation incomplete: cancelled"));
        TEST("FAT32 limit is 4 GiB - 1 byte", kFat32MaxFileBytes == 4294967295u);
    }

    {   // Codex r1 #1: findings made before a later read failure stay EXCEEDS next to the incomplete-validation UNKNOWN
        const fs::path p = make(root / "PART.KST", {{"STYLE/Loose.STG", "x"}, {"STYLE/ZZ/Unreadable.STG", "x"}});
        fs::permissions(p / "STYLE/ZZ/Unreadable.STG", fs::perms::none);
        const auto PL = load(p);
        const auto pv = validate(p);
        TEST("partial: the validator stops at the unreadable file with findings already made", !pv.ok && !pv.findings.empty());
        const auto pc = compatCard(PL, itemAt(PL, "STYLE/Loose.STG"), pv);
        TEST("partial: the R-05 already found on the item stays EXCEEDS", has(line(pc, "Pa5X", "R-05", Compat::Exceeds), "MAN5 p.731-732"));
        TEST("partial: plus the incomplete-validation UNKNOWN", has(line(pc, "Pa5X", "-", Compat::Unknown), "validation incomplete"));
        fs::permissions(p / "STYLE/ZZ/Unreadable.STG", fs::perms::owner_all);
    }
    TEST("the folders are unchanged (read-only)", snapshot(kst) == before && snapshot(set) == setBefore);
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
