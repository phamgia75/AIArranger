// SET -> KST conversion, phase 1 (D54 B1): resources only. STRUCTURALLY VALID - NOT VERIFIED ON A PA5X.
//
// Every rule is the measured Pa5X conversion of one owner Pa700 SET into a KST (KST_DEPENDENCY_EVIDENCE.md, "Pa5X's own
// SET -> KST conversion"; tests/integration/test_set_to_kst.cpp has the private oracle run on that pair):
//   CARRY    PCM/*.PCM, SOUND/*.PCG, MULTISMP/*.KMP, SONGBOOK/*.SBL: byte-identical, same type folder, file name lower-cased
//            ("identical 99/99 (only the name is lower-cased)", 4/4 PCG, KMP, SBL).
//   BLOCKED  STYLE/*.STY, KEYBOARDSET/*.PRF, PAD/*.PAD, SONGBOOK/SONGDB.SBD/*.SBD: the Pa5X re-writes them, and their
//            item payloads are OC31-encoded, which is not decoded ("Still unknown" 1). Nothing is written for them: no
//            partial or guessed .stg/.ksc/.pdc/.SBD.
//   DROPPED  GLOBAL/, VOICEPRESET/, GUITARPRESET/: "not carried over", consistent with MAN5 p.737.
//   UNKNOWN  anything else (not in the measured table): not carried, listed.
// Matching is exact on the observed upper-case names; nothing else is inferred.
//
// Dependency map (read-only, on the carried files):
//   PCM sample = directory record of a PCM file; name = field 2 (D10), sample ID = field 4, 8 bytes (D9; a Pa5X field,
//   "not present in Pa700 PCM records", FINDINGS §3).
//   KMP -> PCM = the KMP item's sample table (FINDINGS §6, raw Pa5X item 11080018: u32 count at +2, 122-byte records
//   from +85, sample ID at record +40). An OC31-encoded KMP item (the Pa700 RAM.KMP) cannot be read: its references
//   are UNKNOWN, and no sample is called unreferenced or dangling then.
#ifndef AI_ARRANGER_MEDIA_PANATIVE_SET_TO_KST_H
#define AI_ARRANGER_MEDIA_PANATIVE_SET_TO_KST_H

#include "media/panative/kst_assemble.h"
#include "media/sha256.h"
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
    std::filesystem::path abs;
    uint64_t bytes = 0;
};

struct Plan {
    bool ok = false;
    std::string error;
    std::vector<Member> members;   // sorted by rel
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
    media::detail::Listing ls;
    if (!media::detail::listFiles(set, ls, p.error)) return p;
    for (const auto& [rel, e] : ls.files) {
        Member m{rel};
        m.abs = e.abs;
        m.bytes = e.size;
        detail::classify(m);
        p.members.push_back(std::move(m));
    }
    for (const auto& rel : ls.skipped) p.members.push_back({rel, Kind::Unknown, "", "a link or special file: not followed, not carried"});
    std::sort(p.members.begin(), p.members.end(), [](const Member& a, const Member& b) { return a.rel < b.rel; });
    p.ok = true;
    return p;
}

// Why the plan cannot be written, or "". The same refusals as the W5 assembler (assemble::build) for carried files:
// a valid KORF container, a safe TYPE/FILE destination, no two destinations equal without case.
inline std::string refusal(const Plan& p) {
    if (!p.ok) return "cannot list the SET: " + p.error;
    if (!p.count(Kind::Carry)) return "refusing: the SET holds no resource that the Pa5X carries unchanged (PCM, PCG, KMP, SBL)";
    std::set<std::string> seen;
    for (const auto& m : p.members) {
        if (m.kind != Kind::Carry) continue;
        const auto slash = m.dest.find('/');
        if (!assemble::safeName(m.dest.substr(0, slash)) || !assemble::safeName(m.dest.substr(slash + 1))) return "refusing: unsafe file name " + m.rel;
        if (!seen.insert(closure::upper(m.dest)).second) return "refusing: two members map to " + m.dest + " (names differ only in case?)";
    }
    for (const auto& m : p.members) {
        if (m.kind != Kind::Carry) continue;
        const auto c = panative::readFile(m.abs);
        if (!c.ok) return "refusing: " + m.rel + " is not a valid KORF container: " + c.error;
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

// The sample IDs of one KMP item, or false when the item is not the observed raw layout (FINDINGS §6).
inline bool kmpSampleIds(const uint8_t* p, size_t n, std::vector<std::string>& ids) {
    constexpr size_t kTable = 85, kRecord = 122, kId = 40;
    if (n < kTable) return false;
    const uint32_t count = panative::detail::be32(p + 2);
    if (count > (n - kTable) / kRecord) return false;
    for (uint32_t i = 0; i < count; ++i) ids.emplace_back(reinterpret_cast<const char*>(p + kTable + size_t(i) * kRecord + kId), 8);
    return true;
}

inline Deps dependencies(const Plan& p) {
    Deps d;
    std::map<std::string, std::string> pcm;   // sample ID -> "file: name"
    std::set<std::string> refs;
    bool allKmpRead = true;
    for (const auto& m : p.members) {
        if (m.kind != Kind::Carry) continue;
        const bool isPcm = detail::endsWith(m.dest, ".pcm"), isKmp = detail::endsWith(m.dest, ".kmp");
        if (!isPcm && !isKmp) continue;
        std::string bytes, err;
        if (!roottree::readBytes(m.abs, bytes, err)) { d.notes.push_back(m.rel + ": cannot read: " + err); allKmpRead = false; continue; }
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
    o += "Dependency map (KMP -> PCM by 8-byte sample ID, D9; names D10): " + std::to_string(d.pcmFiles) + " PCM file(s), " +
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
