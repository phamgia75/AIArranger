#ifndef AI_ARRANGER_MEDIA_SOUND_KORG_KMP_SOURCE_H
#define AI_ARRANGER_MEDIA_SOUND_KORG_KMP_SOURCE_H

// Route B of docs/research/KRONOS_KMP_KSF_IMPORT.md: an Instrument from KRONOS (and TRITON/OASYS/M3) Korg-format
// multisamples, the .KMP + .KSF files written by the KRONOS "Save Sampling Data" (Parameter Guide En 3, p.855-856).
// The layout is Korg's own documentation, KRONOS Parameter Guide En 3 p.1158-1159 "Korg format sample files":
// chunked, big endian; KMP = MSP1 [NAME] RLP1 [RLP2] [RLP3] [MNO1]; KSF = SMP1 SMD1 [SNO1] [NAME]. The SMD1 attribute
// bits are from the figure on p.1159 (MSB bit 7 Loop Off, 6 Reverse, 5 Not Use 2nd Start, 4 Compressed, 3-0
// Compression ID; bit 0 = +12 dB for uncompressed data). Nothing is guessed: every chunk or value outside that
// documentation is refused with the file and offset. Reads only; the source folder is never modified.
//
// Mapping (p.1158 and Sampling P2 p.709-710): a KMP holds one key-split multisample; region i plays from the previous
// region's top key + 1 (the first from C-1) up to its own top key; addresses are in single-sample units and a sample
// plays Start -> End, looping Loop Start -> End when Loop is on. Stereo multisamples are saved as two KMPs named
// "<name> -L" / "<name> -R" (p.836, p.1161); they are joined into stereo samples. Velocity layers live in the KRONOS
// Program (.PCG, not documented), so the musician names the layer order and splits, as for a WAV folder.

#include "media/sound/instrument.h"
#include "media/sound/sound_recipe.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace ai_arranger::media::sound {

namespace korg {

inline uint32_t be32(const std::vector<uint8_t>& b, size_t o) { return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3]; }

inline std::string text(const std::vector<uint8_t>& b, size_t o, size_t n) {   // fixed-length name: trailing NULs/spaces dropped
    std::string s(reinterpret_cast<const char*>(&b[o]), n);
    s = s.substr(0, s.find('\0'));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

inline std::string hex(size_t o) { char t[16]; std::snprintf(t, sizeof t, "0x%zx", o); return t; }

// Walks the chunks: f(id, body offset, length) returns "" or a refusal. Chunks are back to back, no padding.
// Each chunk ID may appear once: a second copy could silently override the first.
template <class F> std::string walk(const std::vector<uint8_t>& b, F f) {
    size_t o = 0;
    std::vector<std::string> seen;
    while (o < b.size()) {
        if (b.size() - o < 8) return "trailing bytes at " + hex(o);
        const std::string id(reinterpret_cast<const char*>(&b[o]), 4);
        const size_t len = be32(b, o + 4), body = o + 8;
        if (len > b.size() - body) return "chunk '" + id + "' at " + hex(o) + " runs past the end of the file";
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) return "chunk '" + id + "' at " + hex(o) + " appears twice";
        seen.push_back(id);
        if (const auto e = f(id, body, len); !e.empty()) return e + " (chunk '" + id + "' at " + hex(o) + ")";
        o = body + len;
    }
    return "";
}

}  // namespace korg

struct KmpRegion {
    uint8_t lo = 0, hi = 127, root = 60;
    bool fixedPitch = false;          // RLP1 original key MSB: 1 = Non Transpose
    int8_t tune = 0, level = 0;
    std::string ksf;                  // as written in RLP1 ("SKIPPEDSAMPL" = no sample)
};

struct Kmp {
    std::string name;                 // NAME (24) when present, else MSP1 (16)
    std::vector<KmpRegion> regions;
    int8_t rlp2[4] = {0, 0, 0, 0};    // transpose, resonance, attack, decay (relative, not in a Pa5X package)
    int8_t rlp3[5] = {0, 0, 0, 0, 0}; // drive, boost, low/mid/high EQ
};

// "" on success, else why the file is refused.
inline std::string readKmp(const std::vector<uint8_t>& b, Kmp* k) {
    *k = {};
    bool msp = false, rlp1 = false;
    size_t n = 0;
    std::string name16, name24;
    const auto e = korg::walk(b, [&](const std::string& id, size_t o, size_t len) -> std::string {
        if (id == "MSP1") {
            if (len != 18) return "MSP1 must be 18 bytes";
            name16 = korg::text(b, o, 16);
            n = b[o + 16];
            msp = true;
        } else if (id == "NAME") {
            if (len != 24) return "NAME must be 24 bytes";
            name24 = korg::text(b, o, 24);
        } else if (id == "RLP1") {
            if (!msp) return "RLP1 before MSP1";
            if (n == 0 || len != 18 * n) return "RLP1 must hold 18 bytes for each of the " + std::to_string(n) + " samples";
            uint8_t lo = 0;
            for (size_t i = 0; i < n; ++i) {
                const size_t r = o + 18 * i;
                KmpRegion g;
                g.fixedPitch = b[r] & 0x80;
                g.root = b[r] & 0x7F;
                if (b[r + 1] > 127) return "region " + std::to_string(i + 1) + ": top key above 127";
                g.lo = lo;
                g.hi = b[r + 1];
                if (g.hi < g.lo) return "region " + std::to_string(i + 1) + ": top key below the previous region's";
                g.tune = static_cast<int8_t>(b[r + 2]);
                g.level = static_cast<int8_t>(b[r + 3]);
                if (g.tune < -99 || g.tune > 99) return "region " + std::to_string(i + 1) + ": tune outside -99..+99";
                if (g.level < -99 || g.level > 99) return "region " + std::to_string(i + 1) + ": level outside -99..+99";
                g.ksf = korg::text(b, r + 6, 12);
                if (g.ksf.empty()) return "region " + std::to_string(i + 1) + ": no KSF file name";
                k->regions.push_back(g);
                if (g.hi == 127 && i + 1 < n) return "region " + std::to_string(i + 2) + ": starts above G9";
                lo = static_cast<uint8_t>(g.hi + 1);
            }
            rlp1 = true;
        } else if (id == "RLP2") {
            if (len != 4) return "RLP2 must be 4 bytes";
            for (int i = 0; i < 4; ++i) k->rlp2[i] = static_cast<int8_t>(b[o + i]);
        } else if (id == "RLP3") {
            if (len != 6) return "RLP3 must be 6 bytes";
            for (int i = 0; i < 5; ++i) k->rlp3[i] = static_cast<int8_t>(b[o + i]);
        } else if (id == "MNO1") {
            if (len != 4) return "MNO1 must be 4 bytes";
        } else {
            return "chunk not in the KRONOS documentation";
        }
        return "";
    });
    if (!e.empty()) return e;
    if (!msp || !rlp1) return "not a KMP file (needs MSP1 and RLP1)";
    k->name = name24.empty() ? name16 : name24;
    return "";
}

struct KsfHead {
    uint32_t rate = 0, frames = 0, start = 0, start2 = 0, loopStart = 0, end = 0;
    bool loopOn = false, plus12dB = false, use2ndStart = false;
    int8_t loopTune = 0;
};

namespace korg {
inline std::string smp1(const std::vector<uint8_t>& b, size_t o, size_t len, KsfHead* h) {
    if (len != 32) return "SMP1 must be 32 bytes";
    h->start = be32(b, o + 16);
    h->start2 = be32(b, o + 20);
    h->loopStart = be32(b, o + 24);
    h->end = be32(b, o + 28);
    return "";
}
// The 12 fixed SMD1 bytes at o (chunk length len, the audio follows them).
inline std::string smd1(const std::vector<uint8_t>& b, size_t o, size_t len, KsfHead* h) {
    if (len < 12) return "SMD1 shorter than 12 bytes";
    h->rate = be32(b, o);
    const uint8_t a = b[o + 4];
    if (a & 0x10) return "compressed sample data (not documented; save it uncompressed on the KRONOS)";
    if (a & 0x0E) return "attribute bits 3-1 set on uncompressed data (undocumented)";
    if (a & 0x40) return "Reverse is on (a reversed sample cannot be carried into a Pa5X package; turn Reverse off on the KRONOS)";
    h->loopOn = !(a & 0x80);
    h->use2ndStart = !(a & 0x20);
    h->plus12dB = a & 0x01;
    h->loopTune = static_cast<int8_t>(b[o + 5]);
    if (h->loopTune < -99 || h->loopTune > 99) return "loop tune outside -99..+99";
    if (b[o + 6] != 1) return std::to_string(b[o + 6]) + " channels (the KRONOS documents 1; stereo is a -L/-R pair)";
    if (b[o + 7] != 16) return std::to_string(b[o + 7]) + "-bit data (only 16-bit is converted)";
    h->frames = be32(b, o + 8);
    if (uint64_t(h->frames) * 2 != len - 12) return "sample count does not match the data length";
    return "";
}
inline std::string ranges(const KsfHead* h) {
    if (h->frames == 0) return "no audio";
    if (h->end >= h->frames || h->start > h->end) return "Start/End outside the audio";
    if (h->loopOn && (h->loopStart < h->start || h->loopStart > h->end)) return "Loop Start outside Start..End";
    return "";
}
}  // namespace korg

// Mono 16-bit uncompressed KSF -> Sample trimmed to Start..End (what plays), loop Loop Start..End inclusive.
// head only (s == nullptr): the parameters, without copying audio.
inline std::string readKsf(const std::vector<uint8_t>& b, KsfHead* h, Sample* s) {
    *h = {};
    bool smp = false, smd = false;
    size_t dataAt = 0;
    const auto e = korg::walk(b, [&](const std::string& id, size_t o, size_t len) -> std::string {
        if (id == "SMP1") {
            if (const auto e = korg::smp1(b, o, len, h); !e.empty()) return e;
            smp = true;
        } else if (id == "SMD1") {
            if (const auto e = korg::smd1(b, o, len, h); !e.empty()) return e;
            dataAt = o + 12;
            smd = true;
        } else if (id == "SNO1") {
            if (len != 4) return "SNO1 must be 4 bytes";
        } else if (id == "NAME") {
            if (len != 24) return "NAME must be 24 bytes";
        } else if (id == "SMF1") {
            return "shared sample data (SMF1): the audio is in another file; load and save it again on the KRONOS";
        } else if (id == "SPD1" || id == "SDD1") {
            return "sample divided across several media; copy it to one medium on the KRONOS first";
        } else {
            return "chunk not in the KRONOS documentation";
        }
        return "";
    });
    if (!e.empty()) return e;
    if (!smp || !smd) return "not a KSF file (needs SMP1 and SMD1)";
    if (const auto r = korg::ranges(h); !r.empty()) return r;
    if (!s) return "";
    s->rate = h->rate;
    s->channels = 1;
    s->pcm.resize(h->end - h->start + 1);
    for (size_t i = 0; i < s->pcm.size(); ++i) {
        const size_t p = dataAt + 2 * (h->start + i);
        s->pcm[i] = static_cast<int16_t>(uint16_t(b[p] << 8 | b[p + 1]));
    }
    s->looped = h->loopOn;
    s->loopStart = h->loopOn ? h->loopStart - h->start : 0;
    s->loopEnd = h->loopOn ? h->end - h->start + 1 : 0;
    return "";
}

// One multisample as the musician sees it: a mono KMP, or a -L/-R pair joined into stereo.
struct KorgMultisample {
    std::string name;                         // layer name: printable ASCII, <= 20, no comma
    std::filesystem::path kmp, kmpR;          // kmpR empty for mono
    Kmp left, right;
    std::vector<std::filesystem::path> ksf, ksfR;   // per region; empty path = SKIPPEDSAMPL
    std::vector<KsfHead> head, headR;
};

namespace korg {
inline std::vector<uint8_t> slurp(const std::filesystem::path& p, std::string* e) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(p, ec);
    if (ec || n > (64u << 20)) { *e = p.filename().string() + ": unreadable or larger than 64 MB"; return {}; }
    std::ifstream is(p, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    if (b.size() != n) *e = p.filename().string() + ": read failed";
    return b;
}
inline std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
// A KSF named in a KMP: next to it ("Save All") or in the folder named like it ("One Multisample", p.856). FAT is
// case-insensitive, so the name is matched without case.
// Two files that differ only in case (possible on a case-sensitive disk) are refused rather than picked.
inline std::filesystem::path findKsf(const std::filesystem::path& kmp, const std::string& name, std::string* why) {
    std::error_code ec;
    for (const auto& d : {kmp.parent_path(), kmp.parent_path() / kmp.stem()}) {
        std::vector<std::filesystem::path> hits;
        for (const auto& e : std::filesystem::directory_iterator(d, ec))
            if (lower(e.path().filename().string()) == lower(name) && e.is_regular_file(ec)) hits.push_back(e.path());
        if (hits.size() > 1) { *why = "several files named " + name + " (differing only in case) in " + d.string(); return {}; }
        if (hits.size() == 1) return hits.front();
    }
    *why = name + " not found (looked next to it and in " + kmp.stem().string() + "/)";
    return {};
}
// The scan reads 60 bytes, not the audio: every documented KSF structure starts SMP1 (32) then SMD1 (p.1159), so any
// other start is refused here. The conversion later parses the whole file.
inline std::string ksfHead(const std::filesystem::path& p, KsfHead* h) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) return "unreadable";
    std::vector<uint8_t> b(60);
    std::ifstream is(p, std::ios::binary);
    if (size >= 60 && is.read(reinterpret_cast<char*>(b.data()), 60) && !std::memcmp(b.data(), "SMP1", 4) && be32(b, 4) == 32 &&
        !std::memcmp(&b[40], "SMD1", 4) && be32(b, 44) <= size - 48) {
        *h = {};
        if (const auto e = smp1(b, 8, 32, h); !e.empty()) return e;
        if (const auto e = smd1(b, 48, be32(b, 44), h); !e.empty()) return e;
        return ranges(h);
    }
    return "does not start with SMP1 (32 bytes) then SMD1 inside the file, as every documented KSF does";
}
inline std::string layerName(std::string s) {
    for (auto& c : s) if (c < 0x20 || c > 0x7E || c == ',') c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s.substr(0, 20);
}
// "PIANO -L" -> ("PIANO", 'L'); also "PIANO-L", "PIANO_L".
inline std::pair<std::string, char> side(const std::string& n) {
    if (n.size() > 2 && (n[n.size() - 2] == '-' || n[n.size() - 2] == '_') && (n.back() == 'L' || n.back() == 'R')) {
        std::string base = n.substr(0, n.size() - 2);
        while (!base.empty() && base.back() == ' ') base.pop_back();
        if (!base.empty()) return {base, n.back()};
    }
    return {n, 0};
}
}  // namespace korg

inline bool isKorgFolder(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (korg::lower(e.path().extension().string()) == ".kmp") return true;
    return false;
}

// Reads every .KMP in dir and the first 60 bytes of every KSF they name; pairs -L/-R. No audio is read (fast enough for
// the UI thread). "" on success.
inline std::string scanKorgFolder(const std::filesystem::path& dir, std::vector<KorgMultisample>& out) {
    namespace fs = std::filesystem;
    out.clear();
    std::error_code ec;
    std::vector<fs::path> kmps;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (korg::lower(e.path().extension().string()) == ".kmp" && e.is_regular_file(ec)) kmps.push_back(e.path());
    if (ec) return "cannot read " + dir.string();
    if (kmps.empty()) return "no .KMP multisample files in " + dir.string();
    std::sort(kmps.begin(), kmps.end());
    struct One { fs::path p; Kmp k; std::vector<fs::path> ksf; std::vector<KsfHead> head; };
    std::map<std::string, std::map<char, One>> byBase;
    std::map<fs::path, KsfHead> heads;   // a KSF used by several regions is read once
    for (const auto& p : kmps) {
        std::string err;
        const auto b = korg::slurp(p, &err);
        if (!err.empty()) return err;
        One one{p, {}, {}, {}};
        if (const auto e = readKmp(b, &one.k); !e.empty()) return p.filename().string() + ": " + e;
        for (const auto& r : one.k.regions) {
            if (r.ksf == "SKIPPEDSAMPL") { one.ksf.emplace_back(); one.head.emplace_back(); continue; }
            if (r.ksf.rfind("INTERNAL", 0) == 0) return p.filename().string() + ": " + r.ksf + " refers to a sample built into an older Korg instrument";
            std::string why;
            const fs::path f = korg::findKsf(p, r.ksf, &why);
            if (f.empty()) return p.filename().string() + ": " + why;
            if (!heads.count(f)) {
                KsfHead h;
                if (const auto e = korg::ksfHead(f, &h); !e.empty()) return f.filename().string() + ": " + e;
                heads[f] = h;
            }
            const KsfHead h = heads[f];
            one.ksf.push_back(f);
            one.head.push_back(h);
        }
        const auto [base, ch] = korg::side(one.k.name);
        auto& slot = byBase[base];
        if (slot.count(ch)) return p.filename().string() + ": a second multisample named '" + one.k.name + "'";
        slot[ch] = std::move(one);
    }
    std::map<std::string, int> names;
    for (auto& [base, sides] : byBase) {
        const auto add = [&](One& a, One* r, const std::string& nm) -> std::string {
            KorgMultisample m;
            m.name = korg::layerName(nm);
            if (m.name.empty()) return a.p.filename().string() + ": the multisample has no printable name";
            if (names[m.name]++) return a.p.filename().string() + ": two multisamples are both called '" + m.name + "' (first 20 characters)";
            m.kmp = a.p; m.left = std::move(a.k); m.ksf = std::move(a.ksf); m.head = std::move(a.head);
            if (r) {
                m.kmpR = r->p; m.right = std::move(r->k); m.ksfR = std::move(r->ksf); m.headR = std::move(r->head);
                const auto& L = m.left.regions; const auto& R = m.right.regions;
                bool same = L.size() == R.size();
                for (size_t i = 0; same && i < L.size(); ++i)
                    same = L[i].lo == R[i].lo && L[i].hi == R[i].hi && L[i].root == R[i].root && L[i].tune == R[i].tune &&
                           L[i].fixedPitch == R[i].fixedPitch && L[i].level == R[i].level &&
                           m.ksf[i].empty() == m.ksfR[i].empty() &&
                           (m.ksf[i].empty() || (m.head[i].rate == m.headR[i].rate && m.head[i].end - m.head[i].start == m.headR[i].end - m.headR[i].start &&
                                                 m.head[i].loopOn == m.headR[i].loopOn && m.head[i].plus12dB == m.headR[i].plus12dB &&
                                                 m.head[i].loopTune == m.headR[i].loopTune && m.head[i].use2ndStart == m.headR[i].use2ndStart &&
                                                 (!m.head[i].use2ndStart || m.head[i].start2 - m.head[i].start == m.headR[i].start2 - m.headR[i].start) &&
                                                 (!m.head[i].loopOn || m.head[i].loopStart - m.head[i].start == m.headR[i].loopStart - m.headR[i].start)));
                same = same && std::equal(std::begin(m.left.rlp2), std::end(m.left.rlp2), std::begin(m.right.rlp2)) &&
                       std::equal(std::begin(m.left.rlp3), std::end(m.left.rlp3), std::begin(m.right.rlp3));
                if (!same) return m.kmp.filename().string() + " and " + m.kmpR.filename().string() +
                                  ": the -L and -R halves differ (keys, roots, tune, level, length, rate, loop, +12 dB or relative parameters)";
            }
            out.push_back(std::move(m));
            return "";
        };
        if (sides.count('L') && sides.count('R') && sides.size() == 2) {
            if (const auto e = add(sides['L'], &sides['R'], base); !e.empty()) return e;
        } else {
            for (auto& [ch, one] : sides) if (const auto e = add(one, nullptr, one.k.name); !e.empty()) return e;
        }
    }
    return "";
}

// layers: multisample names (KorgMultisample::name) in velocity order, softest first (empty = the folder must hold
// exactly one); splits: lowest velocity of layers 2..N.
inline std::string buildFromKorgFolder(const std::filesystem::path& dir, const std::string& name, std::vector<std::string> layers,
                                       const std::vector<uint8_t>& splits, Instrument* out) {
    std::vector<KorgMultisample> ms;
    if (const auto e = scanKorgFolder(dir, ms); !e.empty()) return e;
    if (layers.empty()) {
        if (ms.size() != 1) return "the folder holds " + std::to_string(ms.size()) + " multisamples; name the layers in velocity order";
        layers.push_back(ms.front().name);
    }
    if (splits.size() + 1 != layers.size()) return "need one split per layer after the first";
    Instrument in;
    in.name = name;
    for (size_t l = 0; l < layers.size(); ++l) {
        const auto it = std::find_if(ms.begin(), ms.end(), [&](const KorgMultisample& m) { return m.name == layers[l]; });
        if (it == ms.end()) return "no multisample called '" + layers[l] + "'";
        Layer L;
        L.name = it->name;
        L.velLo = l == 0 ? 0 : splits[l - 1];
        L.velHi = l + 1 == layers.size() ? 127 : static_cast<uint8_t>(splits[l] - 1);
        std::map<std::string, int> loaded;   // a KSF used by several regions becomes one sample
        for (size_t i = 0; i < it->left.regions.size(); ++i) {
            const KmpRegion& r = it->left.regions[i];
            if (it->ksf[i].empty()) continue;   // SKIPPEDSAMPL: those keys stay silent (reported by the checks)
            if (r.fixedPitch && (r.lo != r.hi || r.root != r.lo))   // a one-key zone rooted at its key plays untransposed either way
                return it->kmp.filename().string() + " region " + std::to_string(i + 1) +
                       ": Non Transpose (fixed pitch) where the key differs from the original key (a Pa5X zone always transposes)";
            const std::string key = it->ksf[i].string() + "|" + (it->ksfR.empty() ? "" : it->ksfR[i].string());
            if (!loaded.count(key)) {
                std::string err;
                Sample s;
                KsfHead h;
                const auto b = korg::slurp(it->ksf[i], &err);
                if (!err.empty()) return err;
                if (const auto e = readKsf(b, &h, &s); !e.empty()) return it->ksf[i].filename().string() + ": " + e;
                if (!it->ksfR.empty()) {
                    Sample r2;
                    KsfHead hr;
                    const auto br = korg::slurp(it->ksfR[i], &err);
                    if (!err.empty()) return err;
                    if (const auto e = readKsf(br, &hr, &r2); !e.empty()) return it->ksfR[i].filename().string() + ": " + e;
                    // the full parse decides, not only the scan's header: the halves must play identically
                    const bool same = r2.pcm.size() == s.pcm.size() && hr.rate == h.rate && r2.looped == s.looped && r2.loopStart == s.loopStart &&
                                      r2.loopEnd == s.loopEnd && hr.plus12dB == h.plus12dB && hr.loopTune == h.loopTune &&
                                      hr.use2ndStart == h.use2ndStart && (!h.use2ndStart || hr.start2 - hr.start == h.start2 - h.start);
                    if (!same) return it->ksfR[i].filename().string() + ": the -R sample differs from -L (length, rate, loop, +12 dB, loop tune or 2nd Start)";
                    std::vector<int16_t> st(s.pcm.size() * 2);
                    for (size_t f = 0; f < s.pcm.size(); ++f) { st[2 * f] = s.pcm[f]; st[2 * f + 1] = r2.pcm[f]; }
                    s.pcm = std::move(st);
                    s.channels = 2;
                }
                s.root = r.root;
                s.name = L.name.substr(0, 16) + " " + std::to_string(in.samples.size() + 1);
                loaded[key] = static_cast<int>(in.samples.size());
                in.samples.push_back(std::move(s));
            }
            L.zones.push_back({r.lo, r.hi, loaded[key], r.root, r.tune, 0});
        }
        if (L.zones.empty()) return "multisample '" + L.name + "' has no samples (all regions skipped)";
        in.layers.push_back(std::move(L));
    }
    if (const auto e = validate(in); !e.empty()) return e.front();
    *out = std::move(in);
    return "";
}

// REPORT.txt source section: what was preserved, approximated and lost, per layer.
inline std::string korgFolderNotes(const std::filesystem::path& dir, const Instrument& in) {
    std::vector<KorgMultisample> ms;
    std::string o = "Source: KRONOS Korg-format multisamples (.KMP/.KSF) in " + dir.string() +
                    "\nFormat: KRONOS Parameter Guide En 3 p.1158-1159 (Korg format sample files)\n";
    if (const auto e = scanKorgFolder(dir, ms); !e.empty()) return o + "(source could not be re-read: " + e + ")\n\n" + formatRecipe(recipeFromSamples(in));
    o += "Stereo pairs are converted only when the -L and -R halves carry identical settings, so the left half describes both.\n";
    o += "Preserved: key zones (low key = previous top key + 1), original keys, zone tune (cents), sample rate, 16-bit audio "
         "exactly, Start..End, loops (Loop Start..End), stereo from -L/-R pairs.\n";
    for (const auto& L : in.layers) {
        const auto it = std::find_if(ms.begin(), ms.end(), [&](const KorgMultisample& m) { return m.name == L.name; });
        if (it == ms.end()) continue;
        o += "Layer '" + L.name + "' (velocity " + std::to_string(L.velLo) + "-" + std::to_string(L.velHi) + "): " + it->kmp.filename().string() +
             (it->kmpR.empty() ? "" : " + " + it->kmpR.filename().string()) + ", " + std::to_string(it->left.regions.size()) + " regions\n";
        size_t skipped = 0, level = 0, loopTune = 0, p12 = 0, trimmed = 0, start2 = 0;
        for (size_t i = 0; i < it->left.regions.size(); ++i) {
            if (it->ksf[i].empty()) { ++skipped; continue; }
            level += it->left.regions[i].level != 0;
            const KsfHead& h = it->head[i];
            loopTune += h.loopOn && h.loopTune != 0;
            p12 += h.plus12dB;
            trimmed += h.start > 0 || h.end + 1 < h.frames;
            start2 += h.use2ndStart && h.start2 != h.start;
        }
        const auto line = [&](size_t n, const std::string& what) { if (n) o += "  " + std::to_string(n) + " " + what + "\n"; };
        line(trimmed, "sample(s): audio outside Start..End is not copied (it never plays on the KRONOS either)");
        line(skipped, "region(s) were skipped on the KRONOS (SKIPPEDSAMPL): those keys are silent");
        line(level, "region(s) have a Level offset: LOST (unit not documented for the Pa5X; adjust the zone level on the Pa5X)");
        line(loopTune, "sample(s) have Loop Tune: LOST (no Pa5X equivalent in the package)");
        if (p12 && p12 < it->head.size() - skipped) line(p12, "sample(s) have +12 dB and others not: the relative level is LOST");
        else line(p12, "sample(s) have +12 dB (all of them): not carried; set the Sound level on the Pa5X");
        line(start2, "sample(s) have a 2nd Start in use: not carried (the Pa5X plays from Start; a KRONOS Program may start at the 2nd)");
        bool rel = false;
        for (int v : it->left.rlp2) rel |= v != 0;
        for (int v : it->left.rlp3) rel |= v != 0;
        if (rel) o += "  relative transpose/resonance/attack/decay/drive/boost/EQ (RLP2/RLP3) are set: LOST (Pa5X Sound parameters)\n";
    }
    o += "Not in these files (KRONOS Program, .PCG, not documented): velocity zones, filters, envelopes, LFOs, modulation, "
         "effects. The layer order and velocity splits above were chosen in AIArranger.\n"
         "Validated against files built from Korg's documented layout only; the first real KRONOS files are the acceptance test.\n\n";
    return o + formatRecipe(recipeFromSamples(in));
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_KORG_KMP_SOURCE_H
