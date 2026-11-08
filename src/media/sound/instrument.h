#ifndef AI_ARRANGER_MEDIA_SOUND_INSTRUMENT_H
#define AI_ARRANGER_MEDIA_SOUND_INSTRUMENT_H

// Sample-based instrument model for Sound conversion (docs/SOUND_CONVERSION_ROADMAP.md S1).
// Shaped after the Pa5X Sound chain (docs/research/PA5X_KST_RESOURCE_MODEL.md §3a): a Sound layers
// up to 24 multisamples, one per oscillator, each limited to a velocity zone (TX16Wx guide p.14-16);
// key zones belong to each layer's multisample, one sample (mono or stereo) per zone. Portable, no I/O.

#include <cstdint>
#include <string>
#include <vector>

namespace ai_arranger::media::sound {

struct Sample {
    std::string name;                 // <= 20 chars in SF2
    uint32_t rate = 48000;
    uint8_t  root = 60;               // MIDI note the recording is at
    uint8_t  channels = 1;            // 1 mono, 2 stereo (pcm interleaved L, R)
    std::vector<int16_t> pcm;         // 16-bit
    bool     looped = false;
    uint32_t loopStart = 0, loopEnd = 0;   // frames; loop plays [loopStart, loopEnd)
    size_t frames() const { return channels ? pcm.size() / channels : 0; }
};

struct Zone {
    uint8_t lo = 0, hi = 127;         // key range, inclusive
    int     sample = -1;              // index into Instrument::samples
    uint8_t root = 60;                // zone original key (may differ from the sample's, as on the Pa5X)
    int8_t  tuneCents = 0;
    int16_t levelCentibels = 0;       // attenuation, >= 0 (SF2 initialAttenuation)
};

// One multisample: non-overlapping key zones, played in one velocity zone.
struct Layer {
    std::string name;                 // the imported multisample's name
    std::vector<Zone> zones;
    uint8_t velLo = 0, velHi = 127;   // inclusive; layers tile 0-127 in ascending order
};

struct Instrument {
    std::string name;
    std::vector<Sample> samples;
    std::vector<Layer> layers;        // 1-24, softest first
};

constexpr size_t kMaxLayers = 24;     // multisamples (oscillators) per Pa5X Sound, TX16Wx guide p.14

// Pa5X per-sample limits (manual v1.4 p.1155), shared by Sounds and Drum Kits. Appends one message per violation.
inline void validateSample(const Sample& s, size_t i, std::vector<std::string>& e) {
    auto err = [&](std::string m) { e.push_back(std::move(m)); };
    const std::string n = "sample " + std::to_string(i) + " (" + s.name + ")";
    if (s.rate < 11025 || s.rate > 48000) err(n + ": rate outside 11025-48000 Hz");
    if (s.channels != 1 && s.channels != 2) err(n + ": channels must be 1 (mono) or 2 (stereo)");
    else if (s.pcm.size() % s.channels) err(n + ": stereo audio must hold whole L/R frames");
    if (s.pcm.empty()) err(n + ": no audio");
    if (s.frames() > 1048576) err(n + ": longer than 1 048 576 frames (the Pa5X truncates)");
    if (s.name.empty() || s.name.size() > 20) err(n + ": name must be 1-20 characters");
    if (s.root > 127) err(n + ": root outside 0-127");
    if (s.looped && !(s.loopStart < s.loopEnd && s.loopEnd <= s.frames())) err(n + ": loop outside the sample");
    if (s.looped && s.loopEnd - s.loopStart < 32) err(n + ": loop shorter than 32 frames");
}

// Pa5X limits (DOC): mono or stereo, 8/16-bit, 11 025-48 000 Hz, at most 1 048 576 frames per sample,
// mono or stereo (manual v1.4 p.1155); up to 24 multisamples per Sound in adjacent velocity zones
// with no gap (TX16Wx guide p.14, p.44); SF2 Instruments with overlapping zones may not import
// (manual p.1206). Returns one message per violation; empty = valid.
inline std::vector<std::string> validate(const Instrument& in) {
    std::vector<std::string> e;
    auto err = [&](std::string m) { e.push_back(std::move(m)); };
    if (in.name.empty() || in.name.size() > 20) err("instrument name must be 1-20 characters (SF2 preset name; package folder name)");
    if (in.layers.empty() || in.layers.size() > kMaxLayers) err("an instrument needs 1-24 velocity layers (Pa5X Sound: up to 24 multisamples)");
    for (size_t l = 0; l < in.layers.size(); ++l) {
        const Layer& L = in.layers[l];
        const int expectLo = l == 0 ? 0 : in.layers[l - 1].velHi + 1;
        if (L.velLo != expectLo || L.velLo > L.velHi || L.velHi > 127 || (l + 1 == in.layers.size() && L.velHi != 127))
            err("layer " + std::to_string(l) + ": velocity zones must tile 0-127 in ascending order with no gap or overlap");
    }
    for (size_t i = 0; i < in.samples.size(); ++i) validateSample(in.samples[i], i, e);
    for (size_t l = 0; l < in.layers.size(); ++l) {
        const Layer& L = in.layers[l];
        const std::string n = "layer " + std::to_string(l) + " (" + L.name + ")";
        if (L.zones.empty()) err(n + ": no zones");
        if (L.name.empty() || L.name.size() > 20) err(n + ": name must be 1-20 characters");
        for (size_t a = 0; a < L.zones.size(); ++a) {
            const Zone& z = L.zones[a];
            if (z.lo > z.hi || z.hi > 127) err(n + ": zone " + std::to_string(a) + " key range invalid");
            if (z.sample < 0 || static_cast<size_t>(z.sample) >= in.samples.size()) err(n + ": zone " + std::to_string(a) + " has no sample");
            if (z.root > 127 || z.levelCentibels < 0) err(n + ": zone " + std::to_string(a) + " root or level invalid");
            for (size_t b = a + 1; b < L.zones.size(); ++b)
                if (z.lo <= L.zones[b].hi && L.zones[b].lo <= z.hi)
                    err(n + ": zones " + std::to_string(a) + " and " + std::to_string(b) + " overlap");
        }
    }
    return e;
}

inline uint64_t sampleBytes(const Instrument& in) {   // 16-bit audio, all channels
    uint64_t b = 0;
    for (const auto& s : in.samples) b += s.pcm.size() * 2ull;
    return b;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_INSTRUMENT_H
