// Sound conversion S7 core: multisampling rendered notes (media/sound/rendered_source.h) with a
// synthetic renderer standing in for a plugin: known detune, leading silence, velocity-dependent
// level. SOFTWARE evidence; the plugin host itself is macOS-only (apps/macos).

#include "media/sound/rendered_source.h"
#include "media/sound/sf2_writer.h"
#include "media/sf2_check.h"
#include <cmath>
#include <cstdio>

using namespace ai_arranger::media;
using namespace ai_arranger::media::sound;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

// 0.1 s silence, 1.5 s tone (3 harmonics) at the note +detune cents, 0.3 s release, 0.2 s silence.
static RenderFn synth(double detuneCents, int* calls = nullptr) {
    return [=](uint8_t note, uint8_t vel) {
        if (calls) ++*calls;
        const double f = 440.0 * std::pow(2.0, (note - 69) / 12.0 + detuneCents / 1200.0), kPi = 3.14159265358979323846;
        std::vector<float> x(static_cast<size_t>(2.1 * 48000), 0.f);
        for (size_t n = 4800; n < static_cast<size_t>(1.9 * 48000); ++n) {
            const double t = (n - 4800) / 48000.0;
            const double env = std::min(1.0, t / 0.005) * (t < 1.5 ? 1.0 : std::exp(-(t - 1.5) * 20));
            x[n] = static_cast<float>(vel / 127.0 * 0.5 * env *
                   (std::sin(2 * kPi * f * t) + 0.5 * std::sin(4 * kPi * f * t) + 0.25 * std::sin(6 * kPi * f * t)));
        }
        return x;
    };
}

int main() {
    std::printf("Test: rendered-note multisampling (S7 core)\n");
    RenderSpec spec;
    spec.name = "Test Plugin";
    spec.lowRoot = 36; spec.highRoot = 84; spec.step = 6;
    spec.velocities = {64, 120};
    spec.splits = {96};
    int calls = 0;
    RenderReport rep;
    const Instrument in = buildFromRenders(spec, synth(3.0, &calls), &rep);

    TEST("valid against the Pa5X limits; 9 roots x 2 layers rendered once each", validate(in).empty() && calls == 18 && in.samples.size() == 18);
    bool tiles = in.layers.size() == 2;
    for (const auto& L : in.layers) {
        int next = 0;
        for (const auto& z : L.zones) { tiles &= z.lo == next && z.lo <= z.root && z.root <= z.hi; next = z.hi + 1; }
        tiles &= next == 128;
    }
    TEST("zones tile 0-127, each root inside its zone", tiles);
    TEST("zone split halfway: root 36 -> 0-38, root 42 -> 39-44", in.layers[0].zones[0].hi == 38 && in.layers[0].zones[1].lo == 39 && in.layers[0].zones[1].hi == 44);
    bool pitch = rep.rows.size() == 18;
    for (const auto& r : rep.rows) pitch &= std::fabs(r.cents - 3.0) < 0.2;
    TEST("measured pitch = the renderer's +3 cents detune on every sample (within 0.2)", pitch);
    TEST("no warnings for a pitched, in-tune-enough source", rep.warnings.empty());
    {
        const Sample& s = in.samples[0];
        TEST("leading silence trimmed to the 2 ms pre-roll; release kept; tail silence dropped",
             s.pcm.size() > 1.5 * 48000 && s.pcm.size() < 1.9 * 48000 && std::abs(s.pcm[0]) < 200 && std::abs(s.pcm[200]) > 100);
        TEST("fade-out ends at zero", s.pcm.back() == 0);
    }
    {
        int16_t pLo = 0, pHi = 0;
        for (int16_t v : in.samples[4].pcm) pLo = std::max<int16_t>(pLo, static_cast<int16_t>(std::abs(v)));
        for (int16_t v : in.samples[13].pcm) pHi = std::max<int16_t>(pHi, static_cast<int16_t>(std::abs(v)));
        TEST("one gain for the instrument: the soft layer stays quieter (64/120) and the loudest sample peaks at 0.9 FS",
             std::fabs(double(pLo) / pHi - 64.0 / 120.0) < 0.02 && pHi > 0.88 * 32767 && pHi < 0.91 * 32767);
    }
    TEST("names fit 20 chars: 'Test Plugin C2 L', layers 'Test Plugin Lo/Hi'",
         in.samples[0].name == "Test Plugin C2 L" && in.layers[1].name == "Test Plugin Hi" && in.samples[17].name == "Test Plugin C6 H");
    const auto sf2 = writeSf2(in);
    const auto chk = sf2::check(sf2);
    TEST("SF2 written and re-read: 2 instruments, 18 samples, 0 overlaps", chk.ok && chk.instruments.size() == 2 && chk.samples.size() == 18 && chk.overlapping() == 0);
    {
        RenderReport r2;
        TEST("deterministic", writeSf2(buildFromRenders(spec, synth(3.0), &r2)) == sf2);
    }
    {
        RenderReport r;
        const Instrument d = buildFromRenders(spec, synth(25.0), &r);
        TEST("a detuned source (+25 cents) is reported per sample, still built", validate(d).empty() && r.warnings.size() == 18);
        const Instrument q = buildFromRenders(spec, [](uint8_t, uint8_t) { return std::vector<float>(48000, 0.f); }, &r);
        TEST("silence is refused with a warning", q.samples.empty() && r.warnings.size() == 1);
        RenderSpec bad = spec; bad.velocities = {80, 40}; bad.splits.clear();
        RenderSpec badSplit = spec; badSplit.splits = {130};
        RenderSpec tooMany = spec; tooMany.velocities.clear(); tooMany.splits.clear();
        for (int v = 1; v <= 25; ++v) tooMany.velocities.push_back(uint8_t(v * 5));
        TEST("refused: descending velocities, a split outside its two renders, 25 layers",
             buildFromRenders(bad, synth(0), &r).samples.empty() && buildFromRenders(badSplit, synth(0), &r).samples.empty() &&
             buildFromRenders(tooMany, synth(0), &r).samples.empty());
        // regression: a note still loud when the render stops (e.g. a piano cut at hold + release)
        // must still end at exactly 0 (the fade used to stop at x/(fade+1), a step up to ~136 LSB).
        const Instrument cut = buildFromRenders(spec, [](uint8_t, uint8_t) {
            std::vector<float> x(48000);
            for (size_t n = 0; n < x.size(); ++n) x[n] = 0.8f * static_cast<float>(std::sin(n * 0.05));
            return x;
        }, &r);
        bool zeroEnd = !cut.samples.empty();
        for (const auto& s : cut.samples) zeroEnd &= s.pcm.back() == 0 && std::abs(s.pcm[s.pcm.size() - 2]) < 200;
        TEST("regression: a sample cut while loud fades to exactly 0", zeroEnd);
        const Instrument nan = buildFromRenders(spec, [](uint8_t, uint8_t) { return std::vector<float>(4800, NAN); }, &r);
        TEST("non-finite audio is refused", nan.samples.empty());
    }
    {   // regression: two close lines (detuned unison strings). The true fundamental is the strongest,
        // at -35 cents; a weaker line at +10 cents drew the earlier golden-only search off to it.
        const double kPi = 3.14159265358979323846, f0 = 440.0;
        std::vector<int16_t> x(48000);
        for (size_t n = 0; n < x.size(); ++n)
            x[n] = static_cast<int16_t>(12000 * std::sin(2 * kPi * f0 * std::pow(2.0, -35 / 1200.0) * n / 48000) +
                                        7000 * std::sin(2 * kPi * f0 * std::pow(2.0, 10 / 1200.0) * n / 48000));
        const double c = 1200 * std::log2(measurePitch(x.data(), x.size(), 48000, f0) / f0);
        TEST("regression: measurePitch finds the strongest of two close lines (-35 cents, not the +10 side line)", std::fabs(c + 35) < 0.5);
    }
    {   // sustain loop (loopHoldSec): the synth releases at 1.6 s render time (0.1 s silence + 1.5 s hold)
        RenderSpec ls = spec;
        ls.loopHoldSec = 1.6;
        ls.step = 12;   // 5 roots x 2 layers: the loop search is the slow part under sanitizers
        RenderReport r;
        const Instrument li = buildFromRenders(ls, synth(3.0), &r);
        bool looped = validate(li).empty() && r.warnings.empty(), seam = true, inHold = true, match = true;
        for (size_t k = 0; k < li.samples.size(); ++k) {
            const Sample& s = li.samples[k];
            looped &= s.looped && s.loopEnd - s.loopStart >= 48000 / 4;
            inHold &= s.loopStart >= 0.3 * 48000 && s.loopEnd + 64 == s.pcm.size() && s.loopEnd <= (1.6 - 0.1) * 48000 + 96;
            int maxStep = 0;
            for (uint32_t i = s.loopStart; i + 1 < s.loopEnd; ++i) maxStep = std::max(maxStep, std::abs(s.pcm[i + 1] - s.pcm[i]));
            seam &= s.pcm[s.loopEnd - 1] == s.pcm[s.loopStart - 1] && std::abs(s.pcm[s.loopStart] - s.pcm[s.loopEnd - 1]) <= maxStep &&
                    s.pcm[s.loopEnd] == s.pcm[s.loopStart];
            match &= r.rows[k].loopMatch > 0.99 && std::fabs(r.rows[k].cents - 3.0) < 0.2;
        }
        TEST("loop: every sample looped (>= 0.25 s), valid, no warnings", looped);
        TEST("loop: inside the held part (after 0.3 s, before note-off), cut after the loop + 64 frames", inHold);
        TEST("loop: seam continues the waveform (frame before end == frame before start; jump <= largest in-loop step; tail = loop start)", seam);
        TEST("loop: seam match > 0.99 on a tonal source; pitch report unchanged", match);
        const auto chk = sf2::check(writeSf2(li));
        TEST("looped instrument writes a readable SF2", chk.ok && chk.samples.size() == 10);
        RenderSpec shortHold = ls; shortHold.loopHoldSec = 0.5;
        RenderReport r2;
        const Instrument sh = buildFromRenders(shortHold, synth(0), &r2);
        TEST("a hold too short to loop keeps one-shots and warns per sample", !sh.samples.empty() && !sh.samples[0].looped && r2.warnings.size() == 10);
        uint32_t seed = 1;
        RenderReport r3;
        const Instrument noise = buildFromRenders(ls, [&](uint8_t, uint8_t) {
            std::vector<float> x(static_cast<size_t>(2.1 * 48000), 0.f);
            for (size_t n = 4800; n < static_cast<size_t>(1.9 * 48000); ++n) { seed = seed * 1664525u + 1013904223u; x[n] = (seed >> 8) / 16777216.f - 0.5f; }
            return x;
        }, &r3);
        size_t seamWarn = 0;
        for (const auto& w : r3.warnings) seamWarn += w.find("loop seam") != std::string::npos;
        TEST("white noise: the poor seam match is reported per sample", validate(noise).empty() && seamWarn == 10);
    }
    {   // 3 velocity layers, default splits halfway; stereo rendering with different L/R levels
        RenderSpec s3 = spec;
        s3.step = 12; s3.velocities = {40, 80, 120}; s3.splits.clear(); s3.channels = 2;
        RenderReport r;
        const RenderFn mono = synth(0.0);
        const Instrument in3 = buildFromRenders(s3, [&](uint8_t n, uint8_t v) {
            const auto m = mono(n, v);
            std::vector<float> lr;
            for (float x : m) { lr.push_back(x); lr.push_back(0.5f * x); }   // R at half level
            return lr;
        }, &r);
        bool ranges = in3.layers.size() == 3 && in3.layers[0].velHi == 60 && in3.layers[1].velLo == 61 && in3.layers[1].velHi == 100 &&
                      in3.layers[2].velLo == 101 && in3.layers[2].velHi == 127;
        TEST("3 layers: valid, velocity zones 0-60 / 61-100 / 101-127 (halfway), names V1..V3",
             validate(in3).empty() && ranges && in3.layers[2].name == "Test Plugin V3" && in3.samples[0].name == "Test Plugin C2 1");
        bool stereoOk = !in3.samples.empty();
        for (const auto& s : in3.samples) {
            stereoOk &= s.channels == 2 && s.pcm.size() == 2 * s.frames();
            for (size_t f = 1000; f < s.frames(); f += 997) stereoOk &= std::abs(s.pcm[2 * f + 1] * 2 - s.pcm[2 * f]) <= 2;
        }
        TEST("stereo: L/R kept interleaved with their level difference (R = L/2)", stereoOk);
        bool pitch = r.rows.size() == 15;
        for (const auto& row : r.rows) pitch &= std::fabs(row.cents) < 0.2;
        TEST("stereo: pitch measured on the channel average, in tune", pitch && r.warnings.empty());
        RenderSpec sl = s3; sl.loopHoldSec = 1.6;
        RenderReport rl;
        const Instrument looped = buildFromRenders(sl, [&](uint8_t n, uint8_t v) {
            const auto m = mono(n, v); std::vector<float> lr;
            for (float x : m) { lr.push_back(x); lr.push_back(0.5f * x); }
            return lr;
        }, &rl);
        bool seam = validate(looped).empty();
        for (const auto& s : looped.samples)
            for (size_t c = 0; c < 2; ++c)
                seam &= s.looped && s.pcm[(s.loopEnd - 1) * 2 + c] == s.pcm[(s.loopStart - 1) * 2 + c] && s.pcm[s.loopEnd * 2 + c] == s.pcm[s.loopStart * 2 + c];
        TEST("stereo loop: one seam for both channels, each continues exactly", seam && rl.warnings.empty());
    }
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
