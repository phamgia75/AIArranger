#ifndef AI_ARRANGER_UI_SOUND_PLUGIN_SAMPLER_MODEL_H
#define AI_ARRANGER_UI_SOUND_PLUGIN_SAMPLER_MODEL_H

// "Sample Plugin" tab model: the form a musician fills in, checked with the same rules the sampler
// uses (media/sound/rendered_source.h), turned into the aiarranger-plugin-sampler command line, plus
// parsers for what that tool prints (apps/macos/Tools/plugin_sampler.cpp header). The rendering
// itself runs in that separate process; nothing here renders, loads plugins or opens devices.

#include "media/sound/package_writer.h"
#include "media/sound/rendered_source.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace ai_arranger::ui {

struct PluginEntry { std::string format, name, manufacturer, id; };

// `aiarranger-plugin-sampler list` output: FORMAT<TAB>NAME<TAB>MANUFACTURER<TAB>IDENTIFIER per line;
// any other line (JUCE banner, warnings) is ignored.
inline std::vector<PluginEntry> parsePluginList(const std::string& out) {
    std::vector<PluginEntry> v;
    std::istringstream is(out);
    for (std::string line; std::getline(is, line);) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string t; std::getline(ss, t, '\t');) f.push_back(t);
        if (f.size() == 4 && !f[1].empty() && !f[3].empty()) v.push_back({f[0], f[1], f[2], f[3]});
    }
    return v;
}

// The form, as typed. Numbers stay text until checked.
struct SamplerSettings {
    std::string pluginId, name, program;
    std::string low = "36", high = "96", step = "3", velocities = "100", splits, hold = "2.0", release = "1.0";
    bool loop = false, stereo = false;
};

struct SamplerPlan { media::sound::RenderSpec spec; double hold = 0, release = 0; media::sound::RenderEstimate estimate; };

// Checks the form and fills *plan; returns "" or the first problem, in the musician's words.
inline std::string planRender(const SamplerSettings& s, SamplerPlan* plan) {
    namespace snd = media::sound;
    auto integer = [](const std::string& t, int lo, int hi, int* out) {
        if (t.empty() || t.size() > 4 || t.find_first_not_of("0123456789") != std::string::npos) return false;
        *out = std::stoi(t);
        return *out >= lo && *out <= hi;
    };
    auto seconds = [](const std::string& t, double* out) {
        char* end = nullptr;
        *out = std::strtod(t.c_str(), &end);
        return !t.empty() && end && *end == '\0' && std::isfinite(*out);
    };
    auto list = [&](const std::string& t, std::vector<uint8_t>* out) {
        out->clear();
        std::stringstream ss(t);
        for (std::string x; std::getline(ss, x, ',');) {
            while (!x.empty() && x.front() == ' ') x.erase(x.begin());
            while (!x.empty() && x.back() == ' ') x.pop_back();
            int v = 0;
            if (!integer(x, 1, 127, &v)) return false;
            out->push_back(static_cast<uint8_t>(v));
        }
        return true;
    };
    SamplerPlan p;
    if (s.pluginId.empty()) return "choose an instrument (press Find Instruments)";
    if (s.name.empty() || s.name.size() > 20) return "the Sound name needs 1-20 characters";
    int v = 0;
    if (!s.program.empty() && !integer(s.program, 0, 9999, &v)) return "program must be a number (or empty for the plugin's current one)";
    if (!integer(s.low, 0, 127, &p.spec.lowRoot) || !integer(s.high, 0, 127, &p.spec.highRoot) || p.spec.lowRoot > p.spec.highRoot)
        return "lowest and highest note must be 0-127 (60 = C4), lowest first";
    if (!integer(s.step, 1, 127, &p.spec.step)) return "sample every 1-127 semitones";
    if (!list(s.velocities, &p.spec.velocities) || p.spec.velocities.empty()) return "velocities: numbers 1-127, softest first, e.g. 40,80,120";
    if (!s.splits.empty() && !list(s.splits, &p.spec.splits)) return "splits: numbers 1-127, e.g. 60,100 (empty = halfway)";
    p.spec.name = s.name;
    p.spec.channels = s.stereo ? 2 : 1;   // a mono-only plugin is written mono by the sampler, with a warning
    if (!snd::renderSpecValid(p.spec))
        return "velocities must rise (1-24 layers) and each split must lie above the previous velocity and at most the next";
    if (!seconds(s.hold, &p.hold) || !seconds(s.release, &p.release) || !snd::renderTimingValid(p.hold, p.release))
        return "hold must be above 0 s, release 0 s or more, together at most 21 s";
    if (s.loop) p.spec.loopHoldSec = p.hold;
    p.estimate = snd::estimateRender(p.spec, p.hold, p.release);
    if (p.estimate.workingBytes > snd::kMaxRenderWorkingBytes) {
        char b[200];
        std::snprintf(b, sizeof b, "too large: %zu notes need %.1f GB while rendering (limit %.1f GB); sample fewer notes, layers or seconds",
                      p.estimate.samples, p.estimate.workingBytes / 1e9, snd::kMaxRenderWorkingBytes / 1e9);
        return b;
    }
    if (plan) *plan = p;
    return "";
}

inline std::string estimateText(const SamplerPlan& p) {
    char b[400];
    std::snprintf(b, sizeof b,
                  "%zu samples (%zu notes x %zu layers), %.0f s of audio to render. Package audio at most %.1f MB "
                  "(%.2f%% of the 4 GB User Sample memory, before trimming%s); about %.0f MB of memory while rendering, plus the plugin's own.",
                  p.estimate.samples, p.estimate.roots, p.spec.velocities.size(), p.estimate.audioSec, p.estimate.pcmBytesMax / 1e6,
                  100.0 * double(p.estimate.pcmBytesMax) / 4e9, p.spec.loopHoldSec > 0 ? " and loop cutting" : "", p.estimate.workingBytes / 1e6);
    return b;
}

inline std::vector<std::string> samplerArgs(const SamplerSettings& s, const std::string& outDir) {
    auto strip = [](std::string t) { t.erase(std::remove(t.begin(), t.end(), ' '), t.end()); return t; };   // "40, 80" -> "40,80"
    std::vector<std::string> a{"render", "--plugin", s.pluginId, "--name", s.name, "--out", outDir,
                               "--low", s.low, "--high", s.high, "--step", s.step, "--vel", strip(s.velocities),
                               "--hold", s.hold, "--release", s.release};
    if (!s.splits.empty()) { a.push_back("--splits"); a.push_back(strip(s.splits)); }
    if (!s.program.empty()) { a.push_back("--program"); a.push_back(s.program); }
    if (s.loop) { a.push_back("--loop"); a.push_back("on"); }
    if (s.stereo) { a.push_back("--stereo"); a.push_back("on"); }
    return a;
}

// What the sampler has reported so far, fed one output line at a time.
struct SamplerProgress {
    size_t done = 0, total = 0;
    std::string layers, splits, package;
    std::string lastLine;   // the most recent other line: the error text when the tool fails
};
inline void readSamplerLine(const std::string& line, SamplerProgress& p) {
    auto field = [&](const char* key) -> const char* {
        const size_t k = std::strlen(key);
        return line.size() > k && line.compare(0, k, key) == 0 && line[k] == '\t' ? line.c_str() + k + 1 : nullptr;
    };
    if (const char* v = field("progress")) {
        size_t d = 0, t = 0;
        if (std::sscanf(v, "%zu\t%zu", &d, &t) == 2 && d <= t) { p.done = d; p.total = t; }
    } else if (const char* v = field("layers")) p.layers = v;
    else if (const char* v = field("splits")) p.splits = v;
    else if (const char* v = field("package")) p.package = v;
    else if (!line.empty() && line.rfind("JUCE v", 0) != 0) p.lastLine = line;
}

} // namespace ai_arranger::ui

#endif
