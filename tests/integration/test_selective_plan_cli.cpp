// Slice M2d Synthetic Integration & CLI Tests:
//   - `src/media/sound/selective_plan.h` (Native-KST selective advisory export planner + export_job.h staging)
//   - `src/library/library_selective_preview.h` (zero-breaking-change library::evaluateWithDependencies /
//     planJsonWithDependencies / whereUsedWithManifest enrichment)
//   - `src/media/sound/native_kst_closure.h` (UX Gate #4 cooperative keepGoing cancellation)
//   - `tools/aiarranger_resources.cpp` (`kst-native selective-plan` CLI end-to-end)
//
// 100% synthetic, original fixtures only (zero owner files, proprietary bytes, real names, or local paths).
// Strictly enforces all active HOLD gates and all 4 Phase-2 UX Gates (UX-1..UX-4).

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "jobs/import_kit.h"
#include "library/library_selective_preview.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "media/panative/root_tree.h"
#include "media/sound/selective_plan.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
namespace lib = ai_arranger::library;
namespace jobs = ai_arranger::jobs;
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
    } else {
        // Chunked literals for larger bodies
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

static std::string makeSmf(const std::vector<SmfSelSpec>& sels) {
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

static void writeBytesFile(const fs::path& p, const Bytes& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

static std::string readTextFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static std::map<std::string, std::string> snapshotDirHashes(const fs::path& root) {
    std::map<std::string, std::string> out;
    ai_arranger::media::detail::Listing ls;
    std::string err;
    if (!ai_arranger::media::detail::listFiles(root, ls, err)) return out;
    for (const auto& [rel, e] : ls.files) {
        bool cancelled = false;
        std::string serr;
        out[rel] = ai_arranger::media::detail::sha256File(e.abs, {}, &cancelled, &serr);
    }
    return out;
}

// Subprocess runner using fork/execv + pipes (zero shell expansion, captures exit code + stdout + stderr).
struct CliRunResult {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

static CliRunResult runCli(const std::vector<std::string>& args) {
    CliRunResult r;
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
        if (outPipe[0] >= 0) { ::close(outPipe[0]); ::close(outPipe[1]); }
        if (errPipe[0] >= 0) { ::close(errPipe[0]); ::close(errPipe[1]); }
        return r;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(outPipe[0]); ::close(outPipe[1]);
        ::close(errPipe[0]); ::close(errPipe[1]);
        return r;
    }
    if (pid == 0) {
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        ::close(outPipe[0]); ::close(outPipe[1]);
        ::close(errPipe[0]); ::close(errPipe[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }

    ::close(outPipe[1]);
    ::close(errPipe[1]);

    auto drain = [](int fd, std::string& dst) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n > 0) dst.append(buf, static_cast<size_t>(n));
            else break;
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

// ── Synthetic KST Fixtures Builder ───────────────────────────────────────────────────────────────────────────────────

struct SyntheticFixtures {
    fs::path tmpRoot;
    fs::path sourceKst;
    fs::path targetKst;
    fs::path oc31Kst;
    fs::path symlinkSource;

    SyntheticFixtures() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tmpRoot = fs::temp_directory_path() / ("aia_m2d_test_" + std::to_string(stamp) + "_" + std::to_string(::getpid()));
        sourceKst = tmpRoot / "SynthSource.KST";
        targetKst = tmpRoot / "SynthTarget.KST";
        oc31Kst = tmpRoot / "SynthOc31.KST";
        symlinkSource = tmpRoot / "SymlinkSource.KST";
        fs::create_directories(tmpRoot);
        buildSourceKst();
        buildTargetKst();
        buildOc31Kst();
        fs::create_directory_symlink(sourceKst, symlinkSource);
    }

    ~SyntheticFixtures() {
        std::error_code ec;
        fs::remove_all(tmpRoot, ec);
    }

    static constexpr uint32_t kSoundKind = 0x10050038u;
    static RecordLayout synthLayout() {
        return RecordLayout{12, 24, 8};
    }

    void buildSourceKst() {
        const RecordLayout L = synthLayout();
        const Id8 smp0 = makeId(0x10, 0);
        const Id8 smp1 = makeId(0x10, 1);
        const Id8 smp2 = makeId(0x10, 2);
        const Id8 dupDrumId = makeId(0x20, 1);
        const Id8 ms0 = makeId(0x30, 0);
        const Id8 ms1 = makeId(0x30, 1);
        const Id8 unknownRomMs = makeId(0x99, 1);
        const Id8 specRomMs1 = makeId(0x99, 2);
        const Id8 specRomMs2 = makeId(0x99, 3);

        // KMP with 3 Samples, 2 DrumSamples sharing duplicate Id8, 2 Multisamples sharing smp1
        SynthKmpSpec kmp;
        kmp.samples = {
            {smp0, "SynthSmp0", 48000},
            {smp1, "SynthSmp1", 48000},
            {smp2, "SynthSmp2", 44100}
        };
        kmp.drums = {
            {dupDrumId, "DupKickA", 0, kNoZoneSample},
            {dupDrumId, "DupKickB", 1, kNoZoneSample}
        };
        kmp.zones = {
            {0, 60}, {1, 127},  // ms0: zones 0..1 -> smp0, smp1
            {1, 72}, {2, 127}   // ms1: zones 2..3 -> smp1 (shared!), smp2
        };
        kmp.multis = {
            {ms0, "SynthMS0", 0, 2},
            {ms1, "SynthMS1", 2, 2}
        };
        const Bytes kmpBytes = buildKorfContainer({
            {0x11080018u, 0x11, 0, 0, "RAM", buildPa5xKmpPayload(kmp), ""}
        });
        writeBytesFile(sourceKst / "MULTISMP" / "RAM.KMP", kmpBytes);

        // PCM container matching all 3 samples (small < 4GB to verify UX-3 still says "unverified-proxy"!)
        const Bytes pcmBytes = buildKorfContainer({
            {0x01100018u, 0x01, 0, 0, "SynthSmp0", Bytes(4096, 0x11), idAsStr(smp0)},
            {0x01800018u, 0x01, 0, 1, "SynthSmp1", Bytes(8192, 0x22), idAsStr(smp1)},
            {0x01100018u, 0x01, 0, 2, "SynthSmp2", Bytes(2048, 0x33), idAsStr(smp2)}
        });
        writeBytesFile(sourceKst / "PCM" / "ram01.pcm", pcmBytes);

        // SOUND/USER01.PCG:
        //  slot 0: Analysed SynthLead0 -> ms0 (body fill 0x5A)
        //  slot 1: Analysed SynthPad1  -> ms0 + ms1 (body fill 0x5B)
        //  slot 2: Analysed SynthBass2 -> ms1 (body fill 0x5C)
        //  slot 3: Analysed with 1 unresolved non-RAM slot (inUnanalysedSound == false!) -> ms0 + unknownRomMs
        //  slot 4: NoKnownReference Sound with 2 speculative non-RAM slots (inUnanalysedSound == true!)
        const Bytes s0 = buildSoundBody(L, {{ms0, kZeroId}}, 0x5A);
        const Bytes s1 = buildSoundBody(L, {{ms0, ms1}}, 0x5B);
        const Bytes s2 = buildSoundBody(L, {{ms1, kZeroId}}, 0x5C);
        const Bytes s3 = buildSoundBody(L, {{ms0, unknownRomMs}}, 0x5D);
        const Bytes s4 = buildSoundBody(L, {{specRomMs1, specRomMs2}}, 0x5E);
        // Add 5 more melodic sounds (slots 5..9) so default fitLayout() (fitMinItems == 8) fits RecordLayout{12,24,8}
        const Bytes sFit = buildSoundBody(L, {{ms0, ms1}}, 0x5F);
        const Bytes user01Pcg = buildKorfContainer({
            {kSoundKind, 0x10, 0, 0, "SynthLead0", makeOcLiteralFrame("OC31", s0), ""},
            {kSoundKind, 0x10, 0, 1, "SynthPad1", makeOcLiteralFrame("OC31", s1), ""},
            {kSoundKind, 0x10, 0, 2, "SynthBass2", makeOcLiteralFrame("OC31", s2), ""},
            {kSoundKind, 0x10, 0, 3, "SynthHybrid3", makeOcLiteralFrame("OC31", s3), ""},
            {kSoundKind, 0x10, 0, 4, "SynthRomOnly4", makeOcLiteralFrame("OC31", s4), ""},
            {kSoundKind, 0x10, 0, 5, "SynthFit5", makeOcLiteralFrame("OC31", sFit), ""},
            {kSoundKind, 0x10, 0, 6, "SynthFit6", makeOcLiteralFrame("OC31", sFit), ""},
            {kSoundKind, 0x10, 0, 7, "SynthFit7", makeOcLiteralFrame("OC31", sFit), ""},
            {kSoundKind, 0x10, 0, 8, "SynthFit8", makeOcLiteralFrame("OC31", sFit), ""},
            {kSoundKind, 0x10, 0, 9, "SynthFit9", makeOcLiteralFrame("OC31", sFit), ""}
        });
        writeBytesFile(sourceKst / "SOUND" / "USER01.PCG", user01Pcg);

        // SOUND/USERDK01.PCG: slot 0 unparsed DrumKit (size does not fit melodic layout)
        const Bytes dkBody(77, 0x44);
        const Bytes userDk01 = buildKorfContainer({
            {kSoundKind, 0x10, 0, 0, "SynthDrumKit0", makeOcLiteralFrame("OC31", dkBody), ""}
        });
        writeBytesFile(sourceKst / "SOUND" / "USERDK01.PCG", userDk01);

        // STYLE/Pop/CleanStyle.stg: main track uses USER01/0 + Factory 121/0/33;
        // embedded KbdSet uses USER01/1; embedded Pad uses USER01/2
        const Bytes cleanStyle = rtSerialize({
            rtBranch("Var1.scn", {
                rtBranch("Track.pdc", {
                    rtLeaf("Main.mid", makeSmf({{8, 121, 64, 0}, {11, 121, 0, 33}}))
                })
            }),
            rtBranch("KbdSet1.scn", {
                rtBranch("Part.ksc", {
                    rtLeaf("Main.mid", makeSmf({{0, 121, 64, 1}}))
                })
            }),
            rtBranch("PadSet1.scn", {
                rtBranch("Pad1.pdc", {
                    rtLeaf("Main.mid", makeSmf({{4, 121, 64, 2}}))
                })
            })
        });
        writeBytesFile(sourceKst / "STYLE" / "Pop" / "CleanStyle.stg", cleanStyle);

        // STYLE/Pop/SharedStyle.stg: shares USER01/0 + USER01/1 (exercises Tier-1 dedup!)
        const Bytes sharedStyle = rtSerialize({
            rtBranch("Var1.scn", {
                rtBranch("Track.pdc", {
                    rtLeaf("Main.mid", makeSmf({{8, 121, 64, 0}, {9, 121, 64, 1}}))
                })
            })
        });
        writeBytesFile(sourceKst / "STYLE" / "Pop" / "SharedStyle.stg", sharedStyle);

        // STYLE/Pop/DanglingStyle.stg: references dangling USER12/99 + unparsed USERDK01/0 + USER01/3 + USER01/4
        const Bytes danglingStyle = rtSerialize({
            rtBranch("Var1.scn", {
                rtBranch("Track.pdc", {
                    rtLeaf("Main.mid", makeSmf({
                        {8, 121, 75, 99},  // dangling USER12/99 (bank absent!)
                        {9, 120, 64, 0},   // USERDK01/0 (unparsed DK)
                        {11, 121, 64, 3},  // analysed with 1 unresolved slot
                        {12, 121, 64, 4}   // NoKnownReference with 2 speculative slots
                    }))
                })
            })
        });
        writeBytesFile(sourceKst / "STYLE" / "Pop" / "DanglingStyle.stg", danglingStyle);

        // KEYBOARDSET/Lead/SoloKbd.ksc: references USER01/1
        const Bytes kbdSet = rtSerialize({
            rtBranch("Main.ssn", {
                rtBranch("RTTracks.scn", {
                    rtBranch("Upper1.plc", {
                        rtLeaf("Main.mid", makeSmf({{0, 121, 64, 1}}))
                    })
                })
            })
        });
        writeBytesFile(sourceKst / "KEYBOARDSET" / "Lead" / "SoloKbd.ksc", kbdSet);

        // PAD/Perc/BellPad.pdc: references USER01/2
        const Bytes padObj = rtSerialize({
            rtLeaf("Main.mid", makeSmf({{0, 121, 64, 2}}))
        });
        writeBytesFile(sourceKst / "PAD" / "Perc" / "BellPad.pdc", padObj);
    }

    void buildTargetKst() {
        const RecordLayout L = synthLayout();
        const Id8 smp0 = makeId(0x10, 0);
        const Id8 smp1 = makeId(0x10, 1);
        const Id8 smp2 = makeId(0x10, 2);
        const Id8 ms0 = makeId(0x30, 0);
        const Id8 ms1 = makeId(0x30, 1);

        SynthKmpSpec kmp;
        kmp.samples = {
            {smp0, "SynthSmp0", 48000},
            {smp1, "SynthSmp1", 48000},
            {smp2, "SynthSmp2", 44100}
        };
        kmp.zones = {
            {0, 60}, {1, 127},
            {1, 72}, {2, 127}
        };
        kmp.multis = {
            {ms0, "SynthMS0", 0, 2},
            {ms1, "SynthMS1", 2, 2}
        };
        writeBytesFile(targetKst / "MULTISMP" / "RAM.KMP",
                       buildKorfContainer({{0x11080018u, 0x11, 0, 0, "RAM", buildPa5xKmpPayload(kmp), ""}}));
        writeBytesFile(targetKst / "PCM" / "ram01.pcm",
                       buildKorfContainer({
                           {0x01100018u, 0x01, 0, 0, "SynthSmp0", Bytes(4096, 0x11), idAsStr(smp0)},
                           {0x01800018u, 0x01, 0, 1, "SynthSmp1", Bytes(8192, 0x22), idAsStr(smp1)},
                           {0x01100018u, 0x01, 0, 2, "SynthSmp2", Bytes(2048, 0x33), idAsStr(smp2)}
                       }));

        // Target SOUND/USER01.PCG:
        //  slot 0: DIFFERENT Sound (even with same name "SynthLead0" -> must conflict!)
        //  slot 1: PROVEN-IDENTICAL to source USER01/1 -> reuse-target!
        //  slot 2: DIFFERENT Sound (while proven-identical copy of source USER01/2 lives at USER02/5!)
        const Bytes s0Diff = buildSoundBody(L, {{ms0, kZeroId}}, 0x6A);
        const Bytes s1Same = buildSoundBody(L, {{ms0, ms1}}, 0x5B);
        const Bytes s2Diff = buildSoundBody(L, {{ms1, kZeroId}}, 0x6C);
        const Bytes s2Same = buildSoundBody(L, {{ms1, kZeroId}}, 0x5C);

        const Bytes sFitT = buildSoundBody(L, {{ms0, ms1}}, 0x6F);
        writeBytesFile(targetKst / "SOUND" / "USER01.PCG",
                       buildKorfContainer({
                           {kSoundKind, 0x10, 0, 0, "SynthLead0", makeOcLiteralFrame("OC31", s0Diff), ""},
                           {kSoundKind, 0x10, 0, 1, "SynthPad1", makeOcLiteralFrame("OC31", s1Same), ""},
                           {kSoundKind, 0x10, 0, 2, "TargetOther2", makeOcLiteralFrame("OC31", s2Diff), ""},
                           {kSoundKind, 0x10, 0, 5, "TargetFit5", makeOcLiteralFrame("OC31", sFitT), ""},
                           {kSoundKind, 0x10, 0, 6, "TargetFit6", makeOcLiteralFrame("OC31", sFitT), ""},
                           {kSoundKind, 0x10, 0, 7, "TargetFit7", makeOcLiteralFrame("OC31", sFitT), ""},
                           {kSoundKind, 0x10, 0, 8, "TargetFit8", makeOcLiteralFrame("OC31", sFitT), ""},
                           {kSoundKind, 0x10, 0, 9, "TargetFit9", makeOcLiteralFrame("OC31", sFitT), ""}
                       }));
        writeBytesFile(targetKst / "SOUND" / "USER02.PCG",
                       buildKorfContainer({
                           {kSoundKind, 0x10, 0, 5, "RelocatedBass", makeOcLiteralFrame("OC31", s2Same), ""}
                       }));

        // Target STYLE/Pop/ExistingOther.stg (unrelated existing file)
        const Bytes otherStyle = rtSerialize({
            rtBranch("Var1.scn", {rtBranch("Track.pdc", {rtLeaf("Main.mid", makeSmf({{8, 121, 0, 1}}))})})
        });
        writeBytesFile(targetKst / "STYLE" / "Pop" / "ExistingOther.stg", otherStyle);
    }

    void buildOc31Kst() {
        const RecordLayout L = synthLayout();
        const Id8 smp0 = makeId(0x40, 0);
        const Id8 ms0 = makeId(0x41, 0);

        // OC31-compressed legacy RAM.KMP (chunk ID 0x11050038 with OC31 header!)
        const Bytes fakeOc31Kmp = makeOcLiteralFrame("OC31", Bytes(64, 0xAA));
        writeBytesFile(oc31Kst / "MULTISMP" / "RAM.KMP",
                       buildKorfContainer({{0x11050038u, 0x11, 0, 0, "RAM", fakeOc31Kmp, ""}}));
        writeBytesFile(oc31Kst / "PCM" / "ram01.pcm",
                       buildKorfContainer({
                           {0x01100018u, 0x01, 0, 0, "LegacySmp0", Bytes(2048, 0x55), idAsStr(smp0)}
                       }));
        const Bytes s0 = buildSoundBody(L, {{ms0, kZeroId}}, 0x7A);
        writeBytesFile(oc31Kst / "SOUND" / "USER01.PCG",
                       buildKorfContainer({
                           {kSoundKind, 0x10, 0, 0, "LegacySound0", makeOcLiteralFrame("OC31", s0), ""},
                           {kSoundKind, 0x10, 0, 1, "LegacySound1", makeOcLiteralFrame("OC31", s0), ""}
                       }));
        const Bytes legacyStyle = rtSerialize({
            rtBranch("Var1.scn", {rtBranch("Track.pdc", {rtLeaf("Main.mid", makeSmf({{8, 121, 64, 0}}))})})
        });
        writeBytesFile(oc31Kst / "STYLE" / "Legacy" / "OldStyle.stg", legacyStyle);
    }
};

static SelectionTarget selTarget(SelectedKind k, const std::string& rel,
                                 const std::string& bank = "", const std::string& name = "") {
    return SelectionTarget{k, rel, bank, name};
}

static bool parseStrictJson(const std::string& text, kit::Json& out) {
    return kit::parseJson(text, &out).empty() && out.t == kit::Json::T::Obj;
}
static const kit::Json* jField(const kit::Json* obj, const std::string& k) {
    return (obj && obj->t == kit::Json::T::Obj) ? obj->get(k) : nullptr;
}
static const kit::Json* jField2(const kit::Json& obj, const std::string& k1, const std::string& k2) {
    return jField(obj.get(k1), k2);
}
static bool jBoolEq(const kit::Json* v, bool want) {
    return v && v->t == kit::Json::T::Bool && v->b == want;
}
static bool jStrEq(const kit::Json* v, const std::string& want) {
    return v && v->t == kit::Json::T::Str && v->s == want;
}
static bool jNumEq(const kit::Json* v, const std::string& want) {
    return v && v->t == kit::Json::T::Num && v->s == want;
}
static long long jNumVal(const kit::Json* v) {
    return (v && v->t == kit::Json::T::Num) ? std::stoll(v->s) : -1;
}

int main() {
    std::printf("=== M2d Native-KST Selective Advisory Planner, Library Preview & CLI Tests ===\n");
    const SyntheticFixtures fx;
    const auto origSourceHashes = snapshotDirHashes(fx.sourceKst);
    const auto origTargetHashes = snapshotDirHashes(fx.targetKst);
    const auto origOc31Hashes = snapshotDirHashes(fx.oc31Kst);

    // ── 1. UX Gate #1 (Honest Coverage & UnresolvedSlot Split) & UX Gate #3 (Stored-PCM Proxy Caveat) ────────────────
    {
        NativeSelectivePlanRequest req;
        req.sourceKst = fx.sourceKst;
        req.targets = {
            selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg"),
            selTarget(SelectedKind::Style, "STYLE/Pop/SharedStyle.stg")
        };
        const auto res = planNativeSelectiveExport(req);
        TEST("clean multi-style plan ok & executable", res.ok && !res.cancelled && res.executable());
        TEST("UX-1 deleteProofAvailable is false", !res.report.coverage.deleteProofAvailable);
        TEST("UX-1 unresolvedInAnalysedSounds != unresolvedInUnanalysedSounds (1 vs 2)",
             res.report.coverage.unresolvedInAnalysedSounds == 1 &&
             res.report.coverage.unresolvedInUnanalysedSounds == 2 &&
             res.report.coverage.unresolvedInAnalysedSounds != res.report.coverage.unresolvedInUnanalysedSounds);
        TEST("UX-1 partial coverage notice rendered due to USERDK & dupDrumIds",
             !res.report.coverage.coverageComplete);
        TEST("UX-3 native stored PCM proxy flag true even < 4GB",
             res.report.memoryProjection.sampleBytesAreNativeStoredProxy &&
             std::string(res.report.memoryProjection.unitName()) == "native_kst_stored_pcm_proxy_bytes" &&
             std::string(res.report.memoryProjection.fitVerdictName()) == "unverified-proxy" &&
             std::string(res.report.memoryProjection.caveatText()) == kStoredPcmProxyCaveat);
        TEST("Tier-1 logical dedup saved bytes > 0 across shared styles",
             res.report.memoryProjection.logicalDedupSavedBytes > 0 &&
             res.report.memoryProjection.uniqueLogicalSampleBytes == 4096 + 8192 + 2048);
        TEST("MAN5 p.736 single-Sound steps generated for 3 User Sounds",
             res.pa5xSingleSoundSteps.size() == 3);

        // Determinism under shuffled target ordering
        NativeSelectivePlanRequest reqShuffled = req;
        std::reverse(reqShuffled.targets.begin(), reqShuffled.targets.end());
        const auto res2 = planNativeSelectiveExport(reqShuffled);
        TEST("deterministic planFingerprint across shuffled --select order",
             res2.ok && res2.planFingerprint == res.planFingerprint && res.planFingerprint.size() == 64);
        const std::string json1 = serializeNativeSelectivePlanJson(res);
        const std::string json2 = serializeNativeSelectivePlanJson(res2);
        const std::string md1 = serializeNativeSelectivePlanMarkdown(res);
        const std::string md2 = serializeNativeSelectivePlanMarkdown(res2);
        TEST("deterministic byte-identical JSON & Markdown across shuffled order",
             json1 == json2 && md1 == md2);
        TEST("zero local tmpRoot path leaked in JSON or Markdown",
             json1.find(fx.tmpRoot.string()) == std::string::npos &&
             md1.find(fx.tmpRoot.string()) == std::string::npos);

        // Strict RFC 8259 JSON parse check via kit::parseJson
        kit::Json parsed;
        TEST("selective-plan JSON parses strictly via kit::parseJson()", parseStrictJson(json1, parsed));
        TEST("JSON enforces hardwareVerified:false, deleteProofAvailable:false, fitVerdict:unverified-proxy",
             jBoolEq(parsed.get("hardwareVerified"), false) &&
             jBoolEq(jField2(parsed, "source", "deleteProofAvailable"), false) &&
             jBoolEq(jField2(parsed, "coverage", "deleteProofAvailable"), false) &&
             jStrEq(jField2(parsed, "coverage", "notice"), kPartialCoverageNotice) &&
             jStrEq(jField2(parsed, "memoryProjection", "fitVerdict"), "unverified-proxy") &&
             jStrEq(jField2(parsed, "memoryProjection", "caveat"), kStoredPcmProxyCaveat));
        TEST("Markdown contains Coverage & Honesty Notice + partial notice + MAN5 p.736",
             md1.find("## Coverage & Honesty Notice") != std::string::npos &&
             md1.find(kPartialCoverageNotice) != std::string::npos &&
             md1.find("MAN5 p.736") != std::string::npos);

        // Embedded KBD Set / Pad exclusion toggles
        NativeSelectivePlanRequest reqNoEmbed = req;
        reqNoEmbed.targets = {selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg")};
        reqNoEmbed.includeEmbeddedKbdSets = false;
        reqNoEmbed.includeEmbeddedPads = false;
        const auto resNoEmbed = planNativeSelectiveExport(reqNoEmbed);
        TEST("embedded KBD/Pad toggles restrict closure to main track USER01/0 only",
             resNoEmbed.ok && resNoEmbed.report.userSounds == 1);
    }

    // ── 2. Dangling USER12 + Unparsed USERDK + Graph::deleteCheck() never Unreferenced ───────────────────────────────
    {
        NativeSelectivePlanRequest req;
        req.sourceKst = fx.sourceKst;
        req.targets = {selTarget(SelectedKind::Style, "STYLE/Pop/DanglingStyle.stg")};
        const auto res = planNativeSelectiveExport(req);
        TEST("dangling/USERDK plan ok but NOT executable", res.ok && !res.executable());
        TEST("dangling USER12/99 emits MISSING_USER_SOUND_BANK blocking diagnostic",
             res.dryRunPlan.hasBlockingCode(diag_code::kMissingUserSoundBank));
        TEST("USERDK01/0 emits DRUMKIT_BODY_UNPARSED diagnostic",
             res.dryRunPlan.hasDiagnosticCode(diag_code::kDrumkitBodyUnparsed));
        TEST("analysed & speculative unresolved slot diagnostics emitted distinctly",
             res.dryRunPlan.hasDiagnosticCode(diag_code::kUnresolvedSoundSlotAnalysed, DiagnosticSeverity::Warning) &&
             res.dryRunPlan.hasDiagnosticCode(diag_code::kUnresolvedSoundSlotSpeculative, DiagnosticSeverity::Info));

        // Verify M1 Graph::deleteCheck() NEVER returns Unreferenced
        const auto m1 = resolveNativeKstClosureFolder(fx.sourceKst);
        TEST("M1 graph deleteCheck() NEVER returns Unreferenced for any Sound",
             m1.ok && m1.graph.has_value() &&
             m1.graph->deleteCheck({false, "USER01", Kind::Sound, 0}).verdict != DeleteVerdict::Unreferenced &&
             m1.graph->deleteCheck({false, "USER01", Kind::Sound, 4}).verdict != DeleteVerdict::Unreferenced);
    }

    // ── 3. UX Gate #2: Graceful OC31 KMP Degradation (Source AND Target) ─────────────────────────────────────────────
    {
        NativeSelectivePlanRequest req;
        req.sourceKst = fx.oc31Kst;
        req.targets = {selTarget(SelectedKind::Style, "STYLE/Legacy/OldStyle.stg")};
        const auto res = planNativeSelectiveExport(req);
        TEST("UX-2 OC31 source returns ok=true, executable=false (NOT crash/empty error)",
             res.ok && !res.executable());
        TEST("UX-2 OC31 source populates PartialNativeInspect Sound & PCM counts",
             res.sourcePartialInspect.isPartial &&
             res.sourcePartialInspect.kmpIsOc31 &&
             res.sourcePartialInspect.occupiedUserSounds == 2 &&
             res.sourcePartialInspect.pcmContainerCount == 1 &&
             res.sourcePartialInspect.notice == kOc31KmpDegradedNotice);
        TEST("UX-2 OC31_KMP_UNSUPPORTED diagnostic emitted",
             res.dryRunPlan.hasBlockingCode(diag_code::kOc31KmpUnsupported));
        const std::string j = serializeNativeSelectivePlanJson(res);
        const std::string m = serializeNativeSelectivePlanMarkdown(res);
        kit::Json parsed;
        TEST("UX-2 OC31 JSON parses and exposes partialInspect + kOc31KmpDegradedNotice",
             parseStrictJson(j, parsed) &&
             jBoolEq(jField2(parsed, "partialInspect", "isPartial"), true) &&
             jNumEq(jField2(parsed, "partialInspect", "occupiedUserSounds"), "2") &&
             jNumEq(jField2(parsed, "partialInspect", "pcmContainerCount"), "1") &&
             jStrEq(jField2(parsed, "partialInspect", "notice"), kOc31KmpDegradedNotice));
        TEST("UX-2 OC31 Markdown prints kOc31KmpDegradedNotice",
             m.find(kOc31KmpDegradedNotice) != std::string::npos);

        // OC31 as Target KST
        NativeSelectivePlanRequest reqTgt;
        reqTgt.sourceKst = fx.sourceKst;
        reqTgt.targetKst = fx.oc31Kst;
        reqTgt.targets = {selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg")};
        const auto resTgt = planNativeSelectiveExport(reqTgt);
        TEST("UX-2 OC31 target returns ok=true, executable=false with targetPartialInspect",
             resTgt.ok && !resTgt.executable() &&
             resTgt.targetPartialInspect.isPartial &&
             resTgt.targetPartialInspect.kmpIsOc31 &&
             resTgt.targetPartialInspect.occupiedUserSounds == 2 &&
             resTgt.dryRunPlan.hasBlockingCode(diag_code::kTargetUninspectable));

        // P3-1: Target-only OC31 JSON mirrors targetPartialInspect counts into top-level partialInspect shorthand
        const std::string jTgt = serializeNativeSelectivePlanJson(resTgt);
        kit::Json parsedTgt;
        TEST("P3-1 target-only OC31 JSON mirrors target counts into top-level partialInspect",
             parseStrictJson(jTgt, parsedTgt) &&
             jBoolEq(jField2(parsedTgt, "partialInspect", "isPartial"), true) &&
             jNumEq(jField2(parsedTgt, "partialInspect", "occupiedUserSounds"), "2") &&
             jNumEq(jField2(parsedTgt, "partialInspect", "pcmContainerCount"), "1") &&
             jStrEq(jField2(parsedTgt, "partialInspect", "notice"), kOc31KmpDegradedNotice));
    }

    // ── 4. UX Gate #4: Cooperative `keepGoing` Cancellation (native_kst_closure.h & selective_plan.h) ────────────────
    {
        NativeKstClosureOptions copts;
        copts.keepGoing = [] { return false; };
        const auto cFolder = resolveNativeKstClosureFolder(fx.sourceKst, copts);
        TEST("UX-4 resolveNativeKstClosureFolder respects keepGoing immediate cancel",
             cFolder.cancelled && !cFolder.ok && !cFolder.graph.has_value() && cFolder.manifest.roots.empty());

        NativeKstClosureInput inMem;
        const auto cMem = resolveNativeKstClosure(inMem, copts);
        TEST("UX-4 resolveNativeKstClosure respects keepGoing cancel",
             cMem.cancelled && !cMem.ok && !cMem.graph.has_value());

        int ticks = 0;
        NativeSelectivePlanRequest req;
        req.sourceKst = fx.sourceKst;
        req.targets = {selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg")};
        req.cancelCheckStride = 1;
        req.keepGoing = [&ticks]() { return (++ticks) <= 3; };
        const auto resCancel = planNativeSelectiveExport(req);
        TEST("UX-4 planNativeSelectiveExport mid-scan cancellation drops partial state cleanly",
             resCancel.cancelled && !resCancel.ok && resCancel.report.selection.empty());
    }

    // ── 5. HOLD Gate Enforcement & Target Policy Matrix (Block vs Reuse vs Remap) ────────────────────────────────────
    {
        NativeSelectivePlanRequest holdReq;
        holdReq.sourceKst = fx.sourceKst;
        holdReq.targets = {selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg")};
        holdReq.mode = ExportMode::NativeSoundKst_HOLD;
        const auto holdRes = planNativeSelectiveExport(holdReq);
        TEST("HOLD: ExportMode::NativeSoundKst_HOLD fails closed with NATIVE_SOUND_WRITE_HOLD",
             holdRes.ok && !holdRes.executable() &&
             holdRes.dryRunPlan.hasBlockingCode(diag_code::kNativeSoundWriteHold));

        jobs::JobState holdJob;
        jobs::runExport(advisoryPlanExportSpec(holdReq, holdRes, fx.tmpRoot / "hold-out"),
                        holdJob, [] { return false; }, [](const jobs::JobState&) {});
        TEST("HOLD: advisoryPlanExportSpec refuses NativeSoundKst_HOLD at Phase::Validate",
             holdJob.phase == jobs::Phase::Failed &&
             holdJob.stoppedAt == jobs::Phase::Validate &&
             !fs::exists(fx.tmpRoot / "hold-out"));

        // Symlink source refused fail-closed
        NativeSelectivePlanRequest symReq;
        symReq.sourceKst = fx.symlinkSource;
        symReq.targets = {selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg")};
        const auto symRes = planNativeSelectiveExport(symReq);
        TEST("symlink source KST refused fail-closed", !symRes.ok && symRes.error.find(fx.tmpRoot.string()) == std::string::npos);

        // Target conflict policies: CleanStyle uses USER01/0 (different in target), USER01/1 (identical in target),
        // USER01/2 (identical at target USER02/5)
        NativeSelectivePlanRequest blockReq;
        blockReq.sourceKst = fx.sourceKst;
        blockReq.targetKst = fx.targetKst;
        blockReq.targets = {selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg")};
        blockReq.policy = ConflictPolicy::BlockWithReport;
        const auto blockRes = planNativeSelectiveExport(blockReq);
        TEST("target conflict under block policy is NOT executable (USER01/0 & USER01/2 blocked)",
             blockRes.ok && !blockRes.executable() && blockRes.report.blockingConflictCount() >= 2);

        NativeSelectivePlanRequest remapReq = blockReq;
        remapReq.policy = ConflictPolicy::RemapToFreeSlotInNewLib;
        const auto remapRes = planNativeSelectiveExport(remapReq);
        TEST("target conflict under remap policy resolves cleanly (executable == true)",
             remapRes.ok && remapRes.executable() && remapRes.report.blockingConflictCount() == 0);
        // USER01/1 is exact same-slot identical -> omitted from manual single-Sound load steps!
        bool hasUser01Slot1Step = false;
        for (const auto& st : remapRes.pa5xSingleSoundSteps) {
            if (st.soundId == "pa5x:USER01/Sound/1") hasUser01Slot1Step = true;
        }
        TEST("MAN5 p.736 steps omit exact target-matched USER01/1 and keep non-present/remapped Sounds",
             !hasUser01Slot1Step && remapRes.pa5xSingleSoundSteps.size() == 2);
    }

    // ── 6. End-to-End CLI Tests (`aiarranger-resources kst-native selective-plan`) ───────────────────────────────────
    {
        const std::string bin = RESOURCES_BIN;

        // 6a. Clean exit 0 + --json parseable & path-scrubbed
        const auto rClean = runCli({
            bin, "kst-native", "selective-plan",
            "--source", fx.sourceKst.string(),
            "--select", "STYLE/Pop/CleanStyle.stg",
            "--json"
        });
        kit::Json jClean;
        TEST("CLI exit 0 on clean selective-plan + valid path-scrubbed JSON",
             rClean.exitCode == 0 &&
             rClean.stdoutText.find(fx.tmpRoot.string()) == std::string::npos &&
             parseStrictJson(rClean.stdoutText, jClean) &&
             jBoolEq(jField2(jClean, "summary", "executable"), true) &&
             jBoolEq(jClean.get("hardwareVerified"), false) &&
             jStrEq(jField2(jClean, "memoryProjection", "fitVerdict"), "unverified-proxy"));

        // 6b. Exit 3 on blocking dangling USER12 (stdout STILL gets full JSON!)
        const auto rDangle = runCli({
            bin, "kst-native", "selective-plan",
            "--source", fx.sourceKst.string(),
            "--select", "STYLE/Pop/DanglingStyle.stg",
            "--json"
        });
        kit::Json jDangle;
        TEST("CLI exit 3 on dangling USER12 while stdout prints complete JSON report",
             rDangle.exitCode == 3 &&
             parseStrictJson(rDangle.stdoutText, jDangle) &&
             jBoolEq(jField2(jDangle, "summary", "executable"), false) &&
             jNumVal(jField2(jDangle, "summary", "blockingIssues")) >= 1);

        // 6c. Exit 3 on target conflict under --policy block, exit 0 under --policy remap
        const auto rBlock = runCli({
            bin, "kst-native", "selective-plan",
            "--source", fx.sourceKst.string(),
            "--target", fx.targetKst.string(),
            "--select", "STYLE/Pop/CleanStyle.stg",
            "--policy", "block"
        });
        TEST("CLI exit 3 on target slot conflict under --policy block (Markdown printed)",
             rBlock.exitCode == 3 &&
             rBlock.stdoutText.find("## Conflict Matrix") != std::string::npos &&
             rBlock.stdoutText.find("BLOCKED") != std::string::npos);

        const auto rRemap = runCli({
            bin, "kst-native", "selective-plan",
            "--source", fx.sourceKst.string(),
            "--target", fx.targetKst.string(),
            "--select", "STYLE/Pop/CleanStyle.stg",
            "--target-bank", "STYLE/CustomBank",
            "--target-name", "RenamedStyle.stg",
            "--policy", "remap",
            "--json"
        });
        kit::Json jRemap;
        const kit::Json* objResArr = nullptr;
        const bool remapParsed = parseStrictJson(rRemap.stdoutText, jRemap);
        if (remapParsed) objResArr = jRemap.get("objectResolutions");
        TEST("CLI exit 0 under --policy remap + --target-bank/--target-name override",
             rRemap.exitCode == 0 &&
             remapParsed &&
             jBoolEq(jField2(jRemap, "summary", "executable"), true) &&
             objResArr && objResArr->t == kit::Json::T::Arr && objResArr->a.size() == 1 &&
             jStrEq(objResArr->a[0].get("targetRelPath"), "STYLE/CustomBank/RenamedStyle.stg"));

        // 6d. Exit 3 on OC31 KMP source (UX-2 graceful degradation!)
        const auto rOc31 = runCli({
            bin, "kst-native", "selective-plan",
            "--source", fx.oc31Kst.string(),
            "--select", "STYLE/Legacy/OldStyle.stg",
            "--json"
        });
        kit::Json jOc31;
        TEST("CLI exit 3 on OC31 KMP source with graceful PartialNativeInspect JSON",
             rOc31.exitCode == 3 &&
             parseStrictJson(rOc31.stdoutText, jOc31) &&
             jNumEq(jField2(jOc31, "partialInspect", "occupiedUserSounds"), "2") &&
             jNumEq(jField2(jOc31, "partialInspect", "pcmContainerCount"), "1") &&
             jStrEq(jField2(jOc31, "partialInspect", "notice"), kOc31KmpDegradedNotice));

        // 6e. --export-plan stages verified advisory folder + PACKAGE.sha256 + ZERO native KST files!
        const fs::path advOut = fx.tmpRoot / "cli_advisory_out";
        const auto rExport = runCli({
            bin, "kst-native", "selective-plan",
            "--source", fx.sourceKst.string(),
            "--target", fx.targetKst.string(),
            "--select", "STYLE/Pop/CleanStyle.stg",
            "--policy", "remap",
            "--export-plan", advOut.string()
        });
        size_t manFiles = 0;
        const auto skipMan = [](const std::string& rel) {
            return rel == "PACKAGE.sha256" || rel == ".DS_Store" || rel.rfind("._", 0) == 0;
        };
        const std::string verifyBad = ai_arranger::media::verifyManifest(
            advOut, advOut / "PACKAGE.sha256", {}, &manFiles, skipMan, true);
        bool anyNativeWritten = false;
        for (const auto& entry : fs::recursive_directory_iterator(advOut)) {
            if (entry.is_regular_file() && plan_detail::isForbiddenNativeExtension(entry.path().filename().string())) {
                anyNativeWritten = true;
            }
        }
        kit::Json jExported, jDiag;
        const std::string exportedJsonText = readTextFile(advOut / "selective-plan.json");
        const std::string exportedDiagText = readTextFile(advOut / "DIAGNOSTICS.json");
        TEST("CLI --export-plan stages verified PACKAGE.sha256 (3 advisory files, 0 native KST bytes)",
             rExport.exitCode == 0 &&
             verifyBad.empty() && manFiles == 3 &&
             !anyNativeWritten &&
             fs::is_regular_file(advOut / "SELECTIVE_EXPORT_REPORT.md") &&
             parseStrictJson(exportedJsonText, jExported) &&
             parseStrictJson(exportedDiagText, jDiag) &&
             exportedJsonText.find(fx.tmpRoot.string()) == std::string::npos &&
             exportedDiagText.find(fx.tmpRoot.string()) == std::string::npos);

        // 6f. Exit 1 error cases: usage, missing select, unknown select, D18 overlap (source & target), symlink source
        TEST("CLI exit 1 on missing --select",
             runCli({bin, "kst-native", "selective-plan", "--source", fx.sourceKst.string()}).exitCode == 1);
        TEST("CLI exit 1 on unknown --select path",
             runCli({bin, "kst-native", "selective-plan", "--source", fx.sourceKst.string(),
                     "--select", "STYLE/Pop/DoesNotExist.stg"}).exitCode == 1);
        TEST("CLI exit 1 on D18 overlap inside source KST",
             runCli({bin, "kst-native", "selective-plan", "--source", fx.sourceKst.string(),
                     "--select", "STYLE/Pop/CleanStyle.stg",
                     "--export-plan", (fx.sourceKst / "bad-overlap").string()}).exitCode == 1 &&
             !fs::exists(fx.sourceKst / "bad-overlap"));
        TEST("CLI exit 1 on D18 overlap inside target KST",
             runCli({bin, "kst-native", "selective-plan", "--source", fx.sourceKst.string(),
                     "--target", fx.targetKst.string(),
                     "--select", "STYLE/Pop/CleanStyle.stg",
                     "--policy", "remap",
                     "--export-plan", (fx.targetKst / "bad-overlap").string()}).exitCode == 1 &&
             !fs::exists(fx.targetKst / "bad-overlap"));
        TEST("CLI exit 1 on symlink source KST",
             runCli({bin, "kst-native", "selective-plan", "--source", fx.symlinkSource.string(),
                     "--select", "STYLE/Pop/CleanStyle.stg"}).exitCode == 1);

        // P3-3: CLI rejects duplicate --policy, duplicate --target-bank/--target-name, and sanitizes stderr control chars
        TEST("P3-3 CLI exit 1 on duplicate --policy",
             runCli({bin, "kst-native", "selective-plan", "--source", fx.sourceKst.string(),
                     "--select", "STYLE/Pop/CleanStyle.stg",
                     "--policy", "block", "--policy", "remap"}).exitCode == 1);
        TEST("P3-3 CLI exit 1 on duplicate --target-bank for same --select",
             runCli({bin, "kst-native", "selective-plan", "--source", fx.sourceKst.string(),
                     "--select", "STYLE/Pop/CleanStyle.stg",
                     "--target-bank", "STYLE/A", "--target-bank", "STYLE/B"}).exitCode == 1);
        TEST("P3-3 CLI exit 1 on duplicate --target-name for same --select",
             runCli({bin, "kst-native", "selective-plan", "--source", fx.sourceKst.string(),
                     "--select", "STYLE/Pop/CleanStyle.stg",
                     "--target-name", "A.stg", "--target-name", "B.stg"}).exitCode == 1);
        const auto rCtrl = runCli({
            bin, "kst-native", "selective-plan", "--source", fx.sourceKst.string(),
            "--select", std::string("STYLE/Pop/Bad\x1b[31mName.stg")
        });
        TEST("P3-3 CLI sanitizes control bytes in missing --select stderr via printable()",
             rCtrl.exitCode == 1 &&
             rCtrl.stderrText.find('\x1b') == std::string::npos &&
             rCtrl.stderrText.find("\\x1B") != std::string::npos);
    }

    // ── 6g. P2-4 Hardening Tests: Invalid optionalTargetMap, No-Target FAT R-18 Collisions & Executable Parity ───────
    {
        const M1AdapterView srcView = adaptNativeKstFolder(fx.sourceKst);
        TEST("P2-4 adaptNativeKstFolder succeeds for view-level tests", srcView.ok);

        // (a) Failed/invalid optionalTargetMap -> blocked resolutions (NEVER "no target KST supplied"!)
        TargetOccupancyMap badTarget;
        badTarget.valid = false;
        badTarget.fullyInspectable = false;
        badTarget.uninspectableReason = "simulated target container failure";

        NativeSelectivePlanRequest reqBadTgt;
        reqBadTgt.targets = {selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg")};
        const auto resBadTgt = planNativeSelectiveExportFromViews(srcView, reqBadTgt, &badTarget, 0);
        bool allSoundsBlockedHonest = !resBadTgt.soundResolutions.empty();
        for (const auto& sr : resBadTgt.soundResolutions) {
            if (!sr.blocking || sr.action != "blocked" || sr.resolvedAddress.has_value() ||
                sr.detail.find("no target KST supplied") != std::string::npos ||
                sr.detail.find("simulated target container failure") == std::string::npos) {
                allSoundsBlockedHonest = false;
            }
        }
        bool allObjsBlockedHonest = !resBadTgt.objectResolutions.empty();
        for (const auto& ores : resBadTgt.objectResolutions) {
            if (!ores.blocking || ores.action != "blocked" ||
                ores.detail.find("no target KST supplied") != std::string::npos ||
                ores.detail.find("simulated target container failure") == std::string::npos) {
                allObjsBlockedHonest = false;
            }
        }
        TEST("P2-4(a) invalid optionalTargetMap marks sound/object resolutions blocked with honest reason",
             !resBadTgt.ok && !resBadTgt.executable() && !resBadTgt.dryRunPlan.executable &&
             allSoundsBlockedHonest && allObjsBlockedHonest);

        // (b) Intra-batch case-folded --target-name collision without --target -> FAT_CASE_COLLISION_R18
        NativeSelectivePlanRequest reqFat;
        reqFat.targets = {
            selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg", "STYLE/Out", "Same.stg"),
            selTarget(SelectedKind::Style, "STYLE/Pop/SharedStyle.stg", "STYLE/Out", "same.stg")
        };
        const auto resFat = planNativeSelectiveExportFromViews(srcView, reqFat, nullptr, 0);
        bool anyFatObjBlocked = false;
        for (const auto& ores : resFat.objectResolutions) {
            if (ores.blocking && ores.action == "blocked-fat-case-r18") anyFatObjBlocked = true;
        }
        TEST("P2-4(b) no-target intra-batch case-folded collision emits FAT_CASE_COLLISION_R18 & blocks both report and dryRunPlan",
             resFat.ok && !resFat.report.executable() && !resFat.dryRunPlan.executable &&
             resFat.dryRunPlan.hasBlockingCode(diag_code::kFatCaseCollisionR18) &&
             anyFatObjBlocked);

        // (c) Invalid no-target override -> dryRunPlan.executable == report.executable() == false parity
        NativeSelectivePlanRequest reqBadPath;
        reqBadPath.targets = {
            selTarget(SelectedKind::Style, "STYLE/Pop/CleanStyle.stg", "", "../escape.stg")
        };
        const auto resBadPath = planNativeSelectiveExportFromViews(srcView, reqBadPath, nullptr, 0);
        TEST("P2-4(c) invalid no-target override keeps dryRunPlan.executable == report.executable() == false",
             resBadPath.ok && !resBadPath.report.executable() && !resBadPath.dryRunPlan.executable &&
             resBadPath.dryRunPlan.hasBlockingCode(diag_code::kStyleOrObjectPathCollision) &&
             resBadPath.objectResolutions.size() == 1 &&
             resBadPath.objectResolutions[0].action == "blocked-invalid-path");
    }

    // ── 7. `library_selective_preview.h` Tests (evaluateWithDependencies, planJsonWithDependencies, whereUsedWithManifest)
    {
        const lib::Library libA = lib::load(fx.sourceKst);
        const lib::Library libB = lib::load(fx.targetKst);
        TEST("library::load() succeeds on synthetic source & target KSTs", libA.ok && libB.ok);

        auto findItem = [](const lib::Library& L, const std::string& suffix) -> size_t {
            for (size_t i = 0; i < L.items.size(); ++i) {
                if (L.items[i].file == suffix) return i;
            }
            return L.items.size();
        };
        const size_t idxClean = findItem(libA, "STYLE/Pop/CleanStyle.stg");
        const size_t idxShared = findItem(libA, "STYLE/Pop/SharedStyle.stg");
        const size_t idxDangle = findItem(libA, "STYLE/Pop/DanglingStyle.stg");
        const size_t idxUser01 = findItem(libA, "SOUND/USER01.PCG");
        const size_t idxPcm01 = findItem(libA, "PCM/ram01.pcm");
        TEST("located all synthetic items in library::Library",
             idxClean < libA.items.size() && idxShared < libA.items.size() &&
             idxDangle < libA.items.size() && idxUser01 < libA.items.size() &&
             idxPcm01 < libA.items.size());

        const std::vector<lib::Step> steps = {
            {lib::Op::Copy, 'A', idxClean, "", -1, "", false},
            {lib::Op::Copy, 'A', idxShared, "", -1, "", false},
            {lib::Op::Copy, 'A', idxDangle, "", -1, "", false}
        };

        // 7a. deps == nullptr matches library::evaluate() identically
        const auto evPlain = lib::evaluate(libA, libB, steps);
        const auto evNull = lib::evaluateWithDependencies(libA, libB, steps, nullptr);
        TEST("evaluateWithDependencies(nullptr) matches library::evaluate() identically",
             evPlain.conflicts == evNull.conflicts &&
             evPlain.warnings == evNull.warnings &&
             evPlain.steps.size() == evNull.steps.size() &&
             evPlain.steps[0].warnings == evNull.steps[0].warnings);

        // 7b. With M1 closure & TargetOccupancyMap attached (NO viewA -> P2-2 honest SOUND_SLOT_EQUIVALENCE_INSUFFICIENT)
        const auto closureA = resolveNativeKstClosureFolder(fx.sourceKst);
        const auto occB = inspectTargetKstFolder(fx.targetKst);
        lib::SelectiveDependencyContext ctx;
        ctx.closureA = &closureA;
        ctx.occupancyB = &occB;
        ctx.policy = ConflictPolicy::BlockWithReport;
        ctx.strictBlockingConflicts = false;

        const auto evWarnOnly = lib::evaluateWithDependencies(libA, libB, steps, &ctx);
        auto stepHasSubstr = [](const std::vector<std::string>& v, const std::string& sub) {
            for (const auto& s : v) if (s.find(sub) != std::string::npos) return true;
            return false;
        };
        TEST("P2-2 without equivalence view: generic opaque warning replaced & occupied slot reports SOUND_SLOT_EQUIVALENCE_INSUFFICIENT (NOT OCCUPIED_DIFFERENT)",
             !stepHasSubstr(evWarnOnly.steps[0].warnings, "payload not decoded: which Sounds it uses is UNKNOWN") &&
             stepHasSubstr(evWarnOnly.steps[0].warnings, "M2 dependency closure") &&
             stepHasSubstr(evWarnOnly.steps[0].warnings, "SOUND_SLOT_EQUIVALENCE_INSUFFICIENT") &&
             !stepHasSubstr(evWarnOnly.steps[0].warnings, "SOUND_SLOT_OCCUPIED_DIFFERENT"));
        TEST("second step reports cross-step deduplicated User Sounds",
             stepHasSubstr(evWarnOnly.steps[1].warnings, "deduplicated with earlier plan step"));
        TEST("dangling/USERDK step reports DRUMKIT_BODY_UNPARSED & MISSING_USER_SOUND_SLOT",
             stepHasSubstr(evWarnOnly.steps[2].warnings, "DRUMKIT_BODY_UNPARSED") &&
             stepHasSubstr(evWarnOnly.steps[2].warnings, "MISSING_USER_SOUND_SLOT"));

        // 7c. strictBlockingConflicts == true promotes insufficient equivalence & missing sounds to StepCheck::conflicts
        ctx.strictBlockingConflicts = true;
        const auto evStrict = lib::evaluateWithDependencies(libA, libB, steps, &ctx);
        TEST("strictBlockingConflicts=true promotes unprovable occupied slot & missing issues to StepCheck::conflicts",
             evStrict.blocked() &&
             evStrict.conflicts > evWarnOnly.conflicts &&
             stepHasSubstr(evStrict.steps[0].conflicts, "SOUND_SLOT_EQUIVALENCE_INSUFFICIENT") &&
             !stepHasSubstr(evStrict.steps[0].conflicts, "SOUND_SLOT_OCCUPIED_DIFFERENT") &&
             stepHasSubstr(evStrict.steps[2].conflicts, "MISSING_USER_SOUND_SLOT"));

        // 7c-2. P2-2 WITH viewA attached: real compareSoundEquivalence() proves USER01/0 Different & USER01/1 Identical (reuse-target)!
        const M1AdapterView viewA = adaptNativeKstFolder(fx.sourceKst);
        lib::SelectiveDependencyContext ctxWithView = ctx;
        ctxWithView.viewA = &viewA;
        const auto evStrictWithView = lib::evaluateWithDependencies(libA, libB, steps, &ctxWithView);
        TEST("P2-2 with viewA attached: compareSoundEquivalence() proves USER01/0 OCCUPIED_DIFFERENT and USER01/1 reuse-target",
             evStrictWithView.blocked() &&
             stepHasSubstr(evStrictWithView.steps[0].conflicts, "SOUND_SLOT_OCCUPIED_DIFFERENT") &&
             stepHasSubstr(evStrictWithView.steps[0].warnings, "USER01/1") &&
             stepHasSubstr(evStrictWithView.steps[0].warnings, "reuse-target"));

        // 7c-3. P2-1 Fail-closed on corrupt .stg (parseOk == false) — NEVER reports "0 User Sound(s)"!
        {
            const fs::path corruptKst = fx.tmpRoot / "SynthCorrupt.KST";
            writeBytesFile(corruptKst / "STYLE" / "Pop" / "Broken.stg", Bytes{'B', 'A', 'D', '!'});
            const lib::Library libCorrupt = lib::load(corruptKst);
            const auto closureCorrupt = resolveNativeKstClosureFolder(corruptKst);
            const size_t idxBroken = findItem(libCorrupt, "STYLE/Pop/Broken.stg");
            lib::SelectiveDependencyContext cCtx;
            cCtx.closureA = &closureCorrupt;
            cCtx.strictBlockingConflicts = true;
            const std::vector<lib::Step> cSteps = {{lib::Op::Copy, 'A', idxBroken, "", -1, "", false}};
            const auto evCorrupt = lib::evaluateWithDependencies(libCorrupt, libB, cSteps, &cCtx);
            TEST("P2-1 corrupt .stg (parseOk==false) emits MALFORMED_ROOT_TREE and NEVER claims '0 User Sound(s)'",
                 idxBroken < libCorrupt.items.size() &&
                 evCorrupt.blocked() &&
                 stepHasSubstr(evCorrupt.steps[0].conflicts, "MALFORMED_ROOT_TREE") &&
                 !stepHasSubstr(evCorrupt.steps[0].warnings, "0 User Sound(s)"));
        }

        // 7c-4. P2-3 Mid-inner-loop cancellation inside root->soundRefs returns clean empty Evaluation{} (zero partial leak!)
        {
            const fs::path manyKst = fx.tmpRoot / "SynthManyRefs.KST";
            std::vector<SmfSelSpec> manySels;
            manySels.reserve(80);
            for (int i = 0; i < 80; ++i) {
                manySels.push_back({static_cast<uint8_t>(i % 16), 121, 0, static_cast<uint8_t>(i)});
            }
            const Bytes manyStyle = rtSerialize({
                rtBranch("Var1.scn", {rtBranch("Track.pdc", {rtLeaf("Main.mid", makeSmf(manySels))})})
            });
            writeBytesFile(manyKst / "STYLE" / "Pop" / "ManyRefs.stg", manyStyle);
            const lib::Library libMany = lib::load(manyKst);
            const auto closureMany = resolveNativeKstClosureFolder(manyKst);
            const size_t idxMany = findItem(libMany, "STYLE/Pop/ManyRefs.stg");
            lib::SelectiveDependencyContext mCtx;
            mCtx.closureA = &closureMany;
            const std::vector<lib::Step> mSteps = {{lib::Op::Copy, 'A', idxMany, "", -1, "", false}};
            // Call 1 = inside evaluate() step 0; Call 2 = tick 0 before outer loop; Call 3 = tick 64 inside root->soundRefs!
            int keepCalls = 0;
            const auto evMidCancel = lib::evaluateWithDependencies(
                libMany, libB, mSteps, &mCtx, [&keepCalls]() { return (++keepCalls) <= 2; });
            TEST("P2-3 mid-inner-loop keepGoing cancel in evaluateWithDependencies returns empty Evaluation{} (zero partial leak)",
                 keepCalls >= 3 &&
                 evMidCancel.steps.empty() &&
                 evMidCancel.warnings == 0 &&
                 evMidCancel.conflicts == 0);
        }

        // 7c-5. P3-3 Op::Move + 'B' -> 'A' direction + skipping already-invalidated (moved/gone) step
        {
            const auto closureB = resolveNativeKstClosureFolder(fx.targetKst);
            const auto occA = inspectTargetKstFolder(fx.sourceKst);
            const size_t idxTargetStyle = findItem(libB, "STYLE/Pop/ExistingOther.stg");
            lib::SelectiveDependencyContext dirCtx;
            dirCtx.closureA = &closureA;
            dirCtx.closureB = &closureB;
            dirCtx.occupancyA = &occA;
            dirCtx.occupancyB = &occB;
            dirCtx.strictBlockingConflicts = false;

            const std::vector<lib::Step> moveAndRevSteps = {
                {lib::Op::Move, 'A', idxClean, "", -1, "", false},        // valid Move A->B
                {lib::Op::Copy, 'A', idxClean, "", -1, "", false},        // invalidated: idxClean already moved (gone)!
                {lib::Op::Move, 'B', idxTargetStyle, "", -1, "", false}   // valid Move B->A
            };
            const auto evDir = lib::evaluateWithDependencies(libA, libB, moveAndRevSteps, &dirCtx);
            TEST("P3-3 Op::Move A->B & B->A enriched cleanly and already-moved (invalidated) step skipped",
                 idxTargetStyle < libB.items.size() &&
                 evDir.steps.size() == 3 &&
                 stepHasSubstr(evDir.steps[0].warnings, "M2 dependency closure") &&
                 !evDir.steps[1].conflicts.empty() &&
                 !stepHasSubstr(evDir.steps[1].warnings, "M2 dependency closure") &&
                 stepHasSubstr(evDir.steps[2].warnings, "M2 dependency closure"));
        }

        // 7d. planJsonWithDependencies preserves v1 format + adds selectiveDependencies
        const std::string pjson = lib::planJsonWithDependencies(libA, libB, steps, evStrict, &ctx);
        kit::Json jPlan;
        const bool pjsonOk = parseStrictJson(pjson, jPlan);
        const kit::Json* selDeps = pjsonOk ? jPlan.get("selectiveDependencies") : nullptr;
        TEST("planJsonWithDependencies parses strictly, keeps v1 format, adds selectiveDependencies",
             pjsonOk &&
             jStrEq(jPlan.get("format"), "aiarranger-library-plan") &&
             jNumEq(jPlan.get("version"), "1") &&
             jBoolEq(jField(selDeps, "hardwareVerified"), false) &&
             jBoolEq(jField(selDeps, "deleteProofAvailable"), false) &&
             jBoolEq(jField(selDeps, "strictBlockingConflicts"), true) &&
             jBoolEq(jField(jField(selDeps, "A"), "closureAttached"), true) &&
             jBoolEq(jField(jField(selDeps, "B"), "occupancyAttached"), true));

        // 7e. whereUsedWithManifest exposes verified M1 edges & ALWAYS preserves honest footer
        const auto wuSound = lib::whereUsedWithManifest(libA, libA.items[idxUser01], &closureA, &libB);
        const std::string wuSoundText = lib::whereUsedWithManifestText(libA, libA.items[idxUser01], &closureA, &libB);
        bool sawStyleTrack = false, sawKbdPart = false, sawPadTrack = false;
        for (const auto& e : wuSound.verifiedEdges) {
            if (e.via == "StyleTrack") sawStyleTrack = true;
            if (e.via == "KeyboardSetPart") sawKbdPart = true;
            if (e.via == "PadTrack") sawPadTrack = true;
        }
        TEST("whereUsedWithManifest exposes StyleTrack, KeyboardSetPart & PadTrack M1 edges",
             wuSound.usedManifest && !wuSound.deleteProofAvailable &&
             sawStyleTrack && sawKbdPart && sawPadTrack);
        TEST("whereUsedWithManifest ALWAYS preserves mandatory honest footer",
             wuSound.footer == lib::kWhereUsedHonestFooter &&
             wuSoundText.find(lib::kWhereUsedHonestFooter) != std::string::npos);

        const auto wuPcm = lib::whereUsedWithManifest(libA, libA.items[idxPcm01], &closureA);
        bool sawZone = false, sawDrumSampleOf = false;
        for (const auto& e : wuPcm.verifiedEdges) {
            if (e.via == "Zone") sawZone = true;
            if (e.via == "DrumSampleOf") sawDrumSampleOf = true;
        }
        TEST("whereUsedWithManifest on PCM container exposes Zone & DrumSampleOf edges + deleteProofAvailable=false",
             wuPcm.usedManifest && !wuPcm.deleteProofAvailable && sawZone && sawDrumSampleOf);

        // Fallback when closure == nullptr still preserves mandatory honest footer
        const auto wuFallback = lib::whereUsedWithManifest(libA, libA.items[idxUser01], nullptr);
        TEST("whereUsedWithManifest(nullptr) falls back cleanly with honest footer & deleteProofAvailable=false",
             !wuFallback.usedManifest && !wuFallback.deleteProofAvailable &&
             wuFallback.footer == lib::kWhereUsedHonestFooter);
    }

    // ── 8. Zero Source/Target Mutation Guarantee ─────────────────────────────────────────────────────────────────────
    TEST("zero mutation of source, target, and OC31 KST folders after all planner & CLI runs",
         snapshotDirHashes(fx.sourceKst) == origSourceHashes &&
         snapshotDirHashes(fx.targetKst) == origTargetHashes &&
         snapshotDirHashes(fx.oc31Kst) == origOc31Hashes);

    std::printf("=== Result: %d passed, %d failed ===\n", gPasses, gFailures);
    return gFailures == 0 ? 0 : 1;
}
