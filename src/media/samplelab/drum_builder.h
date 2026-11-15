#ifndef AI_ARRANGER_MEDIA_SAMPLELAB_DRUM_BUILDER_H
#define AI_ARRANGER_MEDIA_SAMPLELAB_DRUM_BUILDER_H

// Drum Kit builder (P1): a folder of the user's WAVs + an editable drum map -> sound::DrumKit, which the EXISTING
// writers turn into a TXPERF Drum Kit package (writeDrumPackage) or, through drumKitAsInstrument(), a KMP + KSF folder
// (writeKorgFolder). One sample per key. The folder is only read. Offline; never the message or audio thread.
//
// Drum map, one line per key:  <key> <file name> [group=<name>] [choke=0-8] [level=<dB>] [pan=<-100..100>] [family=<F>] [name=<name>]
//   key: note name (C-1..G9, C4 = 60) or 0-127. Options are single words (use '_' for spaces), read from the right, so
//   the file name may hold spaces. group = Drum Instrument (TX16Wx group; keys of one group share its choke), choke =
//   Exclusive Group (0 none), level = -60..+6 dB, family = B S H T C L E X (drumkit.h, GUIDE p.80), name = drum sample
//   name (default: the file name without .wav, at most 18 characters).
// WAVs of any PCM depth or rate are read through optimize.h: 16-bit (TPDF dither), rate into 11 025-48 000 Hz.

#include "media/samplelab/optimize.h"
#include "media/samplelab/sample_lab_io.h"   // listBatchWavs
#include "media/sound/drumkit.h"
#include <map>
#include <set>
#include <sstream>

namespace ai_arranger::media::samplelab {

inline std::string drumName(const std::string& file) {
    std::string n = fs::path(file).stem().string().substr(0, 18);
    for (char& c : n) if (c < 0x20 || c > 0x7E || c == '#') c = '_';
    while (!n.empty() && n.back() == ' ') n.pop_back();   // a KSF name drops a trailing space
    return n.empty() ? "Drum" : n;
}

// One line per .wav in `dir` (sorted), on keys from `firstKey` up; each file its own group. "" or the reason.
inline std::string proposeDrumMap(const fs::path& dir, std::string* text, uint8_t firstKey = 36) {
    std::vector<fs::path> paths;
    if (const auto e = listBatchWavs(dir, paths); !e.empty()) return e;
    if (paths.empty()) return "no .wav files in " + dir.string();
    std::sort(paths.begin(), paths.end());
    if (firstKey + paths.size() - 1 > 127) return std::to_string(paths.size()) + " WAVs do not fit on keys " + std::to_string(firstKey) + "-127";
    std::string o = "# Drum map: <key> <file> [group=] [choke=0-8] [level=dB] [pan=-100..100] [family=B|S|H|T|C|L|E|X] [name=]\n"
                    "# One sample per key. Keys sharing a group share its choke (e.g. hi-hats: group=HiHat choke=1).\n";
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string f = paths[i].filename().string();
        if (sound::printable(f) != f || f.find(" #") != std::string::npos || f.find('=') != std::string::npos) {
            o += "# NOT USED: " + sound::printable(f) + " (control characters, \" #\" or '=' in the name: use a renamed copy)\n";
            continue;
        }
        std::string g = drumName(f);
        for (char& c : g) if (c == ' ') c = '_';
        o += sound::noteName(int(firstKey + i)) + " " + f + " group=" + g + "\n";
    }
    *text = o;
    return "";
}

// Bounds checked per line BEFORE its audio is read (Codex r2): one line per key, and the WAV files of the whole map at
// most kDrumKitBudgetBytes, so a pasted map cannot make the job load thousands of samples it must then refuse.
inline constexpr uint64_t kDrumKitBudgetBytes = 128ull << 20;

// The kit from `dir` and the drum map. notes (optional): the per-file format conversion (before -> after).
inline std::string buildDrumKit(const fs::path& dir, const std::string& kitName, const std::string& map, sound::DrumKit* out, std::string* notes = nullptr) {
    sound::DrumKit kit;
    kit.name = kitName;
    std::map<std::string, size_t> group;   // name -> instruments index
    std::map<int, int> keyLine;            // key -> the map line that took it
    uint64_t budget = 0;                   // WAV bytes of the lines read so far
    std::istringstream is(map);
    int n = 0;
    std::string conv;
    for (std::string line; std::getline(is, line);) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (size_t c = 0; c < line.size(); ++c)
            if (line[c] == '#' && (c == 0 || line[c - 1] == ' ' || line[c - 1] == '\t')) { line.resize(c); break; }
        std::vector<std::string> t;
        std::istringstream ls(line);
        for (std::string w; ls >> w;) t.push_back(w);
        if (t.empty()) continue;
        const std::string at = "drum map line " + std::to_string(n) + ": ";
        const int key = sound::parseNote(t[0]);
        if (key < 0) return at + "starts with a key (C-1..G9, C4 = 60, or 0-127)";
        if (auto [k, fresh] = keyLine.emplace(key, n); !fresh)
            return at + "key " + std::to_string(key) + " (" + sound::noteName(key) + ") is already on line " + std::to_string(k->second) + " (one sample per key)";
        std::map<std::string, std::string> opt;
        size_t end = t.size();
        while (end > 1 && t[end - 1].find('=') != std::string::npos) {
            const auto& w = t[end - 1];
            const auto k = w.substr(0, w.find('='));
            if (k != "group" && k != "choke" && k != "level" && k != "pan" && k != "family" && k != "name") return at + "unknown option '" + k + "'";
            opt[k] = w.substr(w.find('=') + 1);
            --end;
        }
        if (end < 2) return at + "needs <key> <file>";
        size_t p = 0;   // the file name: from the second token to the last one before the options, spaces kept
        p = line.find_first_not_of(" \t", line.find_first_of(" \t", line.find_first_not_of(" \t")));
        size_t q = p;
        for (size_t i = 1; i < end; ++i) q = line.find(t[i], q) + t[i].size();
        const std::string file = line.substr(p, q - p);
        if (file.find_first_of("/\\") != std::string::npos) return at + "a file name, not a path";
        auto number = [&](const char* k, double lo, double hi, double def, double* v) {
            *v = def;
            if (!opt.count(k)) return true;
            char* e = nullptr;
            *v = std::strtod(opt[k].c_str(), &e);
            return !opt[k].empty() && e && !*e && *v >= lo && *v <= hi;
        };
        double choke, level, pan;
        if (!number("choke", 0, 8, 0, &choke) || choke != std::floor(choke)) return at + "choke is 0-8";
        if (!number("level", -60, 6, 0, &level)) return at + "level is -60..+6 dB";
        if (!number("pan", -100, 100, 0, &pan) || pan != std::floor(pan)) return at + "pan is a whole number -100..100";
        char family = 0;
        if (opt.count("family")) {
            if (opt["family"].size() != 1 || !sound::drumFamilyName(opt["family"][0])) return at + "family is one of B S H T C L E X";
            family = opt["family"][0];
        }
        // read the sample (exactly this name in the folder, a regular file of at most 16 MiB)
        const fs::path path = dir / file;
        std::error_code ec;
        if (!fs::is_regular_file(fs::symlink_status(path, ec)) || fs::file_size(path, ec) > (16u << 20) || ec)
            return at + file + " is not a regular file of at most 16 MiB in " + dir.string();
        if ((budget += fs::file_size(path, ec)) > kDrumKitBudgetBytes)
            return at + "the kit's WAV files pass the " + std::to_string(kDrumKitBudgetBytes >> 20) + " MiB budget here; use fewer or shorter samples";
        std::ifstream f(path, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        sound::Sample s;
        WavFormat fmt;
        bool noSmpl = false;
        if (const auto e = readWavAny(bytes, &s, &fmt, &noSmpl); !e.empty()) return at + file + ": " + e;
        if (const auto e = resample(s, pa5xRate(s.rate)); !e.empty()) return at + file + ": " + e;
        s.looped = false;   // drums are one-shots (the TXPERF matrix and KSF play them through); resample refused an invalid loop
        s.root = uint8_t(key);   // the drum plays as recorded on its key
        const std::string name = opt.count("name") ? opt["name"] : drumName(file);
        s.name = name;
        const WavFormat after{s.rate, 16, false, s.channels, s.frames()};
        if (fmt.rate != after.rate || fmt.bits != 16 || fmt.isFloat) conv += "  " + name + ": " + formatLine(fmt) + " -> " + formatLine(after) + "\n";
        std::string g = opt.count("group") ? opt["group"] : name;
        auto [it, fresh] = group.emplace(g, kit.instruments.size());
        if (fresh) kit.instruments.push_back({g, uint8_t(choke), {}});
        auto& I = kit.instruments[it->second];
        if (opt.count("choke") && I.choke != uint8_t(choke)) return at + "group " + g + " already has choke " + std::to_string(I.choke);
        I.keys.push_back({uint8_t(key), name, family, {{int(kit.samples.size()), 0, int16_t(std::lround(level * 10))}}, int8_t(pan), 0, 0});
        kit.samples.push_back(std::move(s));
    }
    if (kit.samples.empty()) return "the drum map has no key lines";
    if (const auto e = sound::validate(kit); !e.empty()) return "drum kit: " + e.front();
    if (notes) *notes = conv;
    *out = std::move(kit);
    return "";
}

// The kit as one KRONOS multisample for writeKorgFolder: one zone per key, root = key (plays as recorded), the top
// velocity layer of each key, level as zone attenuation (only cuts). *lost lists what a KMP cannot hold.
inline std::string drumKitAsInstrument(const sound::DrumKit& kit, sound::Instrument* out, std::vector<std::string>* lost) {
    sound::Instrument in;
    in.name = kit.name;
    sound::Layer L;
    L.name = kit.name;
    size_t layers = 0, pans = 0, boosts = 0, cuts = 0, chokes = 0;
    std::map<int, int> index;
    for (const auto& I : kit.instruments) {
        chokes += I.choke != 0;
        for (const auto& k : I.keys) {
            const auto& c = k.layers.back();
            layers += k.layers.size() > 1;
            pans += k.panPct != 0;
            boosts += c.gainDb10 > 0;
            cuts += c.gainDb10 < 0;
            const int root = int(k.key) - k.transpose;
            if (root < 0 || root > 127) return "key " + std::to_string(k.key) + ": transpose leaves the root outside 0-127";
            if (!index.count(c.sample)) { index[c.sample] = int(in.samples.size()); in.samples.push_back(kit.samples[size_t(c.sample)]); }
            L.zones.push_back({k.key, k.key, index[c.sample], uint8_t(root), k.tuneCents, int16_t(c.gainDb10 < 0 ? -c.gainDb10 : 0)});
        }
    }
    std::sort(L.zones.begin(), L.zones.end(), [](const sound::Zone& a, const sound::Zone& b) { return a.lo < b.lo; });
    in.layers.push_back(std::move(L));
    if (const auto e = sound::validate(in); !e.empty()) return e.front();
    lost->push_back("pan (" + std::to_string(pans) + " key(s) panned): a KMP zone has no pan");
    lost->push_back("exclusive groups (" + std::to_string(chokes) + " choking group(s)), group names and drum families: Drum Kit settings, not in a KMP");
    if (layers) lost->push_back(std::to_string(layers) + " key(s) with velocity layers: only the top layer is written");
    if (boosts) lost->push_back(std::to_string(boosts) + " key(s) with a level above 0 dB: written at 0 dB");
    if (cuts) lost->push_back(std::to_string(cuts) + " key level cut(s): a KMP zone level is not written (korg_kmp_writer: 0 dB)");
    *out = std::move(in);
    return "";
}

}  // namespace ai_arranger::media::samplelab

#endif
