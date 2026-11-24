// M1 Synthetic Integration & Determinism Tests for Read-Only Native-KST Dependency Closure & Manifest:
//   Style (.stg) / KeyboardSet (.ksc) / Pad (.pdc)
//     -> Sound (SOUND/USERnn.PCG, SOUND/USERDKnn.PCG)
//     -> Oscillator -> Multisample (MULTISMP/RAM.KMP) -> Zone -> Sample -> PCM (PCM/*.PCM)
//
// 100% synthetic, original fixtures only (zero owner library files, proprietary bytes, real names, or local paths).
// Strictly read-only: no KST/SET writes, no OC31 encoder in production code, deleteCheck() NEVER returns Unreferenced.

#include "media/sound/native_kst_closure.h"
#include "media/panative/root_tree.h"

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
    b.insert(b.end(), reinterpret_cast<const uint8_t*>(s.data()), reinterpret_cast<const uint8_t*>(s.data()) + s.size());
}

static Id8 makeId(uint8_t tag, uint8_t idx) {
    return Id8{tag, idx, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76};
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

static Bytes buildSoundBody(const RecordLayout& L, const std::vector<std::pair<Id8, Id8>>& oscs) {
    Bytes body(L.base + oscs.size() * L.record, 0x5Au);
    for (size_t q = 0; q < oscs.size(); ++q) {
        const size_t at = L.base + q * L.record;
        std::copy(oscs[q].first.begin(), oscs[q].first.end(), body.begin() + static_cast<std::ptrdiff_t>(at));
        std::copy(oscs[q].second.begin(), oscs[q].second.end(), body.begin() + static_cast<std::ptrdiff_t>(at + L.pairOffset));
    }
    return body;
}

// Synthetic Root-tree + SMF builders
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
    const bool ok = roottree::write(r, out, err);
    if (!ok) return {};
    return Bytes(out.begin(), out.end());
}

struct SmfSelSpec {
    uint8_t ch = 0;   // 0..15
    int msb = -1;     // -1 = omit CC0
    int lsb = -1;     // -1 = omit CC32
    uint8_t pc = 0;
};

static std::string makeSmf(const std::vector<SmfSelSpec>& sels, uint16_t format = 0, bool injectHighBitCc11 = false) {
    std::string trk;
    for (const auto& s : sels) {
        const uint8_t ccSt = static_cast<uint8_t>(0xB0u | (s.ch & 0x0Fu));
        const uint8_t pcSt = static_cast<uint8_t>(0xC0u | (s.ch & 0x0Fu));
        if (s.msb >= 0) { trk.push_back(0); trk.push_back(char(ccSt)); trk.push_back(0); trk.push_back(char(s.msb)); }
        if (s.lsb >= 0) { trk.push_back(0); trk.push_back(char(ccSt)); trk.push_back(32); trk.push_back(char(s.lsb)); }
        trk.push_back(0); trk.push_back(char(pcSt)); trk.push_back(char(s.pc));
    }
    if (injectHighBitCc11) {
        // Synthesizes the exact observed Pa5X malformed Pad CC11>=0x80 class (high-bit data byte)
        trk.push_back(0); trk.push_back(char(0xB0)); trk.push_back(0x0B); trk.push_back(char(0xE4));
    }
    trk.push_back(0); trk.push_back(char(0xFF)); trk.push_back(0x2F); trk.push_back(0);

    Bytes b;
    appendStr(b, "MThd");
    pushBe32(b, 6); pushBe16(b, format); pushBe16(b, 1); pushBe16(b, 384);
    appendStr(b, "MTrk");
    pushBe32(b, static_cast<uint32_t>(trk.size()));
    appendStr(b, trk);
    return std::string(b.begin(), b.end());
}

static Bytes makePadTree(const std::vector<SmfSelSpec>& sels, bool badCc11 = false, const std::string& leafName = "Main.mid") {
    return rtSerialize({
        rtLeaf(leafName, makeSmf(sels, 1, badCc11)),
        rtLeaf("ClipDescription.xml", "<CLIP type=\"PadClip\" version=\"0\" release=\"4\"/>")
    });
}

static Bytes makeKscFlatTree(const std::vector<SmfSelSpec>& lower, const std::vector<SmfSelSpec>& u1,
                             const std::vector<SmfSelSpec>& u2, const std::vector<SmfSelSpec>& u3) {
    return rtSerialize({
        rtLeaf("Lower.mid", makeSmf(lower, 0)),
        rtLeaf("Upper1.mid", makeSmf(u1, 0)),
        rtLeaf("Upper2.mid", makeSmf(u2, 0)),
        rtLeaf("Upper3.mid", makeSmf(u3, 0)),
        rtLeaf("MFX.mid", makeSmf({}, 0)),
        rtLeaf("ClipDescription.xml", "<CLIP type=\"KeyboardSetClip\" subtype=\"4\" version=\"1\" release=\"0\"/>")
    });
}

static roottree::Node makeKscSessionBody(const std::vector<SmfSelSpec>& lower, const std::vector<SmfSelSpec>& u1) {
    return rtBranch("Main.ssn", {
        rtBranch("RTTracks.scn", {
            rtBranch("Lower.plc", {rtLeaf("Main.mid", makeSmf(lower, 0)), rtLeaf("ClipDescription.xml", "<CLIP/>")}),
            rtBranch("Upper1.plc", {rtLeaf("Main.mid", makeSmf(u1, 0)), rtLeaf("ClipDescription.xml", "<CLIP/>")}),
            rtLeaf("SceneDescription.xml", "<SCENE/>")
        }),
        rtLeaf("SessionDescription.xml", "<SESSION/>")
    });
}

static Bytes makeKscSessionTree(const std::vector<SmfSelSpec>& lower, const std::vector<SmfSelSpec>& u1) {
    return rtSerialize({
        makeKscSessionBody(lower, u1),
        rtLeaf("MFX.mid", makeSmf({}, 0)),
        rtLeaf("ClipDescription.xml", "<CLIP type=\"KeyboardSetClip\" version=\"0\" release=\"4\"/>")
    });
}

// Style STAGE v1 r2 (KbdSet1.scn + PadSet1.scn + Var1/Var2 tracks)
static Bytes makeStyleV1R2(const std::vector<SmfSelSpec>& var1Track, const std::vector<SmfSelSpec>& var2Track,
                           const std::vector<SmfSelSpec>& embeddedKscU1, const std::vector<SmfSelSpec>& embeddedPad) {
    return rtSerialize({
        rtBranch("Main.ssn", {
            rtBranch("SongStyle1.scn", {
                rtBranch("SynthStyle.stc", {
                    rtBranch("Main.ssn", {
                        rtBranch("Var1.scn", {rtBranch("Acc1.pdc", {rtLeaf("Main.mid", makeSmf(var1Track, 1))})}),
                        rtBranch("Var2.scn", {rtBranch("Acc1.pdc", {rtLeaf("Main.mid", makeSmf(var2Track, 1))})}),
                        rtLeaf("SessionDescription.xml", "<SESSION/>")
                    }),
                    rtLeaf("MFX.mid", makeSmf({}, 0))
                })
            }),
            rtBranch("KbdSet1.scn", {
                rtBranch("EmbSet1.ksc", {makeKscSessionBody({}, embeddedKscU1)})
            }),
            rtBranch("PadSet1.scn", {
                rtBranch("EmbPad1.pdc", {rtLeaf("Main.mid", makeSmf(embeddedPad, 1))})
            })
        }),
        rtLeaf("StageDescription.xml", "<STAGE type=\"1\" version=\"1\" release=\"2\"/>")
    });
}

// Style STAGE v2 r0 (<name>.ksa/ks1/*.ksc)
static Bytes makeStyleV2R0(const std::vector<SmfSelSpec>& var1Track, const std::vector<SmfSelSpec>& ksaSet1) {
    return rtSerialize({
        rtBranch("Main.ssn", {
            rtBranch("SongStyle1.scn", {
                rtBranch("ModernStyle.stc", {
                    rtBranch("Main.ssn", {
                        rtBranch("Var1.scn", {rtBranch("Bass.pdc", {rtLeaf("Main.mid", makeSmf(var1Track, 1))})})
                    })
                }),
                rtBranch("ModernStyle.ksa", {
                    rtBranch("ks1", {
                        rtBranch("Set1.ksc", {makeKscSessionBody({}, ksaSet1)})
                    })
                })
            })
        }),
        rtLeaf("StageDescription.xml", "<STAGE type=\"1\" version=\"2\" release=\"0\"/>")
    });
}

static bool writeFileBytes(const fs::path& p, const Bytes& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// Strict structural JSON sanity validator (checks balanced braces/brackets, string escapes, no raw control chars,
// no unescaped quotes, no non-UTF-8 bytes >= 0x80 unless valid UTF-8, and zero local paths).
static bool isValidJsonAndClean(const std::string& json) {
    enum State { Normal, InStr, Escape };
    State st = Normal;
    std::vector<char> stack;
    for (size_t i = 0; i < json.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(json[i]);
        if (st == InStr) {
            if (c < 0x20) return false;   // unescaped control char illegal in JSON strings
            if (c == '\\') st = Escape;
            else if (c == '"') st = Normal;
        } else if (st == Escape) {
            if (c != '"' && c != '\\' && c != '/' && c != 'b' && c != 'f' && c != 'n' && c != 'r' && c != 't' && c != 'u')
                return false;
            st = InStr;
        } else {
            if (c == '"') st = InStr;
            else if (c == '{' || c == '[') stack.push_back(static_cast<char>(c));
            else if (c == '}') { if (stack.empty() || stack.back() != '{') return false; stack.pop_back(); }
            else if (c == ']') { if (stack.empty() || stack.back() != '[') return false; stack.pop_back(); }
        }
    }
    return st == Normal && stack.empty() && !json.empty() && json.front() == '{';
}

// Builds a rich synthetic KST dataset with:
//   - Samples S0..S4 in RAM.KMP:
//       S0, S1 in PCM/ram01.pcm
//       S2 in PCM/ram02.pcm
//       S3 in PCM/ram03.pcm (orphan PCM file & orphan sample/multisample)
//       S4 in RAM.KMP (for DrumSample D0, in PCM/ram02.pcm)
//   - Multisamples:
//       M0 -> {S0, S1} (used by USER01/Sound/0)
//       M1 -> {S1, S2} (used by USER01/Sound/0 and USER02/Sound/1 -> shared MS & shared S1!)
//       M2 -> {S3}     (used by orphan USER01/Sound/5 -> unreferenced by any Style/KSC/Pad)
//   - Sounds:
//       USER01/0 (Analysed: M0 + M1) -> referenced by StyleA (twice across Var1/Var2 + embedded KSC) & Pad1 (shared!)
//       USER02/1 (Analysed: M1)      -> referenced by StyleB (STAGE v2 r0) & KscSession & SettingsKsc (shared!)
//       USER01/5 (Analysed: M2)      -> ORPHAN User Sound (not referenced by any Style/KSC/Pad)
//       USERDK01/0 (Unanalysed SizeDoesNotFit Drum Kit) -> referenced by StyleA embedded Pad
struct RichSyntheticKst {
    static constexpr uint32_t kKindA = 0x10910038u;
    static constexpr RecordLayout kLayoutA{24u, 32u, 12u};

    Id8 sId[5]{makeId(0x10, 0), makeId(0x10, 1), makeId(0x10, 2), makeId(0x10, 3), makeId(0x10, 4)};
    Id8 mId[3]{makeId(0x20, 0), makeId(0x20, 1), makeId(0x20, 2)};
    Id8 dId[1]{makeId(0x30, 0)};

    NativeKstClosureInput input;
    NativeKstClosureOptions opts;

    RichSyntheticKst() {
        SynthKmpSpec kmp;
        kmp.samples = {
            {sId[0], "SynthSmp0", 44100},
            {sId[1], "SynthSmp1", 44100},
            {sId[2], "SynthSmp2", 48000},
            {sId[3], "SynthOrphanSmp3", 44100},
            {sId[4], "SynthDrumSmp4", 44100},
        };
        kmp.drums = {{dId[0], "SynthDrum0", 4, kNoIndex}};
        kmp.zones = {{0, 60}, {1, 127}, {1, 64}, {2, 127}, {3, 127}};
        kmp.multis = {
            {mId[0], "SynthMS0", 0, 2},
            {mId[1], "SynthMS1", 2, 2},
            {mId[2], "SynthOrphanMS2", 4, 1},
        };
        input.downstream.kmpContainer = RawNamedBlob{
            "MULTISMP/ram.kmp", "",
            buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", buildPa5xKmpPayload(kmp), ""}}, 1)
        };
        input.downstream.pcmContainers = {
            {"PCM/ram01.pcm", "", buildKorfContainer({
                {0x12020010u, 6, 0, 0, "SynthSmp0", Bytes(4000, 0x11), idAsStr(sId[0])},
                {0x12020010u, 6, 0, 1, "SynthSmp1", Bytes(6000, 0x22), idAsStr(sId[1])},
            }, 1)},
            {"PCM/ram02.pcm", "", buildKorfContainer({
                {0x12020010u, 6, 0, 2, "SynthSmp2", Bytes(8000, 0x33), idAsStr(sId[2])},
                {0x12020010u, 6, 0, 3, "SynthDrumSmp4", Bytes(2000, 0x44), idAsStr(sId[4])},
            }, 1)},
            {"PCM/ram03.pcm", "", buildKorfContainer({
                {0x12020010u, 6, 0, 4, "SynthOrphanSmp3", Bytes(3000, 0x55), idAsStr(sId[3])},
            }, 1)},
        };
        const Bytes sndU1_0 = buildSoundBody(kLayoutA, {{mId[0], mId[1]}});
        const Bytes sndU1_5 = buildSoundBody(kLayoutA, {{mId[2], kZeroId}});
        const Bytes sndU2_1 = buildSoundBody(kLayoutA, {{mId[1], kZeroId}});
        const Bytes sndDk_0(37, 0x66);   // does not fit kLayoutA (37 - 24 = 13 % 32 != 0) -> SizeDoesNotFit
        input.downstream.soundContainers = {
            {"SOUND/USER01.PCG", "USER01", buildKorfContainer({
                {kKindA, 4, 9, 0, "SynthLead", makeOcLiteralFrame("OC31", sndU1_0), ""},
                {kKindA, 4, 9, 5, "SynthOrphanPad", makeOcLiteralFrame("OC31", sndU1_5), ""},
            }, 1)},
            {"SOUND/USER02.PCG", "USER02", buildKorfContainer({
                {kKindA, 4, 10, 1, "SynthStrings", makeOcLiteralFrame("OC32", sndU2_1), ""},
            }, 1)},
            {"SOUND/USERDK01.PCG", "USERDK01", buildKorfContainer({
                {kKindA, 4, 11, 0, "SynthKit", makeOcLiteralFrame("OC31", sndDk_0), ""},
            }, 1)},
        };

        // Upstream roots:
        //  1) STYLE/BankA/Alpha.stg (v1 r2):
        //       Var1 Acc1 -> USER01/0 (121/64/0) + Factory (121/0/10)
        //       Var2 Acc1 -> USER01/0 (121/64/0) [duplicate ref across elements!]
        //       EmbKbdSet -> USER01/0 (121/64/0) [different Via: KeyboardSetPart!]
        //       EmbPad    -> USERDK01/0 (120/64/0)
        //  2) STYLE/BankB/Beta.stg (v2 r0 .ksa):
        //       Var1 Bass -> USER02/1 (121/65/1) + Factory Drum (120/0/0)
        //       ksa Set1  -> Unknown origin (121/127/3) + Unknown origin (0/0/7)
        //  3) KEYBOARDSET/BankK/Flat.ksc:
        //       Lower -> Factory (121/5/2), Upper1 -> USER01/0 (121/64/0), Upper2 -> Missing slot USER01/99, Upper3 -> Missing bank USERDK05/2
        //  4) KEYBOARDSET/BankK/Session.ksc:
        //       Lower -> Factory (121/1/0), Upper1 -> USER02/1 (121/65/1)
        //  5) SETTINGS/My Setting.ksc:
        //       Lower -> Factory (121/0/0), Upper1 -> USER02/1 (121/65/1)
        //  6) PAD/BankP/Hit.pdc:
        //       Main -> USER01/0 (121/64/0)
        input.upstreamRoots = {
            {"STYLE/BankA/Alpha.stg", makeStyleV1R2({{0, 121, 64, 0}, {1, 121, 0, 10}}, {{0, 121, 64, 0}}, {{0, 121, 64, 0}}, {{9, 120, 64, 0}})},
            {"STYLE/BankB/Beta.stg", makeStyleV2R0({{0, 121, 65, 1}, {9, 120, 0, 0}}, {{0, 121, 127, 3}, {1, 0, 0, 7}})},
            {"KEYBOARDSET/BankK/Flat.ksc", makeKscFlatTree({{0, 121, 5, 2}}, {{0, 121, 64, 0}}, {{0, 121, 64, 99}}, {{9, 120, 68, 2}})},
            {"KEYBOARDSET/BankK/Session.ksc", makeKscSessionTree({{0, 121, 1, 0}}, {{0, 121, 65, 1}})},
            {"SETTINGS/My Setting.ksc", makeKscSessionTree({{0, 121, 0, 0}}, {{0, 121, 65, 1}})},
            {"PAD/BankP/Hit.pdc", makePadTree({{0, 121, 64, 0}})},
        };
        opts.downstream.layouts = {{kKindA, kLayoutA}};
        opts.downstream.allowFitLayout = false;
    }
};

int main() {
    std::printf("Test: M1 Native-KST Style/KSC/Pad -> Sound -> Multisample -> Sample Closure & Reproducible Manifest\n");

    // ── 1. Resource origin classification unit tests ─────────────────────────────────────────────────────────────────
    {
        const auto uSnd = classifyMidiAddress({121, 64, 5});
        const auto uSnd12 = classifyMidiAddress({121, 75, 127});
        const auto uDk = classifyMidiAddress({120, 66, 12});
        const auto fSnd = classifyMidiAddress({121, 0, 1});
        const auto fDk = classifyMidiAddress({120, 63, 0});
        const auto unkHighLsb = classifyMidiAddress({121, 127, 3});
        const auto unkMsb0 = classifyMidiAddress({0, 0, 7});
        const auto unkMsb99 = classifyMidiAddress({99, 10, 2});
        const auto missingBank = classifyMidiAddress({-1, 64, 5});
        const auto missingLsb = classifyMidiAddress({121, -1, 5});

        TEST("origin classifier: MSB 121/120 LSB 64..75 -> User / VERIFIED with exact bank/slot target",
             uSnd.origin == ResourceOrigin::User && uSnd.epistemic == EpistemicStatus::Verified && !uSnd.isDrumKit &&
             uSnd.userBank == "USER01" && uSnd.target == ResourceId({false, "USER01", Kind::Sound, 5}) &&
             uSnd12.origin == ResourceOrigin::User && uSnd12.userBank == "USER12" &&
             uDk.origin == ResourceOrigin::User && uDk.isDrumKit && uDk.userBank == "USERDK03" &&
             uDk.target == ResourceId({false, "USERDK03", Kind::Sound, 12}));
        TEST("origin classifier: MSB 121/120 LSB 0..63 -> Factory / INFERRED (no KST target)",
             fSnd.origin == ResourceOrigin::Factory && fSnd.epistemic == EpistemicStatus::Inferred && !fSnd.isDrumKit && !fSnd.target.has_value() &&
             fDk.origin == ResourceOrigin::Factory && fDk.epistemic == EpistemicStatus::Inferred && fDk.isDrumKit && !fDk.target.has_value());
        TEST("origin classifier: LSB > 75, non-120/121 MSB, or missing CC0/CC32 -> Unknown / UNSUPPORTED (never guessed)",
             unkHighLsb.origin == ResourceOrigin::Unknown && unkHighLsb.epistemic == EpistemicStatus::Unsupported && !unkHighLsb.bankMissing &&
             unkMsb0.origin == ResourceOrigin::Unknown && unkMsb99.origin == ResourceOrigin::Unknown &&
             missingBank.origin == ResourceOrigin::Unknown && missingBank.bankMissing &&
             missingLsb.origin == ResourceOrigin::Unknown && missingLsb.bankMissing);
    }

    // ── 2. Full end-to-end Style/KSC/Pad -> Sound -> Multisample -> Sample -> PCM closure ────────────────────────────
    {
        RichSyntheticKst ds;
        const NativeKstClosureResult res = resolveNativeKstClosure(ds.input, ds.opts);
        const auto& M = res.manifest;
        const auto& S = M.summary;

        TEST("full synthetic KST closure builds unified Graph and parses all 6 upstream roots",
             res.ok && res.graph.has_value() && res.upstreamOk() && res.downstreamOk() &&
             S.styleCount == 2 && S.keyboardSetCount == 3 && S.settingsKeyboardSetCount == 1 && S.padCount == 1 &&
             S.totalUpstreamRoots == 6 && S.parsedUpstreamRoots == 6 && S.malformedUpstreamRoots == 0);

        // Exact selection accounting:
        //   Resolved user selects: Alpha(2 StyleTrack USER01/0 + 1 EmbKSC USER01/0 + 1 EmbPad USERDK01/0 = 4)
        //                          Beta(1 USER02/1) + Flat(1 USER01/0) + Session(1 USER02/1) + Settings(1 USER02/1) + Hit(1 USER01/0) = 9
        //   Missing: Flat USER01/99 (slotEmpty=1), Flat USERDK05/2 (fileAbsent=1)
        //   Factory: Alpha(121/0/10) + Beta(120/0/0) + Flat(121/5/2) + Session(121/1/0) + Settings(121/0/0) = 5
        //   Unknown origin: Beta(121/127/3, 0/0/7) = 2
        TEST("selection classification counts match exact User/Missing/Factory/Unknown breakdown",
             S.rawSelectEvents == 18 && S.userSelectsResolved == 9 &&
             S.userSelectsSlotEmpty == 1 && S.userSelectsFileAbsent == 1 &&
             S.factorySelects == 5 && S.unknownOriginSelects == 2 && S.unresolvedBankSelects == 0 &&
             M.unknownOriginAddresses.size() == 2);

        // Check Alpha.stg duplicate deduplication + multi-Via + full downstream closure
        const RootClosureEntry* alpha = nullptr;
        const RootClosureEntry* beta = nullptr;
        const RootClosureEntry* flat = nullptr;
        for (const auto& r : M.roots) {
            if (r.relPath == "STYLE/BankA/Alpha.stg") alpha = &r;
            if (r.relPath == "STYLE/BankB/Beta.stg") beta = &r;
            if (r.relPath == "KEYBOARDSET/BankK/Flat.ksc") flat = &r;
        }
        bool alphaDupOk = false, alphaEmbKscOk = false;
        if (alpha) {
            for (const auto& sr : alpha->soundRefs) {
                if (sr.via == Via::StyleTrack && sr.targetId == ResourceId({false, "USER01", Kind::Sound, 0})) {
                    alphaDupOk = (sr.occurrences == 2 && sr.totalDistinctLeafOrigins == 2 && sr.resolved);
                }
                if (sr.via == Via::KeyboardSetPart && sr.targetId == ResourceId({false, "USER01", Kind::Sound, 0})) {
                    alphaEmbKscOk = (sr.occurrences == 1 && sr.resolved);
                }
            }
        }
        const std::vector<std::string> wantAlphaPcm{"PCM/ram01.pcm", "PCM/ram02.pcm"};
        TEST("Alpha.stg deduplicates Var1+Var2 USER01/0 (x2) separately from embedded KSC (x1) and reaches M0,M1 -> S0,S1,S2 -> ram01,ram02",
             alpha != nullptr && alphaDupOk && alphaEmbKscOk &&
             alpha->userSoundIds.size() == 2 && alpha->hasUnanalysedUserSound &&
             alpha->multisampleIds.size() == 2 && alpha->sampleIds.size() == 3 &&
             alpha->pcmFiles == wantAlphaPcm &&
             alpha->storedPcmBytesProxy == (4000u + 6000u + 8000u));

        const ResourceId wantBetaSnd{false, "USER02", Kind::Sound, 1};
        const ResourceId wantBetaMs{false, "KST", Kind::Multisample, 1};
        TEST("Beta.stg (STAGE v2 r0 .ksa) flags unknownOriginRef and reaches USER02/1 -> M1 -> S1,S2 -> ram01,ram02",
             beta != nullptr && beta->hasUnknownOriginRef && !beta->hasUnanalysedUserSound &&
             beta->userSoundIds.size() == 1 && beta->userSoundIds[0] == wantBetaSnd &&
             beta->multisampleIds.size() == 1 && beta->multisampleIds[0] == wantBetaMs &&
             beta->sampleIds.size() == 2 &&
             beta->pcmFiles == wantAlphaPcm);

        TEST("Flat.ksc records both SoundSlotEmpty (USER01/99) and SoundBankAbsent (USERDK05/2)",
             flat != nullptr && flat->hasMissingDependency && flat->missingUserSoundIds.size() == 2 &&
             M.missingUserSounds.size() == 2 &&
             M.missingUserSounds[0].targetId == ResourceId({false, "USER01", Kind::Sound, 99}) &&
             M.missingUserSounds[0].reason == MissingRefReason::SoundSlotEmpty &&
             M.missingUserSounds[1].targetId == ResourceId({false, "USERDK05", Kind::Sound, 2}) &&
             M.missingUserSounds[1].reason == MissingRefReason::SoundBankAbsent);

        // Sharing across multiple roots:
        //   USER01/0 referenced by Alpha.stg, Flat.ksc, Hit.pdc (3 roots)
        //   USER02/1 referenced by Beta.stg, Session.ksc, My Setting.ksc (3 roots)
        //   M0 referenced by 3 roots (via USER01/0); M1 referenced by all 6 roots (via USER01/0 + USER02/1)!
        //   S0 (3 roots, 1 Sound), S1 (6 roots, 2 Sounds), S2 (6 roots, 2 Sounds) -> 3 shared across roots, 2 shared across Sounds!
        //   PCM/ram01.pcm (6 roots), PCM/ram02.pcm (6 roots) -> 2 shared PCM files!
        TEST("shared resource counts accurately track cross-root sharing (Sounds, Multisamples, Samples, PCM files)",
             S.sharedUserSoundsAcrossRoots == 2 && M.sharedUserSounds[0].referencingRootCount == 3 &&
             M.sharedUserSounds[1].referencingRootCount == 3 &&
             S.sharedMultisamplesAcrossRoots == 2 && S.sharedMultisamplesAcrossSounds == 1 &&
             S.sharedSamplesAcrossRoots == 3 && S.sharedSamplesAcrossSounds == 2 &&
             S.sharedPcmFilesAcrossRoots == 2 && M.sharedPcmFiles[0].referencingRootCount == 6);

        // Orphans (NOT provably deletable!):
        //   USER01/5 (orphan User Sound), M2 (orphan Multisample from roots), S3 & S4 (orphan Samples from roots),
        //   D0 (orphan DrumSample), PCM/ram03.pcm (orphan PCM file from roots)
        const ResourceId wantOrphanSnd{false, "USER01", Kind::Sound, 5};
        const ResourceId wantOrphanMs{false, "KST", Kind::Multisample, 2};
        const std::vector<std::string> wantOrphanPcm{"PCM/ram03.pcm"};
        TEST("orphan tracking identifies unreferenced User Sound, RAM Multisample, Samples, DrumSample, and PCM file",
             M.orphanUserSounds.size() == 1 && M.orphanUserSounds[0] == wantOrphanSnd &&
             M.orphanMultisamples.size() == 1 && M.orphanMultisamples[0] == wantOrphanMs &&
             M.orphanSamples.size() == 2 && S.orphanDrumSamples == 1 &&
             M.orphanPcmFiles == wantOrphanPcm);

        // Delete safety HOLD GATE: ZERO Unreferenced across ALL nodes in unified Graph!
        bool anyUnref = false;
        for (const auto& [id, node] : res.graph->nodes()) {
            (void)node;
            anyUnref |= (res.graph->deleteCheck(id).verdict == DeleteVerdict::Unreferenced);
        }
        TEST("deleteCheck() NEVER returns Unreferenced across any node in unified Graph (including orphans)",
             !anyUnref && !M.coverage.deleteProofAvailable && !M.coverage.coverageComplete);

        // Canonical JSON manifest serialization + syntax + privacy sanity
        const std::string json = serializeManifestJson(M);
        TEST("serialized manifest JSON is structurally valid, has schema v1, epistemic tags, and zero local paths",
             isValidJsonAndClean(json) &&
             json.find("\"schema\": \"aiarranger-native-kst-closure-manifest/1\"") != std::string::npos &&
             json.find("\"deleteProofAvailable\": false") != std::string::npos &&
             json.find("\"notProvablyDeletable\": true") != std::string::npos &&
             json.find("\"VERIFIED\"") != std::string::npos &&
             json.find("\"INFERRED\"") != std::string::npos &&
             json.find("\"UNSUPPORTED\"") != std::string::npos &&
             json.find("/Users/") == std::string::npos);

        // Include-sample-lists JSON mode also valid
        ManifestJsonOptions fullOpts;
        fullOpts.includePerRootSampleIds = true;
        fullOpts.includeSampleLists = true;
        const std::string fullJson = serializeManifestJson(M, fullOpts);
        TEST("full manifest JSON with sample ID lists is structurally valid and includes sampleIds",
             isValidJsonAndClean(fullJson) && fullJson.find("\"sampleIds\": [") != std::string::npos);
    }

    // ── 3. Determinism: repeated runs & shuffled input orderings produce byte-identical manifests ────────────────────
    {
        RichSyntheticKst ds1, ds2;
        std::reverse(ds2.input.upstreamRoots.begin(), ds2.input.upstreamRoots.end());
        std::reverse(ds2.input.downstream.soundContainers.begin(), ds2.input.downstream.soundContainers.end());
        std::reverse(ds2.input.downstream.pcmContainers.begin(), ds2.input.downstream.pcmContainers.end());

        const auto r1a = resolveNativeKstClosure(ds1.input, ds1.opts);
        const auto r1b = resolveNativeKstClosure(ds1.input, ds1.opts);
        const auto r2  = resolveNativeKstClosure(ds2.input, ds2.opts);

        ManifestJsonOptions fullOpts{true, true};
        const std::string j1a = serializeManifestJson(r1a.manifest, fullOpts);
        const std::string j1b = serializeManifestJson(r1b.manifest, fullOpts);
        const std::string j2  = serializeManifestJson(r2.manifest, fullOpts);

        TEST("in-memory manifest determinism: repeated runs and reversed/shuffled inputs are byte-identical",
             j1a == j1b && j1a == j2 && !j1a.empty());
    }

    // ── 4. Anomalies: malformed SMF (CC11>=0x80, uppercase .MID), unresolved bank, malformed tree, OC31 KMP, orphan PCM
    {
        RichSyntheticKst ds;
        // Add malformed Pad (CC11 high-bit data byte >= 0x80)
        ds.input.upstreamRoots.push_back({"PAD/BankP/BadCc11.pdc", makePadTree({{0, 121, 64, 0}}, true)});
        // Add uppercase .MID leaf (refused by strict closure)
        ds.input.upstreamRoots.push_back({"PAD/BankP/UpperMid.pdc", makePadTree({{0, 121, 64, 0}}, false, "Main.MID")});
        // Add unresolved bank PC (missing CC0/CC32 before PC)
        ds.input.upstreamRoots.push_back({"PAD/BankP/NoBank.pdc", makePadTree({{0, -1, -1, 12}})});
        // Add malformed Root tree
        ds.input.upstreamRoots.push_back({"STYLE/BankA/Broken.stg", {'R','o','o','t',0,1,0,0,0,0,5,'x','x'}});
        // Add orphan PCM entry absent from RAM.KMP
        const Id8 orphanPcmId = makeId(0x99, 1);
        ds.input.downstream.pcmContainers.push_back({
            "PCM/ram04_orphan.pcm", "",
            buildKorfContainer({{0x12020010u, 6, 0, 9, "UnmatchedPcm", Bytes(512, 0x77), idAsStr(orphanPcmId)}}, 1)
        });
        // Add Latin-1 non-UTF-8 byte in a Sound name to verify JSON UTF-8 safety
        const Bytes extraSnd = buildSoundBody(RichSyntheticKst::kLayoutA, {{ds.mId[0], kZeroId}});
        ds.input.downstream.soundContainers.push_back({
            "SOUND/USER03.PCG", "USER03",
            buildKorfContainer({{RichSyntheticKst::kKindA, 4, 12, 0, std::string("Caf\xE9 Sound"), makeOcLiteralFrame("OC31", extraSnd), ""}}, 1)
        });

        const auto res = resolveNativeKstClosure(ds.input, ds.opts);
        const auto& M = res.manifest;
        const std::string json = serializeManifestJson(M);
        TEST("anomalies detected cleanly: malformed tree, CC11>=0x80 & .MID leaves, missing bank PC, orphan PCM, Latin-1 JSON safe",
             res.ok && !res.upstreamOk() && !res.coverageComplete() &&
             M.malformedTrees.size() == 1 && M.malformedTrees[0].relPath == "STYLE/BankA/Broken.stg" &&
             M.malformedMidiLeaves.size() == 2 &&
             M.unresolvedBankSelects.size() == 1 && M.unresolvedBankSelects[0].program == 12 &&
             M.orphanPcmEntries.size() == 1 && M.orphanPcmEntries[0].id == orphanPcmId &&
             isValidJsonAndClean(json) && json.find("Caf\\u00e9 Sound") != std::string::npos);
    }
    {
        // OC31-encoded Pa700/Pa4X RAM.KMP: downstream fails closed, while upstream Style/KSC/Pad -> Sound STILL resolves!
        RichSyntheticKst ds;
        ds.input.downstream.kmpContainer = RawNamedBlob{
            "MULTISMP/ram.kmp", "",
            buildKorfContainer({{0x11050038u, 5, 0, 0, "RAM.KMP", makeOcLiteralFrame("OC31", Bytes(64, 0x42)), ""}}, 1)
        };
        const auto res = resolveNativeKstClosure(ds.input, ds.opts);
        bool anyUnref = false;
        if (res.graph) {
            for (const auto& [id, n] : res.graph->nodes()) {
                (void)n;
                anyUnref |= (res.graph->deleteCheck(id).verdict == DeleteVerdict::Unreferenced);
            }
        }
        TEST("OC31 legacy RAM.KMP fails closed downstream while upstream Style/KSC/Pad -> Sound still resolves accurately",
             res.ok && res.graph.has_value() && !res.downstreamOk() && !res.coverageComplete() &&
             res.manifest.coverage.downstreamKmpPresent && !res.manifest.coverage.downstreamKmpParsed &&
             res.manifest.coverage.kmpError.find("OC31") != std::string::npos &&
             res.manifest.summary.userSelectsResolved == 9 &&
             res.manifest.summary.userSelectsSlotEmpty == 1 &&
             res.manifest.summary.userSelectsFileAbsent == 1 &&
             res.manifest.summary.soundSlotsOccupied == 4 &&
             res.manifest.summary.multisamplesTotal == 0 && !anyUnref);
    }

    // ── 5. On-disk folder scanning, determinism across write orders, SongBook flag, metadir.json, security & caps ────
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tmpRoot = fs::temp_directory_path() /
            ("aiarr_m1_closure_test_" + std::to_string(::getpid()) + "_" + std::to_string(stamp));
        std::error_code ec;
        fs::remove_all(tmpRoot, ec);
        fs::create_directories(tmpRoot, ec);

        RichSyntheticKst ds;
        auto writeCorpus = [&](const fs::path& dir, bool reverseOrder) {
            writeFileBytes(dir / "MULTISMP" / "ram.kmp", ds.input.downstream.kmpContainer->bytes);
            auto pcms = ds.input.downstream.pcmContainers;
            auto snds = ds.input.downstream.soundContainers;
            auto roots = ds.input.upstreamRoots;
            if (reverseOrder) {
                std::reverse(pcms.begin(), pcms.end());
                std::reverse(snds.begin(), snds.end());
                std::reverse(roots.begin(), roots.end());
            }
            for (const auto& b : pcms) writeFileBytes(dir / b.label, b.bytes);
            for (const auto& b : snds) writeFileBytes(dir / b.label, b.bytes);
            for (const auto& b : roots) writeFileBytes(dir / b.relPath, b.bytes);
            // metadir.json in STYLE and KEYBOARDSET bank folders (expected Pa5X metadata, ignored cleanly)
            const std::string meta = "{\"files\":[]}";
            writeFileBytes(dir / "STYLE" / "BankA" / "metadir.json", Bytes(meta.begin(), meta.end()));
            writeFileBytes(dir / "KEYBOARDSET" / "BankK" / "metadir.json", Bytes(meta.begin(), meta.end()));
        };

        const fs::path kstFwd = tmpRoot / "FWD.KST";
        const fs::path kstRev = tmpRoot / "REV.KST";
        writeCorpus(kstFwd, false);
        writeCorpus(kstRev, true);

        const auto diskFwd1 = resolveNativeKstClosureFolder(kstFwd, ds.opts);
        const auto diskFwd2 = resolveNativeKstClosureFolder(kstFwd, ds.opts);
        const auto diskRev  = resolveNativeKstClosureFolder(kstRev, ds.opts);
        const auto diskSlash = resolveNativeKstClosureFolder(fs::path(kstFwd.string() + "///"), ds.opts);

        ManifestJsonOptions fullOpts{true, true};
        const std::string jf1 = serializeManifestJson(diskFwd1.manifest, fullOpts);
        const std::string jf2 = serializeManifestJson(diskFwd2.manifest, fullOpts);
        const std::string jrv = serializeManifestJson(diskRev.manifest, fullOpts);
        const std::string jsl = serializeManifestJson(diskSlash.manifest, fullOpts);

        TEST("on-disk folder closure succeeds, counts metadir.json, and is byte-identical across runs, write orders, and trailing slashes",
             diskFwd1.ok && diskFwd1.upstreamOk() && diskFwd1.downstreamOk() &&
             diskFwd1.manifest.coverage.metadirJsonFiles == 2 &&
             diskFwd1.manifest.coverage.bytesReadUpstream > 0 &&
             diskFwd1.manifest.coverage.bytesReadDownstream > 0 &&
             jf1 == jf2 && jf1 == jrv && jf1 == jsl && isValidJsonAndClean(jf1));

        // Clean minimal KST with 0 gaps -> coverageComplete() == true (while deleteProofAvailable is STILL false!)
        {
            const fs::path cleanKst = tmpRoot / "CLEAN.KST";
            SynthKmpSpec kmp;
            kmp.samples = {{ds.sId[0], "CleanS0", 44100}};
            kmp.zones = {{0, 127}};
            kmp.multis = {{ds.mId[0], "CleanM0", 0, 1}};
            writeFileBytes(cleanKst / "MULTISMP" / "ram.kmp",
                           buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", buildPa5xKmpPayload(kmp), ""}}, 1));
            writeFileBytes(cleanKst / "PCM" / "ram01.pcm",
                           buildKorfContainer({{0x12020010u, 6, 0, 0, "CleanS0", Bytes(2048, 0x11), idAsStr(ds.sId[0])}}, 1));
            const Bytes cleanBody = buildSoundBody(RichSyntheticKst::kLayoutA, {{ds.mId[0], kZeroId}});
            writeFileBytes(cleanKst / "SOUND" / "USER01.PCG",
                           buildKorfContainer({{RichSyntheticKst::kKindA, 4, 9, 0, "CleanSnd", makeOcLiteralFrame("OC31", cleanBody), ""}}, 1));
            writeFileBytes(cleanKst / "PAD" / "B1" / "CleanPad.pdc", makePadTree({{0, 121, 64, 0}}));

            const auto cleanRes = resolveNativeKstClosureFolder(cleanKst, ds.opts);
            bool anyUnref = false;
            if (cleanRes.graph) {
                for (const auto& [id, n] : cleanRes.graph->nodes()) {
                    (void)n;
                    anyUnref |= (cleanRes.graph->deleteCheck(id).verdict == DeleteVerdict::Unreferenced);
                }
            }
            TEST("fully clean synthetic KST achieves coverageComplete() == true while deleteCheck() STILL never returns Unreferenced",
                 cleanRes.ok && cleanRes.coverageComplete() && !cleanRes.manifest.coverage.deleteProofAvailable && !anyUnref);

            // Adding SONGBOOK/ folder marks songBookPresent == true and coverageComplete() == false!
            fs::create_directories(cleanKst / "SONGBOOK", ec);
            const auto sbRes = resolveNativeKstClosureFolder(cleanKst, ds.opts);
            TEST("presence of SONGBOOK/ marks songBookPresent=true and forces coverageComplete()=false",
                 sbRes.ok && sbRes.manifest.coverage.songBookPresent && !sbRes.manifest.coverage.songBookScanned &&
                 !sbRes.coverageComplete());
        }

        // Security: symlinked root (with and without trailing slashes), symlinked STYLE dir, symlinked .stg file, nested subdir
        {
            const fs::path symRoot = tmpRoot / "SYM_ROOT.KST";
            std::error_code sec;
            fs::create_directory_symlink(kstFwd, symRoot, sec);
            if (!sec) {
                const auto r1 = resolveNativeKstClosureFolder(symRoot, ds.opts);
                const auto r2 = resolveNativeKstClosureFolder(fs::path(symRoot.string() + "/"), ds.opts);
                const auto r3 = resolveNativeKstClosureFolder(fs::path(symRoot.string() + "///"), ds.opts);
                TEST("symlinked KST root refused with and without trailing slashes (O_NOFOLLOW)",
                     !r1.ok && !r2.ok && !r3.ok && !r1.manifest.containerErrors.empty());
            }

            const fs::path hostileKst = tmpRoot / "HOSTILE.KST";
            writeCorpus(hostileKst, false);
            fs::create_symlink(kstFwd / "PAD" / "BankP" / "Hit.pdc", hostileKst / "PAD" / "BankP" / "LinkPad.pdc", sec);
            fs::create_directories(hostileKst / "STYLE" / "BankA" / "UnexpectedNested", sec);
            fs::create_directories(hostileKst / "SETTINGS" / "NestedSettingsDir", sec);
            const auto hRes = resolveNativeKstClosureFolder(hostileKst, ds.opts);
            TEST("symlinked .pdc refused and unexpected nested subdirs recorded in skippedPaths",
                 hRes.ok && !hRes.upstreamOk() && !hRes.coverageComplete() &&
                 !hRes.manifest.containerErrors.empty() && hRes.manifest.skippedPaths.size() >= 3);

            // Symlinked top-level STYLE directory
            const fs::path symStyleKst = tmpRoot / "SYM_STYLE.KST";
            writeFileBytes(symStyleKst / "MULTISMP" / "ram.kmp", ds.input.downstream.kmpContainer->bytes);
            writeFileBytes(symStyleKst / "PCM" / "ram01.pcm", ds.input.downstream.pcmContainers[0].bytes);
            writeFileBytes(symStyleKst / "SOUND" / "USER01.PCG", ds.input.downstream.soundContainers[0].bytes);
            fs::create_directory_symlink(kstFwd / "STYLE", symStyleKst / "STYLE", sec);
            if (!sec) {
                const auto ssRes = resolveNativeKstClosureFolder(symStyleKst, ds.opts);
                TEST("symlinked STYLE/ top-level folder refused fail-closed",
                     ssRes.ok && !ssRes.upstreamOk() && !ssRes.manifest.containerErrors.empty());
            }
        }

        // Caps: upstream byte budget cap & directory entry cap
        {
            NativeKstClosureOptions tinyBudget = ds.opts;
            tinyBudget.maxUpstreamTotalBytes = 128;   // smaller than 6 root trees
            const auto bRes = resolveNativeKstClosureFolder(kstFwd, tinyBudget);
            TEST("upstream total byte budget cap stops reading and reports error",
                 bRes.ok && !bRes.upstreamOk() && !bRes.coverageComplete() &&
                 bRes.manifest.coverage.bytesReadUpstream <= 128 &&
                 !bRes.manifest.containerErrors.empty());

            NativeKstClosureOptions tinyEntries = ds.opts;
            tinyEntries.maxDirEntries = 2;
            const auto eRes = resolveNativeKstClosureFolder(kstFwd, tinyEntries);
            TEST("directory entry cap fails closed cleanly",
                 !eRes.ok && !eRes.manifest.containerErrors.empty());

            NativeKstClosureOptions tinyFileCap = ds.opts;
            tinyFileCap.maxUpstreamFiles = 2;
            const auto fcRes = resolveNativeKstClosureFolder(kstFwd, tinyFileCap);
            TEST("upstream file-count cap stops scanning and records error",
                 fcRes.ok && !fcRes.upstreamOk() && !fcRes.coverageComplete() &&
                 fcRes.manifest.summary.totalUpstreamRoots == 2 &&
                 !fcRes.manifest.containerErrors.empty());
        }

        // Additional anomaly classes: unknown Sound kind (NoLayoutForKind), unresolved slots in analysed vs unanalysed Sounds, FIFO refusal
        {
            RichSyntheticKst dsExtra;
            const Id8 unkA = makeId(0xEE, 0x01);
            const Id8 unkB = makeId(0xEE, 0x02);
            const Bytes analysedUnk = buildSoundBody(RichSyntheticKst::kLayoutA, {{dsExtra.mId[0], unkA}});
            const Bytes noRefUnk = buildSoundBody(RichSyntheticKst::kLayoutA, {{unkA, unkB}});
            const Bytes unknownKindBody(40, 0x33);
            dsExtra.input.downstream.soundContainers.push_back({
                "SOUND/USER04.PCG", "USER04",
                buildKorfContainer({
                    {RichSyntheticKst::kKindA, 4, 13, 0, "AnalysedWithUnk", makeOcLiteralFrame("OC31", analysedUnk), ""},
                    {RichSyntheticKst::kKindA, 4, 13, 1, "NoRefWithUnk",    makeOcLiteralFrame("OC31", noRefUnk), ""},
                    {0x10040038u,              4, 13, 2, "UnknownKindSnd",  makeOcLiteralFrame("OC31", unknownKindBody), ""},
                }, 1)
            });
            const auto exRes = resolveNativeKstClosure(dsExtra.input, dsExtra.opts);
            const std::string exJson = serializeManifestJson(exRes.manifest);
            TEST("unknown Sound kind and unresolved slot provenance (analysed vs unanalysed) surface cleanly in manifest JSON",
                 exRes.ok && !exRes.coverageComplete() &&
                 exRes.manifest.summary.unresolvedSoundSlotsInAnalysed == 1 &&
                 exRes.manifest.summary.unresolvedSoundSlotsInUnanalysed == 2 &&
                 isValidJsonAndClean(exJson) &&
                 exJson.find("\"unresolvedSoundSlotsInAnalysed\": [\n      {\"sound\": \"pa5x:USER04/Sound/0\"") != std::string::npos &&
                 exJson.find("\"unresolvedSoundSlotsInUnanalysedCount\": 2") != std::string::npos &&
                 exJson.find("\"gap\": \"NoLayoutForKind\"") != std::string::npos);

            const fs::path fifoKst = tmpRoot / "FIFO.KST";
            writeCorpus(fifoKst, false);
            const fs::path fifoPath = fifoKst / "PAD" / "BankP" / "Pipe.pdc";
            if (::mkfifo(fifoPath.c_str(), 0600) == 0) {
                const auto fRes = resolveNativeKstClosureFolder(fifoKst, ds.opts);
                TEST("hostile FIFO inside PAD bank is refused non-blocking and recorded in containerErrors",
                     fRes.ok && !fRes.upstreamOk() && !fRes.manifest.containerErrors.empty());
            }
        }

        // ── 6. Audit hardening regressions (P2-1 no local paths in failure JSON, P2-2 GLOBAL & unexpected files, P3-1 early-return errors) ──
        {
            // P2-1: failed root open (missing + symlink) and entry-cap failure manifests never leak caller local paths
            const fs::path missingDir = tmpRoot / "DOES_NOT_EXIST_ROOT.KST";
            const auto missRes = resolveNativeKstClosureFolder(missingDir, ds.opts);
            const std::string missJson = serializeManifestJson(missRes.manifest);

            NativeKstClosureOptions tinyCap = ds.opts;
            tinyCap.maxDirEntries = 2;
            const auto capRes = resolveNativeKstClosureFolder(kstFwd, tinyCap);
            const std::string capJson = serializeManifestJson(capRes.manifest);

            const fs::path symFailRoot = tmpRoot / "SYM_FAIL_P21.KST";
            std::error_code sec;
            fs::create_directory_symlink(kstFwd, symFailRoot, sec);
            std::string symJson;
            if (!sec) {
                const auto symRes = resolveNativeKstClosureFolder(symFailRoot, ds.opts);
                symJson = serializeManifestJson(symRes.manifest);
            }
            const std::string tmpStr = tmpRoot.string();
            TEST("P2-1: failure manifests (missing/symlink/entry-cap) never leak local filesystem paths in serialized JSON",
                 !missRes.ok && isValidJsonAndClean(missJson) &&
                 missJson.find(tmpStr) == std::string::npos &&
                 missJson.find("<kst-root>") != std::string::npos &&
                 !capRes.ok && isValidJsonAndClean(capJson) &&
                 capJson.find(tmpStr) == std::string::npos &&
                 capJson.find("<kst-root>") != std::string::npos &&
                 (sec || (isValidJsonAndClean(symJson) &&
                          symJson.find(tmpStr) == std::string::npos &&
                          symJson.find("<kst-root>") != std::string::npos)));

            // P2-2: GLOBAL/ directory, SETTINGS/SETUP.GBL, and unexpected regular files in STYLE/KEYBOARDSET/PAD/SETTINGS
            // are recorded in skippedPaths & diagnostics and set coverageComplete() == false (while metadir.json stays valid)
            auto hasStr = [](const std::vector<std::string>& v, const std::string& needle) {
                return std::find(v.begin(), v.end(), needle) != v.end();
            };
            auto hasDiagSubstr = [](const std::vector<std::string>& v, const std::string& sub) {
                for (const auto& s : v) if (s.find(sub) != std::string::npos) return true;
                return false;
            };

            const fs::path p22Base = tmpRoot / "P22_CLEAN.KST";
            SynthKmpSpec kmpP22;
            kmpP22.samples = {{ds.sId[0], "P22S0", 44100}};
            kmpP22.zones = {{0, 127}};
            kmpP22.multis = {{ds.mId[0], "P22M0", 0, 1}};
            const Bytes p22Kmp = buildKorfContainer({{kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", buildPa5xKmpPayload(kmpP22), ""}}, 1);
            const Bytes p22Pcm = buildKorfContainer({{0x12020010u, 6, 0, 0, "P22S0", Bytes(2048, 0x11), idAsStr(ds.sId[0])}}, 1);
            const Bytes p22Body = buildSoundBody(RichSyntheticKst::kLayoutA, {{ds.mId[0], kZeroId}});
            const Bytes p22Snd = buildKorfContainer({{RichSyntheticKst::kKindA, 4, 9, 0, "P22Snd", makeOcLiteralFrame("OC31", p22Body), ""}}, 1);
            const Bytes p22Pad = makePadTree({{0, 121, 64, 0}});
            const Bytes metaBytes{'{', '"', 'f', 'i', 'l', 'e', 's', '"', ':', '[', ']', '}'};

            auto initP22 = [&](const fs::path& dir) {
                writeFileBytes(dir / "MULTISMP" / "ram.kmp", p22Kmp);
                writeFileBytes(dir / "PCM" / "ram01.pcm", p22Pcm);
                writeFileBytes(dir / "SOUND" / "USER01.PCG", p22Snd);
                writeFileBytes(dir / "PAD" / "B1" / "Valid.pdc", p22Pad);
                writeFileBytes(dir / "PAD" / "B1" / "metadir.json", metaBytes);
            };
            initP22(p22Base);
            const auto baseRes = resolveNativeKstClosureFolder(p22Base, ds.opts);
            TEST("P2-2 baseline: clean KST with metadir.json has coverageComplete() == true",
                 baseRes.ok && baseRes.coverageComplete() && baseRes.manifest.skippedPaths.empty() &&
                 baseRes.manifest.coverage.metadirJsonFiles == 1);

            const fs::path p22Global = tmpRoot / "P22_GLOBAL.KST";
            initP22(p22Global);
            fs::create_directories(p22Global / "GLOBAL", ec);
            const auto globRes = resolveNativeKstClosureFolder(p22Global, ds.opts);
            TEST("P2-2: top-level GLOBAL/ directory is recorded in skippedPaths/diagnostics and sets coverageComplete()=false",
                 globRes.ok && !globRes.coverageComplete() &&
                 hasStr(globRes.manifest.skippedPaths, "GLOBAL") &&
                 hasDiagSubstr(globRes.manifest.diagnostics, "GLOBAL/"));

            const fs::path p22Files = tmpRoot / "P22_UNEXPECTED_FILES.KST";
            initP22(p22Files);
            const Bytes dummy{'x', 'y', 'z'};
            writeFileBytes(p22Files / "SETTINGS" / "SETUP.GBL", dummy);
            writeFileBytes(p22Files / "SETTINGS" / "extra.dat", dummy);
            writeFileBytes(p22Files / "STYLE" / "Bank1" / "stray.txt", dummy);
            writeFileBytes(p22Files / "KEYBOARDSET" / "top_stray.bin", dummy);
            writeFileBytes(p22Files / "PAD" / "B1" / "notes.md", dummy);
            const auto filesRes = resolveNativeKstClosureFolder(p22Files, ds.opts);
            TEST("P2-2: SETTINGS/SETUP.GBL and unexpected regular files in STYLE/KEYBOARDSET/PAD/SETTINGS recorded and fail coverageComplete()",
                 filesRes.ok && !filesRes.coverageComplete() &&
                 filesRes.manifest.summary.parsedUpstreamRoots == 1 &&
                 filesRes.manifest.coverage.metadirJsonFiles == 1 &&
                 hasStr(filesRes.manifest.skippedPaths, "SETTINGS/SETUP.GBL") &&
                 hasStr(filesRes.manifest.skippedPaths, "SETTINGS/extra.dat") &&
                 hasStr(filesRes.manifest.skippedPaths, "STYLE/Bank1/stray.txt") &&
                 hasStr(filesRes.manifest.skippedPaths, "KEYBOARDSET/top_stray.bin") &&
                 hasStr(filesRes.manifest.skippedPaths, "PAD/B1/notes.md") &&
                 hasDiagSubstr(filesRes.manifest.diagnostics, "unsupported unparsed settings file: SETTINGS/SETUP.GBL") &&
                 hasDiagSubstr(filesRes.manifest.diagnostics, "skipped unexpected regular file: STYLE/Bank1/stray.txt"));

            // P3-1: early-return error paths in finalizeClosure() copy extraErrors + graph error into containerErrors/diagnostics
            {
                NativeKstResult dupNodeDown;
                dupNodeDown.ok = true;
                dupNodeDown.graph = Graph{};
                std::string ge;
                const bool preAdded = dupNodeDown.graph->addNode({{false, "KST", Kind::Style, 0}, "existing", std::nullopt}, &ge);
                closure_detail::ExtractedRoot erNode;
                erNode.relPath = "STYLE/B/Dup.stg";
                erNode.kind = Kind::Style;
                erNode.parseOk = true;
                const auto nodeFailRes = closure_detail::finalizeClosure(
                    std::move(dupNodeDown), {erNode}, false, 0, 0, {}, {"prior node error"}, {}, ds.opts);

                NativeKstResult badRefDown;
                badRefDown.ok = true;
                badRefDown.graph = Graph{};
                closure_detail::ExtractedRoot erRef;
                erRef.relPath = "PAD/B/BadVia.pdc";
                erRef.kind = Kind::Pad;
                erRef.parseOk = true;
                // Via::StyleTrack is disallowed from Kind::Pad -> forces g.addRef() failure!
                erRef.refs[{Via::StyleTrack, MidiSoundAddress{121, 64, 0}}] = {1, {"Main.mid"}};
                const auto refFailRes = closure_detail::finalizeClosure(
                    std::move(badRefDown), {erRef}, false, 0, 0, {}, {"prior ref error"}, {}, ds.opts);

                TEST("P3-1: early-return graph addNode/addRef failure paths surface both prior extraErrors and graph error in containerErrors",
                     preAdded &&
                     !nodeFailRes.ok && !nodeFailRes.coverageComplete() &&
                     hasStr(nodeFailRes.manifest.containerErrors, "prior node error") &&
                     hasDiagSubstr(nodeFailRes.manifest.containerErrors, "graph addNode failed") &&
                     !refFailRes.ok && !refFailRes.coverageComplete() &&
                     hasStr(refFailRes.manifest.containerErrors, "prior ref error") &&
                     hasDiagSubstr(refFailRes.manifest.containerErrors, "graph addRef failed"));
            }
        }

        fs::remove_all(tmpRoot, ec);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
