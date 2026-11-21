#ifndef AI_ARRANGER_MEDIA_OUTPUT_GUARD_H
#define AI_ARRANGER_MEDIA_OUTPUT_GUARD_H

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ai_arranger::media {

// Renames `a` (in `from`) to `b` (in `to`); never replaces an entry.
inline bool renameAtNoReplace(int from, const std::string& a, int to, const std::string& b) {
#ifdef __APPLE__
    return ::renameatx_np(from, a.c_str(), to, b.c_str(), RENAME_EXCL) == 0;
#else
    return ::renameat2(from, a.c_str(), to, b.c_str(), RENAME_NOREPLACE) == 0;
#endif
}
// Ownership-proven cleanup: removes the entry `name` below `dirFd` ONLY when it is still the exact entry this job created
// (`mine`: device + inode, taken from its own fd/stat). POSIX has no unlink-by-inode, so the entry is first renamed (no
// replace) to a private name, re-identified there, and only then removed; if it is not the job's entry, or any step fails,
// nothing is deleted: the entry is put back (or stays under the private name) and the reason is returned so it can be
// reported. "" = removed. A folder is removed with rmdir (refused when not empty, then put back). The remaining window
// is a swap of the private, never-announced name between the re-identification and the unlink.
inline std::string removeOwned(int dirFd, const std::string& name, const struct stat& mine, bool folder) {
    static std::atomic<unsigned> counter{0};
    const auto same = [&](const struct stat& s) { return s.st_dev == mine.st_dev && s.st_ino == mine.st_ino; };
    struct stat now {};
    if (::fstatat(dirFd, name.c_str(), &now, AT_SYMLINK_NOFOLLOW) != 0 || !same(now))
        return "kept " + name + ": it is not the entry this job created (nothing was removed)";
    const std::string hold = name + ".removing-" + std::to_string(::getpid()) + "-" + std::to_string(counter++);
    if (!renameAtNoReplace(dirFd, name, dirFd, hold)) return "kept " + name + ": it could not be set aside for removal";
    struct stat held {};
    if (::fstatat(dirFd, hold.c_str(), &held, AT_SYMLINK_NOFOLLOW) != 0 || !same(held)) {
        const bool back = renameAtNoReplace(dirFd, hold, dirFd, name);
        return "kept " + (back ? name : hold) + ": the entry that was set aside is not the one this job created (nothing was removed)";
    }
    if (::unlinkat(dirFd, hold.c_str(), folder ? AT_REMOVEDIR : 0) != 0) {
        const bool back = renameAtNoReplace(dirFd, hold, dirFd, name);
        return "kept " + (back ? name : hold) + (folder ? ": it is not empty" : ": it could not be removed");
    }
    return "";
}

// ── Pinned root (export runner, path-race remediation) ───────────────────────────────────────────────────────────────
// The export runner hands a legacy path-taking writer a pathname below the job's pinned folder and installs that folder
// here (thread-local, around the writer call). While a root is installed, every creating or renaming helper below
// (and the writers that call them) works relative to the open folder, one component at a time, no link followed, and
// REFUSES any path that is not strictly below the root's own path: the attacker-controlled absolute path is never
// reopened for a write. Without a root (CLI, tests, GUI direct) every helper does what it always did.
namespace pinned {
struct Root { int fd = -1; std::string prefix; };   // prefix: the folder's real path, no trailing slash
inline thread_local const Root* current = nullptr;
struct Scope {
    const Root* prev;
    explicit Scope(const Root* r) : prev(current) { current = r; }
    ~Scope() { current = prev; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
inline bool active() { return current != nullptr; }
// The components of `p` below the root ("" = the root itself); false when p is outside it or holds "..".
inline bool below(const std::filesystem::path& p, std::vector<std::string>* parts) {
    if (!current) return false;
    const std::string s = p.lexically_normal().string();
    const std::string& pre = current->prefix;
    if (s.compare(0, pre.size(), pre) != 0 || (s.size() > pre.size() && s[pre.size()] != '/')) return false;
    parts->clear();
    for (size_t from = pre.size() + 1; from < s.size();) {
        const size_t to = s.find('/', from);
        const std::string c = s.substr(from, to == std::string::npos ? std::string::npos : to - from);
        from = to == std::string::npos ? s.size() : to + 1;
        if (c.empty()) continue;
        if (c == "." || c == "..") return false;
        parts->push_back(c);
    }
    return true;
}
// Opens the folder `p` below the root (a new fd; the root itself when p is the root), never following a link. -1 on failure.
inline int openDir(const std::filesystem::path& p) {
    std::vector<std::string> parts;
    if (!below(p, &parts)) { errno = EXDEV; return -1; }
    int cur = ::dup(current->fd);
    for (const auto& c : parts) {
        if (cur < 0) return -1;
        const int next = ::openat(cur, c.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(cur);
        cur = next;
    }
    return cur;
}
// A new folder `p` (like create_directory: false when it exists or cannot be made) / every missing folder of `p` (create_directories).
inline bool mkdir(const std::filesystem::path& p, bool all) {
    std::vector<std::string> parts;
    if (!below(p, &parts)) { errno = EXDEV; return false; }
    int cur = ::dup(current->fd);
    bool made = false;
    for (size_t i = 0; i < parts.size() && cur >= 0; ++i) {
        const bool last = i + 1 == parts.size();
        const bool ok = ::mkdirat(cur, parts[i].c_str(), 0755) == 0;
        if (!ok && (errno != EEXIST || (last && !all))) { ::close(cur); return false; }
        made = ok;
        const int next = ::openat(cur, parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(cur);
        cur = next;
    }
    if (cur >= 0) ::close(cur);
    return cur >= 0 && (all || made);
}
// Splits `p` into its parent folder (opened below the root) and leaf name. false: outside the root, or no leaf.
inline bool parentOf(const std::filesystem::path& p, int* dirFd, std::string* leaf) {
    std::vector<std::string> parts;
    if (!below(p, &parts) || parts.empty()) { errno = EXDEV; return false; }
    *leaf = parts.back();
    *dirFd = openDir(p.lexically_normal().parent_path());
    return *dirFd >= 0;
}
inline bool renameNoReplace(const std::filesystem::path& a, const std::filesystem::path& b) {
    int da = -1, db = -1;
    std::string na, nb;
    const bool ok = parentOf(a, &da, &na) && parentOf(b, &db, &nb);
    bool done = false;
    if (ok) {
#ifdef __APPLE__
        done = ::renameatx_np(da, na.c_str(), db, nb.c_str(), RENAME_EXCL) == 0;
#else
        done = ::renameat2(da, na.c_str(), db, nb.c_str(), RENAME_NOREPLACE) == 0;
#endif
    }
    if (da >= 0) ::close(da);
    if (db >= 0) ::close(db);
    return done;
}
// A new file `p`, open for writing (exclusive, no link followed), or -1.
inline int createFile(const std::filesystem::path& p) {
    int d = -1;
    std::string leaf;
    if (!parentOf(p, &d, &leaf)) return -1;
    const int fd = ::openat(d, leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    ::close(d);
    return fd;
}
// Removes the file `p` only when it is still the file `mine` (see removeOwned), below the root. "" = removed, else why kept.
inline std::string removeOwnedFile(const std::filesystem::path& p, const struct stat& mine) {
    int d = -1;
    std::string leaf;
    if (!parentOf(p, &d, &leaf)) return "kept " + p.string() + ": outside the job folder";
    const auto why = removeOwned(d, leaf, mine, false);
    ::close(d);
    return why;
}
}  // namespace pinned

// The writers' creating helpers: pinned below a root when one is installed, by path otherwise.
inline bool makeFolder(const std::filesystem::path& p) {
    if (pinned::active()) return pinned::mkdir(p, false);
    std::error_code ec;
    return std::filesystem::create_directory(p, ec);
}
inline void makeFolders(const std::filesystem::path& p) {
    if (pinned::active()) { pinned::mkdir(p, true); return; }
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
}
// A NEW file (exclusive, no link followed, never truncates), open for writing, or -1: below the pinned root when one is installed.
inline int createNewFile(const std::filesystem::path& p) {
    if (pinned::active()) return pinned::createFile(p);
    return ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
}
// A folder open for reading/writing below it (a new fd; -1 on failure): `flags` as for open(); no link followed when pinned.
inline int openFolder(const std::filesystem::path& p, int flags) {
    if (pinned::active()) return pinned::openDir(p);
    return ::open(p.c_str(), flags);
}

// `out` as the folder it names: "/tmp/pkg/" and "/tmp/pkg/." are /tmp/pkg, so its work folder is the sibling /tmp/pkg.INCOMPLETE.
inline std::filesystem::path folderPath(std::filesystem::path out) {
    out = out.lexically_normal();   // drops "." and resolves "x/.." lexically
    while (!out.has_filename() && out.has_parent_path() && out.parent_path() != out) out = out.parent_path();
    return out;
}
inline std::filesystem::path incompletePath(const std::filesystem::path& out) { return folderPath(out).string() + ".INCOMPLETE"; }

// The last of outputFolderRefusal's checks: an unfinished earlier write NAME.INCOMPLETE exists. Separate so an export job
// can tell "only a leftover is in the way" (it may then move it aside with consent) from every other refusal.
inline std::string incompleteRefusal(const std::filesystem::path& out) {
    std::error_code ec;
    const auto inc = incompletePath(out);   // a dangling symlink there counts too
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(inc, ec)) || std::filesystem::exists(inc, ec) || ec)
        return "refusing: " + incompletePath(out).string() + " already exists (an earlier write that did not finish; delete it when you no longer need it)";
    return "";
}

// Why a new folder (package or backup) may not be created at `out`, or "" when it may. A mounted volume (USB stick) is
// refused, also when reached through a symlink, because hardware-side writes are not authorized in
// this project phase: write locally and copy the folder by hand. An existing path is never reused.
// Shared by every writer (WAV conversion, plugin sampler CLI and GUI, KST backup) so the rule cannot drift.
inline std::string outputFolderRefusal(const std::filesystem::path& out) {
    std::error_code ec;
    const auto real = std::filesystem::weakly_canonical(out, ec);   // follows symlinks in the existing part of the path
    if (ec || real.string().rfind("/Volumes/", 0) == 0)
        return "refusing to write onto a mounted volume (" + out.string() + "): write to a local folder, then copy it to the USB stick by hand";
    if (std::filesystem::exists(out, ec) || ec) return "refusing: " + out.string() + " already exists";
    return incompleteRefusal(out);
}

// A new folder is written under NAME.INCOMPLETE and published under NAME by one rename that never replaces anything,
// so a failed or interrupted write never leaves a half-written folder under its real name.
inline bool publishNewFolder(const std::filesystem::path& work, const std::filesystem::path& out) {
    if (pinned::active()) return pinned::renameNoReplace(work, folderPath(out));
#ifdef __APPLE__
    return ::renameatx_np(AT_FDCWD, work.c_str(), AT_FDCWD, folderPath(out).c_str(), RENAME_EXCL) == 0;
#else
    return ::renameat2(AT_FDCWD, work.c_str(), AT_FDCWD, folderPath(out).c_str(), RENAME_NOREPLACE) == 0;
#endif
}

// A new single file (Style SMF, UASF): the same refusals as a folder (mounted volume, also through a link; an existing
// path), then written to `path.tmp` and published by one rename that never replaces anything. "" = written.
// shortcut: a parent folder swapped for a symlink between the guard check and the create is not caught; the guard stops
// accidental writes to hardware volumes, not a concurrent hostile local process (same as the package writers, D16 C1).
inline std::string writeNewFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    if (auto why = outputFolderRefusal(path); !why.empty()) return why;
    const std::filesystem::path tmp = path.string() + ".tmp";
    std::error_code ec;
    if (std::filesystem::exists(tmp, ec) || ec) return "refusing: " + tmp.string() + " already exists";
    // "x": exclusive create (O_EXCL), so a file or a symlink that appears at the temp path is never followed or truncated
    std::FILE* f = nullptr;
    if (pinned::active()) {
        const int fd = pinned::createFile(tmp);
        f = fd < 0 ? nullptr : ::fdopen(fd, "wb");
        if (!f && fd >= 0) ::close(fd);
    } else {
        f = std::fopen(tmp.c_str(), "wbx");
    }
    if (!f) return "refusing: cannot create " + tmp.string() + " (it may already exist)";
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    struct stat mine {};
    const bool known = ::fstat(::fileno(f), &mine) == 0;
    const auto dropTmp = [&]() -> std::string {   // pinned: only the file this call created, else kept and reported
        if (!pinned::active()) { std::filesystem::remove(tmp, ec); return ""; }
        const auto why = known ? pinned::removeOwnedFile(tmp, mine) : "kept " + tmp.string() + ": its identity is unknown";
        return why.empty() ? "" : " (" + why + ")";
    };
    if (std::fclose(f) != 0 || !ok) { const auto kept = dropTmp(); return "cannot write " + tmp.string() + kept; }
    if (!publishNewFolder(tmp, path)) { const auto kept = dropTmp(); return "refusing: " + path.string() + " appeared while writing" + kept; }
    return "";
}

}  // namespace ai_arranger::media

#endif
