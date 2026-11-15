#ifndef AI_ARRANGER_MEDIA_SOUND_DRUM_SOURCE_H
#define AI_ARRANGER_MEDIA_SOUND_DRUM_SOURCE_H

// Original synthesized demo Drum Kit (no recorded or third-party audio): kick and snare with two
// velocity layers, three hi-hat states sharing Exclusive Group 1 (closed/pedal choke open, GUIDE
// p.50), two toms. Deterministic: a fixed-seed noise generator, 48 kHz, 16-bit, mono except the
// stereo open hi-hat. One-shot samples (drums need no loop).

#include "media/sound/drumkit.h"
#include <algorithm>
#include <cmath>

namespace ai_arranger::media::sound {

namespace drumsrc {
struct Noise {   // xorshift32, fixed seed per sample
    uint32_t s;
    double next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s / 2147483648.0 - 1.0; }
};
inline int16_t q(double v) { return static_cast<int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * 32767)); }
}  // namespace drumsrc

inline DrumKit buildDemoKit() {
    using namespace drumsrc;
    constexpr double R = 48000, kPi = 3.14159265358979323846;
    DrumKit kit;
    kit.name = "AIA Demo Kit";
    auto add = [&](const std::string& name, double sec, uint8_t ch, auto fn) {
        Sample s;
        s.name = name; s.rate = 48000; s.root = 60; s.channels = ch;
        const size_t n = static_cast<size_t>(sec * R);
        for (size_t i = 0; i < n; ++i) {
            const double t = i / R, fade = std::min(1.0, (n - 1 - i) / (0.005 * R));   // ends at exactly 0
            for (uint8_t c = 0; c < ch; ++c) s.pcm.push_back(q(fn(t, c) * fade));
        }
        kit.samples.push_back(std::move(s));
        return static_cast<int>(kit.samples.size() - 1);
    };
    auto kick = [&](const char* nm, double amp, double click) {
        double ph = 0;
        return add(nm, 0.6, 1, [=](double t, uint8_t) mutable {
            ph += 2 * kPi * (45 + 110 * std::exp(-t * 30)) / R;
            return amp * (std::sin(ph) * std::exp(-t * 7) + click * std::exp(-t * 400));
        });
    };
    auto snare = [&](const char* nm, double amp, uint32_t seed) {
        Noise z{seed};
        return add(nm, 0.4, 1, [=](double t, uint8_t) mutable {
            return amp * (0.5 * std::sin(2 * kPi * 190 * t) * std::exp(-t * 25) + 0.6 * z.next() * std::exp(-t * 14));
        });
    };
    auto hat = [&](const char* nm, double sec, double decay, uint8_t ch, uint32_t seed) {
        Noise z[2] = {{seed}, {seed * 7 + 1}};
        double prev[2] = {0, 0};
        return add(nm, sec, ch, [=](double t, uint8_t c) mutable {
            const double v = z[c].next(), hp = v - prev[c];   // first difference = bright noise
            prev[c] = v;
            return 0.45 * hp * std::exp(-t * decay);
        });
    };
    auto tom = [&](const char* nm, double f) {
        double ph = 0;
        return add(nm, 0.5, 1, [=](double t, uint8_t) mutable {
            ph += 2 * kPi * f * (1 + 0.3 * std::exp(-t * 20)) / R;
            return 0.8 * std::sin(ph) * std::exp(-t * 8);
        });
    };
    const int k1 = kick("AIA Kick Soft", 0.55, 0.2), k2 = kick("AIA Kick Hard", 0.95, 0.6);
    const int s1 = snare("AIA Snare Soft", 0.5, 11), s2 = snare("AIA Snare Hard", 0.9, 23);
    const int hc = hat("AIA HH Closed", 0.12, 45, 1, 31), hp = hat("AIA HH Pedal", 0.18, 30, 1, 37),
              ho = hat("AIA HH Open", 0.9, 4, 2, 41);
    const int t1 = tom("AIA Tom Hi", 180), t2 = tom("AIA Tom Lo", 110);

    kit.instruments.push_back({"Kick", 0, {{36, "AIA Kick", 'B', {{k1, 0, 0}, {k2, 90, 0}}, 0, 0, 0}}});
    kit.instruments.push_back({"Snare", 0, {{38, "AIA Snare", 'S', {{s1, 0, 0}, {s2, 90, -10}}, 0, 0, 0}}});
    kit.instruments.push_back({"Hi-hat", 1, {{42, "AIA HH Closed", 'H', {{hc, 0, 0}}, 30, 0, 0},
                                             {44, "AIA HH Pedal", 'H', {{hp, 0, 0}}, 30, 0, 0},
                                             {46, "AIA HH Open", 'H', {{ho, 0, -20}}, 30, 0, 0}}});
    kit.instruments.push_back({"Toms", 0, {{48, "AIA Tom Hi", 'T', {{t1, 0, 0}}, 25, 0, 0},
                                           {45, "AIA Tom Lo", 'T', {{t2, 0, 0}}, -25, 0, 0}}});
    return kit;
}

}  // namespace ai_arranger::media::sound

#endif  // AI_ARRANGER_MEDIA_SOUND_DRUM_SOURCE_H
