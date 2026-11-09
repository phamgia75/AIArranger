// Resources window model (ui/resources/resource_browser_model.h), the owner-authorized evidence
// importer (media/sound/evidence_import.h) and the offline Memory Info comparison
// (media/sound/memory_check.h). Original synthetic fixtures only; SOFTWARE evidence.

#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include "media/sound/package_writer.h"
#include "ui/resources/resource_browser_model.h"
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger::media::sound;
using namespace ai_arranger::media::graph;
using ai_arranger::ui::ResourceBrowserModel;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool contains(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }
static ResourceId P(Kind k, uint32_t n) { return {false, "USR", k, n}; }
static void spit(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

// One layer, two zones sharing ONE sample.
static Instrument sharedZoneInstrument() {
    Instrument in;
    in.name = "Shared Tone";
    Sample s; s.name = "Tone"; s.root = 60; s.rate = 48000;
    for (int i = 0; i < 4800; ++i) s.pcm.push_back(int16_t((i % 96) * 300 - 14400));
    in.samples.push_back(s);
    Layer L; L.name = in.name; L.velLo = 0; L.velHi = 127;
    Zone a; a.lo = 0; a.hi = 60; a.sample = 0; a.root = 60;
    Zone b; b.lo = 61; b.hi = 127; b.sample = 0; b.root = 60;
    L.zones = {a, b};
    in.layers.push_back(L);
    return in;
}

static std::string evidence(const std::string& body, const std::string& kind = "pa-manager-report") {
    std::string t = "aiarranger-evidence\t1\nsource\tReport A\towner approval 2026-11-09\t" + kind + "\n" + body;
    size_t lines = 0;
    for (char c : t) lines += c == '\n';
    return t + "end\t" + std::to_string(lines) + "\n";
}
static bool imports(const std::string& t, Graph& g, std::string* e = nullptr) { std::string l, x; return importEvidence(t, g, &l, e ? e : &x); }

static MemoryInfoReading reading(const std::string& body) { MemoryInfoReading r; std::string e; parseMemoryInfo("aiarranger-memory-info\t1\n" + body, r, &e); return r; }

int main() {
    std::printf("Test: resources window model, evidence import, memory check\n");
    std::string err;
    const fs::path root = fs::temp_directory_path() / ("aia-rbrowse-" + std::to_string(::getpid()));
    fs::remove_all(root);
    TEST("packages written", !writePackage(buildDemoOrgan(), root / "organ", "", &err).empty() &&
         !writeDrumPackage(buildDemoKit(), root / "drums", "", &err).empty() && !writePackage(sharedZoneInstrument(), root / "shared", "", &err).empty());

    // ── Resource Graph tab ──
    ResourceBrowserModel m;
    TEST("open three packages", m.addPackage(root / "organ", &err) && m.addPackage(root / "drums", &err) && m.addPackage(root / "shared", &err));
    TEST("the same package twice is refused, not counted twice", !m.addPackage(root / "organ", &err) && contains(err, "already open") && m.packages().size() == 3);
    TEST("a folder that is not a package is refused", !m.addPackage(root, &err));
    const auto roots = m.roots();
    size_t sounds = 0, kits = 0;
    for (const auto& r : roots) { sounds += r.kind == Kind::Sound; kits += r.kind == Kind::DrumKit; }
    TEST("browse: 2 Sounds and 1 Drum Kit at the top", roots.size() == 3 && sounds == 2 && kits == 1);
    ResourceId organ{}, kit{}, shared{};
    for (const auto& r : roots) {
        const std::string n = m.graph().find(r)->name;
        (r.kind == Kind::DrumKit ? kit : n == "Shared Tone" ? shared : organ) = r;
    }
    TEST("expand: Sound -> Oscillators -> Multisample -> Samples", [&] {
        const auto osc = m.children(organ);
        if (osc.size() != 2 || osc[0].to.kind != Kind::Oscillator) return false;
        const auto ms = m.children(osc[0].to);
        return ms.size() == 1 && ms[0].to.kind == Kind::Multisample && !m.children(ms[0].to).empty() &&
               m.children(ms[0].to)[0].to.kind == Kind::Sample;
    }());
    TEST("expand: Drum Kit -> Drum Samples -> Sample", [&] {
        const auto ds = m.children(kit);
        return ds.size() == 9 && ds[0].to.kind == Kind::DrumSample && m.children(ds[0].to).size() == 1;
    }());
    bool noSlot = true;
    for (const auto& [id, n] : m.graph().nodes())
        noSlot &= !contains(m.displayId(id), "Pa5X USR") && contains(m.displayId(id), "provisional: not a Pa5X slot") &&
                  !contains(m.rowLabel(id), "[Pa5X");
    TEST("provisional identities are never shown as Pa5X bank/slot", noSlot);
    const auto ms = m.children(m.children(shared)[0].to)[0].to;
    const auto zones = m.children(ms);
    TEST("shared sample: two zones, one sample, row says shared by 2",
         zones.size() == 2 && zones[0].to == zones[1].to && contains(m.rowLabel(zones[0].to, &zones[0]), "[shared by 2]") &&
         contains(m.rowLabel(zones[0].to, &zones[0]), "9 600 bytes"));
    const std::string sd = m.details(zones[0].to);
    TEST("sample details: exact uncompressed bytes, where used x2, reached from the Sound, delete verdict",
         contains(sd, "uncompressed PCM: 9 600 bytes (exact)") && contains(sd, "Where used: 2 direct reference(s) (shared resource)") &&
         contains(sd, "Reached from:") && contains(sd, "Shared Tone") && contains(sd, "Delete safety: IN USE"));
    const std::string od = m.details(organ);
    TEST("Sound details: total unique sample memory, delete verdict, scope note",
         contains(od, "Samples needed: 38 unique") && contains(od, "Delete safety: No known references") &&
         contains(od, "not visible"));
    TEST("oscillator: IN USE by its Sound", contains(m.details(m.children(organ)[0].to), "Delete safety: IN USE"));
    TEST("budget text: packages + exact PCM + both 4 GB readings", contains(m.budgetText(), "Open: 3 package(s)") &&
         contains(m.budgetText(), "uncompressed PCM") && contains(m.budgetText(), "4 294 967 296"));
    TEST("no unresolved dependencies in clean packages", contains(m.issuesText(), "Unresolved dependencies: 0"));

    // ── evidence importer ──
    {
        Graph g;
        const std::string ok = evidence(
            "node\tpa5x:USR/Sound/5\tWarm Pad\n"
            "node\tpa5x:USR/Style/1\tBallad\n"
            "node\tpa5x:USR/Pad/2\tShaker\n"
            "ref\tpa5x:USR/Style/1\tStyleTrack\tpa5x:USR/Sound/5\tconfirmed\tVar1 Pad track\n"
            "ref\tpa5x:USR/Pad/2\tPadTrack\tpa5x:USR/Sound/9\tinferred\tname match only\n"
            "scanned\tStyleTrack\tall User Styles\n");
        TEST("valid evidence imports", imports(ok, g, &err));
        const auto u = g.whereUsed(P(Kind::Sound, 5));
        TEST("provenance and source kept on every reference", u.size() == 1 && u[0].provenance == "Report A" && u[0].source == Source::PaManagerReport);
        TEST("confirmed reference -> IN USE", g.deleteCheck(P(Kind::Sound, 5)).verdict == DeleteVerdict::InUse);
        const auto d9 = g.deleteCheck(P(Kind::Sound, 9));
        TEST("inferred-only reference is NOT elevated: Unprovable, labelled inferred",
             d9.verdict == DeleteVerdict::Unprovable && contains(d9.reasons[0], "1 inferred (not proven)"));
        TEST("inferred reference to a Sound not described is dangling (unresolved)", !g.dangling().empty());
        const auto d6 = g.deleteCheck(P(Kind::Sound, 6));
        TEST("a Pa5X Sound absent from Styles stays Unprovable (SongBook/Program Change unscanned)",
             d6.verdict == DeleteVerdict::Unprovable && contains(d6.reasons.back(), "not scanned: ProgramChange"));
        Graph g2 = g;
        TEST("importing the same evidence twice is refused", !imports(ok, g2, &err) && g2.edges().size() == g.edges().size());
        Graph g3;
        TEST("mixed confirmed + inferred -> IN USE", imports(evidence(
            "node\tpa5x:USR/Style/1\tBallad\nnode\tpa5x:USR/Pad/1\tP\n"
            "ref\tpa5x:USR/Style/1\tStyleTrack\tpa5x:USR/Sound/5\tconfirmed\tt1\n"
            "ref\tpa5x:USR/Pad/1\tPadTrack\tpa5x:USR/Sound/5\tinferred\tt2\n"), g3) &&
             g3.deleteCheck(P(Kind::Sound, 5)).verdict == DeleteVerdict::InUse);
    }
    {
        auto refused = [&](const std::string& t, const std::string& why) { Graph g; std::string e; return !importEvidence(t, g, nullptr, &e) && contains(e, why) && g.nodes().empty(); };
        TEST("PA Manager report cannot declare SongBook scanned (Q19)", refused(evidence("scanned\tSongBookSound\tall\n"), "Q19"));
        TEST("PA Manager report cannot declare Program Change scanned (Q19)", refused(evidence("scanned\tProgramChange\tall\n"), "Q19"));
        TEST("package-level vias cannot be declared scanned", refused(evidence("scanned\tZone\tall\n"), "only Pa5X-level"));
        TEST("provisional identity refused", refused(evidence("node\tprov:x#1/Sound/0\tA\n"), "Pa5X resources only"));
        TEST("Sample node refused (no PCM facts from a report)", refused(evidence("node\tpa5x:USR/Sample/1\tA\n"), "sample PCM facts"));
        TEST("wrong end count refused", refused("aiarranger-evidence\t1\nsource\tA\tok\tpa-manager-report\nend\t9\n", "end record"));
        TEST("missing authorization field refused", refused("aiarranger-evidence\t1\nsource\tA\tpa-manager-report\nend\t2\n", "authorization"));
        TEST("unknown source kind refused", refused("aiarranger-evidence\t1\nsource\tA\tok\tguess\nend\t2\n", "source"));
        TEST("bad confidence refused", refused(evidence("node\tpa5x:USR/Pad/1\tP\nref\tpa5x:USR/Pad/1\tPadTrack\tpa5x:USR/Sound/1\tlikely\tx\n"), "bad ref"));
        TEST("disallowed edge refused, all-or-nothing", refused(evidence("node\tpa5x:USR/Pad/1\tP\nref\tpa5x:USR/Pad/1\tZone\tpa5x:USR/Sound/1\tconfirmed\tx\n"), "edge not allowed"));
        TEST("reference from an undescribed resource refused", refused(evidence("ref\tpa5x:USR/Pad/1\tPadTrack\tpa5x:USR/Sound/1\tconfirmed\tx\n"), "unknown resource"));
        TEST("unknown record refused", refused(evidence("delete\tpa5x:USR/Sound/1\n"), "unknown or malformed"));
        TEST("truncated (no final newline) refused", refused(evidence("").substr(0, evidence("").size() - 1), "truncated"));
        Graph g;
        imports(evidence("node\tpa5x:USR/Sound/5\tWarm Pad\n"), g);
        std::string t = evidence("node\tpa5x:USR/Sound/5\tCold Pad\n");
        t.replace(t.find("Report A"), 8, "Report B");
        TEST("two sources naming one identity differently: refused, first kept", !imports(t, g, &err) && contains(err, "named") && g.find(P(Kind::Sound, 5))->name == "Warm Pad");
        {   // slot cardinality across sources (Codex P2): inferred never blocks; agreement corroborates; confirmed disagreement conflicts
            const std::string osc = "node\tpa5x:USR/Oscillator/1\tO\n";
            auto src = [&](const std::string& label, const std::string& conf, const std::string& to) {
                std::string t = evidence(osc + "ref\tpa5x:USR/Oscillator/1\tOscHigh\tpa5x:USR/Multisample/" + to + "\t" + conf + "\tH\n");
                return t.replace(t.find("Report A"), 8, label);
            };
            Graph s;
            TEST("inferred slot claim, then a confirmed one from another source: both kept",
                 imports(src("R1", "inferred", "1"), s) && imports(src("R2", "confirmed", "2"), s, &err) && s.uses(P(Kind::Oscillator, 1)).size() == 2);
            TEST("a second source agreeing on the same target corroborates", imports(src("R3", "confirmed", "2"), s, &err));
            TEST("two confirmed sources disagreeing: refused", !imports(src("R4", "confirmed", "3"), s, &err) && contains(err, "slot already holds"));
            TEST("one source claiming two multisamples in one slot: refused", !imports(evidence(osc +
                 "ref\tpa5x:USR/Oscillator/1\tOscHigh\tpa5x:USR/Multisample/1\tinferred\tH\n"
                 "ref\tpa5x:USR/Oscillator/1\tOscHigh\tpa5x:USR/Multisample/2\tinferred\tH\n"), s, &err));
        }
        Graph o;
        TEST("owner transcription may declare SongBook/Program Change scanned; then all five scanned -> Unreferenced",
             imports(evidence("scanned\tStyleTrack\ta\nscanned\tKeyboardSetPart\ta\nscanned\tPadTrack\ta\nscanned\tSongBookSound\ta\nscanned\tProgramChange\ta\n"
                              "node\tpa5x:USR/Sound/7\tX\n", "owner-transcription"), o) &&
             o.deleteCheck(P(Kind::Sound, 7)).verdict == DeleteVerdict::Unreferenced);
    }
    {
        ResourceBrowserModel e;
        spit(root / "ev.txt", evidence("node\tpa5x:USR/Style/1\tBallad\nref\tpa5x:USR/Style/1\tStyleTrack\tpa5x:USR/Sound/5\tinferred\tVar1\n"));
        TEST("model: evidence file opens next to packages", e.addPackage(root / "shared", &err) && e.addEvidence(root / "ev.txt", &err) && e.evidence().size() == 1);
        TEST("evidence roots browse; Pa5X ids shown as Pa5X; inferred label on the reference row", [&] {
            for (const auto& r : e.roots())
                if (r.kind == Kind::Style) {
                    const auto c = e.children(r);
                    return contains(e.displayId(r), "Pa5X USR Style 1") && c.size() == 1 && contains(e.rowLabel(c[0].to, &c[0]), "[inferred]") &&
                           contains(e.rowLabel(c[0].to, &c[0]), "MISSING");
                }
            return false;
        }());
        TEST("details show the evidence source and INFERRED", contains(e.details(P(Kind::Sound, 5)), "INFERRED, not proven") &&
             contains(e.details(P(Kind::Sound, 5)), "[evidence: Report A]"));
        TEST("unresolved dependency listed", contains(e.issuesText(), "Unresolved dependencies: 1") && contains(e.issuesText(), "pa5x:USR/Sound/5"));
        { std::string t = evidence("node\tpa5x:USR/Multisample/3\tStrings\n"); spit(root / "ev-noref.txt", t.replace(t.find("Report A"), 8, "Report N")); }
        TEST("evidence without references opens once; the same source again is refused (Codex P2)",
             e.addEvidence(root / "ev-noref.txt", &err) && !e.addEvidence(root / "ev-noref.txt", &err) && contains(err, "already open") && e.evidence().size() == 2);
        TEST("Pa5X resources from evidence do not inflate the import prediction (Codex P2)",
             predict(e.graph()).multisamples == 1 && predict(e.graph()).samples == 1);
        { std::string t = evidence("ref\tpa5x:USR/Style/1\tStyleTrack\tpa5x:USR/Sound/8\tconfirmed\tVar2\n");
          spit(root / "ev-dep.txt", t.replace(t.find("Report A"), 8, "Report D")); }
        TEST("evidence using another source's node opens (Codex round 2)", e.addEvidence(root / "ev-dep.txt", &err));
        {
            ResourceBrowserModel s;
            {
                ai_arranger::media::graph::Graph work = s.graph();
                const auto v = s.version();
                std::string label;
                TEST("prepare against a snapshot", ResourceBrowserModel::prepareEvidence(root / "ev.txt", work, label, &err));
                s.addPackage(root / "shared", &err);
                TEST("adopt refused when the resources changed meanwhile (Codex round 2)",
                     !s.adoptEvidence(work, label, v, &err) && contains(err, "changed") && s.evidence().empty());
            }
            const auto ep = s.epoch();
            s.clear();
            TEST("Close All changes the epoch (pending package loads are dropped)", s.epoch() != ep);
        }
        fs::create_directory(root / "evdir");
        TEST("evidence: a folder is refused", !e.addEvidence(root / "evdir", &err));
    }

    // ── memory check ──
    {
        ByteRange r;
        TEST("size '12.3 MB' covers rounding and both units", parseDisplayedSize("12.3 MB", r) && std::abs(r.lo - 12.25e6) < 1 && std::abs(r.hi - 12.35 * 1048576.0) < 1);
        TEST("size '5 B'", parseDisplayedSize("5 B", r) && r.lo == 4.5 && r.hi == 5.5);
        TEST("malformed sizes refused", !parseDisplayedSize("12.3MB", r) && !parseDisplayedSize("1.2.3 MB", r) && !parseDisplayedSize("-1 MB", r) &&
             !parseDisplayedSize("3 TB", r) && !parseDisplayedSize(".5 MB", r));
        MemoryInfoReading x;
        TEST("unknown field refused", !parseMemoryInfo("aiarranger-memory-info\t1\nsamplez\t3\n", x, &err));
        TEST("repeated field refused", !parseMemoryInfo("aiarranger-memory-info\t1\nsamples\t3\nsamples\t4\n", x, &err));
        TEST("wrong header refused", !parseMemoryInfo("memory\nsamples\t3\n", x, &err));
        TEST("blank fields (the UI template) are skipped, not errors", parseMemoryInfo("aiarranger-memory-info\t1\nsamples\t\nsamples-size\t\n", x, &err) && !x.samples && !x.samplesSize);
        TEST("hand-typed spaces and CRLF accepted", parseMemoryInfo("aiarranger-memory-info 1\r\nsamples   12\r\nsamples-size  12.3 MB\r\n", x, &err) &&
             x.samples == 12u && x.samplesSize && x.samplesSize->lo > 12e6);
        TEST("blank unknown field still refused", !parseMemoryInfo("aiarranger-memory-info\t1\nsamplez\n", x, &err));
        PredictedImport p{2, 1, 0, 1000000};
        const auto before = reading("samples\t10\nsamples-size\t5.0 MB\nmultisamples\t3\n");
        const std::string ok = compareMemoryInfo(before, reading("samples\t12\nsamples-size\t6.0 MB\nmultisamples\t4\n"), p);
        TEST("consistent: counts MATCH and growth range contains raw PCM", contains(ok, "Samples: 10 -> 12 (+2), predicted +2  MATCH") &&
             contains(ok, "CONSISTENT with raw PCM") && contains(ok, "Drum Samples: not compared"));
        TEST("smaller than raw PCM -> compression or other formula, not 'accurate'",
             contains(compareMemoryInfo(before, reading("samples-size\t5.5 MB\n"), p), "SMALLER than raw PCM"));
        TEST("larger than raw PCM -> overhead", contains(compareMemoryInfo(before, reading("samples-size\t9.0 MB\n"), p), "LARGER than raw PCM"));
        TEST("count difference is evidence for Q6, not a fault", contains(compareMemoryInfo(before, reading("samples\t14\n"), p), "evidence for Q6"));
        TEST("never claims validation", contains(ok, "does not validate the model"));
        TEST("model: needs the imported packages", contains(ResourceBrowserModel{}.memoryCheck("aiarranger-memory-info\t1\n", "aiarranger-memory-info\t1\n"), "Open the packages"));
        TEST("model: bad reading reported", contains(m.memoryCheck("x\n", "aiarranger-memory-info\t1\n"), "Before:"));
        TEST("model: prediction from the open packages (47+1 samples)", contains(m.memoryCheck("aiarranger-memory-info\t1\nsamples\t0\n",
             "aiarranger-memory-info\t1\nsamples\t48\n"), "predicted +48  MATCH"));
    }

    // ── KST Library tab (file level, read-only) ──
    {
        fs::create_directories(root / "kstA/STYLE");
        fs::create_directories(root / "kstB/STYLE");
        spit(root / "kstA/STYLE/BANK01.STY", "one");
        spit(root / "kstB/STYLE/BANK01.STY", "two");
        using A = ResourceBrowserModel::KstAction;
        const std::string inv = ResourceBrowserModel::kst(A::Inventory, (root / "kstA").string());
        TEST("inventory runs and states semantic merge is unavailable", contains(inv, "UNAVAILABLE") && contains(inv, "STY"));
        TEST("diff reports the changed file", contains(ResourceBrowserModel::kst(A::Diff, (root / "kstA").string(), (root / "kstB").string()), "BANK01.STY"));
        const std::string mp = ResourceBrowserModel::kst(A::MergePlan, (root / "kstA").string(), (root / "kstB").string());
        TEST("merge plan predicts a replacement, writes nothing", contains(mp, "BANK01.STY") && [&] {
            std::ifstream f(root / "kstB/STYLE/BANK01.STY"); std::string s; f >> s; return s == "two"; }());
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
