// P0 Keyboard Compatibility card (parity C15, C16): for one library item, per target keyboard, what is DOCUMENTED, which
// documented limit the item exceeds, and what is UNKNOWN. READ-ONLY. Every target keyboard is evaluated independently
// against ITS OWN rule table (never a copy of the source layout's result). Built only from:
//   - the validator's sourced rule tables (library_validate.h R-01..R-18): the item's type folder and extension (R-04),
//     the 48 slots of a SET bank file (R-16), the one-way SET Data Conversion (R-13), and the validator run with that
//     target's table: every finding on the item, its folders or the library, and any unresolved file error;
//   - R-08 (User Sample memory) is never inferred from disk sizes: UNKNOWN until measured or documented data exists;
//   - the documented import limit already in the repo: Pa5X USB devices are FAT32/FAT16 (MAN p.715), so one file is at
//     most 4 GiB - 1 byte (jobs/import_kit.h kFat32MaxFile).
// Every line cites its rule id and source. DOCUMENTED = a documented rule, applied by software; never "compatible". Each
// target has an explicit "hardware: not verified" line and L3 "not measured". Runs the validator: worker thread only.
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

// `v` = validate(L.root), reused for the target whose table it was run with; every other target's table is validated here.
inline std::vector<CompatTarget> compatCard(const Library& L, const Item& it, const Validation& v, const std::function<bool()>& keepGoing = {}) {
    std::vector<CompatTarget> out;
    const std::string type = media::detail::upper(fs::path(it.file).begin()->string());
    const std::string ext = media::detail::upper(fs::path(it.file).extension().string());
    const bool fromSet = L.format == Format::PaSet;
    for (const auto& [kbd, t] : {std::pair{"Pa5X", &pa5xKstTable()}, std::pair{"Pa4X family", &pa4xSetTable()},
                                 std::pair{"Pa700/Pa1000 family", &pa700SetTable()}}) {
        CompatTarget c{kbd, t, {}};
        auto add = [&](Compat s, const char* rule, const std::string& text) {
            const auto r = t->rules.find(rule);
            c.lines.push_back({s, rule, text, r == t->rules.end() ? "" : r->second.source});
        };
        const bool pa5x = t == &pa5xKstTable();
        c.lines.push_back({Compat::Unknown, "HW", std::string("hardware: not verified on a ") + kbd + (pa5x ? "" : " keyboard"), ""});
        // the folder format the target documents for User data
        if (pa5x && fromSet)   // R-13 as the SET tables cite it (the KST table's R-13 is the User Sample note)
            c.lines.push_back({Compat::Note, "R-13", "a SET item reaches a Pa5X only through its one-way Data Conversion; the converted result is UNKNOWN",
                               pa4xSetTable().rules.at("R-13").source});
        else if (!pa5x && !fromSet) c.lines.push_back({Compat::Unknown, "-", "no source in the rule tables says this keyboard reads a Pa5X KST", ""});
        // type folder and extension (R-04)
        const auto te = t->types.find(type);
        if (te != t->types.end() && te->second.count(ext)) add(Compat::Documented, "R-04", type + "/" + ext + " is a file type of this table");
        else c.lines.push_back({Compat::Unknown, "R-04", type + "/" + (ext.empty() ? "(no extension)" : ext) + " is not in this table: whether the keyboard reads it is UNKNOWN",
                                t->rules.at("R-04").source});
        // 48 slots per bank FILE (R-16): the highest slot of any item in the item's bank file, not only the selected one
        if (t->bankFileTypes.count(type) && t->rules.count("R-16")) {
            int top = -1;
            for (const auto& o : L.items) if (o.file == it.file) top = std::max(top, o.slot);
            if (top < 0) add(Compat::Unknown, "R-16", "slots not known: the 48-slot bank limit cannot be checked");
            else if (top >= 48) add(Compat::Exceeds, "R-16", "bank file " + it.file + " holds slot " + std::to_string(top + 1) + ", past the 48 slots of a bank file");
            else add(Compat::Documented, "R-16", "bank file " + it.file + " uses slots up to " + std::to_string(top + 1) + ", within the 48 slots of a bank file");
        }
        // R-08: sample memory is never inferred from disk sizes (the validator's disk-size R-08 is not shown either)
        if (t->rules.count("R-08") && (it.type == "Sample" || it.type == "Multisample"))
            add(Compat::Unknown, "R-08", "User Sample memory use UNKNOWN: no measured value and no documented per-item figure; disk size is not sample memory");
        if (pa5x) {
            const auto f = L.files.find(it.file);
            const uintmax_t size = f == L.files.end() ? it.bytes : f->second.size;
            c.lines.push_back({size > kFat32MaxFileBytes ? Compat::Exceeds : Compat::Documented, "FAT32",
                               "file " + std::to_string(size) + " bytes vs the FAT32 limit of 4 GiB - 1 byte", "MAN p.715 (FAT32/FAT16 USB devices); jobs/import_kit.h"});
        }
        // the validator run with THIS target's table: findings on the item, a folder above it, or the whole library (R-04,
        // R-16 are evaluated per item above; R-08 is never taken from disk sizes). A partial validation keeps the findings
        // made before it stopped, plus the unresolved file error.
        const Validation tv = v.table == t->name && v.root == L.root ? v : validate(L.root, keepGoing, kLibraryLimits, t);
        for (const auto& f : tv.findings)
            if (f.rule != "R-04" && f.rule != "R-08" && f.rule != "R-16" && (f.path == "." || f.path == it.file || it.file.rfind(f.path + "/", 0) == 0))
                c.lines.push_back({f.severity == Severity::Info ? Compat::Note : Compat::Exceeds, f.rule,
                                   std::string(severityName(f.severity)) + " " + f.path + ": " + f.message, f.source});
        if (!tv.ok) c.lines.push_back({Compat::Unknown, "-", "structural validation against this table incomplete; unresolved: " + tv.error, ""});
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
        o << "\n" << c.keyboard << ": documented rules applied by software   [table: " << c.table->name << "]\n";
        for (const auto& l : c.lines) o << "  " << compatName(l.status) << "  " << l.rule << "  " << l.text << (l.source.empty() ? "" : "  [" + l.source + "]") << "\n";
    }
    o << "\nDOCUMENTED = a documented rule, applied by software, that this item meets: rule provenance, not a verification, and never a\n"
         "compatibility claim. EXCEEDS = a documented limit or rule this item does not meet. UNKNOWN = no source or measurement decides\n"
         "it. NOTE = documented behaviour to know. Hardware: nothing here is verified on a keyboard.\n";
    return o.str();
}

}  // namespace ai_arranger::library

#endif
