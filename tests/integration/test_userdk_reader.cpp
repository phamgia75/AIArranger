// Strictly Read-Only USERDK Drum Kit Parser, Resource Graph Integration & 3-Way Equivalence Tests.
// 100% synthetic fixtures (zero owner bytes, zero owner names, zero local paths).
// Covers T1..T18 + DrumKitKeySummary / IDrumKitKeyMap readout + advisory RAM.KMP +113 drumFamilyByte +
// pure data-driven fitDrumKitLayout() + unified NativeKstClosure / M1AdapterView / TargetOccupancyMap parity.

#include "media/panative/oc31.h"
#include "media/panative/pa5x_kmp.h"
#include "media/panative/panative_reader.h"
#include "media/panative/root_tree.h"
#include "media/sound/m1_manifest_adapter.h"
#include "media/sound/native_kst_closure.h"
#include "media/sound/native_refs.h"
#include "media/sound/resource_graph.h"
#include "media/sound/selective_collector.h"
#include "media/sound/slot_conflicts.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ai_arranger::media;
using namespace ai_arranger::media::graph;
using namespace ai_arranger::media::panative;
using namespace ai_arranger::media::selective;

static int passes = 0, failures = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

using Bytes = std::vector<uint8_t>;

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

static void putBe16(Bytes& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v & 0xFFu);
}

static void putBe32(Bytes& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v >> 24);
    b[at + 1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    b[at + 2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    b[at + 3] = static_cast<uint8_t>(v & 0xFFu);
}

static Id8 makeId(uint8_t prefix, uint8_t low) {
    return Id8{prefix, 0, 0, 0, 0, 0, 0, low};
}

static std::string idAsStr(const Id8& id) {
    return std::string(reinterpret_cast<const char*>(id.data()), 8);
}

// Pass decoded synthetic body bytes directly inside raw KORF containers (strictly read-only: zero OC31 encoder).
static const Bytes& rawKorfPayload(const Bytes& raw) {
    return raw;
}

struct SynthKorfItem {
    uint32_t chunkId = 0;
    uint8_t kind = 4;
    uint8_t bank = 11;
    uint8_t slot = 0;
    std::string name;
    Bytes payload;
    std::string sampleId8;
};

static Bytes buildKorfContainer(const std::vector<SynthKorfItem>& items) {
    Bytes dir;
    for (const auto& it : items) {
        Bytes rec;
        const uint16_t nf = it.sampleId8.empty() ? 3u : 4u;
        pushBe16(rec, nf);
        // field 0: kind, bank, slot
        pushBe16(rec, 0); pushBe16(rec, 3);
        rec.push_back(it.kind); rec.push_back(it.bank); rec.push_back(it.slot);
        // field 1: sub = (chunkId >> 8) & 0xFFFF
        pushBe16(rec, 1); pushBe16(rec, 2);
        pushBe16(rec, static_cast<uint16_t>((it.chunkId >> 8) & 0xFFFFu));
        // field 2: name (NUL-terminated)
        pushBe16(rec, 2); pushBe16(rec, static_cast<uint16_t>(it.name.size() + 1u));
        rec.insert(rec.end(), it.name.begin(), it.name.end());
        rec.push_back(0);
        if (!it.sampleId8.empty()) {
            pushBe16(rec, 4); pushBe16(rec, 8);
            rec.insert(rec.end(), it.sampleId8.begin(), it.sampleId8.end());
        }
        // 4-byte directory record trailer
        pushBe32(rec, 0);
        pushBe16(dir, static_cast<uint16_t>(rec.size()));
        dir.insert(dir.end(), rec.begin(), rec.end());
    }

    Bytes body;
    // Header chunk 0x02000018 (12 bytes; "KORF" at offset 7 inside payload = file offset 0x17)
    pushBe32(body, 0x02000018u);
    pushBe32(body, 12u);
    body.insert(body.end(), {0, 0, 0, 0, 0, 0, 0, 'K', 'O', 'R', 'F', 0});

    // Directory chunk 0x05010018 (format 1 TLV)
    std::vector<uint32_t> offsets;
    offsets.push_back(static_cast<uint32_t>(8u + body.size()));
    pushBe32(body, 0x05010018u);
    pushBe32(body, static_cast<uint32_t>(dir.size()));
    body.insert(body.end(), dir.begin(), dir.end());

    for (const auto& it : items) {
        offsets.push_back(static_cast<uint32_t>(8u + body.size()));
        pushBe32(body, it.chunkId);
        pushBe32(body, static_cast<uint32_t>(it.payload.size()));
        body.insert(body.end(), it.payload.begin(), it.payload.end());
    }

    // Index chunk 0xFE000018
    Bytes idx;
    idx.insert(idx.end(), {'K', 'B', 'E', 'G'});
    for (uint32_t off : offsets) pushBe32(idx, off);
    idx.insert(idx.end(), {'K', 'E', 'N', 'D'});
    pushBe32(idx, static_cast<uint32_t>(items.size() + 1u));
    pushBe32(body, 0xFE000018u);
    pushBe32(body, static_cast<uint32_t>(idx.size()));
    body.insert(body.end(), idx.begin(), idx.end());

    Bytes out;
    pushBe32(out, 0x01000114u);
    pushBe32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Synthetic Root-tree + SMF builders (using roottree::write directly).
static roottree::Node rtLeaf(const std::string& name, const Bytes& data) {
    roottree::Node n;
    n.kind = roottree::Node::Leaf;
    n.name = name;
    n.payload.assign(reinterpret_cast<const char*>(data.data()), data.size());
    return n;
}

static roottree::Node rtBranch(const std::string& name, std::vector<roottree::Node> children) {
    roottree::Node n;
    n.kind = roottree::Node::Branch;
    n.name = name;
    n.children = std::move(children);
    return n;
}

static Bytes rtSerialize(std::vector<roottree::Node> topChildren) {
    roottree::Node r;
    r.kind = roottree::Node::Root;
    r.name = "\x01";
    r.children = std::move(topChildren);
    std::string out, err;
    if (!roottree::write(r, out, err)) return {};
    return Bytes(out.begin(), out.end());
}

struct SmfSel { uint8_t ch; int msb; int lsb; uint8_t pc; };

static Bytes makeSmf(const std::vector<SmfSel>& sels) {
    Bytes trk;
    for (const auto& s : sels) {
        if (s.msb >= 0) {
            trk.push_back(0x00);
            trk.push_back(static_cast<uint8_t>(0xB0u | (s.ch & 0x0Fu)));
            trk.push_back(0x00);
            trk.push_back(static_cast<uint8_t>(s.msb));
        }
        if (s.lsb >= 0) {
            trk.push_back(0x00);
            trk.push_back(static_cast<uint8_t>(0xB0u | (s.ch & 0x0Fu)));
            trk.push_back(0x20);
            trk.push_back(static_cast<uint8_t>(s.lsb));
        }
        trk.push_back(0x00);
        trk.push_back(static_cast<uint8_t>(0xC0u | (s.ch & 0x0Fu)));
        trk.push_back(s.pc);
    }
    trk.insert(trk.end(), {0x00, 0xFF, 0x2F, 0x00});
    Bytes smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k'};
    pushBe32(smf, static_cast<uint32_t>(trk.size()));
    smf.insert(smf.end(), trk.begin(), trk.end());
    return smf;
}

static Bytes makeStyleTree(const std::vector<SmfSel>& sels) {
    return rtSerialize({rtBranch("Var1.scn", {rtBranch("T1.pdc", {rtLeaf("Main.mid", makeSmf(sels))})})});
}

// ── Synthetic RAM.KMP + PCM Universe (Spec §2) ────────────────────────────────────────────────────────────────────────

struct SynthSampleSpec { Id8 id; std::string name; uint16_t rate; std::string pcmFile; uint32_t pcmBytes; };
struct SynthDrumSpec { Id8 id; std::string name; uint32_t left; uint32_t right; uint8_t familyByte113; };
struct SynthZoneSpec { uint32_t sample; uint8_t topKey; };
struct SynthMsSpec { Id8 id; std::string name; uint32_t firstZone; uint8_t zoneCount; };

static Bytes buildRawKmpPayload(const std::vector<SynthSampleSpec>& samples,
                                const std::vector<SynthDrumSpec>& drums,
                                const std::vector<SynthZoneSpec>& zones,
                                const std::vector<SynthMsSpec>& multis) {
    const size_t ns = samples.size(), nd = drums.size(), nz = zones.size(), nm = multis.size();
    Bytes b(18u + 122u * ns + 126u * nd + 16u * nz + 176u * nm, 0x00);
    putBe16(b, 0, 0x0103u);
    putBe32(b, 2, static_cast<uint32_t>(ns));
    putBe32(b, 6, static_cast<uint32_t>(nd));
    putBe32(b, 10, static_cast<uint32_t>(nz));
    putBe32(b, 14, static_cast<uint32_t>(nm));
    const size_t S = 18u, D = S + 122u * ns, Z = D + 126u * nd, M = Z + 16u * nz;
    auto putName = [&](size_t at, const std::string& name) {
        const size_t n = std::min<size_t>(31u, name.size());
        std::memcpy(b.data() + at, name.data(), n);
    };
    for (size_t i = 0; i < ns; ++i) {
        putName(S + 122u * i + 75u, samples[i].name);
        std::copy(samples[i].id.begin(), samples[i].id.end(), b.begin() + static_cast<std::ptrdiff_t>(S + 122u * i + 107u));
        putBe16(b, S + 122u * i + 119u, samples[i].rate);
    }
    for (size_t i = 0; i < nd; ++i) {
        putName(D + 126u * i, drums[i].name);
        putBe32(b, D + 126u * i + 32u, drums[i].left);
        putBe32(b, D + 126u * i + 36u, drums[i].right);
        b[D + 126u * i + 112u] = 0x01u;
        b[D + 126u * i + 113u] = drums[i].familyByte113;
        std::copy(drums[i].id.begin(), drums[i].id.end(), b.begin() + static_cast<std::ptrdiff_t>(D + 126u * i + 118u));
    }
    for (size_t i = 0; i < nz; ++i) {
        putBe32(b, Z + 16u * i, zones[i].sample);
        b[Z + 16u * i + 10u] = zones[i].topKey;
    }
    for (size_t i = 0; i < nm; ++i) {
        putName(M + 176u * i, multis[i].name);
        putBe32(b, M + 176u * i + 32u, multis[i].firstZone);
        b[M + 176u * i + 166u] = multis[i].zoneCount;
        std::copy(multis[i].id.begin(), multis[i].id.end(), b.begin() + static_cast<std::ptrdiff_t>(M + 176u * i + 168u));
    }
    return b;
}

struct SynthUniverse {
    std::vector<SynthSampleSpec> samples;
    std::vector<SynthDrumSpec> drums;
    std::vector<SynthZoneSpec> zones;
    std::vector<SynthMsSpec> multis;
    Bytes kmpContainer;
    Bytes pcm1Container;
    Bytes pcm2Container;

    SynthUniverse() {
        samples = {
            {makeId(0xA0, 1),  "SYN_KICK_SOFT_L", 44100, "PCM/ram01.pcm", 20082},
            {makeId(0xA0, 2),  "SYN_KICK_SOFT_R", 44100, "PCM/ram01.pcm", 20082},
            {makeId(0xA0, 3),  "SYN_KICK_HARD_L", 44100, "PCM/ram01.pcm", 24082},
            {makeId(0xA0, 4),  "SYN_KICK_HARD_R", 44100, "PCM/ram01.pcm", 24082},
            {makeId(0xA0, 5),  "SYN_SNARE_M",     44100, "PCM/ram01.pcm", 16082},
            {makeId(0xA0, 6),  "SYN_HH_CLOSED_M", 44100, "PCM/ram02.pcm", 12082},
            {makeId(0xA0, 7),  "SYN_HH_OPEN_M",   44100, "PCM/ram02.pcm", 32082},
            {makeId(0xA0, 8),  "SYN_SHARED_PERC", 48000, "PCM/ram02.pcm", 10082},
            {makeId(0xA0, 9),  "SYN_DUP_AUDIO_M", 44100, "PCM/ram02.pcm", 8082},
            {makeId(0xA0, 10), "SYN_MISSING_PCM", 44100, "",              0},
            {makeId(0xA0, 11), "SYN_UNUSED_DRUM", 44100, "PCM/ram02.pcm", 4082},
        };
        drums = {
            {makeId(0xD0, 1), "SYN_DRUM_KICK_S",  0,  1,        0},
            {makeId(0xD0, 2), "SYN_DRUM_KICK_H",  2,  3,        0},
            {makeId(0xD0, 3), "SYN_DRUM_SNARE",   4,  kNoIndex, 1},
            {makeId(0xD0, 4), "SYN_DRUM_HH_CL",   5,  kNoIndex, 2},
            {makeId(0xD0, 5), "SYN_DRUM_HH_OP",   6,  kNoIndex, 2},
            {makeId(0xD0, 6), "SYN_DRUM_DUP_A",   8,  kNoIndex, 5},
            {makeId(0xD0, 6), "SYN_DRUM_DUP_B",   8,  kNoIndex, 5},
            {makeId(0xD0, 7), "SYN_DRUM_NOPCM_R", 7,  9,        6},
            {makeId(0xD0, 8), "SYN_DRUM_ORPHAN",  10, kNoIndex, 7},
            // Extra valid dual-use DrumSample referencing Sample/7 (valid PCM) for T5:
            {makeId(0xD0, 9), "SYN_DRUM_DUALUSE", 7,  kNoIndex, 5},
        };
        zones = {{7, 127}};
        multis = {{makeId(0xB0, 1), "SYN_MS_DUALUSE", 0, 1}};

        kmpContainer = buildKorfContainer({
            {kPa5xKmpChunkId, 6, 0, 0, "RAM.KMP", buildRawKmpPayload(samples, drums, zones, multis), ""}
        });
        std::vector<SynthKorfItem> p1, p2;
        uint8_t s1 = 0, s2 = 0;
        for (const auto& s : samples) {
            if (s.pcmFile == "PCM/ram01.pcm") {
                p1.push_back({0x12020010u, 6, 0, s1++, s.name, Bytes(s.pcmBytes, 0x11), idAsStr(s.id)});
            } else if (s.pcmFile == "PCM/ram02.pcm") {
                p2.push_back({0x12020010u, 6, 0, s2++, s.name, Bytes(s.pcmBytes, 0x22), idAsStr(s.id)});
            }
        }
        pcm1Container = buildKorfContainer(p1);
        pcm2Container = buildKorfContainer(p2);
    }

    NativeKstInput makeBaseInput() const {
        NativeKstInput in;
        in.kmpContainer = RawNamedBlob{"MULTISMP/RAM.KMP", "", kmpContainer};
        in.pcmContainers = {
            {"PCM/ram01.pcm", "", pcm1Container},
            {"PCM/ram02.pcm", "", pcm2Container},
        };
        return in;
    }
};

// ── Synthetic USERDK Body Builder (Spec §1 & §4) ──────────────────────────────────────────────────────────────────────

struct SynthDrumLayer {
    uint8_t  sourceBank = 0;
    uint16_t tableIndex = 0;
    uint16_t gain       = 0x8000u;
    int8_t   transpose  = 0;
    int8_t   fineTune   = 0;
    Id8      id8{};
};

struct SynthDrumKey {
    uint16_t keyGain = 0x7FFFu;
    uint8_t  pan     = 64;
    uint8_t  choke   = 0;
    std::vector<uint8_t> velTops = {127};
    std::vector<SynthDrumLayer> layers = {{}};
};

struct SynthDkProfile {
    uint32_t kind = 0x10050038u;
    uint32_t hdr = 514;
    uint32_t modeFlagOffset = 2;
    uint32_t kst = 20;
    uint32_t lcOff = 2;
    uint32_t lst = 32;
    bool hasId8 = true;
};

static const SynthDkProfile kProf1001     {0x10010038u, 367, 18, 18, 0, 20, true};
static const SynthDkProfile kProf1002     {0x10020038u, 518, 18, 18, 0, 30, true};
static const SynthDkProfile kProf1003Oc31 {0x10030038u, 502,  2, 18, 0, 30, true};
static const SynthDkProfile kProf1003Oc32 {0x10030038u, 502,  2, 18, 0, 22, false};
static const SynthDkProfile kProf1005     {0x10050038u, 514,  2, 20, 2, 32, true};
static const SynthDkProfile kProf1006     {0x10060038u, 516,  2, 20, 2, 32, true};
static const SynthDkProfile kProf1007     {0x10070038u, 653,  2, 22, 2, 33, true};

static std::array<SynthDrumKey, 128> makeDefaultKeys() {
    std::array<SynthDrumKey, 128> keys{};
    return keys;
}

static Bytes buildSynthUserDkBody(const SynthDkProfile& p, const std::array<SynthDrumKey, 128>& keys,
                                  uint8_t modeFlag = 0x82u) {
    uint32_t totalLayers = 0;
    for (const auto& k : keys) totalLayers += static_cast<uint32_t>(k.layers.size());
    const size_t prefix = size_t(p.hdr) + 128u * size_t(p.kst);
    Bytes body(prefix + size_t(totalLayers) * size_t(p.lst), 0x00);
    putBe16(body, 0, 0x0014u);
    body[p.modeFlagOffset] = modeFlag;

    uint32_t cum = 0;
    size_t lIdx = 0;
    for (size_t k = 0; k < 128u; ++k) {
        const auto& dk = keys[k];
        const size_t ko = size_t(p.hdr) + k * size_t(p.kst);
        const uint8_t lc = static_cast<uint8_t>(dk.layers.size());
        if (p.kst >= 20u) putBe16(body, ko, dk.keyGain);
        body[ko + p.lcOff] = lc;
        body[ko + p.lcOff + 1u] = dk.pan;
        body[ko + p.lcOff + 3u] = 0x28u;
        body[ko + p.lcOff + 4u] = 0x60u;
        if (p.kst == 18u && p.lst == 20u) {
            body[ko + 5u] = dk.choke;
            body[ko + 6u] = 0x05u;
        } else {
            body[ko + p.lcOff + 5u] = 0x60u;
            body[ko + p.lcOff + 6u] = dk.choke;
            body[ko + p.kst - 11u] = 0x05u;
        }
        const size_t velOff = p.kst - 10u;
        for (size_t j = 0; j < 8u; ++j) {
            body[ko + velOff + j] = (j < dk.velTops.size()) ? dk.velTops[j] : 0x7Fu;
        }
        cum += lc;
        const uint16_t rawEnd = static_cast<uint16_t>(cum | ((k == 127u && p.hasId8) ? 0x8000u : 0u));
        putBe16(body, ko + p.kst - 2u, rawEnd);

        for (const auto& ly : dk.layers) {
            const size_t lo = prefix + lIdx * size_t(p.lst);
            ++lIdx;
            body[lo + 0u] = ly.sourceBank;
            body[lo + 1u] = 0x00u;
            putBe16(body, lo + 2u, ly.tableIndex);
            if (p.lst >= 32u) {
                putBe16(body, lo + 4u, ly.gain);
                body[lo + 6u] = static_cast<uint8_t>(ly.transpose);
                body[lo + 7u] = static_cast<uint8_t>(ly.fineTune);
                body[lo + 12u] = 0x1Fu; body[lo + 13u] = 0x52u; body[lo + 14u] = 0x0Au;
                body[lo + 15u] = 1u; body[lo + 16u] = 0x63u; body[lo + 18u] = 0x5Au;
                if (p.lst == 33u) body[lo + 22u] = 0x04u;
                body[lo + p.lst - 10u] = 0x0Fu;
                body[lo + p.lst - 9u] = 0x00u;
            } else {
                body[lo + 5u] = static_cast<uint8_t>(ly.transpose);
                body[lo + 6u] = static_cast<uint8_t>(ly.fineTune);
            }
            if (p.hasId8) {
                std::copy(ly.id8.begin(), ly.id8.end(), body.begin() + static_cast<std::ptrdiff_t>(lo + p.lst - 8u));
            }
        }
    }
    return body;
}

static std::array<SynthDrumKey, 128> makeT1Keys() {
    auto keys = makeDefaultKeys();
    keys[36] = {0x7FFFu, 64, 0, {64, 127}, {
        {1, 0, 0x5A9Eu, 0, 0, makeId(0xD0, 1)},
        {1, 1, 0xFF65u, -1, +25, makeId(0xD0, 2)},
    }};
    keys[38] = {0x7FFFu, 64, 0, {127}, {{1, 2, 0x8000u, 0, 0, makeId(0xD0, 3)}}};
    keys[42] = {0x7FFFu, 90, 1, {127}, {{1, 3, 0x8000u, 0, 0, makeId(0xD0, 4)}}};
    keys[46] = {0x7FFFu, 90, 1, {127}, {{1, 4, 0x8000u, 0, 0, makeId(0xD0, 5)}}};
    return keys;
}

static bool anyDeleteUnreferenced(const Graph& g) {
    for (const auto& [id, node] : g.nodes()) {
        (void)node;
        if (g.deleteCheck(id).verdict == DeleteVerdict::Unreferenced) return true;
    }
    return false;
}

// ── Test Groups ───────────────────────────────────────────────────────────────────────────────────────────────────────

static void testGroupA_PositiveControls(const SynthUniverse& U) {
    std::printf("\n[Group A] Valid Positive-Control USERDK Fixtures (T1..T5)\n");

    // T1: Valid multi-layer + stereo L/R + choke + advisory RAM.KMP +113 drumFamilyByte + DrumKitKeySummary
    const Bytes t1Body = buildSynthUserDkBody(kProf1005, makeT1Keys());
    {
        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 0, "SYN_KIT_STD", rawKorfPayload(t1Body), ""}
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        TEST("T1: valid 10050038 USERDK parses cleanly with 0 unresolved slots",
             res.ok && res.report.ok() && res.graph.has_value() &&
             res.report.refs.analysed == 1 && res.report.refs.drumKitsAnalysed == 1 &&
             res.report.refs.ramDrumLayers == 5 && res.report.refs.romDrumLayers == 124 &&
             res.report.refs.unresolved.empty() && res.report.refs.unanalysed.empty());

        const ResourceId dkId{false, "USERDK01", Kind::Sound, 0};
        const auto dkEdges = res.graph->uses(dkId);
        bool allInferredDrumKey = (dkEdges.size() == 5);
        for (const auto& e : dkEdges) {
            if (e.via != Via::DrumKeyLayer || e.confidence != Confidence::Inferred || e.to.kind != Kind::DrumSample) {
                allInferredDrumKey = false;
            }
        }
        const auto deps = dependencies(*res.graph, dkId);
        size_t dsCnt = 0, smpCnt = 0;
        for (const auto& d : deps) {
            dsCnt += (d.kind == Kind::DrumSample);
            smpCnt += (d.kind == Kind::Sample);
        }
        TEST("T1: emits 5 Inferred DrumKeyLayer edges -> 5 DrumSamples -> 7 stereo/mono Samples",
             allInferredDrumKey && dsCnt == 5 && smpCnt == 7);

        TEST("T1: deleteCheck() NEVER returns Unreferenced on any node",
             !anyDeleteUnreferenced(*res.graph));

        // Verify DrumKitKeySummary / IDrumKitKeyMap readout + advisory metadata
        const IDrumKitKeyMap& keyMapIface = res;
        const DrumKitKeyMap* km = keyMapIface.findDrumKitKeyMap(dkId);
        TEST("T1: DrumKitKeyMap readout exposes verified MIDI notes, velTops, resolved indices & advisory fields",
             km != nullptr && km->totalLayers == 129 && km->ramDrumLayers == 5 && km->romDrumLayers == 124 &&
             km->keys[36].layerCount == 2 && km->keys[36].velTop[0] == 64 && km->keys[36].velTop[1] == 127 &&
             km->keys[36].layers.size() == 2 &&
             km->keys[36].layers[0].resolved && km->keys[36].layers[0].drumSampleIndex == 0 &&
             km->keys[36].layers[0].velLo == 1 && km->keys[36].layers[0].velHi == 64 &&
             km->keys[36].layers[0].advisoryDrumFamilyByte == 0 &&
             km->keys[36].layers[1].resolved && km->keys[36].layers[1].drumSampleIndex == 1 &&
             km->keys[36].layers[1].velLo == 65 && km->keys[36].layers[1].velHi == 127 &&
             km->keys[36].layers[1].advisoryTranspose == -1 && km->keys[36].layers[1].advisoryFineTune == 25 &&
             km->keys[36].layers[1].advisoryLayerGainRaw == 0xFF65u &&
             km->keys[42].advisoryExclusiveGroup == 1 && km->keys[42].advisoryPan == 90 &&
             km->keys[42].layers[0].advisoryDrumFamilyByte == 2 &&
             km->keys[46].advisoryExclusiveGroup == 1 && km->keys[38].layers[0].advisoryDrumFamilyByte == 1);

        // Verify TargetOccupancyMap equivalence & provablyComparable() == true
        const TargetOccupancyMap occ = inspectTargetKstInput(in);
        const OccupiedSlotEntry* e0 = occ.findOccupied({true, 1, 0});
        TEST("T1: TargetOccupancyMap marks verified non-duplicate USERDK as provablyComparable",
             occ.fullyInspectable && e0 != nullptr && e0->equivalence.analysed &&
             e0->equivalence.provablyComparable() && !e0->equivalence.equivalenceDigest.empty() &&
             e0->equivalence.referencedDrumSamples.size() == 5 &&
             occ.coverage.analysedSounds == 1 && occ.coverage.unanalysedDrumKits == 0);
    }

    // T2: 8-layer stress fixture (10060038) + Pa5X 10070038 + shared DrumSample counts
    {
        auto keys1006 = makeDefaultKeys();
        keys1006[38].velTops = {16, 32, 48, 64, 80, 96, 112, 127};
        keys1006[38].layers = {
            {1, 2, 0x8000u, 0, 0, makeId(0xD0, 3)}, {0, 0, 0x8000u, 0, 0, {}},
            {1, 2, 0x8000u, 0, 0, makeId(0xD0, 3)}, {0, 0, 0x8000u, 0, 0, {}},
            {1, 2, 0x8000u, 0, 0, makeId(0xD0, 3)}, {0, 0, 0x8000u, 0, 0, {}},
            {1, 2, 0x8000u, 0, 0, makeId(0xD0, 3)}, {0, 0, 0x8000u, 0, 0, {}},
        };
        auto keys1007 = makeDefaultKeys();
        keys1007[36].velTops = {64, 127};
        keys1007[36].layers = {
            {1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)},
            {1, 1, 0x8000u, 0, 0, makeId(0xD0, 2)},
        };
        const Bytes b1006 = buildSynthUserDkBody(kProf1006, keys1006);
        const Bytes b1007 = buildSynthUserDkBody(kProf1007, keys1007);

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 0, "SYN_KIT_STD",  rawKorfPayload(t1Body), ""},
                {kProf1006.kind, 4, 11, 1, "SYN_KIT_8LYR", rawKorfPayload(b1006),  ""},
                {kProf1007.kind, 4, 11, 2, "SYN_KIT_1007", rawKorfPayload(b1007),  ""},
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        const auto sharedDs = sharedCounts(*res.graph, Kind::DrumSample);
        const auto* km6 = res.findDrumKitKeyMap({false, "USERDK01", Kind::Sound, 1});
        const auto* km7 = res.findDrumKitKeyMap({false, "USERDK01", Kind::Sound, 2});
        TEST("T2: 8-layer 10060038 (L=135) & Pa5X 10070038 (L=129) parse cleanly & share DrumSample/0,1,2",
             res.ok && res.report.refs.drumKitsAnalysed == 3 && res.report.refs.unresolved.empty() &&
             km6 != nullptr && km6->totalLayers == 135 && km6->keys[38].layerCount == 8 &&
             km7 != nullptr && km7->totalLayers == 129 &&
             sharedDs.size() == 3 &&
             sharedDs.at({false, "KST", Kind::DrumSample, 0}) == 2 &&
             sharedDs.at({false, "KST", Kind::DrumSample, 1}) == 2 &&
             sharedDs.at({false, "KST", Kind::DrumSample, 2}) == 2);
    }

    // T3: Mixed ROM + Bank-2 + RAM kit & 100% ROM-only kit
    {
        auto keysMix = makeDefaultKeys();
        keysMix[36].layers = {{1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)}};
        keysMix[38].layers = {{2, 86, 0x8000u, 0, 0, {}}}; // Bank-2 zero-ID (UNVERIFIED_HYPOTHESIS non-RAM)
        auto keysRom = makeDefaultKeys();
        for (size_t k = 0; k < 128u; ++k) keysRom[k].layers = {{0, static_cast<uint16_t>(100u + k), 0x8000u, 0, 0, {}}};

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 3, "SYN_KIT_MIX", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysMix)), ""},
                {kProf1005.kind, 4, 11, 4, "SYN_KIT_ROM", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysRom)), ""},
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        const TargetOccupancyMap occ = inspectTargetKstInput(in);
        const auto* eMix = occ.findOccupied({true, 1, 3});
        const auto* eRom = occ.findOccupied({true, 1, 4});
        const auto mixEq = eMix ? compareSoundEquivalence(eMix->equivalence, eMix->equivalence) : EquivalenceResult{};
        TEST("T3: mixed ROM+Bank2+RAM resolves 1 RAM edge & 0 unresolved; Bank-2 downgrades fail-closed to InsufficientEvidence; ROM-only is NoKnownReference & InsufficientEvidence",
             res.ok && res.report.refs.drumKitsAnalysed == 1 && res.report.refs.ramDrumLayers == 1 &&
             res.report.refs.nonRamDrumLayers == 1 && res.report.refs.romDrumLayers == 254 &&
             res.report.refs.unresolved.empty() &&
             res.report.refs.unanalysed.size() == 1 &&
             res.report.refs.unanalysed[0].sound == ResourceId({false, "USERDK01", Kind::Sound, 4}) &&
             res.report.refs.unanalysed[0].why == Gap::NoKnownReference &&
             eMix != nullptr && eMix->equivalence.analysed &&
             eMix->equivalence.hasNonRamBank2Layers && !eMix->equivalence.provablyComparable() &&
             mixEq.verdict == EquivalenceVerdict::InsufficientEvidence &&
             mixEq.reason.find("contains unverified Bank-2 (0x02) drum layers") != std::string::npos &&
             eRom != nullptr && !eRom->equivalence.provablyComparable() &&
             compareSoundEquivalence(eRom->equivalence, eRom->equivalence).verdict == EquivalenceVerdict::InsufficientEvidence);
    }

    // T4: Exact equivalence & name-independence proof (Identical vs Different)
    {
        auto keysTweaked = makeT1Keys();
        keysTweaked[36].layers[1].transpose = -2; // 1-byte tweak vs T1 (-1)
        const Bytes tweakBody = buildSynthUserDkBody(kProf1005, keysTweaked);

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 0, "SYN_KIT_STD", rawKorfPayload(t1Body), ""},
            })},
            {"SOUND/USERDK02.PCG", "USERDK02", buildKorfContainer({
                {kProf1005.kind, 4, 20, 5, "SYN_RENAMED_KIT", rawKorfPayload(t1Body), ""},
                {kProf1005.kind, 4, 20, 6, "SYN_KIT_STD",     rawKorfPayload(tweakBody), ""},
            })},
        };
        const TargetOccupancyMap occ = inspectTargetKstInput(in);
        const auto* pA = occ.findOccupied({true, 1, 0});
        const auto* pRename = occ.findOccupied({true, 2, 5});
        const auto* pTweak = occ.findOccupied({true, 2, 6});
        TEST("T4: identical body with different name -> Identical; same name with 1-byte tweak -> Different",
             pA && pRename && pTweak &&
             compareSoundEquivalence(pA->equivalence, pRename->equivalence).verdict == EquivalenceVerdict::Identical &&
             pA->equivalence.equivalenceDigest == pRename->equivalence.equivalenceDigest &&
             compareSoundEquivalence(pA->equivalence, pTweak->equivalence).verdict == EquivalenceVerdict::Different);
    }

    // T5: Dual-use Sample (Melodic Multisample ∩ USERDK) + unified NativeKstClosure & M1AdapterView
    {
        // Melodic Sound layout: base 24, record 32, pairOffset 12
        const RecordLayout melL{24, 32, 12};
        Bytes melBody(24 + 32, 0x33u);
        const Id8 ms0 = makeId(0xB0, 1);
        std::copy(ms0.begin(), ms0.end(), melBody.begin() + 24);
        std::fill(melBody.begin() + 36, melBody.begin() + 44, 0x00);

        auto keysDual = makeDefaultKeys();
        keysDual[60].layers = {{1, 9, 0x8000u, 0, 0, makeId(0xD0, 9)}}; // drum[9] -> Sample/7

        NativeKstClosureInput cin;
        cin.downstream = U.makeBaseInput();
        cin.downstream.soundContainers = {
            {"SOUND/USER01.PCG", "USER01", buildKorfContainer({
                {0x10050038u, 4, 9, 0, "SYN_MEL_DUAL", rawKorfPayload(melBody), ""}
            })},
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 5, "SYN_DK_DUAL", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysDual)), ""}
            })},
        };
        cin.upstreamRoots = {
            {"STYLE/BankA/DualUse.stg", makeStyleTree({{0, 121, 64, 0}, {9, 120, 64, 5}})}
        };
        NativeKstClosureOptions copts;
        copts.downstream.layouts = {{0x10050038u, melL}};
        copts.downstream.allowFitLayout = false;
        copts.downstream.allowFitDrumKitLayout = true;

        const NativeKstClosureResult cres = resolveNativeKstClosure(cin, copts);
        const ResourceId smp7{false, "KST", Kind::Sample, 7};
        const ResourceId melId{false, "USER01", Kind::Sound, 0};
        const ResourceId dkId{false, "USERDK01", Kind::Sound, 5};
        const auto roots7 = cres.graph->usedByRoots(smp7);
        const auto& M = cres.manifest;
        M1AdapterOptions aopts;
        aopts.closureOptions = copts;
        const M1AdapterView view = adaptNativeKstInput(cin, aopts);
        const auto viewDkIt = view.sounds.find(dkId);
        TEST("T5: dual-use Sample/7 reached by both USER01/0 & USERDK01/5; deleteCheck==InUse; closure & adapter agree",
             cres.ok && M.coverage.drumKitBodyEdgesSupported && !M.coverage.deleteProofAvailable &&
             !anyDeleteUnreferenced(*cres.graph) &&
             cres.graph->deleteCheck(smp7).verdict == DeleteVerdict::InUse &&
             M.roots.size() == 1 && M.roots[0].drumSampleIds.size() == 1 &&
             M.roots[0].drumSampleIds[0] == ResourceId({false, "KST", Kind::DrumSample, 9}) &&
             M.roots[0].storedPcmBytesProxy == 10082u &&
             M.summary.drumSamplesReachableFromRoots == 1 &&
             M.summary.drumSamplesReachableFromAnalysedSounds == 1 &&
             M.summary.sharedSamplesAcrossSounds == 1 &&
             roots7.count(ResourceId({false, "KST", Kind::Style, 0})) == 1 &&
             view.ok && view.coverage.drumKitBodyEdgesSupported &&
             viewDkIt != view.sounds.end() && viewDkIt->second.analysed &&
             viewDkIt->second.drumSampleIds.size() == 1 &&
             viewDkIt->second.equivalence.provablyComparable() &&
             melId.kind == Kind::Sound);
    }
}

static void testGroupB_AnomaliesAndLegacy(const SynthUniverse& U) {
    std::printf("\n[Group B] Anomaly, Duplicate & Legacy Fail-Closed Fixtures (T6..T9)\n");

    // T6: Duplicate DrumSample ID8 (disambiguated via u16 vs mismatching u16 refused fail-closed)
    {
        auto keysGood = makeDefaultKeys();
        keysGood[68].layers = {{1, 5, 0x8000u, 0, 0, makeId(0xD0, 6)}};
        keysGood[74].layers = {{1, 6, 0x8000u, 0, 0, makeId(0xD0, 6)}};
        auto keysBad = makeDefaultKeys();
        keysBad[68].layers = {{1, 99, 0x8000u, 0, 0, makeId(0xD0, 6)}}; // out-of-sync u16!

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK02.PCG", "USERDK02", buildKorfContainer({
                {kProf1001.kind, 4, 20, 10, "SYN_DUP_OK",  rawKorfPayload(buildSynthUserDkBody(kProf1001, keysGood)), ""},
                {kProf1001.kind, 4, 20, 11, "SYN_DUP_BAD", rawKorfPayload(buildSynthUserDkBody(kProf1001, keysBad)),  ""},
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        const ResourceId dk10{false, "USERDK02", Kind::Sound, 10};
        const ResourceId dk11{false, "USERDK02", Kind::Sound, 11};
        const auto edges10 = res.graph->uses(dk10);
        const auto edges11 = res.graph->uses(dk11);
        bool linked5 = false, linked6 = false;
        for (const auto& e : edges10) {
            if (e.to == ResourceId({false, "KST", Kind::DrumSample, 5})) linked5 = true;
            if (e.to == ResourceId({false, "KST", Kind::DrumSample, 6})) linked6 = true;
        }
        const TargetOccupancyMap occ = inspectTargetKstInput(in);
        const auto* e10 = occ.findOccupied({true, 2, 10});
        const auto* e11 = occ.findOccupied({true, 2, 11});
        TEST("T6: duplicate DrumSample ID8 disambiguated via u16 (5 & 6), mismatching u16 refused, both InsufficientEvidence",
             res.ok && linked5 && linked6 && edges11.empty() &&
             res.report.kmp.duplicateDrumIds.size() == 1 &&
             res.report.refs.disambiguatedDuplicateDrumIds == 2 &&
             !res.report.coverageComplete() &&
             res.report.refs.unresolved.size() == 1 && res.report.refs.unresolved[0].sound == dk11 &&
             e10 != nullptr && e10->equivalence.hasDuplicateDrumSampleId && !e10->equivalence.provablyComparable() &&
             compareSoundEquivalence(e10->equivalence, e10->equivalence).verdict == EquivalenceVerdict::InsufficientEvidence &&
             e11 != nullptr && !e11->equivalence.provablyComparable());
    }

    // T7: Dangling RAM DrumSample ID8 & missing PCM on Right channel
    {
        auto keysDang = makeDefaultKeys();
        keysDang[36].layers = {{1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)}};
        keysDang[44].layers = {{1, 18, 0x8000u, 0, 0, makeId(0xD0, 0xFE)}}; // absent from KMP!
        auto keysNoPcmR = makeDefaultKeys();
        keysNoPcmR[36].layers = {{1, 7, 0x8000u, 0, 0, makeId(0xD0, 7)}};   // drum[7]: L=7 valid, R=9 missing PCM!

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK02.PCG", "USERDK02", buildKorfContainer({
                {kProf1005.kind, 4, 20, 12, "SYN_DANG",   rawKorfPayload(buildSynthUserDkBody(kProf1005, keysDang)),   ""},
                {kProf1005.kind, 4, 20, 13, "SYN_NOPCMR", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysNoPcmR)), ""},
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        const TargetOccupancyMap occ = inspectTargetKstInput(in);
        const auto* e12 = occ.findOccupied({true, 2, 12});
        const auto* e13 = occ.findOccupied({true, 2, 13});
        bool hasDanglingSmp9 = false;
        for (const auto& d : res.graph->dangling()) {
            if (d.to == ResourceId({false, "KST", Kind::Sample, 9})) hasDanglingSmp9 = true;
        }
        TEST("T7: dangling RAM DrumSample (inUnanalysedSound=false) & missing R-channel PCM both fail-closed to InsufficientEvidence",
             res.ok && res.report.refs.drumKitsAnalysed == 2 &&
             res.report.refs.unresolved.size() == 1 &&
             res.report.refs.unresolved[0].record == 44 && !res.report.refs.unresolved[0].inUnanalysedSound &&
             hasDanglingSmp9 &&
             e12 != nullptr && e12->equivalence.unresolvedSlotCount == 1 && !e12->equivalence.provablyComparable() &&
             e13 != nullptr && !e13->equivalence.allReferencedDrumSamplesHavePcm() && !e13->equivalence.provablyComparable());
    }

    // T8: OC31 10030038 (has 0x8000 sentinel, lst=30) resolves cleanly, while OC32 10030038 (no 0x8000 sentinel, lst=22)
    //     & legacy 07000038 (4,322 B fixed) fail closed cleanly!
    {
        auto keysOc31 = makeDefaultKeys();
        keysOc31[36].layers = {{1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)}};
        const Bytes oc31Body = buildSynthUserDkBody(kProf1003Oc31, keysOc31);
        auto keysOc32 = makeDefaultKeys();
        const Bytes oc32Body = buildSynthUserDkBody(kProf1003Oc32, keysOc32);
        Bytes legacyBody(4322u, 0x00);
        legacyBody[16] = 0xA2u;

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {0x10030038u, 4, 11, 19, "SYN_OC31",   rawKorfPayload(oc31Body),   ""},
                {0x10030038u, 4, 11, 20, "SYN_OC32",   rawKorfPayload(oc32Body),   ""},
                {0x07000038u, 4, 11, 21, "SYN_LEGACY", rawKorfPayload(legacyBody), ""},
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        auto gapOf = [&](uint32_t slot) -> std::optional<Gap> {
            for (const auto& u : res.report.refs.unanalysed) {
                if (u.sound == ResourceId({false, "USERDK01", Kind::Sound, slot})) return u.why;
            }
            return std::nullopt;
        };
        const TargetOccupancyMap occ = inspectTargetKstInput(in);
        TEST("T8: OC31 10030038 resolves; OC32 10030038 -> Gap::NoId8InDrumLayers; legacy 07000038 -> Gap::NoLayoutForKind",
             res.ok && res.report.refs.drumKitsAnalysed == 1 && res.report.refs.unresolved.empty() &&
             !gapOf(19).has_value() && gapOf(20) == Gap::NoId8InDrumLayers && gapOf(21) == Gap::NoLayoutForKind &&
             occ.coverage.analysedSounds == 1 && occ.coverage.unanalysedDrumKits == 2 &&
             occ.findOccupied({true, 1, 19})->equivalence.provablyComparable() &&
             !occ.findOccupied({true, 1, 20})->equivalence.provablyComparable() &&
             !occ.findOccupied({true, 1, 21})->equivalence.provablyComparable());
    }

    // T9: Coincidental melodic size collision trap (10020038 USERDK of exact 9,302 B: 518 + 128*18 + 216*30 == 325 + 47*191!)
    {
        auto keys9302 = makeDefaultKeys();
        // Need L = 216 total layers (128 + 88 extra layers across keys 0..21: 4 extra layers each -> 5 layers each)
        for (size_t k = 0; k < 22u; ++k) {
            keys9302[k].velTops = {25, 50, 75, 100, 127};
            keys9302[k].layers = {
                {1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)},
                {0, 0, 0x8000u, 0, 0, {}},
                {0, 0, 0x8000u, 0, 0, {}},
                {0, 0, 0x8000u, 0, 0, {}},
                {0, 0, 0x8000u, 0, 0, {}},
            };
        }
        const Bytes trapBody = buildSynthUserDkBody(kProf1002, keys9302, 0xA2u);
        TEST("T9: synthetic trap body is exact 9,302 B and coincidentally satisfies melodic (9302 - 325) % 191 == 0",
             trapBody.size() == 9302u && ((trapBody.size() - 325u) % 191u == 0u));

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK03.PCG", "USERDK03", buildKorfContainer({
                {0x10020038u, 4, 26, 22, "SYN_TRAP_9302", rawKorfPayload(trapBody), ""}
            })}
        };
        NativeKstOptions opts;
        opts.layouts = {{0x10020038u, RecordLayout{325, 191, 8}}}; // supply melodic 10020038 layout!
        const NativeKstResult res = resolveNativeKst(in, opts);
        TEST("T9: 9,302 B USERDK03/22 routes strictly to Drum Kit parser with 22 valid RAM edges & 0 bogus melodic slots",
             res.ok && res.report.refs.drumKitsAnalysed == 1 && res.report.refs.ramDrumLayers == 22 &&
             res.report.refs.unresolved.empty() && res.report.refs.unanalysed.empty());
    }
}

static void testGroupC_CorruptAndBoundaryRefusals(const SynthUniverse& U) {
    std::printf("\n[Group C] Corrupt / Fuzz / Boundary Rejection Fixtures (T10..T18 + Data-Driven Fit)\n");

    const Bytes validT1 = buildSynthUserDkBody(kProf1005, makeT1Keys());
    auto runSingle = [&](const Bytes& body, uint32_t kind = kProf1005.kind) {
        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kind, 4, 11, 0, "SYN_CORRUPT", rawKorfPayload(body), ""}
            })}
        };
        return resolveNativeKst(in);
    };

    // T10: Truncated bodies (-1 B, mid-header 100 B, mid-key-table 1,000 B)
    {
        Bytes minus1 = validT1; minus1.pop_back();
        Bytes hdr100(validT1.begin(), validT1.begin() + 100);
        Bytes key1000(validT1.begin(), validT1.begin() + 1000);
        const auto r1 = runSingle(minus1);
        const auto r2 = runSingle(hdr100);
        const auto r3 = runSingle(key1000);
        TEST("T10: truncated bodies (-1B, 100B, 1000B) all fail closed with Gap::SizeDoesNotFit & 0 edges",
             r1.ok && r1.report.refs.drumKitsAnalysed == 0 && r1.report.refs.unanalysed.size() == 1 &&
             r1.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit &&
             r2.ok && r2.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit &&
             r3.ok && r3.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit);
    }

    // T11: sum(lc) / cumEnd mismatch
    {
        Bytes badLc = validT1;
        badLc[kProf1005.hdr + 36u * kProf1005.kst + kProf1005.lcOff] = 3u;
        Bytes badCum = validT1;
        const size_t cumOff = kProf1005.hdr + 50u * kProf1005.kst + kProf1005.kst - 2u;
        putBe16(badCum, cumOff, static_cast<uint16_t>(native_detail::be16(&badCum[cumOff]) + 1u));
        const auto r1 = runSingle(badLc);
        const auto r2 = runSingle(badCum);
        TEST("T11: lc mismatch & cumEnd +1 corruption both rejected with Gap::SizeDoesNotFit",
             r1.ok && r1.report.refs.drumKitsAnalysed == 0 && r1.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit &&
             r2.ok && r2.report.refs.drumKitsAnalysed == 0 && r2.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit);
    }

    // T12: Missing 0x8000 sentinel on key[127] while keeping lst=32 (with ID8)
    {
        Bytes noFlag = validT1;
        const size_t end127Off = kProf1005.hdr + 127u * kProf1005.kst + kProf1005.kst - 2u;
        putBe16(noFlag, end127Off, static_cast<uint16_t>(native_detail::be16(&noFlag[end127Off]) & 0x7FFFu));
        const auto r = runSingle(noFlag);
        TEST("T12: cleared 0x8000 sentinel with lst=32 body rejected fail-closed with Gap::SizeDoesNotFit",
             r.ok && r.report.refs.drumKitsAnalysed == 0 && r.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit);
    }

    // T13: Out-of-range lc (0 or 9 with matching padding)
    {
        Bytes lcZero = validT1;
        lcZero[kProf1005.hdr + kProf1005.lcOff] = 0u;
        Bytes lcNine = validT1;
        lcNine[kProf1005.hdr + kProf1005.lcOff] = 9u;
        lcNine.insert(lcNine.end(), 8u * kProf1005.lst, 0x00);
        const auto r0 = runSingle(lcZero);
        const auto r9 = runSingle(lcNine);
        TEST("T13: out-of-range lc (0 and 9) rejected fail-closed with 0 edges",
             r0.ok && r0.report.refs.drumKitsAnalysed == 0 && r0.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit &&
             r9.ok && r9.report.refs.drumKitsAnalysed == 0 && r9.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit);
    }

    // T14: Invalid velocity thresholds (non-monotonic [80, 60] and >127 [128])
    {
        Bytes nonMono = validT1;
        const size_t vOff = kProf1005.hdr + 36u * kProf1005.kst + (kProf1005.kst - 10u);
        nonMono[vOff] = 80u;
        nonMono[vOff + 1u] = 60u;
        Bytes gt127 = validT1;
        gt127[vOff] = 128u;
        const auto r1 = runSingle(nonMono);
        const auto r2 = runSingle(gt127);
        TEST("T14: non-monotonic velTop [80,60] and velTop > 127 rejected fail-closed",
             r1.ok && r1.report.refs.drumKitsAnalysed == 0 && r1.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit &&
             r2.ok && r2.report.refs.drumKitsAnalysed == 0 && r2.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit);
    }

    // T15: Mode flag bit 7 clear in USERDK bank
    {
        Bytes badMode = validT1;
        badMode[kProf1005.modeFlagOffset] = 0x02u;
        const auto r = runSingle(badMode);
        TEST("T15: mode flag bit 7 clear (0x02) in USERDK bank rejected fail-closed",
             r.ok && r.report.refs.drumKitsAnalysed == 0 && r.report.refs.unanalysed[0].why == Gap::SizeDoesNotFit);
    }

    // T16: Cross-type ID8 injection (Multisample ID or Sample ID in USERDK RAM layer)
    {
        auto keysCross = makeT1Keys();
        keysCross[50].layers = {{1, 0, 0x8000u, 0, 0, makeId(0xB0, 1)}}; // Multisample ID!
        keysCross[51].layers = {{1, 0, 0x8000u, 0, 0, makeId(0xA0, 1)}}; // Sample ID!
        const auto r = runSingle(buildSynthUserDkBody(kProf1005, keysCross));
        TEST("T16: Multisample & Sample ID8 injected into USERDK RAM layers -> UnresolvedSlot(wrongKind==true)",
             r.ok && r.report.refs.drumKitsAnalysed == 1 && r.report.refs.unresolved.size() == 2 &&
             r.report.refs.unresolved[0].record == 50 && r.report.refs.unresolved[0].wrongKind &&
             !r.report.refs.unresolved[0].inUnanalysedSound &&
             r.report.refs.unresolved[1].record == 51 && r.report.refs.unresolved[1].wrongKind);
    }

    // T17: ROM bank byte (layer[0] == 0) carrying non-zero ID8
    {
        auto keysRomBad = makeT1Keys();
        keysRomBad[52].layers = {{0, 0, 0x8000u, 0, 0, makeId(0xD0, 1)}};
        const auto r = runSingle(buildSynthUserDkBody(kProf1005, keysRomBad));
        TEST("T17: ROM layer[0]==0 carrying non-zero ID8 flagged as fail-closed UnresolvedSlot",
             r.ok && r.report.refs.drumKitsAnalysed == 1 && r.report.refs.unresolved.size() == 1 &&
             r.report.refs.unresolved[0].record == 52 && !r.report.refs.unresolved[0].inUnanalysedSound);
    }

    // T18: Out-of-spec USERDK07..12 upstream MIDI select (MSB 120, LSB 70..75) inside a synthetic Style .stg leaf
    {
        NativeKstClosureInput cin;
        cin.downstream = U.makeBaseInput();
        cin.upstreamRoots = {
            {"STYLE/BankA/OutOfSpecDk.stg", makeStyleTree({{9, 120, 70, 0}})}
        };
        const M1AdapterView view = adaptNativeKstInput(cin);
        const auto* root = view.findRoot("STYLE/BankA/OutOfSpecDk.stg");
        const auto slot70 = SlotAddress::fromMidi(120, 70, 0);
        const auto sel70 = classifySoundSelection({120, 70, 0});
        TEST("T18: MSB 120 LSB 70 in Style .stg classified as out-of-spec USERDK07 (AmbiguousOrOutOfSpec, never mapped to USERDK01..06)",
             slot70.has_value() && slot70->isOutOfSpecDrumKitBank() && !slot70->isInSpec() &&
             sel70.boundary == SoundBoundaryKind::AmbiguousOrOutOfSpec &&
             sel70.confidence == EdgeConfidence::Unsupported &&
             view.ok && root != nullptr && root->soundSelections.size() == 1 &&
             root->soundSelections[0].boundary == SoundBoundaryKind::AmbiguousOrOutOfSpec &&
             root->soundSelections[0].slotAddress.has_value() &&
             root->soundSelections[0].slotAddress->isOutOfSpecDrumKitBank());
    }

    // Integer overflow guard on validateDrumKitBody with extreme layout numbers
    {
        DrumKitLayout evil{0xFFFFFFFFu, 20u, 2u, 32u, 2u, 0u, true};
        uint32_t tl = 0; bool h8 = false; Gap fg = Gap::NoLayoutForKind;
        TEST("overflow-safe validateDrumKitBody rejects 0xFFFFFFFF headerBytes without UB",
             !validateDrumKitBody(validT1, evil, tl, h8, fg) && fg == Gap::SizeDoesNotFit);
    }

    // Pure data-driven fitDrumKitLayout() on an invented custom chunk kind (0x10990038u, not in knownDrumKitLayout!)
    {
        const SynthDkProfile customProf{0x10990038u, 400, 2, 20, 2, 32, true};
        const Bytes customBody = buildSynthUserDkBody(customProf, makeT1Keys());
        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {customProf.kind, 4, 11, 0, "SYN_CUSTOM_KIND", rawKorfPayload(customBody), ""}
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        const auto it = res.report.usedDrumLayouts.find(customProf.kind);
        TEST("pure data-driven fitDrumKitLayout() discovers unknown custom kind 0x10990038 without hardcoding",
             res.ok && it != res.report.usedDrumLayouts.end() &&
             it->second.headerBytes == 400 && it->second.keyStride == 20 && it->second.layerStride == 32 &&
             res.report.refs.drumKitsAnalysed == 1 && res.report.refs.ramDrumLayers == 5);
    }
}

static void testGroupD_AuditRound1Regressions(const SynthUniverse& U) {
    std::printf("\n[Group D] Audit Round 1 Regression Tests (T19..T22 & P3-1..P3-5)\n");

    // T19 (P1-1): Same-slot Identical USERDK01/0 in RootTreeSubKst STILL blocks with NATIVE_SOUND_WRITE_HOLD
    //             (matching selective_sub_kst.h), AND closureNodes includes Kind::DrumSample!
    {
        auto keysOne = makeDefaultKeys();
        keysOne[36].layers = {{1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)}};
        const Bytes dkBody = buildSynthUserDkBody(kProf1005, keysOne);

        NativeKstClosureInput cin;
        cin.downstream = U.makeBaseInput();
        cin.downstream.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 0, "SYN_KIT_IDENT", rawKorfPayload(dkBody), ""}
            })}
        };
        cin.upstreamRoots = {
            {"STYLE/BankA/UsesIdentDk.stg", makeStyleTree({{9, 120, 64, 0}})}
        };

        const M1AdapterView srcView = adaptNativeKstInput(cin);
        const TargetOccupancyMap targetMap = inspectTargetKstInput(cin.downstream);

        CollectorRequest req;
        req.mode = ExportMode::RootTreeSubKst;
        req.conflictPolicy = ConflictPolicy::BlockWithReport;
        req.targets = {{SelectedKind::Style, "STYLE/BankA/UsesIdentDk.stg", "", ""}};

        const DryRunExportPlan plan = collectSelectiveExportWithTargetMap(srcView, req, &targetMap);
        const ResourceId wantDs0{false, "KST", Kind::DrumSample, 0};
        const bool hasDs0InClosure =
            std::find(plan.closureNodes.begin(), plan.closureNodes.end(), wantDs0) != plan.closureNodes.end();
        TEST("T19 (P1-1): same-slot Identical USERDK blocks with NATIVE_SOUND_WRITE_HOLD in RootTreeSubKst & closureNodes includes DrumSample/0",
             plan.ok && !plan.executable &&
             plan.hasBlockingCode(diag_code::kNativeSoundWriteHold) &&
             hasDs0InClosure);
    }

    // T20 (P2-1): fitDrumKitLayout() handles nullptr elements without crashing
    {
        const Bytes validT1 = buildSynthUserDkBody(kProf1005, makeT1Keys());
        KmpIndex kmp;
        kmp.drums.push_back({makeId(0xD0, 1), "SYN_DRUM_KICK_S", 0, 1, 0});
        DecodedSound validKit{"USERDK01", 0, kProf1005.kind, "SYN_KIT_STD", validT1};

        const std::vector<const DecodedSound*> onlyNull{nullptr};
        const std::vector<const DecodedSound*> leadingNull{nullptr, &validKit};
        const auto rNull = fitDrumKitLayout(onlyNull, kmp);
        const auto rLead = fitDrumKitLayout(leadingNull, kmp);
        TEST("T20 (P2-1): fitDrumKitLayout({nullptr}) returns nullopt & fitDrumKitLayout({nullptr, &valid}) recovers layout safely",
             !rNull.has_value() && rLead.has_value() &&
             rLead->headerBytes == kProf1005.hdr && rLead->layerStride == kProf1005.lst);
    }

    // T21 (P2-2): Custom DrumKitLayout keyStride/layerCountOffset aliasing & bounds safety under ASan+UBSan
    {
        const Bytes dummy(4096u, 0x00);
        DrumKitLayout aliasing{16u, 12u, 9u, 20u, 2u, 0u, true}; // velOff=2, lcOff=9 >= velOff -> must reject!
        uint32_t tl = 0; bool h8 = false; Gap fg = Gap::SizeDoesNotFit;
        const bool aliasOk = validateDrumKitBody(dummy, aliasing, tl, h8, fg);

        // Tight valid custom layout: hdr=16, keyStride=12, lcOff=0 (< velOff=2), layerStride=12 (hasId8=true)
        // Here lcOff+6 == 6 >= velOff(2) and layerStride == 12 < 20; buildNativeGraph must not OOB-read!
        const DrumKitLayout tight{16u, 12u, 0u, 12u, 2u, 0u, true};
        Bytes tightBody(16u + 128u * 12u + 128u * 12u, 0x00);
        tightBody[2] = 0x82u;
        for (uint32_t k = 0; k < 128u; ++k) {
            const size_t ko = 16u + size_t(k) * 12u;
            tightBody[ko + 0u] = 1u;
            tightBody[ko + 1u] = 64u;
            for (size_t j = 0; j < 8u; ++j) tightBody[ko + 2u + j] = 127u;
            const uint16_t rawEnd = static_cast<uint16_t>((k + 1u) | (k == 127u ? 0x8000u : 0u));
            putBe16(tightBody, ko + 10u, rawEnd);
        }
        const size_t l0Off = 16u + 128u * 12u;
        tightBody[l0Off + 0u] = 1u;
        putBe16(tightBody, l0Off + 2u, 0u);
        const Id8 d0 = makeId(0xD0, 1);
        std::copy(d0.begin(), d0.end(), tightBody.begin() + static_cast<std::ptrdiff_t>(l0Off + 4u));

        KmpIndex kmp;
        kmp.drums.push_back({d0, "SYN_DRUM_KICK_S", kNoZoneSample, kNoZoneSample, 0});
        DecodedSound ds{"USERDK01", 0, 0x10980038u, "SYN_TIGHT", tightBody};
        NativeRefReport rep;
        std::map<ResourceId, DrumKitKeyMap> kmaps;
        std::string err;
        const auto g = buildNativeGraph(kmp, {ds}, {}, {{0x10980038u, tight}}, rep, &err, &kmaps);
        const auto kmit = kmaps.find({false, "USERDK01", Kind::Sound, 0});
        TEST("T21 (P2-2): aliasing custom layout rejected (NoLayoutForKind); tight custom keyStride=12 parses with 0 OOB reads",
             !aliasOk && fg == Gap::NoLayoutForKind &&
             g.has_value() && rep.drumKitsAnalysed == 1 && rep.ramDrumLayers == 1 &&
             kmit != kmaps.end() && kmit->second.keys[0].advisoryExclusiveGroup == 0 &&
             !kmit->second.keys[0].layers[0].advisoryTranspose.has_value());
    }

    // T22 (P2-3): Large synthetic melodic Sound (>= 3,074 B) in USER01 with bit 7 set at modeFlagOffset
    //             is NOT misclassified as a Drum Kit and parses normally as melodic!
    {
        // Melodic layout: base=24, record=32, pairOffset=12, 100 oscillators -> 3,224 B (>= 514 + 128*20 = 3,074 B!)
        const RecordLayout melL{24, 32, 12};
        Bytes bigMel(24u + 100u * 32u, 0x00);
        bigMel[2] = 0x82u; // bit 7 set at 10050038 modeFlagOffset!
        const Id8 ms0 = makeId(0xB0, 1);
        std::copy(ms0.begin(), ms0.end(), bigMel.begin() + 24);

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USER01.PCG", "USER01", buildKorfContainer({
                {0x10050038u, 4, 9, 0, "SYN_BIG_MELODIC", rawKorfPayload(bigMel), ""}
            })}
        };
        NativeKstOptions opts;
        opts.layouts = {{0x10050038u, melL}};
        opts.allowFitLayout = false;
        const NativeKstResult res = resolveNativeKst(in, opts);
        TEST("T22 (P2-3): large >=3074 B melodic USER01 Sound with bit-7 set at modeFlagOffset parses cleanly as melodic (not DrumKit)",
             res.ok && res.report.refs.analysed == 1 && res.report.refs.drumKitsAnalysed == 0 &&
             res.report.refs.unanalysed.empty() &&
             res.graph->uses({false, "USER01", Kind::Sound, 0}).size() == 1);
    }

    // P3-1: Analysed USERDK with InsufficientEvidence (duplicate DrumSample ID8) emits kSoundSlotEquivalenceInsufficient
    //       and does NOT emit misleading DRUMKIT_BODY_UNPARSED!
    {
        auto keysDup = makeDefaultKeys();
        keysDup[68].layers = {{1, 5, 0x8000u, 0, 0, makeId(0xD0, 6)}};
        const Bytes dupBody = buildSynthUserDkBody(kProf1001, keysDup);
        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK02.PCG", "USERDK02", buildKorfContainer({
                {kProf1001.kind, 4, 20, 10, "SYN_DUP_KIT", rawKorfPayload(dupBody), ""}
            })}
        };
        const TargetOccupancyMap target = inspectTargetKstInput(in);
        const auto* e = target.findOccupied({true, 2, 10});
        CollisionMatrixInput cmIn;
        cmIn.policy = ConflictPolicy::BlockWithReport;
        cmIn.soundRequests.push_back({"pa5x:USERDK02/Sound/10", {true, 2, 10}, "SYN_DUP_KIT", e->equivalence, false, {"STYLE/B/S.stg"}});
        const CollisionMatrixResult cm = evaluateCollisionMatrix(target, cmIn);
        bool hasUnparsedDiag = false, hasInsuffDiag = false;
        for (const auto& d : cm.diagnostics) {
            if (d.code == diag_code::kDrumkitBodyUnparsed) hasUnparsedDiag = true;
            if (d.code == diag_code::kSoundSlotEquivalenceInsufficient) hasInsuffDiag = true;
        }
        TEST("P3-1: analysed USERDK with duplicate DrumSample ID emits SOUND_SLOT_EQUIVALENCE_INSUFFICIENT (not DRUMKIT_BODY_UNPARSED)",
             e != nullptr && e->equivalence.analysed && e->equivalence.hasDuplicateDrumSampleId &&
             cm.hasBlockingConflicts() && hasInsuffDiag && !hasUnparsedDiag);
    }

    // P3-3: serializeManifestJson() serializes "drumSamples" array inside "orphans"
    {
        NativeKstClosureInput cin;
        cin.downstream = U.makeBaseInput();
        const NativeKstClosureResult cres = resolveNativeKstClosure(cin);
        const std::string json = serializeManifestJson(cres.manifest);
        const size_t orphansPos = json.find("\"orphans\": {");
        const size_t dsOrphanPos = orphansPos != std::string::npos ? json.find("\"drumSamples\": [", orphansPos) : std::string::npos;
        TEST("P3-3: serializeManifestJson() emits orphans.drumSamples list alongside orphanDrumSampleCount",
             cres.ok && orphansPos != std::string::npos && dsOrphanPos != std::string::npos &&
             json.find("pa5x:KST/DrumSample/0", dsOrphanPos) != std::string::npos);
    }

    // P3-4: Sample with rate == 0 marks DrumSampleFingerprint::leftPcmPresent == false (parity with m1_manifest_adapter.h)
    {
        std::vector<SynthSampleSpec> badRateSamples = {
            {makeId(0xA0, 1), "SYN_ZERO_RATE", 0 /*rate==0*/, "PCM/ram01.pcm", 4096}
        };
        std::vector<SynthDrumSpec> badRateDrums = {
            {makeId(0xD0, 1), "SYN_DRUM_ZR", 0, kNoIndex, 0}
        };
        NativeKstInput in;
        in.kmpContainer = RawNamedBlob{"MULTISMP/RAM.KMP", "", buildKorfContainer({
            {kPa5xKmpChunkId, 6, 0, 0, "RAM.KMP", buildRawKmpPayload(badRateSamples, badRateDrums, {}, {}), ""}
        })};
        in.pcmContainers = {{"PCM/ram01.pcm", "", buildKorfContainer({
            {0x12020010u, 6, 0, 0, "SYN_ZERO_RATE", Bytes(4096, 0x11), idAsStr(makeId(0xA0, 1))}
        })}};
        auto keysZr = makeDefaultKeys();
        keysZr[36].layers = {{1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)}};
        in.soundContainers = {{"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
            {kProf1005.kind, 4, 11, 0, "SYN_KIT_ZR", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysZr)), ""}
        })}};
        const TargetOccupancyMap occ = inspectTargetKstInput(in);
        const auto* e0 = occ.findOccupied({true, 1, 0});
        TEST("P3-4: DrumSample pointing to rate==0 Sample marks leftPcmPresent==false & provablyComparable==false",
             e0 != nullptr && e0->equivalence.analysed && !e0->equivalence.allReferencedDrumSamplesHavePcm() &&
             !e0->equivalence.provablyComparable());
    }

    // P3-5: Degenerate velTop sequences ({0, 0, 127} and {64, 64, 127}) clamp velLo <= velHi cleanly
    {
        auto keysDeg = makeDefaultKeys();
        keysDeg[36].velTops = {0, 0, 127};
        keysDeg[36].layers = {
            {1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)},
            {1, 1, 0x8000u, 0, 0, makeId(0xD0, 2)},
            {1, 2, 0x8000u, 0, 0, makeId(0xD0, 3)},
        };
        keysDeg[38].velTops = {64, 64, 127};
        keysDeg[38].layers = {
            {1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)},
            {1, 1, 0x8000u, 0, 0, makeId(0xD0, 2)},
            {1, 2, 0x8000u, 0, 0, makeId(0xD0, 3)},
        };
        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {{"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
            {kProf1005.kind, 4, 11, 0, "SYN_KIT_DEGVEL", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysDeg)), ""}
        })}};
        const NativeKstResult res = resolveNativeKst(in);
        const auto* km = res.findDrumKitKeyMap({false, "USERDK01", Kind::Sound, 0});
        TEST("P3-5: degenerate velTop {0,0,127} and {64,64,127} clamp velLo <= velHi on every layer",
             res.ok && km != nullptr &&
             km->keys[36].layers[0].velLo == 0 && km->keys[36].layers[0].velHi == 0 &&
             km->keys[36].layers[1].velLo == 0 && km->keys[36].layers[1].velHi == 0 &&
             km->keys[36].layers[2].velLo == 1 && km->keys[36].layers[2].velHi == 127 &&
             km->keys[38].layers[0].velLo == 1 && km->keys[38].layers[0].velHi == 64 &&
             km->keys[38].layers[1].velLo == 64 && km->keys[38].layers[1].velHi == 64 &&
             km->keys[38].layers[2].velLo == 65 && km->keys[38].layers[2].velHi == 127);
    }
}

static void testGroupE_SliceA1EnrichmentAndBank2Guard(const SynthUniverse& U) {
    std::printf("\n[Group E] Slice A1: DrumKeyOccupancy, Sample Names, Vel Helpers, Target Exposure & Bank-2 Guard (T23..T27)\n");

    // T23: Four-state DrumKeyOccupancy, activeKeyMask/ramKeyMask, isRomOnly, hasBank2Layers & IDrumKitCatalog alias
    {
        auto keysMixed = makeDefaultKeys();
        // Key 36: resolved RAM (2 layers) -> RamPopulated
        keysMixed[36] = {0x7FFFu, 64, 0, {64, 127}, {
            {1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)},
            {1, 1, 0x8000u, 0, 0, makeId(0xD0, 2)},
        }};
        // Key 38: ROM non-zero index -> RomOrBank2Populated
        keysMixed[38].layers = {{0, 142, 0x8000u, 0, 0, {}}};
        // Key 40: Bank-2 (0x02) -> RomOrBank2Populated
        keysMixed[40].layers = {{2, 86, 0x8000u, 0, 0, {}}};
        // Key 44: Dangling RAM -> EmptyOrUnknown
        keysMixed[44].layers = {{1, 18, 0x8000u, 0, 0, makeId(0xD0, 0xFE)}};
        // Key 45: ROM layer with non-zero ID8 (contradiction) -> EmptyOrUnknown
        keysMixed[45].layers = {{0, 0, 0x8000u, 0, 0, makeId(0xD0, 1)}};
        // All remaining 123 keys: default lc=1, sourceBank=0, tableIndex=0 -> RomIndexZeroPlaceholder

        auto keysRomOnly = makeDefaultKeys();
        keysRomOnly[36].layers = {{0, 201, 0x8000u, 0, 0, {}}};
        keysRomOnly[38].layers = {{0, 202, 0x8000u, 0, 0, {}}};

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 0, "SYN_OCC_MIX", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysMixed)),   ""},
                {kProf1005.kind, 4, 11, 1, "SYN_OCC_ROM", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysRomOnly)), ""},
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        const IDrumKitCatalog& catalog = res; // verifies IDrumKitCatalog alias
        const DrumKitKeyMap* kmMix = catalog.findDrumKitKeyMap({false, "USERDK01", Kind::Sound, 0});
        const DrumKitKeyMap* kmRom = catalog.findDrumKitKeyMap({false, "USERDK01", Kind::Sound, 1});
        TEST("T23: DrumKeyOccupancy classifies RAM, ROM(idx!=0), Bank-2, ROM(idx==0) placeholder & dangling/corrupt accurately",
             res.ok && kmMix != nullptr && kmRom != nullptr &&
             kmMix->keys[36].occupancy == DrumKeyOccupancy::RamPopulated &&
             kmMix->keys[38].occupancy == DrumKeyOccupancy::RomOrBank2Populated &&
             kmMix->keys[40].occupancy == DrumKeyOccupancy::RomOrBank2Populated &&
             kmMix->keys[44].occupancy == DrumKeyOccupancy::EmptyOrUnknown &&
             kmMix->keys[45].occupancy == DrumKeyOccupancy::EmptyOrUnknown &&
             kmMix->keys[0].occupancy == DrumKeyOccupancy::RomIndexZeroPlaceholder &&
             kmMix->keys[60].occupancy == DrumKeyOccupancy::RomIndexZeroPlaceholder &&
             kmMix->activeKeyMask.count() == 3 &&
             kmMix->activeKeyMask.test(36) && kmMix->activeKeyMask.test(38) && kmMix->activeKeyMask.test(40) &&
             !kmMix->activeKeyMask.test(0) && !kmMix->activeKeyMask.test(44) && !kmMix->activeKeyMask.test(45) &&
             kmMix->ramKeyMask.count() == 1 && kmMix->ramKeyMask.test(36) &&
             kmMix->hasKey(36) && kmMix->hasKey(38) && kmMix->hasKey(40) && !kmMix->hasKey(0) &&
             kmMix->nonRamBank2LayerCount == 1 && kmMix->hasBank2Layers() && !kmMix->isRomOnly() &&
             kmRom->isRomOnly() && !kmRom->hasBank2Layers() &&
             kmRom->activeKeyMask.count() == 2 && kmRom->ramKeyMask.none() &&
             kmRom->keys[36].occupancy == DrumKeyOccupancy::RomOrBank2Populated &&
             kmRom->keys[0].occupancy == DrumKeyOccupancy::RomIndexZeroPlaceholder);
    }

    // T24: advisoryDrumSampleName populated from RAM.KMP on resolved RAM layers (empty on ROM/dangling)
    {
        auto keysNames = makeT1Keys();
        keysNames[50].layers = {{0, 120, 0x8000u, 0, 0, {}}};
        keysNames[51].layers = {{1, 18, 0x8000u, 0, 0, makeId(0xD0, 0xFE)}};
        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 0, "SYN_NAMES", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysNames)), ""}
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        const DrumKitKeyMap* km = res.findDrumKitKeyMap({false, "USERDK01", Kind::Sound, 0});
        TEST("T24: advisoryDrumSampleName populated from RAM.KMP on resolved RAM layers and empty on ROM/dangling",
             res.ok && km != nullptr &&
             km->keys[36].layers[0].advisoryDrumSampleName == "SYN_DRUM_KICK_S" &&
             km->keys[36].layers[1].advisoryDrumSampleName == "SYN_DRUM_KICK_H" &&
             km->keys[38].layers[0].advisoryDrumSampleName == "SYN_DRUM_SNARE" &&
             km->keys[42].layers[0].advisoryDrumSampleName == "SYN_DRUM_HH_CL" &&
             km->keys[50].layers[0].advisoryDrumSampleName.empty() &&
             km->keys[51].layers[0].advisoryDrumSampleName.empty());
    }

    // T25: Pure constexpr velocity helpers: effectiveDistinctLayers(), effectiveMaxVel(), hasDegenerateVelSplit()
    {
        DrumKitKeySummary emptyKey;
        auto keysVel = makeDefaultKeys();
        keysVel[36] = {0x7FFFu, 64, 0, {64, 127}, {
            {1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)},
            {1, 1, 0x8000u, 0, 0, makeId(0xD0, 2)},
        }};
        keysVel[38] = {0x7FFFu, 64, 0, {127, 127}, {
            {1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)},
            {1, 2, 0x8000u, 0, 0, makeId(0xD0, 3)},
        }};
        keysVel[40] = {0x7FFFu, 64, 0, {60, 109}, {
            {1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)},
            {1, 2, 0x8000u, 0, 0, makeId(0xD0, 3)},
        }};
        keysVel[42] = {0x7FFFu, 64, 1, {64, 64, 120}, {
            {1, 3, 0x8000u, 0, 0, makeId(0xD0, 4)},
            {1, 3, 0x8000u, 0, 0, makeId(0xD0, 4)},
            {1, 4, 0x8000u, 0, 0, makeId(0xD0, 5)},
        }};
        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 0, "SYN_VEL_HELPERS", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysVel)), ""}
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        const DrumKitKeyMap* km = res.findDrumKitKeyMap({false, "USERDK01", Kind::Sound, 0});
        TEST("T25: effectiveDistinctLayers, effectiveMaxVel & hasDegenerateVelSplit handle normal, [127/127], sub-max ceiling & empty keys",
             emptyKey.effectiveDistinctLayers() == 0 && emptyKey.effectiveMaxVel() == 0 && !emptyKey.hasDegenerateVelSplit() &&
             res.ok && km != nullptr &&
             km->keys[36].effectiveDistinctLayers() == 2 && km->keys[36].effectiveMaxVel() == 127 && !km->keys[36].hasDegenerateVelSplit() &&
             km->keys[38].effectiveDistinctLayers() == 1 && km->keys[38].effectiveMaxVel() == 127 && km->keys[38].hasDegenerateVelSplit() &&
             km->keys[40].effectiveDistinctLayers() == 2 && km->keys[40].effectiveMaxVel() == 109 && !km->keys[40].hasDegenerateVelSplit() &&
             km->keys[42].effectiveDistinctLayers() == 2 && km->keys[42].effectiveMaxVel() == 120 && km->keys[42].hasDegenerateVelSplit());
    }

    // T26: TargetOccupancyMap::drumKeyMaps exposure & M1AdapterView::drumKeyMaps parity (RAM + ROM-only + Bank-2 kits)
    {
        auto keysRam = makeT1Keys();
        auto keysRom = makeDefaultKeys();
        keysRom[36].layers = {{0, 100, 0x8000u, 0, 0, {}}};
        auto keysBank2 = makeDefaultKeys();
        keysBank2[36].layers = {{1, 0, 0x8000u, 0, 0, makeId(0xD0, 1)}};
        keysBank2[38].layers = {{2, 42, 0x8000u, 0, 0, {}}};

        NativeKstClosureInput cin;
        cin.downstream = U.makeBaseInput();
        cin.downstream.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 0, "SYN_RAM_KIT",   rawKorfPayload(buildSynthUserDkBody(kProf1005, keysRam)),   ""},
                {kProf1005.kind, 4, 11, 1, "SYN_ROM_KIT",   rawKorfPayload(buildSynthUserDkBody(kProf1005, keysRom)),   ""},
                {kProf1005.kind, 4, 11, 2, "SYN_BANK2_KIT", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysBank2)), ""},
            })}
        };

        const TargetOccupancyMap occDirect = inspectTargetKstInput(cin.downstream);
        const M1AdapterView view = adaptNativeKstInput(cin);
        const TargetOccupancyMap occFromAdapter = buildTargetOccupancyFromAdapter(view);
        const NativeKstClosureResult m1Res = resolveNativeKstClosure(cin);
        const TargetOccupancyMap occFromM1 = buildTargetOccupancyFromM1Closure(m1Res);

        const SlotAddress s0{true, 1, 0}, s1{true, 1, 1}, s2{true, 1, 2};
        const ResourceId r0{false, "USERDK01", Kind::Sound, 0};
        const ResourceId r1{false, "USERDK01", Kind::Sound, 1};
        const ResourceId r2{false, "USERDK01", Kind::Sound, 2};

        const auto* dKm0 = occDirect.findDrumKitKeyMap(s0);
        const auto* dKm1 = occDirect.findDrumKitKeyMap(s1);
        const auto* dKm2 = occDirect.findDrumKitKeyMap(s2);
        const auto* vKm0 = view.findDrumKitKeyMap(r0);
        const auto* vKm1 = view.findDrumKitKeyMap(r1);
        const auto* vKm2 = view.findDrumKitKeyMap(r2);
        const auto* aKm0 = occFromAdapter.findDrumKitKeyMap(s0);
        const auto* aKm1 = occFromAdapter.findDrumKitKeyMap(s1);
        const auto* aKm2 = occFromAdapter.findDrumKitKeyMap(s2);
        const auto* mKm2 = occFromM1.findDrumKitKeyMap(s2);

        TEST("T26: TargetOccupancyMap & M1AdapterView expose all 3 parsed USERDK keyMaps (including ROM-only) with exact parity",
             occDirect.drumKeyMaps.size() == 3 && view.drumKeyMaps.size() == 3 &&
             occFromAdapter.drumKeyMaps.size() == 3 && occFromM1.drumKeyMaps.size() == 3 &&
             dKm0 && dKm1 && dKm2 && vKm0 && vKm1 && vKm2 && aKm0 && aKm1 && aKm2 && mKm2 &&
             dKm0->activeKeyMask == vKm0->activeKeyMask && dKm0->activeKeyMask == aKm0->activeKeyMask &&
             dKm0->keys[36].layers[0].advisoryDrumSampleName == "SYN_DRUM_KICK_S" &&
             vKm0->keys[36].layers[0].advisoryDrumSampleName == "SYN_DRUM_KICK_S" &&
             dKm1->isRomOnly() && vKm1->isRomOnly() && aKm1->isRomOnly() &&
             dKm2->hasBank2Layers() && vKm2->hasBank2Layers() && aKm2->hasBank2Layers() && mKm2->hasBank2Layers() &&
             dKm2->nonRamBank2LayerCount == 1 && vKm2->nonRamBank2LayerCount == 1);

        // T27: Exact Bank-2 fail-closed equivalence parity across slot_conflicts.h & m1_manifest_adapter.h + CollisionMatrix refusal
        const auto* directE0 = occDirect.findOccupied(s0);
        const auto* directE2 = occDirect.findOccupied(s2);
        const auto* adaptE0 = occFromAdapter.findOccupied(s0);
        const auto* adaptE2 = occFromAdapter.findOccupied(s2);
        const auto viewS2 = view.sounds.find(r2);

        const auto eqDirectBank2 = compareSoundEquivalence(directE2->equivalence, directE2->equivalence);
        const auto eqAdaptBank2 = compareSoundEquivalence(adaptE2->equivalence, adaptE2->equivalence);
        const auto eqCrossBank2 = compareSoundEquivalence(viewS2->second.equivalence, directE2->equivalence);
        const auto eqCleanRam = compareSoundEquivalence(directE0->equivalence, adaptE0->equivalence);

        CollisionMatrixInput cmIn;
        cmIn.policy = ConflictPolicy::BlockWithReport;
        cmIn.soundRequests.push_back({"pa5x:USERDK01/Sound/2", s2, "SYN_BANK2_KIT", viewS2->second.equivalence, false, {"STYLE/B/S.stg"}});
        const CollisionMatrixResult cm = evaluateCollisionMatrix(occDirect, cmIn);
        bool hasInsuffDiag = false, hasUnparsedDiag = false;
        for (const auto& d : cm.diagnostics) {
            if (d.code == diag_code::kSoundSlotEquivalenceInsufficient) hasInsuffDiag = true;
            if (d.code == diag_code::kDrumkitBodyUnparsed) hasUnparsedDiag = true;
        }
        TEST("T27: Bank-2 (0x02) kit fails closed to InsufficientEvidence with exact parity across slot_conflicts & m1_manifest_adapter",
             directE0 && directE2 && adaptE0 && adaptE2 && viewS2 != view.sounds.end() &&
             !directE0->equivalence.hasNonRamBank2Layers && directE0->equivalence.provablyComparable() &&
             eqCleanRam.verdict == EquivalenceVerdict::Identical &&
             directE2->equivalence.analysed && directE2->equivalence.hasNonRamBank2Layers &&
             !directE2->equivalence.provablyComparable() && directE2->equivalence.equivalenceDigest.empty() &&
             adaptE2->equivalence.analysed && adaptE2->equivalence.hasNonRamBank2Layers &&
             !adaptE2->equivalence.provablyComparable() && adaptE2->equivalence.equivalenceDigest.empty() &&
             viewS2->second.equivalence.hasNonRamBank2Layers && !viewS2->second.equivalence.provablyComparable() &&
             eqDirectBank2.verdict == EquivalenceVerdict::InsufficientEvidence &&
             eqDirectBank2.reason.find("contains unverified Bank-2 (0x02) drum layers") != std::string::npos &&
             eqAdaptBank2.verdict == EquivalenceVerdict::InsufficientEvidence &&
             eqCrossBank2.verdict == EquivalenceVerdict::InsufficientEvidence &&
             cm.hasBlockingConflicts() && hasInsuffDiag && !hasUnparsedDiag &&
             occDirect.targetLibraryKey == occFromAdapter.targetLibraryKey);
    }

    // T28 (Audit P2-2): Unknown-bank (0x03/0xFF) & corrupt non-zero-ID8 ROM kits MUST NOT report isRomOnly() == true
    //                   and MUST fail closed to InsufficientEvidence!
    {
        auto keysUnknownBank = makeDefaultKeys();
        keysUnknownBank[36].layers = {{0x03u, 10, 0x8000u, 0, 0, {}}};
        keysUnknownBank[38].layers = {{0xFFu, 11, 0x8000u, 0, 0, {}}};

        auto keysCorruptRom = makeDefaultKeys();
        keysCorruptRom[42].layers = {{0x00u, 100, 0x8000u, 0, 0, makeId(0xD0, 1)}}; // ROM layer carrying non-zero ID8!

        NativeKstInput in = U.makeBaseInput();
        in.soundContainers = {
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kProf1005.kind, 4, 11, 10, "SYN_UNK_BANK",   rawKorfPayload(buildSynthUserDkBody(kProf1005, keysUnknownBank)), ""},
                {kProf1005.kind, 4, 11, 11, "SYN_BAD_ROM_ID", rawKorfPayload(buildSynthUserDkBody(kProf1005, keysCorruptRom)),  ""},
            })}
        };
        const NativeKstResult res = resolveNativeKst(in);
        const TargetOccupancyMap occ = inspectTargetKstInput(in);
        const auto* kmUnk = res.findDrumKitKeyMap({false, "USERDK01", Kind::Sound, 10});
        const auto* kmBad = res.findDrumKitKeyMap({false, "USERDK01", Kind::Sound, 11});
        const auto* occUnk = occ.findOccupied({true, 1, 10});
        const auto* occBad = occ.findOccupied({true, 1, 11});
        TEST("T28 (P2-2): unknown-bank (0x03/0xFF) & corrupt non-zero-ID8 ROM kits track unresolvedNonRamLayerCount, isRomOnly()==false & InsufficientEvidence",
             res.ok && kmUnk != nullptr && kmBad != nullptr &&
             kmUnk->unresolvedNonRamLayerCount == 2 && !kmUnk->isRomOnly() &&
             kmUnk->keys[36].occupancy == DrumKeyOccupancy::EmptyOrUnknown &&
             kmUnk->keys[38].occupancy == DrumKeyOccupancy::EmptyOrUnknown &&
             kmBad->unresolvedNonRamLayerCount == 1 && !kmBad->isRomOnly() &&
             kmBad->keys[42].occupancy == DrumKeyOccupancy::EmptyOrUnknown &&
             occUnk != nullptr && !occUnk->equivalence.provablyComparable() &&
             compareSoundEquivalence(occUnk->equivalence, occUnk->equivalence).verdict == EquivalenceVerdict::InsufficientEvidence &&
             occBad != nullptr && !occBad->equivalence.provablyComparable() &&
             compareSoundEquivalence(occBad->equivalence, occBad->equivalence).verdict == EquivalenceVerdict::InsufficientEvidence);
    }
}

int main() {
    std::printf("Test: Read-Only USERDK Drum Kit Parser, Resource Graph Integration & Equivalence (T1..T28)\n");
    const SynthUniverse U;
    testGroupA_PositiveControls(U);
    testGroupB_AnomaliesAndLegacy(U);
    testGroupC_CorruptAndBoundaryRefusals(U);
    testGroupD_AuditRound1Regressions(U);
    testGroupE_SliceA1EnrichmentAndBank2Guard(U);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
