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
#include <set>

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

// Every file of a member: (source file, kit-relative path). Refuses links and special files; Finder litter is not copied.
inline std::string memberFiles(const Item& it, std::vector<std::pair<fs::path, std::string>>* out) {
    if (it.kind == Kind::Style) { out->push_back({it.source, it.kitPath()}); return ""; }
    media::detail::Listing L;
    std::string err;
    if (!media::detail::listFiles(it.source, L, err)) return it.source.string() + ": " + err;
    if (!L.skipped.empty()) return it.source.string() + ": refusing the link or special file " + L.skipped.front();
    for (const auto& [rel, e] : L.files) if (!detail::finderLitter(rel)) out->push_back({e.abs, it.kitPath() + "/" + rel});
    return "";
}

// Every kit-relative path the kit will hold, checked for the stick: FAT names, the FAT32 4 GiB file limit, and two
// names equal without case in one folder (FAT ignores case). "" or the first refusal.
inline std::string layoutRefusal(const std::vector<Item>& items, std::vector<std::pair<fs::path, std::string>>* files, uint64_t* bytes) {
    std::set<std::string> seen;
    for (const auto* top : {"KIT.md", "manifest.json", "PACKAGE.sha256"}) seen.insert(detail::lower(top));
    for (const auto& it : items) {
        if (const auto e = detail::fatName(it.name); !e.empty()) return "'" + it.name + "' has " + e;
        if (!seen.insert(detail::lower(it.kitPath())).second) return "two outputs would both be " + it.kitPath() + " (FAT ignores case): rename one";
        std::vector<std::pair<fs::path, std::string>> f;
        if (const auto e = memberFiles(it, &f); !e.empty()) return e;
        for (const auto& [src, rel] : f) {
            if (const auto e = detail::fatPath(rel); !e.empty()) return e;
            if (it.kind != Kind::Style && !seen.insert(detail::lower(rel)).second) return "two files would both be " + rel + " (FAT ignores case)";
            std::error_code ec;
            const auto n = fs::file_size(src, ec);
            if (ec) return "cannot read the size of " + src.string();
            if (n >= (uint64_t(1) << 32)) return rel + " is 4 GiB or more, above the FAT32 file size limit";
            *bytes += n;
        }
        files->insert(files->end(), f.begin(), f.end());
    }
    return "";
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
    std::ifstream mdf(dir / "KIT.md", std::ios::binary), jsf(dir / "manifest.json", std::ios::binary);
    const std::string md{std::istreambuf_iterator<char>(mdf), {}}, js{std::istreambuf_iterator<char>(jsf), {}};
    if (md.find(kLabel) == std::string::npos || js.find(kLabel) == std::string::npos) return "KIT.md or manifest.json lacks the label";
    for (const auto& it : found)
        if (js.find("\"path\": \"" + media::graph::jsonEscape(it.kitPath()) + "\"") == std::string::npos) return "manifest.json does not list " + it.kitPath();
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

// The kit as an E6 export job. `inputs`: AIArranger outputs (package folders, KMP folders, Style .mid files).
inline ExportSpec importKit(std::vector<fs::path> inputs, fs::path dest) {
    ExportSpec s;
    s.type = "Pa5X Import Kit (documented imports)";
    for (const auto& p : inputs) s.source += (s.source.empty() ? "" : ", ") + p.string();
    s.sources = inputs;
    s.dest = std::move(dest);
    auto items = std::make_shared<std::vector<Item>>();
    auto files = std::make_shared<std::vector<std::pair<fs::path, std::string>>>();
    auto bytes = std::make_shared<uint64_t>(0);
    s.validate = [inputs, items, files, bytes, kitName = media::folderPath(s.dest).filename().string()] {
        if (inputs.empty()) return std::string("refusing: no outputs chosen for the kit");
        if (const auto e = detail::fatName(kitName); !e.empty()) return "refusing: the kit name '" + kitName + "' has " + e;
        items->clear();
        for (const auto& p : inputs) {
            auto it = classify(p);
            if (it.kind == Kind::Unknown) return "refusing: " + p.string() + " is not an AIArranger output the kit takes (a Sound or Drum Kit package folder, a KMP/KSF folder, or a Style .mid)";
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
    s.write = [items, files](const fs::path& out, const std::function<bool(double)>& progress) -> WriteResult {
        std::error_code ec;
        if (!fs::create_directory(out, ec)) return {"cannot create " + out.string(), ""};
        media::sound::pkgio::FileSet set{out};
        for (size_t i = 0; i < files->size(); ++i) {
            if (!progress(double(i) / double(files->size() + 1))) return {"cancelled by the user while copying; nothing was published", ""};
            const auto& [src, rel] = (*files)[i];
            std::string err;
            // shortcut: one member file is held in memory while it is copied (samples are MB, not GB); stream it if kits get large
            const auto b = media::sound::korg::slurp(src, &err);
            if (!err.empty()) return {"cannot read " + src.string() + ": " + err, ""};
            set.save(rel, b);
        }
        // Round trip: each copied member through the same reader that passed its source (fail closed on any difference)
        for (const auto& it : *items) {
            std::string d;
            if (const auto e = checkMember(it.kind, out / it.kitPath(), &d); !e.empty()) return {"the copy of " + it.kitPath() + " does not re-read: " + e, ""};
            if (d != it.detail) return {"the copy of " + it.kitPath() + " reads differently from its source (" + d + " vs " + it.detail + ")", ""};
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
