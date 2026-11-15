// W5: assemble a native Pa5X KST folder from Root-tree objects. EXPERIMENTAL, NOT HARDWARE-VERIFIED.
//
// Layout as OBSERVED in Korg's own Pa5X output (KST_BINARY_STRUCTURE_FINDINGS.md §4, KST_DEPENDENCY_EVIDENCE.md):
// <Name>.KST/ holding only the type folders that have data, one bank sub-folder per Style/KEYBOARDSET/PAD bank, one
// object per file named after it with a lower-case .stg/.ksc/.pdc extension, no top-level files. Optional KORF files
// (.PCG .PCM .KMP ...) are carried byte-for-byte after the existing reader accepted them, never re-encoded.
//
// Writing: only into a NEW folder (output_guard.h: no existing path, no mounted volume; an existing manifest is refused
// too). Everything is staged in NAME.KST.INCOMPLETE/: the files are read back (byte-compared and re-parsed), then the
// dependency closure (0 dangling, 0 unresolved, 0 problems), the structural validator (0 errors) and a SHA-256 manifest
// (NAME.KST.manifest.sha256, sha256sum format, verified) all run on the staged folder. Only when every check passed are
// the manifest and then the folder published by non-replacing renames. Any failure leaves only NAME.KST.INCOMPLETE,
// never a final-named KST; nothing is deleted except the emptied staging folder.
#ifndef AI_ARRANGER_MEDIA_PANATIVE_KST_ASSEMBLE_H
#define AI_ARRANGER_MEDIA_PANATIVE_KST_ASSEMBLE_H

#include "library/library_validate.h"
#include "media/kst_backup.h"
#include "media/output_guard.h"
#include "media/panative/kst_closure.h"
#include "media/panative/root_tree.h"
#include <cstdio>
#include <set>

namespace ai_arranger::media::panative::assemble {

struct Object {
    std::string type;   // "STYLE" | "KEYBOARDSET" | "PAD"
    std::string bank;   // bank folder name (free text as observed)
    std::string name;   // object name; the file is <name>.stg/.ksc/.pdc
    roottree::Node tree;
};
struct Carried {
    std::filesystem::path source;   // a KORF file, read only
    std::string rel;                // destination inside the KST, e.g. "SOUND/USER01.PCG"
};
struct Result {
    bool ok = false;
    std::string message;
    std::filesystem::path folder, manifest;
    size_t files = 0, validatorErrors = 0, validatorWarnings = 0, dangling = 0, factoryRefs = 0, userRefs = 0;
};

inline const char* extensionFor(const std::string& type) {
    return type == "STYLE" ? ".stg" : type == "KEYBOARDSET" ? ".ksc" : type == "PAD" ? ".pdc" : nullptr;
}
// A path component we write: non-empty, not "." / "..", no separators, no characters the validator flags (R-09).
inline bool safeName(const std::string& s) {
    if (s.empty() || s == "." || s == ".." || s.size() > 200) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == 0x7F) return false;
    return !library::detail::fatBad(s);
}

// The tree must be the object its folder says (top-level marker as observed) and every MIDI leaf must be an SMF the
// closure reader accepts, so nothing is published that our own readers would reject.
inline std::string contentRefusal(const std::string& type, const roottree::Node& t) {
    const roottree::Node* clip = nullptr;
    bool stage = false;
    for (const auto& c : t.children) {
        if (c.kind == roottree::Node::Leaf && c.name == "ClipDescription.xml") clip = &c;
        if (c.kind == roottree::Node::Leaf && c.name == "StageDescription.xml") stage = true;
    }
    const auto clipType = [&](const char* ty) { return clip && clip->payload.find(std::string("<CLIP type=\"") + ty + "\"") != std::string::npos; };
    if (type == "STYLE" ? !(stage && !clip) : type == "PAD" ? !clipType("PadClip") || stage : !clipType("KeyboardSetClip") || stage)
        return "the tree is not a " + type + " object (top-level ClipDescription/StageDescription does not match)";
    std::vector<const roottree::Node*> mids;
    closure::collect(t, mids);
    for (const auto* m : mids) {
        std::vector<closure::Select> s;
        std::string e;
        if (!closure::leafSelects(*m, s, e)) return "MIDI leaf " + m->name + ": " + e;
    }
    return "";
}

// shortcut: path-based writes inside the fresh NAME.INCOMPLETE (not openat-pinned like kst_backup.h); a concurrent local
// process swapping that folder could redirect them. Upgrade to the kst_backup.h *at() pattern before an app/job path.
inline bool writeNew(const std::filesystem::path& p, const std::string& bytes, std::string& err) {
    std::FILE* f = std::fopen(p.c_str(), "wbx");   // exclusive: never replaces
    if (!f) { err = "cannot create " + p.string(); return false; }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0 || !ok) { err = "cannot write " + p.string(); return false; }
    return true;
}

// The checks a staged KST must pass before it is published: dependency closure (0 dangling, 0 unresolved, 0 problems)
// and the structural validator (0 errors). Fills r's counts; "" = passed, else why not (`left` appended as by build).
// Shared with the SET -> KST export job (jobs::setToKst), which stages through the E9 job runner instead.
inline std::string stagedRefusal(const std::filesystem::path& stage, Result& r, const std::string& left = "") {
    const auto c = closure::check(stage);
    r.dangling = c.danglingCount();
    r.factoryRefs = c.factory;
    r.userRefs = c.resolved + c.danglingCount();
    if (!c.ok) return "closure could not read every object: " + (c.problems.empty() ? c.error : c.problems.front()) + left;
    if (r.dangling) return "closure: " + std::to_string(r.dangling) + " dangling user reference(s)" + left;
    if (c.unresolved) return "closure: " + std::to_string(c.unresolved) + " unresolved sound selection(s) (Program Change without CC0/CC32)" + left;
    const auto v = library::validate(stage);
    r.validatorErrors = v.count(library::Severity::Error);
    r.validatorWarnings = v.count(library::Severity::Warn);
    if (!v.ok) return "validator could not finish: " + v.error + left;
    if (r.validatorErrors) return "validator: " + std::to_string(r.validatorErrors) + " error(s)" + left + "\n" + library::validationText(v);
    return "";
}

// Writes parent/<kstName>.KST. Read-only on every source.
inline Result build(const std::filesystem::path& parent, const std::string& kstName, const std::vector<Object>& objects,
                    const std::vector<Carried>& carried = {}) {
    namespace fs = std::filesystem;
    Result r;
    auto fail = [&](const std::string& m) { r.message = m; return r; };
    if (!safeName(kstName)) return fail("refusing: unsafe KST name");
    if (objects.empty() && carried.empty()) return fail("refusing: nothing to write");
    const fs::path out = parent / (kstName + ".KST");
    if (const auto why = outputFolderRefusal(out); !why.empty()) return fail(why);
    if (std::error_code mec; fs::symlink_status(backupManifestPath(out), mec).type() != fs::file_type::not_found) return fail("refusing: " + backupManifestPath(out).string() + " already exists");
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) return fail("refusing: parent folder does not exist: " + parent.string());

    // Everything is serialized and checked before the first byte is written.
    std::vector<std::pair<std::string, std::string>> files;   // rel -> bytes
    std::set<std::string> seen;
    for (const auto& o : objects) {
        const char* ext = extensionFor(o.type);
        if (!ext) return fail("refusing: unknown object type " + o.type);
        if (!safeName(o.bank) || !safeName(o.name + ext)) return fail("refusing: unsafe bank or object name in " + o.type);
        std::string bytes, err;
        if (!roottree::write(o.tree, bytes, err)) return fail("refusing: " + o.type + "/" + o.name + ": " + err);
        if (const auto why = contentRefusal(o.type, o.tree); !why.empty()) return fail("refusing: " + o.type + "/" + o.name + ": " + why);
        const std::string rel = o.type + "/" + o.bank + "/" + o.name + ext;
        if (!seen.insert(closure::upper(rel)).second) return fail("refusing: two objects map to " + rel + " (names differ only in case?)");
        files.emplace_back(rel, std::move(bytes));
    }
    for (const auto& c : carried) {
        std::string bytes, err;
        if (!roottree::readBytes(c.source, bytes, err)) return fail("refusing: cannot read " + c.source.string() + ": " + err);
        const auto k = panative::parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        if (!k.ok) return fail("refusing: " + c.source.string() + " is not a valid KORF container: " + k.error);
        const auto slash = c.rel.find('/');
        if (slash == std::string::npos || !safeName(c.rel.substr(0, slash)) || !safeName(c.rel.substr(slash + 1)))
            return fail("refusing: carried destination must be TYPE/FILE: " + c.rel);
        if (!seen.insert(closure::upper(c.rel)).second) return fail("refusing: duplicate destination " + c.rel);
        files.emplace_back(c.rel, std::move(bytes));
    }

    // Staged layout: NAME.KST.INCOMPLETE/NAME.KST/... and NAME.KST.INCOMPLETE/NAME.KST.manifest.sha256. Every check runs
    // on the staged folder; only then the manifest and then the folder are renamed beside each other (non-replacing).
    const fs::path work = incompletePath(out), stage = work / out.filename(), stagedMan = work / backupManifestPath(out).filename();
    if (!fs::create_directory(work, ec) || ec) return fail("cannot create " + work.string());
    const std::string left = " (staged files left in " + work.string() + ")";
    std::string err;
    for (const auto& [rel, bytes] : files) {
        fs::create_directories(stage / fs::path(rel).parent_path(), ec);
        if (ec) return fail("cannot create a folder for " + rel + left);
        if (!writeNew(stage / rel, bytes, err)) return fail(err + left);
    }
    // Read back: identical bytes, and every file re-parsed by its own reader.
    for (const auto& [rel, bytes] : files) {
        std::string back;
        if (!roottree::readBytes(stage / rel, back, err) || back != bytes) return fail("read-back mismatch: " + rel + left);
        const auto* q = reinterpret_cast<const uint8_t*>(back.data());
        if (roottree::looksLikeRootTree(q, back.size())) {
            const auto t = roottree::parse(q, back.size());
            std::string again;
            if (!t.ok || !roottree::write(t.root, again, err) || again != back) return fail("read-back does not re-parse: " + rel + left);
        } else if (!panative::parse(q, back.size()).ok) return fail("read-back does not re-parse: " + rel + left);
    }
    if (const auto why = stagedRefusal(stage, r, left); !why.empty()) return fail(why);
    // Manifest (beside the folder: a saved KST has no top-level files), written and verified while staged.
    std::string man;
    bool cancelled = false;
    for (const auto& [rel, bytes] : files) {
        const auto h = media::detail::sha256File(stage / rel, {}, &cancelled, &err);
        if (h.empty()) return fail("cannot hash " + rel + left);
        man += h + "  " + rel + "\n";
    }
    if (!writeNew(stagedMan, man, err)) return fail(err + left);
    if (const auto bad = media::verifyManifest(stage, stagedMan, {}, nullptr); !bad.empty()) return fail("manifest check failed" + left + ":\n" + bad);
    // Publish: manifest first, then the folder. If the folder cannot be published, the manifest goes back to the
    // staging folder, so a failure never leaves a final-named KST (nor a final-named manifest).
    const fs::path finalMan = backupManifestPath(out);
    if (!publishNewFolder(stagedMan, finalMan)) return fail("refusing: " + finalMan.string() + " appeared during the build" + left);
    if (!publishNewFolder(stage, out)) {
        const bool back = publishNewFolder(finalMan, stagedMan);
        return fail("refusing: " + out.string() + " appeared during the build" + left + (back ? "" : "; could not move " + finalMan.string() + " back"));
    }
    fs::remove(work, ec);   // the staging folder is empty now; remove() never deletes a non-empty folder
    r.folder = out;
    r.manifest = finalMan;
    r.files = files.size();
    r.ok = true;
    r.message = "wrote " + out.string() + " (" + std::to_string(r.files) + " files), NOT HARDWARE-VERIFIED";
    return r;
}

}  // namespace ai_arranger::media::panative::assemble

#endif
