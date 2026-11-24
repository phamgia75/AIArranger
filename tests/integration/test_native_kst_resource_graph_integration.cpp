// End-to-end synthetic regression tests for read-only Native-KST Resource Graph integration:
//   panative::parse() + panative::decodeOc31() (DEP-A) + panative::parsePa5xKmp() (DEP-B)
//   -> native_refs.h -> resource_graph.h (Sound -> Oscillator -> Multisample -> Zone -> Sample -> PCM,
//      plus DrumSample -> L/R Sample -> PCM).
//
// 100% synthetic, original fixtures only (no owner library files, bytes, or names).
// Strictly read-only: no KST/SET writes, no OC31 encoder in production code, no GUI/hardware/MIDI.

#include "media/sound/native_refs.h"
#include "media/panative/oc31.h"
#include "media/panative/pa5x_kmp.h"
#include "media/panative/panative_reader.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace ai_arranger::media::graph;
using namespace ai_arranger::media::panative;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

using Bytes = std::vector<uint8_t>;

// ── Synthetic fixture builders (test-only) ───────────────────────────────────────────────────────────────────────────

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
static void putBe32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
static void putStr(Bytes& b, size_t at, const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) b[at + i] = static_cast<uint8_t>(s[i]);
}
static void appendStr(Bytes& b, const std::string& s) {
    b.insert(b.end(), reinterpret_cast<const uint8_t*>(s.data()), reinterpret_cast<const uint8_t*>(s.data()) + s.size());
}

static Id8 makeId(uint8_t tag, uint8_t index) {
    return Id8{tag, index, 0x31, 0x42, 0x53, 0x64, 0x75, 0x86};
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

// Synthetic OC31/OC32 test-vector framer (test-only helper; NOT a general compressor).
// Supports raw literal runs up to 273 bytes in one op (t=0, e=len-18 for 18..273; t=len-2 for 3..17; 01 00 yy for 1..3),
// plus explicit M2/M3 back-reference vector builders for exercising LZ77 match decoding in Sound payloads.
static Bytes frameOcRaw(const char magic[4], uint32_t want, const Bytes& ops, uint8_t x) {
    Bytes out;
    out.reserve(9 + ops.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(magic[i]));
    out.push_back(static_cast<uint8_t>(want & 0xFFu));
    out.push_back(static_cast<uint8_t>((want >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((want >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((want >> 24) & 0xFFu));
    out.insert(out.end(), ops.begin(), ops.end());
    out.push_back(x);
    return out;
}

static Bytes makeOcLiteralFrame(const char magic[4], const Bytes& plain) {
    Bytes ops;
    if (plain.size() >= 1 && plain.size() <= 3) {
        ops.push_back(0x01);
        ops.push_back(0x00);
        ops.push_back(static_cast<uint8_t>(plain.size()));
        ops.insert(ops.end(), plain.begin(), plain.end());
    } else if (plain.size() >= 4 && plain.size() <= 17) {
        ops.push_back(static_cast<uint8_t>(plain.size() - 2));
        ops.insert(ops.end(), plain.begin(), plain.end());
    } else if (plain.size() >= 18 && plain.size() <= 273) {
        ops.push_back(0x00);
        ops.push_back(static_cast<uint8_t>(plain.size() - 18));
        ops.insert(ops.end(), plain.begin(), plain.end());
    }
    ops.push_back(0x01); ops.push_back(0x00); ops.push_back(0x00);
    return frameOcRaw(magic, static_cast<uint32_t>(plain.size()), ops, xorBytes(plain));
}

// Builds a synthetic OC31/OC32 payload using literal runs + M2/M3 back-references (exercises LZ77 matches).
// Constructs: `prefix` (18..273 bytes) followed by an M3 back-reference copying `copyLen` bytes from distance `dist`,
// plus optional `suffix` (18..273 bytes) after returning to state 0 via an M2 match with 0 trailing literals.
static Bytes makeOcWithMatchFrame(const char magic[4], const Bytes& prefix, size_t copyDist, size_t copyLen,
                                  const Bytes& suffix, Bytes& expectedPlainOut) {
    expectedPlainOut = prefix;
    Bytes ops;
    // Initial literal run (18..273 bytes)
    ops.push_back(0x00);
    ops.push_back(static_cast<uint8_t>(prefix.size() - 18));
    ops.insert(ops.end(), prefix.begin(), prefix.end());

    // M3 match (L=1 -> 16-bit BE length) with 0 trailing literals (sb & 3 == 0 -> state returns to 0!)
    ops.push_back(0x21);
    ops.push_back(static_cast<uint8_t>((copyLen >> 8) & 0xFFu));
    ops.push_back(static_cast<uint8_t>(copyLen & 0xFFu));
    const size_t dOff = copyDist - 1;
    ops.push_back(static_cast<uint8_t>((dOff >> 6) & 0xFFu));
    ops.push_back(static_cast<uint8_t>((dOff & 0x3Fu) << 2));   // sb & 3 == 0 -> state = 0
    const size_t from = expectedPlainOut.size() - copyDist;
    for (size_t i = 0; i < copyLen; ++i) expectedPlainOut.push_back(expectedPlainOut[from + i]);

    if (!suffix.empty()) {
        // state == 0 so a second literal run is valid!
        if (suffix.size() >= 18 && suffix.size() <= 273) {
            ops.push_back(0x00);
            ops.push_back(static_cast<uint8_t>(suffix.size() - 18));
        } else {
            ops.push_back(static_cast<uint8_t>(suffix.size() - 2));
        }
        ops.insert(ops.end(), suffix.begin(), suffix.end());
        expectedPlainOut.insert(expectedPlainOut.end(), suffix.begin(), suffix.end());
    }
    ops.push_back(0x01); ops.push_back(0x00); ops.push_back(0x00);
    return frameOcRaw(magic, static_cast<uint32_t>(expectedPlainOut.size()), ops, xorBytes(expectedPlainOut));
}

// Synthetic KORF container builder (matches panative_reader.h rules; format 0 or 1).
struct SynthItem {
    uint32_t chunkId = 0;
    uint8_t kind = 0, bank = 0, slot = 0;
    std::string name;
    Bytes payload;
    std::string sampleId;   // optional 8-byte PCM sample ID (directory field 4 in format 1)
};

static Bytes buildKorfContainer(const std::vector<SynthItem>& items, int format = 1) {
    Bytes dir;
    for (const auto& it : items) {
        const uint16_t sub = static_cast<uint16_t>((it.chunkId >> 8) & 0xFFFFu);
        if (format == 0) {
            std::string n = it.name;
            n.resize(18, '\0');
            appendStr(dir, n);
            dir.push_back(it.kind);
            dir.push_back(it.bank);
            dir.push_back(it.slot);
            pushBe16(dir, sub);
            dir.push_back(0);
        } else {
            Bytes rec;
            pushBe16(rec, it.sampleId.empty() ? 3u : 4u);
            std::string n = it.name;
            n.resize(28, '\0');
            pushBe16(rec, 2); pushBe16(rec, 28); appendStr(rec, n);
            pushBe16(rec, 0); pushBe16(rec, 3); rec.push_back(it.kind); rec.push_back(it.bank); rec.push_back(it.slot);
            pushBe16(rec, 1); pushBe16(rec, 2); pushBe16(rec, sub);
            if (!it.sampleId.empty()) { pushBe16(rec, 4); pushBe16(rec, 8); appendStr(rec, it.sampleId); }
            rec.push_back(0x12); rec.push_back(0x34); rec.push_back(0x56); rec.push_back(0x78);
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

    Bytes file;
    pushBe32(file, 0x01000114u);
    pushBe32(file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

// Synthetic Pa5X raw RAM.KMP item builder (matches pa5x_kmp.h layout).
struct SynthKmpSpec {
    struct S { Id8 id; std::string name; uint16_t rate; };
    struct D { Id8 id; std::string name; uint32_t left; uint32_t right; };
    struct Z { uint32_t sample; uint8_t topKey; };
    struct M { Id8 id; std::string name; uint32_t firstZone; uint8_t zoneCount; };
    std::vector<S> samples;
    std::vector<D> drums;
    std::vector<Z> zones;
    std::vector<M> multisamples;
    uint16_t version = 0x0103;
};

static Bytes buildPa5xKmpPayload(const SynthKmpSpec& sp) {
    const size_t ns = sp.samples.size(), nd = sp.drums.size(), nz = sp.zones.size(), nm = sp.multisamples.size();
    Bytes b(18 + 122 * ns + 126 * nd + 16 * nz + 176 * nm, 0x5A);
    b[0] = static_cast<uint8_t>(sp.version >> 8);
    b[1] = static_cast<uint8_t>(sp.version & 0xFFu);
    putBe32(b, 2, static_cast<uint32_t>(ns));
    putBe32(b, 6, static_cast<uint32_t>(nd));
    putBe32(b, 10, static_cast<uint32_t>(nz));
    putBe32(b, 14, static_cast<uint32_t>(nm));
    const size_t S = 18, D = S + 122 * ns, Z = D + 126 * nd, M = Z + 16 * nz;
    for (size_t i = 0; i < ns; ++i) {
        const size_t off = S + 122 * i;
        std::fill(b.begin() + static_cast<ptrdiff_t>(off + 75), b.begin() + static_cast<ptrdiff_t>(off + 107), 0);
        putStr(b, off + 75, sp.samples[i].name);
        std::copy(sp.samples[i].id.begin(), sp.samples[i].id.end(), b.begin() + static_cast<ptrdiff_t>(off + 107));
        b[off + 119] = static_cast<uint8_t>(sp.samples[i].rate >> 8);
        b[off + 120] = static_cast<uint8_t>(sp.samples[i].rate & 0xFFu);
    }
    for (size_t i = 0; i < nd; ++i) {
        const size_t off = D + 126 * i;
        std::fill(b.begin() + static_cast<ptrdiff_t>(off), b.begin() + static_cast<ptrdiff_t>(off + 32), 0);
        putStr(b, off, sp.drums[i].name);
        putBe32(b, off + 32, sp.drums[i].left);
        putBe32(b, off + 36, sp.drums[i].right);
        std::copy(sp.drums[i].id.begin(), sp.drums[i].id.end(), b.begin() + static_cast<ptrdiff_t>(off + 118));
    }
    for (size_t i = 0; i < nz; ++i) {
        putBe32(b, Z + 16 * i, sp.zones[i].sample);
        b[Z + 16 * i + 10] = sp.zones[i].topKey;
    }
    for (size_t i = 0; i < nm; ++i) {
        const size_t off = M + 176 * i;
        std::fill(b.begin() + static_cast<ptrdiff_t>(off), b.begin() + static_cast<ptrdiff_t>(off + 32), 0);
        putStr(b, off, sp.multisamples[i].name);
        putBe32(b, off + 32, sp.multisamples[i].firstZone);
        b[off + 166] = sp.multisamples[i].zoneCount;
        std::copy(sp.multisamples[i].id.begin(), sp.multisamples[i].id.end(), b.begin() + static_cast<ptrdiff_t>(off + 168));
    }
    return b;
}

static Bytes buildSoundBody(const RecordLayout& L, const std::vector<std::pair<Id8, Id8>>& recs, uint8_t filler = 0xA7) {
    Bytes body(L.base + L.record * recs.size(), filler);
    for (size_t q = 0; q < recs.size(); ++q) {
        std::copy(recs[q].first.begin(), recs[q].first.end(),
                  body.begin() + static_cast<ptrdiff_t>(L.base + q * L.record));
        std::copy(recs[q].second.begin(), recs[q].second.end(),
                  body.begin() + static_cast<ptrdiff_t>(L.base + q * L.record + L.pairOffset));
    }
    return body;
}

static bool writeFileBytes(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

static Bytes readFileBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), {});
}

// Builds a standard valid synthetic KST dataset used across multiple test cases.
struct ValidDataset {
    static constexpr uint32_t kKindA = 0x10910038u;
    static constexpr uint32_t kKindB = 0x10920038u;
    static constexpr RecordLayout kLayoutA{20, 24, 10};
    static constexpr RecordLayout kLayoutB{16, 28, 12};

    Id8 sId[5];
    Id8 mId[3];
    Id8 dId[2];
    SynthKmpSpec kmpSpec;
    NativeKstInput input;
    NativeKstOptions opts;

    ValidDataset() {
        for (int i = 0; i < 5; ++i) sId[i] = makeId(0x20, static_cast<uint8_t>(i));
        for (int i = 0; i < 3; ++i) mId[i] = makeId(0x10, static_cast<uint8_t>(i));
        for (int i = 0; i < 2; ++i) dId[i] = makeId(0x30, static_cast<uint8_t>(i));

        kmpSpec.samples = {
            {sId[0], "SynthPianoLo", 48000},
            {sId[1], "SynthPianoHi", 44100},
            {sId[2], "SynthPadStr",  32000},
            {sId[3], "SynthKick",    48000},
            {sId[4], "SynthSnareR",  48000},
        };
        kmpSpec.drums = {
            {dId[0], "DrumKick",  3, kNoIndex},
            {dId[1], "DrumSnare", 3, 4},
        };
        // M0: zones 0..2 -> S0, empty, S1; M1: zone 3 -> S1; M2: zone 4 -> S2
        kmpSpec.zones = {
            {0, 60}, {kNoIndex, 72}, {1, 127},
            {1, 127},
            {2, 127},
        };
        kmpSpec.multisamples = {
            {mId[0], "MS_Piano", 0, 3},
            {mId[1], "MS_Bell",  3, 1},
            {mId[2], "MS_Pad",   4, 1},
        };

        const Bytes kmpPayload = buildPa5xKmpPayload(kmpSpec);
        input.kmpContainer = RawNamedBlob{
            "MULTISMP/ram.kmp", "",
            buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", kmpPayload, ""}}, 1)
        };

        input.pcmContainers.push_back({
            "PCM/ram01.pcm", "",
            buildKorfContainer({
                {0x12010018u, 1, 0, 0, "SynthPianoLo", Bytes(4000, 0x11), idAsStr(sId[0])},
                {0x12010018u, 1, 0, 1, "SynthPianoHi", Bytes(3200, 0x22), idAsStr(sId[1])},
                {0x12010018u, 1, 0, 2, "SynthPadStr",  Bytes(6400, 0x33), idAsStr(sId[2])},
            }, 1)
        });
        input.pcmContainers.push_back({
            "PCM/ram02.pcm", "",
            buildKorfContainer({
                {0x12010018u, 1, 1, 0, "SynthKick",   Bytes(2000, 0x44), idAsStr(sId[3])},
                {0x12010018u, 1, 1, 1, "SynthSnareR", Bytes(2400, 0x55), idAsStr(sId[4])},
            }, 1)
        });

        // Sound 0 in USER01: OC31 compressed with an M3 back-reference duplicating oscillator record 0, then record 2!
        const Bytes rec0 = buildSoundBody(kLayoutA, {{mId[0], mId[1]}});   // 20 + 24 = 44 bytes
        const Bytes rec2 = buildSoundBody(RecordLayout{0, 24, 10}, {{mId[2], kZeroId}});   // 24 bytes
        Bytes expectedSnd0;
        // Prefix = rec0 (44 bytes), copy last 24 bytes (rec0's record) via M3 -> duplicate {mId[0], mId[1]}, then suffix = rec2
        const Bytes snd0Oc31 = makeOcWithMatchFrame("OC31", rec0, 24, 24, rec2, expectedSnd0);

        // Sound 1 in USER01: OC32 compressed (kind B) referencing M0 (High) and M2 (Low)
        const Bytes snd1Plain = buildSoundBody(kLayoutB, {{mId[0], mId[2]}});
        const Bytes snd1Oc32 = makeOcLiteralFrame("OC32", snd1Plain);

        // Sound 2 in USER02: format-0 container with raw uncompressed Sound body referencing M1 (High)
        const Bytes snd2Raw = buildSoundBody(kLayoutA, {{mId[1], kZeroId}});

        input.soundContainers.push_back({
            "SOUND/user01.pcg", "USER01",
            buildKorfContainer({
                {kKindA, 0, 0, 0, "GrandLayered", snd0Oc31, ""},
                {kKindB, 0, 0, 1, "WarmPad",      snd1Oc32, ""},
            }, 1)
        });
        input.soundContainers.push_back({
            "SOUND/user02.pcg", "",   // bank derived from label stem -> "USER02"
            buildKorfContainer({
                {kKindA, 0, 1, 0, "BellRaw", snd2Raw, ""},
            }, 0)
        });

        opts.layouts = {{kKindA, kLayoutA}, {kKindB, kLayoutB}};
    }
};

static bool containsDiag(const std::vector<std::string>& diags, const std::string& needle) {
    for (const std::string& d : diags) if (d.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    std::printf("Test: Native-KST Resource Graph end-to-end integration (OC31 + Pa5X RAM.KMP + PCM -> Graph)\n");

    // ── 1. Complete valid end-to-end resolution (Sound -> Osc -> Multisample -> Zone -> Sample -> PCM + Drum L/R) ────
    {
        ValidDataset ds;
        // Snapshot input bytes before running to assert zero input mutation
        const Bytes origKmp = ds.input.kmpContainer->bytes;
        const Bytes origPcm0 = ds.input.pcmContainers[0].bytes;
        const Bytes origSnd0 = ds.input.soundContainers[0].bytes;

        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        TEST("valid end-to-end KST resolves cleanly",
             res.ok && res.graph.has_value() && res.report.ok() && res.report.coverageComplete());
        TEST("KMP parsed counts and contiguous zones match",
             res.report.kmpPresent && res.report.kmpParsed && res.report.kmp.zonesContiguous &&
             res.report.kmp.samplesTotal == 5 && res.report.kmp.samplesWithPcm == 5 &&
             res.report.kmp.multisamplesTotal == 3 && res.report.kmp.drumsTotal == 2 &&
             res.report.kmp.zonesTotal == 5 && res.report.kmp.missingPcm.empty());
        TEST("OC31/OC32 and raw sound counts match",
             res.report.soundContainers == 2 && res.report.soundsDecodedOc31 == 2 && res.report.soundsLoadedRaw == 1 &&
             res.report.refs.sounds == 3 && res.report.refs.analysed == 3 &&
             res.report.refs.unanalysed.empty() && res.report.refs.unresolved.empty() &&
             res.report.decodeFailures.empty() && res.report.containerErrors.empty());

        const Graph& g = *res.graph;
        TEST("valid graph has zero dangling edges and zero structural issues",
             g.dangling().empty() && g.issues().empty());

        // Verify Sound 0 -> Oscillators -> M0, M1, M2 -> Samples S0, S1, S2 -> PCM files
        const ResourceId snd0{false, "USER01", Kind::Sound, 0};
        const auto dep0 = dependencies(g, snd0);
        auto hasDep = [&](const std::vector<ResourceId>& v, Kind k, uint32_t n) {
            for (const auto& id : v) if (id.kind == k && id.number == n) return true;
            return false;
        };
        TEST("Sound 0 (OC31 + M3 match) reaches M0, M1, M2 and S0, S1, S2 (not drum-only S3/S4)",
             hasDep(dep0, Kind::Multisample, 0) && hasDep(dep0, Kind::Multisample, 1) && hasDep(dep0, Kind::Multisample, 2) &&
             hasDep(dep0, Kind::Sample, 0) && hasDep(dep0, Kind::Sample, 1) && hasDep(dep0, Kind::Sample, 2) &&
             !hasDep(dep0, Kind::Sample, 3) && !hasDep(dep0, Kind::Sample, 4));

        // Verify Sound 1 (OC32) reaches M0, M2 and S0, S1, S2
        const auto dep1 = dependencies(g, {false, "USER01", Kind::Sound, 1});
        TEST("Sound 1 (OC32) reaches M0, M2 and S0, S1, S2",
             hasDep(dep1, Kind::Multisample, 0) && hasDep(dep1, Kind::Multisample, 2) &&
             hasDep(dep1, Kind::Sample, 0) && hasDep(dep1, Kind::Sample, 1) && hasDep(dep1, Kind::Sample, 2));

        // Verify Sound 2 (format-0 raw, bank derived from label stem USER02) reaches M1 and S1
        const auto dep2 = dependencies(g, {false, "USER02", Kind::Sound, 0});
        TEST("Sound 2 (format-0, stem-derived USER02) reaches M1 and S1 only",
             hasDep(dep2, Kind::Multisample, 1) && hasDep(dep2, Kind::Sample, 1) &&
             !hasDep(dep2, Kind::Sample, 0) && !hasDep(dep2, Kind::Sample, 2));

        // Verify Sample -> PCM facts (rate from KMP + file from PCM container + derived frames)
        const Node* nS0 = g.find({false, "KST", Kind::Sample, 0});
        const Node* nS1 = g.find({false, "KST", Kind::Sample, 1});
        const Node* nS4 = g.find({false, "KST", Kind::Sample, 4});
        TEST("Sample nodes carry KMP rates and PCM container file targets",
             nS0 && nS0->sample && nS0->sample->rate == 48000 && nS0->sample->frames == 2000 && nS0->sample->file == "PCM/ram01.pcm" &&
             nS1 && nS1->sample && nS1->sample->rate == 44100 && nS1->sample->frames == 1600 && nS1->sample->file == "PCM/ram01.pcm" &&
             nS4 && nS4->sample && nS4->sample->rate == 48000 && nS4->sample->frames == 1200 && nS4->sample->file == "PCM/ram02.pcm");

        // Verify Drum L/R chains: DrumSample/0 (L=S3 only) and DrumSample/1 (L=S3, R=S4)
        const ResourceId dr0{false, "KST", Kind::DrumSample, 0};
        const ResourceId dr1{false, "KST", Kind::DrumSample, 1};
        const auto dr0Edges = g.uses(dr0);
        const auto dr1Edges = g.uses(dr1);
        TEST("DrumSample/0 (mono L) and DrumSample/1 (stereo L/R) resolve to Sample -> PCM",
             dr0Edges.size() == 1 && dr0Edges[0].via == Via::DrumSampleOf && dr0Edges[0].detail == "L" &&
             dr0Edges[0].to == (ResourceId{false, "KST", Kind::Sample, 3}) && dr0Edges[0].confidence == Confidence::Confirmed &&
             dr1Edges.size() == 2 &&
             dr1Edges[0].via == Via::DrumSampleOf && dr1Edges[1].via == Via::DrumSampleOf &&
             g.closure({dr1}).count({false, "KST", Kind::Sample, 3}) == 1 &&
             g.closure({dr1}).count({false, "KST", Kind::Sample, 4}) == 1);

        // Verify shared resource counts across Sounds
        const auto sharedMs = sharedCounts(g, Kind::Multisample);
        const auto sharedSmp = sharedCounts(g, Kind::Sample);
        TEST("sharedCounts identifies shared Multisamples and Samples across Sounds",
             sharedMs.at({false, "KST", Kind::Multisample, 0}) == 2 &&
             sharedMs.at({false, "KST", Kind::Multisample, 1}) == 2 &&
             sharedMs.at({false, "KST", Kind::Multisample, 2}) == 2 &&
             sharedSmp.at({false, "KST", Kind::Sample, 0}) == 2 &&
             sharedSmp.at({false, "KST", Kind::Sample, 1}) == 3 &&
             sharedSmp.at({false, "KST", Kind::Sample, 2}) == 2);

        // Verify confidence separation: Zone + DrumSampleOf Confirmed, Oscillator/High/Low Inferred
        bool confOk = true;
        for (const Edge& e : g.edges()) {
            if (e.via == Via::Zone || e.via == Via::DrumSampleOf) confOk &= (e.confidence == Confidence::Confirmed);
            else confOk &= (e.confidence == Confidence::Inferred);
        }
        TEST("KMP Zone/DrumSampleOf edges are Confirmed; Sound/Oscillator edges are Inferred", confOk);

        // Delete safety: NO resource in a native KST graph is ever concluded Unreferenced
        bool anyUnref = false;
        for (const auto& [id, node] : g.nodes()) {
            (void)node;
            anyUnref |= (g.deleteCheck(id).verdict == DeleteVerdict::Unreferenced);
        }
        TEST("delete safety: no resource in native KST graph is ever Unreferenced", !anyUnref);

        // Input buffers were never mutated
        TEST("input buffers remain byte-for-byte immutable",
             ds.input.kmpContainer->bytes == origKmp &&
             ds.input.pcmContainers[0].bytes == origPcm0 &&
             ds.input.soundContainers[0].bytes == origSnd0);
    }

    // ── 2. Auto-layout inference (fitLayout) over OC31-decoded Sounds ────────────────────────────────────────────────
    {
        ValidDataset ds;
        ds.input.soundContainers.clear();
        ds.opts.layouts.clear();
        ds.opts.allowFitLayout = true;
        ds.opts.fitMinItems = 8;

        const RecordLayout invented{14, 26, 11};
        constexpr uint32_t kInventedKind = 0x10AA0038u;
        std::vector<SynthItem> items;
        for (uint8_t i = 0; i < 10; ++i) {
            std::vector<std::pair<Id8, Id8>> recs;
            recs.push_back({ds.mId[i % 3], (i % 2 == 0) ? ds.mId[(i + 1) % 3] : kZeroId});
            if (i > 0 && (i % 3 == 0)) recs.push_back({ds.mId[2], ds.mId[0]});   // item 0 keeps 1 record so size is unambiguous
            const Bytes plain = buildSoundBody(invented, recs);
            const Bytes oc = makeOcLiteralFrame((i & 1) ? "OC32" : "OC31", plain);
            items.push_back({kInventedKind, 0, 0, i, "FitSnd" + std::to_string(i), oc, ""});
        }
        ds.input.soundContainers.push_back({"SOUND/user03.pcg", "USER03", buildKorfContainer(items, 1)});

        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        const auto it = res.report.usedLayouts.find(kInventedKind);
        TEST("fitLayout automatically derives layout from OC31/OC32-decoded Sounds and resolves all 10",
             res.ok && res.report.ok() && res.report.coverageComplete() &&
             res.report.soundsDecodedOc31 == 10 && res.report.refs.analysed == 10 &&
             it != res.report.usedLayouts.end() &&
             it->second.base == invented.base && it->second.record == invented.record && it->second.pairOffset == invented.pairOffset);
    }

    // ── 3. Missing references detection (dangling multisample, missing PCM targets, missing PCM IDs, zero-rate) ──────
    {
        // 3a: Dangling multisample ID + wrong-kind (Drum ID and Sample ID in multisample slots)
        ValidDataset ds;
        const Id8 unknownMs = makeId(0xEE, 0x99);
        const Bytes badRefsPlain = buildSoundBody(ValidDataset::kLayoutA, {
            {ds.mId[0], unknownMs},    // slot 1 unknown multisample ID
            {ds.dId[0], ds.sId[0]},    // slot 0 drum ID (wrongKind), slot 1 sample ID (wrongKind)
        });
        ds.input.soundContainers = {{
            "SOUND/user01.pcg", "USER01",
            buildKorfContainer({{ValidDataset::kKindA, 0, 0, 0, "BadRefs", makeOcLiteralFrame("OC31", badRefsPlain), ""}}, 1)
        }};
        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        size_t wrongKindCount = 0, unknownCount = 0;
        for (const auto& u : res.report.refs.unresolved) (u.wrongKind ? wrongKindCount : unknownCount)++;
        TEST("dangling multisample ID and wrong-kind Drum/Sample IDs detected accurately",
             res.ok && !res.report.coverageComplete() &&
             res.report.refs.unresolved.size() == 3 && wrongKindCount == 2 && unknownCount == 1 &&
             containsDiag(res.report.diagnostics, "unresolved slot"));
    }
    {
        // 3b: Missing PCM targets for Sample 2 (used by M2 zone) and Sample 4 (used by DrumSnare R)
        ValidDataset ds;
        ds.input.pcmContainers = {
            {"PCM/ram01.pcm", "", buildKorfContainer({
                {0x12010018u, 1, 0, 0, "SynthPianoLo", Bytes(4000, 0x11), idAsStr(ds.sId[0])},
                {0x12010018u, 1, 0, 1, "SynthPianoHi", Bytes(3200, 0x22), idAsStr(ds.sId[1])},
            }, 1)},
            {"PCM/ram02.pcm", "", buildKorfContainer({
                {0x12010018u, 1, 1, 0, "SynthKick", Bytes(2000, 0x44), idAsStr(ds.sId[3])},
            }, 1)},
        };
        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        TEST("missing PCM targets reported in kmp.missingPcm and fail coverageComplete()",
             res.ok && res.graph.has_value() && !res.report.coverageComplete() &&
             res.report.kmp.missingPcm.size() == 2 &&
             res.report.kmp.samplesWithPcm == 3 &&
             containsDiag(res.report.diagnostics, "missing pcm"));

        const auto dang = res.graph->dangling();
        bool hasZoneDang = false, hasDrumDang = false;
        for (const Edge& e : dang) {
            if (e.via == Via::Zone && e.to == (ResourceId{false, "KST", Kind::Sample, 2})) hasZoneDang = true;
            if (e.via == Via::DrumSampleOf && e.detail == "R" && e.to == (ResourceId{false, "KST", Kind::Sample, 4})) hasDrumDang = true;
        }
        const MemoryBudget b = budget(*res.graph);
        TEST("missing PCM targets create explicit dangling Zone and DrumSampleOf edges + budget warning",
             dang.size() == 2 && hasZoneDang && hasDrumDang && b.unknownSizeSamples == 2 &&
             containsDiag(res.report.diagnostics, "dangling:"));
    }
    {
        // 3c: PCM item lacking 8-byte sample ID (Pa700-style PCM without directory field 4)
        ValidDataset ds;
        ds.input.pcmContainers.push_back({
            "PCM/legacy.pcm", "",
            buildKorfContainer({{0x12010018u, 1, 2, 0, "NoIdSample", Bytes(1000, 0x77), ""}}, 1)
        });
        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        TEST("PCM sample without 8-byte sample ID flagged and marks coverage incomplete",
             res.ok && res.report.pcmSamplesWithoutId == 1 && !res.report.coverageComplete() &&
             containsDiag(res.report.diagnostics, "lacks 8-byte PCM sample ID"));
    }
    {
        // 3d: KMP sample with rate == 0 and PCM item with payloadBytes == 0 -> no bogus Sample node
        ValidDataset ds;
        ds.kmpSpec.samples[0].rate = 0;
        ds.input.kmpContainer->bytes = buildKorfContainer(
            {{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", buildPa5xKmpPayload(ds.kmpSpec), ""}}, 1);
        ds.input.pcmContainers[0].bytes = buildKorfContainer({
            {0x12010018u, 1, 0, 0, "SynthPianoLo", Bytes(4000, 0x11), idAsStr(ds.sId[0])},
            {0x12010018u, 1, 0, 1, "SynthPianoHi", Bytes{},           idAsStr(ds.sId[1])},   // empty payload
            {0x12010018u, 1, 0, 2, "SynthPadStr",  Bytes(6400, 0x33), idAsStr(ds.sId[2])},
        }, 1);
        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        TEST("zero-rate KMP sample and empty PCM payload fail closed as missing PCM facts",
             res.ok && !res.report.coverageComplete() && res.report.kmp.missingPcm.size() == 2 &&
             !res.graph->find({false, "KST", Kind::Sample, 0}) &&
             !res.graph->find({false, "KST", Kind::Sample, 1}));
    }

    // ── 4. Unresolved dependencies & malformed / corrupt data (strictly fail-closed) ─────────────────────────────────
    {
        // 4a: Absent RAM.KMP
        ValidDataset ds;
        ds.input.kmpContainer.reset();
        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        TEST("absent RAM.KMP fails closed with explicit unresolved diagnostic",
             !res.ok && !res.graph.has_value() && !res.report.ok() && !res.report.coverageComplete() &&
             !res.report.kmpPresent && !res.report.kmpParsed &&
             res.report.kmpError.find("absent") != std::string::npos);
    }
    {
        // 4b & 4c & 4d: Corrupt / truncated / invalid RAM.KMP variants
        auto checkBadKmp = [&](const char* label, const Bytes& kmpContainerBytes) {
            ValidDataset ds;
            ds.input.kmpContainer = RawNamedBlob{"MULTISMP/ram.kmp", "", kmpContainerBytes};
            const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
            TEST(label, !res.ok && !res.graph.has_value() && res.report.kmpPresent && !res.report.kmpParsed &&
                        !res.report.kmpError.empty() && !res.report.ok() && !res.report.coverageComplete());
        };

        ValidDataset base;
        const Bytes validPayload = buildPa5xKmpPayload(base.kmpSpec);

        // Truncated KMP payload (by 1 byte)
        Bytes truncPayload = validPayload;
        truncPayload.pop_back();
        checkBadKmp("truncated RAM.KMP payload fails closed",
                    buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", truncPayload, ""}}, 1));

        // Bad version (0x0104)
        Bytes badVer = validPayload;
        badVer[1] = 0x04;
        checkBadKmp("unsupported RAM.KMP version fails closed",
                    buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", badVer, ""}}, 1));

        // Huge / overflow counts (0xFFFFFFFF) without OOM
        Bytes hugeCounts(18, 0xFF);
        hugeCounts[0] = 0x01; hugeCounts[1] = 0x03;
        checkBadKmp("RAM.KMP with 0xFFFFFFFF header counts fails closed without allocating",
                    buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", hugeCounts, ""}}, 1));

        // Out-of-range zone sample index
        SynthKmpSpec badZone = base.kmpSpec;
        badZone.zones[0].sample = 99;
        checkBadKmp("RAM.KMP with out-of-range zone sample index fails closed",
                    buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", buildPa5xKmpPayload(badZone), ""}}, 1));

        // Out-of-range multisample zone span
        SynthKmpSpec badSpan = base.kmpSpec;
        badSpan.multisamples[2].firstZone = 4;
        badSpan.multisamples[2].zoneCount = 5;
        checkBadKmp("RAM.KMP with multisample zones past zone table fails closed",
                    buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", buildPa5xKmpPayload(badSpan), ""}}, 1));

        // Out-of-range drum R sample index
        SynthKmpSpec badDrum = base.kmpSpec;
        badDrum.drums[1].right = 77;
        checkBadKmp("RAM.KMP with out-of-range drum R index fails closed",
                    buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", buildPa5xKmpPayload(badDrum), ""}}, 1));

        // OC31-encoded KMP payload (Pa700/Pa4X unsupported layout)
        const Bytes ocKmp = makeOcLiteralFrame("OC31", Bytes(32, 0x41));
        checkBadKmp("OC31-encoded KMP item fails closed (unsupported layout)",
                    buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", ocKmp, ""}}, 1));

        // Wrong KMP chunk ID
        checkBadKmp("KMP container with wrong item chunk ID fails closed",
                    buildKorfContainer({{0x11080019u, 0, 0, 0, "RAM.KMP", validPayload, ""}}, 1));

        // Corrupt KORF wrapper for KMP
        checkBadKmp("corrupt KMP KORF wrapper fails closed", Bytes{0x01, 0x00, 0x01, 0x14, 0x00, 0x00, 0x00, 0x99});
    }
    {
        // 4e: Direct kmpIndexFromPa5x() defense-in-depth on hand-constructed invalid Pa5xKmp
        Pa5xKmp bad;
        bad.samples = {{makeId(0x20, 0), "s0", 48000}};
        bad.zones = {{5, 60}};   // out of range
        bad.multisamples = {{makeId(0x10, 0), "m0", 0, 1}};
        KmpIndex idx;
        KmpBuildDiagnostics diag;
        std::string err;
        TEST("kmpIndexFromPa5x rejects hand-constructed out-of-range zone sample index",
             !kmpIndexFromPa5x(bad, PcmInventory{}, idx, &diag, &err) && idx.samples.empty() && !err.empty());

        Pa5xKmp dupMs;
        dupMs.multisamples = {{makeId(0x10, 0), "m0", 0, 0}, {makeId(0x10, 0), "m1", 0, 0}};
        TEST("kmpIndexFromPa5x rejects duplicate multisample IDs",
             !kmpIndexFromPa5x(dupMs, PcmInventory{}, idx, &diag, &err) && idx.multisamples.empty() &&
             err.find("duplicate") != std::string::npos);
    }
    {
        // 4f: Corrupt OC31/OC32 Sound payloads alongside one valid Sound -> explicit decodeFailures + Gap::DecodeFailed
        ValidDataset ds;
        const Bytes goodPlain = buildSoundBody(ValidDataset::kLayoutA, {{ds.mId[0], ds.mId[1]}});
        const Bytes goodOc = makeOcLiteralFrame("OC31", goodPlain);

        Bytes badXor = goodOc; badXor.back() ^= 0xFFu;                                            // bad XOR
        Bytes truncOc(goodOc.begin(), goodOc.begin() + 14);                                       // truncated frame
        Bytes trailingOc = goodOc; trailingOc.insert(trailingOc.end() - 1, 0x00);                 // trailing byte after 01 00 00
        Bytes unverifiedOp = frameOcRaw("OC31", 8, {0x02, 'a', 'b', 'c', 'd', 0x02, 'e', 'f', 'g', 'h', 0x01, 0x00, 0x00}, 0);
        Bytes overMax = frameOcRaw("OC31", static_cast<uint32_t>(kMaxOc31DecodedBytes + 1), {0x01, 0x00, 0x00}, 0);
        Bytes fake64M = frameOcRaw("OC31", static_cast<uint32_t>(kMaxOc31DecodedBytes), {0x01, 0x00, 0x00}, 0);

        ds.input.soundContainers = {{
            "SOUND/user01.pcg", "USER01",
            buildKorfContainer({
                {ValidDataset::kKindA, 0, 0, 0, "ValidSnd",     goodOc,       ""},
                {ValidDataset::kKindA, 0, 0, 1, "BadXor",       badXor,       ""},
                {ValidDataset::kKindA, 0, 0, 2, "TruncOc",      truncOc,      ""},
                {ValidDataset::kKindA, 0, 0, 3, "TrailingOc",   trailingOc,   ""},
                {ValidDataset::kKindA, 0, 0, 4, "UnverifiedOp", unverifiedOp, ""},
                {ValidDataset::kKindA, 0, 0, 5, "Over64M",      overMax,      ""},
                {ValidDataset::kKindA, 0, 0, 6, "Fake64M",      fake64M,      ""},
            }, 1)
        }};
        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        size_t decodeFailedGaps = 0;
        for (const auto& u : res.report.refs.unanalysed) decodeFailedGaps += (u.why == Gap::DecodeFailed);
        TEST("corrupt OC31 frames fail closed into decodeFailures + Gap::DecodeFailed while valid sibling resolves",
             res.ok && res.graph.has_value() && !res.report.ok() && !res.report.coverageComplete() &&
             res.report.soundsDecodedOc31 == 1 && res.report.refs.analysed == 1 &&
             res.report.decodeFailures.size() == 6 && decodeFailedGaps == 6 &&
             res.graph->uses({false, "USER01", Kind::Sound, 1}).empty() &&
             !res.graph->uses({false, "USER01", Kind::Sound, 0}).empty());

        // SoundPayloadMode::RequireOc31 rejects raw non-OC31 payloads
        ds.input.soundContainers = {{
            "SOUND/user01.pcg", "USER01",
            buildKorfContainer({{ValidDataset::kKindA, 0, 0, 0, "RawNotAllowed", goodPlain, ""}}, 1)
        }};
        ds.opts.soundMode = SoundPayloadMode::RequireOc31;
        const NativeKstResult reqRes = resolveNativeKst(ds.input, ds.opts);
        TEST("SoundPayloadMode::RequireOc31 rejects non-OC31 Sound payloads",
             reqRes.ok && !reqRes.report.ok() && reqRes.report.decodeFailures.size() == 1 &&
             reqRes.report.decodeFailures[0].reason.find("OC31") != std::string::npos);
    }
    {
        // 4g: Unknown Sound kind, size mismatch, all-empty slots
        ValidDataset ds;
        ds.opts.allowFitLayout = false;
        const Bytes okBody = buildSoundBody(ValidDataset::kLayoutA, {{ds.mId[0], kZeroId}});
        Bytes badSizeBody = okBody; badSizeBody.push_back(0x00);
        const Bytes emptyBody = buildSoundBody(ValidDataset::kLayoutA, {{kZeroId, kZeroId}});
        ds.input.soundContainers = {{
            "SOUND/user01.pcg", "USER01",
            buildKorfContainer({
                {0x10FF0038u,           0, 0, 0, "UnknownKind", makeOcLiteralFrame("OC31", okBody),      ""},
                {ValidDataset::kKindA,  0, 0, 1, "BadSize",     makeOcLiteralFrame("OC31", badSizeBody), ""},
                {ValidDataset::kKindA,  0, 0, 2, "EmptySlots",  makeOcLiteralFrame("OC31", emptyBody),   ""},
            }, 1)
        }};
        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        auto hasGap = [&](uint32_t num, Gap g) {
            for (const auto& u : res.report.refs.unanalysed) if (u.sound.number == num && u.why == g) return true;
            return false;
        };
        TEST("unknown Sound kind, size mismatch, and empty slots reported with exact Gap reasons",
             res.ok && res.report.refs.analysed == 0 && !res.report.coverageComplete() &&
             hasGap(0, Gap::NoLayoutForKind) && hasGap(1, Gap::SizeDoesNotFit) && hasGap(2, Gap::NoKnownReference));
    }
    {
        // 4h: Corrupt .pcg container, corrupt .pcm container, duplicate PCM ID, duplicate Sound slot
        ValidDataset ds;
        ds.input.pcmContainers.push_back({"PCM/corrupt.pcm", "", Bytes{0x01, 0x00, 0x01, 0x14, 0, 0, 0, 99}});
        ds.input.pcmContainers.push_back({
            "PCM/dup.pcm", "",
            buildKorfContainer({{0x12010018u, 1, 9, 0, "DupS0", Bytes(100, 0x11), idAsStr(ds.sId[0])}}, 1)
        });
        ds.input.soundContainers.push_back({"SOUND/corrupt.pcg", "USER09", Bytes{'B', 'A', 'D', '!'}});
        // Duplicate Sound slot USER01/0 in another container
        const Bytes dupPlain = buildSoundBody(ValidDataset::kLayoutA, {{ds.mId[0], kZeroId}});
        ds.input.soundContainers.push_back({
            "SOUND/user01_dup.pcg", "USER01",
            buildKorfContainer({{ValidDataset::kKindA, 0, 0, 0, "DupSlot0", makeOcLiteralFrame("OC31", dupPlain), ""}}, 1)
        });
        const NativeKstResult res = resolveNativeKst(ds.input, ds.opts);
        TEST("corrupt containers, duplicate PCM sample ID, and duplicate Sound slot fail closed with diagnostics",
             res.ok && !res.report.ok() && !res.report.coverageComplete() &&
             res.report.containerErrors.size() == 4 && res.report.decodeFailures.size() == 1);

        // Invalid bank scope (contains space) rejected fail-closed
        const SoundExtractResult badBank = extractSoundsFromPcgBytes(
            ds.input.soundContainers[0].bytes.data(), ds.input.soundContainers[0].bytes.size(),
            "BAD BANK", "SOUND/bad bank.pcg");
        TEST("invalid bank scope rejected fail-closed",
             !badBank.containerOk && !badBank.containerError.empty() && badBank.sounds.empty());

        // Explicit extraPcm SampleInfo (valid stereo looped vs invalid 3-channel refused) + Drum L==R
        ValidDataset dsExtra;
        dsExtra.kmpSpec.drums.push_back({makeId(0x30, 2), "DrumMonoDual", 3, 3});   // L and R share S3
        dsExtra.input.kmpContainer->bytes = buildKorfContainer(
            {{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", buildPa5xKmpPayload(dsExtra.kmpSpec), ""}}, 1);
        dsExtra.input.pcmContainers.clear();
        PcmInventory customPcm;
        for (int i = 0; i < 5; ++i) {
            SampleInfo si{static_cast<uint8_t>(i == 0 ? 2 : 1), 48000, 16, 4096, static_cast<uint64_t>(i == 0 ? 1024 : 0), "", ""};
            customPcm.byId.emplace(dsExtra.sId[i], PcmEntry{dsExtra.sId[i], "S" + std::to_string(i), "PCM/custom.pcm", 0, si});
        }
        dsExtra.input.extraPcm = customPcm;
        const NativeKstResult extraOk = resolveNativeKst(dsExtra.input, dsExtra.opts);
        const Node* stereoS0 = extraOk.graph ? extraOk.graph->find({false, "KST", Kind::Sample, 0}) : nullptr;
        TEST("extraPcm explicit stereo/looped SampleInfo and Drum L==R resolve cleanly",
             extraOk.ok && extraOk.report.ok() && extraOk.report.coverageComplete() &&
             stereoS0 && stereoS0->sample && stereoS0->sample->channels == 2 && stereoS0->sample->loopFrames == 1024 &&
             stereoS0->sample->file == "PCM/custom.pcm" &&
             extraOk.graph->uses({false, "KST", Kind::DrumSample, 2}).size() == 2);

        dsExtra.input.extraPcm->byId.at(dsExtra.sId[0]).info->channels = 3;   // invalid channels
        const NativeKstResult extraBad = resolveNativeKst(dsExtra.input, dsExtra.opts);
        TEST("invalid explicit SampleInfo in extraPcm fails closed",
             !extraBad.ok && !extraBad.graph.has_value() && !extraBad.report.kmpParsed &&
             extraBad.report.kmpError.find("invalid PCM SampleInfo") != std::string::npos);
    }

    // ── 5. Safe read-only disk folder resolution & hostile path hardening ────────────────────────────────────────────
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tmpRoot = fs::temp_directory_path() /
            ("aiarr_native_kst_integ_" + std::to_string(::getpid()) + "_" + std::to_string(stamp));
        const fs::path kstDir = tmpRoot / "SYNTH.KST";
        fs::create_directories(kstDir);

        ValidDataset ds;
        const bool wrote =
            writeFileBytes(kstDir / "MULTISMP" / "ram.kmp", ds.input.kmpContainer->bytes) &&
            writeFileBytes(kstDir / "PCM" / "ram01.pcm", ds.input.pcmContainers[0].bytes) &&
            writeFileBytes(kstDir / "PCM" / "ram02.pcm", ds.input.pcmContainers[1].bytes) &&
            writeFileBytes(kstDir / "SOUND" / "user01.pcg", ds.input.soundContainers[0].bytes) &&
            writeFileBytes(kstDir / "SOUND" / "user02.pcg", ds.input.soundContainers[1].bytes);
        TEST("synthetic KST folder written to temp dir for read-only test", wrote);

        // 5a: Valid disk folder resolves identically and mutates zero files
        const NativeKstResult diskRes = resolveNativeKstFolder(kstDir, ds.opts);
        TEST("resolveNativeKstFolder resolves valid KST on disk cleanly",
             diskRes.ok && diskRes.graph.has_value() && diskRes.report.ok() && diskRes.report.coverageComplete() &&
             diskRes.report.soundsDecodedOc31 == 2 && diskRes.report.soundsLoadedRaw == 1 &&
             diskRes.report.refs.analysed == 3 && diskRes.graph->dangling().empty() &&
             diskRes.report.skippedPaths.empty());
        TEST("resolveNativeKstFolder leaves every disk file byte-identical (strictly read-only)",
             readFileBytes(kstDir / "MULTISMP" / "ram.kmp") == ds.input.kmpContainer->bytes &&
             readFileBytes(kstDir / "SOUND" / "user01.pcg") == ds.input.soundContainers[0].bytes);

        // 5b: Total read cap enforced (zero OOM on large folders)
        NativeKstOptions tinyCap = ds.opts;
        tinyCap.maxTotalBytes = 64;
        const NativeKstResult capRes = resolveNativeKstFolder(kstDir, tinyCap);
        TEST("maxTotalBytes cap refuses oversized reads fail-closed",
             !capRes.ok && !capRes.report.ok() && !capRes.report.containerErrors.empty());

        // 5c: Symlink as KST root refused
        const fs::path rootLink = tmpRoot / "LINK.KST";
        std::error_code ec;
        fs::create_directory_symlink(kstDir, rootLink, ec);
        if (!ec) {
            const NativeKstResult linkRes = resolveNativeKstFolder(rootLink, ds.opts);
            TEST("symlink as KST root is refused (O_NOFOLLOW)",
                 !linkRes.ok && !linkRes.graph.has_value() && !linkRes.report.containerErrors.empty());
        }

        // 5d: Non-existent path and regular file as KST root refused
        const NativeKstResult missingRes = resolveNativeKstFolder(tmpRoot / "DOES_NOT_EXIST.KST", ds.opts);
        const NativeKstResult fileAsRootRes = resolveNativeKstFolder(kstDir / "MULTISMP" / "ram.kmp", ds.opts);
        TEST("non-existent path and regular file as KST root are refused",
             !missingRes.ok && !missingRes.report.containerErrors.empty() &&
             !fileAsRootRes.ok && !fileAsRootRes.report.containerErrors.empty());

        // 5e: Hostile symlinked SOUND subdir, symlinked ram.kmp, and FIFO inside SOUND/
        const fs::path hostileKst = tmpRoot / "HOSTILE.KST";
        fs::create_directories(hostileKst / "MULTISMP");
        fs::create_directories(hostileKst / "PCM");
        fs::create_directories(hostileKst / "SOUND");
        writeFileBytes(hostileKst / "PCM" / "ram01.pcm", ds.input.pcmContainers[0].bytes);
        writeFileBytes(hostileKst / "SOUND" / "user01.pcg", ds.input.soundContainers[0].bytes);
        fs::create_symlink(kstDir / "MULTISMP" / "ram.kmp", hostileKst / "MULTISMP" / "ram.kmp", ec);
        const fs::path fifoPath = hostileKst / "SOUND" / "hang.pcg";
        const bool madeFifo = (::mkfifo(fifoPath.c_str(), 0600) == 0);
        const NativeKstResult hostileRes = resolveNativeKstFolder(hostileKst, ds.opts);
        TEST("symlinked RAM.KMP and FIFO in SOUND/ are skipped without following or hanging",
             madeFifo && !hostileRes.ok && !hostileRes.graph.has_value() && !hostileRes.report.ok() &&
             std::find(hostileRes.report.skippedPaths.begin(), hostileRes.report.skippedPaths.end(), "MULTISMP/ram.kmp") != hostileRes.report.skippedPaths.end() &&
             std::find(hostileRes.report.skippedPaths.begin(), hostileRes.report.skippedPaths.end(), "SOUND/hang.pcg") != hostileRes.report.skippedPaths.end());

        // 5f: Symlinked SOUND directory at top level is never followed
        const fs::path symDirKst = tmpRoot / "SYMDIR.KST";
        fs::create_directories(symDirKst / "MULTISMP");
        fs::create_directories(symDirKst / "PCM");
        writeFileBytes(symDirKst / "MULTISMP" / "ram.kmp", ds.input.kmpContainer->bytes);
        writeFileBytes(symDirKst / "PCM" / "ram01.pcm", ds.input.pcmContainers[0].bytes);
        writeFileBytes(symDirKst / "PCM" / "ram02.pcm", ds.input.pcmContainers[1].bytes);
        fs::create_directory_symlink(kstDir / "SOUND", symDirKst / "SOUND", ec);
        const NativeKstResult symDirRes = resolveNativeKstFolder(symDirKst, ds.opts);
        TEST("symlinked SOUND/ directory is refused without following",
             !symDirRes.report.ok() && !symDirRes.report.coverageComplete() &&
             symDirRes.report.soundContainers == 0 &&
             std::find(symDirRes.report.skippedPaths.begin(), symDirRes.report.skippedPaths.end(), "SOUND") != symDirRes.report.skippedPaths.end());

        // 5g: Multiple .kmp files in MULTISMP/ fails closed without guessing
        const fs::path multiKmp = tmpRoot / "MULTIKMP.KST";
        writeFileBytes(multiKmp / "MULTISMP" / "ram.kmp", ds.input.kmpContainer->bytes);
        writeFileBytes(multiKmp / "MULTISMP" / "extra.kmp", ds.input.kmpContainer->bytes);
        const NativeKstResult multiRes = resolveNativeKstFolder(multiKmp, ds.opts);
        TEST("multiple .kmp files in MULTISMP/ fail closed without guessing",
             !multiRes.ok && !multiRes.graph.has_value() && !multiRes.report.ok() &&
             containsDiag(multiRes.report.diagnostics, "multiple .kmp files"));

        fs::remove_all(tmpRoot, ec);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
