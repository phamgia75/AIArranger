#ifndef AI_ARRANGER_MEDIA_SAMPLELAB_SAMPLE_LAB_H
#define AI_ARRANGER_MEDIA_SAMPLELAB_SAMPLE_LAB_H

// Sample Lab analysis core (roadmap E4, Modules 4 and 7): offline, deterministic edits and measurements on the Sound
// pipeline's own Sample model (media/sound/instrument.h). Mono or stereo 16-bit; analysis runs on the mono mix
// (channel average), edits apply to every channel alike. Nothing here runs on the audio or MIDI thread.
//   normalize          peak or RMS target with headroom; reports the gain
//   trimSilence        cuts leading/trailing silence, adds linear fades; never cuts into a loop
//   detectOnsets       energy-rise onsets for drum hits (thresholds in OnsetOptions)
//   findDuplicates     exact (SHA-256 of format + PCM) and near (normalised cross-correlation)
//   zeroCrossingNear / snapLoop / crossfadeLoop   loop helpers

#include "media/sha256.h"
#include "media/sound/instrument.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ai_arranger::media::samplelab {

using sound::Sample;

inline constexpr double kFullScale = 32767.0;   // 0 dBFS; a gain to 0 dBFS never clips

inline double monoAt(const Sample& s, size_t f) {
    double v = 0;
    for (size_t c = 0; c < s.channels; ++c) v += s.pcm[f * s.channels + c];
    return v / s.channels;
}
inline std::vector<double> monoMix(const Sample& s) {
    std::vector<double> m(s.frames());
    for (size_t f = 0; f < m.size(); ++f) m[f] = monoAt(s, f);
    return m;
}
inline double toDb(double linear) { return linear > 0 ? 20 * std::log10(linear) : -120.0; }

struct Level { double peakDb = -120, rmsDb = -120; };   // dBFS over all channels
inline Level measureLevel(const Sample& s) {
    double pk = 0, sq = 0;
    for (int16_t v : s.pcm) { pk = std::max(pk, std::abs(double(v))); sq += double(v) * v; }
    return {toDb(pk / kFullScale), s.pcm.empty() ? -120.0 : toDb(std::sqrt(sq / s.pcm.size()) / kFullScale)};
}

// Gain by `dB`, rounded and clamped to 16 bits.
inline void applyGain(Sample& s, double dB) {
    const double g = std::pow(10.0, dB / 20);
    for (int16_t& v : s.pcm) v = static_cast<int16_t>(std::clamp<long>(std::lround(v * g), -32768, 32767));
}

enum class NormMode { Peak, Rms };
struct GainReport { Level before, after; double gainDb = 0; bool limited = false; bool silent = false; };

// Peak: the peak goes to min(targetDb, -headroomDb). Rms: the RMS goes to targetDb, but the gain is capped so the peak
// stays at or below -headroomDb (limited = true then). A silent sample is left as it is (silent = true).
inline GainReport normalize(Sample& s, NormMode mode, double targetDb, double headroomDb = 1.0) {
    GainReport r;
    r.before = measureLevel(s);
    if (r.before.peakDb <= -120) { r.silent = true; r.after = r.before; return r; }
    const double ceiling = -std::max(0.0, headroomDb) - r.before.peakDb;   // largest gain that keeps the headroom
    double g = mode == NormMode::Peak ? std::min(targetDb, -std::max(0.0, headroomDb)) - r.before.peakDb : targetDb - r.before.rmsDb;
    if (g > ceiling) { g = ceiling; r.limited = mode == NormMode::Rms; }
    r.gainDb = g;
    applyGain(s, g);
    r.after = measureLevel(s);
    return r;
}

struct TrimReport { uint32_t cutStart = 0, cutEnd = 0; bool silent = false; };

// Keeps [first, last] frame whose level on any channel reaches thresholdDb (dBFS), widened by keepFrames of pre-roll,
// then fades in over fadeIn frames and out over fadeOut frames (linear). A loop is never cut: the kept range grows to
// hold it, its points move with the cut, and a looped sample gets no fade-out (it would sit inside the loop or after
// it). A silent sample is left as it is (silent = true).
inline TrimReport trimSilence(Sample& s, double thresholdDb = -60, uint32_t fadeIn = 32, uint32_t fadeOut = 256, uint32_t keepFrames = 0) {
    TrimReport r;
    const size_t n = s.frames(), ch = s.channels;
    const double thr = kFullScale * std::pow(10.0, thresholdDb / 20);
    auto loud = [&](size_t f) { for (size_t c = 0; c < ch; ++c) if (std::abs(double(s.pcm[f * ch + c])) >= thr) return true; return false; };
    size_t a = 0, b = n;
    while (a < n && !loud(a)) ++a;
    if (a == n) { r.silent = true; return r; }
    while (b > a && !loud(b - 1)) --b;
    a = a > keepFrames ? a - keepFrames : 0;
    if (s.looped) { a = std::min<size_t>(a, s.loopStart); b = std::max<size_t>(b, s.loopEnd); }
    r.cutStart = static_cast<uint32_t>(a);
    r.cutEnd = static_cast<uint32_t>(n - b);
    s.pcm = std::vector<int16_t>(s.pcm.begin() + a * ch, s.pcm.begin() + b * ch);
    if (s.looped) { s.loopStart -= r.cutStart; s.loopEnd -= r.cutStart; }
    const size_t len = b - a;
    const size_t fi = std::min<size_t>(fadeIn, s.looped ? std::min<size_t>(len, s.loopStart) : len);
    for (size_t f = 0; f < fi; ++f)
        for (size_t c = 0; c < ch; ++c) s.pcm[f * ch + c] = static_cast<int16_t>(std::lround(s.pcm[f * ch + c] * double(f) / fi));
    const size_t fo = s.looped ? 0 : std::min<size_t>(fadeOut, len);
    for (size_t k = 0; k < fo; ++k) {   // k = 0 is the last frame, silenced
        const size_t f = len - 1 - k;
        for (size_t c = 0; c < ch; ++c) s.pcm[f * ch + c] = static_cast<int16_t>(std::lround(s.pcm[f * ch + c] * double(k) / fo));
    }
    return r;
}

// Onsets by energy rise, tuned for drum hits. Window n covers frames [n*hop, n*hop + window); it is a candidate when its
// energy is at least gateDb (dBFS) and at least riseDb above the mean energy of the 4 * window frames just before it
// (before the start counts as silence, so a hit at frame 0 is found; the longer stretch averages out the level ripple
// inside one period of a low tone, which a window shorter than the period would read as a rise). Candidates must be the largest rise within one window either side
// (the earliest wins a tie), and onsets closer than minGapMs to the previous one are dropped. The onset frame is then
// the first frame from one hop before the window to its end whose mono level reaches attackFraction of that stretch's
// peak. Defaults: 512/128 frames (11.6/2.9 ms at 44.1 kHz), 10 dB rise, -50 dBFS gate, 30 ms gap, 25 % attack.
// Limit: a hit masked by a louder tail (rise < riseDb) is not found; lower riseDb for dense material.
struct OnsetOptions {
    uint32_t window = 512, hop = 128;
    double riseDb = 10, gateDb = -50, minGapMs = 30, attackFraction = 0.25;
};

inline std::vector<uint32_t> detectOnsets(const Sample& s, const OnsetOptions& o = {}) {
    const size_t N = s.frames(), W = o.window, H = o.hop;
    if (!N || !H || W < H) return {};
    const std::vector<double> m = monoMix(s);
    std::vector<double> P(N + 1, 0.0);   // prefix sums of squares
    for (size_t i = 0; i < N; ++i) P[i + 1] = P[i] + m[i] * m[i];
    auto energyDb = [&](size_t a, size_t b) {   // mean square of [a, b) in dBFS; empty = silence
        b = std::min(b, N);
        return b > a ? 10 * std::log10((P[b] - P[a]) / (b - a) / (kFullScale * kFullScale) + 1e-12) : -120.0;
    };
    const size_t windows = (N + H - 1) / H, k = W / H, B = 4 * W;   // B: the "before" stretch, long enough to average a low tone's period
    std::vector<double> e(windows), rise(windows);
    for (size_t n = 0; n < windows; ++n) {
        e[n] = energyDb(n * H, n * H + W);
        rise[n] = e[n] - energyDb(n * H > B ? n * H - B : 0, n * H);
    }
    const size_t gap = static_cast<size_t>(o.minGapMs * s.rate / 1000);
    std::vector<uint32_t> out;
    for (size_t n = 0; n < windows; ++n) {
        if (e[n] < o.gateDb || rise[n] < o.riseDb) continue;
        bool best = true;
        for (size_t j = n > k ? n - k : 0; best && j <= std::min(windows - 1, n + k); ++j)
            best = j == n || (j < n ? rise[j] < rise[n] : rise[j] <= rise[n]);
        if (!best) continue;
        const size_t a = n * H > H ? n * H - H : 0, b = std::min(N, n * H + W);
        double pk = 0;
        for (size_t i = a; i < b; ++i) pk = std::max(pk, std::abs(m[i]));
        size_t at = a;
        while (at < b && std::abs(m[at]) < o.attackFraction * pk) ++at;
        if (!out.empty() && at < out.back() + gap) continue;
        out.push_back(static_cast<uint32_t>(at));
    }
    return out;
}

// SHA-256 over rate, channels and the PCM (little-endian): equal hashes = bit-identical audio in the same format.
inline std::string pcmHash(const Sample& s) {
    Sha256 h;
    const uint8_t fmt[5] = {uint8_t(s.rate), uint8_t(s.rate >> 8), uint8_t(s.rate >> 16), uint8_t(s.rate >> 24), s.channels};
    h.update(fmt, sizeof fmt);
    for (int16_t v : s.pcm) { const uint8_t b[2] = {uint8_t(uint16_t(v)), uint8_t(uint16_t(v) >> 8)}; h.update(b, 2); }
    return h.hex();
}

// Normalised cross-correlation of the mono mixes, each taken from its first frame at 10 % of its own peak, over the
// shorter remainder, best of lags -maxLag..maxLag. Gain-independent: a louder copy still scores 1. 0 when the rates
// differ, the lengths differ by more than 10 %, either is silent, or fewer than 64 frames overlap.
inline double similarity(const Sample& x, const Sample& y, uint32_t maxLag = 16) {
    if (x.rate != y.rate || !x.frames() || !y.frames()) return 0;
    const double lx = double(x.frames()), ly = double(y.frames());
    if (std::abs(lx - ly) > 0.1 * std::max(lx, ly)) return 0;
    const std::vector<double> a = monoMix(x), b = monoMix(y);
    auto start = [](const std::vector<double>& v) {
        double pk = 0;
        for (double d : v) pk = std::max(pk, std::abs(d));
        size_t i = 0;
        while (i < v.size() && std::abs(v[i]) < 0.1 * pk) ++i;
        return pk > 0 ? i : v.size();
    };
    const size_t sa = start(a), sb = start(b);
    if (sa == a.size() || sb == b.size()) return 0;
    double best = 0;
    for (long d = -long(maxLag); d <= long(maxLag); ++d) {
        const long ia = long(sa) + std::max(0L, -d), ib = long(sb) + std::max(0L, d);
        if (ia >= long(a.size()) || ib >= long(b.size())) continue;
        const size_t L = std::min(a.size() - size_t(ia), b.size() - size_t(ib));
        if (L < 64) continue;
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = 0; i < L; ++i) { const double p = a[size_t(ia) + i], q = b[size_t(ib) + i]; ab += p * q; aa += p * p; bb += q * q; }
        if (aa > 0 && bb > 0) best = std::max(best, ab / std::sqrt(aa * bb));
    }
    return best;
}

struct DuplicatePair { size_t a, b; bool exact; double correlation; };

// Every pair (a < b) that is bit-identical (exact, correlation 1) or whose similarity() reaches nearThreshold.
// shortcut: all pairs are compared (O(n^2) correlations, each prefiltered by rate and length); index by an envelope
// fingerprint if folders of thousands of samples become slow.
inline std::vector<DuplicatePair> findDuplicates(const std::vector<Sample>& v, double nearThreshold = 0.98) {
    std::vector<std::string> h;
    for (const Sample& s : v) h.push_back(pcmHash(s));
    std::vector<DuplicatePair> out;
    for (size_t a = 0; a < v.size(); ++a)
        for (size_t b = a + 1; b < v.size(); ++b) {
            if (h[a] == h[b]) { out.push_back({a, b, true, 1.0}); continue; }
            if (const double c = similarity(v[a], v[b]); c >= nearThreshold) out.push_back({a, b, false, c});
        }
    return out;
}

// The rising zero crossing nearest to `frame` within `radius` frames: f with mono(f - 1) < 0 <= mono(f). Returns
// `frame` when there is none. A loop whose start and end both sit on rising crossings has no step at the seam
// beyond the waveform's own slope.
inline uint32_t zeroCrossingNear(const Sample& s, uint32_t frame, uint32_t radius) {
    const size_t N = s.frames();
    auto rising = [&](long f) { return f >= 1 && size_t(f) < N && monoAt(s, size_t(f) - 1) < 0 && monoAt(s, size_t(f)) >= 0; };
    for (long d = 0; d <= long(radius); ++d) {
        if (rising(long(frame) + d)) return uint32_t(long(frame) + d);
        if (rising(long(frame) - d)) return uint32_t(long(frame) - d);
    }
    return frame;
}

// Moves both loop points to their nearest rising zero crossing. "" or the reason nothing was changed.
inline std::string snapLoop(Sample& s, uint32_t radius) {
    if (!s.looped) return "the sample has no loop";
    const uint32_t a = zeroCrossingNear(s, s.loopStart, radius), b = zeroCrossingNear(s, s.loopEnd, radius);
    if (b <= a || b - a < 32 || b > s.frames()) return "snapping would leave a loop shorter than 32 frames";
    s.loopStart = a;
    s.loopEnd = b;
    return "";
}

// Crossfade loop: the last `xfade` frames of the loop are blended linearly into the `xfade` frames just before
// loopStart, reaching them fully at loopEnd - 1, so playback flows from loopEnd - 1 into loopStart exactly as the
// recording flows from loopStart - 1. Linear (not equal-power) so the last frame is exactly the pre-loop frame.
// Sets the loop to [loopStart, loopEnd). "" or the reason nothing was changed.
inline std::string crossfadeLoop(Sample& s, uint32_t loopStart, uint32_t loopEnd, uint32_t xfade) {
    if (!(loopStart < loopEnd && loopEnd <= s.frames()) || loopEnd - loopStart < 32) return "loop outside the sample or shorter than 32 frames";
    if (!xfade || xfade > loopStart || xfade > loopEnd - loopStart) return "crossfade must be 1 frame up to both loopStart and the loop length";
    const size_t ch = s.channels;
    for (size_t i = 0; i < xfade; ++i) {
        const double w = double(i + 1) / xfade;
        const size_t dst = loopEnd - xfade + i, src = loopStart - xfade + i;
        for (size_t c = 0; c < ch; ++c)
            s.pcm[dst * ch + c] = static_cast<int16_t>(std::lround((1 - w) * s.pcm[dst * ch + c] + w * s.pcm[src * ch + c]));
    }
    s.looped = true;
    s.loopStart = loopStart;
    s.loopEnd = loopEnd;
    return "";
}

}  // namespace ai_arranger::media::samplelab

#endif
