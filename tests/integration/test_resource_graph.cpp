// Resource graph + multi-package memory budget (docs/research/PA5X_SOUND_RESOURCE_GRAPH.md):
// identity vs content, shared resources, where-used, dangling references, delete safety (a Sound is
// never "unused" from Styles/KBD Sets/Pads alone), remap and copy impact, manifests (strict parser,
// WAV cross-check), memory limits, determinism. Original synthetic fixtures only; SOFTWARE evidence.

#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include "media/sound/package_writer.h"
#include <chrono>
#include <cstdio>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace ai_arranger::media::sound;
using namespace ai_arranger::media::graph;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static ResourceId P(Kind k, uint32_t n, const std::string& bank = "USR") { return {false, bank, k, n}; }
static ResourceId V(const std::string& pkg, Kind k, uint32_t n) { return {true, pkg, k, n}; }
static Node sampleNode(const ResourceId& id, uint64_t frames, uint8_t ch = 1, const std::string& hash = "00000000000000aa") {
    return {id, "S" + std::to_string(id.number), SampleInfo{ch, 48000, 16, frames, 0, hash, ""}};
}
static bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x.find(s) != std::string::npos) return true;
    return false;
}
// A tiny original instrument: one layer, two zones sharing ONE sample (a deliberately shared resource).
static Instrument sharedZoneInstrument(const std::string& name) {
    Instrument in;
    in.name = name;
    Sample s; s.name = "Tone"; s.root = 60; s.rate = 48000;
    for (int i = 0; i < 4800; ++i) s.pcm.push_back(int16_t((i % 96) * 300 - 14400));
    in.samples.push_back(s);
    Layer L; L.name = name; L.velLo = 0; L.velHi = 127;
    Zone a; a.lo = 0; a.hi = 60; a.sample = 0; a.root = 60;
    Zone b; b.lo = 61; b.hi = 127; b.sample = 0; b.root = 60;
    L.zones = {a, b};
    in.layers.push_back(L);
    return in;
}
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }
static void spit(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }
static bool parses(const std::string& m) { Graph g; std::string pkg, e; return parseManifest(m, g, pkg, &e); }
static std::string replaceOnce(std::string s, const std::string& a, const std::string& b) {
    const auto i = s.find(a); if (i == std::string::npos) return "NOT FOUND";
    return s.replace(i, a.size(), b);
}

int main() {
    std::printf("Test: resource graph + memory budget\n");
    std::string err;

    // ── identity ──
    {
        ResourceId r;
        TEST("ids round-trip; malformed ids refused",
             ResourceId::parse("pa5x:USR/Multisample/12", r) && r.str() == "pa5x:USR/Multisample/12" && !r.provisional &&
             !ResourceId::parse("pa5x:USR/Multisample/012", r) && !ResourceId::parse("pa5x:USR/Bogus/1", r) &&
             !ResourceId::parse("prov:a/b/Sample/1", r) && !ResourceId::parse("x:USR/Sample/1", r) && !ResourceId::parse("pa5x:/Sample/1", r) &&
             !ResourceId::parse("pa5x:USR/Sample/", r) && !ResourceId::parse("pa5x:USR/Sample/-1", r));
    }
    // ── shared samples (identity-based sharing) ──
    {
        Graph g;
        const Instrument in = sharedZoneInstrument("Shared");
        TEST("instrument graph built", addInstrument(g, in, "pkgA", &err));
        const ResourceId s0 = V("pkgA", Kind::Sample, 0);
        TEST("one Sample node used by two zones (where-used = 2 edges)", g.whereUsed(s0).size() == 2);
        TEST("usedByRoots reaches the Sound", g.usedByRoots(s0) == std::set<ResourceId>{V("pkgA", Kind::Sound, 0)});
        const auto b = budget(g);
        TEST("budget counts the shared sample once", b.samples == 1 && b.uncompressedBytes == 4800 * 2);
        TEST("in-use sample: InUse", g.deleteCheck(s0).verdict == DeleteVerdict::InUse);
        TEST("unused provisional Sound: Unreferenced (no Pa5X data can refer to it before import)",
             g.deleteCheck(V("pkgA", Kind::Sound, 0)).verdict == DeleteVerdict::Unreferenced);
        TEST("no issues", g.issues().empty());
    }
    // ── duplicate content, distinct identities: never deduplicated ──
    {
        Graph g;
        TEST("two packages with byte-identical audio",
             addInstrument(g, sharedZoneInstrument("One"), "pkgA", &err) && addInstrument(g, sharedZoneInstrument("Two"), "pkgB", &err));
        const auto b = budget(g);
        TEST("both counted; reported as one identical-content group, extra bytes shown but not removed",
             b.samples == 2 && b.uncompressedBytes == 2 * 9600 && b.identicalContent.size() == 1 && b.identicalExtraBytes == 9600 &&
             has(b.warnings, "byte-identical audio under distinct identities"));
    }
    // ── merge: same identity = shared only when identical ──
    {
        Graph a, b, c;
        a.addNode(sampleNode(P(Kind::Sample, 7), 1000), &err);
        b.addNode(sampleNode(P(Kind::Sample, 7), 1000), &err);
        c.addNode(sampleNode(P(Kind::Sample, 7), 999), &err);
        TEST("identical node under one identity merges to one shared resource", a.merge(b, &err) && a.nodes().size() == 1 && budget(a).samples == 1);
        TEST("same identity, different resource: merge refused", !a.merge(c, &err) && err.find("two different resources") != std::string::npos);
    }
    // ── missing / dangling references ──
    {
        Graph g;
        g.addNode({P(Kind::Multisample, 3), "MS", {}}, &err);
        g.addNode(sampleNode(P(Kind::Sample, 1), 100), &err);
        g.addRef({P(Kind::Multisample, 3), P(Kind::Sample, 1), Via::Zone, "zone 0"}, &err);
        TEST("reference to an absent resource is kept (dangling, not dropped)", g.addRef({P(Kind::Multisample, 3), P(Kind::Sample, 2), Via::Zone, "zone 1"}, &err));
        TEST("dangling() lists it; issues() reports it", g.dangling().size() == 1 && has(g.issues(), "dangling: pa5x:USR/Multisample/3"));
        const auto b = budget(g);
        TEST("budget: dangling sample has unknown size, not counted, with a warning",
             b.samples == 1 && b.unknownSizeSamples == 1 && has(b.warnings, "size is unknown and NOT counted"));
        g.addNode({P(Kind::Oscillator, 1), "OSC", {}}, &err);
        TEST("Pa5X oscillator of undescribed structure: unknown, not reported missing", !has(g.issues(), "pa5x:USR/Oscillator/1 (OSC) has no"));
        g.declareStructureKnown("USR");
        TEST("oscillator with no multisample reported as missing (plays silent)", has(g.issues(), "pa5x:USR/Oscillator/1 (OSC) has no multisample"));
        TEST("reference from an unknown resource refused", !g.addRef({P(Kind::Multisample, 9), P(Kind::Sample, 1), Via::Zone, "z"}, &err));
        TEST("edge type outside the spec refused (Sound -Zone-> Sample)",
             g.addNode({P(Kind::Sound, 1, "User1"), "Snd", {}}, &err) && !g.addRef({P(Kind::Sound, 1, "User1"), P(Kind::Sample, 1), Via::Zone, "z"}, &err));
        TEST("a Pa5X Style cannot reference a provisional Sound",
             g.addNode({P(Kind::Style, 1, "Fav"), "Sty", {}}, &err) && !g.addRef({P(Kind::Style, 1, "Fav"), V("pkg", Kind::Sound, 0), Via::StyleTrack, "t"}, &err));
        TEST("duplicate identity refused", !g.addNode(sampleNode(P(Kind::Sample, 1), 100), &err));
        TEST("sample facts on a non-Sample node refused", !g.addNode({P(Kind::Sound, 5), "x", SampleInfo{}}, &err));
        TEST("inconsistent sample facts refused (3 channels, 0 frames, loop > frames)",
             !g.addNode(sampleNode(P(Kind::Sample, 20), 100, 3), &err) && !g.addNode(sampleNode(P(Kind::Sample, 21), 0), &err) &&
             !g.addNode({P(Kind::Sample, 22), "x", SampleInfo{1, 48000, 16, 10, 11, "", ""}}, &err));
    }
    // ── delete safety: never "unused" from Styles / KBD Sets / Pads alone ──
    {
        Graph g;
        const ResourceId snd = P(Kind::Sound, 5, "User3");
        g.addNode({snd, "Piano", {}}, &err);
        TEST("Pa5X Sound with nothing scanned: Unprovable", g.deleteCheck(snd).verdict == DeleteVerdict::Unprovable);
        g.declareScanned(Via::StyleTrack, "fixture"); g.declareScanned(Via::KeyboardSetPart, "fixture"); g.declareScanned(Via::PadTrack, "fixture");
        const auto d = g.deleteCheck(snd);
        TEST("Styles + KBD Sets + Pads scanned, no users: STILL Unprovable (SongBook, Program Change unscanned)",
             d.verdict == DeleteVerdict::Unprovable && has(d.reasons, "not scanned: SongBookSound") && has(d.reasons, "not scanned: ProgramChange"));
        g.declareScanned(Via::SongBookSound, "fixture"); g.declareScanned(Via::ProgramChange, "fixture");
        TEST("all five referrer kinds scanned and no users: Unreferenced", g.deleteCheck(snd).verdict == DeleteVerdict::Unreferenced);
        g.addNode({P(Kind::Style, 1, "Fav"), "Pop", {}}, &err);
        g.addRef({P(Kind::Style, 1, "Fav"), snd, Via::ProgramChange, "PC in Var 1"}, &err);
        TEST("a Program Change reference makes it InUse", g.deleteCheck(snd).verdict == DeleteVerdict::InUse);
        TEST("Style: referrers not modelled, so Unprovable", g.deleteCheck(P(Kind::Style, 1, "Fav")).verdict == DeleteVerdict::Unprovable);
        TEST("unsafeDeletes lists both", g.unsafeDeletes().size() == 2);
    }
    // ── remap impact ──
    {
        Graph g;
        addInstrument(g, sharedZoneInstrument("R"), "pkgR", &err);
        addInstrument(g, sharedZoneInstrument("Q"), "pkgQ", &err);
        const ResourceId s0 = V("pkgR", Kind::Sample, 0);
        TEST("remap to self refused", has(g.remapImpact(s0, s0).conflicts, "same resource"));
        TEST("remap across kinds refused", has(g.remapImpact(s0, V("pkgR", Kind::Multisample, 0)).conflicts, "kind mismatch"));
        TEST("remap to a missing target refused (would dangle)", has(g.remapImpact(s0, V("pkgR", Kind::Sample, 9)).conflicts, "would dangle"));
        const auto x = g.remapImpact(s0, V("pkgQ", Kind::Sample, 0));
        TEST("remap into another package's provisional resource refused; 2 references listed",
             x.rewritten.size() == 2 && has(x.conflicts, "another package's provisional resource"));
        TEST("remap warns the source stays in memory", has(x.warnings, "stays in memory"));
        Graph h;
        h.addNode({P(Kind::Multisample, 1), "A", {}}, &err); h.addNode({P(Kind::Multisample, 2), "B", {}}, &err);
        h.addNode({P(Kind::Oscillator, 1), "O", {}}, &err);
        h.addRef({P(Kind::Oscillator, 1), P(Kind::Multisample, 1), Via::OscHigh, "High"}, &err);
        const auto y = h.remapImpact(P(Kind::Multisample, 1), P(Kind::Multisample, 2));
        TEST("Pa5X remap without scans: no conflict, but unscanned referrers warned",
             y.conflicts.empty() && y.rewritten.size() == 1 && has(y.warnings, "not scanned: OscLow"));
    }
    // ── copy impact ──
    {
        Graph src, dst;
        src.addNode({P(Kind::Sound, 1, "User1"), "Snd", {}}, &err);
        src.addNode({P(Kind::Oscillator, 1, "User1"), "O", {}}, &err);
        src.addNode({P(Kind::Multisample, 4), "MS", {}}, &err);
        src.addNode(sampleNode(P(Kind::Sample, 8), 100), &err);
        src.addRef({P(Kind::Sound, 1, "User1"), P(Kind::Oscillator, 1, "User1"), Via::OscillatorOf, "1"}, &err);
        src.addRef({P(Kind::Oscillator, 1, "User1"), P(Kind::Multisample, 4), Via::OscHigh, "High"}, &err);
        src.addRef({P(Kind::Multisample, 4), P(Kind::Sample, 8), Via::Zone, "zone 0"}, &err);
        src.addRef({P(Kind::Multisample, 4), P(Kind::Sample, 9), Via::Zone, "zone 1"}, &err);   // dangling in the source
        dst.addNode(sampleNode(P(Kind::Sample, 8), 555), &err);                                // different resource, same slot
        dst.addNode({P(Kind::Multisample, 70), "Other", {}}, &err);
        dst.addRef({P(Kind::Multisample, 70), P(Kind::Sample, 8), Via::Zone, "zone 0"}, &err);
        const auto c = Graph::copyImpact(src, {P(Kind::Sound, 1, "User1")}, dst);
        TEST("copy closure: Sound, oscillator, multisample, both samples (one dangling)", c.closure.size() == 5);
        TEST("overwrite of an occupied slot with a different resource is a conflict; destination user listed",
             c.overwrites == std::vector<ResourceId>{P(Kind::Sample, 8)} && has(c.conflicts, "overwrites it") &&
             c.destinationUsersAffected.size() == 1 && c.destinationUsersAffected[0].from == P(Kind::Multisample, 70));
        TEST("dangling reference carried as unresolved", c.unresolved.size() == 1 && c.unresolved[0].to == P(Kind::Sample, 9));
        Graph pk;
        addInstrument(pk, sharedZoneInstrument("New"), "pkgN", &err);
        const auto n = Graph::copyImpact(pk, {V("pkgN", Kind::Sound, 0)}, dst);
        TEST("provisional resources need a Pa5X slot; no overwrite possible", n.needsSlot.size() == 4 && n.overwrites.empty());
    }
    // ── memory limits: both 4 GB readings, 8 GB, sample count ──
    {
        auto fitOf = [](uint64_t bytes) {
            Graph g; std::string e;
            g.addNode(sampleNode(P(Kind::Sample, 0), bytes / 2), &e);   // 16-bit mono: 2 bytes per frame
            return budget(g).fit;
        };
        TEST("exactly 4 000 000 000 bytes: fits", fitOf(4000000000ull) == Fit::Fits);
        TEST("4 000 000 002: ambiguous (between decimal and binary 4 GB)", fitOf(4000000002ull) == Fit::Ambiguous);
        TEST("exactly 4 294 967 296: ambiguous", fitOf(4294967296ull) == Fit::Ambiguous);
        TEST("4 294 967 298: needs compression", fitOf(4294967298ull) == Fit::NeedsCompression);
        TEST("exactly 8 000 000 000: needs compression", fitOf(8000000000ull) == Fit::NeedsCompression);
        TEST("8 000 000 002: exceeds", fitOf(8000000002ull) == Fit::Exceeds);
        Graph g;
        g.addNode(sampleNode(P(Kind::Sample, 0), 3000000000ull), &err);
        const auto b = budget(g);
        TEST("over 4 GB: irreversible-compression warning; compressed figure labelled estimate",
             has(b.warnings, "cannot be undone") && b.compressedLowerBoundBytes == 3000000000ull &&
             formatBudget(b).find("compressed: UNKNOWN; ESTIMATE") != std::string::npos && budgetJson(b).find("\"reversible\": false") != std::string::npos);
        Graph many;
        for (uint32_t i = 0; i <= kPaManagerSampleCount; ++i) many.addNode(sampleNode(P(Kind::Sample, i), 10), &err);
        TEST("53 131 samples: warning against PA Manager's count figure", has(budget(many).warnings, "53 130"));
        Graph st;
        st.addNode(sampleNode(P(Kind::Sample, 0), 1000, 2), &err);
        st.addNode(sampleNode(P(Kind::Sample, 1), 1000, 1, "00000000000000bb"), &err);
        const auto sb = budget(st);
        TEST("mono/stereo and format contributions", sb.stereoBytes == 4000 && sb.monoBytes == 2000 && sb.byFormat.size() == 2 &&
             sb.byFormat.at("16-bit 48000 Hz stereo").second == 4000);
    }

    // ── regressions from the independent Codex review of 889eeff ──
    {
        Graph g;   // #5 byte arithmetic overflow through the API
        TEST("frame count beyond 2^32 refused (bytes() cannot wrap)", !g.addNode(sampleNode(P(Kind::Sample, 1), 1ull << 62, 2), &err));
        // #6 provisional resources added incrementally: no Unreferenced verdict before the package is declared complete
        g.addNode(sampleNode(V("pkgI", Kind::Sample, 0), 100), &err);
        TEST("provisional sample in an incomplete package: Unprovable", g.deleteCheck(V("pkgI", Kind::Sample, 0)).verdict == DeleteVerdict::Unprovable);
        g.declarePackageComplete("pkgI");
        TEST("after the package is declared complete: Unreferenced", g.deleteCheck(V("pkgI", Kind::Sample, 0)).verdict == DeleteVerdict::Unreferenced);
        // #2 copy conflict when the node matches but its references differ
        Graph src, dst;
        for (Graph* x : {&src, &dst}) {
            x->addNode({P(Kind::Multisample, 4), "MS", {}}, &err);
            x->addNode(sampleNode(P(Kind::Sample, 1), 100), &err);
            x->addNode(sampleNode(P(Kind::Sample, 2), 100, 1, "00000000000000bb"), &err);
        }
        src.addRef({P(Kind::Multisample, 4), P(Kind::Sample, 1), Via::Zone, "zone 0"}, &err);
        dst.addRef({P(Kind::Multisample, 4), P(Kind::Sample, 2), Via::Zone, "zone 0"}, &err);
        TEST("same multisample identity and name, different zone sample: copy conflict",
             has(Graph::copyImpact(src, {P(Kind::Multisample, 4)}, dst).conflicts, "pa5x:USR/Multisample/4 holds a different resource"));
        TEST("...and merging the two graphs is refused", !Graph(src).merge(dst, &err));
        // #4 adjacency indexes: a graph at PA Manager's sample-count figure stays fast
        Graph big;
        big.addNode({P(Kind::Multisample, 0), "Big", {}}, &err);
        for (uint32_t i = 0; i < 60000; ++i) {
            big.addNode(sampleNode(P(Kind::Sample, i), 1000, 1, hex16(i)), &err);
            big.addRef({P(Kind::Multisample, 0), P(Kind::Sample, i), Via::Zone, "zone " + std::to_string(i)}, &err);
        }
        const auto t0 = std::chrono::steady_clock::now();
        const auto bb = budget(big);
        const auto unsafe = big.unsafeDeletes();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("    60 000-sample graph: budget + unsafeDeletes %.3f s\n", secs);
        // round 2: slot cardinality
        Graph sl;
        sl.addNode({P(Kind::Sound, 1, "User1"), "A", {}}, &err); sl.addNode({P(Kind::Sound, 2, "User1"), "B", {}}, &err);
        sl.addNode({P(Kind::Oscillator, 1, "User1"), "O", {}}, &err);
        sl.addNode({P(Kind::Multisample, 1), "M1", {}}, &err); sl.addNode({P(Kind::Multisample, 2), "M2", {}}, &err);
        TEST("one High multisample per oscillator: a second High refused, Low accepted",
             sl.addRef({P(Kind::Oscillator, 1, "User1"), P(Kind::Multisample, 1), Via::OscHigh, "High"}, &err) &&
             !sl.addRef({P(Kind::Oscillator, 1, "User1"), P(Kind::Multisample, 2), Via::OscHigh, "High"}, &err) &&
             sl.addRef({P(Kind::Oscillator, 1, "User1"), P(Kind::Multisample, 2), Via::OscLow, "Low"}, &err));
        TEST("an oscillator belongs to one Sound",
             sl.addRef({P(Kind::Sound, 1, "User1"), P(Kind::Oscillator, 1, "User1"), Via::OscillatorOf, "1"}, &err) &&
             !sl.addRef({P(Kind::Sound, 2, "User1"), P(Kind::Oscillator, 1, "User1"), Via::OscillatorOf, "1"}, &err));
        Graph sl2;
        sl2.addNode({P(Kind::Sound, 2, "User1"), "B", {}}, &err); sl2.addNode({P(Kind::Oscillator, 1, "User1"), "O", {}}, &err);
        sl2.addRef({P(Kind::Sound, 2, "User1"), P(Kind::Oscillator, 1, "User1"), Via::OscillatorOf, "1"}, &err);
        const size_t before = sl.edges().size();
        // round 3 #2: remap impact applies the one-Sound-per-oscillator rule
        Graph rm;
        rm.addNode({V("pk", Kind::Sound, 0), "A", {}}, &err); rm.addNode({V("pk", Kind::Sound, 1), "B", {}}, &err);
        rm.addNode({V("pk", Kind::Oscillator, 1), "O1", {}}, &err); rm.addNode({V("pk", Kind::Oscillator, 2), "O2", {}}, &err);
        rm.addRef({V("pk", Kind::Sound, 0), V("pk", Kind::Oscillator, 1), Via::OscillatorOf, "1"}, &err);
        rm.addRef({V("pk", Kind::Sound, 1), V("pk", Kind::Oscillator, 2), Via::OscillatorOf, "1"}, &err);
        TEST("remap of an oscillator onto another Sound's oscillator is a conflict",
             has(rm.remapImpact(V("pk", Kind::Oscillator, 1), V("pk", Kind::Oscillator, 2)).conflicts, "already belongs to"));
        TEST("merge applies the same checks and is all-or-nothing", !sl.merge(sl2, &err) && sl.edges().size() == before);
        TEST("60 000 samples: budget and delete checks well under 10 s (was O(nodes x edges))", bb.samples == 60000 && unsafe.size() == 60001 && secs < 10);
    }

    // ── packages: writers emit RESOURCES.tsv; loader cross-checks WAVs; strict parser ──
    const fs::path root = fs::temp_directory_path() / ("aia-rgraph-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const Instrument organ = buildDemoOrgan();
    const DrumKit kit = buildDemoKit();
    TEST("Sound and Drum Kit packages written", !writePackage(organ, root / "organ", "", &err).empty() && !writeDrumPackage(kit, root / "drums", "", &err).empty());
    Graph go, gd; std::string po, pd;
    TEST("both load; every WAV matches its manifest record", loadPackage(root / "organ", go, po, &err) && loadPackage(root / "drums", gd, pd, &err));
    TEST("organ: 1 Sound, 2 oscillators, 2 multisamples, 38 samples", go.nodes().size() == 43);
    TEST("drums: 1 Drum Kit, 9 drum samples, 9 samples; 9 key layers", gd.nodes().size() == 19 &&
         gd.uses(V(pd, Kind::DrumKit, 0)).size() == 9);
    Graph all = go;
    TEST("multi-package merge", all.merge(gd, &err));
    const auto ab = budget(all);
    TEST("multi-package budget = sum of the packages' PCM bytes; 1 stereo sample (open hi-hat)",
         ab.uncompressedBytes == sampleBytes(organ) + [&] { uint64_t n = 0; for (const auto& s : kit.samples) n += s.pcm.size() * 2; return n; }() &&
         ab.samples == 47 && ab.stereoSamples == 1 && ab.fit == Fit::Fits);
    TEST("package ids differ and are provisional", po != pd && go.nodes().begin()->first.provisional);
    const std::string man = slurp(root / "organ/RESOURCES.tsv");
    TEST("deterministic manifest (rewritten package is byte-identical)",
         !writePackage(organ, root / "organ2", "", &err).empty() && slurp(root / "organ2/RESOURCES.tsv") == man);
    TEST("deterministic budget output", budgetJson(budget(all)) == budgetJson(ab) && formatBudget(budget(all)) == formatBudget(ab));

    // WAV folder converter -> package -> graph (integration)
    {
        Instrument w;
        const std::string e = buildFromWavFolder(root / "organ/wav", "From WAV", {"AIA_Organ_Soft", "AIA_Organ_Bright"}, {90}, &w);
        TEST("WAV-folder source packages carry a loadable manifest",
             e.empty() && !writePackage(w, root / "fromwav", "", &err).empty() && [&] { Graph x; std::string p; return loadPackage(root / "fromwav", x, p, &err); }());
    }
    // malformed and inconsistent manifests
    {
        const std::string pkgLine = "package\t" + po + "\tSound";
        TEST("wrong header refused", !parses(replaceOnce(man, "aiarranger-resources\t1", "aiarranger-resources\t2")));
        TEST("truncated (end record removed) refused", !parses(man.substr(0, man.rfind("end\t"))));
        TEST("truncated mid-line refused", !parses(man.substr(0, man.size() / 2)));
        TEST("wrong end count refused", !parses(replaceOnce(man, "end\t", "end\t1")));
        const std::string s0 = "prov:" + po + "/Sample/0";
        TEST("package claiming a Pa5X identity refused", !parses(replaceOnce(man, "sample\t" + s0, "sample\tpa5x:USR/Sample/0")));
        TEST("identity from another package refused", !parses(replaceOnce(man, "sample\t" + s0, "sample\tprov:other#1/Sample/0")));
        TEST("non-canonical number refused", !parses(replaceOnce(man, "\t48000\t", "\t048000\t")));
        TEST("parent path refused", !parses(replaceOnce(man, "\ttx/AIA Demo Organ/", "\ttx/../AIA Demo Organ/")));
        TEST("absolute path refused", !parses(replaceOnce(man, "\ttx/AIA Demo Organ/", "\t/tx/AIA Demo Organ/")));
        TEST("unknown via refused", !parses(replaceOnce(man, "\tZone\t", "\tZoneX\t")));
        TEST("edge type outside the spec refused", !parses(replaceOnce(man, "\tOscHigh\t", "\tZone\t")));
        TEST("reference to an undescribed resource refused (packages are self-contained)",
             !parses(replaceOnce(man, "\tZone\tprov:" + po + "/Sample/0\t", "\tZone\tprov:" + po + "/Sample/999\t")));
        TEST("duplicate record refused", !parses(replaceOnce(man, pkgLine + "\n", pkgLine + "\n" + man.substr(man.find("node\t"), man.find('\n', man.find("node\t")) - man.find("node\t") + 1))));
        TEST("zero frames refused", !parses(replaceOnce(man, "\t48000\t16\t", "\t48000\t16\t0\tX")));
        TEST("extra field refused", !parses(replaceOnce(man, "\tHigh\n", "\tHigh\textra\n")));
        TEST("control character in a name refused", !parses(replaceOnce(man, "AIA Demo Organ\n", "AIA Demo\x01Organ\n")));
        TEST("CRLF line endings refused", !parses(replaceOnce(man, "\n", "\r\n")));
        TEST("untouched manifest parses", parses(man));
        // WAV/manifest disagreement
        fs::copy(root / "organ", root / "bad", fs::copy_options::recursive);
        const fs::path wav = root / "bad/tx/AIA Demo Organ/AIA Demo Organ Samples" / (txw::fileSafe(organ.samples[0].name) + ".wav");
        std::string bytes = slurp(wav);
        bytes[bytes.find("data") + 8 + 100] ^= 0x01;             // one PCM bit flipped
        spit(wav, bytes);
        Graph x; std::string p;
        TEST("changed audio detected (fingerprint mismatch)", !loadPackage(root / "bad", x, p, &err) && err.find("does not match") != std::string::npos);
        fs::remove(wav);
        TEST("missing WAV detected", !loadPackage(root / "bad", x, p, &err) && err.find("cannot read") != std::string::npos);
        TEST("missing manifest detected", !loadPackage(root / "organ/wav", x, p, &err));
        // #1 manifest bit depth must match the WAV (8 instead of 16 would halve the budget)
        fs::copy(root / "organ", root / "bits", fs::copy_options::recursive);
        spit(root / "bits/RESOURCES.tsv", replaceOnce(man, "\t48000\t16\t", "\t48000\t8\t"));
        TEST("bit depth differing from the WAV refused", !loadPackage(root / "bits", x, p, &err) && err.find("does not match") != std::string::npos);
        // round 2 #1: a manifest that drops a sample's references (end count adjusted) is caught by the TX cross-check
        {
            std::string m2;
            size_t kept = 0, dropped = 0;
            const std::string target = "\tZone\tprov:" + po + "/Sample/0\t";
            std::stringstream in(man);
            for (std::string l; std::getline(in, l);) {
                if (l.rfind("end\t", 0) == 0) continue;
                if (l.find(target) != std::string::npos) { ++dropped; continue; }
                m2 += l + "\n"; ++kept;
            }
            m2 += "end\t" + std::to_string(kept) + "\n";
            fs::copy(root / "organ", root / "noref", fs::copy_options::recursive);
            spit(root / "noref/RESOURCES.tsv", m2);
            TEST("dropped references: manifest still parses, but the package load is refused",
                 dropped > 0 && parses(m2) && !loadPackage(root / "noref", x, p, &err) && err.find("no reference in RESOURCES.tsv") != std::string::npos);
        }
        // round 3 #1: a dropped DrumKeyLayer cannot make a drum sample look unreferenced in a loaded package
        {
            const std::string dm = slurp(root / "drums/RESOURCES.tsv");
            std::string m3; size_t kept = 0; bool dropped = false;
            std::stringstream in(dm);
            for (std::string l; std::getline(in, l);) {
                if (l.rfind("end\t", 0) == 0) continue;
                if (!dropped && l.find("\tDrumKeyLayer\t") != std::string::npos) { dropped = true; continue; }
                m3 += l + "\n"; ++kept;
            }
            m3 += "end\t" + std::to_string(kept) + "\n";
            fs::copy(root / "drums", root / "nolayer", fs::copy_options::recursive);
            spit(root / "nolayer/RESOURCES.tsv", m3);
            Graph y; std::string py;
            bool anyUnreferenced = false;
            const bool loaded = loadPackage(root / "nolayer", y, py, &err);
            for (const auto& [id, n] : y.nodes()) anyUnreferenced |= id.kind != Kind::DrumKit && y.deleteCheck(id).verdict == DeleteVerdict::Unreferenced;
            TEST("loaded package: no inner resource is ever Unreferenced (DrumKeyLayer dropped)", loaded && !anyUnreferenced);
            TEST("loaded package: the top-level Drum Kit is still provably unreferenced", y.deleteCheck(V(py, Kind::DrumKit, 0)).verdict == DeleteVerdict::Unreferenced);
            TEST("manifest declaring a Style node refused", !parses(replaceOnce(man, "/Oscillator/1\tOSC 1", "/Style/1\tOSC 1")));
        }
        // round 2 #3: FIFO and oversized files are input errors, not hangs
        {
            fs::copy(root / "organ", root / "fifo", fs::copy_options::recursive);
            const fs::path w = root / "fifo/tx/AIA Demo Organ/AIA Demo Organ Samples" / (txw::fileSafe(organ.samples[0].name) + ".wav");
            fs::remove(w);
            const bool made = ::mkfifo(w.c_str(), 0600) == 0;
            TEST("FIFO in place of a WAV refused without blocking", made && !loadPackage(root / "fifo", x, p, &err));
            fs::remove(w);
            { std::ofstream big(w, std::ios::binary); big.seekp((17 << 20) - 1); big.put(0); }
            TEST("WAV over 16 MiB refused before reading", !loadPackage(root / "fifo", x, p, &err) && err.find("cannot read") != std::string::npos);
        }
        // #3 a symlinked folder inside the package cannot lead the loader outside it
        fs::copy(root / "organ", root / "outside", fs::copy_options::recursive);
        fs::copy(root / "organ", root / "link", fs::copy_options::recursive);
        fs::remove_all(root / "link/tx");
        std::error_code lec;
        fs::create_directory_symlink(root / "outside/tx", root / "link/tx", lec);
        TEST("sample path through a symlinked folder refused", lec || (!loadPackage(root / "link", x, p, &err) && err.find("symlink") != std::string::npos));
    }
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
