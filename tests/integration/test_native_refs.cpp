// Native-KST reference adapter (src/media/sound/native_refs.h): synthetic, original fixtures only (no owner data, no
// real layout constants). Shared references, dangling references, unsupported layouts, partially analysed libraries,
// and "incomplete coverage never enables a delete". SOFTWARE evidence, not a Pa5X check.

#include "media/sound/native_refs.h"
#include <cstdio>

using namespace ai_arranger::media::graph;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static Id8 mk(uint8_t a, uint8_t b = 0x5A) { return Id8{a, b, 1, 2, 3, 4, 5, 6}; }
static const Id8 kEmpty{};   // all 00
static SampleInfo info(uint64_t frames = 100) { return SampleInfo{1, 48000, 16, frames, 0, "", ""}; }

// Synthetic KMP: multisamples M0..M3 (ids mk(0x10+i)), samples S0..S3 (mk(0x20+i)); M0:{S0,S1} M1:{S1} M2:{} M3:{S9 out of range}.
static KmpIndex kmp() {
    KmpIndex k;
    for (int i = 0; i < 4; ++i) k.samples.push_back({mk(0x20 + i), "s" + std::to_string(i), info()});
    k.samples[3].info.reset();                                   // no PCM facts: S3 has no node
    k.multisamples = {{mk(0x10), "m0", {0, 1}}, {mk(0x11), "m1", {1}}, {mk(0x12), "m2", {}}, {mk(0x13), "m3", {9, 3}}};
    k.drumIds = {mk(0x30)};
    return k;
}
// A body for a layout {base, record, pair}: slot values per record (slot0, slot1); other bytes carry a filler.
static DecodedSound snd(const std::string& bank, uint32_t num, uint32_t kind, const RecordLayout& L, const std::vector<std::pair<Id8, Id8>>& recs) {
    DecodedSound s; s.bank = bank; s.number = num; s.kind = kind; s.name = "snd" + std::to_string(num);
    s.body.assign(L.base + L.record * recs.size(), 0xA7);
    for (size_t q = 0; q < recs.size(); ++q) {
        std::copy(recs[q].first.begin(), recs[q].first.end(), s.body.begin() + L.base + q * L.record);
        std::copy(recs[q].second.begin(), recs[q].second.end(), s.body.begin() + L.base + q * L.record + L.pairOffset);
    }
    return s;
}

static bool build(const KmpIndex& k, const std::vector<DecodedSound>& v, const LayoutTable& l, Graph& g, NativeRefReport& r, std::string* err) {
    auto o = buildNativeGraph(k, v, l, r, err); if (o) g = std::move(*o); return o.has_value();
}

int main() {
    const RecordLayout A{20, 24, 9}, B{33, 40, 13};   // invented, unrelated to any real kind
    const uint32_t kA = 0x10990038, kB = 0x10980038, kC = 0x10970038;
    const LayoutTable layouts{{kA, A}, {kB, B}};
    const KmpIndex K = kmp();

    {   // shared + dependency listing
        NativeRefReport r; std::string err; Graph g;
        std::vector<DecodedSound> v{snd("U1", 0, kA, A, {{mk(0x10), kEmpty}, {mk(0x11), mk(0x10)}}), snd("U1", 1, kB, B, {{mk(0x10), mk(0x11)}})};
        TEST("adapter accepts consistent input", build(K, v, layouts, g, r, &err));
        TEST("both Sounds analysed", r.sounds == 2 && r.analysed == 2 && r.unanalysed.empty() && r.unresolved.empty());
        TEST("two different layouts coexist (nothing is universal)", r.analysed == 2);
        const auto sh = sharedCounts(g, Kind::Multisample);
        TEST("multisample 0 and 1 are shared by 2 Sounds", sh.size() == 2 && sh.at({false, "KST", Kind::Multisample, 0}) == 2 && sh.at({false, "KST", Kind::Multisample, 1}) == 2);
        const auto dep = dependencies(g, {false, "U1", Kind::Sound, 0});
        auto has = [&](Kind k, uint32_t n) { for (auto& d : dep) if (d.kind == k && d.number == n) return true; return false; };
        TEST("Sound 0 reaches M0, M1, S0, S1 through Oscillators", has(Kind::Multisample, 0) && has(Kind::Multisample, 1) && has(Kind::Sample, 0) && has(Kind::Sample, 1) && !has(Kind::Sample, 2));
        TEST("empty slot is counted, not an edge", r.emptySlots == 1);
        // role edges are inferred, zone edges confirmed
        bool oscInferred = true, zoneConfirmed = true;
        for (const Edge& e : g.edges()) { if (e.via == Via::Zone) zoneConfirmed &= e.confidence == Confidence::Confirmed; else oscInferred &= e.confidence == Confidence::Inferred; }
        TEST("oscillator/High/Low roles are Inferred, KMP zones Confirmed", oscInferred && zoneConfirmed);
        // delete safety: nothing is ever provably unreferenced, including M2 that nobody uses and S2 that nobody uses
        bool any = false; for (const auto& [id, n] : g.nodes()) any |= g.deleteCheck(id).verdict == DeleteVerdict::Unreferenced;
        TEST("no resource is Unreferenced, even with zero known users", !any);
        TEST("unused multisample M2 is Unprovable, not safe", g.deleteCheck({false, "KST", Kind::Multisample, 2}).verdict == DeleteVerdict::Unprovable);
        TEST("a multisample used only through Inferred edges is Unprovable, not InUse", g.deleteCheck({false, "KST", Kind::Multisample, 0}).verdict == DeleteVerdict::Unprovable);
    }
    {   // dangling references are explicit
        NativeRefReport r; std::string err; Graph g;
        std::vector<DecodedSound> v{snd("U1", 0, kA, A, {{mk(0x13), kEmpty}})};
        TEST("accepted", build(K, v, layouts, g, r, &err));
        int dang = 0; for (const Edge& e : g.dangling()) dang += e.from.kind == Kind::Multisample;
        TEST("zone to missing sample row and to sample without PCM facts are dangling", dang == 2);
        bool issue = false; for (const auto& s : g.issues()) issue |= s.find("dangling") != std::string::npos;
        TEST("dangling surfaces in issues()", issue);
    }
    {   // unsupported layout, size mismatch, no known reference, unresolved and wrong-kind slots, partial coverage
        NativeRefReport r; std::string err; Graph g;
        DecodedSound badSize = snd("U1", 1, kA, A, {{mk(0x10), kEmpty}}); badSize.body.push_back(0);
        DecodedSound noRef = snd("U1", 2, kA, A, {{kEmpty, kEmpty}});
        DecodedSound noLayout = snd("U1", 3, kC, A, {{mk(0x10), kEmpty}});
        DecodedSound mixed = snd("U1", 4, kA, A, {{mk(0x10), mk(0xEE)}, {kEmpty, mk(0x30)}, {mk(0x20), kEmpty}});   // unknown id, drum id, sample id
        DecodedSound good = snd("U1", 5, kA, A, {{mk(0x11), kEmpty}});
        TEST("accepted", build(K, {badSize, noRef, noLayout, mixed, good}, layouts, g, r, &err));
        auto why = [&](uint32_t n, Gap w) { for (auto& u : r.unanalysed) if (u.sound.number == n && u.why == w) return true; return false; };
        TEST("size mismatch reported UNANALYSED", why(1, Gap::SizeDoesNotFit));
        TEST("no known reference reported UNANALYSED", why(2, Gap::NoKnownReference));
        TEST("kind without a layout reported UNANALYSED", why(3, Gap::NoLayoutForKind));
        TEST("analysed: the mixed and the good Sound", r.analysed == 2 && r.sounds == 5);
        size_t wrong = 0, unk = 0; for (auto& u : r.unresolved) (u.wrongKind ? wrong : unk)++;
        TEST("unknown ID, drum ID and sample ID in a multisample slot are UNRESOLVED (2 wrong kind, 1 unknown)", r.unresolved.size() == 3 && wrong == 2 && unk == 1);
        TEST("coverage is not complete", !r.coverageComplete());
        bool any = false; for (const auto& [id, n] : g.nodes()) any |= g.deleteCheck(id).verdict == DeleteVerdict::Unreferenced;
        TEST("partial coverage enables no delete", !any);
        TEST("unanalysed Sound nodes exist but reach nothing", g.uses({false, "U1", Kind::Sound, 1}).empty());
    }
    {   // fitLayout derives layouts from data, differently for different data
        for (const RecordLayout& L : {A, B}) {
            std::vector<DecodedSound> items;
            for (uint32_t i = 0; i < 10; ++i) {
                std::vector<std::pair<Id8, Id8>> rec;
                for (uint32_t q = 0; q <= i % 3; ++q) rec.push_back({mk(0x10 + (i + q) % 3), (q & 1) ? kEmpty : mk(0x11)});
                if (i % 3 == 0 && i > 0) rec.push_back({mk(0x10), mk(0x12)});   // item 0 keeps exactly one record (otherwise the size is ambiguous)
                items.push_back(snd("U", i, 1, L, rec));
            }
            std::vector<const DecodedSound*> p; for (auto& s : items) p.push_back(&s);
            const auto f = fitLayout(p, K);
            TEST("fitLayout recovers an invented layout", f && f->base == L.base && f->record == L.record && f->pairOffset == L.pairOffset);
            std::vector<const DecodedSound*> few(p.begin(), p.begin() + 3);
            TEST("fitLayout refuses too few items", !fitLayout(few, K));
        }
        std::vector<DecodedSound> junk(10); for (auto& s : junk) s.body.assign(50, 0x33);
        std::vector<const DecodedSound*> p; for (auto& s : junk) p.push_back(&s);
        TEST("fitLayout returns nothing when no ID occurs", !fitLayout(p, K));
    }
    {   // all-or-nothing
        KmpIndex d = kmp(); d.multisamples.push_back(d.multisamples[0]);
        NativeRefReport r; std::string err; Graph g;
        TEST("duplicate multisample ID refused, graph untouched", !build(d, {}, layouts, g, r, &err) && g.nodes().empty() && !err.empty());
        NativeRefReport r2;
        TEST("duplicate Sound identity refused", !buildNativeGraph(K, {snd("U", 0, kA, A, {{mk(0x10), kEmpty}}), snd("U", 0, kA, A, {{mk(0x10), kEmpty}})}, layouts, r2, &err));
    }
    {   // Codex round 1 regressions
        NativeRefReport r; std::string err;
        const LayoutTable evil{{kA, RecordLayout{0, 8, 0xFFFFFFFFu}}};
        DecodedSound e; e.bank = "U"; e.number = 0; e.kind = kA; e.body = std::vector<uint8_t>(8, 0x11);
        auto o = buildNativeGraph(K, {e}, evil, r, &err);
        TEST("pairOffset that wraps 32-bit arithmetic is a bad layout, no out-of-bounds read", o && r.unanalysed.size() == 1 && r.unanalysed[0].why == Gap::NoLayoutForKind);
        DecodedSound tiny = e; tiny.body.assign(8, 0);
        TEST("record smaller than one ID is a bad layout", buildNativeGraph(K, {tiny}, {{kA, RecordLayout{0, 4, 0}}}, r, &err) && r.unanalysed[0].why == Gap::NoLayoutForKind);
        DecodedSound big = snd("U", 536870912u, kA, A, {{mk(0x10), kEmpty}}), z = snd("U", 0, kA, A, {{mk(0x10), kEmpty}});
        TEST("huge Sound numbers cannot wrap into another oscillator identity", buildNativeGraph(K, {z, big}, layouts, r, &err) && r.unanalysed.size() == 1 && r.unanalysed[0].why == Gap::NumberOutOfRange && r.analysed == 1);
        DecodedSound onlyUnknown = snd("U", 7, kA, A, {{mk(0xEE), kEmpty}});
        TEST("unknown slot is kept when the Sound has no known reference", buildNativeGraph(K, {onlyUnknown}, layouts, r, &err) && r.unanalysed.size() == 1 && r.unresolved.size() == 1 && r.unresolved[0].record == 0 && r.unresolved[0].slot == 0);
        {   // pair offset is voted by the items that define the base only
            std::vector<DecodedSound> it; const RecordLayout L{0, 16, 8};
            for (uint32_t i = 0; i < 5; ++i) it.push_back(snd("U", i, 1, L, {{mk(0x10), mk(0x11)}}));
            for (uint32_t i = 5; i < 9; ++i) { DecodedSound d; d.bank = "U"; d.number = i; d.body.assign(60, 0xA7);   // base 1; IDs at 1, 11, 17, 27 (remainder 1 and 11 mod 16)
                const Id8 a = mk(0x10), b = mk(0x11); for (size_t o : {1, 11, 17, 27}) std::copy((o % 2 ? a : b).begin(), (o % 2 ? a : b).end(), d.body.begin() + long(o)); it.push_back(d); }
            std::vector<const DecodedSound*> p; for (auto& d : it) p.push_back(&d);
            const auto f = fitLayout(p, K, 5);
            TEST("pair offset is not outvoted by items of another base", f && f->base == 0 && f->record == 16 && f->pairOffset == 8);
        }
        {   // ambiguity fails closed
            auto make = [&](auto rows) { std::vector<DecodedSound> it; uint32_t i = 0; for (auto& r : rows) it.push_back(snd("U", i++, 1, A, r)); return it; };
            using Rows = std::vector<std::pair<Id8, Id8>>;
            std::vector<Rows> twoOnly(10, Rows{{mk(0x10), mk(0x11)}, {mk(0x11), kEmpty}});   // every item holds exactly 2 records: size alone cannot tell 24 from 48
            auto a = make(twoOnly); std::vector<const DecodedSound*> pa; for (auto& d : a) pa.push_back(&d);
            TEST("every item has >= 2 records: ambiguous record size, no layout", !fitLayout(pa, K));
            std::vector<Rows> ok(10, Rows{{mk(0x10), mk(0x11)}}); ok[3].push_back({mk(0x11), kEmpty});
            auto b = make(ok); std::vector<const DecodedSound*> pb; for (auto& d : b) pb.push_back(&d);
            TEST("same data with one single-record item is accepted", fitLayout(pb, K) && fitLayout(pb, K)->record == A.record);
            b[5].body[A.base + 3] = 0; b[5].body[A.base + 4] = 0;                      // damage nothing relevant, then add a stray ID at a third remainder
            const Id8 stray = mk(0x12); std::copy(stray.begin(), stray.end(), b[0].body.begin() + A.base + 16);
            TEST("an ID outside slot 0 / pair slot makes the fit refuse", !fitLayout(pb, K));
        }
        std::vector<const DecodedSound*> none;
        TEST("fitLayout with no items and minItems 0 returns nothing", !fitLayout(none, K, 0));
        // declarations on a caller's graph cannot leak: the result is a fresh graph, and it is Unprovable even for an unused multisample
        Graph theirs; theirs.declareScanned(Via::OscHigh, "caller"); theirs.declareScanned(Via::OscLow, "caller");
        auto fresh = buildNativeGraph(K, {}, layouts, r, &err);
        TEST("fresh graph: unused multisample is Unprovable although the caller scanned elsewhere", fresh && fresh->deleteCheck({false, "KST", Kind::Multisample, 2}).verdict == DeleteVerdict::Unprovable);
    }
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
