// Library Manager: the staged plan survives a restart. A plan file holds ONLY the steps (not the libraries), tagged
// with a key of the content of both libraries: steps name items by index, so a plan is restored only against the same
// libraries (same files, same SHA-256) and is otherwise refused with the reason. Format: versioned text, one step per
// line (tab separated, percent-escaped), and a last line "sha256 <hex>" over everything before it.
// Written atomically (temp file in the same folder, fsync, one rename) and NEVER inside either library: a source
// library is only read, as everywhere in the Library Manager.
#ifndef AI_ARRANGER_LIBRARY_LIBRARY_PLAN_STORE_H
#define AI_ARRANGER_LIBRARY_LIBRARY_PLAN_STORE_H

#include "library/library_plan.h"
#include "media/sha256.h"
#include <fcntl.h>
#include <unistd.h>

namespace ai_arranger::library {

inline constexpr const char* kPlanFileMagic = "AIARRANGER-LIBRARY-PLAN 1";   // bump the number on any format change

// Same libraries = same format, layout and (path, size, SHA-256) of every file. The folder's own path is not part of it,
// so a library that moved still matches; a changed, added or removed file does not.
inline std::string libraryKey(const Library& L) {
    media::Sha256 h;
    auto add = [&](const std::string& s) { h.update(s.data(), s.size()); const char z = 0; h.update(&z, 1); };
    add(formatName(L.format)); add(L.layout);
    for (const auto& [rel, f] : L.files) { add(rel); add(std::to_string(f.size)); add(f.sha256); }   // std::map: sorted
    return h.hex();
}

namespace detail {
inline std::string esc(const std::string& s) {
    static const char* d = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == '%' || c == 0x7F) { o += '%'; o += d[c >> 4]; o += d[c & 15]; } else o += char(c);
    }
    return o;
}
inline bool unesc(const std::string& s, std::string& o) {
    o.clear();
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') { o += s[i]; continue; }
        if (i + 2 >= s.size()) return false;
        const int a = hex(s[i + 1]), b = hex(s[i + 2]);
        if (a < 0 || b < 0) return false;
        o += char(a * 16 + b);
        i += 2;
    }
    return true;
}
inline std::vector<std::string> splitTab(const std::string& s) {
    std::vector<std::string> v;
    size_t from = 0;
    for (;;) { const size_t t = s.find('\t', from); v.push_back(s.substr(from, t == std::string::npos ? t : t - from)); if (t == std::string::npos) return v; from = t + 1; }
}
inline std::string sha(const std::string& s) { media::Sha256 h; h.update(s.data(), s.size()); return h.hex(); }
}  // namespace detail

// keyA / keyB: libraryKey() of each library, computed once when it was opened (it costs ~0.6 us per file: 120 ms at 100k files).
inline std::string serializePlan(const std::vector<Step>& plan, const std::string& keyA, const std::string& keyB) {
    std::string body = std::string(kPlanFileMagic) + "\nA " + keyA + "\nB " + keyB + "\n";
    for (const auto& s : plan)
        body += "step\t" + std::to_string(int(s.op)) + "\t" + s.side + "\t" + std::to_string(s.item) + "\t" + detail::esc(s.bank) + "\t" +
                std::to_string(s.slot) + "\t" + detail::esc(s.text) + "\t" + (s.wholeFile ? "1" : "0") + "\n";
    return body + "sha256 " + detail::sha(body) + "\n";
}

// "" = `out` holds the plan; else why the file was refused (out is left empty). Every field is validated: a hand-edited
// or damaged file can never produce a step the UI could not have made.
inline std::string parsePlan(const std::string& text, const std::string& keyA, const std::string& keyB, const Library& A, const Library& B, std::vector<Step>& out) {
    out.clear();
    const size_t tail = text.rfind("sha256 ");
    if (tail == std::string::npos || (tail != 0 && text[tail - 1] != '\n')) return "not a plan file (no checksum line)";
    const std::string body = text.substr(0, tail);
    if (text.substr(tail) != "sha256 " + detail::sha(body) + "\n") return "the plan file is damaged (checksum mismatch)";
    std::vector<std::string> lines;
    for (size_t from = 0; from < body.size();) { const size_t n = body.find('\n', from); lines.push_back(body.substr(from, n - from)); from = n + 1; }
    if (lines.size() < 3 || lines[0] != kPlanFileMagic) return "unknown plan file version (expected \"" + std::string(kPlanFileMagic) + "\")";
    if (lines[1] != "A " + keyA || lines[2] != "B " + keyB) return "the plan was made for different libraries (their files changed)";
    std::vector<Step> steps;
    for (size_t i = 3; i < lines.size(); ++i) {
        const auto f = detail::splitTab(lines[i]);
        const std::string at = "step " + std::to_string(i - 2) + ": ";
        if (f.size() != 8 || f[0] != "step" || f[2].size() != 1 || (f[2] != "A" && f[2] != "B") || (f[7] != "0" && f[7] != "1")) return at + "malformed";
        Step s;
        long long op = -1, slot = -2;
        unsigned long long item = 0;
        auto num = [](const std::string& t, auto& v) { size_t used = 0; try { v = std::is_signed_v<std::remove_reference_t<decltype(v)>> ? std::stoll(t, &used) : std::stoull(t, &used); } catch (...) { return false; } return used == t.size(); };
        if (!num(f[1], op) || !num(f[3], item) || !num(f[5], slot)) return at + "malformed number";
        if (op < 0 || op > int(Op::BankSwap)) return at + "unknown operation";
        const Library& L = f[2] == "A" ? A : B;
        if (item >= L.items.size()) return at + "item index outside the library";   // the UI sets the selected item for every operation
        s.op = static_cast<Op>(op); s.side = f[2][0]; s.item = size_t(item); s.slot = slot; s.wholeFile = f[7] == "1";
        if (!detail::unesc(f[4], s.bank) || !detail::unesc(f[6], s.text)) return at + "bad escape";
        // Only what addStep() can make: no target slot (-1), Rename has a name and no bank, Copy/Move have no text,
        // BankSwap has two banks and is never whole-file.
        const bool shape = slot == -1 && (s.op == Op::Rename ? s.bank.empty() && !s.text.empty()
                                          : s.op == Op::BankSwap ? !s.bank.empty() && !s.text.empty() && !s.wholeFile
                                                                 : s.text.empty());
        if (!shape) return at + "fields no plan step can have";
        steps.push_back(std::move(s));
    }
    out = std::move(steps);
    return "";
}

// The plan file for this pair of libraries inside `dir` (the app's own data folder).
inline fs::path planFilePath(const fs::path& dir, const std::string& keyA, const std::string& keyB) {
    return dir / ("plan-" + keyA.substr(0, 16) + "-" + keyB.substr(0, 16) + ".aiaplan");
}

// Atomic save. Refuses a destination inside either library. "" = written (and read back equal), else the reason.
inline std::string savePlan(const fs::path& file, const std::string& text, const Library& A, const Library& B) {
    for (const Library* L : {&A, &B})
        if (!L->root.empty() && media::samplelab::sameOrBelow(file.parent_path(), L->root)) return "refused: the plan file would be inside a source library";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".INCOMPLETE";
    ::unlink(tmp.c_str());   // a stale or planted leftover (even a symlink) is removed, never followed or truncated
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) return "cannot create " + tmp.string();
    size_t done = 0;
    while (done < text.size()) { const ssize_t n = ::write(fd, text.data() + done, text.size() - done); if (n <= 0) break; done += size_t(n); }
    const bool ok = done == text.size() && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) { fs::remove(tmp, ec); return "cannot write the plan file"; }
    fs::rename(tmp, file, ec);   // replaces the previous plan file in one step
    if (ec) { fs::remove(tmp, ec); return "cannot publish the plan file"; }
    return "";
}

inline std::string loadPlanText(const fs::path& file, std::string& text) {
    std::error_code ec;
    const auto size = fs::is_regular_file(file, ec) ? fs::file_size(file, ec) : 0;   // a symlink or special file is not a plan
    if (ec || !fs::is_regular_file(fs::symlink_status(file, ec))) return "no saved plan";
    if (size > (1u << 22)) return "plan file too large";   // checked before reading: a huge file is never loaded
    std::ifstream in(file, std::ios::binary);
    if (!in) return "no saved plan";
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return "";
}

}  // namespace ai_arranger::library

#endif
