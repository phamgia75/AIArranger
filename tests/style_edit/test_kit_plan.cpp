// BL-E1 S1-S3: the read-only Import Kit review (kit::plan), the plan fingerprint the job refuses a changed source with,
// and the destination check (kit::checkDestination). Synthetic data only. The review never writes: every test that calls
// plan() or checkDestination() also proves the outputs folder and the destination parent are byte-identical afterwards.

#include "jobs/export_specs.h"
#include "jobs/import_kit.h"
#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include "media/sound/korg_kmp_writer.h"
#include "media/sound/package_writer.h"
#include <cstdio>
#include <numeric>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;
namespace k = ai_arranger::jobs::kit;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
static bool anyHas(const std::vector<std::string>& v, const std::string& what) {
    for (const auto& s : v) if (has(s, what)) return true;
    return false;
}
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static JobState run(const ExportSpec& s) {
    JobState st;
    st.id = 1;
    runExport(s, st, [] { return false; }, [](const JobState&) {});
    return st;
}
// path -> SHA-256 of every file below root, plus every directory (a plan must not even create an empty folder)
static std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> r;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
        const auto rel = fs::relative(it->path(), root).string();
        if (it->is_symlink(ec)) { r[rel] = "link"; continue; }
        if (it->is_directory(ec)) { r[rel] = "dir"; continue; }
        bool c = false;
        std::string e;
        r[rel] = media::detail::sha256File(it->path(), [](size_t) { return true; }, &c, &e);
    }
    return r;
}

int main() {
    std::printf("Test: Import Kit review plan, fingerprint, destination check (BL-E1 S1-S3)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-kit-plan-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path out = root / "outputs", parent = root / "dest";
    fs::create_directories(out);
    fs::create_directories(parent);

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
    const auto outBefore = snapshot(out), parentBefore = snapshot(parent);

    // ── S1: the review plan ──
    const auto p = k::plan(out);
    TEST("S1 plan: four members, each STRUCTURALLY VALID, nothing blocking",
         p.items.size() == 4 && p.blocking.empty() && std::all_of(p.items.begin(), p.items.end(), [](const k::Item& i) { return i.status == k::kValid; }));
    {
        std::set<std::string> got, want{"SOUNDS/AIA Demo Organ", "DRUMKITS/AIA Demo Kit", "KMP/AIA Organ KMP", "STYLES/Waltz.mid"};
        for (const auto& i : p.items) got.insert(i.kitPath());
        TEST("S1 plan: kit paths per import route", got == want);
    }
    TEST("S1 plan: a fingerprint (64 hex chars)", p.fingerprint.size() == 64);
    TEST("S1 plan: bytes = the sum of the planned files", p.bytes > 0 && p.bytes == std::accumulate(p.files.begin(), p.files.end(), uint64_t(0), [](uint64_t a, const k::Planned& f) { return a + f.bytes; }));
    TEST("S1 plan: writes nothing (outputs byte-identical, no new entry)", snapshot(out) == outBefore);

    // the job over the same folder: same members, same statuses, same bytes (the review is advisory, the job is the authority)
    {
        const auto st = run(k::importKit({}, parent / "Job Kit", out));
        TEST("S1 job over the same folder is done", st.phase == Phase::Done);
        size_t valid = 0;
        for (const auto& n : st.notes) valid += n.rfind("STRUCTURALLY VALID: ", 0) == 0;
        TEST("S1 plan == job: member count", valid == p.items.size());
        uint64_t kitBytes = 0;
        for (const auto& it : p.items) {
            const auto at = parent / "Job Kit" / it.kitPath();
            if (it.kind == k::Kind::Style) kitBytes += fs::file_size(at);
            else for (const auto& f : listFiles(at)) kitBytes += f.bytes;
        }
        TEST("S1 plan == job: bytes", kitBytes == p.bytes);
        fs::remove_all(parent / "Job Kit");
        for (const auto& e : fs::directory_iterator(parent)) fs::remove_all(e.path());
    }

    // fingerprint behaviour
    {
        const auto again = k::plan(out);
        TEST("S1 fingerprint is stable for an unchanged folder", again.fingerprint == p.fingerprint);
        const auto w = out / "Waltz.mid";
        auto bytes = slurp(w);
        put(w, bytes + "x");
        const auto grown = k::plan(out);
        TEST("S1 fingerprint changes when a file grows", grown.fingerprint != p.fingerprint);
        put(w, bytes);
        fs::last_write_time(w, fs::file_time_type::clock::now() + std::chrono::hours(1));
        TEST("S1 fingerprint changes with the modification time alone", k::plan(out).fingerprint != p.fingerprint);
        fs::last_write_time(w, fs::file_time_type::clock::now());
        put(out / "Another.mid", bytes);
        TEST("S1 fingerprint changes when a member is added", k::plan(out).fingerprint != p.fingerprint);
        fs::remove(out / "Another.mid");
    }
    const auto plan2 = k::plan(out);   // after the churn above: the baseline for the tamper tests below

    // blocking lines, one per class, named
    {
        const fs::path o2 = root / "o2";
        fs::create_directories(o2);
        fs::copy(out / "AIA Demo Organ", o2 / "AIA Demo Organ", fs::copy_options::recursive);
        put(o2 / "notes.txt", "not an output");
        const auto q = k::plan(o2);
        TEST("S1 blocking: an entry the kit does not take is named and blocks", !q.blocking.empty() && anyHas(q.blocking, "notes.txt"));
        fs::remove(o2 / "notes.txt");
        fs::create_symlink("/etc/hosts", o2 / "hosts-link");
        const auto l = k::plan(o2);
        TEST("S1 blocking: a link in the outputs folder is not followed and blocks, naming it", !l.blocking.empty() && anyHas(l.blocking, "hosts-link"));
        fs::remove(o2 / "hosts-link");
        fs::create_symlink("/etc/hosts", o2 / "AIA Demo Organ" / "hosts");
        const auto li = k::plan(o2);
        TEST("S1 blocking: a link inside a member blocks", !li.blocking.empty() && anyHas(li.blocking, "link"));
        fs::remove(o2 / "AIA Demo Organ" / "hosts");
        // tamper: a member flipped after export fails its reader on the SOURCE
        std::string sf2;
        for (const auto& e : fs::directory_iterator(o2 / "AIA Demo Organ")) if (e.path().extension() == ".sf2") sf2 = e.path().string();
        auto b = slurp(sf2);
        b[b.size() / 2] = char(b[b.size() / 2] ^ 0x5a);
        put(sf2, b);
        const auto t = k::plan(o2);
        TEST("S1 tamper: a corrupted member is FAILED and blocks", !t.blocking.empty() && t.items.size() == 1 && t.items[0].status == "FAILED");
        const auto none = k::plan(root / "nothing-here");
        TEST("S1 blocking: a missing or empty folder says no outputs", !none.blocking.empty() && none.items.empty());
        const auto cap = k::plan(out, {}, 2);
        TEST("S1 listing cap: refused with the count, nothing planned", !cap.blocking.empty() && anyHas(cap.blocking, "more") && cap.items.empty());
        int calls = 0;
        const auto cancelled = k::plan(out, [&] { return ++calls < 2; });
        TEST("S1 cancellable: stops between members and blocks, never a partial plan", !cancelled.blocking.empty() && anyHas(cancelled.blocking, "cancel"));
    }

    // ── S2: the job refuses a changed source ──
    {
        const auto good = run(k::importKit({}, parent / "Kit A", out, plan2.fingerprint));
        TEST("S2 the job runs when the fingerprint still matches", good.phase == Phase::Done);
        fs::remove_all(parent / "Kit A");
        for (const auto& e : fs::directory_iterator(parent)) fs::remove_all(e.path());
        const auto w = out / "Waltz.mid";
        const auto orig = slurp(w);
        put(w, orig + "tampered");
        const auto st = run(k::importKit({}, parent / "Kit B", out, plan2.fingerprint));
        TEST("S2 a source changed after the review: refused at Validate", st.phase == Phase::Failed && st.stoppedAt == Phase::Validate && has(st.reason, "changed since"));
        TEST("S2 the refused job published nothing and left no work folder", !fs::exists(parent / "Kit B") && !fs::exists(parent / "Kit B.INCOMPLETE"));
        put(w, orig);
        fs::last_write_time(w, fs::file_time_type::clock::now() + std::chrono::hours(2));
        const auto st2 = run(k::importKit({}, parent / "Kit C", out, plan2.fingerprint));
        TEST("S2 a changed modification time alone is refused too", st2.phase == Phase::Failed && has(st2.reason, "changed since"));
        const auto none = run(k::importKit({}, parent / "Kit D", out));
        TEST("S2 no fingerprint given: the job behaves as before", none.phase == Phase::Done);
        fs::remove_all(parent / "Kit D");
        for (const auto& e : fs::directory_iterator(parent)) fs::remove_all(e.path());
    }

    // ── S3: the destination check ──
    {
        const auto before = snapshot(parent);
        const uint64_t need = plan2.bytes;
        const auto okd = k::checkDestination(parent, "My Kit", need, {out});
        TEST("S3 a new name under an existing local folder is accepted", okd.ok && okd.refusals.empty());
        TEST("S3 reports free space and what the kit needs (bytes + 64 KiB + 10%)", okd.freeBytes > 0 && okd.neededBytes == (need + 65536) + (need + 65536) / 10);
        fs::create_directories(parent / "Existing");
        const auto ex = k::checkDestination(parent, "Existing", need, {out});
        TEST("S3 an existing destination is refused, never overwritten", !ex.ok && anyHas(ex.refusals, "exists"));
        fs::create_directories(parent / "Half.INCOMPLETE");
        const auto lo = k::checkDestination(parent, "Half", need, {out});
        TEST("S3 a leftover NAME.INCOMPLETE is refused and flagged (consent path only)", !lo.ok && lo.leftover && anyHas(lo.refusals, "INCOMPLETE"));
        TEST("S3 an unsafe kit name is refused naming FAT", !k::checkDestination(parent, "bad:name", need, {out}).ok && anyHas(k::checkDestination(parent, "bad:name", need, {out}).refusals, "FAT"));
        TEST("S3 a missing parent is refused", !k::checkDestination(root / "no-such-parent", "K", need, {out}).ok);
        TEST("S3 a destination inside a source is refused (D18)", !k::checkDestination(out / "AIA Demo Organ", "K", need, {out / "AIA Demo Organ"}).ok);
        TEST("S3 a parent that is itself a source is refused (D18)", !k::checkDestination(out, "K", need, {out}).ok);
        const auto vol = k::checkDestination("/Volumes/AIARR-NO-SUCH", "K", need, {out});
        TEST("S3 a /Volumes destination is refused, before any job", !vol.ok);
        const auto big = k::checkDestination(parent, "Big", uint64_t(1) << 62, {out});
        TEST("S3 not enough free space is refused with both numbers", !big.ok && anyHas(big.refusals, "free space"));
        fs::create_symlink(parent, root / "parent-link");
        TEST("S3 a parent reached through a symlink is refused", !k::checkDestination(root / "parent-link", "K", need, {out}).ok);
        fs::remove_all(parent / "Existing");
        fs::remove_all(parent / "Half.INCOMPLETE");
        TEST("S3 the check wrote nothing", snapshot(parent) == before);
    }
    TEST("sources unchanged at the end (apart from the mtime test above)", [&] {
        auto now = snapshot(out);
        return now == outBefore;
    }());

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
