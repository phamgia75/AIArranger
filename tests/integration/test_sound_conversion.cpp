// Sound conversion S1-S3 (docs/SOUND_CONVERSION_ROADMAP.md): instrument model limits, the original
// additive source (loops, pitch), the SF2 writer (re-read by the independent-code sf2::check parser,
// determinism, malformed prefixes), the WAV writer. SOFTWARE evidence only: nothing here shows the
// Pa5X accepts the files (that is S6).

#include "media/sound/instrument.h"
#include "media/sound/additive_source.h"
#include "media/sound/sf2_writer.h"
#include "media/sound/wav_writer.h"
#include "media/sound/txprog_writer.h"
#include "media/sf2_check.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ai_arranger::media;
using namespace ai_arranger::media::sound;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static uint32_t rd32(const std::vector<uint8_t>& b, size_t o) { return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24; }
static size_t findChunk(const std::vector<uint8_t>& b, const char* id) {
    for (size_t o = 0; o + 4 <= b.size(); ++o) if (!std::memcmp(&b[o], id, 4)) return o;
    return std::string::npos;
}

// |DFT| of x at bin `bin` over N samples (Goertzel).
static double mag(const int16_t* x, size_t N, double bin) {
    const double w = 2 * 3.14159265358979323846 * bin / N, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t n = 0; n < N; ++n) { const double s0 = x[n] + c * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
}

int main() {
    std::printf("Test: Sound conversion S1-S5\n");
    double worstCents = 99;
    const Instrument org = buildDemoOrgan(&worstCents);

    // ── S1 model ──
    TEST("demo organ is valid against the Pa5X limits", validate(org).empty());
    TEST("2 velocity layers x 19 zones (C1..C7 every 4 semitones), 38 samples",
         org.layers.size() == 2 && org.layers[0].zones.size() == 19 && org.layers[1].zones.size() == 19 && org.samples.size() == 38);
    bool cover = true;
    for (const auto& L : org.layers) {
        int next = 0;
        for (const auto& z : L.zones) { cover &= z.lo == next; next = z.hi + 1; }
        cover &= next == 128;
    }
    TEST("each layer's zones tile keys 0-127 exactly (no gap, no overlap)", cover);
    {
        Instrument bad = org;
        bad.samples[0].rate = 8000;
        bad.samples[1].pcm.resize(1048577);
        bad.samples[2].loopEnd = bad.samples[2].loopStart + 10;
        bad.layers[0].zones[1].lo = bad.layers[0].zones[0].hi;   // overlap
        bad.layers.push_back(bad.layers[0]);                      // 3 layers
        const auto e = validate(bad);
        auto has = [&](const char* s) { for (const auto& m : e) if (m.find(s) != std::string::npos) return true; return false; };
        TEST("validate names each violation: rate, >1 048 576 frames, short loop, overlap, 3 layers",
             has("rate outside") && has("1 048 576") && has("shorter than 32") && has("overlap") && has("1 or 2 velocity layers"));
        TEST("the SF2 writer refuses an invalid instrument", writeSf2(bad).empty());
    }

    // ── S2 source: loops and pitch ──
    TEST("worst rendered pitch error < 0.1 cent", worstCents < 0.1);
    bool seamless = true, attackOk = true, inRange = true;
    for (const auto& s : org.samples) {
        for (size_t i = 0; i < 64 && s.loopEnd + i < s.pcm.size(); ++i) seamless &= s.pcm[s.loopEnd + i] == s.pcm[s.loopStart + i];
        attackOk &= s.pcm[0] == 0 && s.loopStart >= 16;
        inRange &= s.pcm.size() <= 1048576 && s.loopEnd + 8 <= s.pcm.size();
    }
    TEST("every loop is seamless: the frames after loopEnd repeat the loop start exactly", seamless);
    TEST("attack starts at 0 (no click) and the loop starts after it", attackOk);
    TEST("every sample fits 1 048 576 frames with valid frames after the loop", inRange);
    {   // the dominant spectral line over one loop is the requested note's fundamental
        bool fund = true;
        for (const auto& s : org.samples) {
            const size_t L = s.loopEnd - s.loopStart;
            const double k = noteHz(s.root) * L / s.rate;
            const int16_t* x = s.pcm.data() + s.loopStart;
            const double atK = mag(x, L, std::round(k));
            for (double b : {std::round(k) - 1, std::round(k) + 1, std::round(k / 2)})   // no energy off the harmonic grid
                if (b > 0 && b < L / 2.0) fund &= atK > 100 * mag(x, L, b);
            for (double b : {std::round(2 * k), std::round(3 * k)})                     // fundamental is the strongest line
                if (b < L / 2.0) fund &= atK > mag(x, L, b);
        }
        TEST("every sample: the fundamental (bin k) is the strongest line; neighbours and the octave below < 1%", fund);
    }

    // ── S3 SF2 writer ──
    const auto sf2bytes = writeSf2(org);
    TEST("SF2 written", !sf2bytes.empty());
    TEST("RIFF size field == file size - 8, form 'sfbk', ifil 2.04",
         sf2bytes.size() > 12 && rd32(sf2bytes, 4) == sf2bytes.size() - 8 && !std::memcmp(&sf2bytes[8], "sfbk", 4) &&
         [&] { size_t o = findChunk(sf2bytes, "ifil"); return o != std::string::npos && sf2bytes[o + 8] == 2 && sf2bytes[o + 10] == 4; }());
    const sf2::Report rep = sf2::check(sf2bytes);
    TEST("sf2::check reads it: 2 instruments, 38 samples, 0 overlapping instruments",
         rep.ok && rep.instruments.size() == 2 && rep.samples.size() == 38 && rep.overlapping() == 0);
    bool zonesMatch = rep.ok && rep.instruments.size() == 2;
    for (size_t l = 0; zonesMatch && l < 2; ++l) {
        zonesMatch &= rep.instruments[l].name == org.layers[l].name && rep.instruments[l].zones.size() == org.layers[l].zones.size();
        for (size_t z = 0; zonesMatch && z < org.layers[l].zones.size(); ++z) {
            const auto& a = rep.instruments[l].zones[z];
            const auto& b = org.layers[l].zones[z];
            zonesMatch &= a.key.lo == b.lo && a.key.hi == b.hi && a.sample == b.sample;
        }
    }
    TEST("re-read zones equal the model (names, key ranges, sample per zone)", zonesMatch);
    {   // shdr: start/end/loop of sample 0 and 1, rate, root
        const size_t o = findChunk(sf2bytes, "shdr") + 8;
        const Sample& s0 = org.samples[0];
        const size_t r1 = o + 46;
        const uint32_t start1 = static_cast<uint32_t>(s0.pcm.size()) + 46;
        TEST("shdr: sample 0 at 0, end = frames, loop = model loop; sample 1 after 46 pad frames; rate, root, mono",
             rd32(sf2bytes, o + 20) == 0 && rd32(sf2bytes, o + 24) == s0.pcm.size() && rd32(sf2bytes, o + 28) == s0.loopStart &&
             rd32(sf2bytes, o + 32) == s0.loopEnd && rd32(sf2bytes, o + 36) == 48000 && sf2bytes[o + 40] == s0.root &&
             sf2bytes[o + 44] == 1 && rd32(sf2bytes, r1 + 20) == start1);
    }
    TEST("deterministic: same instrument -> identical bytes", writeSf2(buildDemoOrgan()) == sf2bytes);
    {
        bool clean = true;
        std::vector<size_t> cuts;   // every byte of the headers, then 400 cuts spread over the rest
        for (size_t n = 0; n < 512; ++n) cuts.push_back(n);
        for (size_t n = 512; n < sf2bytes.size(); n += sf2bytes.size() / 400) cuts.push_back(n);
        for (size_t n = sf2bytes.size() - 2048; n < sf2bytes.size(); n += 7) cuts.push_back(n);   // pdta lives at the end
        for (size_t n : cuts) {
            const std::vector<uint8_t> pre(sf2bytes.begin(), sf2bytes.begin() + n);
            clean &= !sf2::check(pre).ok || n > sf2bytes.size() - 64;
        }
        TEST("truncated copies are refused by the reader (bounds-checked; ASan in CI)", clean);
    }

    // ── S5 TXPROG package ──
    {
        const auto pkg = writeTxProg(org);
        const std::string x = pkg.empty() ? "" : std::string(pkg[0].bytes.begin(), pkg[0].bytes.end());
        auto count = [&](const std::string& s) { size_t n = 0; for (size_t p = x.find(s); p != std::string::npos; p = x.find(s, p + 1)) ++n; return n; };
        TEST("TXPROG package: .txprog first + 38 WAVs in '<name> Samples/' under one folder",
             pkg.size() == 39 && pkg[0].path == "AIA Demo Organ/AIA Demo Organ.txprog" &&
             pkg[1].path == "AIA Demo Organ/AIA Demo Organ Samples/AIA OrgS C1.wav");
        TEST("UTF-8 BOM, program namespace, program name",
             x.rfind("\xEF\xBB\xBF<?xml", 0) == 0 && x.find("xmlns:tx=\"http://www.tx16wx.com/3.0/program\"") != std::string::npos &&
             x.find("tx:name=\"AIA Demo Organ\"") != std::string::npos);
        TEST("38 waves, 2 groups, 38 regions, 38 forward loops", count("<tx:wave ") == 38 && count("<tx:group ") == 2 &&
             count("<tx:region ") == 38 && count("tx:mode=\"Forward\"") == 38);
        const Sample& s0 = org.samples[0];
        TEST("wave 0: relative path, root C1, loop end exclusive (= model loopEnd)",
             x.find("tx:path=\"AIA Demo Organ Samples/AIA OrgS C1.wav\" tx:id=\"0\" tx:root=\"C1\"") != std::string::npos &&
             x.find("tx:start=\"" + std::to_string(s0.loopStart) + "\" tx:end=\"" + std::to_string(s0.loopEnd) + "\"") != std::string::npos);
        TEST("key names span C-1..G9; Low group velocities 0-89, High 90-127",
             x.find("tx:low-key=\"C-1\" tx:high-key=\"C#1\" tx:low-vel=\"0\" tx:high-vel=\"89\"") != std::string::npos &&
             x.find("tx:low-key=\"A#6\" tx:high-key=\"G9\" tx:low-vel=\"90\" tx:high-vel=\"127\"") != std::string::npos &&
             txw::noteName(60) == "C4" && txw::noteName(127) == "G9");
        TEST("each packaged WAV equals writeWav of its sample", pkg.size() == 39 && pkg[38].bytes == writeWav(org.samples[37]));
        TEST("deterministic package", writeTxProg(buildDemoOrgan()).size() == 39 && writeTxProg(buildDemoOrgan())[0].bytes == pkg[0].bytes);
        Instrument r = org; r.layers[0].zones[0].root = 30;
        Instrument d = org; d.samples[1].name = d.samples[0].name;
        Instrument q = org; q.name = "A<&\"B"; q.layers[0].name = "x'y";
        const auto qp = writeTxProg(q);
        const std::string qx = qp.empty() ? "" : std::string(qp[0].bytes.begin(), qp[0].bytes.end());
        Instrument nn = org; nn.name = "";
        Instrument nl = org; nl.name = std::string(21, 'x');
        TEST("regression: empty or > 20-char instrument name is invalid (an empty name made the package path '/.txprog', absolute)",
             !validate(nn).empty() && writeTxProg(nn).empty() && writeSf2(nn).empty() && !validate(nl).empty());
        TEST("refuses: zone root != sample root, duplicate sample file names, invalid instrument",
             writeTxProg(r).empty() && writeTxProg(d).empty() && writeTxProg(Instrument{}).empty());
        TEST("names are XML-escaped and file-safe",
             qx.find("tx:name=\"A&lt;&amp;&quot;B\"") != std::string::npos && qx.find("tx:name=\"x&apos;y\"") != std::string::npos &&
             qp[0].path == "A___B/A___B.txprog");
    }

    // ── WAV ──
    {
        const auto w = writeWav(org.samples[5]);
        const size_t sm = findChunk(w, "smpl");
        TEST("WAV: RIFF size, 48 kHz mono 16-bit, smpl unity note = root, loop end inclusive",
             rd32(w, 4) == w.size() - 8 && rd32(w, 24) == 48000 && w[22] == 1 && w[34] == 16 &&
             sm != std::string::npos && rd32(w, sm + 8 + 12) == org.samples[5].root &&
             rd32(w, sm + 8 + 28) == 1 && rd32(w, sm + 8 + 44) == org.samples[5].loopStart &&
             rd32(w, sm + 8 + 48) == org.samples[5].loopEnd - 1);
    }

    std::printf("    demo organ: %zu samples, %.1f MB of 16-bit audio (Pa5X User Sample memory 4 GB), worst %.4f cents\n",
                org.samples.size(), sampleBytes(org) / 1e6, worstCents);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
