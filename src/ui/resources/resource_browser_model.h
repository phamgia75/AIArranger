#ifndef AI_ARRANGER_UI_RESOURCE_BROWSER_MODEL_H
#define AI_ARRANGER_UI_RESOURCE_BROWSER_MODEL_H

// UI-toolkit-agnostic state behind the macOS Resources window (Resource Graph, KST Library, Memory
// Check tabs). Read-only over everything it opens: AIArranger packages, owner-authorized evidence
// files, KST folders (file level, never decoded) and transcribed Memory Info readings. It never writes,
// sends MIDI or talks to an instrument.
//
// Identity display rule: a provisional (package-generated) identity is never shown as a Pa5X bank or
// slot. The Pa5X assigns slots on import, and that numbering is unknown.

#include "media/kst_diff.h"
#include "media/sound/evidence_import.h"
#include "media/sound/memory_check.h"
#include <set>

namespace ai_arranger::ui {

namespace rg = media::graph;

class ResourceBrowserModel {
public:
    struct Package { std::string id, dir; };

    bool addPackage(const std::filesystem::path& dir, std::string* err) {
        rg::Graph one;
        std::string pkg, e;
        if (!media::sound::loadPackage(dir, one, pkg, &e)) return fail(err, e);
        return adoptPackage(std::move(one), pkg, dir, err);
    }
    // Second half of addPackage, for a package loaded elsewhere (the UI loads off the message thread).
    bool adoptPackage(rg::Graph one, const std::string& pkg, const std::filesystem::path& dir, std::string* err) {
        std::string e;
        for (const auto& p : packages_)
            if (p.id == pkg) return fail(err, "package " + pkg + " is already open (" + p.dir + "): one import or two? Not counted twice.");
        if (!g_.merge(one, &e)) return fail(err, e);
        packages_.push_back({pkg, dir.string()});
        ++version_;
        return true;
    }
    bool addEvidence(const std::filesystem::path& file, std::string* err) {
        rg::Graph work = g_;
        std::string label;
        return prepareEvidence(file, work, label, err) && adoptEvidence(std::move(work), label, version_, err);
    }
    // Evidence in two halves so the UI can parse off the message thread: prepareEvidence imports into
    // `work` (a copy of graph() taken at version()); adoptEvidence installs it only if nothing changed since.
    static bool prepareEvidence(const std::filesystem::path& file, rg::Graph& work, std::string& label, std::string* err) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file, ec)) || std::filesystem::file_size(file, ec) > (16u << 20) || ec)
            return fail(err, file.string() + ": not a regular file of at most 16 MiB");
        std::ifstream f(file, std::ios::binary);
        const std::string t((std::istreambuf_iterator<char>(f)), {});
        std::string e;
        if (!media::sound::importEvidence(t, work, &label, &e)) return fail(err, file.string() + ": " + e);
        return true;
    }
    bool adoptEvidence(rg::Graph work, const std::string& label, uint64_t atVersion, std::string* err) {
        if (atVersion != version_) return fail(err, "the open resources changed while the evidence was read; open it again");
        for (const auto& x : evidence_) if (x == label) return fail(err, "evidence source '" + label + "' is already open");
        g_ = std::move(work);
        evidence_.push_back(label);
        ++version_;
        return true;
    }
    void clear() { g_ = {}; packages_.clear(); evidence_.clear(); ++version_; ++epoch_; }
    uint64_t version() const { return version_; }   // changes on every change of the open resources
    uint64_t epoch() const { return epoch_; }       // changes on Close All only

    const rg::Graph& graph() const { return g_; }
    const std::vector<Package>& packages() const { return packages_; }
    const std::vector<std::string>& evidence() const { return evidence_; }

    // Top-level resources: nothing in the graph refers to them (Sounds, Drum Kits, and evidence roots).
    std::vector<rg::ResourceId> roots() const {
        std::vector<rg::ResourceId> r;
        for (const auto& [id, n] : g_.nodes()) if (g_.whereUsed(id).empty() && isTop(id.kind)) r.push_back(id);
        return r;
    }
    std::vector<rg::Edge> children(const rg::ResourceId& id) const { return g_.uses(id); }

    std::string displayId(const rg::ResourceId& id) const {
        if (!id.provisional) return "Pa5X " + id.scope + " " + rg::kindName(id.kind) + " " + std::to_string(id.number);
        return std::string("package-local ") + rg::kindName(id.kind) + " #" + std::to_string(id.number) +
               " (provisional: not a Pa5X slot; the Pa5X assigns one on import)";
    }

    // One tree row. `via` is the reference that leads here (nullptr for a root).
    std::string rowLabel(const rg::ResourceId& id, const rg::Edge* via = nullptr) const {
        std::string s = via ? via->detail + ": " : "";
        const rg::Node* n = g_.find(id);
        s += std::string(rg::kindName(id.kind)) + " ";
        s += n ? "'" + n->name + "'" : "MISSING (" + id.str() + " not in the graph)";
        if (n && n->sample) s += "  " + bytes(n->sample->bytes());
        if (const size_t u = g_.whereUsed(id).size(); u > 1) s += "  [shared by " + std::to_string(u) + "]";
        if (via && via->confidence == rg::Confidence::Inferred) s += "  [inferred]";
        if (!id.provisional) s += "  [Pa5X " + id.scope + "]";
        return s;
    }

    std::string details(const rg::ResourceId& id) const {
        std::string o;
        const rg::Node* n = g_.find(id);
        o += "Identity: " + displayId(id) + "\n  id " + id.str() + "\n";
        if (id.provisional) o += "  package: " + packageDir(id.scope) + "\n";
        o += std::string("Kind: ") + rg::kindName(id.kind) + "\nName: " + (n ? n->name : "(not in the graph: MISSING)") + "\n";
        if (n && n->sample) {
            const auto& x = *n->sample;
            o += "Sample: " + std::string(x.channels == 2 ? "stereo" : "mono") + ", " + std::to_string(x.rate) + " Hz, " + std::to_string(x.bits) +
                 "-bit, " + std::to_string(x.frames) + " frames" + (x.loopFrames ? ", loop " + std::to_string(x.loopFrames) + " frames" : "") +
                 "\n  uncompressed PCM: " + bytes(x.bytes()) + " (exact)\n  file: " + x.file + "\n";
        }
        if ((id.kind == rg::Kind::Sound || id.kind == rg::Kind::DrumKit) && !g_.structureKnown(id)) {
            o += "Samples needed: UNKNOWN (the evidence does not describe this resource's oscillators or samples)\n";
        } else if (id.kind == rg::Kind::Sound || id.kind == rg::Kind::DrumKit) {
            uint64_t sum = 0, count = 0, missing = 0;
            for (const auto& c : g_.closure({id}))
                if (c.kind == rg::Kind::Sample) {
                    if (const rg::Node* s = g_.find(c)) { sum += s->sample->bytes(); ++count; } else ++missing;
                }
            o += "Samples needed: " + std::to_string(count) + " unique, " + bytes(sum) + " uncompressed PCM (exact; overhead and compression not modelled)" +
                 (missing ? ", plus " + std::to_string(missing) + " MISSING of unknown size" : "") + "\n";
        }
        const auto users = g_.whereUsed(id);
        o += "Where used: " + std::to_string(users.size()) + " direct reference(s)" + (users.size() > 1 ? " (shared resource)" : "") + "\n";
        for (const auto& e : users) o += "  " + edgeText(e) + "\n";
        const auto roots = g_.usedByRoots(id), sure = g_.usedByRoots(id, true);
        if (!roots.empty()) {
            o += "Reached from:\n";
            for (const auto& r : roots)
                o += "  " + rowLabel(r) + "  (" + r.str() + ")" + (sure.count(r) ? "" : "  POSSIBLY: only through an inferred reference") + "\n";
        }
        const auto d = g_.deleteCheck(id);
        static const char* verdict[] = {"IN USE: do not delete", "CANNOT BE PROVEN SAFE to delete", "No known references"};
        o += std::string("Delete safety: ") + verdict[static_cast<int>(d.verdict)] + "\n";
        for (const auto& r : d.reasons) o += "  " + r + "\n";
        std::vector<std::string> unresolved;
        for (const auto& e : g_.uses(id)) if (!g_.find(e.to)) unresolved.push_back("references " + e.to.str() + " (" + e.detail + "), which is not in the graph");
        for (const auto& i : g_.issues())
            if (i.rfind("missing: " + id.str() + " ", 0) == 0) unresolved.push_back(i);
        o += "Unresolved: " + (unresolved.empty() ? std::string("none") : std::to_string(unresolved.size())) + "\n";
        for (const auto& u : unresolved) o += "  " + u + "\n";
        o += scopeNote();
        return o;
    }

    std::string issuesText() const {
        const auto issues = g_.issues();
        std::string o = "Unresolved dependencies: " + std::to_string(issues.size()) + "\n";
        for (const auto& i : issues) o += "  " + i + "\n";
        return o;
    }

    std::string budgetText() const {
        std::string o = "Open: " + std::to_string(packages_.size()) + " package(s), " + std::to_string(evidence_.size()) + " evidence source(s)\n";
        for (const auto& p : packages_) o += "  " + p.dir + "  (" + p.id + ")\n";
        for (const auto& e : evidence_) o += "  evidence: " + e + "\n";
        return o + rg::formatBudget(rg::budget(g_));
    }

    std::string memoryCheck(const std::string& beforeText, const std::string& afterText) const {
        media::sound::MemoryInfoReading b, a;
        std::string e;
        if (!media::sound::parseMemoryInfo(beforeText, b, &e)) return "Before: " + e + "\n";
        if (!media::sound::parseMemoryInfo(afterText, a, &e)) return "After: " + e + "\n";
        if (packages_.empty()) return "Open the packages that were imported first: the prediction comes from them.\n";
        return media::sound::compareMemoryInfo(b, a, media::sound::predict(g_));
    }

    // KST Library tab: the existing read-only file-level tools. Semantic (dependency-aware) merge is
    // deliberately unavailable: no validated dependency data exists for KST content (Q17, Q18).
    enum class KstAction { Inventory, Diff, MergePlan };
    static std::string kst(KstAction a, const std::string& first, const std::string& second = "") {
        std::string o;
        switch (a) {
            case KstAction::Inventory: o = media::format(media::scan(first), first); break;
            case KstAction::Diff:      o = media::format(media::diff(first, second), first, second); break;
            case KstAction::MergePlan: o = media::format(media::planMerge(first, second), first, second); break;
        }
        return o + "\n" + kSemanticMergeNote;
    }
    static constexpr const char* kSemanticMergeNote =
        "Semantic merge (by Sound/sample dependency): UNAVAILABLE until validated dependency data exists (Q17, Q18).\n"
        "These results are file-level only and read-only. AIArranger never merges, copies or deletes KST data.\n";

private:
    static bool fail(std::string* err, const std::string& m) { if (err) *err = m; return false; }
    static bool isTop(rg::Kind k) {
        return k == rg::Kind::Sound || k == rg::Kind::DrumKit || k == rg::Kind::Style || k == rg::Kind::KeyboardSet ||
               k == rg::Kind::Pad || k == rg::Kind::SongBookEntry || k == rg::Kind::MidiSong;
    }
    static std::string bytes(uint64_t b) {
        std::string s = std::to_string(b);
        for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), " ");
        return s + " bytes";
    }
    std::string packageDir(const std::string& scope) const {
        for (const auto& p : packages_) if (p.id == scope) return p.dir;
        return scope;
    }
    std::string edgeText(const rg::Edge& e) const {
        const rg::Node* f = g_.find(e.from);
        return std::string(rg::viaName(e.via)) + " from " + rg::kindName(e.from.kind) + " '" + (f ? f->name : "?") + "' (" + e.detail + ")" +
               (e.confidence == rg::Confidence::Inferred ? "  INFERRED, not proven" : "") +
               (e.source == rg::Source::PaManagerReport ? "  [evidence: " + e.provenance + "]" : "");
    }
    std::string scopeNote() const {
        std::string o = "Scope: open packages";
        o += evidence_.empty() ? " only. Styles, Keyboard Sets, Pads, SongBook Entries and Program Change messages on a Pa5X are not visible.\n"
                               : " and imported evidence. SongBook and Program Change references stay unresolved unless genuinely scanned.\n";
        return o;
    }

    rg::Graph g_;
    std::vector<Package> packages_;
    std::vector<std::string> evidence_;
    uint64_t version_ = 0, epoch_ = 0;
};

} // namespace ai_arranger::ui

#endif // AI_ARRANGER_UI_RESOURCE_BROWSER_MODEL_H
