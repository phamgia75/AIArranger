// E8 structural validator for a keyboard library folder (in-repo port of the research prototype
// pa-studio-research/interop/prototypes/kst_struct_check.py, rules R-01..R-14, plus SET rules R-15..R-17 and the
// case-fold collision rule R-18).
//
// FILE LEVEL ONLY: folder and file names, extensions, sizes, an AIArranger placeholder marker and, for a SET, whether a
// bank file opens as the observed Pa-native container. It never interprets a payload, so a clean result does NOT prove
// that a keyboard loads the folder ("not verified on a keyboard").
//
// One rule table per layout (the layout comes from media::detail::detectLayout, as the Library model uses it):
//   - Pa5X KST: Pa5X manual v1.4 (MAN5, printed page) + one KST listing of unknown provenance (docs/research/
//     PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md §3c: 15 type folders, bank sub-folders in five of them) + three owner
//     Pa5X KST working copies (one saved, two converted from SETs by the Pa5X; folder and file names only, 2026-11-14).
//   - Pa4X-family SET: Pa4X manual OS 3.1 (MAN4, PDF page) p.973-975 (file types, Media structure diagram).
//   - Pa700/Pa1000-family SET: OBSERVED in one owner Pa700 SET working copy (folder and extension listing only).
// Every rule names its source; a rule that is not in a layout's table is not run on that layout. No rule is invented:
// where the sources say nothing (bank-folder names, name lengths, KST file content) nothing is checked.
// Read-only: symlinks are reported, never followed; the listing is bounded (kLibraryLimits).
#ifndef AI_ARRANGER_LIBRARY_LIBRARY_VALIDATE_H
#define AI_ARRANGER_LIBRARY_LIBRARY_VALIDATE_H

#include "library/library_model.h"

namespace ai_arranger::library {

enum class Severity { Error, Warn, Info };
inline const char* severityName(Severity s) { return s == Severity::Error ? "ERROR" : s == Severity::Warn ? "WARN" : "INFO"; }

struct Finding { Severity severity; std::string rule, path, message, source; };

struct Validation {
    bool ok = false;           // every entry was listed and read: the findings are complete (false = NOT validated, never a pass)
    std::string error, root, layout, table;
    std::vector<Finding> findings;
    size_t count(Severity s) const { return size_t(std::count_if(findings.begin(), findings.end(), [&](const Finding& f) { return f.severity == s; })); }
};

// The AIArranger synthetic placeholder (tools/make_synthetic_kst_library.py, test fixtures): never a Korg file.
inline constexpr const char* kPlaceholder = "AIARRANGER-SYNTHETIC-NOT-A-KORG-FILE";

struct RuleInfo { Severity severity; const char* source; };
struct RuleTable {
    const char* name;
    const char* suffix;                                  // folder-name suffix (R-01); "" = none checked
    std::map<std::string, std::set<std::string>> types;  // type folder -> extensions seen/documented there (R-03, R-04)
    std::set<std::string> banked;                        // type folders whose files sit in bank sub-folders (R-05)
    std::set<std::string> subFolders;                    // "TYPE/SUB" folders allowed in a bank-file layout (R-05)
    std::set<std::string> topFiles;                      // files allowed at the top level (R-06), upper case
    std::set<std::string> bankFileTypes;                 // SET: type folders whose files are 48-slot bank files (R-16, R-17)
    std::vector<std::string> bankFilePrefixes;           // SET: documented/observed bank file names, PREFIXnn (R-17)
    std::map<std::string, RuleInfo> rules;
    std::set<std::string> typeFiles;                     // "TYPE/NAME" files allowed directly in a banked type folder (R-05), upper case
    std::set<std::string> bankFiles;                     // "TYPE/NAME" files allowed in that type's bank sub-folders whatever the extension (R-04), upper case
};

inline const RuleTable& pa5xKstTable() {
    static const char* obs = "OBSERVED: one KST listing of unknown provenance (audit §3c) + three owner Pa5X KST copies (names only)";
    static const RuleTable t{
        "Pa5X KST (MAN5 + one OBSERVED KST listing)",
        ".KST",
        {{"STYLE", {".STG"}}, {"KEYBOARDSET", {".KSC"}}, {"PAD", {".PDC"}}, {"CHORDSEQUENCE", {".CSC"}}, {"SONGBOOK", {".SBD", ".SBL"}},
         {"SOUND", {".PCG"}}, {"PCM", {".PCM"}}, {"MULTISMP", {".KMP"}}, {"SETTINGS", {".GBL", ".KSC"}}, {"MIDIPRESET", {".MPR"}},
         {"VOICEPRESET", {".VOC"}}, {"GUITARPRESET", {".GTR"}}, {"QTPRESET", {".QTP"}}, {"MAXXAUDIOPRESET", {".MXP"}}, {"MATRIXPRESET", {".MTP"}}},
        {"STYLE", "KEYBOARDSET", "PAD", "CHORDSEQUENCE", "SONGBOOK"},
        {},
        {"CONVERTREPORT.TXT", "CONVERSIONREPORT.TXT"},   // MAN5 p.738 name; the name two Pa5X-converted owner KSTs carry
        {},
        {},
        {{"R-01", {Severity::Error, "MAN5 p.721: a KST is a reserved folder named .KST"}},
         {"R-02", {Severity::Error, "MAN5 p.721: the KST holds the User data"}},
         {"R-03", {Severity::Warn, obs}},
         {"R-04", {Severity::Warn, "MAN5 p.721 extension table + folder pairing OBSERVED (owner KST copies: .MTP, SETTINGS .KSC, bank metadir.json)"}},
         {"R-05", {Severity::Warn, "MAN5 p.731-732 (type, bank, file) + OBSERVED (owner KST copies: SONGBOOK/LISTDB.SBL, empty banks)"}},
         {"R-06", {Severity::Warn, "MAN5 p.738 ConvertReport.txt; OBSERVED ConversionReport.txt in Pa5X-converted owner KSTs"}},
         {"R-07", {Severity::Error, "an empty file is no resource (file level)"}},
         {"R-08", {Severity::Warn, "MAN5 p.1174/p.1936: 4 GB User Sample memory (disk size here, not RAM)"}},
         {"R-09", {Severity::Error, "FAT32/exFAT naming rule (general file-system rule, not Korg-specific)"}},
         {"R-10", {Severity::Error, "AIArranger's own synthetic placeholder marker"}},
         {"R-11", {Severity::Warn, "macOS metadata (general rule, not Korg-specific)"}},
         {"R-12", {Severity::Warn, "AIArranger safety rule: links are never followed"}},
         {"R-13", {Severity::Info, "MAN5 p.736: loading the whole KST deletes the User Samples in memory"}},
         {"R-14", {Severity::Warn, "MAN5 p.736 (User Sample Sounds) + OBSERVED MULTISMP folder"}},
         {"R-18", {Severity::Error, "FAT32/exFAT names are case-insensitive (general file-system rule, not Korg-specific)"}}},
        {"SONGBOOK/LISTDB.SBL"},   // OBSERVED beside the SONGDB.SBD bank folder in all three owner KSTs
        {"STYLE/METADIR.JSON", "KEYBOARDSET/METADIR.JSON"}};   // OBSERVED only there (38 + 27 banks of the owner KST); content not read
    return t;
}

// Pa4X SET: MAN4 PDF p.975 "Media structure" (MYDATA.SET: GLOBAL, KEYBOARDSET, SOUND, MULTISMP + PCM, STYLE, PAD,
// SONGBOOK, VOICEPRESET) and p.973-974 (file types).
inline const RuleTable& pa4xSetTable() {
    static const char* d = "MAN4 PDF p.975 (Media structure)";
    static const RuleTable t{
        "Pa4X-family SET (MAN4 PDF p.973-975)",
        ".SET",
        {{"GLOBAL", {".GBL", ".MXP", ".MPR", ".QTP"}}, {"KEYBOARDSET", {".PRF"}}, {"SOUND", {".PCG"}}, {"MULTISMP", {".KMP"}},
         {"PCM", {".PCM"}}, {"STYLE", {".STY"}}, {"PAD", {".PAD"}}, {"SONGBOOK", {".SBD", ".SBL"}}, {"VOICEPRESET", {".VOC"}}},
        {},
        {},
        {},
        {"STYLE", "KEYBOARDSET", "PAD"},
        {"BANK", "USER", "FAVORITE"},
        {{"R-01", {Severity::Warn, "MAN4 PDF p.973: SET is a reserved folder (.SET); a Pa700-family SET was OBSERVED without the suffix"}},
         {"R-02", {Severity::Error, "MAN4 PDF p.973: the SET holds the User data"}},
         {"R-03", {Severity::Warn, d}},
         {"R-04", {Severity::Warn, "MAN4 PDF p.973-975 (file types per folder)"}},
         {"R-05", {Severity::Warn, "MAN4 PDF p.975: banks are files (.STY/.PRF/.PAD) directly in the type folder"}},
         {"R-06", {Severity::Warn, d}},
         {"R-07", {Severity::Error, "an empty file is no resource (file level)"}},
         {"R-09", {Severity::Error, "MAN4 PDF p.972: FAT16/FAT32 devices (FAT naming rule)"}},
         {"R-10", {Severity::Error, "AIArranger's own synthetic placeholder marker"}},
         {"R-11", {Severity::Warn, "macOS metadata (general rule, not Korg-specific)"}},
         {"R-12", {Severity::Warn, "AIArranger safety rule: links are never followed"}},
         {"R-13", {Severity::Info, "MAN5 p.737: a Pa5X reads a SET only through its one-way Data Conversion"}},
         {"R-14", {Severity::Warn, "MAN4 PDF p.975: RAM.KMP goes with the RAMnn.PCM samples"}},
         {"R-15", {Severity::Info, "OBSERVED on one Pa700 SET only: Pa4X files may differ (UNKNOWN)"}},
         {"R-16", {Severity::Error, "MAN4 PDF p.975: 48 Styles / Keyboard Sets / Pads per bank file"}},
         {"R-17", {Severity::Warn, "MAN4 PDF p.975: bank files BANKnn / USERnn / FAVORITEnn"}},
         {"R-18", {Severity::Error, "MAN4 PDF p.972: FAT16/FAT32 devices (FAT names are case-insensitive)"}}}};
    return t;
}

// Pa700/Pa1000 SET: what one owner Pa700 SET holds (folder and extension listing; no names or bytes kept here).
inline const RuleTable& pa700SetTable() {
    static const char* obs = "OBSERVED: one owner Pa700 SET (folders and extensions only)";
    static const RuleTable t{
        "Pa700/Pa1000-family SET (OBSERVED, one owner Pa700 SET; MAN4 where it agrees)",
        "",
        {{"GLOBAL", {".GBL", ".MPR", ".QTP", ".AUD"}}, {"KEYBOARDSET", {".PRF"}}, {"SOUND", {".PCG"}}, {"MULTISMP", {".KMP"}},
         {"PCM", {".PCM"}}, {"STYLE", {".STY"}}, {"PAD", {".PAD"}}, {"SONGBOOK", {".SBD", ".SBL"}}, {"VOICEPRESET", {".VOC"}},
         {"GUITARPRESET", {}}},
        {},
        {"SONGBOOK/SONGDB.SBD"},   // a folder of SongBook bank files on the Pa700 (a file in MAN4's diagram)
        {},
        {"STYLE", "KEYBOARDSET", "PAD"},
        {"USER"},
        {{"R-02", {Severity::Error, "MAN4 PDF p.973: the SET holds the User data"}},
         {"R-03", {Severity::Warn, obs}},
         {"R-04", {Severity::Warn, obs}},
         {"R-05", {Severity::Warn, obs}},
         {"R-06", {Severity::Warn, obs}},
         {"R-07", {Severity::Error, "an empty file is no resource (file level)"}},
         {"R-09", {Severity::Error, "MAN4 PDF p.972: FAT16/FAT32 devices (FAT naming rule)"}},
         {"R-10", {Severity::Error, "AIArranger's own synthetic placeholder marker"}},
         {"R-11", {Severity::Warn, "macOS metadata (general rule, not Korg-specific)"}},
         {"R-12", {Severity::Warn, "AIArranger safety rule: links are never followed"}},
         {"R-13", {Severity::Info, "MAN5 p.737: a Pa5X reads a SET only through its one-way Data Conversion"}},
         {"R-14", {Severity::Warn, "MAN4 PDF p.975 + OBSERVED: RAM.KMP goes with the RAMnn.PCM samples"}},
         {"R-15", {Severity::Warn, "OBSERVED 142/142 files of one Pa700 SET (PA_NATIVE_CONTAINER_NOTES.md §1)"}},
         {"R-16", {Severity::Error, "MAN4 PDF p.975: 48 Styles / Keyboard Sets / Pads per bank file"}},
         {"R-17", {Severity::Warn, "OBSERVED: user bank files USERnn (MAN4 p.975 also lists BANKnn / FAVORITEnn)"}},
         {"R-18", {Severity::Error, "MAN4 PDF p.972: FAT16/FAT32 devices (FAT names are case-insensitive)"}}}};
    return t;
}

inline const RuleTable* tableFor(const std::string& layout) {
    if (layout == media::kLayoutKst) return &pa5xKstTable();
    if (layout == media::kLayoutPa4x) return &pa4xSetTable();
    if (layout == media::kLayoutPa700) return &pa700SetTable();
    return nullptr;
}

namespace detail {
inline bool fatBad(const std::string& n) {
    for (unsigned char c : n) if (c < 32 || std::string("\\/:*?\"<>|").find(char(c)) != std::string::npos) return true;
    return false;
}
inline bool macMeta(const std::string& n) {
    return n.rfind("._", 0) == 0 || n == ".DS_Store" || n == ".Spotlight-V100" || n == ".fseventsd" || n == ".Trashes";
}
// 1 = starts with the placeholder marker, 0 = does not, -1 = cannot be read (the validation is then incomplete).
inline int placeholder(const fs::path& p) {
    std::FILE* f = media::detail::openRegular(AT_FDCWD, p.c_str());   // no symlink followed; bounded read
    if (!f) return -1;
    char b[64] = {};
    const size_t n = std::fread(b, 1, std::strlen(kPlaceholder), f);
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err) return -1;
    return n == std::strlen(kPlaceholder) && std::memcmp(b, kPlaceholder, n) == 0;
}
// Relative paths that equal an earlier one once letter case is folded over the WHOLE path (so a file below two folders
// that differ only in case collides too): later path -> the first one, in sorted order. ASCII folding (FAT upper-cases).
inline std::map<std::string, std::string> caseCollisions(std::vector<std::string> all) {
    std::sort(all.begin(), all.end());
    std::map<std::string, std::string> first, r;
    for (const auto& rel : all)
        if (const auto [it, fresh] = first.emplace(media::detail::upper(rel), rel); !fresh) r[rel] = it->second;
    return r;
}
// PREFIXnn (two digits), case-insensitive.
inline bool bankFileName(const std::string& stem, const std::vector<std::string>& prefixes) {
    const std::string s = media::detail::upper(stem);
    for (const auto& p : prefixes)
        if (s.size() == p.size() + 2 && s.compare(0, p.size(), p) == 0 && std::isdigit((unsigned char)s[p.size()]) && std::isdigit((unsigned char)s[p.size() + 1])) return true;
    return false;
}
}  // namespace detail

// Validates the folder `root` against the rule table of its detected layout. Read-only.
inline Validation validate(const fs::path& root, const std::function<bool()>& keepGoing = {}, const media::detail::ListLimits& limits = kLibraryLimits) {
    Validation v;
    v.root = root.string();
    media::detail::Listing ls;
    if (!media::detail::listFiles(root, ls, v.error, keepGoing, limits)) return v;
    v.layout = media::detail::detectLayout(root, ls);
    const RuleTable* t = tableFor(v.layout);
    if (!t) {
        v.table = "none";
        v.findings.push_back({Severity::Error, "R-01", ".", "no rule table: layout " + v.layout + "; name the folder .KST or .SET if you know which it is",
                              "MAN5 p.721 (.KST) / MAN4 PDF p.973-975 (.SET)"});
        v.ok = true;
        return v;
    }
    v.table = t->name;
    auto add = [&](const char* rule, const std::string& path, const std::string& msg) {
        const auto r = t->rules.find(rule);
        if (r != t->rules.end()) v.findings.push_back({r->second.severity, rule, path, msg, r->second.source});
    };
    const std::string up = media::detail::upper(media::folderPath(fs::absolute(root)).filename().string());
    if (*t->suffix && !(up.size() > std::strlen(t->suffix) && up.compare(up.size() - std::strlen(t->suffix), std::string::npos, t->suffix) == 0))
        add("R-01", ".", std::string("folder name does not end in ") + t->suffix);
    // names of every entry (folders, files, skipped)
    std::vector<std::string> all(ls.dirs);
    for (const auto& [rel, e] : ls.files) all.push_back(rel);
    all.insert(all.end(), ls.skipped.begin(), ls.skipped.end());
    std::sort(all.begin(), all.end());
    for (const auto& rel : all) {
        const std::string n = fs::path(rel).filename().string();
        if (detail::fatBad(n)) add("R-09", rel, "character not allowed on a FAT/exFAT USB stick");
        if (detail::macMeta(n)) add("R-11", rel, "macOS metadata entry; delete it before copying to the stick");
    }
    for (const auto& [rel, was] : detail::caseCollisions(all))
        add("R-18", rel, "differs from " + was + " only in letter case: one name on a FAT/exFAT stick, one would replace the other");
    for (const auto& s : ls.skipped) add("R-12", s, "symlink or special file, not followed");
    for (const auto& d : ls.dirs) {
        std::vector<std::string> parts;
        for (const auto& c : fs::path(d)) parts.push_back(media::detail::upper(c.string()));
        if (detail::macMeta(fs::path(d).filename().string()) || (parts.size() > 1 && detail::macMeta(fs::path(d).begin()->string()))) continue;
        const std::string type = parts[0];
        if (parts.size() == 1 && !t->types.count(type)) add("R-03", d, "unknown type folder (not in this layout's reserved set)");
        if (parts.size() == 2 && !t->banked.count(type) && !t->subFolders.count(type + "/" + parts[1]))
            add("R-05", d, t->banked.empty() ? "sub-folder in a type folder (banks are files here)" : "grid-type folder has sub-folders (observed: files directly inside)");
        if (parts.size() > 2) add("R-05", d, "folder deeper than type/bank");
    }
    std::map<std::string, uint32_t> has;
    uintmax_t sampleBytes = 0;
    for (const auto& [rel, e] : ls.files) {
        if (keepGoing && !keepGoing()) { v.error = "cancelled"; return v; }
        const fs::path p(rel);
        const std::string f = p.filename().string();
        if (detail::macMeta(f)) continue;
        std::vector<std::string> parts;
        for (const auto& c : p) parts.push_back(media::detail::upper(c.string()));
        const std::string ext = media::detail::upper(p.extension().string());
        if (e.size == 0) add("R-07", rel, "empty file");
        const int ph = detail::placeholder(e.abs);
        if (ph < 0) { v.error = "cannot read " + rel + ": validation incomplete"; return v; }   // ok stays false: never a pass
        if (ph) add("R-10", rel, "AIArranger synthetic placeholder bytes: never load this on a keyboard");
        if (parts.size() == 1) {
            if (!t->topFiles.count(parts[0])) add("R-06", rel, "loose file at the top level");
            continue;
        }
        const std::string type = parts[0];
        ++has[type];
        if (ext == ".PCM" || ext == ".KMP") sampleBytes += e.size;
        const auto te = t->types.find(type);
        const bool bankMeta = parts.size() == 3 && t->banked.count(type) && t->bankFiles.count(type + "/" + parts[2]);
        if (te != t->types.end() && !te->second.count(ext) && !bankMeta) {
            std::string want;
            for (const auto& x : te->second) want += (want.empty() ? "" : "/") + x;
            add("R-04", rel, "extension " + (ext.empty() ? std::string("(none)") : ext) + " not expected in " + type + " (expected " + (want.empty() ? "no files" : want) + ")");
        }
        if (t->banked.count(type) && parts.size() == 2 && !t->typeFiles.count(type + "/" + parts[1])) add("R-05", rel, "file directly in a bank-organized type folder (observed: inside a bank sub-folder)");
        const bool expected = te != t->types.end() && te->second.count(ext);
        if (expected && t->bankFileTypes.count(type) && parts.size() == 2 && !detail::bankFileName(p.stem().string(), t->bankFilePrefixes)) {
            std::string want;
            for (const auto& x : t->bankFilePrefixes) want += (want.empty() ? "" : "/") + x + "nn";
            add("R-17", rel, "bank file name is not " + want);
        }
        if (expected && t->rules.count("R-15")) {
            const auto c = media::panative::readFile(e.abs);   // bounded (panative::kMaxFileBytes), no symlink followed
            if (c.ioError) { v.error = "cannot read " + rel + " (" + c.error + "): validation incomplete"; return v; }
            if (!c.ok) add("R-15", rel, c.ioError ? "unreadable: " + c.error : c.wrapper ? "INVALID Pa-native container: " + c.error : "no Pa-native wrapper");
            else if (t->bankFileTypes.count(type))
                for (const auto& it : c.items)
                    if (it.slot >= 48) { add("R-16", rel, "slot " + std::to_string(it.slot + 1) + " is past the 48 slots of a bank file"); break; }
        }
    }
    if (has.count("PCM")) {
        add("R-13", "PCM", t == &pa5xKstTable() ? "PCM present: loading this WHOLE KST deletes all User Samples in memory"
                                                 : "PCM present: this SET holds User Samples");
        if (!has.count("MULTISMP")) add("R-14", "PCM", "samples present but no MULTISMP/*.KMP: nothing seen that maps them");
    }
    if (sampleBytes > (uintmax_t(4) << 30)) add("R-08", ".", "PCM+KMP disk bytes " + std::to_string(sampleBytes) + " exceed 4 GB User Sample memory (disk size, not RAM)");
    if (has.empty()) add("R-02", ".", "no files inside any type folder");
    v.ok = true;
    return v;
}

inline std::string validationText(const Validation& v) {
    std::ostringstream o;
    o << "Structural validation (file level, read-only): " << v.root << "\n";
    if (!v.ok) { o << "NOT validated: " << v.error << "\n"; return o.str(); }
    o << "Layout: " << v.layout << "\nRule table: " << v.table << "\n";
    for (const auto& f : v.findings) o << "  " << severityName(f.severity) << "  " << f.rule << "  " << f.path << ": " << f.message << "  [" << f.source << "]\n";
    o << v.count(Severity::Error) << " error(s), " << v.count(Severity::Warn) << " warning(s), " << v.count(Severity::Info) << " info. "
      << "File-level check only: content not decoded; not verified on a keyboard.\n";
    return o.str();
}

}  // namespace ai_arranger::library

#endif
