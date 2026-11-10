// Verified KST backup (K1 in the app; same guarantees as tools/kst_copy.py, whose manifest format it shares).
//
// Folder level only: files are copied as opaque bytes, never decoded or rewritten. The source is only read.
//   1. Refuse: a source that is not a folder or holds symlinks, special files or line breaks in names; a
//      destination that exists, sits on a mounted volume or inside the source; its manifest or work folder
//      existing; too little free space.
//   2. The destination's parent folder is opened once and checked through that handle (its real path and
//      its identity versus the source); every write then happens relative to it with *at() calls that never
//      follow symlinks and never replace anything (O_EXCL, exclusive rename). A path swapped after the check
//      cannot redirect the writes.
//   3. Copy into DST.INCOMPLETE, hashing each file with SHA-256 as it is read; read each copy back and compare.
//   4. Re-hash the whole source and re-list its folders: if anything changed during the copy, the backup fails.
//   5. Only then rename DST.INCOMPLETE -> DST and publish DST.manifest.sha256 beside it (the backup folder stays
//      byte-identical to the source).
// Nothing is ever deleted: a cancelled or failed backup stays as DST.INCOMPLETE (also after a crash), clearly
// marked, for the user to inspect or delete. Success is reported only after every step passed.
#ifndef AI_ARRANGER_MEDIA_KST_BACKUP_H
#define AI_ARRANGER_MEDIA_KST_BACKUP_H

#include "media/kst_inventory.h"
#include "media/output_guard.h"
#include "media/sha256.h"
#include <climits>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

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
struct Fd {
    int v = -1;
    explicit Fd(int x = -1) : v(x) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { if (v >= 0) ::close(v); }
};
// SHA-256 of a stream; when `out` is set, also writes the bytes there. "" on any error or when `more` returns
// false (cancelled is then set). Closes neither stream.
inline std::string sha256Stream(std::FILE* in, std::FILE* out, const std::function<bool(size_t)>& more, bool* cancelled, std::string* error,
                                const std::string& name) {
    Sha256 h;
    static thread_local char buf[1 << 20];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, in)) > 0;) {
        h.update(buf, n);
        if (out && std::fwrite(buf, 1, n, out) != n) { *error = "cannot write the copy of " + name + " (disk full?)"; return ""; }
        if (more && !more(n)) { *cancelled = true; return ""; }
    }
    if (std::ferror(in)) { *error = "read error in " + name; return ""; }
    return h.hex();
}
// Opens an existing file for reading only if it is a regular file: no symlink is followed and a FIFO or device
// swapped in after the listing cannot block the open. nullptr on failure.
inline std::FILE* openRegular(int dir, const char* path) {
    const int fd = ::openat(dir, path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { if (fd >= 0) ::close(fd); return nullptr; }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    std::FILE* f = ::fdopen(fd, "rb");
    if (!f) ::close(fd);
    return f;
}
inline std::string sha256File(const std::filesystem::path& p, const std::function<bool(size_t)>& more, bool* cancelled, std::string* error) {
    std::FILE* in = openRegular(AT_FDCWD, p.c_str());
    if (!in) { *error = "cannot read " + p.string() + " (missing, or no longer a regular file)"; return ""; }
    auto h = sha256Stream(in, nullptr, more, cancelled, error, p.string());
    std::fclose(in);
    return h;
}
// The real path of an open folder, as the kernel sees it.
inline std::string fdPath(int fd) {
    char b[PATH_MAX] = {};
#ifdef __APPLE__
    return fcntl(fd, F_GETPATH, b) == 0 ? std::string(b) : std::string();
#else
    const ssize_t n = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), b, sizeof b - 1);
    return n > 0 ? std::string(b, size_t(n)) : std::string();
#endif
}
// True when the open folder `dir` is `target` or lies anywhere below it (compared by device and inode, walking "..").
inline bool isSameOrBelow(int dir, const struct stat& target) {
    Fd cur(::dup(dir));
    for (int depth = 0; cur.v >= 0 && depth < 4096; ++depth) {
        struct stat st {};
        if (::fstat(cur.v, &st) != 0) return true;   // unknown: treat as inside (refuse)
        if (st.st_dev == target.st_dev && st.st_ino == target.st_ino) return true;
        Fd up(::openat(cur.v, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        struct stat ust {};
        if (up.v < 0 || ::fstat(up.v, &ust) != 0) return true;
        if (ust.st_dev == st.st_dev && ust.st_ino == st.st_ino) return false;   // reached the root
        std::swap(cur.v, up.v);
    }
    return true;
}
// Opens folder `rel` below `base` one component at a time, never following a symlink. -1 on failure.
inline int openDirBelow(int base, const std::string& rel) {
    int cur = ::dup(base);
    size_t from = 0;
    while (cur >= 0 && from < rel.size()) {
        const size_t to = rel.find('/', from);
        const std::string part = rel.substr(from, to == std::string::npos ? std::string::npos : to - from);
        from = to == std::string::npos ? rel.size() : to + 1;
        if (part.empty()) continue;
        const int next = ::openat(cur, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(cur);
        cur = next;
    }
    return cur;
}
// Rename inside one folder that never replaces an existing entry.
inline bool renameNewAt(int dir, const std::string& from, const std::string& to) {
#ifdef __APPLE__
    return ::renameatx_np(dir, from.c_str(), dir, to.c_str(), RENAME_EXCL) == 0;
#else
    return ::renameat2(dir, from.c_str(), dir, to.c_str(), RENAME_NOREPLACE) == 0;
#endif
}
inline std::pair<std::string, std::string> splitRel(const std::string& rel) {
    const auto k = rel.rfind('/');
    return k == std::string::npos ? std::pair<std::string, std::string>{"", rel} : std::pair<std::string, std::string>{rel.substr(0, k), rel.substr(k + 1)};
}
}  // namespace detail

inline BackupResult backupKst(const std::filesystem::path& source, const std::filesystem::path& destination, const BackupCallback& progress = {},
                              const std::function<void()>& afterCopy = {}) {
    namespace fs = std::filesystem;
    using detail::Fd;
    BackupResult r;
    std::error_code ec;
    const fs::path src = fs::canonical(source, ec);
    if (ec || !fs::is_directory(fs::symlink_status(source, ec))) { r.message = "refusing: " + source.string() + " is not a folder"; return r; }
    const fs::path dst = fs::absolute(destination);
    if (const auto why = outputFolderRefusal(dst); !why.empty()) { r.message = why; return r; }
    const fs::path parent = dst.parent_path();
    const std::string name = dst.filename().string(), workName = name + ".INCOMPLETE", manName = name + ".manifest.sha256",
                      manTmpName = manName + ".INCOMPLETE";
    if (name.empty() || name == "." || name == "..") { r.message = "refusing: no folder name in " + dst.string(); return r; }
    const fs::path work = parent / workName, man = parent / manName;

    // Pin the destination's parent and check it through the handle.
    Fd parentFd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (parentFd.v < 0) { r.message = "refusing: the folder " + parent.string() + " does not exist"; return r; }
    const std::string realParent = detail::fdPath(parentFd.v);
    if (realParent.empty() || realParent.rfind("/Volumes/", 0) == 0 || realParent == "/Volumes") {
        r.message = "refusing to write onto a mounted volume (" + dst.string() + "): write to a local folder, then copy it to the USB stick by hand";
        return r;
    }
    Fd srcFd(::open(src.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));   // pinned: a source folder swapped later is detected
    struct stat srcSt {};
    if (srcFd.v < 0 || ::fstat(srcFd.v, &srcSt) != 0) { r.message = "cannot read " + src.string(); return r; }
    if (detail::isSameOrBelow(parentFd.v, srcSt)) { r.message = "refusing: the backup would be inside the folder being backed up"; return r; }
    for (const auto& n : {name, workName, manName, manTmpName}) {
        struct stat st {};
        if (::fstatat(parentFd.v, n.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) { r.message = "refusing: " + (parent / n).string() + " already exists"; return r; }
    }

    BackupProgress pr;
    const auto wanted = [&] { return !progress || progress(pr); };
    detail::Listing L;
    std::string err;
    if (!detail::listFiles(src, L, err, wanted)) {
        r.cancelled = err == "cancelled";
        r.message = r.cancelled ? "Backup cancelled while reading the source; nothing was created" : "cannot list the source: " + err;
        return r;
    }
    if (!L.skipped.empty()) { r.message = "refusing: the source holds a symbolic link or special file, not copied: " + L.skipped.front(); return r; }
    auto badName = [](const std::string& rel) { return rel.find_first_of("\n\r") != std::string::npos; };   // the manifest is one line per file
    for (const auto& d : L.dirs) if (badName(d)) { r.message = "refusing: a folder name holds a line break, which the manifest cannot record"; return r; }
    for (const auto& [rel, e] : L.files) if (badName(rel)) { r.message = "refusing: a file name holds a line break, which the manifest cannot record"; return r; }
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

    if (::mkdirat(parentFd.v, workName.c_str(), 0755) != 0) { r.message = "cannot create " + work.string(); return r; }
    auto fail = [&](const std::string& why) {
        r.message = why + ". Nothing was deleted: the incomplete copy stays at " + work.string() +
                    " (marked INCOMPLETE; it is not a backup). Delete it when you no longer need it.";
        return r;
    };
    Fd workFd(::openat(parentFd.v, workName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (workFd.v < 0) return fail("Backup failed: cannot open " + work.string());
    const auto cancel = [&] { r.cancelled = true; return fail("Backup cancelled"); };
    for (const auto& d : L.dirs) {   // listed parents first
        const auto [up, leaf] = detail::splitRel(d);
        Fd at(detail::openDirBelow(workFd.v, up));
        if (at.v < 0 || ::mkdirat(at.v, leaf.c_str(), 0755) != 0) return fail("Backup failed: cannot create the folder " + d);
        if (!wanted()) return cancel();
    }
    const std::function<bool(size_t)> tick = [&](size_t n) { pr.bytesDone += n; return wanted(); };
    const std::function<bool(size_t)> stillWanted = [&](size_t) { return wanted(); };   // read-back and re-hash: cancel works there too
    std::map<std::string, std::string> hashes;
    bool cancelled = false;
    for (const auto& [rel, e] : L.files) {
        const auto [up, leaf] = detail::splitRel(rel);
        Fd at(detail::openDirBelow(workFd.v, up));
        const int fd = at.v < 0 ? -1 : ::openat(at.v, leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0) return fail("Backup failed: cannot create the copy of " + rel);
        std::FILE* out = ::fdopen(fd, "wb");
        std::FILE* in = detail::openRegular(AT_FDCWD, e.abs.c_str());
        if (!out || !in) {
            if (out) std::fclose(out); else ::close(fd);
            if (in) std::fclose(in);
            return fail("Backup failed: cannot read " + rel);
        }
        std::string hs = detail::sha256Stream(in, out, tick, &cancelled, &err, rel);
        std::fclose(in);
        if (std::fclose(out) != 0 && !hs.empty()) { hs.clear(); err = "cannot write the copy of " + rel; }
        if (cancelled) return cancel();
        if (hs.empty()) return fail("Backup failed: " + err);
        std::FILE* back = detail::openRegular(at.v, leaf.c_str());   // read back what was written (a FIFO swapped in cannot block)
        if (!back) return fail("Backup failed: cannot read back the copy of " + rel + " (missing, or no longer a regular file)");
        const std::string hd = detail::sha256Stream(back, nullptr, stillWanted, &cancelled, &err, rel);
        std::fclose(back);
        if (cancelled) return cancel();
        if (hd != hs) return fail("Backup failed: the copy of " + rel + " does not match its source");
        hashes[rel] = hs;
        ++pr.filesDone;
        if (!wanted()) return cancel();
    }
    if (afterCopy) afterCopy();
    detail::Listing again;   // the source must be exactly as it was: same folders, same files, same bytes
    if (!detail::listFiles(src, again, err, wanted)) return err == "cancelled" ? cancel() : fail("Backup failed: cannot re-read the source: " + err);
    auto a = L.dirs, b = again.dirs;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    bool same = a == b && again.files.size() == L.files.size() && again.skipped.empty();
    for (const auto& [rel, e] : again.files) {
        if (!same) break;
        const auto it = hashes.find(rel);
        same = it != hashes.end() && detail::sha256File(e.abs, stillWanted, &cancelled, &err) == it->second;
    }
    if (cancelled) return cancel();
    struct stat srcNow {};
    if (same && (::stat(src.c_str(), &srcNow) != 0 || srcNow.st_dev != srcSt.st_dev || srcNow.st_ino != srcSt.st_ino))
        return fail("Backup failed: the source folder " + src.string() + " was replaced by another folder during the backup");
    if (!same) return fail("Backup failed: the source changed during the backup");
    if (!wanted()) return cancel();   // last chance, also for a backup of folders only

    const int mfd = ::openat(parentFd.v, manTmpName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    std::FILE* m = mfd < 0 ? nullptr : ::fdopen(mfd, "w");
    if (!m) { if (mfd >= 0) ::close(mfd); return fail("Backup failed: cannot create " + (parent / manTmpName).string()); }
    bool wrote = true;
    for (const auto& [rel, h] : hashes) wrote &= std::fprintf(m, "%s  %s\n", h.c_str(), rel.c_str()) > 0;
    wrote &= std::fclose(m) == 0;
    if (!wrote) return fail("Backup failed: cannot write " + (parent / manTmpName).string());
    // Final check of the finished copy, through the pinned work folder: exactly the expected folders and files, same bytes.
    detail::Listing pub;
    const std::string workNow = detail::fdPath(workFd.v);   // where the pinned folder is now, even if a parent was swapped
    if (workNow.empty() || !detail::listFiles(workNow, pub, err, wanted))
        return err == "cancelled" ? cancel() : fail("Backup failed: cannot re-read the copy: " + err);
    auto pd = pub.dirs;
    std::sort(pd.begin(), pd.end());
    bool exact = pd == a && pub.files.size() == hashes.size() && pub.skipped.empty();
    for (const auto& [rel, h] : hashes) {
        if (!exact) break;
        const auto [up, leaf] = detail::splitRel(rel);
        Fd at(detail::openDirBelow(workFd.v, up));
        std::FILE* f = at.v < 0 ? nullptr : detail::openRegular(at.v, leaf.c_str());
        exact = f && pub.files.count(rel) && detail::sha256Stream(f, nullptr, stillWanted, &cancelled, &err, rel) == h;
        if (f) std::fclose(f);
    }
    if (cancelled) return cancel();
    if (!exact) return fail("Backup failed: the copy changed after it was verified");
    // Publish exactly the folder this run filled and checked: same device and inode before and after the rename.
    struct stat mine {}, there {};
    const auto isMine = [&](const std::string& n) {
        return ::fstat(workFd.v, &mine) == 0 && ::fstatat(parentFd.v, n.c_str(), &there, AT_SYMLINK_NOFOLLOW) == 0 && mine.st_dev == there.st_dev &&
               mine.st_ino == there.st_ino;
    };
    if (!isMine(workName)) return fail("Backup failed: " + work.string() + " was replaced by another folder during the backup");
    // Manifest first, folder last: until the folder rename, the copy is still named INCOMPLETE (also after a crash), and a
    // manifest without its folder verifies as nothing. If the folder cannot be published, the manifest is renamed back.
    if (!detail::renameNewAt(parentFd.v, manTmpName, manName)) return fail("refusing: " + man.string() + " appeared during the backup");
    const auto unpublish = [&](const std::string& why) {
        const bool back = detail::renameNewAt(parentFd.v, manName, manTmpName);
        return fail(why + (back ? "" : "; the manifest " + man.string() + " could not be withdrawn: it does not describe a backup"));
    };
    if (!detail::renameNewAt(parentFd.v, workName, name)) return unpublish("refusing: " + dst.string() + " appeared during the backup");
    if (!isMine(name)) {
        r.message = "Backup failed: the folder at " + dst.string() + " is not the one this run filled; it is NOT a verified backup.";
        return r;
    }
    // Report where the backup really is: the pinned folder, even if a parent was moved or swapped during the backup.
    const fs::path realDir = detail::fdPath(workFd.v), realParentNow = detail::fdPath(parentFd.v);
    const fs::path chosen = fs::weakly_canonical(dst, ec);
    r.ok = true;
    r.folder = realDir.empty() ? dst : realDir;
    r.manifest = realParentNow.empty() ? man : realParentNow / manName;
    r.files = hashes.size(); r.bytes = pr.bytesTotal;
    r.message = "Backup complete and verified: " + std::to_string(r.files) + " files, " + std::to_string(r.bytes) + " bytes, " + src.string() +
                " -> " + r.folder.string() + ". Every file was read back and matched its source by SHA-256, and the source was re-hashed "
                "after the copy (unchanged). Manifest: " + r.manifest.string() + "\n";
    if (r.folder != chosen)
        r.message += "Note: the destination folder was moved or replaced during the backup. The backup is at " + r.folder.string() + ", NOT at " +
                     dst.string() + ".\n";
    return r;
}

// Re-check a backup against its manifest (ours or tools/kst_copy.py's). "" = every file matches; otherwise
// the problems (changed / missing / extra / unreadable), one per line.
inline std::string verifyBackup(const std::filesystem::path& dst) {
    const auto man = backupManifestPath(dst);
    std::FILE* f = detail::openRegular(AT_FDCWD, man.c_str());
    if (!f) return "no manifest at " + man.string() + " (missing or not a regular file)";
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
    const bool readError = std::ferror(f) != 0;
    std::fclose(f);
    if (readError) return "cannot read the manifest " + man.string();
    detail::Listing L;
    std::string err;
    if (!detail::listFiles(dst, L, err)) return "cannot list " + dst.string() + ": " + err;
    for (const auto& s : L.skipped) bad += "unexpected link or special file: " + s + "\n";
    bool cancelled = false;
    for (const auto& [rel, h] : want) {
        const auto it = L.files.find(rel);
        if (it == L.files.end()) { bad += "missing: " + rel + "\n"; continue; }
        const auto have = detail::sha256File(it->second.abs, {}, &cancelled, &err);
        if (have.empty()) bad += "unreadable: " + rel + "\n";
        else if (have != h) bad += "changed: " + rel + "\n";
    }
    for (const auto& [rel, e] : L.files) if (!want.count(rel)) bad += "extra:   " + rel + "\n";
    return bad;
}

}  // namespace ai_arranger::media

#endif
