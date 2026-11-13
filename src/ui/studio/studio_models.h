#ifndef AI_ARRANGER_UI_STUDIO_MODELS_H
#define AI_ARRANGER_UI_STUDIO_MODELS_H

// UI-toolkit-agnostic state behind the macOS AI Music Studio window (E2 shell): what an opened folder looks like
// (source recognition, read-only) and the L1/L2/L3 verification badges. Nothing here writes, sends MIDI or talks to
// an instrument. Recognition reads folder layout and file names only, never Korg file content, so "content" is never
// claimed for a keyboard backup: no KST content validator exists yet.
//
// Badge rules (UI spec rev 2 §3.3): L1 structural = our validator checked the files (package manifest, SF2 structure);
// L2 app = our own reader re-opened the output; L3 hardware = ONLY an owner-recorded test file that names this exact
// output (SHA-256 of its manifest) while L1 still passes. L3 is never inferred from anything else.

#include "media/kst_backup.h"
#include "media/kst_inventory.h"
#include "media/sound/package_verify.h"
#include <cctype>

namespace ai_arranger::ui::studio {

namespace fs = std::filesystem;

// ── Source recognition ──
struct SourceProbe {
    enum class Kind { NotRecognised, Pa5xUserData, KstBackupCopy, Pa4xSet, AiaPackage, KorgSamplingData, WavFolder };
    Kind kind = Kind::NotRecognised;
    std::string name, path, detected, error;
    bool layout = false;   // the folder layout matched what we know; content is never claimed here
    media::Inventory inv;  // file-level scan (empty for a package)
};

inline const char* kindName(SourceProbe::Kind k) {
    using K = SourceProbe::Kind;
    switch (k) {
        case K::Pa5xUserData: return "Pa5X user data (folder layout)";
        case K::KstBackupCopy: return "Pa5X user data, verified copy made by Backup KST (folder layout)";
        case K::Pa4xSet: return "Pa4X-family SET (folder layout; its files are not decoded)";
        case K::AiaPackage: return "AIArranger Sound / Drum Kit package";
        case K::KorgSamplingData: return "KRONOS-family sampling data (KMP/KSF)";
        case K::WavFolder: return "Folder of WAV samples";
        case K::NotRecognised: break;
    }
    return "Not recognised";
}

// Read-only. Scans only names and sizes (media::scan), never file content.
inline SourceProbe probeSource(const fs::path& dir) {
    SourceProbe p;
    p.path = dir.string();
    p.name = media::folderPath(dir).filename().string();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { p.error = "Not a folder: " + dir.string(); p.detected = kindName(p.kind); return p; }
    using K = SourceProbe::Kind;
    if (fs::exists(dir / media::sound::pkgio::FileSet::kPackageManifest, ec) || fs::exists(dir / "RESOURCES.tsv", ec)) {
        p.kind = K::AiaPackage;
        p.layout = true;
    } else {
        p.inv = media::scan(dir);
        if (!p.inv.ok) { p.error = p.inv.error; p.detected = kindName(p.kind); return p; }
        auto count = [&](std::initializer_list<const char*> exts) {
            uint32_t n = 0;
            for (const char* e : exts) if (const auto it = p.inv.by_ext.find(e); it != p.inv.by_ext.end()) n += it->second.files;
            return n;
        };
        std::string upper = p.name;
        for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
        const bool setSuffix = upper.size() > 4 && upper.compare(upper.size() - 4, 4, ".SET") == 0;
        // Pa5X-only extensions (manual v1.4 p.721-722 type table, as in kst_inventory.h); .KMP/.WAV/.MID also occur elsewhere.
        const uint32_t pa5x = count({".STG", ".KSC", ".PDC", ".CSC", ".SBD", ".SBL", ".GBL", ".PCG", ".PCM"});
        if (setSuffix) p.kind = K::Pa4xSet;
        else if (pa5x > 0) p.kind = fs::exists(media::backupManifestPath(media::folderPath(dir)), ec) ? K::KstBackupCopy : K::Pa5xUserData;
        else if (count({".KSF", ".KMP"}) > 0) p.kind = K::KorgSamplingData;
        else if (count({".WAV"}) > 0 && count({".WAV"}) == p.inv.files) p.kind = K::WavFolder;
        p.layout = p.kind != K::NotRecognised;
    }
    p.detected = kindName(p.kind);
    if (!p.layout)
        p.error = "Not a keyboard backup: no Style, Pad or Sound files found. Choose the folder that ends in .KST, "
                  "a package folder, or a folder of KMP/KSF/WAV samples.";
    return p;
}

// One-line header (spec §4.1.1): name · what was detected · what was checked · read-only.
inline std::string sourceHeader(const SourceProbe& p) {
    return p.name + "  ·  " + p.detected + "  ·  layout " + (p.layout ? "\xE2\x97\x86" : "\xE2\x97\x87") +
           " content \xE2\x97\x87  ·  Read-only source";
}

// The read-only loaded-library view: our type names, zero rows kept visible, then banks and the memory estimate.
inline std::string libraryView(const SourceProbe& p) {
    using K = SourceProbe::Kind;
    std::ostringstream o;
    o << sourceHeader(p) << "\n" << p.path << "\n\n";
    if (!p.error.empty()) { o << p.error << "\n"; return o.str(); }
    if (p.kind == K::AiaPackage) {
        o << "A package opens in Resource Graph (Library > Resource Graph) and is checked in Validation & Reports.\n";
        return o.str();
    }
    o << "Content: not checked. Only names and sizes were read; no KST content validator exists yet.\n\n";
    if (p.kind == K::Pa4xSet)
        o << "Pa4X-family SET: its .STY files are Korg Style banks (not Yamaha styles), and none of its files are decoded.\n"
             "Pa4X limits are not sourced here; the counts below are file counts only.\n\n";
    static const std::pair<const char*, const char*> types[] = {
        {"Style", "Styles"}, {"Keyboard Set", "Keyboard Sets"}, {"Pad", "Pads"}, {"Sound", "Sounds"},
        {"Multisample", "Multisamples"}, {"Sample", "Samples"}, {"SongBook database", "SongBook"}};
    o << "Type             Files        Bytes\n";
    for (const auto& [doc, ours] : types) {
        const auto it = p.inv.by_type.find(doc);
        char line[96];
        if (it == p.inv.by_type.end() || it->second.files == 0) std::snprintf(line, sizeof line, "%-16s  none in this source\n", ours);
        else std::snprintf(line, sizeof line, "%-16s %5u %12ju\n", ours, it->second.files, it->second.bytes);
        o << line;
    }
    o << "\nBanks (type/bank: files)\n";
    if (p.inv.by_bank.empty()) o << "  no bank sub-folders\n";
    for (const auto& [bank, s] : p.inv.by_bank) o << "  " << bank << ": " << s.files << "\n";
    if (p.kind == K::Pa5xUserData || p.kind == K::KstBackupCopy) o << media::formatMemoryEstimate(p.inv, media::kPaManagerPa5xBudgetBytes);
    return o.str();
}

// ── Verification badges ──
enum class Check { NotRun, Passed, Failed };
struct Level { Check state = Check::NotRun; std::string detail; };
struct Verification { Level l1, l2, l3; };

// Shape plus text, never colour alone: filled = reached, hollow = not reached, cross = failed.
inline std::string badgeLine(const Verification& v) {
    auto one = [](const Level& l, const char* filled, const char* name) {
        return std::string(l.state == Check::Passed ? filled : l.state == Check::Failed ? "\xE2\x9C\x95" : "\xE2\x97\x87") + " " + name;
    };
    return one(v.l1, "\xE2\x97\x86", "L1") + "  " + one(v.l2, "\xE2\x97\x86\xE2\x97\x86", "L2") + "  " +
           one(v.l3, "\xE2\x97\x86\xE2\x97\x86\xE2\x97\x86", "L3");
}

// What VoiceOver reads and what the Inspector prints under the badges.
inline std::string spokenBadges(const Verification& v) {
    auto one = [](const Level& l, const char* what, const char* notRun) {
        std::string s = std::string(what) + (l.state == Check::Passed ? " passed" : l.state == Check::Failed ? " FAILED" : notRun);
        return s + (l.detail.empty() ? "" : " (" + l.detail + ")") + ".";
    };
    return one(v.l1, "Level 1 structural check", " not run") + " " + one(v.l2, "Level 2 app re-read", " not run") + " " +
           one(v.l3, "Level 3 hardware test", " not recorded: not tested on a Pa5X");
}

// L1/L2 of a package from the shared verifier: L1 = every file matches PACKAGE.sha256; L2 = our reader loads it back.
// The prefixes are package_verify.h's own messages; test_studio_models fails if they drift.
inline Verification fromPackageCheck(const media::sound::PackageCheck& c) {
    Verification v;
    const auto& t = c.text;
    if (c.ok) {
        v.l1 = {Check::Passed, "files match PACKAGE.sha256"};
        v.l2 = {Check::Passed, "package loads back"};
    } else if (t.rfind("NOT verified: the files match", 0) == 0) {
        v.l1 = {Check::Passed, "files match PACKAGE.sha256"};
        v.l2 = {Check::Failed, "package does not load"};
    } else if (t.rfind("Verify cancelled", 0) == 0) {
        v.l1.detail = "cancelled";
    } else {
        v.l1 = {Check::Failed, t.rfind("Not verified:", 0) == 0 ? "no PACKAGE.sha256" : "files do not match PACKAGE.sha256"};
    }
    return v;
}

// L3 record file, written by the owner after a hardware test (one "key<TAB or space>value" per line):
//   aiarranger-l3-record 1
//   manifest-sha256 <SHA-256 of the output's PACKAGE.sha256 file>
//   device Pa5X
//   date 2026-11-20
//   tester <who ran it>
//   result pass | fail
// Granted only when the record names this exact output and L1 passes now (the tested files are still the files).
inline Level checkL3Record(const fs::path& record, const fs::path& output, const Level& l1) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(record, ec)) || fs::file_size(record, ec) > 65536 || ec)
        return {Check::NotRun, "record is not a regular file of at most 64 KiB"};
    std::ifstream f(record);
    std::map<std::string, std::string> kv;
    std::string line, first;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto sp = line.find_first_of(" \t");
        const std::string k = line.substr(0, sp);
        std::string val;
        if (const auto vs = sp == std::string::npos ? sp : line.find_first_not_of(" \t", sp); vs != std::string::npos) val = line.substr(vs);
        if (first.empty()) first = k + " " + val;
        if (!k.empty()) kv[k] = val;
    }
    if (first != "aiarranger-l3-record 1") return {Check::NotRun, "not an aiarranger-l3-record 1 file"};
    for (const char* k : {"manifest-sha256", "device", "date", "tester", "result"})
        if (kv[k].empty()) return {Check::NotRun, std::string("record has no ") + k};
    if (kv["device"] != "Pa5X") return {Check::NotRun, "record is not a Pa5X test"};
    if (kv["result"] != "pass" && kv["result"] != "fail") return {Check::NotRun, "result must be pass or fail"};
    bool cancelled = false;
    std::string err;
    const auto man = output / media::sound::pkgio::FileSet::kPackageManifest;
    const std::string h = media::detail::sha256File(man, [](size_t) { return true; }, &cancelled, &err);
    if (h.empty()) return {Check::NotRun, "output has no readable " + std::string(media::sound::pkgio::FileSet::kPackageManifest)};
    std::string want = kv["manifest-sha256"];
    for (auto& c : want) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (want != h) return {Check::NotRun, "record is for a different output (manifest SHA-256 differs)"};
    if (l1.state != Check::Passed) return {Check::NotRun, "run L1 first: the tested files must still match their manifest"};
    const std::string who = kv["tester"] + ", " + kv["date"];
    return kv["result"] == "pass" ? Level{Check::Passed, "owner test " + who} : Level{Check::Failed, "owner test failed " + who};
}

}  // namespace ai_arranger::ui::studio

#endif
