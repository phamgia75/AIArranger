// aiarranger-resources: resource graph and memory budget over AIArranger packages
// (docs/research/PA5X_SOUND_RESOURCE_GRAPH.md; media/sound/resource_graph.h). Read-only.
//   budget [--json] [--kst KSTDIR] PKG...   unique-sample memory vs the documented Pa5X limits
//   check PKG...                            dangling/missing references, resources not provably safe to delete
//   where-used ID PKG...                    direct users and the top-level Sounds/Drum Kits that reach ID
//   delete-check ID PKG...                  InUse / Unprovable / Unreferenced, with reasons
//   remap FROM TO PKG...                    references a remap would rewrite, conflicts, warnings
//   memory-compare BEFORE AFTER PKG...      transcribed Pa5X Memory Info readings vs the raw PCM prediction
//   panative inspect FILE|FOLDER            READ-ONLY / EXPERIMENTAL: Pa-native (SET) container check + item
//                                           names/slots (media/panative/panative_reader.h); exit 3 = an invalid file, 1 = unreadable
//   validate FOLDER                         file-level structural check of a KST or SET folder against the rule table
//                                           of its layout (library/library_validate.h); exit 3 = an ERROR finding, 1 = unreadable
//   kst-native closure KSTDIR               READ-ONLY / EXPERIMENTAL: user Sound references of every .STG/.KSC/.PDC vs SOUND/*.PCG
//                                           (media/panative/kst_closure.h); exit 3 = dangling or unresolved references, 1 = unreadable objects
//   kst-native demo OUTDIR                  EXPERIMENTAL, NOT HARDWARE-VERIFIED: writes the three W6 test KSTs (one Pad, one
//                                           KBD Set, one Style; factory/GM sounds, no samples) as NEW folders in OUTDIR
//   kst-native from-set SETDIR OUT.KST       SET -> KST phase 1 (media/panative/set_to_kst.h): the SET's PCM/PCG/KMP/SBL
//                                           byte-identical in the NEW folder OUT.KST (export job, report beside it); Styles,
//                                           KBD Sets, Pads, SongBook are BLOCKED and listed. NOT VERIFIED ON A PA5X
// Every command also takes --evidence FILE (repeatable, before the other arguments): an owner-authorized
// aiarranger-evidence file (media/sound/evidence_import.h) added as a separate evidence source.
// PKG is a package folder written by aiarranger-sound (it holds RESOURCES.tsv); every sample is checked
// against its WAV. --kst only summarises an undecoded KST folder (kst-inventory): its resources are
// unknown and never enter the graph or the sample count.
// Exit 0 = ok (budget fits / delete provable / remap conflict-free), 3 = not, 1 = usage or input error.
#include "library/library_validate.h"
#include "media/kst_inventory.h"
#include "media/panative/kst_closure.h"
#include "media/panative/kst_demo.h"
#include "jobs/export_specs.h"   // kst-native from-set: the SET -> KST export job (E9 publish)
#include "jobs/selective_sub_kst.h"
#include "media/panative/panative_reader.h"
#include "media/sound/evidence_import.h"
#include "media/sound/memory_check.h"
#include "media/sound/selective_plan.h"
#include <cstdio>
#include <set>

using namespace ai_arranger::media;
using namespace ai_arranger::media::sound;
using graph::ResourceId;

static int usage() {
    std::fprintf(stderr, "usage: aiarranger-resources budget [--json] [--kst KSTDIR] PKG...\n"
                         "       aiarranger-resources check PKG...\n"
                         "       aiarranger-resources where-used ID PKG...\n"
                         "       aiarranger-resources delete-check ID PKG...\n"
                         "       aiarranger-resources remap FROM TO PKG...\n"
                         "       aiarranger-resources memory-compare BEFORE AFTER PKG...\n"
                         "       aiarranger-resources panative inspect FILE|FOLDER   (read-only, experimental)\n"
                         "       aiarranger-resources validate FOLDER   (KST or SET folder, file level, read-only)\n"
                         "       aiarranger-resources kst-native closure KSTDIR   (read-only, experimental)\n"
                         "       aiarranger-resources kst-native selective-plan --source SRC.KST [--target DST.KST]\n"
                         "           --select REL [--target-bank BANK] [--target-name NAME] [--select ...]\n"
                         "           [--policy block|reuse|remap] [--no-embedded-kbdsets] [--no-embedded-pads]\n"
                         "           [--json] [--export-plan OUT_DIR]   (read-only advisory plan; zero native KST writes)\n"
                         "       aiarranger-resources kst-native selective-export --source SRC.KST [--target DST.KST]\n"
                         "           --select REL [--target-bank BANK] [--target-name NAME] [--select ...]\n"
                         "           [--policy block|reuse|remap] [--approve-remap SRC=DST] --out OUT.KST\n"
                         "           [--expect-fingerprint HEX64] [--move-leftover-aside] [--json]\n"
                         "           (EXPERIMENTAL Root-tree sub-KST ONLY; ZERO PCM/SOUND/MULTISMP/SONGBOOK; NOT VERIFIED ON A PA5X)\n"
                         "       aiarranger-resources kst-native demo OUTDIR   (writes 3 NOT HARDWARE-VERIFIED test KSTs)\n"
                         "       aiarranger-resources kst-native from-set SETDIR OUT.KST   (resources only, NOT VERIFIED ON A PA5X)\n"
                         "  any command: --evidence FILE (owner-authorized evidence, repeatable)\n");
    return 1;
}
static std::string edgeText(const graph::Edge& e) {
    return e.from.str() + " -" + graph::viaName(e.via) + "-> " + e.to.str() + "  (" + e.detail + ")" +
           (e.confidence == graph::Confidence::Inferred ? "  INFERRED, not proven" : "") +
           (e.source == graph::Source::PaManagerReport ? "  [evidence: " + e.provenance + "]" : "");
}

static int panativeInspect(const std::string& path) {
    int invalid = 0, unreadable = 0;
    std::string err;
    const std::string rep = panative::inspect(path, &invalid, &unreadable, &err);
    if (rep.empty()) { std::fprintf(stderr, "ERROR: %s\n", err.c_str()); return 1; }
    std::fputs(rep.c_str(), stdout);
    return unreadable ? 1 : invalid ? 3 : 0;
}

struct SelectiveKstCliArgs {
    std::string sourceKst;
    std::optional<std::string> targetKst;
    std::vector<ai_arranger::media::selective::SelectionTarget> targets;
    ai_arranger::media::selective::ConflictPolicy policy =
        ai_arranger::media::selective::ConflictPolicy::BlockWithReport;
    bool includeEmbeddedKbdSets = true;
    bool includeEmbeddedPads = true;
    bool jsonOut = false;
    std::string exportPlanDir;        // selective-plan only
    std::string outKstDir;            // selective-export only
    std::string expectFingerprint;    // selective-export only
    bool moveLeftoverAside = false;   // selective-export only
    std::map<graph::ResourceId, graph::ResourceId> approvedSoundRemaps; // selective-export only
};

static int parseSelectiveKstArgs(int argc, char** argv, bool isExport, SelectiveKstCliArgs& out) {
    using namespace ai_arranger::media::selective;
    bool hasSource = false;
    bool hasTarget = false;
    bool hasPolicy = false;
    bool hasOut = false;
    bool hasExpectFingerprint = false;
    bool hasTargetBankForCurrent = false;
    bool hasTargetNameForCurrent = false;

    auto inferKind = [](const std::string& rel) -> SelectedKind {
        if (graph::native_detail::endsWithUpper(rel, ".KSC")) return SelectedKind::KeyboardSet;
        if (graph::native_detail::endsWithUpper(rel, ".PDC")) return SelectedKind::Pad;
        return SelectedKind::Style;
    };

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto needVal = [&](std::string& v) -> bool {
            if (i + 1 >= argc) return false;
            v = argv[++i];
            return true;
        };
        if (a == "--source") {
            std::string v;
            if (hasSource || !needVal(v) || v.empty()) return usage();
            out.sourceKst = std::move(v);
            hasSource = true;
        } else if (a == "--target") {
            std::string v;
            if (hasTarget || !needVal(v) || v.empty()) return usage();
            out.targetKst = std::move(v);
            hasTarget = true;
        } else if (a == "--select") {
            std::string v;
            if (!needVal(v) || v.empty()) return usage();
            SelectionTarget t;
            t.relPath = v;
            t.kind = inferKind(v);
            out.targets.push_back(std::move(t));
            hasTargetBankForCurrent = false;
            hasTargetNameForCurrent = false;
        } else if (a == "--target-bank") {
            std::string v;
            if (out.targets.empty() || hasTargetBankForCurrent || !needVal(v) || v.empty()) return usage();
            out.targets.back().targetBank = std::move(v);
            hasTargetBankForCurrent = true;
        } else if (a == "--target-name") {
            std::string v;
            if (out.targets.empty() || hasTargetNameForCurrent || !needVal(v) || v.empty()) return usage();
            out.targets.back().targetName = std::move(v);
            hasTargetNameForCurrent = true;
        } else if (a == "--policy") {
            std::string v;
            if (hasPolicy || !needVal(v)) return usage();
            if (v == "block") out.policy = ConflictPolicy::BlockWithReport;
            else if (v == "reuse") out.policy = ConflictPolicy::ReuseIdenticalTargetSlot;
            else if (v == "remap") out.policy = ConflictPolicy::RemapToFreeSlotInNewLib;
            else {
                std::fprintf(stderr, "ERROR: invalid --policy '%s' (expected block|reuse|remap)\n",
                             panative::printable(v).c_str());
                return 1;
            }
            hasPolicy = true;
        } else if (a == "--json") {
            out.jsonOut = true;
        } else if (!isExport && a == "--no-embedded-kbdsets") {
            out.includeEmbeddedKbdSets = false;
        } else if (!isExport && a == "--no-embedded-pads") {
            out.includeEmbeddedPads = false;
        } else if (!isExport && a == "--export-plan") {
            std::string v;
            if (!out.exportPlanDir.empty() || !needVal(v) || v.empty()) return usage();
            out.exportPlanDir = std::move(v);
        } else if (isExport && a == "--out") {
            std::string v;
            if (hasOut || !needVal(v) || v.empty()) return usage();
            out.outKstDir = std::move(v);
            hasOut = true;
        } else if (isExport && a == "--expect-fingerprint") {
            std::string v;
            if (hasExpectFingerprint || !needVal(v) || !ai_arranger::jobs::subkst::detail::isHex64(v)) {
                std::fprintf(stderr, "ERROR: --expect-fingerprint requires a single 64-character hex SHA-256\n");
                return 1;
            }
            out.expectFingerprint = std::move(v);
            hasExpectFingerprint = true;
        } else if (isExport && a == "--move-leftover-aside") {
            out.moveLeftoverAside = true;
        } else if (isExport && a == "--approve-remap") {
            std::string v;
            if (!needVal(v)) return usage();
            const size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 >= v.size()) {
                std::fprintf(stderr, "ERROR: invalid --approve-remap '%s' (expected SRC_SLOT=DST_SLOT, e.g. USER01/5=USER04/12)\n",
                             panative::printable(v).c_str());
                return 1;
            }
            graph::ResourceId srcId, dstId;
            if (!ai_arranger::jobs::subkst::parseSlotOrResourceId(v.substr(0, eq), srcId) ||
                !ai_arranger::jobs::subkst::parseSlotOrResourceId(v.substr(eq + 1), dstId)) {
                std::fprintf(stderr, "ERROR: invalid Sound slot in --approve-remap '%s'\n",
                             panative::printable(v).c_str());
                return 1;
            }
            if (!out.approvedSoundRemaps.emplace(srcId, dstId).second) {
                std::fprintf(stderr, "ERROR: duplicate --approve-remap source slot '%s'\n",
                             srcId.str().c_str());
                return 1;
            }
        } else {
            return usage();
        }
    }

    if (!hasSource || out.targets.empty()) return usage();
    if (isExport && !hasOut) return usage();
    return 0;
}

static int kstNativeSelectivePlan(int argc, char** argv) {
    using namespace ai_arranger::media::selective;
    SelectiveKstCliArgs cli;
    if (const int rc = parseSelectiveKstArgs(argc, argv, false, cli); rc != 0) return rc;

    NativeSelectivePlanRequest req;
    req.sourceKst = cli.sourceKst;
    if (cli.targetKst.has_value()) req.targetKst = *cli.targetKst;
    req.targets = std::move(cli.targets);
    req.mode = ExportMode::AdvisoryPlan;
    req.policy = cli.policy;
    req.includeEmbeddedKbdSets = cli.includeEmbeddedKbdSets;
    req.includeEmbeddedPads = cli.includeEmbeddedPads;

    const NativeSelectivePlanResult res = planNativeSelectiveExport(req);
    if (!res.ok || res.cancelled) {
        std::fprintf(stderr, "ERROR: %s\n", res.error.empty() ? "selective plan failed" : res.error.c_str());
        return 1;
    }
    if (res.missingSelectionTarget) {
        for (const auto& d : res.report.diagnostics) {
            if (d.code == collector_diag_code::kSelectedRootNotFound) {
                std::fprintf(stderr, "ERROR: selected path not found in source KST: %s\n",
                             panative::printable(d.subject).c_str());
            }
        }
        return 1;
    }

    if (!cli.exportPlanDir.empty()) {
        ai_arranger::jobs::JobState st;
        st.id = 1;
        ai_arranger::jobs::runExport(
            advisoryPlanExportSpec(req, res, cli.exportPlanDir),
            st,
            [] { return false; },
            [](const ai_arranger::jobs::JobState&) {});
        if (st.phase != ai_arranger::jobs::Phase::Done) {
            std::fprintf(stderr, "ERROR: advisory plan NOT written (%s): %s\n",
                         ai_arranger::jobs::phaseName(st.stoppedAt), st.reason.c_str());
            return 1;
        }
    }

    const std::string output = cli.jsonOut
                                   ? serializeNativeSelectivePlanJson(res)
                                   : serializeNativeSelectivePlanMarkdown(res);
    std::fputs(output.c_str(), stdout);
    return res.executable() ? 0 : 3;
}

static int kstNativeSelectiveExport(int argc, char** argv) {
    using namespace ai_arranger::media::selective;
    using namespace ai_arranger::jobs::subkst;
    SelectiveKstCliArgs cli;
    if (const int rc = parseSelectiveKstArgs(argc, argv, true, cli); rc != 0) return rc;

    SubKstExportRequest req;
    req.sourceKst = cli.sourceKst;
    if (cli.targetKst.has_value()) req.targetKst = *cli.targetKst;
    req.targets = std::move(cli.targets);
    req.mode = ExportMode::RootTreeSubKst;
    req.policy = cli.policy;
    req.approvedSoundRemaps = std::move(cli.approvedSoundRemaps);
    req.moveLeftoverAside = cli.moveLeftoverAside;
    req.expectFingerprint = cli.expectFingerprint;

    // 1. Pre-check destination symlink / overlap / output-guard / safe .KST name before heavy KST reads
    std::error_code absEc;
    const std::filesystem::path destAbs =
        ai_arranger::media::folderPath(std::filesystem::absolute(cli.outKstDir, absEc));
    {
        std::error_code symEc;
        const auto destStat = std::filesystem::symlink_status(destAbs, symEc);
        if (symEc && destStat.type() != std::filesystem::file_type::not_found) {
            std::fprintf(stderr, "ERROR: cannot inspect destination path: %s\n",
                         panative::printable(symEc.message()).c_str());
            return 1;
        }
        if (std::filesystem::is_symlink(destStat)) {
            std::fprintf(stderr, "ERROR: refusing destination symlink (symlinks are never followed or replaced)\n");
            return 1;
        }
    }
    const std::string leaf = destAbs.filename().string();
    if (leaf.size() <= 4 || leaf.compare(leaf.size() - 4, 4, ".KST") != 0 ||
        !panative::assemble::safeName(leaf.substr(0, leaf.size() - 4))) {
        std::fprintf(stderr, "ERROR: output folder must be named NAME.KST with a safe NAME: %s\n",
                     panative::printable(leaf).c_str());
        return 1;
    }
    if (ai_arranger::jobs::detail::overlaps(destAbs, req.sourceKst) ||
        (req.targetKst.has_value() && ai_arranger::jobs::detail::overlaps(destAbs, *req.targetKst))) {
        std::fprintf(stderr, "ERROR: refusing D18 overlap — output folder overlaps source or target KST\n");
        return 1;
    }

    // 2. Plan & evaluate eligibility strictly READ-ONLY (zero disk writes if blocked!)
    const SubKstExportPlan reviewed = planSubKstExport(req);
    if (!reviewed.ok || reviewed.cancelled) {
        std::fprintf(stderr, "ERROR: %s\n",
                     reviewed.error.empty() ? "selective export planning failed"
                                            : panative::printable(reviewed.error).c_str());
        return 1;
    }
    if (reviewed.missingSelectionTarget) {
        for (const auto& d : reviewed.planResult.report.diagnostics) {
            if (d.code == collector_diag_code::kSelectedRootNotFound) {
                std::fprintf(stderr, "ERROR: selected path not found in source KST: %s\n",
                             panative::printable(d.subject).c_str());
            }
        }
        return 1;
    }
    if (!req.expectFingerprint.empty() && reviewed.planFingerprint != req.expectFingerprint) {
        std::fprintf(stderr, "ERROR: sources or target changed since review (planFingerprint mismatch); nothing written\n");
        return 1;
    }
    if (!reviewed.executable()) {
        // Exit 3: analysis succeeded, blocked by HOLD / missing Sound / conflict / malformed MIDI / OC31.
        // Emit full deterministic JSON or Markdown report to stdout and write NOTHING to disk!
        const std::string output = cli.jsonOut
                                       ? serializeNativeSelectivePlanJson(reviewed.planResult)
                                       : serializeNativeSelectivePlanMarkdown(reviewed.planResult);
        std::fputs(output.c_str(), stdout);
        return 3;
    }

    // 3. Pin the reviewed planFingerprint & targetLibraryKey into the E6/E9 export job (TOCTOU guard!)
    req.expectFingerprint = reviewed.planFingerprint;
    req.expectTargetLibraryKey = reviewed.targetLibraryKey;

    const auto finalPlan = std::make_shared<SubKstExportPlan>();
    const auto execSummary = std::make_shared<SubKstExecutionSummary>();
    ai_arranger::jobs::JobState st;
    st.id = 1;
    ai_arranger::jobs::runExport(
        selectiveSubKstExportSpec(req, cli.outKstDir, finalPlan, execSummary),
        st,
        [] { return false; },
        [](const ai_arranger::jobs::JobState&) {});

    if (st.phase != ai_arranger::jobs::Phase::Done) {
        std::fprintf(stderr, "ERROR: sub-KST NOT written (%s): %s\n",
                     ai_arranger::jobs::phaseName(st.stoppedAt), panative::printable(st.reason).c_str());
        return 1;
    }
    execSummary->published = true;

    const std::string output = cli.jsonOut
                                   ? serializeSubKstExportJson(*finalPlan, *execSummary, st)
                                   : serializeSubKstExportMarkdown(*finalPlan, *execSummary, st);
    std::fputs(output.c_str(), stdout);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    if (cmd == "validate") {
        if (argc != 3) return usage();
        const auto v = ai_arranger::library::validate(argv[2]);
        std::fputs(ai_arranger::library::validationText(v).c_str(), v.ok ? stdout : stderr);
        return !v.ok ? 1 : v.count(ai_arranger::library::Severity::Error) ? 3 : 0;
    }
    if (cmd == "kst-native" && argc >= 3 && std::string(argv[2]) == "selective-plan") {
        return kstNativeSelectivePlan(argc, argv);
    }
    if (cmd == "kst-native" && argc >= 3 && std::string(argv[2]) == "selective-export") {
        return kstNativeSelectiveExport(argc, argv);
    }
    if (cmd == "kst-native" && argc == 4 && std::string(argv[2]) == "closure") {
        const auto c = panative::closure::check(argv[3]);
        for (const auto& p : c.problems) std::fprintf(stderr, "UNREADABLE: %s\n", p.c_str());
        for (const auto& [ref, n] : c.dangling) std::printf("DANGLING %s (%zu)\n", ref.c_str(), n);
        std::printf("Root trees %zu, MIDI leaves %zu; user refs resolved %zu, dangling %zu (slot empty %zu, file absent %zu); factory/GM %zu; unresolved %zu%s\n",
                    c.trees, c.midis, c.resolved, c.danglingCount(), c.slotEmpty, c.fileAbsent, c.factory, c.unresolved, c.error.empty() ? "" : (" ERROR: " + c.error).c_str());
        return !c.ok ? 1 : c.danglingCount() || c.unresolved ? 3 : 0;
    }
    if (cmd == "kst-native" && argc == 5 && std::string(argv[2]) == "from-set") {   // D54 B1: SET -> KST phase 1
        ai_arranger::jobs::JobState st;
        st.id = 1;
        ai_arranger::jobs::runExport(ai_arranger::jobs::setToKst(argv[3], argv[4]), st, [] { return false; }, [](const ai_arranger::jobs::JobState&) {});
        if (st.phase != ai_arranger::jobs::Phase::Done) {
            std::fprintf(stderr, "KST NOT written (%s): %s\n", ai_arranger::jobs::phaseName(st.stoppedAt), st.reason.c_str());
            return 1;
        }
        std::printf("Written: %s\n%s\n%s\nReport: %s\n", st.dest.c_str(), st.verifyText.c_str(), st.writerText.c_str(), st.reportPath.c_str());
        return 0;
    }
    if (cmd == "kst-native" && argc == 4 && std::string(argv[2]) == "demo") {
        int bad = 0;
        for (const auto& k : panative::demo::kits()) {
            const auto r = panative::assemble::build(argv[3], k.name, k.objects);
            std::printf("%s: %s\n  files %zu, validator %zu error(s) %zu warning(s), user refs %zu, dangling %zu, factory/GM refs %zu, manifest %s\n",
                        k.name.c_str(), r.ok ? "OK" : "FAILED", r.files, r.validatorErrors, r.validatorWarnings, r.userRefs, r.dangling, r.factoryRefs,
                        r.manifest.string().c_str());
            std::printf("  %s\n", r.message.c_str());
            bad += !r.ok;
        }
        std::printf("NOT HARDWARE-VERIFIED: no Pa5X has loaded these files. Omitted (UNKNOWN, not fabricated):\n");
        for (const auto& u : panative::build::unknownOmitted()) std::printf("  - %s\n", u.c_str());
        return bad ? 3 : 0;
    }
    if (cmd == "panative") return argc == 4 && std::string(argv[2]) == "inspect" ? panativeInspect(argv[3]) : usage();
    std::vector<std::string> args(argv + 2, argv + argc);
    bool json = false;
    std::string kst;
    std::vector<std::string> evidence;
    std::vector<ResourceId> ids;
    size_t want = cmd == "where-used" || cmd == "delete-check" ? 1 : cmd == "remap" ? 2 : 0;
    if (cmd != "budget" && cmd != "check" && cmd != "memory-compare" && !want) return usage();
    while (!args.empty() && args[0].rfind("--", 0) == 0) {
        if (args[0] == "--json" && cmd == "budget") { json = true; args.erase(args.begin()); }
        else if (args[0] == "--kst" && cmd == "budget" && args.size() > 1) { kst = args[1]; args.erase(args.begin(), args.begin() + 2); }
        else if (args[0] == "--evidence" && args.size() > 1) { evidence.push_back(args[1]); args.erase(args.begin(), args.begin() + 2); }
        else return usage();
    }
    std::string beforeFile, afterFile;
    if (cmd == "memory-compare") {
        if (args.size() < 3) return usage();
        beforeFile = args[0]; afterFile = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    for (size_t i = 0; i < want; ++i) {
        ResourceId id;
        if (args.empty() || !ResourceId::parse(args[0], id)) { std::fprintf(stderr, "not a resource id: %s\n", args.empty() ? "" : args[0].c_str()); return 1; }
        ids.push_back(id);
        args.erase(args.begin());
    }
    if (args.empty()) return usage();

    graph::Graph g;
    std::set<std::string> pkgs;
    for (const auto& dir : args) {
        graph::Graph one;
        std::string pkg, err;
        if (!loadPackage(dir, one, pkg, &err)) { std::fprintf(stderr, "ERROR: %s\n", err.c_str()); return 1; }
        if (!pkgs.insert(pkg).second) {
            std::fprintf(stderr, "ERROR: package %s given twice (%s): one import or two? Refusing to guess.\n", pkg.c_str(), dir.c_str());
            return 1;
        }
        if (!g.merge(one, &err)) { std::fprintf(stderr, "ERROR: %s\n", err.c_str()); return 1; }
    }
    auto slurp = [](const std::string& f, std::string& out) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(f, ec)) || std::filesystem::file_size(f, ec) > (16u << 20) || ec) return false;
        std::ifstream in(f, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(in), {});
        return !in.bad();
    };
    std::set<std::string> labels;
    for (const auto& f : evidence) {
        std::string t, label, err;
        if (!slurp(f, t)) { std::fprintf(stderr, "ERROR: cannot read evidence %s (regular file, at most 16 MiB)\n", f.c_str()); return 1; }
        if (!importEvidence(t, g, &label, &err)) { std::fprintf(stderr, "ERROR: %s: %s\n", f.c_str(), err.c_str()); return 1; }
        if (!labels.insert(label).second) { std::fprintf(stderr, "ERROR: evidence source '%s' given twice\n", label.c_str()); return 1; }
    }
    if (cmd == "memory-compare") {
        std::string bt, at, err;
        MemoryInfoReading b, a;
        if (!slurp(beforeFile, bt) || !slurp(afterFile, at)) { std::fprintf(stderr, "ERROR: cannot read a Memory Info reading\n"); return 1; }
        if (!parseMemoryInfo(bt, b, &err) || !parseMemoryInfo(at, a, &err)) { std::fprintf(stderr, "ERROR: %s\n", err.c_str()); return 1; }
        std::fputs(compareMemoryInfo(b, a, predict(g)).c_str(), stdout);
        return 0;
    }

    if (cmd == "budget") {
        const auto b = graph::budget(g);
        std::string kstText, kstJson;
        if (!kst.empty()) {
            const Inventory inv = scan(kst);
            if (!inv.ok) { std::fprintf(stderr, "ERROR: --kst %s: %s\n", kst.c_str(), inv.error.c_str()); return 1; }
            const auto it = inv.by_type.find("Sample");
            const ExtStats pcm = it == inv.by_type.end() ? ExtStats{} : it->second;
            kstText = "Undecoded KST " + kst + " (NOT part of the graph; its resources are unknown and not counted):\n  " +
                      std::to_string(pcm.files) + " .PCM files, " + std::to_string(pcm.bytes) + " disk bytes; PCM folder: " +
                      (inv.has_pcm_folder ? "YES (loading the whole KST deletes all User Samples, MAN p.736)" : "no") + "\n";
            kstJson = "  \"undecodedKst\": {\"inGraph\": false, \"pcmFiles\": " + std::to_string(pcm.files) + ", \"pcmDiskBytes\": " +
                      std::to_string(pcm.bytes) + ", \"pcmFolder\": " + (inv.has_pcm_folder ? "true" : "false") + "},\n";
        }
        if (json) std::fputs(graph::budgetJson(b, kstJson).c_str(), stdout);
        else { std::printf("Packages: %zu\n", pkgs.size()); std::fputs(graph::formatBudget(b).c_str(), stdout); std::fputs(kstText.c_str(), stdout); }
        return b.fit == graph::Fit::Fits ? 0 : 3;
    }
    if (cmd == "check") {
        const auto issues = g.issues();
        std::printf("Resources: %zu nodes, %zu references, %zu package(s)\nIssues: %zu\n", g.nodes().size(), g.edges().size(), pkgs.size(), issues.size());
        for (const auto& i : issues) std::printf("  %s\n", i.c_str());
        const auto unsafe = g.unsafeDeletes();
        std::printf("Not provably safe to delete: %zu\n", unsafe.size());
        for (const auto& [id, d] : unsafe)
            std::printf("  %s  %s: %s\n", id.str().c_str(), d.verdict == graph::DeleteVerdict::InUse ? "IN USE" : "UNPROVABLE", d.reasons.front().c_str());
        return issues.empty() ? 0 : 3;
    }
    if (cmd == "where-used") {
        const auto users = g.whereUsed(ids[0]);
        std::printf("%s%s\nDirect users: %zu\n", ids[0].str().c_str(), g.find(ids[0]) ? "" : "  (NOT in the graph)", users.size());
        for (const auto& e : users) std::printf("  %s\n", edgeText(e).c_str());
        const auto roots = g.usedByRoots(ids[0]), sure = g.usedByRoots(ids[0], true);
        std::printf("Reached from: %zu\n", roots.size());
        for (const auto& r : roots)
            std::printf("  %s (%s)%s\n", r.str().c_str(), g.find(r) ? g.find(r)->name.c_str() : "?", sure.count(r) ? "" : "  POSSIBLY: only through an inferred reference");
        std::printf(evidence.empty() ? "Scope: AIArranger packages only. Styles, Keyboard Sets, Pads, SongBook Entries and Program Change messages on a Pa5X are not visible here.\n"
                                     : "Scope: AIArranger packages and the evidence given. References outside it, and SongBook/Program Change unless genuinely scanned, are not visible.\n");
        return 0;
    }
    if (cmd == "delete-check") {
        const auto d = g.deleteCheck(ids[0]);
        static const char* v[] = {"IN USE", "UNPROVABLE", "UNREFERENCED"};
        std::printf("%s: %s\n", ids[0].str().c_str(), v[static_cast<int>(d.verdict)]);
        for (const auto& r : d.reasons) std::printf("  %s\n", r.c_str());
        for (const auto& e : d.users) std::printf("  user: %s\n", edgeText(e).c_str());
        std::printf("Advisory only: AIArranger deletes nothing.\n");
        return d.verdict == graph::DeleteVerdict::Unreferenced ? 0 : 3;
    }
    const auto m = g.remapImpact(ids[0], ids[1]);   // remap
    std::printf("Remap %s -> %s (advisory; nothing is changed)\nReferences rewritten: %zu\n", ids[0].str().c_str(), ids[1].str().c_str(), m.rewritten.size());
    for (const auto& e : m.rewritten) std::printf("  %s\n", edgeText(e).c_str());
    std::printf("Conflicts: %zu\n", m.conflicts.size());
    for (const auto& c : m.conflicts) std::printf("  %s\n", c.c_str());
    for (const auto& w : m.warnings) std::printf("  warning: %s\n", w.c_str());
    return m.conflicts.empty() ? 0 : 3;
}
