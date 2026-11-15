// SET -> KST conversion, phase 1 (D54 B1): resources only. STRUCTURALLY VALID - NOT VERIFIED ON A PA5X.
//
// Every rule, its evidence class, counts and limits: docs/research/pa-studio/PA5X_SET_TO_KST_EVIDENCE.md (rules R1-R8),
// measured on the Pa5X's own conversion of one owner Pa700 SET; tests/style_edit/test_set_to_kst.cpp has the private
// oracle run on that pair:
//   CARRY    PCM/*.PCM, SOUND/*.PCG, MULTISMP/*.KMP, SONGBOOK/*.SBL: byte-identical, same type folder, file name lower-cased
//            (R1, R2).
//   BLOCKED  STYLE/*.STY, KEYBOARDSET/*.PRF, PAD/*.PAD, SONGBOOK/SONGDB.SBD/*.SBD: the Pa5X re-writes them, and their
//            item payloads are OC31-encoded, which is not decoded (R3). Nothing is written for them: no
//            partial or guessed .stg/.ksc/.pdc/.SBD.
//   DROPPED  GLOBAL/, VOICEPRESET/, GUITARPRESET/: not carried over, consistent with MAN5 p.737 (R4).
//   UNKNOWN  anything else (not in the measured table): not carried, listed.
// Matching is exact on the observed upper-case names; nothing else is inferred.
//
// Dependency map (read-only, on the carried files):
//   PCM sample = directory record of a PCM file; name = field 2 (R6), sample ID = field 4, 8 bytes (R6; a Pa5X field,
//   absent from Pa700 PCM records).
//   KMP -> PCM = the KMP item's sample table, read only in the one documented layout (R7: raw item 11080018, u16 0x0103,
//   u32 count at +2, 122-byte records from +85, sample ID at record +40). Any other KMP item (OC31-encoded as in the
//   Pa700 RAM.KMP, another version, a table past the item) is UNKNOWN: then no sample is called unreferenced or dangling.
//
// Reads (Codex r1 C1): the SET root is opened once (O_NOFOLLOW) and every member is read below it by descriptor, each
// folder opened O_NOFOLLOW (library::detail::openDirIn, E8) and the file a regular file (media::detail::openRegular), so
// a folder swapped for a link fails instead of being read through. A SET holding any link or special file is refused.
//
// One snapshot (Codex r2): each CARRY member is read exactly ONCE, at validation, through the pinned descriptors, and
// kept in memory with its SHA-256 and its identity (device, inode, size, modification time). The KORF check, the
// dependency map, the CONVERSION report and the staged write all use those bytes; nothing re-reads a source for
// content. The total is bounded (kMaxSnapshotBytes); a larger SET is refused with that reason. The publish gate
// (jobs::setToKst beforePublish) then checks, in this order: (1) the SET pathname still names the pinned root and every
// member still has its snapshot identity; (2) LAST, right before the rename: every staged file re-hashed through the
// pinned staged folder and its device/inode equal to the one recorded when it was written (library::recheckPinned).
// Residual boundary: after the publish rename a concurrent process of the same user can still change files; the
// report states what was verified at publish time. See PA5X_SET_TO_KST_EVIDENCE.md, "Data flow".
#ifndef AI_ARRANGER_MEDIA_PANATIVE_SET_TO_KST_H
#define AI_ARRANGER_MEDIA_PANATIVE_SET_TO_KST_H

#include "library/library_apply.h"   // detail::openDirIn: the E8 descriptor-based source reads
#include "media/panative/kst_assemble.h"
#include "media/sha256.h"
#include <atomic>
#include <map>

namespace ai_arranger::media::panative::settokst {

inline constexpr const char* kLabel = "STRUCTURALLY VALID - NOT VERIFIED ON A PA5X";

enum class Kind { Carry, Blocked, Dropped, Unknown };
inline const char* kindName(Kind k) { return k == Kind::Carry ? "CARRY" : k == Kind::Blocked ? "BLOCKED" : k == Kind::Dropped ? "DROPPED" : "UNKNOWN"; }

struct Member {
    std::string rel;        // inside the SET
    Kind kind = Kind::Unknown;
    std::string dest;       // CARRY: the path inside the KST
    std::string reason;
    uint64_t bytes = 0;
};

struct Plan {
    bool ok = false;
    std::string error;
    std::vector<Member> members;   // sorted by rel
    std::shared_ptr<const media::detail::Fd> root;   // the SET folder, opened once (O_NOFOLLOW); every read goes below it
    std::vector<std::string> links;                  // links and special files found (the plan is refused)
    size_t count(Kind k) const { return size_t(std::count_if(members.begin(), members.end(), [k](const Member& m) { return m.kind == k; })); }
};

namespace detail {
inline std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
inline bool endsWith(const std::string& s, const std::string& e) { return s.size() > e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }

inline void classify(Member& m) {
    const auto slash = m.rel.rfind('/');
    const std::string dir = slash == std::string::npos ? "" : m.rel.substr(0, slash), leaf = m.rel.substr(slash + 1);
    const std::string top = m.rel.substr(0, m.rel.find('/'));
    static const struct { const char *dir, *ext; } carry[] = {{"PCM", ".PCM"}, {"SOUND", ".PCG"}, {"MULTISMP", ".KMP"}, {"SONGBOOK", ".SBL"}};
    static const struct { const char *dir, *ext, *target; } blocked[] = {
        {"STYLE", ".STY", "Root-tree .stg files"}, {"KEYBOARDSET", ".PRF", "Root-tree .ksc files"},
        {"PAD", ".PAD", "Root-tree .pdc files"}, {"SONGBOOK/SONGDB.SBD", ".SBD", "new KORF .SBD bytes"}};
    for (const auto& c : carry)
        if (dir == c.dir && endsWith(leaf, c.ext)) {
            m.kind = Kind::Carry;
            m.dest = dir + "/" + lower(leaf);
            m.reason = "byte-identical, name lower-cased (measured Pa5X conversion)";
            return;
        }
    for (const auto& b : blocked)
        if (dir == b.dir && endsWith(leaf, b.ext)) {
            m.kind = Kind::Blocked;
            m.reason = std::string("the Pa5X re-writes it as ") + b.target + "; its item payloads are OC31-encoded, not decoded (phase 2)";
            return;
        }
    if (top == "GLOBAL" || top == "VOICEPRESET" || top == "GUITARPRESET") {
        m.kind = Kind::Dropped;
        m.reason = "not carried into a KST by the Pa5X conversion (MAN5 p.737)";
        return;
    }
    m.kind = Kind::Unknown;
    m.reason = "not in the measured Pa5X conversion table: not carried";
}
}  // namespace detail

// Every entry of the SET folder, classified. Read-only; links and special files are listed as UNKNOWN, never followed.
inline Plan inventory(const std::filesystem::path& set) {
    Plan p;
    p.root = std::make_shared<const media::detail::Fd>(::open(set.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat pinned {}, now {};
    if (p.root->v < 0 || ::fstat(p.root->v, &pinned) != 0) { p.error = "cannot open " + set.string() + " (a folder, not a link)"; return p; }
    media::detail::Listing ls;
    if (!media::detail::listFiles(set, ls, p.error)) return p;
    // the listing went by name: it must have listed the pinned folder
    if (::lstat(set.c_str(), &now) != 0 || now.st_dev != pinned.st_dev || now.st_ino != pinned.st_ino) { p.error = set.string() + " was replaced while it was listed"; return p; }
    for (const auto& [rel, e] : ls.files) {
        Member m{rel};
        m.bytes = e.size;
        detail::classify(m);
        p.members.push_back(std::move(m));
    }
    for (const auto& rel : ls.skipped) {
        p.members.push_back({rel, Kind::Unknown, "", "a link or special file: not followed, not carried"});
        p.links.push_back(rel);
    }
    std::sort(p.members.begin(), p.members.end(), [](const Member& a, const Member& b) { return a.rel < b.rel; });
    p.ok = true;
    return p;
}

// Tests only: how many member reads of content happened (each CARRY member is read exactly once), and a hook that runs
// after each member's identity check at the publish gate (to time a swap inside the gate).
inline std::atomic<size_t> contentReads{0};
inline std::function<void(const std::string& rel)> sourceCheckHook;

// A member's identity: compared at the publish gate without reading the content again.
struct Identity {
    dev_t dev = 0;
    ino_t ino = 0;
    off_t size = 0;
    long mtimeSec = 0, mtimeNsec = 0;
    bool operator==(const Identity& o) const { return dev == o.dev && ino == o.ino && size == o.size && mtimeSec == o.mtimeSec && mtimeNsec == o.mtimeNsec; }
};
inline Identity identityOf(const struct stat& st) {
#ifdef __APPLE__
    return {st.st_dev, st.st_ino, st.st_size, long(st.st_mtimespec.tv_sec), long(st.st_mtimespec.tv_nsec)};
#else
    return {st.st_dev, st.st_ino, st.st_size, long(st.st_mtim.tv_sec), long(st.st_mtim.tv_nsec)};
#endif
}
struct Snap { std::string bytes, sha; Identity id; };
// Bytes held by every live snapshot, process-wide (observable: a finished job must hold none, Codex r3 E3).
inline std::atomic<uint64_t> retainedSnapshotBytes{0};
struct Snapshot {
    std::map<std::string, Snap> files;   // CARRY member rel -> its bytes as read once (hash and identity stay after release)
    uint64_t total = 0, held = 0;        // total read; held = bytes still in memory
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    ~Snapshot() { releaseBytes(); }
    void add(const std::string& rel, Snap s) { held += s.bytes.size(); retainedSnapshotBytes += s.bytes.size(); files[rel] = std::move(s); }
    void releaseBytes() { for (auto& [rel, f] : files) std::string().swap(f.bytes); retainedSnapshotBytes -= held; held = 0; }
    void reset() { releaseBytes(); files.clear(); total = 0; }
};
inline constexpr uint64_t kMaxSnapshotBytes = uint64_t(2) << 30;   // shortcut: phase 1 holds the carried bytes in memory; stream them before larger SETs

// Member `rel` below the open SET root, read by descriptor (no component followed), at most kMaxFileBytes; *id = the
// identity of the descriptor that was read.
inline bool readIn(int root, const std::string& rel, std::string& bytes, std::string& err, Identity* id = nullptr) {
    ++contentReads;
    const auto [dir, leaf] = media::detail::splitRel(rel);
    const media::detail::Fd d(library::detail::openDirIn(root, dir, false));
    std::FILE* in = d.v >= 0 ? media::detail::openRegular(d.v, leaf.c_str()) : nullptr;
    if (!in) { err = "cannot read " + rel + " (missing, not a regular file, or a folder on its path is a link: links are never followed)"; return false; }
    struct stat st {};
    const bool sized = ::fstat(::fileno(in), &st) == 0 && uint64_t(st.st_size) <= panative::kMaxFileBytes;
    if (id) *id = identityOf(st);
    bytes.assign(sized ? size_t(st.st_size) : 0, '\0');
    struct stat after {};
    const bool ok = sized && std::fread(bytes.data(), 1, bytes.size(), in) == bytes.size() && std::fgetc(in) == EOF &&
                    ::fstat(::fileno(in), &after) == 0 && identityOf(after) == identityOf(st);   // not written to while read
    std::fclose(in);
    if (!ok) err = sized ? "cannot read " + rel : rel + " is larger than the reader's limit";
    return ok;
}
inline std::string sha(const std::string& b) { media::Sha256 h; h.update(b.data(), b.size()); return h.hex(); }

// Why the plan cannot be written, or "". The same refusals as the W5 assembler (assemble::build) for carried files:
// a valid KORF container, a safe TYPE/FILE destination, no two destinations equal without case.
// `snap` receives every CARRY member, read once (the only content read of the conversion).
inline std::string refusal(const Plan& p, Snapshot& snap) {
    snap.reset();
    if (!p.ok) return "cannot list the SET: " + p.error;
    if (!p.links.empty()) return "refusing: " + p.links.front() + " is a link or special file (links are never followed)";
    if (!p.count(Kind::Carry)) return "refusing: the SET holds no resource that the Pa5X carries unchanged (PCM, PCG, KMP, SBL)";
    std::set<std::string> seen;
    for (const auto& m : p.members) {
        if (m.kind != Kind::Carry) continue;
        const auto slash = m.dest.find('/');
        if (!assemble::safeName(m.dest.substr(0, slash)) || !assemble::safeName(m.dest.substr(slash + 1))) return "refusing: unsafe file name " + m.rel;
        if (!seen.insert(closure::upper(m.dest)).second) return "refusing: two members map to " + m.dest + " (names differ only in case?)";
    }
    uint64_t listed = 0;
    for (const auto& m : p.members) if (m.kind == Kind::Carry) listed += m.bytes;
    if (listed > kMaxSnapshotBytes)
        return "refusing: the SET's carried resources are " + std::to_string(listed) + " bytes, more than the " + std::to_string(kMaxSnapshotBytes) +
               " this phase-1 converter reads into memory";
    for (const auto& m : p.members) {
        if (m.kind != Kind::Carry) continue;
        Snap s;
        std::string err;
        if (!p.root || !readIn(p.root->v, m.rel, s.bytes, err, &s.id)) return "refusing: " + (p.root ? err : "the SET is not open");
        if ((snap.total += s.bytes.size()) > kMaxSnapshotBytes) return "refusing: the SET's carried resources grew past " + std::to_string(kMaxSnapshotBytes) + " bytes while read";
        const auto c = panative::parse(reinterpret_cast<const uint8_t*>(s.bytes.data()), s.bytes.size());
        if (!c.ok) return "refusing: " + m.rel + " is not a valid KORF container: " + c.error;
        s.sha = sha(s.bytes);
        snap.add(m.rel, std::move(s));
    }
    return "";
}

// ---- dependency map -------------------------------------------------------------------------------------------------
struct Deps {
    size_t pcmFiles = 0, samples = 0, samplesWithoutId = 0, kmpFiles = 0, kmpEncoded = 0, kmpOther = 0, kmpRefs = 0;
    bool decided = false;                          // every sample has an ID and every KMP sample table was read
    std::vector<std::string> unreferenced;         // PCM samples no KMP lists ("file: name")
    std::vector<std::string> dangling;             // KMP sample IDs no PCM holds (hex)
    std::vector<std::string> notes;
};

inline std::string hex(const std::string& b) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : b) { o += d[c >> 4]; o += d[c & 15]; }
    return o;
}

// The sample IDs of one KMP item, or false when the item is not the observed raw layout (R7, docs/research/pa-studio/PA5X_SET_TO_KST_EVIDENCE.md).
inline bool kmpSampleIds(const uint8_t* p, size_t n, std::vector<std::string>& ids) {
    constexpr size_t kTable = 85, kRecord = 122, kId = 40;
    if (n < kTable || panative::detail::be16(p) != 0x0103) return false;   // R7: the one observed version
    const uint32_t count = panative::detail::be32(p + 2);
    if (count > (n - kTable) / kRecord) return false;
    for (uint32_t i = 0; i < count; ++i) ids.emplace_back(reinterpret_cast<const char*>(p + kTable + size_t(i) * kRecord + kId), 8);
    return true;
}

// From the snapshot only: the bytes that are exported are the bytes described.
inline Deps dependencies(const Plan& p, const Snapshot& snap) {
    Deps d;
    std::map<std::string, std::string> pcm;   // sample ID -> "file: name"
    std::set<std::string> refs;
    bool allKmpRead = true;
    for (const auto& m : p.members) {
        if (m.kind != Kind::Carry) continue;
        const bool isPcm = detail::endsWith(m.dest, ".pcm"), isKmp = detail::endsWith(m.dest, ".kmp");
        if (!isPcm && !isKmp) continue;
        const auto f = snap.files.find(m.rel);
        if (f == snap.files.end()) { d.notes.push_back(m.rel + ": not in the snapshot"); allKmpRead = false; continue; }
        const std::string& bytes = f->second.bytes;
        const auto* q = reinterpret_cast<const uint8_t*>(bytes.data());
        const auto c = panative::parse(q, bytes.size());
        if (!c.ok) { d.notes.push_back(m.rel + ": " + c.error); allKmpRead = false; continue; }
        if (isPcm) {
            ++d.pcmFiles;
            for (const auto& it : c.items) {
                ++d.samples;
                if (it.sampleId.empty()) { ++d.samplesWithoutId; continue; }
                pcm.emplace(it.sampleId, m.dest + ": " + panative::printable(it.name));
            }
            continue;
        }
        ++d.kmpFiles;
        for (const auto& it : c.items) {
            const uint8_t* body = q + it.offset + 8;
            if (it.size >= 4 && std::string(reinterpret_cast<const char*>(body), 4) == "OC31") { ++d.kmpEncoded; allKmpRead = false; continue; }
            std::vector<std::string> ids;
            if (it.chunkId != 0x11080018u || !kmpSampleIds(body, it.size, ids)) { ++d.kmpOther; allKmpRead = false; continue; }
            d.kmpRefs += ids.size();
            refs.insert(ids.begin(), ids.end());
        }
    }
    d.decided = allKmpRead && d.kmpFiles && !d.samplesWithoutId;
    if (d.samplesWithoutId) d.notes.push_back(std::to_string(d.samplesWithoutId) + " sample(s) carry no sample ID (directory field 4 is a Pa5X field; Pa700 PCM records lack it)");
    if (d.kmpEncoded) d.notes.push_back(std::to_string(d.kmpEncoded) + " KMP item(s) are OC31-encoded: which samples they use is UNKNOWN (carried unchanged, as the Pa5X does)");
    if (d.kmpOther) d.notes.push_back(std::to_string(d.kmpOther) + " KMP item(s) do not have the observed sample-table layout: references UNKNOWN");
    if (!d.kmpFiles && d.pcmFiles) d.notes.push_back("no MULTISMP/*.KMP: no multisample uses the samples");
    if (d.decided) {
        for (const auto& [id, where] : pcm) if (!refs.count(id)) d.unreferenced.push_back(where);
        for (const auto& id : refs) if (!pcm.count(id)) d.dangling.push_back(hex(id));
    }
    return d;
}

// Publish gate step (1): the SET pathname still names the pinned root, and every CARRY member below it (folders opened
// by descriptor, no link followed) still has its snapshot identity. No content is read. "" = unchanged.
inline std::string sourceChanged(const Plan& p, const std::filesystem::path& set, const Snapshot& snap) {
    struct stat pinned {}, now {};
    if (!p.root || ::fstat(p.root->v, &pinned) != 0 || ::lstat(set.c_str(), &now) != 0 || pinned.st_dev != now.st_dev || pinned.st_ino != now.st_ino)
        return set.string() + " is no longer the folder that was read (renamed or replaced)";
    for (const auto& [rel, s] : snap.files) {
        const auto [dir, leaf] = media::detail::splitRel(rel);
        const media::detail::Fd d(library::detail::openDirIn(p.root->v, dir, false));
        struct stat st {};
        if (d.v < 0 || ::fstatat(d.v, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) || !(identityOf(st) == s.id))
            return rel + " changed or was replaced since it was read";
        if (sourceCheckHook) sourceCheckHook(rel);
    }
    // again after the member checks (Codex r3 E2): the members were checked below the pinned root, the publish goes by name
    if (::lstat(set.c_str(), &now) != 0 || pinned.st_dev != now.st_dev || pinned.st_ino != now.st_ino)
        return set.string() + " was renamed or replaced while its members were checked";
    return "";
}

// The CONVERSION report: every member with its class and reason, then the dependency map.
inline std::string report(const Plan& p, const Deps& d) {
    std::string o = std::string("SET -> KST conversion, phase 1 (resources only). ") + kLabel + "\n";
    o += "Members: " + std::to_string(p.members.size()) + "; CARRY " + std::to_string(p.count(Kind::Carry)) + ", BLOCKED " +
         std::to_string(p.count(Kind::Blocked)) + ", DROPPED " + std::to_string(p.count(Kind::Dropped)) + ", UNKNOWN " +
         std::to_string(p.count(Kind::Unknown)) + "\n";
    if (p.count(Kind::Blocked))
        o += "Not converted: Styles, Keyboard Sets, Pads and SongBook entries (no .stg/.ksc/.pdc/.SBD is written). The KST is\n"
             "NOT a full conversion of the SET.\n";
    for (Kind k : {Kind::Carry, Kind::Blocked, Kind::Dropped, Kind::Unknown})
        for (const auto& m : p.members)
            if (m.kind == k) o += std::string(kindName(k)) + "  " + m.rel + (m.dest.empty() ? "" : " -> " + m.dest) + "  (" + m.reason + ")\n";
    o += "Dependency map (KMP -> PCM by 8-byte sample ID, R6/R7; names R6): " + std::to_string(d.pcmFiles) + " PCM file(s), " +
         std::to_string(d.samples) + " sample(s), " + std::to_string(d.kmpFiles) + " KMP file(s), " + std::to_string(d.kmpRefs) + " KMP sample reference(s)\n";
    if (d.decided) {
        o += "Unreferenced samples: " + std::to_string(d.unreferenced.size()) + "; dangling KMP references: " + std::to_string(d.dangling.size()) + "\n";
        for (const auto& s : d.unreferenced) o += "  unreferenced  " + s + "\n";
        for (const auto& s : d.dangling) o += "  dangling      sample ID " + s + "\n";
    } else o += "Unreferenced samples / dangling references: UNKNOWN (see notes)\n";
    for (const auto& n : d.notes) o += "  note: " + n + "\n";
    return o;
}

}  // namespace ai_arranger::media::panative::settokst

#endif
