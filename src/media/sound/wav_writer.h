#ifndef AI_ARRANGER_MEDIA_SOUND_WAV_WRITER_H
#define AI_ARRANGER_MEDIA_SOUND_WAV_WRITER_H

// Mono 16-bit PCM WAV with a standard 'smpl' chunk (MIDI unity note + one forward loop), so the
// root and loop survive in samplers that read it (e.g. TX16Wx). The Pa5X loads WAV samples
// (manual p.1155); whether it reads the 'smpl' loop is UNKNOWN (hardware validation pending).

#include "media/sound/instrument.h"
#include <cstring>

namespace ai_arranger::media::sound {

inline std::vector<uint8_t> writeWav(const Sample& s) {
    std::vector<uint8_t> b;
    auto u16 = [&](uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    auto id = [&](const char* f) { b.insert(b.end(), f, f + 4); };
    const uint32_t data = static_cast<uint32_t>(s.pcm.size() * 2);
    const uint32_t smpl = s.looped ? 36 + 24 : 36;
    id("RIFF"); u32(4 + (8 + 16) + (8 + data) + (8 + smpl)); id("WAVE");
    id("fmt "); u32(16); u16(1); u16(1); u32(s.rate); u32(s.rate * 2); u16(2); u16(16);
    id("data"); u32(data);
    for (int16_t v : s.pcm) u16(static_cast<uint16_t>(v));
    id("smpl"); u32(smpl);
    u32(0); u32(0); u32(s.rate ? 1000000000u / s.rate : 0); u32(s.root); u32(0); u32(0); u32(0);
    u32(s.looped ? 1 : 0); u32(0);
    if (s.looped) { u32(0); u32(0); u32(s.loopStart); u32(s.loopEnd - 1); u32(0); u32(0); }   // end is inclusive in 'smpl'
    return b;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_WAV_WRITER_H
