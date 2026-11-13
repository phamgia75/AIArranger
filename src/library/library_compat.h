// P0 Keyboard Compatibility card (parity C15, C16): for one library item, per target keyboard, what is DOCUMENTED, which
// documented limit the item exceeds, and what is UNKNOWN. READ-ONLY. Built only from:
//   - the validator's sourced rule tables (library_validate.h R-01..R-18): the item's type folder and extension (R-04),
//     the 48 slots of a SET bank file (R-16), the 4 GB User Sample memory as disk size (R-08), the one-way SET Data
//     Conversion (R-13), and, for the item's own layout, every validator finding on the item, its folders or the library;
//   - the documented import limit already in the repo: Pa5X USB devices are FAT32/FAT16 (MAN p.715), so one file is at
//     most 4 GiB - 1 byte (jobs/import_kit.h kFat32MaxFile).
// Every line cites its rule id and source. Nothing is hardware-verified: each target says "not verified on a <keyboard>"
// and the measured value (L3) is "not measured" until owner hardware evidence exists. Runs the validator: worker thread.
#ifndef AI_ARRANGER_LIBRARY_LIBRARY_COMPAT_H
#define AI_ARRANGER_LIBRARY_LIBRARY_COMPAT_H

#include "library/library_validate.h"

namespace ai_arranger::library {

enum class Compat { Documented, Exceeds, Unknown, Note };
inline const char* compatName(Compat c) {
    return c == Compat::Documented ? "DOCUMENTED" : c == Compat::Exceeds ? "EXCEEDS" : c == Compat::Unknown ? "UNKNOWN" : "NOTE";
}

struct CompatLine { Compat status; std::string rule, text, source; };
struct CompatTarget { std::string keyboard; const RuleTable* table; std::vector<CompatLine> lines; };

inline constexpr uintmax_t kFat32MaxFileBytes = (uintmax_t(1) << 32) - 1;   // same value as jobs::kit::kFat32MaxFile

// `v` is validate(L.root); its findings are used for the table of L's own layout only (the validator runs no other table).
inline std::vector<CompatTarget> compatCard(const Library& L, const Item& it, const Validation& v) {
    std::vector<CompatTarget> out;
    const std::string type = media::detail::upper(fs::path(it.file).begin()->string());
    const std::string ext = media::detail::upper(fs::path(it.file).extension().string());
    const bool fromSet = L.format == Format::PaSet;
    const RuleTable* own = tableFor(L.layout);
    for (const auto& [kbd, t] : {std::pair{"Pa5X", &pa5xKstTable()}, std::pair{"Pa4X family", &pa4xSetTable()},
                                 std::pair{"Pa700/Pa1000 family", &pa700SetTable()}}) {
        CompatTarget c{kbd, t, {}};
        auto add = [&](Compat s, const char* rule, const std::string& text) {
            const auto r = t->rules.find(rule);
            c.lines.push_back({s, rule, text, r == t->rules.end() ? "" : r->second.source});
        };
        const bool pa5x = t == &pa5xKstTable();
        // the folder format the target documents for User data
        if (pa5x && fromSet)   // R-13 as the SET tables cite it (the KST table's R-13 is the User Sample note)
            c.lines.push_back({Compat::Documented, "R-13", "a SET item reaches a Pa5X only through its one-way Data Conversion; the converted result is UNKNOWN",
                               pa4xSetTable().rules.at("R-13").source});
        else if (!pa5x && !fromSet) c.lines.push_back({Compat::Unknown, "-", "no source in the rule tables says this keyboard reads a Pa5X KST", ""});
        // type folder and extension (R-04)
        const auto te = t->types.find(type);
        if (te != t->types.end() && te->second.count(ext)) add(Compat::Documented, "R-04", type + "/" + ext + " is a file type of this table");
        else c.lines.push_back({Compat::Unknown, "R-04", type + "/" + (ext.empty() ? "(no extension)" : ext) + " is not in this table: whether the keyboard reads it is UNKNOWN",
                                t->rules.at("R-04").source});
        // 48 slots per bank file (R-16)
        if (t->bankFileTypes.count(type)) {
            if (it.slot < 0) add(Compat::Unknown, "R-16", "slot not known: the 48-slot bank limit cannot be checked");
            else if (it.slot >= 48) add(Compat::Exceeds, "R-16", "slot " + std::to_string(it.slot + 1) + " is past the 48 slots of a bank file");
            else add(Compat::Documented, "R-16", "slot " + std::to_string(it.slot + 1) + " is within the 48 slots of a bank file");
        }
        // 4 GB User Sample memory, as disk size (R-08); FAT32 file size (MAN p.715)
        if (pa5x && (it.type == "Sample" || it.type == "Multisample"))
            add(L.sampleDiskBytes > (uintmax_t(4) << 30) ? Compat::Exceeds : Compat::Documented, "R-08",
                "library sample data " + std::to_string(L.sampleDiskBytes) + " disk bytes vs 4 GB User Sample memory (disk size, not RAM)");
        if (pa5x) {
            const auto f = L.files.find(it.file);
            const uintmax_t size = f == L.files.end() ? it.bytes : f->second.size;
            c.lines.push_back({size > kFat32MaxFileBytes ? Compat::Exceeds : Compat::Documented, "FAT32",
                               "file " + std::to_string(size) + " bytes vs the FAT32 limit of 4 GiB - 1 byte", "MAN p.715 (FAT32/FAT16 USB devices); jobs/import_kit.h"});
        }
        // the validator's findings for the item's own layout: on the item, a folder above it, or the whole library
        if (t == own && v.ok)
            for (const auto& f : v.findings)
                if (f.path == "." || f.path == it.file || it.file.rfind(f.path + "/", 0) == 0)
                    c.lines.push_back({f.severity == Severity::Info ? Compat::Note : Compat::Exceeds, f.rule,
                                       std::string(severityName(f.severity)) + " " + f.path + ": " + f.message, f.source});
        if (t == own && !v.ok) c.lines.push_back({Compat::Unknown, "-", "structural validation incomplete: " + v.error, ""});
        c.lines.push_back({Compat::Unknown, "-", "content not decoded: whether the keyboard loads this item is UNKNOWN", "library_validate.h: file level only"});
        c.lines.push_back({Compat::Unknown, "L3", "measured on hardware: not measured (no owner hardware evidence)", ""});
        out.push_back(std::move(c));
    }
    return out;
}

inline std::string compatCardText(const Item& it, const std::vector<CompatTarget>& card) {
    std::ostringstream o;
    o << "Keyboard Compatibility card (read-only, from sourced rule tables): " << it.file << (it.name.empty() ? "" : "  " + it.name) << "\n";
    for (const auto& c : card) {
        o << "\n" << c.keyboard << ": NOT VERIFIED ON A " << media::detail::upper(c.keyboard) << (c.keyboard.find("family") != std::string::npos ? " KEYBOARD" : "") << "   [table: " << c.table->name << "]\n";
        for (const auto& l : c.lines) o << "  " << compatName(l.status) << "  " << l.rule << "  " << l.text << (l.source.empty() ? "" : "  [" + l.source + "]") << "\n";
    }
    o << "\nDOCUMENTED = a sourced rule this item meets, never a compatibility claim. EXCEEDS = a documented limit or rule this item does not\n"
         "meet. UNKNOWN = no source decides it. NOTE = documented behaviour to know. Nothing here is verified on a keyboard.\n";
    return o.str();
}

}  // namespace ai_arranger::library

#endif
