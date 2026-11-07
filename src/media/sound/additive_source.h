#ifndef AI_ARRANGER_MEDIA_SOUND_ADDITIVE_SOURCE_H
#define AI_ARRANGER_MEDIA_SOUND_ADDITIVE_SOURCE_H

// Original, synthesized sample source (docs/SOUND_CONVERSION_ROADMAP.md S2): sums sine harmonics,
// so the audio is AIArranger's own and free of third-party rights. Loop-ready by construction: the
// loop holds exactly k periods of the tone, so its last frame continues into its first. The tone's
// frequency is k * rate / loopFrames; the loop length is searched so that it is within a fraction of
// a cent of the requested note, and the remaining error is reported (centsError).

#include "media/sound/instrument.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ai_arranger::media::sound {

struct Tone {
    Sample sample;
    double frequency = 0;     // what was rendered
    double centsError = 0;    // rendered vs equal-tempered (A4 = 440 Hz)
};

inline double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// `harmonics[i]` = amplitude of harmonic i+1 (relative). Harmonics at or above Nyquist are dropped.
inline Tone renderLoopedTone(const std::string& name, uint8_t root, uint32_t rate,
                             const std::vector<double>& harmonics, double attackSec = 0.03,
                             double minLoopSec = 0.25, double maxLoopSec = 0.5, double peak = 0.7) {
    const double f = noteHz(root);
    const uint32_t minL = static_cast<uint32_t>(minLoopSec * rate), maxL = static_cast<uint32_t>(maxLoopSec * rate);
    uint32_t bestL = minL; uint32_t bestK = 1; double bestErr = 1e9;
    for (uint32_t L = minL; L <= maxL; ++L) {
        const double cycles = f * L / rate;
        const uint32_t k = static_cast<uint32_t>(std::lround(cycles));
        if (k == 0) continue;
        const double err = std::fabs(1200.0 * std::log2(static_cast<double>(k) * rate / L / f));
        if (err < bestErr) { bestErr = err; bestL = L; bestK = k; }
    }
    const double fr = static_cast<double>(bestK) * rate / bestL;
    const uint32_t attack = std::max<uint32_t>(16, static_cast<uint32_t>(attackSec * rate));
    const uint32_t tail = 64;   // SF2 wants valid frames after the loop end
    const uint32_t total = attack + bestL + tail;

    std::vector<double> amp;
    for (size_t h = 0; h < harmonics.size(); ++h)
        amp.push_back((h + 1) * fr < rate / 2.0 ? harmonics[h] : 0.0);
    const double kPi = 3.14159265358979323846;
    auto wave = [&](uint32_t n) {   // exactly periodic in bestL: every term completes (h+1)*k cycles
        double v = 0;
        for (size_t h = 0; h < amp.size(); ++h)
            if (amp[h] != 0) v += amp[h] * std::sin(2.0 * kPi * double((h + 1) * bestK) * double(n % bestL) / bestL);
        return v;
    };
    double mx = 0;
    for (uint32_t n = 0; n < bestL; ++n) mx = std::max(mx, std::fabs(wave(n)));
    const double gain = mx > 0 ? peak * 32767.0 / mx : 0;

    Tone t;
    t.frequency = fr;
    t.centsError = 1200.0 * std::log2(fr / f);
    Sample& s = t.sample;
    s.name = name; s.rate = rate; s.root = root;
    s.pcm.resize(total);
    for (uint32_t n = 0; n < total; ++n) {
        const double env = n < attack ? 0.5 - 0.5 * std::cos(kPi * n / attack) : 1.0;   // raised-cosine attack
        s.pcm[n] = static_cast<int16_t>(std::lround(env * gain * wave(n)));
    }
    s.looped = true; s.loopStart = attack; s.loopEnd = attack + bestL;
    return t;
}

// The demo instrument: an original drawbar-style organ, one zone every 4 semitones from C1 to C7,
// two velocity layers (soft = fewer upper harmonics). Mono, 16-bit, 48 kHz.
inline Instrument buildDemoOrgan(double* worstCents = nullptr) {
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const std::vector<double> soft = {1.0, 0.55, 0.25, 0.12};
    const std::vector<double> bright = {1.0, 0.8, 0.6, 0.5, 0.35, 0.3, 0.2, 0.15};
    Instrument in;
    in.name = "AIA Demo Organ";
    double worst = 0;
    for (int l = 0; l < 2; ++l) {
        Layer layer;
        layer.name = l == 0 ? "AIA Organ Soft" : "AIA Organ Bright";
        layer.velLo = l == 0 ? 0 : 90;
        layer.velHi = l == 0 ? 89 : 127;
        for (int root = 24; root <= 96; root += 4) {
            char nm[21];
            std::snprintf(nm, sizeof nm, "AIA Org%c %s%d", l == 0 ? 'S' : 'B', kNames[root % 12], root / 12 - 1);
            Tone t = renderLoopedTone(nm, static_cast<uint8_t>(root), 48000, l == 0 ? soft : bright);
            worst = std::max(worst, std::fabs(t.centsError));
            Zone z;
            z.lo = static_cast<uint8_t>(root == 24 ? 0 : root - 2);
            z.hi = static_cast<uint8_t>(root == 96 ? 127 : root + 1);
            z.root = static_cast<uint8_t>(root);
            z.sample = static_cast<int>(in.samples.size());
            in.samples.push_back(std::move(t.sample));
            layer.zones.push_back(z);
        }
        in.layers.push_back(std::move(layer));
    }
    if (worstCents) *worstCents = worst;
    return in;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_ADDITIVE_SOURCE_H
