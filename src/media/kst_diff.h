// KST folder comparison and load/merge planning (P2, non-destructive, FILE LEVEL ONLY).
//
// What this is: byte-level comparison of two folders (added / removed / changed / moved), and a
// file-level prediction of a Pa5X load under the manual's documented rules. What this is NOT: a
// resource dependency analysis or a Sound-aware merge. Which Sound uses which sample lives inside
// the KST files; they are opaque bytes here and are never parsed.
//
// Same rules as kst_inventory.h: symlinks are not followed, nothing is written. Equality is
// byte-by-byte; a hash only pre-selects candidates. Any scan or read error fails the whole result
// (ok = false) instead of being reported as "same" or "safe".
#ifndef AI_ARRANGER_MEDIA_KST_DIFF_H
#define AI_ARRANGER_MEDIA_KST_DIFF_H

#include "media/kst_inventory.h"
#include <set>

namespace ai_arranger::media {

namespace detail {
// Byte-identical lookup into a pool of files: same size, then same FNV-1a hash (each file hashed at
// most once), then compareFiles. Cost is O(total bytes), not O(pairs x bytes).
class ContentIndex {
public:
    explicit ContentIndex(const std::map<std::string, Entry>& pool) : pool_(pool) {
        for (const auto& [rel, e] : pool) bySize_[e.size].push_back(rel);
    }
    // A byte-identical pool file not in `exclude`, or "". Sets `error` on a read failure.
    std::string find(const Entry& e, const std::set<std::string>& exclude, std::string& error) {
        const auto it = bySize_.find(e.size);
        if (it == bySize_.end()) return "";
        uint64_t h = 0;
        if (!hashFile(e.abs, h)) { error = "cannot read " + e.abs.string(); return ""; }
        for (const auto& rel : it->second) {
            if (exclude.count(rel)) continue;
            uint64_t ph = 0;
            if (const auto c = hashes_.find(rel); c != hashes_.end()) ph = c->second;
            else if (!hashFile(pool_.at(rel).abs, ph)) { error = "cannot read " + pool_.at(rel).abs.string(); return ""; }
            else hashes_[rel] = ph;
            if (ph != h) continue;
            const int c = compareFiles(e.abs, pool_.at(rel).abs);
            if (c < 0) { error = "cannot read " + e.abs.string() + " or " + pool_.at(rel).abs.string(); return ""; }
            if (c == 1) return rel;
        }
        return "";
    }
private:
    const std::map<std::string, Entry>& pool_;
    std::map<uintmax_t, std::vector<std::string>> bySize_;
    std::map<std::string, uint64_t> hashes_;
};

inline std::string lower(std::string x) {
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return std::tolower(c); });
    return x;
}
}  // namespace detail

// ── Byte-level comparison ──────────────────────────────────────────────────────────────────────

struct KstDiff {
    bool ok = false;
    std::string error;
    uint32_t identical = 0;
    std::vector<std::string> added, removed, changed;          // relative paths, sorted
    std::vector<std::pair<std::string, std::string>> moved;    // (path in A, path in B), byte-identical
    std::vector<std::string> skipped;                          // "A: rel" / "B: rel" (symlinks, special files)
    // Same content only if nothing differs AND nothing was skipped (skipped entries were not compared).
    bool same() const { return ok && added.empty() && removed.empty() && changed.empty() && moved.empty() && skipped.empty(); }
};

inline KstDiff diff(const std::filesystem::path& a, const std::filesystem::path& b) {
    KstDiff d;
    detail::Listing A, B;
    if (!detail::listFiles(a, A, d.error) || !detail::listFiles(b, B, d.error)) return d;
    for (const auto& s : A.skipped) d.skipped.push_back("A: " + s);
    for (const auto& s : B.skipped) d.skipped.push_back("B: " + s);
    std::map<std::string, detail::Entry> onlyB;
    std::vector<std::string> onlyA;
    for (const auto& [rel, e] : A.files) {
        const auto it = B.files.find(rel);
        if (it == B.files.end()) { onlyA.push_back(rel); continue; }
        const int c = e.size == it->second.size ? detail::compareFiles(e.abs, it->second.abs) : 0;
        if (c < 0) { d.error = "cannot read " + rel; return d; }
        if (c == 1) ++d.identical; else d.changed.push_back(rel);
    }
    for (const auto& [rel, e] : B.files) if (!A.files.count(rel)) onlyB[rel] = e;
    // A removed file byte-identical to an added one is a move (rename / re-bank), each used once.
    detail::ContentIndex idx(onlyB);
    std::set<std::string> used;
    for (const auto& ra : onlyA) {
        const std::string m = idx.find(A.files[ra], used, d.error);
        if (!d.error.empty()) return d;
        if (m.empty()) d.removed.push_back(ra);
        else { used.insert(m); d.moved.push_back({ra, m}); }
    }
    for (const auto& [rel, e] : onlyB) if (!used.count(rel)) d.added.push_back(rel);
    std::sort(d.skipped.begin(), d.skipped.end());
    d.ok = true;
    return d;
}

inline std::string format(const KstDiff& d, const std::string& a, const std::string& b) {
    if (!d.ok) return "error: " + d.error + "\n";
    std::ostringstream o;
    o << "Byte-level folder comparison (file contents are not interpreted)\nA: " << a << "\nB: " << b << "\n"
      << "identical " << d.identical << ", changed " << d.changed.size() << ", removed " << d.removed.size()
      << ", added " << d.added.size() << ", moved " << d.moved.size() << ", skipped " << d.skipped.size() << "\n";
    for (const auto& p : d.changed) o << "  changed  " << p << "\n";
    for (const auto& p : d.removed) o << "  removed  " << p << "\n";
    for (const auto& p : d.added) o << "  added    " << p << "\n";
    for (const auto& [x, y] : d.moved) o << "  moved    " << x << " -> " << y << "\n";
    for (const auto& p : d.skipped) o << "  skipped  " << p << " (not compared)\n";
    o << (d.same() ? "RESULT: same content\n" : d.skipped.empty() ? "RESULT: different\n" : "RESULT: not verified (skipped entries)\n");
    return o.str();
}

// ── File-level load/merge plan ─────────────────────────────────────────────────────────────────
// What loading folder SRC onto the User data saved in folder DST would do, file by file, under the
// Pa5X's documented load rules (manual v1.4): Merge replaces only files with the same name and
// keeps the rest; Replace All deletes every existing User file of the type being loaded (p.734);
// a whole KST holding a PCM folder deletes ALL User Samples in memory (p.736). "Same name" is taken
// as the same relative path; how the Pa5X compares case is UNKNOWN, so names that differ only in
// case count as possible replacements. Sound/sample dependencies are NOT analysed.

struct MergePlan {
    bool ok = false;
    std::string error;
    std::vector<std::string> replace, add, unchanged;              // SRC paths
    std::vector<std::pair<std::string, std::string>> caseOnly;     // (SRC path, DST path)
    std::vector<std::pair<std::string, std::string>> duplicate;    // (added SRC path, byte-identical DST path)
    std::vector<std::string> kept;                                 // DST paths no SRC file names (Merge keeps them)
    std::vector<std::string> replaceAllDeletes;                    // kept DST files of a type SRC also holds
    std::vector<std::string> skipped;
    bool srcPcm = false;
    bool mergeSafe() const { return ok && replace.empty() && caseOnly.empty() && !srcPcm && skipped.empty(); }
    bool replaceAllSafe() const { return mergeSafe() && replaceAllDeletes.empty(); }
};

inline MergePlan planMerge(const std::filesystem::path& src, const std::filesystem::path& dst) {
    MergePlan p;
    detail::Listing S, D;
    if (!detail::listFiles(src, S, p.error) || !detail::listFiles(dst, D, p.error)) return p;
    p.srcPcm = S.pcm;
    for (const auto& s : S.skipped) p.skipped.push_back("SRC: " + s);
    for (const auto& s : D.skipped) p.skipped.push_back("DST: " + s);
    std::multimap<std::string, std::string> dstLower;
    for (const auto& [rel, e] : D.files) dstLower.insert({detail::lower(rel), rel});
    auto type = [](const std::string& rel) { return detail::lower(rel.substr(0, rel.find('/'))); };   // first folder = data type
    std::set<std::string> named, srcTypes;   // DST paths matched by name (exact or case-only)
    detail::ContentIndex idx(D.files);
    for (const auto& [rel, e] : S.files) {
        srcTypes.insert(type(rel));
        if (const auto it = D.files.find(rel); it != D.files.end()) {
            named.insert(rel);
            const int c = e.size == it->second.size ? detail::compareFiles(e.abs, it->second.abs) : 0;
            if (c < 0) { p.error = "cannot read " + rel; return p; }
            (c == 1 ? p.unchanged : p.replace).push_back(rel);
        }
        bool caseHit = false;   // checked even after an exact match: DST may hold both "Jazz" and "JAZZ"
        for (auto [lo, hi] = dstLower.equal_range(detail::lower(rel)); lo != hi; ++lo)
            if (lo->second != rel) { p.caseOnly.push_back({rel, lo->second}); named.insert(lo->second); caseHit = true; }
        if (caseHit || D.files.count(rel)) continue;
        p.add.push_back(rel);
        const std::string m = idx.find(e, {}, p.error);   // any DST file, including ones SRC also names
        if (!p.error.empty()) return p;
        if (!m.empty()) p.duplicate.push_back({rel, m});
    }
    for (const auto& [rel, e] : D.files)
        if (!named.count(rel)) {
            p.kept.push_back(rel);
            if (srcTypes.count(type(rel))) p.replaceAllDeletes.push_back(rel);
        }
    std::sort(p.skipped.begin(), p.skipped.end());
    p.ok = true;
    return p;
}

inline std::string format(const MergePlan& p, const std::string& src, const std::string& dst) {
    if (!p.ok) return "error: " + p.error + "\n";
    std::ostringstream o;
    o << "File-level load plan (read-only; nothing is loaded; Sound/sample dependencies are NOT analysed)\n"
      << "SRC (to load): " << src << "\nDST (saved current User data): " << dst << "\n"
      << "  replace (same name, different content; the existing file is deleted, p.734-735): " << p.replace.size() << "\n"
      << "  name differs only in case (Pa5X behaviour UNKNOWN; treated as a possible replace): " << p.caseOnly.size() << "\n"
      << "  add (new name): " << p.add.size() << ", of which byte-identical to an existing DST file: " << p.duplicate.size() << "\n"
      << "  unchanged (same name, same bytes): " << p.unchanged.size() << "\n"
      << "  DST files SRC does not name: " << p.kept.size() << " (kept by Merge; " << p.replaceAllDeletes.size()
      << " deleted by Replace All of their type, p.734)\n";
    for (const auto& x : p.replace) o << "  REPLACE    " << x << "\n";
    for (const auto& [s, d] : p.caseOnly) o << "  CASE?      " << s << "  vs existing " << d << "\n";
    for (const auto& x : p.replaceAllDeletes) o << "  REPLACE-ALL deletes  " << x << "\n";
    for (const auto& x : p.add) o << "  add        " << x << "\n";
    for (const auto& [s, d] : p.duplicate) o << "  duplicate  " << s << " (same bytes as DST " << d << ")\n";
    for (const auto& x : p.skipped) o << "  skipped    " << x << " (not compared)\n";
    if (p.srcPcm) o << "WARNING: SRC has User Sample data (PCM folder). Loading the whole KST deletes ALL User Samples in memory\n"
                       "         (manual p.736). Load individual Sounds instead to keep existing samples.\n";
    o << "RESULT (Merge): " << (p.mergeSafe() ? "no existing User data replaced or deleted"
                                              : p.skipped.empty() ? "existing User data would be replaced or deleted"
                                                                  : "not verified (skipped entries)") << "\n"
      << "RESULT (Replace All): " << (p.replaceAllSafe() ? "no existing User data replaced or deleted"
                                                         : "existing User data would be replaced or deleted") << "\n";
    if (!p.replaceAllSafe()) o << "Back up first (manual p.7) and compare with kst-diff after the load.\n";
    return o.str();
}

}  // namespace ai_arranger::media

#endif  // AI_ARRANGER_MEDIA_KST_DIFF_H
