#ifndef AI_ARRANGER_MEDIA_SOUND_TXPROG_WRITER_H
#define AI_ARRANGER_MEDIA_SOUND_TXPROG_WRITER_H

// TX16Wx Program (.txprog) package writer (docs/SOUND_CONVERSION_ROADMAP.md S5, format facts in
// docs/research/TXPROG_FORMAT_NOTES.md). The Pa5X imports a .txprog as a complete Sound: Program name ->
// Sound name, each Group -> a Multisample on an oscillator, Region key/velocity bounds -> zones,
// wave root -> Original Note, Region attenuation -> sample Level, loops kept (Korg guide "Making
// Pa5X Sounds and Drum Kits with TX16Wx", pp.14-16, 24-27, 84). Package layout follows the guide
// (p.31): <folder>/<name>.txprog with the samples in a subfolder, paths relative.
// One Group per velocity layer; with two layers the Low Group's regions take velocities 0..switch-1
// and the High Group's switch..127. Envelopes and filters do not transfer (guide p.85).
// SOFTWARE output: no file from this writer has been imported on a Pa5X yet.

#include "media/sound/instrument.h"
#include "media/sound/wav_writer.h"
#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <utility>

namespace ai_arranger::media::sound {

namespace txw {
inline std::string noteName(int n) {   // TX16Wx key names: 0 = C-1 ... 127 = G9
    static const char* k[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(k[n % 12]) + std::to_string(n / 12 - 1);
}
inline std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}
// File names: keep letters, digits, space, '-', '_'; everything else becomes '_'.
inline std::string fileSafe(const std::string& s) {
    std::string o;
    for (char c : s) o += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') ? c : '_';
    return o;
}
} // namespace txw

struct PackageFile {
    std::string path;              // relative, '/' separated
    std::vector<uint8_t> bytes;
};

// Returns the package files (the .txprog first), or empty when the instrument is invalid, a zone
// root differs from its sample's root (a .txprog keeps the root on the wave), or two samples would
// share a file name.
inline std::vector<PackageFile> writeTxProg(const Instrument& in) {
    using namespace txw;
    if (!validate(in).empty()) return {};
    for (const Layer& L : in.layers)
        for (const Zone& z : L.zones)
            if (z.root != in.samples[static_cast<size_t>(z.sample)].root) return {};   // shortcut: no per-zone root; add a coarse/fine offset if a source needs it

    const std::string base = fileSafe(in.name);
    const std::string dir = base + " Samples";
    std::vector<PackageFile> files(1);
    files[0].path = base + "/" + base + ".txprog";
    std::set<std::string> seen;
    std::string x = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n";
    // Element and attribute set mirrors Korg's example Sound (guide download, "Acoustic Piano.txprog"),
    // the one .txprog known to import on a Pa5X. Omitted on purpose: tx:created-by (TX16Wx's own build stamp).
    x += "<tx:program tx:quality=\"Default\" tx:name=\"" + esc(in.name) +
         "\" xsi:schemaLocation=\"http://www.tx16wx.com/3.0/ program\" xmlns:tx=\"http://www.tx16wx.com/3.0/program\"\n"
         "   xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n";
    for (size_t i = 0; i < in.samples.size(); ++i) {
        const Sample& s = in.samples[i];
        const std::string wav = fileSafe(s.name) + ".wav";
        if (!seen.insert(wav).second) return {};
        files.push_back({base + "/" + dir + "/" + wav, writeWav(s)});
        x += "   <tx:wave tx:path=\"" + esc(dir + "/" + wav) + "\" tx:id=\"" + std::to_string(i) +
             "\" tx:root=\"" + noteName(s.root) + "\">\n";
        // loop 1 = the sample's loop (end exclusive: Korg's piano example ends a loop at the frame count),
        // loop 2 = the empty "None" loop every example wave carries
        x += s.looped ? "      <tx:loop tx:name=\"1\" tx:end=\"" + std::to_string(s.loopEnd) + "\" tx:mode=\"Forward\" tx:start=\"" +
                            std::to_string(s.loopStart) + "\"/>\n"
                      : "      <tx:loop tx:name=\"1\" tx:end=\"1\" tx:mode=\"None\" tx:start=\"0\"/>\n";
        x += "      <tx:loop tx:name=\"2\" tx:end=\"1\" tx:mode=\"None\" tx:start=\"0\"/>\n"
             "      <tx:bounds tx:high-vel=\"127\" tx:low-key=\"C-1\" tx:high-key=\"G9\" tx:low-vel=\"1\"/>\n"
             "   </tx:wave>\n";
    }
    // Preview shape only (the Pa5X ignores it, guide p.85): 0.1 s release like the SF2 writer,
    // velocity curve Exponential 2 = the Pa5X's Velocity Intensity 80 (guide p.42).
    x += "   <tx:bounds tx:high-vel=\"127\" tx:low-key=\"C-1\" tx:high-key=\"G9\" tx:low-vel=\"0\"/>\n";   // program range, as in every example
    uint64_t h = 1469598103934665603ull;   // deterministic UUID-shaped id (the examples use UUIDs)
    for (char c : in.name) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ull; }
    char shape[40];
    std::snprintf(shape, sizeof shape, "a1a0a1a0-0000-4000-8000-%012llx", static_cast<unsigned long long>(h & 0xFFFFFFFFFFFFull));
    x += std::string("   <tx:soundshape tx:id=\"") + shape + "\" tx:name=\"" + esc(in.name) + "\" tx:pan=\"0%\" tx:unison-depth=\"100%\"\n"
         "      tx:unison=\"1\" tx:pwm=\"0%\" tx:volume=\"0 dB\" tx:glide-mode=\"Held\" tx:unison-spread=\"0Ct\" tx:unison-pan=\"100%\"\n"
         "      tx:unison-start=\"0ms\" tx:unison-cyclic-spread=\"false\">\n"
         "      <tx:aeg tx:attack=\"0ms\" tx:attack-shape=\"-50%\" tx:decay1=\"500ms\" tx:decay1-shape=\"-50%\" tx:decay2=\"500ms\"\n"
         "         tx:decay2-shape=\"-50%\" tx:release=\"100ms\" tx:release-shape=\"-50%\" tx:level1=\"0 dB\" tx:level2=\"0 dB\"\n"
         "         tx:sustain=\"0 dB\"/>\n"
         "      <tx:send tx:level=\"0 dB\"/>\n      <tx:send tx:level=\"0 dB\"/>\n      <tx:send tx:level=\"0 dB\"/>\n"
         "      <tx:modulation/>\n"
         "      <tx:velocity-curve tx:smooth=\"true\" tx:shape=\"Exponential 2\"/>\n"
         "   </tx:soundshape>\n";
    for (size_t l = 0; l < in.layers.size(); ++l) {
        const Layer& L = in.layers[l];
        const int vlo = in.layers.size() == 2 && l == 1 ? in.velocitySwitch : 0;
        const int vhi = in.layers.size() == 2 && l == 0 ? in.velocitySwitch - 1 : 127;
        x += "   <tx:group tx:name=\"" + esc(L.name) + "\" tx:volume=\"0 dB\" tx:pan=\"0%\" tx:coarse=\"0\" tx:fine=\"0\" "
             "tx:playmode=\"Normal\"\n      tx:polymode=\"Poly\" tx:playback=\"Resample\" tx:noteprio=\"Last\" tx:scale=\"100\" "
             "tx:choke-group=\"0\" tx:output=\"--\"\n      tx:quality=\"Default\" tx:color=\"yellow\" tx:soundshape=\"" + std::string(shape) + "\">\n";
        for (const Zone& z : L.zones) {
            char att[32];
            if (z.levelCentibels) std::snprintf(att, sizeof att, "%.1f dB", -z.levelCentibels / 10.0);
            else std::snprintf(att, sizeof att, "0 dB");   // the examples write zero as "0 dB"
            x += "      <tx:region tx:mode=\"DFD\" tx:attenuation=\"" + std::string(att) + "\" tx:pan=\"0%\" tx:loop=\"0\" "
                 "tx:release=\"0\" tx:wave=\"" + std::to_string(z.sample) + "\" tx:tune=\"" + std::to_string(z.tuneCents) + "\">\n";
            x += "         <tx:bounds tx:low-key=\"" + noteName(z.lo) + "\" tx:high-key=\"" + noteName(z.hi) +
                 "\" tx:low-vel=\"" + std::to_string(vlo) + "\" tx:high-vel=\"" + std::to_string(vhi) + "\"/>\n";
            x += "      </tx:region>\n";
        }
        x += "   </tx:group>\n";
    }
    x += "</tx:program>\n";
    files[0].bytes.assign(x.begin(), x.end());
    return files;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_TXPROG_WRITER_H
