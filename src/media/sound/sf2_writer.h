#ifndef AI_ARRANGER_MEDIA_SOUND_SF2_WRITER_H
#define AI_ARRANGER_MEDIA_SOUND_SF2_WRITER_H

// SoundFont 2.04 writer (public specification; docs/SOUND_CONVERSION_ROADMAP.md S3).
// Each Layer becomes one SF2 Instrument (the Pa5X imports SF2 Instruments as multisamples, manual
// v1.4 p.1206/1209); one Preset plays them split by velocity (Low below the switch, High from it),
// so an SF2 player auditions the file the way the Pa5X Sound will be built. Mono 16-bit samples,
// looped continuously when the Sample has a loop. Call validate() first; this writer refuses an
// invalid instrument (empty result).

#include "media/sound/instrument.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace ai_arranger::media::sound {

namespace sf2w {
enum Gen : uint16_t {
    kReleaseVolEnv = 38, kInstrument = 41, kKeyRange = 43, kVelRange = 44, kInitialAttenuation = 48,
    kFineTune = 52, kSampleId = 53, kSampleModes = 54, kOverridingRootKey = 58,
};
struct Buf {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void s16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
    void id(const char* f) { b.insert(b.end(), f, f + 4); }
    void name20(const std::string& s) {
        char n[20] = {};
        std::memcpy(n, s.data(), std::min<size_t>(s.size(), 20));
        b.insert(b.end(), n, n + 20);
    }
    void append(const Buf& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
};
inline Buf chunk(const char* id, const Buf& body) {
    Buf c; c.id(id); c.u32(static_cast<uint32_t>(body.b.size())); c.append(body);
    if (body.b.size() & 1) c.u8(0);
    return c;
}
inline Buf list(const char* type, const Buf& body) { Buf l; l.id(type); l.append(body); return chunk("LIST", l); }
inline Buf zstr(const std::string& s) {   // zero-terminated, even length (INFO strings)
    Buf b; for (char c : s) b.u8(static_cast<uint8_t>(c)); b.u8(0); if (b.b.size() & 1) b.u8(0); return b;
}
struct GenRec { uint16_t op; uint16_t amount; };
inline uint16_t range(uint8_t lo, uint8_t hi) { return uint16_t(lo | (hi << 8)); }
} // namespace sf2w

inline std::vector<uint8_t> writeSf2(const Instrument& in) {
    using namespace sf2w;
    if (!validate(in).empty()) return {};

    // sdta: every sample followed by 46 zero data points (spec 6.1)
    Buf smpl;
    std::vector<uint32_t> start(in.samples.size());
    uint32_t pos = 0;
    for (size_t i = 0; i < in.samples.size(); ++i) {
        start[i] = pos;
        for (int16_t v : in.samples[i].pcm) smpl.s16(v);
        for (int k = 0; k < 46; ++k) smpl.s16(0);
        pos += static_cast<uint32_t>(in.samples[i].pcm.size()) + 46;
    }

    // Instruments: one per layer, one zone per key zone; generator order per spec 8.1.2
    // (keyRange first, sampleID last).
    Buf inst, ibag, imod, igen;
    uint16_t bagN = 0, genN = 0;
    for (const Layer& L : in.layers) {
        inst.name20(L.name); inst.u16(bagN);
        for (const Zone& z : L.zones) {
            ibag.u16(genN); ibag.u16(0); ++bagN;
            const Sample& s = in.samples[static_cast<size_t>(z.sample)];
            std::vector<GenRec> g = {{kKeyRange, range(z.lo, z.hi)}, {kOverridingRootKey, z.root},
                                     {kReleaseVolEnv, static_cast<uint16_t>(int16_t(-3986))}};   // 0.1 s release
            if (z.tuneCents) g.push_back({kFineTune, static_cast<uint16_t>(int16_t(z.tuneCents))});
            if (z.levelCentibels) g.push_back({kInitialAttenuation, static_cast<uint16_t>(z.levelCentibels)});
            if (s.looped) g.push_back({kSampleModes, 1});
            g.push_back({kSampleId, static_cast<uint16_t>(z.sample)});
            for (const auto& r : g) { igen.u16(r.op); igen.u16(r.amount); ++genN; }
        }
    }
    inst.name20("EOI"); inst.u16(bagN);
    ibag.u16(genN); ibag.u16(0);
    for (int k = 0; k < 10; ++k) imod.u8(0);
    for (int k = 0; k < 4; ++k) igen.u8(0);

    // One preset: each instrument in its velocity range.
    Buf phdr, pbag, pmod, pgen;
    uint16_t pg = 0;
    phdr.name20(in.name); phdr.u16(0); phdr.u16(0); phdr.u16(0); phdr.u32(0); phdr.u32(0); phdr.u32(0);
    for (size_t l = 0; l < in.layers.size(); ++l) {
        pbag.u16(pg); pbag.u16(0);
        if (in.layers.size() == 2) {
            const uint8_t lo = l == 0 ? 0 : in.velocitySwitch, hi = l == 0 ? uint8_t(in.velocitySwitch - 1) : 127;
            pgen.u16(kVelRange); pgen.u16(range(lo, hi)); ++pg;
        }
        pgen.u16(kInstrument); pgen.u16(static_cast<uint16_t>(l)); ++pg;
    }
    phdr.name20("EOP"); phdr.u16(0); phdr.u16(0); phdr.u16(static_cast<uint16_t>(in.layers.size()));
    phdr.u32(0); phdr.u32(0); phdr.u32(0);
    pbag.u16(pg); pbag.u16(0);
    for (int k = 0; k < 10; ++k) pmod.u8(0);
    for (int k = 0; k < 4; ++k) pgen.u8(0);

    Buf shdr;
    for (size_t i = 0; i < in.samples.size(); ++i) {
        const Sample& s = in.samples[i];
        shdr.name20(s.name);
        shdr.u32(start[i]);
        shdr.u32(start[i] + static_cast<uint32_t>(s.pcm.size()));
        shdr.u32(start[i] + (s.looped ? s.loopStart : 0));
        shdr.u32(start[i] + (s.looped ? s.loopEnd : 0));
        shdr.u32(s.rate);
        shdr.u8(s.root); shdr.u8(0);       // original pitch, pitch correction (cents)
        shdr.u16(0); shdr.u16(1);          // no link, monoSample
    }
    shdr.name20("EOS");
    for (int k = 0; k < 26; ++k) shdr.u8(0);

    Buf info;
    { Buf v; v.u16(2); v.u16(4); info.append(chunk("ifil", v)); }
    info.append(chunk("isng", zstr("EMU8000")));
    info.append(chunk("INAM", zstr(in.name)));
    info.append(chunk("ISFT", zstr("AIArranger")));
    Buf pdta;
    for (auto [id, b] : std::initializer_list<std::pair<const char*, const Buf*>>{
             {"phdr", &phdr}, {"pbag", &pbag}, {"pmod", &pmod}, {"pgen", &pgen}, {"inst", &inst},
             {"ibag", &ibag}, {"imod", &imod}, {"igen", &igen}, {"shdr", &shdr}})
        pdta.append(chunk(id, *b));
    Buf body; body.id("sfbk");
    body.append(list("INFO", info));
    body.append(list("sdta", chunk("smpl", smpl)));
    body.append(list("pdta", pdta));
    return chunk("RIFF", body).b;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_SF2_WRITER_H
