#ifndef AI_ARRANGER_JOBS_IMPORT_KIT_H
#define AI_ARRANGER_JOBS_IMPORT_KIT_H

// E9 Pa5X Import Kit: AIArranger outputs that a Pa5X imports through its DOCUMENTED import pages, gathered into ONE new
// folder for a USB stick, with the owner's import steps (KIT.md), a machine manifest (manifest.json) and PACKAGE.sha256.
// Page numbers: Pa5X User Manual v1.4, printed pages ("MAN").
//
//   SOUNDS/<package>/    Sound package: TX Sound (TXPROG) import, alternative SoundFont (SF2)   MAN p.1206, 1207, 1209, 1210
//   DRUMKITS/<package>/  Drum Kit package: TX DrumKit (TXPERF) import                          MAN p.1206, 1210
//   KMP/<folder>/        KMP/KSF folder (EXPERIMENTAL): KORG Multisample (KMP) import            MAN p.1206, 1209
//   STYLES/<name>.mid    marker SMF Style (with the user's sound map): Style Edit Import SMF   MAN p.1037
//
// Each member is copied byte for byte; nothing is converted, and no Korg-native (KST/STG/PCG/...) file is written. Every
// member is checked by AIArranger's independent readers on the source BEFORE the copy and on the copy AFTER it (package
// manifest + package reader + SF2 reader; KMP/KSF reader reading every KSF in full; Korg Style SMF reader). Any failure
// fails the whole kit (fail closed). The best label a kit can carry is kLabel: structurally valid, never "compatible".
// Written only through the E6 job runner (export_job.h): NAME.INCOMPLETE, checks on the staged files, one no-replace
// rename; sources are only read.

#include "jobs/export_job.h"
#include "media/kst_inventory.h"
#include "media/sf2_check.h"
#include "media/sound/korg_kmp_source.h"
#include "media/sound/package_verify.h"
#include "ui/studio/style_studio_model.h"
#include <atomic>
#include <fcntl.h>
#include <set>
#include <unistd.h>

namespace ai_arranger::jobs::kit {

inline constexpr const char* kLabel = "STRUCTURALLY VALID — NOT VERIFIED ON A PA5X";
inline constexpr const char* kValid = "STRUCTURALLY VALID";
inline constexpr const char* kSchema = "aiarranger-pa5x-import-kit/1";

enum class Kind { Unknown, Sound, DrumKit, Kmp, Style };
inline const char* typeFolder(Kind k) {
    switch (k) {
        case Kind::Sound: return "SOUNDS";
        case Kind::DrumKit: return "DRUMKITS";
        case Kind::Kmp: return "KMP";
        case Kind::Style: return "STYLES";
        default: return "";
    }
}
inline const char* kindName(Kind k) {
    switch (k) {
        case Kind::Sound: return "Sound (TXPROG, SF2)";
        case Kind::DrumKit: return "Drum Kit (TXPERF)";
        case Kind::Kmp: return "Multisamples (KMP/KSF, EXPERIMENTAL)";
        case Kind::Style: return "Style (marker SMF)";
        default: return "unknown";
    }
}

struct Item {
    Kind kind = Kind::Unknown;
    fs::path source;                      // only read
    std::string name;                     // the source's own leaf name, kept in the kit
    std::vector<std::string> select, alt; // member-relative files the owner selects on the Pa5X (alt: the SF2 route)
    std::string status, detail;           // kValid or "FAILED"; the readers that passed, or why it failed
    std::string kitPath() const { return std::string(typeFolder(kind)) + "/" + name; }
};

namespace detail {
inline std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
inline bool finderLitter(const std::string& rel) {   // added by Finder on a FAT stick; never copied, never expected
    const std::string leaf = fs::path(rel).filename().string();
    return leaf == ".DS_Store" || leaf.rfind("._", 0) == 0;
}
// Files in `dir` (not recursive) with extension `ext` (lower case), as names relative to `base`, sorted.
inline std::vector<std::string> withExt(const fs::path& dir, const std::string& ext, const fs::path& base) {
    std::vector<std::string> r;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && !it->is_symlink(ec) && lower(it->path().extension().string()) == ext && !finderLitter(it->path().string()))
            r.push_back(it->path().lexically_relative(base).generic_string());
    std::sort(r.begin(), r.end());
    return r;
}
// TX files of a package: tx/<project>/<project>.<ext>
inline std::vector<std::string> txFiles(const fs::path& pkg, const std::string& ext) {
    std::vector<std::string> r;
    std::error_code ec;
    for (fs::directory_iterator it(pkg / "tx", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && !it->is_symlink(ec)) for (auto& f : withExt(it->path(), ext, pkg)) r.push_back(f);
    std::sort(r.begin(), r.end());
    return r;
}
// Why one path component may not be written on the stick, or "". Pa5X: FAT32/FAT16 with long file names (MAN p.715).
// The FAT long-name rules (general file-system rules, not Pa5X-specific): at most 255 characters (counted here as UTF-8
// bytes, which is never fewer), none of \ / : * ? " < > | or control characters, no trailing space or dot, not a
// reserved device name. Name limits of the Pa5X itself (Sound/Style names, path depth) are UNKNOWN (not in MAN).
inline std::string fatName(const std::string& c) {
    if (c.empty() || c == "." || c == "..") return "an empty or dot name";
    if (c.size() > 255) return "longer than 255 characters (FAT long file name limit)";
    for (unsigned char ch : c)
        if (ch < 0x20 || std::strchr("\\/:*?\"<>|", ch)) return "a character FAT does not allow (\\ / : * ? \" < > | or a control character)";
    if (c.back() == ' ' || c.back() == '.') return "a trailing space or dot (FAT drops it)";
    std::string stem = c.substr(0, c.find('.'));
    for (auto& ch : stem) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    static const std::set<std::string> dev = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                              "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (dev.count(stem)) return "a reserved device name";
    return "";
}
inline std::string fatPath(const std::string& rel) {
    for (const auto& c : fs::path(rel)) if (const auto e = fatName(c.string()); !e.empty()) return "'" + rel + "' has " + e;
    return "";
}
}  // namespace detail

// What one AIArranger output is, from its files (read-only). Unknown when it is none of the four.
inline Item classify(const fs::path& p) {
    Item it;
    it.source = p;
    it.name = media::folderPath(p).filename().string();
    std::error_code ec;
    const auto st = fs::symlink_status(p, ec);
    if (fs::is_regular_file(st)) {
        if (detail::lower(p.extension().string()) == ".mid") { it.kind = Kind::Style; it.select = {""}; }
        return it;
    }
    if (!fs::is_directory(st)) return it;
    if (media::sound::isKorgFolder(p)) { it.kind = Kind::Kmp; it.select = detail::withExt(p, ".kmp", p); return it; }
    if (auto perf = detail::txFiles(p, ".txperf"); !perf.empty()) { it.kind = Kind::DrumKit; it.select = perf; return it; }
    if (auto prog = detail::txFiles(p, ".txprog"); !prog.empty()) { it.kind = Kind::Sound; it.select = prog; it.alt = detail::withExt(p, ".sf2", p); }
    return it;
}

// The independent readers on one member at `p` (the source, or its copy in the kit). "" = structurally valid; *detail
// says which readers passed. Read-only.
inline std::string checkMember(Kind k, const fs::path& p, std::string* detail) {
    namespace snd = media::sound;
    std::error_code ec;
    if (k == Kind::Style) {
        const auto v = ui::studio::StyleStudioModel::checkStyleSmf(p);
        if (v.l1.state != Check::Passed) return "Korg Style SMF reader: " + v.l1.detail;
        *detail = "Korg Style SMF reader and validator (" + v.l1.detail + ")";
        return "";
    }
    if (k == Kind::Sound || k == Kind::DrumKit) {
        const auto c = snd::verifyPackage(p);
        if (!c.ok) return c.text;
        std::string sf2;
        for (const auto& rel : detail::withExt(p, ".sf2", p)) {
            const auto r = media::sf2::checkFile((p / rel).string());
            if (!r.ok) return rel + ": SF2 reader: " + r.error;
            if (r.overlapping()) return rel + ": SF2 reader: " + std::to_string(r.overlapping()) + " instrument(s) with overlapping zones, which the Pa5X may not import (MAN p.1206)";
            sf2 += ", SF2 reader on " + rel + " (" + std::to_string(r.instruments.size()) + " instrument(s), no overlapping zones)";
        }
        *detail = "PACKAGE.sha256 (every file, nothing extra) and the package reader (RESOURCES.tsv, every WAV, TX references)" + sf2;
        return "";
    }
    if (k == Kind::Kmp) {
        size_t files = 0;
        const auto bad = media::verifyManifest(p, p / snd::pkgio::FileSet::kPackageManifest, {}, &files,
                                               [](const std::string& rel) { return rel == snd::pkgio::FileSet::kPackageManifest || detail::finderLitter(rel); }, true);
        if (!bad.empty()) return "PACKAGE.sha256: " + bad;
        std::vector<snd::KorgMultisample> ms;
        if (const auto e = snd::scanKorgFolder(p, ms); !e.empty()) return "KMP/KSF reader: " + e;
        std::set<fs::path> ksfs;
        for (const auto& m : ms) for (const auto* v : {&m.ksf, &m.ksfR}) for (const auto& f : *v) if (!f.empty()) ksfs.insert(f);
        for (const auto& f : ksfs) {   // every KSF read in full (audio included), not only its header
            std::string err;
            const auto b = snd::korg::slurp(f, &err);
            if (!err.empty()) return "KMP/KSF reader: " + err;
            snd::KsfHead h;
            snd::Sample s;
            if (const auto e = snd::readKsf(b, &h, &s, f); !e.empty()) return "KMP/KSF reader: " + f.filename().string() + ": " + e;
        }
        *detail = "PACKAGE.sha256 (" + std::to_string(files) + " files) and the KMP/KSF reader (" + std::to_string(ms.size()) + " multisample(s), " +
                  std::to_string(ksfs.size()) + " KSF read in full)";
        return "";
    }
    return "not an AIArranger output the kit takes (a Sound or Drum Kit package folder, a KMP/KSF folder, or a Style .mid)";
}

// One file to copy: `from` is relative to the member's pinned source folder (a Style: the folder holding the .mid).
struct Planned { size_t item = 0; std::string from, to; uint64_t bytes = 0; };
inline constexpr uint64_t kFat32MaxFile = (uint64_t(1) << 32) - 1;   // FAT32: a file is at most 4 GiB - 1 byte

// Every file of a member. Refuses links and special files; Finder litter is not copied.
inline std::string memberFiles(const Item& it, size_t idx, std::vector<Planned>* out) {
    std::error_code ec;
    if (it.kind == Kind::Style) {
        const auto n = fs::file_size(it.source, ec);
        if (ec) return "cannot read the size of " + it.source.string();
        out->push_back({idx, it.source.filename().string(), it.kitPath(), n});
        return "";
    }
    media::detail::Listing L;
    std::string err;
    if (!media::detail::listFiles(it.source, L, err)) return it.source.string() + ": " + err;
    if (!L.skipped.empty()) return it.source.string() + ": refusing the link or special file " + L.skipped.front();
    for (const auto& [rel, e] : L.files) if (!detail::finderLitter(rel)) out->push_back({idx, rel, it.kitPath() + "/" + rel, e.size});
    return "";
}

// The folder every file of a member is read from, opened once and kept open (P1, Codex r1): a member folder itself
// (no symlink), or for a Style the folder holding the .mid. Files are later opened below this descriptor one component
// at a time without following any link, so a file or folder swapped for a link after validation cannot be read.
inline std::string pinRoot(const Item& it, std::shared_ptr<media::detail::Fd>* fd) {
    const fs::path dir = it.kind == Kind::Style ? (it.source.has_parent_path() ? it.source.parent_path() : fs::path(".")) : it.source;
    *fd = std::make_shared<media::detail::Fd>(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | (it.kind == Kind::Style ? 0 : O_NOFOLLOW)));
    struct stat a {}, b {};
    if ((*fd)->v < 0 || ::fstat((*fd)->v, &a) != 0) return "cannot open " + dir.string() + " (missing, or a link)";
    // the member folder itself must not be a link; the folder holding a Style may be reached through one (e.g. /tmp)
    if ((it.kind == Kind::Style ? ::stat(dir.c_str(), &b) : ::lstat(dir.c_str(), &b)) != 0 || a.st_dev != b.st_dev || a.st_ino != b.st_ino) return dir.string() + " changed while it was opened";
    return "";
}

// Copies `from` (below the pinned folder `root`, no link followed at any level) to the new file `to` of `set` (created
// exclusively below its pinned root, no link followed: FileSet::createFile) with a bounded buffer, hashing exactly the
// bytes read. Refuses a file above the FAT32 limit. "" = copied; *bytes and *sha256 set.
inline std::string copyPinned(int root, const std::string& from, media::sound::pkgio::FileSet& set, const std::string& to, uint64_t* bytes,
                              std::string* sha256) {
    const auto [dir, leaf] = media::detail::splitRel(from);
    media::detail::Fd d(dir.empty() ? ::dup(root) : media::detail::openDirBelow(root, dir));
    std::FILE* in = d.v < 0 ? nullptr : media::detail::openRegular(d.v, leaf.c_str());
    if (!in) return from + ": cannot open it without following a link (missing, swapped for a link, or not a regular file)";
    media::detail::Fd out(set.createFile(to));
    if (out.v < 0) { std::fclose(in); return "cannot create " + to + " in the kit (it exists, or a folder on its way is a link)"; }
    media::Sha256 h;
    std::vector<char> buf(1 << 20);
    uint64_t n = 0;
    std::string why;
    for (size_t got; why.empty() && (got = std::fread(buf.data(), 1, buf.size(), in)) > 0;) {
        if ((n += got) > kFat32MaxFile) { why = from + " is 4 GiB or more, above the FAT32 file size limit"; break; }
        h.update(buf.data(), got);
        for (size_t w = 0; w < got;) {
            const ssize_t k = ::write(out.v, buf.data() + w, got - w);
            if (k <= 0) { why = "write failed for " + to; break; }
            w += size_t(k);
        }
    }
    if (why.empty() && std::ferror(in)) why = "read error in " + from;
    std::fclose(in);
    if (!why.empty()) return why;
    const int closed = ::close(out.v);
    out.v = -1;
    if (closed != 0) return "write failed for " + to;
    *bytes = n;
    *sha256 = h.hex();
    return "";
}

// FAT ignores case (E9 Codex r2): every path in the kit AND every folder on its way must be unique without case, a name
// may not be a file in one place and a folder in another, and one folder may not be spelled two ways. `files`: every
// file the kit will hold, relative. "" or the first collision. Pure (no disk access), so it is testable on any volume.
inline std::string collisionRefusal(const std::vector<std::string>& files) {
    std::map<std::string, std::pair<std::string, bool>> seen;   // folded prefix -> (as written, is a folder)
    for (const auto& f : files)
        for (size_t at = 0;;) {
            const size_t slash = f.find('/', at);
            const bool dir = slash != std::string::npos;
            const std::string p = dir ? f.substr(0, slash) : f;
            const auto [it, fresh] = seen.try_emplace(detail::lower(p), p, dir);
            if (!fresh) {
                if (it->second.second != dir) return "'" + p + "' and '" + it->second.first + "' would be a file and a folder of the same name (FAT ignores case)";
                if (!dir) return "two files would both be " + p + " (FAT ignores case)";
                if (it->second.first != p) return "two folders would both be " + p + " and " + it->second.first + " (FAT ignores case)";
            }
            if (!dir) break;
            at = slash + 1;
        }
    return "";
}

// Every kit-relative path the kit will hold, checked for the stick: FAT names, the FAT32 4 GiB file limit, and
// collisions without case (collisionRefusal). "" or the first refusal.
inline std::string layoutRefusal(const std::vector<Item>& items, std::vector<Planned>* files, uint64_t* bytes) {
    std::set<std::string> outputs;
    std::vector<std::string> all = {"KIT.md", "manifest.json", "PACKAGE.sha256"};
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        if (const auto e = detail::fatName(it.name); !e.empty()) return "'" + it.name + "' has " + e;
        if (!outputs.insert(detail::lower(it.kitPath())).second) return "two outputs would both be " + it.kitPath() + " (FAT ignores case): rename one";
        std::vector<Planned> f;
        if (const auto e = memberFiles(it, i, &f); !e.empty()) return e;
        for (const auto& x : f) {
            if (const auto e = detail::fatPath(x.to); !e.empty()) return e;
            if (x.bytes > kFat32MaxFile) return x.to + " is 4 GiB or more, above the FAT32 file size limit";
            *bytes += x.bytes;
            all.push_back(x.to);
        }
        files->insert(files->end(), f.begin(), f.end());
    }
    return collisionRefusal(all);
}

// ── KIT.md and manifest.json ──
inline std::string pagesFor(Kind k) {
    switch (k) {
        case Kind::Sound: return "MAN p.1206, 1207, 1210 (TXPROG); p.1209 (SF2 alternative)";
        case Kind::DrumKit: return "MAN p.1206, 1207, 1210";
        case Kind::Kmp: return "MAN p.1206, 1207, 1209";
        case Kind::Style: return "MAN p.1037 (Import SMF), p.974 and p.980 (markers, chords), p.1039-1040 (Save Style)";
        default: return "";
    }
}
inline std::string formatFor(Kind k) {
    switch (k) {
        case Kind::Sound: return "TX Sound (TXPROG)";
        case Kind::DrumKit: return "TX DrumKit (TXPERF)";
        case Kind::Kmp: return "KORG Multisample (KMP)";
        case Kind::Style: return "Standard MIDI File (Style Edit > Import SMF)";
        default: return "";
    }
}
// Import order: Drum Kits and Sounds first (they create the Sounds a Style may point to), multisamples, Styles last.
inline std::vector<const Item*> importOrder(const std::vector<Item>& items) {
    std::vector<const Item*> r;
    for (Kind k : {Kind::DrumKit, Kind::Sound, Kind::Kmp, Kind::Style}) for (const auto& it : items) if (it.kind == k) r.push_back(&it);
    return r;
}

inline std::string kitMd(const std::string& kitName, const std::vector<Item>& items) {
    const auto path = [](const Item& it, const std::string& rel) { return "`" + it.kitPath() + (rel.empty() ? "" : "/" + rel) + "`"; };
    std::string o = "# Pa5X Import Kit: " + kitName + "\n\n**" + kLabel + "**\n\n"
        "Every file here was written by AIArranger and re-read by AIArranger's own independent readers after it was copied "
        "into this kit. That proves the files are what AIArranger meant to write. It does NOT prove that a Pa5X accepts "
        "them: no AIArranger output has been imported on a Pa5X yet (owner test S6). Nothing here is a Pa5X-native file "
        "(no KST, STG, PCG, KMP from a Pa, PCM): the Pa5X builds its own Sounds, Drum Kits and Styles from these documented "
        "import formats. Page numbers: Pa5X User Manual v1.4, printed pages (MAN).\n\n"
        "## Before you start (owner)\n\n"
        "1. Back up all User data: FILE > Internal > All > page menu > Save, into a new KST folder on a separate USB device "
        "(MAN p.7, pp.789-791). Do not touch the saved folder afterwards.\n"
        "2. TX Sound and TX DrumKit import need OS 1.3.0 or later (MAN p.1216; System Info, MAN p.785).\n"
        "3. Copy this whole folder to a USB device formatted FAT32 or FAT16 with long file names (MAN p.715). On the stick, "
        "from inside this folder: `shasum -a 256 -c PACKAGE.sha256` (every line OK), or `aiarranger-kit verify <this folder>`.\n"
        "4. Check free User Sample memory: Sample Edit > Main > Memory Info (MAN p.1174). Imported samples stay in User "
        "Sample memory after power-off (MAN p.1207).\n"
        "5. Any error message on the Pa5X: write it down exactly and stop.\n\n"
        "## Import steps, in this order\n\n"
        "Drum Kits and Sounds first, multisamples next, Styles last: a Style's Program Changes may point to a Sound you "
        "import and save first. Every Sample Edit import starts at Sample Edit > Menu > Import/Export > Import "
        "(MAN p.1207): choose the format in the Import pop-up, touch Path, select the file, EXIT, Execute.\n";
    int n = 0;
    for (const auto* it : importOrder(items)) {
        o += "\n### Step " + std::to_string(++n) + ": " + kindName(it->kind) + ", " + it->name + "  [" + it->status + "]\n\n";
        switch (it->kind) {
            case Kind::Sound:
                o += "- Import pop-up: **TX Sound (TXPROG)** (MAN p.1206). Select:";
                for (const auto& s : it->select) o += " " + path(*it, s);
                o += "\n- In the dialog choose the target Sound location (pick an empty User location) and the name; compression off "
                     "(MAN p.1210). The linked samples are imported with it (MAN p.1206).\n";
                if (!it->alt.empty()) {
                    o += "- Alternative only if the TXPROG import fails (never both): **SoundFont (SF2)**, select";
                    for (const auto& s : it->alt) o += " " + path(*it, s);
                    o += ", Multisample or Complete Bank (MAN p.1209). SF2 gives multisamples only: assign them in Sound Edit and save "
                         "the Sound to an empty User location (MAN p.1207, p.1147).\n";
                }
                break;
            case Kind::DrumKit:
                o += "- Import pop-up: **TX DrumKit (TXPERF)** (MAN p.1206). Select:";
                for (const auto& s : it->select) o += " " + path(*it, s);
                o += "\n- In the dialog choose the target location and the name; compression off (MAN p.1210).\n";
                break;
            case Kind::Kmp:
                o += "- EXPERIMENTAL (KRONOS-format KMP/KSF written from Korg's KRONOS documentation; whether the Pa5X accepts it "
                     "is UNKNOWN). Import pop-up: **KORG Multisample (KMP)** (MAN p.1206), one multisample per import (MAN p.1209). Select, one at a time:";
                for (const auto& s : it->select) o += " " + path(*it, s);
                o += "\n- A stereo multisample is the `_L`/`_R` pair (MAN p.1206); which file of the pair to select is UNKNOWN. "
                     "A multisample is not a Sound: assign it in Sound Edit and save the Sound (MAN p.1207, p.1147).\n";
                break;
            case Kind::Style:
                o += "- Style Edit > Import/Export > Import SMF (MAN p.1037): From Song > Select " + path(*it, "") +
                     ", Execute. Then Save Style to a User location you choose (MAN p.1039-1040).\n"
                     "- Sounds are named in the file only as Bank Select/Program Change numbers from your sound map. Whether the Pa5X "
                     "keeps them as the Style's track Sounds is UNPROVEN (owner test P1.2); check each track and pick Sounds by hand if needed.\n";
                break;
            default: break;
        }
        o += "- Checked by: " + it->detail + "\n";
    }
    o += "\n## What the Pa5X decides itself (AIArranger writes none of it)\n\n"
         "- Every location: the User Sound / Drum Kit location in the import dialog (MAN p.1210), the Sound you save after an "
         "SF2 or KMP import (MAN p.1147), the User Style location on Save Style (MAN p.1039-1040). AIArranger assigns no slot.\n"
         "- The names of the samples and multisamples it creates in User Sample memory, and whether it compresses them (your choice in the dialog).\n"
         "- The Bank Select/Program Change address of a User Sound: observed in PA Manager, not yet checked on the Pa5X.\n"
         "- Saving a KST: only the Pa5X writes one (FILE > Save, MAN pp.789-791). AIArranger cannot write a KST "
         "(see docs/PA5X_KST_EXPORT_PLAN.md).\n\n"
         "## Not verified\n\n"
         "- Any import of these files on a Pa5X (owner tests S6 and P1.2 not run). L3 comes only from an owner's recorded test.\n"
         "- Pa5X limits on name length and folder depth (not in the manual).\n"
         "- Files are checked with FAT long-name rules (general rules, not Pa5X-specific) and the FAT32 4 GiB file limit.\n";
    return o;
}

inline std::string manifestJson(const std::string& kitName, const std::vector<Item>& items) {
    const auto q = [](const std::string& v) { return "\"" + media::graph::jsonEscape(v) + "\""; };
    const auto list = [&](const Item& it, const std::vector<std::string>& v) {
        std::string o = "[";
        for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + q(it.kitPath() + (v[i].empty() ? "" : "/" + v[i]));
        return o + "]";
    };
    std::string o = "{\n  \"schema\": " + q(kSchema) + ",\n  \"kit\": " + q(kitName) + ",\n  \"label\": " + q(kLabel) +
                    ",\n  \"hardwareVerified\": false,\n  \"manualPages\": \"Pa5X User Manual v1.4, printed pages\",\n  \"items\": [";
    int n = 0;
    for (const auto* it : importOrder(items)) {
        o += std::string(n ? "," : "");
        o += "\n    {\"step\": " + std::to_string(++n) + ", \"kind\": " + q(typeFolder(it->kind)) + ", \"name\": " + q(it->name) +
             ", \"path\": " + q(it->kitPath()) + ", \"importFormat\": " + q(formatFor(it->kind)) + ", \"select\": " + list(*it, it->select) +
             ", \"alternative\": " + list(*it, it->alt) + ", \"manual\": " + q(pagesFor(it->kind)) + ", \"status\": " + q(it->status) +
             ", \"checks\": " + q(it->detail) + "}";
    }
    return o + "\n  ]\n}\n";
}

// ── a small strict JSON reader for manifest.json (the repo has no general JSON parser; P2, Codex r1) ──
// RFC 8259 values; strings with every escape (\uXXXX to UTF-8, surrogate pairs joined); nesting at most 16; trailing
// bytes other than white space refused. Numbers are kept as their text. Bounded by the caller's file size limit.
struct Json {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    std::string s;                                    // Str, or the text of a Num
    std::vector<Json> a;                              // Arr
    std::vector<std::pair<std::string, Json>> o;      // Obj, in file order (duplicate keys refused)
    const Json* get(const std::string& k) const { for (const auto& [n, v] : o) if (n == k) return &v; return nullptr; }
};
namespace detail {
struct JsonIn {
    const std::string& x;
    size_t i = 0;
    std::string err;
    void ws() { while (i < x.size() && (x[i] == ' ' || x[i] == '\t' || x[i] == '\n' || x[i] == '\r')) ++i; }
    bool fail(const std::string& m) { if (err.empty()) err = m + " at byte " + std::to_string(i); return false; }
    bool lit(const char* w) { const size_t n = std::strlen(w); if (x.compare(i, n, w) != 0) return fail("bad literal"); i += n; return true; }
    bool hex4(uint32_t* v) {
        if (i + 4 > x.size()) return fail("short \\u escape");
        *v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = x[i++];
            if (!std::isxdigit(static_cast<unsigned char>(c))) return fail("bad \\u escape");
            *v = *v * 16 + uint32_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        return true;
    }
    bool str(std::string* out) {
        if (i >= x.size() || x[i] != '"') return fail("string expected");
        for (++i; i < x.size();) {
            const unsigned char c = static_cast<unsigned char>(x[i++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in a string");
            if (c != '\\') { *out += char(c); continue; }
            if (i >= x.size()) break;
            const char e = x[i++];
            uint32_t cp = 0;
            switch (e) {
                case '"': case '\\': case '/': *out += e; continue;
                case 'b': *out += '\b'; continue;
                case 'f': *out += '\f'; continue;
                case 'n': *out += '\n'; continue;
                case 'r': *out += '\r'; continue;
                case 't': *out += '\t'; continue;
                case 'u': if (!hex4(&cp)) return false; break;
                default: return fail("bad escape");
            }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo = 0;
                if (x.compare(i, 2, "\\u") != 0) return fail("lone surrogate");
                i += 2;
                if (!hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("lone surrogate");
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone surrogate");
            if (cp < 0x80) *out += char(cp);
            else if (cp < 0x800) { *out += char(0xC0 | cp >> 6); *out += char(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { *out += char(0xE0 | cp >> 12); *out += char(0x80 | (cp >> 6 & 0x3F)); *out += char(0x80 | (cp & 0x3F)); }
            else { *out += char(0xF0 | cp >> 18); *out += char(0x80 | (cp >> 12 & 0x3F)); *out += char(0x80 | (cp >> 6 & 0x3F)); *out += char(0x80 | (cp & 0x3F)); }
        }
        return fail("unterminated string");
    }
    bool num(Json* v) {
        const size_t from = i;
        const auto digits = [&] { const size_t k = i; while (i < x.size() && std::isdigit(static_cast<unsigned char>(x[i]))) ++i; return i > k; };
        if (i < x.size() && x[i] == '-') ++i;
        if (i < x.size() && x[i] == '0') ++i;
        else if (!digits()) return fail("bad number");
        if (i < x.size() && x[i] == '.') { ++i; if (!digits()) return fail("bad number"); }
        if (i < x.size() && (x[i] == 'e' || x[i] == 'E')) { ++i; if (i < x.size() && (x[i] == '+' || x[i] == '-')) ++i; if (!digits()) return fail("bad number"); }
        v->t = Json::T::Num;
        v->s = x.substr(from, i - from);
        return true;
    }
    bool value(Json* v, int depth) {
        if (depth > 16) return fail("nested too deeply");
        ws();
        if (i >= x.size()) return fail("value expected");
        const char c = x[i];
        if (c == '{') {
            v->t = Json::T::Obj;
            ++i;
            ws();
            if (i < x.size() && x[i] == '}') { ++i; return true; }
            for (;;) {
                std::string k;
                ws();
                if (!str(&k)) return false;
                if (v->get(k)) return fail("duplicate key " + k);
                ws();
                if (i >= x.size() || x[i++] != ':') return fail("':' expected");
                Json m;
                if (!value(&m, depth + 1)) return false;
                v->o.emplace_back(std::move(k), std::move(m));
                ws();
                if (i < x.size() && x[i] == ',') { ++i; continue; }
                if (i < x.size() && x[i] == '}') { ++i; return true; }
                return fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            v->t = Json::T::Arr;
            ++i;
            ws();
            if (i < x.size() && x[i] == ']') { ++i; return true; }
            for (;;) {
                Json m;
                if (!value(&m, depth + 1)) return false;
                v->a.push_back(std::move(m));
                ws();
                if (i < x.size() && x[i] == ',') { ++i; continue; }
                if (i < x.size() && x[i] == ']') { ++i; return true; }
                return fail("',' or ']' expected");
            }
        }
        if (c == '"') { v->t = Json::T::Str; return str(&v->s); }
        if (c == 't') { v->t = Json::T::Bool; v->b = true; return lit("true"); }
        if (c == 'f') { v->t = Json::T::Bool; return lit("false"); }
        if (c == 'n') return lit("null");
        return num(v);
    }
};
}  // namespace detail
// "" and *out set, or why `text` is not one JSON value.
inline std::string parseJson(const std::string& text, Json* out) {
    detail::JsonIn in{text};
    if (!in.value(out, 0)) return in.err;
    in.ws();
    return in.i == text.size() ? "" : "trailing bytes after the JSON value at byte " + std::to_string(in.i);
}

// manifest.json against the members found in the kit: the schema, the label, "hardwareVerified": false, and exactly one
// entry per member (no extra, none missing) whose kind, name, path, format, pages, status and selected files are what
// this kit holds. "" = it matches.
inline std::string manifestRefusal(const fs::path& dir, const std::vector<Item>& found) {
    std::error_code ec;
    const auto size = fs::file_size(dir / "manifest.json", ec);
    if (ec || size > (1u << 20)) return "manifest.json is missing or larger than 1 MiB";
    std::FILE* f = media::detail::openRegular(AT_FDCWD, (dir / "manifest.json").c_str());
    if (!f) return "manifest.json is not a regular file";
    std::string text(size, '\0');
    const size_t got = std::fread(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (got != text.size()) return "cannot read manifest.json";
    Json j;
    if (const auto e = parseJson(text, &j); !e.empty()) return "manifest.json is not valid JSON: " + e;
    const auto str = [](const Json* v, const std::string& want) { return v && v->t == Json::T::Str && v->s == want; };
    if (j.t != Json::T::Obj) return "manifest.json is not an object";
    if (!str(j.get("schema"), kSchema)) return std::string("manifest.json: schema is not ") + kSchema;
    if (!str(j.get("label"), kLabel)) return "manifest.json: the label is missing or different";
    if (const Json* h = j.get("hardwareVerified"); !h || h->t != Json::T::Bool || h->b) return "manifest.json: hardwareVerified must be false";
    const Json* kit = j.get("kit");
    const Json* items = j.get("items");
    if (!kit || kit->t != Json::T::Str || !items || items->t != Json::T::Arr) return "manifest.json: kit or items missing";
    std::map<std::string, const Item*> byPath;
    for (const auto& it : found) byPath[it.kitPath()] = &it;
    if (items->a.size() != found.size()) return "manifest.json lists " + std::to_string(items->a.size()) + " members, the kit holds " + std::to_string(found.size());
    std::set<std::string> listed;
    static const std::set<std::string> keys = {"step", "kind", "name", "path", "importFormat", "select", "alternative", "manual", "status", "checks"};
    for (size_t n = 0; n < items->a.size(); ++n) {
        const Json& e = items->a[n];
        if (e.t != Json::T::Obj || e.o.size() != keys.size()) return "manifest.json: entry " + std::to_string(n + 1) + " does not have exactly the member fields";
        for (const auto& [k, v] : e.o) if (!keys.count(k)) return "manifest.json: entry " + std::to_string(n + 1) + " has the unknown field " + k;
        const Json* path = e.get("path");
        if (!path || path->t != Json::T::Str || !byPath.count(path->s)) return "manifest.json: entry " + std::to_string(n + 1) + " names no member of the kit";
        if (!listed.insert(path->s).second) return "manifest.json lists " + path->s + " twice";
        const Item& it = *byPath[path->s];
        const Json* step = e.get("step");
        if (!step || step->t != Json::T::Num || step->s != std::to_string(n + 1)) return "manifest.json: " + path->s + " has the wrong step";
        if (!str(e.get("kind"), typeFolder(it.kind)) || !str(e.get("name"), it.name) || !str(e.get("importFormat"), formatFor(it.kind)) ||
            !str(e.get("manual"), pagesFor(it.kind)) || !str(e.get("status"), kValid))
            return "manifest.json: " + path->s + " does not match the member (kind, name, format, pages or status)";
        if (const Json* c = e.get("checks"); !c || c->t != Json::T::Str || c->s.empty()) return "manifest.json: " + path->s + " has no checks";
        for (const auto& [field, want] : {std::pair<const char*, const std::vector<std::string>*>{"select", &it.select}, {"alternative", &it.alt}}) {
            const Json* a = e.get(field);
            if (!a || a->t != Json::T::Arr || a->a.size() != want->size()) return "manifest.json: " + path->s + " " + field + " does not match the member";
            for (size_t k = 0; k < want->size(); ++k)
                if (!str(&a->a[k], it.kitPath() + ((*want)[k].empty() ? "" : "/" + (*want)[k]))) return "manifest.json: " + path->s + " " + field + " does not match the member";
        }
    }
    return "";
}

// ── checking a written kit (the staged one, or a copy on a stick) ──
// Re-reads the whole kit: every file against PACKAGE.sha256 (nothing missing or extra), only the known top-level entries,
// every member through its reader, and KIT.md/manifest.json carrying the label and every member. `expect` (optional):
// the members the kit must hold. Read-only. "" = structurally valid; *text says what was checked.
inline std::string verifyKit(const fs::path& dir, const std::vector<Item>* expect, std::string* text) {
    size_t files = 0;
    const auto bad = media::verifyManifest(dir, dir / "PACKAGE.sha256", {}, &files,
                                           [](const std::string& rel) { return rel == "PACKAGE.sha256" || detail::finderLitter(rel); }, true);
    if (!bad.empty()) return "PACKAGE.sha256: " + bad;
    std::vector<Item> found;
    std::error_code ec;
    for (fs::directory_iterator top(dir, ec), end; !ec && top != end; top.increment(ec)) {
        const std::string n = top->path().filename().string();
        if (n == "KIT.md" || n == "manifest.json" || n == "PACKAGE.sha256" || detail::finderLitter(n)) continue;
        Kind want = Kind::Unknown;
        for (Kind k : {Kind::Sound, Kind::DrumKit, Kind::Kmp, Kind::Style}) if (n == typeFolder(k)) want = k;
        if (want == Kind::Unknown || !top->is_directory(ec) || top->is_symlink(ec)) return "unexpected " + n + " at the top of the kit";
        for (fs::directory_iterator m(top->path(), ec), e2; !ec && m != e2; m.increment(ec)) {
            if (detail::finderLitter(m->path().filename().string())) continue;
            auto it = classify(m->path());
            if (it.kind != want) return n + "/" + it.name + " is not a " + kindName(want);
            std::string why;
            if (const auto e = checkMember(it.kind, m->path(), &why); !e.empty()) return it.kitPath() + ": " + e;
            found.push_back(std::move(it));
        }
    }
    if (ec) return "cannot list " + dir.string() + ": " + ec.message();
    if (found.empty()) return "the kit holds no member";
    std::ifstream mdf(dir / "KIT.md", std::ios::binary);
    const std::string md{std::istreambuf_iterator<char>(mdf), {}};
    if (md.find(kLabel) == std::string::npos) return "KIT.md lacks the label";
    if (const auto e = manifestRefusal(dir, found); !e.empty()) return e;
    if (expect) {
        std::set<std::string> want, got;
        for (const auto& it : *expect) want.insert(it.kitPath());
        for (const auto& it : found) got.insert(it.kitPath());
        if (want != got) return "the kit does not hold exactly the planned members";
    }
    *text = "Kit verified: all " + std::to_string(files) + " files match PACKAGE.sha256, and every one of the " + std::to_string(found.size()) +
            " members re-reads through its reader. " + kLabel + ".";
    return "";
}

// The candidates in a folder of outputs (the app's "folder holding the outputs"): its visible entries, sorted. Read-only.
inline std::vector<fs::path> outputsIn(const fs::path& folder) {
    std::vector<fs::path> r;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().rfind('.', 0) != 0) r.push_back(it->path());
    std::sort(r.begin(), r.end());
    return r;
}

// Test hook (smoke): the thread that last listed a folder of outputs for a kit job; it must be the job's worker.
inline std::atomic<size_t> lastListingThread{0};

// The kit as an E6 export job. `inputs`: AIArranger outputs (package folders, KMP folders, Style .mid files). With
// `outputsFolder` set instead, its visible entries are the inputs, listed by the job itself (off the UI thread), and the
// folder is the job's source.
inline ExportSpec importKit(std::vector<fs::path> inputs, fs::path dest, fs::path outputsFolder = {}) {
    ExportSpec s;
    s.type = "Pa5X Import Kit (documented imports)";
    for (const auto& p : inputs) s.source += (s.source.empty() ? "" : ", ") + p.string();
    s.sources = inputs;
    if (!outputsFolder.empty()) { s.source = outputsFolder.string(); s.sources = {outputsFolder}; }
    s.dest = std::move(dest);
    auto in = std::make_shared<std::vector<fs::path>>(std::move(inputs));
    auto items = std::make_shared<std::vector<Item>>();
    auto roots = std::make_shared<std::vector<std::shared_ptr<media::detail::Fd>>>();   // pinned per member, in item order
    auto files = std::make_shared<std::vector<Planned>>();
    auto bytes = std::make_shared<uint64_t>(0);
    s.validate = [in, outputsFolder, items, roots, files, bytes, kitName = media::folderPath(s.dest).filename().string()] {
        if (!outputsFolder.empty()) {
            lastListingThread = thisThread();
            *in = outputsIn(outputsFolder);
        }
        if (in->empty()) return std::string("refusing: no outputs chosen for the kit");
        if (const auto e = detail::fatName(kitName); !e.empty()) return "refusing: the kit name '" + kitName + "' has " + e;
        items->clear();
        roots->clear();
        for (const auto& p : *in) {
            auto it = classify(p);
            if (it.kind == Kind::Unknown) return "refusing: " + p.string() + " is not an AIArranger output the kit takes (a Sound or Drum Kit package folder, a KMP/KSF folder, or a Style .mid)";
            std::shared_ptr<media::detail::Fd> fd;
            if (const auto e = pinRoot(it, &fd); !e.empty()) return "refusing: " + e;
            roots->push_back(std::move(fd));
            items->push_back(std::move(it));
        }
        files->clear();
        *bytes = 0;
        if (const auto e = layoutRefusal(*items, files.get(), bytes.get()); !e.empty()) return "refusing: " + e;
        return std::string();
    };
    // Per-member status on the SOURCE: the job report lists every one; a FAILED member stops the kit (fail closed).
    s.dependencies = [items](std::vector<std::string>& notes, uint64_t&) {
        std::vector<std::string> failed;
        for (auto& it : *items) {
            std::string d;
            const auto e = checkMember(it.kind, it.source, &d);
            it.status = e.empty() ? kValid : "FAILED";
            it.detail = e.empty() ? d : e;
            (e.empty() ? notes : failed).push_back(it.status + ": " + it.kitPath() + " (" + it.detail + ")");
        }
        notes.push_back("Members are copied byte for byte; nothing is converted and no Korg-native file is written. Best possible label: " + std::string(kLabel));
        return failed;
    };
    s.preview = [items, bytes] {
        std::vector<OutFile> r;
        for (const auto& it : *items) r.push_back({it.kitPath() + (it.kind == Kind::Style ? "" : "/"), 0, "", false});
        r.push_back({"KIT.md, manifest.json, PACKAGE.sha256", 65536, "", false});
        r.front().bytes = *bytes;   // shortcut: the members' total is shown on the first line, sizes per member are not split
        return r;
    };
    s.write = [items, roots, files](const fs::path& out, const std::function<bool(double)>& progress) -> WriteResult {
        std::error_code ec;
        if (!fs::create_directory(out, ec)) return {"cannot create " + out.string(), ""};
        media::sound::pkgio::FileSet set{out};
        // Every file read once, from the pinned member folders without following a link, streamed and hashed as it is read
        for (size_t i = 0; i < files->size(); ++i) {
            if (!progress(double(i) / double(files->size() + 1))) return {"cancelled by the user while copying; nothing was published", ""};
            const auto& f = (*files)[i];
            uint64_t n = 0;
            std::string sha;
            if (const auto e = copyPinned((*roots)[f.item]->v, f.from, set, f.to, &n, &sha); !e.empty())
                return {"cannot copy " + (*items)[f.item].kitPath() + ": " + e, ""};
            set.want[f.to] = {n, sha};
        }
        // The copy against the SHA-256 of the bytes read from the sources (exactly these files, nothing extra) ...
        if (const auto e = set.checkFiles(); !e.empty()) return {"the copy does not match the bytes read from the sources: " + e, ""};
        // ... and each copied member through the readers that passed its source
        for (const auto& it : *items) {
            std::string d;
            if (const auto e = checkMember(it.kind, out / it.kitPath(), &d); !e.empty()) return {"the copy of " + it.kitPath() + " does not re-read: " + e, ""};
        }
        const auto name = out.filename().string();
        set.save("KIT.md", kitMd(name, *items));
        set.save("manifest.json", manifestJson(name, *items));
        set.saveManifest();
        if (!set.ok) return {"write failed in " + out.string(), ""};
        if (const auto e = set.checkFiles(); !e.empty()) return {"the written kit does not read back as written: " + e, ""};
        return {"", kitMd(name, *items)};
    };
    s.verify = [items](const fs::path& out) {
        Verification v;
        std::string text;
        const auto e = verifyKit(out, items.get(), &text);
        v.l1 = e.empty() ? ui::studio::Level{Check::Passed, std::string(kLabel) + ": every file matches PACKAGE.sha256 and every member re-reads"}
                         : ui::studio::Level{Check::Failed, e};
        v.l2 = {Check::NotRun, "not claimed for a kit: its members keep their own levels (KMP and Style SMF are L1 only)"};
        return std::pair<Verification, std::string>{v, e.empty() ? text : "NOT verified: " + e};
    };
    return s;
}

}  // namespace ai_arranger::jobs::kit

#endif
