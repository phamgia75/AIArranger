// W5: assemble a native Pa5X KST folder from Root-tree objects. EXPERIMENTAL, NOT HARDWARE-VERIFIED.
//
// Layout as OBSERVED in Korg's own Pa5X output (KST_BINARY_STRUCTURE_FINDINGS.md §4, KST_DEPENDENCY_EVIDENCE.md):
// <Name>.KST/ holding only the type folders that have data, one bank sub-folder per Style/KEYBOARDSET/PAD bank, one
// object per file named after it with a lower-case .stg/.ksc/.pdc extension, no top-level files. Optional KORF files
// (.PCG .PCM .KMP ...) are carried byte-for-byte after the existing reader accepted them, never re-encoded.
//
// Writing: only into a NEW folder (output_guard.h: no existing path, no mounted volume), built under NAME.INCOMPLETE and
// published by one non-replacing rename. Then, read back: every object re-parsed and byte-compared, the structural
// validator (0 errors), the dependency closure (0 dangling, 0 problems), and a SHA-256 manifest beside the folder
// (NAME.KST.manifest.sha256, sha256sum format, re-verified). Any failure leaves the folder as NAME.INCOMPLETE or
// reports it; nothing is deleted.
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

inline bool writeNew(const std::filesystem::path& p, const std::string& bytes, std::string& err) {
    std::FILE* f = std::fopen(p.c_str(), "wbx");   // exclusive: never replaces
    if (!f) { err = "cannot create " + p.string(); return false; }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0 || !ok) { err = "cannot write " + p.string(); return false; }
    return true;
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

    const fs::path work = incompletePath(out);
    if (!fs::create_directory(work, ec) || ec) return fail("cannot create " + work.string());
    std::string err;
    for (const auto& [rel, bytes] : files) {
        fs::create_directories(work / fs::path(rel).parent_path(), ec);
        if (ec) return fail("cannot create a folder for " + rel + " (left in " + work.string() + ")");
        if (!writeNew(work / rel, bytes, err)) return fail(err + " (left in " + work.string() + ")");
    }
    // Read back before publishing.
    for (const auto& [rel, bytes] : files) {
        std::string back;
        if (!roottree::readBytes(work / rel, back, err) || back != bytes) return fail("read-back mismatch: " + rel + " (left in " + work.string() + ")");
    }
    if (!publishNewFolder(work, out)) return fail("refusing: " + out.string() + " appeared during the write (work left in " + work.string() + ")");
    r.folder = out;
    r.files = files.size();

    // Manifest beside the folder (never inside: a saved KST has no top-level files).
    std::string man;
    bool cancelled = false;
    for (const auto& [rel, bytes] : files) {
        const auto h = media::detail::sha256File(out / rel, {}, &cancelled, &err);
        if (h.empty()) return fail("cannot hash " + rel);
        man += h + "  " + rel + "\n";
    }
    r.manifest = backupManifestPath(out);
    if (!writeNew(r.manifest, man, err)) return fail(err);
    if (const auto bad = media::verifyBackup(out); !bad.empty()) return fail("manifest check failed:\n" + bad);

    const auto v = library::validate(out);
    r.validatorErrors = v.count(library::Severity::Error);
    r.validatorWarnings = v.count(library::Severity::Warn);
    if (!v.ok) return fail("validator could not finish: " + v.error);
    if (r.validatorErrors) return fail("validator: " + std::to_string(r.validatorErrors) + " error(s)\n" + library::validationText(v));
    const auto c = closure::check(out);
    r.dangling = c.danglingCount();
    r.factoryRefs = c.factory;
    r.userRefs = c.resolved + c.danglingCount();
    if (!c.ok) return fail("closure could not read every object: " + (c.problems.empty() ? c.error : c.problems.front()));
    if (r.dangling) return fail("closure: " + std::to_string(r.dangling) + " dangling user reference(s)");
    r.ok = true;
    r.message = "wrote " + out.string() + " (" + std::to_string(r.files) + " files), NOT HARDWARE-VERIFIED";
    return r;
}

}  // namespace ai_arranger::media::panative::assemble

#endif
