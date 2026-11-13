#ifndef AI_ARRANGER_MEDIA_SAMPLELAB_OPTIMIZE_H
#define AI_ARRANGER_MEDIA_SAMPLELAB_OPTIMIZE_H

// Sample optimisation into the Pa5X import limits (P1): bit depth and sample rate. The Pa5X imports 8/16-bit mono or
// stereo samples of 11 025-48 000 Hz, at most 1 048 576 frames (manual v1.4 p.1155, sound::validateSample); the Sound
// pipeline is 16-bit. Offline and deterministic, never on the audio thread.
//   to16bit     any PCM WAV (8/16/24/32-bit integer, 32-bit float, also as WAVE_FORMAT_EXTENSIBLE) -> the same file as
//               16-bit PCM. 8-bit is exact (a shift); deeper words get TPDF dither (fixed seed: the same input gives
//               the same bytes). Every other chunk (smpl: root and loop) is copied unchanged.
//   pa5xRate    the rate a sample must be converted to, or its own when it is already inside 11 025-48 000 Hz.
//   resample    windowed-sinc (Blackman, low-pass at 97 % of the lower Nyquist); loop points scale with the rate;
//               refused before allocating when the output would exceed 1 048 576 frames.
//   formatLine  "<rate> Hz <bits>-bit <ch> ch <frames> frames" for the before/after report.

#include "media/sound/wav_folder_source.h"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace ai_arranger::media::samplelab {

struct WavFormat { uint32_t rate = 0; uint16_t bits = 0; bool isFloat = false; uint8_t channels = 0; size_t frames = 0; uint16_t validBits = 0; };

inline std::string formatLine(const WavFormat& f) {
    return std::to_string(f.rate) + " Hz " + std::to_string(f.bits) + "-bit" + (f.isFloat ? " float" : "") +
           (f.validBits && f.validBits < f.bits ? " (" + std::to_string(f.validBits) + " valid)" : "") + " " + std::to_string(f.channels) + " ch " +
           std::to_string(f.frames) + " frames";
}

// Input rates a file may declare (owner gate #52): wider than the Pa5X range so 8 kHz and 96/192 kHz material converts,
// narrow enough that a declared rate cannot make the resampler's output or its filter impractically large.
inline constexpr uint32_t kMinInputRate = 4000, kMaxInputRate = 192000;

// The WAV `b` as 16-bit PCM bytes, its own format in *in. Empty with *err set when it is not a PCM/float WAV.
inline std::vector<uint8_t> to16bit(const std::vector<uint8_t>& b, WavFormat* in, std::string* err) {
    auto u16 = [&](size_t o) { return uint32_t(b[o] | b[o + 1] << 8); };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    auto fail = [&](const std::string& m) { *err = m; return std::vector<uint8_t>{}; };
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(&b[8], "WAVE", 4)) return fail("not a RIFF/WAVE file");
    size_t fmtAt = 0, dataAt = 0, dataLen = 0;
    for (size_t o = 12; o + 8 <= b.size();) {
        const size_t len = u32(o + 4), body = o + 8;
        if (len > b.size() - body) return fail("chunk overruns the file");
        if (!std::memcmp(&b[o], "fmt ", 4)) { if (len < 16) return fail("fmt chunk too short"); fmtAt = body; }
        if (!std::memcmp(&b[o], "data", 4)) { dataAt = body; dataLen = len; }
        o = body + len + (len & 1);
    }
    if (!fmtAt || !dataAt) return fail("missing fmt or data chunk");
    // Samples are decoded by their CONTAINER (wBitsPerSample, nBlockAlign); wValidBitsPerSample (EXTENSIBLE) says how many
    // of its top bits carry audio, the rest are padding and are ignored.
    uint32_t tag = u16(fmtAt);
    const uint32_t ch = u16(fmtAt + 2), rate = u32(fmtAt + 4), align = u16(fmtAt + 12), bits = u16(fmtAt + 14);
    uint32_t valid = bits;
    if (tag == 0xFFFE) {   // WAVE_FORMAT_EXTENSIBLE
        if (u32(fmtAt - 4) < 40) return fail("extensible fmt chunk shorter than 40 bytes");
        if (u16(fmtAt + 16) < 22) return fail("extensible fmt cbSize " + std::to_string(u16(fmtAt + 16)) + " below 22");
        if (u16(fmtAt + 18)) valid = u16(fmtAt + 18);   // 0: not stated, every container bit is valid
        static const uint8_t guidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        tag = u16(fmtAt + 24);
        if (std::memcmp(&b[fmtAt + 26], guidTail, 14) || (tag != 1 && tag != 3)) return fail("unsupported subformat (only PCM or IEEE float)");
    }
    if (ch != 1 && ch != 2) return fail("only mono or stereo is supported");
    if (rate < kMinInputRate || rate > kMaxInputRate)
        return fail("sample rate " + std::to_string(rate) + " Hz outside the accepted input range " + std::to_string(kMinInputRate) + "-" +
                    std::to_string(kMaxInputRate) + " Hz");
    if (!((tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && bits == 32)))
        return fail("only 8/16/24/32-bit PCM or 32-bit float is supported");
    if (align != ch * bits / 8) return fail("block align " + std::to_string(align) + " is not channels * container bytes (" + std::to_string(ch * bits / 8) + ")");
    if (valid > bits || (tag == 3 && valid != bits)) return fail("valid bits " + std::to_string(valid) + " do not fit the " + std::to_string(bits) + "-bit container");
    in->channels = uint8_t(ch);
    in->rate = rate;
    in->bits = uint16_t(bits);
    in->validBits = uint16_t(valid);
    in->isFloat = tag == 3;
    const unsigned bytes = bits / 8;
    if (dataLen % (bytes * ch)) return fail("data chunk does not hold whole frames");
    in->frames = dataLen / (bytes * ch);
    if (u16(fmtAt) == 1 && bits == 16) return b;   // plain 16-bit PCM: already what the pipeline reads
    const uint32_t keep = valid >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - valid)) - 1);   // the valid top bits of a left-justified 32-bit word

    uint32_t seed = 0x9E3779B9u;   // xorshift32: TPDF dither, the same for every run
    auto rnd = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed / 4294967296.0 - 0.5; };
    std::vector<uint8_t> o(b.begin(), b.begin() + 12);
    auto put16 = [&](uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); };
    auto put32 = [&](uint32_t v) { put16(v & 0xFFFF); put16(v >> 16); };
    for (size_t at = 12; at + 8 <= b.size();) {
        const size_t len = u32(at + 4), body = at + 8;
        if (body == fmtAt) {
            o.insert(o.end(), {'f', 'm', 't', ' '});
            put32(16); put16(1); put16(in->channels); put32(in->rate); put32(in->rate * 2 * in->channels); put16(2 * in->channels); put16(16);
        } else if (body == dataAt) {
            o.insert(o.end(), {'d', 'a', 't', 'a'});
            put32(uint32_t(in->frames * in->channels * 2));
            for (size_t i = 0; i < in->frames * in->channels; ++i) {
                const size_t p = dataAt + i * bytes;
                long v;
                if (in->isFloat) {
                    float f;
                    std::memcpy(&f, &b[p], 4);
                    v = std::lround((std::isfinite(f) ? double(f) * 32768.0 : 0.0) + rnd() + rnd());
                } else {   // the container value, left-justified in 32 bits, padding bits cleared
                    const uint32_t w = bits == 8 ? uint32_t(uint8_t(b[p] - 128)) << 24 : bits == 16 ? u16(p) << 16
                                     : bits == 24 ? uint32_t(b[p]) << 8 | uint32_t(b[p + 1]) << 16 | uint32_t(b[p + 2]) << 24 : u32(p);
                    const int32_t x = int32_t(w & keep);
                    v = valid <= 16 ? long(x >> 16)                                 // 16 valid bits or fewer: exact
                                    : std::lround(double(x) / 65536.0 + rnd() + rnd());   // deeper: TPDF dither to 16 bits
                }
                put16(uint16_t(int16_t(std::clamp(v, -32768L, 32767L))));
            }
        } else {
            o.insert(o.end(), b.begin() + long(at), b.begin() + long(body + len));
            if (len & 1) o.push_back(0);
        }
        at = body + len + (len & 1);
    }
    const uint32_t riff = uint32_t(o.size() - 8);
    std::memcpy(&o[4], &riff, 4);   // little-endian hosts only (macOS arm64/x86_64)
    return o;
}

// Any PCM WAV read into the 16-bit Sample; *fmt receives the file's own format. "" or the reason.
inline std::string readWavAny(const std::vector<uint8_t>& b, sound::Sample* s, WavFormat* fmt, bool* noSmpl = nullptr) {
    std::string err;
    const auto w = to16bit(b, fmt, &err);
    if (w.empty()) return err;
    return sound::readWav(w, s, noSmpl);
}

inline uint32_t pa5xRate(uint32_t r) {
    if (r > 48000) return r % 44100 == 0 ? 44100 : 48000;
    return r < 11025 ? 11025 : r;
}

// Checked before anything is allocated (owner gate #52): both rates inside the accepted input range, the output length
// computed in overflow-checked 64-bit integers, then the Pa5X frame limit (MAN p.1155) and the memory budget of the
// output PCM. A declared rate is untrusted (1 Hz, 1 000 000 frames would ask for 11 025 000 000 frames).
// "" or the reason; s is unchanged then.
inline constexpr uint64_t kMaxFrames = 1048576;
inline constexpr uint64_t kResampleBudgetBytes = kMaxFrames * 2 * 2;   // stereo 16-bit at the frame limit: 4 MiB
inline std::string resample(sound::Sample& s, uint32_t to) {
    if (to == s.rate || !s.channels) return "";
    // The SOURCE loop must lie inside the source audio: an invalid one is refused here, never clamped or dropped (Codex r2).
    if (s.looped && !(s.loopStart < s.loopEnd && s.loopEnd <= s.frames()))
        return "smpl loop " + std::to_string(s.loopStart) + "-" + std::to_string(s.loopEnd) + " lies outside the " + std::to_string(s.frames()) + " frames";
    for (uint32_t r : {s.rate, to})
        if (r < kMinInputRate || r > kMaxInputRate)
            return "sample rate " + std::to_string(r) + " Hz outside " + std::to_string(kMinInputRate) + "-" + std::to_string(kMaxInputRate) + " Hz";
    const uint64_t n64 = s.frames();
    if (n64 > UINT64_MAX / to) return "resampling: frame count overflows";
    const uint64_t outFrames = (n64 * to + s.rate / 2) / s.rate;   // rounded half up, as llround on these positive values
    if (outFrames > kMaxFrames)
        return "resampling " + std::to_string(s.rate) + " Hz to " + std::to_string(to) + " Hz would make " + std::to_string(outFrames) +
               " frames, over the Pa5X limit of 1048576";
    if (outFrames * s.channels * 2 > kResampleBudgetBytes) return "resampling: output over the " + std::to_string(kResampleBudgetBytes) + "-byte budget";
    const double ratio = double(to) / s.rate, fc = 0.5 * std::min(1.0, ratio) * 0.97;   // cut-off in input cycles/frame
    const double want = double(outFrames);
    // the loop scaled with the rate (rounded); a loop with no frames left is refused, before any work
    const uint32_t loopA = uint32_t(std::llround(s.loopStart * ratio)), loopB = uint32_t(std::llround(s.loopEnd * ratio));
    if (s.looped && !(loopA < loopB && loopB <= want))
        return "smpl loop " + std::to_string(s.loopStart) + "-" + std::to_string(s.loopEnd) + " has no frames left at " + std::to_string(to) + " Hz";
    const long H = long(std::ceil(16 / fc));                                               // half width: 16 zero crossings
    const size_t n = s.frames(), m = size_t(want), ch = s.channels;
    const double kPi = 3.14159265358979323846;
    std::vector<int16_t> out(m * ch);
    for (size_t j = 0; j < m; ++j) {
        const double t = j / ratio;
        const long c = long(std::floor(t));
        double acc[2] = {0, 0};
        for (long k = c - H + 1; k <= c + H; ++k) {
            if (k < 0 || size_t(k) >= n) continue;
            const double d = t - k, x = 2 * fc * d;
            const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double w = 0.42 + 0.5 * std::cos(kPi * d / H) + 0.08 * std::cos(2 * kPi * d / H);   // Blackman over [-H, H]
            const double h = 2 * fc * sinc * w;
            for (size_t q = 0; q < ch; ++q) acc[q] += h * s.pcm[size_t(k) * ch + q];
        }
        for (size_t q = 0; q < ch; ++q) out[j * ch + q] = int16_t(std::clamp(std::lround(acc[q]), -32768L, 32767L));
    }
    if (s.looped) { s.loopStart = loopA; s.loopEnd = loopB; }
    s.pcm = std::move(out);
    s.rate = to;
    return "";
}

}  // namespace ai_arranger::media::samplelab

#endif
