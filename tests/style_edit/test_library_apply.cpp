// E8 Library Manager plan EXECUTION (library/library_apply.h through the E6 job, jobs::libraryApply): every operation,
// every refusal, interruption (only NAME.INCOMPLETE remains), a source changed before the copy or before publishing,
// a step that would add a validator ERROR, and both sources byte-identical after every case. Synthetic libraries only
// (placeholder bytes; Pa-native containers from tests/support/panative_fixture.h); no owner data.

#include "jobs/export_specs.h"
#include "../support/panative_fixture.h"
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;
using library::Op;
using library::Step;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
// Plain test bytes, not the placeholder marker: copying placeholder bytes into a library is itself an ERROR (R-10)
static std::string blob(const std::string& tag) { return "synthetic test bytes, not a Korg file: " + tag + "\n"; }
static std::map<std::string, std::string> tree(const fs::path& root) {   // every file's bytes, plus every folder
    std::map<std::string, std::string> m;
    for (const auto& e : fs::recursive_directory_iterator(root)) m[fs::relative(e.path(), root).generic_string()] = e.is_regular_file() ? slurp(e.path()) : "<dir>";
    return m;
}
static std::vector<std::string> named(const fs::path& dir, const std::string& stem) {
    std::vector<std::string> r;
    for (const auto& e : fs::directory_iterator(dir)) if (e.path().filename().string().rfind(stem, 0) == 0) r.push_back(e.path().filename().string());
    std::sort(r.begin(), r.end());
    return r;
}
using Lib = std::shared_ptr<const library::Library>;
static Lib open(const fs::path& p) { return std::make_shared<const library::Library>(library::load(p)); }
static size_t item(const Lib& L, const std::string& name) {
    for (size_t i = 0; i < L->items.size(); ++i) if (library::trimmed(L->items[i].name) == name) return i;
    return size_t(-1);
}
static JobState run(const ExportSpec& s, Phase cancelAt = Phase::Queued) {
    JobState st;
    st.id = 8;
    runExport(s, st, [&] { return cancelAt != Phase::Queued && st.phase == cancelAt; }, [](const JobState&) {});
    return st;
}

int main() {
    std::printf("Test: library plan execution (E8, file level)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-library-apply-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path ka = root / "A.KST", kb = root / "B.KST", out = root / "out";
    put(ka / "STYLE/USER01/Ballad.STG", blob("ballad"));
    put(ka / "STYLE/USER01/Swing.STG", blob("swing"));
    put(ka / "STYLE/USER02/Empty.STG", "");   // copying it would add an R-07 ERROR to the output
    put(ka / "PAD/USER01/Hit.PDC", blob("hit"));
    put(kb / "STYLE/USER01/Waltz.STG", blob("waltz"));
    put(kb / "STYLE/USER03/Tango.STG", blob("tango"));
    put(kb / "PAD/USER01/Kick.PDC", blob("kick"));
    put(kb / "SOUND/USER01.PCG", blob("sound"));
    fs::create_directories(out);
    const auto A = open(ka), B = open(kb);
    const auto ta = tree(ka), tb = tree(kb);
    const auto untouched = [&] { return tree(ka) == ta && tree(kb) == tb; };
    TEST("KST libraries open", A->ok && B->ok && A->format == library::Format::Pa5xKst && item(A, "Ballad") != size_t(-1));

    // ── KST: copy, move, rename, bank swap in one plan, into a NEW copy of B ──
    {
        const std::vector<Step> plan = {
            {Op::Copy, 'A', item(A, "Ballad"), "STYLE/USER03", -1, ""},     // into an existing bank of B
            {Op::Move, 'A', item(A, "Hit"), "", -1, ""},                    // same bank id: PAD/USER01; A keeps it
            {Op::Rename, 'B', item(B, "Waltz"), "", -1, "Old Waltz"},
            {Op::BankSwap, 'B', 0, "STYLE/USER01", -1, "style/user03"},    // typed in another case: resolved to B's spelling
            {Op::BankSwap, 'B', 0, "PAD/USER01", -1, "PAD/USER07"},        // into a bank that does not exist yet
        };
        const auto ev = library::evaluate(*A, *B, plan);
        const auto x = library::executable(*A, *B, plan, ev);
        TEST("KST plan: conflict-free, target B", !ev.blocked() && x.refusal.empty() && x.target == 'B');
        const fs::path dest = out / library::outputName(*B, "T1");
        TEST("output name keeps the .KST suffix", dest.filename() == "B edited T1.KST");
        const auto st = run(libraryApply(A, B, plan, dest));
        TEST("KST execution: done, published, nothing left over", st.phase == Phase::Done && fs::is_directory(dest) && !fs::exists(media::incompletePath(dest)));
        const auto t = tree(dest);
        TEST("copy: A's Ballad is in B's bank USER03, which the swap moved to USER01",
             t.count("STYLE/USER01/Ballad.STG") && t.at("STYLE/USER01/Ballad.STG") == blob("ballad") && t.at("STYLE/USER01/Tango.STG") == blob("tango"));
        TEST("rename + swap: Waltz renamed, then its bank USER01 swapped to USER03", t.count("STYLE/USER03/Old Waltz.STG") && !t.count("STYLE/USER03/Waltz.STG") &&
             !t.count("STYLE/USER01/Waltz.STG"));
        TEST("move = copy into the output, swap into a new bank folder", t.count("PAD/USER07/Hit.PDC") && t.count("PAD/USER07/Kick.PDC") && !t.count("PAD/USER01/Kick.PDC"));
        TEST("untouched files copied byte for byte", t.at("SOUND/USER01.PCG") == blob("sound"));
        TEST("manifest beside the output, verified", fs::is_regular_file(dest.string() + ".manifest.sha256") && media::verifyBackup(dest).empty());
        TEST("level: L1 passed (manifest, plan re-check, re-opened model, validator, sources), L2 n/a, never L3",
             st.level.l1.state == Check::Passed && st.level.l2.state == Check::NotRun && st.level.l3.state == Check::NotRun &&
             has(st.verifyText, "PASS  plan re-check") && has(st.verifyText, "PASS  re-opened with the Library model: Pa5X KST") &&
             has(st.verifyText, "PASS  structural validator") && has(st.verifyText, "PASS  source unchanged") && !has(st.verifyText, "FAIL"));
        const auto md = slurp(dest.string() + ".export.md"), js = slurp(dest.string() + ".export.json");
        TEST("report (.export.md/.json) lists every step and its result, labelled file level",
             has(md, "1. copy A:STYLE/USER01 \"Ballad\" -> B:STYLE/USER03: copied STYLE/USER01/Ballad.STG -> STYLE/USER03/Ballad.STG: done") &&
             has(md, "2. move") && has(md, "nothing is deleted") && has(md, "3. rename in B") && has(md, "renamed STYLE/USER01/Waltz.STG -> STYLE/USER01/Old Waltz.STG") &&
             has(md, "swapped STYLE/USER01 <-> STYLE/USER03 (bank folders)") && has(md, "5. bank-swap") &&
             has(md, "file-level operation; not verified on a keyboard") && has(js, "file-level operation; not verified on a keyboard") &&
             has(js, "\"writer\": \"Executed plan") && has(js, "3. rename in B"));
        TEST("no compatibility claim", !has(md, "ompatible") && !has(js, "ompatible"));
        TEST("both sources byte-identical", untouched());
        TEST("the output re-validates with no ERROR or WARN (KST table)", library::validate(dest).count(library::Severity::Error) == 0 &&
             library::validate(dest).count(library::Severity::Warn) == 0);
    }

    // ── refusals: nothing created, sources unchanged ──
    const auto refused = [&](const std::vector<Step>& plan, const std::string& why, const std::string& label, Lib a = nullptr, Lib b = nullptr) {
        const fs::path dest = out / ("refused " + label);
        const auto st = run(libraryApply(a ? a : A, b ? b : B, plan, dest));
        TEST(("refused: " + label).c_str(), st.phase == Phase::Failed && st.stoppedAt == Phase::Validate && has(st.reason, why) &&
             named(out, "refused " + label).empty());
    };
    refused({{Op::Copy, 'A', item(A, "Swing"), "SOUND", -1, ""}}, "conflict", "conflict (bank holds another type)");
    refused({{Op::Copy, 'A', item(A, "Swing"), "", -1, ""}, {Op::Rename, 'A', item(A, "Ballad"), "", -1, "X"}}, "changes both libraries", "both libraries changed");
    refused({}, "no steps", "empty plan");
    refused({{Op::Rename, 'B', item(B, "Waltz"), "", -1, "Wal:tz"}}, "FAT/exFAT", "rename to a name a USB stick refuses");

    // ── SET: whole bank files (same generation), item-level refused, cross-generation and cross-format refused ──
    const fs::path sa = root / "SetA", sb = root / "SetB", s4 = root / "MY.SET";
    const std::string aud = build({{0x17000030, 19, 0, 0, "Limiter", "l"}}, 1);
    put(sa / "GLOBAL/LIMITER.AUD", aud);
    put(sa / "STYLE/USER01.STY", build({{0x06000030, 2, 17, 0, "Pop", "p"}, {0x06000030, 2, 17, 1, "Rock", "r"}}, 1));
    put(sa / "STYLE/USER02.STY", build({{0x06000030, 2, 18, 0, "Jazz", "j"}}, 1));
    put(sb / "GLOBAL/LIMITER.AUD", aud);
    put(sb / "STYLE/USER01.STY", build({{0x06000030, 2, 17, 0, "Waltz", "w"}}, 1));
    put(sb / "STYLE/USER04.STY", build({{0x06000030, 2, 20, 0, "Samba", "s"}}, 1));
    put(sb / "KEYBOARDSET/USER01.PRF", build({{0x09020130, 1, 11, 0, "Piano", "k"}}, 1));
    put(s4 / "SOUND/USERDK.PCG", build({{0x10000030, 4, 9, 0, "Kit", "d"}}, 1));
    put(s4 / "STYLE/USER01.STY", build({{0x06000030, 2, 17, 0, "Bossa", "b"}}, 1));
    const auto SA = open(sa), SB = open(sb), S4 = open(s4);
    const auto tsa = tree(sa), tsb = tree(sb), ts4 = tree(s4);
    const auto setsUntouched = [&] { return tree(sa) == tsa && tree(sb) == tsb && tree(s4) == ts4; };
    TEST("SET libraries open (Pa700 x2, Pa4X)", SA->ok && SB->ok && S4->ok && SA->layout == media::kLayoutPa700 && S4->layout == media::kLayoutPa4x);
    {
        const std::vector<Step> plan = {
            {Op::Copy, 'A', item(SA, "Jazz"), "STYLE/USER03", -1, "", true},   // SetA's USER02.STY -> SetB's new USER03.STY
            {Op::BankSwap, 'B', 0, "STYLE/USER01", -1, "STYLE/USER04"},        // two bank files swapped
            {Op::Rename, 'B', item(SB, "Piano"), "", -1, "USER02", true},       // KEYBOARDSET/USER01.PRF -> USER02.PRF
        };
        const fs::path dest = out / library::outputName(*SB, "T2");
        const auto st = run(libraryApply(SA, SB, plan, dest));
        const auto t = tree(dest);
        TEST("SET execution: done, published", st.phase == Phase::Done && fs::is_directory(dest) && dest.filename() == "SetB edited T2");
        TEST("SET: whole bank file copied as a file", t.count("STYLE/USER03.STY") && t.at("STYLE/USER03.STY") == slurp(sa / "STYLE/USER02.STY"));
        TEST("SET: two bank files swapped", t.at("STYLE/USER01.STY") == slurp(sb / "STYLE/USER04.STY") && t.at("STYLE/USER04.STY") == slurp(sb / "STYLE/USER01.STY"));
        TEST("SET: bank file renamed", t.count("KEYBOARDSET/USER02.PRF") && !t.count("KEYBOARDSET/USER01.PRF"));
        const auto R = library::load(dest);
        TEST("SET output re-opens: the copied bank's items are read from the copied file", R.ok && R.format == library::Format::PaSet &&
             R.items.size() == 5 && has(st.verifyText, "PASS  plan re-check") && st.level.l1.state == Check::Passed);
        TEST("SET: sources byte-identical", setsUntouched());
    }
    refused({{Op::Copy, 'A', item(SA, "Rock"), "STYLE/USER04", -1, ""}}, "item-level edit inside a Pa-native container", "SET item copy", SA, SB);
    refused({{Op::Rename, 'B', item(SB, "Waltz"), "", -1, "Slow Waltz"}}, "4-byte directory record trailer", "SET item rename", SA, SB);
    refused({{Op::Copy, 'A', item(SA, "Jazz"), "STYLE/USER03", -1, "", true}}, "SET generations differ", "SET cross-generation", SA, S4);
    refused({{Op::Copy, 'A', item(A, "Swing"), "STYLE/USER03", -1, "", true}}, "formats differ", "KST to SET", A, SB);
    refused({{Op::Copy, 'A', item(SA, "Jazz"), "STYLE/USER04", -1, "", true}}, "conflict", "SET bank file exists", SA, SB);
    TEST("refusals: every source byte-identical", untouched() && setsUntouched());

    // ── interruption: cancel inside the write, and at verify: only NAME.INCOMPLETE stays ──
    const std::vector<Step> one = {{Op::Copy, 'A', item(A, "Swing"), "STYLE/USER03", -1, ""}};
    for (Phase p : {Phase::Run, Phase::Manifest, Phase::Verify}) {
        const fs::path dest = out / ("cancel " + std::to_string(int(p)) + ".KST");
        const auto st = run(libraryApply(A, B, one, dest), p);
        // at write the job is cancelled before its NAME.INCOMPLETE exists; later phases keep the staged copy there
        const auto left = p == Phase::Run ? std::vector<std::string>{} : std::vector<std::string>{dest.filename().string() + ".INCOMPLETE"};
        TEST(("cancelled at " + std::string(phaseName(p)) + ": nothing published, at most NAME.INCOMPLETE").c_str(),
             st.phase == Phase::Cancelled && !fs::exists(dest) && named(out, "cancel " + std::to_string(int(p))) == left);
    }
    {   // a placeholder file copied in is a NEW R-10 ERROR: not published
        put(ka / "STYLE/USER04/Fake.STG", "AIARRANGER-SYNTHETIC-NOT-A-KORG-FILE x");
        const auto A2 = open(ka);
        const auto st = run(libraryApply(A2, B, {{Op::Copy, 'A', item(A2, "Fake"), "STYLE/USER03", -1, ""}}, out / "fake.KST"));
        fs::remove_all(ka / "STYLE/USER04");
        TEST("copying placeholder bytes adds R-10: L1 fails, nothing published", st.phase == Phase::Failed && has(st.verifyText, "NEW: R-10") && !fs::exists(out / "fake.KST"));
        // an ERROR the target already had, moved by a swap, is not new
        const fs::path e = root / "E.KST";
        put(e / "STYLE/USER01/Zero.STG", "");
        put(e / "STYLE/USER02/One.STG", blob("one"));
        const auto E = open(e);
        const auto st2 = run(libraryApply(A, E, {{Op::BankSwap, 'B', 0, "STYLE/USER01", -1, "STYLE/USER02"}}, out / "moved-error.KST"));
        TEST("a target's own ERROR moved by a swap is not a new one: published", st2.phase == Phase::Done && has(st2.verifyText, "1 error(s) (target had 1)") &&
             fs::exists(out / "moved-error.KST/STYLE/USER02/Zero.STG"));
    }
    {   // cancel from inside the copy (the progress callback): the writer stops part-way
        ExportSpec s = libraryApply(A, B, one, out / "midcopy.KST");
        const auto w = s.write;
        int calls = 0;
        s.write = [&](const fs::path& o, const std::function<bool(double)>& progress) { return w(o, [&](double f) { return ++calls < 2 && progress(f); }); };
        const auto st = run(s);
        TEST("cancelled inside the copy: Failed/Cancelled, nothing published, only NAME.INCOMPLETE",
             (st.phase == Phase::Failed || st.phase == Phase::Cancelled) && has(st.reason, "cancelled") && !fs::exists(out / "midcopy.KST") &&
             named(out, "midcopy") == std::vector<std::string>{"midcopy.KST.INCOMPLETE"});
    }
    TEST("interruptions: sources byte-identical", untouched());

    // ── a source changed after it was opened: refused while copying, or before publishing ──
    {
        const fs::path c = root / "C.KST";
        put(c / "STYLE/USER01/One.STG", blob("one"));
        put(c / "STYLE/USER02/Two.STG", blob("two"));
        const auto C = open(c);
        put(c / "STYLE/USER01/One.STG", blob("ONE"));   // same size, other bytes
        const auto st = run(libraryApply(A, C, {{Op::Copy, 'A', item(A, "Swing"), "STYLE/USER02", -1, ""}}, out / "changed.KST"));
        TEST("target changed since opened: the copy's SHA-256 check fails the job, nothing published",
             st.phase == Phase::Failed && has(st.reason, "changed since its library was opened") && !fs::exists(out / "changed.KST") &&
             named(out, "changed") == std::vector<std::string>{"changed.KST.INCOMPLETE"});
        ExportSpec s = libraryApply(A, B, one, out / "late.KST");
        const auto v = s.verify;
        s.verify = [&](const fs::path& o) { put(ka / "PAD/USER01/Late.PDC", blob("late")); return v(o); };   // A changes after the write
        const auto st2 = run(s);
        fs::remove(ka / "PAD/USER01/Late.PDC");
        TEST("a source changed before publishing: snapshot compare fails L1, nothing published",
             st2.phase == Phase::Failed && st2.stoppedAt == Phase::Verify && has(st2.verifyText, "FAIL  source unchanged") && !fs::exists(out / "late.KST"));
    }

    // ── a step that adds a validator ERROR is not published ──
    {
        const auto st = run(libraryApply(A, B, {{Op::Copy, 'A', item(A, "Empty"), "STYLE/USER03", -1, ""}}, out / "empty.KST"));
        TEST("copying an empty file adds R-07: L1 fails, nothing published", st.phase == Phase::Failed && has(st.verifyText, "NEW: R-07") &&
             !fs::exists(out / "empty.KST"));
    }

    // ── output inside a source, unknown layout, a link in the target ──
    {
        const auto st = run(libraryApply(A, B, one, kb / "inside.KST"));
        TEST("output inside a source refused before anything is created (D18)", st.phase == Phase::Failed && !fs::exists(kb / "inside.KST") &&
             !fs::exists(kb / "inside.KST.INCOMPLETE"));
        put(root / "plain/notes/a.txt", "x");
        put(root / "plain2/notes/b.txt", "y");
        const auto P = open(root / "plain"), P2 = open(root / "plain2");
        refused({{Op::Copy, 'A', 0, "notes", -1, ""}}, "layout is unknown", "unknown layout", P, P2);
        const fs::path l = root / "L.KST";
        put(l / "STYLE/USER01/x.STG", blob("x"));
        fs::create_symlink(ka / "STYLE/USER01/Ballad.STG", l / "STYLE/USER01/link.STG");
        refused({{Op::Copy, 'A', item(A, "Swing"), "STYLE/USER01", -1, ""}}, "never followed", "link in target", A, open(l));
    }
    // ── Codex r1 P1: a destination path from the plan may not leave the staging root (absolute, ../) ──
    {
        const std::string absA = (ka / "STYLE/USER01").string();
        const std::vector<std::pair<std::string, std::vector<Step>>> bad = {
            {"absolute swap destination", {{Op::BankSwap, 'B', 0, "STYLE/USER01", -1, absA}}},
            {"../ swap destination", {{Op::BankSwap, 'B', 0, "STYLE/USER01", -1, "../../A.KST/STYLE/USER01"}}},
            {"absolute copy bank", {{Op::Copy, 'A', item(A, "Swing"), (root / "escape").string(), -1, ""}}},
            {"../ copy bank", {{Op::Copy, 'A', item(A, "Swing"), "STYLE/../../../escape", -1, ""}}},
        };
        int k = 0;
        for (const auto& [label, plan] : bad) {
            const auto ev = library::evaluate(*A, *B, plan);
            TEST(("plan: " + label + " is a conflict").c_str(), ev.blocked());
            const fs::path stage = out / ("direct" + std::to_string(k++));
            const auto r = library::applyPlan(*A, *B, plan, stage);   // execution itself, without the job's Validate phase
            TEST(("execute: " + label + " refused, both sources byte-identical, nothing outside staging").c_str(),
                 !r.error.empty() && untouched() && !fs::exists(root / "escape") && !fs::exists(out / "escape"));
            const auto st = run(libraryApply(A, B, plan, out / ("job " + label)));
            TEST(("job: " + label + " refused, nothing published").c_str(), st.phase == Phase::Failed && !fs::exists(out / ("job " + label)) && untouched());
        }
    }
    // ── Codex r1 P1: a staged folder swapped for a symlink to a source mid-job never redirects a write ──
    {
        const fs::path dest = out / "symlinked.KST";
        const fs::path staged = media::incompletePath(dest) / dest.filename();
        ExportSpec s = libraryApply(A, B, {{Op::Copy, 'A', item(A, "Swing"), "STYLE/USER03", -1, ""}}, dest);
        const auto w = s.write;
        bool swapped = false;
        s.write = [&](const fs::path& o, const std::function<bool(double)>& progress) {
            return w(o, [&](double f) {
                if (!swapped && fs::is_directory(staged / "STYLE/USER03")) {   // after the first copied file: STYLE/USER03 -> link to A's STYLE/USER01
                    swapped = true;
                    fs::rename(staged / "STYLE/USER03", staged / "STYLE/moved-aside");
                    fs::create_directory_symlink(ka / "STYLE/USER01", staged / "STYLE/USER03");
                }
                return progress(f);
            });
        };
        const auto st = run(s);
        TEST("staged folder swapped for a symlink: job fails, nothing published, both sources byte-identical",
             swapped && st.phase == Phase::Failed && !fs::exists(dest) && untouched());
    }
    // ── Codex r2 P1: bytes changed in the staged output after verify (the Publish update hook) are never published ──
    for (const std::string how : {"rewrite", "extra"}) {
        const fs::path dest = out / ("late-" + how + ".KST");
        const fs::path staged = media::incompletePath(dest) / dest.filename();
        JobState st;
        bool hit = false;
        runExport(libraryApply(A, B, one, dest), st, [] { return false; }, [&](const JobState& s) {
            if (s.phase != Phase::Publish || hit) return;
            hit = true;
            if (how == "rewrite") std::ofstream(staged / "STYLE/USER03/Tango.STG", std::ios::binary) << blob("TANGO");   // same size, other bytes
            else put(staged / "STYLE/USER03/Extra.STG", blob("extra"));
        });
        TEST(("staged output changed after verify (" + how + "): not published, job fails at publish").c_str(),
             hit && st.phase == Phase::Failed && st.stoppedAt == Phase::Publish && !fs::exists(dest) && has(st.reason, "after it was verified") && untouched());
    }
    // ── Codex r2 P2: a source folder swapped for a symlink to an outside folder is never read through ──
    {
        const fs::path outside = root / "outside";
        put(outside / "Swing.STG", blob("swing"));   // the same bytes: only the descriptor walk can tell
        fs::rename(ka / "STYLE/USER01", root / "USER01.aside");
        fs::create_directory_symlink(outside, ka / "STYLE/USER01");
        const auto st = run(libraryApply(A, B, one, out / "via-link.KST"));
        fs::remove(ka / "STYLE/USER01");   // the test's own link
        fs::rename(root / "USER01.aside", ka / "STYLE/USER01");
        TEST("source folder replaced by a symlink: refused while writing, before reading the outside file, nothing published",
             st.phase == Phase::Failed && st.stoppedAt == Phase::Run && has(st.reason, "links are never followed") && !fs::exists(out / "via-link.KST") && untouched());
    }
    // ── Codex r2 P2: names that differ only in case (one name on a FAT/exFAT stick) are never published ──
    put(root / "cs/a", "x");
    if (fs::exists(root / "cs/A")) std::printf("  note: the temp folder is case-insensitive: the case-collision job cannot be built here (set TMPDIR to a case-sensitive volume)\n");
    else {
        const fs::path f = root / "F.KST";
        put(f / "STYLE/USER01/Foo.STG", blob("foo"));
        put(f / "STYLE/USER01/foo.STG", blob("FOO"));
        const auto F = open(f);
        const auto st = run(libraryApply(A, F, {{Op::Copy, 'A', item(A, "Swing"), "STYLE/USER02", -1, ""}}, out / "case.KST"));
        TEST("case-sensitive disk: a target with Foo.STG + foo.STG is not published (R-18 fails L1)",
             st.phase == Phase::Failed && st.stoppedAt == Phase::Verify && has(st.verifyText, "R-18") && !fs::exists(out / "case.KST"));
    }
    TEST("all cases: sources byte-identical", untouched() && setsUntouched());

    // Optional, never in CI: AIARR_LIBRARY_OWNER_SET names a local COPY of an owner SET folder. It is only read; the
    // output (a swap of two Style bank files) goes to this test's temp folder and is removed with it. Counts only.
    if (const char* owner = std::getenv("AIARR_LIBRARY_OWNER_SET")) {
        const auto before = tree(owner);
        const auto O = open(owner);
        const auto st = run(libraryApply(O, O, {{Op::BankSwap, 'B', 0, "STYLE/USER01", -1, "STYLE/USER02"}}, out / "owner-swap"));
        std::printf("  owner SET: %zu files, job %s, L1 %s\n", O->files.size(), phaseName(st.phase), st.level.l1.state == Check::Passed ? "passed" : "not passed");
        TEST("owner SET copy: bank-file swap executed into a new folder, validator PASS, the copy unchanged",
             O->ok && st.phase == Phase::Done && has(st.verifyText, "PASS  structural validator") && tree(owner) == before);
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
