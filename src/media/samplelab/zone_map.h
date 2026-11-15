#ifndef AI_ARRANGER_MEDIA_SAMPLELAB_ZONE_MAP_H
#define AI_ARRANGER_MEDIA_SAMPLELAB_ZONE_MAP_H

// Editable key/velocity zone map (P1), one text form for a Sound decoded from ANY source (WAV folder, KRONOS KMP + KSF,
// KSF alone): velocity layers, key split points, root, tune and level per zone, and each sample's loop (start/end,
// zero-crossing snap, crossfade). formatZoneMap() proposes the text from the decoded Sound; applyZoneMap() builds a new
// Sound from the decoded one and the edited text. The audio is the decoded audio; only loop crossfades change samples
// (on the copy). The writers (SF2 + TXPROG + WAV, KMP + KSF) then carry the zones and loops as they already do.
//
//   layer <vel low> <vel high> <name>
//   zone <sample> <low key> <high key> <root> <tune> <level dB> loop <start> <end> [snap <frames>] [xfade <frames>]
//   zone <sample> <low key> <high key> <root> <tune> <level dB> oneshot
// Keys are note names (C-1..G9, C4 = 60) or 0-127; a sample has one root and one loop (zones sharing it must agree); tune is whole cents with its sign (-99..+99); level is 0 or an
// attenuation (-0.1 .. -100.0 dB); loop frames, end exclusive.
// '#' at the start of a line or after a space starts a comment.

#include "media/samplelab/sample_lab.h"
#include "media/sound/ksf_folder_source.h"   // parseNote, noteName
#include <map>
#include <sstream>

namespace ai_arranger::media::samplelab {

inline std::string formatZoneMap(const sound::Instrument& in) {
    std::string o =
        "# Zone map (edit, then press Preview). Layers softest first; their velocity ranges tile 0-127.\n"
        "# layer <vel low> <vel high> <name>\n"
        "# zone <sample> <low> <high> <root> <tune cents> <level dB> loop <start> <end> [snap <frames>] [xfade <frames>] | oneshot\n"
        "# Samples:\n";
    char b[160];
    for (size_t i = 0; i < in.samples.size(); ++i) {
        const auto& s = in.samples[i];
        std::snprintf(b, sizeof b, "#   %zu = %s (%zu frames, %u Hz, %u ch)\n", i, sound::printable(s.name).c_str(), s.frames(), s.rate, unsigned(s.channels));
        o += b;
    }
    for (const auto& L : in.layers) {
        o += "layer " + std::to_string(L.velLo) + " " + std::to_string(L.velHi) + " " + L.name + "\n";
        for (const auto& z : L.zones) {
            const auto& s = in.samples[size_t(z.sample)];
            std::snprintf(b, sizeof b, "zone %d %s %s %s %c%d %.1f ", z.sample, sound::noteName(z.lo).c_str(), sound::noteName(z.hi).c_str(),
                          sound::noteName(z.root).c_str(), z.tuneCents < 0 ? '-' : '+', std::abs(int(z.tuneCents)), -z.levelCentibels / 10.0);
            o += b + (s.looped ? "loop " + std::to_string(s.loopStart) + " " + std::to_string(s.loopEnd) : std::string("oneshot")) + "\n";
        }
    }
    return o;
}

// The Sound `base` re-mapped by `text` into *out (samples no zone uses are left out). "" or the reason, with the line.
// notes (optional): one line per loop operation, for the report.
inline std::string applyZoneMap(const sound::Instrument& base, const std::string& text, sound::Instrument* out, std::string* notes = nullptr) {
    sound::Instrument in;
    in.name = base.name;
    struct LoopSpec { bool looped = false; uint32_t a = 0, b = 0, snap = 0, xfade = 0; int line = 0, root = 0; };
    std::map<int, LoopSpec> loops;   // base sample -> its loop as the map states it
    std::istringstream is(text);
    int n = 0;
    for (std::string line; std::getline(is, line);) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (size_t c = 0; c < line.size(); ++c)
            if (line[c] == '#' && (c == 0 || line[c - 1] == ' ' || line[c - 1] == '\t')) { line.resize(c); break; }
        std::istringstream ls(line);
        std::vector<std::string> t;
        for (std::string w; ls >> w;) t.push_back(w);
        if (t.empty()) continue;
        const std::string at = "zone map line " + std::to_string(n) + ": ";
        auto num = [](const std::string& w, long lo, long hi, long* v) {
            if (w.empty() || w.size() > 9 || w.find_first_not_of("0123456789") != std::string::npos) return false;
            *v = std::stol(w);
            return *v >= lo && *v <= hi;
        };
        if (t[0] == "layer") {
            long lo, hi;
            if (t.size() < 4 || !num(t[1], 0, 127, &lo) || !num(t[2], 0, 127, &hi)) return at + "needs layer <vel low 0-127> <vel high 0-127> <name>";
            size_t p = 0;   // the name is the rest of the line after the third token, spaces included
            for (int i = 0; i < 3; ++i) p = line.find_first_of(" \t", line.find_first_not_of(" \t", p));
            std::string name = line.substr(line.find_first_not_of(" \t", p));
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
            in.layers.push_back({name, {}, uint8_t(lo), uint8_t(hi)});
            continue;
        }
        if (t[0] != "zone") return at + "a line starts with 'layer' or 'zone'";
        if (in.layers.empty()) return at + "a zone needs a layer line above it";
        if (t.size() < 8) return at + "needs zone <sample> <low> <high> <root> <tune> <level dB> loop <start> <end> | oneshot";
        long smp;
        if (!num(t[1], 0, long(base.samples.size()) - 1, &smp)) return at + "sample must be a number from the list (0-" + std::to_string(base.samples.size() - 1) + ")";
        const int lo = sound::parseNote(t[2]), hi = sound::parseNote(t[3]), root = sound::parseNote(t[4]);
        if (lo < 0 || hi < 0 || root < 0) return at + "keys are note names (C-1..G9, C4 = 60) or numbers 0-127";
        if (lo > hi) return at + "low key above high key";
        const std::string& tu = t[5];
        long cents;
        if (tu.size() < 2 || (tu[0] != '+' && tu[0] != '-') || !num(tu.substr(1), 0, 99, &cents)) return at + "tune is whole cents from -99 to +99, with its sign";
        double db;
        char* end = nullptr;
        db = std::strtod(t[6].c_str(), &end);
        if (!end || *end || !(db <= 0 && db >= -100)) return at + "level is 0 or a negative dB value down to -100";
        LoopSpec L;
        L.line = n;
        size_t k = 8;
        if (t[7] == "loop") {
            long a, b;
            if (t.size() < 10 || !num(t[8], 0, 1L << 30, &a) || !num(t[9], 0, 1L << 30, &b)) return at + "loop needs <start> <end> in frames";
            L.looped = true; L.a = uint32_t(a); L.b = uint32_t(b);
            for (k = 10; k + 1 < t.size(); k += 2) {
                long v;
                if (!num(t[k + 1], 1, 1L << 20, &v) || (t[k] != "snap" && t[k] != "xfade")) return at + "after the loop only 'snap <frames>' and 'xfade <frames>'";
                (t[k] == "snap" ? L.snap : L.xfade) = uint32_t(v);
            }
        } else if (t[7] != "oneshot") return at + "end the zone with 'loop <start> <end>' or 'oneshot'";
        if (k < t.size()) return at + "unexpected '" + t[k] + "'";
        L.root = root;
        if (auto [it, fresh] = loops.emplace(int(smp), L); !fresh) {
            const auto& o = it->second;
            if (o.root != root)   // the TXPROG and KMP writers keep one original key per sample
                return at + "sample " + std::to_string(smp) + " has root " + sound::noteName(o.root) + " on line " + std::to_string(o.line) + " (a sample has one root)";
            if (o.looped != L.looped || o.a != L.a || o.b != L.b || o.snap != L.snap || o.xfade != L.xfade)
                return at + "sample " + std::to_string(smp) + " has another loop on line " + std::to_string(o.line) + " (a sample has one loop)";
        }
        in.layers.back().zones.push_back({uint8_t(lo), uint8_t(hi), int(smp), uint8_t(root), int8_t(tu[0] == '-' ? -cents : cents),
                                          int16_t(std::lround(-db * 10))});
    }
    // only the samples the map uses, in the order of the base list
    std::map<int, int> index;
    for (const auto& [b, spec] : loops) {
        sound::Sample s = base.samples[size_t(b)];
        const std::string at = "zone map line " + std::to_string(spec.line) + ": sample " + std::to_string(b) + ": ";
        s.looped = spec.looped;
        s.root = uint8_t(spec.root);
        if (spec.looped) {
            if (!(spec.a < spec.b && spec.b <= s.frames()) || spec.b - spec.a < 32)
                return at + "loop must lie inside its " + std::to_string(s.frames()) + " frames and hold at least 32";
            s.loopStart = spec.a;
            s.loopEnd = spec.b;
            if (spec.snap) if (const auto e = snapLoop(s, spec.snap); !e.empty()) return at + e;
            if (spec.xfade) if (const auto e = crossfadeLoop(s, s.loopStart, s.loopEnd, spec.xfade); !e.empty()) return at + e;
            if (notes && (spec.snap || spec.xfade || spec.a != base.samples[size_t(b)].loopStart || spec.b != base.samples[size_t(b)].loopEnd ||
                          !base.samples[size_t(b)].looped))
                *notes += "  " + s.name + ": loop " + std::to_string(s.loopStart) + "-" + std::to_string(s.loopEnd) + " (asked " + std::to_string(spec.a) + "-" +
                          std::to_string(spec.b) + (spec.snap ? ", snapped to rising zero crossings within " + std::to_string(spec.snap) : std::string()) +
                          (spec.xfade ? ", crossfade " + std::to_string(spec.xfade) + " frames" : std::string()) + ")\n";
        } else if (notes && base.samples[size_t(b)].looped) {
            *notes += "  " + s.name + ": loop removed (one-shot)\n";
        }
        index[b] = int(in.samples.size());
        in.samples.push_back(std::move(s));
    }
    for (auto& L : in.layers) for (auto& z : L.zones) z.sample = index[z.sample];
    if (in.layers.empty()) return "the zone map has no layer";
    if (const auto e = sound::validate(in); !e.empty()) return "zone map: " + e.front();
    *out = std::move(in);
    return "";
}

}  // namespace ai_arranger::media::samplelab

#endif
