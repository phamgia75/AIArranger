#ifndef AI_ARRANGER_MEDIA_SOUND_PACKAGE_VERIFY_H
#define AI_ARRANGER_MEDIA_SOUND_PACKAGE_VERIFY_H

// Re-check a Sound or Drum Kit package folder, for example the copy made by hand on a USB stick before it goes to
// the Pa5X. Read-only. Two checks, both shared with the rest of the code: every file against PACKAGE.sha256 (the
// manifest verifier of the KST backup) and the package reader loadPackage (RESOURCES.tsv, every WAV, TX references).
// Finder's .DS_Store and ._* files (added when copying to a FAT stick) are neither expected nor reported.

#include "media/kst_backup.h"
#include "media/sound/package_writer.h"

namespace ai_arranger::media::sound {

struct PackageCheck { bool ok = false; std::string text; };

inline PackageCheck verifyPackage(const std::filesystem::path& dir, const std::function<bool()>& keepGoing = {}) {
    namespace fs = std::filesystem;
    PackageCheck r;
    const fs::path man = dir / pkgio::FileSet::kPackageManifest;
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(man, ec))) {
        r.text = "Not verified: " + dir.string() + " has no " + pkgio::FileSet::kPackageManifest +
                 " (not an AIArranger package, or one written before 2026-11-11: write it again to get one).";
        return r;
    }
    size_t files = 0;
    const auto skip = [](const std::string& rel) {
        const std::string leaf = fs::path(rel).filename().string();
        return rel == pkgio::FileSet::kPackageManifest || leaf == ".DS_Store" || leaf.rfind("._", 0) == 0;
    };
    const std::string bad = verifyManifest(dir, man, keepGoing, &files, skip);
    if (bad == "cancelled") { r.text = "Verify cancelled. Nothing was changed."; return r; }
    if (!bad.empty()) { r.text = "NOT verified (" + dir.string() + "):\n" + bad + "Do not import this copy; copy the package again."; return r; }
    graph::Graph g;
    std::string pkg, err;
    if (!loadPackage(dir, g, pkg, &err)) { r.text = "NOT verified: the files match, but the package does not load: " + err; return r; }
    r.ok = true;
    r.text = "Package verified: all " + std::to_string(files) + " files match PACKAGE.sha256 by SHA-256, nothing is missing or extra, "
             "and it loads back (RESOURCES.tsv, every WAV, TX references). " + dir.string();
    return r;
}

}  // namespace ai_arranger::media::sound

#endif
