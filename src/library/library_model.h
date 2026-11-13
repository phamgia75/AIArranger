// E7 Library Workspace model: a unified READ-ONLY view of a Pa5X KST folder or a Pa-family (Pa4X/Pa700) SET folder.
//
// What it reads, and how far each fact goes:
//   - layout, banks and sizes: folder layout and file names (media/kst_inventory.h detectLayout, listFiles; symlinks
//     and special files are listed as skipped, never followed). Pa5X KST: bank = TYPE/BANK folder (manual p.731-732),
//     one item per file, named after the file. SET: one file = one bank (MAN4 p.975: 48 per .STY/.PRF/.PAD file).
//   - SET items (name, kind, bank, slot): the E5 Pa-native reader (media/panative/panative_reader.h), READ-ONLY and
//     EXPERIMENTAL: layout observed on one owner Pa700 SET. A file it cannot read stays one file-level item and says why.
//     Pa5X KST files are never parsed (their container is UNKNOWN, D14).
//   - duplicates: identical files by SHA-256; identical item names across banks are flagged as NAME duplicates only
//     (content not compared).
//   - dependencies only where evidenced: Multisample -> Sample by file presence; Sound -> multisample is UNKNOWN.
//   - memory: DISK size of sample data (.PCM + .KMP files), never keyboard sample RAM.
// Nothing here writes. Every function is file/CPU work for a worker thread, never the message or audio thread.
#ifndef AI_ARRANGER_LIBRARY_LIBRARY_MODEL_H
#define AI_ARRANGER_LIBRARY_LIBRARY_MODEL_H

#include "media/kst_backup.h"   // detail::sha256File (no symlink followed)
#include "media/kst_diff.h"
#include "media/output_guard.h"  // folderPath
#include "media/panative/panative_reader.h"
#include <set>

namespace ai_arranger::library {

namespace fs = std::filesystem;

enum class Format { Pa5xKst, PaSet, Unknown };

inline const char* formatName(Format f) {
    return f == Format::Pa5xKst ? "Pa5X KST" : f == Format::PaSet ? "Pa-family SET" : "unknown layout";
}

// Our type names across generations (UI spec §4.1.1), by extension (Pa5X manual p.721-722, Pa4X manual p.973-974).
inline const char* typeOf(const std::string& upperExt) {
    static const std::map<std::string, const char*> t = {
        {".STG", "Style"}, {".STY", "Style"}, {".KSC", "Keyboard Set"}, {".PRF", "Keyboard Set"}, {".PDC", "Pad"}, {".PAD", "Pad"},
        {".PCG", "Sound"}, {".KMP", "Multisample"}, {".PCM", "Sample"}, {".SBD", "SongBook"}, {".SBL", "SongBook"},
        {".MID", "MIDI Song"}, {".MP3", "MP3 Song"}, {".WAV", "Audio"}, {".AIF", "Audio"}};
    const auto it = t.find(upperExt);
    return it == t.end() ? "Other" : it->second;
}

// Slots per SET bank file: 48 for Style, Keyboard Set and Pad banks (MAN4 p.975). 0 = not documented.
inline int bankCapacity(Format f, const std::string& type) {
    return f == Format::PaSet && (type == "Style" || type == "Keyboard Set" || type == "Pad") ? 48 : 0;
}

struct Item {
    std::string file;      // relative path of the file holding it
    std::string type;      // typeOf(extension)
    std::string bank;      // KST: "TYPE/BANK" folder (or "TYPE", "(root)"); SET: the bank file without extension
    std::string name;      // printable (non-ASCII bytes shown as \xNN)
    int slot = -1;         // 0-based, from the Pa-native directory; -1 = not known
    int kind = -1;         // raw Pa-native directory kind byte; -1 = none
    uintmax_t bytes = 0;   // KST/file item: file size; Pa-native item: payload size
    std::string from;      // where the item came from: "file name" or "Pa-native item table"
};

struct Bank { std::string type; uint32_t files = 0, items = 0; uintmax_t bytes = 0; };
struct FileInfo { uintmax_t size = 0; std::string sha256, container; };   // container: Pa-native status (SET only)
struct Dependency { std::string from, to, status, reason; };               // status: EVIDENCED | MISSING | UNKNOWN

struct Library {
    bool ok = false;
    std::string error, root, name, layout;
    Format format = Format::Unknown;
    std::vector<Item> items;
    std::map<std::string, Bank> banks;
    std::map<std::string, FileInfo> files;   // relative path -> size, SHA-256, container status
    std::vector<std::string> skipped;        // symlinks and special files (not followed)
    uintmax_t bytes = 0, sampleDiskBytes = 0;
    uint32_t sampleFiles = 0;
    std::vector<Dependency> deps;
};

inline std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

// Library-wide dependencies, only where the files themselves are evidence.
inline std::vector<Dependency> dependencies(const Library& L) {
    uint32_t kmp = 0, pcm = 0, pcg = 0;
    for (const auto& [rel, f] : L.files) {
        const std::string t = typeOf(media::detail::upper(fs::path(rel).extension().string()));
        kmp += t == "Multisample"; pcm += t == "Sample"; pcg += t == "Sound";
    }
    std::vector<Dependency> d;
    if (kmp > 0) {
        const std::string from = "Multisample (.KMP) files: " + std::to_string(kmp), to = "Sample (.PCM) files: " + std::to_string(pcm);
        if (pcm > 0) d.push_back({from, to, "EVIDENCED", "by file presence: sample files are here; which sample each multisample uses is not decoded"});
        else d.push_back({from, to, "MISSING", "by file presence: no .PCM sample file in this library, so the multisamples' own samples are not here"});
    }
    if (pcg > 0)
        d.push_back({"Sound (.PCG) files: " + std::to_string(pcg), "multisamples", "UNKNOWN",
                     "Sound payloads are not decoded, so which multisample (factory or user) a Sound uses is UNKNOWN"});
    return d;
}

// What one item needs, only where evidenced; used for plan warnings and the item details.
inline std::vector<Dependency> itemDependencies(const Library& L, const Item& it) {
    std::vector<Dependency> d;
    if (it.type == "Multisample") {
        for (const auto& x : dependencies(L)) if (x.from.rfind("Multisample", 0) == 0) d.push_back(x);
    } else if (it.type == "Sound") {
        d.push_back({it.name, "multisamples", "UNKNOWN", "Sound payloads are not decoded: which multisample it uses is UNKNOWN"});
    } else if (it.type == "Style" || it.type == "Keyboard Set" || it.type == "Pad") {
        d.push_back({it.name, "Sounds", "UNKNOWN", "payload not decoded: which Sounds it uses is UNKNOWN (factory Sounds need nothing; user Sounds must come along)"});
    }
    return d;
}

// Bounds for one library, checked while the folder is listed (before anything is hashed or parsed), so a hostile or
// mistaken folder is refused instead of exhausting memory or hashing forever. A Pa5X KST is TYPE/BANK/file and holds at
// most 4 GB (8 GB compressed) of samples (MAN pp.1174, 1176); these leave wide room for that.
inline constexpr media::detail::ListLimits kLibraryLimits{200000, 16, uintmax_t(64) << 30};   // entries, depth, 64 GiB

// Reads a library folder. Strict like media::scan: any listing or read error fails it (ok = false) instead of showing
// a partial library as complete. `keepGoing` (optional) is checked per file and per hashed block. Hashing reads at most
// limits.bytes (the listing already refused anything larger).
inline Library load(const fs::path& root, const std::function<bool()>& keepGoing = {}, const media::detail::ListLimits& limits = kLibraryLimits) {
    Library L;
    L.root = root.string();
    L.name = media::folderPath(root).filename().string();
    media::detail::Listing ls;
    if (!media::detail::listFiles(root, ls, L.error, keepGoing, limits)) return L;
    L.layout = media::detail::detectLayout(root, ls);
    L.format = L.layout == media::kLayoutKst ? Format::Pa5xKst : media::isSetLayout(L.layout) ? Format::PaSet : Format::Unknown;
    L.skipped = ls.skipped;
    const std::function<bool(size_t)> more = [&](size_t) { return !keepGoing || keepGoing(); };
    for (const auto& [rel, e] : ls.files) {
        bool cancelled = false;
        std::string err;
        const std::string sha = media::detail::sha256File(e.abs, more, &cancelled, &err);
        if (sha.empty()) { L.error = cancelled ? "cancelled" : err; return L; }
        const fs::path p(rel);
        const std::string type = typeOf(media::detail::upper(p.extension().string()));
        std::vector<std::string> parts;
        for (const auto& c : p) parts.push_back(c.string());
        std::string bank;
        if (L.format == Format::PaSet) bank = (p.parent_path() / p.stem()).generic_string();   // one file = one bank
        else bank = parts.size() >= 3 ? parts[0] + "/" + parts[1] : parts.size() == 2 ? parts[0] : "(root)";
        FileInfo fi{e.size, sha, ""};
        auto& b = L.banks[bank];
        if (b.files == 0) b.type = type;
        else if (b.type != type) b.type = "mixed";
        ++b.files;
        b.bytes += e.size;
        bool listed = false;
        if (L.format == Format::PaSet) {
            const auto c = media::panative::readFile(e.abs);
            if (c.ok) {
                fi.container = "Pa-native, " + std::to_string(c.items.size()) + " items";
                for (const auto& it : c.items)
                    L.items.push_back({rel, type, bank, media::panative::printable(it.name), it.slot, it.kind, it.size, "Pa-native item table"});
                b.items += uint32_t(c.items.size());
                listed = true;
            } else {
                fi.container = c.ioError ? "unreadable: " + c.error : c.wrapper ? "INVALID Pa-native container: " + c.error : "no Pa-native wrapper";
            }
        }
        if (!listed) {
            L.items.push_back({rel, type, bank, media::panative::printable(p.stem().string()), -1, -1, e.size,
                               L.format == Format::PaSet ? "file name (" + fi.container + ")" : "file name"});
            ++b.items;
        }
        if (type == "Sample" || type == "Multisample") { L.sampleDiskBytes += e.size; ++L.sampleFiles; }
        L.bytes += e.size;
        L.files[rel] = fi;
    }
    std::sort(L.items.begin(), L.items.end(), [](const Item& a, const Item& b) {
        return std::tie(a.bank, a.slot, a.name, a.file) < std::tie(b.bank, b.slot, b.name, b.file);
    });
    L.deps = dependencies(L);
    L.ok = true;
    return L;
}

// ── Search and filter ──
inline std::string lower(std::string s) { return media::detail::lower(std::move(s)); }

// Indices of the items whose name, bank or file contains `query` (case-insensitive); `type`/`bank` "" = any.
inline std::vector<size_t> search(const Library& L, const std::string& query, const std::string& type = "", const std::string& bank = "") {
    const std::string q = lower(query);
    std::vector<size_t> r;
    for (size_t i = 0; i < L.items.size(); ++i) {
        const auto& it = L.items[i];
        if ((!type.empty() && it.type != type) || (!bank.empty() && it.bank != bank)) continue;
        if (q.empty() || lower(it.name).find(q) != std::string::npos || lower(it.bank).find(q) != std::string::npos ||
            lower(it.file).find(q) != std::string::npos)
            r.push_back(i);
    }
    return r;
}

// ── Duplicates ──
struct Duplicates {
    std::vector<std::vector<std::string>> files;   // identical files (same SHA-256), relative paths
    std::vector<std::vector<size_t>> names;        // items of one type with the same name in two or more banks (name only)
};

inline Duplicates duplicates(const Library& L) {
    Duplicates d;
    std::map<std::string, std::vector<std::string>> bySha;
    for (const auto& [rel, f] : L.files) bySha[f.sha256].push_back(rel);
    for (auto& [h, g] : bySha) if (g.size() > 1) d.files.push_back(g);
    std::map<std::pair<std::string, std::string>, std::vector<size_t>> byName;
    for (size_t i = 0; i < L.items.size(); ++i)
        if (const auto n = trimmed(L.items[i].name); !n.empty()) byName[{L.items[i].type, n}].push_back(i);
    for (auto& [k, g] : byName) {
        std::set<std::string> banks;
        for (size_t i : g) banks.insert(L.items[i].bank);
        if (banks.size() > 1) d.names.push_back(g);
    }
    return d;
}

// ── Compare two libraries (file level, media::diff) ──
struct Comparison {
    bool ok = false;
    std::string error, note;
    std::vector<std::string> onlyA, onlyB, same, different;
    std::vector<std::pair<std::string, std::string>> moved;   // same bytes, different path
    std::vector<std::string> skipped;
};

inline Comparison compare(const Library& A, const Library& B) {
    Comparison c;
    const auto d = media::diff(A.root, B.root, kLibraryLimits);   // the same bounds as load()
    if (!d.ok) { c.error = d.error; return c; }
    c.onlyA = d.removed; c.onlyB = d.added; c.different = d.changed; c.moved = d.moved; c.skipped = d.skipped;
    const std::set<std::string> changed(d.changed.begin(), d.changed.end());
    for (const auto& [rel, f] : A.files) if (B.files.count(rel) && !changed.count(rel)) c.same.push_back(rel);
    // the diff re-lists both folders: its counts must match the snapshots the user is looking at
    if (c.same.size() != d.identical || d.identical + d.changed.size() + d.removed.size() + d.moved.size() != A.files.size() ||
        d.identical + d.changed.size() + d.added.size() + d.moved.size() != B.files.size()) { c.error = "a library changed since it was opened: open both again, then compare"; return c; }
    if (A.format != B.format)
        c.note = std::string("A is a ") + formatName(A.format) + " and B a " + formatName(B.format) +
                 ": their files do not share paths, so the comparison shows them as different files only.";
    c.ok = true;
    return c;
}

// ── Text views ──
inline std::string bytesText(uintmax_t b) {
    char s[48];
    if (b >= (1u << 20)) std::snprintf(s, sizeof s, "%.1f MB", double(b) / (1 << 20));
    else if (b >= 1024) std::snprintf(s, sizeof s, "%.1f KB", double(b) / 1024);
    else std::snprintf(s, sizeof s, "%ju B", b);
    return s;
}

inline std::string summary(const Library& L) {
    std::ostringstream o;
    if (!L.ok) { o << L.name << ": not read: " << L.error << "\n"; return o.str(); }
    o << L.name << "  ·  " << formatName(L.format) << " (" << L.layout << ", folder layout and names)  ·  Read-only source\n"
      << L.files.size() << " files, " << L.items.size() << " items, " << L.banks.size() << " banks, " << bytesText(L.bytes) << " total\n"
      << "Sample data on disk (.PCM + .KMP file sizes, not keyboard sample RAM): " << L.sampleFiles << " files, " << bytesText(L.sampleDiskBytes) << "\n";
    if (L.format == Format::PaSet) o << "SET items come from the Pa-native reader (READ-ONLY, EXPERIMENTAL; layout observed on one owner Pa700 SET).\n";
    if (L.format == Format::Pa5xKst) o << "KST items are files (one per item, named after the file); KST file content is not decoded (container UNKNOWN).\n";
    if (!L.skipped.empty()) o << "Skipped (links and special files, not followed): " << L.skipped.size() << "\n";
    return o.str();
}

inline std::string bankView(const Library& L) {
    std::ostringstream o;
    o << "Banks (type · files · items · size)\n";
    for (const auto& [id, b] : L.banks) {
        const int cap = bankCapacity(L.format, b.type);
        o << "  " << id << "  " << b.type << " · " << b.files << " · " << b.items << (cap ? "/" + std::to_string(cap) : "") << " · " << bytesText(b.bytes) << "\n";
    }
    o << "Total " << bytesText(L.bytes) << "\n";
    return o.str();
}

inline std::string dependencyView(const std::vector<Dependency>& d) {
    std::string o;
    for (const auto& x : d) o += "  " + x.status + ": " + x.from + " -> " + x.to + " (" + x.reason + ")\n";
    return o.empty() ? "  none evidenced\n" : o;
}

inline std::string itemLine(const Item& it) {
    char s[64];
    std::snprintf(s, sizeof s, "%-12s ", it.type.c_str());
    std::string slot = it.slot >= 0 ? " slot " + std::to_string(it.slot + 1) : "";
    return s + it.bank + slot + "  " + it.name;
}

inline std::string itemDetails(const Library& L, const Item& it) {
    std::ostringstream o;
    o << it.name << "\n  type " << it.type << ", bank " << it.bank << (it.slot >= 0 ? ", slot " + std::to_string(it.slot + 1) + " (shown 1-based)" : "")
      << (it.kind >= 0 ? ", Pa-native kind " + std::to_string(it.kind) + " (meaning UNKNOWN)" : "") << "\n  file " << it.file << ", "
      << bytesText(it.bytes) << (it.from == "Pa-native item table" ? " payload" : "") << ", from the " << it.from << "\n";
    if (const auto f = L.files.find(it.file); f != L.files.end()) o << "  file SHA-256 " << f->second.sha256 << "\n";
    o << "Dependencies (only where evidenced):\n" << dependencyView(itemDependencies(L, it));
    return o.str();
}

inline std::string duplicatesView(const Library& L, const Duplicates& d) {
    std::ostringstream o;
    o << L.name << ": identical files (same SHA-256): " << d.files.size() << " group(s)\n";
    for (const auto& g : d.files) {
        o << "  -";
        for (const auto& r : g) o << " " << r;
        o << "\n";
    }
    o << "Same name in two or more banks (NAME ONLY: content not compared): " << d.names.size() << " group(s)\n";
    for (const auto& g : d.names) {
        o << "  " << L.items[g.front()].type << " \"" << trimmed(L.items[g.front()].name) << "\":";
        for (size_t i : g) o << " " << L.items[i].bank << (L.items[i].slot >= 0 ? "#" + std::to_string(L.items[i].slot + 1) : "");
        o << "\n";
    }
    return o.str();
}

inline std::string compareView(const Library& A, const Library& B, const Comparison& c) {
    std::ostringstream o;
    o << "Compare (file level, byte equality; contents not interpreted)\nA: " << A.root << "\nB: " << B.root << "\n";
    if (!c.ok) { o << "Not compared: " << c.error << "\n"; return o.str(); }
    if (!c.note.empty()) o << c.note << "\n";
    o << "same " << c.same.size() << ", different " << c.different.size() << ", only in A " << c.onlyA.size() << ", only in B " << c.onlyB.size()
      << ", same bytes at another path " << c.moved.size() << ", skipped " << c.skipped.size() << "\n";
    for (const auto& p : c.different) o << "  different   " << p << "\n";
    for (const auto& p : c.onlyA) o << "  only in A   " << p << "\n";
    for (const auto& p : c.onlyB) o << "  only in B   " << p << "\n";
    for (const auto& [a, b] : c.moved) o << "  moved       " << a << " -> " << b << "\n";
    for (const auto& p : c.same) o << "  same        " << p << "\n";
    for (const auto& p : c.skipped) o << "  skipped     " << p << " (not compared)\n";
    return o.str();
}

}  // namespace ai_arranger::library

#endif
