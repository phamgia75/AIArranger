// aiarranger-sound demo --out DIR (roadmap S4): builds the original demo organ and writes a
// USB-ready folder: <name>.sf2 (for Pa5X Import SF2), tx/<name>/ (a .txprog + samples, for Pa5X
// Import TX Sound), wav/ (looped WAVs named
// <name>_<OrgKey>_<LowKey>_<HighKey>.wav, the Pa5X's own export naming, p.1213) and REPORT.txt.
// Refuses to write into an existing DIR. Exit 0 = written, 1 = usage/IO/validation error.
#include "media/sound/additive_source.h"
#include "media/sound/sf2_writer.h"
#include "media/sound/wav_writer.h"
#include "media/sound/txprog_writer.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace ai_arranger::media::sound;

static bool save(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

static uint64_t fnv1a(const std::vector<uint8_t>& b) {   // report fingerprint, not a security hash
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char** argv) {
    if (argc != 4 || std::string(argv[1]) != "demo" || std::string(argv[2]) != "--out") {
        std::fprintf(stderr, "usage: aiarranger-sound demo --out DIR   (DIR must not exist)\n");
        return 1;
    }
    const fs::path out = argv[3];
    std::error_code ec;
    if (fs::exists(out, ec)) { std::fprintf(stderr, "refusing: %s already exists\n", out.c_str()); return 1; }

    double worst = 0;
    const Instrument in = buildDemoOrgan(&worst);
    if (const auto e = validate(in); !e.empty()) {
        for (const auto& m : e) std::fprintf(stderr, "invalid: %s\n", m.c_str());
        return 1;
    }
    const auto sf2 = writeSf2(in);
    if (!fs::create_directories(out / "wav", ec)) { std::fprintf(stderr, "cannot create %s\n", out.c_str()); return 1; }

    const std::string stem = "AIA_Demo_Organ";
    bool ok = save(out / (stem + ".sf2"), sf2);
    std::string rep = "AIArranger Sound export (SOFTWARE evidence only; not yet imported on a Pa5X)\n\n";
    char line[256];
    std::snprintf(line, sizeof line, "Instrument: %s\nSF2: %s.sf2  %zu bytes  fnv1a %016llx\nVelocity switch: %u (Low < %u <= High)\n"
                  "Samples: %zu, 16-bit mono 48 kHz, %.2f MB (%.4f%% of the 4 GB User Sample memory)\nWorst pitch error: %.4f cents\n\n",
                  in.name.c_str(), stem.c_str(), sf2.size(), static_cast<unsigned long long>(fnv1a(sf2)), in.velocitySwitch,
                  in.velocitySwitch, in.samples.size(), sampleBytes(in) / 1e6, 100.0 * sampleBytes(in) / 4e9, worst);
    rep += line;
    for (size_t l = 0; l < in.layers.size(); ++l) {
        const Layer& L = in.layers[l];
        rep += "Layer " + std::to_string(l) + (l == 0 ? " (Low)  " : " (High) ") + L.name + "\n";
        rep += "  keys     root  sample                frames   loop\n";
        const std::string lname = l == 0 ? "Soft" : "Bright";
        for (const Zone& z : L.zones) {
            const Sample& s = in.samples[static_cast<size_t>(z.sample)];
            std::snprintf(line, sizeof line, "  %3u-%-3u  %3u   %-20s  %6zu   %u-%u\n", z.lo, z.hi, z.root, s.name.c_str(),
                          s.pcm.size(), s.loopStart, s.loopEnd);
            rep += line;
            std::snprintf(line, sizeof line, "AIA_Org%s_%03u_%03u_%03u.wav", lname.c_str(), z.root, z.lo, z.hi);
            ok &= save(out / "wav" / line, writeWav(s));
        }
        rep += "\n";
    }
    const auto tx = writeTxProg(in);
    if (tx.empty()) { std::fprintf(stderr, "TXPROG package refused\n"); return 1; }
    for (const auto& f : tx) {
        const fs::path p = out / "tx" / f.path;
        ok &= fs::create_directories(p.parent_path(), ec) || fs::is_directory(p.parent_path());
        ok &= save(p, f.bytes);
    }
    rep += "TXPROG: tx/" + tx[0].path + " + " + std::to_string(tx.size() - 1) + " WAVs (copy the whole tx/ folder content)\n\n";
    rep += "Pa5X import (owner, requires approval; docs/SOUND_CONVERSION_ROADMAP.md S6):\n"
           "  A) Sample Edit > Menu > Import/Export > Import (manual p.1207), format TX Sound (TXPROG),\n"
           "     select tx/.../<name>.txprog, choose an empty User Sound location: one step (p.1210).\n"
           "  B) Same page, format SoundFont (SF2), Complete Bank (p.1209): two multisamples; then in\n"
           "     Sound Edit assign them to an oscillator's Low/High layers with the velocity switch above\n"
           "     and save to an empty User Sound.\n";
    ok &= save(out / "REPORT.txt", std::vector<uint8_t>(rep.begin(), rep.end()));
    if (!ok) { std::fprintf(stderr, "write failed under %s\n", out.c_str()); return 1; }
    std::fputs(rep.c_str(), stdout);
    return 0;
}
