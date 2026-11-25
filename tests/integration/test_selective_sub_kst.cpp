// Slice M2e-2 Synthetic Integration, Round-Trip, Negative, TOCTOU, D18 & Mid-Failure Recovery Tests:
//   - `src/jobs/selective_sub_kst.h` (Experimental Root-Tree Sub-KST selective exporter)
//   - `tools/aiarranger_resources.cpp` (`kst-native selective-export` CLI end-to-end)
//
// 100% synthetic, original fixtures only (zero owner files, proprietary bytes, real names, or local paths).
// Strictly enforces all active HOLD gates, MAN5 p.736 RAM-wipe prevention, D17, and D18.

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "jobs/import_kit.h"
#include "jobs/selective_sub_kst.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace ai_arranger::media::graph;
using namespace ai_arranger::media::panative;
using namespace ai_arranger::media::selective;
namespace subkst = ai_arranger::jobs::subkst;
namespace jobs = ai_arranger::jobs;
namespace kit = ai_arranger::jobs::kit;
namespace lib = ai_arranger::library;
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

// ── Synthetic Binary Builders (100% Original) ────────────────────────────────────────────────────────────────────────

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
    return Id8{tag, idx, 0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB};
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
    size_t pos = 0;
    while (pos < plain.size()) {
        const size_t n = std::min<size_t>(250, plain.size() - pos);
        if (n <= 3) {
            ops.push_back(0x01); ops.push_back(0x00); ops.push_back(static_cast<uint8_t>(n));
        } else if (n <= 17) {
            ops.push_back(static_cast<uint8_t>(n - 2));
        } else {
            ops.push_back(0x00); ops.push_back(static_cast<uint8_t>(n - 18));
        }
        ops.insert(ops.end(), plain.begin() + static_cast<std::ptrdiff_t>(pos),
                   plain.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
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

static Bytes buildKorfContainer(const std::vector<SynthKorfItem>& items) {
    Bytes dir;
    for (const auto& it : items) {
        const uint16_t sub = static_cast<uint16_t>((it.chunkId >> 8) & 0xFFFFu);
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
    Bytes body;
    pushBe32(body, 0x02000018u); pushBe32(body, 12u);
    const uint8_t hdr[12] = {0x0B, 0, 0, 0, 0, 0, 0, 'K', 'O', 'R', 'F', 0};
    body.insert(body.end(), hdr, hdr + 12);
    pushBe32(body, 0x05010018u);
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

struct MidiSelSpec { int ch; int msb; int lsb; int pc; };

static std::string buildSmf(const std::vector<MidiSelSpec>& sels,
                            bool includeKorgSysex = false,
                            bool sharedBankRunningStatus = false,
                            bool malformedHighBit = false) {
    Bytes trk;
    if (includeKorgSysex) {
        // Synthetic Korg SysEx event F0 42 30 ... F7 (must be preserved byte-identically!)
        trk.push_back(0x00); trk.push_back(0xF0); trk.push_back(0x05);
        trk.push_back(0x42); trk.push_back(0x30); trk.push_back(0x7F); trk.push_back(0x01); trk.push_back(0xF7);
    }
    if (sharedBankRunningStatus && sels.size() >= 2) {
        // Emit CC0 + CC32 once on ch, then PC_0 (explicit), then PC_1 via running status, plus a note!
        const uint8_t ch0 = static_cast<uint8_t>((sels[0].ch - 1) & 0x0F);
        trk.push_back(0x00); trk.push_back(static_cast<uint8_t>(0xB0u | ch0)); trk.push_back(0x00); trk.push_back(static_cast<uint8_t>(sels[0].msb));
        trk.push_back(0x00); trk.push_back(0x20); trk.push_back(static_cast<uint8_t>(sels[0].lsb)); // running status CC32
        trk.push_back(0x00); trk.push_back(static_cast<uint8_t>(0xC0u | ch0)); trk.push_back(static_cast<uint8_t>(sels[0].pc));
        trk.push_back(0x10); trk.push_back(static_cast<uint8_t>(sels[1].pc)); // running status PC_1!
        trk.push_back(0x04); trk.push_back(static_cast<uint8_t>(0x90u | ch0)); trk.push_back(0x3C); trk.push_back(0x64);
        trk.push_back(0x20); trk.push_back(0x3C); trk.push_back(0x00);
    } else {
        for (const auto& s : sels) {
            const uint8_t ch0 = static_cast<uint8_t>((s.ch - 1) & 0x0F);
            if (s.msb >= 0) {
                trk.push_back(0x00); trk.push_back(static_cast<uint8_t>(0xB0u | ch0));
                trk.push_back(0x00); trk.push_back(static_cast<uint8_t>(s.msb));
            }
            if (s.lsb >= 0) {
                trk.push_back(0x00); trk.push_back(static_cast<uint8_t>(0xB0u | ch0));
                trk.push_back(0x20); trk.push_back(static_cast<uint8_t>(s.lsb));
            }
            trk.push_back(0x00); trk.push_back(static_cast<uint8_t>(0xC0u | ch0));
            trk.push_back(static_cast<uint8_t>(s.pc));
        }
    }
    if (malformedHighBit) {
        // CC11 with high bit set (>= 0x80) matching real-world malformed Pad leaves
        trk.push_back(0x00); trk.push_back(0xB0); trk.push_back(0x0B); trk.push_back(0x88);
    }
    trk.push_back(0x00); trk.push_back(0xFF); trk.push_back(0x2F); trk.push_back(0x00);

    Bytes smf;
    appendStr(smf, "MThd"); pushBe32(smf, 6u);
    pushBe16(smf, 0u); pushBe16(smf, 1u); pushBe16(smf, 480u);
    appendStr(smf, "MTrk"); pushBe32(smf, static_cast<uint32_t>(trk.size()));
    smf.insert(smf.end(), trk.begin(), trk.end());
    return std::string(reinterpret_cast<const char*>(smf.data()), smf.size());
}

static roottree::Node makeLeaf(const std::string& name, const std::string& payload) {
    roottree::Node n; n.kind = roottree::Node::Leaf; n.name = name; n.payload = payload; return n;
}
static roottree::Node makeDir(const std::string& name, std::vector<roottree::Node> children) {
    roottree::Node n; n.kind = roottree::Node::Branch; n.name = name; n.children = std::move(children); return n;
}

static std::string serializeTree(std::vector<roottree::Node> children) {
    roottree::Node root;
    root.kind = roottree::Node::Root;
    root.name = "\x01";
    root.children = std::move(children);
    std::string out, err;
    const bool ok = roottree::write(root, out, err);
    if (!ok) {
        std::fprintf(stderr, "serializeTree failed: %s\n", err.c_str());
        std::abort();
    }
    return out;
}

static std::string makeStyleBytes(const std::string& xmlTitle,
                                  const std::string& mainMid,
                                  const std::string& kbdMid = "",
                                  const std::string& padMid = "",
                                  const std::string& midiLeafName = "Main.mid") {
    std::vector<roottree::Node> children;
    children.push_back(makeLeaf("StageDescription.xml", "<STAGE name=\"" + xmlTitle + "\"/>"));
    children.push_back(makeDir("Var1.scn", {
        makeDir("Track1.pdc", {
            makeLeaf("ClipDescription.xml", "<CLIP type=\"StyleClip\"/>"),
            makeLeaf(midiLeafName, mainMid)
        })
    }));
    if (!kbdMid.empty()) {
        children.push_back(makeDir("KbdSet1.scn", {
            makeDir("KS1.ksc", {
                makeLeaf("ClipDescription.xml", "<CLIP type=\"KeyboardSetClip\"/>"),
                makeDir("Main.ssn", {
                    makeDir("RTTracks.scn", {
                        makeDir("Upper1.plc", {makeLeaf("Main.mid", kbdMid)})
                    })
                })
            })
        }));
    }
    if (!padMid.empty()) {
        children.push_back(makeDir("PadSet1.scn", {
            makeDir("Pad1.pdc", {
                makeLeaf("ClipDescription.xml", "<CLIP type=\"PadClip\"/>"),
                makeLeaf("Main.mid", padMid)
            })
        }));
    }
    return serializeTree(std::move(children));
}

static std::string makeKscBytes(const std::string& xmlTitle, const std::string& mid) {
    return serializeTree({
        makeLeaf("ClipDescription.xml", "<CLIP type=\"KeyboardSetClip\" name=\"" + xmlTitle + "\"/>"),
        makeDir("Main.ssn", {
            makeDir("RTTracks.scn", {
                makeDir("Upper1.plc", {makeLeaf("Main.mid", mid)})
            })
        })
    });
}

static std::string makePdcBytes(const std::string& xmlTitle, const std::string& mid,
                                const std::string& midiLeafName = "Main.mid") {
    return serializeTree({
        makeLeaf("ClipDescription.xml", "<CLIP type=\"PadClip\" name=\"" + xmlTitle + "\"/>"),
        makeLeaf(midiLeafName, mid)
    });
}

static void writeFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    f.close();
}

static void writeBytes(const fs::path& p, const Bytes& b) {
    writeFile(p, std::string(reinterpret_cast<const char*>(b.data()), b.size()));
}

static std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static std::string dirDigest(const fs::path& root) {
    ai_arranger::media::Sha256 h;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file()) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        const std::string rel = fs::relative(f, root).generic_string();
        const std::string data = readAll(f);
        h.update(reinterpret_cast<const uint8_t*>(rel.data()), rel.size());
        h.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }
    return h.hex();
}

static bool hasAnyForbiddenDir(const fs::path& kstDir) {
    static const std::set<std::string> kForbidden = {"PCM", "SOUND", "MULTISMP", "SONGBOOK"};
    if (!fs::exists(kstDir)) return false;
    for (const auto& e : fs::directory_iterator(kstDir)) {
        const std::string up = ai_arranger::media::graph::native_detail::upperAscii(e.path().filename().string());
        if (kForbidden.count(up)) return true;
    }
    return false;
}

// ── Synthetic Source & Target KST Fixtures ───────────────────────────────────────────────────────────────────────────

struct SynthFixture {
    fs::path tmpRoot;
    fs::path srcKst;
    fs::path tgtKst;
    fs::path oc31TgtKst;

    SynthFixture() {
        const auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
        tmpRoot = fs::temp_directory_path() / ("aiarranger_m2e2_test_" + std::to_string(::getpid()) + "_" + std::to_string(ns));
        srcKst = tmpRoot / "SourceLib.KST";
        tgtKst = tmpRoot / "TargetLib.KST";
        oc31TgtKst = tmpRoot / "Oc31Target.KST";
        fs::create_directories(srcKst);
        fs::create_directories(tgtKst);
        fs::create_directories(oc31TgtKst);
        buildFixtures();
    }

    ~SynthFixture() {
        std::error_code ec;
        fs::remove_all(tmpRoot, ec);
    }

    void buildFixtures() {
        const RecordLayout L{12, 24, 8};
        const Id8 smpA = makeId(0xA1, 1);
        const Id8 smpB = makeId(0xA1, 2);
        const Id8 smpC = makeId(0xA1, 3);
        const Id8 msA  = makeId(0xB1, 1);
        const Id8 msB  = makeId(0xB1, 2);
        const Id8 msC  = makeId(0xB1, 3);

        // Source RAM.KMP & PCM
        SynthKmpSpec srcKmp;
        srcKmp.samples = {{smpA, "SynthSmpA", 48000}, {smpB, "SynthSmpB", 48000}, {smpC, "SynthSmpC", 48000}};
        srcKmp.zones   = {{0, 127}, {1, 127}, {2, 127}};
        srcKmp.multis  = {{msA, "SynthMsA", 0, 1}, {msB, "SynthMsB", 1, 1}, {msC, "SynthMsC", 2, 1}};
        writeBytes(srcKst / "MULTISMP/RAM.KMP",
                   buildKorfContainer({{0x11080018u, 0x11, 0, 0, "RAM", buildPa5xKmpPayload(srcKmp), ""}}));

        Bytes pcmPayloadA(4096, 0x11), pcmPayloadB(3072, 0x22), pcmPayloadC(2048, 0x33);
        writeBytes(srcKst / "PCM/RAM01.PCM", buildKorfContainer({
            {0x01100018u, 0x01, 0, 0, "SmpA", pcmPayloadA, idAsStr(smpA)},
            {0x01100018u, 0x01, 0, 1, "SmpB", pcmPayloadB, idAsStr(smpB)},
            {0x01100018u, 0x01, 0, 2, "SmpC", pcmPayloadC, idAsStr(smpC)}
        }));

        // Source USER01.PCG:
        //  slot 5: SoundA (analysed, msA)  -> exact same slot Identical in targetKst!
        //  slot 6: SoundB (analysed, msB)  -> targetKst has DIFFERENT Sound at USER01/6, IDENTICAL SoundB at USER04/12!
        //  slot 7: SoundC (analysed, msC)  -> NOT in targetKst (missing -> HOLD refusal!)
        //  slot 8: SoundNoRef (NoKnownReference: 0 RAM IDs -> InsufficientEvidence!)
        const Bytes bodyA = buildSoundBody(L, {{msA, kZeroId}}, 0x41);
        const Bytes bodyB = buildSoundBody(L, {{msB, kZeroId}}, 0x42);
        const Bytes bodyC = buildSoundBody(L, {{msC, kZeroId}}, 0x43);
        const Bytes bodyNoRef = buildSoundBody(L, {{kZeroId, kZeroId}}, 0x44);

        const Bytes bodyFit = buildSoundBody(L, {{msA, msB}}, 0x45);

        // Include >= 8 melodic items of kind 0x10080038u so default fitLayout() (fitMinItems == 8) fits RecordLayout{12,24,8}
        std::vector<SynthKorfItem> srcUser01Items = {
            {0x10080038u, 0x10, 0, 5, "SynthLeadA",  makeOcLiteralFrame("OC31", bodyA), ""},
            {0x10080038u, 0x10, 0, 6, "SynthBrassB", makeOcLiteralFrame("OC31", bodyB), ""},
            {0x10080038u, 0x10, 0, 7, "SynthPadC",   makeOcLiteralFrame("OC31", bodyC), ""},
            {0x10080038u, 0x10, 0, 8, "SynthRomRef", makeOcLiteralFrame("OC31", bodyNoRef), ""}
        };
        for (uint8_t s = 20; s < 26; ++s) {
            srcUser01Items.push_back({0x10080038u, 0x10, 0, s, "SynthFit" + std::to_string(s),
                                      makeOcLiteralFrame("OC31", bodyFit), ""});
        }
        writeBytes(srcKst / "SOUND/USER01.PCG", buildKorfContainer(srcUser01Items));

        // Source USERDK01.PCG: slot 0 unanalysed Drum Kit (size doesn't fit melodic layout)
        const Bytes dkBody(77, 0x77);
        writeBytes(srcKst / "SOUND/USERDK01.PCG", buildKorfContainer({
            {0x10080038u, 0x10, 0, 0, "SynthDrumKit", makeOcLiteralFrame("OC31", dkBody), ""}
        }));

        // Source Root-tree objects:
        // 1. Factory-only Style, KSC, Pad
        writeFile(srcKst / "STYLE/Pop/FactoryBeat.stg",
                  makeStyleBytes("FactoryBeat",
                                 buildSmf({{1, 121, 0, 33}, {2, 121, 1, 48}}, true),
                                 buildSmf({{1, 121, 0, 0}}),
                                 buildSmf({{1, 121, 0, 25}})));
        writeFile(srcKst / "KEYBOARDSET/Pop/FactoryKeys.ksc",
                  makeKscBytes("FactoryKeys", buildSmf({{1, 121, 0, 4}})));
        writeFile(srcKst / "PAD/Pop/FactoryPad.pdc",
                  makePdcBytes("FactoryPad", buildSmf({{1, 121, 0, 88}})));

        // 2. Exact target-matched User Sound Style (uses Factory + USER01/5 in main & embedded KBD Set)
        writeFile(srcKst / "STYLE/Pop/ExactUser.stg",
                  makeStyleBytes("ExactUser",
                                 buildSmf({{1, 121, 0, 33}, {2, 121, 64, 5}}, true),
                                 buildSmf({{1, 121, 64, 5}})));

        // 3. Cross-slot remap Style: uses USER01/6 (shared bank with USER01/5 + running status + Korg SysEx!)
        //    USER01/6 -> remaps to USER04/12 (MSB 121, LSB 67, PC 12), while USER01/5 stays (121, 64, 5)!
        writeFile(srcKst / "STYLE/Pop/RemapUser.stg",
                  makeStyleBytes("RemapUser",
                                 buildSmf({{1, 121, 64, 6}, {1, 121, 64, 5}}, true, true),
                                 buildSmf({{2, 121, 64, 6}})));

        // 4. Non-present User Sound Style (uses USER01/7, absent in targetKst)
        writeFile(srcKst / "STYLE/Pop/MissingInTarget.stg",
                  makeStyleBytes("MissingInTarget", buildSmf({{1, 121, 64, 7}})));

        // 5. Dangling source User Sound Style (uses USER01/99, empty slot in sourceKst!)
        writeFile(srcKst / "STYLE/Pop/DanglingSource.stg",
                  makeStyleBytes("DanglingSource", buildSmf({{1, 121, 64, 99}})));

        // 6. Unanalysed USERDK Style (uses MSB 120, LSB 64, PC 0 -> USERDK01/0)
        writeFile(srcKst / "STYLE/Pop/UsesDrumKit.stg",
                  makeStyleBytes("UsesDrumKit", buildSmf({{10, 120, 64, 0}})));

        // 7. NoKnownReference / InsufficientEvidence Sound Style (uses USER01/8)
        writeFile(srcKst / "STYLE/Pop/UsesNoKnownRef.stg",
                  makeStyleBytes("UsesNoKnownRef", buildSmf({{1, 121, 64, 8}})));

        // 8. Malformed MIDI leaf Pad (CC11 >= 0x80)
        writeFile(srcKst / "PAD/Pop/BadHighBit.pdc",
                  makePdcBytes("BadHighBit", buildSmf({{1, 121, 0, 1}}, false, false, true)));

        // 9. Non-lowercase .MID leaf Pad
        writeFile(srcKst / "PAD/Pop/UpperMidLeaf.pdc",
                  makePdcBytes("UpperMidLeaf", buildSmf({{1, 121, 0, 1}}), "Main.MID"));

        // 10. Unresolved bank select Style (PC without CC0/CC32)
        writeFile(srcKst / "STYLE/Pop/UnresolvedBank.stg",
                  makeStyleBytes("UnresolvedBank", buildSmf({{1, -1, -1, 12}})));

        // ── Target KST ───────────────────────────────────────────────────────────────────────────────────────────────
        // Holds identical RAM.KMP & PCM for smpA/smpB, plus different Sound at USER01/6 and identical SoundB at USER04/12!
        SynthKmpSpec tgtKmp;
        tgtKmp.samples = {{smpA, "SynthSmpA", 48000}, {smpB, "SynthSmpB", 48000}};
        tgtKmp.zones   = {{0, 127}, {1, 127}};
        tgtKmp.multis  = {{msA, "SynthMsA", 0, 1}, {msB, "SynthMsB", 1, 1}};
        writeBytes(tgtKst / "MULTISMP/RAM.KMP",
                   buildKorfContainer({{0x11080018u, 0x11, 0, 0, "RAM", buildPa5xKmpPayload(tgtKmp), ""}}));
        writeBytes(tgtKst / "PCM/RAM01.PCM", buildKorfContainer({
            {0x01100018u, 0x01, 0, 0, "SmpA", pcmPayloadA, idAsStr(smpA)},
            {0x01100018u, 0x01, 0, 1, "SmpB", pcmPayloadB, idAsStr(smpB)}
        }));

        const Bytes diffBodyAtSlot6 = buildSoundBody(L, {{msA, kZeroId}}, 0x99); // different body at USER01/6!
        std::vector<SynthKorfItem> tgtUser01Items = {
            {0x10080038u, 0x10, 0, 5, "SynthLeadA",  makeOcLiteralFrame("OC31", bodyA), ""},
            {0x10080038u, 0x10, 0, 6, "OtherSound",  makeOcLiteralFrame("OC31", diffBodyAtSlot6), ""},
            {0x10080038u, 0x10, 0, 8, "SynthRomRef", makeOcLiteralFrame("OC31", bodyNoRef), ""}
        };
        for (uint8_t s = 20; s < 26; ++s) {
            tgtUser01Items.push_back({0x10080038u, 0x10, 0, s, "TgtFit" + std::to_string(s),
                                      makeOcLiteralFrame("OC31", bodyFit), ""});
        }
        writeBytes(tgtKst / "SOUND/USER01.PCG", buildKorfContainer(tgtUser01Items));
        writeBytes(tgtKst / "SOUND/USER04.PCG", buildKorfContainer({
            {0x10080038u, 0x10, 0, 12, "SynthBrassB", makeOcLiteralFrame("OC31", bodyB), ""}
        }));
        writeBytes(tgtKst / "SOUND/USERDK01.PCG", buildKorfContainer({
            {0x10080038u, 0x10, 0, 0, "SynthDrumKit", makeOcLiteralFrame("OC31", dkBody), ""}
        }));
        // Add an existing colliding Style in targetKst with DIFFERENT bytes at STYLE/Pop/CollideMe.stg
        writeFile(srcKst / "STYLE/Pop/CollideMe.stg",
                  makeStyleBytes("CollideSource", buildSmf({{1, 121, 0, 1}})));
        writeFile(tgtKst / "STYLE/Pop/CollideMe.stg",
                  makeStyleBytes("CollideTargetDifferent", buildSmf({{1, 121, 0, 2}})));

        // ── OC31-KMP Target KST (Uninspectable legacy Pa700/Pa4X conversion) ─────────────────────────────────────────
        const Bytes fakeOc31KmpPlain(64, 0x55);
        writeBytes(oc31TgtKst / "MULTISMP/RAM.KMP",
                   buildKorfContainer({{0x11050038u, 0x11, 0, 0, "RAM", makeOcLiteralFrame("OC31", fakeOc31KmpPlain), ""}}));
        writeBytes(oc31TgtKst / "SOUND/USER01.PCG", buildKorfContainer({
            {0x10080038u, 0x10, 0, 5, "SynthLeadA", makeOcLiteralFrame("OC31", bodyA), ""}
        }));
        writeBytes(oc31TgtKst / "PCM/RAM01.PCM", buildKorfContainer({
            {0x01100018u, 0x01, 0, 0, "SmpA", pcmPayloadA, idAsStr(smpA)}
        }));
    }
};

// ── Subprocess Helper for CLI Tests ──────────────────────────────────────────────────────────────────────────────────

struct CliResult {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

static CliResult runCli(const std::vector<std::string>& args) {
    CliResult r;
    int outPipe[2], errPipe[2];
    if (::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) return r;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        ::close(outPipe[0]); ::close(outPipe[1]);
        ::close(errPipe[0]); ::close(errPipe[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        std::string bin = RESOURCES_BIN;
        argv.push_back(bin.data());
        std::vector<std::string> copy = args;
        for (auto& s : copy) argv.push_back(s.data());
        argv.push_back(nullptr);
        ::execv(bin.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    auto drain = [](int fd, std::string& dst) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            dst.append(buf, static_cast<size_t>(n));
        }
        ::close(fd);
    };
    drain(outPipe[0], r.stdoutText);
    drain(errPipe[0], r.stderrText);
    int status = 0;
    if (::waitpid(pid, &status, 0) > 0 && WIFEXITED(status)) {
        r.exitCode = WEXITSTATUS(status);
    }
    return r;
}

// ── Tests ────────────────────────────────────────────────────────────────────────────────────────────────────────────

int main() {
    SynthFixture fx;
    const std::string srcDigestBefore = dirDigest(fx.srcKst);
    const std::string tgtDigestBefore = dirDigest(fx.tgtKst);

    std::printf("=== 1. Factory-Only Sub-KST Export & Determinism ===\n");
    {
        subkst::SubKstExportRequest req;
        req.sourceKst = fx.srcKst;
        req.targets = {
            {SelectedKind::Style, "STYLE/Pop/FactoryBeat.stg", "", ""},
            {SelectedKind::KeyboardSet, "KEYBOARDSET/Pop/FactoryKeys.ksc", "", ""},
            {SelectedKind::Pad, "PAD/Pop/FactoryPad.pdc", "", ""}
        };
        const auto planA = subkst::planSubKstExport(req);
        TEST("Factory-only plan ok & executable", planA.ok && planA.executable());
        TEST("Factory-only has 0 target-matched user slots", planA.allowedTargetMatchedUserSlots.empty());
        TEST("Factory-only has 0 sound remaps", planA.soundRemaps.empty());

        // Shuffled selection order produces identical planFingerprint
        subkst::SubKstExportRequest reqRev = req;
        std::reverse(reqRev.targets.begin(), reqRev.targets.end());
        const auto planB = subkst::planSubKstExport(reqRev);
        TEST("Shuffled --select produces identical planFingerprint", planA.planFingerprint == planB.planFingerprint);

        const fs::path out1 = fx.tmpRoot / "FactoryOut1.KST";
        const fs::path out2 = fx.tmpRoot / "FactoryOut2.KST";
        auto sum1 = std::make_shared<subkst::SubKstExecutionSummary>();
        auto p1 = std::make_shared<subkst::SubKstExportPlan>();
        jobs::JobState st1; st1.id = 1;
        jobs::runExport(subkst::selectiveSubKstExportSpec(req, out1, p1, sum1), st1, [] { return false; }, [](const jobs::JobState&) {});
        TEST("Factory-only export Done", st1.phase == jobs::Phase::Done);
        TEST("Factory-only L1 Passed, L2 NotRun, L3 NotRun",
             st1.level.l1.state == jobs::Check::Passed &&
             st1.level.l2.state == jobs::Check::NotRun &&
             st1.level.l3.state == jobs::Check::NotRun);
        TEST("Factory-only verifyText carries unverified banner",
             st1.verifyText.find(kHardwareUnverifiedBanner) != std::string::npos);
        TEST("Factory-only ZERO PCM/SOUND/MULTISMP/SONGBOOK dirs", !hasAnyForbiddenDir(out1));

        // Independent W5 stagedRefusal + library::validate on published Factory-only sub-KST
        assemble::Result asmRes;
        TEST("Factory-only passes assemble::stagedRefusal verbatim", assemble::stagedRefusal(out1, asmRes).empty());
        const auto val = lib::validate(out1);
        TEST("Factory-only library::validate 0 errors", val.ok && val.count(lib::Severity::Error) == 0);
        TEST("Factory-only .stg byte-identical to source",
             readAll(out1 / "STYLE/Pop/FactoryBeat.stg") == readAll(fx.srcKst / "STYLE/Pop/FactoryBeat.stg"));
        TEST("Factory-only .ksc byte-identical to source",
             readAll(out1 / "KEYBOARDSET/Pop/FactoryKeys.ksc") == readAll(fx.srcKst / "KEYBOARDSET/Pop/FactoryKeys.ksc"));
        TEST("Factory-only .pdc byte-identical to source",
             readAll(out1 / "PAD/Pop/FactoryPad.pdc") == readAll(fx.srcKst / "PAD/Pop/FactoryPad.pdc"));

        // Second run with reversed selection order produces byte-identical folder & JSON
        auto sum2 = std::make_shared<subkst::SubKstExecutionSummary>();
        auto p2 = std::make_shared<subkst::SubKstExportPlan>();
        jobs::JobState st2; st2.id = 2;
        jobs::runExport(subkst::selectiveSubKstExportSpec(reqRev, out2, p2, sum2), st2, [] { return false; }, [](const jobs::JobState&) {});
        TEST("Reversed-order second export Done", st2.phase == jobs::Phase::Done);
        TEST("Repeated & reversed runs produce byte-identical sub-KST", dirDigest(out1) == dirDigest(out2));

        const std::string j1 = subkst::serializeSubKstExportJson(*p1, *sum1, st1);
        kit::Json parsed;
        TEST("Export JSON parses cleanly", kit::parseJson(j1, &parsed).empty());
        TEST("Export JSON has hardwareVerified=false & deleteProofAvailable=false",
             j1.find("\"hardwareVerified\": false") != std::string::npos &&
             j1.find("\"deleteProofAvailable\": false") != std::string::npos &&
             j1.find(kHardwareUnverifiedBanner) != std::string::npos);
        TEST("Export JSON scrubs local temp paths", j1.find(fx.tmpRoot.string()) == std::string::npos);
    }

    std::printf("=== 2. Exact Same-Slot Target-Matched User Sound Export ===\n");
    {
        subkst::SubKstExportRequest req;
        req.sourceKst = fx.srcKst;
        req.targetKst = fx.tgtKst;
        req.targets = {{SelectedKind::Style, "STYLE/Pop/ExactUser.stg", "", ""}};
        req.policy = ConflictPolicy::BlockWithReport;

        const auto plan = subkst::planSubKstExport(req);
        TEST("Exact same-slot target match is executable", plan.ok && plan.executable());
        TEST("Allowed target slots contains USER01.PCG slot 5",
             plan.allowedTargetMatchedUserSlots.size() == 1 &&
             plan.allowedTargetMatchedUserSlots.count("USER01.PCG slot 5") == 1);
        TEST("Exact same-slot requires 0 SMF remaps", plan.soundRemaps.empty() && !plan.anySoundRemapped);

        const fs::path out = fx.tmpRoot / "ExactUserOut.KST";
        jobs::JobState st; st.id = 3;
        jobs::runExport(subkst::selectiveSubKstExportSpec(req, out), st, [] { return false; }, [](const jobs::JobState&) {});
        TEST("Exact target-matched export Done", st.phase == jobs::Phase::Done);
        TEST("Exact target-matched ZERO forbidden native dirs", !hasAnyForbiddenDir(out));
        TEST("Exact target-matched .stg byte-identical to source",
             readAll(out / "STYLE/Pop/ExactUser.stg") == readAll(fx.srcKst / "STYLE/Pop/ExactUser.stg"));
        const auto val = lib::validate(out);
        TEST("Exact target-matched library::validate 0 errors", val.ok && val.count(lib::Severity::Error) == 0);
        const auto cl = closure::check(out);
        TEST("Exact target-matched closure has only expected target slot dangling",
             cl.ok && cl.unresolved == 0 && cl.resolved == 0 &&
             cl.dangling.size() == 1 && cl.dangling.count("USER01.PCG slot 5") == 1);
    }

    std::printf("=== 3. Cross-Slot ReuseIdenticalTargetSlot Remap & SONGBOOK_UNREWRITTEN ===\n");
    {
        // Under BlockWithReport -> blocked fail-closed!
        subkst::SubKstExportRequest reqBlock;
        reqBlock.sourceKst = fx.srcKst;
        reqBlock.targetKst = fx.tgtKst;
        reqBlock.targets = {{SelectedKind::Style, "STYLE/Pop/RemapUser.stg", "", ""}};
        reqBlock.policy = ConflictPolicy::BlockWithReport;
        const auto planBlock = subkst::planSubKstExport(reqBlock);
        TEST("Cross-slot under BlockWithReport is NOT executable", planBlock.ok && !planBlock.executable());

        // Under ReuseIdenticalTargetSlot with requireExplicitRemapApproval=true and no approvals -> blocked!
        subkst::SubKstExportRequest reqExplicit = reqBlock;
        reqExplicit.policy = ConflictPolicy::ReuseIdenticalTargetSlot;
        reqExplicit.requireExplicitRemapApproval = true;
        const auto planExplicitBlocked = subkst::planSubKstExport(reqExplicit);
        TEST("Cross-slot with requireExplicitRemapApproval & no approvals fails closed (SMF_REMAP_REQUIRED)",
             planExplicitBlocked.ok && !planExplicitBlocked.executable() &&
             planExplicitBlocked.planResult.dryRunPlan.hasBlockingCode(collector_diag_code::kSmfRemapRequired));

        // Under ReuseIdenticalTargetSlot -> succeeds & remaps USER01/6 -> USER04/12!
        subkst::SubKstExportRequest reqReuse = reqBlock;
        reqReuse.policy = ConflictPolicy::ReuseIdenticalTargetSlot;
        const auto planReuse = subkst::planSubKstExport(reqReuse);
        TEST("Cross-slot ReuseIdenticalTargetSlot is executable", planReuse.ok && planReuse.executable());
        TEST("Cross-slot remap table maps USER01/6 -> USER04/12",
             planReuse.soundRemaps.size() == 1 &&
             planReuse.soundRemaps[0].srcTriple == (smf_remap::MidiTriple{121, 64, 6}) &&
             planReuse.soundRemaps[0].dstTriple == (smf_remap::MidiTriple{121, 67, 12}));
        TEST("Cross-slot emits mandatory SONGBOOK_UNREWRITTEN warning",
             planReuse.planResult.dryRunPlan.hasDiagnosticCode(diag_code::kSongBookUnrewritten, DiagnosticSeverity::Warning));

        const fs::path out = fx.tmpRoot / "RemapUserOut.KST";
        auto execSum = std::make_shared<subkst::SubKstExecutionSummary>();
        jobs::JobState st; st.id = 4;
        jobs::runExport(subkst::selectiveSubKstExportSpec(reqReuse, out, nullptr, execSum), st, [] { return false; }, [](const jobs::JobState&) {});
        TEST("Cross-slot remap export Done", st.phase == jobs::Phase::Done);
        TEST("Cross-slot remap ZERO forbidden native dirs", !hasAnyForbiddenDir(out));
        TEST("Cross-slot remap summary carries SONGBOOK_UNREWRITTEN warning",
             !execSum->warnings.empty() &&
             execSum->warnings.front().find(diag_code::kSongBookUnrewritten) != std::string::npos);

        // Re-parse exported .stg & verify post-remap MIDI selects, Korg SysEx, and XML byte preservation!
        const std::string outBytes = readAll(out / "STYLE/Pop/RemapUser.stg");
        const std::string srcBytes = readAll(fx.srcKst / "STYLE/Pop/RemapUser.stg");
        TEST("Remapped .stg differs from source bytes", outBytes != srcBytes);

        const auto origTree = roottree::parse(reinterpret_cast<const uint8_t*>(srcBytes.data()), srcBytes.size());
        const auto outTree = roottree::parse(reinterpret_cast<const uint8_t*>(outBytes.data()), outBytes.size());
        TEST("Exported remapped .stg re-parses cleanly", origTree.ok && outTree.ok);

        std::map<std::string, std::string> origNonMidi, outNonMidi;
        std::vector<std::pair<std::string, std::vector<closure::Select>>> origSel, outSel;
        std::string derr;
        TEST("Extract digests from orig & exported .stg",
             subkst::detail::collectRootTreeDigest(origTree.root, "", origNonMidi, origSel, derr) &&
             subkst::detail::collectRootTreeDigest(outTree.root, "", outNonMidi, outSel, derr));
        TEST("XML leaves preserved byte-identically after remap", origNonMidi == outNonMidi && !origNonMidi.empty());
        TEST("Remapped .stg MIDI leaf 0 remapped PC0->(121,67,12) and preserved PC1->(121,64,5)",
             outSel.size() == 2 &&
             outSel[0].second.size() == 2 &&
             outSel[0].second[0].msb == 121 && outSel[0].second[0].lsb == 67 && outSel[0].second[0].program == 12 &&
             outSel[0].second[1].msb == 121 && outSel[0].second[1].lsb == 64 && outSel[0].second[1].program == 5);
        TEST("Remapped embedded KBD Set MIDI leaf remapped to (121,67,12)",
             outSel.size() >= 2 &&
             outSel[1].second.size() == 1 &&
             outSel[1].second[0].msb == 121 && outSel[1].second[0].lsb == 67 && outSel[1].second[0].program == 12);
        // Verify Korg SysEx F0 42 30 7F 01 F7 still intact in exported .stg!
        const std::string sysexNeedle("\xF0\x05\x42\x30\x7F\x01\xF7", 7);
        TEST("Korg SysEx preserved verbatim in remapped .stg", outBytes.find(sysexNeedle) != std::string::npos);

        // Target-verified closure check on published output
        const auto cl = closure::check(out);
        TEST("Remapped sub-KST closure has ONLY USER01/5 and USER04/12 dangling",
             cl.ok && cl.unresolved == 0 && cl.dangling.size() == 2 &&
             cl.dangling.count("USER01.PCG slot 5") == 1 &&
             cl.dangling.count("USER04.PCG slot 12") == 1);

        // Also verify rename/rebank with NO targetKst emits SONGBOOK_UNREWRITTEN warning!
        subkst::SubKstExportRequest reqRename;
        reqRename.sourceKst = fx.srcKst;
        reqRename.targets = {{SelectedKind::Style, "STYLE/Pop/FactoryBeat.stg", "Dance", "RenamedBeat.stg"}};
        const auto planRename = subkst::planSubKstExport(reqRename);
        TEST("Rename/rebank emits SONGBOOK_UNREWRITTEN warning even without targetKst",
             planRename.ok && planRename.executable() &&
             planRename.planResult.dryRunPlan.hasDiagnosticCode(diag_code::kSongBookUnrewritten, DiagnosticSeverity::Warning));
        const fs::path outRen = fx.tmpRoot / "RenamedOut.KST";
        jobs::JobState stRen; stRen.id = 5;
        jobs::runExport(subkst::selectiveSubKstExportSpec(reqRename, outRen), stRen, [] { return false; }, [](const jobs::JobState&) {});
        TEST("Rename/rebank export publishes to STYLE/Dance/RenamedBeat.stg",
             stRen.phase == jobs::Phase::Done && fs::is_regular_file(outRen / "STYLE/Dance/RenamedBeat.stg"));
    }

    std::printf("=== 4. Negative & Fail-Closed HOLD / Missing / USERDK / Malformed / OC31 Tests ===\n");
    {
        auto assertRefusedNothingWritten = [&](const char* label, const subkst::SubKstExportRequest& req,
                                               const std::string& expectCode, const std::string& outName) {
            const auto plan = subkst::planSubKstExport(req);
            const bool hasCode = expectCode.empty() || plan.planResult.dryRunPlan.hasBlockingCode(expectCode) ||
                                 std::any_of(plan.planResult.report.diagnostics.begin(),
                                             plan.planResult.report.diagnostics.end(),
                                             [&](const Diagnostic& d) {
                                                 return d.severity == DiagnosticSeverity::Blocking && d.code == expectCode;
                                             });
            const fs::path out = fx.tmpRoot / outName;
            jobs::JobState st; st.id = 10;
            jobs::runExport(subkst::selectiveSubKstExportSpec(req, out), st, [] { return false; }, [](const jobs::JobState&) {});
            TEST(label, !plan.executable() && hasCode && st.phase == jobs::Phase::Failed &&
                        !fs::exists(out) && !fs::exists(ai_arranger::media::incompletePath(out)));
        };

        // 4a. ExportMode::NativeSoundKst_HOLD
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.mode = ExportMode::NativeSoundKst_HOLD;
            r.targets = {{SelectedKind::Style, "STYLE/Pop/FactoryBeat.stg", "", ""}};
            assertRefusedNothingWritten("NativeSoundKst_HOLD refused fail-closed", r,
                                        diag_code::kNativeSoundWriteHold, "HoldModeOut.KST");
        }
        // 4b. Non-present User Sound (no target)
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targets = {{SelectedKind::Style, "STYLE/Pop/ExactUser.stg", "", ""}};
            assertRefusedNothingWritten("User Sound without target refused (NATIVE_SOUND_WRITE_HOLD)", r,
                                        diag_code::kNativeSoundWriteHold, "NoTargetUserOut.KST");
        }
        // 4c. Non-present User Sound (absent in targetKst) + MAN5 p.736 steps populated
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targetKst = fx.tgtKst;
            r.policy = ConflictPolicy::RemapToFreeSlotInNewLib;
            r.targets = {{SelectedKind::Style, "STYLE/Pop/MissingInTarget.stg", "", ""}};
            const auto pl = subkst::planSubKstExport(r);
            TEST("Missing-in-target plan emits MAN5 p.736 single-Sound remediation step",
                 !pl.planResult.pa5xSingleSoundSteps.empty() &&
                 pl.planResult.pa5xSingleSoundSteps[0].instruction.find("MAN5 p.736") != std::string::npos);
            assertRefusedNothingWritten("User Sound absent in target refused even under remap (NATIVE_SOUND_WRITE_HOLD)", r,
                                        diag_code::kNativeSoundWriteHold, "MissingInTargetOut.KST");
        }
        // 4d. Dangling User Sound in source KST (VERIFIED_MISSING)
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targetKst = fx.tgtKst;
            r.targets = {{SelectedKind::Style, "STYLE/Pop/DanglingSource.stg", "", ""}};
            assertRefusedNothingWritten("Dangling source User Sound refused (MISSING_USER_SOUND_SLOT)", r,
                                        diag_code::kMissingUserSoundSlot, "DanglingSourceOut.KST");
        }
        // 4e. Unanalysed USERDK
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targetKst = fx.tgtKst;
            r.policy = ConflictPolicy::ReuseIdenticalTargetSlot;
            r.targets = {{SelectedKind::Style, "STYLE/Pop/UsesDrumKit.stg", "", ""}};
            assertRefusedNothingWritten("Unanalysed USERDK refused fail-closed", r,
                                        diag_code::kDrumkitBodyUnparsed, "UsesDrumKitOut.KST");
        }
        // 4f. NoKnownReference / InsufficientEvidence Sound
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targetKst = fx.tgtKst;
            r.policy = ConflictPolicy::ReuseIdenticalTargetSlot;
            r.targets = {{SelectedKind::Style, "STYLE/Pop/UsesNoKnownRef.stg", "", ""}};
            assertRefusedNothingWritten("NoKnownReference (InsufficientEvidence) Sound refused fail-closed", r,
                                        diag_code::kNativeSoundWriteHold, "UsesNoKnownRefOut.KST");
        }
        // 4g. Malformed MIDI leaf (CC11 >= 0x80)
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targets = {{SelectedKind::Pad, "PAD/Pop/BadHighBit.pdc", "", ""}};
            assertRefusedNothingWritten("Malformed MIDI leaf (CC11 >= 0x80) refused (MALFORMED_MIDI_LEAF)", r,
                                        diag_code::kMalformedMidiLeaf, "BadHighBitOut.KST");
        }
        // 4h. Non-lowercase .MID leaf
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targets = {{SelectedKind::Pad, "PAD/Pop/UpperMidLeaf.pdc", "", ""}};
            assertRefusedNothingWritten("Non-lowercase .MID leaf refused (MALFORMED_MIDI_LEAF)", r,
                                        diag_code::kMalformedMidiLeaf, "UpperMidOut.KST");
        }
        // 4i. Unresolved MIDI bank select (msb < 0 || lsb < 0)
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targets = {{SelectedKind::Style, "STYLE/Pop/UnresolvedBank.stg", "", ""}};
            assertRefusedNothingWritten("Unresolved MIDI bank select refused (UNRESOLVED_MIDI_BANK_SELECT)", r,
                                        diag_code::kUnresolvedMidiBankSelect, "UnresolvedBankOut.KST");
        }
        // 4j. OC31 KMP TargetUninspectable when User Sound referenced
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targetKst = fx.oc31TgtKst;
            r.policy = ConflictPolicy::ReuseIdenticalTargetSlot;
            r.targets = {{SelectedKind::Style, "STYLE/Pop/ExactUser.stg", "", ""}};
            assertRefusedNothingWritten("OC31 KMP target refused fail-closed (TARGET_UNINSPECTABLE)", r,
                                        diag_code::kTargetUninspectable, "Oc31TargetOut.KST");
        }
        // 4k. Target object path collision (different bytes at same path)
        {
            subkst::SubKstExportRequest r;
            r.sourceKst = fx.srcKst;
            r.targetKst = fx.tgtKst;
            r.policy = ConflictPolicy::ReuseIdenticalTargetSlot;
            r.targets = {{SelectedKind::Style, "STYLE/Pop/CollideMe.stg", "", ""}};
            assertRefusedNothingWritten("Target object path collision refused (STYLE_OR_OBJECT_PATH_COLLISION)", r,
                                        diag_code::kStyleOrObjectPathCollision, "CollideOut.KST");
        }
    }

    std::printf("=== 5. TOCTOU Protection (Validate + Pre-Publish Source, Target & Staged Recheck) ===\n");
    {
        // Make isolated disposable copies of src & tgt so main fixtures remain pristine
        const fs::path tSrc = fx.tmpRoot / "ToctouSrc.KST";
        const fs::path tTgt = fx.tmpRoot / "ToctouTgt.KST";
        fs::copy(fx.srcKst, tSrc, fs::copy_options::recursive);
        fs::copy(fx.tgtKst, tTgt, fs::copy_options::recursive);

        subkst::SubKstExportRequest req;
        req.sourceKst = tSrc;
        req.targetKst = tTgt;
        req.targets = {{SelectedKind::Style, "STYLE/Pop/ExactUser.stg", "", ""}};
        const auto reviewed = subkst::planSubKstExport(req);
        TEST("TOCTOU baseline review executable", reviewed.ok && reviewed.executable());

        // 5a. Mutate source .stg between review & execute -> caught at Validate via expectFingerprint!
        const std::string origStg = readAll(tSrc / "STYLE/Pop/ExactUser.stg");
        writeFile(tSrc / "STYLE/Pop/ExactUser.stg",
                  makeStyleBytes("ExactUserMutated", buildSmf({{1, 121, 0, 33}, {2, 121, 64, 5}}, true)));
        {
            subkst::SubKstExportRequest rMut = req;
            rMut.expectFingerprint = reviewed.planFingerprint;
            const fs::path out = fx.tmpRoot / "ToctouValSrcOut.KST";
            jobs::JobState st; st.id = 20;
            jobs::runExport(subkst::selectiveSubKstExportSpec(rMut, out), st, [] { return false; }, [](const jobs::JobState&) {});
            TEST("TOCTOU Validate catches source .stg mutation",
                 st.phase == jobs::Phase::Failed && st.stoppedAt == jobs::Phase::Validate && !fs::exists(out));
        }
        writeFile(tSrc / "STYLE/Pop/ExactUser.stg", origStg);

        // 5b. Mutate targetKst/SOUND/USER01.PCG between review & execute -> caught at Validate!
        const std::string origPcg = readAll(tTgt / "SOUND/USER01.PCG");
        {
            std::string mutatedPcg = origPcg;
            const size_t pos = mutatedPcg.find("OtherSound");
            if (pos != std::string::npos) mutatedPcg[pos] = 'X';
            writeFile(tTgt / "SOUND/USER01.PCG", mutatedPcg);

            subkst::SubKstExportRequest rMut = req;
            rMut.expectFingerprint = reviewed.planFingerprint;
            const fs::path out = fx.tmpRoot / "ToctouValTgtOut.KST";
            jobs::JobState st; st.id = 21;
            jobs::runExport(subkst::selectiveSubKstExportSpec(rMut, out), st, [] { return false; }, [](const jobs::JobState&) {});
            TEST("TOCTOU Validate catches target USER01.PCG mutation",
                 st.phase == jobs::Phase::Failed && st.stoppedAt == jobs::Phase::Validate && !fs::exists(out));
        }
        writeFile(tTgt / "SOUND/USER01.PCG", origPcg);

        // 5c. Mutate source .stg at beforePublish -> caught by beforePublish source SHA-256 check!
        {
            const fs::path out = fx.tmpRoot / "ToctouPubSrcOut.KST";
            jobs::JobState st; st.id = 22;
            jobs::runExport(
                subkst::selectiveSubKstExportSpec(req, out, nullptr, nullptr, [&](const char* phase) {
                    if (std::strcmp(phase, "beforePublish") == 0) {
                        std::string s = origStg;
                        const size_t p = s.find("ExactUser");
                        if (p != std::string::npos) s[p] = 'Z'; // same size, flipped byte!
                        writeFile(tSrc / "STYLE/Pop/ExactUser.stg", s);
                    }
                }),
                st, [] { return false; }, [](const jobs::JobState&) {});
            TEST("TOCTOU beforePublish catches source .stg mutation",
                 st.phase == jobs::Phase::Failed && st.stoppedAt == jobs::Phase::Publish && !fs::exists(out));
        }
        writeFile(tSrc / "STYLE/Pop/ExactUser.stg", origStg);

        // 5d. Mutate targetKst/SOUND/USER01.PCG at beforePublish -> caught by targetLibraryKey check!
        {
            const fs::path out = fx.tmpRoot / "ToctouPubTgtOut.KST";
            jobs::JobState st; st.id = 23;
            jobs::runExport(
                subkst::selectiveSubKstExportSpec(req, out, nullptr, nullptr, [&](const char* phase) {
                    if (std::strcmp(phase, "beforePublish") == 0) {
                        std::string m = origPcg;
                        const size_t p = m.find("OtherSound");
                        if (p != std::string::npos) m[p] = 'Y';
                        writeFile(tTgt / "SOUND/USER01.PCG", m);
                    }
                }),
                st, [] { return false; }, [](const jobs::JobState&) {});
            TEST("TOCTOU beforePublish catches target USER01.PCG mutation",
                 st.phase == jobs::Phase::Failed && st.stoppedAt == jobs::Phase::Publish && !fs::exists(out));
        }
        writeFile(tTgt / "SOUND/USER01.PCG", origPcg);

        // 5e. Tamper staged file between verify and publish -> caught by library::recheckPinned()!
        {
            const fs::path out = fx.tmpRoot / "ToctouPubStagedOut.KST";
            jobs::JobState st; st.id = 24;
            jobs::runExport(
                subkst::selectiveSubKstExportSpec(req, out, nullptr, nullptr, [&](const char* phase) {
                    if (std::strcmp(phase, "beforePublish") == 0) {
                        const fs::path stagedStg = ai_arranger::media::incompletePath(out) / out.filename() / "STYLE/Pop/ExactUser.stg";
                        std::string b = readAll(stagedStg);
                        if (!b.empty()) b[0] ^= 0x01;
                        writeFile(stagedStg, b);
                    }
                }),
                st, [] { return false; }, [](const jobs::JobState&) {});
            TEST("TOCTOU beforePublish recheckPinned catches staged tamper",
                 st.phase == jobs::Phase::Failed && st.stoppedAt == jobs::Phase::Publish && !fs::exists(out));
        }
    }

    std::printf("=== 6. D18 Overlap Protection (Direct & Symlink) ===\n");
    {
        subkst::SubKstExportRequest req;
        req.sourceKst = fx.srcKst;
        req.targetKst = fx.tgtKst;
        req.targets = {{SelectedKind::Style, "STYLE/Pop/ExactUser.stg", "", ""}};

        // 6a. Direct overlap inside sourceKst
        {
            const fs::path out = fx.srcKst / "NestedInsideSource.KST";
            jobs::JobState st; st.id = 30;
            jobs::runExport(subkst::selectiveSubKstExportSpec(req, out), st, [] { return false; }, [](const jobs::JobState&) {});
            TEST("D18 refuses --out inside sourceKst before any mkdir",
                 st.phase == jobs::Phase::Failed && !fs::exists(out) && !fs::exists(ai_arranger::media::incompletePath(out)));
        }
        // 6b. Direct overlap inside targetKst
        {
            const fs::path out = fx.tgtKst / "NestedInsideTarget.KST";
            jobs::JobState st; st.id = 31;
            jobs::runExport(subkst::selectiveSubKstExportSpec(req, out), st, [] { return false; }, [](const jobs::JobState&) {});
            TEST("D18 refuses --out inside targetKst before any mkdir",
                 st.phase == jobs::Phase::Failed && !fs::exists(out) && !fs::exists(ai_arranger::media::incompletePath(out)));
        }
        // 6c. Symlink overlap pointing inside targetKst
        {
            const fs::path linkDir = fx.tmpRoot / "link_to_target";
            fs::create_directory_symlink(fx.tgtKst, linkDir);
            const fs::path out = linkDir / "ViaSymlink.KST";
            jobs::JobState st; st.id = 32;
            jobs::runExport(subkst::selectiveSubKstExportSpec(req, out), st, [] { return false; }, [](const jobs::JobState&) {});
            TEST("D18 refuses --out via symlink pointing inside targetKst",
                 st.phase == jobs::Phase::Failed && !fs::exists(fx.tgtKst / "ViaSymlink.KST"));
        }
        // 6d. Symlink sourceKst refused
        {
            const fs::path linkSrc = fx.tmpRoot / "SymlinkSource.KST";
            fs::create_directory_symlink(fx.srcKst, linkSrc);
            subkst::SubKstExportRequest rLink = req;
            rLink.sourceKst = linkSrc;
            const auto pl = subkst::planSubKstExport(rLink);
            TEST("Symlink sourceKst refused (O_NOFOLLOW)", !pl.ok);
        }
    }

    std::printf("=== 7. Mid-Failure Recovery & D17 Leftover .INCOMPLETE Handling ===\n");
    {
        subkst::SubKstExportRequest req;
        req.sourceKst = fx.srcKst;
        req.targets = {{SelectedKind::Style, "STYLE/Pop/FactoryBeat.stg", "", ""}};
        const fs::path out = fx.tmpRoot / "RecoverMe.KST";
        const fs::path inc = ai_arranger::media::incompletePath(out);

        // Step 1: Inject a forbidden PCM/ dir right before verify -> verify fails (HOLD VIOLATION!),
        //         leaving RecoverMe.KST.INCOMPLETE on disk and NEVER publishing RecoverMe.KST!
        jobs::JobState stFail; stFail.id = 40;
        jobs::runExport(
            subkst::selectiveSubKstExportSpec(req, out, nullptr, nullptr, [&](const char* phase) {
                if (std::strcmp(phase, "beforeVerify") == 0) {
                    writeFile(inc / out.filename() / "PCM/INJECTED.PCM", "bad");
                }
            }),
            stFail, [] { return false; }, [](const jobs::JobState&) {});
        TEST("Injected forbidden PCM/ fails Phase::Verify & never publishes OUT.KST",
             stFail.phase == jobs::Phase::Failed && stFail.stoppedAt == jobs::Phase::Verify &&
             !fs::exists(out) && fs::is_directory(inc));

        // Step 2: Retry WITHOUT moveLeftoverAside -> refused fail-closed (D17), leftover kept untouched!
        jobs::JobState stRefuse; stRefuse.id = 41;
        jobs::runExport(subkst::selectiveSubKstExportSpec(req, out), stRefuse, [] { return false; }, [](const jobs::JobState&) {});
        TEST("Retry without consent refuses leftover .INCOMPLETE (D17)",
             stRefuse.phase == jobs::Phase::Failed && !fs::exists(out) && fs::is_directory(inc));

        // Step 3: Retry WITH moveLeftoverAside=true -> moves stale leftover aside (never deleted!) and publishes verified OUT.KST!
        subkst::SubKstExportRequest reqRecover = req;
        reqRecover.moveLeftoverAside = true;
        jobs::JobState stOk; stOk.id = 42;
        jobs::runExport(subkst::selectiveSubKstExportSpec(reqRecover, out), stOk, [] { return false; }, [](const jobs::JobState&) {});
        const auto lefts = jobs::leftovers(fx.tmpRoot);
        bool hasStale = false;
        for (const auto& l : lefts) {
            if (l.rfind("RecoverMe.KST.INCOMPLETE.stale-", 0) == 0) hasStale = true;
        }
        TEST("Retry with --move-leftover-aside moves stale leftover aside & publishes verified OUT.KST",
             stOk.phase == jobs::Phase::Done && fs::is_directory(out) && !fs::exists(inc) && hasStale);
        TEST("Recovered OUT.KST has ZERO forbidden native dirs", !hasAnyForbiddenDir(out));
        assemble::Result asmRes;
        TEST("Recovered OUT.KST passes assemble::stagedRefusal", assemble::stagedRefusal(out, asmRes).empty());
    }

    std::printf("=== 8. CLI End-to-End Subprocess Tests (aiarranger-resources kst-native selective-export) ===\n");
    {
        // 8a. Duplicate flags & usage errors -> exit 1
        const fs::path dummyOut = fx.tmpRoot / "CliDummy.KST";
        TEST("CLI rejects duplicate --policy (exit 1)",
             runCli({"kst-native", "selective-export", "--source", fx.srcKst.string(),
                     "--select", "STYLE/Pop/FactoryBeat.stg", "--policy", "block", "--policy", "reuse",
                     "--out", dummyOut.string()}).exitCode == 1);
        TEST("CLI rejects duplicate --source (exit 1)",
             runCli({"kst-native", "selective-export", "--source", fx.srcKst.string(),
                     "--source", fx.srcKst.string(), "--select", "STYLE/Pop/FactoryBeat.stg",
                     "--out", dummyOut.string()}).exitCode == 1);
        TEST("CLI rejects duplicate --out (exit 1)",
             runCli({"kst-native", "selective-export", "--source", fx.srcKst.string(),
                     "--select", "STYLE/Pop/FactoryBeat.stg", "--out", dummyOut.string(),
                     "--out", dummyOut.string()}).exitCode == 1);
        TEST("CLI rejects duplicate --target-bank for same --select (exit 1)",
             runCli({"kst-native", "selective-export", "--source", fx.srcKst.string(),
                     "--select", "STYLE/Pop/FactoryBeat.stg", "--target-bank", "A", "--target-bank", "B",
                     "--out", dummyOut.string()}).exitCode == 1);
        TEST("CLI rejects output folder not ending in .KST (exit 1)",
             runCli({"kst-native", "selective-export", "--source", fx.srcKst.string(),
                     "--select", "STYLE/Pop/FactoryBeat.stg",
                     "--out", (fx.tmpRoot / "BadNameNoSuffix").string()}).exitCode == 1);

        // 8b. Factory-only CLI export --json -> exit 0, pure JSON
        const fs::path cliFactOut = fx.tmpRoot / "CliFactory.KST";
        const auto rFact = runCli({"kst-native", "selective-export",
                                   "--source", fx.srcKst.string(),
                                   "--select", "STYLE/Pop/FactoryBeat.stg",
                                   "--select", "PAD/Pop/FactoryPad.pdc",
                                   "--out", cliFactOut.string(),
                                   "--json"});
        kit::Json jFact;
        TEST("CLI Factory-only export returns exit 0 & pure JSON",
             rFact.exitCode == 0 && kit::parseJson(rFact.stdoutText, &jFact).empty() &&
             rFact.stdoutText.find("\"published\": true") != std::string::npos &&
             rFact.stdoutText.find("\"hardwareVerified\": false") != std::string::npos &&
             rFact.stdoutText.find("\"deleteProofAvailable\": false") != std::string::npos &&
             rFact.stdoutText.find(fx.tmpRoot.string()) == std::string::npos &&
             fs::is_directory(cliFactOut) && !hasAnyForbiddenDir(cliFactOut));

        // 8c. Cross-slot --policy reuse --json -> exit 0, remapped .stg verified
        const fs::path cliRemapOut = fx.tmpRoot / "CliRemap.KST";
        const auto rRemap = runCli({"kst-native", "selective-export",
                                    "--source", fx.srcKst.string(),
                                    "--target", fx.tgtKst.string(),
                                    "--select", "STYLE/Pop/RemapUser.stg",
                                    "--policy", "reuse",
                                    "--out", cliRemapOut.string(),
                                    "--json"});
        kit::Json jRemap;
        TEST("CLI cross-slot --policy reuse returns exit 0, pure JSON & SONGBOOK_UNREWRITTEN",
             rRemap.exitCode == 0 && kit::parseJson(rRemap.stdoutText, &jRemap).empty() &&
             rRemap.stdoutText.find("USER04/12") != std::string::npos &&
             rRemap.stdoutText.find(diag_code::kSongBookUnrewritten) != std::string::npos &&
             fs::is_regular_file(cliRemapOut / "STYLE/Pop/RemapUser.stg"));

        // 8d. Blocked HOLD (User Sound absent in target) -> exit 3, pure JSON, NOTHING written!
        const fs::path cliBlockedOut = fx.tmpRoot / "CliBlocked.KST";
        const auto rBlocked = runCli({"kst-native", "selective-export",
                                      "--source", fx.srcKst.string(),
                                      "--select", "STYLE/Pop/ExactUser.stg",
                                      "--out", cliBlockedOut.string(),
                                      "--json"});
        kit::Json jBlocked;
        TEST("CLI blocked HOLD returns exit 3, pure JSON & writes nothing",
             rBlocked.exitCode == 3 && kit::parseJson(rBlocked.stdoutText, &jBlocked).empty() &&
             rBlocked.stdoutText.find(diag_code::kNativeSoundWriteHold) != std::string::npos &&
             !fs::exists(cliBlockedOut) && !fs::exists(ai_arranger::media::incompletePath(cliBlockedOut)));

        // 8e. Malformed MIDI leaf -> exit 3, nothing written
        const fs::path cliBadMidiOut = fx.tmpRoot / "CliBadMidi.KST";
        const auto rBadMidi = runCli({"kst-native", "selective-export",
                                      "--source", fx.srcKst.string(),
                                      "--select", "PAD/Pop/BadHighBit.pdc",
                                      "--out", cliBadMidiOut.string(),
                                      "--json"});
        TEST("CLI malformed MIDI leaf returns exit 3 & writes nothing",
             rBadMidi.exitCode == 3 &&
             rBadMidi.stdoutText.find(diag_code::kMalformedMidiLeaf) != std::string::npos &&
             !fs::exists(cliBadMidiOut));

        // 8f. OC31 target -> exit 3, nothing written
        const fs::path cliOc31Out = fx.tmpRoot / "CliOc31.KST";
        const auto rOc31 = runCli({"kst-native", "selective-export",
                                   "--source", fx.srcKst.string(),
                                   "--target", fx.oc31TgtKst.string(),
                                   "--select", "STYLE/Pop/ExactUser.stg",
                                   "--policy", "reuse",
                                   "--out", cliOc31Out.string(),
                                   "--json"});
        TEST("CLI OC31 target returns exit 3 & writes nothing",
             rOc31.exitCode == 3 &&
             rOc31.stdoutText.find(diag_code::kTargetUninspectable) != std::string::npos &&
             !fs::exists(cliOc31Out));

        // 8g. Mismatched --expect-fingerprint -> exit 1, nothing written
        const fs::path cliBadFpOut = fx.tmpRoot / "CliBadFp.KST";
        const auto rBadFp = runCli({"kst-native", "selective-export",
                                    "--source", fx.srcKst.string(),
                                    "--select", "STYLE/Pop/FactoryBeat.stg",
                                    "--expect-fingerprint", std::string(64, '0'),
                                    "--out", cliBadFpOut.string()});
        TEST("CLI mismatched --expect-fingerprint returns exit 1 & writes nothing",
             rBadFp.exitCode == 1 && !fs::exists(cliBadFpOut));

        // 8h. CLI --move-leftover-aside recovery
        const fs::path cliRecOut = fx.tmpRoot / "CliRecover.KST";
        const fs::path cliRecInc = ai_arranger::media::incompletePath(cliRecOut);
        fs::create_directories(cliRecInc);
        // Write valid job marker matching selectiveSubKstExportSpec()
        {
            const fs::path destAbs = ai_arranger::media::folderPath(fs::absolute(cliRecOut));
            writeFile(cliRecInc / jobs::kJobMarker,
                      "export job 1 Selective Root-Tree Sub-KST (EXPERIMENTAL, NOT VERIFIED ON A PA5X) -> " +
                          destAbs.string() + "\n");
        }
        TEST("CLI refuses leftover .INCOMPLETE without --move-leftover-aside (exit 1)",
             runCli({"kst-native", "selective-export",
                     "--source", fx.srcKst.string(),
                     "--select", "STYLE/Pop/FactoryBeat.stg",
                     "--out", cliRecOut.string()}).exitCode == 1 && !fs::exists(cliRecOut));
        TEST("CLI recovers leftover .INCOMPLETE with --move-leftover-aside (exit 0)",
             runCli({"kst-native", "selective-export",
                     "--source", fx.srcKst.string(),
                     "--select", "STYLE/Pop/FactoryBeat.stg",
                     "--out", cliRecOut.string(),
                     "--move-leftover-aside"}).exitCode == 0 && fs::is_directory(cliRecOut));
    }

    std::printf("=== 9. Hardening Regression Tests (P2-1, P2-2, P3-2, P3-3, P3-4) ===\n");
    {
        // P2-1: Invalid --approve-remap for an exact-matched Sound (USER01/5 -> USER04/12, different Sound)
        // must synchronize dryRunPlan.executable == false, report.executable() == false, and canonicalize diagnostics!
        subkst::SubKstExportRequest reqBadRemap;
        reqBadRemap.sourceKst = fx.srcKst;
        reqBadRemap.targetKst = fx.tgtKst;
        reqBadRemap.policy = ConflictPolicy::ReuseIdenticalTargetSlot;
        reqBadRemap.targets = {{SelectedKind::Style, "STYLE/Pop/ExactUser.stg", "", ""}};
        ResourceId src5, dst12;
        TEST("Parse slot IDs for P2-1 test",
             subkst::parseSlotOrResourceId("USER01/5", src5) &&
             subkst::parseSlotOrResourceId("USER04/12", dst12));
        reqBadRemap.approvedSoundRemaps[src5] = dst12;
        const auto plBadRemap = subkst::planSubKstExport(reqBadRemap);
        const bool diagsSorted = std::is_sorted(
            plBadRemap.planResult.report.diagnostics.begin(),
            plBadRemap.planResult.report.diagnostics.end());
        TEST("P2-1: invalid --approve-remap fails closed & keeps dryRunPlan/report executable in sync",
             plBadRemap.ok && !plBadRemap.executable() &&
             !plBadRemap.planResult.dryRunPlan.executable &&
             !plBadRemap.planResult.report.executable() &&
             plBadRemap.planResult.dryRunPlan.hasBlockingCode(diag_code::kNativeSoundWriteHold) &&
             diagsSorted);
        const fs::path badRemapOut = fx.tmpRoot / "P21BadRemap.KST";
        const auto rBadRemap = runCli({"kst-native", "selective-export",
                                       "--source", fx.srcKst.string(),
                                       "--target", fx.tgtKst.string(),
                                       "--select", "STYLE/Pop/ExactUser.stg",
                                       "--policy", "reuse",
                                       "--approve-remap", "USER01/5=USER04/12",
                                       "--out", badRemapOut.string(),
                                       "--json"});
        TEST("P2-1 CLI: invalid --approve-remap exits 3 & writes nothing",
             rBadRemap.exitCode == 3 && !fs::exists(badRemapOut) &&
             !fs::exists(ai_arranger::media::incompletePath(badRemapOut)));

        // P2-2: Cross-type --target-bank redirect (Style -> KEYBOARDSET/Pop/Fake.ksc) refused at PLAN TIME!
        subkst::SubKstExportRequest reqCrossType;
        reqCrossType.sourceKst = fx.srcKst;
        reqCrossType.targets = {{SelectedKind::Style, "STYLE/Pop/FactoryBeat.stg", "KEYBOARDSET/Pop", "Fake.ksc"}};
        const auto plCrossType = subkst::planSubKstExport(reqCrossType);
        TEST("P2-2: cross-type Style -> KEYBOARDSET/Pop/Fake.ksc refused at plan time",
             plCrossType.ok && !plCrossType.executable() &&
             !plCrossType.planResult.dryRunPlan.executable &&
             !plCrossType.planResult.report.executable());
        const fs::path crossOut = fx.tmpRoot / "P22CrossType.KST";
        const auto rCross = runCli({"kst-native", "selective-export",
                                    "--source", fx.srcKst.string(),
                                    "--select", "STYLE/Pop/FactoryBeat.stg",
                                    "--target-bank", "KEYBOARDSET/Pop",
                                    "--target-name", "Fake.ksc",
                                    "--out", crossOut.string(),
                                    "--json"});
        TEST("P2-2 CLI: cross-type redirect exits 3 with zero output & zero .INCOMPLETE",
             rCross.exitCode == 3 && !fs::exists(crossOut) &&
             !fs::exists(ai_arranger::media::incompletePath(crossOut)));

        // P3-2: outSummary->published stays false if publish fails after beforePublish
        {
            subkst::SubKstExportRequest rP32;
            rP32.sourceKst = fx.srcKst;
            rP32.targets = {{SelectedKind::Style, "STYLE/Pop/FactoryBeat.stg", "", ""}};
            const fs::path outP32 = fx.tmpRoot / "P32NoPremature.KST";
            auto sumP32 = std::make_shared<subkst::SubKstExecutionSummary>();
            bool sawPublishedInBeforePublish = false;
            jobs::JobState stP32; stP32.id = 50;
            jobs::runExport(
                subkst::selectiveSubKstExportSpec(rP32, outP32, nullptr, sumP32, [&](const char* phase) {
                    if (std::strcmp(phase, "beforePublish") == 0) {
                        sawPublishedInBeforePublish = sumP32->published;
                        // Create dest concurrently so renameAtNoReplace fails after beforePublish returns ""!
                        fs::create_directory(outP32);
                    }
                }),
                stP32, [] { return false; }, [](const jobs::JobState&) {});
            TEST("P3-2: outSummary->published is NOT set prematurely before renameAtNoReplace",
                 !sawPublishedInBeforePublish && stP32.phase == jobs::Phase::Failed && !sumP32->published);
        }

        // P3-3: Markdown serializer safely escapes backticks inside code spans via mdCodeSpan
        {
            subkst::SubKstExportPlan dummyPlan;
            dummyPlan.planFingerprint = std::string(64, 'a');
            subkst::SubKstExecutionSummary dummyExec;
            dummyExec.outputLeafName = "Weird`Name.KST";
            dummyExec.exportedFiles.push_back({"STYLE/A/x.stg", "STYLE/A`B/x.stg", std::string(64, 'b'), 42, false, 0});
            jobs::JobState dummyJob;
            dummyJob.phase = jobs::Phase::Done;
            dummyJob.level.l1 = {jobs::Check::Passed, "ok"};
            const std::string md = subkst::serializeSubKstExportMarkdown(dummyPlan, dummyExec, dummyJob);
            TEST("P3-3: Markdown code spans escape backticks via mdCodeSpan",
                 md.find("`Weird\\x60Name.KST`") != std::string::npos &&
                 md.find("`STYLE/A\\x60B/x.stg`") != std::string::npos);
        }

        // P3-4: Dangling symlink at --out refused cleanly BEFORE staging (zero .INCOMPLETE created!)
        {
            const fs::path danglingOut = fx.tmpRoot / "DanglingOut.KST";
            const fs::path danglingInc = ai_arranger::media::incompletePath(danglingOut);
            fs::create_directory_symlink(fx.tmpRoot / "does_not_exist_target", danglingOut);
            subkst::SubKstExportRequest rDang;
            rDang.sourceKst = fx.srcKst;
            rDang.targets = {{SelectedKind::Style, "STYLE/Pop/FactoryBeat.stg", "", ""}};
            jobs::JobState stDang; stDang.id = 51;
            jobs::runExport(subkst::selectiveSubKstExportSpec(rDang, danglingOut), stDang, [] { return false; }, [](const jobs::JobState&) {});
            TEST("P3-4: dangling symlink at --out refused at Phase::Validate with zero .INCOMPLETE",
                 stDang.phase == jobs::Phase::Failed && stDang.stoppedAt == jobs::Phase::Validate &&
                 !fs::exists(danglingInc) && fs::is_symlink(fs::symlink_status(danglingOut)));
            const auto rDangCli = runCli({"kst-native", "selective-export",
                                          "--source", fx.srcKst.string(),
                                          "--select", "STYLE/Pop/FactoryBeat.stg",
                                          "--out", danglingOut.string()});
            TEST("P3-4 CLI: dangling symlink at --out returns exit 1 with zero .INCOMPLETE",
                 rDangCli.exitCode == 1 && !fs::exists(danglingInc) &&
                 fs::is_symlink(fs::symlink_status(danglingOut)));
        }
    }

    // Final invariant: source & target KSTs 100% byte-identical before & after all tests!
    TEST("Source KST 100% unchanged after entire test suite", dirDigest(fx.srcKst) == srcDigestBefore);
    TEST("Target KST 100% unchanged after entire test suite", dirDigest(fx.tgtKst) == tgtDigestBefore);

    std::printf("\nResult: %d passed, %d failed\n", gPasses, gFailures);
    return gFailures == 0 ? 0 : 1;
}
