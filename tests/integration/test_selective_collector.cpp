// Slice M2b Synthetic Regression Tests:
//   - M1 Manifest Adapter (`src/media/sound/m1_manifest_adapter.h`)
//   - Automatic Selective Dependency Collector, 3-Tier Deduplicator, Memory Projection & Reproducible Dry-Run Plan
//     (`src/media/sound/selective_collector.h`)
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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
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
        trk.push_back(static_cast<char>(0x80u)); // invalid high bit >= 0x80
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

static bool hasBlocker(const DryRunExportPlan& p, BlockingCode code) {
    for (const auto& b : p.blockingReasons) if (b.code == code) return true;
    return false;
}

static bool hasNotice(const DryRunExportPlan& p, const std::string& code, const std::string& sev = "") {
    for (const auto& n : p.notices) {
        if (n.code == code && (sev.empty() || n.severity == sev)) return true;
    }
    return false;
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
    // 10 synthetic Samples with distinct sizes: 2000 + 200*i bytes (all even, 16-bit proxy)
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
    // 2 Multisamples sharing Samples 4 and 5:
    //   MS0 uses Samples 0..5 (6 zones)
    //   MS1 uses Samples 4..9 (6 zones)
    for (uint32_t s = 0; s <= 5; ++s) kmp.zones.push_back({s, static_cast<uint8_t>(40 + s * 4)});
    for (uint32_t s = 4; s <= 9; ++s) kmp.zones.push_back({s, static_cast<uint8_t>(70 + (s - 4) * 4)});
    const Id8 ms0Id = makeId(0xB1, 0);
    const Id8 ms1Id = makeId(0xB1, 1);
    kmp.multis.push_back({ms0Id, "MsAlpha", 0, 6});
    kmp.multis.push_back({ms1Id, "MsBeta", 6, 6});

    // 3 Sounds sharing MS0 & MS1:
    //   Sound 0: osc0 -> (MS0, 0)         -> reaches Samples 0..5
    //   Sound 1: osc0 -> (MS1, 0)         -> reaches Samples 4..9
    //   Sound 2: osc0 -> (MS0, MS1)       -> reaches Samples 0..9
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

    // 5 synthetic Styles each referencing all 3 Sounds (Sound 0 in main track, Sound 1 in embedded KBD Set, Sound 2 in embedded Pad)
    for (int i = 1; i <= 5; ++i) {
        const std::string smfMain = makeSmf({{0, 121, 64, 0}, {1, 121, 0, 0}}); // USER01/0 + Factory ROM
        const std::string smfKbd  = makeSmf({{0, 121, 64, 1}});                  // USER01/1
        const std::string smfPad  = makeSmf({{0, 121, 64, 2}});                  // USER01/2
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

    // Verify Graph::deleteCheck() NEVER returns Unreferenced on any node!
    size_t unrefCount = 0;
    for (const auto& [nid, _] : view.graph->nodes()) {
        if (view.graph->deleteCheck(nid).verdict == DeleteVerdict::Unreferenced) ++unrefCount;
    }
    TEST("Native-KST deleteCheck() NEVER Unreferenced", unrefCount == 0);

    // Collect all 5 Styles (with embedded KBD Sets & Pads enabled)
    CollectorRequest req;
    req.mode = CollectorExportMode::AdvisoryPlan;
    for (int i = 1; i <= 5; ++i) req.targets.push_back({"STYLE/BankA/Style0" + std::to_string(i) + ".stg", "", ""});
    const DryRunExportPlan plan = collectSelectiveExport(view, req);

    // Exact pre-dedup bytes: per Style -> Sound0 (samples 0..5) + Sound1 (samples 4..9) + Sound2 (samples 0..9)
    uint64_t perStyleRaw = 0;
    for (int s = 0; s <= 5; ++s) perStyleRaw += sampleSizes[size_t(s)];
    for (int s = 4; s <= 9; ++s) perStyleRaw += sampleSizes[size_t(s)];
    for (int s = 0; s <= 9; ++s) perStyleRaw += sampleSizes[size_t(s)];
    const uint64_t expectedRawBytes = 5u * perStyleRaw;
    const uint64_t expectedSavedBytes = expectedRawBytes - expectedUniqueSampleBytes;

    TEST("5-Style plan ok & executable in AdvisoryPlan", plan.ok && plan.executable && plan.blockingReasons.empty());
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
             plan.memoryProjection.unit == "native_kst_stored_pcm_proxy_bytes" &&
             plan.memoryProjection.fitVerdict == "unverified-proxy" &&
             !plan.memoryProjection.hardwareVerified &&
             plan.memoryProjection.caveat.find("Stored PCM size") != std::string::npos);

    // Verify Tier 1 DedupGroups carry resource-id+kmp-id8 and appliedInExport=true
    size_t tier1Sound = 0, tier1Ms = 0, tier1Smp = 0;
    for (const auto& dg : plan.dedupGroups) {
        if (dg.tier == DedupTier::Tier1LogicalIdentity && dg.appliedInExport) {
            if (dg.resourceKind == "Sound") ++tier1Sound;
            if (dg.resourceKind == "Multisample" && dg.keyType == "resource-id+kmp-id8") ++tier1Ms;
            if (dg.resourceKind == "Sample" && dg.keyType == "resource-id+kmp-id8") ++tier1Smp;
        }
    }
    TEST("5-Style Tier-1 dedup groups (3 Sounds, 2 MS, 10 Samples)", tier1Sound == 3 && tier1Ms == 2 && tier1Smp == 10);

    // Toggle embedded KBD Set & Pad flags on Style01
    CollectorRequest reqNoKbd;
    reqNoKbd.targets = {{"STYLE/BankA/Style01.stg", "", ""}};
    reqNoKbd.includeEmbeddedKbdSets = false;
    reqNoKbd.includeEmbeddedPads = true;
    const DryRunExportPlan planNoKbd = collectSelectiveExport(view, reqNoKbd);
    TEST("includeEmbeddedKbdSets=false excludes USER01/1, keeps main+pad",
         planNoKbd.collectedSounds.size() == 2 &&
             planNoKbd.collectedSounds[0].id.number == 0 &&
             planNoKbd.collectedSounds[1].id.number == 2);

    CollectorRequest reqNoPad;
    reqNoPad.targets = {{"STYLE/BankA/Style01.stg", "", ""}};
    reqNoPad.includeEmbeddedKbdSets = true;
    reqNoPad.includeEmbeddedPads = false;
    const DryRunExportPlan planNoPad = collectSelectiveExport(view, reqNoPad);
    TEST("includeEmbeddedPads=false excludes USER01/2, keeps main+kbd",
         planNoPad.collectedSounds.size() == 2 &&
             planNoPad.collectedSounds[0].id.number == 0 &&
             planNoPad.collectedSounds[1].id.number == 1);

    CollectorRequest reqMainOnly;
    reqMainOnly.targets = {{"STYLE/BankA/Style01.stg", "", ""}};
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

    // PkgAlpha: 2 samples: S_shared (1000 frames = 2000 B), S_sameNameA (500 frames = 1000 B, seed 11)
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

    // PkgBeta: 2 samples: byte-identical audio to ToneShared (different filename!) + CommonLabel with DIFFERENT audio (seed 99)
    sound::Instrument instB;
    instB.name = "SynthBeta";
    instB.samples.push_back(makeSample("RenamedClone", 1000, 42)); // exact same PCM as ToneShared!
    instB.samples.push_back(makeSample("CommonLabel", 500, 99));   // same name, DIFFERENT PCM!
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
    r2.addressToPackageId[{121, 64, 0}] = pkgAId; // shared identical package!
    r2.addressToPackageId[{121, 64, 1}] = pkgBId;

    ws.roots.push_back(r1);
    ws.roots.push_back(r2);

    const M1AdapterView view = adaptProvisionalWorkspace(ws);
    TEST("provisional view ok", view.ok && view.isProvisional() && view.packagesById.size() == 2);

    CollectorRequest req;
    req.mode = CollectorExportMode::ImportKitBundle;
    req.targets = {{"STYLES/BalladOne.mid", "", ""}, {"STYLES/BalladTwo.mid", "", ""}};
    const DryRunExportPlan plan = collectSelectiveExport(view, req);

    // Raw = PkgA (3000) + PkgA (3000) + PkgB (3000) = 9000; Unique = 6000; LogicalSaved = 3000; IdenticalExtra = 2000
    TEST("provisional plan executable", plan.ok && plan.executable && plan.coverageComplete);
    TEST("PkgAlpha included once in uniqueProvisionalPackages",
         plan.uniqueProvisionalPackages.size() == 2 &&
             plan.uniqueProvisionalPackages[0] == pkgAId &&
             plan.uniqueProvisionalPackages[1] == pkgBId);
    TEST("exact uncompressed memory math",
         plan.memoryProjection.unit == "uncompressed_pcm_bytes" &&
             !plan.memoryProjection.sampleBytesAreNativeStoredProxy &&
             plan.memoryProjection.fitVerdict == "fits" &&
             !plan.memoryProjection.hardwareVerified &&
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
        badReq.targets = {{"STYLES/BadSmf.mid", "", ""}};
        const DryRunExportPlan badPlan = collectSelectiveExport(badView, badReq);
        TEST("malformed provisional SMF fails closed with MALFORMED_MIDI_LEAF",
             !badPlan.executable && hasBlocker(badPlan, BlockingCode::MALFORMED_MIDI_LEAF));
    }

    // Provisional source + Native KST target baseline MUST switch to native_kst_stored_pcm_proxy_bytes / unverified-proxy
    {
        M1AdapterView synthTarget;
        synthTarget.ok = true;
        synthTarget.sourceKind = AdapterSourceKind::NativeKst;
        SampleRecord tr;
        tr.id = ResourceId{false, "KST", Kind::Sample, 0};
        tr.hasPcm = true;
        tr.isStoredSizeProxy = true;
        tr.payloadBytes = 100000;
        synthTarget.samples.emplace(tr.id, tr);
        synthTarget.manifestSha256 = std::string(64, 'a');

        const DryRunExportPlan mixedPlan = collectSelectiveExport(view, req, &synthTarget);
        TEST("provisional + Native-KST target baseline switches to unverified-proxy caveat",
             mixedPlan.memoryProjection.sampleBytesAreNativeStoredProxy &&
                 mixedPlan.memoryProjection.unit == "native_kst_stored_pcm_proxy_bytes" &&
                 mixedPlan.memoryProjection.fitVerdict == "unverified-proxy" &&
                 mixedPlan.memoryProjection.targetBaselineSampleBytes == 100000 &&
                 mixedPlan.memoryProjection.projectedCombinedBytes == 106000);
    }
}

// ── Test 3: Tier-3 Native KST Content Equivalence (Identical vs Different vs InsufficientEvidence) & HOLD Gates ───────

static void testTier3NativeEquivalenceAndHoldGates() {
    std::printf("[Test 3] Tier-3 Native KST 3-way equivalence (Identical/Different/InsufficientEvidence) & HOLD gates\n");

    const RecordLayout L{12, 24, 8};
    const uint32_t kKind = 0x10050038u;
    const Id8 msIdA = makeId(0xC1, 1);
    const Id8 msIdB = makeId(0xC1, 2);
    const Id8 smpIdA = makeId(0xD1, 1);
    const Id8 smpIdB = makeId(0xD1, 2);
    const Id8 unknownSlotId = makeId(0xEE, 9);

    auto makeKst = [&](uint32_t smpABytes, uint8_t s1Fill, bool includeUnanalysedDk, bool addUnresolvedToS2) {
        SynthKmpSpec kmp;
        kmp.samples.push_back({smpIdA, "SmpA", 48000});
        kmp.samples.push_back({smpIdB, "SmpB", 48000});
        kmp.zones.push_back({0, 60});
        kmp.zones.push_back({1, 127});
        kmp.multis.push_back({msIdA, "MsA", 0, 1});
        kmp.multis.push_back({msIdB, "MsB", 1, 1});

        const Bytes s0Body = buildSoundBody(L, {{msIdA, kZeroId}}, 0x5Au);
        const Bytes s1Body = buildSoundBody(L, {{msIdB, kZeroId}}, s1Fill);
        const Bytes s2Body = buildSoundBody(L, {{msIdA, addUnresolvedToS2 ? unknownSlotId : kZeroId}}, 0x5Au);
        const Bytes noRefBody = buildSoundBody(L, {{kZeroId, kZeroId}}, 0x33u); // Gap::NoKnownReference

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
            Bytes dkBody(19, 0x77u); // does not fit layout -> SizeDoesNotFit
            in.downstream.soundContainers.push_back({"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kKind, 0, 0, 0, "KitOne", makeOcLiteralFrame("OC31", dkBody), ""}
            })});
        }
        // Styles
        in.upstreamRoots.push_back({"STYLE/BankA/FactoryOnly.stg", rtSerialize({rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 0, 5}}))})})})});
        in.upstreamRoots.push_back({"STYLE/BankA/TargetMatched.stg", rtSerialize({rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 64, 0}}))})})})});
        in.upstreamRoots.push_back({"STYLE/BankA/SameNameDiff.stg", rtSerialize({rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({{0, 121, 64, 1}}))})})})});
        return in;
    };

    M1AdapterOptions aopts;
    aopts.closureOptions.downstream.layouts[kKind] = L;
    aopts.closureOptions.downstream.allowFitLayout = false;

    const M1AdapterView srcView = adaptNativeKstInput(makeKst(3000, 0x5Au, true, true), aopts);
    const M1AdapterView dstViewSame = adaptNativeKstInput(makeKst(3000, 0x6Bu, true, true), aopts);
    const M1AdapterView dstViewDiffPcmSize = adaptNativeKstInput(makeKst(3200, 0x5Au, true, false), aopts);

    const ResourceId s0{false, "USER01", Kind::Sound, 0};
    const ResourceId s1{false, "USER01", Kind::Sound, 1};
    const ResourceId s2{false, "USER01", Kind::Sound, 2};
    const ResourceId s3{false, "USER01", Kind::Sound, 3};
    const ResourceId dk0{false, "USERDK01", Kind::Sound, 0};

    const auto eqIdentical = classifyNativeSoundEquivalence(srcView, s0, dstViewSame, s0);
    const auto eqDiffBody = classifyNativeSoundEquivalence(srcView, s1, dstViewSame, s1);
    const auto eqDiffPcm = classifyNativeSoundEquivalence(srcView, s0, dstViewDiffPcmSize, s0);
    const auto eqUnresSlot = classifyNativeSoundEquivalence(srcView, s2, dstViewSame, s2);
    const auto eqNoRef = classifyNativeSoundEquivalence(srcView, s3, dstViewSame, s3);
    const auto eqDk = classifyNativeSoundEquivalence(srcView, dk0, dstViewSame, dk0);

    TEST("Tier-3 Identical proven (SHA-256 + Analysed + Id8/size match)",
         eqIdentical.verdict == NativeEquivalenceVerdict::Identical && !eqIdentical.mergesPa5xId8);
    TEST("Tier-3 same-name different-body -> Different (NEVER merged)",
         eqDiffBody.verdict == NativeEquivalenceVerdict::Different);
    TEST("Tier-3 same-body different-PCM-size -> Different",
         eqDiffPcm.verdict == NativeEquivalenceVerdict::Different);
    TEST("Tier-3 unresolved slot in analysed Sound -> InsufficientEvidence",
         eqUnresSlot.verdict == NativeEquivalenceVerdict::InsufficientEvidence);
    TEST("Tier-3 NoKnownReference Sound -> InsufficientEvidence",
         eqNoRef.verdict == NativeEquivalenceVerdict::InsufficientEvidence);
    TEST("Tier-3 unanalysed USERDK -> InsufficientEvidence",
         eqDk.verdict == NativeEquivalenceVerdict::InsufficientEvidence);

    // RootTreeSubKst mode: FactoryOnly -> executable; TargetMatched -> executable; SameNameDiff -> BLOCKED (HOLD)
    {
        CollectorRequest req;
        req.mode = CollectorExportMode::RootTreeSubKst;
        req.targets = {{"STYLE/BankA/FactoryOnly.stg", "", ""}};
        const DryRunExportPlan p = collectSelectiveExport(srcView, req, &dstViewSame);
        TEST("RootTreeSubKst FactoryOnly executable", p.ok && p.executable);
    }
    {
        CollectorRequest req;
        req.mode = CollectorExportMode::RootTreeSubKst;
        req.targets = {{"STYLE/BankA/TargetMatched.stg", "", ""}};
        const DryRunExportPlan p = collectSelectiveExport(srcView, req, &dstViewSame);
        TEST("RootTreeSubKst TargetMatched executable (reuses identical target)",
             p.ok && p.executable && p.collectedSounds.size() == 1 && p.collectedSounds[0].satisfiedByTarget);
    }
    {
        CollectorRequest req;
        req.mode = CollectorExportMode::RootTreeSubKst;
        req.targets = {{"STYLE/BankA/SameNameDiff.stg", "", ""}};
        const DryRunExportPlan p = collectSelectiveExport(srcView, req, &dstViewSame);
        TEST("RootTreeSubKst SameNameDiff blocked with NATIVE_SOUND_WRITE_HOLD",
             p.ok && !p.executable && hasBlocker(p, BlockingCode::NATIVE_SOUND_WRITE_HOLD));
    }
    {
        CollectorRequest req;
        req.mode = CollectorExportMode::NativeSoundKst_HOLD;
        req.targets = {{"STYLE/BankA/FactoryOnly.stg", "", ""}};
        const DryRunExportPlan p = collectSelectiveExport(srcView, req);
        TEST("NativeSoundKst_HOLD ALWAYS fails closed",
             p.ok && !p.executable && hasBlocker(p, BlockingCode::NATIVE_SOUND_WRITE_HOLD));
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
    kmp.samples.push_back({smpMissingPcm, "SmpNoPcm", 48000}); // absent from PCM!
    kmp.drums.push_back({dupDrumId, "Dr1", 0, kNoZoneSample});
    kmp.drums.push_back({dupDrumId, "Dr2", 0, kNoZoneSample}); // duplicate DrumSample ID!
    kmp.zones.push_back({0, 60});
    kmp.zones.push_back({1, 127});
    kmp.multis.push_back({msId, "MsMixed", 0, 2});

    // Sound 0: analysed with 1 unresolved slot (inUnanalysedSound == false) and reaches smpMissingPcm!
    // Sound 1: unanalysed NoKnownReference with 1 non-empty slot (inUnanalysedSound == true)!
    const Bytes s0Body = buildSoundBody(L, {{msId, badSlotId}});
    const Bytes s1Body = buildSoundBody(L, {{badSlotId, kZeroId}});
    const Bytes dkBody(19, 0x44u); // SizeDoesNotFit -> unanalysed USERDK

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

    // Root 1: missing slot (USER01/99), missing bank (USER05/0), out-of-spec DK bank (120/72/0), unknown MSB (99/5/0), missing CC0/CC32
    in.upstreamRoots.push_back({"STYLE/BankA/BrokenRefs.stg", rtSerialize({
        rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({
            {0, 121, 64, 99}, // slot empty
            {1, 121, 68, 0},  // bank absent (USER05)
            {2, 120, 72, 0},  // AmbiguousOrOutOfSpec (USERDK09 absent; NEVER coerced to Factory!)
            {3, 99, 5, 0},    // UnknownOrUnanalysed (NEVER coerced to Factory!)
            {4, -1, -1, 7}    // missing bank select
        }))})})
    })});
    // Root 2: malformed CC11 >= 0x80 leaf
    in.upstreamRoots.push_back({"PAD/BankA/BadPad.pdc", rtSerialize({rtLeaf("Main.mid", makeSmf({{0, 121, 0, 0}}, true))})});
    // Root 3: USERDK01/0 + AnalysedWithUnres + NoRefSpeculative
    in.upstreamRoots.push_back({"STYLE/BankA/DkAndUnres.stg", rtSerialize({
        rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf({
            {0, 120, 64, 0}, // USERDK01/0
            {1, 121, 64, 0}, // USER01/0 (analysed, unresolved slot + missing PCM!)
            {2, 121, 64, 1}  // USER01/1 (NoKnownReference + speculative slot!)
        }))})})
    })});

    M1AdapterOptions aopts;
    aopts.closureOptions.downstream.layouts[kKind] = L;
    aopts.closureOptions.downstream.allowFitLayout = false;
    const M1AdapterView view = adaptNativeKstInput(in, aopts);

    // Check strict boundary classification on BrokenRefs.stg
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
        req.targets = {{"STYLE/BankA/BrokenRefs.stg", "", ""}, {"PAD/BankA/BadPad.pdc", "", ""}};
        const DryRunExportPlan p = collectSelectiveExport(view, req);
        TEST("BrokenRefs + BadPad fail closed with exact codes",
             !p.executable &&
                 hasBlocker(p, BlockingCode::MISSING_USER_SOUND_SLOT) &&
                 hasBlocker(p, BlockingCode::MISSING_USER_SOUND_BANK) &&
                 hasBlocker(p, BlockingCode::UNRESOLVED_MIDI_BANK_SELECT) &&
                 hasBlocker(p, BlockingCode::UNKNOWN_ORIGIN_MIDI_ADDRESS) &&
                 hasBlocker(p, BlockingCode::MALFORMED_MIDI_LEAF) &&
                 p.factorySoundSelectCount == 0);
    }

    {
        CollectorRequest req;
        req.targets = {{"STYLE/BankA/DkAndUnres.stg", "", ""}};
        req.requireCompleteSampleClosure = true;
        const DryRunExportPlan p = collectSelectiveExport(view, req);
        TEST("DkAndUnres blocks on USERDK & missing PCM, separates UnresolvedSlot",
             !p.executable && !p.coverageComplete &&
                 hasBlocker(p, BlockingCode::DRUMKIT_BODY_UNPARSED) &&
                 hasBlocker(p, BlockingCode::MISSING_PCM_FOR_KMP_SAMPLE) &&
                 p.memoryProjection.unknownSizeSamplesCount == 1 &&
                 p.unresolvedSlotsInAnalysedCount == 1 &&
                 p.unresolvedSlotsInUnanalysedCount == 1 &&
                 hasNotice(p, "UNRESOLVED_SOUND_SLOT_ANALYSED", "warning") &&
                 hasNotice(p, "UNRESOLVED_SOUND_SLOT_SPECULATIVE", "info") &&
                 hasNotice(p, "SOUND_NO_RAM_REF_INFERRED_ROM", "info") &&
                 hasNotice(p, "DUPLICATE_KMP_DRUM_ID", "warning"));
    }

    // OC31-encoded RAM.KMP -> OC31_KMP_UNSUPPORTED
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
        TEST("OC31 KMP detected cleanly while Sound catalog stays populated",
             ocView.ok && ocView.coverage.kmpIsOc31 && ocView.sounds.size() == 1);

        CollectorRequest req;
        req.targets = {{"STYLE/BankA/LegacyStyle.stg", "", ""}};
        req.requireCompleteSampleClosure = true;
        const DryRunExportPlan p = collectSelectiveExport(ocView, req);
        TEST("OC31 KMP fails closed with OC31_KMP_UNSUPPORTED",
             !p.executable && !p.coverageComplete && hasBlocker(p, BlockingCode::OC31_KMP_UNSUPPORTED));
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

    // Cancel mid-adaptation
    size_t adaptCalls = 0;
    const M1AdapterView cancelledView = adaptNativeKstInput(in, aopts, [&]() { return ++adaptCalls <= 3; });
    TEST("mid-scan adapt cancelled cleanly with empty partial state",
         cancelledView.cancelled && !cancelledView.ok && cancelledView.roots.empty() &&
             cancelledView.sounds.empty() && cancelledView.samples.empty());

    // Build full view, then cancel mid-collection (even with stride > 256, clamped to <= 256!)
    const M1AdapterView fullView = adaptNativeKstInput(in, aopts);
    CollectorRequest req;
    req.cancelCheckStride = 9999; // clamped to <= 256
    for (const auto& r : fullView.roots) req.targets.push_back({r.relPath, "", ""});

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
    reqA.targets = {{"STYLE/BankZ/Zeta.stg", "", ""}, {"STYLE/BankA/Alpha.stg", "", ""}, {"PAD/BankM/MidPad.pdc", "", ""}};
    CollectorRequest reqB;
    reqB.targets = {{"PAD/BankM/MidPad.pdc", "", ""}, {"STYLE/BankZ/Zeta.stg", "", ""}, {"STYLE/BankA/Alpha.stg", "", ""}};

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
         json1.find(kStructuralOnlyBanner) != std::string::npos &&
             md1.find(kStructuralOnlyBanner) != std::string::npos);

    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
}

int main() {
    std::printf("=== Slice M2b Selective Collector & 3-Tier Deduplicator Synthetic Tests ===\n");
    testMultiStyleSharingAndEmbeddedFlags();
    testProvisionalTier2DedupAndMemoryProjection();
    testTier3NativeEquivalenceAndHoldGates();
    testFailClosedBlockingAndBoundaries();
    testCooperativeCancellation();
    testReproducibilityJsonAndPathFree();
    std::printf("=== Summary: %d passed, %d failed ===\n", gPasses, gFailures);
    return gFailures == 0 ? 0 : 1;
}
