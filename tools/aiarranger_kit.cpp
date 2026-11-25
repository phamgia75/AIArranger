// aiarranger-kit: the Pa5X Import Kit (roadmap E9 + M2c, src/jobs/import_kit.h, src/jobs/selective_import_kit.h).
//   build --in PATH [--in PATH ...] --out DIR
//       AIArranger outputs (Sound or Drum Kit package folders, KMP/KSF folders, Style .mid files; only read) gathered into
//       the new folder DIR for a USB stick, with KIT.md, manifest.json and PACKAGE.sha256. Runs as an E6 export job:
//       DIR.INCOMPLETE, every member re-read, one no-replace rename, DIR.export.md/.json beside it.
//   verify DIR
//       Re-checks a kit (for example the copy on the stick): PACKAGE.sha256 and every member through its reader.
//   select-plan [--workspace DIR] --style PATH... [--in PATH...] [--bind BANK/SLOT=PKG_PATH...]
//               [--sound-map PATH] [--target-kst KSTDIR] [--policy block|reuse|remap]
//               [--allow-experimental-kmp] [--json]
//       Read-only target-aware selective Pa5X Import Kit preview: dependency collection, deduplication,
//       target slot occupancy/collision checks, memory projection, and recommended free slots.
//   select-build <same flags as select-plan> --out DIR [--expect-fingerprint HEX64] [--move-leftover-aside]
//       Stages, verifies (verifyKit), and atomically publishes a selective Pa5X Import Kit through export_job.h.
// Best possible label: STRUCTURALLY VALID — NOT VERIFIED ON A PA5X.
// Exit codes: 0 = built / verified / executable plan, 3 = analysis ok with blocking diagnostics/conflicts (nothing written),
//             1 = usage error, missing/unreadable input, symlink/TOCTOU/overlap/IO failure.

#include "jobs/import_kit.h"
#include "jobs/selective_import_kit.h"
#include <cstdio>
#include <cstdlib>

using namespace ai_arranger::jobs;
namespace sel = ai_arranger::media::selective;

static int usage() {
    std::fprintf(stderr,
                 "usage: aiarranger-kit build --in PATH [--in PATH ...] --out DIR\n"
                 "       aiarranger-kit verify DIR\n"
                 "       aiarranger-kit select-plan [--workspace DIR] --style PATH [--style PATH ...]\n"
                 "           [--in PATH ...] [--bind BANK/SLOT=PKG_PATH ...] [--sound-map [STYLE=]PATH ...]\n"
                 "           [--target-kst KSTDIR] [--policy block|reuse|remap]\n"
                 "           [--allow-experimental-kmp] [--json]\n"
                 "       aiarranger-kit select-build <select-plan flags> --out DIR\n"
                 "           [--expect-fingerprint HEX64] [--move-leftover-aside]\n");
    return 1;
}

static bool parseBindArg(const std::string& arg, sel::SlotAddress& addrOut, fs::path& pkgOut, std::string& errOut) {
    const size_t eq = arg.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= arg.size()) {
        errOut = "invalid --bind value (expected BANK/SLOT=PKG_PATH): " + arg;
        return false;
    }
    const std::string lhs = arg.substr(0, eq);
    const std::string rhs = arg.substr(eq + 1);
    const size_t slash = lhs.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= lhs.size() || lhs.find('/', slash + 1) != std::string::npos) {
        errOut = "invalid --bind slot address (expected USER01..12/0..127 or USERDK01..06/0..127): " + lhs;
        return false;
    }
    const std::string bankStr = lhs.substr(0, slash);
    const std::string slotStr = lhs.substr(slash + 1);
    for (unsigned char c : slotStr) {
        if (!std::isdigit(c)) {
            errOut = "invalid --bind slot number: " + slotStr;
            return false;
        }
    }
    const unsigned long slotVal = std::strtoul(slotStr.c_str(), nullptr, 10);
    if (slotVal >= sel::kSlotsPerBank) {
        errOut = "slot number out of range (0..127): " + slotStr;
        return false;
    }
    const auto parsed = sel::SlotAddress::fromBankAndSlot(bankStr, static_cast<uint32_t>(slotVal));
    if (!parsed.has_value() || !parsed->isInSpec()) {
        errOut = "invalid --bind bank/slot (expected USER01..12/0..127 or USERDK01..06/0..127): " + lhs;
        return false;
    }
    addrOut = *parsed;
    pkgOut = fs::path(rhs);
    return true;
}

static std::string formatSelectPlanMarkdown(const kit::SelectiveKitPlan& plan) {
    std::string out = sel::serializeSelectiveReportMarkdown(plan.report);
    out += "\n## Selective Import Kit Plan Details\n\n";
    out += "- **Selective Plan Fingerprint (SHA-256):** `" + plan.selectiveFingerprint + "`\n";
    if (!plan.targetLibraryKey.empty()) {
        out += "- **Target KST Library Key (SHA-256):** `" + plan.targetLibraryKey + "`\n";
    }
    out += "- **Planned Kit Members:** " + std::to_string(plan.basePlan.items.size()) +
           " (unused candidate packages excluded: " + std::to_string(plan.excludedUnusedPackages) + ")\n";
    out += "- **SMF Program Change Caveat (K9):** " + std::string(sel::kSmfPcCaveatK9) + "\n";
    if (!plan.slotRecommendations.empty()) {
        out += "\n### Recommended Free Target User Slots\n\n";
        out += "| Kit Package | Requested Slot | Recommended Free Target Slot | Action |\n";
        out += "|---|---|---|---|\n";
        for (const auto& rec : plan.slotRecommendations) {
            out += "| " + sel::diag_detail::mdCell(rec.kitPath) +
                   " | " + sel::diag_detail::mdCell(rec.requestedSlot ? rec.requestedSlot->label() : "-") +
                   " | " + sel::diag_detail::mdCell(rec.recommendedSlot ? rec.recommendedSlot->label() : "BLOCKED") +
                   " | `" + sel::diag_detail::mdCell(rec.action) + "` |\n";
        }
    }
    if (plan.targetPartialInspect.isPartial) {
        out += "\n### Partial Target Inspection (Graceful Degradation)\n\n";
        out += "- **Notice:** " + sel::diag_detail::mdCell(plan.targetPartialInspect.notice) + "\n";
        out += "- **Occupied User Sounds:** " + std::to_string(plan.targetPartialInspect.occupiedUserSounds) +
               ", **Occupied User Drum Kits:** " + std::to_string(plan.targetPartialInspect.occupiedUserDrumKits) +
               ", **PCM Containers:** " + std::to_string(plan.targetPartialInspect.pcmContainerCount) + "\n";
    }
    return out;
}

static int runSelectiveCli(const std::string& cmd, int argc, char** argv) {
    const bool isBuild = (cmd == "select-build");
    kit::SelectiveKitRequest req;
    fs::path outDir;
    std::string expectFp;
    bool moveLeftoverAside = false;
    bool jsonOut = false;
    bool policySet = false;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto needVal = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--workspace") {
            const char* v = needVal("--workspace");
            if (!v || !req.workspaceRoot.empty()) return usage();
            req.workspaceRoot = v;
        } else if (a == "--style") {
            const char* v = needVal("--style");
            if (!v) return usage();
            req.selectedStyles.emplace_back(v);
        } else if (a == "--in") {
            const char* v = needVal("--in");
            if (!v) return usage();
            req.packageInputs.emplace_back(v);
        } else if (a == "--bind") {
            const char* v = needVal("--bind");
            if (!v) return usage();
            sel::SlotAddress addr;
            fs::path pkg;
            std::string berr;
            if (!parseBindArg(v, addr, pkg, berr)) {
                std::fprintf(stderr, "error: %s\n", berr.c_str());
                return 1;
            }
            if (req.slotToPackageMap.count(addr) > 0) {
                std::fprintf(stderr, "error: duplicate --bind for slot %s\n", addr.shortLabel().c_str());
                return 1;
            }
            req.slotToPackageMap[addr] = std::move(pkg);
        } else if (a == "--sound-map") {
            const char* v = needVal("--sound-map");
            if (!v) return usage();
            const std::string sv = v;
            const size_t eq = sv.find('=');
            std::string smKey;
            fs::path smPath;
            if (eq != std::string::npos && eq > 0 && eq + 1 < sv.size() && sv[0] != '/') {
                smKey = sv.substr(0, eq);
                smPath = fs::path(sv.substr(eq + 1));
            } else {
                smKey = "";
                smPath = fs::path(sv);
            }
            if (req.styleSoundMaps.count(smKey) > 0) {
                std::fprintf(stderr, "error: duplicate --sound-map for '%s'\n",
                             smKey.empty() ? "<default>" : smKey.c_str());
                return 1;
            }
            req.styleSoundMaps[smKey] = std::move(smPath);
        } else if (a == "--target-kst") {
            const char* v = needVal("--target-kst");
            if (!v || req.targetKst.has_value()) return usage();
            req.targetKst = fs::path(v);
        } else if (a == "--policy") {
            const char* v = needVal("--policy");
            if (!v) return usage();
            if (policySet) {
                std::fprintf(stderr, "error: duplicate --policy flag\n");
                return 1;
            }
            policySet = true;
            const std::string pv = v;
            if (pv == "block") req.conflictPolicy = sel::ConflictPolicy::BlockWithReport;
            else if (pv == "reuse") req.conflictPolicy = sel::ConflictPolicy::ReuseIdenticalTargetSlot;
            else if (pv == "remap") req.conflictPolicy = sel::ConflictPolicy::RemapToFreeSlotInNewLib;
            else {
                std::fprintf(stderr, "error: unknown --policy '%s' (expected block|reuse|remap)\n", v);
                return 1;
            }
        } else if (a == "--allow-experimental-kmp") {
            req.allowExperimentalKmp = true;
        } else if (a == "--json") {
            jsonOut = true;
        } else if (isBuild && a == "--out") {
            const char* v = needVal("--out");
            if (!v || !outDir.empty()) return usage();
            outDir = fs::path(v);
        } else if (isBuild && a == "--expect-fingerprint") {
            const char* v = needVal("--expect-fingerprint");
            if (!v || !expectFp.empty()) return usage();
            expectFp = v;
            if (expectFp.size() != 64) {
                std::fprintf(stderr, "error: --expect-fingerprint must be a 64-character hex SHA-256\n");
                return 1;
            }
        } else if (isBuild && a == "--move-leftover-aside") {
            moveLeftoverAside = true;
        } else {
            return usage();
        }
    }

    if (req.selectedStyles.empty()) {
        std::fprintf(stderr, "error: at least one --style PATH is required\n");
        return 1;
    }
    if (isBuild && outDir.empty()) {
        std::fprintf(stderr, "error: select-build requires --out DIR\n");
        return 1;
    }

    const kit::SelectiveKitPlan plan = kit::selectivePlan(req);
    if (!plan.ok || plan.cancelled) {
        std::fprintf(stderr, "Selective plan failed: %s\n", plan.fatalError.c_str());
        return 1;
    }

    if (!isBuild) {
        const std::string out = jsonOut ? plan.selectiveManifestJson : formatSelectPlanMarkdown(plan);
        std::fwrite(out.data(), 1, out.size(), stdout);
        return plan.executable() ? 0 : 3;
    }

    // select-build: TOCTOU fingerprint check before touching disk
    if (!expectFp.empty() && plan.selectiveFingerprint != expectFp) {
        std::fprintf(stderr,
                     "Selective kit NOT written: sources or target KST changed since review "
                     "(expected fingerprint %s, got %s)\n",
                     expectFp.c_str(), plan.selectiveFingerprint.c_str());
        return 1;
    }

    if (!plan.executable()) {
        if (jsonOut) {
            std::fwrite(plan.selectiveManifestJson.data(), 1, plan.selectiveManifestJson.size(), stdout);
        }
        std::fprintf(stderr, "Selective kit NOT written (blocked by diagnostics/conflicts; nothing created):\n");
        for (const auto& r : plan.blockingReasons) {
            std::fprintf(stderr, "  %s\n", r.c_str());
        }
        return 3;
    }

    ExportSpec spec = kit::selectiveImportKit(
        req, outDir,
        expectFp.empty() ? plan.selectiveFingerprint : expectFp,
        plan.basePlan.sha,
        plan.targetLibraryKey);
    spec.moveLeftoverAside = moveLeftoverAside;

    JobState st;
    st.id = 1;
    runExport(spec, st, [] { return false; }, [](const JobState&) {});
    if (st.phase != Phase::Done) {
        std::fprintf(stderr, "Selective kit NOT written (%s): %s\n", phaseName(st.stoppedAt), st.reason.c_str());
        for (const auto& n : st.missing) std::fprintf(stderr, "  %s\n", n.c_str());
        return 1;
    }

    if (jsonOut) {
        std::fwrite(plan.selectiveManifestJson.data(), 1, plan.selectiveManifestJson.size(), stdout);
    } else {
        std::printf("Written: %s\n%s\nFingerprint: %s\n", st.dest.c_str(), kit::kLabel, plan.selectiveFingerprint.c_str());
        for (const auto& n : st.notes) std::printf("  %s\n", n.c_str());
        std::printf("Report: %s\n", st.reportPath.c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    if (cmd == "verify" && argc == 3) {
        std::string text;
        const auto e = kit::verifyKit(argv[2], nullptr, &text);
        std::printf("%s\n", e.empty() ? text.c_str() : ("NOT verified: " + e).c_str());
        return e.empty() ? 0 : 1;
    }
    if (cmd == "select-plan" || cmd == "select-build") {
        return runSelectiveCli(cmd, argc, argv);
    }
    if (cmd != "build") return usage();
    std::vector<fs::path> in;
    fs::path out;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--in") in.push_back(argv[i + 1]);
        else if (a == "--out" && out.empty()) out = argv[i + 1];
        else return usage();
    }
    if (argc % 2 || in.empty() || out.empty()) return usage();
    JobState st;
    st.id = 1;
    runExport(kit::importKit(in, out), st, [] { return false; }, [](const JobState&) {});
    if (st.phase != Phase::Done) {
        std::fprintf(stderr, "Kit NOT written (%s): %s\n", phaseName(st.stoppedAt), st.reason.c_str());
        for (const auto& n : st.missing) std::fprintf(stderr, "  %s\n", n.c_str());
        return 1;
    }
    std::printf("Written: %s\n%s\n", st.dest.c_str(), kit::kLabel);
    for (const auto& n : st.notes) std::printf("  %s\n", n.c_str());
    std::printf("Report: %s\n", st.reportPath.c_str());
    return 0;
}
