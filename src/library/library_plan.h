// E7 SET/KST Manager PLANNING: copy / move / rename / bank-swap steps between two read-only libraries, checked step by
// step against a simulated state of both libraries, and exported as a staged plan document (plan.json + PLAN.txt).
//
// No native-format write exists or happens here. The only verified writer for keyboard data is Backup KST
// (media/kst_backup.h, tools/kst_copy.py): a byte copy of a WHOLE folder into a new folder. It cannot apply a single
// copy, move, rename or bank swap, so every plan is EXPORT-ONLY and says so in both files. The export itself is a new
// local folder written by the shared Sample Lab folder writer (NAME.INCOMPLETE, every file hashed, PACKAGE.sha256,
// checked, published by one no-replace rename; mounted volumes refused; never inside either library).
#ifndef AI_ARRANGER_LIBRARY_LIBRARY_PLAN_H
#define AI_ARRANGER_LIBRARY_LIBRARY_PLAN_H

#include "library/library_model.h"
#include "media/samplelab/sample_lab_io.h"   // writeLabFolder, sameOrBelow
#include "media/sound/resource_graph.h"      // graph::jsonEscape

namespace ai_arranger::library {

enum class Op { Copy, Move, Rename, BankSwap };
inline const char* opName(Op o) { return o == Op::Copy ? "copy" : o == Op::Move ? "move" : o == Op::Rename ? "rename" : "bank-swap"; }

struct Step {
    Op op = Op::Copy;
    char side = 'A';          // the library the step starts from (Copy/Move: source; Rename/BankSwap: the library changed)
    size_t item = 0;          // Copy/Move/Rename: index into that library's items
    std::string bank;         // Copy/Move: target bank in the other library ("" = same bank id); BankSwap: first bank
    int slot = -1;            // Copy/Move: 0-based target slot (SET); -1 = first free
    std::string text;         // Rename: new name; BankSwap: second bank
};

struct StepCheck { std::string summary; std::vector<std::string> conflicts, warnings; };
struct Evaluation {
    std::vector<StepCheck> steps;
    size_t conflicts = 0, warnings = 0;
    bool blocked() const { return conflicts > 0; }
};

inline constexpr const char* kApplyReason =
    "EXPORT-ONLY: no verified writer can apply these steps. The only verified writer for keyboard data is Backup KST "
    "(a byte copy of a whole folder into a new folder); it cannot copy, move, rename or swap single items or banks, and "
    "AIArranger writes no Pa-native or KST file. Carry the steps out on the keyboard (or by hand on a Backup KST copy), "
    "then check the result with Library Compare. Both libraries were only read.";

namespace detail {
struct SimItem { std::string type, bank, name, file, sha; int slot; bool gone = false; };
struct Sim { Format format; std::vector<SimItem> items; };

inline Sim simOf(const Library& L) {
    Sim s{L.format, {}};
    for (const auto& it : L.items) {
        const auto f = L.files.find(it.file);
        s.items.push_back({it.type, it.bank, trimmed(it.name), it.file, f == L.files.end() ? "" : f->second.sha256, it.slot});
    }
    return s;
}
inline std::string fileName(const std::string& rel) { return fs::path(rel).filename().string(); }
// Names, banks and files are compared ignoring case, as media::planMerge does: how the keyboard treats case is UNKNOWN.
inline bool same(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
// `file` moved from bank `from` to bank `to` (KST "TYPE/BANK/x.STG", SET "TYPE/BANK.STY"); unchanged if not in `from`.
inline bool rebank(std::string& file, const std::string& from, const std::string& to) {
    if (file.size() < from.size() || !same(file.substr(0, from.size()), from)) return false;
    const std::string rest = file.substr(from.size());
    if (!rest.empty() && rest[0] != '/' && rest[0] != '.') return false;   // "USER01" is not a prefix of "USER010"
    file = to + rest;
    return true;
}
inline std::string slotText(int s) { return s >= 0 ? " slot " + std::to_string(s + 1) : ""; }

// Names are written by the keyboard's own UI: printable ASCII, no path separator. Limits beyond that are UNKNOWN.
inline std::string badName(const std::string& n) {
    if (trimmed(n).empty()) return "the new name is empty";
    for (unsigned char c : n) if (c < 0x20 || c > 0x7E || c == '/' || c == '\\') return "the new name holds a character outside printable ASCII or a slash";
    return "";
}
}  // namespace detail

// Applies the steps in order to simulated copies of A and B and records, per step, what blocks it (conflicts) and
// what to look at (warnings: dependencies, duplicates). Read-only: the libraries themselves are never touched.
inline Evaluation evaluate(const Library& A, const Library& B, const std::vector<Step>& plan) {
    Evaluation ev;
    detail::Sim sa = detail::simOf(A), sb = detail::simOf(B);
    for (const auto& st : plan) {
        StepCheck c;
        const Library& L = st.side == 'A' ? A : B;
        detail::Sim& S = st.side == 'A' ? sa : sb;
        detail::Sim& D = st.side == 'A' ? sb : sa;
        const char other = st.side == 'A' ? 'B' : 'A';
        auto conflict = [&](std::string s) { c.conflicts.push_back(std::move(s)); };
        auto warn = [&](std::string s) { c.warnings.push_back(std::move(s)); };
        if (st.op == Op::BankSwap) {
            c.summary = std::string("bank-swap in ") + st.side + ": " + st.bank + " <-> " + st.text;
            std::string tx, ty;
            for (const auto& it : S.items) if (!it.gone) { if (detail::same(it.bank, st.bank)) tx = it.type; if (detail::same(it.bank, st.text)) ty = it.type; }
            if (detail::same(st.bank, st.text)) conflict("both banks are the same bank");
            else if (st.bank == "(root)" || st.text == "(root)") conflict("files at the library root are not a bank");
            else if (tx.empty() && ty.empty()) conflict("neither bank holds an item in " + std::string(1, st.side));
            else if (!tx.empty() && !ty.empty() && tx != ty) conflict("the banks hold different types (" + tx + ", " + ty + ")");
            else {
                for (auto& it : S.items) {   // gone items too: their paths must not shadow the swapped ones
                    if (detail::same(it.bank, st.bank)) { it.bank = st.text; detail::rebank(it.file, st.bank, st.text); }
                    else if (detail::same(it.bank, st.text)) { it.bank = st.bank; detail::rebank(it.file, st.text, st.bank); }
                }
                warn("what refers to these banks by bank and slot (SongBook entries, Keyboard Sets, Style settings) is not decoded: "
                     "those references may point elsewhere after the swap (UNKNOWN)");
            }
        } else if (st.item >= L.items.size() || S.items[st.item].gone) {
            c.summary = std::string(opName(st.op)) + " from " + st.side + ": item " + std::to_string(st.item);
            conflict("the item is not in library " + std::string(1, st.side) + (st.item < L.items.size() ? " any more (an earlier step moved it)" : ""));
        } else if (st.op == Op::Rename) {
            auto& it = S.items[st.item];
            c.summary = std::string("rename in ") + st.side + ": " + it.bank + detail::slotText(it.slot) + " \"" + it.name + "\" -> \"" + st.text + "\"";
            if (const auto why = detail::badName(st.text); !why.empty()) conflict(why);
            else {
                for (const auto& o : S.items)
                    if (&o != &it && !o.gone && detail::same(o.bank, it.bank) && detail::same(o.name, trimmed(st.text))) warn("the name is already used in " + it.bank + detail::slotText(o.slot));
                if (S.format == Format::PaSet && st.text.size() > 18)
                    warn("longer than 18 characters: the 24-byte Pa-native directory holds 18 (observed); this file's limit is UNKNOWN");
                if (S.format != Format::PaSet) {   // a KST/folder item is a file: the rename renames it
                    const std::string to = (fs::path(it.file).parent_path() / (trimmed(st.text) + fs::path(it.file).extension().string())).generic_string();
                    for (const auto& o : S.items) if (&o != &it && !o.gone && detail::same(o.file, to)) conflict("the file " + to + " already exists in " + std::string(1, st.side));
                    if (c.conflicts.empty()) it.file = to;
                }
                if (c.conflicts.empty()) it.name = trimmed(st.text);
            }
        } else {   // Copy or Move into the other library
            const auto& src = S.items[st.item];
            const std::string bank = st.bank.empty() ? src.bank : st.bank;
            c.summary = std::string(opName(st.op)) + " " + st.side + ":" + src.bank + detail::slotText(src.slot) + " \"" + src.name + "\" -> " + other + ":" + bank;
            if (S.format != D.format) conflict(std::string("formats differ (") + formatName(S.format) + " -> " + formatName(D.format) + "): converting is not available");
            std::string btype;
            std::set<int> used;
            bool unknownOccupancy = false;   // an undecoded SET bank file: which slots are taken is UNKNOWN
            for (const auto& o : D.items) if (!o.gone && detail::same(o.bank, bank)) { unknownOccupancy |= o.slot < 0; btype = btype.empty() || btype == o.type ? o.type : "mixed"; used.insert(o.slot); }
            if (!btype.empty() && btype != src.type) conflict("bank " + bank + " in " + other + " holds " + btype + ", not " + src.type);
            int slot = -1;
            bool already = false;   // the same bytes are already at the target path: nothing to add
            std::string file = src.file;
            if (D.format == Format::PaSet && S.format == Format::PaSet) {
                const int cap = bankCapacity(D.format, src.type);
                slot = st.slot;
                if (slot < 0 && !unknownOccupancy) { slot = 0; while (used.count(slot)) ++slot; }
                if (src.slot < 0) conflict("the source is a whole SET bank file that was not read as a Pa-native container, not a single item");
                if (unknownOccupancy)
                    conflict("occupancy of " + std::string(1, other) + ":" + bank + " is UNKNOWN (its file was not read as a Pa-native container): no slot can be chosen");
                else if (cap && st.slot >= cap) conflict("slot " + std::to_string(st.slot + 1) + " is past the " + std::to_string(cap) + " slots of a bank file (MAN4 p.975)");
                else if (cap && slot >= cap) conflict("bank " + bank + " is full (" + std::to_string(cap) + " per bank file, MAN4 p.975)");
                else if (used.count(slot)) {
                    for (const auto& o : D.items) if (!o.gone && detail::same(o.bank, bank) && o.slot == slot)
                        conflict("slot " + std::to_string(slot + 1) + " of " + bank + " is taken by \"" + o.name + "\": the copy would replace it");
                }
                c.summary += detail::slotText(slot);
                file = bank + fs::path(src.file).extension().string();
                warn("a SET item lives inside its bank file: applying this needs a Pa-native writer, which does not exist");
            } else if (D.format == S.format) {
                file = (bank == "(root)" ? "" : bank + "/") + detail::fileName(src.file);
                for (const auto& o : D.items) {
                    if (o.gone || !detail::same(o.file, file)) continue;
                    if (o.sha == src.sha) { already = true; warn(std::string(1, other) + " already has " + file + " with the same bytes: nothing to do"); }
                    else conflict(std::string(1, other) + " already has " + file + " with other bytes: the copy would replace it");
                }
            }
            for (const auto& o : D.items)
                if (!o.gone && detail::same(o.bank, bank) && detail::same(o.name, src.name) && !detail::same(o.file, file)) warn("the name \"" + src.name + "\" is already used in " + bank + detail::slotText(o.slot));
            if (D.format == Format::Pa5xKst || D.format == Format::Unknown)
                for (const auto& o : D.items) if (!o.gone && !o.sha.empty() && o.sha == src.sha && !detail::same(o.file, file)) { warn("duplicate: same bytes as " + std::string(1, other) + ":" + o.file); break; }
            for (const auto& d : itemDependencies(L, L.items[st.item])) warn("dependency " + d.status + ": " + d.to + " (" + d.reason + ")");
            if (src.type == "Sample" || src.type == "Multisample")
                warn("loading a whole KST that holds a PCM folder deletes all User Samples in memory (Pa5X manual p.736): load single Sounds");
            if (st.op == Op::Move) warn(std::string("library ") + st.side + " is only read: removing the item there is a step for the keyboard");
            if (c.conflicts.empty()) {
                if (!already) D.items.push_back({src.type, bank, src.name, file, src.sha, slot});
                if (st.op == Op::Move) S.items[st.item].gone = true;
            }
        }
        ev.conflicts += c.conflicts.size();
        ev.warnings += c.warnings.size();
        ev.steps.push_back(std::move(c));
    }
    return ev;
}

inline std::string planReport(const Library& A, const Library& B, const std::vector<Step>& plan, const Evaluation& ev) {
    std::ostringstream o;
    o << "AIArranger library plan (staged; nothing applied)\nA: " << A.root << "  (" << formatName(A.format) << ")\nB: " << B.root << "  ("
      << formatName(B.format) << ")\n"
      << "Status: " << (plan.empty() ? "EMPTY" : ev.blocked() ? "BLOCKED by conflicts" : "READY TO EXPORT") << " · " << plan.size() << " steps, "
      << ev.conflicts << " conflicts, " << ev.warnings << " warnings\nApply: " << kApplyReason << "\n\n";
    for (size_t i = 0; i < ev.steps.size(); ++i) {
        o << i + 1 << ". " << ev.steps[i].summary << "\n";
        for (const auto& x : ev.steps[i].conflicts) o << "     CONFLICT  " << x << "\n";
        for (const auto& x : ev.steps[i].warnings) o << "     warning   " << x << "\n";
    }
    return o.str();
}

inline std::string planJson(const Library& A, const Library& B, const std::vector<Step>& plan, const Evaluation& ev) {
    using media::graph::jsonEscape;
    auto q = [](const std::string& s) { return "\"" + jsonEscape(s) + "\""; };
    auto list = [&](const std::vector<std::string>& v) {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + q(v[i]);
        return s + "]";
    };
    auto lib = [&](const Library& L) {
        return "{\"path\": " + q(L.root) + ", \"format\": " + q(formatName(L.format)) + ", \"layout\": " + q(L.layout) +
               ", \"files\": " + std::to_string(L.files.size()) + ", \"bytes\": " + std::to_string(L.bytes) + "}";
    };
    std::ostringstream o;
    o << "{\n  \"format\": \"aiarranger-library-plan\",\n  \"version\": 1,\n  \"apply\": \"export-only\",\n  \"apply_reason\": " << q(kApplyReason)
      << ",\n  \"status\": " << q(plan.empty() ? "empty" : ev.blocked() ? "blocked" : "ready") << ",\n  \"libraries\": {\"A\": " << lib(A)
      << ", \"B\": " << lib(B) << "},\n  \"steps\": [";
    for (size_t i = 0; i < plan.size(); ++i) {
        const auto& st = plan[i];
        const Library& L = st.side == 'A' ? A : B;
        o << (i ? "," : "") << "\n    {\"n\": " << i + 1 << ", \"op\": " << q(opName(st.op)) << ", \"side\": " << q(std::string(1, st.side));
        if (st.op != Op::BankSwap && st.item < L.items.size()) {
            const auto& it = L.items[st.item];
            const auto f = L.files.find(it.file);
            o << ", \"item\": {\"file\": " << q(it.file) << ", \"type\": " << q(it.type) << ", \"bank\": " << q(it.bank) << ", \"slot\": " << it.slot
              << ", \"name\": " << q(it.name) << ", \"file_sha256\": " << q(f == L.files.end() ? "" : f->second.sha256) << "}";
        }
        if (st.op == Op::Copy || st.op == Op::Move) o << ", \"to_bank\": " << q(st.bank) << ", \"to_slot\": " << st.slot;
        if (st.op == Op::Rename) o << ", \"new_name\": " << q(st.text);
        if (st.op == Op::BankSwap) o << ", \"banks\": [" << q(st.bank) << ", " << q(st.text) << "]";
        o << ", \"summary\": " << q(ev.steps[i].summary) << ", \"conflicts\": " << list(ev.steps[i].conflicts) << ", \"warnings\": " << list(ev.steps[i].warnings)
          << ", \"applicable\": false}";
    }
    o << "\n  ]\n}\n";
    return o.str();
}

// Writes plan.json + PLAN.txt as the NEW local folder `dest`. "" = written and checked, or the reason it was not.
inline std::string exportPlan(const Library& A, const Library& B, const std::vector<Step>& plan, const fs::path& dest) {
    if (plan.empty()) return "refusing: the plan has no steps";
    for (const Library* L : {&A, &B})
        if (media::samplelab::sameOrBelow(dest, L->root)) return "refusing: " + dest.string() + " is inside library " + L->root + " (sources are only read)";
    const auto ev = evaluate(A, B, plan);
    const auto b = [](const std::string& s) { return media::samplelab::bytesOf(s); };
    return media::samplelab::writeLabFolder(dest, {{"plan.json", b(planJson(A, B, plan, ev))}, {"PLAN.txt", b(planReport(A, B, plan, ev))}});
}

}  // namespace ai_arranger::library

#endif
