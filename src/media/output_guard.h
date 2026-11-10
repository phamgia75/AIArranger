#ifndef AI_ARRANGER_MEDIA_OUTPUT_GUARD_H
#define AI_ARRANGER_MEDIA_OUTPUT_GUARD_H

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <string>

namespace ai_arranger::media {

// `out` without trailing separators: "/tmp/pkg/" names the folder /tmp/pkg, so its work folder is the sibling /tmp/pkg.INCOMPLETE.
inline std::filesystem::path folderPath(std::filesystem::path out) {
    while (!out.has_filename() && out.has_parent_path() && out.parent_path() != out) out = out.parent_path();
    return out;
}
inline std::filesystem::path incompletePath(const std::filesystem::path& out) { return folderPath(out).string() + ".INCOMPLETE"; }

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
    if (std::filesystem::exists(incompletePath(out), ec) || ec)
        return "refusing: " + incompletePath(out).string() + " already exists (an earlier write that did not finish; delete it when you no longer need it)";
    return "";
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

}  // namespace ai_arranger::media

#endif
