// E8 plan EXECUTION: carries out a validated, conflict-free Library Manager plan by writing a NEW library: a verified
// copy of the target library with the plan's file-level steps applied. Never in place; neither source is ever written.
//
// What can be executed (file level only; "file-level operation; not verified on a keyboard"):
//   - Pa5X KST (one item = one file; layout MAN5 p.731-732 + one OBSERVED listing): Copy, Move (= copy into the output;
//     the source is never changed), Rename (renames the file), Bank swap (swaps the TYPE/BANK folders).
//   - Pa-family SET: whole bank FILES (e.g. STYLE/USER03.STY) copied, swapped or renamed as files, between SETs of the
//     SAME detected generation (Step::wholeFile; a bank swap is always file level). Item-level edits INSIDE a Pa-native
//     container are REFUSED: the item payload and the 4-byte directory record trailer are UNKNOWN
//     (PA_NATIVE_CONTAINER_NOTES.md §2-§3), so no item can be written without guessing.
//   - SET <-> KST (cross-format) and an unknown layout: refused.
// The output library is the TARGET's copy: every step must change the same library (Copy/Move into it, Rename/Swap in it).
// Writing runs inside an E6 export job (jobs/export_specs.h libraryApply): NAME.INCOMPLETE, every source file read with
// O_NOFOLLOW and checked against the SHA-256 it had when the library was opened, steps applied with renames that never
// replace anything, NAME.manifest.sha256 beside the output, then verifyApplied() re-opens the staged output (manifest,
// the expected file list, the Library model, the structural validator) and re-checks both sources before publishing.
// Nothing is ever deleted.
#ifndef AI_ARRANGER_LIBRARY_LIBRARY_APPLY_H
#define AI_ARRANGER_LIBRARY_LIBRARY_APPLY_H

#include "library/library_plan.h"
#include "library/library_validate.h"
#include <dirent.h>

namespace ai_arranger::library {

inline constexpr const char* kFileLevelLabel = "file-level operation; not verified on a keyboard";

namespace detail {
// A relative, normalized path: no root, no empty, "." or ".." component, no backslash or control character.
inline bool plainRel(const std::string& r) {
    if (r.empty() || r[0] == '/') return false;
    for (unsigned char ch : r) if (ch < 0x20 || ch == 0x7F || ch == '\\') return false;
    for (size_t from = 0; from <= r.size();) {
        const size_t to = std::min(r.find('/', from), r.size());
        const std::string c = r.substr(from, to - from);
        if (c.empty() || c == "." || c == "..") return false;
        from = to + 1;
    }
    return true;
}
}  // namespace detail

struct Executable {
    char target = 0;                       // 'A' or 'B': the library the output is a copy of
    std::string refusal;                   // "" = the plan can be executed
    std::vector<std::string> stepRefusals; // per step ("" = fine), same order as the plan
};

// Can this plan be executed (file level), and into a copy of which library? Read-only.
inline Executable executable(const Library& A, const Library& B, const std::vector<Step>& plan, const Evaluation& ev) {
    Executable x;
    x.stepRefusals.assign(plan.size(), "");
    if (!A.ok || !B.ok) { x.refusal = "a plan needs a library open on both sides"; return x; }
    if (plan.empty()) { x.refusal = "the plan has no steps"; return x; }
    if (ev.steps.size() != plan.size()) { x.refusal = "the plan was not fully checked"; return x; }
    if (ev.blocked()) {
        for (size_t i = 0; i < ev.steps.size(); ++i)
            if (!ev.steps[i].conflicts.empty()) { x.refusal = "the plan has " + std::to_string(ev.conflicts) + " conflict(s), first at step " + std::to_string(i + 1) + ": " + ev.steps[i].conflicts.front(); break; }
        return x;
    }
    for (const auto& st : plan) {
        const char t = st.op == Op::Copy || st.op == Op::Move ? (st.side == 'A' ? 'B' : 'A') : st.side;
        if (x.target && t != x.target) {
            x.target = 0;
            x.refusal = "the plan changes both libraries; one execution writes one new library (a copy of the target): split the plan";
            return x;
        }
        x.target = t;
    }
    const Library& T = x.target == 'A' ? A : B;
    if (T.format == Format::Unknown) { x.refusal = "the target's layout is unknown (no KST or SET markers): no documented or observed layout to keep"; return x; }
    if (!T.skipped.empty()) { x.refusal = "the target holds " + std::to_string(T.skipped.size()) + " link(s) or special file(s), which are never followed: the copy would not be complete"; return x; }
    for (size_t i = 0; i < plan.size(); ++i) {
        const auto& st = plan[i];
        if (T.format == Format::PaSet && st.op != Op::BankSwap && !st.wholeFile)
            x.stepRefusals[i] = "item-level edit inside a Pa-native container: refused (the item payload and the 4-byte directory record trailer "
                                "are UNKNOWN); use a whole bank file step instead";
    }
    for (size_t i = 0; i < plan.size(); ++i)   // every path an op writes must stay inside the staging root (and every source path inside S)
        for (const auto& op : ev.steps[i].ops)
            for (const auto* rel : {&op.from, &op.to})
                if (!detail::plainRel(*rel)) x.stepRefusals[i] = "the path \"" + *rel + "\" is not a plain relative path inside the library: refused";
    for (size_t i = 0; i < plan.size(); ++i)
        if (!x.stepRefusals[i].empty()) { x.refusal = "step " + std::to_string(i + 1) + ": " + x.stepRefusals[i]; break; }
    return x;
}

// Where a target path ends up after the steps' renames and swaps (a copied-in file is not a target path).
inline std::string mapPath(std::string rel, const Evaluation& ev) {
    for (const auto& sc : ev.steps)
        for (const auto& op : sc.ops) {
            const auto under = [&](const std::string& p) { return rel == p || (op.folder && rel.rfind(p + "/", 0) == 0); };
            if (op.kind == FileOp::Rename && rel == op.from) rel = op.to;
            else if (op.kind == FileOp::Swap && under(op.from)) rel = op.to + rel.substr(op.from.size());
            else if (op.kind == FileOp::Swap && under(op.to)) rel = op.from + rel.substr(op.to.size());
        }
    return rel;
}

// The file list (relative path -> SHA-256) the output must have: the target's files moved by the renames and swaps, plus
// every copied-in file (a later rename or swap moves a copied file too).
inline std::map<std::string, std::string> expectedFiles(const Library& T, const Evaluation& ev) {
    std::map<std::string, std::string> m;
    for (const auto& [rel, f] : T.files) m[rel] = f.sha256;
    for (const auto& sc : ev.steps)
        for (const auto& op : sc.ops) {
            if (op.kind == FileOp::CopyIn) { m[op.to] = op.sha; continue; }
            std::map<std::string, std::string> n;
            Evaluation one;
            one.steps.push_back({"", {}, {}, {op}});
            for (const auto& [rel, h] : m) n[mapPath(rel, one)] = h;
            m = std::move(n);
        }
    return m;
}

struct ApplyResult { std::string error, text; };

namespace detail {
using media::detail::Fd;
// Opens folder `rel` below the open folder `root` one component at a time (O_NOFOLLOW | O_DIRECTORY: a component that is
// a symlink, or was swapped for one, fails instead of being followed), creating missing components with mkdirat when
// `create`. "" = root itself. -1 on failure or for a path that is not plainRel. Codex E8 r1: writes never resolve a
// parent folder by pathname.
inline int openDirIn(int root, const std::string& rel, bool create) {
    if (rel.empty()) return ::dup(root);
    if (!plainRel(rel)) return -1;
    int cur = ::dup(root);
    for (size_t from = 0; cur >= 0 && from <= rel.size();) {
        const size_t to = std::min(rel.find('/', from), rel.size());
        const std::string c = rel.substr(from, to - from);
        from = to + 1;
        if (create) ::mkdirat(cur, c.c_str(), 0755);   // EEXIST is fine: the open below decides
        const int next = ::openat(cur, c.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(cur);
        cur = next;
    }
    return cur;
}
inline std::pair<std::string, std::string> split(const std::string& rel) { return media::detail::splitRel(rel); }
// True when `rel` names an entry (any kind, not followed) below `root`.
inline bool existsIn(int root, const std::string& rel) {
    const auto [dir, leaf] = split(rel);
    Fd d(openDirIn(root, dir, false));
    struct stat st {};
    return d.v >= 0 && ::fstatat(d.v, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
}
// Rename below `root` that never replaces anything; both parents opened descriptor by descriptor (to's created).
inline bool renameIn(int root, const std::string& from, const std::string& to) {
    const auto [fd, fl] = split(from);
    const auto [td, tl] = split(to);
    Fd a(openDirIn(root, fd, false)), b(openDirIn(root, td, true));
    if (a.v < 0 || b.v < 0) return false;
#ifdef __APPLE__
    return ::renameatx_np(a.v, fl.c_str(), b.v, tl.c_str(), RENAME_EXCL) == 0;
#else
    return ::renameat2(a.v, fl.c_str(), b.v, tl.c_str(), RENAME_NOREPLACE) == 0;
#endif
}
// Copies `from` below the open source root `src` (every folder opened by descriptor with O_NOFOLLOW, the file itself a
// regular file: a folder swapped for a link fails instead of being read through, Codex E8 r2) to the NEW file `to`
// below `root` (parents opened by descriptor, O_EXCL | O_NOFOLLOW: nothing is replaced or followed), hashing as it
// reads; the bytes must have the SHA-256 the library had when it was opened. "" = copied and matching.
inline std::string copyChecked(int src, const std::string& srcName, const std::string& from, int root, const std::string& to, const std::string& sha,
                               const std::function<bool(size_t)>& more) {
    const auto [dir, leaf] = split(to);
    Fd d(openDirIn(root, dir, true));
    if (d.v < 0) return "cannot open the staged folder for " + to + " (missing, or not a plain folder: links are never followed)";
    const auto [sdir, sleaf] = split(from);
    Fd s(openDirIn(src, sdir, false));
    std::FILE* in = s.v >= 0 ? media::detail::openRegular(s.v, sleaf.c_str()) : nullptr;
    if (!in) return "cannot read " + srcName + "/" + from + " (missing, not a regular file, or a folder on its path is a link: links are never followed)";
    const int fd = ::openat(d.v, leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    std::FILE* out = fd >= 0 ? ::fdopen(fd, "wb") : nullptr;
    if (!out) { if (fd >= 0) ::close(fd); std::fclose(in); return "cannot create " + to + " (it may already exist)"; }
    bool cancelled = false;
    std::string err;
    const auto got = media::detail::sha256Stream(in, out, more, &cancelled, &err, from);
    std::fclose(in);
    const bool closed = std::fclose(out) == 0;
    if (cancelled) return "cancelled";
    if (got.empty()) return err;
    if (!closed) return "cannot write " + to + " (disk full?)";
    if (got != sha) return srcName + "/" + from + " changed since its library was opened (SHA-256 differs): open both libraries again";
    return "";
}
inline std::function<void(const std::string& rel)> hashWalkHook;   // tests only: runs after each file hashTree() hashes
// SHA-256 of every file below the open folder `dir`, walked by descriptor only (openat O_NOFOLLOW per component): rel ->
// hash, folders as "rel/" -> "". Any link or special file is an error, never followed. Bounded by kLibraryLimits.
inline std::string hashTree(int dir, const std::string& prefix, std::map<std::string, std::string>& m, int depth = 0) {
    if (depth > kLibraryLimits.depth) return "folder deeper than " + std::to_string(kLibraryLimits.depth) + " at " + prefix;
    const int d2 = ::dup(dir);
    DIR* dp = d2 >= 0 ? ::fdopendir(d2) : nullptr;
    if (!dp) { if (d2 >= 0) ::close(d2); return "cannot list " + (prefix.empty() ? std::string(".") : prefix); }
    ::rewinddir(dp);
    std::vector<std::string> names;
    while (const dirent* e = ::readdir(dp)) if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) names.push_back(e->d_name);
    ::closedir(dp);
    for (const auto& n : names) {
        const std::string rel = prefix.empty() ? n : prefix + "/" + n;
        if (m.size() >= kLibraryLimits.entries) return "more than " + std::to_string(kLibraryLimits.entries) + " entries";
        struct stat st {};
        if (::fstatat(dir, n.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return "cannot stat " + rel;
        if (S_ISDIR(st.st_mode)) {
            const Fd sub(::openat(dir, n.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (sub.v < 0) return "cannot open " + rel + " (links are never followed)";
            m[rel + "/"] = "";
            if (auto e = hashTree(sub.v, rel, m, depth + 1); !e.empty()) return e;
        } else if (S_ISREG(st.st_mode)) {
            std::FILE* in = media::detail::openRegular(dir, n.c_str());
            if (!in) return "cannot read " + rel + " (no longer a regular file)";
            bool c = false;
            std::string err;
            m[rel] = media::detail::sha256Stream(in, nullptr, {}, &c, &err, rel);
            std::fclose(in);
            if (m[rel].empty()) return err;
            if (hashWalkHook) hashWalkHook(rel);
        } else return rel + " is a link or special file (never followed)";
    }
    return "";
}
}  // namespace detail

// The staged output as verifyApplied() saw it: its folder and the folder holding it stay OPEN (descriptors, not names)
// and every entry's SHA-256 was taken through them. recheckPinned() repeats that walk right before the publishing rename.
struct Pinned {
    int parent = -1, dir = -1;
    dev_t dev = 0;
    ino_t ino = 0;
    std::map<std::string, std::string> tree;   // rel -> SHA-256 ("rel/" -> "" for a folder)
    std::string manifest, error;               // SHA-256 of NAME.manifest.sha256; error = could not be pinned
    Pinned() = default;
    Pinned(const Pinned&) = delete;
    Pinned& operator=(const Pinned&) = delete;
    ~Pinned() { for (int f : {parent, dir}) if (f >= 0) ::close(f); }
};
namespace detail {
inline std::string manifestSha(int parent, const std::string& leaf) {
    std::FILE* in = media::detail::openRegular(parent, (leaf + ".manifest.sha256").c_str());
    if (!in) return "";
    bool c = false;
    std::string err;
    auto h = media::detail::sha256Stream(in, nullptr, {}, &c, &err, leaf);
    std::fclose(in);
    return h;
}
inline void pin(const fs::path& out, Pinned& p) {
    const std::string leaf = out.filename().string();
    p.parent = ::open(out.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    p.dir = p.parent >= 0 ? ::openat(p.parent, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    struct stat st {};
    if (p.dir < 0 || ::fstat(p.dir, &st) != 0) { p.error = "cannot open the staged output " + out.string() + " (links are never followed)"; return; }
    p.dev = st.st_dev;
    p.ino = st.st_ino;
    p.error = hashTree(p.dir, "", p.tree);
    p.manifest = manifestSha(p.parent, leaf);
    if (p.error.empty() && p.manifest.empty()) p.error = "cannot read " + leaf + ".manifest.sha256";
}
}  // namespace detail

// Right before the publishing rename: `staged` must still be the pinned folder (same device and inode, reached from the
// pinned parent) holding exactly the pinned entries with the pinned SHA-256s, and the manifest beside it unchanged.
// "" = publish; otherwise why not (Codex E8 r2: verified bytes cannot change between verify and publish unnoticed).
inline std::string recheckPinned(const Pinned& p, const fs::path& staged) {
    if (p.dir < 0 || !p.error.empty()) return "the staged output was not verified (" + p.error + ")";
    const std::string leaf = staged.filename().string();
    const auto same = [&] {   // the staged PATHNAME still names the pinned folder, reached from the pinned parent
        struct stat a {}, b {};
        const detail::Fd par(::open(staged.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        return par.v >= 0 && ::fstat(par.v, &a) == 0 && ::fstat(p.parent, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
               ::fstatat(p.parent, leaf.c_str(), &a, AT_SYMLINK_NOFOLLOW) == 0 && a.st_dev == p.dev && a.st_ino == p.ino;
    };
    if (!same()) return "the staged output folder was replaced after it was verified";
    std::map<std::string, std::string> now;
    if (const auto e = detail::hashTree(p.dir, "", now); !e.empty()) return "the staged output changed after it was verified: " + e;
    for (const auto& [rel, h] : p.tree) {
        const auto f = now.find(rel);
        if (f == now.end()) return "the staged output changed after it was verified: " + rel + " is gone";
        if (f->second != h) return "the staged output changed after it was verified: other bytes at " + rel;
    }
    for (const auto& [rel, h] : now) if (!p.tree.count(rel)) return "the staged output changed after it was verified: unexpected " + rel;
    if (detail::manifestSha(p.parent, leaf) != p.manifest) return "the manifest beside the staged output changed after it was verified";
    // last: the walk above read the pinned folder; the job renames by NAME, so that name must still be this folder (Codex E8 r3)
    if (!same()) return "the staged output folder was replaced while it was being re-checked";
    return "";
}

// Writes the new library at `out` (which must not exist; the export job's staged path) and `out`.manifest.sha256 beside
// it. `progress` (0..1) returns false to cancel. Reads only; writes only below out's parent folder.
inline ApplyResult applyPlan(const Library& A, const Library& B, const std::vector<Step>& plan, const fs::path& out,
                             const std::function<bool(double)>& progress = {}) {
    const auto ev = evaluate(A, B, plan);
    const auto x = executable(A, B, plan, ev);
    if (!x.refusal.empty()) return {"refusing: " + x.refusal, ""};
    const Library& T = x.target == 'A' ? A : B;
    const Library& S = x.target == 'A' ? B : A;
    // the target as it is now must be the target that was opened: same files and sizes (the bytes are checked while copied)
    media::detail::Listing ls;
    std::string err;
    if (!media::detail::listFiles(T.root, ls, err, {}, kLibraryLimits)) return {"cannot list the target " + T.root + ": " + err, ""};
    if (!ls.skipped.empty()) return {"refusing: the target holds links or special files now (" + ls.skipped.front() + ")", ""};
    bool same = ls.files.size() == T.files.size();
    for (const auto& [rel, e] : ls.files) { const auto f = T.files.find(rel); same = same && f != T.files.end() && f->second.size == e.size; }
    if (!same) return {"library " + std::string(1, x.target) + " changed since it was opened: open both libraries again", ""};

    uintmax_t total = 1, done = 0;
    for (const auto& [rel, f] : T.files) total += f.size;
    for (const auto& sc : ev.steps) for (const auto& op : sc.ops) if (op.kind == FileOp::CopyIn) total += S.files.count(op.from) ? S.files.at(op.from).size : 0;
    const std::function<bool(size_t)> more = [&](size_t n) { done += n; return !progress || progress(double(done) / double(total)); };

    // both source roots are opened once; every source file is read through them, never through a pathname (Codex E8 r2)
    const detail::Fd tr(::open(T.root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)), sr(::open(S.root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (tr.v < 0 || sr.v < 0) return {"cannot open " + (tr.v < 0 ? T.root : S.root) + " (missing, or a link: links are never followed)", ""};
    std::error_code ec;
    if (!fs::create_directory(out, ec)) return {"cannot create " + out.string() + (ec ? ": " + ec.message() : ""), ""};
    // the staging root is opened once; every write below goes through descriptors from it, never through a pathname
    const detail::Fd root(::open(out.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (root.v < 0) return {"cannot open " + out.string(), ""};
    std::sort(ls.dirs.begin(), ls.dirs.end());   // parents before children
    for (const auto& d : ls.dirs) {
        const detail::Fd f(detail::openDirIn(root.v, d, true));
        if (f.v < 0) return {"cannot create " + (out / d).string() + " (or it is not a plain folder)", ""};
    }
    for (const auto& [rel, e] : ls.files)
        if (const auto e2 = detail::copyChecked(tr.v, T.root, rel, root.v, rel, T.files.at(rel).sha256, more); !e2.empty()) return {e2 == "cancelled" ? "cancelled" : e2, ""};

    std::ostringstream o;
    o << "Executed plan into a NEW library (" << kFileLevelLabel << ")\nTarget (copied, only read): " << std::string(1, x.target) << ": " << T.root
      << "\nOther source (only read): " << S.root << "\n";
    for (size_t i = 0; i < ev.steps.size(); ++i) {
        const auto& sc = ev.steps[i];
        o << i + 1 << ". " << sc.summary << ": ";
        if (sc.ops.empty()) o << "nothing to do (" << (sc.warnings.empty() ? std::string("no file change") : sc.warnings.front()) << ")\n";
        for (size_t k = 0; k < sc.ops.size(); ++k) {
            const auto& op = sc.ops[k];
            if (progress && !progress(double(done) / double(total))) return {"cancelled", ""};
            std::string why;
            if (!detail::plainRel(op.from) || !detail::plainRel(op.to)) why = "a path is not a plain relative path inside the library";   // executable() refused it already
            else if (op.kind == FileOp::CopyIn) {
                why = detail::copyChecked(sr.v, S.root, op.from, root.v, op.to, op.sha, more);
                if (why.empty()) o << (k ? "; " : "") << "copied " << op.from << " -> " << op.to;
            } else if (op.kind == FileOp::Rename) {
                if (!detail::renameIn(root.v, op.from, op.to)) why = "cannot rename " + op.from + " to " + op.to + " (missing, or the new name exists)";
                else o << (k ? "; " : "") << "renamed " << op.from << " -> " << op.to;
            } else {
                const std::string tmp = op.from + ".e8-swap";
                const bool ha = detail::existsIn(root.v, op.from), hb = detail::existsIn(root.v, op.to);
                if (ha && hb) {
                    if (!detail::renameIn(root.v, op.from, tmp) || !detail::renameIn(root.v, op.to, op.from) || !detail::renameIn(root.v, tmp, op.to))
                        why = "cannot swap " + op.from + " and " + op.to;
                } else if (ha) { if (!detail::renameIn(root.v, op.from, op.to)) why = "cannot rename " + op.from + " to " + op.to; }
                else if (hb) { if (!detail::renameIn(root.v, op.to, op.from)) why = "cannot rename " + op.to + " to " + op.from; }
                else why = "neither " + op.from + " nor " + op.to + " exists";
                if (why.empty()) o << (k ? "; " : "") << "swapped " << op.from << " <-> " << op.to << (op.folder ? " (bank folders)" : " (bank files)");
            }
            if (!why.empty()) return {why == "cancelled" ? "cancelled" : "step " + std::to_string(i + 1) + " failed: " + why, ""};
        }
        if (!sc.ops.empty()) o << ": done" << (plan[i].op == Op::Move ? " (a move is a copy here: the source keeps it, nothing is deleted)" : "") << "\n";
    }
    // manifest beside the output (inside it, a top-level file would break the layout: validator R-06)
    std::map<std::string, std::string> w;
    if (const auto e = detail::hashTree(root.v, "", w); !e.empty()) return {"cannot read back the output: " + e, ""};
    std::string man;
    size_t files = 0;
    for (const auto& [rel, h] : w) if (rel.back() != '/') { man += h + "  " + rel + "\n"; ++files; }
    if (const auto e = media::writeNewFile(media::backupManifestPath(out), {man.begin(), man.end()}); !e.empty()) return {"manifest not written: " + e, ""};
    o << files << " files, manifest " << media::backupManifestPath(out).filename().string() << " beside the library.\n";
    return {"", o.str()};
}

// Checks the STAGED output before it is published: L1 = its manifest, the exact expected file list, the re-opened
// Library model (same format), the structural validator (no ERROR the target did not already have) and both sources
// unchanged since they were opened. The text lists every check.
// `keep` receives the pinned staged output for recheckPinned() (the job runs it right before publishing).
inline std::pair<bool, std::string> verifyApplied(const Library& A, const Library& B, const std::vector<Step>& plan, const fs::path& out, Pinned* keep = nullptr) {
    const auto ev = evaluate(A, B, plan);
    const auto x = executable(A, B, plan, ev);
    if (!x.refusal.empty()) return {false, "refusing: " + x.refusal};
    const Library& T = x.target == 'A' ? A : B;
    std::ostringstream o;
    bool ok = true;
    const auto check = [&](bool pass, const std::string& what) { o << (pass ? "  PASS  " : "  FAIL  ") << what << "\n"; ok = ok && pass; };
    size_t n = 0;
    Pinned local;
    Pinned& p = keep ? *keep : local;
    detail::pin(out, p);   // first: the expected-file check below uses the bytes read through the pinned folder
    check(p.error.empty(), "staged output pinned (folder kept open, every file hashed through it)" + (p.error.empty() ? "" : ": " + p.error));
    const auto bad = media::verifyBackup(out, {}, &n);
    check(bad.empty(), "manifest: all " + std::to_string(n) + " files match NAME.manifest.sha256" + (bad.empty() ? "" : " (" + bad.substr(0, bad.find('\n')) + ")"));
    const auto R = load(out);
    std::map<std::string, std::string> have;
    for (const auto& [rel, h] : p.tree) if (rel.back() != '/') have[rel] = h;
    const auto want = expectedFiles(T, ev);
    std::string diff;
    for (const auto& [rel, h] : want) if (!have.count(rel)) diff = "missing " + rel; else if (have[rel] != h) diff = "other bytes at " + rel;
    for (const auto& [rel, h] : have) if (!want.count(rel)) diff = "unexpected " + rel;
    check(R.ok && R.format == T.format, std::string("re-opened with the Library model: ") + (R.ok ? formatName(R.format) + std::string(", ") + std::to_string(R.items.size()) + " items" : R.error));
    check(R.ok && diff.empty(), "plan re-check: every step's result is in the output (" + std::to_string(want.size()) + " files as expected)" + (diff.empty() ? "" : ": " + diff));
    const auto vt = validate(T.root), vo = validate(out);
    std::set<std::pair<std::string, std::string>> before;
    for (const auto& f : vt.findings) if (f.severity == Severity::Error) before.insert({f.rule, mapPath(f.path, ev)});   // where the step moved it
    std::vector<std::string> added;
    for (const auto& f : vo.findings) if (f.severity == Severity::Error && !before.count({f.rule, f.path})) added.push_back(f.rule + " " + f.path + ": " + f.message);
    std::string fold;   // R-18 is never published, even when the target already had it: one name on the stick
    for (const auto& f : vo.findings) if (f.rule == "R-18") fold = f.path + ": " + f.message;
    check(vo.ok && fold.empty(), "no names that differ only in letter case (R-18)" + (fold.empty() ? "" : ": " + fold));
    check(vo.ok && added.empty(), "structural validator: " + (vo.ok ? std::to_string(vo.count(Severity::Error)) + " error(s) (target had " + std::to_string(vt.count(Severity::Error)) +
                                                                       "), " + std::to_string(vo.count(Severity::Warn)) + " warning(s)" : vo.error) +
                                      (added.empty() ? "" : "; NEW: " + added.front()));
    for (const Library* L : {&A, &B}) {   // snapshot compare: nothing in either source changed while the job ran
        const auto now = load(L->root);
        bool same = now.ok && now.files.size() == L->files.size();
        for (const auto& [rel, f] : L->files) { const auto g = now.files.find(rel); same = same && g != now.files.end() && g->second.sha256 == f.sha256 && g->second.size == f.size; }
        check(same, "source unchanged since it was opened (" + std::to_string(L->files.size()) + " files, SHA-256): " + L->root);
    }
    o << "\n" << validationText(vo) << "Label: " << kFileLevelLabel << ".\n";
    return {ok, o.str()};
}

// "<target name> edited <time>" keeping a .KST / .SET suffix (validator R-01).
inline std::string outputName(const Library& T, const std::string& stamp) {
    const std::string n = T.name, up = media::detail::upper(n);
    for (const char* sfx : {".KST", ".SET"})
        if (up.size() > 4 && up.compare(up.size() - 4, 4, sfx) == 0) return n.substr(0, n.size() - 4) + " edited " + stamp + n.substr(n.size() - 4);
    return n + " edited " + stamp;
}

}  // namespace ai_arranger::library

#endif
