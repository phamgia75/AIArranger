// P1.9 KST inventory: a read-only report on a Pa5X KST folder (or any folder).
// File contents are opaque: only paths, extensions, sizes and byte equality are
// used. Never writes, never follows symlinks.
#ifndef AI_ARRANGER_MEDIA_KST_INVENTORY_H
#define AI_ARRANGER_MEDIA_KST_INVENTORY_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
    std::vector<std::vector<std::string>> duplicates;  // byte-identical groups (relative paths, sorted)
    std::vector<std::string> skipped;            // symlinks, special files, unreadable entries
    std::map<std::string, ExtStats> by_type;     // documented Pa5X file type (manual v1.4 p.721-722)
    bool has_pcm_folder = false;                 // a folder named PCM: the KST holds User Samples (p.736)
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
inline bool sameBytes(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    if (!x || !y) return false;
    char bx[65536], by[65536];
    for (;;) {
        x.read(bx, sizeof bx);
        y.read(by, sizeof by);
        if (x.gcount() != y.gcount() || !std::equal(bx, bx + x.gcount(), by)) return false;
        if (x.gcount() == 0 || !x) return x.eof() && y.eof();
    }
}
}  // namespace detail

inline Inventory scan(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    Inventory inv;
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(root, ec))) {
        inv.error = "not a directory (symlinks are not followed): " + root.string();
        return inv;
    }
    struct F { fs::path abs; std::string rel; uintmax_t size; };
    std::vector<F> all;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) { inv.error = "cannot open: " + ec.message(); return inv; }
    for (; it != end; it.increment(ec)) {
        if (ec) { inv.skipped.push_back("(iteration error) " + ec.message()); ec.clear(); continue; }
        const auto st = it->symlink_status(ec);
        const std::string rel = it->path().lexically_relative(root).generic_string();  // not fs::relative: it resolves symlinks
        if (fs::is_directory(st)) {
            std::string n = it->path().filename().string();
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::toupper(c); });
            if (n == "PCM") inv.has_pcm_folder = true;
            continue;
        }
        if (!fs::is_regular_file(st)) { inv.skipped.push_back(rel); continue; }  // symlink, fifo, ...
        const uintmax_t size = it->file_size(ec);
        if (ec) { inv.skipped.push_back(rel); ec.clear(); continue; }
        all.push_back({it->path(), rel, size});
    }
    std::sort(all.begin(), all.end(), [](const F& a, const F& b) { return a.rel < b.rel; });

    std::map<std::pair<uintmax_t, uint64_t>, std::vector<const F*>> candidates;
    std::map<uintmax_t, std::vector<const F*>> bySize;
    for (const auto& f : all) {
        ++inv.files;
        inv.bytes += f.size;
        std::string ext = fs::path(f.rel).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::toupper(c); });
        auto& e = inv.by_ext[ext.empty() ? "(none)" : ext];
        ++e.files; e.bytes += f.size;
        auto& ty = inv.by_type[documentedType(ext)];
        ++ty.files; ty.bytes += f.size;
        const auto slash = f.rel.find('/');
        auto& d = inv.by_folder[slash == std::string::npos ? "." : f.rel.substr(0, slash)];
        ++d.files; d.bytes += f.size;
        bySize[f.size].push_back(&f);
    }
    // Hash only files that share a size, then confirm every match byte by byte.
    for (const auto& [size, group] : bySize) {
        if (group.size() < 2) continue;
        std::map<uint64_t, std::vector<std::vector<const F*>>> byHash;
        for (const F* f : group) {
            uint64_t h = 0;
            if (!detail::hashFile(f->abs, h)) { inv.skipped.push_back(f->rel); continue; }
            auto& sets = byHash[h];
            auto s = std::find_if(sets.begin(), sets.end(),
                                  [&](const auto& set) { return detail::sameBytes(set.front()->abs, f->abs); });
            if (s == sets.end()) sets.push_back({f}); else s->push_back(f);
        }
        for (const auto& [h, sets] : byHash)
            for (const auto& set : sets)
                if (set.size() > 1) {
                    std::vector<std::string> g;
                    for (const F* f : set) g.push_back(f->rel);
                    inv.duplicates.push_back(g);
                }
    }
    std::sort(inv.duplicates.begin(), inv.duplicates.end());
    std::sort(inv.skipped.begin(), inv.skipped.end());
    inv.ok = true;
    return inv;
}

inline std::string format(const Inventory& inv, const std::string& label) {
    std::ostringstream o;
    o << "KST inventory (read-only): " << label << "\n";
    if (!inv.ok) { o << "ERROR: " << inv.error << "\n"; return o.str(); }
    o << "Files: " << inv.files << ", bytes: " << inv.bytes << "\n\nBy extension:\n";
    for (const auto& [k, v] : inv.by_ext) o << "  " << k << "  " << v.files << " files, " << v.bytes << " bytes\n";
    o << "\nBy documented Pa5X file type (manual v1.4 p.721; identification by extension only):\n";
    for (const auto& [k, v] : inv.by_type) o << "  " << k << "  " << v.files << " files, " << v.bytes << " bytes\n";
    o << "\nPCM folder: " << (inv.has_pcm_folder
          ? "YES. This KST holds User Samples: loading the WHOLE KST on a Pa5X deletes the User Samples in memory "
            "(manual p.736); load single Sounds to keep them."
          : "no") << "\n";
    o << "\nBy top-level folder:\n";
    for (const auto& [k, v] : inv.by_folder) o << "  " << k << "  " << v.files << " files, " << v.bytes << " bytes\n";
    o << "\nByte-identical duplicates: " << inv.duplicates.size() << " group(s)\n";
    for (const auto& g : inv.duplicates) {
        o << "  -";
        for (const auto& p : g) o << " " << p;
        o << "\n";
    }
    o << "\nSkipped (not regular files or unreadable): " << inv.skipped.size() << "\n";
    for (const auto& s : inv.skipped) o << "  " << s << "\n";
    o << "\nFile contents were not interpreted. Compare against the Pa5X Media listing "
         "(HARDWARE VALIDATION PENDING).\n";
    return o.str();
}

}  // namespace ai_arranger::media

#endif
