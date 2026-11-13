// Pa5X Import Kit (jobs/import_kit.h, roadmap E9): layout, FAT-safe naming, manifest.json/KIT.md/PACKAGE.sha256, the
// round trip through the independent readers, fail-closed on a corrupted member, interruption leaves only NAME.INCOMPLETE,
// sources unchanged. Synthetic data only (demo organ, demo drum kit, the repo's synthetic Style).

#include "jobs/import_kit.h"
#include "media/kst_diff.h"
#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include "media/sound/korg_kmp_writer.h"
#include "media/sound/package_writer.h"
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;
namespace k = ai_arranger::jobs::kit;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
static void flipLastByte(const fs::path& p) {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(-1, std::ios::end);
    const char c = char(f.get());
    f.seekp(-1, std::ios::end);
    f.put(char(c ^ 0x5a));
}
static JobState run(const ExportSpec& s, Phase cancelAt = Phase::Queued, double cancelAtProgress = 2) {
    JobState st;
    st.id = 1;
    runExport(s, st, [&] { return (cancelAt != Phase::Queued && st.phase == cancelAt) || st.progress >= cancelAtProgress; }, [](const JobState&) {});
    return st;
}
// path -> SHA-256 of every file below root (a file root: itself)
static std::map<std::string, std::string> hashes(const fs::path& root) {
    std::map<std::string, std::string> r;
    for (const auto& f : listFiles(root)) {
        bool c = false;
        std::string e;
        r[f.rel] = media::detail::sha256File(fs::is_directory(root) ? root / f.rel : root, [](size_t) { return true; }, &c, &e);
    }
    return r;
}
// Rewrites dir/PACKAGE.sha256 for the files now in dir (what someone re-signing a tampered folder would do).
static void resign(const fs::path& dir) {
    std::string man;
    int n = 0;
    for (const auto& f : listFiles(dir))
        if (f.rel != "PACKAGE.sha256" && !k::detail::finderLitter(f.rel)) {
            bool c = false;
            std::string e;
            man += media::detail::sha256File(dir / f.rel, [](size_t) { return true; }, &c, &e) + "  " + f.rel + "\n";
            ++n;
        }
    put(dir / "PACKAGE.sha256", man + "# " + std::to_string(n) + " files\n");
}
static void replaceIn(const fs::path& p, const std::string& from, const std::string& to) {
    auto t = slurp(p);
    const auto at = t.find(from);
    if (at != std::string::npos) t.replace(at, from.size(), to);
    put(p, t);
}
static size_t countOf(const std::string& s, const std::string& w) { size_t n = 0; for (size_t p = 0; (p = s.find(w, p)) != std::string::npos; ++p) ++n; return n; }
static std::vector<std::string> named(const fs::path& dir, const std::string& stem) {
    std::vector<std::string> r;
    for (const auto& e : fs::directory_iterator(dir)) if (e.path().filename().string().rfind(stem, 0) == 0) r.push_back(e.path().filename().string());
    std::sort(r.begin(), r.end());
    return r;
}

int main() {
    std::printf("Test: Pa5X Import Kit\n");
    const fs::path root = fs::temp_directory_path() / ("aia-import-kit-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path out = root / "outputs";
    fs::create_directories(out);

    // ── synthetic AIArranger outputs, written by the existing writers ──
    const auto organ = media::sound::buildDemoOrgan();
    std::string err;
    bool ok = !media::sound::writePackage(organ, out / "AIA Demo Organ", "synthetic", &err).empty();
    ok &= !media::sound::writeDrumPackage(media::sound::buildDemoKit(), out / "AIA Demo Kit", "synthetic", &err).empty();
    ok &= !media::sound::writeKorgFolder(organ, out / "AIA Organ KMP", "", &err).empty();
    ui::studio::StyleStudioModel m;
    ok &= m.load(fs::path(REPO_DIR) / "fixtures/synthetic/SYNTH_WALTZ_68.sty", &err);
    const auto smf = style_edit::exportPa5xSmf(m.style());
    ok &= smf.ok && media::writeNewFile(out / "Waltz.mid", smf.bytes).empty();
    TEST("fixtures written by the existing writers", ok);
    const std::vector<fs::path> inputs = {out / "AIA Demo Organ", out / "AIA Demo Kit", out / "AIA Organ KMP", out / "Waltz.mid"};
    const auto before = hashes(out);

    // ── classification and FAT-name rules ──
    TEST("classify: Sound, Drum Kit, KMP, Style", k::classify(inputs[0]).kind == k::Kind::Sound && k::classify(inputs[1]).kind == k::Kind::DrumKit &&
         k::classify(inputs[2]).kind == k::Kind::Kmp && k::classify(inputs[3]).kind == k::Kind::Style);
    TEST("classify: the selected files per import page",
         k::classify(inputs[0]).select == std::vector<std::string>{"tx/AIA Demo Organ/AIA Demo Organ.txprog"} &&
         k::classify(inputs[0]).alt == std::vector<std::string>{"AIA_Demo_Organ.sf2"} &&
         k::classify(inputs[1]).select == std::vector<std::string>{"tx/AIA Demo Kit/AIA Demo Kit.txperf"} && k::classify(inputs[2]).select.size() == 2);
    TEST("FAT names: ordinary names pass", k::detail::fatName("AIA Demo Organ").empty() && k::detail::fatName("MS0001.KSF").empty());
    TEST("FAT names: forbidden characters, trailing dot/space, device names, 256 chars refused",
         !k::detail::fatName("a:b").empty() && !k::detail::fatName("a?").empty() && !k::detail::fatName("a|b").empty() &&
         !k::detail::fatName(std::string("a\x01")).empty() && !k::detail::fatName("name.").empty() && !k::detail::fatName("name ").empty() &&
         !k::detail::fatName("CON").empty() && !k::detail::fatName("lpt1.mid").empty() && !k::detail::fatName(std::string(256, 'a')).empty() &&
         k::detail::fatName(std::string(255, 'a')).empty());
    TEST("outputsIn lists the visible entries", k::outputsIn(out) == inputs || [&] {
        auto v = inputs; std::sort(v.begin(), v.end()); return k::outputsIn(out) == v; }());

    // ── the kit: layout, manifest, KIT.md, levels ──
    {
        const auto st = run(k::importKit(inputs, root / "AIA Kit"));
        TEST("kit job done through the E6 runner", st.phase == Phase::Done && st.reason.empty());
        const fs::path kit = root / "AIA Kit";
        TEST("layout: one folder per import route, the three kit files", fs::is_regular_file(kit / "KIT.md") && fs::is_regular_file(kit / "manifest.json") &&
             fs::is_regular_file(kit / "PACKAGE.sha256") && fs::is_regular_file(kit / "SOUNDS/AIA Demo Organ/tx/AIA Demo Organ/AIA Demo Organ.txprog") &&
             fs::is_regular_file(kit / "SOUNDS/AIA Demo Organ/AIA_Demo_Organ.sf2") &&
             fs::is_regular_file(kit / "DRUMKITS/AIA Demo Kit/tx/AIA Demo Kit/AIA Demo Kit.txperf") &&
             fs::is_regular_file(kit / "KMP/AIA Organ KMP/PACKAGE.sha256") && fs::is_regular_file(kit / "STYLES/Waltz.mid"));
        size_t top = 0;
        for (const auto& e : fs::directory_iterator(kit)) top += !e.path().filename().empty();
        TEST("layout: nothing else at the top", top == 7);
        TEST("members are byte copies of their sources", media::diff(inputs[0], kit / "SOUNDS/AIA Demo Organ").same() &&
             media::diff(inputs[1], kit / "DRUMKITS/AIA Demo Kit").same() && media::diff(inputs[2], kit / "KMP/AIA Organ KMP").same() &&
             slurp(inputs[3]) == slurp(kit / "STYLES/Waltz.mid"));
        TEST("published, no NAME.INCOMPLETE, report beside", (named(root, "AIA Kit") == std::vector<std::string>{"AIA Kit", "AIA Kit.export.json", "AIA Kit.export.md"}));
        TEST("level: L1 passed (whole kit re-read), L2 not claimed, L3 never", st.level.l1.state == Check::Passed && has(st.level.l1.detail, k::kLabel) &&
             st.level.l2.state == Check::NotRun && st.level.l3.state == Check::NotRun);
        TEST("per-item status in the job report: 4 x STRUCTURALLY VALID",
             std::count_if(st.notes.begin(), st.notes.end(), [](const std::string& n) { return n.rfind("STRUCTURALLY VALID: ", 0) == 0; }) == 4);
        const auto js = slurp(kit / "manifest.json"), md = slurp(kit / "KIT.md");
        TEST("manifest.json: schema, label, not hardware verified", has(js, "\"schema\": \"aiarranger-pa5x-import-kit/1\"") &&
             has(js, std::string("\"label\": \"") + k::kLabel + "\"") && has(js, "\"hardwareVerified\": false"));
        TEST("manifest.json: every member with its path, format, selected file, pages, status",
             has(js, "\"path\": \"SOUNDS/AIA Demo Organ\"") && has(js, "\"importFormat\": \"TX Sound (TXPROG)\"") &&
             has(js, "\"select\": [\"SOUNDS/AIA Demo Organ/tx/AIA Demo Organ/AIA Demo Organ.txprog\"]") &&
             has(js, "\"alternative\": [\"SOUNDS/AIA Demo Organ/AIA_Demo_Organ.sf2\"]") && has(js, "\"path\": \"DRUMKITS/AIA Demo Kit\"") &&
             has(js, "\"path\": \"KMP/AIA Organ KMP\"") && has(js, "\"select\": [\"STYLES/Waltz.mid\"]") && has(js, "MAN p.1037") &&
             std::count(js.begin(), js.end(), '{') == 5 && countOf(js, "\"status\": \"STRUCTURALLY VALID\"") == 4);
        TEST("KIT.md: label, backup first, FAT stick, shasum check", has(md, std::string("**") + k::kLabel + "**") && has(md, "MAN p.7, pp.789-791") &&
             has(md, "FAT32 or FAT16 with long file names (MAN p.715)") && has(md, "shasum -a 256 -c PACKAGE.sha256"));
        const auto pos = [&](const char* s) { return md.find(s); };
        TEST("KIT.md: steps in import order (Drum Kit, Sound, KMP, Style), each with its pages",
             pos("Step 1: Drum Kit") < pos("Step 2: Sound") && pos("Step 2: Sound") < pos("Step 3: Multisamples") &&
             pos("Step 3: Multisamples") < pos("Step 4: Style") && pos("Step 4: Style") != std::string::npos && has(md, "TX DrumKit (TXPERF)** (MAN p.1206)") &&
             has(md, "(MAN p.1210)") && has(md, "(MAN p.1209)") && has(md, "Import SMF (MAN p.1037)") && has(md, "Sample Edit > Menu > Import/Export > Import"));
        TEST("KIT.md: what the Pa5X decides (slots), UNPROVEN Style Sounds", has(md, "AIArranger assigns no slot") && has(md, "UNPROVEN"));
        TEST("never called compatible", !has(md, "ompatible") && !has(js, "ompatible") && !has(st.reportMd, "ompatible"));
        TEST("PACKAGE.sha256 lists every kit file (members' own manifests included)", [&] {
            const auto man = slurp(kit / "PACKAGE.sha256");
            const auto all = listFiles(kit);
            return has(man, "# " + std::to_string(all.size() - 1) + " files") && has(man, "  SOUNDS/AIA Demo Organ/PACKAGE.sha256\n") && has(man, "  KIT.md\n");
        }());
        std::string text;
        TEST("the published kit re-verifies standalone", k::verifyKit(kit, nullptr, &text).empty() && has(text, k::kLabel));

        // a copy on a stick: Finder litter is ignored; an extra, a missing or a changed file is not
        const fs::path stick = root / "stick" / "AIA Kit";
        fs::create_directories(stick.parent_path());
        fs::copy(kit, stick, fs::copy_options::recursive);
        put(stick / ".DS_Store", "x");
        put(stick / "SOUNDS/._AIA Demo Organ", "x");
        TEST("stick copy with Finder litter verifies", k::verifyKit(stick, nullptr, &text).empty());
        put(stick / "SOUNDS/AIA Demo Organ/extra.wav", "x");
        TEST("stick copy with an extra file fails", !k::verifyKit(stick, nullptr, &text).empty());
        fs::remove(stick / "SOUNDS/AIA Demo Organ/extra.wav");
        flipLastByte(stick / "STYLES/Waltz.mid");
        TEST("stick copy with a changed byte fails", has(k::verifyKit(stick, nullptr, &text), "PACKAGE.sha256"));
        // a member that matches a re-signed kit manifest but is not a Style SMF: only the member's reader can catch it
        put(stick / "STYLES/Waltz.mid", "MThd not a style");
        resign(stick);
        TEST("re-signed kit with a broken Style: the Style SMF reader fails it", has(k::verifyKit(stick, nullptr, &text), "STYLES/Waltz.mid: Korg Style SMF reader"));

        // ── Codex r1 P2: manifest.json is parsed and must match the members exactly (each case re-signed) ──
        const auto tamper = [&](const char* what, const std::function<void(const fs::path&)>& edit) {
            const fs::path t = root / "tamper" / what / "AIA Kit";
            fs::create_directories(t.parent_path());
            fs::copy(kit, t, fs::copy_options::recursive);
            edit(t / "manifest.json");
            resign(t);
            std::string tx;
            return k::verifyKit(t, nullptr, &tx);
        };
        TEST("manifest.json untouched copy verifies", tamper("none", [](const fs::path&) {}).empty());
        TEST("manifest.json invalid JSON fails verify", has(tamper("invalid", [](const fs::path& m) { auto t = slurp(m); t.erase(t.rfind('}')); put(m, t); }), "not valid JSON"));
        TEST("manifest.json hardwareVerified true fails verify",
             has(tamper("hw", [](const fs::path& m) { replaceIn(m, "\"hardwareVerified\": false", "\"hardwareVerified\": true"); }), "hardwareVerified must be false"));
        TEST("manifest.json with an extra entry fails verify", !tamper("extra", [](const fs::path& m) {
            auto t = slurp(m);
            const auto a = t.find("\n    {\"step\": 1"), b = t.find(",\n    {\"step\": 2");
            t.insert(t.rfind("\n  ]"), "," + t.substr(a, b - a));
            put(m, t);
        }).empty());
        TEST("manifest.json with a missing entry fails verify", has(tamper("missing", [](const fs::path& m) {
            auto t = slurp(m);
            const auto a = t.find(",\n    {\"step\": 4"), b = t.rfind("\n  ]");
            t.erase(a, b - a);
            put(m, t);
        }), "lists 3 members"));
        TEST("manifest.json with another label or schema fails verify",
             has(tamper("label", [](const fs::path& m) { replaceIn(m, " — NOT VERIFIED ON A PA5X", ""); }), "label") &&
             has(tamper("schema", [](const fs::path& m) { replaceIn(m, "import-kit/1", "import-kit/2"); }), "schema"));
        TEST("manifest.json with a wrong selected file fails verify",
             has(tamper("select", [](const fs::path& m) { replaceIn(m, "AIA Demo Organ.txprog", "Other.txprog"); }), "select does not match"));
        TEST("manifest.json with an unknown field fails verify",
             has(tamper("field", [](const fs::path& m) { replaceIn(m, "\"step\": 1,", "\"step\": 1, \"slot\": \"U1\","); }), "manifest.json"));
    }
    // ── the strict JSON reader ──
    {
        k::Json j;
        TEST("JSON: escapes and a surrogate pair decode", k::parseJson("{\"a\": \"x\\n\\u00e9\\ud83c\\udfb9\\/\", \"b\": [1, -2.5e3, true, null]}", &j).empty() &&
             j.get("a") && j.get("a")->s == "x\n\xC3\xA9\xF0\x9F\x8E\xB9/" && j.get("b")->a.size() == 4 && j.get("b")->a[1].s == "-2.5e3");
        for (const char* bad : {"{\"a\": 1,}", "{\"a\": 1} x", "{\"a\": 1, \"a\": 2}", "[01]", "\"\\ud800\"", "\"\\uZZ00\"", "\"a\x01\"", "{\"a\" 1}", "[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]", ""}) {
            k::Json x;
            TEST((std::string("JSON: refused: ") + bad).c_str(), !k::parseJson(bad, &x).empty());
        }
    }
    TEST("sources unchanged by the kit", hashes(out) == before);

    // ── Codex r1 P1: a validated source file swapped for a link before the copy is never read through it ──
    for (const char* which : {"STYLES", "SOUNDS"}) {
        const fs::path src = root / ("swap-" + std::string(which)), outside = root / ("outside-" + std::string(which));
        fs::create_directories(src);
        fs::create_directories(outside);
        const bool style = std::string(which) == "STYLES";
        const fs::path member = style ? src / "Waltz.mid" : src / "AIA Demo Organ";
        fs::copy(style ? inputs[3] : inputs[0], member, fs::copy_options::recursive);
        const fs::path victim = style ? member : member / "REPORT.txt";
        auto s = k::importKit({member}, root / ("Swap Kit " + std::string(which)));
        const auto real = s.dependencies;   // runs after validate, before the copy: the swap happens in between
        s.dependencies = [&, real](std::vector<std::string>& notes, uint64_t& b) {
            auto r = real(notes, b);
            fs::copy_file(victim, outside / victim.filename());   // the same bytes, outside the source root
            fs::remove(victim);
            fs::create_symlink(outside / victim.filename(), victim);
            return r;
        };
        const auto st = run(s);
        TEST(("swap for a link after validation (" + std::string(which) + "): FAILED at write, nothing published").c_str(),
             st.phase == Phase::Failed && st.stoppedAt == Phase::Run && has(st.reason, "without following a link") &&
             named(root, "Swap Kit " + std::string(which)) == std::vector<std::string>{"Swap Kit " + std::string(which) + ".INCOMPLETE"});
    }

    // ── Codex r1 P2: a Drum Kit's .txperf is read (garbage, re-signed, is refused) ──
    {
        const fs::path dk = root / "txperf" / "AIA Demo Kit";
        fs::create_directories(dk.parent_path());
        fs::copy(inputs[1], dk, fs::copy_options::recursive);
        put(dk / "tx/AIA Demo Kit/AIA Demo Kit.txperf", "garbage, not a performance");
        resign(dk);
        TEST("garbage .txperf, re-signed: the package check refuses it", has(media::sound::verifyPackage(dk).text, ".txperf"));
        const auto st = run(k::importKit({dk}, root / "Perf Kit"));
        TEST("garbage .txperf, re-signed: the kit is FAILED at the member check", st.phase == Phase::Failed && st.stoppedAt == Phase::Dependencies &&
             has(st.reason, ".txperf") && named(root, "Perf Kit").empty());
        fs::copy_file(inputs[1] / "tx/AIA Demo Kit/AIA Demo Kit.txperf", dk / "tx/AIA Demo Kit/AIA Demo Kit.txperf", fs::copy_options::overwrite_existing);
        replaceIn(dk / "tx/AIA Demo Kit/AIA Demo Kit.txperf", "tx:program=\"AIA Demo Kit.txprog\"", "tx:program=\"../x.txprog\"");
        resign(dk);
        TEST(".txperf naming a program outside its folder: refused", has(media::sound::verifyPackage(dk).text, "is not a .txprog file name"));
        fs::copy_file(inputs[1] / "tx/AIA Demo Kit/AIA Demo Kit.txperf", dk / "tx/AIA Demo Kit/AIA Demo Kit.txperf", fs::copy_options::overwrite_existing);
        resign(dk);
        TEST("the real .txperf passes", media::sound::verifyPackage(dk).ok);
    }

    // ── Codex r1 P2: a member file above 64 MiB is streamed, not refused (sparse 70 MiB SF2) ──
    {
        const fs::path big = root / "big" / "AIA Demo Organ";
        fs::create_directories(big.parent_path());
        fs::copy(inputs[0], big, fs::copy_options::recursive);
        fs::resize_file(big / "AIA_Demo_Organ.sf2", uint64_t(70) << 20);   // zero tail after the RIFF chunk (sparse on APFS)
        resign(big);
        const auto st = run(k::importKit({big}, root / "Big Kit"));
        TEST("70 MiB SF2 member: kit done, the copy is the same bytes", st.phase == Phase::Done &&
             fs::file_size(root / "Big Kit/SOUNDS/AIA Demo Organ/AIA_Demo_Organ.sf2") == (uint64_t(70) << 20) &&
             hashes(big / "AIA_Demo_Organ.sf2") == hashes(root / "Big Kit/SOUNDS/AIA Demo Organ/AIA_Demo_Organ.sf2"));
    }

    // ── Codex r1 P2: a folder of outputs is listed by the job, not when the job is made ──
    {
        const fs::path later = root / "later";
        auto s = k::importKit({}, root / "Later Kit", later);   // the folder does not exist yet
        fs::create_directories(later);
        fs::copy(inputs[3], later / "Waltz.mid");
        k::lastListingThread = 0;
        const auto st = run(s);
        TEST("folder of outputs: listed inside the job (validate), on the job's thread", st.phase == Phase::Done &&
             fs::is_regular_file(root / "Later Kit/STYLES/Waltz.mid") && k::lastListingThread.load() == thisThread() && st.source == later.string());
        const auto in = run(k::importKit({}, later / "kit", later));
        TEST("folder of outputs: a kit inside that folder is refused (D18)", in.phase == Phase::Failed && in.stoppedAt == Phase::Destination);
    }

    // ── fail closed: a corrupted source member stops the kit before anything is created ──
    {
        const fs::path bad = root / "bad" / "AIA Demo Organ";
        fs::create_directories(bad.parent_path());
        fs::copy(inputs[0], bad, fs::copy_options::recursive);
        flipLastByte(bad / "tx/AIA Demo Organ/AIA Demo Organ.txprog");
        const auto st = run(k::importKit({bad, inputs[1]}, root / "Bad Kit"));
        TEST("corrupted member: FAILED at the member check, listed as FAILED", st.phase == Phase::Failed && st.stoppedAt == Phase::Dependencies &&
             has(st.reason, "FAILED: SOUNDS/AIA Demo Organ") && !st.missing.empty());
        TEST("corrupted member: the other member is reported STRUCTURALLY VALID", std::any_of(st.notes.begin(), st.notes.end(), [](const std::string& n) {
                 return n.rfind("STRUCTURALLY VALID: DRUMKITS/AIA Demo Kit", 0) == 0; }));
        TEST("corrupted member: nothing created", named(root, "Bad Kit").empty());
    }
    // ── fail closed: the copy differs from what was checked (the disk returns other bytes) -> not published ──
    for (const char* member : {"SOUNDS/AIA Demo Organ/wav", "KMP/AIA Organ KMP", "STYLES/Waltz.mid"}) {
        auto s = k::importKit(inputs, root / "Torn Kit");
        const auto real = s.write;
        const std::string mem = member;
        s.write = [real, mem](const fs::path& o, const std::function<bool(double)>& p) {
            auto r = real(o, p);
            fs::path victim = o / mem;
            if (fs::is_directory(victim)) for (const auto& e : fs::directory_iterator(victim)) if (e.is_regular_file()) { victim = e.path(); break; }
            flipLastByte(victim);
            return r;
        };
        const auto st = run(s);
        TEST(("corrupted copy (" + mem + "): FAILED at verify, not published, only NAME.INCOMPLETE kept").c_str(),
             st.phase == Phase::Failed && st.stoppedAt == Phase::Verify && named(root, "Torn Kit") == std::vector<std::string>{"Torn Kit.INCOMPLETE"});
        fs::remove_all(root / "Torn Kit.INCOMPLETE");
    }

    // ── interruption: cancel at every phase up to publish, and inside the copy -> at most NAME.INCOMPLETE ──
    for (Phase p : {Phase::Validate, Phase::Dependencies, Phase::Destination, Phase::Preview, Phase::Estimate, Phase::Run, Phase::Manifest,
                    Phase::Verify, Phase::Publish}) {
        const auto st = run(k::importKit(inputs, root / "Cut Kit"), p);
        const auto left = named(root, "Cut Kit");
        TEST((std::string("cancel at ") + phaseName(p) + ": cancelled, nothing published, at most NAME.INCOMPLETE").c_str(),
             st.phase == Phase::Cancelled && (left.empty() || left == std::vector<std::string>{"Cut Kit.INCOMPLETE"}));
        fs::remove_all(root / "Cut Kit.INCOMPLETE");
    }
    {
        // the write phase spans progress [0.5, 0.6); cancel half way through the member copy
        const auto st = run(k::importKit(inputs, root / "Mid Kit"), Phase::Queued, 0.55);
        TEST("cancel inside the copy: cancelled while writing, only NAME.INCOMPLETE (partial copy) kept",
             st.phase == Phase::Cancelled && st.stoppedAt == Phase::Run && named(root, "Mid Kit") == std::vector<std::string>{"Mid Kit.INCOMPLETE"} &&
             fs::exists(root / "Mid Kit.INCOMPLETE/Mid Kit") && !fs::exists(root / "Mid Kit.INCOMPLETE/Mid Kit/PACKAGE.sha256"));
        const auto again = run(k::importKit(inputs, root / "Mid Kit"));
        TEST("re-run without consent refuses the leftover", again.phase == Phase::Failed && has(again.reason, "INCOMPLETE"));
    }
    TEST("sources unchanged after failures and interruptions", hashes(out) == before);

    // ── refusals before anything is created ──
    {
        fs::create_directories(root / "junk/stuff");
        put(root / "junk/stuff/a.txt", "x");
        const auto a = run(k::importKit({root / "junk/stuff"}, root / "K1"));
        TEST("not an AIArranger output: refused at validate", a.phase == Phase::Failed && a.stoppedAt == Phase::Validate && has(a.reason, "not an AIArranger output"));
        const auto b = run(k::importKit({}, root / "K2"));
        TEST("no outputs: refused", b.phase == Phase::Failed && has(b.reason, "no outputs"));
        const auto c = run(k::importKit({inputs[3]}, root / "bad:kit"));
        TEST("kit name FAT-unsafe: refused", c.phase == Phase::Failed && has(c.reason, "kit name"));
        put(root / "x1/Song?.mid", std::string(smf.bytes.begin(), smf.bytes.end()));
        const auto d = run(k::importKit({root / "x1/Song?.mid"}, root / "K4"));
        TEST("member name FAT-unsafe: refused", d.phase == Phase::Failed && d.stoppedAt == Phase::Validate && has(d.reason, "FAT"));
        put(root / "x2/waltz.mid", std::string(smf.bytes.begin(), smf.bytes.end()));
        const auto e = run(k::importKit({inputs[3], root / "x2/waltz.mid"}, root / "K5"));
        TEST("two names equal without case: refused (FAT ignores case)", e.phase == Phase::Failed && has(e.reason, "ignores case"));
        const fs::path linked = root / "linked/AIA Demo Kit";
        fs::create_directories(linked.parent_path());
        fs::copy(inputs[1], linked, fs::copy_options::recursive);
        fs::create_symlink("/etc/hosts", linked / "hosts");
        const auto f = run(k::importKit({linked}, root / "K6"));
        TEST("a symlink inside a member: refused", f.phase == Phase::Failed && f.stoppedAt == Phase::Validate && has(f.reason, "link"));
        const auto g = run(k::importKit({inputs[0]}, inputs[0] / "kit"));
        TEST("kit inside a source: refused (D18)", g.phase == Phase::Failed && g.stoppedAt == Phase::Destination && !fs::exists(inputs[0] / "kit.INCOMPLETE"));
        TEST("no refused kit created anything", named(root, "K").empty() && named(root, "bad:kit").empty());
    }
    TEST("sources unchanged at the end", hashes(out) == before);

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
