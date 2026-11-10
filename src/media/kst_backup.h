// Verified KST backup (K1 in the app; same guarantees as tools/kst_copy.py, whose manifest format it shares).
//
// Folder level only: files are copied as opaque bytes, never decoded or rewritten. The source is only read.
//   1. Refuse: a source that is not a folder or holds symlinks/special files, a destination that exists, sits
//      on a mounted volume or inside the source, its manifest or work folder existing, or too little free space.
//   2. Copy into DST.INCOMPLETE (the name marks an interrupted backup, e.g. after a crash), hashing each file
//      with SHA-256 as it is read, then re-read the copy and compare hashes.
//   3. Re-hash the whole source: if it changed during the copy, the backup is discarded.
//   4. Only then rename DST.INCOMPLETE -> DST (never over an existing folder) and publish DST.manifest.sha256
//      next to it, so the backup folder stays byte-identical to the source.
// Cancel or any failure removes DST.INCOMPLETE (only this run's own folder). Success is reported only after
// every step passed.
#ifndef AI_ARRANGER_MEDIA_KST_BACKUP_H
#define AI_ARRANGER_MEDIA_KST_BACKUP_H

#include "media/kst_inventory.h"
#include "media/output_guard.h"
#include "media/sha256.h"
#include <cstdio>
#include <functional>
#ifdef __APPLE__
#include <stdio.h>   // renamex_np, RENAME_EXCL
#endif

namespace ai_arranger::media {

struct BackupProgress { uintmax_t bytesDone = 0, bytesTotal = 0; size_t filesDone = 0, filesTotal = 0; };
// Called often from the copying thread; return false to cancel.
using BackupCallback = std::function<bool(const BackupProgress&)>;

struct BackupResult {
    bool ok = false, cancelled = false;
    std::string message;
    std::filesystem::path folder, manifest;
    size_t files = 0;
    uintmax_t bytes = 0;
};

inline std::filesystem::path backupManifestPath(const std::filesystem::path& dst) {
    return std::filesystem::path(std::filesystem::absolute(dst).string() + ".manifest.sha256");
}

namespace detail {
// SHA-256 of a file; when `to` is set, also writes the bytes there (created exclusively: never overwrites).
// "" on any error or when `more` returns false (cancelled is then set).
inline std::string sha256File(const std::filesystem::path& from, const std::filesystem::path* to, const std::function<bool(size_t)>& more,
                              bool* cancelled, std::string* error) {
    std::FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) { *error = "cannot read " + from.string(); return ""; }
    std::FILE* out = to ? std::fopen(to->c_str(), "wbx") : nullptr;
    if (to && !out) { std::fclose(in); *error = "cannot create " + to->string(); return ""; }
    Sha256 h;
    static thread_local char buf[1 << 20];
    bool ok = true;
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, in)) > 0;) {
        h.update(buf, n);
        if (out && std::fwrite(buf, 1, n, out) != n) { *error = "cannot write " + to->string() + " (disk full?)"; ok = false; break; }
        if (more && !more(n)) { *cancelled = true; ok = false; break; }
    }
    if (ok && std::ferror(in)) { *error = "read error in " + from.string(); ok = false; }
    std::fclose(in);
    if (out && std::fclose(out) != 0 && ok) { *error = "cannot write " + to->string(); ok = false; }
    return ok ? h.hex() : "";
}
// Rename that never replaces an existing destination.
inline bool renameNew(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef __APPLE__
    return renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0;
#else
    // shortcut: check-then-rename; POSIX rename() can replace an EMPTY folder created in between. Use renameat2(RENAME_NOREPLACE) on Linux if needed.
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::symlink_status(to, ec))) return false;
    std::filesystem::rename(from, to, ec);
    return !ec;
#endif
}
}  // namespace detail

inline BackupResult backupKst(const std::filesystem::path& source, const std::filesystem::path& destination, const BackupCallback& progress = {},
                              const std::function<void()>& afterCopy = {}) {
    namespace fs = std::filesystem;
    BackupResult r;
    std::error_code ec;
    const fs::path src = fs::canonical(source, ec);
    if (ec || !fs::is_directory(fs::symlink_status(source, ec))) { r.message = "refusing: " + source.string() + " is not a folder"; return r; }
    const fs::path dst = fs::absolute(destination);
    if (const auto why = outputFolderRefusal(dst); !why.empty()) { r.message = why; return r; }
    const fs::path parent = dst.parent_path();
    if (!fs::is_directory(parent, ec)) { r.message = "refusing: the folder " + parent.string() + " does not exist"; return r; }
    const fs::path realParent = fs::canonical(parent, ec);
    if (ec) { r.message = "cannot resolve " + parent.string(); return r; }
    if (const auto rel = realParent.lexically_relative(src); !rel.empty() && *rel.begin() != "..") {
        r.message = "refusing: the backup would be inside the folder being backed up";
        return r;
    }
    const fs::path man = backupManifestPath(dst), work = dst.string() + ".INCOMPLETE";
    for (const auto& p : {man, work})
        if (fs::exists(fs::symlink_status(p, ec))) { r.message = "refusing: " + p.string() + " already exists"; return r; }

    detail::Listing L;
    std::string err;
    if (!detail::listFiles(src, L, err)) { r.message = "cannot list the source: " + err; return r; }
    if (!L.skipped.empty()) { r.message = "refusing: the source holds a symbolic link or special file, not copied: " + L.skipped.front(); return r; }
    BackupProgress pr;
    pr.filesTotal = L.files.size();
    for (const auto& [rel, e] : L.files) pr.bytesTotal += e.size;
    const auto space = fs::space(realParent, ec);
    const uintmax_t needed = pr.bytesTotal + pr.bytesTotal / 100 + (16u << 20);   // copy + 1% + 16 MB headroom for the manifest and the file system
    if (ec || space.available < needed) {
        r.message = ec ? "cannot read the free space of " + parent.string()
                       : "not enough free space: the backup needs " + std::to_string(needed / 1000000) + " MB, " +
                             std::to_string(space.available / 1000000) + " MB available at " + parent.string();
        return r;
    }

    if (!fs::create_directory(work, ec) || ec) { r.message = "cannot create " + work.string(); return r; }
    auto fail = [&](const std::string& why) {
        fs::remove_all(work, ec);   // only this run's own folder: it did not exist before
        r.message = why + (fs::exists(work) ? "; the incomplete copy is left at " + work.string() : "; nothing was kept");
        return r;
    };
    for (const auto& d : L.dirs)
        if (fs::create_directories(work / d, ec); ec) return fail("cannot create " + (work / d).string());
    const auto tick = [&](size_t n) { pr.bytesDone += n; return !progress || progress(pr); };
    const std::function<bool(size_t)> stillWanted = [&](size_t) { return !progress || progress(pr); };   // read-back and re-hash: cancel works there too
    std::map<std::string, std::string> hashes;
    bool cancelled = false;
    for (const auto& [rel, e] : L.files) {
        const fs::path to = work / rel;
        const std::string hs = detail::sha256File(e.abs, &to, tick, &cancelled, &err);
        if (cancelled) { r.cancelled = true; return fail("Backup cancelled"); }
        if (hs.empty()) return fail("Backup failed: " + err);
        const std::string hd = detail::sha256File(to, nullptr, stillWanted, &cancelled, &err);   // read back what was written
        if (cancelled) { r.cancelled = true; return fail("Backup cancelled"); }
        if (hd != hs) return fail("Backup failed: the copy of " + rel + " does not match its source");
        hashes[rel] = hs;
        ++pr.filesDone;
        if (progress && !progress(pr)) { r.cancelled = true; return fail("Backup cancelled"); }
    }
    if (afterCopy) afterCopy();
    detail::Listing again;   // the source must be exactly as it was: same files, same bytes
    if (!detail::listFiles(src, again, err)) return fail("Backup failed: cannot re-read the source: " + err);
    bool same = again.files.size() == L.files.size() && again.skipped.empty();
    for (const auto& [rel, e] : again.files) {
        if (!same) break;
        const auto it = hashes.find(rel);
        same = it != hashes.end() && detail::sha256File(e.abs, nullptr, stillWanted, &cancelled, &err) == it->second;
    }
    if (cancelled) { r.cancelled = true; return fail("Backup cancelled"); }
    if (!same) return fail("Backup failed: the source changed during the backup");

    const fs::path manTmp = man.string() + ".INCOMPLETE";
    std::FILE* m = std::fopen(manTmp.c_str(), "wx");
    if (!m) return fail("cannot create " + manTmp.string());
    bool wrote = true;
    for (const auto& [rel, h] : hashes) wrote &= std::fprintf(m, "%s  %s\n", h.c_str(), rel.c_str()) > 0;
    wrote &= std::fclose(m) == 0;
    if (!wrote) { fs::remove(manTmp, ec); return fail("cannot write " + manTmp.string()); }
    if (!detail::renameNew(work, dst)) { fs::remove(manTmp, ec); return fail("refusing: " + dst.string() + " appeared during the backup"); }
    if (!detail::renameNew(manTmp, man)) {
        r.message = "Backup copied and verified at " + dst.string() + ", but its manifest could not be published (" + manTmp.string() +
                    " is left): treat it as INCOMPLETE until Compare Folders shows it equals the source";
        return r;
    }
    r.ok = true;
    r.folder = dst; r.manifest = man; r.files = hashes.size(); r.bytes = pr.bytesTotal;
    r.message = "Backup complete and verified: " + std::to_string(r.files) + " files, " + std::to_string(r.bytes) + " bytes, " + src.string() +
                " -> " + dst.string() + ". Every file was read back and matched its source by SHA-256, and the source was re-hashed "
                "after the copy (unchanged). Manifest: " + man.string() + "\n";
    return r;
}

// Re-check a backup against its manifest (ours or tools/kst_copy.py's). "" = every file matches; otherwise
// the problems (changed / missing / extra / unreadable), one per line.
inline std::string verifyBackup(const std::filesystem::path& dst) {
    const auto man = backupManifestPath(dst);
    std::FILE* f = std::fopen(man.c_str(), "r");
    if (!f) return "no manifest at " + man.string();
    std::map<std::string, std::string> want;
    char line[8192];
    std::string bad;
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        if (!s.empty() && s.back() == '\n') s.pop_back();
        const auto sep = s.find("  ");
        if (sep != 64) { bad += "malformed manifest line: " + s + "\n"; continue; }
        want[s.substr(66)] = s.substr(0, 64);
    }
    std::fclose(f);
    detail::Listing L;
    std::string err;
    if (!detail::listFiles(dst, L, err)) return "cannot list " + dst.string() + ": " + err;
    for (const auto& s : L.skipped) bad += "unexpected link or special file: " + s + "\n";
    bool cancelled = false;
    for (const auto& [rel, h] : want) {
        const auto it = L.files.find(rel);
        if (it == L.files.end()) { bad += "missing: " + rel + "\n"; continue; }
        const auto have = detail::sha256File(it->second.abs, nullptr, {}, &cancelled, &err);
        if (have.empty()) bad += "unreadable: " + rel + "\n";
        else if (have != h) bad += "changed: " + rel + "\n";
    }
    for (const auto& [rel, e] : L.files) if (!want.count(rel)) bad += "extra:   " + rel + "\n";
    return bad;
}

}  // namespace ai_arranger::media

#endif
