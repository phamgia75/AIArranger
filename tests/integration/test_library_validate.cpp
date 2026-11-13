// E8 structural validator (library/library_validate.h): one clean synthetic folder per layout, then one mutation per
// rule, each expected to fire exactly that rule. Synthetic folders only (placeholder-free bytes, Pa-native containers
// from tests/support/panative_fixture.h); no owner data. Every folder is checked unchanged after validation.
// Optional: AIARR_LIBRARY_OWNER_SET names a local COPY of an owner SET folder; it is only read, and only counts are printed.

#include "library/library_validate.h"
#include "../support/panative_fixture.h"
#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger::library;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

using Tree = std::map<std::string, std::string>;
static fs::path make(const fs::path& p, const Tree& t) {
    fs::create_directories(p);
    for (const auto& [rel, b] : t) {
        if (rel.back() == '/') { fs::create_directories(p / rel); continue; }
        fs::create_directories((p / rel).parent_path());
        std::ofstream(p / rel, std::ios::binary) << b;
    }
    return p;
}
static std::set<std::string> rules(const Validation& v, int sev = -1) {
    std::set<std::string> r;
    for (const auto& f : v.findings) if (sev < 0 || int(f.severity) == sev) r.insert(f.rule);
    return r;
}
static std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> m;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file()) { std::ifstream f(e.path(), std::ios::binary); m[e.path().string()] = std::string(std::istreambuf_iterator<char>(f), {}); }
    return m;
}
static Tree with(Tree t, const Tree& extra) { for (const auto& [k, v] : extra) t[k] = v; return t; }

int main() {
    std::printf("Test: structural validator (KST and SET rule tables, E8)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-validate-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const std::string pnStyle = build({{0x06000030, 2, 17, 0, "S", "p"}}, 1), pnPrf = build({{0x09020130, 1, 11, 0, "K", "p"}}, 1),
                      pnPad = build({{0x0C000030, 9, 20, 0, "P", "p"}}, 1), pnPcg = build({{0x10000030, 4, 9, 0, "Snd", "p"}}, 1),
                      pnKmp = build({{0x11050038, 5, 1, 0, "RAM", "k"}}, 1), pnPcm = build({{0x12000018, 6, 0, 0, "C5", "pcm"}}, 1),
                      pnAny = build({{0x0A0A0010, 1, 0, 0, "G", "g"}}, 1);

    // ── Pa5X KST ──
    const Tree kst = {{"STYLE/BANK01/My Style.STG", "x"}, {"KEYBOARDSET/BANK01/My Kbs.KSC", "x"}, {"PAD/BANK01/P.PDC", "x"},
                      {"SOUND/USER01.PCG", "x"}, {"MULTISMP/USER.KMP", "x"}, {"PCM/RAM01.PCM", "x"}, {"CONVERTREPORT.TXT", "x"}};
    const auto good = validate(make(root / "GOOD.KST", kst));
    TEST("clean KST: Pa5X table, no error or warning, only the PCM note (R-13 INFO)",
         good.ok && good.layout == ai_arranger::media::kLayoutKst && rules(good) == std::set<std::string>{"R-13"} && good.count(Severity::Info) == 1);
    TEST("every finding cites its source", !good.findings.empty() && good.findings[0].source.find("MAN5 p.736") != std::string::npos);
    const std::vector<std::pair<std::string, Tree>> kstCases = {
        {"R-03", {{"KEYBOARD_SET/BANK01/K.KSC", "x"}}},          // SET_KST_GAP §5 #1: the observed name is KEYBOARDSET
        {"R-04", {{"STYLE/BANK01/x.KSC", "x"}}},
        {"R-05", {{"STYLE/loose.STG", "x"}}},                    // SET_KST_GAP §5 #2: STYLE holds bank sub-folders
        {"R-06", {{"NOTES.TXT", "x"}}},
        {"R-07", {{"PAD/BANK01/empty.PDC", ""}}},
        {"R-09", {{"PAD/BANK01/a:b.PDC", "x"}}},
        {"R-10", {{"SOUND/USER02.PCG", std::string(kPlaceholder) + " sound"}}},
        {"R-11", {{"STYLE/BANK01/._My Style.STG", "x"}}},
    };
    int i = 0;
    for (const auto& [rule, extra] : kstCases) {
        const auto v = validate(make(root / ("K" + std::to_string(i++) + ".KST"), with(kst, extra)));
        auto got = rules(v);
        got.erase("R-13");
        if (rule == "R-03") got.erase("R-05");   // an unknown type folder with a bank sub-folder is also "grid type with sub-folders"
        TEST(("KST mutation fires " + rule + " only").c_str(), v.ok && got == std::set<std::string>{rule});
    }
    {
        Tree nokmp = kst;
        nokmp.erase("MULTISMP/USER.KMP");
        TEST("KST: PCM without a KMP is R-14", rules(validate(make(root / "NOKMP.KST", nokmp))).count("R-14"));
        TEST("KST: folder without the .KST suffix (Pa5X markers) is R-01 ERROR", rules(validate(make(root / "NOSUFFIX", kst)), 0) == std::set<std::string>{"R-01"});
        TEST("KST: no files in any type folder is R-02 ERROR", rules(validate(make(root / "EMPTY.KST", {{"CONVERTREPORT.TXT", "x"}, {"STYLE/", ""}})), 0) == std::set<std::string>{"R-02"});
        const fs::path lk = make(root / "LINK.KST", kst);
        fs::create_symlink(root / "GOOD.KST/STYLE/BANK01/My Style.STG", lk / "STYLE/BANK01/Link.STG");
        const auto vl = validate(lk);
        TEST("KST: a symlink is R-12, never followed", rules(vl).count("R-12") && vl.findings.size() == 2);
    }

    // ── Pa700/Pa1000 SET (OBSERVED table) ──
    const Tree p700 = {{"GLOBAL/LIMITER.AUD", pnAny}, {"GLOBAL/SETUP.GBL", pnAny}, {"STYLE/USER01.STY", pnStyle}, {"KEYBOARDSET/USER01.PRF", pnPrf},
                       {"PAD/USER01.PAD", pnPad}, {"SOUND/USERDK01.PCG", pnPcg}, {"MULTISMP/RAM.KMP", pnKmp}, {"PCM/RAM01.PCM", pnPcm},
                       {"SONGBOOK/SONGDB.SBD/BANK01.SBD", pnAny}, {"GUITARPRESET/", ""}};
    const auto g7 = validate(make(root / "set700", p700));
    TEST("clean Pa700 SET: Pa700 table, only the PCM note",
         g7.ok && g7.layout == ai_arranger::media::kLayoutPa700 && rules(g7) == std::set<std::string>{"R-13"} &&
             g7.table.find("OBSERVED") != std::string::npos && g7.findings[0].message.find("WHOLE KST") == std::string::npos);
    std::vector<Synth> full;
    for (int s = 0; s < 49; ++s) full.push_back({0x06000030, 2, 17, uint8_t(s), "F", "f"});
    const std::vector<std::pair<std::string, Tree>> setCases = {
        {"R-03", {{"KEYBOARD_SET/USER02.PRF", pnPrf}}},
        {"R-04", {{"STYLE/USER02.PRF", pnPrf}}},
        {"R-05", {{"STYLE/USER02/x.STY", pnStyle}}},
        {"R-06", {{"README.TXT", "x"}}},
        {"R-15", {{"STYLE/USER02.STY", "not a container"}}},
        {"R-16", {{"STYLE/USER02.STY", build(full, 1)}}},
        {"R-17", {{"STYLE/MYBANK.STY", pnStyle}}},
    };
    i = 0;
    for (const auto& [rule, extra] : setCases) {
        const auto v = validate(make(root / ("S" + std::to_string(i++)), with(p700, extra)));
        auto got = rules(v);
        got.erase("R-13");
        if (rule == "R-05") got.erase("R-17");   // ...also not a bank-file name
        if (rule == "R-03") got.erase("R-15");   // unknown folder: its file is still a known type? no: never read
        TEST(("Pa700 SET mutation fires " + rule + " only").c_str(), v.ok && got == std::set<std::string>{rule});
    }
    TEST("Pa700 SET: no .SET suffix needed (OBSERVED without it)", !rules(g7).count("R-01"));

    // ── Pa4X SET (MAN4 table) ──
    const Tree p4 = {{"GLOBAL/SETUP.GBL", pnAny}, {"GLOBAL/MXPRESET.MXP", pnAny}, {"STYLE/USER01.STY", pnStyle}, {"STYLE/FAVORITE01.STY", pnStyle},
                     {"KEYBOARDSET/BANK01.PRF", pnPrf}, {"PAD/USER01.PAD", pnPad}, {"SOUND/USERDK.PCG", pnPcg}, {"MULTISMP/RAM.KMP", pnKmp},
                     {"PCM/RAM01.PCM", pnPcm}, {"SONGBOOK/SONGDB.SBD", pnAny}, {"VOICEPRESET/USER01.VOC", pnAny}};
    const auto g4 = validate(make(root / "MYDATA.SET", p4));
    TEST("clean Pa4X SET: Pa4X table (MAN4 p.975), only the PCM note", g4.ok && g4.layout == ai_arranger::media::kLayoutPa4x && rules(g4) == std::set<std::string>{"R-13"});
    const auto n4 = validate(make(root / "MYDATA", p4));
    TEST("Pa4X SET without the .SET suffix: R-01 WARN (not ERROR)", rules(n4, 1) == std::set<std::string>{"R-01"} && n4.count(Severity::Error) == 0);
    // (a SONGBOOK/SONGDB.SBD folder or GLOBAL/LIMITER.AUD would make it a Pa700-family SET: detectLayout markers)
    const auto x4 = validate(make(root / "AUD.SET", with(p4, {{"GLOBAL/OTHER.AUD", pnAny}, {"STYLE/USER02/x.STY", pnStyle}})));
    TEST("Pa4X table: .AUD is not in MAN4 (R-04); banks are files (R-05)", x4.ok && x4.layout == ai_arranger::media::kLayoutPa4x &&
         (rules(x4) == std::set<std::string>{"R-04", "R-05", "R-13"}));
    const auto bad4 = validate(make(root / "BAD.SET", with(p4, {{"STYLE/USER02.STY", "junk"}})));
    TEST("Pa4X table: a non-container bank file is only INFO (R-15 observed on Pa700 only)",
         (rules(bad4) == std::set<std::string>{"R-13", "R-15"}) && bad4.count(Severity::Warn) == 0);

    // ── unknown layout, unreadable ──
    const auto u = validate(make(root / "plain", {{"notes/a.txt", "x"}}));
    TEST("unknown layout: no rule table, one R-01 ERROR", u.ok && u.table == "none" && rules(u, 0) == std::set<std::string>{"R-01"});
    TEST("a missing folder is not validated", !validate(root / "missing").ok);
    {   // Codex r1 P2: an unreadable file makes the validation INCOMPLETE (not a pass)
        const fs::path k = make(root / "UNREAD.KST", kst);
        fs::permissions(k / "SOUND/USER01.PCG", fs::perms::none);
        const auto v = validate(k);
        fs::permissions(k / "SOUND/USER01.PCG", fs::perms::owner_read | fs::perms::owner_write);
        TEST("KST: an unreadable file -> not validated (incomplete), says which file", !v.ok && v.error.find("SOUND/USER01.PCG") != std::string::npos &&
             validationText(v).find("NOT validated") != std::string::npos);
        const fs::path s7 = make(root / "unread700", p700);
        fs::permissions(s7 / "STYLE/USER01.STY", fs::perms::none);
        const auto v7 = validate(s7);
        fs::permissions(s7 / "STYLE/USER01.STY", fs::perms::owner_read | fs::perms::owner_write);
        TEST("SET: an unreadable bank file -> not validated (incomplete)", !v7.ok && v7.error.find("STYLE/USER01.STY") != std::string::npos);
    }
    TEST("text: summary line says file level, not verified on a keyboard",
         validationText(good).find("not verified on a keyboard") != std::string::npos && validationText(good).find("[MAN5 p.736") != std::string::npos);

    // read-only: validating changes nothing
    const auto before = snapshot(root);
    for (const auto& e : fs::directory_iterator(root)) (void)validate(e.path());
    TEST("validation writes nothing (every test folder byte-identical)", snapshot(root) == before);

    if (const char* owner = std::getenv("AIARR_LIBRARY_OWNER_SET")) {
        const auto snap = snapshot(owner);
        const auto v = validate(owner);
        std::printf("  owner SET: layout %s, %zu error(s), %zu warning(s), %zu info\n", v.layout.c_str(), v.count(Severity::Error),
                    v.count(Severity::Warn), v.count(Severity::Info));
        TEST("owner SET copy: validated, Pa700 table, no ERROR, unchanged", v.ok && v.layout == ai_arranger::media::kLayoutPa700 &&
             v.count(Severity::Error) == 0 && snapshot(owner) == snap);
    }

    fs::remove_all(root);
    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
