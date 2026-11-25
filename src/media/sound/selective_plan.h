#ifndef AI_ARRANGER_MEDIA_SOUND_SELECTIVE_PLAN_H
#define AI_ARRANGER_MEDIA_SOUND_SELECTIVE_PLAN_H

// Slice M2d: Native-KST Selective Advisory Export Planner & Verified Advisory Report Export Spec.
//
// Reuses M1 (`native_kst_closure.h`), M2a (`selective_diagnostics.h`, `slot_conflicts.h`), and
// M2b (`m1_manifest_adapter.h`, `selective_collector.h`) directly as single sources of truth.
//
// Boundaries & HOLD Gates (INVARIANTS):
//  * ZERO Native Sound KST export, ZERO OC31 encoder, ZERO `.PCG`/`.KMP`/`.PCM`/`.STG`/`.KSC`/`.PDC` writes.
//  * Source and optional Target KSTs are opened strictly READ-ONLY (`O_NOFOLLOW`, `S_ISREG`).
//  * Optional advisory export writes ONLY human/machine reports (`SELECTIVE_EXPORT_REPORT.md`,
//    `selective-plan.json`, `DIAGNOSTICS.json`, `PACKAGE.sha256`) strictly through `jobs::runExport()`
//    (`export_job.h` `writeAt` + `FileSet::pin(outFd)` + `PACKAGE.sha256` verification + D18 overlap guard).
//  * `ExportMode::NativeSoundKst_HOLD` always fails closed (`NATIVE_SOUND_WRITE_HOLD`).
//  * Enforces all 4 Phase-2 UX Gates:
//      UX-1: Honest CoverageSummary (`deleteProofAvailable: false`, analysed vs speculative UnresolvedSlot split)
//      UX-2: Graceful OC31 KMP degradation (`PartialNativeInspect` with Sound/PCM inventory + `OC31_KMP_UNSUPPORTED`)
//      UX-3: Mandatory `unverified-proxy` + `kStoredPcmProxyCaveat` + `hardwareVerified: false` for Native KST PCMs
//      UX-4: Cooperative `keepGoing` cancellation checked every <= 256 items
//  * Path-free & deterministic: local filesystem paths are scrubbed cleanly; output is byte-identical across
//    shuffled `--select` ordering and repeated runs.

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "jobs/export_job.h"
#include "media/kst_backup.h"
#include "media/sound/package_writer.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "media/sound/m1_manifest_adapter.h"
#include "media/sound/native_kst_closure.h"
#include "media/sound/selective_collector.h"
#include "media/sound/selective_diagnostics.h"
#include "media/sound/slot_conflicts.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ai_arranger::media::selective {

inline constexpr const char* kAdvisoryDiagnosticsSchema = "aiarranger-selective-diagnostics/1";

struct ManualLoadInstruction {
    std::string soundId;                  // e.g. "pa5x:USER03/Sound/12"
    std::string sourceBank;               // e.g. "USER03" or "USERDK01"
    uint32_t sourceSlot = 0;              // 0..127
    std::string soundName;                // printable sanitized name
    std::string targetSlotHint;           // e.g. "USER03/12", "USER04/0", or "BLOCKED"
    bool isDrumKit = false;
    std::vector<std::string> affectedRoots;
    std::string instruction;              // concrete MAN5 p.736 single-Sound load step

    auto operator<=>(const ManualLoadInstruction&) const = default;
};

struct NativeSelectivePlanRequest {
    std::filesystem::path sourceKst;
    std::optional<std::filesystem::path> targetKst;
    std::vector<SelectionTarget> targets;            // 1..N source-relative paths (.stg/.ksc/.pdc)
    ExportMode mode = ExportMode::AdvisoryPlan;      // NativeSoundKst_HOLD fails closed
    ConflictPolicy policy = ConflictPolicy::BlockWithReport;
    bool includeEmbeddedKbdSets = true;
    bool includeEmbeddedPads = true;
    bool requireCompleteSampleClosure = true;
    std::map<graph::ResourceId, graph::ResourceId> approvedSoundRemaps;
    KeepGoing keepGoing = {};                        // UX Gate #4 cooperative cancellation (<= 256 stride)
    size_t cancelCheckStride = kDefaultCancelCheckStride;
};

struct NativeSelectivePlanResult {
    bool ok = false;
    bool cancelled = false;
    bool missingSelectionTarget = false;             // true when >= 1 requested --select path is absent in source KST
    std::string error;                               // path-free error explanation when !ok
    std::string planFingerprint;                     // deterministic 64-hex SHA-256
    SelectiveExportReport report;                    // M2a canonical report (`aiarranger-selective-export-report/1`)
    DryRunExportPlan dryRunPlan;                     // M2b detailed dry-run plan
    PartialNativeInspect sourcePartialInspect;       // UX Gate #2: OC31 / degraded source inventory
    PartialNativeInspect targetPartialInspect;       // UX Gate #2: OC31 / degraded target inventory
    std::vector<SoundResolutionEntry> soundResolutions;
    std::vector<ObjectPathResolutionEntry> objectResolutions;
    std::vector<ManualLoadInstruction> pa5xSingleSoundSteps; // MAN5 p.736 single-Sound load guidance
    std::map<std::string, std::string> sourceFileSha256;

    bool executable() const { return ok && !cancelled && report.executable(); }
};

namespace plan_detail {

inline bool isRealDirectoryNoFollow(const std::filesystem::path& p, std::string& errOut) {
    errOut.clear();
    if (p.empty()) {
        errOut = "path must not be empty";
        return false;
    }
    const std::string clean = graph::native_detail::stripTrailingSlashes(p);
    struct stat lst{};
    if (::lstat(clean.c_str(), &lst) != 0) {
        errOut = "path does not exist or cannot be stat'd";
        return false;
    }
    if (S_ISLNK(lst.st_mode)) {
        errOut = "symlinks are refused (must be a real directory)";
        return false;
    }
    if (!S_ISDIR(lst.st_mode)) {
        errOut = "not a directory";
        return false;
    }
    const graph::native_detail::ScopedFd fd(
        ::open(clean.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (fd.fd < 0) {
        errOut = "cannot open directory read-only (symlinks are refused)";
        return false;
    }
    return true;
}

inline std::string scrubLocalPaths(std::string text,
                                   const std::filesystem::path& src,
                                   const std::optional<std::filesystem::path>& dst) {
    auto replaceAll = [](std::string& s, const std::string& needle, const char* repl) {
        if (needle.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(needle, pos)) != std::string::npos) {
            s.replace(pos, needle.size(), repl);
            pos += std::char_traits<char>::length(repl);
        }
    };
    const std::string sRaw = src.string();
    const std::string sClean = graph::native_detail::stripTrailingSlashes(src);
    replaceAll(text, sRaw, "<source>");
    if (sClean != sRaw) replaceAll(text, sClean, "<source>");
    if (dst.has_value()) {
        const std::string dRaw = dst->string();
        const std::string dClean = graph::native_detail::stripTrailingSlashes(*dst);
        replaceAll(text, dRaw, "<target>");
        if (dClean != dRaw) replaceAll(text, dClean, "<target>");
    }
    return text;
}

// Fast metadata-only PCM baseline byte sum via pread() (zero audio payloads loaded; O_NOFOLLOW + S_ISREG).
inline bool sumTargetPcmBaselineBytesFolder(const std::filesystem::path& targetKst,
                                            uint64_t& bytesOut,
                                            const KeepGoing& keepGoing,
                                            size_t stride,
                                            size_t maxTotalBytes = 256u << 20,
                                            size_t maxDirEntries = 1000000) {
    bytesOut = 0;
    size_t tick = 0;
    if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return false;

    const std::string rootPath = graph::native_detail::stripTrailingSlashes(targetKst);
    const graph::native_detail::ScopedFd rootFd(
        ::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (rootFd.fd < 0) return true;

    std::vector<std::string> topEntries;
    std::string err;
    if (!graph::native_detail::listDirEntriesAt(rootFd.fd, topEntries, maxDirEntries, err)) return true;

    size_t remainingBudget = maxTotalBytes;
    size_t pcmBytesRead = 0;
    graph::PcmInventory inv;
    for (const std::string& top : topEntries) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return false;
        if (graph::native_detail::upperAscii(top) != "PCM") continue;
        struct stat st{};
        if (::fstatat(rootFd.fd, top.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) continue;
        const graph::native_detail::ScopedFd pcmFd(
            ::openat(rootFd.fd, top.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (pcmFd.fd < 0) continue;
        std::vector<std::string> leaves;
        if (!graph::native_detail::listDirEntriesAt(pcmFd.fd, leaves, maxDirEntries, err)) continue;
        for (const std::string& leaf : leaves) {
            if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return false;
            if (!graph::native_detail::endsWithUpper(leaf, ".PCM")) continue;
            std::string perr;
            (void)graph::addPcmContainerFdAt(pcmFd.fd, leaf.c_str(), top + "/" + leaf,
                                             panative::kMaxFileBytes, remainingBudget, pcmBytesRead, inv, &perr);
        }
    }
    for (const auto& [id, pe] : inv.byId) {
        if (!adapter_detail::tickCancel(keepGoing, tick, stride)) return false;
        bytesOut = adapter_detail::satAdd(bytesOut, static_cast<uint64_t>(pe.payloadBytes));
    }
    return true;
}

inline bool isForbiddenNativeExtension(const std::string& relPath) {
    const std::string up = graph::native_detail::upperAscii(relPath);
    return graph::native_detail::endsWithUpper(up, ".PCG") ||
           graph::native_detail::endsWithUpper(up, ".KMP") ||
           graph::native_detail::endsWithUpper(up, ".PCM") ||
           graph::native_detail::endsWithUpper(up, ".STG") ||
           graph::native_detail::endsWithUpper(up, ".KSC") ||
           graph::native_detail::endsWithUpper(up, ".PDC") ||
           graph::native_detail::endsWithUpper(up, ".SBD") ||
           graph::native_detail::endsWithUpper(up, ".GBL");
}

inline std::string serializePartialInspectJson(const PartialNativeInspect& p) {
    auto q = [](const std::string& s) { return "\"" + diag_detail::safeJsonEscape(s) + "\""; };
    auto bstr = [](bool b) -> const char* { return b ? "true" : "false"; };
    std::string o = "{\n";
    o += "      \"isPartial\": " + std::string(bstr(p.isPartial)) + ",\n";
    o += "      \"kmpPresent\": " + std::string(bstr(p.kmpPresent)) + ",\n";
    o += "      \"kmpIsOc31\": " + std::string(bstr(p.kmpIsOc31)) + ",\n";
    o += "      \"kmpParsed\": " + std::string(bstr(p.kmpParsed)) + ",\n";
    o += "      \"kmpError\": " + q(p.kmpError) + ",\n";
    o += "      \"soundContainerCount\": " + std::to_string(p.soundContainerCount) + ",\n";
    o += "      \"occupiedUserSounds\": " + std::to_string(p.occupiedUserSounds) + ",\n";
    o += "      \"occupiedUserDrumKits\": " + std::to_string(p.occupiedUserDrumKits) + ",\n";
    o += "      \"pcmContainerCount\": " + std::to_string(p.pcmContainerCount) + ",\n";
    o += "      \"pcmSampleCount\": " + std::to_string(p.pcmSampleCount) + ",\n";
    o += "      \"notice\": " + q(p.notice) + "\n";
    o += "    }";
    return o;
}

inline std::vector<ManualLoadInstruction> buildPa5xSingleSoundSteps(
    const M1AdapterView& source,
    const DryRunExportPlan& dryRun,
    const std::vector<SoundResolutionEntry>& soundResolutions,
    bool targetPresent) {
    std::map<std::string, const SoundResolutionEntry*> resById;
    for (const auto& sr : soundResolutions) resById[sr.requestId] = &sr;

    std::vector<ManualLoadInstruction> steps;
    for (const CollectedSoundSummary& cs : dryRun.collectedSounds) {
        if (targetPresent && cs.satisfiedByTarget && !cs.requiresRemap) continue;

        ManualLoadInstruction step;
        step.soundId = cs.id.str();
        step.sourceBank = cs.id.scope;
        step.sourceSlot = cs.id.number;
        step.soundName = panative::printable(cs.name);
        step.isDrumKit = cs.isDrumKit;
        step.affectedRoots = cs.referencingRoots;
        std::sort(step.affectedRoots.begin(), step.affectedRoots.end());
        step.affectedRoots.erase(std::unique(step.affectedRoots.begin(), step.affectedRoots.end()),
                                 step.affectedRoots.end());

        std::string slotShort = step.sourceBank + "/" + std::to_string(step.sourceSlot);
        if (const auto sit = source.sounds.find(cs.id); sit != source.sounds.end() && sit->second.slotAddress.has_value()) {
            slotShort = sit->second.slotAddress->shortLabel();
        }

        if (const auto rit = resById.find(step.soundId); rit != resById.end()) {
            const SoundResolutionEntry& r = *rit->second;
            if (r.action == "reuse-target") continue;
            if (r.action == "remap-reuse-target" && r.resolvedAddress.has_value()) {
                step.targetSlotHint = "REUSE_EXISTING " + r.resolvedAddress->shortLabel();
                step.instruction =
                    "Target KST already contains a proven-identical Sound at " + r.resolvedAddress->shortLabel() +
                    ". Do NOT copy Sound bytes; remap referencing Style/KBD Set/Pad tracks from " + slotShort +
                    " to " + r.resolvedAddress->shortLabel() + " (or verify in Style Edit).";
            } else if (r.action == "remap-free-slot" && r.resolvedAddress.has_value()) {
                step.targetSlotHint = r.resolvedAddress->shortLabel();
                step.instruction =
                    "On Pa5X: Media -> Load -> open source .KST -> SOUND/" + step.sourceBank + ".PCG -> select single Sound '" +
                    step.soundName + "' (slot " + std::to_string(step.sourceSlot) + ") -> load into free target slot " +
                    r.resolvedAddress->shortLabel() + " (MAN5 p.736 single-Sound load preserves existing User Samples in RAM; "
                    "NEVER load a whole KST containing PCM/), then remap referencing tracks to " +
                    r.resolvedAddress->shortLabel() + ".";
            } else if (r.blocking) {
                step.targetSlotHint = "BLOCKED (" + slotShort + " conflict)";
                step.instruction =
                    "Slot " + slotShort + " is blocked in Target KST (" + r.detail +
                    "). Re-run with --policy remap to pick a free target slot, then use Pa5X Media -> Load single Sound "
                    "(MAN5 p.736; preserves existing User Samples in RAM; NEVER load a whole KST containing PCM/).";
            } else {
                step.targetSlotHint = r.resolvedAddress.has_value() ? r.resolvedAddress->shortLabel() : slotShort;
                step.instruction =
                    "On Pa5X: Media -> Load -> open source .KST -> SOUND/" + step.sourceBank + ".PCG -> select single Sound '" +
                    step.soundName + "' (slot " + std::to_string(step.sourceSlot) + ") -> load into target slot " +
                    step.targetSlotHint + " (MAN5 p.736 single-Sound load preserves existing User Samples in RAM; "
                    "NEVER load a whole KST containing PCM/).";
            }
        } else {
            step.targetSlotHint = slotShort;
            step.instruction =
                "On Pa5X: Media -> Load -> open source .KST -> SOUND/" + step.sourceBank + ".PCG -> select single Sound '" +
                step.soundName + "' (slot " + std::to_string(step.sourceSlot) + ") -> load into slot " + slotShort +
                " (MAN5 p.736 single-Sound load preserves existing User Samples in RAM; NEVER load a whole KST containing PCM/).";
        }
        if (step.isDrumKit) {
            step.instruction += " Note: User Drum Kit internal DrumSample edges are unparsed offline; verify all drum samples load on Pa5X.";
        }
        steps.push_back(std::move(step));
    }
    std::sort(steps.begin(), steps.end(), [](const ManualLoadInstruction& a, const ManualLoadInstruction& b) {
        return std::tie(a.isDrumKit, a.sourceBank, a.sourceSlot, a.soundId) <
               std::tie(b.isDrumKit, b.sourceBank, b.sourceSlot, b.soundId);
    });
    return steps;
}

}  // namespace plan_detail

// In-memory / pre-adapted entry point (order-independent, deterministic, zero disk I/O).
inline NativeSelectivePlanResult planNativeSelectiveExportFromViews(
    const M1AdapterView& source,
    const NativeSelectivePlanRequest& req,
    const TargetOccupancyMap* optionalTargetMap = nullptr,
    uint64_t targetBaselineBytes = 0) {
    const size_t stride = adapter_detail::clampStride(req.cancelCheckStride);
    size_t tick = 0;
    auto makeCancelled = [&]() {
        NativeSelectivePlanResult c;
        c.ok = false;
        c.cancelled = true;
        c.error = "selective export planning cancelled";
        c.report.mode = req.mode;
        c.report.conflictPolicy = req.policy;
        c.report.coverage.deleteProofAvailable = false;
        c.report.memoryProjection.sampleBytesAreNativeStoredProxy = true;
        return c;
    };
    if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();
    if (source.cancelled || (optionalTargetMap && optionalTargetMap->cancelled)) return makeCancelled();

    NativeSelectivePlanResult out;
    out.sourcePartialInspect = source.partialInspect;
    if (optionalTargetMap) out.targetPartialInspect = optionalTargetMap->partialInspect;

    CollectorRequest creq;
    creq.targets = req.targets;
    creq.mode = req.mode;
    creq.conflictPolicy = req.policy;
    creq.includeEmbeddedKbdSets = req.includeEmbeddedKbdSets;
    creq.includeEmbeddedPads = req.includeEmbeddedPads;
    creq.requireCompleteSampleClosure = req.requireCompleteSampleClosure;
    creq.approvedSoundRemaps = req.approvedSoundRemaps;
    creq.cancelCheckStride = stride;

    DryRunExportPlan dry = collectSelectiveExportWithTargetMap(
        source, creq, optionalTargetMap, targetBaselineBytes, optionalTargetMap != nullptr, req.keepGoing);
    if (dry.cancelled) return makeCancelled();

    // Detect any requested --select path missing from source KST (used by CLI for exit 1)
    for (const auto& d : dry.diagnostics) {
        if (d.code == collector_diag_code::kSelectedRootNotFound) {
            out.missingSelectionTarget = true;
            break;
        }
    }

    // Build canonical M2a SelectiveExportReport with scrubbed path-free labels ("<source>" / "<target>")
    SelectiveExportReport rep = toSelectiveExportReport(dry, "<source>", "<target>");
    // Enforce UX Gate #1 & UX Gate #3 invariants unconditionally on Native KST plans
    rep.coverage.deleteProofAvailable = false;
    rep.memoryProjection.sampleBytesAreNativeStoredProxy = true;

    // Populate Sound & Object resolutions from M2a CollisionMatrix (or fail-closed invalid target / no-target checks)
    if (dry.collisionMatrix.has_value()) {
        out.soundResolutions = dry.collisionMatrix->soundResolutions;
        out.objectResolutions = dry.collisionMatrix->objectResolutions;
    } else if (optionalTargetMap != nullptr && !optionalTargetMap->valid) {
        // P2-4: Explicit fail-closed branch when target inspection failed (NEVER claim "no target KST supplied"!)
        const std::string reason = optionalTargetMap->uninspectableReason.empty()
                                       ? std::string("target KST inspection failed; refusing fail-closed")
                                       : optionalTargetMap->uninspectableReason;
        for (const CollectedSoundSummary& cs : dry.collectedSounds) {
            if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();
            const auto sit = source.sounds.find(cs.id);
            std::optional<SlotAddress> addr =
                (sit != source.sounds.end() && sit->second.slotAddress.has_value())
                    ? sit->second.slotAddress
                    : SlotAddress::fromBankAndSlot(cs.id.scope, cs.id.number);
            if (!addr.has_value()) continue;
            SoundResolutionEntry sr;
            sr.requestId = cs.id.str();
            sr.sourceAddress = *addr;
            sr.resolvedAddress = std::nullopt;
            sr.remapped = false;
            sr.action = "blocked";
            sr.equivalence = EquivalenceVerdict::InsufficientEvidence;
            sr.blocking = true;
            sr.detail = reason;
            out.soundResolutions.push_back(std::move(sr));
        }
        std::sort(out.soundResolutions.begin(), out.soundResolutions.end());

        for (const SelectionTarget& sel : dry.selectedTargets) {
            if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();
            const std::string eff = conflict_detail::applyTargetPathOverride(sel.relPath, sel.targetBank, sel.targetName);
            ObjectPathResolutionEntry ores;
            ores.kind = sel.kind;
            ores.sourceRelPath = sel.relPath;
            ores.targetRelPath = eff;
            ores.foldedTargetRelPath = conflict_detail::asciiLower(eff);
            ores.action = "blocked";
            ores.blocking = true;
            ores.detail = reason;
            out.objectResolutions.push_back(std::move(ores));
        }
        std::sort(out.objectResolutions.begin(), out.objectResolutions.end());
    } else {
        for (const CollectedSoundSummary& cs : dry.collectedSounds) {
            if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();
            const auto sit = source.sounds.find(cs.id);
            std::optional<SlotAddress> addr =
                (sit != source.sounds.end() && sit->second.slotAddress.has_value())
                    ? sit->second.slotAddress
                    : SlotAddress::fromBankAndSlot(cs.id.scope, cs.id.number);
            if (!addr.has_value()) continue;
            SoundResolutionEntry sr;
            sr.requestId = cs.id.str();
            sr.sourceAddress = *addr;
            sr.resolvedAddress = *addr;
            sr.remapped = false;
            sr.action = "advisory-source-slot";
            sr.equivalence = EquivalenceVerdict::InsufficientEvidence;
            sr.blocking = false;
            sr.detail = "no target KST supplied; preserving source slot in advisory plan";
            out.soundResolutions.push_back(std::move(sr));
        }
        std::sort(out.soundResolutions.begin(), out.soundResolutions.end());

        // P2-4: Two-pass intra-batch path validation + FAT32/exFAT ASCII case-folded R-18 collision detection
        // even when no target KST is supplied, mirroring every added conflict/diagnostic into `dry` so
        // `out.dryRunPlan.executable` and `out.report.executable()` NEVER diverge!
        std::vector<std::optional<std::string>> effPaths(dry.selectedTargets.size());
        std::vector<std::string> effErrs(dry.selectedTargets.size());
        std::vector<std::string> effFolded(dry.selectedTargets.size());
        std::map<std::string, std::vector<size_t>> batchByFolded;
        std::map<std::string, std::set<std::string>> batchDirsByFolded;

        for (size_t i = 0; i < dry.selectedTargets.size(); ++i) {
            if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();
            const SelectionTarget& sel = dry.selectedTargets[i];
            std::string perr;
            effPaths[i] = conflict_detail::resolveTargetPathOverride(sel.relPath, sel.targetBank, sel.targetName, &perr);
            if (!effPaths[i].has_value()) {
                effErrs[i] = std::move(perr);
                continue;
            }
            effFolded[i] = conflict_detail::asciiLower(*effPaths[i]);
            batchByFolded[effFolded[i]].push_back(i);
            for (const std::string& dir : conflict_detail::parentDirectoryPrefixes(*effPaths[i])) {
                batchDirsByFolded[conflict_detail::asciiLower(dir)].insert(dir);
            }
        }

        bool anyNoTargetPathFinding = false;
        auto recordFinding = [&](SlotConflict sc, Diagnostic dg) {
            anyNoTargetPathFinding = true;
            rep.conflicts.push_back(sc);
            rep.diagnostics.push_back(dg);
            dry.conflicts.push_back(std::move(sc));
            dry.diagnostics.push_back(std::move(dg));
        };

        for (size_t i = 0; i < dry.selectedTargets.size(); ++i) {
            if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();
            const SelectionTarget& sel = dry.selectedTargets[i];
            ObjectPathResolutionEntry ores;
            ores.kind = sel.kind;
            ores.sourceRelPath = sel.relPath;
            if (!effPaths[i].has_value()) {
                ores.action = "blocked-invalid-path";
                ores.blocking = true;
                ores.detail = effErrs[i];
                recordFinding(
                    {SlotConflict::Kind::StyleOrObjectPathCollision,
                     sel.relPath, sel.relPath, "(invalid target path override)",
                     "RENAME_REQUIRED", true, "Invalid target path override refused fail-closed: " + effErrs[i]},
                    {DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                     diag_code::kStyleOrObjectPathCollision,
                     DiagnosticSeverity::Blocking,
                     EvidenceLevel::Verified,
                     sel.relPath, sel.targetBank + "/" + sel.targetName,
                     "Target path override is invalid (" + effErrs[i] + "); refusing fail-closed.",
                     "Provide safe relative bank/file names without '..', leading/trailing '/', or path separators in targetName.",
                     {sel.relPath}});
                out.objectResolutions.push_back(std::move(ores));
                continue;
            }

            const std::string& eff = *effPaths[i];
            const std::string& folded = effFolded[i];
            ores.targetRelPath = eff;
            ores.foldedTargetRelPath = folded;

            if (batchByFolded[folded].size() > 1 || batchDirsByFolded.count(folded) > 0) {
                ores.action = "blocked-fat-case-r18";
                ores.blocking = true;
                ores.detail = "batch FAT32/exFAT R-18 case collision";
                recordFinding(
                    {SlotConflict::Kind::StyleOrObjectPathCollision,
                     eff, sel.relPath, "(batch case-folded collision)",
                     "RENAME_REQUIRED", true,
                     "Multiple selected objects collide under ASCII case-folding (FAT32/exFAT R-18)."},
                    {DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                     diag_code::kFatCaseCollisionR18,
                     DiagnosticSeverity::Blocking,
                     EvidenceLevel::Verified,
                     eff, folded,
                     "Output path collides with another selected item under ASCII case-folding (FAT32/exFAT R-18).",
                     "Provide distinct targetBank or targetName overrides before export.",
                     {sel.relPath}});
                out.objectResolutions.push_back(std::move(ores));
                continue;
            }

            std::string dirCollisionTarget;
            std::string dirCollisionDetail;
            for (const std::string& dir : conflict_detail::parentDirectoryPrefixes(eff)) {
                const std::string fd = conflict_detail::asciiLower(dir);
                if (batchByFolded.count(fd) > 0) {
                    dirCollisionTarget = fd;
                    dirCollisionDetail = "Directory prefix '" + dir + "' collides case-insensitively with a selected file path (FAT32/exFAT R-18).";
                    break;
                }
                if (const auto bit = batchDirsByFolded.find(fd); bit != batchDirsByFolded.end() && bit->second.size() > 1) {
                    for (const std::string& other : bit->second) {
                        if (other != dir) { dirCollisionTarget = other; break; }
                    }
                    dirCollisionDetail = "Batch bank/type directory '" + dir + "' collides case-insensitively with '" +
                                         dirCollisionTarget + "' (FAT32/exFAT R-18).";
                    break;
                }
            }
            if (!dirCollisionDetail.empty()) {
                ores.action = "blocked-fat-case-r18";
                ores.blocking = true;
                ores.detail = dirCollisionDetail;
                recordFinding(
                    {SlotConflict::Kind::StyleOrObjectPathCollision,
                     eff, sel.relPath, dirCollisionTarget,
                     "RENAME_REQUIRED", true, dirCollisionDetail},
                    {DiagnosticCategory::AMBIGUOUS_OR_DUPLICATE,
                     diag_code::kFatCaseCollisionR18,
                     DiagnosticSeverity::Blocking,
                     EvidenceLevel::Verified,
                     eff, dirCollisionTarget, dirCollisionDetail,
                     "Use consistent bank directory casing or distinct targetBank values to eliminate the FAT32/exFAT R-18 collision.",
                     {sel.relPath}});
                out.objectResolutions.push_back(std::move(ores));
                continue;
            }

            ores.action = "allow-new";
            ores.blocking = false;
            ores.detail = "no target KST supplied; advisory target path";
            out.objectResolutions.push_back(std::move(ores));
        }
        std::sort(out.objectResolutions.begin(), out.objectResolutions.end());
        if (anyNoTargetPathFinding) {
            canonicalizeConflicts(dry.conflicts);
            canonicalizeDiagnostics(dry.diagnostics);
            dry.executable = dry.ok && !dry.cancelled &&
                             dry.blockingDiagnosticCount() == 0 &&
                             dry.blockingConflictCount() == 0 &&
                             req.mode != ExportMode::NativeSoundKst_HOLD;
            dry.planFingerprint = collector_detail::computePlanFingerprint(dry);
        }
        rep = canonicalizeReport(std::move(rep));
    }

    out.pa5xSingleSoundSteps = plan_detail::buildPa5xSingleSoundSteps(
        source, dry, out.soundResolutions, optionalTargetMap != nullptr);
    out.sourceFileSha256 = dry.sourceFileSha256;
    out.planFingerprint = dry.planFingerprint;
    rep.planFingerprint = dry.planFingerprint;
    out.report = std::move(rep);
    out.dryRunPlan = std::move(dry);
    out.ok = out.dryRunPlan.ok;
    if (!out.ok && out.error.empty()) {
        out.error = !source.containerErrors.empty()
                        ? plan_detail::scrubLocalPaths(source.containerErrors.front(), req.sourceKst, req.targetKst)
                        : "selective export analysis failed";
    }
    return out;
}

// End-to-end strictly READ-ONLY disk entry point for Native-KST selective advisory planning.
inline NativeSelectivePlanResult planNativeSelectiveExport(const NativeSelectivePlanRequest& req) {
    const size_t stride = adapter_detail::clampStride(req.cancelCheckStride);
    size_t tick = 0;
    auto makeCancelled = [&]() {
        NativeSelectivePlanResult c;
        c.ok = false;
        c.cancelled = true;
        c.error = "selective export planning cancelled";
        c.report.mode = req.mode;
        c.report.conflictPolicy = req.policy;
        c.report.coverage.deleteProofAvailable = false;
        c.report.memoryProjection.sampleBytesAreNativeStoredProxy = true;
        return c;
    };
    if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();

    std::string dirErr;
    if (!plan_detail::isRealDirectoryNoFollow(req.sourceKst, dirErr)) {
        NativeSelectivePlanResult fail;
        fail.ok = false;
        fail.error = "cannot open source KST <source>: " + dirErr;
        fail.report.mode = req.mode;
        fail.report.conflictPolicy = req.policy;
        fail.report.coverage.deleteProofAvailable = false;
        fail.report.memoryProjection.sampleBytesAreNativeStoredProxy = true;
        return fail;
    }
    if (req.targetKst.has_value() && !plan_detail::isRealDirectoryNoFollow(*req.targetKst, dirErr)) {
        NativeSelectivePlanResult fail;
        fail.ok = false;
        fail.error = "cannot open target KST <target>: " + dirErr;
        fail.report.mode = req.mode;
        fail.report.conflictPolicy = req.policy;
        fail.report.coverage.deleteProofAvailable = false;
        fail.report.memoryProjection.sampleBytesAreNativeStoredProxy = true;
        return fail;
    }

    // 1. Run M1 read-only closure on sourceKst with UX Gate #4 cooperative keepGoing
    graph::NativeKstClosureOptions closureOpts;
    closureOpts.keepGoing = req.keepGoing;
    const graph::NativeKstClosureResult m1 = graph::resolveNativeKstClosureFolder(req.sourceKst, closureOpts);
    if (m1.cancelled) return makeCancelled();
    if (!adapter_detail::tickCancel(req.keepGoing, tick, stride)) return makeCancelled();

    // 2. Extract Sound equivalence metadata & hash only the requested selected .stg/.ksc/.pdc files (O_NOFOLLOW)
    NativeEquivalenceExtra extra;
    if (m1.ok) {
        if (!extractNativeEquivalenceExtraFolder(req.sourceKst, extra, req.keepGoing, stride,
                                                 closureOpts.downstream.maxTotalBytes,
                                                 closureOpts.maxDirEntries,
                                                 closureOpts.downstream.soundMode)) {
            return makeCancelled();
        }
        std::vector<std::string> selectedRels;
        selectedRels.reserve(req.targets.size());
        for (const auto& t : req.targets) selectedRels.push_back(conflict_detail::normalizeRelPath(t.relPath));
        if (!hashNativeRelFilesFolder(req.sourceKst, selectedRels, extra.fileSha256, req.keepGoing, stride)) {
            return makeCancelled();
        }
    }

    M1AdapterView srcView = adaptFromM1Closure(m1, extra, req.keepGoing, stride);
    if (srcView.cancelled) return makeCancelled();

    // 3. Optionally inspect targetKst strictly read-only via M2a inspectTargetKstFolder()
    std::optional<TargetOccupancyMap> targetMap;
    uint64_t targetBaselineBytes = 0;
    if (req.targetKst.has_value()) {
        TargetInspectOptions tOpts;
        tOpts.keepGoing = req.keepGoing;
        TargetOccupancyMap tm = inspectTargetKstFolder(*req.targetKst, tOpts);
        if (tm.cancelled) return makeCancelled();
        if (tm.valid) {
            if (!plan_detail::sumTargetPcmBaselineBytesFolder(*req.targetKst, targetBaselineBytes, req.keepGoing, stride)) {
                return makeCancelled();
            }
        }
        targetMap = std::move(tm);
    }

    return planNativeSelectiveExportFromViews(
        srcView, req, targetMap.has_value() ? &(*targetMap) : nullptr, targetBaselineBytes);
}

// ── Deterministic Path-Free Serializers ──────────────────────────────────────────────────────────────────────────────

// Emits `aiarranger-selective-export-report/1` JSON enriched with additive UX Gate #2 `partialInspect`,
// `soundResolutions`, `objectResolutions`, and `pa5xSingleSoundSteps` blocks.
inline std::string serializeNativeSelectivePlanJson(const NativeSelectivePlanResult& res) {
    std::string base = serializeSelectiveReportJson(res.report);
    auto q = [](const std::string& s) { return "\"" + diag_detail::safeJsonEscape(s) + "\""; };
    auto bstr = [](bool b) -> const char* { return b ? "true" : "false"; };
    auto writeStrVec = [&](std::string& o, const std::vector<std::string>& v) {
        o += "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ", "; o += q(v[i]); }
        o += "]";
    };

    // Splice additive top-level fields right before the closing "\n}\n" of serializeSelectiveReportJson().
    const size_t closePos = base.rfind("\n}\n");
    if (closePos == std::string::npos) return base;
    base.erase(closePos);

    std::string ext;
    ext.reserve(2048 + res.soundResolutions.size() * 160 + res.pa5xSingleSoundSteps.size() * 256);

    // UX Gate #2: structured PartialNativeInspect for source & target
    // P3-1: when only target is partial (e.g. target OC31 KMP), mirror target counts in top-level shorthand fields
    const PartialNativeInspect& primaryPartial =
        (!res.sourcePartialInspect.isPartial && res.targetPartialInspect.isPartial)
            ? res.targetPartialInspect
            : res.sourcePartialInspect;
    ext += ",\n  \"partialInspect\": {\n";
    ext += "    \"isPartial\": " + std::string(bstr(res.sourcePartialInspect.isPartial || res.targetPartialInspect.isPartial)) + ",\n";
    ext += "    \"occupiedUserSounds\": " + std::to_string(primaryPartial.occupiedUserSounds) + ",\n";
    ext += "    \"occupiedUserDrumKits\": " + std::to_string(primaryPartial.occupiedUserDrumKits) + ",\n";
    ext += "    \"pcmContainerCount\": " + std::to_string(primaryPartial.pcmContainerCount) + ",\n";
    ext += "    \"pcmSampleCount\": " + std::to_string(primaryPartial.pcmSampleCount) + ",\n";
    ext += "    \"notice\": " + q(!primaryPartial.notice.empty()
                                      ? primaryPartial.notice
                                      : (!res.sourcePartialInspect.notice.empty()
                                             ? res.sourcePartialInspect.notice
                                             : res.targetPartialInspect.notice)) + ",\n";
    ext += "    \"source\": " + plan_detail::serializePartialInspectJson(res.sourcePartialInspect) + ",\n";
    ext += "    \"target\": " + plan_detail::serializePartialInspectJson(res.targetPartialInspect) + "\n";
    ext += "  },\n";

    // Sound resolutions
    ext += "  \"soundResolutions\": [";
    for (size_t i = 0; i < res.soundResolutions.size(); ++i) {
        const auto& sr = res.soundResolutions[i];
        ext += i ? ",\n    {" : "\n    {";
        ext += "\"requestId\": " + q(sr.requestId) +
               ", \"sourceSlot\": " + q(sr.sourceAddress.shortLabel()) +
               ", \"resolvedSlot\": " + (sr.resolvedAddress.has_value() ? q(sr.resolvedAddress->shortLabel()) : std::string("null")) +
               ", \"remapped\": " + std::string(bstr(sr.remapped)) +
               ", \"action\": " + q(sr.action) +
               ", \"equivalence\": " + q(equivalenceVerdictName(sr.equivalence)) +
               ", \"blocking\": " + std::string(bstr(sr.blocking)) +
               ", \"detail\": " + q(sr.detail) + "}";
    }
    ext += res.soundResolutions.empty() ? "],\n" : "\n  ],\n";

    // Object resolutions
    ext += "  \"objectResolutions\": [";
    for (size_t i = 0; i < res.objectResolutions.size(); ++i) {
        const auto& ores = res.objectResolutions[i];
        ext += i ? ",\n    {" : "\n    {";
        ext += "\"kind\": " + q(selectedKindName(ores.kind)) +
               ", \"sourceRelPath\": " + q(ores.sourceRelPath) +
               ", \"targetRelPath\": " + q(ores.targetRelPath) +
               ", \"action\": " + q(ores.action) +
               ", \"blocking\": " + std::string(bstr(ores.blocking)) +
               ", \"detail\": " + q(ores.detail) + "}";
    }
    ext += res.objectResolutions.empty() ? "],\n" : "\n  ],\n";

    // Actionable MAN5 p.736 Pa5X single-Sound load steps
    ext += "  \"pa5xSingleSoundSteps\": [";
    for (size_t i = 0; i < res.pa5xSingleSoundSteps.size(); ++i) {
        const auto& st = res.pa5xSingleSoundSteps[i];
        ext += i ? ",\n    {" : "\n    {";
        ext += "\"soundId\": " + q(st.soundId) +
               ", \"sourceBank\": " + q(st.sourceBank) +
               ", \"sourceSlot\": " + std::to_string(st.sourceSlot) +
               ", \"soundName\": " + q(st.soundName) +
               ", \"isDrumKit\": " + std::string(bstr(st.isDrumKit)) +
               ", \"targetSlotHint\": " + q(st.targetSlotHint) +
               ", \"affectedRoots\": ";
        writeStrVec(ext, st.affectedRoots);
        ext += ", \"instruction\": " + q(st.instruction) + "}";
    }
    ext += res.pa5xSingleSoundSteps.empty() ? "]\n}\n" : "\n  ]\n}\n";

    base += ext;
    return base;
}

inline std::string serializeDiagnosticsOnlyJson(const SelectiveExportReport& rawReport) {
    const SelectiveExportReport r = canonicalizeReport(rawReport);
    auto q = [](const std::string& s) { return "\"" + diag_detail::safeJsonEscape(s) + "\""; };
    auto bstr = [](bool b) -> const char* { return b ? "true" : "false"; };
    auto writeStrVec = [&](std::string& o, const std::vector<std::string>& v) {
        o += "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ", "; o += q(v[i]); }
        o += "]";
    };

    std::string o;
    o.reserve(1024 + r.diagnostics.size() * 256 + r.conflicts.size() * 192);
    o += "{\n";
    o += "  \"schema\": " + q(kAdvisoryDiagnosticsSchema) + ",\n";
    o += "  \"label\": " + q(kHardwareUnverifiedBanner) + ",\n";
    o += "  \"hardwareVerified\": false,\n";
    o += "  \"deleteProofAvailable\": false,\n";
    o += "  \"planFingerprint\": " + q(r.planFingerprint) + ",\n";
    o += "  \"executable\": " + std::string(bstr(r.executable())) + ",\n";
    o += "  \"coverageComplete\": " + std::string(bstr(r.coverage.coverageComplete)) + ",\n";
    o += "  \"blockingIssues\": " + std::to_string(r.blockingDiagnosticCount()) + ",\n";
    o += "  \"blockingConflicts\": " + std::to_string(r.blockingConflictCount()) + ",\n";
    o += "  \"warnings\": " + std::to_string(r.warningDiagnosticCount()) + ",\n";
    o += "  \"infos\": " + std::to_string(r.infoDiagnosticCount()) + ",\n";

    o += "  \"conflicts\": [";
    for (size_t i = 0; i < r.conflicts.size(); ++i) {
        const auto& c = r.conflicts[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"kind\": " + q(slotConflictKindName(c.kind)) +
             ", \"address\": " + q(c.address) +
             ", \"sourceLabel\": " + q(c.sourceLabel) +
             ", \"targetLabel\": " + q(c.targetLabel) +
             ", \"resolution\": " + q(c.resolution) +
             ", \"blocking\": " + std::string(bstr(c.blocking)) +
             ", \"detail\": " + q(c.detail) + "}";
    }
    o += r.conflicts.empty() ? "],\n" : "\n  ],\n";

    o += "  \"diagnostics\": [";
    for (size_t i = 0; i < r.diagnostics.size(); ++i) {
        const auto& d = r.diagnostics[i];
        o += i ? ",\n    {\n" : "\n    {\n";
        o += "      \"category\": " + q(diagnosticCategoryName(d.category)) + ",\n";
        o += "      \"code\": " + q(d.code) + ",\n";
        o += "      \"severity\": " + q(diagnosticSeverityName(d.severity)) + ",\n";
        o += "      \"evidence\": " + q(evidenceLevelName(d.evidence)) + ",\n";
        o += "      \"subject\": " + q(d.subject) + ",\n";
        o += "      \"referencedTarget\": " + q(d.referencedTarget) + ",\n";
        o += "      \"summary\": " + q(d.summary) + ",\n";
        o += "      \"remediation\": " + q(d.remediation) + ",\n";
        o += "      \"affectedRoots\": ";
        writeStrVec(o, d.affectedRoots);
        o += "\n    }";
    }
    o += r.diagnostics.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

inline std::string serializeNativeSelectivePlanMarkdown(const NativeSelectivePlanResult& res) {
    std::string o = serializeSelectiveReportMarkdown(res.report);

    if (res.sourcePartialInspect.isPartial || res.targetPartialInspect.isPartial) {
        o += "## Partial Native Inspection (Degraded / OC31)\n\n";
        auto emitPartial = [&](const char* who, const PartialNativeInspect& p) {
            if (!p.isPartial) return;
            o += "- **" + std::string(who) + ":** " + panative::printable(p.notice.empty() ? p.kmpError : p.notice) + "\n";
            o += "  - Sound containers: " + std::to_string(p.soundContainerCount) +
                 ", occupied User Sounds: " + std::to_string(p.occupiedUserSounds) +
                 ", occupied User Drum Kits: " + std::to_string(p.occupiedUserDrumKits) + "\n";
            o += "  - PCM containers: " + std::to_string(p.pcmContainerCount) +
                 ", PCM samples: " + std::to_string(p.pcmSampleCount) + "\n";
        };
        emitPartial("Source KST", res.sourcePartialInspect);
        emitPartial("Target KST", res.targetPartialInspect);
        o += "\n";
    }

    if (!res.soundResolutions.empty()) {
        o += "## Sound Slot Resolutions\n\n";
        o += "| Request | Source Slot | Resolved Slot | Action | Equivalence | Blocking | Detail |\n";
        o += "|---|---|---|---|---|---|---|\n";
        for (const auto& sr : res.soundResolutions) {
            o += "| `" + diag_detail::mdCell(sr.requestId) + "`" +
                 " | `" + diag_detail::mdCell(sr.sourceAddress.shortLabel()) + "`" +
                 " | `" + diag_detail::mdCell(sr.resolvedAddress ? sr.resolvedAddress->shortLabel() : "-") + "`" +
                 " | `" + diag_detail::mdCell(sr.action) + "`" +
                 " | `" + std::string(equivalenceVerdictName(sr.equivalence)) + "`" +
                 " | " + std::string(sr.blocking ? "YES" : "no") +
                 " | " + diag_detail::mdCell(sr.detail) + " |\n";
        }
        o += "\n";
    }

    if (!res.pa5xSingleSoundSteps.empty()) {
        o += "## Pa5X Single-Sound Load Steps (MAN5 p.736)\n\n";
        o += "> **Important (MAN5 p.736):** Loading a whole `.KST` containing a `PCM/` folder deletes ALL User Samples in Pa5X RAM. "
             "Load each required Sound individually via **Media -> Load -> open source .KST -> SOUND/*.PCG** to preserve existing RAM samples.\n\n";
        for (size_t i = 0; i < res.pa5xSingleSoundSteps.size(); ++i) {
            const auto& st = res.pa5xSingleSoundSteps[i];
            o += std::to_string(i + 1) + ". **`" + diag_detail::mdCell(st.soundId) + "`** (" +
                 diag_detail::mdCell(st.soundName) + ") -> `" + diag_detail::mdCell(st.targetSlotHint) + "`\n";
            o += "   - " + panative::printable(st.instruction) + "\n";
        }
        o += "\n";
    }

    o += "## Advisory Caveats\n\n";
    o += "- **Export Scope:** Advisory plan ONLY — zero native KST binaries (`.PCG`, `.KMP`, `.PCM`, `.STG`, `.KSC`, `.PDC`) are written.\n";
    o += "- **SMF Program Changes (K9):** " + std::string(kSmfPcCaveatK9) + "\n";
    o += "- **SongBook Boundary:** " + std::string(kSongBookUnrewrittenWarning) + "\n";
    return o;
}

// ── Verified Advisory Plan Export Job (`export_job.h` + `FileSet::pin(outFd)` + D18 Overlap Guard) ──────────────────
// Writes ONLY advisory reports (`SELECTIVE_EXPORT_REPORT.md`, `selective-plan.json`, `DIAGNOSTICS.json`,
// `PACKAGE.sha256`) into a NEW local folder. Zero native KST bytes are ever written!
inline jobs::ExportSpec advisoryPlanExportSpec(
    const NativeSelectivePlanRequest& req,
    const NativeSelectivePlanResult& reviewed,
    std::filesystem::path outDir) {
    namespace fs = std::filesystem;

    struct PayloadState {
        std::string reportMd;
        std::string planJson;
        std::string diagJson;
        std::string fingerprint;
        bool ok = false;
        bool cancelled = false;
        ExportMode mode = ExportMode::AdvisoryPlan;
        size_t blockingIssues = 0;
        size_t blockingConflicts = 0;
        std::map<std::string, std::string> verifiedSha;
    };
    const auto st = std::make_shared<PayloadState>();
    st->ok = reviewed.ok;
    st->cancelled = reviewed.cancelled;
    st->mode = req.mode;
    st->fingerprint = reviewed.planFingerprint;
    st->blockingIssues = reviewed.report.blockingDiagnosticCount();
    st->blockingConflicts = reviewed.report.blockingConflictCount();
    if (reviewed.ok && !reviewed.cancelled) {
        st->reportMd = serializeNativeSelectivePlanMarkdown(reviewed);
        st->planJson = serializeNativeSelectivePlanJson(reviewed);
        st->diagJson = serializeDiagnosticsOnlyJson(reviewed.report);
    }

    jobs::ExportSpec s;
    s.type = "Native-KST selective advisory plan (EXPORT-ONLY)";
    s.source = req.targetKst.has_value() ? "<source> -> <target>" : "<source>";
    s.sources.push_back(req.sourceKst);
    if (req.targetKst.has_value()) s.sources.push_back(*req.targetKst);
    s.dest = std::move(outDir);

    const fs::path srcPath = req.sourceKst;
    const std::optional<fs::path> tgtPath = req.targetKst;
    const fs::path destSnap = s.dest;

    s.validate = [st, srcPath, tgtPath, destSnap]() -> std::string {
        if (st->mode == ExportMode::NativeSoundKst_HOLD) {
            return "refusing (" + std::string(diag_code::kNativeSoundWriteHold) +
                   "): Native Sound KST export is on HOLD";
        }
        if (st->cancelled) return "refusing: selective plan was cancelled";
        if (!st->ok) return "refusing: selective plan analysis did not succeed";
        if (const auto why = media::outputFolderRefusal(destSnap);
            !why.empty() && why != media::incompleteRefusal(destSnap)) {
            return why;
        }
        std::string dirErr;
        if (!plan_detail::isRealDirectoryNoFollow(srcPath, dirErr)) {
            return "refusing: source KST is unreadable or not a real directory (" + dirErr + ")";
        }
        if (tgtPath.has_value() && !plan_detail::isRealDirectoryNoFollow(*tgtPath, dirErr)) {
            return "refusing: target KST is unreadable or not a real directory (" + dirErr + ")";
        }
        if (jobs::detail::overlaps(destSnap, srcPath)) {
            return "refusing: advisory plan output folder overlaps source KST (D18)";
        }
        if (tgtPath.has_value() && jobs::detail::overlaps(destSnap, *tgtPath)) {
            return "refusing: advisory plan output folder overlaps target KST (D18)";
        }
        return {};
    };

    s.dependencies = [st](std::vector<std::string>& notes, uint64_t& sampleBytes) {
        sampleBytes = 0;  // advisory report only: zero sample bytes written
        notes.push_back("EXPORT-ONLY advisory plan: zero native KST binaries (.PCG/.KMP/.PCM/.STG/.KSC/.PDC) are written.");
        notes.push_back("Plan fingerprint: " + st->fingerprint);
        notes.push_back("Findings recorded: blockingDiagnostics=" + std::to_string(st->blockingIssues) +
                        ", blockingConflicts=" + std::to_string(st->blockingConflicts));
        return std::vector<std::string>{};
    };

    s.preview = [st]() {
        return std::vector<jobs::OutFile>{
            {"DIAGNOSTICS.json", st->diagJson.size(), "", true},
            {"PACKAGE.sha256", 512, "", false},
            {"SELECTIVE_EXPORT_REPORT.md", st->reportMd.size(), "", true},
            {"selective-plan.json", st->planJson.size(), "", true}
        };
    };

    s.writeAt = [st, srcs = s.sources](int outFd, const fs::path& out,
                                       const std::function<bool(double)>& progress) -> jobs::WriteResult {
        const std::string real = media::detail::fdPath(outFd);
        if (real.empty()) {
            return {"refusing: cannot confirm real path of pinned staging folder; nothing published", ""};
        }
        for (const auto& src : srcs) {
            if (jobs::detail::overlaps(real, src)) {
                return {"refusing: staged advisory plan overlaps a source/target KST (D18); nothing published", ""};
            }
        }
        if (!progress(0.1)) return {"cancelled before writing advisory plan", ""};

        media::sound::pkgio::FileSet set;
        set.root = out;
        set.pin(outFd);
        set.save("SELECTIVE_EXPORT_REPORT.md", st->reportMd);
        if (!progress(0.4)) return {"cancelled while writing advisory plan", ""};
        set.save("selective-plan.json", st->planJson);
        if (!progress(0.7)) return {"cancelled while writing advisory plan", ""};
        set.save("DIAGNOSTICS.json", st->diagJson);
        set.saveManifest();
        if (!set.ok) return {"write failed while staging advisory plan files", ""};
        if (const auto err = set.checkFiles(); !err.empty()) {
            return {"staged advisory plan read-back failed: " + err, ""};
        }
        if (!progress(1.0)) return {"cancelled after writing advisory plan", ""};
        return {"", "Advisory plan staged (4 files including PACKAGE.sha256; zero native KST bytes written)."};
    };

    auto verifyAdvisoryDir = [st](const fs::path& out, std::string& detailOut) -> bool {
        st->verifiedSha.clear();
        const fs::path man = out / media::sound::pkgio::FileSet::kPackageManifest;
        size_t files = 0;
        const auto skip = [](const std::string& rel) {
            const std::string leaf = fs::path(rel).filename().string();
            return rel == media::sound::pkgio::FileSet::kPackageManifest ||
                   leaf == ".DS_Store" || leaf.rfind("._", 0) == 0;
        };
        const std::string bad = media::verifyManifest(out, man, {}, &files, skip, true);
        if (!bad.empty()) {
            detailOut = "PACKAGE.sha256 verification failed: " + bad;
            return false;
        }
        if (files != 3) {
            detailOut = "expected exactly 3 advisory files in PACKAGE.sha256, found " + std::to_string(files);
            return false;
        }
        media::detail::Listing ls;
        std::string lerr;
        if (!media::detail::listFiles(out, ls, lerr)) {
            detailOut = "cannot list staged advisory folder: " + lerr;
            return false;
        }
        if (!ls.skipped.empty()) {
            detailOut = "refusing symlink or special file in staged advisory folder";
            return false;
        }
        static const std::set<std::string> kExpectedFiles = {
            "SELECTIVE_EXPORT_REPORT.md", "selective-plan.json", "DIAGNOSTICS.json", "PACKAGE.sha256"
        };
        for (const auto& [rel, entry] : ls.files) {
            if (plan_detail::isForbiddenNativeExtension(rel)) {
                detailOut = "HOLD VIOLATION: forbidden native KST file present in advisory output: " + rel;
                return false;
            }
            if (!kExpectedFiles.count(rel)) {
                detailOut = "unexpected file in advisory output: " + rel;
                return false;
            }
            bool cancelled = false;
            std::string serr;
            const std::string sha = media::detail::sha256File(entry.abs, {}, &cancelled, &serr);
            if (sha.empty()) {
                detailOut = "failed to re-hash " + rel + ": " + serr;
                return false;
            }
            st->verifiedSha[rel] = sha;
        }
        detailOut = "L1: all 3 advisory report files match PACKAGE.sha256; zero native KST bytes written (" +
                    std::string(kHardwareUnverifiedBanner) + ").";
        return true;
    };

    s.verify = [verifyAdvisoryDir](const fs::path& out) {
        jobs::Verification v;
        std::string detail;
        const bool ok = verifyAdvisoryDir(out, detail);
        v.l1 = ok ? ui::studio::Level{jobs::Check::Passed, detail}
                  : ui::studio::Level{jobs::Check::Failed, detail};
        v.l2 = {jobs::Check::NotRun, "not applicable: advisory plan document"};
        v.l3 = {jobs::Check::NotRun, "not verified on a Pa5X"};
        return std::pair<jobs::Verification, std::string>{v, detail};
    };

    s.verifiedHashes = [st]() { return st->verifiedSha; };

    s.beforePublish = [verifyAdvisoryDir](const fs::path& out) -> std::string {
        std::string detail;
        return verifyAdvisoryDir(out, detail) ? std::string{} : detail;
    };

    return s;
}

}  // namespace ai_arranger::media::selective

#endif  // AI_ARRANGER_MEDIA_SOUND_SELECTIVE_PLAN_H
