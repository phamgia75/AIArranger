#ifndef AI_ARRANGER_MEDIA_SOUND_RENDERED_SOURCE_H
#define AI_ARRANGER_MEDIA_SOUND_RENDERED_SOURCE_H

// Multisample an instrument from rendered notes (docs/SOUND_CONVERSION_ROADMAP.md S7). The caller
// supplies render(note, velocity): float audio (mono, or interleaved L/R) of one held note plus its
// release, e.g. from a VST/AU instrument the user is licensed for, rendered offline (apps/macos
// plugin sampler). This builds the S1 model: one zone per sampled root, 1-24 velocity layers
// (one per rendered velocity), mono or stereo samples, leading silence trimmed,
// one gain for the whole instrument (keeps the plugin's own level differences), a short fade-out,
// and a per-sample pitch measurement so a detuned or unpitched result is reported, not hidden.
// One-shot by default; with loopHoldSec the held part is looped (loopSustain) so sustained sounds
// (organ, strings, pads) hold for as long as the key is down.

#include "media/sound/instrument.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace ai_arranger::media::sound {

struct RenderSpec {
    std::string name;                        // instrument name, 1-20 characters
    int lowRoot = 36, highRoot = 96, step = 3;   // sampled roots: lowRoot, lowRoot+step, ... <= highRoot
    std::vector<uint8_t> velocities = {100}; // rendered velocity per layer, ascending (1-24 layers)
    std::vector<uint8_t> splits;             // lowest velocity of layers 2..N; empty = halfway between renders
    uint8_t channels = 1;                    // what render() returns: 1 mono, 2 interleaved stereo
    uint32_t rate = 48000;
    double silenceDb = -60;                  // trim threshold relative to the instrument's peak
    double loopHoldSec = 0;                  // > 0: the renderer releases the note at this time; loop the held part
};

using RenderFn = std::function<std::vector<float>(uint8_t note, uint8_t velocity)>;   // interleaved when stereo

struct RenderReport {
    struct Row { uint8_t note, velocity; size_t frames; double cents; double loopMatch;   // loopMatch NaN = one-shot
                 double releaseSec; };   // note-off to -60 dB (measureRelease); NaN = not looped or not reached
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
    std::vector<double> xw(n);
    for (size_t i = 0; i < n; ++i) xw[i] = x[i] * (0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1)));
    auto mag = [&](double f) {
        const double w = 2 * kPi * f / rate, c = std::cos(w), s = std::sin(w);
        double re = 0, im = 0, cr = 1, ci = 0;
        for (size_t i = 0; i < n; ++i) {
            const double a = xw[i];
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

// Sustain loop for a held note. The loop ends 50 ms before note-off; its start is the position in
// [0.3 s, end - 0.25 s] whose neighbourhood (2 x 256 frames) best matches the end's, by normalised
// correlation. The C frames before the end are crossfaded into the C frames before the start, so the
// jump end -> start continues the waveform exactly. The sample is cut after the loop (plus 64 frames
// copied from the loop start, for interpolation); the release then comes from the player's envelope.
// Returns the correlation (1 = identical neighbourhoods), or -1 when the held part is too short.
inline double loopSustain(Sample& s, size_t noteOff) {
    const size_t R = s.rate, W = 256, minLen = R / 4, startMin = R * 3 / 10, ch = s.channels;
    noteOff = std::min(noteOff, s.frames());
    if (noteOff < R / 20 + startMin + minLen + W) return -1;
    const size_t e = noteOff - R / 20;
    auto mid = [&](size_t f) { double m = 0; for (size_t c = 0; c < ch; ++c) m += s.pcm[f * ch + c]; return m; };
    auto corr = [&](size_t p) {   // on the channel sum, so a stereo loop uses one seam for both channels
        double xy = 0, xx = 0, yy = 0;
        for (size_t k = 0; k < 2 * W; ++k) {
            const double u = mid(p - W + k), v = mid(e - W + k);
            xy += u * v; xx += u * u; yy += v * v;
        }
        return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : -1.0;
    };
    size_t best = startMin;
    double bc = -2;
    for (size_t p = startMin; p + minLen <= e; p += 4)
        if (const double c = corr(p); c > bc) { bc = c; best = p; }
    for (size_t p = best > startMin + 4 ? best - 4 : startMin; p <= best + 4 && p + minLen <= e; ++p)
        if (const double c = corr(p); c > bc) { bc = c; best = p; }
    const size_t C = std::min<size_t>((e - best) / 4, 2048);
    for (size_t k = 0; k < C; ++k) {
        const double w = double(k + 1) / C;   // the last frame before the end becomes the one before the start
        const size_t i = (e - C + k) * ch, j = (best - C + k) * ch;
        for (size_t c = 0; c < ch; ++c)
            s.pcm[i + c] = static_cast<int16_t>(std::lround(s.pcm[i + c] * (1 - w) + s.pcm[j + c] * w));
    }
    s.pcm.resize((e + 64) * ch);
    for (size_t k = 0; k < 64 * ch; ++k) s.pcm[e * ch + k] = s.pcm[best * ch + k];
    s.looped = true; s.loopStart = static_cast<uint32_t>(best); s.loopEnd = static_cast<uint32_t>(e);
    return bc;
}

// Release time of a render (interleaved, ch channels): seconds from note-off frame `off` until the
// 10 ms RMS falls 60 dB below the 10 ms before note-off. Looping cuts the release away and the Pa5X
// does not import envelopes (TX16Wx guide p.85), so this is reported for setting the Amp EG by hand.
// NaN when there is no level before note-off or the render ends first.
inline double measureRelease(const std::vector<float>& x, size_t ch, uint32_t rate, size_t off) {
    const size_t W = rate / 100, total = x.size() / ch;
    auto rms = [&](size_t f) { double e = 0; for (size_t k = f * ch; k < (f + W) * ch; ++k) e += double(x[k]) * x[k]; return std::sqrt(e / (W * ch)); };
    if (off < W || off > total) return NAN;
    const double thr = rms(off - W) * 1e-3;
    if (!(thr > 0)) return NAN;
    for (size_t f = off; f + W <= total; f += W)
        if (rms(f) < thr) return double(f - off) / rate;
    return NAN;
}

inline Instrument buildFromRenders(const RenderSpec& spec, const RenderFn& render, RenderReport* report = nullptr) {
    RenderReport local;
    RenderReport& rep = report ? *report : local;
    rep = {};
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::vector<int> roots;
    for (int r = spec.lowRoot; r <= spec.highRoot && r <= 127; r += std::max(1, spec.step))
        if (r >= 0) roots.push_back(r);
    const size_t N = spec.velocities.size(), ch = spec.channels;
    bool ascending = N >= 1 && N <= kMaxLayers && (ch == 1 || ch == 2) && (spec.splits.empty() || spec.splits.size() + 1 == N) &&
                     spec.velocities.front() >= 1 && spec.velocities.back() <= 127;
    for (size_t l = 1; ascending && l < N; ++l) {
        ascending &= spec.velocities[l - 1] < spec.velocities[l];
        if (!spec.splits.empty())
            ascending &= spec.velocities[l - 1] < spec.splits[l - 1] && spec.splits[l - 1] <= spec.velocities[l] &&
                         (l == 1 || spec.splits[l - 2] < spec.splits[l - 1]);
    }
    if (roots.empty() || !ascending) {
        rep.warnings.push_back("nothing to render: need roots in 0-127, 1-24 ascending velocities, splits between them, 1 or 2 channels");
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
    const std::string pfx = spec.name.substr(0, 12);
    for (size_t l = 0; l < N; ++l) {
        Layer layer;
        // 2 layers keep the Lo/Hi names; more are numbered from the softest
        const std::string tag = N == 1 ? "" : N == 2 ? (l == 0 ? "Lo" : "Hi") : "V" + std::to_string(l + 1);
        layer.name = tag.empty() ? spec.name : spec.name.substr(0, 19 - tag.size()) + " " + tag;   // <= 20 chars
        layer.velLo = static_cast<uint8_t>(l == 0 ? 0 : in.layers.back().velHi + 1);
        layer.velHi = static_cast<uint8_t>(l + 1 == N ? 127
                      : spec.splits.empty() ? (spec.velocities[l] + spec.velocities[l + 1]) / 2 : spec.splits[l] - 1);
        const std::string stag = N == 1 ? "" : N == 2 ? (l == 0 ? " L" : " H") : " " + std::to_string(l + 1);
        for (size_t i = 0; i < roots.size(); ++i) {
            const int r = roots[i];
            const std::vector<float>& x = raw[l * roots.size() + i];
            const size_t total = x.size() / ch;
            auto loud = [&](size_t f) { for (size_t c = 0; c < ch; ++c) if (std::fabs(x[f * ch + c]) >= thr) return true; return false; };
            size_t a = 0, b = total;
            while (a < b && !loud(a)) ++a;
            while (b > a && !loud(b - 1)) --b;
            a -= std::min<size_t>(a, spec.rate / 500);   // keep 2 ms before the onset
            b = std::min<size_t>({b, a + 1048576, total});
            Sample s;
            char nm[32];
            std::snprintf(nm, sizeof nm, "%s %s%d%s", pfx.c_str(), kNames[r % 12], r / 12 - 1, stag.c_str());
            s.name = nm; s.rate = spec.rate; s.root = static_cast<uint8_t>(r); s.channels = static_cast<uint8_t>(ch);
            const size_t n = b - a, fade = std::min<size_t>(n / 4, spec.rate / 200);   // 5 ms fade-out
            s.pcm.assign(std::max<size_t>(n, 1) * ch, 0);
            std::vector<int16_t> mono(std::max<size_t>(n, 1), 0);   // channel average, for the pitch measurement
            for (size_t k = 0; k < n; ++k) {
                const double env = fade && k + fade >= n ? double(n - 1 - k) / fade : 1.0;   // reaches exactly 0
                double m = 0;
                for (size_t c = 0; c < ch; ++c) {
                    s.pcm[k * ch + c] = static_cast<int16_t>(std::lround(std::clamp(x[(a + k) * ch + c] * rep.gain * env, -32767.0, 32767.0)));
                    m += s.pcm[k * ch + c];
                }
                mono[k] = static_cast<int16_t>(std::lround(m / ch));
            }
            const size_t from = std::min<size_t>(n, spec.rate / 5), len = std::min<size_t>(n - from, spec.rate);
            const double f = measurePitch(mono.data() + from, len, spec.rate, 440.0 * std::pow(2.0, (r - 69) / 12.0));
            const double cents = f > 0 ? 1200 * std::log2(f / (440.0 * std::pow(2.0, (r - 69) / 12.0))) : NAN;
            double match = NAN, release = NAN;
            if (spec.loopHoldSec > 0) {
                const size_t off = static_cast<size_t>(spec.loopHoldSec * spec.rate);
                release = measureRelease(x, ch, spec.rate, off);
                match = off > a ? loopSustain(s, off - a) : -1;
                if (!(match >= 0.9)) {
                    char w[112];
                    std::snprintf(w, sizeof w, "%s: %s", s.name.c_str(), match < 0 ? "held part too short to loop (one-shot kept)"
                                  : "loop seam match below 0.9 (noisy or evolving sound; listen before use)");
                    rep.warnings.push_back(w);
                }
            }
            rep.rows.push_back({static_cast<uint8_t>(r), spec.velocities[l], s.frames(), cents, match, release});
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
