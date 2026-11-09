#ifndef AI_ARRANGER_MEDIA_SOUND_PACKAGE_GRAPH_H
#define AI_ARRANGER_MEDIA_SOUND_PACKAGE_GRAPH_H

// Resource graph of AIArranger's own packages (docs/research/PA5X_SOUND_RESOURCE_GRAPH.md §8 A1).
// Every package-generated resource gets a PROVISIONAL identity prov:<package id>/<Kind>/<n>; the
// Pa5X assigns real slots on import. Package manifests (RESOURCES.tsv) can never claim a Pa5X
// identity, so package-generated and Pa5X/KST resources stay apart. Nothing here reads a KST.
//
// RESOURCES.tsv, tab-separated, one record per line:
//   aiarranger-resources  1
//   package  <id>  <Sound|DrumKit>
//   node     <id>  <name>                                                  (non-Sample resources)
//   sample   <id>  <name> <ch> <rate> <bits> <frames> <loopFrames> <fnv1a64> <package-relative WAV>
//   ref      <from> <via> <to> <detail>
//   end      <number of records above>

#include "media/sound/drumkit.h"
#include "media/sound/resource_graph.h"
#include "media/sound/txprog_writer.h"
#include "media/sound/wav_folder_source.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ai_arranger::media::sound {

inline uint64_t fnv1a(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
inline std::string hex16(uint64_t h) { char b[17]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h)); return b; }

// Fingerprint of the audio (format + little-endian PCM). A fingerprint, never an identity.
inline std::string contentHash(const Sample& s) {
    uint8_t fmt[5] = {s.channels, uint8_t(s.rate), uint8_t(s.rate >> 8), uint8_t(s.rate >> 16), uint8_t(s.rate >> 24)};
    uint64_t h = fnv1a(fmt, sizeof fmt);
    for (int16_t v : s.pcm) { const uint8_t le[2] = {uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF)}; h = fnv1a(le, 2, h); }
    return hex16(h);
}

// Package id: file-safe name + fingerprint of the written TX files (deterministic; a byte-identical
// package written twice gets the same id, and the loader refuses to count it twice).
inline std::string packageId(const std::string& name, const std::vector<PackageFile>& tx) {
    std::string base = txw::fileSafe(name);
    for (char& c : base) if (c == ' ') c = '_';
    uint64_t h = 1469598103934665603ull;
    for (const auto& f : tx) { h = fnv1a(f.path.data(), f.path.size(), h); h = fnv1a(f.bytes.data(), f.bytes.size(), h); }
    return base.substr(0, 40) + "#" + hex16(h).substr(0, 12);
}

namespace pg {
using graph::Edge; using graph::Graph; using graph::Kind; using graph::Node; using graph::ResourceId; using graph::SampleInfo; using graph::Via;
inline ResourceId pid(const std::string& pkg, Kind k, uint32_t n) { return {true, pkg, k, n}; }
inline SampleInfo info(const Sample& s, const std::string& file) {
    return {s.channels, s.rate, 16, s.frames(), s.looped ? uint64_t(s.loopEnd - s.loopStart) : 0, contentHash(s), file};
}
inline std::string wavPath(const std::string& owner, const Sample& s) {
    const std::string base = txw::fileSafe(owner);
    return "tx/" + base + "/" + base + " Samples/" + txw::fileSafe(s.name) + ".wav";
}
}  // namespace pg

// Sound -> oscillator n (one per velocity layer, GUIDE p.14) -> High multisample -> zones -> samples.
inline bool addInstrument(graph::Graph& g, const Instrument& in, const std::string& pkg, std::string* err) {
    using namespace pg;
    if (!validate(in).empty()) { if (err) *err = "invalid instrument"; return false; }
    const ResourceId snd = pid(pkg, Kind::Sound, 0);
    bool ok = g.addNode({snd, in.name, {}}, err);
    for (size_t i = 0; ok && i < in.samples.size(); ++i)
        ok = g.addNode({pid(pkg, Kind::Sample, uint32_t(i)), in.samples[i].name, info(in.samples[i], wavPath(in.name, in.samples[i]))}, err);
    for (size_t l = 0; ok && l < in.layers.size(); ++l) {
        const Layer& L = in.layers[l];
        const ResourceId osc = pid(pkg, Kind::Oscillator, uint32_t(l + 1)), ms = pid(pkg, Kind::Multisample, uint32_t(l));
        ok = g.addNode({osc, "OSC " + std::to_string(l + 1), {}}, err) && g.addNode({ms, L.name, {}}, err) &&
             g.addRef({snd, osc, Via::OscillatorOf, "oscillator " + std::to_string(l + 1) + " velocity " + std::to_string(L.velLo) + "-" + std::to_string(L.velHi)}, err) &&
             g.addRef({osc, ms, Via::OscHigh, "High"}, err);
        for (size_t z = 0; ok && z < L.zones.size(); ++z) {
            const Zone& Z = L.zones[z];
            ok = g.addRef({ms, pid(pkg, Kind::Sample, uint32_t(Z.sample)), Via::Zone,
                           "zone " + std::to_string(z) + " keys " + std::to_string(Z.lo) + "-" + std::to_string(Z.hi) + " root " + std::to_string(Z.root)}, err);
        }
    }
    if (ok) g.declarePackageComplete(pkg);
    return ok;
}

// Drum Kit -> key layer -> Drum Sample -> Sample. One Drum Sample per kit sample: the TX16Wx route
// gives one wave per matrix cell; the Pa5X-side Drum Sample cardinality is UNK (spec §2).
inline bool addDrumKit(graph::Graph& g, const DrumKit& kit, const std::string& pkg, std::string* err) {
    using namespace pg;
    if (!validate(kit).empty()) { if (err) *err = "invalid drum kit"; return false; }
    const ResourceId dk = pid(pkg, Kind::DrumKit, 0);
    bool ok = g.addNode({dk, kit.name, {}}, err);
    for (size_t i = 0; ok && i < kit.samples.size(); ++i)
        ok = g.addNode({pid(pkg, Kind::Sample, uint32_t(i)), kit.samples[i].name, info(kit.samples[i], wavPath(kit.name, kit.samples[i]))}, err) &&
             g.addNode({pid(pkg, Kind::DrumSample, uint32_t(i)), kit.samples[i].name, {}}, err) &&
             g.addRef({pid(pkg, Kind::DrumSample, uint32_t(i)), pid(pkg, Kind::Sample, uint32_t(i)), Via::DrumSampleOf, "wave"}, err);
    for (const auto& I : kit.instruments)
        for (const auto& k : I.keys)
            for (size_t l = 0; ok && l < k.layers.size(); ++l)
                ok = g.addRef({dk, pid(pkg, Kind::DrumSample, uint32_t(k.layers[l].sample)), Via::DrumKeyLayer,
                               "key " + std::to_string(k.key) + " layer " + std::to_string(l) + " vel " + std::to_string(k.layers[l].velLo)}, err);
    if (ok) g.declarePackageComplete(pkg);
    return ok;
}

inline std::string writeManifest(const graph::Graph& g, const std::string& pkg, graph::Kind top) {
    std::vector<std::string> rec;
    rec.push_back(std::string("aiarranger-resources\t1"));
    rec.push_back("package\t" + pkg + "\t" + graph::kindName(top));
    for (const auto& [id, n] : g.nodes()) {
        if (!n.sample) { rec.push_back("node\t" + id.str() + "\t" + n.name); continue; }
        const auto& s = *n.sample;
        rec.push_back("sample\t" + id.str() + "\t" + n.name + "\t" + std::to_string(s.channels) + "\t" + std::to_string(s.rate) + "\t" +
                      std::to_string(s.bits) + "\t" + std::to_string(s.frames) + "\t" + std::to_string(s.loopFrames) + "\t" + s.contentHash + "\t" + s.file);
    }
    for (const auto& e : g.edges()) rec.push_back("ref\t" + e.from.str() + "\t" + graph::viaName(e.via) + "\t" + e.to.str() + "\t" + e.detail);
    std::string o;
    for (const auto& r : rec) o += r + "\n";
    return o + "end\t" + std::to_string(rec.size()) + "\n";
}

namespace pg {
inline std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> f;
    size_t a = 0;
    for (size_t b; (b = s.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(s.substr(a, b - a));
    f.push_back(s.substr(a));
    return f;
}
inline bool num(const std::string& s, uint64_t max, uint64_t& v) {
    if (s.empty() || s.size() > 19 || s.find_first_not_of("0123456789") != std::string::npos || (s.size() > 1 && s[0] == '0')) return false;
    v = std::stoull(s);
    return v <= max;
}
inline bool text(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (unsigned char c : s) if (c < 0x20 || c >= 0x7F) return false;
    return true;
}
inline bool safeRel(const std::string& p) {
    if (!text(p) || p[0] == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) return false;
    std::stringstream ss(p);
    for (std::string part; std::getline(ss, part, '/');) if (part.empty() || part == "." || part == "..") return false;
    return true;
}
}  // namespace pg

// Strict parser: any malformed or inconsistent record refuses the whole manifest.
inline bool parseManifest(const std::string& textIn, graph::Graph& out, std::string& pkg, std::string* err) {
    using namespace pg;
    auto fail = [&](size_t line, const std::string& m) { if (err) *err = "RESOURCES.tsv line " + std::to_string(line) + ": " + m; return false; };
    if (textIn.empty() || textIn.back() != '\n') return fail(0, "empty or truncated (no final newline)");
    std::vector<std::string> lines;
    std::stringstream ss(textIn);
    for (std::string l; std::getline(ss, l);) lines.push_back(l);
    if (lines.size() < 3 || lines[0] != "aiarranger-resources\t1") return fail(1, "not an aiarranger-resources version 1 manifest");
    const auto pf = split(lines[1]);
    Kind top{};
    if (pf.size() != 3 || pf[0] != "package" || !ResourceId::validScope(pf[1]) || !graph::parseKind(pf[2], top) ||
        (top != Kind::Sound && top != Kind::DrumKit)) return fail(2, "bad package record");
    pkg = pf[1];
    const auto ef = split(lines.back());
    uint64_t count = 0;
    if (ef.size() != 2 || ef[0] != "end" || !num(ef[1], 1u << 24, count) || count != lines.size() - 1) return fail(lines.size(), "missing or wrong end record (truncated?)");
    Graph g;
    std::vector<Edge> refs;
    for (size_t i = 2; i + 1 < lines.size(); ++i) {
        const auto f = split(lines[i]);
        ResourceId id;
        auto ownId = [&](const std::string& s, ResourceId& r) { return ResourceId::parse(s, r) && r.provisional && r.scope == pkg; };
        std::string e;
        if (f[0] == "node" && f.size() == 3) {
            if (!ownId(f[1], id)) return fail(i + 1, "identity must be provisional and inside package " + pkg + ": " + f[1]);
            if (id.kind == Kind::Sample || !text(f[2])) return fail(i + 1, "bad node record");
            if (!g.addNode({id, f[2], {}}, &e)) return fail(i + 1, e);
        } else if (f[0] == "sample" && f.size() == 10) {
            uint64_t ch, rate, bits, frames, loop;
            if (!ownId(f[1], id) || id.kind != Kind::Sample) return fail(i + 1, "sample identity must be a provisional Sample of package " + pkg);
            if (!text(f[2]) || !num(f[3], 2, ch) || !num(f[4], 384000, rate) || !num(f[5], 16, bits) || !num(f[6], 1u << 30, frames) ||
                !num(f[7], 1u << 30, loop) || f[8].size() != 16 || f[8].find_first_not_of("0123456789abcdef") != std::string::npos || !safeRel(f[9]))
                return fail(i + 1, "bad sample record");
            if (!g.addNode({id, f[2], SampleInfo{uint8_t(ch), uint32_t(rate), uint8_t(bits), frames, loop, f[8], f[9]}}, &e)) return fail(i + 1, e);
        } else if (f[0] == "ref" && f.size() == 5) {
            Edge r;
            if (!ownId(f[1], r.from) || !ownId(f[3], r.to) || !graph::parseVia(f[2], r.via) || !text(f[4])) return fail(i + 1, "bad ref record");
            r.detail = f[4];
            refs.push_back(r);
        } else {
            return fail(i + 1, "unknown or malformed record");
        }
    }
    for (const Edge& r : refs) {
        std::string e;
        if (!g.addRef(r, &e)) return fail(0, e);
        if (!g.find(r.to)) return fail(0, "package is not self-contained: " + r.to.str() + " is referenced but not described");
    }
    if (!g.find({true, pkg, top, 0})) return fail(0, std::string("no ") + graph::kindName(top) + " resource");
    g.declarePackageComplete(pkg);   // the end record proves every record of the package was read
    out = std::move(g);
    return true;
}

// Loads one package folder: parses RESOURCES.tsv and checks every sample against its WAV on disk
// (channels, rate, frames, loop length, audio fingerprint). Read-only.
inline bool loadPackage(const std::filesystem::path& dir, graph::Graph& g, std::string& pkg, std::string* err) {
    namespace fs = std::filesystem;
    // Regular files of bounded size only: a FIFO or a huge file is an input error, not a hang.
    // (Package WAVs are at most 1 048 576 stereo 16-bit frames = 4 MiB, MAN p.1155.)
    auto read = [](const fs::path& p, std::vector<uint8_t>& b) {
        std::error_code ec;
        const auto st = fs::symlink_status(p, ec);
        if (ec || !fs::is_regular_file(st)) return false;
        const auto size = fs::file_size(p, ec);
        if (ec || size > (16u << 20)) return false;
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        b.assign(std::istreambuf_iterator<char>(f), {});
        return !f.bad();
    };
    // Every path below the package folder must stay inside it: no symlinked component (lexical checks
    // alone would let a symlinked folder lead outside the package).
    auto contained = [&](const std::string& rel) {
        fs::path p = dir;
        std::error_code ec;
        for (const auto& part : fs::path(rel)) {
            p /= part;
            const auto st = fs::symlink_status(p, ec);
            if (fs::is_symlink(st)) return false;
            if (!fs::exists(st)) break;   // missing: the read below reports it
        }
        return true;
    };
    std::vector<uint8_t> m;
    if (!contained("RESOURCES.tsv")) { if (err) *err = dir.string() + ": RESOURCES.tsv is a symlink"; return false; }
    if (!read(dir / "RESOURCES.tsv", m)) { if (err) *err = "cannot read " + (dir / "RESOURCES.tsv").string(); return false; }
    graph::Graph tmp;
    std::string e;
    if (!parseManifest(std::string(m.begin(), m.end()), tmp, pkg, &e)) { if (err) *err = dir.string() + ": " + e; return false; }
    for (const auto& [id, n] : tmp.nodes()) {
        if (!n.sample) continue;
        std::vector<uint8_t> b;
        Sample s;
        if (!contained(n.sample->file)) { if (err) *err = dir.string() + ": " + n.sample->file + " leaves the package through a symlink"; return false; }
        if (!read(dir / n.sample->file, b)) { if (err) *err = dir.string() + ": cannot read " + n.sample->file; return false; }
        if (const auto we = readWav(b, &s); !we.empty()) { if (err) *err = dir.string() + ": " + n.sample->file + ": " + we; return false; }
        const auto& x = *n.sample;
        if (x.bits != 16 /* readWav decodes 16-bit PCM only */ || s.channels != x.channels || s.rate != x.rate || s.frames() != x.frames ||
            (s.looped ? uint64_t(s.loopEnd - s.loopStart) : 0) != x.loopFrames || contentHash(s) != x.contentHash) {
            if (err) *err = dir.string() + ": " + n.sample->file + " does not match RESOURCES.tsv (" + id.str() + ")";
            return false;
        }
    }
    // Cross-check references against the package's own TX files (documented TX16Wx output written by
    // AIArranger): every WAV a .txprog/.txmatrix names must be a manifest sample with at least one
    // reference, and every manifest sample must be named by the TX files. A manifest that drops
    // references therefore cannot make a used sample look unreferenced.
    std::set<std::string> txWavs;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir / "tx", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string ext = it->path().extension().string();
        if (ext != ".txprog" && ext != ".txmatrix") continue;
        std::vector<uint8_t> b;
        if (!read(it->path(), b)) { if (err) *err = dir.string() + ": cannot read " + it->path().string(); return false; }
        const std::string x(b.begin(), b.end());
        const std::string rel = it->path().parent_path().lexically_relative(dir).generic_string();
        for (size_t at = 0; (at = x.find("tx:path=\"", at)) != std::string::npos;) {
            at += 9;
            const size_t q = x.find('"', at);
            if (q == std::string::npos) break;
            std::string v = x.substr(at, q - at);
            for (const auto& [ent, ch] : std::vector<std::pair<std::string, std::string>>{{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}})
                for (size_t i; (i = v.find(ent)) != std::string::npos;) v.replace(i, ent.size(), ch);
            if (v.size() > 4 && v.compare(v.size() - 4, 4, ".wav") == 0) txWavs.insert(rel + "/" + v);
        }
    }
    if (ec) { if (err) *err = dir.string() + ": cannot list tx/: " + ec.message(); return false; }
    std::set<std::string> manifestWavs;
    for (const auto& [id, n] : tmp.nodes()) {
        if (!n.sample) continue;
        manifestWavs.insert(n.sample->file);
        if (!txWavs.count(n.sample->file)) { if (err) *err = dir.string() + ": " + id.str() + " (" + n.sample->file + ") is not used by the package's TX files"; return false; }
        if (tmp.whereUsed(id).empty()) { if (err) *err = dir.string() + ": " + n.sample->file + " is used by the TX files but has no reference in RESOURCES.tsv"; return false; }
    }
    for (const auto& w : txWavs)
        if (!manifestWavs.count(w)) { if (err) *err = dir.string() + ": TX files use " + w + ", which RESOURCES.tsv does not describe"; return false; }
    g = std::move(tmp);
    return true;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_PACKAGE_GRAPH_H
