#ifndef AI_ARRANGER_MEDIA_OUTPUT_GUARD_H
#define AI_ARRANGER_MEDIA_OUTPUT_GUARD_H

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <vector>

namespace ai_arranger::media {

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
    std::FILE* f = std::fopen(tmp.c_str(), "wbx");
    if (!f) return "refusing: cannot create " + tmp.string() + " (it may already exist)";
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0 || !ok) { std::filesystem::remove(tmp, ec); return "cannot write " + tmp.string(); }
    if (!publishNewFolder(tmp, path)) { std::filesystem::remove(tmp, ec); return "refusing: " + path.string() + " appeared while writing"; }
    return "";
}

}  // namespace ai_arranger::media

#endif
