// W4: dependency closure of a Pa5X KST folder (read-only). EXPERIMENTAL.
//
// Every Program Change in every MIDI leaf of every .STG/.KSC/.PDC Root tree is a sound reference (CC0 = MSB, CC32 =
// LSB, per channel, as last set in that track). A reference with MSB 121 (User Sound) or 120 (User Drum Kit) and
// LSB 64-75 is a user reference to SOUND/USER<LSB-63>.PCG or SOUND/USERDK<LSB-63>.PCG, directory slot = Program
// Change; it resolves when that file holds an item at that slot. Everything else points into the keyboard's ROM and
// is counted as factory/GM. Rule data-confirmed (98.9 % of 32 508 owner references, KST_DEPENDENCY_EVIDENCE.md D3-D5);
// not hardware-verified.
#ifndef AI_ARRANGER_MEDIA_PANATIVE_KST_CLOSURE_H
#define AI_ARRANGER_MEDIA_PANATIVE_KST_CLOSURE_H

#include "media/kst_inventory.h"
#include "media/panative/panative_reader.h"
#include "media/panative/root_tree.h"
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ai_arranger::media::panative::closure {

struct Select { int channel; int msb, lsb; int program; };   // msb/lsb -1 = not set before the Program Change

// Sound selects of one SMF in event order. False (error set) on any structure that runs past its chunk.
inline bool selects(const uint8_t* p, size_t n, std::vector<Select>& out, std::string& error) {
    using panative::detail::be32;
    auto fail = [&](const std::string& e) { error = e; return false; };
    if (n < 14 || std::memcmp(p, "MThd", 4) != 0) return fail("not a Standard MIDI File");
    const uint32_t hl = be32(p + 4);
    if (hl < 6 || hl > n - 8) return fail("header length is not 6..file size");
    const unsigned declared = unsigned(p[10]) << 8 | p[11];
    unsigned found = 0;
    size_t o = 8 + size_t(hl);
    auto vlq = [&](size_t& q, size_t end, uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; ++i) {
            if (q >= end) return false;
            const uint8_t b = p[q++];
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) return true;
        }
        return false;
    };
    while (n - o >= 8) {
        const uint32_t len = be32(p + o + 4);
        // Only MTrk chunks were observed (231 087/231 087 owner MIDI leaves): anything else is refused, not skipped.
        if (std::memcmp(p + o, "MTrk", 4) != 0) return fail("chunk other than MTrk");
        o += 8;
        if (len > n - o) return fail("chunk runs past the file");
        const size_t end = o + len;
        ++found;
        bool endOfTrack = false;
        std::map<std::pair<int, int>, int> bank;   // (channel, 0|32) -> value, per track
        uint8_t status = 0;
        while (o < end) {
            uint32_t v;
            if (endOfTrack) return fail("event after End of Track");
            if (!vlq(o, end, v) || o >= end) return fail("event runs past its track");
            const uint8_t b = p[o];
            if (b == 0xFF) {
                if (end - o < 2) return fail("meta event runs past its track");
                const uint8_t type = p[o + 1];
                o += 2;
                if (!vlq(o, end, v) || v > end - o) return fail("meta event runs past its track");
                endOfTrack = type == 0x2F && v == 0;
                o += v;
                continue;
            }
            if (b == 0xF0 || b == 0xF7) {
                ++o;
                if (!vlq(o, end, v) || v > end - o) return fail("SysEx runs past its track");
                o += v;
                continue;
            }
            if (b & 0x80) { status = b; ++o; }
            if (status < 0x80 || status >= 0xF0) return fail("data byte without a running status");
            const int hi = status & 0xF0, ch = (status & 0x0F) + 1, k = hi == 0xC0 || hi == 0xD0 ? 1 : 2;
            if (size_t(k) > end - o) return fail("channel message runs past its track");
            if (hi == 0xB0 && (p[o] == 0 || p[o] == 32)) bank[{ch, p[o]}] = p[o + 1];
            else if (hi == 0xC0) {
                auto get = [&](int c) { auto it = bank.find({ch, c}); return it == bank.end() ? -1 : it->second; };
                out.push_back({ch, get(0), get(32), p[o]});
            }
            o += size_t(k);
        }
        if (!endOfTrack) return fail("track does not end with End of Track");
    }
    if (o != n) return fail("trailing bytes after the last track");
    if (found != declared) return fail("header declares " + std::to_string(declared) + " tracks, file has " + std::to_string(found));
    return true;
}

struct Result {
    bool ok = false;               // every relevant file was read and parsed
    std::string error;
    size_t trees = 0, midis = 0;
    size_t resolved = 0, slotEmpty = 0, fileAbsent = 0, factory = 0;
    std::vector<std::string> problems;   // unreadable / unparsable files (relative paths)
    std::map<std::string, size_t> dangling;   // "USER11.PCG slot 40" -> count
    size_t danglingCount() const { return slotEmpty + fileAbsent; }
};

inline std::string upper(std::string s) { for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }

inline void collect(const roottree::Node& n, std::vector<const roottree::Node*>& mids) {
    for (const auto& c : n.children) {
        if (c.kind == roottree::Node::Leaf) { if (c.name.size() > 4 && c.name.compare(c.name.size() - 4, 4, ".mid") == 0) mids.push_back(&c); }
        else collect(c, mids);
    }
}

// Read-only check of one KST folder.
inline Result check(const std::filesystem::path& kst) {
    Result r;
    media::detail::Listing ls;
    if (!media::detail::listFiles(kst, ls, r.error)) return r;
    std::map<std::string, std::set<int>> slots;   // upper-case SOUND file name -> occupied slots
    for (const auto& [rel, e] : ls.files) {
        const auto slash = rel.find('/');
        if (slash == std::string::npos || upper(rel.substr(0, slash)) != "SOUND" || rel.find('/', slash + 1) != std::string::npos) continue;
        const std::string name = upper(rel.substr(slash + 1));
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".PCG") != 0) continue;
        const auto c = panative::readFile(e.abs);
        if (!c.ok) { r.problems.push_back(rel + ": " + c.error); continue; }
        auto& s = slots[name];
        for (const auto& it : c.items) s.insert(it.slot);
    }
    for (const auto& [rel, e] : ls.files) {
        const std::string ext = upper(std::filesystem::path(rel).extension().string());
        if (ext != ".STG" && ext != ".KSC" && ext != ".PDC") continue;
        const auto t = roottree::readFile(e.abs);
        if (!t.ok) { r.problems.push_back(rel + ": " + t.error); continue; }
        ++r.trees;
        std::vector<const roottree::Node*> mids;
        collect(t.root, mids);
        for (const auto* m : mids) {
            ++r.midis;
            std::vector<Select> sel;
            std::string err;
            if (!selects(reinterpret_cast<const uint8_t*>(m->payload.data()), m->payload.size(), sel, err)) { r.problems.push_back(rel + ":" + m->name + ": " + err); continue; }
            for (const auto& s : sel) {
                if ((s.msb == 120 || s.msb == 121) && s.lsb >= 64 && s.lsb <= 75) {
                    char f[24];
                    std::snprintf(f, sizeof f, s.msb == 121 ? "USER%02d.PCG" : "USERDK%02d.PCG", s.lsb - 63);
                    const auto it = slots.find(f);
                    if (it == slots.end()) ++r.fileAbsent;
                    else if (!it->second.count(s.program)) ++r.slotEmpty;
                    else { ++r.resolved; continue; }
                    ++r.dangling[std::string(f) + " slot " + std::to_string(s.program)];
                } else ++r.factory;
            }
        }
    }
    r.ok = r.problems.empty();
    return r;
}

}  // namespace ai_arranger::media::panative::closure

#endif
