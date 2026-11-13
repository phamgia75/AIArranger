// Export job model (jobs/export_job.h, export_specs.h, job_queue.h, roadmap E6): the phase order, cancel at every
// phase (nothing published, only NAME.INCOMPLETE), failure recovery with an injected writer failure, consent-only
// cleanup of the job's own NAME.INCOMPLETE, manifest and report content, honest levels per export, and the serial
// queue's order and cancellation. Synthetic data only (demo organ, demo drum kit, the repo's synthetic Style).

#include "jobs/export_specs.h"
#include "jobs/job_queue.h"
#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include <chrono>
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }

// Runs a spec synchronously; `cancelAt` cancels when the job enters that phase. Records every phase seen.
static JobState run(const ExportSpec& s, std::vector<Phase>* seen = nullptr, Phase cancelAt = Phase::Queued) {
    JobState st;
    st.id = 1;
    runExport(s, st, [&] { return cancelAt != Phase::Queued && st.phase == cancelAt; },
              [&](const JobState& now) { if (seen && (seen->empty() || seen->back() != now.phase)) seen->push_back(now.phase); });
    return st;
}
// Names in `dir` that start with `stem` (the output, its INCOMPLETE folder, its side files).
static std::vector<std::string> named(const fs::path& dir, const std::string& stem) {
    std::vector<std::string> r;
    for (const auto& e : fs::directory_iterator(dir)) if (e.path().filename().string().rfind(stem, 0) == 0) r.push_back(e.path().filename().string());
    std::sort(r.begin(), r.end());
    return r;
}

int main() {
    std::printf("Test: export jobs\n");
    const fs::path root = fs::temp_directory_path() / ("aia-export-jobs-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const auto organ = std::make_shared<const media::sound::Instrument>(media::sound::buildDemoOrgan());

    // ── state machine: every phase in order, published, manifest, report, level ──
    {
        std::vector<Phase> seen;
        const auto st = run(soundPackage(organ, root / "src", "", root / "organ"), &seen);
        const std::vector<Phase> want = {Phase::Validate, Phase::Dependencies, Phase::Destination, Phase::Preview, Phase::Estimate, Phase::Run,
                                         Phase::Manifest, Phase::Verify, Phase::Publish, Phase::Report, Phase::Done};
        TEST("sound package job: every phase in order, done", st.phase == Phase::Done && seen == want && st.progress == 1 && st.reason.empty());
        TEST("published under its name; no NAME.INCOMPLETE left", fs::is_regular_file(root / "organ/PACKAGE.sha256") && !fs::exists(root / "organ.INCOMPLETE"));
        TEST("dependencies via the resource graph: none missing, memory note", st.missing.empty() && !st.notes.empty() && has(st.notes[0], "sample memory") &&
             st.sampleBytes == media::sound::sampleBytes(*organ));
        TEST("preview lists the planned files with sizes", st.planned.size() == 4 && st.plannedBytes > st.sampleBytes);
        TEST("level: L1 + L2 from the shared verifier, never L3", st.level.l1.state == Check::Passed && st.level.l2.state == Check::Passed &&
             st.level.l3.state == Check::NotRun);
        bool c = false;
        std::string err;
        const auto manHash = media::detail::sha256File(root / "organ/PACKAGE.sha256", [](size_t) { return true; }, &c, &err);
        const auto it = std::find_if(st.files.begin(), st.files.end(), [](const OutFile& f) { return f.rel == "PACKAGE.sha256"; });
        TEST("manifest: every published file with size and SHA-256", st.files.size() == listFiles(root / "organ").size() && it != st.files.end() &&
             it->sha256 == manHash && std::all_of(st.files.begin(), st.files.end(), [](const OutFile& f) { return f.sha256.size() == 64; }));
        const auto md = slurp(root / "organ.export.md"), js = slurp(root / "organ.export.json");
        TEST("report beside the output: human-readable", st.reportPath == (root / "organ.export.md").string() && md == st.reportMd &&
             has(md, "# Export report: Sound package") && has(md, "Result: done") && has(md, (manHash + "  PACKAGE.sha256").c_str()) &&
             has(md, "Not verified: loading on a Pa5X"));
        TEST("report beside the output: machine JSON", js == st.reportJson && has(js, "\"schema\": \"aiarranger-export-report/1\"") &&
             has(js, "\"result\": \"done\"") && has(js, ("\"sha256\": \"" + manHash + "\"").c_str()) && has(js, "\"L3\": {\"state\": \"not-run\""));
        TEST("the report beside does not break the output's own check", ui::studio::verifyOutput(root / "organ").level->l1.state == Check::Passed);
        TEST("nothing called compatible", !has(md, "ompatible") && !has(js, "ompatible"));
    }

    // ── cancel at each phase before publication: nothing published, at most NAME.INCOMPLETE ──
    for (Phase p : {Phase::Validate, Phase::Dependencies, Phase::Destination, Phase::Preview, Phase::Estimate, Phase::Run, Phase::Manifest,
                    Phase::Verify, Phase::Publish}) {
        const std::string name = std::string("cancel-") + std::to_string(int(p));
        const auto st = run(soundPackage(organ, {}, "", root / name), nullptr, p);
        const auto left = named(root, name);
        const bool onlyWork = p > Phase::Run ? left == std::vector<std::string>{name + ".INCOMPLETE"} &&
                                                        fs::is_regular_file(root / (name + ".INCOMPLETE") / kJobMarker)
                                                  : left.empty();
        TEST((std::string("cancel at ") + phaseName(p) + ": cancelled, nothing published").c_str(),
             st.phase == Phase::Cancelled && st.stoppedAt == p && onlyWork && has(st.reason, "nothing was published") && st.reportPath.empty());
    }
    {   // after the publish rename a job cannot be cancelled any more: it finishes
        const auto st = run(soundPackage(organ, {}, "", root / "late"), nullptr, Phase::Report);
        TEST("cancel after publication is ignored: the job completes", st.phase == Phase::Done && fs::exists(root / "late/PACKAGE.sha256"));
    }

    // ── failure recovery: injected writer failure, refusal on re-run, consent-only cleanup ──
    {
        auto s = soundPackage(organ, {}, "", root / "faulty");
        const auto real = s.write;
        s.write = [real](const fs::path& out, const std::function<bool(double)>& p) {
            real(out, p);   // the real writer finishes in the job folder, then the disk "fails"
            return WriteResult{"I/O error: disk full (injected)", ""};
        };
        const auto st = run(s);
        TEST("injected writer failure: failed with the reason, nothing published", st.phase == Phase::Failed && st.stoppedAt == Phase::Run &&
             has(st.reason, "disk full (injected)") && (named(root, "faulty") == std::vector<std::string>{"faulty.INCOMPLETE"}) && has(st.reason, "faulty.INCOMPLETE"));
        TEST("a failed job keeps its report in memory only", has(st.reportJson, "\"result\": \"failed\"") && st.reportPath.empty());
        const auto again = run(soundPackage(organ, {}, "", root / "faulty"));
        TEST("re-run without consent refuses the leftover", again.phase == Phase::Failed && again.stoppedAt == Phase::Destination &&
             has(again.reason, "Re-run with consent") && fs::exists(root / "faulty.INCOMPLETE"));
        auto ok = soundPackage(organ, {}, "", root / "faulty");
        ok.moveLeftoverAside = true;
        const auto st2 = run(ok);
        const auto stale = named(root, "faulty.INCOMPLETE.stale-");
        TEST("D17 re-run with consent moves the leftover aside (never deletes) and publishes", st2.phase == Phase::Done &&
             fs::exists(root / "faulty/PACKAGE.sha256") && !fs::exists(root / "faulty.INCOMPLETE") && stale.size() == 1 &&
             fs::is_regular_file(root / stale[0] / "faulty/PACKAGE.sha256") && !st2.warnings.empty() && has(st2.warnings[0], stale[0].c_str()) &&
             has(st2.warnings[0], "delete it by hand"));
    }

    // ── D17 (Codex C1+C2): a job never deletes; the guard runs before any leftover is touched ──
    {
        // a leftover next to an output the guard refuses (it already exists): untouched, even with consent
        put(root / "guarded/PACKAGE.sha256", "existing output\n");
        put(root / "guarded.INCOMPLETE" / kJobMarker, "export job 9\n");
        put(root / "guarded.INCOMPLETE/user.txt", "user data");
        auto g = soundPackage(organ, {}, "", root / "guarded");
        g.moveLeftoverAside = true;
        const auto gs = run(g);
        TEST("C1 leftover on a guarded path: refused by the output guard first, leftover untouched", gs.phase == Phase::Failed &&
             has(gs.reason, "guarded already exists") && slurp(root / "guarded.INCOMPLETE/user.txt") == "user data" &&
             fs::is_regular_file(root / "guarded.INCOMPLETE" / kJobMarker) && named(root, "guarded.INCOMPLETE.stale").empty());
        // a symlink in the destination's path: refused, leftover untouched
        put(root / "realp/x.INCOMPLETE" / kJobMarker, "export job 9 Sound package -> " + (root / "linkp/x").string() + "\n");   // valid: the symlink refuses
        put(root / "realp/x.INCOMPLETE/user.txt", "user data");
        fs::create_directory_symlink(root / "realp", root / "linkp");
        auto l = soundPackage(organ, {}, "", root / "linkp/x");
        l.moveLeftoverAside = true;
        const auto ls = run(l);
        TEST("C1 symlinked parent: refused, the leftover is not moved or removed", ls.phase == Phase::Failed && has(ls.reason, "symlink") &&
             slurp(root / "realp/x.INCOMPLETE/user.txt") == "user data" && named(root / "realp", "x.INCOMPLETE.stale").empty() && !fs::exists(root / "realp/x"));
        // D18: a folder without the job marker, or with another job's marker, is never moved (the user deletes or renames it)
        put(root / "swap.INCOMPLETE" / kJobMarker, "forged\n");
        put(root / "swap.INCOMPLETE/Documents/thesis.txt", "precious");
        auto w = soundPackage(organ, {}, "", root / "swap");
        w.moveLeftoverAside = true;
        const auto ws = run(w);
        TEST("D18 a leftover whose marker is not this job's: refused, untouched", ws.phase == Phase::Failed && has(ws.reason, "marker") &&
             slurp(root / "swap.INCOMPLETE/Documents/thesis.txt") == "precious" && named(root, "swap.INCOMPLETE.stale-").empty());
        put(root / "nomark.INCOMPLETE/Documents/thesis.txt", "precious");
        auto nm = soundPackage(organ, {}, "", root / "nomark");
        nm.moveLeftoverAside = true;
        const auto nms = run(nm);
        TEST("D18 a leftover without a job marker: refused, untouched", nms.phase == Phase::Failed && has(nms.reason, "marker") &&
             slurp(root / "nomark.INCOMPLETE/Documents/thesis.txt") == "precious" && named(root, "nomark.INCOMPLETE.stale-").empty());
        // D18 C1: dest.INCOMPLETE IS the job's source: a KST library named archive.INCOMPLETE, backed up to archive
        put(root / "archive.INCOMPLETE/STYLE/BANK01.STY", std::string(100, 's'));
        put(root / "archive.INCOMPLETE" / kJobMarker, "export job 1 Verified KST backup -> " + (root / "archive").string() + "\n");
        auto kb = kstBackup(root / "archive.INCOMPLETE", root / "archive");
        kb.moveLeftoverAside = true;
        const auto kbs = run(kb);
        TEST("D18 C1 the source is the output's NAME.INCOMPLETE: refused before any rename, the source stays", kbs.phase == Phase::Failed &&
             has(kbs.reason, "source") && fs::is_regular_file(root / "archive.INCOMPLETE/STYLE/BANK01.STY") && named(root, "archive.INCOMPLETE.stale-").empty() &&
             !fs::exists(root / "archive"));
        // ... also when the names differ only by case (case-insensitive volume, the macOS default)
        auto kc = kstBackup(root / "archive.INCOMPLETE", root / "ARCHIVE");
        kc.moveLeftoverAside = true;
        const auto kcs = run(kc);
        TEST("D18 C1 the same check ignores case on a case-insensitive volume", kcs.phase == Phase::Failed && fs::exists(root / "archive.INCOMPLETE/STYLE/BANK01.STY") &&
             named(root, "archive.INCOMPLETE.stale-").empty() && named(root, "ARCHIVE.INCOMPLETE.stale-").empty());
        // generic for every job type: an output inside a source folder is refused
        put(root / "srcdir/a.txt", "x");
        auto inside = soundPackage(organ, root / "srcdir", "", root / "srcdir/pkg");
        const auto ins = run(inside);
        TEST("D18 every job: an output inside one of its sources is refused, nothing created there", ins.phase == Phase::Failed &&
             ins.stoppedAt == Phase::Destination && has(ins.reason, "source") && named(root / "srcdir", "pkg").empty());
        // D18 C2: a folder swapped in between the checks and the rename is renamed back, and the job fails
        put(root / "race.INCOMPLETE" / kJobMarker, "m");
        fs::path to;
        const auto re = detail::moveAside(root / "race.INCOMPLETE", &to, [&] {
            fs::rename(root / "race.INCOMPLETE", root / "race-original");   // the inspected folder goes away ...
            put(root / "race.INCOMPLETE/other.txt", "another folder");      // ... and another takes its name
        });
        TEST("D18 C2 identity mismatch after the move: renamed back, reported", !re.empty() && has(re, "renamed back") && !fs::exists(to) &&
             slurp(root / "race.INCOMPLETE/other.txt") == "another folder" && fs::exists(root / "race-original" / kJobMarker));
        // no deletion path at all in the job code
        bool removeAll = false;
        for (const auto& e : fs::directory_iterator(fs::path(REPO_DIR) / "src/jobs")) removeAll |= has(slurp(e.path()), "remove_all");
        TEST("D17 src/jobs contains no remove_all", !removeAll);
    }

    // ── D17 (Codex C3): an exception inside a job fails that job; the queue goes on ──
    {
        auto t = styleUasf("throws", {'u'}, root / "throws.uasf");
        t.validate = []() -> std::string { throw std::runtime_error("validator blew up (injected)"); };
        const auto ts = run(t);
        TEST("C3 a throwing job: failed with the message, nothing created", ts.phase == Phase::Failed && has(ts.reason, "validator blew up") &&
             named(root, "throws").empty());
        auto tw = styleUasf("throws2", {'u'}, root / "throws2.uasf");
        tw.write = [](const fs::path&, auto&) -> WriteResult { throw fs::filesystem_error("disk vanished (injected)", std::error_code(5, std::generic_category())); };
        JobQueue q;
        const auto a = q.enqueue(tw);
        const auto b = q.enqueue(styleUasf("after", {'u', 'a'}, root / "after.uasf"));
        for (int i = 0; i < 300 && !q.idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto snap = q.snapshot();
        TEST("C3 queue: a writer that throws fails its job; the next job still runs", snap.size() == 2 && snap[0].id == a && snap[0].phase == Phase::Failed &&
             has(snap[0].reason, "disk vanished") && snap[1].id == b && snap[1].phase == Phase::Done && fs::is_regular_file(root / "after.uasf") &&
             !fs::exists(root / "throws2.uasf"));
    }

    // ── D17 (Codex C4): verify the STAGED output before publishing; a failed check publishes nothing ──
    {
        ui::studio::StyleStudioModel m;
        std::string err;
        m.load(fs::path(REPO_DIR) / "fixtures/synthetic/SYNTH_WALTZ_68.sty", &err);
        auto c = styleSmf("SYNTH_WALTZ_68", style_edit::exportPa5xSmf(m.style()), root / "corrupt.mid");
        const auto real = c.write;
        c.write = [real](const fs::path& out, const std::function<bool(double)>& p) {
            auto r = real(out, p);
            std::fstream f(out, std::ios::in | std::ios::out | std::ios::binary);   // the disk returns other bytes than were written
            f.seekp(-1, std::ios::end);
            f.put('\x5a');
            return r;
        };
        const auto cs = run(c);
        TEST("C4 corrupt read-back of a Style SMF: failed, nothing published", cs.phase == Phase::Failed && !fs::exists(root / "corrupt.mid") &&
             fs::exists(root / "corrupt.mid.INCOMPLETE") && !fs::exists(root / "corrupt.mid.export.md") && has(cs.reason, "corrupt.mid.INCOMPLETE"));
        auto u = styleUasf("SYNTH_WALTZ_68", m.uasfBytes(), root / "corrupt.uasf");
        const auto ru = u.write;
        u.write = [ru](const fs::path& out, const std::function<bool(double)>& p) {
            auto r = ru(out, p);
            std::ofstream(out, std::ios::app) << "x";
            return r;
        };
        const auto us = run(u);
        TEST("C4 corrupt read-back of a UASF: failed, nothing published", us.phase == Phase::Failed && !fs::exists(root / "corrupt.uasf"));
        auto pk = soundPackage(organ, {}, "", root / "corrupt-pkg");
        const auto rp = pk.write;
        pk.write = [rp](const fs::path& out, const std::function<bool(double)>& p) {
            auto r = rp(out, p);
            std::ofstream(out / "RESOURCES.tsv", std::ios::app) << "changed after the writer's own check\n";
            return r;
        };
        const auto ps = run(pk);
        TEST("C4 staged package fails L1: failed at verify, nothing published", ps.phase == Phase::Failed && !fs::exists(root / "corrupt-pkg") &&
             fs::exists(root / "corrupt-pkg.INCOMPLETE") && ps.level.l1.state == Check::Failed);
    }

    // ── validation, dependencies and destination refusals ──
    {
        auto bad = std::make_shared<media::sound::Instrument>(*organ);
        bad->name = "";
        const auto st = run(soundPackage(bad, {}, "", root / "bad"));
        TEST("invalid source: failed at validate, nothing created", st.phase == Phase::Failed && st.stoppedAt == Phase::Validate && named(root, "bad").empty());
        auto miss = soundPackage(organ, {}, "", root / "miss");
        miss.dependencies = [](auto&, auto&) { return std::vector<std::string>{"missing: Sound 'Lead' (factory reference)"}; };
        const auto m = run(miss);
        TEST("missing resource: blocking, reported, nothing created", m.phase == Phase::Failed && m.stoppedAt == Phase::Dependencies &&
             m.missing.size() == 1 && has(m.reason, "1 missing resource") && named(root, "miss").empty());
        const auto ex = run(soundPackage(organ, {}, "", root / "organ"));
        TEST("existing output refused by the output guard", ex.phase == Phase::Failed && ex.stoppedAt == Phase::Destination && has(ex.reason, "already exists"));
        const auto vol = run(soundPackage(organ, {}, "", "/Volumes/AIA-JOBS-" + std::to_string(::getpid()) + "/x"));
        TEST("mounted volume refused by the output guard", vol.phase == Phase::Failed && has(vol.reason, "mounted volume"));
        auto huge = soundPackage(organ, {}, "", root / "huge");
        huge.dependencies = [](auto&, uint64_t& b) { b = kBlockingSampleBytes + 1; return std::vector<std::string>{}; };
        const auto h = run(huge);
        TEST("estimate: sample memory above every Pa5X capacity blocks", h.phase == Phase::Failed && h.stoppedAt == Phase::Estimate && named(root, "huge").empty());
    }

    // ── every export kind through the job model, with its honest level ──
    {
        const auto kit = std::make_shared<const media::sound::DrumKit>(media::sound::buildDemoKit());
        const auto d = run(drumKit(kit, "synthetic", root / "drums"));
        TEST("drum kit job: L1 + L2", d.phase == Phase::Done && d.level.l1.state == Check::Passed && d.level.l2.state == Check::Passed);
        const auto k = run(kmpFolder(organ, root / "organ-kmp"));
        TEST("KMP/KSF job: L1 only (EXPERIMENTAL)", k.phase == Phase::Done && k.level.l1.state == Check::Passed && k.level.l2.state == Check::NotRun &&
             has(k.writerText, "EXPERIMENTAL"));

        // Sample Lab: a short WAV folder
        media::sound::Sample s;
        s.rate = 44100;
        s.pcm.assign(44100, 0);
        for (size_t i = 0; i < 4; ++i) for (size_t j = 0; j < 4000; ++j) s.pcm[i * 11025 + j] = int16_t(9000 * std::sin(double(j) / 7));
        const auto wav = media::sound::writeWav(s);
        put(root / "labin/a.wav", std::string(wav.begin(), wav.end()));
        put(root / "labin/b.wav", std::string(wav.begin(), wav.end()));
        const auto b = run(labBatch(root / "labin", true, false, root / "batch"));
        TEST("Sample Lab batch job: L1 only, writer report kept", b.phase == Phase::Done && b.level.l1.state == Check::Passed && b.level.l2.state == Check::NotRun &&
             !has(b.writerText, "Written: "));
        const auto inside = run(labBatch(root / "labin", true, false, root / "labin/out"));
        TEST("batch into its own input: refused before anything is created in the input", inside.phase == Phase::Failed &&
             inside.stoppedAt == Phase::Validate && inside.reason.rfind("Batch refused", 0) == 0 && named(root / "labin", "out").empty());
        const auto sl = run(labSlice(root / "labin/a.wav", 1, 4, root / "slices"));
        TEST("Time Slice job: L1 only", sl.phase == Phase::Done && sl.level.l1.state == Check::Passed);
        const auto lp = run(labLoop(root / "labin/a.wav", 1000, 20000, 0, 0, root / "loop"));
        TEST("Loop job: L1 only", lp.phase == Phase::Done && lp.level.l1.state == Check::Passed);

        // Style SMF and UASF from the repo's synthetic Style
        ui::studio::StyleStudioModel m;
        std::string err;
        const bool open = m.load(fs::path(REPO_DIR) / "fixtures/synthetic/SYNTH_WALTZ_68.sty", &err);
        const auto smf = run(styleSmf("SYNTH_WALTZ_68", style_edit::exportPa5xSmf(m.style()), root / "waltz.mid"));
        TEST("Style SMF job: a single new file at L1, report beside", open && smf.phase == Phase::Done && fs::is_regular_file(root / "waltz.mid") &&
             smf.level.l1.state == Check::Passed && smf.level.l2.state == Check::NotRun && smf.files.size() == 1 &&
             fs::is_regular_file(root / "waltz.mid.export.json") && !fs::exists(root / "waltz.mid.INCOMPLETE"));
        const auto u = run(styleUasf("SYNTH_WALTZ_68", m.uasfBytes(), root / "waltz.uasf"));
        TEST("Style UASF job: written, no Pa5X level claimed", u.phase == Phase::Done && slurp(root / "waltz.uasf").size() == m.uasfBytes().size() &&
             u.level.l1.state == Check::NotRun);
        const auto again = run(styleSmf("SYNTH_WALTZ_68", style_edit::exportPa5xSmf(m.style()), root / "waltz.mid"));
        TEST("an existing Style SMF is never overwritten", again.phase == Phase::Failed && has(again.reason, "already exists"));
    }

    // ── KST backup: the manifest beside is published with the folder; cancel inside the copy ──
    {
        put(root / "Mine.KST/STYLE/BANK01.STY", std::string(4000, 'x'));
        put(root / "Mine.KST/SOUND/BANK01.PCG", std::string(3000, 'y'));
        const auto k = run(kstBackup(root / "Mine.KST", root / "backup"));
        TEST("KST backup job: folder and manifest published, L1 from the manifest, no L2", k.phase == Phase::Done &&
             fs::is_regular_file(root / "backup/STYLE/BANK01.STY") && fs::is_regular_file(root / "backup.manifest.sha256") &&
             k.level.l1.state == Check::Passed && k.level.l2.state == Check::NotRun && k.planned.size() == 2);
        const auto in = run(kstBackup(root / "Mine.KST", root / "Mine.KST/STYLE/copy"));
        TEST("KST backup into its own source: refused before anything is created in the source", in.phase == Phase::Failed &&
             in.stoppedAt == Phase::Validate && named(root / "Mine.KST/STYLE", "copy").empty());
        auto s = kstBackup(root / "Mine.KST", root / "backup2");
        JobState st;
        bool inWrite = false;
        runExport(s, st, [&] { return inWrite; }, [&](const JobState& now) { if (now.phase == Phase::Run) inWrite = true; });
        TEST("KST backup cancelled inside the copy: nothing published, no manifest beside", st.phase == Phase::Cancelled &&
             !fs::exists(root / "backup2") && !fs::exists(root / "backup2.manifest.sha256") && fs::exists(root / "backup2.INCOMPLETE"));
    }

    // ── serial queue: order, cancel of a queued job, non-blocking destruction ──
    {
        std::mutex m;
        std::vector<int> order;
        auto spec = [&](int n, int ms) {
            auto s = styleUasf("q" + std::to_string(n), {'u', 'a', 's', 'f'}, root / ("q" + std::to_string(n) + ".uasf"));
            const auto w = s.write;
            s.write = [&, n, ms, w](const fs::path& out, const std::function<bool(double)>& p) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                { std::lock_guard<std::mutex> l(m); order.push_back(n); }
                return w(out, p);
            };
            return s;
        };
        std::atomic<int> changes{0};
        JobQueue q([&] { ++changes; });
        const auto a = q.enqueue(spec(1, 80)), b = q.enqueue(spec(2, 10)), c = q.enqueue(spec(3, 10));
        q.cancel(c);
        for (int i = 0; i < 500 && !q.idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto snap = q.snapshot();
        TEST("queue: jobs run one at a time in the order added", (order == std::vector<int>{1, 2}) && a < b && b < c);
        TEST("queue: a cancelled queued job never runs and writes nothing", snap.size() == 3 && snap[2].phase == Phase::Cancelled &&
             !fs::exists(root / "q3.uasf") && !fs::exists(root / "q3.uasf.INCOMPLETE"));
        TEST("queue: states done, change notifications sent", snap[0].phase == Phase::Done && snap[1].phase == Phase::Done && changes > 3);
        const auto t0 = std::chrono::steady_clock::now();
        {
            JobQueue q2;
            q2.enqueue(spec(4, 400));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        TEST("queue: destroying it while a job runs returns at once (the job is cancelled)", ms < 300);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        TEST("queue: the job cancelled by destruction published nothing", !fs::exists(root / "q4.uasf"));
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
