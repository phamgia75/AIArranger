#ifndef AI_ARRANGER_MEDIA_SOUND_TXPERF_WRITER_H
#define AI_ARRANGER_MEDIA_SOUND_TXPERF_WRITER_H

// TX16Wx Performance package for a Pa5X Standard Drum Kit (the Pa5X imports a .txperf as a Drum Kit,
// manual v1.4 p.1206; GUIDE pp.18-20). Every element and attribute mirrors Korg's example
// "Real-Drums DrumKit" (guide download): a .txperf with one slot -> one .txprog -> one .txmatrix per
// key (single column, Y axis = velocity, one row per layer, row value = lowest velocity) -> WAVs in
// a samples folder. Group = Drum Instrument with tx:choke-group (Exclusive Group); region = one key
// with pan, shift (transpose) and tune. Nothing outside that example is written.
// SOFTWARE output: no file from this writer has been imported on a Pa5X yet.

#include "media/sound/drumkit.h"
#include "media/sound/txprog_writer.h"

namespace ai_arranger::media::sound {

// Returns the package files (the .txperf first, then the .txprog), or empty when the kit is invalid
// or two samples/matrices would share a file name.
inline std::vector<PackageFile> writeTxPerf(const DrumKit& kit) {
    using namespace txw;
    if (!validate(kit).empty()) return {};
    const std::string base = fileSafe(kit.name), dir = base + " Samples";
    const std::string bom = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n";
    std::vector<PackageFile> files(2);
    std::set<std::string> names;   // every file name in the package folder, case-insensitive (FAT USB sticks)
    auto unique = [&](std::string n) { for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return names.insert(n).second; };

    std::string perf = bom + "<tx:performance tx:name=\"" + esc(kit.name) + "\" xsi:schemaLocation=\"http://www.tx16wx.com/3.0/ performance\"\n"
        "   tx:created-by=\"30601\" xmlns:tx=\"http://www.tx16wx.com/3.0/performance\"\n"
        "   xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
        "   <tx:slot tx:name=\"Ch01\" tx:program=\"" + esc(base + ".txprog") + "\" tx:channel=\"1\" tx:output=\"Out 1\" tx:color=\"blue\">\n"
        "      <tx:velocity-curve tx:smooth=\"true\" tx:shape=\"Exponential 2\"/>\n"
        "      <tx:volume tx:value=\"0 dB\"/>\n      <tx:pan tx:value=\"0%\"/>\n      <tx:transpose tx:value=\"0\"/>\n      <tx:detune tx:value=\"0\"/>\n";
    for (int i = 0; i < 3; ++i) perf += "      <tx:send>\n         <tx:level tx:value=\"0 dB\"/>\n      </tx:send>\n";
    perf += "   </tx:slot>\n</tx:performance>\n";
    files[0] = {base + "/" + base + ".txperf", std::vector<uint8_t>(perf.begin(), perf.end())};
    unique(base + ".txperf"); unique(base + ".txprog"); unique(dir);

    std::vector<std::string> wavOf(kit.samples.size());
    std::set<std::string> wavNames;
    for (size_t i = 0; i < kit.samples.size(); ++i) {
        wavOf[i] = fileSafe(kit.samples[i].name) + ".wav";
        std::string lower = wavOf[i];
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!wavNames.insert(lower).second) return {};
    }
    auto gain = [](int16_t g) {   // the example writes "0 dB", "2.5 dB", "-2.2 dB"
        char b[24];
        if (g) std::snprintf(b, sizeof b, "%.1f dB", g / 10.0); else std::snprintf(b, sizeof b, "0 dB");
        return std::string(b);
    };

    std::string prog = bom + "<tx:program tx:quality=\"Default\" tx:name=\"" + esc(kit.name) +
        "\" xsi:schemaLocation=\"http://www.tx16wx.com/3.0/ program\"\n   tx:created-by=\"30601\" xmlns:tx=\"http://www.tx16wx.com/3.0/program\"\n"
        "   xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n";
    int id = 0;
    for (const DrumInstrument& I : kit.instruments)
        for (const DrumKey& k : I.keys) {
            const std::string mname = matrixName(k), mfile = fileSafe(mname) + ".txmatrix";
            if (!unique(mfile)) return {};
            prog += "   <tx:wavematrix tx:id=\"" + std::to_string(id++) + "\" tx:path=\"" + esc(mfile) + "\"/>\n";
            std::string m = bom + "<tx:wavematrix xmlns:tx=\"http://www.tx16wx.com/3.0/wavematrix\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                "   xsi:schemaLocation=\"http://www.tx16wx.com/3.0/ wavematrix\" tx:created-by=\"30601\" tx:name=\"" + esc(mname) + "\">\n";
            for (size_t l = 0; l < k.layers.size(); ++l) {
                const Sample& s = kit.samples[static_cast<size_t>(k.layers[l].sample)];
                m += "   <tx:wave tx:root=\"" + noteName(s.root) + "\" tx:id=\"" + std::to_string(l) + "\" tx:path=\"" + esc(dir + "/" + wavOf[static_cast<size_t>(k.layers[l].sample)]) + "\"";
                m += s.looped ? ">\n      <tx:loop tx:end=\"" + std::to_string(s.loopEnd) + "\" tx:start=\"" + std::to_string(s.loopStart) +
                                    "\" tx:mode=\"Forward\" tx:name=\"1\"/>\n   </tx:wave>\n"
                              : "/>\n";
            }
            m += "   <tx:xaxis tx:source=\"--\">\n      <tx:curve tx:shape=\"Linear\" tx:smooth=\"true\"/>\n   </tx:xaxis>\n"
                 "   <tx:yaxis tx:source=\"Vel\">\n      <tx:curve tx:shape=\"Linear\" tx:smooth=\"true\"/>\n   </tx:yaxis>\n";
            for (size_t l = 0; l < k.layers.size(); ++l)
                m += "   <tx:row tx:value=\"" + std::to_string(k.layers[l].velLo) + "\">\n      <tx:cell tx:pan=\"0%\" tx:attenuation=\"" +
                     gain(k.layers[l].gainDb10) + "\" tx:mode=\"DFD\" tx:wave=\"" + std::to_string(l) + "\"/>\n   </tx:row>\n";
            m += "</tx:wavematrix>\n";
            files.push_back({base + "/" + mfile, std::vector<uint8_t>(m.begin(), m.end())});
        }
    for (size_t i = 0; i < kit.samples.size(); ++i) files.push_back({base + "/" + dir + "/" + wavOf[i], writeWav(kit.samples[i])});

    prog += "   <tx:bounds tx:high-vel=\"127\" tx:low-key=\"C-1\" tx:high-key=\"G9\" tx:low-vel=\"0\"/>\n";
    uint64_t h = 1469598103934665603ull;   // deterministic UUID-shaped id (the examples use UUIDs)
    for (char c : kit.name) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ull; }
    char shape[40];
    std::snprintf(shape, sizeof shape, "a1a0a1a0-0000-4000-8000-%012llx", static_cast<unsigned long long>(h & 0xFFFFFFFFFFFFull));
    // Preview shape only (the Pa5X does not import it, GUIDE p.85); values as in the drum example.
    prog += std::string("   <tx:soundshape tx:id=\"") + shape + "\" tx:name=\"" + esc(kit.name) + "\" tx:pan=\"0%\" tx:unison-depth=\"100%\"\n"
        "      tx:unison=\"1\" tx:pwm=\"0%\" tx:volume=\"0 dB\" tx:glide-mode=\"Held\" tx:unison-spread=\"0Ct\" tx:unison-pan=\"100%\"\n"
        "      tx:unison-start=\"0ms\" tx:unison-cyclic-spread=\"false\">\n"
        "      <tx:aeg tx:attack=\"0ms\" tx:attack-shape=\"-50%\" tx:decay1=\"500ms\" tx:decay1-shape=\"-50%\" tx:decay2=\"500ms\"\n"
        "         tx:decay2-shape=\"-50%\" tx:release=\"50.0ms\" tx:release-shape=\"-50%\" tx:level1=\"0 dB\" tx:level2=\"0 dB\"\n"
        "         tx:sustain=\"0 dB\"/>\n"
        "      <tx:send tx:level=\"0 dB\"/>\n      <tx:send tx:level=\"0 dB\"/>\n      <tx:send tx:level=\"0 dB\"/>\n"
        "      <tx:modulation/>\n   </tx:soundshape>\n";
    id = 0;
    for (const DrumInstrument& I : kit.instruments) {
        prog += "   <tx:group tx:name=\"" + esc(I.name) + "\" tx:volume=\"0 dB\" tx:pan=\"0%\" tx:coarse=\"0\" tx:fine=\"0\" tx:playmode=\"Oneshot\"\n"
                "      tx:polymode=\"Poly\" tx:playback=\"Resample\" tx:noteprio=\"Last\" tx:scale=\"100\" tx:choke-group=\"" + std::to_string(I.choke) +
                "\" tx:output=\"--\"\n      tx:quality=\"Default\" tx:color=\"yellow\" tx:soundshape=\"" + std::string(shape) + "\">\n";
        for (const DrumKey& k : I.keys) {
            prog += "      <tx:region tx:mode=\"--\" tx:attenuation=\"0 dB\" tx:pan=\"" + std::to_string(k.panPct) + "%\" tx:loop=\"0\" tx:release=\"0\" tx:matrix=\"" +
                    std::to_string(id++) + "\" tx:shift=\"" + std::to_string(k.transpose) + "\"\n         tx:tune=\"" + std::to_string(k.tuneCents) + "\">\n"
                    "         <tx:bounds tx:high-vel=\"127\" tx:low-key=\"" + noteName(k.key) + "\" tx:low-vel=\"0\" tx:high-key=\"" + noteName(k.key) + "\"/>\n"
                    "      </tx:region>\n";
        }
        prog += "   </tx:group>\n";
    }
    prog += "</tx:program>\n";
    files[1] = {base + "/" + base + ".txprog", std::vector<uint8_t>(prog.begin(), prog.end())};
    return files;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_TXPERF_WRITER_H
