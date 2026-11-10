#ifndef AI_ARRANGER_MEDIA_SOUND_PACKAGE_WRITER_H
#define AI_ARRANGER_MEDIA_SOUND_PACKAGE_WRITER_H

// USB-ready folder for one instrument (docs/SOUND_CONVERSION_ROADMAP.md S4/S8):
//   <stem>.sf2        Pa5X Import > SoundFont (SF2): multisamples (manual p.1209)
//   tx/<name>/...     Pa5X Import > TX Sound (.txprog): a complete Sound (p.1210)
//   wav/<layer>_<OrgKey>_<LowKey>_<HighKey>.wav   looped WAVs, the Pa5X's own export naming (p.1213)
//   REPORT.txt        zones, loops, memory use, import steps, caller notes
// Never writes into an existing folder. Returns the report, or "" with *error set.

#include "media/kst_inventory.h"
#include "media/output_guard.h"
#include "media/sha256.h"
#include "media/sound/package_graph.h"
#include "media/sound/sf2_writer.h"
#include "media/sound/txprog_writer.h"
#include "media/sound/txperf_writer.h"
#include "media/sound/wav_writer.h"
#include <filesystem>
#include <fstream>

namespace ai_arranger::media::sound {

// Pa5X sample-memory facts every package REPORT states (consolidated research R1-R3, R7).
inline constexpr const char* kMemoryNote =
    "Memory: the Pa5X holds 4 GB of uncompressed User Samples, or up to 8 GB of samples if compressed\n"
    "(MAN pp.1174, 1176). Compression is offered at import and cannot be undone on the Pa5X (MAN p.1181):\n"
    "keep this package as the uncompressed source. Import single Sounds/Drum Kits; loading a whole KST that\n"
    "contains a PCM folder deletes all User Samples in memory (MAN p.736).\n\n";

// The <layer> part of the package's WAV names: <layer>_<OrgKey>_<LowKey>_<HighKey>.wav.
inline std::string wavPrefix(const std::string& layerName) {
    std::string s = txw::fileSafe(layerName);
    for (char& c : s) if (c == ' ') c = '_';
    return s;
}

using media::outputFolderRefusal;   // media/output_guard.h: shared by every writer and the KST backup

namespace pkgio {
// The files of one package being written. Each is closed and checked as it is written, and its length and SHA-256
// are kept, so the finished folder can be checked against them before it is published.
struct FileSet {
    std::filesystem::path root;
    std::map<std::string, std::pair<uint64_t, std::string>> want;   // relative path -> (bytes, SHA-256)
    bool ok = true;
    void save(const std::string& rel, const void* data, size_t n) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path p = root / rel;
        ok &= fs::create_directories(p.parent_path(), ec) || fs::is_directory(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
        f.close();   // the last buffered bytes are written here; a failure must stop publication
        ok &= static_cast<bool>(f);
        Sha256 h;
        h.update(data, n);
        want[rel] = {n, h.hex()};
    }
    void save(const std::string& rel, const std::vector<uint8_t>& b) { save(rel, b.data(), b.size()); }
    void save(const std::string& rel, const std::string& s) { save(rel, s.data(), s.size()); }
    // "" when the folder holds exactly these files, each with its length and SHA-256, and the package loads back
    // with the reader every other tool uses (loadPackage: WAVs against RESOURCES.tsv, TX references). Read-only.
    std::string check() const {
        detail::Listing L;
        std::string err;
        if (!detail::listFiles(root, L, err)) return "cannot list it: " + err;
        if (!L.skipped.empty()) return "unexpected link or special file " + L.skipped.front();
        for (const auto& [rel, e] : L.files) if (!want.count(rel)) return "unexpected file " + rel;
        for (const auto& [rel, w] : want) {
            const auto it = L.files.find(rel);
            if (it == L.files.end()) return rel + " is missing";
            if (it->second.size != w.first) return rel + " has " + std::to_string(it->second.size) + " bytes, " + std::to_string(w.first) + " written";
            std::ifstream f(it->second.abs, std::ios::binary);
            Sha256 h;
            char buf[65536];
            while (f.read(buf, sizeof buf) || f.gcount() > 0) { h.update(buf, static_cast<size_t>(f.gcount())); if (!f) break; }
            if (!f.eof() || h.hex() != w.second) return rel + " does not read back as written";
        }
        graph::Graph g;
        std::string pkg;
        if (!loadPackage(root, g, pkg, &err)) return "it does not load back: " + err;
        return "";
    }
};
}  // namespace pkgio

// Written under DEST.INCOMPLETE and renamed to DEST only once every file is written (media/output_guard.h).
inline std::string writePackage(const Instrument& in, const std::filesystem::path& destGiven, const std::string& notes,
                                std::string* error) {
    namespace fs = std::filesystem;
    auto fail = [&](std::string m) { if (error) *error = std::move(m); return std::string(); };
    std::error_code ec;
    const fs::path dest = folderPath(destGiven), out = incompletePath(dest);
    if (fs::exists(dest, ec) || ec) return fail("refusing: " + dest.string() + " already exists");
    if (fs::exists(out, ec) || ec) return fail("refusing: " + out.string() + " already exists (an earlier write that did not finish)");
    if (const auto e = validate(in); !e.empty()) return fail("invalid: " + e.front());
    const auto sf2 = writeSf2(in);
    const auto tx = writeTxProg(in);
    if (sf2.empty() || tx.empty()) return fail("a writer refused the instrument (see txprog_writer.h preconditions)");
    const std::string pkg = packageId(in.name, tx);
    graph::Graph rg;
    std::string ge;
    if (!addInstrument(rg, in, pkg, &ge)) return fail("resource graph: " + ge);
    const std::string manifest = writeManifest(rg, pkg, graph::Kind::Sound);
    fs::create_directories(dest.parent_path(), ec);   // as before: missing parents are made
    if (!fs::create_directory(out, ec) || !fs::create_directory(out / "wav", ec)) return fail("cannot create " + out.string());

    pkgio::FileSet files{out};
    auto under = [](std::string s) { for (char& c : s) if (c == ' ') c = '_'; return s; };
    uint64_t h = 1469598103934665603ull;   // FNV-1a fingerprint for the report, not a security hash
    for (uint8_t c : sf2) { h ^= c; h *= 1099511628211ull; }

    const std::string stem = under(txw::fileSafe(in.name));
    files.save(stem + ".sf2", sf2);
    for (const auto& f : tx) files.save("tx/" + f.path, f.bytes);

    std::string rep = "AIArranger Sound package (SOFTWARE evidence only until imported on a Pa5X)\n\n";
    char line[1024];   // the header line with a 20-char name and paths is ~330 chars
    std::snprintf(line, sizeof line,
                  "Instrument: %s\nSF2: %s.sf2  %zu bytes  fnv1a %016llx\nTXPROG: tx/%s + %zu WAVs (copy the whole folder, structure unchanged)\n"
                  "Velocity layers: %zu (= multisamples, one oscillator each)\nSamples: %zu (%zu stereo), 16-bit, %.2f MB (%.4f%% of the 4 GB User Sample memory)\n\n",
                  in.name.c_str(), stem.c_str(), sf2.size(), static_cast<unsigned long long>(h), tx[0].path.c_str(), tx.size() - 1,
                  in.layers.size(), in.samples.size(),
                  static_cast<size_t>(std::count_if(in.samples.begin(), in.samples.end(), [](const Sample& x) { return x.channels == 2; })),
                  sampleBytes(in) / 1e6, 100.0 * sampleBytes(in) / 4e9);
    rep += line;
    for (size_t l = 0; l < in.layers.size(); ++l) {
        const Layer& L = in.layers[l];
        rep += "Layer " + std::to_string(l) + "  velocity " + std::to_string(L.velLo) + "-" + std::to_string(L.velHi) + "  " + L.name + "\n";
        rep += "  keys     root  sample                frames    rate  ch  loop\n";
        for (const Zone& z : L.zones) {
            const Sample& s = in.samples[static_cast<size_t>(z.sample)];
            std::snprintf(line, sizeof line, "  %3u-%-3u  %3u   %-20s  %7zu  %5u  %u   %s\n", z.lo, z.hi, z.root, s.name.c_str(),
                          s.frames(), s.rate, s.channels, s.looped ? (std::to_string(s.loopStart) + "-" + std::to_string(s.loopEnd)).c_str() : "one-shot");
            rep += line;
            std::snprintf(line, sizeof line, "%s_%03u_%03u_%03u.wav", wavPrefix(L.name).c_str(), z.root, z.lo, z.hi);
            files.save(std::string("wav/") + line, writeWav(s));
        }
        rep += "\n";
    }
    if (!notes.empty()) rep += notes + "\n";
    files.save("RESOURCES.tsv", manifest);
    rep += "Resources: RESOURCES.tsv, package " + pkg + " (provisional identities; the Pa5X assigns slots on import).\n"
           "  Budget several packages together: aiarranger-resources budget DIR...\n\n";
    rep += kMemoryNote;
    rep += "Pa5X import (owner, requires approval; docs/evidence/hardware/PA5X_SOUND_IMPORT_PROCEDURE.md):\n"
           "  A) Sample Edit > Menu > Import/Export > Import (manual p.1207), format TX Sound (TXPROG),\n"
           "     select tx/.../<name>.txprog, choose an empty User Sound location: one step (p.1210).\n"
           "  B) Same page, format SoundFont (SF2), Complete Bank (p.1209): one multisample per layer; then\n"
           "     in Sound Edit assign each to its own oscillator with the velocity zone above and save to an\n"
           "     empty User Sound.\n";
    files.save("REPORT.txt", rep);
    if (!files.ok) return fail("write failed; the unfinished package stays at " + out.string() + " (not a package; delete it)");
    if (const auto why = files.check(); !why.empty())
        return fail("the written package does not check (" + why + "); the unfinished package stays at " + out.string() + " (not a package; delete it)");
    if (!publishNewFolder(out, dest)) return fail("refusing: " + dest.string() + " appeared while writing; the package stays at " + out.string());
    return rep;
}

// Drum Kit package: tx/<name>/ (.txperf, .txprog, .txmatrix, samples) + REPORT.txt. No SF2: an SF2
// import yields multisamples, not a Drum Kit (manual p.1206). Never writes into an existing folder.
inline std::string writeDrumPackage(const DrumKit& kit, const std::filesystem::path& destGiven, const std::string& notes,
                                    std::string* error) {
    namespace fs = std::filesystem;
    auto fail = [&](std::string m) { if (error) *error = std::move(m); return std::string(); };
    std::error_code ec;
    const fs::path dest = folderPath(destGiven), out = incompletePath(dest);   // renamed to dest once complete, as writePackage
    if (fs::exists(dest, ec) || ec) return fail("refusing: " + dest.string() + " already exists");
    if (fs::exists(out, ec) || ec) return fail("refusing: " + out.string() + " already exists (an earlier write that did not finish)");
    if (const auto e = validate(kit); !e.empty()) return fail("invalid: " + e.front());
    const auto tx = writeTxPerf(kit);
    if (tx.empty()) return fail("the writer refused the kit (two samples or keys map to the same file name)");
    const std::string pkg = packageId(kit.name, tx);
    graph::Graph rg;
    std::string ge;
    if (!addDrumKit(rg, kit, pkg, &ge)) return fail("resource graph: " + ge);
    const std::string manifest = writeManifest(rg, pkg, graph::Kind::DrumKit);
    fs::create_directories(dest.parent_path(), ec);
    if (!fs::create_directory(out, ec)) return fail("cannot create " + out.string());
    pkgio::FileSet files{out};
    for (const auto& f : tx) files.save("tx/" + f.path, f.bytes);
    uint64_t bytes = 0;
    size_t stereo = 0;
    for (const auto& s : kit.samples) { bytes += s.pcm.size() * 2ull; stereo += s.channels == 2; }
    std::string rep = "AIArranger Drum Kit package (SOFTWARE evidence only until imported on a Pa5X)\n\n";
    char line[512];
    std::snprintf(line, sizeof line, "Drum Kit: %s\nTXPERF: tx/%s (copy the whole folder, structure unchanged)\n"
                  "Samples: %zu (%zu stereo), 16-bit, %.2f MB (%.4f%% of the 4 GB User Sample memory)\n\n",
                  kit.name.c_str(), tx[0].path.c_str(), kit.samples.size(), stereo, bytes / 1e6, 100.0 * bytes / 4e9);
    rep += line;
    rep += "Imported by the Pa5X from the TX16Wx files (GUIDE pp.24-25):\n"
           "  key  name                   family        excl  pan   trans tune  layers (lowest velocity: sample, level)\n";
    for (const auto& I : kit.instruments)
        for (const auto& k : I.keys) {
            const char* fam = k.family ? drumFamilyName(k.family) : "-";
            std::snprintf(line, sizeof line, "  %-4s %-22s %-13s %-5s %+4d%% %+4d  %+4d  ", txw::noteName(k.key).c_str(),
                          matrixName(k).c_str(), fam, I.choke ? std::to_string(I.choke).c_str() : "-", k.panPct, k.transpose, k.tuneCents);
            rep += line;
            for (const auto& c : k.layers) {
                std::snprintf(line, sizeof line, "[%u: %s %+.1f dB] ", c.velLo, kit.samples[static_cast<size_t>(c.sample)].name.c_str(), c.gainDb10 / 10.0);
                rep += line;
            }
            rep += "\n";
        }
    rep += "\nNOT imported; set by hand in Sound Edit if needed (GUIDE p.85: TX16Wx Sounds parameters are not imported):\n"
           "  amplitude/pitch envelopes, filters, decay; Drum Kit EQ; Voice Mixer settings other than pan and exclusive\n"
           "  group; kit effects. Ambience layers and round-robins are not produced by AIArranger (GUIDE pp.21-23).\n"
           "  Velocity switch points can be refined in Sound Edit > Drum Kit > Drum Sample Levels (GUIDE p.83).\n\n";
    if (!notes.empty()) rep += notes + "\n";
    files.save("RESOURCES.tsv", manifest);
    rep += "Resources: RESOURCES.tsv, package " + pkg + " (provisional identities; the Pa5X assigns slots on import).\n"
           "  Budget several packages together: aiarranger-resources budget DIR...\n\n";
    rep += kMemoryNote;
    rep += "Pa5X import (owner, requires approval):\n"
           "  Sample Edit > Menu > Import/Export > Import (manual p.1207), format TX DrumKit (TXPERF), select\n"
           "  tx/.../<name>.txperf, choose an empty User Drum Kit location (manual p.1210).\n";
    files.save("REPORT.txt", rep);
    if (!files.ok) return fail("write failed; the unfinished package stays at " + out.string() + " (not a package; delete it)");
    if (const auto why = files.check(); !why.empty())
        return fail("the written package does not check (" + why + "); the unfinished package stays at " + out.string() + " (not a package; delete it)");
    if (!publishNewFolder(out, dest)) return fail("refusing: " + dest.string() + " appeared while writing; the package stays at " + out.string());
    return rep;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_PACKAGE_WRITER_H
