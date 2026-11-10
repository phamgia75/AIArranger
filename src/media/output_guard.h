#ifndef AI_ARRANGER_MEDIA_OUTPUT_GUARD_H
#define AI_ARRANGER_MEDIA_OUTPUT_GUARD_H

#include <filesystem>
#include <string>

namespace ai_arranger::media {

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
    return "";
}

}  // namespace ai_arranger::media

#endif
