// KST folder comparison (P2, non-destructive): which files were added, removed, changed or moved
// between two folders, e.g. a source and its backup (S6 owner check O4), or two saves of the same
// KST. Same rules as kst_inventory.h: contents are opaque bytes, symlinks are not followed, nothing
// is written. Equality is byte-by-byte, never a hash alone.
#ifndef AI_ARRANGER_MEDIA_KST_DIFF_H
#define AI_ARRANGER_MEDIA_KST_DIFF_H

#include "media/kst_inventory.h"

namespace ai_arranger::media {

struct KstDiff {
    bool ok = false;
    std::string error;
    uint32_t identical = 0;
    std::vector<std::string> added, removed, changed;          // relative paths, sorted
    std::vector<std::pair<std::string, std::string>> moved;    // (path in A, path in B), byte-identical
    std::vector<std::string> skipped;                          // "A: rel" / "B: rel" (symlinks, unreadable)
    bool same() const { return ok && added.empty() && removed.empty() && changed.empty() && moved.empty(); }
};

namespace detail {
struct Entry { std::filesystem::path abs; uintmax_t size; };
inline bool listFiles(const std::filesystem::path& root, const char* tag, std::map<std::string, Entry>& out,
                      std::vector<std::string>& skipped, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(root, ec))) { error = "not a directory (symlinks are not followed): " + root.string(); return false; }
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) { error = "cannot open " + root.string() + ": " + ec.message(); return false; }
    for (; it != end; it.increment(ec)) {
        if (ec) { skipped.push_back(std::string(tag) + ": (iteration error) " + ec.message()); ec.clear(); continue; }
        const auto st = it->symlink_status(ec);
        const std::string rel = it->path().lexically_relative(root).generic_string();
        if (fs::is_directory(st)) continue;
        if (!fs::is_regular_file(st)) { skipped.push_back(std::string(tag) + ": " + rel); continue; }
        const uintmax_t size = it->file_size(ec);
        if (ec) { skipped.push_back(std::string(tag) + ": " + rel); ec.clear(); continue; }
        out[rel] = {it->path(), size};
    }
    return true;
}
}  // namespace detail

inline KstDiff diff(const std::filesystem::path& a, const std::filesystem::path& b) {
    KstDiff d;
    std::map<std::string, detail::Entry> A, B;
    if (!detail::listFiles(a, "A", A, d.skipped, d.error) || !detail::listFiles(b, "B", B, d.skipped, d.error)) return d;
    std::vector<std::string> onlyA, onlyB;
    for (const auto& [rel, e] : A) {
        const auto it = B.find(rel);
        if (it == B.end()) { onlyA.push_back(rel); continue; }
        if (e.size == it->second.size && detail::sameBytes(e.abs, it->second.abs)) ++d.identical;
        else d.changed.push_back(rel);
    }
    for (const auto& [rel, e] : B) if (!A.count(rel)) onlyB.push_back(rel);
    // A removed file byte-identical to an added one is a move (rename / re-bank), each used once.
    // shortcut: pairwise byte compare of equal-size files, O(removed x added); hash first if folders reach 10k+ moves.
    std::vector<bool> used(onlyB.size(), false);
    for (const auto& ra : onlyA) {
        bool found = false;
        for (size_t j = 0; j < onlyB.size() && !found; ++j)
            if (!used[j] && A[ra].size == B[onlyB[j]].size && detail::sameBytes(A[ra].abs, B[onlyB[j]].abs)) {
                used[j] = found = true;
                d.moved.push_back({ra, onlyB[j]});
            }
        if (!found) d.removed.push_back(ra);
    }
    for (size_t j = 0; j < onlyB.size(); ++j) if (!used[j]) d.added.push_back(onlyB[j]);
    std::sort(d.skipped.begin(), d.skipped.end());
    d.ok = true;
    return d;
}

inline std::string format(const KstDiff& d, const std::string& a, const std::string& b) {
    if (!d.ok) return "error: " + d.error + "\n";
    std::ostringstream o;
    o << "A: " << a << "\nB: " << b << "\n"
      << "identical " << d.identical << ", changed " << d.changed.size() << ", removed " << d.removed.size()
      << ", added " << d.added.size() << ", moved " << d.moved.size() << ", skipped " << d.skipped.size() << "\n";
    for (const auto& p : d.changed) o << "  changed  " << p << "\n";
    for (const auto& p : d.removed) o << "  removed  " << p << "\n";
    for (const auto& p : d.added) o << "  added    " << p << "\n";
    for (const auto& [x, y] : d.moved) o << "  moved    " << x << " -> " << y << "\n";
    for (const auto& p : d.skipped) o << "  skipped  " << p << "\n";
    o << (d.same() ? "RESULT: same content\n" : "RESULT: different\n");
    return o.str();
}

// Merge plan (read-only): what loading folder SRC onto the User data saved in folder DST would do,
// file by file, under the Pa5X's documented load rules (manual v1.4): Merge replaces only files with
// the same name and keeps the rest; Replace All deletes every existing User file of that type
// (p.734); a whole KST holding a PCM folder deletes ALL User Samples in memory (p.736).
// The same relative path is taken as "same name"; how the Pa5X compares case is UNKNOWN, so names
// differing only in case are flagged separately. Nothing is copied or loaded.
// True when the folder holds a directory named PCM (any case, any depth, symlinks not followed):
// the manual's test for "this KST contains User Samples" (p.736).
inline bool hasPcmFolder(const std::filesystem::path& root) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec) || it->is_symlink(ec)) continue;
        std::string n = it->path().filename().string();
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::toupper(c); });
        if (n == "PCM") return true;
    }
    return false;
}

inline std::string formatMergePlan(const KstDiff& d, const std::string& src, const std::string& dst, bool srcHasPcm) {
    if (!d.ok) return "error: " + d.error + "\n";
    auto lower = [](std::string x) { std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return std::tolower(c); }); return x; };
    std::map<std::string, std::string> dstLower;   // dst-only paths by lower case
    for (const auto& p : d.removed) dstLower[lower(p)] = p;
    const bool pcm = srcHasPcm;
    std::vector<std::string> add, caseOnly;
    for (const auto& p : d.added) {
        if (dstLower.count(lower(p))) caseOnly.push_back(p + "  vs existing " + dstLower[lower(p)]); else add.push_back(p);
    }
    std::ostringstream o;
    o << "Merge plan (read-only; nothing is loaded): SRC " << src << " onto the User data saved in DST " << dst << "\n"
      << "  replace (same name, different content; the existing file is deleted, p.734-735): " << d.changed.size() << "\n"
      << "  add (new name): " << add.size() << "\n"
      << "  unchanged (same name, same bytes): " << d.identical << "\n"
      << "  duplicate (SRC file already in DST under another name; would be stored twice): " << d.moved.size() << "\n"
      << "  name differs only in case (Pa5X behaviour UNKNOWN; treat as a possible replace): " << caseOnly.size() << "\n"
      << "  kept on Merge, deleted by Replace All of that type (DST only, p.734): " << (d.removed.size() - caseOnly.size()) << "\n";
    for (const auto& p : d.changed) o << "  REPLACE    " << p << "\n";
    for (const auto& p : caseOnly) o << "  CASE?      " << p << "\n";
    for (const auto& p : add) o << "  add        " << p << "\n";
    for (const auto& [x, y] : d.moved) o << "  duplicate  " << y << " (same bytes as DST " << x << ")\n";
    for (const auto& p : d.skipped) o << "  skipped    " << p << "\n";
    if (pcm) o << "WARNING: SRC has User Sample data (PCM folder). Loading the whole KST deletes ALL User Samples in memory\n"
                  "         (manual p.736). Load individual Sounds instead to keep existing samples.\n";
    o << (d.changed.empty() && caseOnly.empty() && !pcm ? "RESULT: no existing User data would be replaced\n"
                                                        : "RESULT: existing User data would be replaced or deleted; back up first (manual p.7)\n");
    return o.str();
}

}  // namespace ai_arranger::media

#endif  // AI_ARRANGER_MEDIA_KST_DIFF_H
