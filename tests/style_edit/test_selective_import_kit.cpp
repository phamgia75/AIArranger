// Slice M2c Synthetic & TOCTOU Regression Tests (src/jobs/selective_import_kit.h):
//   1. End-to-end target-aware selective kit + Tier 2 package dedup + unused package exclusion
//   2. Existing target User Sound reuse (`reuse-target`) + Factory ROM Sound ref
//   3. Missing referenced User Sound blocking (`VERIFIED_MISSING: MISSING_USER_SOUND_SLOT`)
//   4. Target slot collision policy matrix (`block` vs `remap` free-slot recommendation)
//   5. Memory projection (provisional uncompressed vs target stored-PCM proxy caveat vs identical-WAV extra bytes)
//   6. TOCTOU — Source Style .mid & package WAV 1-byte flip preserving size & mtime!
//   7. TOCTOU — Target KST SOUND/USER01.PCG mutation between plan & execute AND right before beforePublish!
//   8. D18 overlap guard — refusing dest inside workspaceRoot, package, AND targetKst (direct & symlink)
//   9. Target OC31 RAM.KMP (0x11050038) graceful fail-closed refusal with PartialNativeInspect
//  10. Cooperative cancellation (`keepGoing`) dropping partial state cleanly
//  11. Determinism across shuffled input order + verifyKit() strict selective JSON validation
//
// 100% synthetic, original fixtures only (zero private owner data, zero real Sound/Style names, zero local paths).

#include "jobs/selective_import_kit.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include "media/sound/korg_kmp_writer.h"
#include "media/sound/package_writer.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::jobs;
namespace k = ai_arranger::jobs::kit;
namespace sel = ai_arranger::media::selective;
namespace gr = ai_arranger::media::graph;
namespace pan = ai_arranger::media::panative;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
static void check(const char* name, bool ok) {
    if (!ok) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; }
    else { std::printf("  PASS: %s\n", name); ++passes; }
}
#define TEST(name, ...) check((name), (__VA_ARGS__))

using Bytes = std::vector<uint8_t>;

static bool has(const std::string& s, const std::string& w) { return s.find(w) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static void putBytes(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
static void putStrFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

static JobState runJob(const ExportSpec& s) {
    JobState st;
    st.id = 1;
    runExport(s, st, [] { return false; }, [](const JobState&) {});
    return st;
}

static std::map<std::string, std::string> snapshotDir(const fs::path& root) {
    std::map<std::string, std::string> m;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
        const auto rel = fs::relative(it->path(), root).generic_string();
        if (it->is_symlink(ec)) { m[rel] = "link"; continue; }
        if (it->is_directory(ec)) { m[rel] = "dir"; continue; }
        bool c = false;
        std::string e;
        m[rel] = media::detail::sha256File(it->path(), [](size_t) { return true; }, &c, &e);
    }
    return m;
}

// Re-signs PACKAGE.sha256 in a test copy (to test verifyKit's strict selective JSON checks)
static void resignKit(const fs::path& dir) {
    std::string man;
    int n = 0;
    for (const auto& f : listFiles(dir)) {
        if (f.rel != "PACKAGE.sha256" && !k::detail::finderLitter(f.rel)) {
            bool c = false;
            std::string e;
            man += media::detail::sha256File(dir / f.rel, [](size_t) { return true; }, &c, &e) + "  " + f.rel + "\n";
            ++n;
        }
    }
    putStrFile(dir / "PACKAGE.sha256", man + "# " + std::to_string(n) + " files\n");
}

// ── Synthetic Native Pa5X KST Builders (100% original test-only builders) ─────────────────────────────────────────────

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
static void putAt(Bytes& b, size_t at, const std::string& s) {
    for (size_t i = 0; i < s.size() && at + i < b.size(); ++i) b[at + i] = static_cast<uint8_t>(s[i]);
}
static void appendStr(Bytes& b, const std::string& s) {
    b.insert(b.end(), reinterpret_cast<const uint8_t*>(s.data()), reinterpret_cast<const uint8_t*>(s.data()) + s.size());
}
static gr::Id8 makeId(uint8_t tag, uint8_t idx) {
    return gr::Id8{tag, idx, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
}
static std::string idAsStr(const gr::Id8& id) {
    return std::string(reinterpret_cast<const char*>(id.data()), id.size());
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
        std::string n = it.name;
        n.resize(28, '\0');
        pushBe16(rec, 2); pushBe16(rec, 28); appendStr(rec, n);
        pushBe16(rec, 0); pushBe16(rec, 3); rec.push_back(it.kind); rec.push_back(it.bank); rec.push_back(it.slot);
        pushBe16(rec, 1); pushBe16(rec, 2); pushBe16(rec, sub);
        if (!it.sampleId.empty()) { pushBe16(rec, 4); pushBe16(rec, 8); appendStr(rec, it.sampleId); }
        rec.push_back(0x10); rec.push_back(0x20); rec.push_back(0x30); rec.push_back(0x40);
        pushBe16(dir, static_cast<uint16_t>(rec.size()));
        dir.insert(dir.end(), rec.begin(), rec.end());
    }
    Bytes hdr(12, 0);
    putAt(hdr, 7, "KORF");
    const uint32_t dirOff = 8u + 8u + static_cast<uint32_t>(hdr.size());
    std::vector<uint32_t> offsets{dirOff};
    uint32_t cur = dirOff + 8u + static_cast<uint32_t>(dir.size());
    for (const auto& it : items) {
        offsets.push_back(cur);
        cur += 8u + static_cast<uint32_t>(it.payload.size());
    }
    Bytes idx;
    appendStr(idx, "KBEG");
    for (uint32_t off : offsets) pushBe32(idx, off);
    appendStr(idx, "KEND");
    pushBe32(idx, static_cast<uint32_t>(offsets.size()));

    Bytes body;
    pushBe32(body, 0x02000018u); pushBe32(body, static_cast<uint32_t>(hdr.size())); body.insert(body.end(), hdr.begin(), hdr.end());
    pushBe32(body, 0x05010018u); pushBe32(body, static_cast<uint32_t>(dir.size())); body.insert(body.end(), dir.begin(), dir.end());
    for (const auto& it : items) {
        pushBe32(body, it.chunkId);
        pushBe32(body, static_cast<uint32_t>(it.payload.size()));
        body.insert(body.end(), it.payload.begin(), it.payload.end());
    }
    pushBe32(body, 0xFE000018u); pushBe32(body, static_cast<uint32_t>(idx.size())); body.insert(body.end(), idx.begin(), idx.end());

    Bytes file;
    pushBe32(file, 0x01000114u);
    pushBe32(file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

static Bytes buildRawPa5xKmpPayload(const std::vector<pan::Pa5xSample>& samples,
                                    const std::vector<pan::Pa5xDrumRecord>& drums,
                                    const std::vector<pan::Pa5xZone>& zones,
                                    const std::vector<pan::Pa5xMultisample>& multisamples) {
    Bytes b;
    pushBe16(b, 0x0103u);
    pushBe32(b, static_cast<uint32_t>(samples.size()));
    pushBe32(b, static_cast<uint32_t>(drums.size()));
    pushBe32(b, static_cast<uint32_t>(zones.size()));
    pushBe32(b, static_cast<uint32_t>(multisamples.size()));
    for (const auto& s : samples) {
        Bytes rec(122, 0);
        putAt(rec, 75, s.name);
        std::copy(s.id.begin(), s.id.end(), rec.begin() + 107);
        rec[119] = static_cast<uint8_t>(s.rate >> 8);
        rec[120] = static_cast<uint8_t>(s.rate & 0xFFu);
        b.insert(b.end(), rec.begin(), rec.end());
    }
    for (const auto& d : drums) {
        Bytes rec(126, 0);
        putAt(rec, 0, d.name);
        rec[32] = static_cast<uint8_t>(d.left >> 24);
        rec[33] = static_cast<uint8_t>((d.left >> 16) & 0xFFu);
        rec[34] = static_cast<uint8_t>((d.left >> 8) & 0xFFu);
        rec[35] = static_cast<uint8_t>(d.left & 0xFFu);
        rec[36] = static_cast<uint8_t>(d.right >> 24);
        rec[37] = static_cast<uint8_t>((d.right >> 16) & 0xFFu);
        rec[38] = static_cast<uint8_t>((d.right >> 8) & 0xFFu);
        rec[39] = static_cast<uint8_t>(d.right & 0xFFu);
        std::copy(d.id.begin(), d.id.end(), rec.begin() + 118);
        b.insert(b.end(), rec.begin(), rec.end());
    }
    for (const auto& z : zones) {
        Bytes rec(16, 0);
        rec[0] = static_cast<uint8_t>(z.sample >> 24);
        rec[1] = static_cast<uint8_t>((z.sample >> 16) & 0xFFu);
        rec[2] = static_cast<uint8_t>((z.sample >> 8) & 0xFFu);
        rec[3] = static_cast<uint8_t>(z.sample & 0xFFu);
        rec[10] = z.topKey;
        b.insert(b.end(), rec.begin(), rec.end());
    }
    for (const auto& m : multisamples) {
        Bytes rec(176, 0);
        putAt(rec, 0, m.name);
        rec[32] = static_cast<uint8_t>(m.firstZone >> 24);
        rec[33] = static_cast<uint8_t>((m.firstZone >> 16) & 0xFFu);
        rec[34] = static_cast<uint8_t>((m.firstZone >> 8) & 0xFFu);
        rec[35] = static_cast<uint8_t>(m.firstZone & 0xFFu);
        rec[166] = m.zoneCount;
        std::copy(m.id.begin(), m.id.end(), rec.begin() + 168);
        b.insert(b.end(), rec.begin(), rec.end());
    }
    return b;
}

static Bytes buildSoundBody(uint8_t tagByte, const gr::Id8& msHigh, const gr::Id8& msLow) {
    Bytes body(40, tagByte);
    std::copy(msHigh.begin(), msHigh.end(), body.begin() + 16);
    std::copy(msLow.begin(), msLow.end(), body.begin() + 28);
    return body;
}

// Writes a synthetic valid Target Pa5X KST folder with USER01/0..2 occupied (3 sounds),
// raw Pa5X RAM.KMP (0x11080018), and PCM/ram01.pcm (stored payload = 2 * pcmPayloadLen).
static void writeSyntheticTargetKst(const fs::path& kstDir, bool oc31Kmp = false, uint32_t extraSlot = 255) {
    fs::remove_all(kstDir);
    const gr::Id8 smp0 = makeId(0xA1, 0), smp1 = makeId(0xA1, 1);
    const gr::Id8 ms0 = makeId(0xB1, 0), ms1 = makeId(0xB1, 1);

    const Bytes kmpPayload = buildRawPa5xKmpPayload(
        {{smp0, "SynthSmp0", 48000}, {smp1, "SynthSmp1", 48000}},
        {},
        {{0, 60}, {1, 127}},
        {{ms0, "SynthMs0", 0, 1}, {ms1, "SynthMs1", 1, 1}});

    if (oc31Kmp) {
        Bytes ocPayload = {'O', 'C', '3', '1', 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
        const Bytes kmpFile = buildKorfContainer({{0x11050038u, 0x11, 0, 0, "RAM_OC31", ocPayload, ""}});
        putBytes(kstDir / "MULTISMP" / "RAM.KMP", kmpFile);
    } else {
        const Bytes kmpFile = buildKorfContainer({{0x11080018u, 0x11, 0, 0, "RAM", kmpPayload, ""}});
        putBytes(kstDir / "MULTISMP" / "RAM.KMP", kmpFile);
    }

    const Bytes pcmFile = buildKorfContainer({
        {0x0D010018u, 0x0D, 0, 0, "SynthSmp0", Bytes(8192, 0x22), idAsStr(smp0)},
        {0x0D010018u, 0x0D, 0, 1, "SynthSmp1", Bytes(4096, 0x33), idAsStr(smp1)}
    });
    putBytes(kstDir / "PCM" / "ram01.pcm", pcmFile);

    std::vector<SynthKorfItem> user01Items = {
        {0x10020038u, 0x10, 0, 0, "TargetSound0", buildSoundBody(0x11, ms0, ms1), ""},
        {0x10020038u, 0x10, 0, 1, "TargetSound1", buildSoundBody(0x22, ms0, ms1), ""},
        {0x10020038u, 0x10, 0, 2, "TargetSound2", buildSoundBody(0x33, ms1, ms0), ""}
    };
    if (extraSlot < 128) {
        user01Items.push_back({0x10020038u, 0x10, 0, static_cast<uint8_t>(extraSlot),
                               "TargetSoundExtra", buildSoundBody(0x44, ms0, ms1), ""});
    }
    putBytes(kstDir / "SOUND" / "USER01.PCG", buildKorfContainer(user01Items));
    putBytes(kstDir / "SOUND" / "USER02.PCG", buildKorfContainer({}));
    putBytes(kstDir / "SOUND" / "USERDK01.PCG", buildKorfContainer({}));
}

// Writes a valid synthetic Pa5X marker-SMF Style using StyleStudioModel + custom Korg channel SoundRefs.
static bool writeSyntheticStyleSmf(const fs::path& outPath,
                                   const std::map<uint8_t, uasf::KorgSound>& sounds) {
    ui::studio::StyleStudioModel model;
    std::string err;
    if (!model.load(fs::path(REPO_DIR) / "fixtures/synthetic/SYNTH_WALTZ_68.sty", &err)) return false;
    uasf::StyleDefinition st = model.style();
    st.korg_export.sounds = sounds;
    st.korg_export.element_sounds.clear();
    const auto smf = style_edit::exportPa5xSmf(st);
    if (!smf.ok) return false;
    fs::create_directories(outPath.parent_path());
    return media::writeNewFile(outPath, smf.bytes).empty();
}

int main() {
    std::printf("Test: Slice M2c Target-Aware Selective Pa5X Import Kit\n");
    const fs::path root = fs::temp_directory_path() / ("aia-m2c-selective-kit-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    // ── Synthetic workspace fixtures (100% original synthetic data) ──
    const fs::path ws = root / "workspace";
    fs::create_directories(ws);
    std::string err;

    const auto organ = media::sound::buildDemoOrgan();
    auto unusedOrgan = organ;
    unusedOrgan.name = "AIA Unused Org";
    auto dupWavOrgan = organ;
    dupWavOrgan.name = "AIA Organ Copy";

    bool setupOk = !media::sound::writePackage(organ, ws / "AIA Demo Organ", "synthetic", &err).empty();
    setupOk &= !media::sound::writeDrumPackage(media::sound::buildDemoKit(), ws / "AIA Demo Kit", "synthetic", &err).empty();
    setupOk &= !media::sound::writePackage(unusedOrgan, ws / "UnusedOrg", "synthetic", &err).empty();
    setupOk &= !media::sound::writePackage(dupWavOrgan, ws / "SecondOrgan", "synthetic", &err).empty();

    // StyleA: bass -> Factory (121/0/33), drum -> USERDK01/0 (120/64/0), acc1 -> USER01/3 (121/64/3)
    setupOk &= writeSyntheticStyleSmf(ws / "StyleA.mid", {
        {8, {121, 0, 33}},
        {9, {120, 64, 0}},
        {11, {121, 64, 3}}
    });
    // StyleB: shares USER01/3 (AIA Demo Organ) + Factory (121/0/0)
    setupOk &= writeSyntheticStyleSmf(ws / "StyleB.mid", {
        {8, {121, 0, 0}},
        {11, {121, 64, 3}}
    });
    // StyleReuse: references existing Target USER01/0 (121/64/0) + Factory (121/0/33)
    setupOk &= writeSyntheticStyleSmf(ws / "StyleReuse.mid", {
        {8, {121, 0, 33}},
        {11, {121, 64, 0}}
    });
    // StyleMissing: references missing USER11/40 (121/74/40)
    setupOk &= writeSyntheticStyleSmf(ws / "StyleMissing.mid", {
        {8, {121, 0, 33}},
        {11, {121, 74, 40}}
    });
    // StyleCollide: references occupied USER01/0 (bound to AIA Demo Organ -> collision!)
    setupOk &= writeSyntheticStyleSmf(ws / "StyleCollide.mid", {
        {8, {121, 0, 33}},
        {11, {121, 64, 0}}
    });
    // StyleDupWav: references USER01/3 (AIA Demo Organ) + USER01/4 (SecondOrgan, identical WAV audio!)
    setupOk &= writeSyntheticStyleSmf(ws / "StyleDupWav.mid", {
        {8, {121, 64, 3}},
        {11, {121, 64, 4}}
    });

    const fs::path targetKst = root / "Target.KST";
    writeSyntheticTargetKst(targetKst, /*oc31Kmp=*/false);
    const fs::path oc31TargetKst = root / "Oc31Target.KST";
    writeSyntheticTargetKst(oc31TargetKst, /*oc31Kmp=*/true);

    TEST("fixtures created cleanly", setupOk);
    const auto wsBefore = snapshotDir(ws);
    const auto targetBefore = snapshotDir(targetKst);

    // ── Test 1: End-to-End Target-Aware Selective Kit + Tier 2 Dedup + Unused Excluded ──
    {
        k::SelectiveKitRequest req;
        req.workspaceRoot = ws;
        req.selectedStyles = {"StyleA.mid", "StyleB.mid"};
        req.slotToPackageMap[{false, 1, 3}] = ws / "AIA Demo Organ";
        req.slotToPackageMap[{true, 1, 0}] = ws / "AIA Demo Kit";
        req.targetKst = targetKst;
        req.conflictPolicy = sel::ConflictPolicy::BlockWithReport;

        const auto plan = k::selectivePlan(req);
        TEST("T1: plan ok & executable", plan.ok && plan.executable());
        TEST("T1: unused packages excluded (UnusedOrg & SecondOrgan)", plan.excludedUnusedPackages == 2);
        std::set<std::string> kitPaths;
        for (const auto& it : plan.basePlan.items) kitPaths.insert(it.kitPath());
        const std::set<std::string> wantPaths = {
            "DRUMKITS/AIA Demo Kit", "SOUNDS/AIA Demo Organ", "STYLES/StyleA.mid", "STYLES/StyleB.mid"
        };
        TEST("T1: shared Sound included ONCE & unused excluded", kitPaths == wantPaths);
        TEST("T1: Tier 2 package dedup savedBytes > 0", plan.report.memoryProjection.logicalDedupSavedBytes > 0);
        TEST("T1: slot recommendations include USER01/3 and USERDK01/0",
             plan.slotRecommendations.size() == 2 &&
             plan.slotRecommendations[0].recommendedSlot.has_value() &&
             plan.slotRecommendations[1].recommendedSlot.has_value());
        TEST("T1: selectiveFingerprint & targetLibraryKey are 64-hex",
             plan.selectiveFingerprint.size() == 64 && plan.targetLibraryKey.size() == 64);

        // Execute E6 job & verify published kit
        const fs::path kitOut = root / "SelectiveKit1";
        const auto st = runJob(k::selectiveImportKit(req, kitOut, plan.selectiveFingerprint, plan.basePlan.sha, plan.targetLibraryKey));
        TEST("T1: E6 selectiveImportKit Done", st.phase == Phase::Done && st.reason.empty());
        TEST("T1: L1 passed, L2/L3 NotRun",
             st.level.l1.state == Check::Passed && st.level.l2.state == Check::NotRun && st.level.l3.state == Check::NotRun);

        std::string vtext;
        TEST("T1: standalone kit::verifyKit() PASS", k::verifyKit(kitOut, nullptr, &vtext).empty() && has(vtext, k::kLabel));
        TEST("T1: SELECTIVE_MANIFEST.json and DIAGNOSTICS.json exist",
             fs::is_regular_file(kitOut / "SELECTIVE_MANIFEST.json") && fs::is_regular_file(kitOut / "DIAGNOSTICS.json"));
        TEST("T1: UnusedOrg absent from kit", !fs::exists(kitOut / "SOUNDS" / "UnusedOrg"));

        const std::string md = slurp(kitOut / "KIT.md");
        TEST("T1: KIT.md contains kLabel, K9 caveat, USER01/3, stored-PCM proxy caveat",
             has(md, k::kLabel) && has(md, sel::kSmfPcCaveatK9) && has(md, "USER01/3") && has(md, sel::kStoredPcmProxyCaveat));

        k::Json sj;
        const std::string smJson = slurp(kitOut / "SELECTIVE_MANIFEST.json");
        TEST("T1: SELECTIVE_MANIFEST.json parses via kit::parseJson() with hardwareVerified=false",
             k::parseJson(smJson, &sj).empty() &&
             sj.get("schema") && sj.get("schema")->s == sel::kSelectiveReportSchema &&
             sj.get("hardwareVerified") && sj.get("hardwareVerified")->t == k::Json::T::Bool && !sj.get("hardwareVerified")->b &&
             sj.get("planFingerprint") && sj.get("planFingerprint")->s == plan.selectiveFingerprint);

        // Tamper test on SELECTIVE_MANIFEST.json: hardwareVerified:true -> verifyKit() MUST fail even after resign!
        const fs::path tamperKit = root / "TamperSelectiveKit";
        fs::copy(kitOut, tamperKit, fs::copy_options::recursive);
        std::string badSm = smJson;
        const auto pos = badSm.find("\"hardwareVerified\": false");
        if (pos != std::string::npos) badSm.replace(pos, 25, "\"hardwareVerified\": true");
        putStrFile(tamperKit / "SELECTIVE_MANIFEST.json", badSm);
        resignKit(tamperKit);
        std::string badText;
        TEST("T1: verifyKit refuses tampered SELECTIVE_MANIFEST.json (hardwareVerified=true)",
             has(k::verifyKit(tamperKit, nullptr, &badText), "hardwareVerified must be false"));
    }

    // ── Test 2: Existing Target User Sound Verification (`reuse-target`) ──
    {
        k::SelectiveKitRequest req;
        req.workspaceRoot = ws;
        req.selectedStyles = {"StyleReuse.mid"};
        req.targetKst = targetKst;

        const auto plan = k::selectivePlan(req);
        TEST("T2: existing target USER01/0 reused, executable=true", plan.ok && plan.executable());
        TEST("T2: no Sound package needed in kit (only StyleReuse.mid)",
             plan.basePlan.items.size() == 1 && plan.basePlan.items[0].kitPath() == "STYLES/StyleReuse.mid");
        TEST("T2: targetReusedSounds records USER01/0",
             plan.targetReusedSounds.size() == 1 &&
             plan.targetReusedSounds[0].slot == (sel::SlotAddress{false, 1, 0}));
        bool hasFactoryDiag = false;
        for (const auto& d : plan.report.diagnostics) {
            if (d.code == sel::diag_code::kFactoryRomSoundRef) hasFactoryDiag = true;
        }
        TEST("T2: FACTORY_ROM_SOUND_REF info diagnostic emitted", hasFactoryDiag);
    }

    // ── Test 3: Missing Referenced User Sound Blocking ──
    {
        k::SelectiveKitRequest req;
        req.workspaceRoot = ws;
        req.selectedStyles = {"StyleMissing.mid"};
        req.targetKst = targetKst;

        const auto plan = k::selectivePlan(req);
        TEST("T3: missing USER11/40 blocks plan (ok=true, executable=false)", plan.ok && !plan.executable());
        bool hasMissingDiag = false;
        for (const auto& d : plan.report.diagnostics) {
            if (d.severity == sel::DiagnosticSeverity::Blocking &&
                (d.code == sel::diag_code::kMissingUserSoundSlot || d.code == sel::diag_code::kMissingUserSoundBank)) {
                hasMissingDiag = true;
            }
        }
        TEST("T3: VERIFIED_MISSING diagnostic emitted for USER11/40", hasMissingDiag);

        const fs::path blockedOut = root / "BlockedMissingKit";
        const auto st = runJob(k::selectiveImportKit(req, blockedOut));
        TEST("T3: selectiveImportKit refuses and writes nothing",
             st.phase == Phase::Failed && !fs::exists(blockedOut) && !fs::exists(root / "BlockedMissingKit.INCOMPLETE"));
    }

    // ── Test 4: Target Slot Collision Policy Matrix (`block` vs `remap`) ──
    {
        k::SelectiveKitRequest req;
        req.workspaceRoot = ws;
        req.selectedStyles = {"StyleCollide.mid"};
        req.slotToPackageMap[{false, 1, 0}] = ws / "AIA Demo Organ"; // USER01/0 is OCCUPIED in Target.KST!
        req.targetKst = targetKst;
        req.conflictPolicy = sel::ConflictPolicy::BlockWithReport;

        const auto blocked = k::selectivePlan(req);
        bool hasOccConflict = false;
        for (const auto& c : blocked.report.conflicts) {
            if (c.blocking && c.kind == sel::SlotConflict::Kind::SoundSlotOccupiedDifferent) hasOccConflict = true;
        }
        TEST("T4a: policy=block refuses occupied USER01/0 with SoundSlotOccupiedDifferent",
             blocked.ok && !blocked.executable() && hasOccConflict);

        req.conflictPolicy = sel::ConflictPolicy::RemapToFreeSlotInNewLib;
        const auto remapped = k::selectivePlan(req);
        TEST("T4b: policy=remap allocates deterministic lowest free slot USER01/3 & executable=true",
             remapped.ok && remapped.executable() &&
             remapped.slotRecommendations.size() == 1 &&
             remapped.slotRecommendations[0].remapped &&
             remapped.slotRecommendations[0].recommendedSlot == (sel::SlotAddress{false, 1, 3}));
        TEST("T4b: enhancedKitMd lists recommended free slot USER01/3", has(remapped.enhancedKitMd, "USER01/3"));
    }

    // ── Test 5: Memory Projection (Provisional vs Target Baseline vs Identical-WAV Advisory Extra) ──
    {
        // (a) No target KST -> uncompressed_pcm_bytes, fits
        k::SelectiveKitRequest reqNoTarget;
        reqNoTarget.workspaceRoot = ws;
        reqNoTarget.selectedStyles = {"StyleA.mid"};
        reqNoTarget.slotToPackageMap[{false, 1, 3}] = ws / "AIA Demo Organ";
        reqNoTarget.slotToPackageMap[{true, 1, 0}] = ws / "AIA Demo Kit";

        const auto pNoTarget = k::selectivePlan(reqNoTarget);
        TEST("T5a: no target -> unit=uncompressed_pcm_bytes, fitVerdict=fits, proxy=false",
             pNoTarget.ok && pNoTarget.executable() &&
             !pNoTarget.report.memoryProjection.sampleBytesAreNativeStoredProxy &&
             std::string(pNoTarget.report.memoryProjection.unitName()) == "uncompressed_pcm_bytes" &&
             std::string(pNoTarget.report.memoryProjection.fitVerdictName()) == "fits" &&
             pNoTarget.report.memoryProjection.uniqueLogicalSampleBytes > 0);

        // (b) Target KST attached -> stored-PCM proxy, unverified-proxy, kStoredPcmProxyCaveat
        k::SelectiveKitRequest reqWithTarget = reqNoTarget;
        reqWithTarget.targetKst = targetKst;
        const auto pWithTarget = k::selectivePlan(reqWithTarget);
        TEST("T5b: target attached -> proxy=true, fitVerdict=unverified-proxy, baseline=12288, caveat present",
             pWithTarget.ok && pWithTarget.executable() &&
             pWithTarget.report.memoryProjection.sampleBytesAreNativeStoredProxy &&
             std::string(pWithTarget.report.memoryProjection.unitName()) == "native_kst_stored_pcm_proxy_bytes" &&
             std::string(pWithTarget.report.memoryProjection.fitVerdictName()) == "unverified-proxy" &&
             pWithTarget.report.memoryProjection.targetBaselineSampleBytes == (8192 + 4096) &&
             std::string(pWithTarget.report.memoryProjection.caveatText()) == sel::kStoredPcmProxyCaveat);

        // (c) 2 distinct packages with identical WAVs -> identicalContentExtraBytes > 0, packages unmutated
        k::SelectiveKitRequest reqDupWav;
        reqDupWav.workspaceRoot = ws;
        reqDupWav.selectedStyles = {"StyleDupWav.mid"};
        reqDupWav.slotToPackageMap[{false, 1, 3}] = ws / "AIA Demo Organ";
        reqDupWav.slotToPackageMap[{false, 1, 4}] = ws / "SecondOrgan";
        const auto pDupWav = k::selectivePlan(reqDupWav);
        TEST("T5c: distinct packages with identical WAVs -> identicalContentExtraBytes > 0, both kept",
             pDupWav.ok && pDupWav.executable() &&
             pDupWav.report.memoryProjection.identicalContentExtraBytes > 0 &&
             pDupWav.basePlan.items.size() == 3);
    }

    // ── Test 6: TOCTOU — Source Style / WAV 1-byte mutation preserving exact size & mtime! ──
    {
        const fs::path mutWs = root / "toctou-src-ws";
        fs::copy(ws, mutWs, fs::copy_options::recursive);
        k::SelectiveKitRequest req;
        req.workspaceRoot = mutWs;
        req.selectedStyles = {"StyleA.mid"};
        req.slotToPackageMap[{false, 1, 3}] = mutWs / "AIA Demo Organ";
        req.slotToPackageMap[{true, 1, 0}] = mutWs / "AIA Demo Kit";

        const auto reviewed = k::selectivePlan(req);
        TEST("T6: baseline review executable", reviewed.ok && reviewed.executable());

        // (6a) Flip 1 byte in StyleA.mid while restoring exact size AND mtime -> caught at Phase::Validate!
        const fs::path stylePath = mutWs / "StyleA.mid";
        const auto origMtime = fs::last_write_time(stylePath);
        std::string origBytes = slurp(stylePath);
        std::string mutatedBytes = origBytes;
        mutatedBytes[mutatedBytes.size() / 2] = static_cast<char>(mutatedBytes[mutatedBytes.size() / 2] ^ 0x01);
        putStrFile(stylePath, mutatedBytes);
        fs::last_write_time(stylePath, origMtime);

        const fs::path out6a = root / "ToctouStyleKit";
        const auto st6a = runJob(k::selectiveImportKit(req, out6a, reviewed.selectiveFingerprint, reviewed.basePlan.sha, ""));
        TEST("T6a: 1-byte Style mutation (same size & mtime) refused, nothing published",
             st6a.phase == Phase::Failed && !fs::exists(out6a));

        // Restore StyleA.mid & test mid-run WAV swap (same size & mtime) between Validate and Run -> caught by expectSha!
        putStrFile(stylePath, origBytes);
        fs::last_write_time(stylePath, origMtime);

        fs::path wavPath;
        for (const auto& e : fs::recursive_directory_iterator(mutWs / "AIA Demo Organ")) {
            if (e.is_regular_file() && e.path().extension() == ".wav") { wavPath = e.path(); break; }
        }
        TEST("T6b: located package WAV for mid-run TOCTOU check", !wavPath.empty());
        const auto wavMtime = fs::last_write_time(wavPath);
        const std::string wavOrig = slurp(wavPath);

        ExportSpec spec6b = k::selectiveImportKit(req, root / "ToctouWavKit", reviewed.selectiveFingerprint, reviewed.basePlan.sha, "");
        const auto origDeps = spec6b.dependencies;
        spec6b.dependencies = [&](std::vector<std::string>& notes, uint64_t& sb) {
            auto r = origDeps(notes, sb);
            // Flip 1 byte in WAV preserving exact size & mtime right before writeAt!
            std::string b = wavOrig;
            b[b.size() - 1] = static_cast<char>(b[b.size() - 1] ^ 0x5A);
            putStrFile(wavPath, b);
            fs::last_write_time(wavPath, wavMtime);
            return r;
        };
        const auto st6b = runJob(spec6b);
        TEST("T6b: mid-run 1-byte WAV mutation (same size & mtime) caught by expectSha at Run, nothing published",
             st6b.phase == Phase::Failed && st6b.stoppedAt == Phase::Run && !fs::exists(root / "ToctouWavKit"));
    }

    // ── Test 7: TOCTOU — Target KST .PCG Mutated (both before Validate AND right before beforePublish!) ──
    {
        const fs::path mutTarget = root / "ToctouTarget.KST";
        writeSyntheticTargetKst(mutTarget, false);

        k::SelectiveKitRequest req;
        req.workspaceRoot = ws;
        req.selectedStyles = {"StyleA.mid"};
        req.slotToPackageMap[{false, 1, 3}] = ws / "AIA Demo Organ";
        req.slotToPackageMap[{true, 1, 0}] = ws / "AIA Demo Kit";
        req.targetKst = mutTarget;

        const auto reviewed = k::selectivePlan(req);
        TEST("T7: baseline target plan executable", reviewed.ok && reviewed.executable());

        // (7a) Mutate targetKst/SOUND/USER01.PCG before selectiveImportKit() -> refused at Validate!
        writeSyntheticTargetKst(mutTarget, false, /*extraSlot=*/10);
        const fs::path out7a = root / "ToctouTargetValKit";
        const auto st7a = runJob(k::selectiveImportKit(req, out7a, reviewed.selectiveFingerprint, reviewed.basePlan.sha, reviewed.targetLibraryKey));
        TEST("T7a: target KST .PCG mutation between review & execute refused at Validate",
             st7a.phase == Phase::Failed && st7a.stoppedAt == Phase::Validate &&
             has(st7a.reason, "targetLibraryKey") && !fs::exists(out7a));

        // (7b) Restore targetKst, then mutate targetKst right before beforePublish!
        writeSyntheticTargetKst(mutTarget, false);
        ExportSpec spec7b = k::selectiveImportKit(req, root / "ToctouTargetPubKit", reviewed.selectiveFingerprint, reviewed.basePlan.sha, reviewed.targetLibraryKey);
        const auto origBeforePublish = spec7b.beforePublish;
        spec7b.beforePublish = [&](const fs::path& stagedOut) {
            writeSyntheticTargetKst(mutTarget, false, /*extraSlot=*/11);
            return origBeforePublish(stagedOut);
        };
        const auto st7b = runJob(spec7b);
        TEST("T7b: target KST .PCG mutation right before publish refused at Publish, nothing published",
             st7b.phase == Phase::Failed && st7b.stoppedAt == Phase::Publish &&
             has(st7b.reason, "targetLibraryKey") && !fs::exists(root / "ToctouTargetPubKit"));
    }

    // ── Test 8: D18 Overlap Guard (workspaceRoot, package, AND targetKst — direct & via symlink) ──
    {
        k::SelectiveKitRequest req;
        req.workspaceRoot = ws;
        req.selectedStyles = {"StyleA.mid"};
        req.slotToPackageMap[{false, 1, 3}] = ws / "AIA Demo Organ";
        req.slotToPackageMap[{true, 1, 0}] = ws / "AIA Demo Kit";
        req.targetKst = targetKst;

        const auto stInWs = runJob(k::selectiveImportKit(req, ws / "InsideWorkspaceKit"));
        TEST("T8a: D18 refuses dest inside workspaceRoot",
             stInWs.phase == Phase::Failed && stInWs.stoppedAt == Phase::Destination && !fs::exists(ws / "InsideWorkspaceKit"));

        const auto stInPkg = runJob(k::selectiveImportKit(req, ws / "AIA Demo Organ" / "InsidePkgKit"));
        TEST("T8b: D18 refuses dest inside package",
             stInPkg.phase == Phase::Failed && stInPkg.stoppedAt == Phase::Destination &&
             !fs::exists(ws / "AIA Demo Organ" / "InsidePkgKit"));

        const auto stInTarget = runJob(k::selectiveImportKit(req, targetKst / "InsideTargetKit"));
        TEST("T8c: D18 refuses dest inside targetKst",
             stInTarget.phase == Phase::Failed && stInTarget.stoppedAt == Phase::Destination &&
             !fs::exists(targetKst / "InsideTargetKit"));

        const fs::path targetSymlink = root / "target-link";
        fs::create_directory_symlink(targetKst, targetSymlink);
        const auto stViaLink = runJob(k::selectiveImportKit(req, targetSymlink / "InsideLinkKit"));
        TEST("T8d: D18 refuses dest inside targetKst via symlink",
             stViaLink.phase == Phase::Failed && !fs::exists(targetKst / "InsideLinkKit"));

        // Also verify kit::checkDestination catches targetKst overlap
        const auto dc = k::checkDestination(targetKst, "CheckDestKit", 1024, {ws, targetKst});
        TEST("T8e: checkDestination refuses targetKst overlap", !dc.ok);
    }

    // ── Test 9: Target OC31 RAM.KMP (0x11050038) Graceful Refusal with PartialNativeInspect ──
    {
        k::SelectiveKitRequest req;
        req.workspaceRoot = ws;
        req.selectedStyles = {"StyleA.mid"};
        req.slotToPackageMap[{false, 1, 3}] = ws / "AIA Demo Organ";
        req.slotToPackageMap[{true, 1, 0}] = ws / "AIA Demo Kit";
        req.targetKst = oc31TargetKst;

        const auto plan = k::selectivePlan(req);
        bool hasOc31Diag = false, hasUninspectConflict = false;
        for (const auto& d : plan.report.diagnostics) {
            if (d.code == sel::diag_code::kOc31KmpUnsupported && d.severity == sel::DiagnosticSeverity::Blocking) hasOc31Diag = true;
        }
        for (const auto& c : plan.report.conflicts) {
            if (c.kind == sel::SlotConflict::Kind::TargetUninspectable && c.blocking) hasUninspectConflict = true;
        }
        TEST("T9: OC31 KMP target -> ok=true, executable=false, TargetUninspectable + OC31_KMP_UNSUPPORTED + PartialNativeInspect",
             plan.ok && !plan.executable() && hasOc31Diag && hasUninspectConflict &&
             plan.targetPartialInspect.isPartial && plan.targetPartialInspect.kmpIsOc31 &&
             plan.targetPartialInspect.occupiedUserSounds == 3 && plan.targetPartialInspect.pcmContainerCount == 1);

        const fs::path out9 = root / "Oc31RefusedKit";
        const auto st9 = runJob(k::selectiveImportKit(req, out9));
        TEST("T9: selectiveImportKit refuses OC31 target and writes nothing",
             st9.phase == Phase::Failed && !fs::exists(out9) && !fs::exists(root / "Oc31RefusedKit.INCOMPLETE"));
    }

    // ── Test 10: Cooperative Cancellation (`keepGoing`) ──
    {
        k::SelectiveKitRequest req;
        req.workspaceRoot = ws;
        req.selectedStyles = {"StyleA.mid", "StyleB.mid"};
        req.slotToPackageMap[{false, 1, 3}] = ws / "AIA Demo Organ";
        req.slotToPackageMap[{true, 1, 0}] = ws / "AIA Demo Kit";
        req.targetKst = targetKst;

        int calls = 0;
        const auto cancelled = k::selectivePlan(req, [&] { return ++calls <= 2; });
        TEST("T10: keepGoing cancellation drops partial state cleanly",
             !cancelled.ok && cancelled.cancelled && !cancelled.executable() &&
             cancelled.basePlan.items.empty() && cancelled.slotRecommendations.empty() &&
             !cancelled.blockingReasons.empty());
    }

    // ── Test 11: Determinism Across Shuffled Input Orders + Zero Source/Target Mutation ──
    {
        k::SelectiveKitRequest req1;
        req1.workspaceRoot = ws;
        req1.selectedStyles = {"StyleA.mid", "StyleB.mid"};
        req1.packageInputs = {ws / "AIA Demo Organ", ws / "AIA Demo Kit", ws / "UnusedOrg"};
        req1.slotToPackageMap[{false, 1, 3}] = ws / "AIA Demo Organ";
        req1.slotToPackageMap[{true, 1, 0}] = ws / "AIA Demo Kit";
        req1.targetKst = targetKst;

        k::SelectiveKitRequest req2 = req1;
        req2.selectedStyles = {"StyleB.mid", "StyleA.mid"};
        req2.packageInputs = {ws / "UnusedOrg", ws / "AIA Demo Kit", ws / "AIA Demo Organ"};

        const auto p1 = k::selectivePlan(req1);
        const auto p2 = k::selectivePlan(req2);
        TEST("T11a: byte-identical fingerprint, JSON, and KIT.md across shuffled inputs",
             p1.ok && p2.ok &&
             p1.selectiveFingerprint == p2.selectiveFingerprint &&
             p1.selectiveManifestJson == p2.selectiveManifestJson &&
             p1.enhancedKitMd == p2.enhancedKitMd &&
             p1.slotRecommendations == p2.slotRecommendations);
        TEST("T11b: zero local filesystem paths leaked into SELECTIVE_MANIFEST.json",
             !has(p1.selectiveManifestJson, root.string()) &&
             !has(p1.selectiveManifestJson, "/Users/") &&
             !has(p1.selectiveManifestJson, "/tmp/"));
        TEST("T11c: workspace and Target.KST 100% byte-identical (strictly read-only)",
             snapshotDir(ws) == wsBefore && snapshotDir(targetKst) == targetBefore);
    }

    fs::remove_all(root);
    std::printf("Summary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
