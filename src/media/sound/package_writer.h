#ifndef AI_ARRANGER_MEDIA_SOUND_PACKAGE_WRITER_H
#define AI_ARRANGER_MEDIA_SOUND_PACKAGE_WRITER_H

// USB-ready folder for one instrument (docs/SOUND_CONVERSION_ROADMAP.md S4/S8):
//   <stem>.sf2        Pa5X Import > SoundFont (SF2): multisamples (manual p.1209)
//   tx/<name>/...     Pa5X Import > TX Sound (.txprog): a complete Sound (p.1210)
//   wav/<layer>_<OrgKey>_<LowKey>_<HighKey>.wav   looped WAVs, the Pa5X's own export naming (p.1213)
//   REPORT.txt        zones, loops, memory use, import steps, caller notes
// Never writes into an existing folder. Returns the report, or "" with *error set.

#include "media/sound/sf2_writer.h"
#include "media/sound/txprog_writer.h"
#include "media/sound/wav_writer.h"
#include <filesystem>
#include <fstream>

namespace ai_arranger::media::sound {

inline std::string writePackage(const Instrument& in, const std::filesystem::path& out, const std::string& notes,
                                std::string* error) {
    namespace fs = std::filesystem;
    auto fail = [&](std::string m) { if (error) *error = std::move(m); return std::string(); };
    std::error_code ec;
    if (fs::exists(out, ec) || ec) return fail("refusing: " + out.string() + " already exists");
    if (const auto e = validate(in); !e.empty()) return fail("invalid: " + e.front());
    const auto sf2 = writeSf2(in);
    const auto tx = writeTxProg(in);
    if (sf2.empty() || tx.empty()) return fail("a writer refused the instrument (see txprog_writer.h preconditions)");
    if (!fs::create_directories(out / "wav", ec)) return fail("cannot create " + out.string());

    bool ok = true;
    auto save = [&](const fs::path& p, const std::vector<uint8_t>& b) {
        ok &= fs::create_directories(p.parent_path(), ec) || fs::is_directory(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        ok &= static_cast<bool>(f);
    };
    auto under = [](std::string s) { for (char& c : s) if (c == ' ') c = '_'; return s; };
    uint64_t h = 1469598103934665603ull;   // FNV-1a fingerprint for the report, not a security hash
    for (uint8_t c : sf2) { h ^= c; h *= 1099511628211ull; }

    const std::string stem = under(txw::fileSafe(in.name));
    save(out / (stem + ".sf2"), sf2);
    for (const auto& f : tx) save(out / "tx" / f.path, f.bytes);

    std::string rep = "AIArranger Sound package (SOFTWARE evidence only until imported on a Pa5X)\n\n";
    char line[320];
    std::snprintf(line, sizeof line,
                  "Instrument: %s\nSF2: %s.sf2  %zu bytes  fnv1a %016llx\nTXPROG: tx/%s + %zu WAVs (copy the whole folder, structure unchanged)\n"
                  "Velocity layers: %zu%s\nSamples: %zu, 16-bit mono, %.2f MB (%.4f%% of the 4 GB User Sample memory)\n\n",
                  in.name.c_str(), stem.c_str(), sf2.size(), static_cast<unsigned long long>(h), tx[0].path.c_str(), tx.size() - 1,
                  in.layers.size(), in.layers.size() == 2 ? (" (High from velocity " + std::to_string(in.velocitySwitch) + ")").c_str() : "",
                  in.samples.size(), sampleBytes(in) / 1e6, 100.0 * sampleBytes(in) / 4e9);
    rep += line;
    for (size_t l = 0; l < in.layers.size(); ++l) {
        const Layer& L = in.layers[l];
        rep += "Layer " + std::to_string(l) + (in.layers.size() == 1 ? " " : l == 0 ? " (Low)  " : " (High) ") + L.name + "\n";
        rep += "  keys     root  sample                frames    rate   loop\n";
        for (const Zone& z : L.zones) {
            const Sample& s = in.samples[static_cast<size_t>(z.sample)];
            std::snprintf(line, sizeof line, "  %3u-%-3u  %3u   %-20s  %7zu  %5u   %s\n", z.lo, z.hi, z.root, s.name.c_str(),
                          s.pcm.size(), s.rate, s.looped ? (std::to_string(s.loopStart) + "-" + std::to_string(s.loopEnd)).c_str() : "one-shot");
            rep += line;
            std::snprintf(line, sizeof line, "%s_%03u_%03u_%03u.wav", under(txw::fileSafe(L.name)).c_str(), z.root, z.lo, z.hi);
            save(out / "wav" / line, writeWav(s));
        }
        rep += "\n";
    }
    if (!notes.empty()) rep += notes + "\n";
    rep += "Pa5X import (owner, requires approval; docs/evidence/hardware/PA5X_SOUND_IMPORT_PROCEDURE.md):\n"
           "  A) Sample Edit > Menu > Import/Export > Import (manual p.1207), format TX Sound (TXPROG),\n"
           "     select tx/.../<name>.txprog, choose an empty User Sound location: one step (p.1210).\n"
           "  B) Same page, format SoundFont (SF2), Complete Bank (p.1209): one multisample per layer; then\n"
           "     in Sound Edit assign them to an oscillator's Low/High layers with the velocity switch above\n"
           "     and save to an empty User Sound.\n";
    save(out / "REPORT.txt", std::vector<uint8_t>(rep.begin(), rep.end()));
    if (!ok) return fail("write failed under " + out.string());
    return rep;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_PACKAGE_WRITER_H
