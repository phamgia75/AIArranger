// SET -> KST phase 1 (media/panative/set_to_kst.h, jobs::setToKst): member classification from the measured Pa5X
// conversion, CARRY files byte-identical at their lower-cased KST paths, nothing written for BLOCKED members, the
// CONVERSION report, the KMP -> PCM dependency map, and the refusals. Generated fixtures only (no owner bytes).
//
// --oracle (CTest test_set_to_kst_oracle, label private-corpus): converts a local COPY of an owner SET
// (AIARR_SET_ORACLE_SET) and compares with the Pa5X's own conversion of it (AIARR_SET_ORACLE_KST). Prints counts only;
// without both variables it prints ORACLE UNAVAILABLE and exits 77 (Skipped, never passed).

#include "jobs/export_specs.h"
#include "../support/panative_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;
namespace s2k = media::panative::settokst;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
static JobState run(const ExportSpec& s) {
    JobState st;
    st.id = 1;
    runExport(s, st, [] { return false; }, [](const JobState&) {});
    return st;
}
static std::map<std::string, std::string> tree(const fs::path& d) {   // rel -> bytes
    std::map<std::string, std::string> m;
    for (const auto& e : fs::recursive_directory_iterator(d))
        if (e.is_regular_file()) m[fs::relative(e.path(), d).generic_string()] = slurp(e.path());
    return m;
}
static std::string id8(int n) { std::string s = "SMPID"; s += char('0' + n / 100); s += char('0' + n / 10 % 10); s += char('0' + n % 10); return s; }
// A raw KMP item in the observed Pa5X layout (FINDINGS §6): u16 version, u32 sample count, table at +85 of 122-byte
// records with the sample ID at +40.
static std::string kmpPayload(const std::vector<int>& ids) {
    std::string p;
    be16(p, 0x0103); be32(p, uint32_t(ids.size()));
    p.resize(85, '\0');
    for (int i : ids) { std::string r(122, '\0'); r.replace(0, 6, "zone_" + std::to_string(i % 10)); r.replace(40, 8, id8(i)); p += r; }
    return p;
}
static Synth pcmItem(int n) { Synth s{0x12020010, 6, 1, uint8_t(n), "Sample " + std::to_string(n), std::string(32, '\x11')}; s.sampleId = id8(n); return s; }

// A synthetic SET: every member class of the measured table, plus one unknown file.
static void makeSet(const fs::path& d, const std::vector<int>& pcmIds, const std::vector<int>& kmpIds) {
    std::vector<Synth> pcm;
    for (int n : pcmIds) pcm.push_back(pcmItem(n));
    put(d / "PCM/RAM01.PCM", build(pcm, 1));
    put(d / "SOUND/USER01.PCG", build({{0x10000038, 4, 9, 0, "Piano", "OC31sound"}}, 1));
    put(d / "SOUND/USERDK01.PCG", build({{0x10000038, 4, 11, 0, "Kit", "OC31kit"}}, 0));   // format-0 directory, carried as is
    put(d / "MULTISMP/RAM.KMP", build({{0x11080018, 5, 0, 0, "RAM", kmpPayload(kmpIds)}}, 1));
    put(d / "SONGBOOK/LISTDB.SBL", build({{0x1e000130, 12, 0, 0, "List", "OC31list"}}, 1));
    put(d / "SONGBOOK/SONGDB.SBD/BANK01.SBD", build({{0x1e000130, 11, 0, 0, "Song", "OC31song"}}, 1));
    put(d / "STYLE/USER01.STY", build({{0x08000030, 1, 0, 0, "Style", "OC31style"}}, 1));
    put(d / "KEYBOARDSET/USER01.PRF", build({{0x09000030, 2, 0, 0, "Set", "OC31set"}}, 1));
    put(d / "PAD/USER01.PAD", build({{0x0a000030, 3, 0, 0, "Pad", "OC31pad"}}, 1));
    put(d / "GLOBAL/GLOBAL.GBL", build({{0x0b000030, 3, 0, 0, "G", "OC31g"}}, 1));
    put(d / "VOICEPRESET/VOICE.VOC", "voice");
    put(d / "GUITARPRESET/GUITAR.GTR", "guitar");
    put(d / "NOTES.TXT", "unknown");
}

static int oracle() {
    const char* setEnv = std::getenv("AIARR_SET_ORACLE_SET");
    const char* kstEnv = std::getenv("AIARR_SET_ORACLE_KST");
    if (!setEnv || !kstEnv || !*setEnv || !*kstEnv) { std::printf("ORACLE UNAVAILABLE (set AIARR_SET_ORACLE_SET and AIARR_SET_ORACLE_KST)\n"); return 77; }
    const fs::path set = setEnv, kst = kstEnv;
    const fs::path root = fs::temp_directory_path() / ("aia-set2kst-oracle-" + std::to_string(::getpid()));
    fs::create_directories(root);
    const auto before = tree(set);
    const auto st = run(setToKst(set, root / "ORACLE.KST"));
    TEST("oracle: the conversion is published", st.phase == Phase::Done);
    if (st.phase != Phase::Done) std::fprintf(stderr, "  reason: %s\n", st.reason.c_str());
    const auto ours = st.phase == Phase::Done ? tree(root / "ORACLE.KST") : std::map<std::string, std::string>{};
    const auto theirs = tree(kst);
    std::map<std::string, int> same, differ;
    for (const auto& [rel, b] : ours) {
        const auto it = theirs.find(rel);
        (it != theirs.end() && it->second == b ? same : differ)[rel.substr(0, rel.find('/'))]++;
    }
    std::set<std::string> setBytes;
    for (const auto& [rel, b] : before) setBytes.insert(b);
    size_t identicalInPa5x = 0, missedByUs = 0;
    for (const auto& [rel, b] : theirs)
        if (setBytes.count(b)) { ++identicalInPa5x; missedByUs += !ours.count(rel) || ours.at(rel) != b; }
    for (const auto& [k, n] : same) std::printf("  oracle: %s %d file(s) byte-identical to the Pa5X's file at the same path\n", k.c_str(), n);
    for (const auto& [k, n] : differ) std::printf("  oracle: %s %d file(s) NOT identical / not at the same path\n", k.c_str(), n);
    std::printf("  oracle: Pa5X files byte-identical to a SET member: %zu; not produced by us at the same path: %zu\n", identicalInPa5x, missedByUs);
    TEST("oracle: every CARRY output file is byte-identical to the Pa5X's file at the same path", differ.empty() && !ours.empty());
    TEST("oracle: every Pa5X file that equals a SET member is produced at the same path", missedByUs == 0);
    TEST("oracle: the SET copy is unchanged", tree(set) == before);
    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--oracle") return oracle();
    std::printf("Test: SET -> KST phase 1 (resources only)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-set2kst-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path set = root / "DEMO.SET";
    makeSet(set, {1, 2, 3}, {1, 2, 9});   // sample 3 unreferenced, KMP ID 9 dangling

    // ---- a) inventory ---------------------------------------------------------------------------------------------
    const auto plan = s2k::inventory(set);
    auto kindOf = [&](const std::string& rel) {
        for (const auto& m : plan.members) if (m.rel == rel) return m.kind;
        return s2k::Kind::Unknown;
    };
    auto destOf = [&](const std::string& rel) {
        for (const auto& m : plan.members) if (m.rel == rel) return m.dest;
        return std::string();
    };
    TEST("inventory: 13 members", plan.ok && plan.members.size() == 13);
    TEST("inventory: PCM, PCG, KMP, SBL are CARRY", kindOf("PCM/RAM01.PCM") == s2k::Kind::Carry && kindOf("SOUND/USER01.PCG") == s2k::Kind::Carry &&
         kindOf("SOUND/USERDK01.PCG") == s2k::Kind::Carry && kindOf("MULTISMP/RAM.KMP") == s2k::Kind::Carry && kindOf("SONGBOOK/LISTDB.SBL") == s2k::Kind::Carry);
    TEST("inventory: CARRY destination keeps the type folder and lower-cases the file name",
         destOf("PCM/RAM01.PCM") == "PCM/ram01.pcm" && destOf("SOUND/USERDK01.PCG") == "SOUND/userdk01.pcg" &&
         destOf("MULTISMP/RAM.KMP") == "MULTISMP/ram.kmp" && destOf("SONGBOOK/LISTDB.SBL") == "SONGBOOK/listdb.sbl");
    TEST("inventory: STY, PRF, PAD, SBD are BLOCKED", kindOf("STYLE/USER01.STY") == s2k::Kind::Blocked && kindOf("KEYBOARDSET/USER01.PRF") == s2k::Kind::Blocked &&
         kindOf("PAD/USER01.PAD") == s2k::Kind::Blocked && kindOf("SONGBOOK/SONGDB.SBD/BANK01.SBD") == s2k::Kind::Blocked);
    TEST("inventory: GLOBAL, VOICEPRESET, GUITARPRESET are DROPPED", kindOf("GLOBAL/GLOBAL.GBL") == s2k::Kind::Dropped &&
         kindOf("VOICEPRESET/VOICE.VOC") == s2k::Kind::Dropped && kindOf("GUITARPRESET/GUITAR.GTR") == s2k::Kind::Dropped);
    TEST("inventory: a file outside the measured table is UNKNOWN, not carried", kindOf("NOTES.TXT") == s2k::Kind::Unknown && destOf("NOTES.TXT").empty());
    TEST("inventory: counts 5 CARRY / 4 BLOCKED / 3 DROPPED / 1 UNKNOWN", plan.count(s2k::Kind::Carry) == 5 && plan.count(s2k::Kind::Blocked) == 4 &&
         plan.count(s2k::Kind::Dropped) == 3 && plan.count(s2k::Kind::Unknown) == 1);
    {
        const fs::path lc = root / "case.SET";
        put(lc / "pcm/ram01.pcm", build({pcmItem(1)}, 1));
        const auto p = s2k::inventory(lc);
        TEST("inventory: matching is exact on the observed upper-case names (pcm/ram01.pcm is UNKNOWN)", p.ok && p.members.size() == 1 && p.members[0].kind == s2k::Kind::Unknown);
    }

    // ---- c) dependency map ----------------------------------------------------------------------------------------
    const auto deps = s2k::dependencies(plan);
    TEST("deps: 1 PCM file, 3 samples with IDs, 1 KMP, 3 KMP references", deps.pcmFiles == 1 && deps.samples == 3 && deps.samplesWithoutId == 0 &&
         deps.kmpFiles == 1 && deps.kmpRefs == 3 && deps.decided);
    TEST("deps: the sample no KMP lists is unreferenced (by name, D10)", deps.unreferenced.size() == 1 && has(deps.unreferenced[0], "Sample 3"));
    TEST("deps: the KMP sample ID no PCM holds is dangling", deps.dangling.size() == 1 && deps.dangling[0] == s2k::hex(id8(9)));
    {
        const fs::path enc = root / "enc.SET";
        Synth noId{0x12020010, 6, 1, 0, "Old", std::string(32, '\x22')};   // Pa700-style PCM record: no field 4
        put(enc / "PCM/RAM01.PCM", build({noId}, 1));
        put(enc / "MULTISMP/RAM.KMP", build({{0x11080018, 5, 0, 0, "RAM", "OC31encoded"}}, 1));
        const auto d = s2k::dependencies(s2k::inventory(enc));
        TEST("deps: an OC31 KMP and ID-less samples leave the map UNKNOWN (nothing called unreferenced)",
             !d.decided && d.kmpEncoded == 1 && d.samplesWithoutId == 1 && d.unreferenced.empty() && d.dangling.empty());
        TEST("deps: the report says UNKNOWN", has(s2k::report(s2k::inventory(enc), d), "UNKNOWN"));
        const fs::path enc2 = root / "enc2.SET";
        put(enc2 / "PCM/RAM01.PCM", build({pcmItem(1)}, 1));
        put(enc2 / "MULTISMP/RAM.KMP", build({{0x11080018, 5, 0, 0, "RAM", "OC31encoded"}}, 1));
        const auto d2 = s2k::dependencies(s2k::inventory(enc2));
        TEST("deps: an OC31 KMP alone leaves the map UNKNOWN even when every sample has an ID",
             !d2.decided && d2.kmpEncoded == 1 && d2.samplesWithoutId == 0 && d2.unreferenced.empty());
        const fs::path bad = root / "badkmp.SET";
        std::string p;
        be16(p, 0x0103); be32(p, 1000); p.resize(85 + 122, '\0');   // count larger than the table
        put(bad / "MULTISMP/RAM.KMP", build({{0x11080018, 5, 0, 0, "RAM", p}}, 1));
        const auto b = s2k::dependencies(s2k::inventory(bad));
        TEST("deps: a sample count past the item is not read (references UNKNOWN)", !b.decided && b.kmpOther == 1 && b.kmpRefs == 0);
    }

    // ---- b) the converter as an export job ------------------------------------------------------------------------
    const auto srcBefore = tree(set);
    const fs::path out = root / "out";
    fs::create_directories(out);
    const auto st = run(setToKst(set, out / "DEMO.KST"));
    TEST("job: published", st.phase == Phase::Done);
    if (st.phase != Phase::Done) std::fprintf(stderr, "  reason: %s\n", st.reason.c_str());
    const auto got = fs::exists(out / "DEMO.KST") ? tree(out / "DEMO.KST") : std::map<std::string, std::string>{};
    const std::map<std::string, std::string> want = {
        {"PCM/ram01.pcm", srcBefore.at("PCM/RAM01.PCM")},         {"SOUND/user01.pcg", srcBefore.at("SOUND/USER01.PCG")},
        {"SOUND/userdk01.pcg", srcBefore.at("SOUND/USERDK01.PCG")}, {"MULTISMP/ram.kmp", srcBefore.at("MULTISMP/RAM.KMP")},
        {"SONGBOOK/listdb.sbl", srcBefore.at("SONGBOOK/LISTDB.SBL")}};
    TEST("job: exactly the CARRY members, byte-identical, at their lower-cased paths", got == want);
    TEST("job: no STYLE / KEYBOARDSET / PAD / SONGDB.SBD and no GLOBAL is written",
         !fs::exists(out / "DEMO.KST/STYLE") && !fs::exists(out / "DEMO.KST/KEYBOARDSET") && !fs::exists(out / "DEMO.KST/PAD") &&
         !fs::exists(out / "DEMO.KST/SONGBOOK/SONGDB.SBD") && !fs::exists(out / "DEMO.KST/GLOBAL"));
    TEST("job: the SET is unchanged", tree(set) == srcBefore);
    TEST("job: L1 passed (staged files = source bytes, closure, validator)", st.level.l1.state == ui::studio::Check::Passed);
    TEST("job: L2/L3 not claimed", st.level.l2.state != ui::studio::Check::Passed && st.level.l3.state != ui::studio::Check::Passed);
    const std::string rep = st.writerText;
    TEST("report: label STRUCTURALLY VALID - NOT VERIFIED ON A PA5X", has(rep, s2k::kLabel));
    bool everyListed = true;
    for (const auto& m : plan.members)
        if (m.kind != s2k::Kind::Carry) everyListed &= has(rep, std::string(s2k::kindName(m.kind)) + "  " + m.rel + "  (" + m.reason + ")");
    TEST("report: every blocked / dropped / unknown member is listed with its reason", everyListed);
    TEST("report: dependency map with 1 unreferenced and 1 dangling", has(rep, "Unreferenced samples: 1; dangling KMP references: 1"));
    TEST("report: says the KST is not a full conversion", has(rep, "NOT a full conversion"));
    const auto md = slurp(st.reportPath);
    size_t missing = 0, exact = 0;
    for (const auto& [k, v] : st.conversion) { missing += k == "MISSING"; exact += k == "EXACT"; }
    TEST("job report: conversion lists 5 EXACT and 8 MISSING (4 blocked, 3 dropped, 1 unknown)", exact == 5 && missing == 8);
    TEST("job report beside the KST holds the CONVERSION report", has(md, "BLOCKED  STYLE/USER01.STY") && has(md, "DROPPED  GLOBAL/GLOBAL.GBL"));

    // ---- refusals -------------------------------------------------------------------------------------------------
    {
        const auto again = run(setToKst(set, out / "DEMO.KST"));
        TEST("refuse: an existing output path", again.phase == Phase::Failed && has(again.reason, "already exists") && tree(out / "DEMO.KST") == got);
        const auto noExt = run(setToKst(set, out / "DEMO"));
        TEST("refuse: an output not named NAME.KST", noExt.phase == Phase::Failed && has(noExt.reason, ".KST") && !fs::exists(out / "DEMO"));
        const auto inside = run(setToKst(set, set / "IN.KST"));
        TEST("refuse: an output inside the SET", inside.phase == Phase::Failed && !fs::exists(set / "IN.KST"));
        const fs::path broken = root / "broken.SET";
        put(broken / "PCM/RAM01.PCM", "not korf");
        const auto b = run(setToKst(broken, out / "BROKEN.KST"));
        TEST("refuse: a CARRY member that is not a valid KORF container (nothing published)",
             b.phase == Phase::Failed && has(b.reason, "not a valid KORF") && !fs::exists(out / "BROKEN.KST"));
        const fs::path none = root / "none.SET";
        put(none / "STYLE/USER01.STY", "x");
        const auto n = run(setToKst(none, out / "NONE.KST"));
        TEST("refuse: a SET with nothing to carry", n.phase == Phase::Failed && has(n.reason, "no resource") && !fs::exists(out / "NONE.KST"));
        s2k::Plan twin;
        twin.ok = true;
        twin.members = {{"PCM/RAM01.PCM", s2k::Kind::Carry, "PCM/ram01.pcm"}, {"PCM/ram01.PCM", s2k::Kind::Carry, "PCM/ram01.pcm"}};
        TEST("refuse: two members with the same destination without case", has(s2k::refusal(twin), "two members map to"));
    }
    {   // the source changes after validation: the write refuses (the published bytes are the validated bytes)
        const fs::path moving = root / "moving.SET";
        makeSet(moving, {1}, {1});
        auto spec = setToKst(moving, out / "MOVING.KST");
        TEST("source change: validates", spec.validate().empty());
        put(moving / "SOUND/USER01.PCG", build({{0x10000038, 4, 9, 0, "Other", "OC31other"}}, 1));
        const fs::path stage = root / "stage";
        fs::create_directories(stage);
        const int fd = ::open(stage.c_str(), O_RDONLY | O_DIRECTORY);
        const auto w = spec.writeAt(fd, stage, [](double) { return true; });
        ::close(fd);
        TEST("source change: the write refuses", has(w.error, "changed"));
    }
    {   // verify refuses a staged file that differs from the validated source bytes
        const fs::path v = root / "verify.SET";
        makeSet(v, {1}, {1});
        auto spec = setToKst(v, out / "VERIFY.KST");
        spec.validate();
        const fs::path stage = root / "stage2";
        fs::create_directories(stage);
        const int fd = ::open(stage.c_str(), O_RDONLY | O_DIRECTORY);
        const auto w = spec.writeAt(fd, stage, [](double) { return true; });
        ::close(fd);
        TEST("verify: the written stage passes", w.error.empty() && spec.verify(stage).first.l1.state == ui::studio::Check::Passed);
        put(stage / "SONGBOOK/listdb.sbl.extra", "x");
        TEST("verify: an extra staged file fails L1", spec.verify(stage).first.l1.state == ui::studio::Check::Failed);
        fs::remove(stage / "SONGBOOK/listdb.sbl.extra");
        auto b = slurp(stage / "SONGBOOK/listdb.sbl");
        b.replace(b.find("OC31list"), 8, "OC31LIST");   // same size, still a valid KORF container
        fs::remove(stage / "SONGBOOK/listdb.sbl");
        put(stage / "SONGBOOK/listdb.sbl", b);
        TEST("verify: a staged file that differs from the source (same size, valid KORF) fails L1", spec.verify(stage).first.l1.state == ui::studio::Check::Failed);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
