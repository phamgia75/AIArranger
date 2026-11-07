#ifndef AI_ARRANGER_MEDIA_SOUND_INSTRUMENT_H
#define AI_ARRANGER_MEDIA_SOUND_INSTRUMENT_H

// Sample-based instrument model for Sound conversion (docs/SOUND_CONVERSION_ROADMAP.md S1).
// Shaped after the Pa5X Sound chain (docs/research/PA5X_KST_RESOURCE_MODEL.md §3a): velocity layers
// belong to the Sound (an oscillator's Low and High layer, one velocity switch), key zones belong
// to each layer's multisample, one sample per zone. Portable, no I/O.

#include <cstdint>
#include <string>
#include <vector>

namespace ai_arranger::media::sound {

struct Sample {
    std::string name;                 // <= 20 chars in SF2
    uint32_t rate = 48000;
    uint8_t  root = 60;               // MIDI note the recording is at
    std::vector<int16_t> pcm;         // mono, 16-bit
    bool     looped = false;
    uint32_t loopStart = 0, loopEnd = 0;   // frames; loop plays [loopStart, loopEnd)
};

struct Zone {
    uint8_t lo = 0, hi = 127;         // key range, inclusive
    int     sample = -1;              // index into Instrument::samples
    uint8_t root = 60;                // zone original key (may differ from the sample's, as on the Pa5X)
    int8_t  tuneCents = 0;
    int16_t levelCentibels = 0;       // attenuation, >= 0 (SF2 initialAttenuation)
};

// One multisample: non-overlapping key zones.
struct Layer {
    std::string name;                 // the imported multisample's name
    std::vector<Zone> zones;
};

struct Instrument {
    std::string name;
    std::vector<Sample> samples;
    std::vector<Layer> layers;        // 1 (no switch) or 2: [0] Low, [1] High
    uint8_t velocitySwitch = 64;      // with 2 layers: velocity >= this plays the High layer
};

// Pa5X User Manual v1.4 limits (DOC): 8/16-bit, 11 025-48 000 Hz, at most 1 048 576 frames per
// sample (p.1155); an oscillator has at most a Low and a High layer (p.1076-1077); SF2 Instruments
// with overlapping zones may not import (p.1206). Returns one message per violation; empty = valid.
inline std::vector<std::string> validate(const Instrument& in) {
    std::vector<std::string> e;
    auto err = [&](std::string m) { e.push_back(std::move(m)); };
    if (in.name.empty() || in.name.size() > 20) err("instrument name must be 1-20 characters (SF2 preset name; package folder name)");
    if (in.layers.empty() || in.layers.size() > 2) err("an instrument needs 1 or 2 velocity layers (Pa5X oscillator Low/High)");
    if (in.layers.size() == 2 && (in.velocitySwitch < 2 || in.velocitySwitch > 127)) err("velocity switch must be 2..127");
    for (size_t i = 0; i < in.samples.size(); ++i) {
        const Sample& s = in.samples[i];
        const std::string n = "sample " + std::to_string(i) + " (" + s.name + ")";
        if (s.rate < 11025 || s.rate > 48000) err(n + ": rate outside 11025-48000 Hz");
        if (s.pcm.empty()) err(n + ": no audio");
        if (s.pcm.size() > 1048576) err(n + ": longer than 1 048 576 frames (the Pa5X truncates)");
        if (s.name.empty() || s.name.size() > 20) err(n + ": name must be 1-20 characters");
        if (s.root > 127) err(n + ": root outside 0-127");
        if (s.looped && !(s.loopStart < s.loopEnd && s.loopEnd <= s.pcm.size())) err(n + ": loop outside the sample");
        if (s.looped && s.loopEnd - s.loopStart < 32) err(n + ": loop shorter than 32 frames");
    }
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

inline uint64_t sampleBytes(const Instrument& in) {   // 16-bit mono audio only
    uint64_t b = 0;
    for (const auto& s : in.samples) b += s.pcm.size() * 2ull;
    return b;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_INSTRUMENT_H
