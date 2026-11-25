// M2a Synthetic Regression Tests:
//   - Structured Missing-Resource Diagnostics & Canonical Code Catalog (selective_diagnostics.h)
//   - Deterministic JSON (aiarranger-selective-export-report/1) & Markdown Serializers
//   - Target Sound Bank/Slot Occupancy Grid, Strict 3-Way Sound Equivalence Fingerprinting,
//     Deterministic Canonical Free-Slot Allocator & Complete Collision Matrix (slot_conflicts.h)
//
// 100% synthetic, original fixtures only (zero owner data, zero real Sound/Style/Sample names,
// zero proprietary bytes, zero committed local paths). Strictly read-only core logic.

#include "media/sound/selective_diagnostics.h"
#include "media/sound/slot_conflicts.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "jobs/import_kit.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ai_arranger::media::selective;
using namespace ai_arranger::media::graph;
using namespace ai_arranger::media::panative;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
static void check(const char* name, bool ok) {
    if (!ok) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; }
    else { std::printf("  PASS: %s\n", name); ++passes; }
}
#define TEST(name, ...) check((name), (__VA_ARGS__))

using Bytes = std::vector<uint8_t>;

// ── Synthetic Binary Builders (100% original test-only helpers) ───────────────────────────────────────────────────────

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
    for (size_t i = 0; i < s.size() && at + i < b.size(); ++i) b[at + i] = static_cast<uint8_t>(s[i]);
}
static void appendStr(Bytes& b, const std::string& s) {
    b.insert(b.end(), reinterpret_cast<const uint8_t*>(s.data()), reinterpret_cast<const uint8_t*>(s.data()) + s.size());
}
static Id8 makeId(uint8_t tag, uint8_t idx) {
    return Id8{tag, idx, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
}
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
        rec.push_back(0x10); rec.push_back(0x20); rec.push_back(0x30); rec.push_back(0x40);
        pushBe16(dir, static_cast<uint16_t>(rec.size()));
        dir.insert(dir.end(), rec.begin(), rec.end());
    }
    Bytes hdr(12, 0);
    putStr(hdr, 7, "KORF");
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

static Bytes buildRawPa5xKmpPayload(const std::vector<Pa5xSample>& samples,
                                    const std::vector<Pa5xDrumRecord>& drums,
                                    const std::vector<Pa5xZone>& zones,
                                    const std::vector<Pa5xMultisample>& multisamples) {
    Bytes b;
    pushBe16(b, 0x0103u);
    pushBe32(b, static_cast<uint32_t>(samples.size()));
    pushBe32(b, static_cast<uint32_t>(drums.size()));
    pushBe32(b, static_cast<uint32_t>(zones.size()));
    pushBe32(b, static_cast<uint32_t>(multisamples.size()));
    for (const auto& s : samples) {
        Bytes rec(122, 0);
        putStr(rec, 75, s.name);
        std::copy(s.id.begin(), s.id.end(), rec.begin() + 107);
        rec[119] = static_cast<uint8_t>(s.rate >> 8);
        rec[120] = static_cast<uint8_t>(s.rate & 0xFFu);
        b.insert(b.end(), rec.begin(), rec.end());
    }
    for (const auto& d : drums) {
        Bytes rec(126, 0);
        putStr(rec, 0, d.name);
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
        putStr(rec, 0, m.name);
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

static Bytes buildSoundBody(uint8_t tagByte, const Id8& msHigh, const Id8& msLow) {
    // base=16, record=24, pairOffset=12 (1 record -> 40 bytes)
    Bytes body(40, tagByte);
    std::copy(msHigh.begin(), msHigh.end(), body.begin() + 16);
    std::copy(msLow.begin(), msLow.end(), body.begin() + 28);
    return body;
}

static SoundEquivalenceProfile makeAnalysedProfile(uint32_t kindChunk, const std::string& bodySeed,
                                                   uint8_t msIdx, uint8_t topKey, uint8_t smpIdx,
                                                   uint32_t pcmBytes) {
    SoundEquivalenceProfile p;
    p.kindChunkId = kindChunk;
    p.bodySha256 = conflict_detail::sha256Bytes(reinterpret_cast<const uint8_t*>(bodySeed.data()), bodySeed.size());
    p.decoded = true;
    p.analysed = true;
    p.isDrumKit = false;
    p.kmpComplete = true;
    p.unresolvedSlotCount = 0;
    MultisampleFingerprint ms;
    ms.multisampleId = makeId(0xA0, msIdx);
    ms.zones.push_back({topKey, true, makeId(0xB0, smpIdx), pcmBytes, pcmBytes > 0});
    p.referencedMultisamples.push_back(std::move(ms));
    finalizeSoundEquivalenceProfile(p);
    return p;
}

static void writeFileBytes(const fs::path& p, const Bytes& data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

struct TempDirGuard {
    fs::path dir;
    explicit TempDirGuard(const std::string& tag) {
        const auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() / ("aiarr_m2a_" + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(ns));
        fs::create_directories(dir);
    }
    ~TempDirGuard() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// ── Test 1: Diagnostic Taxonomy & Canonical Catalog ───────────────────────────────────────────────────────────────────

static void testDiagnosticCatalog() {
    std::printf("[Test 1] Diagnostic taxonomy & canonical code catalog\n");
    TEST("category names match canonical 4 values",
         std::string(diagnosticCategoryName(DiagnosticCategory::VERIFIED_MISSING)) == "VERIFIED_MISSING" &&
         std::string(diagnosticCategoryName(DiagnosticCategory::UNSUPPORTED_FORMAT)) == "UNSUPPORTED_FORMAT" &&
         std::string(diagnosticCategoryName(DiagnosticCategory::FACTORY_ASSUMED_EXTERNAL)) == "FACTORY_ASSUMED_EXTERNAL" &&
         std::string(diagnosticCategoryName(DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE)) == "AMBIGUOUS_OR_DUPLICATE");

    const auto& cat = diagnosticCatalog();
    TEST("canonical catalog has >= 20 codes covering all 4 categories", cat.size() >= 20);
    std::set<DiagnosticCategory> seenCats;
    for (const auto& s : cat) seenCats.insert(s.category);
    TEST("all 4 categories present in catalog", seenCats.size() == 4);

    const auto* oc31 = findDiagnosticCodeSpec(diag_code::kOc31KmpUnsupported);
    TEST("OC31_KMP_UNSUPPORTED spec is UNSUPPORTED_FORMAT/Blocking/Unsupported",
         oc31 != nullptr && oc31->category == DiagnosticCategory::UNSUPPORTED_FORMAT &&
         oc31->defaultSeverity == DiagnosticSeverity::Blocking && oc31->defaultEvidence == EvidenceLevel::Unsupported);

    const auto* r18 = findDiagnosticCodeSpec(diag_code::kFatCaseCollisionR18);
    TEST("FAT_CASE_COLLISION_R18 spec is AMBIGUOUS_OR_DUPLICATE/Blocking/Verified",
         r18 != nullptr && r18->category == DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE &&
         r18->defaultSeverity == DiagnosticSeverity::Blocking && r18->defaultEvidence == EvidenceLevel::Verified);

    const auto* fac = findDiagnosticCodeSpec(diag_code::kFactoryRomSoundRef);
    TEST("FACTORY_ROM_SOUND_REF spec is FACTORY_ASSUMED_EXTERNAL/Info/Inferred",
         fac != nullptr && fac->category == DiagnosticCategory::FACTORY_ASSUMED_EXTERNAL &&
         fac->defaultSeverity == DiagnosticSeverity::Info && fac->defaultEvidence == EvidenceLevel::Inferred);
}

// ── Test 2: Strict 3-Way Sound Equivalence Fingerprinting ─────────────────────────────────────────────────────────────

static void testSoundEquivalence() {
    std::printf("[Test 2] Strict 3-way Sound equivalence (Identical vs Different vs InsufficientEvidence)\n");

    const SoundEquivalenceProfile a = makeAnalysedProfile(0x10050038u, "body-alpha", 1, 72, 1, 4096);
    const SoundEquivalenceProfile aSame = makeAnalysedProfile(0x10050038u, "body-alpha", 1, 72, 1, 4096);
    TEST("identical analysed Sound profiles -> Identical + matching digest",
         compareSoundEquivalence(a, aSame).verdict == EquivalenceVerdict::Identical &&
         !a.equivalenceDigest.empty() && a.equivalenceDigest == aSame.equivalenceDigest);

    // Same Sound name in collision requests, DIFFERENT body -> NEVER Identical!
    const SoundEquivalenceProfile diffBody = makeAnalysedProfile(0x10050038u, "body-beta", 1, 72, 1, 4096);
    TEST("same name/slot but different body SHA-256 -> Different",
         compareSoundEquivalence(a, diffBody).verdict == EquivalenceVerdict::Different &&
         a.equivalenceDigest != diffBody.equivalenceDigest);

    // Different chunk kind
    const SoundEquivalenceProfile diffKind = makeAnalysedProfile(0x10060038u, "body-alpha", 1, 72, 1, 4096);
    TEST("different chunk kind -> Different",
         compareSoundEquivalence(a, diffKind).verdict == EquivalenceVerdict::Different);

    // Same body & same MS/Sample IDs, DIFFERENT zone topKey -> Different!
    const SoundEquivalenceProfile diffTopKey = makeAnalysedProfile(0x10050038u, "body-alpha", 1, 75, 1, 4096);
    TEST("different zone topKey -> Different",
         compareSoundEquivalence(a, diffTopKey).verdict == EquivalenceVerdict::Different);

    // Same body & same MS/Sample IDs & topKey, DIFFERENT PCM payload size -> Different!
    const SoundEquivalenceProfile diffPcmSize = makeAnalysedProfile(0x10050038u, "body-alpha", 1, 72, 1, 8192);
    TEST("different Sample PCM payload size -> Different",
         compareSoundEquivalence(a, diffPcmSize).verdict == EquivalenceVerdict::Different);

    // Different Sample Id8 -> Different!
    const SoundEquivalenceProfile diffSmpId = makeAnalysedProfile(0x10050038u, "body-alpha", 1, 72, 2, 4096);
    TEST("different Sample Id8 -> Different",
         compareSoundEquivalence(a, diffSmpId).verdict == EquivalenceVerdict::Different);

    // Unanalysed USERDK (even with identical decoded body SHA-256!) -> InsufficientEvidence!
    SoundEquivalenceProfile dk1 = a;
    dk1.isDrumKit = true;
    dk1.analysed = false;
    dk1.unanalysedReason = "SizeDoesNotFit";
    finalizeSoundEquivalenceProfile(dk1);
    SoundEquivalenceProfile dk2 = dk1;
    TEST("unanalysed USERDK with identical body SHA -> InsufficientEvidence & empty digest",
         compareSoundEquivalence(dk1, dk2).verdict == EquivalenceVerdict::InsufficientEvidence &&
         dk1.equivalenceDigest.empty());

    // Analysed Sound with >0 unresolved slots -> InsufficientEvidence!
    SoundEquivalenceProfile unres = a;
    unres.unresolvedSlotCount = 1;
    finalizeSoundEquivalenceProfile(unres);
    TEST("analysed Sound with unresolved oscillator slot -> InsufficientEvidence",
         compareSoundEquivalence(a, unres).verdict == EquivalenceVerdict::InsufficientEvidence &&
         unres.equivalenceDigest.empty());

    // NoKnownReference -> InsufficientEvidence!
    SoundEquivalenceProfile noRef = a;
    noRef.analysed = false;
    noRef.unanalysedReason = "NoKnownReference";
    noRef.referencedMultisamples.clear();
    finalizeSoundEquivalenceProfile(noRef);
    TEST("NoKnownReference Sound -> InsufficientEvidence",
         compareSoundEquivalence(noRef, noRef).verdict == EquivalenceVerdict::InsufficientEvidence);

    // OC31 KMP unavailable (kmpComplete == false) -> InsufficientEvidence!
    SoundEquivalenceProfile oc31P = a;
    oc31P.kmpComplete = false;
    finalizeSoundEquivalenceProfile(oc31P);
    TEST("kmpComplete==false (OC31 KMP) -> InsufficientEvidence",
         compareSoundEquivalence(oc31P, a).verdict == EquivalenceVerdict::InsufficientEvidence);
}

// ── Test 3: Complete Collision Matrix Across All 3 Policies ───────────────────────────────────────────────────────────

static void testCollisionMatrixAllPolicies() {
    std::printf("[Test 3] Complete CollisionMatrix across BlockWithReport / ReuseIdenticalTargetSlot / RemapToFreeSlotInNewLib\n");

    TargetOccupancyMap target = TargetOccupancyMap::makeEmpty(true);
    const SoundEquivalenceProfile pLead = makeAnalysedProfile(0x10050038u, "lead-body", 1, 84, 1, 2048);
    const SoundEquivalenceProfile pBrassA = makeAnalysedProfile(0x10050038u, "brass-body-A", 2, 72, 2, 4096);
    const SoundEquivalenceProfile pBrassB = makeAnalysedProfile(0x10050038u, "brass-body-B", 3, 72, 3, 4096);
    const SoundEquivalenceProfile pPad = makeAnalysedProfile(0x10050038u, "pad-body", 4, 60, 4, 8192);

    // Target occupies USER01/0 (pLead), USER01/1 (pBrassA), USER02/5 (pPad), USER01/2 (pBrassA_clone)
    target.addOccupiedSlot({{false, 1, 0}, "Target Lead", "SOUND/USER01.PCG", pLead});
    target.addOccupiedSlot({{false, 1, 1}, "Brass 1", "SOUND/USER01.PCG", pBrassA});
    target.addOccupiedSlot({{false, 2, 5}, "Renamed Pad", "SOUND/USER02.PCG", pPad});
    // Also place an unanalysed USERDK at USERDK01/0
    SoundEquivalenceProfile dkProf;
    dkProf.kindChunkId = 0x10050038u;
    dkProf.bodySha256 = conflict_detail::sha256Bytes(reinterpret_cast<const uint8_t*>("dk-same"), 7);
    dkProf.decoded = true;
    dkProf.isDrumKit = true;
    dkProf.analysed = false;
    dkProf.unanalysedReason = "SizeDoesNotFit";
    target.addOccupiedSlot({{true, 1, 0}, "Kit 1", "SOUND/USERDK01.PCG", dkProf});
    target.rebuildIndexesAndKey();

    // Build source requests covering all matrix cases:
    //  R1: USER01/10 (free in target)
    //  R2: USER01/0  (identical Sound at exact same slot, even with different source name!)
    //  R3: USER01/1  (occupied by pBrassA in target, source wants pPad which IS identical at USER02/5!)
    //  R4: USER02/5  (occupied by pPad in target, source wants pBrassB with SAME NAME "Brass 1" — NO match in target!)
    //  R5: USERDK01/0 (occupied by unanalysed USERDK — same body SHA & name, MUST be InsufficientEvidence!)
    CollisionMatrixInput base;
    base.soundRequests.push_back({"pa5x:USER01/Sound/10", {false, 1, 10}, "Free Piano", pBrassB, false, {"STYLE/B1/S1.stg"}});
    base.soundRequests.push_back({"pa5x:USER01/Sound/0",  {false, 1, 0},  "Different Name Lead", pLead, false, {"STYLE/B1/S1.stg"}});
    base.soundRequests.push_back({"pa5x:USER01/Sound/1",  {false, 1, 1},  "Source Pad", pPad, false, {"STYLE/B1/S1.stg"}});
    base.soundRequests.push_back({"pa5x:USER02/Sound/5",  {false, 2, 5},  "Brass 1", pBrassB, false, {"STYLE/B1/S1.stg"}});
    base.soundRequests.push_back({"pa5x:USERDK01/Sound/0",{true, 1, 0},   "Kit 1", dkProf, false, {"STYLE/B1/S1.stg"}});

    auto findRes = [](const CollisionMatrixResult& r, const std::string& id) -> const SoundResolutionEntry* {
        for (const auto& s : r.soundResolutions) if (s.requestId == id) return &s;
        return nullptr;
    };

    // Policy 1: BlockWithReport
    {
        CollisionMatrixInput in = base;
        in.policy = ConflictPolicy::BlockWithReport;
        const auto r = evaluateCollisionMatrix(target, in);
        const auto* r1 = findRes(r, "pa5x:USER01/Sound/10");
        const auto* r2 = findRes(r, "pa5x:USER01/Sound/0");
        const auto* r3 = findRes(r, "pa5x:USER01/Sound/1");
        const auto* r4 = findRes(r, "pa5x:USER02/Sound/5");
        const auto* r5 = findRes(r, "pa5x:USERDK01/Sound/0");
        TEST("BlockWithReport: free slot allowed & preserved", r1 && !r1->blocking && r1->action == "allow-free" && r1->resolvedAddress == SlotAddress{false, 1, 10});
        TEST("BlockWithReport: identical at same slot reused with 0 remap", r2 && !r2->blocking && r2->action == "reuse-target" && !r2->remapped);
        TEST("BlockWithReport: identical elsewhere blocked with hint", r3 && r3->blocking && r3->detail.find("USER02/5") != std::string::npos);
        TEST("BlockWithReport: occupied different blocked (never overwritten)", r4 && r4->blocking && r4->equivalence == EquivalenceVerdict::Different);
        TEST("BlockWithReport: unanalysed USERDK blocked as InsufficientEvidence", r5 && r5->blocking && r5->equivalence == EquivalenceVerdict::InsufficientEvidence);
    }

    // Policy 2: ReuseIdenticalTargetSlot
    {
        CollisionMatrixInput in = base;
        in.policy = ConflictPolicy::ReuseIdenticalTargetSlot;
        const auto r = evaluateCollisionMatrix(target, in);
        const auto* r3 = findRes(r, "pa5x:USER01/Sound/1");
        const auto* r4 = findRes(r, "pa5x:USER02/Sound/5");
        const auto* r5 = findRes(r, "pa5x:USERDK01/Sound/0");
        TEST("ReuseIdenticalTargetSlot: remaps R3 to proven-identical USER02/5",
             r3 && !r3->blocking && r3->remapped && r3->action == "remap-reuse-target" &&
             r3->resolvedAddress == SlotAddress{false, 2, 5});
        TEST("ReuseIdenticalTargetSlot: still blocks R4 (different body)", r4 && r4->blocking);
        TEST("ReuseIdenticalTargetSlot: NEVER auto-reuses unanalysed USERDK R5", r5 && r5->blocking && r5->equivalence == EquivalenceVerdict::InsufficientEvidence);
        bool hasSongBookWarn = false;
        for (const auto& d : r.diagnostics) if (d.code == diag_code::kSongBookUnrewritten) hasSongBookWarn = true;
        TEST("ReuseIdenticalTargetSlot: emits SONGBOOK_UNREWRITTEN warning on remap", hasSongBookWarn);
    }

    // Policy 3: RemapToFreeSlotInNewLib
    {
        CollisionMatrixInput in = base;
        in.policy = ConflictPolicy::RemapToFreeSlotInNewLib;
        const auto r = evaluateCollisionMatrix(target, in);
        const auto* r3 = findRes(r, "pa5x:USER01/Sound/1");
        const auto* r4 = findRes(r, "pa5x:USER02/Sound/5");
        const auto* r5 = findRes(r, "pa5x:USERDK01/Sound/0");
        TEST("RemapToFreeSlotInNewLib: prefers reusing proven-identical USER02/5 for R3",
             r3 && !r3->blocking && r3->action == "remap-reuse-target" && r3->resolvedAddress == SlotAddress{false, 2, 5});
        TEST("RemapToFreeSlotInNewLib: remaps R4 to lowest free slot in same bank (USER02/0)",
             r4 && !r4->blocking && r4->remapped && r4->action == "remap-free-slot" &&
             r4->resolvedAddress == SlotAddress{false, 2, 0});
        TEST("RemapToFreeSlotInNewLib: remaps unanalysed USERDK R5 to free USERDK01/1 (never overwrites USERDK01/0)",
             r5 && !r5->blocking && r5->remapped && r5->action == "remap-free-slot" &&
             r5->resolvedAddress == SlotAddress{true, 1, 1});
        TEST("RemapToFreeSlotInNewLib: zero blocking conflicts remain", !r.hasBlockingConflicts());
    }
}

// ── Test 4: 1,536-Slot User Sound & 768-Slot User Drum Kit Exhaustion + Same-Bank/Next-Bank Fallback ─────────────────

static void testGridExhaustionAndAllocator() {
    std::printf("[Test 4] 1,536-slot USER01..12 & 768-slot USERDK01..06 exhaustion and canonical allocation\n");

    // 4a. Same-bank full -> spills to next ascending bank (lowest free slot)
    {
        TargetOccupancyMap target = TargetOccupancyMap::makeEmpty(true);
        const SoundEquivalenceProfile occ = makeAnalysedProfile(0x10050038u, "occ", 1, 60, 1, 1024);
        // Fill all 128 slots of USER01 and slot 0 of USER02
        for (uint32_t s = 0; s < kSlotsPerBank; ++s) {
            target.addOccupiedSlot({{false, 1, s}, "Occ", "SOUND/USER01.PCG", occ});
        }
        target.addOccupiedSlot({{false, 2, 0}, "Occ2", "SOUND/USER02.PCG", occ});
        target.rebuildIndexesAndKey();

        CanonicalSlotAllocator alloc(target);
        bool preserved = true;
        const auto assigned = alloc.allocateOne({false, 1, 42}, &preserved);
        TEST("full USER01 spills to lowest free in USER02 -> USER02/1",
             assigned.has_value() && !preserved && *assigned == SlotAddress{false, 2, 1});
    }

    // 4b. Exhaust all 1,536 User Sound slots -> fail-closed BANK_GRID_EXHAUSTED
    {
        TargetOccupancyMap fullSounds = TargetOccupancyMap::makeEmpty(true);
        const SoundEquivalenceProfile occ = makeAnalysedProfile(0x10050038u, "fill-sound", 1, 60, 1, 1024);
        for (uint32_t b = 1; b <= kUserSoundBankCount; ++b) {
            for (uint32_t s = 0; s < kSlotsPerBank; ++s) {
                fullSounds.addOccupiedSlot({{false, b, s}, "Full", "SOUND/USER.PCG", occ});
            }
        }
        fullSounds.rebuildIndexesAndKey();
        TEST("1,536 User Sound slots occupied, 0 free; 768 DK slots still free",
             fullSounds.occupiedCount(false) == kTotalUserSoundSlots &&
             fullSounds.freeCount(false) == 0 &&
             fullSounds.freeCount(true) == kTotalUserDrumKitSlots);

        CollisionMatrixInput in;
        in.policy = ConflictPolicy::RemapToFreeSlotInNewLib;
        const SoundEquivalenceProfile wantNew = makeAnalysedProfile(0x10050038u, "brand-new-sound", 9, 72, 9, 2048);
        in.soundRequests.push_back({"pa5x:USER01/Sound/0", {false, 1, 0}, "New Sound", wantNew, false, {"STYLE/B1/S.stg"}});
        const auto r = evaluateCollisionMatrix(fullSounds, in);
        bool hasExhaustedDiag = false, hasExhaustedConflict = false;
        for (const auto& d : r.diagnostics) if (d.code == diag_code::kBankGridExhausted && d.severity == DiagnosticSeverity::Blocking) hasExhaustedDiag = true;
        for (const auto& c : r.conflicts) if (c.kind == SlotConflict::Kind::BankGridExhausted && c.blocking) hasExhaustedConflict = true;
        TEST("1,536-slot exhaustion fails closed with BANK_GRID_EXHAUSTED",
             r.hasBlockingConflicts() && hasExhaustedDiag && hasExhaustedConflict &&
             r.soundResolutions.size() == 1 && !r.soundResolutions[0].resolvedAddress.has_value());
    }

    // 4c. Exhaust all 768 User Drum Kit slots -> fail-closed BANK_GRID_EXHAUSTED
    {
        TargetOccupancyMap fullDk = TargetOccupancyMap::makeEmpty(true);
        SoundEquivalenceProfile dk;
        dk.isDrumKit = true;
        dk.kindChunkId = 0x10050038u;
        for (uint32_t b = 1; b <= kUserDrumKitBankCount; ++b) {
            for (uint32_t s = 0; s < kSlotsPerBank; ++s) {
                fullDk.addOccupiedSlot({{true, b, s}, "DK", "SOUND/USERDK.PCG", dk});
            }
        }
        fullDk.rebuildIndexesAndKey();
        TEST("768 User Drum Kit slots occupied, 0 free",
             fullDk.occupiedCount(true) == kTotalUserDrumKitSlots && fullDk.freeCount(true) == 0);

        CollisionMatrixInput in;
        in.policy = ConflictPolicy::RemapToFreeSlotInNewLib;
        in.soundRequests.push_back({"pa5x:USERDK01/Sound/0", {true, 1, 0}, "New DK", dk, false, {"STYLE/B1/S.stg"}});
        const auto r = evaluateCollisionMatrix(fullDk, in);
        bool hasExhausted = false;
        for (const auto& d : r.diagnostics) if (d.code == diag_code::kBankGridExhausted) hasExhausted = true;
        TEST("768-slot USERDK exhaustion fails closed with BANK_GRID_EXHAUSTED",
             r.hasBlockingConflicts() && hasExhausted && !r.soundResolutions[0].resolvedAddress.has_value());
    }
}

// ── Test 5: USERDK05 Absent vs Empty Slot & Out-of-Spec LSB 70..75 ────────────────────────────────────────────────────

static void testBankAbsentAndOutOfSpecDrumKit() {
    std::printf("[Test 5] USERDK05 absent vs empty slot & MSB 120 LSB 70..75 out-of-spec flagging\n");

    TargetOccupancyMap target = TargetOccupancyMap::makeEmpty(true);
    target.userDrumKitBankPresent[4] = false;  // USERDK05.PCG absent!
    target.rebuildIndexesAndKey();

    TEST("USERDK01/3 is Free while USERDK05/3 is BankFileAbsent",
         target.slotState({true, 1, 3}) == SlotState::Free &&
         target.slotState({true, 5, 3}) == SlotState::BankFileAbsent);

    const auto outOfSpecAddr = SlotAddress::fromMidi(120, 72, 10);  // USERDK09/10 (MSB 120 LSB 72)
    TEST("MSB 120 LSB 72 parses as out-of-spec USERDK09",
         outOfSpecAddr.has_value() && outOfSpecAddr->isOutOfSpecDrumKitBank() &&
         !outOfSpecAddr->isInSpec() && outOfSpecAddr->bankName() == "USERDK09");

    CollisionMatrixInput in;
    in.policy = ConflictPolicy::BlockWithReport;
    SoundEquivalenceProfile dk;
    dk.isDrumKit = true;
    // 1. Style expects USERDK05/3 to exist -> MISSING_USER_SOUND_BANK
    in.soundRequests.push_back({"pa5x:USERDK05/Sound/3", {true, 5, 3}, "Missing Bank DK", dk, true, {"STYLE/B1/S1.stg"}});
    // 2. Style expects USERDK01/3 to exist (bank present, slot empty) -> MISSING_USER_SOUND_SLOT
    in.soundRequests.push_back({"pa5x:USERDK01/Sound/3", {true, 1, 3}, "Empty Slot DK", dk, true, {"STYLE/B1/S1.stg"}});
    // 3. Out-of-spec LSB 72 (USERDK09/10) -> OUT_OF_SPEC_DRUMKIT_BANK + MISSING_USER_SOUND_BANK
    in.soundRequests.push_back({"pa5x:USERDK09/Sound/10", *outOfSpecAddr, "OutOfSpec DK", dk, false, {"STYLE/B1/S1.stg"}});

    const auto r = evaluateCollisionMatrix(target, in);
    bool sawMissingBank = false, sawMissingSlot = false, sawOutOfSpec = false;
    for (const auto& d : r.diagnostics) {
        if (d.code == diag_code::kMissingUserSoundBank && d.subject == "pa5x:USERDK05/Sound/3") sawMissingBank = true;
        if (d.code == diag_code::kMissingUserSoundSlot && d.subject == "pa5x:USERDK01/Sound/3") sawMissingSlot = true;
        if (d.code == diag_code::kOutOfSpecDrumkitBank && d.subject == "pa5x:USERDK09/Sound/10") sawOutOfSpec = true;
    }
    TEST("USERDK05 absent -> MISSING_USER_SOUND_BANK", sawMissingBank);
    TEST("USERDK01 empty slot -> MISSING_USER_SOUND_SLOT", sawMissingSlot);
    TEST("MSB 120 LSB 72 -> OUT_OF_SPEC_DRUMKIT_BANK flagged & blocked", sawOutOfSpec && r.hasBlockingConflicts());
}

// ── Test 6: FAT32/exFAT R-18 Case Collisions & Object Path Matrix ─────────────────────────────────────────────────────

static void testFatCaseCollisionsR18() {
    std::printf("[Test 6] FAT32/exFAT R-18 case-folded object path collisions & overrides\n");

    const std::string shaA = conflict_detail::sha256Bytes(reinterpret_cast<const uint8_t*>("style-bytes-A"), 13);
    const std::string shaB = conflict_detail::sha256Bytes(reinterpret_cast<const uint8_t*>("style-bytes-B"), 13);

    TargetOccupancyMap target = TargetOccupancyMap::makeEmpty(true);
    target.addObjectPath("STYLE/Pop/Ballad.stg", shaA, 13);
    target.addObjectPath("KEYBOARDSET/Fav/Piano.ksc", shaA, 13);
    target.addObjectPath("PAD/Perc/Shaker.pdc", shaA, 13);
    target.rebuildIndexesAndKey();

    // Case-only diff in target (even with SAME SHA!) -> BLOCK under ALL 3 policies
    for (ConflictPolicy pol : {ConflictPolicy::BlockWithReport, ConflictPolicy::ReuseIdenticalTargetSlot, ConflictPolicy::RemapToFreeSlotInNewLib}) {
        CollisionMatrixInput in;
        in.policy = pol;
        in.objectRequests.push_back({SelectedKind::Style, "STYLE/POP/ballad.stg", "", "", shaA, {}});
        const auto r = evaluateCollisionMatrix(target, in);
        bool hasR18 = false;
        for (const auto& d : r.diagnostics) if (d.code == diag_code::kFatCaseCollisionR18 && d.severity == DiagnosticSeverity::Blocking) hasR18 = true;
        TEST("target case-folded R-18 collision blocked under policy",
             r.hasBlockingConflicts() && hasR18 && r.objectResolutions[0].action == "blocked-fat-case-r18");
    }

    // Intra-batch case collision -> BLOCK
    {
        CollisionMatrixInput in;
        in.policy = ConflictPolicy::RemapToFreeSlotInNewLib;
        in.objectRequests.push_back({SelectedKind::Style, "STYLE/Bank1/Demo.stg", "", "", shaA, {}});
        in.objectRequests.push_back({SelectedKind::Style, "STYLE/BANK1/demo.stg", "", "", shaB, {}});
        const auto r = evaluateCollisionMatrix(target, in);
        TEST("intra-batch case-folded R-18 collision blocked",
             r.hasBlockingConflicts() && r.objectResolutions.size() == 2 &&
             r.objectResolutions[0].blocking && r.objectResolutions[1].blocking);
    }

    // Exact path + identical SHA -> non-blocking WARN (already-present-noop)
    {
        CollisionMatrixInput in;
        in.policy = ConflictPolicy::BlockWithReport;
        in.objectRequests.push_back({SelectedKind::KeyboardSet, "KEYBOARDSET/Fav/Piano.ksc", "", "", shaA, {}});
        const auto r = evaluateCollisionMatrix(target, in);
        TEST("exact path + identical SHA -> already-present-noop (non-blocking warning)",
             !r.hasBlockingConflicts() && r.objectResolutions[0].action == "already-present-noop");
    }

    // Exact path + different SHA -> BLOCK; with explicit targetName override -> ALLOW + SongBook warning
    {
        CollisionMatrixInput in;
        in.policy = ConflictPolicy::RemapToFreeSlotInNewLib;
        in.objectRequests.push_back({SelectedKind::Pad, "PAD/Perc/Shaker.pdc", "", "", shaB, {}});
        const auto rBlocked = evaluateCollisionMatrix(target, in);
        TEST("exact path + different SHA -> blocked under RemapToFreeSlotInNewLib",
             rBlocked.hasBlockingConflicts() && rBlocked.objectResolutions[0].action == "blocked-collision");

        in.objectRequests[0].targetName = "Shaker_v2.pdc";
        const auto rRenamed = evaluateCollisionMatrix(target, in);
        TEST("explicit targetName override resolves path collision cleanly",
             !rRenamed.hasBlockingConflicts() && rRenamed.objectResolutions[0].action == "allow-new" &&
             rRenamed.objectResolutions[0].targetRelPath == "PAD/Perc/Shaker_v2.pdc");
    }
}

// ── Test 7: Read-Only Disk Folder Inspection (Raw Pa5X KMP, OC31 KMP PartialInspect, Corrupt PCG, Symlinks) ──────────

static void testFolderInspectionAndPartialDegradation() {
    std::printf("[Test 7] Read-only folder inspection: raw Pa5X KMP, OC31 KMP PartialNativeInspect, corrupt PCG, symlinks\n");

    const Id8 smpId1 = makeId(0xB0, 1);
    const Id8 msId1 = makeId(0xA0, 1);
    const Id8 msId2 = makeId(0xA0, 2);
    const Bytes pcmBytes = buildKorfContainer({
        {0x09000018u, 0, 0, 0, "SynthSmp1", Bytes(2048, 0x2A), idAsStr(smpId1)}
    });
    const Bytes rawKmpPayload = buildRawPa5xKmpPayload(
        {{smpId1, "SynthSmp1", 48000}},
        {},
        {{0, 72}},
        {{msId1, "SynthMs1", 0, 1}, {msId2, "SynthMs2", 0, 1}});
    const Bytes rawKmpFile = buildKorfContainer({
        {kPa5xKmpChunkId, 0, 0, 0, "RAM.KMP", rawKmpPayload, ""}
    });

    // Build 8 melodic sounds of kind 0x10050038 so fitLayout() (minItems=8) succeeds naturally!
    // Intentionally set KORF dir bank byte to 9 while file name is USER03.PCG (verifies D4/D7 rule!).
    std::vector<SynthKorfItem> soundItems;
    Bytes body0;
    for (uint8_t slot = 0; slot < 8; ++slot) {
        Bytes body = buildSoundBody(static_cast<uint8_t>(0x30 + slot), msId1, (slot % 2 == 0) ? msId2 : msId1);
        if (slot == 0) body0 = body;
        soundItems.push_back({0x10050038u, 1, 9 /*bogus dir bank*/, slot,
                              "SynthSound_" + std::to_string(slot),
                              makeOcLiteralFrame("OC31", body), ""});
    }
    const Bytes user03Pcg = buildKorfContainer(soundItems);

    // DrumKit with non-fitting body (19 bytes) -> unanalysed USERDK
    const Bytes userDk01Pcg = buildKorfContainer({
        {0x10050038u, 1, 0, 0, "SynthDK0", makeOcLiteralFrame("OC31", Bytes(19, 0x55)), ""}
    });

    // 7a. Valid authentic-shaped Pa5X target KST folder
    {
        TempDirGuard tmp("valid_kst");
        writeFileBytes(tmp.dir / "MULTISMP" / "RAM.KMP", rawKmpFile);
        writeFileBytes(tmp.dir / "PCM" / "ram01.pcm", pcmBytes);
        writeFileBytes(tmp.dir / "SOUND" / "USER03.PCG", user03Pcg);
        writeFileBytes(tmp.dir / "SOUND" / "USERDK01.PCG", userDk01Pcg);
        writeFileBytes(tmp.dir / "STYLE" / "BankA" / "Demo.stg", Bytes{'S', 'T', 'G', '1'});

        const TargetOccupancyMap map = inspectTargetKstFolder(tmp.dir);
        TEST("valid synthetic Pa5X KST folder -> valid & fullyInspectable",
             map.valid && map.fullyInspectable && !map.partialInspect.isPartial);
        TEST("bank identity comes from filename USER03.PCG, NOT KORF dir bank byte 9",
             map.isBankFilePresent(false, 3) && !map.isBankFilePresent(false, 9) &&
             map.slotState({false, 3, 0}) == SlotState::Occupied &&
             map.slotState({false, 9, 0}) == SlotState::BankFileAbsent);
        TEST("USERDK01/0 occupied & unanalysed; USERDK05 BankFileAbsent",
             map.slotState({true, 1, 0}) == SlotState::Occupied &&
             map.slotState({true, 5, 0}) == SlotState::BankFileAbsent &&
             map.coverage.unanalysedDrumKits == 1 && map.coverage.analysedSounds == 8);

        // Verify exact decoded body SHA-256 + zone topKey + PCM payloadBytes match!
        const OccupiedSlotEntry* s0 = map.findOccupied({false, 3, 0});
        TEST("inspected USER03/0 is provablyComparable with exact body SHA-256 & topKey 72 & 2048 B PCM",
             s0 != nullptr && s0->equivalence.provablyComparable() &&
             s0->equivalence.bodySha256 == conflict_detail::sha256Bytes(body0.data(), body0.size()) &&
             s0->equivalence.referencedMultisamples.size() == 2 &&
             s0->equivalence.referencedMultisamples[0].zones.size() == 1 &&
             s0->equivalence.referencedMultisamples[0].zones[0].topKey == 72 &&
             s0->equivalence.referencedMultisamples[0].zones[0].pcmPayloadBytes == 2048);

        // Verify M1 closure bridge
        const NativeKstClosureResult m1 = resolveNativeKstClosureFolder(tmp.dir);
        const TargetOccupancyMap fromM1 = buildTargetOccupancyFromM1Closure(m1);
        TEST("buildTargetOccupancyFromM1Closure preserves occupancy and fails closed on equivalence",
             fromM1.valid && fromM1.fullyInspectable &&
             fromM1.slotState({false, 3, 0}) == SlotState::Occupied &&
             !fromM1.findOccupied({false, 3, 0})->equivalence.provablyComparable());

        // Verify cooperative cancellation (UX Gate #4)
        TargetInspectOptions cancelOpts;
        cancelOpts.keepGoing = []() { return false; };
        const TargetOccupancyMap cancelledMap = inspectTargetKstFolder(tmp.dir, cancelOpts);
        TEST("keepGoing()==false cancels inspection cleanly without partial claims",
             cancelledMap.cancelled && !cancelledMap.valid && !cancelledMap.fullyInspectable);

        // Verify repeated folder inspection yields byte-identical targetLibraryKey
        const TargetOccupancyMap mapAgain = inspectTargetKstFolder(tmp.dir);
        TEST("repeated folder inspection yields byte-identical targetLibraryKey",
             !map.targetLibraryKey.empty() && map.targetLibraryKey == mapAgain.targetLibraryKey);

        // Verify shuffled in-memory NativeKstInput yields byte-identical targetLibraryKey
        NativeKstInput inA;
        inA.kmpContainer = RawNamedBlob{"MULTISMP/RAM.KMP", "", rawKmpFile};
        inA.pcmContainers = {{"PCM/ram01.pcm", "", pcmBytes}};
        inA.soundContainers = {
            {"SOUND/USER03.PCG", "USER03", user03Pcg},
            {"SOUND/USERDK01.PCG", "USERDK01", userDk01Pcg}
        };
        NativeKstInput inB = inA;
        std::reverse(inB.soundContainers.begin(), inB.soundContainers.end());
        const TargetOccupancyMap memA = inspectTargetKstInput(inA, {{"STYLE/BankA/Demo.stg", "style/banka/demo.stg", std::string(64, 'e'), 4}});
        const TargetOccupancyMap memB = inspectTargetKstInput(inB, {{"STYLE/BankA/Demo.stg", "style/banka/demo.stg", std::string(64, 'e'), 4}});
        TEST("shuffled in-memory inspectTargetKstInput yields byte-identical targetLibraryKey",
             memA.fullyInspectable && memB.fullyInspectable && memA.targetLibraryKey == memB.targetLibraryKey);
    }

    // 7b. OC31-compressed RAM.KMP target (Pa700/Pa4X converted) -> graceful PartialNativeInspect + fail-closed TargetUninspectable
    {
        TempDirGuard tmp("oc31_kmp_kst");
        const Bytes oc31Kmp = buildKorfContainer({
            {0x11050038u, 0, 0, 0, "RAM.KMP", makeOcLiteralFrame("OC31", Bytes(32, 0xAB)), ""}
        });
        writeFileBytes(tmp.dir / "MULTISMP" / "ram.kmp", oc31Kmp);
        writeFileBytes(tmp.dir / "PCM" / "ram01.pcm", pcmBytes);
        writeFileBytes(tmp.dir / "SOUND" / "USER03.PCG", user03Pcg);

        const TargetOccupancyMap map = inspectTargetKstFolder(tmp.dir);
        TEST("OC31 RAM.KMP target -> fail-closed fullyInspectable==false + graceful PartialNativeInspect",
             map.valid && !map.fullyInspectable && map.partialInspect.isPartial &&
             map.partialInspect.kmpIsOc31 && map.partialInspect.soundInventory.size() == 8 &&
             map.partialInspect.pcmContainerCount == 1);

        bool hasOc31Diag = false, hasUninspectDiag = false;
        for (const auto& d : map.targetDiagnostics) {
            if (d.code == diag_code::kOc31KmpUnsupported) hasOc31Diag = true;
            if (d.code == diag_code::kTargetUninspectable) hasUninspectDiag = true;
        }
        TEST("OC31 target emits OC31_KMP_UNSUPPORTED + TARGET_UNINSPECTABLE diagnostics",
             hasOc31Diag && hasUninspectDiag);

        // Collision matrix MUST fail closed under ALL 3 policies!
        for (ConflictPolicy pol : {ConflictPolicy::BlockWithReport, ConflictPolicy::ReuseIdenticalTargetSlot, ConflictPolicy::RemapToFreeSlotInNewLib}) {
            CollisionMatrixInput in;
            in.policy = pol;
            in.soundRequests.push_back({"pa5x:USER01/Sound/0", {false, 1, 0}, "Test",
                                        makeAnalysedProfile(0x10050038u, "x", 1, 60, 1, 1024), false, {}});
            const auto r = evaluateCollisionMatrix(map, in);
            TEST("OC31 target blocks CollisionMatrix under policy",
                 !r.targetInspectable && r.hasBlockingConflicts() && r.soundResolutions[0].blocking);
        }
    }

    // 7c. Corrupt .PCG -> fail-closed TargetUninspectable while preserving valid PCG inventory
    {
        TempDirGuard tmp("corrupt_pcg");
        writeFileBytes(tmp.dir / "MULTISMP" / "RAM.KMP", rawKmpFile);
        writeFileBytes(tmp.dir / "PCM" / "ram01.pcm", pcmBytes);
        writeFileBytes(tmp.dir / "SOUND" / "USER03.PCG", user03Pcg);
        writeFileBytes(tmp.dir / "SOUND" / "USER04.PCG", Bytes{'B', 'A', 'D', '!', 0, 1, 2});

        const TargetOccupancyMap map = inspectTargetKstFolder(tmp.dir);
        TEST("corrupt PCG -> fullyInspectable==false, valid USER03 inventory still exposed",
             !map.fullyInspectable && map.partialInspect.isPartial && map.partialInspect.soundInventory.size() == 8);
    }

    // 7d. Symlink target root refused (O_NOFOLLOW) and zero local path leaked in diagnostics
    {
        TempDirGuard tmp("symlink_root");
        const fs::path realDir = tmp.dir / "real.KST";
        const fs::path linkDir = tmp.dir / "link.KST";
        fs::create_directories(realDir);
        fs::create_directory_symlink(realDir, linkDir);

        const TargetOccupancyMap map = inspectTargetKstFolder(linkDir);
        TEST("symlink target KST root refused fail-closed", !map.valid && !map.fullyInspectable);
        TEST("uninspectableReason leaks zero local filesystem path",
             map.uninspectableReason.find(tmp.dir.string()) == std::string::npos &&
             map.uninspectableReason.find("/Users/") == std::string::npos);
    }
}

// ── Test 8: JSON & Markdown Serializers, parseJson(), Path Scrubbing & Shuffled Determinism ──────────────────────────

static void testSerializationAndDeterminism() {
    std::printf("[Test 8] JSON/Markdown serializers, parseJson() validity, path-free output & shuffled determinism\n");

    SelectiveExportReport rep;
    rep.mode = ExportMode::AdvisoryPlan;
    rep.conflictPolicy = ConflictPolicy::BlockWithReport;
    rep.planFingerprint = std::string(64, 'a');
    rep.sourceKind = "native-kst";
    // Intentionally supply absolute local paths to prove canonical scrubbing!
    rep.sourceLabel = "/Users/private-owner/secret/MyLib.KST";
    rep.targetPresent = true;
    rep.targetLabel = "/tmp/local/Target.KST";
    rep.targetLibraryKey = std::string(64, 'b');
    rep.m1ManifestSha256 = std::string(64, 'c');

    rep.selection = {
        {SelectedKind::Style, "STYLE/BankB/Two.stg", "", ""},
        {SelectedKind::Style, "STYLE/BankA/One.stg", "STYLE/Custom", "One_Renamed.stg"}
    };
    rep.soundsReferenced = 12;
    rep.factorySounds = 9;
    rep.userSounds = 3;
    rep.multisamples = 5;
    rep.samples = 42;
    rep.coverage.analysedSounds = 2;
    rep.coverage.unanalysedDrumKits = 1;
    rep.coverage.unresolvedInAnalysedSounds = 1;
    rep.coverage.unresolvedInUnanalysedSounds = 4;
    rep.coverage.coverageComplete = false;
    rep.coverage.deleteProofAvailable = true;  // Serializer MUST force false!

    rep.memoryProjection.sampleBytesAreNativeStoredProxy = true;
    rep.memoryProjection.rawReferencedSampleBytes = 1000000;
    rep.memoryProjection.uniqueLogicalSampleBytes = 400000;
    rep.memoryProjection.logicalDedupSavedBytes = 600000;

    // Include tricky control chars, quotes, backslash, DEL (0x7F), raw Latin-1 (0xE9, 0xFF), and Markdown '|'
    const std::string nastyName = std::string("Caf") + '\xE9' + " \"Lead\"\t\\Pipe|\x01\x7F\xFF";
    rep.conflicts = {
        {SlotConflict::Kind::SoundSlotOccupiedDifferent, "USER01/5", nastyName, "Target | Sound", "BLOCKED", true, "detail with \n newline"},
        {SlotConflict::Kind::StyleOrObjectPathCollision, "STYLE/BankA/One.stg", "STYLE/BankA/One.stg", "STYLE/BANKA/one.stg", "RENAME_REQUIRED", true, "FAT R-18"}
    };
    rep.deduplication = {
        {"resource-id", "pa5x:KST/Multisample/2", "STYLE/BankA/One.stg", {"STYLE/BankB/Two.stg", "STYLE/BankA/One.stg"}, 600000, true}
    };
    rep.diagnostics = {
        {DiagnosticCategory::FACTORY_ASSUMED_EXTERNAL, diag_code::kFactoryRomSoundRef, DiagnosticSeverity::Info, EvidenceLevel::Inferred,
         "MSB 121 LSB 0 PC 0", "", "Factory ROM ref", "No copy needed.", {"STYLE/BankB/Two.stg", "STYLE/BankA/One.stg"}},
        {DiagnosticCategory::UNSUPPORTED_FORMAT, diag_code::kNativeSoundWriteHold, DiagnosticSeverity::Blocking, EvidenceLevel::Unsupported,
         "pa5x:USER01/Sound/5", "", "Sound '" + nastyName + "' write is on HOLD.", "Use single-Sound load.", {"STYLE/BankB/Two.stg", "STYLE/BankA/One.stg"}}
    };

    const std::string json1 = serializeSelectiveReportJson(rep);
    const std::string md1 = serializeSelectiveReportMarkdown(rep);

    ai_arranger::jobs::kit::Json parsed;
    const std::string perr = ai_arranger::jobs::kit::parseJson(json1, &parsed);
    TEST("JSON parses cleanly with jobs::kit::parseJson()", perr.empty() && parsed.t == ai_arranger::jobs::kit::Json::T::Obj);

    const auto* schema = parsed.get("schema");
    const auto* label = parsed.get("label");
    const auto* hw = parsed.get("hardwareVerified");
    const auto* src = parsed.get("source");
    const auto* mp = parsed.get("memoryProjection");
    TEST("JSON carries schema, mandatory banner, hardwareVerified==false, deleteProofAvailable==false",
         schema && schema->s == kSelectiveReportSchema &&
         label && label->s == kHardwareUnverifiedBanner &&
         hw && hw->t == ai_arranger::jobs::kit::Json::T::Bool && !hw->b &&
         src && src->get("deleteProofAvailable") && !src->get("deleteProofAvailable")->b &&
         src->get("coverageComplete") && !src->get("coverageComplete")->b);
    TEST("JSON memoryProjection marks unverified-proxy with caveat when native stored proxy",
         mp && mp->get("fitVerdict") && mp->get("fitVerdict")->s == "unverified-proxy" &&
         mp->get("caveat") && mp->get("caveat")->s == kStoredPcmProxyCaveat);

    TEST("JSON and Markdown contain ZERO local paths (/Users/, /tmp/)",
         json1.find("/Users/") == std::string::npos && json1.find("/tmp/") == std::string::npos &&
         md1.find("/Users/") == std::string::npos && md1.find("/tmp/") == std::string::npos);
    TEST("Markdown contains mandatory banner, partial coverage notice, and sanitized printable bytes",
         md1.find(kHardwareUnverifiedBanner) != std::string::npos &&
         md1.find(kPartialCoverageNotice) != std::string::npos &&
         md1.find("\\xE9") != std::string::npos &&
         md1.find("\\xFF") != std::string::npos &&
         md1.find("\\|") != std::string::npos);

    // Byte-identical determinism across shuffled input orderings!
    std::mt19937 rng(20261125u);
    bool allMatch = true;
    for (int trial = 0; trial < 5; ++trial) {
        SelectiveExportReport shuffled = rep;
        std::shuffle(shuffled.selection.begin(), shuffled.selection.end(), rng);
        std::shuffle(shuffled.conflicts.begin(), shuffled.conflicts.end(), rng);
        std::shuffle(shuffled.diagnostics.begin(), shuffled.diagnostics.end(), rng);
        for (auto& d : shuffled.diagnostics) std::shuffle(d.affectedRoots.begin(), d.affectedRoots.end(), rng);
        for (auto& g : shuffled.deduplication) std::shuffle(g.duplicates.begin(), g.duplicates.end(), rng);
        if (serializeSelectiveReportJson(shuffled) != json1 ||
            serializeSelectiveReportMarkdown(shuffled) != md1) {
            allMatch = false;
        }
    }
    TEST("JSON and Markdown reports are 100% byte-identical across 5 shuffled input orderings", allMatch);

    // Also verify CollisionMatrix determinism across shuffled requests!
    TargetOccupancyMap target = TargetOccupancyMap::makeEmpty(true);
    const SoundEquivalenceProfile p1 = makeAnalysedProfile(0x10050038u, "s1", 1, 60, 1, 1024);
    const SoundEquivalenceProfile p2 = makeAnalysedProfile(0x10050038u, "s2", 2, 72, 2, 2048);
    target.addOccupiedSlot({{false, 1, 0}, "T0", "SOUND/USER01.PCG", p1});
    target.addOccupiedSlot({{false, 1, 1}, "T1", "SOUND/USER01.PCG", p1});
    target.rebuildIndexesAndKey();

    CollisionMatrixInput cIn;
    cIn.policy = ConflictPolicy::RemapToFreeSlotInNewLib;
    for (uint32_t i = 0; i < 6; ++i) {
        const auto prof = makeAnalysedProfile(0x10050038u, "req_" + std::to_string(i), static_cast<uint8_t>(10 + i), 64, static_cast<uint8_t>(10 + i), 1024);
        cIn.soundRequests.push_back({"req-" + std::to_string(i), {false, 1, i % 3}, "Name_" + std::to_string(i), prof, false, {}});
    }
    cIn.objectRequests = {
        {SelectedKind::Style, "STYLE/B/Z.stg", "", "", std::string(64, '1'), {}},
        {SelectedKind::Style, "STYLE/A/A.stg", "", "", std::string(64, '2'), {}}
    };
    const auto baseCol = evaluateCollisionMatrix(target, cIn);
    bool colDeterministic = true;
    for (int trial = 0; trial < 5; ++trial) {
        CollisionMatrixInput sh = cIn;
        std::shuffle(sh.soundRequests.begin(), sh.soundRequests.end(), rng);
        std::shuffle(sh.objectRequests.begin(), sh.objectRequests.end(), rng);
        const auto got = evaluateCollisionMatrix(target, sh);
        if (got.soundResolutions != baseCol.soundResolutions ||
            got.objectResolutions != baseCol.objectResolutions ||
            got.conflicts != baseCol.conflicts ||
            got.diagnostics != baseCol.diagnostics) {
            colDeterministic = false;
        }
    }
    TEST("CollisionMatrix + CanonicalSlotAllocator are 100% deterministic across shuffled inputs", colDeterministic);
}

int main() {
    testDiagnosticCatalog();
    testSoundEquivalence();
    testCollisionMatrixAllPolicies();
    testGridExhaustionAndAllocator();
    testBankAbsentAndOutOfSpecDrumKit();
    testFatCaseCollisionsR18();
    testFolderInspectionAndPartialDegradation();
    testSerializationAndDeterminism();

    std::printf("\nSummary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
