// P1.9 KST inventory: a read-only report on a Pa5X KST folder (or any folder).
// File contents are opaque: only paths, extensions, sizes and byte equality are
// used. Never writes, never follows symlinks.
#ifndef AI_ARRANGER_MEDIA_KST_INVENTORY_H
#define AI_ARRANGER_MEDIA_KST_INVENTORY_H

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ai_arranger::media {

struct ExtStats { uint32_t files = 0; uintmax_t bytes = 0; };

struct Inventory {
    bool ok = false;
    std::string error;
    uint32_t files = 0;
    uintmax_t bytes = 0;
    std::map<std::string, ExtStats> by_ext;      // ".STY" (upper case), "(none)"
    std::map<std::string, ExtStats> by_folder;   // first path component, "." = root
    std::map<std::string, ExtStats> by_bank;     // "TYPE/BANK" for files at depth >= 3 (manual p.731-732: type -> bank -> file)
    std::vector<std::vector<std::string>> duplicates;  // byte-identical groups (relative paths, sorted)
    std::vector<std::string> skipped;            // symlinks and special files (not followed); unreadable entries fail the scan
    std::map<std::string, ExtStats> by_type;     // documented Pa5X file type (manual v1.4 p.721-722)
    bool has_pcm_folder = false;                 // a folder named PCM: the KST holds User Samples (p.736)
    std::string layout;                          // detectLayout(): Pa5X KST, Pa4X-family SET, Pa700/Pa1000-family SET, unknown
};

// Pa5X file type by extension, from the manual's file-type table (v1.4 p.721-722).
// Identification only: it says nothing about which resource uses which.
inline const char* documentedType(const std::string& upperExt) {
    static const std::map<std::string, const char*> t = {
        {".KSC", "Keyboard Set"}, {".STG", "Style"}, {".PDC", "Pad"}, {".CSC", "Chord Sequence"},
        {".MID", "MIDI Song"}, {".MP3", "MP3 Song"}, {".JBX", "Jukebox"}, {".TXT", "Text"},
        {".ARG", "Arrangement"}, {".SBD", "SongBook database"}, {".SBL", "SongBook Set List"},
        {".GBL", "Global Settings"}, {".MPR", "MIDI Preset"}, {".QTP", "Quarter Tone Preset"},
        {".VOC", "Mic Preset"}, {".GTR", "Guitar Preset"}, {".MXP", "MaxxAudio Preset"},
        {".PCG", "Sound"}, {".KMP", "Multisample"}, {".PCM", "Sample"},
        {".AIF", "AIFF audio"}, {".WAV", "WAVE audio"},
    };
    const auto it = t.find(upperExt);
    return it == t.end() ? "(not a documented Pa5X type)" : it->second;
}

// Pa4X-family SET member types, from the Pa4X manual's file-type table (OS 3.1, PDF p.973-974).
inline const char* setDocumentedType(const std::string& upperExt) {
    static const std::map<std::string, const char*> t = {
        {".BKP", "Full Resource Backup"}, {".GBL", "Global Setup"}, {".VOC", "Voice Preset bank"},
        {".MXP", "MaxxAudio Preset"}, {".QTP", "Quarter Tone Preset"}, {".MPR", "MIDI Preset"},
        {".PRF", "Keyboard Set bank (48 per file, MAN4 p.975)"}, {".PCG", "Sound"}, {".KMP", "Multisample"},
        {".PCM", "Sample"}, {".AIF", "AIFF audio"}, {".WAV", "WAVE audio"},
        {".STY", "Style bank (48 per file, MAN4 p.975)"}, {".PAD", "Pad bank (48 per file, MAN4 p.975)"},
        {".SBD", "SongBook database"}, {".SBL", "SongBook Set List"}, {".JBX", "Jukebox"},
        {".MID", "MIDI Song"}, {".MP3", "MP3 Song"}, {".TXT", "Text"},
        // Seen only in the owner Pa700 SET (GLOBAL/LIMITER.AUD, MASTEREQ.AUD); not in the Pa4X table.
        {".AUD", "GLOBAL audio preset (Pa700-family, not in the Pa4X table)"},
    };
    const auto it = t.find(upperExt);
    return it == t.end() ? "(not a documented SET type)" : it->second;
}

constexpr const char* kLayoutKst = "Pa5X KST";
constexpr const char* kLayoutPa700 = "Pa700/Pa1000-family SET";
constexpr const char* kLayoutPa4x = "Pa4X-family SET (generation not determined)";
constexpr const char* kLayoutUnknown = "unknown (no KST or SET markers)";
inline bool isSetLayout(const std::string& layout) { return layout == kLayoutPa700 || layout == kLayoutPa4x; }

namespace detail {
inline bool hashFile(const std::filesystem::path& p, uint64_t& h) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    h = 1469598103934665603ull;
    char buf[65536];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) {
        for (std::streamsize i = 0; i < in.gcount(); ++i)
            h = (h ^ static_cast<unsigned char>(buf[i])) * 1099511628211ull;
        if (!in) break;
    }
    return in.eof();
}
struct Entry { std::filesystem::path abs; uintmax_t size; };
struct Listing {
    std::map<std::string, Entry> files;   // relative path -> entry
    std::vector<std::string> skipped;     // symlinks and special files (not followed, not compared)
    std::vector<std::string> dirs;        // every subfolder, relative (a backup recreates empty ones too)
    bool pcm = false;                     // a directory named PCM exists (manual p.736: the KST holds User Samples)
};

// Bounds on a listing, checked during enumeration (a hostile folder cannot exhaust memory or run forever). The defaults
// sit far above any keyboard backup (a Pa5X KST is TYPE/BANK/file, a few thousand files) so normal results never change.
struct ListLimits {
    size_t entries = 1000000;              // files + folders + skipped entries
    int depth = 64;                        // path components below the root
    uintmax_t bytes = UINTMAX_MAX;         // sum of regular-file sizes
};

// Full recursive listing. Returns false (with error) on any permission, iteration or metadata error, when
// `keepGoing` (checked per entry, optional) returns false, or when a limit is passed (the listing stops there).
inline bool listFiles(const std::filesystem::path& root, Listing& out, std::string& error, const std::function<bool()>& keepGoing = {},
                      const ListLimits& limits = {}) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(root, ec))) { error = "not a directory (symlinks are not followed): " + root.string(); return false; }
    fs::recursive_directory_iterator it(root, ec), end;   // no skip_permission_denied: an unreadable folder is an error
    size_t entries = 0;
    uintmax_t bytes = 0;
    for (;;) {
        if (ec) { error = "cannot read " + root.string() + ": " + ec.message(); return false; }
        if (it == end) break;
        if (keepGoing && !keepGoing()) { error = "cancelled"; return false; }
        const auto st = it->symlink_status(ec);
        if (ec) { error = "cannot stat " + it->path().string() + ": " + ec.message(); return false; }
        const std::string rel = it->path().lexically_relative(root).generic_string();
        if (++entries > limits.entries) { error = "refusing: more than " + std::to_string(limits.entries) + " entries in " + root.string(); return false; }
        if (it.depth() + 1 > limits.depth) { error = "refusing: " + rel + " is nested deeper than " + std::to_string(limits.depth) + " folders"; return false; }
        if (fs::is_directory(st)) {
            std::string n = it->path().filename().string();
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::toupper(c); });
            out.pcm |= n == "PCM";
            out.dirs.push_back(rel);
        } else if (fs::is_regular_file(st)) {
            const uintmax_t size = it->file_size(ec);
            if (ec) { error = "cannot stat " + it->path().string() + ": " + ec.message(); return false; }
            if (size > limits.bytes - bytes) { error = "refusing: more than " + std::to_string(limits.bytes) + " bytes of files in " + root.string(); return false; }
            bytes += size;
            out.files[rel] = {it->path(), size};
        } else {
            out.skipped.push_back(rel);   // symlink or special file; recursive_directory_iterator does not follow symlinks by default
        }
        it.increment(ec);
    }
    return true;
}

// 1 = same bytes, 0 = different, -1 = a file could not be read completely.
inline int compareFiles(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    if (!x || !y) return -1;
    static thread_local char bx[65536], by[65536];
    for (;;) {
        x.read(bx, sizeof bx);
        y.read(by, sizeof by);
        if (x.bad() || y.bad()) return -1;
        if (x.gcount() != y.gcount() || !std::equal(bx, bx + x.gcount(), by)) return 0;
        if (x.gcount() == 0) return x.eof() && y.eof() ? 1 : -1;
    }
}

inline std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

// Folder suffix first (.KST: MAN5 p.721; .SET: MAN4 PDF p.973), then names only:
// Pa700/Pa1000 markers (owner Pa700 SET, pa-studio-research FORMAT_MATRIX.md): GLOBAL/LIMITER.AUD,
//   GLOBAL/MASTEREQ.AUD, SOUND/USERDK01.PCG, folder SONGBOOK/SONGDB.SBD. Pa4X's manual layout has none of them.
// Pa4X-family markers (MAN4 PDF p.975): .STY/.PRF/.PAD, SOUND/USERDK.PCG, MULTISMP/RAM.KMP.
// Pa5X markers (MAN5 p.721): .STG/.KSC/.PDC.
inline std::string detectLayout(const std::filesystem::path& root, const Listing& l) {
    // The folder's own name, however it was spelled ("X.KST", "X.KST/", "X.KST/.", "."): resolve it first.
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::weakly_canonical(root, ec);
    if (ec) dir = std::filesystem::absolute(root, ec).lexically_normal();
    if (dir.filename().empty()) dir = dir.parent_path();
    const std::string name = upper(dir.filename().string());
    auto ends = [&](const char* s) { const std::string x(s); return name.size() >= x.size() && name.compare(name.size() - x.size(), x.size(), x) == 0; };
    bool pa700 = false, pa4x = false, pa5x = false;
    for (const auto& d : l.dirs) pa700 |= upper(d) == "SONGBOOK/SONGDB.SBD";
    for (const auto& [rel, e] : l.files) {
        const std::string r = upper(rel), ext = upper(std::filesystem::path(rel).extension().string());
        pa700 |= r == "GLOBAL/LIMITER.AUD" || r == "GLOBAL/MASTEREQ.AUD" || r == "SOUND/USERDK01.PCG";
        pa4x |= ext == ".STY" || ext == ".PRF" || ext == ".PAD" || r == "SOUND/USERDK.PCG" || r == "MULTISMP/RAM.KMP";
        pa5x |= ext == ".STG" || ext == ".KSC" || ext == ".PDC";
    }
    if (ends(".KST")) return kLayoutKst;
    if (pa700) return kLayoutPa700;
    if (ends(".SET") || pa4x) return kLayoutPa4x;
    return pa5x ? kLayoutKst : kLayoutUnknown;
}

}  // namespace detail

// Strict: any permission, iteration, metadata or read error fails the whole scan (ok = false), so a
// partial scan can never print file totals or duplicate groups that look complete. Symlinks and
// special files are listed as skipped, never followed.
inline Inventory scan(const std::filesystem::path& root) {
    Inventory inv;
    detail::Listing l;
    if (!detail::listFiles(root, l, inv.error)) return inv;
    inv.skipped = l.skipped;
    inv.has_pcm_folder = l.pcm;
    inv.layout = detail::detectLayout(root, l);
    const bool set = isSetLayout(inv.layout);
    std::map<uintmax_t, std::vector<const std::string*>> bySize;
    for (const auto& [rel, e] : l.files) {
        // Every file must at least open: a file nobody can read is an error even when its size is unique
        // (a full read happens below only where a duplicate is possible).
        if (!std::ifstream(e.abs, std::ios::binary)) { inv.error = "cannot read " + e.abs.string(); return inv; }
        ++inv.files;
        inv.bytes += e.size;
        std::string ext = std::filesystem::path(rel).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::toupper(c); });
        auto& x = inv.by_ext[ext.empty() ? "(none)" : ext];
        ++x.files; x.bytes += e.size;
        auto& ty = inv.by_type[set ? setDocumentedType(ext) : documentedType(ext)];
        ++ty.files; ty.bytes += e.size;
        const auto slash = rel.find('/');
        auto& dir = inv.by_folder[slash == std::string::npos ? "." : rel.substr(0, slash)];
        ++dir.files; dir.bytes += e.size;
        if (slash != std::string::npos) {
            const auto slash2 = rel.find('/', slash + 1);
            if (slash2 != std::string::npos) {
                auto& bk = inv.by_bank[rel.substr(0, slash2)];
                ++bk.files; bk.bytes += e.size;
            }
        }
        bySize[e.size].push_back(&rel);
    }
    // Hash only files that share a size, then confirm every match byte by byte.
    for (const auto& [size, group] : bySize) {
        if (group.size() < 2) continue;
        std::map<uint64_t, std::vector<std::vector<const std::string*>>> byHash;
        for (const std::string* rel : group) {
            const auto& abs = l.files.at(*rel).abs;
            uint64_t h = 0;
            if (!detail::hashFile(abs, h)) { inv.error = "cannot read " + abs.string(); return inv; }
            auto& sets = byHash[h];
            auto it = sets.begin();
            for (; it != sets.end(); ++it) {
                const int c = detail::compareFiles(l.files.at(*it->front()).abs, abs);
                if (c < 0) { inv.error = "cannot read " + abs.string() + " or " + l.files.at(*it->front()).abs.string(); return inv; }
                if (c == 1) break;
            }
            if (it == sets.end()) sets.push_back({rel}); else it->push_back(rel);
        }
        for (const auto& [h, sets] : byHash)
            for (const auto& set : sets)
                if (set.size() > 1) {
                    std::vector<std::string> g;
                    for (const std::string* r : set) g.push_back(*r);
                    inv.duplicates.push_back(g);
                }
    }
    std::sort(inv.duplicates.begin(), inv.duplicates.end());
    std::sort(inv.skipped.begin(), inv.skipped.end());
    inv.ok = true;
    return inv;
}

// PA Manager v5.2 "[4 GB] | PA5X" memory profile: Total Size 4 294 967 296 bytes (audit §3f).
// An application observation, not an independently verified Pa5X hardware specification.
constexpr uintmax_t kPaManagerPa5xBudgetBytes = 4294967296ull;

// Parses a --budget-bytes value: decimal digits only, > 0, no overflow. Returns false otherwise.
inline bool parseBudgetBytes(const char* s, uintmax_t& out) {
    if (!s || *s < '0' || *s > '9') return false;  // rejects "", "-1", "+1", " 1"
    errno = 0;
    char* end = nullptr;
    const uintmax_t v = std::strtoumax(s, &end, 10);
    if (*end || errno == ERANGE || v == 0) return false;
    out = v;
    return true;
}

// P2.3: sample-data DISK size (.PCM Sample + .KMP Multisample files) against a budget.
// shortcut: disk bytes only; replace with a RAM model once Pa5X allocation is measured on hardware.
inline std::string formatMemoryEstimate(const Inventory& inv, uintmax_t budget) {
    auto stat = [&](const char* t) { const auto it = inv.by_type.find(t); return it == inv.by_type.end() ? ExtStats{} : it->second; };
    const ExtStats pcm = stat("Sample"), kmp = stat("Multisample");
    const uintmax_t disk = pcm.bytes + kmp.bytes;
    const uintmax_t permille = budget ? disk * 1000 / budget : 0;
    std::ostringstream o;
    o << "\nSample memory estimate (P2.3) - DISK SIZE, not Pa5X sample RAM:\n"
      << "  .PCM Sample files       " << pcm.files << " files, " << pcm.bytes << " bytes\n"
      << "  .KMP Multisample files  " << kmp.files << " files, " << kmp.bytes << " bytes\n"
      << "  Disk total " << disk << " bytes = " << permille / 10 << "." << permille % 10 << "% of budget " << budget << " bytes"
      << (budget == kPaManagerPa5xBudgetBytes ? " (PA Manager v5.2 '[4 GB] | PA5X' profile; application figure, not a hardware spec)" : "")
      << (disk > budget ? "\n  OVER BUDGET on disk size" : "") << "\n"
      << "  Not modelled: sample compression, per-sample overhead, how the Pa5X allocates sample RAM.\n"
      << "  PA Manager's own 'Used Size' differs from file sizes on disk (audit §3f). HARDWARE VALIDATION PENDING.\n";
    return o.str();
}

inline std::string format(const Inventory& inv, const std::string& label, uintmax_t budget = kPaManagerPa5xBudgetBytes) {
    std::ostringstream o;
    o << "KST inventory (read-only): " << label << "\n";
    if (!inv.ok) { o << "ERROR: " << inv.error << "\n"; return o.str(); }
    o << "Layout: " << inv.layout << " (folder suffix and file names only)\n";
    o << "Files: " << inv.files << ", bytes: " << inv.bytes << "\n\nBy extension:\n";
    for (const auto& [k, v] : inv.by_ext) o << "  " << k << "  " << v.files << " files, " << v.bytes << " bytes\n";
    o << (isSetLayout(inv.layout) ? "\nBy documented SET file type (Pa4X manual OS 3.1 PDF p.973-975; identification by extension only):\n"
                                  : "\nBy documented Pa5X file type (manual v1.4 p.721; identification by extension only):\n");
    for (const auto& [k, v] : inv.by_type) o << "  " << k << "  " << v.files << " files, " << v.bytes << " bytes\n";
    o << "\nPCM folder: " << (!inv.has_pcm_folder ? "no"
          : isSetLayout(inv.layout)
          ? "YES. This SET holds User Samples. A Pa5X reads a SET only through its one-way Data Conversion (manual p.737)."
          : "YES. This KST holds User Samples: loading the WHOLE KST on a Pa5X deletes the User Samples in memory "
            "(manual p.736); load single Sounds to keep them.") << "\n";
    o << "\nBy top-level folder:\n";
    for (const auto& [k, v] : inv.by_folder) o << "  " << k << "  " << v.files << " files, " << v.bytes << " bytes\n";
    o << "\nBy type folder / bank folder (files two or more levels down):\n";
    for (const auto& [k, v] : inv.by_bank) o << "  " << k << "  " << v.files << " files, " << v.bytes << " bytes\n";
    o << "\nByte-identical duplicates: " << inv.duplicates.size() << " group(s)\n";
    for (const auto& g : inv.duplicates) {
        o << "  -";
        for (const auto& p : g) o << " " << p;
        o << "\n";
    }
    o << "\nSkipped (symlinks and special files, not followed): " << inv.skipped.size() << "\n";
    for (const auto& s : inv.skipped) o << "  " << s << "\n";
    if (isSetLayout(inv.layout)) {   // the Pa5X budget and Media listing are Pa5X facts: not applied to a Pa4X/Pa700-family SET
        const auto stat = [&](const char* t) { const auto it = inv.by_type.find(t); return it == inv.by_type.end() ? ExtStats{} : it->second; };
        o << "\nSample data on disk (.PCM + .KMP file sizes): " << stat("Sample").bytes + stat("Multisample").bytes << " bytes\n"
          << "  No Pa5X memory budget applied: this is a SET (Pa4X/Pa700 family); a Pa5X reads a SET only through its one-way\n"
          << "  Data Conversion (Pa5X manual p.737). The SET's own keyboard limits are not modelled.\n"
          << "\nFile contents were not interpreted. Compare against the keyboard's Media listing (HARDWARE VALIDATION PENDING).\n";
        return o.str();
    }
    o << formatMemoryEstimate(inv, budget);
    o << "\nFile contents were not interpreted. Compare against the Pa5X Media listing "
         "(HARDWARE VALIDATION PENDING).\n";
    return o.str();
}

}  // namespace ai_arranger::media

#endif
