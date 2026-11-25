#ifndef AI_ARRANGER_JOBS_SELECTIVE_IMPORT_KIT_H
#define AI_ARRANGER_JOBS_SELECTIVE_IMPORT_KIT_H

// Slice M2c: Target-Aware Selective Pa5X Import Kit Planner & Staged E6 Export Job.
//
// Builds on:
//  * M2a (`selective_diagnostics.h`, `slot_conflicts.h`): TargetOccupancyMap, CanonicalSlotAllocator,
//    evaluateCollisionMatrix(), SelectiveExportReport, 4-category Diagnostic taxonomy & JSON/MD serializers.
//  * M2b (`m1_manifest_adapter.h`, `selective_collector.h`): ProvisionalWorkspace adaptation, Tier 1/2
//    deduplication, and honest MemoryProjectionReport (uncompressed PCM vs stored-PCM proxy caveat).
//  * E6/E8/E9 (`export_job.h`, `import_kit.h`, `library_apply.h`): descriptor-pinned staging (`writeAt`,
//    `NAME.INCOMPLETE`, atomic non-replacing publish), `kit::verifyKit()`, `library::recheckPinned()`,
//    D18 source + target overlap guard, and TOCTOU review-to-execute binding (`selectiveFingerprint`,
//    `expectSha`, `targetLibraryKey`).
//
// Invariants & HOLD Gates:
//  * Portable C++20 header-only, zero JUCE/GUI dependency, zero apps/macos/ wiring.
//  * ZERO Native Sound KST writes, ZERO OC31 encoder, ZERO mutations to source workspace or target KST.
//  * Source inputs and optional targetKst are opened strictly read-only (`O_NOFOLLOW`, `S_ISREG`).
//  * NEVER overwrites an occupied target Sound slot or Style path; fails closed on unresolved dependencies
//    or uninspectable/OC31-KMP targets (`TargetUninspectable` + `PartialNativeInspect`).
//  * Always stamps `kit::kLabel` (`STRUCTURALLY VALID — NOT VERIFIED ON A PA5X`), `"hardwareVerified": false`,
//    `"deleteProofAvailable": false`, `L3 = NotRun`, and honest K9 / stored-PCM proxy caveats.

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "exporters/korg/sound_map.h"
#include "jobs/import_kit.h"
#include "library/library_apply.h"
#include "media/sound/korg_kmp_source.h"
#include "media/sound/package_graph.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "media/sound/m1_manifest_adapter.h"
#include "media/sound/selective_collector.h"
#include "media/sound/selective_diagnostics.h"
#include "media/sound/slot_conflicts.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ai_arranger::jobs::kit {

struct RecommendedSlotAssignment {
    std::string kitPath;                                           // e.g. "SOUNDS/AIA Demo Organ"
    std::string packageId;                                         // canonical packageId (or kitPath for KMP)
    bool isDrumKit = false;
    std::optional<media::selective::SlotAddress> requestedSlot;    // original slot bound by Style
    std::optional<media::selective::SlotAddress> recommendedSlot;  // deterministic free slot from CanonicalSlotAllocator
    bool remapped = false;                                         // true iff recommendedSlot != requestedSlot
    std::string action;                                            // "preserve-free" | "remap-free-slot" | "blocked"
    std::string note;

    auto operator<=>(const RecommendedSlotAssignment&) const = default;
};

struct TargetReusedSound {
    media::selective::SlotAddress slot;
    std::string targetDisplayName;
    std::vector<std::string> affectedStyles;

    auto operator<=>(const TargetReusedSound&) const = default;
};

struct SelectiveKitRequest {
    fs::path workspaceRoot;                                                 // optional workspace folder holding Styles + packages
    std::vector<fs::path> selectedStyles;                                   // 1..N .mid/.uasf paths (abs or workspace-relative)
    std::vector<fs::path> packageInputs;                                    // optional explicit Sound/DrumKit/KMP package folders
    std::map<std::string, fs::path> styleSoundMaps;                         // optional per-style (or "" for all) .soundmap files
    std::map<media::selective::SlotAddress, fs::path> slotToPackageMap;     // SlotAddress -> provisional package path
    std::optional<fs::path> targetKst;                                      // optional read-only Target Pa5X KST folder
    media::selective::ConflictPolicy conflictPolicy = media::selective::ConflictPolicy::BlockWithReport;
    bool allowExperimentalKmp = false;
};

struct SelectiveKitPlan {
    bool ok = false;                                                        // true = analysis succeeded (check executable() before build!)
    bool cancelled = false;
    std::string fatalError;                                                 // populated when ok == false (IO/usage/symlink/cancel)

    ReviewPlan basePlan;                                                    // deduplicated Items, Planned files, bytes, sha, base fingerprint
    std::string selectiveFingerprint;                                       // SHA-256 binding basePlan + policy + styles + recs + targetKey + soundmaps
    std::string targetLibraryKey;                                           // from TargetOccupancyMap::targetLibraryKey ("" if no targetKst)
    std::vector<RecommendedSlotAssignment> slotRecommendations;             // deterministic sorted free-slot recommendations
    std::vector<TargetReusedSound> targetReusedSounds;                      // User Sounds verified already present in targetKst
    size_t candidatePackagesScanned = 0;
    size_t excludedUnusedPackages = 0;

    media::selective::SelectiveExportReport report;                         // full 4-category diagnostics, conflicts, dedup, memoryProjection, coverage
    media::selective::PartialNativeInspect targetPartialInspect;            // UX Gate #2 payload on OC31 KMP / partial target
    std::string enhancedKitMd;                                              // standard kitMd() + selective summary, memory, slots, K9 & proxy caveats
    std::string selectiveManifestJson;                                      // aiarranger-selective-export-report/1
    std::string diagnosticsJson;                                            // aiarranger-selective-export-report/1

    std::vector<std::string> blockingReasons;
    std::vector<fs::path> resolvedStylePaths;
    std::vector<fs::path> resolvedPackagePaths;
    std::vector<fs::path> resolvedSoundMapPaths;

    bool executable() const {
        return ok && !cancelled && basePlan.blocking.empty() && blockingReasons.empty() && report.executable();
    }
};

namespace selective_detail {

inline constexpr size_t kMaxStyleBytes = 16u << 20;
inline constexpr size_t kMaxSoundMapBytes = 1u << 20;
inline constexpr size_t kCancelStride = 64;

inline bool tickCancel(const std::function<bool()>& keepGoing, size_t& counter) {
    if (!keepGoing) return true;
    const size_t c = counter++;
    if (c == 0 || (c % kCancelStride) == 0) return keepGoing();
    return true;
}

inline std::string canonicalPathKey(const fs::path& p) {
    std::error_code ec;
    const fs::path wc = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return ec ? p.lexically_normal().generic_string() : wc.generic_string();
}

// Checks whether the entry itself (and when `workspaceRoot` is given, any relative component below it) is a symlink.
inline bool hasSymlinkBelow(const fs::path& base, const fs::path& rel) {
    std::error_code ec;
    fs::path cur = base;
    for (const auto& part : rel) {
        if (part.empty() || part == ".") continue;
        cur /= part;
        const auto st = fs::symlink_status(cur, ec);
        if (ec) return false; // missing entry handled by caller
        if (fs::is_symlink(st)) return true;
    }
    return false;
}

// Reads a regular file strictly with O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK and S_ISREG check.
inline bool readRegularBytesNoFollow(const fs::path& p, size_t maxBytes, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    err.clear();
    struct stat lst{};
    if (::lstat(p.c_str(), &lst) != 0) {
        err = "cannot stat regular file (missing or unreadable)";
        return false;
    }
    if (S_ISLNK(lst.st_mode)) {
        err = "refusing symlink (links are never followed)";
        return false;
    }
    if (!S_ISREG(lst.st_mode)) {
        err = "not a regular file";
        return false;
    }
    const int fd = ::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        err = "cannot open regular file without following links";
        return false;
    }
    media::detail::Fd guard(fd);
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        err = "not a regular file";
        return false;
    }
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) > maxBytes) {
        err = "file size exceeds reader limit";
        return false;
    }
    const size_t want = static_cast<size_t>(st.st_size);
    out.resize(want);
    size_t off = 0;
    while (off < want) {
        const ssize_t r = ::read(fd, out.data() + off, want - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            err = "read failed";
            out.clear();
            return false;
        }
        if (r == 0) {
            err = "file shrank while being read";
            out.clear();
            return false;
        }
        off += static_cast<size_t>(r);
    }
    uint8_t extra = 0;
    for (;;) {
        const ssize_t r = ::read(fd, &extra, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            err = "read failed";
            out.clear();
            return false;
        }
        if (r > 0) {
            err = "file grew while being read";
            out.clear();
            return false;
        }
        break;
    }
    return true;
}

// Computes exact stored PCM baseline bytes from a read-only Pa5X KST folder using metadata-only pread()
// (`addPcmContainerFdAt`), checking `keepGoing` every <= 256 items and never following symlinks.
inline bool computeTargetPcmBaselineBytes(const fs::path& kstDir,
                                          uint64_t& bytesOut,
                                          const std::function<bool()>& keepGoing,
                                          size_t& tick) {
    bytesOut = 0;
    const std::string rootPath = media::graph::native_detail::stripTrailingSlashes(kstDir);
    const media::graph::native_detail::ScopedFd rootFd(
        ::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (rootFd.fd < 0) return true;

    std::vector<std::string> topEntries;
    std::string err;
    if (!media::graph::native_detail::listDirEntriesAt(rootFd.fd, topEntries, 1000000, err)) return true;

    media::graph::PcmInventory pcm;
    size_t remainingBudget = size_t(256) << 20;
    size_t bytesRead = 0;
    for (const std::string& top : topEntries) {
        if (!tickCancel(keepGoing, tick)) return false;
        if (media::graph::native_detail::upperAscii(top) != "PCM") continue;
        struct stat st{};
        if (::fstatat(rootFd.fd, top.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) continue;
        const media::graph::native_detail::ScopedFd subFd(
            ::openat(rootFd.fd, top.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (subFd.fd < 0) continue;
        std::vector<std::string> leaves;
        if (!media::graph::native_detail::listDirEntriesAt(subFd.fd, leaves, 1000000, err)) continue;
        for (const std::string& leaf : leaves) {
            if (!tickCancel(keepGoing, tick)) return false;
            if (!media::graph::native_detail::endsWithUpper(leaf, ".PCM")) continue;
            std::string perr;
            const std::string rel = top + "/" + leaf;
            (void)media::graph::addPcmContainerFdAt(subFd.fd, leaf.c_str(), rel, media::panative::kMaxFileBytes,
                                                    remainingBudget, bytesRead, pcm, &perr);
        }
    }
    for (const auto& [_, pe] : pcm.byId) {
        if (!tickCancel(keepGoing, tick)) return false;
        bytesOut = media::selective::adapter_detail::satAdd(bytesOut, pe.payloadBytes);
    }
    return true;
}

// Resolves a user-supplied Style or Package path relative to workspaceRoot (if relative).
// Also checks standard workspace subfolders ("SOUNDS", "DRUMKITS", "KMP", "STYLES") for relative leaf paths.
inline fs::path resolveWorkspacePath(const fs::path& workspaceRoot, const fs::path& raw, bool isStyle) {
    if (raw.empty() || raw.is_absolute() || workspaceRoot.empty()) return raw;
    std::error_code ec;
    const fs::path direct = workspaceRoot / raw;
    if (fs::exists(fs::symlink_status(direct, ec))) return direct;
    if (isStyle) {
        const fs::path inStyles = workspaceRoot / "STYLES" / raw;
        if (fs::exists(fs::symlink_status(inStyles, ec))) return inStyles;
    } else {
        for (const char* sub : {"SOUNDS", "DRUMKITS", "KMP"}) {
            const fs::path cand = workspaceRoot / sub / raw;
            if (fs::exists(fs::symlink_status(cand, ec))) return cand;
        }
    }
    return direct;
}

inline std::string makeRelativeDisplayLabel(const fs::path& workspaceRoot, const fs::path& p, const char* fallbackPrefix) {
    std::error_code ec;
    if (!workspaceRoot.empty()) {
        const fs::path rel = p.lexically_relative(workspaceRoot);
        if (!rel.empty() && rel.native().rfind("..", 0) != 0 && !rel.is_absolute()) {
            return media::selective::conflict_detail::normalizeRelPath(rel.generic_string());
        }
    }
    const std::string leaf = media::folderPath(p).filename().string();
    return std::string(fallbackPrefix) + "/" + (leaf.empty() ? std::string("item") : leaf);
}

struct CandidatePackage {
    fs::path path;
    std::string canonicalKey;
    Item classified;
    std::string kitPath;          // e.g. "SOUNDS/AIA Demo Organ"
    std::string packageId;        // canonical packageId from loadPackage() (or kitPath for KMP)
    std::string displayName;
    std::string packageSha256;
    bool valid = false;
    std::string error;
    media::graph::Graph graph;
    media::graph::Kind topKind = media::graph::Kind::Sound;
    uint64_t kmpSampleBytes = 0;
    size_t kmpSampleCount = 0;
    bool explicitInput = false;
};

inline bool loadKmpSampleBudget(const fs::path& kmpDir,
                                uint64_t& bytesOut,
                                size_t& sampleCountOut,
                                std::string& shaOut,
                                std::string& errOut,
                                const std::function<bool()>& keepGoing,
                                size_t& tick) {
    bytesOut = 0;
    sampleCountOut = 0;
    shaOut.clear();
    errOut.clear();
    std::vector<media::sound::KorgMultisample> ms;
    if (const auto e = media::sound::scanKorgFolder(kmpDir, ms); !e.empty()) {
        errOut = e;
        return false;
    }
    std::set<fs::path> ksfs;
    for (const auto& m : ms) {
        for (const auto* v : {&m.ksf, &m.ksfR}) {
            for (const auto& f : *v) {
                if (!f.empty()) ksfs.insert(f);
            }
        }
    }
    media::Sha256 h;
    for (const auto& f : ksfs) {
        if (!tickCancel(keepGoing, tick)) return false;
        std::vector<uint8_t> b;
        std::string rerr;
        if (!readRegularBytesNoFollow(f, 32u << 20, b, rerr)) {
            errOut = f.filename().string() + ": " + rerr;
            return false;
        }
        media::sound::KsfHead head;
        media::sound::Sample smp;
        if (const auto ke = media::sound::readKsf(b, &head, &smp, f); !ke.empty()) {
            errOut = f.filename().string() + ": " + ke;
            return false;
        }
        bytesOut = media::selective::adapter_detail::satAdd(
            bytesOut, static_cast<uint64_t>(smp.frames()) * static_cast<uint64_t>(smp.channels) * 2ull);
        ++sampleCountOut;
        const std::string name = f.filename().string();
        media::selective::conflict_detail::hashStr(h, name);
        h.update(b.data(), b.size());
    }
    shaOut = h.hex();
    return true;
}

inline std::string buildEnhancedKitMd(const std::string& kitName,
                                      const SelectiveKitPlan& plan) {
    namespace sel = media::selective;
    std::string o = kitMd(kitName, plan.basePlan.items);

    o += "\n## Selective Summary, Memory Projection & Recommended Target Slots\n\n";
    o += "- **Verification Banner:** `" + std::string(kLabel) + "` (`hardwareVerified: false`, `deleteProofAvailable: false`, `L3: NotRun`)\n";
    o += "- **Mode:** `import-kit` | **Conflict Policy:** `" + std::string(sel::conflictPolicyName(plan.report.conflictPolicy)) +
         "` | **Executable:** `" + std::string(plan.executable() ? "true" : "false") + "`\n";
    o += "- **Selective Plan Fingerprint (SHA-256):** `" + plan.selectiveFingerprint + "`\n";
    if (!plan.targetLibraryKey.empty()) {
        o += "- **Target Pa5X KST Library Key (SHA-256):** `" + plan.targetLibraryKey + "`\n";
    }
    o += "- **Selection & Deduplication:** " + std::to_string(plan.report.selection.size()) + " selected Style(s), " +
         std::to_string(plan.basePlan.items.size()) + " total kit member(s), " +
         std::to_string(plan.excludedUnusedPackages) + " unused candidate package(s) excluded\n";
    o += "- **SMF Program Change Caveat (K9):** " + std::string(sel::kSmfPcCaveatK9) + "\n";

    // Memory projection (§4.4, UX Gate #3)
    const auto& mp = plan.report.memoryProjection;
    o += "\n### Memory Projection\n\n";
    o += "- **Unit:** `" + std::string(mp.unitName()) + "` (`hardwareVerified: false`)\n";
    o += "- **Raw Referenced Sample Bytes:** " + std::to_string(mp.rawReferencedSampleBytes) + "\n";
    o += "- **Unique Logical Sample Bytes:** " + std::to_string(mp.uniqueLogicalSampleBytes) + "\n";
    o += "- **Logical Deduplication Saved Bytes:** " + std::to_string(mp.logicalDedupSavedBytes) + "\n";
    o += "- **Identical-Content Advisory Extra Bytes (Distinct Packages):** " + std::to_string(mp.identicalContentExtraBytes) + "\n";
    if (mp.identicalContentExtraBytes > 0) {
        o += "  - *Advisory:* Distinct TXPROG/TXPERF packages share byte-identical WAV audio; packages are kept unmodified. Whether Pa5X deduplicates samples across separate TX imports is UNKNOWN.\n";
    }
    o += "- **Unique Samples Count:** " + std::to_string(mp.uniqueSamplesCount) +
         " (unknown size: " + std::to_string(mp.unknownSizeSamplesCount) + ")\n";
    if (plan.report.targetPresent) {
        o += "- **Target KST Baseline Stored PCM Bytes:** " + std::to_string(mp.targetBaselineSampleBytes) + "\n";
    }
    o += "- **Projected Combined Bytes:** " + std::to_string(mp.projectedCombinedBytes) + "\n";
    o += "- **Fit Verdict:** `" + std::string(mp.fitVerdictName()) + "`\n";
    if (mp.sampleBytesAreNativeStoredProxy) {
        o += "- **Mandatory Stored-PCM Proxy Caveat:** " + std::string(sel::kStoredPcmProxyCaveat) + "\n";
    }

    // Recommended free target User Sound / Drum Kit slots
    if (!plan.slotRecommendations.empty()) {
        o += "\n### Recommended Free Target User Slots\n\n";
        o += "On the Pa5X, choose the target User location in the TXPROG/TXPERF import dialog (MAN p.1210). "
             "AIArranger never writes to your target KST; the table below lists deterministic free slots verified against the target grid:\n\n";
        o += "| Kit Package | Requested Slot | Recommended Free Target Slot | Action | Note |\n";
        o += "|---|---|---|---|---|\n";
        for (const auto& rec : plan.slotRecommendations) {
            const std::string reqStr = rec.requestedSlot ? rec.requestedSlot->label() : "-";
            const std::string recStr = rec.recommendedSlot ? rec.recommendedSlot->label() : "BLOCKED";
            o += "| " + sel::diag_detail::mdCell(rec.kitPath) +
                 " | " + sel::diag_detail::mdCell(reqStr) +
                 " | " + sel::diag_detail::mdCell(recStr) +
                 " | `" + sel::diag_detail::mdCell(rec.action) + "`" +
                 " | " + sel::diag_detail::mdCell(rec.note) + " |\n";
        }
    }

    // Target-reused User Sounds
    if (!plan.targetReusedSounds.empty()) {
        o += "\n### Target User Sounds Reused (Already Present in Target KST — No Import Needed)\n\n";
        for (const auto& tr : plan.targetReusedSounds) {
            o += "- `" + tr.slot.label() + "`";
            if (!tr.targetDisplayName.empty()) {
                o += " (" + sel::diag_detail::mdCell(tr.targetDisplayName) + ")";
            }
            o += " — verified occupied in target KST (`reuse-target`)\n";
        }
    }

    // Diagnostics & Conflicts counts
    o += "\n### Diagnostics Summary\n\n";
    o += "- Blocking diagnostics: " + std::to_string(plan.report.blockingDiagnosticCount()) +
         ", Blocking conflicts: " + std::to_string(plan.report.blockingConflictCount()) +
         ", Warnings: " + std::to_string(plan.report.warningDiagnosticCount()) +
         ", Info: " + std::to_string(plan.report.infoDiagnosticCount()) + "\n";
    o += "- Full machine-readable details: `SELECTIVE_MANIFEST.json` and `DIAGNOSTICS.json` (`" +
         std::string(sel::kSelectiveReportSchema) + "`, `hardwareVerified: false`).\n";
    return o;
}

inline std::string computeSelectiveFingerprint(
    const ReviewPlan& basePlan,
    const SelectiveKitRequest& req,
    const std::vector<std::pair<std::string, std::string>>& styleRelAndSha,
    const std::vector<RecommendedSlotAssignment>& recs,
    const std::string& targetLibraryKey,
    const std::vector<std::pair<std::string, std::string>>& soundMapRelAndSha) {
    namespace cd = media::selective::conflict_detail;
    media::Sha256 h;
    cd::hashStr(h, "aiarranger-selective-import-kit-plan/1");
    cd::hashStr(h, basePlan.fingerprint);
    cd::hashStr(h, media::selective::conflictPolicyName(req.conflictPolicy));
    cd::hashStr(h, req.allowExperimentalKmp ? "1" : "0");
    cd::hashStr(h, targetLibraryKey);

    cd::hashU64Be(h, static_cast<uint64_t>(styleRelAndSha.size()));
    for (const auto& [rel, sha] : styleRelAndSha) {
        cd::hashStr(h, rel);
        cd::hashStr(h, sha);
    }

    cd::hashU64Be(h, static_cast<uint64_t>(recs.size()));
    for (const auto& r : recs) {
        cd::hashStr(h, r.kitPath);
        cd::hashStr(h, r.packageId);
        cd::hashStr(h, r.isDrumKit ? "1" : "0");
        cd::hashStr(h, r.requestedSlot ? r.requestedSlot->shortLabel() : "");
        cd::hashStr(h, r.recommendedSlot ? r.recommendedSlot->shortLabel() : "");
        cd::hashStr(h, r.remapped ? "1" : "0");
        cd::hashStr(h, r.action);
    }

    cd::hashU64Be(h, static_cast<uint64_t>(soundMapRelAndSha.size()));
    for (const auto& [rel, sha] : soundMapRelAndSha) {
        cd::hashStr(h, rel);
        cd::hashStr(h, sha);
    }

    cd::hashU64Be(h, static_cast<uint64_t>(basePlan.sha.size()));
    for (const auto& [kitRel, sha] : basePlan.sha) {
        cd::hashStr(h, kitRel);
        cd::hashStr(h, sha);
    }
    return h.hex();
}

}  // namespace selective_detail

// Read-only target-aware selective Pa5X Import Kit planner.
// Deterministic across shuffled input orders, strictly read-only (zero disk writes, O_NOFOLLOW),
// cooperatively cancellable via `keepGoing` (checked every <= 256 items).
inline SelectiveKitPlan selectivePlan(const SelectiveKitRequest& req,
                                      const std::function<bool()>& keepGoing = {}) {
    namespace sel = media::selective;
    namespace sd = selective_detail;
    size_t tick = 0;

    auto makeCancelled = []() {
        SelectiveKitPlan c;
        c.ok = false;
        c.cancelled = true;
        c.fatalError = "review cancelled; nothing planned";
        c.basePlan.blocking.push_back("review cancelled; nothing planned");
        c.blockingReasons.push_back("review cancelled; nothing planned");
        c.report.mode = sel::ExportMode::ImportKitBundle;
        c.report.sourceKind = "provisional-workspace";
        c.report.coverage.deleteProofAvailable = false;
        return c;
    };

    auto makeFatal = [&](const std::string& reason) {
        SelectiveKitPlan f;
        f.ok = false;
        f.cancelled = false;
        f.fatalError = reason;
        f.basePlan.blocking.push_back(reason);
        f.blockingReasons.push_back(reason);
        f.report.mode = sel::ExportMode::ImportKitBundle;
        f.report.conflictPolicy = req.conflictPolicy;
        f.report.sourceKind = "provisional-workspace";
        f.report.sourceLabel = "<workspace>";
        f.report.coverage.deleteProofAvailable = false;
        f.report.diagnostics.push_back({
            sel::DiagnosticCategory::VERIFIED_MISSING,
            sel::collector_diag_code::kSourceUnavailable,
            sel::DiagnosticSeverity::Blocking,
            sel::EvidenceLevel::Verified,
            "<workspace>",
            "",
            reason,
            "Verify input paths are existing, readable, non-symlink local files/folders.",
            {}
        });
        f.report = sel::canonicalizeReport(std::move(f.report));
        f.selectiveManifestJson = sel::serializeSelectiveReportJson(f.report);
        f.diagnosticsJson = f.selectiveManifestJson;
        return f;
    };

    if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();

    // 1. Validate optional workspaceRoot (strictly read-only, no symlinks)
    if (!req.workspaceRoot.empty()) {
        std::error_code ec;
        const auto st = fs::symlink_status(req.workspaceRoot, ec);
        if (ec || !fs::exists(st)) return makeFatal("workspace root does not exist or cannot be examined");
        if (fs::is_symlink(st)) return makeFatal("workspace root is a symlink; links are never followed");
        if (!fs::is_directory(st)) return makeFatal("workspace root is not a directory");
    }

    // 2. Validate & inspect optional Target Pa5X KST strictly read-only
    std::optional<sel::TargetOccupancyMap> targetMap;
    uint64_t targetBaselineBytes = 0;
    if (req.targetKst.has_value()) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        std::error_code ec;
        const auto tst = fs::symlink_status(*req.targetKst, ec);
        if (ec || !fs::exists(tst)) return makeFatal("target KST folder does not exist or cannot be examined");
        if (fs::is_symlink(tst)) return makeFatal("target KST folder is a symlink; links are never followed");
        if (!fs::is_directory(tst)) return makeFatal("target KST path is not a directory");

        sel::TargetInspectOptions topts;
        topts.keepGoing = keepGoing;
        topts.hashObjectFiles = true;
        targetMap = sel::inspectTargetKstFolder(*req.targetKst, topts);
        if (targetMap->cancelled) return makeCancelled();
        if (!targetMap->valid) {
            return makeFatal(targetMap->uninspectableReason.empty()
                                 ? "target KST folder could not be inspected"
                                 : targetMap->uninspectableReason);
        }
        if (!sd::computeTargetPcmBaselineBytes(*req.targetKst, targetBaselineBytes, keepGoing, tick)) {
            return makeCancelled();
        }
    }

    SelectiveKitPlan plan;
    plan.ok = true;
    plan.report.mode = sel::ExportMode::ImportKitBundle;
    plan.report.conflictPolicy = req.conflictPolicy;
    plan.report.sourceKind = "provisional-workspace";
    plan.report.sourceLabel = req.workspaceRoot.empty()
                                  ? std::string("<workspace>")
                                  : sel::diag_detail::canonicalPathFreeLabel(
                                        media::folderPath(req.workspaceRoot).filename().string(), "<workspace>");
    plan.report.coverage.deleteProofAvailable = false;

    if (targetMap.has_value()) {
        plan.targetLibraryKey = targetMap->targetLibraryKey;
        plan.targetPartialInspect = targetMap->partialInspect;
        plan.report.targetPresent = true;
        plan.report.targetLabel = sel::diag_detail::canonicalPathFreeLabel(
            media::folderPath(*req.targetKst).filename().string(), "<target>");
        plan.report.targetLibraryKey = targetMap->targetLibraryKey;
    }

    // 3. Parse optional .soundmap files (read-only, O_NOFOLLOW, hashed for selectiveFingerprint)
    struct ParsedSoundMap {
        std::string key;           // "" (default) or style leaf/relPath
        fs::path path;
        std::string label;
        std::string sha256;
        exporters::korg::SoundMapResult parsed;
    };
    std::vector<ParsedSoundMap> soundMaps;
    std::vector<std::pair<std::string, std::string>> soundMapRelAndSha;
    for (const auto& [skey, rawPath] : req.styleSoundMaps) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        const fs::path resolved = sd::resolveWorkspacePath(req.workspaceRoot, rawPath, false);
        if (!req.workspaceRoot.empty() && !rawPath.is_absolute() && sd::hasSymlinkBelow(req.workspaceRoot, rawPath)) {
            return makeFatal("sound map path traverses a symlink; links are never followed");
        }
        std::vector<uint8_t> bytes;
        std::string rerr;
        if (!sd::readRegularBytesNoFollow(resolved, sd::kMaxSoundMapBytes, bytes, rerr)) {
            return makeFatal("cannot read sound map (" + rerr + ")");
        }
        const std::string text(bytes.begin(), bytes.end());
        std::istringstream iss(text);
        const std::string lbl = sd::makeRelativeDisplayLabel(req.workspaceRoot, resolved, "soundmap");
        const auto parsed = exporters::korg::parseSoundMap(iss, lbl);
        if (!parsed.ok) {
            return makeFatal("invalid sound map: " + parsed.error);
        }
        const std::string sha = sel::conflict_detail::sha256Bytes(bytes.data(), bytes.size());
        soundMapRelAndSha.emplace_back(lbl + "|" + skey, sha);
        plan.resolvedSoundMapPaths.push_back(resolved);
        soundMaps.push_back({skey, resolved, lbl, sha, parsed});
    }
    std::sort(soundMapRelAndSha.begin(), soundMapRelAndSha.end());

    // 4. Discover & deduplicate candidate packages (workspaceRoot + packageInputs + slotToPackageMap)
    std::map<std::string, sd::CandidatePackage> candidatesByCanonKey;
    std::vector<std::pair<fs::path, bool>> rawCandidates; // (path, explicitInput)

    auto enqueueCandidate = [&](const fs::path& raw, bool explicitInput) -> std::string {
        const fs::path p = sd::resolveWorkspacePath(req.workspaceRoot, raw, false);
        if (!req.workspaceRoot.empty() && !raw.is_absolute() && sd::hasSymlinkBelow(req.workspaceRoot, raw)) {
            return "candidate package path traverses a symlink; links are never followed";
        }
        std::error_code ec;
        const auto st = fs::symlink_status(p, ec);
        if (fs::is_symlink(st)) {
            return p.filename().string() + " is a symlink; links are never followed";
        }
        rawCandidates.emplace_back(p, explicitInput);
        return "";
    };

    for (const auto& p : req.packageInputs) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        if (const auto e = enqueueCandidate(p, true); !e.empty()) return makeFatal(e);
    }
    for (const auto& [_, p] : req.slotToPackageMap) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        if (const auto e = enqueueCandidate(p, true); !e.empty()) return makeFatal(e);
    }
    if (!req.workspaceRoot.empty()) {
        std::vector<fs::path> scanDirs{req.workspaceRoot};
        for (const char* sub : {"SOUNDS", "DRUMKITS", "KMP"}) {
            std::error_code ec;
            const fs::path d = req.workspaceRoot / sub;
            const auto st = fs::symlink_status(d, ec);
            if (!ec && fs::is_directory(st) && !fs::is_symlink(st)) scanDirs.push_back(d);
        }
        for (const auto& dir : scanDirs) {
            if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
            const auto entries = outputsIn(dir);
            for (const auto& e : entries) {
                if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
                std::error_code ec;
                const auto st = fs::symlink_status(e, ec);
                if (ec || fs::is_symlink(st) || !fs::is_directory(st)) continue;
                const auto cl = classify(e);
                if (cl.kind == Kind::Sound || cl.kind == Kind::DrumKit || cl.kind == Kind::Kmp) {
                    rawCandidates.emplace_back(e, false);
                }
            }
        }
    }

    // Load candidate packages in deterministic sorted order
    std::sort(rawCandidates.begin(), rawCandidates.end(), [](const auto& a, const auto& b) {
        const std::string ka = sd::canonicalPathKey(a.first);
        const std::string kb = sd::canonicalPathKey(b.first);
        return ka != kb ? ka < kb : (a.second > b.second);
    });

    for (const auto& [p, explicitIn] : rawCandidates) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        const std::string ckey = sd::canonicalPathKey(p);
        if (auto it = candidatesByCanonKey.find(ckey); it != candidatesByCanonKey.end()) {
            it->second.explicitInput = it->second.explicitInput || explicitIn;
            continue;
        }
        sd::CandidatePackage cand;
        cand.path = p;
        cand.canonicalKey = ckey;
        cand.explicitInput = explicitIn;
        cand.classified = classify(p);
        cand.kitPath = cand.classified.kind == Kind::Unknown
                           ? ("UNKNOWN/" + cand.classified.name)
                           : cand.classified.kitPath();

        if (cand.classified.kind == Kind::Sound || cand.classified.kind == Kind::DrumKit) {
            std::string lerr;
            if (!media::sound::loadPackage(p, cand.graph, cand.packageId, &lerr)) {
                cand.valid = false;
                cand.error = lerr.empty() ? "package validation failed" : lerr;
                cand.packageId = "invalid:" + cand.kitPath;
            } else {
                cand.valid = true;
                cand.topKind = (cand.classified.kind == Kind::DrumKit) ? media::graph::Kind::DrumKit : media::graph::Kind::Sound;
                if (const auto* n = cand.graph.find({true, cand.packageId, cand.topKind, 0})) {
                    cand.displayName = n->name;
                }
                if (cand.displayName.empty()) cand.displayName = cand.classified.name;
                std::vector<uint8_t> manBytes;
                std::string merr;
                if (sd::readRegularBytesNoFollow(p / "PACKAGE.sha256", 4u << 20, manBytes, merr)) {
                    cand.packageSha256 = sel::conflict_detail::sha256Bytes(manBytes.data(), manBytes.size());
                } else {
                    const std::string tsv = media::sound::writeManifest(cand.graph, cand.packageId, cand.topKind);
                    cand.packageSha256 = sel::adapter_detail::sha256Str(tsv);
                }
            }
        } else if (cand.classified.kind == Kind::Kmp) {
            cand.packageId = "kmp:" + cand.kitPath;
            cand.displayName = cand.classified.name;
            std::string kerr;
            if (!sd::loadKmpSampleBudget(p, cand.kmpSampleBytes, cand.kmpSampleCount, cand.packageSha256, kerr, keepGoing, tick)) {
                if (keepGoing && !keepGoing()) return makeCancelled();
                cand.valid = false;
                cand.error = kerr.empty() ? "KMP/KSF validation failed" : kerr;
            } else {
                cand.valid = true;
            }
        } else {
            cand.valid = false;
            cand.error = "not a Sound/DrumKit package or KMP/KSF folder";
            cand.packageId = "invalid:" + cand.kitPath;
        }
        candidatesByCanonKey.emplace(ckey, std::move(cand));
    }
    plan.candidatePackagesScanned = candidatesByCanonKey.size();

    // Map SlotAddress -> canonicalKey for explicit bindings
    std::map<sel::SlotAddress, std::string> slotToCandKey;
    for (const auto& [addr, rawPkg] : req.slotToPackageMap) {
        const fs::path resolved = sd::resolveWorkspacePath(req.workspaceRoot, rawPkg, false);
        slotToCandKey[addr] = sd::canonicalPathKey(resolved);
    }

    // 5. Resolve, deduplicate, and read selected Styles strictly read-only
    struct SelectedStyleData {
        fs::path path;
        std::string canonKey;
        std::string leaf;
        std::string relPath;           // "STYLES/<leaf>"
        bool isUasf = false;
        std::vector<uint8_t> bytes;
        std::string sha256;
        std::vector<media::graph::MidiSoundAddress> explicitSelections;
        bool hasUnresolvedSoundMap = false;
    };
    std::vector<SelectedStyleData> styles;
    std::set<std::string> seenStyleCanon;

    for (const auto& rawStyle : req.selectedStyles) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        const fs::path resolved = sd::resolveWorkspacePath(req.workspaceRoot, rawStyle, true);
        if (!req.workspaceRoot.empty() && !rawStyle.is_absolute() && sd::hasSymlinkBelow(req.workspaceRoot, rawStyle)) {
            return makeFatal("selected style path traverses a symlink; links are never followed");
        }
        std::error_code ec;
        const auto sst = fs::symlink_status(resolved, ec);
        if (ec || !fs::exists(sst)) return makeFatal("selected style does not exist: " + rawStyle.filename().string());
        if (fs::is_symlink(sst)) return makeFatal("selected style is a symlink; links are never followed");
        if (!fs::is_regular_file(sst)) return makeFatal("selected style is not a regular file");

        const std::string ckey = sd::canonicalPathKey(resolved);
        if (!seenStyleCanon.insert(ckey).second) continue;

        const std::string ext = detail::lower(resolved.extension().string());
        if (ext != ".mid" && ext != ".uasf") {
            return makeFatal("selected style must be a .mid or .uasf file: " + resolved.filename().string());
        }

        SelectedStyleData st;
        st.path = resolved;
        st.canonKey = ckey;
        st.leaf = resolved.filename().string();
        st.relPath = "STYLES/" + st.leaf;
        st.isUasf = (ext == ".uasf");
        std::string rerr;
        if (!sd::readRegularBytesNoFollow(resolved, sd::kMaxStyleBytes, st.bytes, rerr)) {
            return makeFatal("cannot read selected style " + st.leaf + ": " + rerr);
        }
        st.sha256 = sel::conflict_detail::sha256Bytes(st.bytes.data(), st.bytes.size());

        if (st.isUasf) {
            uasf::UasfDeserializer des;
            const auto ur = des.deserialize(st.bytes);
            if (!ur.success) {
                return makeFatal("invalid UASF style " + st.leaf + ": " + ur.error);
            }
            for (const auto& [_, sr] : ur.style.korg_export.sounds) {
                st.explicitSelections.push_back({static_cast<int>(sr.msb), static_cast<int>(sr.lsb), static_cast<int>(sr.pc)});
            }
            for (const auto& [_, sr] : ur.style.korg_export.element_sounds) {
                st.explicitSelections.push_back({static_cast<int>(sr.msb), static_cast<int>(sr.lsb), static_cast<int>(sr.pc)});
            }
        }

        // Attach matching .soundmap entries (default "" or matching leaf / relPath)
        for (const auto& sm : soundMaps) {
            if (sm.key.empty() || sm.key == st.leaf || sm.key == st.relPath ||
                sm.key == rawStyle.generic_string()) {
                for (const auto& [_, sr] : sm.parsed.sounds) {
                    st.explicitSelections.push_back({static_cast<int>(sr.msb), static_cast<int>(sr.lsb), static_cast<int>(sr.pc)});
                }
                if (!sm.parsed.unresolved.empty()) st.hasUnresolvedSoundMap = true;
            }
        }
        styles.push_back(std::move(st));
    }

    // Canonical sort of selected styles (100% order-independent determinism!)
    std::sort(styles.begin(), styles.end(), [](const SelectedStyleData& a, const SelectedStyleData& b) {
        return a.relPath != b.relPath ? a.relPath < b.relPath : a.canonKey < b.canonKey;
    });

    std::vector<std::pair<std::string, std::string>> styleRelAndSha;
    for (const auto& st : styles) {
        plan.resolvedStylePaths.push_back(st.path);
        styleRelAndSha.emplace_back(st.relPath, st.sha256);
    }

    // 6. Build M2b ProvisionalWorkspaceInput & adapt with M2b `adaptProvisionalWorkspace()`
    sel::ProvisionalWorkspaceInput wsIn;
    for (const auto& [_, cand] : candidatesByCanonKey) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        if (cand.classified.kind != Kind::Sound && cand.classified.kind != Kind::DrumKit) continue;
        sel::ProvisionalPackageInput pi;
        pi.relDir = cand.kitPath;
        pi.packageSha256 = cand.packageSha256;
        pi.prebuiltPackageId = cand.packageId;
        pi.prebuiltTopKind = cand.topKind;
        pi.prebuiltDisplayName = cand.displayName;
        if (cand.valid) {
            pi.prebuiltGraph = cand.graph;
        }
        wsIn.packages.push_back(std::move(pi));
    }

    std::set<std::string> usedPackageCanonKeys;
    std::map<sel::SlotAddress, std::string> usedBoundSlotToCandKey;
    std::map<sel::SlotAddress, std::set<std::string>> usedBoundSlotRoots;
    std::map<sel::SlotAddress, std::set<std::string>> targetReusedSlotRoots;
    std::vector<sel::Diagnostic> extraDiagnostics;
    std::vector<sel::SlotConflict> extraConflicts;

    for (const auto& st : styles) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        sel::ProvisionalRootInput ri;
        ri.relPath = st.relPath;
        ri.kind = st.isUasf ? sel::RootItemKind::ProvisionalUasf : sel::RootItemKind::ProvisionalMidi;
        if (!st.isUasf) ri.smfBytes = st.bytes;
        ri.explicitSelections = st.explicitSelections;
        ri.contentSha256 = st.sha256;

        // Bind any SlotAddress in req.slotToPackageMap that resolves to a Sound/DrumKit package
        for (const auto& [addr, ckey] : slotToCandKey) {
            const auto cit = candidatesByCanonKey.find(ckey);
            if (cit != candidatesByCanonKey.end() &&
                (cit->second.classified.kind == Kind::Sound || cit->second.classified.kind == Kind::DrumKit)) {
                ri.addressToPackageId[{static_cast<int>(addr.msb()), static_cast<int>(addr.lsb()), static_cast<int>(addr.pc())}] =
                    cit->second.packageId;
            }
        }
        wsIn.roots.push_back(std::move(ri));

        if (st.isUasf) {
            extraDiagnostics.push_back({
                sel::DiagnosticCategory::UNSUPPORTED_FORMAT,
                sel::diag_code::kNativeSoundWriteHold,
                sel::DiagnosticSeverity::Blocking,
                sel::EvidenceLevel::Unsupported,
                st.relPath,
                "",
                "Selected UASF style '" + st.leaf + "' must be exported to Pa5X marker-SMF (.mid) before bundling into a Pa5X Import Kit.",
                "Run `aiarranger-style export-smf` or `aiarranger-convert --target pa5x` and pass the resulting .mid file.",
                {st.relPath}
            });
        }
        if (st.hasUnresolvedSoundMap) {
            extraDiagnostics.push_back({
                sel::DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                sel::diag_code::kUnresolvedMidiBankSelect,
                sel::DiagnosticSeverity::Warning,
                sel::EvidenceLevel::Unsupported,
                st.relPath,
                "",
                "Attached .soundmap leaves one or more Korg tracks explicitly `unresolved`.",
                "Assign verified Pa5X Bank/PC addresses in the .soundmap or verify track Sounds in Pa5X Style Edit.",
                {st.relPath}
            });
        }
    }

    sel::M1AdapterView wsView = sel::adaptProvisionalWorkspace(wsIn, keepGoing);
    if (wsView.cancelled) return makeCancelled();

    // Inspect each selected root's SoundSelectionRecords to:
    //  (a) identify actually-used bound packages (so unused candidates stay excluded!),
    //  (b) recognize existing User Sounds already occupied in targetKst (`reuse-target`),
    //  (c) set accurate MissingRefReason (BankAbsent vs SlotEmpty) against targetKst.
    for (auto& root : wsView.roots) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        for (auto& srec : root.soundSelections) {
            if (!srec.slotAddress.has_value()) continue;
            const sel::SlotAddress addr = *srec.slotAddress;

            // Check explicit slotToPackageMap binding
            if (const auto bit = slotToCandKey.find(addr); bit != slotToCandKey.end()) {
                const auto cit = candidatesByCanonKey.find(bit->second);
                usedBoundSlotToCandKey[addr] = bit->second;
                usedBoundSlotRoots[addr].insert(root.relPath);
                if (cit != candidatesByCanonKey.end()) {
                    usedPackageCanonKeys.insert(bit->second);
                    if (cit->second.classified.kind == Kind::Kmp) {
                        // KMP bound to a User slot: require explicit opt-in
                        srec.resolved = true;
                        if (!req.allowExperimentalKmp) {
                            extraDiagnostics.push_back({
                                sel::DiagnosticCategory::UNSUPPORTED_FORMAT,
                                sel::diag_code::kLegacySoundKindUnsupported,
                                sel::DiagnosticSeverity::Blocking,
                                sel::EvidenceLevel::Unsupported,
                                cit->second.kitPath,
                                addr.shortLabel(),
                                "Experimental KMP/KSF folder '" + cit->second.kitPath + "' requires explicit opt-in (--allow-experimental-kmp).",
                                "Re-run with --allow-experimental-kmp or replace with a TXPROG/SF2 Sound package.",
                                {root.relPath}
                            });
                        } else if (!cit->second.valid) {
                            extraDiagnostics.push_back({
                                sel::DiagnosticCategory::VERIFIED_MISSING,
                                sel::diag_code::kMissingPackageWavOrTx,
                                sel::DiagnosticSeverity::Blocking,
                                sel::EvidenceLevel::Verified,
                                cit->second.kitPath,
                                addr.shortLabel(),
                                "Experimental KMP/KSF folder failed validation: " + cit->second.error,
                                "Regenerate the KMP/KSF folder cleanly before building the kit.",
                                {root.relPath}
                            });
                        }
                    } else if (!cit->second.valid) {
                        // Invalid TXPROG/TXPERF package handled by M2b collector (emits kMissingPackageWavOrTx)
                    }
                } else {
                    srec.resolved = false;
                    extraDiagnostics.push_back({
                        sel::DiagnosticCategory::VERIFIED_MISSING,
                        sel::diag_code::kMissingPackageWavOrTx,
                        sel::DiagnosticSeverity::Blocking,
                        sel::EvidenceLevel::Verified,
                        addr.shortLabel(),
                        "",
                        "Bound package for slot " + addr.shortLabel() + " does not exist.",
                        "Verify the --bind package path.",
                        {root.relPath}
                    });
                }
                continue;
            }

            // Unbound User Sound / Drum Kit slot: check targetKst occupancy!
            if (srec.boundary == sel::SoundBoundaryKind::NativeUserSound ||
                srec.boundary == sel::SoundBoundaryKind::NativeUserDrumKit) {
                if (targetMap.has_value() && targetMap->valid && targetMap->fullyInspectable) {
                    const sel::SlotState st = targetMap->slotState(addr);
                    if (st == sel::SlotState::Occupied) {
                        srec.resolved = true;
                        srec.missingReason = media::graph::MissingRefReason::None;
                        targetReusedSlotRoots[addr].insert(root.relPath);
                        if (addr.isDrumKit) {
                            extraDiagnostics.push_back({
                                sel::DiagnosticCategory::UNSUPPORTED_FORMAT,
                                sel::diag_code::kDrumkitBodyUnparsed,
                                sel::DiagnosticSeverity::Warning,
                                sel::EvidenceLevel::Unsupported,
                                addr.resourceId().str(),
                                addr.shortLabel(),
                                "Reusing occupied target User Drum Kit " + addr.shortLabel() +
                                    " (Drum Kit -> DrumSample internals are unparsed; verify audibly on Pa5X).",
                                "No import needed if this Drum Kit already exists on your Pa5X.",
                                {root.relPath}
                            });
                        }
                    } else if (st == sel::SlotState::BankFileAbsent) {
                        srec.resolved = false;
                        srec.missingReason = media::graph::MissingRefReason::SoundBankAbsent;
                    } else {
                        srec.resolved = false;
                        srec.missingReason = media::graph::MissingRefReason::SoundSlotEmpty;
                    }
                } else {
                    srec.resolved = false;
                    srec.missingReason = media::graph::MissingRefReason::SoundSlotEmpty;
                }
            }
        }
    }

    // Explicit KMP packageInputs (since KMP folders aren't bound via MIDI Bank/PC):
    for (const auto& [ckey, cand] : candidatesByCanonKey) {
        if (cand.classified.kind == Kind::Kmp && cand.explicitInput && !usedPackageCanonKeys.count(ckey)) {
            if (req.allowExperimentalKmp) {
                usedPackageCanonKeys.insert(ckey);
                if (!cand.valid) {
                    extraDiagnostics.push_back({
                        sel::DiagnosticCategory::VERIFIED_MISSING,
                        sel::diag_code::kMissingPackageWavOrTx,
                        sel::DiagnosticSeverity::Blocking,
                        sel::EvidenceLevel::Verified,
                        cand.kitPath,
                        "",
                        "Experimental KMP/KSF folder failed validation: " + cand.error,
                        "Regenerate the KMP/KSF folder cleanly before building the kit.",
                        {}
                    });
                }
            } else {
                extraDiagnostics.push_back({
                    sel::DiagnosticCategory::UNSUPPORTED_FORMAT,
                    sel::diag_code::kLegacySoundKindUnsupported,
                    sel::DiagnosticSeverity::Blocking,
                    sel::EvidenceLevel::Unsupported,
                    cand.kitPath,
                    "",
                    "Explicit KMP/KSF folder '" + cand.kitPath + "' requires --allow-experimental-kmp.",
                    "Pass --allow-experimental-kmp to include EXPERIMENTAL KMP/KSF folders in the kit.",
                    {}
                });
            }
        }
    }

    // Populate targetReusedSounds cleanly
    for (const auto& [addr, roots] : targetReusedSlotRoots) {
        TargetReusedSound tr;
        tr.slot = addr;
        if (targetMap.has_value()) {
            if (const auto* occ = targetMap->findOccupied(addr)) tr.targetDisplayName = occ->displayName;
        }
        tr.affectedStyles.assign(roots.begin(), roots.end());
        plan.targetReusedSounds.push_back(std::move(tr));
    }

    // 7. Run M2b `collectSelectiveExportWithTargetMap()` (Tier 1/Tier 2 dedup + memory projection + diagnostics!)
    sel::CollectorRequest creq;
    creq.mode = sel::ExportMode::ImportKitBundle;
    creq.conflictPolicy = req.conflictPolicy;
    creq.requireCompleteSampleClosure = false;
    for (const auto& st : styles) {
        creq.targets.push_back({sel::SelectedKind::Style, st.relPath, "", ""});
    }

    const sel::DryRunExportPlan dryRun = sel::collectSelectiveExportWithTargetMap(
        wsView, creq, targetMap.has_value() ? &(*targetMap) : nullptr,
        targetBaselineBytes, targetMap.has_value(), keepGoing);
    if (dryRun.cancelled) return makeCancelled();

    plan.report = sel::toSelectiveExportReport(
        dryRun, plan.report.sourceLabel, plan.report.targetLabel);
    plan.report.targetPresent = targetMap.has_value();
    plan.report.targetLibraryKey = plan.targetLibraryKey;
    plan.report.userSounds = usedBoundSlotToCandKey.size() + targetReusedSlotRoots.size();

    // Add KMP uncompressed sample bytes if any KMP folders were selected
    for (const std::string& ckey : usedPackageCanonKeys) {
        const auto cit = candidatesByCanonKey.find(ckey);
        if (cit == candidatesByCanonKey.end() || cit->second.classified.kind != Kind::Kmp || !cit->second.valid) continue;
        auto& mp = plan.report.memoryProjection;
        mp.rawReferencedSampleBytes = sel::adapter_detail::satAdd(mp.rawReferencedSampleBytes, cit->second.kmpSampleBytes);
        mp.uniqueLogicalSampleBytes = sel::adapter_detail::satAdd(mp.uniqueLogicalSampleBytes, cit->second.kmpSampleBytes);
        mp.uniqueSamplesCount += cit->second.kmpSampleCount;
        mp.projectedCombinedBytes = sel::adapter_detail::satAdd(mp.targetBaselineSampleBytes, mp.uniqueLogicalSampleBytes);
        if (mp.projectedCombinedBytes <= media::graph::kPa5xUserSampleBytesDecimal) mp.projectedFit = media::graph::Fit::Fits;
        else if (mp.projectedCombinedBytes <= media::graph::kPa5xUserSampleBytesBinary) mp.projectedFit = media::graph::Fit::Ambiguous;
        else if (mp.projectedCombinedBytes <= media::graph::kPa5xCompressedEquivalentDecimal) mp.projectedFit = media::graph::Fit::NeedsCompression;
        else mp.projectedFit = media::graph::Fit::Exceeds;
        plan.report.samples += cit->second.kmpSampleCount;
    }

    // 8. Run M2a `evaluateCollisionMatrix()` for all used bound Sound/DrumKit slots (for target conflicts & free-slot recs!)
    if (!usedBoundSlotToCandKey.empty()) {
        const sel::TargetOccupancyMap emptyTarget = sel::TargetOccupancyMap::makeEmpty(true);
        const sel::TargetOccupancyMap& evalTarget = targetMap.has_value() ? *targetMap : emptyTarget;

        sel::CollisionMatrixInput cmIn;
        cmIn.policy = req.conflictPolicy;
        cmIn.mode = sel::ExportMode::ImportKitBundle;

        for (const auto& [addr, ckey] : usedBoundSlotToCandKey) {
            if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
            const auto cit = candidatesByCanonKey.find(ckey);
            sel::SoundCollisionRequest sreq;
            sreq.requestId = (cit != candidatesByCanonKey.end()) ? cit->second.kitPath : addr.shortLabel();
            sreq.sourceAddress = addr;
            sreq.sourceLabel = (cit != candidatesByCanonKey.end()) ? cit->second.displayName : addr.shortLabel();
            sreq.expectExistingInTarget = false;
            const auto& roots = usedBoundSlotRoots[addr];
            sreq.affectedRoots.assign(roots.begin(), roots.end());
            cmIn.soundRequests.push_back(std::move(sreq));
        }

        const sel::CollisionMatrixResult cmRes = sel::evaluateCollisionMatrix(evalTarget, std::move(cmIn));

        // Carry forward conflicts & normalize provisional Sound slot collision diagnostics cleanly
        for (const auto& c : cmRes.conflicts) {
            extraConflicts.push_back(c);
        }
        for (auto d : cmRes.diagnostics) {
            // For a provisional Import Kit package colliding with an occupied native target slot,
            // classify as SOUND_SLOT_OCCUPIED_DIFFERENT (never overwritten!).
            if (d.code == sel::diag_code::kSoundSlotEquivalenceInsufficient && d.severity == sel::DiagnosticSeverity::Blocking) {
                d.code = sel::diag_code::kSoundSlotOccupiedDifferent;
                d.evidence = sel::EvidenceLevel::Verified;
                d.summary = "Target slot " + d.referencedTarget + " is occupied in target KST; Import Kit package '" +
                            d.subject + "' will never overwrite an occupied slot.";
                d.remediation = "Switch policy to `remap` (--policy remap) to recommend a free target slot, or bind to an unoccupied slot.";
            }
            extraDiagnostics.push_back(std::move(d));
        }

        // Convert M2a's SoundResolutionEntry list into deterministic RecommendedSlotAssignments
        for (const auto& sr : cmRes.soundResolutions) {
            RecommendedSlotAssignment rec;
            rec.kitPath = sr.requestId;
            rec.isDrumKit = sr.sourceAddress.isDrumKit;
            rec.requestedSlot = sr.sourceAddress;
            rec.recommendedSlot = sr.resolvedAddress;
            rec.remapped = sr.remapped;
            if (const auto bit = usedBoundSlotToCandKey.find(sr.sourceAddress); bit != usedBoundSlotToCandKey.end()) {
                if (const auto cit = candidatesByCanonKey.find(bit->second); cit != candidatesByCanonKey.end()) {
                    rec.packageId = cit->second.packageId;
                }
            }
            if (sr.blocking) {
                rec.action = "blocked";
                rec.note = sr.detail;
            } else if (sr.remapped) {
                rec.action = "remap-free-slot";
                rec.note = "Original slot occupied in target; import into recommended free slot (MAN p.1210).";
            } else {
                rec.action = "preserve-free";
                rec.note = targetMap.has_value() ? "Slot verified free in target KST." : "No target KST attached; slot preserved.";
            }
            plan.slotRecommendations.push_back(std::move(rec));
        }
        std::sort(plan.slotRecommendations.begin(), plan.slotRecommendations.end());
    }

    // Merge extra conflicts & diagnostics into plan.report and canonicalize
    plan.report.conflicts.insert(plan.report.conflicts.end(), extraConflicts.begin(), extraConflicts.end());
    plan.report.diagnostics.insert(plan.report.diagnostics.end(), extraDiagnostics.begin(), extraDiagnostics.end());
    plan.report = sel::canonicalizeReport(std::move(plan.report));

    // Count excluded unused candidate packages
    plan.excludedUnusedPackages = 0;
    for (const auto& [ckey, cand] : candidatesByCanonKey) {
        if (!usedPackageCanonKeys.count(ckey)) ++plan.excludedUnusedPackages;
    }

    // 9. Build kit::ReviewPlan (`basePlan`) over ONLY the deduplicated selected Styles (.mid) + required packages!
    std::vector<fs::path> kitInputs;
    for (const auto& st : styles) {
        if (!st.isUasf) kitInputs.push_back(st.path);
    }
    for (const std::string& ckey : usedPackageCanonKeys) {
        const auto cit = candidatesByCanonKey.find(ckey);
        if (cit == candidatesByCanonKey.end()) continue;
        if (cit->second.classified.kind == Kind::Kmp && !req.allowExperimentalKmp) continue;
        kitInputs.push_back(cit->second.path);
        plan.resolvedPackagePaths.push_back(cit->second.path);
    }

    // Canonical deterministic ordering of kit inputs by (Kind, kitPath, canonKey)
    std::sort(kitInputs.begin(), kitInputs.end(), [](const fs::path& a, const fs::path& b) {
        const auto ca = classify(a);
        const auto cb = classify(b);
        if (ca.kind != cb.kind) return static_cast<int>(ca.kind) < static_cast<int>(cb.kind);
        if (ca.kitPath() != cb.kitPath()) return ca.kitPath() < cb.kitPath();
        return sd::canonicalPathKey(a) < sd::canonicalPathKey(b);
    });

    std::vector<std::shared_ptr<media::detail::Fd>> pins;
    for (const auto& p : kitInputs) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        auto it = classify(p);
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(p, ec))) {
            plan.basePlan.blocking.push_back(p.filename().string() + " is a link; the kit never follows links");
            continue;
        }
        if (it.kind == Kind::Unknown) {
            plan.basePlan.blocking.push_back(p.filename().string() + " is not an AIArranger output the kit takes");
            continue;
        }
        std::shared_ptr<media::detail::Fd> fd;
        if (const auto e = pinRoot(it, &fd); !e.empty()) {
            plan.basePlan.blocking.push_back(e);
            continue;
        }
        pins.push_back(std::move(fd));
        plan.basePlan.items.push_back(std::move(it));
    }
    if (const auto e = layoutRefusal(plan.basePlan.items, &plan.basePlan.files, &plan.basePlan.bytes); !e.empty()) {
        plan.basePlan.blocking.push_back(e);
    }
    for (auto& it : plan.basePlan.items) {
        if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
        std::string d;
        const auto e = checkMember(it.kind, it.source, &d);
        it.status = e.empty() ? kValid : "FAILED";
        it.detail = e.empty() ? d : e;
        if (!e.empty()) {
            plan.basePlan.blocking.push_back("FAILED: " + it.kitPath() + " (" + it.detail + ")");
            plan.report.diagnostics.push_back({
                sel::DiagnosticCategory::VERIFIED_MISSING,
                sel::diag_code::kMissingPackageWavOrTx,
                sel::DiagnosticSeverity::Blocking,
                sel::EvidenceLevel::Verified,
                it.kitPath(),
                "",
                "Kit member failed independent structural reader: " + it.detail,
                "Repair or regenerate the member before building the kit.",
                {}
            });
        }
    }
    if (plan.basePlan.blocking.empty()) {
        for (const auto& f : plan.basePlan.files) {
            if (!sd::tickCancel(keepGoing, tick)) return makeCancelled();
            const auto& it = plan.basePlan.items[f.item];
            bool stopped = false;
            std::string herr;
            const auto sum = media::detail::sha256File(
                it.kind == Kind::Style ? it.source : it.source / f.from,
                [&](size_t) { return !keepGoing || keepGoing(); }, &stopped, &herr);
            if (stopped) return makeCancelled();
            if (sum.empty() || !herr.empty()) {
                plan.basePlan.blocking.push_back("cannot read " + f.to + " to hash it: " + herr);
                break;
            }
            plan.basePlan.sha[f.to] = sum;
        }
    }
    if (!plan.basePlan.items.empty()) {
        plan.basePlan.fingerprint = planFingerprint(plan.basePlan.items, plan.basePlan.files);
    }

    // Re-canonicalize report & compute deterministic selectiveFingerprint
    plan.report = sel::canonicalizeReport(std::move(plan.report));
    plan.selectiveFingerprint = sd::computeSelectiveFingerprint(
        plan.basePlan, req, styleRelAndSha, plan.slotRecommendations, plan.targetLibraryKey, soundMapRelAndSha);
    plan.report.planFingerprint = plan.selectiveFingerprint;

    // Collect concise human-readable blockingReasons
    for (const auto& b : plan.basePlan.blocking) plan.blockingReasons.push_back(b);
    for (const auto& d : plan.report.diagnostics) {
        if (d.severity == sel::DiagnosticSeverity::Blocking) {
            plan.blockingReasons.push_back("[" + d.code + "] " + d.subject + ": " + d.summary);
        }
    }
    for (const auto& c : plan.report.conflicts) {
        if (c.blocking) {
            plan.blockingReasons.push_back("[" + std::string(sel::slotConflictKindName(c.kind)) + "] " + c.address + ": " + c.detail);
        }
    }

    // Serialize JSON reports & enhanced KIT.md
    plan.selectiveManifestJson = sel::serializeSelectiveReportJson(plan.report);
    plan.diagnosticsJson = plan.selectiveManifestJson;
    plan.enhancedKitMd = sd::buildEnhancedKitMd("Selective Pa5X Import Kit", plan);
    return plan;
}

// Target-aware selective Pa5X Import Kit as an E6 export job (descriptor-pinned writeAt,
// D18 source + targetKst overlap guard, TOCTOU selectiveFingerprint + expectSha + targetLibraryKey
// verification, verifyKit(), and pre-publish targetLibraryKey + library::recheckPinned() gate).
inline ExportSpec selectiveImportKit(const SelectiveKitRequest& req,
                                     fs::path dest,
                                     std::string expectSelectiveFingerprint = {},
                                     std::map<std::string, std::string> expectSha = {},
                                     std::string expectTargetLibraryKey = {}) {
    namespace sel = media::selective;
    namespace sd = selective_detail;

    ExportSpec s;
    s.type = "Pa5X Selective Import Kit (documented imports)";
    s.dest = std::move(dest);

    // D18 overlap guard: register workspaceRoot, every input style/package/soundmap, AND *targetKst!
    std::set<std::string> seenSrc;
    auto addSource = [&](const fs::path& p) {
        if (p.empty()) return;
        const std::string key = sd::canonicalPathKey(p);
        if (seenSrc.insert(key).second) {
            s.sources.push_back(p);
            s.source += (s.source.empty() ? "" : ", ") + p.string();
        }
    };
    if (!req.workspaceRoot.empty()) addSource(req.workspaceRoot);
    for (const auto& st : req.selectedStyles) addSource(sd::resolveWorkspacePath(req.workspaceRoot, st, true));
    for (const auto& pk : req.packageInputs) addSource(sd::resolveWorkspacePath(req.workspaceRoot, pk, false));
    for (const auto& [_, pk] : req.slotToPackageMap) addSource(sd::resolveWorkspacePath(req.workspaceRoot, pk, false));
    for (const auto& [_, sm] : req.styleSoundMaps) addSource(sd::resolveWorkspacePath(req.workspaceRoot, sm, false));
    if (req.targetKst.has_value()) addSource(*req.targetKst);

    struct SharedState {
        SelectiveKitPlan reviewed;
        std::vector<std::shared_ptr<media::detail::Fd>> roots;
        std::string pinnedTargetKey;
        std::shared_ptr<library::Pinned> stagedPin;
    };
    auto state = std::make_shared<SharedState>();
    auto expectedShaPtr = std::make_shared<const std::map<std::string, std::string>>(std::move(expectSha));

    s.validate = [req, state, expectSelectiveFingerprint, expectTargetLibraryKey,
                  kitName = media::folderPath(s.dest).filename().string()]() -> std::string {
        if (const auto e = detail::fatName(kitName); !e.empty()) {
            return "refusing: the kit name '" + kitName + "' has " + e;
        }
        SelectiveKitPlan reviewed = selectivePlan(req);
        if (!reviewed.ok) {
            return "refusing: " + (reviewed.fatalError.empty() ? "selective plan failed" : reviewed.fatalError);
        }
        if (!expectTargetLibraryKey.empty() && reviewed.targetLibraryKey != expectTargetLibraryKey) {
            return "refusing: the target KST changed since the review (targetLibraryKey differs); review again";
        }
        if (!expectSelectiveFingerprint.empty() && reviewed.selectiveFingerprint != expectSelectiveFingerprint) {
            return "refusing: the sources or target KST changed since the review (selectiveFingerprint differs); review again";
        }
        if (!reviewed.executable()) {
            const std::string first = reviewed.blockingReasons.empty()
                                          ? std::string("blocking diagnostics or slot conflicts")
                                          : reviewed.blockingReasons.front();
            return "refusing: selective plan is blocked (" + first + ")";
        }
        if (reviewed.basePlan.items.empty()) {
            return "refusing: selective plan produced no kit members";
        }

        // Pin every member root descriptor before Phase::Run (no symlinks followed!)
        std::vector<std::shared_ptr<media::detail::Fd>> roots;
        roots.reserve(reviewed.basePlan.items.size());
        for (const auto& it : reviewed.basePlan.items) {
            std::shared_ptr<media::detail::Fd> fd;
            if (const auto e = pinRoot(it, &fd); !e.empty()) return "refusing: " + e;
            roots.push_back(std::move(fd));
        }

        state->pinnedTargetKey = !expectTargetLibraryKey.empty() ? expectTargetLibraryKey : reviewed.targetLibraryKey;
        state->reviewed = std::move(reviewed);
        state->roots = std::move(roots);
        return "";
    };

    s.dependencies = [state](std::vector<std::string>& notes, uint64_t& sampleBytes) {
        std::vector<std::string> failed;
        for (auto& it : state->reviewed.basePlan.items) {
            std::string d;
            const auto e = checkMember(it.kind, it.source, &d);
            it.status = e.empty() ? kValid : "FAILED";
            it.detail = e.empty() ? d : e;
            (e.empty() ? notes : failed).push_back(it.status + ": " + it.kitPath() + " (" + it.detail + ")");
        }
        sampleBytes = state->reviewed.report.memoryProjection.uniqueLogicalSampleBytes;
        notes.push_back("Selective plan fingerprint: " + state->reviewed.selectiveFingerprint);
        if (!state->reviewed.targetLibraryKey.empty()) {
            notes.push_back("Target KST library key (read-only): " + state->reviewed.targetLibraryKey);
        }
        notes.push_back("Members are copied byte for byte; nothing is converted and no Korg-native file is written. Best possible label: " + std::string(kLabel));
        return failed;
    };

    s.preview = [state] {
        std::vector<OutFile> r;
        for (const auto& it : state->reviewed.basePlan.items) {
            r.push_back({it.kitPath() + (it.kind == Kind::Style ? "" : "/"), 0, "", false});
        }
        r.push_back({"KIT.md, manifest.json, SELECTIVE_MANIFEST.json, DIAGNOSTICS.json, PACKAGE.sha256", 131072, "", false});
        if (!r.empty()) r.front().bytes = state->reviewed.basePlan.bytes;
        return r;
    };

    s.writeAt = [state, expectedShaPtr, srcs = s.sources](int outFd, const fs::path& out,
                                                          const std::function<bool(double)>& progress) -> WriteResult {
        {
            const auto real = detail::fdPath(outFd);
            if (real.empty()) {
                return {"refusing: cannot tell where the staged kit is (no path for the pinned staging folder); nothing was published", ""};
            }
            for (const auto& src : srcs) {
                if (jobs::detail::overlaps(real, src)) {
                    return {"refusing: the staged kit overlaps a source or target KST (the destination changed during the run); nothing was published", ""};
                }
            }
        }

        const auto& items = state->reviewed.basePlan.items;
        const auto& files = state->reviewed.basePlan.files;
        const auto& reviewedSha = state->reviewed.basePlan.sha;

        media::sound::pkgio::FileSet set{};
        set.root = out;
        set.pin(outFd);

        for (size_t i = 0; i < files.size(); ++i) {
            if (!progress(double(i) / double(files.size() + 1))) {
                return {"cancelled by the user while copying; nothing was published", ""};
            }
            const auto& f = files[i];
            uint64_t n = 0;
            std::string sha;
            if (const auto e = copyPinned(state->roots[f.item]->v, f.from, set, f.to, &n, &sha); !e.empty()) {
                return {"cannot copy " + items[f.item].kitPath() + ": " + e, ""};
            }
            if (!expectedShaPtr->empty()) {
                const auto want = expectedShaPtr->find(f.to);
                if (want == expectedShaPtr->end() || want->second != sha) {
                    return {"a source file differs from the review (its content changed since): " + f.to + "; review again; nothing was published", ""};
                }
            }
            if (const auto rit = reviewedSha.find(f.to); rit != reviewedSha.end() && rit->second != sha) {
                return {"a source file changed during the run: " + f.to + "; review again; nothing was published", ""};
            }
            set.want[f.to] = {n, sha};
        }

        if (const auto e = set.checkFiles(); !e.empty()) {
            return {"the copy does not match the bytes read from the sources: " + e, ""};
        }
        for (const auto& it : items) {
            std::string d;
            if (const auto e = checkMember(it.kind, out / it.kitPath(), &d); !e.empty()) {
                return {"the copy of " + it.kitPath() + " does not re-read: " + e, ""};
            }
        }

        const auto name = out.filename().string();
        const std::string md = sd::buildEnhancedKitMd(name, state->reviewed);
        set.save("KIT.md", md);
        set.save("manifest.json", manifestJson(name, items));
        set.save("SELECTIVE_MANIFEST.json", state->reviewed.selectiveManifestJson);
        set.save("DIAGNOSTICS.json", state->reviewed.diagnosticsJson);
        set.saveManifest();
        if (!set.ok) return {"write failed in " + out.string(), ""};
        if (const auto e = set.checkFiles(); !e.empty()) {
            return {"the written kit does not read back as written: " + e, ""};
        }
        return {"", md};
    };

    s.verify = [state](const fs::path& out) {
        Verification v;
        std::string text;
        const auto e = verifyKit(out, &state->reviewed.basePlan.items, &text);
        if (e.empty()) {
            auto pinned = std::make_shared<library::Pinned>();
            library::detail::pin(out, *pinned, /*manifest=*/false);
            if (!pinned->error.empty()) {
                v.l1 = ui::studio::Level{Check::Failed, "cannot pin staged kit: " + pinned->error};
                v.l2 = {Check::NotRun, "not claimed for a kit"};
                return std::pair<Verification, std::string>{v, "NOT verified: " + pinned->error};
            }
            state->stagedPin = std::move(pinned);
        }
        v.l1 = e.empty() ? ui::studio::Level{Check::Passed, std::string(kLabel) + ": every file matches PACKAGE.sha256, selective manifests validate, and every member re-reads"}
                         : ui::studio::Level{Check::Failed, e};
        v.l2 = {Check::NotRun, "not claimed for a kit: its members keep their own levels (KMP and Style SMF are L1 only)"};
        return std::pair<Verification, std::string>{v, e.empty() ? text : "NOT verified: " + e};
    };

    s.beforePublish = [req, state](const fs::path& out) -> std::string {
        if (req.targetKst.has_value()) {
            std::error_code ec;
            const auto tst = fs::symlink_status(*req.targetKst, ec);
            if (ec || !fs::is_directory(tst) || fs::is_symlink(tst)) {
                return "target KST folder became unavailable or a symlink before publish";
            }
            sel::TargetInspectOptions topts;
            topts.hashObjectFiles = true;
            const sel::TargetOccupancyMap nowTarget = sel::inspectTargetKstFolder(*req.targetKst, topts);
            if (!nowTarget.valid || !nowTarget.fullyInspectable || nowTarget.targetLibraryKey != state->pinnedTargetKey) {
                return "target KST changed before publish (targetLibraryKey mismatch); review again";
            }
        }
        if (!state->stagedPin) return "staged kit was not pinned during verification";
        if (const auto pe = library::recheckPinned(*state->stagedPin, out); !pe.empty()) {
            return pe;
        }
        return "";
    };

    s.finished = [state] {
        state->roots.clear();
        state->stagedPin.reset();
    };

    return s;
}

}  // namespace ai_arranger::jobs::kit

#endif  // AI_ARRANGER_JOBS_SELECTIVE_IMPORT_KIT_H
