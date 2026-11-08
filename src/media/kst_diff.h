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

}  // namespace ai_arranger::media

#endif  // AI_ARRANGER_MEDIA_KST_DIFF_H
