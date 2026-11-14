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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
// A real UASF for jobs that must publish: a UASF job's verify parses and validates what it wrote (D33 P2).
static std::vector<uint8_t> realUasf() {
    static const auto b = [] {
        ui::studio::StyleStudioModel m;
        std::string e;
        m.load(fs::path(REPO_DIR) / "fixtures/synthetic/SYNTH_WALTZ_68.sty", &e);
        return m.uasfBytes();
    }();
    return b;
}
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
        const auto b = q.enqueue(styleUasf("after", realUasf(), root / "after.uasf"));
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
        // D33 P2: a UASF job checks the staged file independently: it must parse as UASF and pass the UASF validator
        const auto junk = run(styleUasf("junk", {'n', 'o', 't', ' ', 'u', 'a', 's', 'f'}, root / "junk.uasf"));
        TEST("D33 P2 a UASF job whose bytes do not parse as UASF: failed at verify, nothing published", junk.phase == Phase::Failed &&
             junk.level.l1.state == Check::Failed && has(junk.reason, "UASF reader") && !fs::exists(root / "junk.uasf") &&
             fs::exists(root / "junk.uasf.INCOMPLETE"));
        auto inv = m.style();
        inv.sections[0].beats_per_bar = 0;
        const auto invBytes = uasf::UasfSerializer().serialize(inv);
        const auto iv = run(styleUasf("invalid", invBytes.data, root / "invalid.uasf"));
        TEST("D33 P2 a UASF job whose Style fails the UASF validator: failed at verify, nothing published", invBytes.success &&
             iv.phase == Phase::Failed && iv.level.l1.state == Check::Failed && has(iv.reason, "UASF validator") && !fs::exists(root / "invalid.uasf"));
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
        TEST("D33 P2 Style UASF job: the published file was read back, parsed and validated", has(u.verifyText, "parsed by the UASF reader"));
        const auto again = run(styleSmf("SYNTH_WALTZ_68", style_edit::exportPa5xSmf(m.style()), root / "waltz.mid"));
        TEST("an existing Style SMF is never overwritten", again.phase == Phase::Failed && has(again.reason, "already exists"));
        const auto conv = run(styleSmf("SYNTH_WALTZ_68", m.smfExport(), root / "conv.mid"));
        TEST("Style SMF job: the conversion report is the writer report, labelled not verified on a Pa5X",
             conv.phase == Phase::Done && has(conv.writerText, "Section map") && has(conv.writerText, "SFF2") &&
             has(conv.writerText, "not verified on a Pa5X") && has(conv.verifyText, "not verified on a Pa5X") &&
             has(slurp(root / "conv.mid.export.md"), "Dropped events"));
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
            auto s = styleUasf("q" + std::to_string(n), realUasf(), root / ("q" + std::to_string(n) + ".uasf"));
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

    // ── queue shutdown: an idle worker is joined; a running, cancelled job never makes destruction wait ──
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));   // workers from the blocks above have all exited
        const int before = JobQueue::workers();
        {
            JobQueue q;
            TEST("shutdown: a queue starts one worker", JobQueue::workers() == before + 1);
            q.enqueue(styleUasf("j1", realUasf(), root / "j1.uasf"));
            for (int i = 0; i < 500 && !q.idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        TEST("shutdown: destroying an idle queue joins its worker (gone when the destructor returns)", JobQueue::workers() == before &&
             fs::is_regular_file(root / "j1.uasf"));
        std::atomic<bool> release{false};
        auto blocked = styleUasf("j2", realUasf(), root / "j2.uasf");
        blocked.write = [&release](const fs::path&, const std::function<bool(double)>&) {   // ignores cancel until released
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return WriteResult{"stopped", ""};
        };
        const auto t0 = std::chrono::steady_clock::now();
        {
            JobQueue q;
            q.enqueue(blocked);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        TEST("shutdown: a running job that ignores cancel does not hang destruction (detached)", ms < 300 && JobQueue::workers() == before + 1);
        release = true;
        for (int i = 0; i < 200 && JobQueue::workers() != before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TEST("shutdown: the detached worker exits once its job stops, publishing nothing", JobQueue::workers() == before && !fs::exists(root / "j2.uasf"));
    }

    // ── leftovers: the read-only listing behind the studio's banner ──
    {
        const auto d = root / "left";
        fs::create_directories(d / "Pkg.INCOMPLETE");
        fs::create_directories(d / "Old.INCOMPLETE.stale-20260101T000000Z");
        fs::create_directories(d / "Done");
        put(d / "notes.INCOMPLETE.txt", "x");
        TEST("leftovers: NAME.INCOMPLETE and .stale- folders, sorted, nothing else",
             (leftovers(d) == std::vector<std::string>{"Old.INCOMPLETE.stale-20260101T000000Z", "Pkg.INCOMPLETE"}) && fs::is_directory(d / "Pkg.INCOMPLETE"));
        TEST("leftovers: a missing folder lists nothing", leftovers(root / "no-such").empty());
    }

    // ── the output record and the banner listing (#43 gate D e and note 2): every queue records, running work excluded ──
    {
        const auto rec = root / "state" / "last-output-folder.txt";
        setOutputRecord(rec);
        {
            JobQueue studio, window;   // two windows' own queues
            studio.enqueue(styleUasf("s", realUasf(), root / "rec-a" / "s.uasf"));
            window.enqueue(styleUasf("w", realUasf(), root / "rec-b" / "w.uasf"));
            studio.enqueue(styleUasf("s2", realUasf(), root / "rec-a" / "s2.uasf"));
            for (int i = 0; i < 500 && !(studio.idle() && window.idle()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        TEST("record: every queue records its output folder, most recent first, no duplicates",
             (recordedFolders() == std::vector<std::string>{(root / "rec-a").string(), (root / "rec-b").string()}));
        for (int i = 0; i < 12; ++i) recordOutputFolder((root / ("many" + std::to_string(i))).string());
        const auto many = recordedFolders();
        TEST("record: bounded to kRecordedFolders, newest kept first", many.size() == kRecordedFolders && many.front() == (root / "many11").string() &&
             many.back() == (root / "many4").string());

        fs::create_directories(root / "rec-a" / "old-a.INCOMPLETE");
        fs::create_directories(root / "rec-b" / "old-b.INCOMPLETE");
        recordOutputFolder((root / "rec-b").string());
        recordOutputFolder((root / "rec-a").string());
        auto report = leftoverReport();
        TEST("banner: every recorded folder with leftovers, most recent first; a later folder hides no earlier one",
             report.size() == 2 && report[0].first == (root / "rec-a").string() && report[0].second == std::vector<std::string>{"old-a.INCOMPLETE"} &&
             report[1].first == (root / "rec-b").string() && report[1].second == std::vector<std::string>{"old-b.INCOMPLETE"});

        std::atomic<bool> release{false};
        auto blocked = styleUasf("busy", realUasf(), root / "rec-a" / "busy.uasf");
        blocked.write = [&release](const fs::path& out, const std::function<bool(double)>&) {
            fs::create_directories(out.parent_path());
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return WriteResult{"stopped", ""};
        };
        {
            JobQueue q;
            q.enqueue(blocked);
            for (int i = 0; i < 500 && !fs::exists(root / "rec-a" / "busy.uasf.INCOMPLETE"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            report = leftoverReport();
            TEST("banner: a job running in this process is not a leftover (its own NAME.INCOMPLETE is left out)",
                 fs::is_directory(root / "rec-a" / "busy.uasf.INCOMPLETE") && !report.empty() &&
                 report[0].second == std::vector<std::string>{"old-a.INCOMPLETE"});
            release = true;
            for (int i = 0; i < 500 && !q.idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        report = leftoverReport();
        TEST("banner: once that job has failed, its NAME.INCOMPLETE is listed; nothing was deleted",
             !report.empty() && report[0].second == (std::vector<std::string>{"busy.uasf.INCOMPLETE", "old-a.INCOMPLETE"}));
        setOutputRecord({});
        recordOutputFolder((root / "ignored").string());
        TEST("record: no record file set (the default) records nothing", recordedFolders().empty());
    }

    // ── Codex r1 #2 (report export): enqueue does no file I/O on the caller (message) thread; the worker records the
    // output folder before the job runs, in enqueue order ──
    {
        const auto rec = root / "state-io" / "record.txt";
        setOutputRecord(rec);
        std::atomic<bool> release{false}, started{false};
        auto first = styleUasf("first", {'u'}, root / "io-a" / "first.uasf");
        first.write = [&](const fs::path&, const std::function<bool(double)>&) {
            started = true;
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return WriteResult{"stopped", ""};
        };
        {
            JobQueue q;
            q.enqueue(first);
            for (int i = 0; i < 500 && !started; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto before = slurp(rec);
            TEST("r1#2: the running job's folder was recorded before it wrote", started && has(before, (root / "io-a").string().c_str()));
            q.enqueue(styleUasf("second", {'u'}, root / "io-b" / "second.uasf"));
            TEST("r1#2: enqueue does not touch the output record on the caller thread", slurp(rec) == before);
            fs::create_directories(root / "io-b" / "old-b.INCOMPLETE");
            const auto report = leftoverReport();   // what a page opening runs, while io-b is only a note
            TEST("r2#2: leftover detection with a note pending writes nothing (the record file is unchanged)", slurp(rec) == before);
            TEST("r2#2: ... and still sees the noted folder's leftovers", !report.empty() && report[0].first == (root / "io-b").string() &&
                 report[0].second == std::vector<std::string>{"old-b.INCOMPLETE"});
            release = true;
            for (int i = 0; i < 500 && !q.idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        TEST("r1#2: the worker recorded the second folder, most recent first",
             (recordedFolders() == std::vector<std::string>{(root / "io-b").string(), (root / "io-a").string()}));
        setOutputRecord({});
    }
    {   // Codex r3 #1: enqueue (message thread) never waits for another thread's record I/O. The record is a FIFO, so a
        // reader blocks inside the open until the test opens the writing end: real slow I/O, no hook.
        const auto rec = root / "state-fifo" / "record.txt";
        fs::create_directories(rec.parent_path());
        setOutputRecord(root / "state-fifo" / "plain.txt");
        std::atomic<bool> release{false}, started{false};
        auto first = styleUasf("first", {'u'}, root / "fifo-a" / "first.uasf");
        first.write = [&](const fs::path&, const std::function<bool(double)>&) {
            started = true;
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return WriteResult{"stopped", ""};
        };
        JobQueue q;
        q.enqueue(first);   // the worker flushes into plain.txt, then blocks in this writer
        for (int i = 0; i < 500 && !started; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        TEST("r3#1: a FIFO record file made", ::mkfifo(rec.c_str(), 0600) == 0);
        setOutputRecord(rec);   // nothing pending: no I/O
        std::atomic<bool> reading{false};
        std::thread reader([&] { reading = true; (void)recordedFolders(); });   // blocks opening the FIFO
        while (!reading) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the reader is inside the open now
        std::thread opener([&] {   // ends the slow I/O after 1.5 s whatever happens
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            const int fd = ::open(rec.c_str(), O_WRONLY);
            if (fd >= 0) ::close(fd);
        });
        const auto t0 = std::chrono::steady_clock::now();
        q.enqueue(styleUasf("second", {'u'}, root / "fifo-b" / "second.uasf"));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  info: r3#1 enqueue took %lld ms while another thread read the record\n", (long long)ms);
        TEST("r3#1: enqueue returns within 200 ms while another thread holds the record in slow I/O", ms < 200);
        opener.join();
        reader.join();
        fs::remove(rec);   // a plain file again, so the worker's flush of fifo-b completes
        std::ofstream(rec) << "";
        release = true;
        for (int i = 0; i < 500 && !q.idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TEST("r3#1: the second folder is still recorded by the worker", recordedFolders() == std::vector<std::string>{(root / "fifo-b").string()});
        setOutputRecord({});
    }
    {   // Codex r1 #2: whether the output is a package is found by the job's worker, so the studio needs no file check
        const auto pkg = run(soundPackage(organ, root / "src", "", root / "pm-organ"));
        const auto one = run(styleUasf("pm", {'u'}, root / "pm.uasf"));
        TEST("r1#2: a published package is flagged by the job (PACKAGE.sha256)", pkg.phase == Phase::Done && pkg.packageManifest);
        TEST("r1#2: a single-file output is not", one.phase == Phase::Done && !one.packageManifest);
    }

    // ── Library Manager plan (E7) as a job: D18 covers both libraries, EXPORT-ONLY, L1 only ──
    {
        put(root / "LibA.KST/STYLE/USER01/Ballad.STG", "s");
        put(root / "LibB.KST/STYLE/USER02/Waltz.STG", "w");
        const auto A = std::make_shared<const library::Library>(library::load(root / "LibA.KST"));
        const auto B = std::make_shared<const library::Library>(library::load(root / "LibB.KST"));
        const std::vector<library::Step> plan = {{library::Op::Copy, 'A', 0, "", -1, ""}};
        TEST("plan job: both libraries loaded", A->ok && B->ok && A->items.size() == 1);
        const auto stamp = fs::last_write_time(root / "LibA.KST/STYLE/USER01/Ballad.STG");
        for (const auto& [lib, where] : {std::pair{"A", root / "LibA.KST/plans/p"}, std::pair{"B", root / "LibB.KST/STYLE/new/p"}}) {
            const auto st = run(libraryPlan(A, B, plan, where));
            TEST((std::string("plan export into a folder inside library ") + lib + " is refused").c_str(),
                 st.phase == Phase::Failed && st.stoppedAt == Phase::Destination && has(st.reason, "overlaps the job's source") &&
                 !fs::exists(where) && !fs::exists(media::incompletePath(where)) && !fs::exists(where.parent_path()));
        }
        const auto st = run(libraryPlan(A, B, plan, root / "My Plan"));
        TEST("plan job: published with plan.json + PLAN.txt, report beside it", st.phase == Phase::Done && fs::is_regular_file(root / "My Plan/plan.json") &&
             fs::is_regular_file(root / "My Plan/PACKAGE.sha256") && fs::is_regular_file(root / "My Plan.export.md") && !fs::exists(root / "My Plan.INCOMPLETE"));
        TEST("plan job: L1 passed, L2 not applicable, never L3", st.level.l1.state == Check::Passed && st.level.l2.state == Check::NotRun &&
             has(st.level.l2.detail, "plan document") && st.level.l3.state == Check::NotRun);
        TEST("plan job: the writer report says EXPORT-ONLY", has(st.writerText, "EXPORT-ONLY"));
        TEST("plan job: an existing plan folder is refused", run(libraryPlan(A, B, plan, root / "My Plan")).phase == Phase::Failed);
        TEST("plan job: an empty plan is refused", has(run(libraryPlan(A, B, {}, root / "Empty Plan")).reason, "no steps"));
        TEST("plan job: the libraries are unchanged", fs::last_write_time(root / "LibA.KST/STYLE/USER01/Ballad.STG") == stamp &&
             !fs::exists(root / "LibA.KST/plans"));
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
