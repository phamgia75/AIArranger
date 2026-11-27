// Slice M2b Synthetic Regression & M2a Integration Tests:
//   - M1 Manifest Adapter (`src/media/sound/m1_manifest_adapter.h`) reusing M2a canonical types
//   - Automatic Selective Dependency Collector, 3-Tier Deduplicator, M2a CollisionMatrix Integration,
//     Memory Projection & Reproducible Dry-Run Plan (`src/media/sound/selective_collector.h`)
//
// 100% synthetic, original fixtures only (zero owner library files, proprietary bytes, real names, or local paths).
// Strictly read-only: zero source/target library mutations, zero OC31 encoder in production code,
// deleteCheck() NEVER returns Unreferenced for Native-KST.

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "jobs/import_kit.h"
#include "media/sound/package_graph.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "media/panative/root_tree.h"
#include "media/sound/m1_manifest_adapter.h"
#include "media/sound/selective_collector.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace ai_arranger::media::graph;
using namespace ai_arranger::media::panative;
using namespace ai_arranger::media::selective;
namespace sound = ai_arranger::media::sound;
namespace kit = ai_arranger::jobs::kit;
namespace fs = std::filesystem;

static int gFailures = 0;
static int gPasses = 0;

#define TEST(name, expr)                                              \
    do {                                                              \
        if (!(expr)) {                                                \
            std::fprintf(stderr, "  FAIL: %s\n", name);               \
            ++gFailures;                                              \
        } else {                                                      \
            std::printf("  PASS: %s\n", name);                        \
            ++gPasses;                                                \
        }                                                             \
    } while (0)

using Bytes = std::vector<uint8_t>;

// ── Synthetic fixture builders (test-only, 100% original) ────────────────────────────────────────────────────────────

static void pushBe16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
}
static void pushBe32(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 24));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
}
static void putStr(Bytes& b, size_t at, const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) b[at + i] = static_cast<uint8_t>(s[i]);
}
static void appendStr(Bytes& b, const std::string& s) {
    b.insert(b.end(), reinterpret_cast<const uint8_t*>(s.data()),
             reinterpret_cast<const uint8_t*>(s.data()) + s.size());
}
static Id8 makeId(uint8_t tag, uint8_t idx) {
    return Id8{tag, idx, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
}
static const Id8 kZeroId{};
static std::string idAsStr(const Id8& id) {
    return std::string(reinterpret_cast<const char*>(id.data()), id.size());
}
static uint8_t xorBytes(const Bytes& b) {
    uint8_t x = 0;
    for (uint8_t v : b) x ^= v;
    return x;
}

static Bytes makeOcLiteralFrame(const char magic[4], const Bytes& plain) {
    Bytes ops;
    if (plain.size() >= 1 && plain.size() <= 3) {
        ops.push_back(0x01); ops.push_back(0x00); ops.push_back(static_cast<uint8_t>(plain.size()));
        ops.insert(ops.end(), plain.begin(), plain.end());
    } else if (plain.size() >= 4 && plain.size() <= 17) {
        ops.push_back(static_cast<uint8_t>(plain.size() - 2));
        ops.insert(ops.end(), plain.begin(), plain.end());
    } else if (plain.size() >= 18 && plain.size() <= 273) {
        ops.push_back(0x00); ops.push_back(static_cast<uint8_t>(plain.size() - 18));
        ops.insert(ops.end(), plain.begin(), plain.end());
    }
    ops.push_back(0x01); ops.push_back(0x00); ops.push_back(0x00);
    Bytes out;
    out.reserve(9 + ops.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(magic[i]));
    const uint32_t want = static_cast<uint32_t>(plain.size());
    out.push_back(static_cast<uint8_t>(want & 0xFFu));
    out.push_back(static_cast<uint8_t>((want >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((want >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((want >> 24) & 0xFFu));
    out.insert(out.end(), ops.begin(), ops.end());
    out.push_back(xorBytes(plain));
    return out;
}

struct SynthKorfItem {
    uint32_t chunkId = 0;
    uint8_t kind = 0, bank = 0, slot = 0;
    std::string name;
    Bytes payload;
    std::string sampleId;
};

static Bytes buildKorfContainer(const std::vector<SynthKorfItem>& items, int format = 1) {
    Bytes dir;
    for (const auto& it : items) {
        const uint16_t sub = static_cast<uint16_t>((it.chunkId >> 8) & 0xFFFFu);
        if (format == 0) {
            std::string n = it.name; n.resize(18, '\0');
            appendStr(dir, n);
            dir.push_back(it.kind); dir.push_back(it.bank); dir.push_back(it.slot);
            pushBe16(dir, sub); dir.push_back(0);
        } else {
            Bytes rec;
            pushBe16(rec, it.sampleId.empty() ? 3u : 4u);
            std::string n = it.name; n.resize(28, '\0');
            pushBe16(rec, 2); pushBe16(rec, 28); appendStr(rec, n);
            pushBe16(rec, 0); pushBe16(rec, 3); rec.push_back(it.kind); rec.push_back(it.bank); rec.push_back(it.slot);
            pushBe16(rec, 1); pushBe16(rec, 2); pushBe16(rec, sub);
            if (!it.sampleId.empty()) { pushBe16(rec, 4); pushBe16(rec, 8); appendStr(rec, it.sampleId); }
            rec.push_back(0x11); rec.push_back(0x22); rec.push_back(0x33); rec.push_back(0x44);
            pushBe16(dir, static_cast<uint16_t>(rec.size()));
            dir.insert(dir.end(), rec.begin(), rec.end());
        }
    }
    Bytes body;
    pushBe32(body, 0x02000018u); pushBe32(body, 12u);
    const uint8_t hdr[12] = {0x0B, 0, 0, 0, 0, 0, 0, 'K', 'O', 'R', 'F', 0};
    body.insert(body.end(), hdr, hdr + 12);
    pushBe32(body, format ? 0x05010018u : 0x05000018u);
    pushBe32(body, static_cast<uint32_t>(dir.size()));
    body.insert(body.end(), dir.begin(), dir.end());

    std::vector<uint32_t> offs{0x1Cu};
    for (const auto& it : items) {
        offs.push_back(static_cast<uint32_t>(8u + body.size()));
        pushBe32(body, it.chunkId);
        pushBe32(body, static_cast<uint32_t>(it.payload.size()));
        body.insert(body.end(), it.payload.begin(), it.payload.end());
    }
    Bytes idx;
    appendStr(idx, "KBEG");
    for (uint32_t o : offs) pushBe32(idx, o);
    appendStr(idx, "KEND");
    pushBe32(idx, static_cast<uint32_t>(offs.size()));
    pushBe32(body, 0xFE000018u);
    pushBe32(body, static_cast<uint32_t>(idx.size()));
    body.insert(body.end(), idx.begin(), idx.end());

    Bytes out;
    pushBe32(out, 0x01000114u);
    pushBe32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

struct SynthKmpSpec {
    struct S { Id8 id; std::string name; uint16_t rate; };
    struct D { Id8 id; std::string name; uint32_t left; uint32_t right; };
    struct Z { uint32_t sample; uint8_t topKey; };
    struct M { Id8 id; std::string name; uint32_t firstZone; uint8_t zoneCount; };
    std::vector<S> samples;
    std::vector<D> drums;
    std::vector<Z> zones;
    std::vector<M> multis;
};

static Bytes buildPa5xKmpPayload(const SynthKmpSpec& sp) {
    Bytes out;
    pushBe16(out, 0x0103u);
    pushBe32(out, static_cast<uint32_t>(sp.samples.size()));
    pushBe32(out, static_cast<uint32_t>(sp.drums.size()));
    pushBe32(out, static_cast<uint32_t>(sp.zones.size()));
    pushBe32(out, static_cast<uint32_t>(sp.multis.size()));
    for (const auto& s : sp.samples) {
        Bytes r(122, 0);
        putStr(r, 75, s.name);
        std::copy(s.id.begin(), s.id.end(), r.begin() + 107);
        r[119] = static_cast<uint8_t>(s.rate >> 8);
        r[120] = static_cast<uint8_t>(s.rate & 0xFFu);
        out.insert(out.end(), r.begin(), r.end());
    }
    for (const auto& d : sp.drums) {
        Bytes r(126, 0);
        putStr(r, 0, d.name);
        for (int i = 0; i < 4; ++i) r[32 + size_t(i)] = static_cast<uint8_t>(d.left >> (24 - 8 * i));
        for (int i = 0; i < 4; ++i) r[36 + size_t(i)] = static_cast<uint8_t>(d.right >> (24 - 8 * i));
        std::copy(d.id.begin(), d.id.end(), r.begin() + 118);
        out.insert(out.end(), r.begin(), r.end());
    }
    for (const auto& z : sp.zones) {
        Bytes r(16, 0);
        for (int i = 0; i < 4; ++i) r[size_t(i)] = static_cast<uint8_t>(z.sample >> (24 - 8 * i));
        r[10] = z.topKey;
        out.insert(out.end(), r.begin(), r.end());
    }
    for (const auto& m : sp.multis) {
        Bytes r(176, 0);
        putStr(r, 0, m.name);
        for (int i = 0; i < 4; ++i) r[32 + size_t(i)] = static_cast<uint8_t>(m.firstZone >> (24 - 8 * i));
        r[166] = m.zoneCount;
        std::copy(m.id.begin(), m.id.end(), r.begin() + 168);
        out.insert(out.end(), r.begin(), r.end());
    }
    return out;
}

static Bytes buildSoundBody(const RecordLayout& L, const std::vector<std::pair<Id8, Id8>>& oscs, uint8_t fill = 0x5Au) {
    Bytes body(L.base + oscs.size() * L.record, fill);
    for (size_t q = 0; q < oscs.size(); ++q) {
        const size_t at = L.base + q * L.record;
        std::copy(oscs[q].first.begin(), oscs[q].first.end(), body.begin() + static_cast<std::ptrdiff_t>(at));
        std::copy(oscs[q].second.begin(), oscs[q].second.end(), body.begin() + static_cast<std::ptrdiff_t>(at + L.pairOffset));
    }
    return body;
}

static roottree::Node rtLeaf(const std::string& name, const std::string& payload) {
    roottree::Node n;
    n.kind = roottree::Node::Leaf;
    n.name = name;
    n.payload = payload;
    return n;
}
static roottree::Node rtBranch(const std::string& name, std::vector<roottree::Node> kids) {
    roottree::Node n;
    n.kind = roottree::Node::Branch;
    n.name = name;
    n.children = std::move(kids);
    return n;
}
static Bytes rtSerialize(std::vector<roottree::Node> kids) {
    roottree::Node r;
    r.kind = roottree::Node::Root;
    r.name = "\x01";
    r.children = std::move(kids);
    std::string out, err;
    if (!roottree::write(r, out, err)) return {};
    return Bytes(out.begin(), out.end());
}

struct SmfSelSpec {
    uint8_t ch = 0;
    int msb = -1;
    int lsb = -1;
    uint8_t pc = 0;
};

static std::string makeSmf(const std::vector<SmfSelSpec>& sels, bool injectMalformedCc11 = false) {
    std::string trk;
    for (const auto& s : sels) {
        if (s.msb >= 0) {
            trk.push_back(0x00);
            trk.push_back(static_cast<char>(0xB0u | (s.ch & 0x0Fu)));
            trk.push_back(0x00);
            trk.push_back(static_cast<char>(s.msb & 0x7Fu));
        }
        if (s.lsb >= 0) {
            trk.push_back(0x00);
            trk.push_back(static_cast<char>(0xB0u | (s.ch & 0x0Fu)));
            trk.push_back(0x20);
            trk.push_back(static_cast<char>(s.lsb & 0x7Fu));
        }
        trk.push_back(0x00);
        trk.push_back(static_cast<char>(0xC0u | (s.ch & 0x0Fu)));
        trk.push_back(static_cast<char>(s.pc & 0x7Fu));
    }
    if (injectMalformedCc11) {
        trk.push_back(0x00);
        trk.push_back(static_cast<char>(0xB0u));
        trk.push_back(0x0B);
        trk.push_back(static_cast<char>(0x80u));
    }
    trk.push_back(0x00); trk.push_back(static_cast<char>(0xFFu)); trk.push_back(0x2F); trk.push_back(0x00);

    std::string smf;
    smf.append("MThd", 4);
    const uint8_t hdr[10] = {0, 0, 0, 6, 0, 0, 0, 1, 0, 0x60};
    smf.append(reinterpret_cast<const char*>(hdr), sizeof hdr);
    smf.append("MTrk", 4);
    const uint32_t len = static_cast<uint32_t>(trk.size());
    smf.push_back(static_cast<char>((len >> 24) & 0xFFu));
    smf.push_back(static_cast<char>((len >> 16) & 0xFFu));
    smf.push_back(static_cast<char>((len >> 8) & 0xFFu));
    smf.push_back(static_cast<char>(len & 0xFFu));
    smf += trk;
    return smf;
}

static SelectionTarget selStyle(const std::string& relPath, const std::string& targetBank = "", const std::string& targetName = "") {
    return SelectionTarget{SelectedKind::Style, relPath, targetBank, targetName};
}

static void writeFileBytes(const fs::path& p, const Bytes& data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// ── Test 1: Multi-Style Sharing (5 Styles -> 3 Sounds -> 2 Multisamples -> 10 Samples) + Embedded KBD/Pad Flags ───────

static void testMultiStyleSharingAndEmbeddedFlags() {
    std::printf("[Test 1] Multi-Style sharing (5 Styles / 3 Sounds / 2 MS / 10 Samples) & embedded KBD/Pad flags\n");

    const RecordLayout L{12, 24, 8};
    const uint32_t kKind = 0x10050038u;

    SynthKmpSpec kmp;
    std::vector<SynthKorfItem> pcm1Items, pcm2Items;
    uint64_t expectedUniqueSampleBytes = 0;
    std::vector<uint64_t> sampleSizes(10, 0);
    for (uint8_t i = 0; i < 10; ++i) {
        const Id8 sid = makeId(0xA1, i);
        const uint32_t bytes = 2000u + 200u * i;
        sampleSizes[i] = bytes;
        expectedUniqueSampleBytes += bytes;
        kmp.samples.push_back({sid, "Smp" + std::to_string(i), 48000});
        SynthKorfItem pi{0x01010018u, 0, 0, i, "Smp" + std::to_string(i), Bytes(bytes, uint8_t(0x10 + i)), idAsStr(sid)};
        if (i < 5) pcm1Items.push_back(std::move(pi));
        else pcm2Items.push_back(std::move(pi));
    }
    for (uint32_t s = 0; s <= 5; ++s) kmp.zones.push_back({s, static_cast<uint8_t>(40 + s * 4)});
    for (uint32_t s = 4; s <= 9; ++s) kmp.zones.push_back({s, static_cast<uint8_t>(70 + (s - 4) * 4)});
    const Id8 ms0Id = makeId(0xB1, 0);
    const Id8 ms1Id = makeId(0xB1, 1);
    kmp.multis.push_back({ms0Id, "MsAlpha", 0, 6});
    kmp.multis.push_back({ms1Id, "MsBeta", 6, 6});

    const Bytes s0Body = buildSoundBody(L, {{ms0Id, kZeroId}});
    const Bytes s1Body = buildSoundBody(L, {{ms1Id, kZeroId}});
    const Bytes s2Body = buildSoundBody(L, {{ms0Id, ms1Id}});

    const std::vector<SynthKorfItem> sndItems{
        {kKind, 0, 0, 0, "SynthLeadA", makeOcLiteralFrame("OC31", s0Body), ""},
        {kKind, 0, 0, 1, "SynthPadB",  makeOcLiteralFrame("OC31", s1Body), ""},
        {kKind, 0, 0, 2, "SynthStackC", makeOcLiteralFrame("OC31", s2Body), ""},
    };

    NativeKstClosureInput in;
    in.downstream.kmpContainer = RawNamedBlob{"MULTISMP/RAM.KMP", "", buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM", buildPa5xKmpPayload(kmp), ""}})};
    in.downstream.pcmContainers.push_back({"PCM/ram01.pcm", "", buildKorfContainer(pcm1Items)});
    in.downstream.pcmContainers.push_back({"PCM/ram02.pcm", "", buildKorfContainer(pcm2Items)});
    in.downstream.soundContainers.push_back({"SOUND/USER01.PCG", "USER01", buildKorfContainer(sndItems)});

    for (int i = 1; i <= 5; ++i) {
        const std::string smfMain = makeSmf({{0, 121, 64, 0}, {1, 121, 0, 0}});
        const std::string smfKbd  = makeSmf({{0, 121, 64, 1}});
        const std::string smfPad  = makeSmf({{0, 121, 64, 2}});
        const Bytes stg = rtSerialize({
            rtBranch("Var1.scn", {rtBranch("Track1.pdc", {rtLeaf("Main.mid", smfMain)})}),
            rtBranch("KbdSet1-4.scn", {rtBranch("KS1.ksc", {rtBranch("Main.ssn", {rtBranch("RTTracks.scn", {rtBranch("Upper1.plc", {rtLeaf("Main.mid", smfKbd)})})})})}),
            rtBranch("PadSet1.scn", {rtBranch("Pad1.pdc", {rtLeaf("Main.mid", smfPad)})})
        });
        in.upstreamRoots.push_back({"STYLE/BankA/Style0" + std::to_string(i) + ".stg", stg});
    }

    M1AdapterOptions aopts;
    aopts.closureOptions.downstream.layouts[kKind] = L;
    aopts.closureOptions.downstream.allowFitLayout = false;
    const M1AdapterView view = adaptNativeKstInput(in, aopts);

    TEST("adapter view ok & native", view.ok && view.isNativeKst() && !view.cancelled);
    TEST("adapter roots count == 5", view.roots.size() == 5);
    TEST("deleteProofAvailable == false", !view.coverage.deleteProofAvailable);

    // M2a parity check: equivalenceDigest computed by M2b adapter MUST match M2a inspectTargetKstInput() byte-for-byte!
    TargetInspectOptions topts;
    topts.downstream.layouts[kKind] = L;
    topts.downstream.allowFitLayout = false;
    const TargetOccupancyMap m2aMap = inspectTargetKstInput(in.downstream, {}, topts);
    const ResourceId s0Id{false, "USER01", Kind::Sound, 0};
    const OccupiedSlotEntry* m2aS0 = m2aMap.findOccupied({false, 1, 0});
    TEST("M2b SoundEquivalenceProfile digest matches M2a inspectTargetKstInput() byte-for-byte",
         m2aS0 != nullptr && !m2aS0->equivalence.equivalenceDigest.empty() &&
             view.sounds.at(s0Id).equivalence.equivalenceDigest == m2aS0->equivalence.equivalenceDigest);

    size_t unrefCount = 0;
    for (const auto& [nid, _] : view.graph->nodes()) {
        if (view.graph->deleteCheck(nid).verdict == DeleteVerdict::Unreferenced) ++unrefCount;
    }
    TEST("Native-KST deleteCheck() NEVER Unreferenced", unrefCount == 0);

    CollectorRequest req;
    req.mode = ExportMode::AdvisoryPlan;
    for (int i = 1; i <= 5; ++i) req.targets.push_back(selStyle("STYLE/BankA/Style0" + std::to_string(i) + ".stg"));
    const DryRunExportPlan plan = collectSelectiveExport(view, req);

    uint64_t perStyleRaw = 0;
    for (int s = 0; s <= 5; ++s) perStyleRaw += sampleSizes[size_t(s)];
    for (int s = 4; s <= 9; ++s) perStyleRaw += sampleSizes[size_t(s)];
    for (int s = 0; s <= 9; ++s) perStyleRaw += sampleSizes[size_t(s)];
    const uint64_t expectedRawBytes = 5u * perStyleRaw;
    const uint64_t expectedSavedBytes = expectedRawBytes - expectedUniqueSampleBytes;

    TEST("5-Style plan ok & executable in AdvisoryPlan", plan.ok && plan.executable && plan.blockingDiagnosticCount() == 0);
    TEST("5-Style exact unique counts (3 Sounds, 2 MS, 10 Samples, 2 PCMs)",
         plan.collectedSounds.size() == 3 && plan.uniqueMultisamples.size() == 2 &&
             plan.uniqueSamples.size() == 10 && plan.uniquePcmFiles.size() == 2);
    TEST("5-Style factory selects counted", plan.factorySoundSelectCount == 5);
    TEST("5-Style exact raw/unique/saved bytes",
         plan.memoryProjection.rawReferencedSampleBytes == expectedRawBytes &&
             plan.memoryProjection.uniqueLogicalSampleBytes == expectedUniqueSampleBytes &&
             plan.memoryProjection.logicalDedupSavedBytes == expectedSavedBytes);
    TEST("5-Style native proxy caveat enforced",
         plan.memoryProjection.sampleBytesAreNativeStoredProxy &&
             std::string(plan.memoryProjection.unitName()) == "native_kst_stored_pcm_proxy_bytes" &&
             std::string(plan.memoryProjection.fitVerdictName()) == "unverified-proxy" &&
             !plan.hardwareVerified &&
             plan.memoryCaveat.find("Stored PCM size") != std::string::npos);

    size_t tier1Sound = 0, tier1Ms = 0, tier1Smp = 0;
    for (const auto& dg : plan.dedupGroups) {
        if (dg.tier == DedupTier::Tier1LogicalIdentity && dg.appliedInExport) {
            if (dg.resourceKind == "Sound") ++tier1Sound;
            if (dg.resourceKind == "Multisample" && dg.keyType == "resource-id+kmp-id8") ++tier1Ms;
            if (dg.resourceKind == "Sample" && dg.keyType == "resource-id+kmp-id8") ++tier1Smp;
        }
    }
    TEST("5-Style Tier-1 dedup groups (3 Sounds, 2 MS, 10 Samples)", tier1Sound == 3 && tier1Ms == 2 && tier1Smp == 10);

    // Bridge to M2a SelectiveExportReport -> serializeSelectiveReportJson parses cleanly!
    const SelectiveExportReport m2aRep = toSelectiveExportReport(plan, "SyntheticSource");
    const std::string m2aJson = serializeSelectiveReportJson(m2aRep);
    kit::Json parsedM2a;
    TEST("toSelectiveExportReport() serializes valid M2a report JSON",
         kit::parseJson(m2aJson, &parsedM2a).empty() && m2aJson.find(kSelectiveReportSchema) != std::string::npos);

    // Toggle embedded KBD Set & Pad flags on Style01
    CollectorRequest reqNoKbd;
    reqNoKbd.targets = {selStyle("STYLE/BankA/Style01.stg")};
    reqNoKbd.includeEmbeddedKbdSets = false;
    reqNoKbd.includeEmbeddedPads = true;
    const DryRunExportPlan planNoKbd = collectSelectiveExport(view, reqNoKbd);
    TEST("includeEmbeddedKbdSets=false excludes USER01/1, keeps main+pad",
         planNoKbd.collectedSounds.size() == 2 &&
             planNoKbd.collectedSounds[0].id.number == 0 &&
             planNoKbd.collectedSounds[1].id.number == 2);

    CollectorRequest reqNoPad;
    reqNoPad.targets = {selStyle("STYLE/BankA/Style01.stg")};
    reqNoPad.includeEmbeddedKbdSets = true;
    reqNoPad.includeEmbeddedPads = false;
    const DryRunExportPlan planNoPad = collectSelectiveExport(view, reqNoPad);
    TEST("includeEmbeddedPads=false excludes USER01/2, keeps main+kbd",
         planNoPad.collectedSounds.size() == 2 &&
             planNoPad.collectedSounds[0].id.number == 0 &&
             planNoPad.collectedSounds[1].id.number == 1);

    CollectorRequest reqMainOnly;
    reqMainOnly.targets = {selStyle("STYLE/BankA/Style01.stg")};
    reqMainOnly.includeEmbeddedKbdSets = false;
    reqMainOnly.includeEmbeddedPads = false;
    const DryRunExportPlan planMainOnly = collectSelectiveExport(view, reqMainOnly);
    TEST("both embedded flags false keeps main track USER01/0 only",
         planMainOnly.collectedSounds.size() == 1 && planMainOnly.collectedSounds[0].id.number == 0);
}

// ── Test 2: Provisional Package & WAV contentHash Tier-2 Dedup + Uncompressed Memory Projection ───────────────────────

static void testProvisionalTier2DedupAndMemoryProjection() {
    std::printf("[Test 2] Provisional package & WAV contentHash Tier-2 dedup + uncompressed memory projection\n");

    auto makeSample = [](const std::string& name, uint32_t frames, int16_t seed) {
        sound::Sample s;
        s.name = name;
        s.channels = 1;
        s.rate = 48000;
        s.pcm.resize(frames);
        for (uint32_t i = 0; i < frames; ++i) s.pcm[i] = static_cast<int16_t>(seed + static_cast<int16_t>(i * 7));
        return s;
    };

    sound::Instrument instA;
    instA.name = "SynthAlpha";
    instA.samples.push_back(makeSample("ToneShared", 1000, 42));
    instA.samples.push_back(makeSample("CommonLabel", 500, 11));
    sound::Layer layA;
    layA.name = "Main";
    layA.velLo = 0; layA.velHi = 127;
    layA.zones.push_back(sound::Zone{0, 60, 0, 60, 0, 0});
    layA.zones.push_back(sound::Zone{61, 127, 1, 72, 0, 0});
    instA.layers.push_back(std::move(layA));

    sound::Instrument instB;
    instB.name = "SynthBeta";
    instB.samples.push_back(makeSample("RenamedClone", 1000, 42));
    instB.samples.push_back(makeSample("CommonLabel", 500, 99));
    sound::Layer layB;
    layB.name = "Main";
    layB.velLo = 0; layB.velHi = 127;
    layB.zones.push_back(sound::Zone{0, 60, 0, 60, 0, 0});
    layB.zones.push_back(sound::Zone{61, 127, 1, 72, 0, 0});
    instB.layers.push_back(std::move(layB));

    Graph gA, gB;
    std::string err;
    const std::string pkgAId = "SynthAlpha#aaaaaaaa1111";
    const std::string pkgBId = "SynthBeta#bbbbbbbb2222";
    TEST("addInstrument PkgAlpha", sound::addInstrument(gA, instA, pkgAId, &err));
    TEST("addInstrument PkgBeta", sound::addInstrument(gB, instB, pkgBId, &err));

    ProvisionalWorkspaceInput ws;
    ws.packages.push_back({"SOUNDS/SynthAlpha", sound::writeManifest(gA, pkgAId, Kind::Sound), "", std::nullopt, "", Kind::Sound, ""});
    ws.packages.push_back({"SOUNDS/SynthBeta",  sound::writeManifest(gB, pkgBId, Kind::Sound), "", std::nullopt, "", Kind::Sound, ""});

    const std::string smfA = makeSmf({{0, 121, 64, 0}, {1, 121, 0, 1}});
    const std::string smfB = makeSmf({{0, 121, 64, 0}, {2, 121, 64, 1}});

    ProvisionalRootInput r1;
    r1.relPath = "STYLES/BalladOne.mid";
    r1.kind = RootItemKind::ProvisionalMidi;
    r1.smfBytes.assign(smfA.begin(), smfA.end());
    r1.addressToPackageId[{121, 64, 0}] = pkgAId;

    ProvisionalRootInput r2;
    r2.relPath = "STYLES/BalladTwo.mid";
    r2.kind = RootItemKind::ProvisionalMidi;
    r2.smfBytes.assign(smfB.begin(), smfB.end());
    r2.addressToPackageId[{121, 64, 0}] = pkgAId;
    r2.addressToPackageId[{121, 64, 1}] = pkgBId;

    ws.roots.push_back(r1);
    ws.roots.push_back(r2);

    const M1AdapterView view = adaptProvisionalWorkspace(ws);
    TEST("provisional view ok", view.ok && view.isProvisional() && view.packagesById.size() == 2);

    CollectorRequest req;
    req.mode = ExportMode::ImportKitBundle;
    req.targets = {selStyle("STYLES/BalladOne.mid"), selStyle("STYLES/BalladTwo.mid")};
    const DryRunExportPlan plan = collectSelectiveExport(view, req);

    TEST("provisional plan executable", plan.ok && plan.executable && plan.coverageComplete);
    TEST("PkgAlpha included once in uniqueProvisionalPackages",
         plan.uniqueProvisionalPackages.size() == 2 &&
             plan.uniqueProvisionalPackages[0] == pkgAId &&
             plan.uniqueProvisionalPackages[1] == pkgBId);
    TEST("exact uncompressed memory math",
         std::string(plan.memoryProjection.unitName()) == "uncompressed_pcm_bytes" &&
             !plan.memoryProjection.sampleBytesAreNativeStoredProxy &&
             std::string(plan.memoryProjection.fitVerdictName()) == "fits" &&
             !plan.hardwareVerified &&
             plan.memoryProjection.rawReferencedSampleBytes == 9000 &&
             plan.memoryProjection.uniqueLogicalSampleBytes == 6000 &&
             plan.memoryProjection.logicalDedupSavedBytes == 3000 &&
             plan.memoryProjection.identicalContentExtraBytes == 2000);

    bool foundTier2aPkg = false, foundTier2bWav = false;
    size_t tier2bCount = 0;
    for (const auto& dg : plan.dedupGroups) {
        if (dg.tier == DedupTier::Tier2ProvisionalContent && dg.keyType == "provisional-package-id" &&
            dg.appliedInExport && dg.savedBytes == 3000) {
            foundTier2aPkg = true;
        }
        if (dg.tier == DedupTier::Tier2ProvisionalContent && dg.keyType == "audio-fnv1a64+bytes") {
            ++tier2bCount;
            if (!dg.appliedInExport && dg.savedBytes == 2000 && dg.members.size() == 2) foundTier2bWav = true;
        }
    }
    TEST("Tier-2a provisional package dedup appliedInExport=true", foundTier2aPkg);
    TEST("Tier-2b byte-identical WAV advisory only (same-name/diff-content NOT grouped)",
         foundTier2bWav && tier2bCount == 1);

    // Malformed provisional SMF fails closed with MALFORMED_MIDI_LEAF
    {
        ProvisionalWorkspaceInput badWs = ws;
        const std::string badSmf = makeSmf({{0, 121, 0, 0}}, true);
        ProvisionalRootInput badRoot;
        badRoot.relPath = "STYLES/BadSmf.mid";
        badRoot.kind = RootItemKind::ProvisionalMidi;
        badRoot.smfBytes.assign(badSmf.begin(), badSmf.end());
        badWs.roots.push_back(badRoot);
        const M1AdapterView badView = adaptProvisionalWorkspace(badWs);
        CollectorRequest badReq;
        badReq.targets = {selStyle("STYLES/BadSmf.mid")};
        const DryRunExportPlan badPlan = collectSelectiveExport(badView, badReq);
        TEST("malformed provisional SMF fails closed with MALFORMED_MIDI_LEAF",
             !badPlan.executable && badPlan.hasBlockingCode(diag_code::kMalformedMidiLeaf));
    }

    // Provisional source + Native KST target baseline switches to native_kst_stored_pcm_proxy_bytes / unverified-proxy
    {
        TargetOccupancyMap emptyTarget = TargetOccupancyMap::makeEmpty(true);
        const DryRunExportPlan mixedPlan = collectSelectiveExportWithTargetMap(view, req, &emptyTarget, 100000, true);
        TEST("provisional + Native-KST target baseline switches to unverified-proxy caveat",
             mixedPlan.memoryProjection.sampleBytesAreNativeStoredProxy &&
                 std::string(mixedPlan.memoryProjection.unitName()) == "native_kst_stored_pcm_proxy_bytes" &&
                 std::string(mixedPlan.memoryProjection.fitVerdictName()) == "unverified-proxy" &&
                 mixedPlan.memoryProjection.targetBaselineSampleBytes == 100000 &&
                 mixedPlan.memoryProjection.projectedCombinedBytes == 106000);
    }
}

// ── Test 3: Tier-3 Native KST Equivalence (via M2a compareSoundEquivalence) & HOLD Gates ─────────────────────────────

static void testTier3NativeEquivalenceAndHoldGates() {
    std::printf("[Test 3] Tier-3 Native KST 3-way equivalence (M2a single source of truth) & HOLD gates\n");

    const RecordLayout L{12, 24, 8};
    const uint32_t kKind = 0x10050038u;
    const Id8 msIdA = makeId(0xC1, 1);
    const Id8 msIdB = makeId(0xC1, 2);
    const Id8 smpIdA = makeId(0xD1, 1);
    const Id8 smpIdB = makeId(0xD1, 2);
    const Id8 unknownSlotId = makeId(0xEE, 9);

    auto makeKst = [&](uint32_t smpABytes, uint8_t s1Fill, bool includeUnanalysedDk, bool addUnresolvedToS2, uint8_t zone0TopKey = 60) {
        SynthKmpSpec kmp;
        kmp.samples.push_back({smpIdA, "SmpA", 48000});
        kmp.samples.push_back({smpIdB, "SmpB", 48000});
        kmp.zones.push_back({0, zone0TopKey});
        kmp.zones.push_back({1, 127});
        kmp.multis.push_back({msIdA, "MsA", 0, 1});
        kmp.multis.push_back({msIdB, "MsB", 1, 1});

        const Bytes s0Body = buildSoundBody(L, {{msIdA, kZeroId}}, 0x5Au);
        const Bytes s1Body = buildSoundBody(L, {{msIdB, kZeroId}}, s1Fill);
        const Bytes s2Body = buildSoundBody(L, {{msIdA, addUnresolvedToS2 ? unknownSlotId : kZeroId}}, 0x5Au);
        const Bytes noRefBody = buildSoundBody(L, {{kZeroId, kZeroId}}, 0x33u);

        std::vector<SynthKorfItem> user01{
            {kKind, 0, 0, 0, "SharedPiano",  makeOcLiteralFrame("OC31", s0Body), ""},
            {kKind, 0, 0, 1, "SameNameLead", makeOcLiteralFrame("OC31", s1Body), ""},
            {kKind, 0, 0, 2, "UnresLead",    makeOcLiteralFrame("OC31", s2Body), ""},
            {kKind, 0, 0, 3, "RomOnlyPad",   makeOcLiteralFrame("OC31", noRefBody), ""},
        };

        NativeKstClosureInput in;
        in.downstream.kmpContainer = RawNamedBlob{"MULTISMP/RAM.KMP", "", buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM", buildPa5xKmpPayload(kmp), ""}})};
        in.downstream.pcmContainers.push_back({"PCM/ram01.pcm", "", buildKorfContainer({
            {0x01010018u, 0, 0, 0, "SmpA", Bytes(smpABytes, 0x11), idAsStr(smpIdA)},
            {0x01010018u, 0, 0, 1, "SmpB", Bytes(4000, 0x22), idAsStr(smpIdB)}
        })});
        in.downstream.soundContainers.push_back({"SOUND/USER01.PCG", "USER01", buildKorfContainer(user01)});
        if (includeUnanalysedDk) {
            Bytes dkBody(19, 0x77u);
            in.downstream.soundContainers.push_back({"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kKind, 0, 0, 0, "KitOne", makeOcLiteralFrame("OC31", dkBody), ""}
            })});
        }
        in.upstreamRoots.push_back({"STYLE/BankA/FactoryOnly.stg", rtSerialize({rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 0, 5}}))})})})});
        in.upstreamRoots.push_back({"STYLE/BankA/TargetMatched.stg", rtSerialize({rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 64, 0}}))})})})});
        in.upstreamRoots.push_back({"STYLE/BankA/SameNameDiff.stg", rtSerialize({rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 64, 1}}))})})})});
        return in;
    };

    M1AdapterOptions aopts;
    aopts.closureOptions.downstream.layouts[kKind] = L;
    aopts.closureOptions.downstream.allowFitLayout = false;

    const M1AdapterView srcView = adaptNativeKstInput(makeKst(3000, 0x5Au, true, true, 60), aopts);
    const M1AdapterView dstViewSame = adaptNativeKstInput(makeKst(3000, 0x6Bu, true, true, 60), aopts);
    const M1AdapterView dstViewDiffPcmSize = adaptNativeKstInput(makeKst(3200, 0x5Au, true, false, 60), aopts);
    const M1AdapterView dstViewDiffTopKey = adaptNativeKstInput(makeKst(3000, 0x5Au, true, false, 72), aopts);

    const ResourceId s0{false, "USER01", Kind::Sound, 0};
    const ResourceId s1{false, "USER01", Kind::Sound, 1};
    const ResourceId s2{false, "USER01", Kind::Sound, 2};
    const ResourceId s3{false, "USER01", Kind::Sound, 3};
    const ResourceId dk0{false, "USERDK01", Kind::Sound, 0};

    const auto eqIdentical = classifyNativeSoundEquivalence(srcView, s0, dstViewSame, s0);
    const auto eqDiffBody = classifyNativeSoundEquivalence(srcView, s1, dstViewSame, s1);
    const auto eqDiffPcm = classifyNativeSoundEquivalence(srcView, s0, dstViewDiffPcmSize, s0);
    const auto eqDiffTopKey = classifyNativeSoundEquivalence(srcView, s0, dstViewDiffTopKey, s0);
    const auto eqUnresSlot = classifyNativeSoundEquivalence(srcView, s2, dstViewSame, s2);
    const auto eqNoRef = classifyNativeSoundEquivalence(srcView, s3, dstViewSame, s3);
    const auto eqDk = classifyNativeSoundEquivalence(srcView, dk0, dstViewSame, dk0);

    TEST("Tier-3 Identical proven via M2a compareSoundEquivalence (SHA-256 + Analysed + Id8/topKey/size)",
         eqIdentical.verdict == EquivalenceVerdict::Identical && !eqIdentical.mergesPa5xId8);
    TEST("Tier-3 same-name different-body -> Different (NEVER merged)",
         eqDiffBody.verdict == EquivalenceVerdict::Different);
    TEST("Tier-3 same-body different-PCM-size -> Different",
         eqDiffPcm.verdict == EquivalenceVerdict::Different);
    TEST("Tier-3 same-body different-zone-topKey -> Different (M2a zone fidelity!)",
         eqDiffTopKey.verdict == EquivalenceVerdict::Different);
    TEST("Tier-3 unresolved slot in analysed Sound -> InsufficientEvidence",
         eqUnresSlot.verdict == EquivalenceVerdict::InsufficientEvidence);
    TEST("Tier-3 NoKnownReference Sound -> InsufficientEvidence",
         eqNoRef.verdict == EquivalenceVerdict::InsufficientEvidence);
    TEST("Tier-3 unanalysed USERDK -> InsufficientEvidence",
         eqDk.verdict == EquivalenceVerdict::InsufficientEvidence);

    {
        CollectorRequest req;
        req.mode = ExportMode::RootTreeSubKst;
        req.targets = {selStyle("STYLE/BankA/FactoryOnly.stg")};
        const DryRunExportPlan p = collectSelectiveExport(srcView, req, &dstViewSame);
        TEST("RootTreeSubKst FactoryOnly executable (already-present-noop non-blocking)", p.ok && p.executable);
    }
    {
        CollectorRequest req;
        req.mode = ExportMode::RootTreeSubKst;
        req.targets = {selStyle("STYLE/BankA/TargetMatched.stg")};
        const DryRunExportPlan p = collectSelectiveExport(srcView, req, &dstViewSame);
        TEST("RootTreeSubKst TargetMatched executable (reuses exact-slot identical target)",
             p.ok && p.executable && p.collectedSounds.size() == 1 &&
                 p.collectedSounds[0].satisfiedByTarget && !p.collectedSounds[0].requiresRemap);
    }
    {
        CollectorRequest req;
        req.mode = ExportMode::RootTreeSubKst;
        req.targets = {selStyle("STYLE/BankA/SameNameDiff.stg")};
        const DryRunExportPlan p = collectSelectiveExport(srcView, req, &dstViewSame);
        TEST("RootTreeSubKst SameNameDiff blocked with NATIVE_SOUND_WRITE_HOLD & M2a SoundSlotOccupiedDifferent",
             p.ok && !p.executable && p.hasBlockingCode(diag_code::kNativeSoundWriteHold) &&
                 p.hasBlockingCode(diag_code::kSoundSlotOccupiedDifferent) &&
                 p.blockingConflictCount() >= 1);
    }
    {
        CollectorRequest req;
        req.mode = ExportMode::NativeSoundKst_HOLD;
        req.targets = {selStyle("STYLE/BankA/FactoryOnly.stg")};
        const DryRunExportPlan p = collectSelectiveExport(srcView, req);
        TEST("NativeSoundKst_HOLD ALWAYS fails closed",
             p.ok && !p.executable && p.hasBlockingCode(diag_code::kNativeSoundWriteHold));
    }
}

// ── Test 4: Fail-Closed Blocking (Missing, OC31 KMP, Unanalysed USERDK, Malformed SMF, UnresolvedSlot Split) ─────────

static void testFailClosedBlockingAndBoundaries() {
    std::printf("[Test 4] Fail-closed blocking (missing, OC31 KMP, USERDK, malformed SMF, out-of-spec, UnresolvedSlot)\n");

    const RecordLayout L{12, 24, 8};
    const uint32_t kKind = 0x10050038u;
    const Id8 msId = makeId(0xE1, 0);
    const Id8 smpGood = makeId(0xF1, 0);
    const Id8 smpMissingPcm = makeId(0xF1, 1);
    const Id8 dupDrumId = makeId(0xF9, 7);
    const Id8 badSlotId = makeId(0xEE, 1);

    SynthKmpSpec kmp;
    kmp.samples.push_back({smpGood, "SmpGood", 48000});
    kmp.samples.push_back({smpMissingPcm, "SmpNoPcm", 48000});
    kmp.drums.push_back({dupDrumId, "Dr1", 0, kNoZoneSample});
    kmp.drums.push_back({dupDrumId, "Dr2", 0, kNoZoneSample});
    kmp.zones.push_back({0, 60});
    kmp.zones.push_back({1, 127});
    kmp.multis.push_back({msId, "MsMixed", 0, 2});

    const Bytes s0Body = buildSoundBody(L, {{msId, badSlotId}});
    const Bytes s1Body = buildSoundBody(L, {{badSlotId, kZeroId}});
    const Bytes dkBody(19, 0x44u);

    NativeKstClosureInput in;
    in.downstream.kmpContainer = RawNamedBlob{"MULTISMP/RAM.KMP", "", buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM", buildPa5xKmpPayload(kmp), ""}})};
    in.downstream.pcmContainers.push_back({"PCM/ram01.pcm", "", buildKorfContainer({
        {0x01010018u, 0, 0, 0, "SmpGood", Bytes(2048, 0x33), idAsStr(smpGood)}
    })});
    in.downstream.soundContainers.push_back({"SOUND/USER01.PCG", "USER01", buildKorfContainer({
        {kKind, 0, 0, 0, "AnalysedWithUnres", makeOcLiteralFrame("OC31", s0Body), ""},
        {kKind, 0, 0, 1, "NoRefSpeculative",  makeOcLiteralFrame("OC31", s1Body), ""}
    })});
    in.downstream.soundContainers.push_back({"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
        {kKind, 0, 0, 0, "UnparsedKit", makeOcLiteralFrame("OC31", dkBody), ""}
    })});

    in.upstreamRoots.push_back({"STYLE/BankA/BrokenRefs.stg", rtSerialize({
        rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({
            {0, 121, 64, 99},
            {1, 121, 68, 0},
            {2, 120, 72, 0},
            {3, 99, 5, 0},
            {4, -1, -1, 7}
        }))})})
    })});
    in.upstreamRoots.push_back({"PAD/BankA/BadPad.pdc", rtSerialize({rtLeaf("Main.mid", makeSmf({{0, 121, 0, 0}}, true))})});
    in.upstreamRoots.push_back({"STYLE/BankA/DkAndUnres.stg", rtSerialize({
        rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({
            {0, 120, 64, 0},
            {1, 121, 64, 0},
            {2, 121, 64, 1}
        }))})})
    })});

    M1AdapterOptions aopts;
    aopts.closureOptions.downstream.layouts[kKind] = L;
    aopts.closureOptions.downstream.allowFitLayout = false;
    const M1AdapterView view = adaptNativeKstInput(in, aopts);

    const RootItemRecord* br = view.findRoot("STYLE/BankA/BrokenRefs.stg");
    TEST("BrokenRefs root adapted", br != nullptr && br->soundSelections.size() == 5);
    bool sawOutOfSpec = false, sawUnknown = false, sawBankMissing = false;
    for (const auto& s : br->soundSelections) {
        if (s.address.msb == 120 && s.address.lsb == 72 && s.boundary == SoundBoundaryKind::AmbiguousOrOutOfSpec) sawOutOfSpec = true;
        if (s.address.msb == 99 && s.boundary == SoundBoundaryKind::UnknownOrUnanalysed && !s.bankMissing) sawUnknown = true;
        if (s.address.program == 7 && s.boundary == SoundBoundaryKind::UnknownOrUnanalysed && s.bankMissing) sawBankMissing = true;
    }
    TEST("strict boundary classification (OutOfSpec & Unknown NEVER coerced to Factory)",
         sawOutOfSpec && sawUnknown && sawBankMissing);

    {
        CollectorRequest req;
        req.targets = {selStyle("STYLE/BankA/BrokenRefs.stg"), SelectionTarget{SelectedKind::Pad, "PAD/BankA/BadPad.pdc", "", ""}};
        const DryRunExportPlan p = collectSelectiveExport(view, req);
        TEST("BrokenRefs + BadPad fail closed with exact M2a diagnostic codes",
             !p.executable &&
                 p.hasBlockingCode(diag_code::kMissingUserSoundSlot) &&
                 p.hasBlockingCode(diag_code::kMissingUserSoundBank) &&
                 p.hasBlockingCode(diag_code::kUnresolvedMidiBankSelect) &&
                 p.hasBlockingCode(collector_diag_code::kUnknownOriginMidiAddress) &&
                 p.hasBlockingCode(diag_code::kMalformedMidiLeaf) &&
                 p.factorySoundSelectCount == 0);
    }

    {
        CollectorRequest req;
        req.targets = {selStyle("STYLE/BankA/DkAndUnres.stg")};
        req.requireCompleteSampleClosure = true;
        const DryRunExportPlan p = collectSelectiveExport(view, req);
        TEST("DkAndUnres blocks on USERDK & missing PCM, separates UnresolvedSlot",
             !p.executable && !p.coverageComplete &&
                 p.hasBlockingCode(diag_code::kDrumkitBodyUnparsed) &&
                 p.hasBlockingCode(diag_code::kMissingPcmForKmpSample) &&
                 p.memoryProjection.unknownSizeSamplesCount == 1 &&
                 p.unresolvedSlotsInAnalysedCount == 1 &&
                 p.unresolvedSlotsInUnanalysedCount == 1 &&
                 p.hasDiagnosticCode(diag_code::kUnresolvedSoundSlotAnalysed, DiagnosticSeverity::Warning) &&
                 p.hasDiagnosticCode(diag_code::kUnresolvedSoundSlotSpeculative, DiagnosticSeverity::Info) &&
                 p.hasDiagnosticCode(diag_code::kSoundNoRamRefInferredRom, DiagnosticSeverity::Info) &&
                 p.hasDiagnosticCode(diag_code::kDuplicateKmpDrumId, DiagnosticSeverity::Warning));
    }

    // OC31-encoded RAM.KMP -> OC31_KMP_UNSUPPORTED (and as target -> TargetUninspectable!)
    {
        NativeKstClosureInput ocKmpIn;
        const Bytes fakeOc31 = makeOcLiteralFrame("OC31", Bytes(32, 0x22));
        ocKmpIn.downstream.kmpContainer = RawNamedBlob{"MULTISMP/RAM.KMP", "", buildKorfContainer({{0x11050038u, 0, 0, 0, "RAM", fakeOc31, ""}})};
        ocKmpIn.downstream.soundContainers.push_back({"SOUND/USER01.PCG", "USER01", buildKorfContainer({
            {kKind, 0, 0, 0, "LegacySound", makeOcLiteralFrame("OC31", Bytes(36, 0x11)), ""}
        })});
        ocKmpIn.upstreamRoots.push_back({"STYLE/BankA/LegacyStyle.stg", rtSerialize({
            rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 64, 0}}))})})
        })});
        const M1AdapterView ocView = adaptNativeKstInput(ocKmpIn);
        TEST("OC31 KMP detected cleanly while Sound catalog & PartialNativeInspect stay populated",
             ocView.ok && ocView.coverage.kmpIsOc31 && ocView.partialInspect.isPartial && ocView.sounds.size() == 1);

        CollectorRequest req;
        req.targets = {selStyle("STYLE/BankA/LegacyStyle.stg")};
        req.requireCompleteSampleClosure = true;
        const DryRunExportPlan p = collectSelectiveExport(ocView, req);
        TEST("OC31 KMP fails closed with OC31_KMP_UNSUPPORTED",
             !p.executable && !p.coverageComplete && p.hasBlockingCode(diag_code::kOc31KmpUnsupported));

        // Using OC31 KST as optionalTarget -> M2a CollisionMatrix blocks with TargetUninspectable!
        CollectorRequest reqVsOcTarget;
        reqVsOcTarget.targets = {selStyle("STYLE/BankA/DkAndUnres.stg")};
        reqVsOcTarget.requireCompleteSampleClosure = false;
        const DryRunExportPlan pVsOc = collectSelectiveExport(view, reqVsOcTarget, &ocView);
        TEST("OC31 KST used as optionalTarget fails closed via M2a TargetUninspectable",
             !pVsOc.executable && pVsOc.hasBlockingCode(diag_code::kTargetUninspectable) &&
                 pVsOc.blockingConflictCount() >= 1);
    }
}

// ── Test 5: Cooperative Mid-Scan Cancellation Drops Partial State Cleanly ────────────────────────────────────────────

static void testCooperativeCancellation() {
    std::printf("[Test 5] Cooperative cancellation (keepGoing checked <= 256 items, drops partial state)\n");

    const RecordLayout L{12, 24, 8};
    const uint32_t kKind = 0x10050038u;
    SynthKmpSpec kmp;
    kmp.samples.push_back({makeId(0x11, 0), "S0", 48000});
    kmp.zones.push_back({0, 127});
    kmp.multis.push_back({makeId(0x22, 0), "M0", 0, 1});

    NativeKstClosureInput in;
    in.downstream.kmpContainer = RawNamedBlob{"MULTISMP/RAM.KMP", "", buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM", buildPa5xKmpPayload(kmp), ""}})};
    in.downstream.pcmContainers.push_back({"PCM/ram01.pcm", "", buildKorfContainer({
        {0x01010018u, 0, 0, 0, "S0", Bytes(1024, 0x05), idAsStr(makeId(0x11, 0))}
    })});
    in.downstream.soundContainers.push_back({"SOUND/USER01.PCG", "USER01", buildKorfContainer({
        {kKind, 0, 0, 0, "Snd0", makeOcLiteralFrame("OC31", buildSoundBody(L, {{makeId(0x22, 0), kZeroId}})), ""}
    })});
    for (int i = 0; i < 300; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "STYLE/BankA/S%03d.stg", i);
        in.upstreamRoots.push_back({buf, rtSerialize({rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 64, 0}}))})})})});
    }

    M1AdapterOptions aopts;
    aopts.closureOptions.downstream.layouts[kKind] = L;
    aopts.closureOptions.downstream.allowFitLayout = false;
    aopts.cancelCheckStride = 16;

    size_t adaptCalls = 0;
    const M1AdapterView cancelledView = adaptNativeKstInput(in, aopts, [&]() { return ++adaptCalls <= 3; });
    TEST("mid-scan adapt cancelled cleanly with empty partial state",
         cancelledView.cancelled && !cancelledView.ok && cancelledView.roots.empty() &&
             cancelledView.sounds.empty() && cancelledView.samples.empty());

    const M1AdapterView fullView = adaptNativeKstInput(in, aopts);
    CollectorRequest req;
    req.cancelCheckStride = 9999; // clamped to <= 256
    for (const auto& r : fullView.roots) req.targets.push_back(selStyle(r.relPath));

    size_t collectCalls = 0;
    const DryRunExportPlan cancelledPlan = collectSelectiveExport(fullView, req, nullptr, [&]() { return ++collectCalls <= 2; });
    TEST("mid-scan collect cancelled (clamped <= 256 stride) with empty partial state",
         cancelledPlan.cancelled && !cancelledPlan.ok && !cancelledPlan.executable &&
             cancelledPlan.closureNodes.empty() && cancelledPlan.dedupGroups.empty() &&
             cancelledPlan.collectedSounds.empty() && collectCalls == 3);
}

// ── Test 6: Byte-Identical Reproducibility, Shuffled Selection Order, RFC 8259 JSON & Zero Local Paths ───────────────

static void testReproducibilityJsonAndPathFree() {
    std::printf("[Test 6] Reproducibility across shuffled selections, RFC 8259 JSON & zero local paths\n");

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path tmpRoot = fs::temp_directory_path() / ("aiarr_m2b_path_probe_" + std::to_string(nonce));
    const fs::path kstDir = tmpRoot / "SyntheticProbe.KST";

    const RecordLayout L{12, 24, 8};
    const uint32_t kKind = 0x10050038u;
    const Id8 msId = makeId(0x55, 0);
    const Id8 smpId = makeId(0x66, 0);
    SynthKmpSpec kmp;
    kmp.samples.push_back({smpId, "ProbeSmp", 48000});
    kmp.zones.push_back({0, 127});
    kmp.multis.push_back({msId, "ProbeMs", 0, 1});

    writeFileBytes(kstDir / "MULTISMP/RAM.KMP", buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM", buildPa5xKmpPayload(kmp), ""}}));
    writeFileBytes(kstDir / "PCM/ram01.pcm", buildKorfContainer({{0x01010018u, 0, 0, 0, "ProbeSmp", Bytes(4096, 0x2A), idAsStr(smpId)}}));
    writeFileBytes(kstDir / "SOUND/USER01.PCG", buildKorfContainer({
        {kKind, 0, 0, 0, "Probe\"Snd\xE9", makeOcLiteralFrame("OC31", buildSoundBody(L, {{msId, kZeroId}})), ""}
    }));

    const std::vector<std::string> rels = {"STYLE/BankZ/Zeta.stg", "STYLE/BankA/Alpha.stg", "PAD/BankM/MidPad.pdc"};
    for (const std::string& rel : rels) {
        writeFileBytes(kstDir / rel, rtSerialize({rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 64, 0}}))})})}));
    }

    M1AdapterOptions aopts;
    aopts.closureOptions.downstream.layouts[kKind] = L;
    aopts.closureOptions.downstream.allowFitLayout = false;
    aopts.computeAllRootFileHashes = true;

    const M1AdapterView view1 = adaptNativeKstFolder(kstDir, aopts);
    const M1AdapterView view2 = adaptNativeKstFolder(kstDir, aopts);

    CollectorRequest reqA;
    reqA.targets = {selStyle("STYLE/BankZ/Zeta.stg"), selStyle("STYLE/BankA/Alpha.stg"), SelectionTarget{SelectedKind::Pad, "PAD/BankM/MidPad.pdc", "", ""}};
    CollectorRequest reqB;
    reqB.targets = {SelectionTarget{SelectedKind::Pad, "PAD/BankM/MidPad.pdc", "", ""}, selStyle("STYLE/BankZ/Zeta.stg"), selStyle("STYLE/BankA/Alpha.stg")};

    const DryRunExportPlan plan1 = collectSelectiveExport(view1, reqA);
    const DryRunExportPlan plan2 = collectSelectiveExport(view2, reqB);

    const std::string json1 = serializeDryRunPlanJson(plan1);
    const std::string json2 = serializeDryRunPlanJson(plan2);
    const std::string md1 = serializeDryRunPlanMarkdown(plan1);
    const std::string md2 = serializeDryRunPlanMarkdown(plan2);

    TEST("planFingerprint 64-hex & byte-identical across shuffled selection",
         plan1.planFingerprint.size() == 64 && plan1.planFingerprint == plan2.planFingerprint);
    TEST("JSON dry-run preview byte-identical across repeated runs & shuffled selection", json1 == json2);
    TEST("Markdown dry-run preview byte-identical across repeated runs & shuffled selection", md1 == md2);

    kit::Json parsed;
    const std::string jerr = kit::parseJson(json1, &parsed);
    TEST("JSON dry-run preview parses cleanly via jobs::kit::parseJson()", jerr.empty());

    const std::string probeToken = "aiarr_m2b_path_probe_";
    const std::string tmpStr = tmpRoot.string();
    TEST("zero local paths in JSON & Markdown previews",
         json1.find(probeToken) == std::string::npos &&
             json1.find(tmpStr) == std::string::npos &&
             json1.find("/Users/") == std::string::npos &&
             md1.find(probeToken) == std::string::npos &&
             md1.find(tmpStr) == std::string::npos &&
             md1.find("/Users/") == std::string::npos);
    TEST("mandatory STRUCTURALLY VALID banner present in JSON & Markdown",
         json1.find(kHardwareUnverifiedBanner) != std::string::npos &&
             md1.find(kHardwareUnverifiedBanner) != std::string::npos);

    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
}

// ── Test 7: Audit Hardening & M2a Collision Integration Regressions (P1, P2-1/2/3, P3-2/3, FAT R-18) ──────────────────

static void testAuditHardeningAndM2aIntegration() {
    std::printf("[Test 7] Audit hardening & M2a integration (P1 cross-slot, P2-1/2/3, P3-2/3, FAT R-18)\n");

    const RecordLayout L{12, 24, 8};
    const uint32_t kKind = 0x10050038u;
    const Id8 msId = makeId(0x71, 1);
    const Id8 smpId = makeId(0x72, 1);

    auto makeSingleSoundKst = [&](const std::vector<std::pair<std::string, uint8_t>>& targetSlots,
                                  const std::string& styleRel, int styleMsb, int styleLsb, uint8_t stylePc,
                                  const std::string& extraObjectRel = "") {
        SynthKmpSpec kmp;
        kmp.samples.push_back({smpId, "SmpAud", 48000});
        kmp.zones.push_back({0, 127});
        kmp.multis.push_back({msId, "MsAud", 0, 1});
        const Bytes body = buildSoundBody(L, {{msId, kZeroId}}, 0x5Au);

        NativeKstClosureInput in;
        in.downstream.kmpContainer = RawNamedBlob{"MULTISMP/RAM.KMP", "", buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM", buildPa5xKmpPayload(kmp), ""}})};
        in.downstream.pcmContainers.push_back({"PCM/ram01.pcm", "", buildKorfContainer({
            {0x01010018u, 0, 0, 0, "SmpAud", Bytes(4096, 0x3A), idAsStr(smpId)}
        })});
        std::map<std::string, std::vector<SynthKorfItem>> byBank;
        for (const auto& [bank, slot] : targetSlots) {
            byBank[bank].push_back({kKind, 0, 0, slot, "AuditLead", makeOcLiteralFrame("OC31", body), ""});
        }
        for (const auto& [bank, items] : byBank) {
            in.downstream.soundContainers.push_back({"SOUND/" + bank + ".PCG", bank, buildKorfContainer(items)});
        }
        if (!styleRel.empty()) {
            in.upstreamRoots.push_back({styleRel, rtSerialize({
                rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, styleMsb, styleLsb, stylePc}}))})})
            })});
        }
        if (!extraObjectRel.empty()) {
            in.upstreamRoots.push_back({extraObjectRel, rtSerialize({
                rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 0, 2}}))})})
            })});
        }
        M1AdapterOptions aopts;
        aopts.closureOptions.downstream.layouts[kKind] = L;
        aopts.closureOptions.downstream.allowFitLayout = false;
        return adaptNativeKstInput(in, aopts);
    };

    // P1(a): Source Style references USER02/5 (LSB 65 PC 5); Target holds identical Sound ONLY at USER01/0 (cross-slot!)
    // MUST record requiresRemap=true and stay blocked (executable == false, SMF_REMAP_REQUIRED) without explicit remap!
    {
        const M1AdapterView src = makeSingleSoundKst({{"USER02", 5}}, "STYLE/BankA/CrossSlot.stg", 121, 65, 5);
        const M1AdapterView dstCrossOnly = makeSingleSoundKst({{"USER01", 0}}, "", 0, 0, 0);
        CollectorRequest req;
        req.mode = ExportMode::RootTreeSubKst;
        req.targets = {selStyle("STYLE/BankA/CrossSlot.stg")};
        const DryRunExportPlan p = collectSelectiveExport(src, req, &dstCrossOnly);
        TEST("P1(a) cross-slot identical match records requiresRemap=true and stays blocked (SMF_REMAP_REQUIRED)",
             p.ok && !p.executable && p.hasBlockingCode(collector_diag_code::kSmfRemapRequired) &&
                 p.collectedSounds.size() == 1 && !p.collectedSounds[0].satisfiedByTarget &&
                 p.collectedSounds[0].requiresRemap &&
                 p.collectedSounds[0].matchedTargetSlot == "pa5x:USER01/Sound/0");

        // With explicit approved remap override USER02/5 -> USER01/0, RootTreeSubKst becomes executable
        req.approvedSoundRemaps[{false, "USER02", Kind::Sound, 5}] = {false, "USER01", Kind::Sound, 0};
        const DryRunExportPlan pApproved = collectSelectiveExport(src, req, &dstCrossOnly);
        TEST("P1(a') explicit approvedSoundRemaps override lifts block while preserving requiresRemap=true",
             pApproved.ok && pApproved.executable && pApproved.collectedSounds.size() == 1 &&
                 pApproved.collectedSounds[0].satisfiedByTarget && pApproved.collectedSounds[0].requiresRemap &&
                 pApproved.collectedSounds[0].matchedTargetSlot == "pa5x:USER01/Sound/0");
    }

    // P1(b): Target holds identical Sound at BOTH earlier slot USER01/0 AND exact slot USER02/5 -> exact slot USER02/5 MUST win!
    {
        const M1AdapterView src = makeSingleSoundKst({{"USER02", 5}}, "STYLE/BankA/ExactSlot.stg", 121, 65, 5);
        const M1AdapterView dstBoth = makeSingleSoundKst({{"USER01", 0}, {"USER02", 5}}, "", 0, 0, 0);
        CollectorRequest req;
        req.mode = ExportMode::RootTreeSubKst;
        req.targets = {selStyle("STYLE/BankA/ExactSlot.stg")};
        const DryRunExportPlan p = collectSelectiveExport(src, req, &dstBoth);
        TEST("P1(b) exact same-slot USER02/5 wins over lexicographically earlier USER01/0 (requiresRemap=false)",
             p.ok && p.executable && p.collectedSounds.size() == 1 &&
                 p.collectedSounds[0].satisfiedByTarget && !p.collectedSounds[0].requiresRemap &&
                 p.collectedSounds[0].matchedTargetSlot == "pa5x:USER02/Sound/5");
    }

    // M2a Integration: Target occupies USER02/5 with a DIFFERENT Sound AND holds proven-identical Sound at USER01/0:
    // BlockWithReport blocks both M2a (SoundSlotIdenticalElsewhere) & M2b (SMF_REMAP_REQUIRED);
    // ReuseIdenticalTargetSlot + approvedSoundRemaps lifts both cleanly!
    {
        const M1AdapterView src = makeSingleSoundKst({{"USER02", 5}}, "STYLE/BankA/RemapPolicy.stg", 121, 65, 5);
        M1AdapterView dstOccupied = makeSingleSoundKst({{"USER01", 0}, {"USER02", 5}}, "", 0, 0, 0);
        // Make target USER02/5 a genuinely DIFFERENT Sound (different decoded body SHA-256)
        auto& occ = dstOccupied.sounds.at({false, "USER02", Kind::Sound, 5});
        occ.decodedBodySha256 = std::string(64, 'a');
        occ.equivalence.bodySha256 = occ.decodedBodySha256;
        finalizeSoundEquivalenceProfile(occ.equivalence);

        CollectorRequest req;
        req.mode = ExportMode::RootTreeSubKst;
        req.conflictPolicy = ConflictPolicy::BlockWithReport;
        req.targets = {selStyle("STYLE/BankA/RemapPolicy.stg")};
        const DryRunExportPlan pBlocked = collectSelectiveExport(src, req, &dstOccupied);
        TEST("M2a+M2b cross-slot occupied target blocks under BlockWithReport",
             pBlocked.ok && !pBlocked.executable && pBlocked.blockingConflictCount() >= 1 &&
                 pBlocked.hasBlockingCode(collector_diag_code::kSmfRemapRequired));

        req.conflictPolicy = ConflictPolicy::ReuseIdenticalTargetSlot;
        req.approvedSoundRemaps[{false, "USER02", Kind::Sound, 5}] = {false, "USER01", Kind::Sound, 0};
        const DryRunExportPlan pReused = collectSelectiveExport(src, req, &dstOccupied);
        TEST("M2a+M2b ReuseIdenticalTargetSlot + approvedSoundRemaps resolves cross-slot cleanly",
             pReused.ok && pReused.executable && pReused.blockingConflictCount() == 0 &&
                 pReused.collectedSounds.size() == 1 &&
                 pReused.collectedSounds[0].satisfiedByTarget &&
                 pReused.collectedSounds[0].requiresRemap &&
                 pReused.collectedSounds[0].matchedTargetSlot == "pa5x:USER01/Sound/0" &&
                 pReused.hasDiagnosticCode(diag_code::kSongBookUnrewritten, DiagnosticSeverity::Warning));
    }

    // M2a Integration: FAT32/exFAT R-18 case-folded object path collision detected automatically via evaluateCollisionMatrix()
    {
        const M1AdapterView src = makeSingleSoundKst({{"USER01", 0}}, "STYLE/BankA/Ballad.stg", 121, 0, 1);
        const M1AdapterView dstCase = makeSingleSoundKst({{"USER01", 0}}, "", 0, 0, 0, "STYLE/BANKA/ballad.stg");
        CollectorRequest req;
        req.mode = ExportMode::RootTreeSubKst;
        req.targets = {selStyle("STYLE/BankA/Ballad.stg")};
        const DryRunExportPlan p = collectSelectiveExport(src, req, &dstCase);
        TEST("M2a integration: FAT32 R-18 case-folded path collision blocks via CollisionMatrix",
             p.ok && !p.executable && p.hasBlockingCode(diag_code::kFatCaseCollisionR18) &&
                 p.blockingConflictCount() >= 1 && p.collisionMatrix.has_value());
    }

    // P2-1: Failed or cancelled optionalTarget MUST fail closed (never silent 0-byte baseline / executable==true!)
    {
        const M1AdapterView src = makeSingleSoundKst({{"USER01", 0}}, "STYLE/BankA/Factory.stg", 121, 0, 1);
        M1AdapterView failedTarget;
        failedTarget.ok = false;
        failedTarget.cancelled = false;
        failedTarget.sourceKind = AdapterSourceKind::NativeKst;

        CollectorRequest req;
        req.mode = ExportMode::AdvisoryPlan;
        req.targets = {selStyle("STYLE/BankA/Factory.stg")};
        const DryRunExportPlan pFail = collectSelectiveExport(src, req, &failedTarget);
        TEST("P2-1 failed optionalTarget fails closed (ok=false, executable=false, TARGET_UNINSPECTABLE)",
             !pFail.ok && !pFail.executable && pFail.hasBlockingCode(diag_code::kTargetUninspectable));

        M1AdapterView cancelledTarget;
        cancelledTarget.ok = false;
        cancelledTarget.cancelled = true;
        const DryRunExportPlan pCancel = collectSelectiveExport(src, req, &cancelledTarget);
        TEST("P2-1 cancelled optionalTarget propagates cancellation cleanly",
             pCancel.cancelled && !pCancel.ok && !pCancel.executable);
    }

    // P2-2: Untrusted control chars / newlines / backticks in Markdown output must be sanitized safely
    {
        DryRunExportPlan dirty;
        dirty.ok = true;
        dirty.planFingerprint = std::string(64, '0');
        dirty.diagnostics.push_back({
            DiagnosticCategory::VERIFIED_MISSING,
            collector_diag_code::kSelectedRootNotFound,
            DiagnosticSeverity::Blocking,
            EvidenceLevel::Verified,
            "STYLE/Evil`Backtick\n# ForgedHeading\x01.stg",
            "",
            "Bad summary with `code` and\n- forged bullet",
            "Fix\r\n> forged quote",
            {}
        });
        dirty.dedupGroups.push_back({
            DedupTier::Tier1LogicalIdentity, "Sound", "resource-id", "k",
            "pa5x:Evil`Name\n## Inject/Sound/0", {}, 2, 0, true
        });
        dirty.nativeEquivalences.push_back({
            "source-vs-target-sound", "src`bad\nline", "dst`bad\r\x02",
            EquivalenceVerdict::Different, "reason\n# inject", false
        });
        const std::string md = serializeDryRunPlanMarkdown(dirty);
        TEST("P2-2 Markdown sanitizes newlines, control chars, and backticks safely",
             md.find("\n# ForgedHeading") == std::string::npos &&
                 md.find("\n## Inject") == std::string::npos &&
                 md.find("\n- forged bullet") == std::string::npos &&
                 md.find("\n> forged quote") == std::string::npos &&
                 md.find('\x01') == std::string::npos &&
                 md.find('\x02') == std::string::npos &&
                 md.find("Evil`Backtick") == std::string::npos &&
                 md.find("Evil`Name") == std::string::npos);
    }

    // P2-3: Huge uint64_t byte counts saturate cleanly without unsigned wraparound
    {
        M1AdapterView bigSrc;
        bigSrc.ok = true;
        bigSrc.sourceKind = AdapterSourceKind::NativeKst;
        bigSrc.coverage.kmpPresent = true;
        bigSrc.coverage.kmpParsed = true;
        bigSrc.manifestSha256 = std::string(64, '1');

        const uint64_t kHuge = (std::numeric_limits<uint64_t>::max() / 2u) + 1000u;
        const ResourceId smp0{false, "KST", Kind::Sample, 0};
        const ResourceId smp1{false, "KST", Kind::Sample, 1};
        const ResourceId snd0{false, "USER01", Kind::Sound, 0};
        bigSrc.samples[smp0] = {smp0, 0, makeId(1, 0), "Huge0", "PCM/ram01.pcm", kHuge, 48000, true, true, std::nullopt};
        bigSrc.samples[smp1] = {smp1, 1, makeId(1, 1), "Huge1", "PCM/ram01.pcm", kHuge, 48000, true, true, std::nullopt};

        SoundClosureRecord sc;
        sc.id = snd0;
        sc.slotAddress = SlotAddress{false, 1, 0};
        sc.bank = "USER01";
        sc.slot = 0;
        sc.decoded = true;
        sc.analysed = true;
        sc.sampleIds = {smp0, smp1};
        sc.storedPcmBytesProxy = std::numeric_limits<uint64_t>::max();
        bigSrc.sounds[snd0] = sc;

        for (int i = 0; i < 3; ++i) {
            RootItemRecord r;
            r.id = {false, "KST", Kind::Style, static_cast<uint32_t>(i)};
            r.kind = RootItemKind::Style;
            r.relPath = "STYLE/Big/S" + std::to_string(i) + ".stg";
            r.parseOk = true;
            SoundSelectionRecord sel = classifySoundSelection({121, 64, 0});
            sel.resolved = true;
            sel.fromStyleMainTrack = true;
            r.soundSelections.push_back(sel);
            bigSrc.rootIndexByRelPath[r.relPath] = bigSrc.roots.size();
            bigSrc.roots.push_back(std::move(r));
        }

        CollectorRequest req;
        req.targets = {selStyle("STYLE/Big/S0.stg"), selStyle("STYLE/Big/S1.stg"), selStyle("STYLE/Big/S2.stg")};
        const DryRunExportPlan pBig = collectSelectiveExport(bigSrc, req, &bigSrc);
        TEST("P2-3 huge uint64_t byte sums & products saturate cleanly without wraparound",
             pBig.memoryProjection.rawReferencedSampleBytes == std::numeric_limits<uint64_t>::max() &&
                 pBig.memoryProjection.uniqueLogicalSampleBytes == std::numeric_limits<uint64_t>::max() &&
                 pBig.memoryProjection.targetBaselineSampleBytes == std::numeric_limits<uint64_t>::max() &&
                 pBig.memoryProjection.projectedCombinedBytes == std::numeric_limits<uint64_t>::max());
    }

    // P3-2: Shuffled provisional inputs with no SMF yield byte-identical contentSha256 & planFingerprint
    {
        auto makeProvNoSmf = [](bool reversed) {
            ProvisionalWorkspaceInput ws;
            ProvisionalRootInput r;
            r.relPath = "STYLES/UasfOrder.uasf";
            r.kind = RootItemKind::ProvisionalUasf;
            r.explicitSelections = {{121, 0, 1}, {121, 0, 5}, {120, 0, 0}};
            r.directPackageIds = {};
            if (reversed) std::reverse(r.explicitSelections.begin(), r.explicitSelections.end());
            ws.roots.push_back(r);
            return adaptProvisionalWorkspace(ws);
        };
        const M1AdapterView vA = makeProvNoSmf(false);
        const M1AdapterView vB = makeProvNoSmf(true);
        CollectorRequest req;
        req.targets = {selStyle("STYLES/UasfOrder.uasf")};
        const DryRunExportPlan pA = collectSelectiveExport(vA, req);
        const DryRunExportPlan pB = collectSelectiveExport(vB, req);
        TEST("P3-2 shuffled provisional no-SMF inputs yield byte-identical hash & planFingerprint",
             vA.roots[0].contentSha256 == vB.roots[0].contentSha256 &&
                 pA.planFingerprint == pB.planFingerprint);
    }
}

// ── Test 8: Track A Slice A3 — USERDK DrumSample Tier-1 Closure, Dedup Parity, Dual-Use Sample Accounting & HOLD ─────

static Bytes buildSynthUserDk1005Body(
    const std::vector<std::tuple<uint8_t, uint8_t, uint16_t, Id8>>& keyAssignments) {
    // Known variable-layer 0x10050038 USERDK layout: hdr=514 (modeFlag @ +2 = 0x82), kst=20 (lcOff=2), lst=32 (hasId8=true).
    constexpr size_t kHdr = 514;
    constexpr size_t kKst = 20;
    constexpr size_t kLcOff = 2;
    constexpr size_t kLst = 32;
    constexpr size_t kVelOff = kKst - 10;

    struct LayerSpec { uint8_t bank = 0; uint16_t tblIdx = 0; Id8 id8{}; };
    std::array<LayerSpec, 128> keyLayer{};
    for (const auto& [note, bank, tblIdx, id8] : keyAssignments) {
        if (note < 128u) keyLayer[note] = LayerSpec{bank, tblIdx, id8};
    }

    Bytes body(kHdr + 128u * kKst + 128u * kLst, 0x00);
    body[2] = 0x82u; // modeFlag bit 7 set
    for (uint32_t k = 0; k < 128u; ++k) {
        const size_t ko = kHdr + size_t(k) * kKst;
        body[ko + 0u] = 0x7Fu;
        body[ko + 1u] = 0xFFu;
        body[ko + kLcOff] = 1u;
        body[ko + kLcOff + 1u] = 64u;
        for (size_t j = 0; j < 8u; ++j) body[ko + kVelOff + j] = 127u;
        const uint16_t cumEnd = static_cast<uint16_t>((k + 1u) | (k == 127u ? 0x8000u : 0u));
        body[ko + kKst - 2u] = static_cast<uint8_t>(cumEnd >> 8);
        body[ko + kKst - 1u] = static_cast<uint8_t>(cumEnd & 0xFFu);

        const size_t lo = kHdr + 128u * kKst + size_t(k) * kLst;
        body[lo + 0u] = keyLayer[k].bank;
        body[lo + 2u] = static_cast<uint8_t>(keyLayer[k].tblIdx >> 8);
        body[lo + 3u] = static_cast<uint8_t>(keyLayer[k].tblIdx & 0xFFu);
        body[lo + 4u] = 0x80u;
        body[lo + 5u] = 0x00u;
        std::copy(keyLayer[k].id8.begin(), keyLayer[k].id8.end(),
                  body.begin() + static_cast<std::ptrdiff_t>(lo + kLst - 8u));
    }
    return body;
}

static void testUserDkDrumSampleTier1Parity() {
    std::printf("[Test 8] Track A Slice A3 USERDK DrumSample Tier-1 closure, dedup parity, dual-use Sample & HOLD\n");

    const RecordLayout melL{12, 24, 8};
    const uint32_t kKind = 0x10050038u;

    const Id8 smp0Id = makeId(0x91, 0); // 2000 B (Kick L)
    const Id8 smp1Id = makeId(0x91, 1); // 2500 B (Kick R)
    const Id8 smp2Id = makeId(0x91, 2); // 3000 B (Snare)
    const Id8 smp3Id = makeId(0x91, 3); // 4096 B (Dual-use: Multisample/0 ∩ DrumSample/2!)
    const Id8 smp4Id = makeId(0x91, 4); // 5000 B (KitB extra perc)

    const Id8 ds0Id = makeId(0x92, 0); // Kick stereo (smp0, smp1)
    const Id8 ds1Id = makeId(0x92, 1); // Snare mono (smp2)
    const Id8 ds2Id = makeId(0x92, 2); // Shared perc mono (smp3)
    const Id8 ds3Id = makeId(0x92, 3); // Extra perc mono (smp4)
    const Id8 ms0Id = makeId(0x93, 0); // Melodic MS referencing smp3 (dual-use!)

    SynthKmpSpec kmp;
    kmp.samples.push_back({smp0Id, "SynKickL", 48000});
    kmp.samples.push_back({smp1Id, "SynKickR", 48000});
    kmp.samples.push_back({smp2Id, "SynSnare", 48000});
    kmp.samples.push_back({smp3Id, "SynDualPerc", 48000});
    kmp.samples.push_back({smp4Id, "SynExtraPerc", 48000});
    kmp.drums.push_back({ds0Id, "SynDrumKick", 0, 1});
    kmp.drums.push_back({ds1Id, "SynDrumSnare", 2, kNoZoneSample});
    kmp.drums.push_back({ds2Id, "SynDrumDual", 3, kNoZoneSample});
    kmp.drums.push_back({ds3Id, "SynDrumExtra", 4, kNoZoneSample});
    kmp.zones.push_back({3, 127});
    kmp.multis.push_back({ms0Id, "SynMsDual", 0, 1});

    const Bytes melBody = buildSoundBody(melL, {{ms0Id, kZeroId}}, 0x4Au);
    const Bytes dk0Body = buildSynthUserDk1005Body({
        {36, 1, 0, ds0Id},
        {38, 1, 1, ds1Id},
        {42, 1, 2, ds2Id},
    });
    const Bytes dk1Body = buildSynthUserDk1005Body({
        {42, 1, 2, ds2Id},
        {46, 1, 3, ds3Id},
    });
    // ROM-only USERDK: populated ROM key (bank=0, tblIdx=15, zero ID8) + default placeholders
    const Bytes dkRomBody = buildSynthUserDk1005Body({
        {36, 0, 15, kZeroId},
        {38, 0, 22, kZeroId},
    });

    NativeKstClosureInput in;
    in.downstream.kmpContainer = RawNamedBlob{
        "MULTISMP/RAM.KMP", "", buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM", buildPa5xKmpPayload(kmp), ""}})};
    in.downstream.pcmContainers.push_back({"PCM/ram01.pcm", "", buildKorfContainer({
        {0x01010018u, 0, 0, 0, "SynKickL",    Bytes(2000, 0x11), idAsStr(smp0Id)},
        {0x01010018u, 0, 0, 1, "SynKickR",    Bytes(2500, 0x12), idAsStr(smp1Id)},
        {0x01010018u, 0, 0, 2, "SynSnare",    Bytes(3000, 0x13), idAsStr(smp2Id)},
        {0x01010018u, 0, 0, 3, "SynDualPerc", Bytes(4096, 0x14), idAsStr(smp3Id)},
        {0x01010018u, 0, 0, 4, "SynExtraPerc", Bytes(5000, 0x15), idAsStr(smp4Id)},
    })});
    in.downstream.soundContainers.push_back({"SOUND/USER01.PCG", "USER01", buildKorfContainer({
        {kKind, 0, 0, 0, "SynDualMel", makeOcLiteralFrame("OC31", melBody), ""}
    })});
    in.downstream.soundContainers.push_back({"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
        {kKind, 4, 11, 0, "SynKitAlpha", dk0Body, ""},
        {kKind, 4, 11, 1, "SynKitBeta",  dk1Body, ""},
        {kKind, 4, 11, 2, "SynKitRom",   dkRomBody, ""},
    })});

    // StyleSoloDk: single Style referencing RAM USERDK01/0 only
    in.upstreamRoots.push_back({"STYLE/BankA/SoloDk.stg", rtSerialize({
        rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{9, 120, 64, 0}}))})})
    })});
    // StyleDualA: melodic USER01/0 (Smp3) + USERDK01/0 (ds0, ds1, ds2 -> Smp0..Smp3)
    in.upstreamRoots.push_back({"STYLE/BankA/DualA.stg", rtSerialize({
        rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 64, 0}, {9, 120, 64, 0}}))})})
    })});
    // StyleDualB: USERDK01/0 (ds0, ds1, ds2) + USERDK01/1 (ds2, ds3 -> Smp3, Smp4)
    in.upstreamRoots.push_back({"STYLE/BankA/DualB.stg", rtSerialize({
        rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{9, 120, 64, 0}, {10, 120, 64, 1}}))})})
    })});
    // StyleRomDk: ROM-only USERDK01/2
    in.upstreamRoots.push_back({"STYLE/BankA/RomDk.stg", rtSerialize({
        rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{9, 120, 64, 2}}))})})
    })});

    M1AdapterOptions aopts;
    aopts.closureOptions.downstream.layouts[kKind] = melL;
    aopts.closureOptions.downstream.allowFitLayout = false;
    const M1AdapterView view = adaptNativeKstInput(in, aopts);

    TEST("A3 adapter ok with RAM & ROM USERDKs", view.ok && !view.coverage.deleteProofAvailable);

    size_t unrefCount = 0;
    for (const auto& [nid, _] : view.graph->nodes()) {
        if (view.graph->deleteCheck(nid).verdict == DeleteVerdict::Unreferenced) ++unrefCount;
    }
    TEST("A3 Native-KST deleteCheck() NEVER Unreferenced with USERDK", unrefCount == 0);

    // 8a. Single Style referencing RAM USERDK -> uniqueDrumSamples == 3, drumSampleCount == 3, closureNodes has DrumSample,
    //     and RootTreeSubKst keeps blocking NATIVE_SOUND_WRITE_HOLD even on identical same-slot target!
    {
        CollectorRequest req;
        req.mode = ExportMode::AdvisoryPlan;
        req.targets = {selStyle("STYLE/BankA/SoloDk.stg")};
        const DryRunExportPlan p = collectSelectiveExport(view, req);

        const ResourceId ds0{false, "KST", Kind::DrumSample, 0};
        const ResourceId ds1{false, "KST", Kind::DrumSample, 1};
        const ResourceId ds2{false, "KST", Kind::DrumSample, 2};
        const bool allDsInClosure =
            std::find(p.closureNodes.begin(), p.closureNodes.end(), ds0) != p.closureNodes.end() &&
            std::find(p.closureNodes.begin(), p.closureNodes.end(), ds1) != p.closureNodes.end() &&
            std::find(p.closureNodes.begin(), p.closureNodes.end(), ds2) != p.closureNodes.end();

        size_t tier1Ds = 0;
        for (const auto& dg : p.dedupGroups) {
            if (dg.tier == DedupTier::Tier1LogicalIdentity && dg.resourceKind == "DrumSample") ++tier1Ds;
        }
        TEST("A3(a) single RAM USERDK Style: uniqueDrumSamples==3, drumSampleCount==3, closureNodes has DrumSamples, 0 DS dedup groups",
             p.ok && p.executable && !p.deleteProofAvailable && !p.hardwareVerified &&
                 p.uniqueDrumSamples.size() == 3 &&
                 p.uniqueDrumSamples[0] == ds0 && p.uniqueDrumSamples[1] == ds1 && p.uniqueDrumSamples[2] == ds2 &&
                 p.collectedSounds.size() == 1 &&
                 p.collectedSounds[0].isDrumKit && p.collectedSounds[0].analysed &&
                 p.collectedSounds[0].drumSampleCount == 3 &&
                 p.collectedSounds[0].multisampleCount == 0 &&
                 p.collectedSounds[0].sampleCount == 4 &&
                 p.collectedSounds[0].uniqueSampleBytes == 11596u &&
                 allDsInClosure && tier1Ds == 0);

        // Same-slot Identical target in RootTreeSubKst MUST keep blocking NATIVE_SOUND_WRITE_HOLD (T19 parity)
        CollectorRequest subReq = req;
        subReq.mode = ExportMode::RootTreeSubKst;
        const DryRunExportPlan pSub = collectSelectiveExport(view, subReq, &view);
        TEST("A3(a') same-slot Identical USERDK stays blocked by NATIVE_SOUND_WRITE_HOLD in RootTreeSubKst",
             pSub.ok && !pSub.executable && pSub.hasBlockingCode(diag_code::kNativeSoundWriteHold) &&
                 pSub.uniqueDrumSamples.size() == 3 &&
                 std::find(pSub.closureNodes.begin(), pSub.closureNodes.end(), ds0) != pSub.closureNodes.end());
    }

    // 8b. Two Styles sharing USERDK / DrumSample + dual-use Sample (Multisample ∩ DrumSample):
    //     Tier-1 DrumSample DedupGroups emitted, uniqueDrumSamples deduplicated, ZERO double-counting of dual-use Sample bytes!
    {
        CollectorRequest reqAB;
        reqAB.mode = ExportMode::AdvisoryPlan;
        reqAB.targets = {selStyle("STYLE/BankA/DualA.stg"), selStyle("STYLE/BankA/DualB.stg")};
        const DryRunExportPlan pAB = collectSelectiveExport(view, reqAB);

        CollectorRequest reqBA = reqAB;
        std::reverse(reqBA.targets.begin(), reqBA.targets.end());
        const DryRunExportPlan pBA = collectSelectiveExport(view, reqBA);

        // Unique samples: Smp0(2000) + Smp1(2500) + Smp2(3000) + Smp3(4096) + Smp4(5000) = 16596 (Smp3 counted ONCE!)
        // Raw refs: DualA = USER01/0(4096) + USERDK01/0(11596) = 15692;
        //           DualB = USERDK01/0(11596) + USERDK01/1(9096) = 20692; total raw = 36388; saved = 19792.
        const uint64_t wantUniqueBytes = 2000u + 2500u + 3000u + 4096u + 5000u;
        const uint64_t wantRawBytes = 4096u + 11596u + 11596u + 9096u;
        const uint64_t wantSavedBytes = wantRawBytes - wantUniqueBytes;

        size_t tier1DsCount = 0;
        std::set<std::string> dedupedDsIds;
        bool allDsZeroSavedApplied = true;
        bool foundDualSmpDedup = false;
        for (const auto& dg : pAB.dedupGroups) {
            if (dg.tier == DedupTier::Tier1LogicalIdentity && dg.resourceKind == "DrumSample") {
                ++tier1DsCount;
                dedupedDsIds.insert(dg.canonicalMember);
                if (dg.keyType != "resource-id+kmp-id8" || dg.savedBytes != 0u || !dg.appliedInExport || dg.occurrenceCount != 2u) {
                    allDsZeroSavedApplied = false;
                }
            }
            if (dg.tier == DedupTier::Tier1LogicalIdentity && dg.resourceKind == "Sample" &&
                dg.canonicalMember == "pa5x:KST/Sample/3") {
                // Referenced 4 times: USER01/0 (DualA), USERDK01/0 (DualA), USERDK01/0 (DualB), USERDK01/1 (DualB)
                foundDualSmpDedup = (dg.appliedInExport && dg.occurrenceCount == 4u && dg.savedBytes == 3u * 4096u);
            }
        }

        const CollectedSoundSummary* melSum = nullptr;
        const CollectedSoundSummary* dk0Sum = nullptr;
        const CollectedSoundSummary* dk1Sum = nullptr;
        for (const auto& cs : pAB.collectedSounds) {
            if (cs.id == ResourceId{false, "USER01", Kind::Sound, 0}) melSum = &cs;
            if (cs.id == ResourceId{false, "USERDK01", Kind::Sound, 0}) dk0Sum = &cs;
            if (cs.id == ResourceId{false, "USERDK01", Kind::Sound, 1}) dk1Sum = &cs;
        }

        TEST("A3(b) two Styles sharing USERDK/DrumSample + dual-use Sample: exact counts & ZERO double-counting",
             pAB.ok && pAB.executable &&
                 pAB.uniqueMultisamples.size() == 1 &&
                 pAB.uniqueDrumSamples.size() == 4 &&
                 pAB.uniqueSamples.size() == 5 &&
                 pAB.memoryProjection.uniqueLogicalSampleBytes == wantUniqueBytes &&
                 pAB.memoryProjection.rawReferencedSampleBytes == wantRawBytes &&
                 pAB.memoryProjection.logicalDedupSavedBytes == wantSavedBytes &&
                 melSum != nullptr && melSum->multisampleCount == 1 && melSum->drumSampleCount == 0 &&
                 dk0Sum != nullptr && dk0Sum->multisampleCount == 0 && dk0Sum->drumSampleCount == 3 &&
                 dk1Sum != nullptr && dk1Sum->multisampleCount == 0 && dk1Sum->drumSampleCount == 2);

        TEST("A3(b') Tier-1 DrumSample DedupGroups emitted for shared ds0/ds1/ds2 (not unshared ds3) & dual-use Sample dedup exact",
             tier1DsCount == 3 && allDsZeroSavedApplied && foundDualSmpDedup &&
                 dedupedDsIds.count("pa5x:KST/DrumSample/0") == 1 &&
                 dedupedDsIds.count("pa5x:KST/DrumSample/1") == 1 &&
                 dedupedDsIds.count("pa5x:KST/DrumSample/2") == 1 &&
                 dedupedDsIds.count("pa5x:KST/DrumSample/3") == 0);

        // Single Style (DualB alone) with 2 distinct USERDK kits sharing DrumSample/2 -> Tier-1 DrumSample DedupGroup emitted via sounds.size()==2
        CollectorRequest reqB;
        reqB.mode = ExportMode::AdvisoryPlan;
        reqB.targets = {selStyle("STYLE/BankA/DualB.stg")};
        const DryRunExportPlan pB = collectSelectiveExport(view, reqB);
        size_t singleRootDsDedup = 0;
        bool ds2SharedAcrossKits = false;
        for (const auto& dg : pB.dedupGroups) {
            if (dg.tier == DedupTier::Tier1LogicalIdentity && dg.resourceKind == "DrumSample") {
                ++singleRootDsDedup;
                if (dg.canonicalMember == "pa5x:KST/DrumSample/2" && dg.occurrenceCount == 2u &&
                    dg.savedBytes == 0u && dg.appliedInExport) {
                    ds2SharedAcrossKits = true;
                }
            }
        }
        TEST("A3(b'') single Style referencing 2 USERDKs sharing DrumSample/2 emits Tier-1 DrumSample DedupGroup (occurrenceCount==2)",
             pB.ok && pB.uniqueDrumSamples.size() == 4 &&
                 pB.memoryProjection.uniqueLogicalSampleBytes == wantUniqueBytes &&
                 singleRootDsDedup == 1 && ds2SharedAcrossKits);

        // JSON & Markdown additive serialization + byte-identical reproducibility + M2a bridge
        const std::string jsonAB = serializeDryRunPlanJson(pAB);
        const std::string jsonBA = serializeDryRunPlanJson(pBA);
        const std::string mdAB = serializeDryRunPlanMarkdown(pAB);
        const std::string mdBA = serializeDryRunPlanMarkdown(pBA);
        kit::Json parsedDry;
        const std::string dryErr = kit::parseJson(jsonAB, &parsedDry);
        const SelectiveExportReport m2aRep = toSelectiveExportReport(pAB, "SyntheticSource");
        const std::string m2aJson = serializeSelectiveReportJson(m2aRep);
        kit::Json parsedM2a;
        const std::string m2aErr = kit::parseJson(m2aJson, &parsedM2a);

        TEST("A3(d) JSON & Markdown include additive uniqueDrumSamples/drumSampleCount/DrumSample dedup & stay reproducible",
             dryErr.empty() && m2aErr.empty() &&
                 pAB.planFingerprint == pBA.planFingerprint &&
                 jsonAB == jsonBA && mdAB == mdBA &&
                 jsonAB.find("\"uniqueDrumSamples\": 4") != std::string::npos &&
                 jsonAB.find("\"drumSampleCount\": 3") != std::string::npos &&
                 jsonAB.find("\"drumSampleCount\": 2") != std::string::npos &&
                 jsonAB.find("\"drumSampleCount\": 0") != std::string::npos &&
                 jsonAB.find("\"resourceKind\": \"DrumSample\"") != std::string::npos &&
                 mdAB.find("4 DrumSample(s)") != std::string::npos &&
                 mdAB.find("`DrumSample`") != std::string::npos);
    }

    // 8c. ROM-only USERDK -> uniqueDrumSamples == 0, drumSampleCount == 0, no crash, HOLD preserved in RootTreeSubKst
    {
        CollectorRequest req;
        req.mode = ExportMode::AdvisoryPlan;
        req.requireCompleteSampleClosure = false;
        req.targets = {selStyle("STYLE/BankA/RomDk.stg")};
        const DryRunExportPlan pRom = collectSelectiveExport(view, req);

        CollectorRequest reqSub = req;
        reqSub.mode = ExportMode::RootTreeSubKst;
        const DryRunExportPlan pRomSub = collectSelectiveExport(view, reqSub, &view);

        TEST("A3(c) ROM-only USERDK: uniqueDrumSamples==0, drumSampleCount==0, no crash, HOLD preserved in RootTreeSubKst",
             pRom.ok && !pRom.deleteProofAvailable &&
                 pRom.uniqueDrumSamples.empty() &&
                 pRom.collectedSounds.size() == 1 &&
                 pRom.collectedSounds[0].isDrumKit &&
                 pRom.collectedSounds[0].drumSampleCount == 0 &&
                 pRom.collectedSounds[0].sampleCount == 0 &&
                 pRom.collectedSounds[0].uniqueSampleBytes == 0 &&
                 pRomSub.ok && !pRomSub.executable &&
                 pRomSub.hasBlockingCode(diag_code::kNativeSoundWriteHold));
    }
}

int main() {
    std::printf("=== Slice M2b Selective Collector & M2a Unified Integration Synthetic Tests ===\n");
    testMultiStyleSharingAndEmbeddedFlags();
    testProvisionalTier2DedupAndMemoryProjection();
    testTier3NativeEquivalenceAndHoldGates();
    testFailClosedBlockingAndBoundaries();
    testCooperativeCancellation();
    testReproducibilityJsonAndPathFree();
    testAuditHardeningAndM2aIntegration();
    testUserDkDrumSampleTier1Parity();
    std::printf("=== Summary: %d passed, %d failed ===\n", gPasses, gFailures);
    return gFailures == 0 ? 0 : 1;
}
