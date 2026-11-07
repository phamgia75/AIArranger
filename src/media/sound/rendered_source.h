#ifndef AI_ARRANGER_MEDIA_SOUND_RENDERED_SOURCE_H
#define AI_ARRANGER_MEDIA_SOUND_RENDERED_SOURCE_H

// Multisample an instrument from rendered notes (docs/SOUND_CONVERSION_ROADMAP.md S7). The caller
// supplies render(note, velocity): mono float audio of one held note plus its release, e.g. from a
// VST/AU instrument the user is licensed for, rendered offline (apps/macos plugin sampler). This
// builds the S1 model: one zone per sampled root, 1 or 2 velocity layers, leading silence trimmed,
// one gain for the whole instrument (keeps the plugin's own level differences), a short fade-out,
// and a per-sample pitch measurement so a detuned or unpitched result is reported, not hidden.
// shortcut: samples are one-shots (no loop search); fine up to 1 048 576 frames (21.8 s at 48 kHz),
// add loop detection when sustained sounds must hold longer than the rendered note.

#include "media/sound/instrument.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace ai_arranger::media::sound {

struct RenderSpec {
    std::string name;                        // instrument name, 1-20 characters
    int lowRoot = 36, highRoot = 96, step = 3;   // sampled roots: lowRoot, lowRoot+step, ... <= highRoot
    std::vector<uint8_t> velocities = {100}; // one per layer: [Low] or [Low, High]
    uint8_t velocitySwitch = 90;             // with 2 layers
    uint32_t rate = 48000;
    double silenceDb = -60;                  // trim threshold relative to the instrument's peak
};

using RenderFn = std::function<std::vector<float>(uint8_t note, uint8_t velocity)>;

struct RenderReport {
    struct Row { uint8_t note, velocity; size_t frames; double cents; };
    std::vector<Row> rows;
    std::vector<std::string> warnings;       // empty spec, silent render, detuned/unpitched samples
    double gain = 0;
};

// Fundamental of x near `expectHz` (+-50 cents): the highest Hann-windowed DTFT magnitude on a
// 1-cent grid, refined by golden-section search between its neighbours. The grid matters: real
// instruments (e.g. detuned unison strings) have several close peaks, and a golden search over the
// whole range can settle on a side peak. Returns 0 if the window is shorter than 2048 frames.
inline double measurePitch(const int16_t* x, size_t n, uint32_t rate, double expectHz) {
    if (n < 2048) return 0;
    const double kPi = 3.14159265358979323846;
    auto mag = [&](double f) {
        const double w = 2 * kPi * f / rate, c = std::cos(w), s = std::sin(w);
        double re = 0, im = 0, cr = 1, ci = 0;
        for (size_t i = 0; i < n; ++i) {
            const double a = x[i] * (0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1)));
            re += a * cr; im -= a * ci;
            const double t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
        }
        return std::hypot(re, im);
    };
    int best = -50;
    double bestMag = -1;
    for (int c = -50; c <= 50; ++c)
        if (const double m = mag(expectHz * std::pow(2.0, c / 1200.0)); m > bestMag) { bestMag = m; best = c; }
    double lo = expectHz * std::pow(2.0, (best - 1) / 1200.0), hi = expectHz * std::pow(2.0, (best + 1) / 1200.0);
    const double g = (std::sqrt(5.0) - 1) / 2;
    for (int k = 0; k < 20; ++k) {
        const double m1 = hi - g * (hi - lo), m2 = lo + g * (hi - lo);
        if (mag(m1) > mag(m2)) hi = m2; else lo = m1;
    }
    return (lo + hi) / 2;
}

inline Instrument buildFromRenders(const RenderSpec& spec, const RenderFn& render, RenderReport* report = nullptr) {
    RenderReport local;
    RenderReport& rep = report ? *report : local;
    rep = {};
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::vector<int> roots;
    for (int r = spec.lowRoot; r <= spec.highRoot && r <= 127; r += std::max(1, spec.step))
        if (r >= 0) roots.push_back(r);
    if (roots.empty() || spec.velocities.empty() || spec.velocities.size() > 2) {
        rep.warnings.push_back("nothing to render: need roots in 0-127 and 1 or 2 velocities");
        return {};
    }

    // Render everything first: the gain is shared by all samples.
    std::vector<std::vector<float>> raw;
    float peak = 0;
    for (uint8_t v : spec.velocities)
        for (int r : roots) {
            raw.push_back(render(static_cast<uint8_t>(r), v));
            for (float s : raw.back()) peak = std::max(peak, std::fabs(s));
        }
    if (!(peak > 0) || !std::isfinite(peak)) {
        rep.warnings.push_back("the instrument rendered silence (or non-finite audio)");
        return {};
    }
    rep.gain = 0.9 * 32767.0 / peak;
    const float thr = peak * static_cast<float>(std::pow(10.0, spec.silenceDb / 20));

    Instrument in;
    in.name = spec.name;
    in.velocitySwitch = spec.velocitySwitch;
    const std::string pfx = spec.name.substr(0, 12);
    for (size_t l = 0; l < spec.velocities.size(); ++l) {
        Layer layer;
        layer.name = spec.name.substr(0, 17) + (spec.velocities.size() == 1 ? "" : l == 0 ? " Lo" : " Hi");
        for (size_t i = 0; i < roots.size(); ++i) {
            const int r = roots[i];
            const std::vector<float>& x = raw[l * roots.size() + i];
            size_t a = 0, b = x.size();
            while (a < b && std::fabs(x[a]) < thr) ++a;
            while (b > a && std::fabs(x[b - 1]) < thr) --b;
            a -= std::min<size_t>(a, spec.rate / 500);   // keep 2 ms before the onset
            b = std::min<size_t>({b, a + 1048576, x.size()});
            Sample s;
            char nm[32];
            std::snprintf(nm, sizeof nm, "%s %s%d%s", pfx.c_str(), kNames[r % 12], r / 12 - 1,
                          spec.velocities.size() == 1 ? "" : l == 0 ? " L" : " H");
            s.name = nm; s.rate = spec.rate; s.root = static_cast<uint8_t>(r);
            const size_t n = b - a, fade = std::min<size_t>(n / 4, spec.rate / 200);   // 5 ms fade-out
            s.pcm.resize(std::max<size_t>(n, 1));
            for (size_t k = 0; k < n; ++k) {
                const double env = fade && k + fade >= n ? double(n - 1 - k) / fade : 1.0;   // reaches exactly 0
                s.pcm[k] = static_cast<int16_t>(std::lround(std::clamp(x[a + k] * rep.gain * env, -32767.0, 32767.0)));
            }
            const size_t from = std::min<size_t>(n, spec.rate / 5), len = std::min<size_t>(n - from, spec.rate);
            const double f = measurePitch(s.pcm.data() + from, len, spec.rate, 440.0 * std::pow(2.0, (r - 69) / 12.0));
            const double cents = f > 0 ? 1200 * std::log2(f / (440.0 * std::pow(2.0, (r - 69) / 12.0))) : NAN;
            rep.rows.push_back({static_cast<uint8_t>(r), spec.velocities[l], n, cents});
            if (!(std::fabs(cents) <= 10)) {
                char w[96];
                std::snprintf(w, sizeof w, "%s: pitch %+.1f cents from %s%d (detuned, unpitched or too short)",
                              s.name.c_str(), cents, kNames[r % 12], r / 12 - 1);
                rep.warnings.push_back(w);
            }
            Zone z;
            z.lo = static_cast<uint8_t>(i == 0 ? 0 : layer.zones.back().hi + 1);
            z.hi = static_cast<uint8_t>(i + 1 == roots.size() ? 127 : r + (roots[i + 1] - r - 1) / 2);
            z.root = static_cast<uint8_t>(r);
            z.sample = static_cast<int>(in.samples.size());
            in.samples.push_back(std::move(s));
            layer.zones.push_back(z);
        }
        in.layers.push_back(std::move(layer));
    }
    return in;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_RENDERED_SOURCE_H
