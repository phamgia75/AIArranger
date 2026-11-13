// E7 Library Workspace model + SET/KST Manager planning. Every library is synthetic and built here (placeholder bytes
// in the make_synthetic_kst_library.py style, Pa-native containers from tests/support/panative_fixture.h); no owner
// data. The optional owner check runs only when AIARR_LIBRARY_OWNER_SET names a local COPY of an owner SET folder,
// and only reads it.

#include "library/library_plan.h"
#include "../support/panative_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger;
using namespace ai_arranger::library;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static std::string blob(const std::string& tag, size_t n) {
    const std::string seed = "AIARRANGER-SYNTHETIC-NOT-A-KORG-FILE " + tag + "\n";
    std::string s;
    while (s.size() < n) s += seed;
    return s.substr(0, n);
}
static void put(const fs::path& p, const std::string& b) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << b; }
static bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }
static bool anyHas(const std::vector<std::string>& v, const std::string& x) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(x) != std::string::npos; });
}
static size_t find(const Library& L, const std::string& name, const std::string& bank = "") {
    for (size_t i = 0; i < L.items.size(); ++i) if (trimmed(L.items[i].name) == name && (bank.empty() || L.items[i].bank == bank)) return i;
    return size_t(-1);
}
static std::map<std::string, std::pair<fs::file_time_type, std::string>> stamp(const Library& L) {
    std::map<std::string, std::pair<fs::file_time_type, std::string>> m;
    for (const auto& [rel, f] : L.files) m[rel] = {fs::last_write_time(fs::path(L.root) / rel), f.sha256};
    return m;
}

int main() {
    std::printf("Test: Library Workspace model and SET/KST Manager planning (read-only, E7)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-library-" + std::to_string(::getpid()));
    fs::remove_all(root);

    // ── Pa5X KST folders (one file = one item; banks are TYPE/BANK folders) ──
    const fs::path ka = root / "A.KST", kb = root / "B.KST";
    put(ka / "STYLE/USER01/Ballad.STG", blob("ballad", 4096));
    put(ka / "STYLE/USER01/Swing.STG", blob("swing", 3000));
    put(ka / "STYLE/USER02/Ballad.STG", blob("other ballad", 2000));   // same name, other bank, other bytes
    put(ka / "STYLE/USER02/Copy.STG", blob("ballad", 4096));           // same bytes as USER01/Ballad
    put(ka / "SOUND/USER01/Piano.PCG", blob("sound", 900));
    put(ka / "MULTISAMPLE/USER01/Piano.KMP", blob("kmp", 300));
    put(ka / "PAD/USER01/Hit.PDC", blob("pad", 150));
    put(kb / "STYLE/USER01/Ballad.STG", blob("ballad", 4096));          // same
    put(kb / "STYLE/USER01/Swing.STG", blob("swing2", 3000));           // different
    put(kb / "STYLE/USER03/Moved.STG", blob("other ballad", 2000));     // A's USER02/Ballad, moved
    put(kb / "PAD/USER01/Hit.PDC", blob("pad", 150));
    put(kb / "PAD/USER01/Extra.PDC", blob("extra", 160));               // only in B
    put(kb / "PCM/USER01/Smp.PCM", blob("pcm", 5000));

    const auto A = load(ka), B = load(kb);
    TEST("KST loads read-only by layout", A.ok && A.format == Format::Pa5xKst && B.ok && B.format == Format::Pa5xKst);
    TEST("KST banks are TYPE/BANK folders with per-bank sizes", A.banks.count("STYLE/USER01") && A.banks.at("STYLE/USER01").files == 2 &&
         A.banks.at("STYLE/USER01").bytes == 7096 && A.banks.at("STYLE/USER02").items == 2 && A.banks.at("STYLE/USER01").type == "Style");
    TEST("KST items are files named after the file, no slot", find(A, "Swing") != size_t(-1) && A.items[find(A, "Swing")].slot == -1 &&
         A.items[find(A, "Swing")].from == "file name" && A.items.size() == 7);
    TEST("total size and sample data on disk (.PCM + .KMP only)", A.bytes == 4096 + 3000 + 2000 + 4096 + 900 + 300 + 150 && A.sampleDiskBytes == 300 &&
         A.sampleFiles == 1 && B.sampleDiskBytes == 5000);
    TEST("summary labels sample size as disk size, not keyboard RAM", summary(A).find("not keyboard sample RAM") != std::string::npos &&
         summary(A).find("Read-only source") != std::string::npos);

    // search and filter
    TEST("search is case-insensitive over name, bank and file", search(A, "BALL").size() == 2 && search(A, "user02").size() == 2 && search(A, "").size() == 7);
    TEST("type and bank filters", search(A, "", "Style").size() == 4 && search(A, "ballad", "", "STYLE/USER02").size() == 1 && search(A, "", "Sample").empty());

    // duplicates
    const auto dA = duplicates(A);
    TEST("identical files found by SHA-256", dA.files.size() == 1 && (dA.files[0] == std::vector<std::string>{"STYLE/USER01/Ballad.STG", "STYLE/USER02/Copy.STG"}));
    TEST("same name in two banks flagged as a NAME duplicate only", dA.names.size() == 1 && trimmed(A.items[dA.names[0][0]].name) == "Ballad" &&
         duplicatesView(A, dA).find("NAME ONLY") != std::string::npos);

    // dependencies, only where evidenced
    TEST("KMP without any PCM file: samples MISSING (by file presence)", A.deps.size() == 2 && A.deps[0].status == "MISSING" &&
         A.deps[0].reason.find("file presence") != std::string::npos);
    TEST("Sound -> multisample stays UNKNOWN with its reason", A.deps[1].status == "UNKNOWN" && A.deps[1].reason.find("not decoded") != std::string::npos);
    TEST("no dependency invented where nothing is evidenced", B.deps.empty() && dependencyView(B.deps) == "  none evidenced\n");

    // compare (media::diff)
    const auto c = compare(A, B);
    TEST("compare: same / different / only A / only B / moved", c.ok && (c.same == std::vector<std::string>{"PAD/USER01/Hit.PDC", "STYLE/USER01/Ballad.STG"}) &&
         c.different == std::vector<std::string>{"STYLE/USER01/Swing.STG"} && has(c.onlyA, "STYLE/USER02/Copy.STG") && has(c.onlyB, "PAD/USER01/Extra.PDC") &&
         c.moved.size() == 1 && c.moved[0].first == "STYLE/USER02/Ballad.STG" && c.note.empty());
    put(kb / "PAD/USER01/Late.PDC", "x");   // B changes after it was opened: the stale snapshot must not be reported as compared
    const auto stale = compare(A, B);
    TEST("compare refuses a library that changed since it was opened", !stale.ok && stale.error.find("changed since") != std::string::npos);
    fs::remove(kb / "PAD/USER01/Late.PDC");

    // ── Pa-family SET folders (one file = one bank; items from the Pa-native reader) ──
    const fs::path sa = root / "SetA", sb = root / "SetB";
    put(sa / "GLOBAL/LIMITER.AUD", "x");   // Pa700 marker, no .SET suffix
    put(sa / "STYLE/USER01.STY", build({{0x06000030, 2, 1, 0, "Synth Pop", "p1"}, {0x06000030, 2, 1, 2, "Synth Rock", "p22"}}, 1));
    put(sa / "STYLE/USER02.STY", build({{0x06000030, 2, 2, 0, "Synth Pop", "p333"}}, 0));
    put(sa / "SOUND/USERDK01.PCG", build({{0x09020130, 1, 3, 0, "Synth Kit", "s"}}, 1));
    put(sa / "MULTISMP/RAM.KMP", build({{0x11050038, 5, 1, 0, "RAM", "k"}}, 1));
    put(sa / "PCM/RAM01.PCM", build({{0x12000018, 6, 0, 0, "C5", "pcm"}}, 1));
    put(sa / "PAD/USER01.PAD", std::string("\x01\x00\x01\x14\x00\x00", 6));   // magic only: invalid container
    std::vector<Synth> full;
    for (int i = 0; i < 48; ++i) full.push_back({0x06000030, 2, 9, uint8_t(i), "Full " + std::to_string(i), "f"});
    put(sb / "GLOBAL/LIMITER.AUD", "x");
    put(sb / "STYLE/USER01.STY", build({{0x06000030, 2, 1, 0, "B Waltz", "w"}, {0x06000030, 2, 1, 1, "B Tango", "t"}}, 1));
    put(sb / "STYLE/USER09.STY", build(full, 1));
    put(sb / "KEYBOARDSET/USER01.PRF", build({{0x09020130, 1, 11, 0, "B Piano", "kb"}}, 1));
    const auto SA = load(sa), SB = load(sb);
    TEST("SET loads by its Pa700 markers", SA.ok && SA.format == Format::PaSet && SB.ok && SB.format == Format::PaSet);
    const size_t pop1 = find(SA, "Synth Pop", "STYLE/USER01"), rock = find(SA, "Synth Rock");
    TEST("SET items: name, bank file, slot and kind from the Pa-native table", pop1 != size_t(-1) && rock != size_t(-1) &&
         SA.items[rock].bank == "STYLE/USER01" && SA.items[rock].slot == 2 && SA.items[rock].kind == 2 && SA.items[rock].from == "Pa-native item table" &&
         SA.items[rock].bytes == 3 && SA.banks.at("STYLE/USER01").items == 2 && SA.banks.at("STYLE/USER01").files == 1);
    const size_t pad = find(SA, "USER01", "PAD/USER01");
    TEST("an unreadable SET file stays one file-level item and says why", pad != size_t(-1) && SA.items[pad].slot == -1 &&
         SA.items[pad].from.find("INVALID Pa-native container") != std::string::npos);
    TEST("SET: KMP with PCM present = EVIDENCED by file presence; Sound UNKNOWN", SA.deps.size() == 2 && SA.deps[0].status == "EVIDENCED" &&
         SA.deps[1].status == "UNKNOWN");
    TEST("SET: same Style name in two bank files is a name duplicate; files differ", duplicates(SA).names.size() == 1 && duplicates(SA).files.empty());
    TEST("bank view shows the documented 48-slot capacity", bankView(SB).find("STYLE/USER09  Style · 1 · 48/48") != std::string::npos);
    TEST("item details: slot 1-based, kind meaning UNKNOWN, dependencies", itemDetails(SA, SA.items[rock]).find("slot 3 (shown 1-based)") != std::string::npos &&
         itemDetails(SA, SA.items[rock]).find("kind 2 (meaning UNKNOWN)") != std::string::npos &&
         itemDetails(SA, SA.items[rock]).find("UNKNOWN: Synth Rock -> Sounds") != std::string::npos);
    TEST("compare across formats says so", compare(A, SA).ok && !compare(A, SA).note.empty());

    // ── Planning ──
    {
        const size_t waltz = find(SB, "B Waltz"), kit = find(SA, "Synth Kit"), piano = find(SB, "B Piano");
        const std::vector<Step> plan = {
            {Op::Copy, 'A', rock, "STYLE/USER01", -1, ""},       // 1 first free slot in B:USER01 = slot 3
            {Op::Copy, 'A', pop1, "STYLE/USER01", 0, ""},        // 2 slot 1 taken by B Waltz
            {Op::Copy, 'A', pop1, "STYLE/USER09", -1, ""},       // 3 bank full
            {Op::Copy, 'A', pop1, "KEYBOARDSET/USER01", -1, ""}, // 4 type mismatch
            {Op::Move, 'A', kit, "SOUND/USERDK01", -1, ""},      // 5 ok, dependency UNKNOWN, A only read
            {Op::Copy, 'A', kit, "", -1, ""},                    // 6 gone after the move
            {Op::Rename, 'B', waltz, "", -1, "Bad/Name"},        // 7 invalid name
            {Op::Rename, 'B', waltz, "", -1, "B Tango"},         // 8 name already used (warning)
            {Op::BankSwap, 'B', 0, "STYLE/USER01", -1, "KEYBOARDSET/USER01"},   // 9 different types
            {Op::BankSwap, 'B', 0, "STYLE/USER01", -1, "STYLE/USER05"},         // 10 ok (empty target bank)
            {Op::Copy, 'A', rock, "STYLE/USER01", -1, ""},       // 11 after the swap, B:USER01 is empty again -> slot 1
            {Op::Copy, 'B', piano, "", -1, ""},                  // 12 B -> A into a bank A does not have yet
        };
        const auto ev = evaluate(SA, SB, plan);
        const auto& s = ev.steps;
        TEST("copy picks the first free slot", s[0].conflicts.empty() && s[0].summary.find("slot 3") != std::string::npos &&
             anyHas(s[0].warnings, "needs a Pa-native writer"));
        TEST("copy onto a taken slot is a conflict naming the item", anyHas(s[1].conflicts, "slot 1 of STYLE/USER01 is taken by \"B Waltz\""));
        TEST("copy into a full bank is a conflict (48 per bank file)", anyHas(s[2].conflicts, "is full (48"));
        TEST("copy into a bank of another type is a conflict", anyHas(s[3].conflicts, "holds Keyboard Set, not Style"));
        TEST("move: dependency UNKNOWN and the read-only source are warnings", s[4].conflicts.empty() && anyHas(s[4].warnings, "dependency UNKNOWN") &&
             anyHas(s[4].warnings, "only read"));
        TEST("an item moved by an earlier step cannot be copied again", anyHas(s[5].conflicts, "an earlier step moved it"));
        TEST("rename refuses a name outside printable ASCII / with a slash", anyHas(s[6].conflicts, "slash"));
        TEST("rename to a name in use in the bank is a warning", s[7].conflicts.empty() && anyHas(s[7].warnings, "already used"));
        TEST("bank swap of different types is a conflict", anyHas(s[8].conflicts, "different types"));
        TEST("bank swap warns that references are UNKNOWN", s[9].conflicts.empty() && anyHas(s[9].warnings, "UNKNOWN"));
        TEST("steps see the effect of earlier steps (swap emptied the bank)", s[10].conflicts.empty() && s[10].summary.find("slot 1") != std::string::npos);
        TEST("copy B -> A works the other way", s[11].conflicts.empty() && s[11].summary.find("A:KEYBOARDSET/USER01") != std::string::npos);
        TEST("evaluation totals: blocked by 6 conflicts", ev.blocked() && ev.conflicts == 6);
        TEST("an explicit slot past the 48 is a conflict that says so",
             anyHas(evaluate(SA, SB, {{Op::Copy, 'A', rock, "STYLE/USER01", 48, ""}}).steps[0].conflicts, "slot 49 is past the 48 slots"));
        TEST("cross-format copy is a conflict", anyHas(evaluate(A, SB, {{Op::Copy, 'A', find(A, "Swing"), "STYLE/USER01", -1, ""}}).steps[0].conflicts, "formats differ"));

        // KST plans: a copy is a file at TYPE/BANK/<file>
        const auto k = evaluate(A, B, {{Op::Copy, 'A', find(A, "Swing"), "", -1, ""},
                                       {Op::Copy, 'A', find(A, "Ballad", "STYLE/USER01"), "", -1, ""},
                                       {Op::Copy, 'A', find(A, "Copy"), "STYLE/USER07", -1, ""},
                                       {Op::Rename, 'A', find(A, "Swing"), "", -1, "Ballad"},
                                       {Op::Copy, 'A', find(A, "Piano", "MULTISAMPLE/USER01"), "", -1, ""}});
        TEST("KST copy onto a file with other bytes is a conflict", anyHas(k.steps[0].conflicts, "with other bytes"));
        TEST("KST copy onto the same bytes is a warning (nothing to do)", k.steps[1].conflicts.empty() && anyHas(k.steps[1].warnings, "same bytes: nothing to do"));
        TEST("KST copy of bytes B already holds elsewhere warns duplicate", anyHas(k.steps[2].warnings, "duplicate: same bytes as B:STYLE/USER01/Ballad.STG"));
        TEST("KST rename onto an existing file is a conflict", anyHas(k.steps[3].conflicts, "STYLE/USER01/Ballad.STG already exists"));
        TEST("multisample copy carries the MISSING sample warning and the p.736 note", anyHas(k.steps[4].warnings, "dependency MISSING") &&
             anyHas(k.steps[4].warnings, "p.736"));

        // export: staged document, export-only, through the shared folder writer
        const auto before = stamp(SA), beforeB = stamp(SB);
        const fs::path out = root / "out/My Plan";
        TEST("an empty plan is not exported", exportPlan(SA, SB, {}, root / "out/empty") == "refusing: the plan has no steps");
        TEST("export writes a new folder", exportPlan(SA, SB, plan, out).empty());
        size_t n = 0;
        TEST("export: plan.json + PLAN.txt, checked against PACKAGE.sha256, no work folder left",
             fs::is_regular_file(out / "plan.json") && fs::is_regular_file(out / "PLAN.txt") &&
             media::verifyManifest(out, out / "PACKAGE.sha256", {}, &n, [](const std::string& r) { return r == "PACKAGE.sha256"; }, true).empty() && n == 2 &&
             !fs::exists(media::incompletePath(out)));
        std::ifstream j(out / "plan.json"), t(out / "PLAN.txt");
        const std::string js((std::istreambuf_iterator<char>(j)), {}), tx((std::istreambuf_iterator<char>(t)), {});
        TEST("plan says EXPORT-ONLY and why, in both files", js.find("\"apply\": \"export-only\"") != std::string::npos &&
             js.find("\"applicable\": false") != std::string::npos && tx.find("EXPORT-ONLY") != std::string::npos && tx.find("BLOCKED") != std::string::npos);
        TEST("plan.json records each item's file SHA-256", js.find(SA.files.at("STYLE/USER01.STY").sha256) != std::string::npos);
        if (std::system("command -v python3 >/dev/null 2>&1") == 0)
            TEST("plan.json is valid JSON", std::system(("python3 -I -c 'import json,sys; json.load(open(sys.argv[1]))' '" + (out / "plan.json").string() + "'").c_str()) == 0);
        TEST("an existing export folder is never overwritten", exportPlan(SA, SB, plan, out).find("already exists") != std::string::npos);
        TEST("export inside a library is refused", exportPlan(SA, SB, plan, sb / "plan").find("inside library") != std::string::npos && !fs::exists(sb / "plan"));
        TEST("export onto a mounted volume is refused", exportPlan(SA, SB, plan, "/Volumes/AIA-NO-SUCH-STICK/plan").find("mounted volume") != std::string::npos);
        TEST("both libraries are unchanged (mtime and SHA-256)", stamp(load(sa)) == before && stamp(load(sb)) == beforeB);
    }

    // strictness: missing folder, links not followed
    TEST("a missing folder fails the load", !load(root / "nope").ok);
    fs::create_symlink(ka / "STYLE", kb / "LINK");
    const auto BL = load(kb);
    TEST("a symlink is listed as skipped, not followed", BL.ok && BL.skipped == std::vector<std::string>{"LINK"} && BL.files.size() == 6);
    fs::remove(kb / "LINK");

    // optional: an owner SET copy, read only (counts printed, never names)
    if (const char* own = std::getenv("AIARR_LIBRARY_OWNER_SET")) {
        const auto O = load(own);
        const auto before = stamp(O);
        TEST("owner SET copy: loads as a Pa-family SET with Pa-native items", O.ok && O.format == Format::PaSet && O.items.size() > O.files.size());
        std::printf("  info: owner SET: %zu files, %zu items, %zu banks, %ju bytes, %zu exact duplicate groups, %zu name duplicate groups\n",
                    O.files.size(), O.items.size(), O.banks.size(), O.bytes, duplicates(O).files.size(), duplicates(O).names.size());
        TEST("owner SET copy unchanged after reading", stamp(load(own)) == before);
    } else {
        std::printf("  info: AIARR_LIBRARY_OWNER_SET not set: owner check skipped\n");
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
