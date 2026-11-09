// aiarranger-resources: resource graph and memory budget over AIArranger packages
// (docs/research/PA5X_SOUND_RESOURCE_GRAPH.md; media/sound/resource_graph.h). Read-only.
//   budget [--json] [--kst KSTDIR] PKG...   unique-sample memory vs the documented Pa5X limits
//   check PKG...                            dangling/missing references, resources not provably safe to delete
//   where-used ID PKG...                    direct users and the top-level Sounds/Drum Kits that reach ID
//   delete-check ID PKG...                  InUse / Unprovable / Unreferenced, with reasons
//   remap FROM TO PKG...                    references a remap would rewrite, conflicts, warnings
//   memory-compare BEFORE AFTER PKG...      transcribed Pa5X Memory Info readings vs the raw PCM prediction
// Every command also takes --evidence FILE (repeatable, before the other arguments): an owner-authorized
// aiarranger-evidence file (media/sound/evidence_import.h) added as a separate evidence source.
// PKG is a package folder written by aiarranger-sound (it holds RESOURCES.tsv); every sample is checked
// against its WAV. --kst only summarises an undecoded KST folder (kst-inventory): its resources are
// unknown and never enter the graph or the sample count.
// Exit 0 = ok (budget fits / delete provable / remap conflict-free), 3 = not, 1 = usage or input error.
#include "media/kst_inventory.h"
#include "media/sound/evidence_import.h"
#include "media/sound/memory_check.h"
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
                         "  any command: --evidence FILE (owner-authorized evidence, repeatable)\n");
    return 1;
}
static std::string edgeText(const graph::Edge& e) { return e.from.str() + " -" + graph::viaName(e.via) + "-> " + e.to.str() + "  (" + e.detail + ")"; }

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
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
        const auto roots = g.usedByRoots(ids[0]);
        std::printf("Reached from: %zu\n", roots.size());
        for (const auto& r : roots) std::printf("  %s (%s)\n", r.str().c_str(), g.find(r) ? g.find(r)->name.c_str() : "?");
        std::printf("Scope: AIArranger packages only. Styles, Keyboard Sets, Pads, SongBook Entries and Program Change messages on a Pa5X are not visible here.\n");
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
