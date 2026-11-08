#ifndef AI_ARRANGER_MEDIA_SOUND_DRUMKIT_H
#define AI_ARRANGER_MEDIA_SOUND_DRUMKIT_H

// Pa5X Standard Drum Kit model (Korg guide "Making Pa5X Sounds and Drum Kits with TX16Wx", GUIDE):
// each key holds one Drum Instrument with up to 16 velocity layers (GUIDE pp.18-20, 78); keys are
// grouped into Drum Instruments (TX16Wx Groups) that may share an Exclusive Group 1-8, e.g. hi-hat
// open/closed/pedal choking each other (GUIDE pp.43, 46, 50); a family prefix (B_, S_, H_, ...) on
// the matrix name sets the Pa5X Drum Family (GUIDE p.80). Round-robins (matrix columns, GUIDE p.79)
// and Ambience kits (a second "_A" Program, GUIDE p.23) are not modelled.
// Portable, no I/O.

#include "media/sound/instrument.h"
#include <set>

namespace ai_arranger::media::sound {

constexpr size_t kMaxDrumLayers = 16;   // velocity layers per key (GUIDE pp.76, 78)
constexpr int kMaxChoke = 8;            // Exclusive Groups 1-8 (GUIDE p.50); 0 = none

// Drum Family by matrix-name prefix (GUIDE p.80); '\0' = no family prefix.
inline const char* drumFamilyName(char f) {
    switch (f) {
        case 'B': return "Bass Drums"; case 'S': return "Snare Drums"; case 'H': return "HiHat"; case 'T': return "Toms";
        case 'C': return "Cymbals"; case 'L': return "Latin Percussions"; case 'E': return "Ethnic Percussions";
        case 'X': return "Special FX"; default: return nullptr;
    }
}

struct DrumCell {                 // one velocity layer of a key (a matrix row)
    int     sample = -1;          // index into DrumKit::samples
    uint8_t velLo = 0;            // lowest velocity of this layer; layers ascend, the first starts at 0
    int16_t gainDb10 = 0;         // level in 0.1 dB (matrix cell attenuation); Pa5X allows at most +6 dB (GUIDE p.81)
};

struct DrumKey {
    uint8_t key = 36;             // MIDI note 0-127
    std::string name;             // matrix name without the family prefix -> Pa5X drum sample name (GUIDE p.25)
    char    family = 0;           // B S H T C L E X, or 0
    std::vector<DrumCell> layers; // 1-16, softest first
    int8_t  panPct = 0;           // -100..100 -> Voice Mixer Pan (GUIDE p.25)
    int8_t  transpose = 0;        // semitones; Pa5X range +-12 (GUIDE p.25)
    int8_t  tuneCents = 0;        // TX16Wx range +-50 cents (GUIDE p.25)
};

struct DrumInstrument {           // a TX16Wx Group
    std::string name;
    uint8_t choke = 0;            // Exclusive Group 1-8, 0 = none
    std::vector<DrumKey> keys;
};

struct DrumKit {
    std::string name;             // Performance/Program name -> Pa5X Drum Kit name
    std::vector<Sample> samples;
    std::vector<DrumInstrument> instruments;
};

inline std::string matrixName(const DrumKey& k) { return k.family ? std::string(1, k.family) + "_" + k.name : k.name; }

// One message per violation; empty = valid.
inline std::vector<std::string> validate(const DrumKit& kit) {
    std::vector<std::string> e;
    auto err = [&](std::string m) { e.push_back(std::move(m)); };
    if (kit.name.empty() || kit.name.size() > 20) err("kit name must be 1-20 characters");
    if (kit.instruments.empty()) err("a kit needs at least one drum instrument");
    for (size_t i = 0; i < kit.samples.size(); ++i) validateSample(kit.samples[i], i, e);
    std::set<int> keys, used;
    std::set<std::string> matrices;
    for (size_t g = 0; g < kit.instruments.size(); ++g) {
        const DrumInstrument& I = kit.instruments[g];
        const std::string gn = "instrument " + std::to_string(g) + " (" + I.name + ")";
        if (I.name.empty() || I.name.size() > 20) err(gn + ": name must be 1-20 characters");
        if (I.choke > kMaxChoke) err(gn + ": exclusive group must be 0-8");
        if (I.keys.empty()) err(gn + ": no keys");
        for (const DrumKey& k : I.keys) {
            const std::string kn = gn + " key " + std::to_string(k.key) + " (" + k.name + ")";
            if (k.key > 127) err(kn + ": key outside 0-127");
            if (!keys.insert(k.key).second) err(kn + ": key already assigned (one drum instrument per key)");
            if (k.name.empty() || matrixName(k).size() > 20) err(kn + ": name (with family prefix) must be 1-20 characters");
            if (k.family && !drumFamilyName(k.family)) err(kn + ": unknown drum family prefix");
            if (!matrices.insert(matrixName(k)).second) err(kn + ": matrix name used twice");
            if (k.panPct < -100 || k.panPct > 100) err(kn + ": pan outside -100..100%");
            if (k.transpose < -12 || k.transpose > 12) err(kn + ": transpose outside the Pa5X's +-12 semitones");
            if (k.tuneCents < -50 || k.tuneCents > 50) err(kn + ": tune outside TX16Wx's +-50 cents");
            if (k.layers.empty() || k.layers.size() > kMaxDrumLayers) err(kn + ": needs 1-16 velocity layers");
            for (size_t l = 0; l < k.layers.size(); ++l) {
                const DrumCell& c = k.layers[l];
                if (l == 0 ? c.velLo != 0 : (c.velLo <= k.layers[l - 1].velLo || c.velLo > 127))
                    err(kn + ": layer " + std::to_string(l) + " velocities must start at 0 and ascend");
                if (c.sample < 0 || static_cast<size_t>(c.sample) >= kit.samples.size()) err(kn + ": layer " + std::to_string(l) + " has no sample");
                else used.insert(c.sample);
                if (c.gainDb10 > 60) err(kn + ": layer " + std::to_string(l) + " level above the Pa5X's +6 dB");
            }
        }
    }
    if (used.size() != kit.samples.size()) err("every sample must be used by a key (no orphan files)");
    return e;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_DRUMKIT_H
