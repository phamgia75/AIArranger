// Sample Lab analysis core (roadmap E4): normalization, silence trim, onsets, Time Slice (+ slice MIDI and the chain
// into the Sound package writer), duplicates, loop helpers, batch. Synthetic audio generated here; no third-party files.

#include "media/samplelab/sample_lab_io.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <unistd.h>

using namespace ai_arranger::media::samplelab;
namespace sound = ai_arranger::media::sound;
using ai_arranger::media::verifyManifest;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static constexpr uint32_t kRate = 44100;
static const double kPi = 3.14159265358979323846;

static Sample silence(size_t frames, uint8_t ch = 1) { Sample s; s.name = "T"; s.rate = kRate; s.channels = ch; s.pcm.assign(frames * ch, 0); return s; }
static void addSine(Sample& s, size_t from, size_t n, double hz, double amp) {
    for (size_t i = 0; i < n && from + i < s.frames(); ++i)
        for (size_t c = 0; c < s.channels; ++c) s.pcm[(from + i) * s.channels + c] = int16_t(std::lround(amp * std::sin(2 * kPi * hz * i / kRate)));
}
// Drum-like hit: white noise, exponential decay (tau frames), added in place.
static void addHit(Sample& s, size_t at, double amp, std::mt19937& rng, double tau = 300) {
    std::uniform_real_distribution<double> u(-1, 1);
    for (size_t i = 0; i < 6 * tau && at + i < s.frames(); ++i) {
        const double v = s.pcm[at + i] + amp * u(rng) * std::exp(-double(i) / tau);
        s.pcm[at + i] = int16_t(std::clamp(std::lround(v), -32768L, 32767L));
    }
}
static void addNoise(Sample& s, double amp, std::mt19937& rng) {
    std::uniform_real_distribution<double> u(-1, 1);
    for (int16_t& v : s.pcm) v = int16_t(std::clamp(std::lround(v + amp * u(rng)), -32768L, 32767L));
}
static bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Minimal SMF reader for the slice file: note-on ticks, tempo, end tick.
struct Midi { uint32_t tempo = 0, end = 0; uint16_t ppq = 0; std::vector<std::pair<uint32_t, uint8_t>> ons; bool ok = false; };
static Midi parseMidi(const std::vector<uint8_t>& m) {
    Midi r;
    if (m.size() < 22 || std::memcmp(m.data(), "MThd", 4) || std::memcmp(&m[14], "MTrk", 4) || m[9] != 0) return r;
    r.ppq = uint16_t(m[12] << 8 | m[13]);
    size_t p = 22;
    uint32_t now = 0;
    while (p < m.size()) {
        uint32_t d = 0;
        do d = d << 7 | (m[p] & 0x7F); while (m[p++] & 0x80);
        now += d;
        const uint8_t st = m[p++];
        if (st == 0xFF) {
            const uint8_t type = m[p++], len = m[p++];
            if (type == 0x51) r.tempo = uint32_t(m[p] << 16 | m[p + 1] << 8 | m[p + 2]);
            if (type == 0x2F) { r.end = now; r.ok = p + len == m.size(); return r; }
            p += len;
        } else {
            if ((st & 0xF0) == 0x90 && m[p + 1]) r.ons.push_back({now, m[p]});
            p += 2;
        }
    }
    return r;
}

static std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void put(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
static bool manifestOk(const fs::path& dir) {
    size_t n = 0;
    return verifyManifest(dir, dir / "PACKAGE.sha256", {}, &n, [](const std::string& r) { return r == "PACKAGE.sha256"; }, true).empty() && n > 0;
}

int main() {
    std::printf("Test: Sample Lab (E4)\n");
    std::mt19937 rng(12345);
    const fs::path root = fs::temp_directory_path() / ("aia-samplelab-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    // ── normalization ──
    {
        Sample s = silence(44100);
        addSine(s, 0, 44100, 440, 8000);
        Sample p = s;
        const GainReport g = normalize(p, NormMode::Peak, 0, 1);
        TEST("peak: lands at -headroom (-1 dBFS)", near(g.after.peakDb, -1, 0.01));
        TEST("peak: reported gain = 20log10(32767*10^(-1/20)/8000)", near(g.gainDb, 20 * std::log10(kFullScale * std::pow(10, -0.05) / 8000), 0.001));
        Sample r = s;
        const GainReport h = normalize(r, NormMode::Rms, -12, 1);
        TEST("rms: RMS lands at -12 dBFS, not limited", near(h.after.rmsDb, -12, 0.01) && !h.limited);
        TEST("rms: gain = before/after RMS difference", near(h.gainDb, h.after.rmsDb - h.before.rmsDb, 0.01));
        Sample q = s;
        const GainReport k = normalize(q, NormMode::Rms, -1, 1);
        TEST("rms: target above the headroom is limited, peak kept at -1 dBFS", k.limited && near(k.after.peakDb, -1, 0.01));
        Sample st = silence(10000, 2);
        for (size_t i = 0; i < 10000; ++i) st.pcm[2 * i + 1] = int16_t(i % 2 ? 4000 : -4000);   // right channel only
        const GainReport m = normalize(st, NormMode::Peak, -3, 0);
        TEST("stereo peak uses all channels, left stays silent", near(m.after.peakDb, -3, 0.01) && st.pcm[0] == 0 && st.pcm[2] == 0);
        Sample z = silence(1000);
        const GainReport n = normalize(z, NormMode::Peak, -1, 1);
        TEST("silent sample untouched", n.silent && n.gainDb == 0 && std::all_of(z.pcm.begin(), z.pcm.end(), [](int16_t v) { return v == 0; }));
    }

    // ── silence trim with fades ──
    {
        Sample s = silence(6000);
        addSine(s, 1000, 2000, 440, 10000);   // sample 1000 is sin(0) = 0, 1001 is the first audible one
        const TrimReport t = trimSilence(s, -60, 16, 64);
        TEST("trim: leading silence cut to the first audible frame", t.cutStart == 1001);
        TEST("trim: trailing silence cut", t.cutEnd >= 3000 && t.cutEnd <= 3002 && s.frames() == 6000 - t.cutStart - t.cutEnd);
        TEST("trim: fades start and end at zero", s.pcm.front() == 0 && s.pcm.back() == 0);
        Sample l = silence(6000);
        addSine(l, 1000, 2000, 440, 10000);
        l.looped = true; l.loopStart = 2000; l.loopEnd = 5000;   // loop runs into the silent tail
        trimSilence(l, -60, 16, 64);
        TEST("trim: never cuts into a loop; points follow the cut", l.loopStart == 999 && l.loopEnd == 3999 && l.frames() == 3999);
        Sample bad = silence(1000);
        addSine(bad, 0, 1000, 440, 10000);
        bad.looped = true; bad.loopStart = 100; bad.loopEnd = 50000;   // as a crafted 'smpl' chunk can claim
        trimSilence(bad, -60, 0, 0);
        TEST("trim: a loop past the audio never extends the kept range past it", bad.frames() <= 1000);
        Sample z = silence(500);
        TEST("trim: silent sample reported and untouched", trimSilence(z).silent && z.frames() == 500);
    }

    // ── onsets ──
    const std::vector<uint32_t> hits = {0, 4410, 15000, 30000, 52000, 70000};
    {
        Sample s = silence(88200);
        for (uint32_t h : hits) addHit(s, h, 20000, rng);
        addNoise(s, 30, rng);   // about -60 dBFS floor
        const auto on = detectOnsets(s);
        bool within = on.size() == hits.size();
        for (size_t i = 0; within && i < hits.size(); ++i) within = near(on[i], hits[i], 32);
        TEST("onsets: 6 drum hits (incl. frame 0) found within 32 frames (0.7 ms)", within);
        Sample c = silence(44100);
        for (uint32_t h : {3000u, 11000u, 20000u}) c.pcm[h] = 25000;   // single-sample clicks
        const auto oc = detectOnsets(c);
        TEST("onsets: single-sample clicks found at their exact frame", oc == std::vector<uint32_t>({3000, 11000, 20000}));
        Sample n = silence(44100);
        std::uniform_real_distribution<double> u(-1, 1);
        for (size_t i = 10000; i < 44100; ++i) n.pcm[i] = int16_t(std::lround(8000 * u(rng)));
        const auto on2 = detectOnsets(n);
        TEST("onsets: steady noise from frame 10000 gives exactly one onset there", on2.size() == 1 && near(on2[0], 10000, 4));
        Sample low = silence(44100);
        addSine(low, 4000, 40100, 32.7, 12000);   // C1: one period (1349 frames) is longer than the 512-frame window
        const auto ol = detectOnsets(low);
        TEST("onsets: a sustained low tone gives one onset, not one per period", ol.size() == 1 && near(ol[0], 4000, 300));
        Sample quiet = silence(44100);
        for (uint32_t h : {5000u, 25000u}) quiet.pcm[h] = 50;   // -56 dBFS click energy below the -50 dB gate
        TEST("onsets: hits below the gate are ignored", detectOnsets(quiet).empty());
        OnsetOptions o;
        o.minGapMs = 200;
        const auto og = detectOnsets(s, o);
        TEST("onsets: min gap drops hits closer than it", og.size() == 5 && og[1] > 14000);   // 4410 is 100 ms after 0
    }

    // ── Time Slice ──
    {
        // One 4/4 bar at 120 BPM (2 s): kick on beats 1-4, an off-beat hat at 2.5 beats.
        const std::vector<uint32_t> at = {0, 22050, 44100, 55125, 66150};
        Sample loop = silence(88200);
        for (uint32_t h : at) addHit(loop, h, 20000, rng, 400);
        SliceOptions o;
        o.bpm = 120;
        const SliceResult r = timeSlice(loop, "Loop A", o);
        TEST("slice: no error", r.error.empty());
        bool pos = r.starts.size() == at.size();
        for (size_t i = 0; pos && i < at.size(); ++i) pos = near(r.starts[i], at[i], 32);
        TEST("slice: 5 slices at the hits", pos && r.instrument.samples.size() == 5);
        size_t total = 0;
        bool keys = true;
        for (size_t i = 0; i < r.instrument.samples.size(); ++i) {
            total += r.instrument.samples[i].frames();
            const auto& z = r.instrument.layers[0].zones[i];
            keys &= z.lo == 36 + i && z.hi == z.lo && z.root == z.lo && r.instrument.samples[size_t(z.sample)].root == z.lo;
        }
        TEST("slice: slices cover the loop exactly", total == loop.frames());
        TEST("slice: one key per slice from C2 (36), root = key", keys && sound::validate(r.instrument).empty());
        const Midi m = parseMidi(r.midi);
        bool ticks = m.ok && m.ons.size() == 5;
        for (size_t i = 0; ticks && i < 5; ++i) ticks = m.ons[i].second == 36 + i && near(m.ons[i].first, r.starts[i] / 22050.0 * 480, 0.5);
        TEST("slice MIDI: 120 BPM tempo, 480 PPQ, one bar long", m.ok && m.tempo == 500000 && m.ppq == 480 && m.end == 1920);
        TEST("slice MIDI: each slice's note at its start tick on its key", ticks);
        TEST("slice MIDI: beat hits on exact beats", ticks && m.ons[1].first == 480 && m.ons[2].first == 960 && m.ons[4].first == 1440);

        std::string err;
        const std::string rep = writeSliceFolder(r, o, "synthetic", root / "slice", &err);
        TEST("slice folder written", !rep.empty() && err.empty());
        TEST("slice folder: PACKAGE.sha256 checks, no .INCOMPLETE left", manifestOk(root / "slice") && !fs::exists(root / "slice.INCOMPLETE"));
        TEST("slice folder: MIDI and key map files", slurp(root / "slice" / "Loop_A.mid") == r.midi && fs::exists(root / "slice" / "SLICES.tsv") &&
                                                      fs::exists(root / "slice" / "wav" / "Loop_A_036_036_036.wav"));
        Instrument back;
        TEST("slice wav/ reads back through the WAV-folder source", sound::buildFromWavFolder(root / "slice" / "wav", "Loop A", {}, {}, &back).empty() &&
                                                                     back.samples.size() == 5 && back.samples[3].pcm == r.instrument.samples[3].pcm);
        TEST("slice wav/ becomes a Sound package (SF2 + TXPROG)", !sound::writePackage(back, root / "slicepkg", "", &err).empty());
        TEST("slice folder: an existing destination is refused", writeSliceFolder(r, o, "synthetic", root / "slice", &err).empty() && err.find("exists") != std::string::npos);
        o.baseNote = 125;
        TEST("slice: more slices than keys left is refused", !timeSlice(loop, "Loop A", o).error.empty());
        o.baseNote = 36;
        o.bpm = 0;
        TEST("slice: bpm required", !timeSlice(loop, "Loop A", o).error.empty());
    }

    // ── duplicates ──
    std::vector<Sample> dup(5);
    {
        Sample a = silence(20000);
        addHit(a, 100, 15000, rng, 2000);
        Sample c = a;
        applyGain(c, -6);
        Sample d = a;
        addNoise(d, 60, rng);   // ~ -40 dB below the hit
        Sample e = silence(20000);
        addHit(e, 100, 15000, rng, 2000);   // a different hit
        dup = {a, a, c, d, e};
        const auto p = findDuplicates(dup);
        auto has = [&](size_t x, size_t y, bool exact) {
            return std::any_of(p.begin(), p.end(), [&](const DuplicatePair& q) { return q.a == x && q.b == y && q.exact == exact; });
        };
        TEST("duplicates: bit-identical copy is EXACT", has(0, 1, true) && pcmHash(dup[0]) == pcmHash(dup[1]));
        TEST("duplicates: quieter copy and noisy copy are NEAR", has(0, 2, false) && has(0, 3, false) && has(2, 3, false));
        TEST("duplicates: a different hit matches nothing",
             std::none_of(p.begin(), p.end(), [](const DuplicatePair& q) { return q.a == 4 || q.b == 4; }) && p.size() == 6);
        Sample f = a;
        f.rate = 48000;
        TEST("duplicates: hash includes the rate", pcmHash(f) != pcmHash(a) && similarity(f, a) == 0);
    }

    // ── loop helpers ──
    {
        Sample s = silence(20000);
        addSine(s, 0, 20000, 441, 12000);   // period 100 frames
        double step = 0;
        for (size_t i = 1; i < s.frames(); ++i) step = std::max(step, std::abs(double(s.pcm[i]) - s.pcm[i - 1]));
        auto seam = [](const Sample& x) { return std::abs(double(x.pcm[x.loopStart]) - x.pcm[x.loopEnd - 1]); };
        Sample a = s;
        a.looped = true; a.loopStart = 2025; a.loopEnd = 9050;   // not a whole number of periods: a step at the seam
        TEST("loop: unprocessed seam jumps far beyond the waveform's own step", seam(a) > 5 * step);
        Sample b = a;
        TEST("crossfade loop applied", crossfadeLoop(b, 2025, 9050, 1000).empty());
        TEST("crossfade: last loop frame equals the frame before loopStart", b.pcm[b.loopEnd - 1] == s.pcm[b.loopStart - 1]);
        TEST("crossfade: seam no larger than the waveform's own step", seam(b) <= step + 1);
        TEST("crossfade: audio outside the fade untouched", std::equal(b.pcm.begin(), b.pcm.begin() + 8050, s.pcm.begin()));
        Sample c = a;
        TEST("snap loop applied", snapLoop(c, 60).empty());
        TEST("snap: points on rising zero crossings, seam no larger than the step",
             c.pcm[c.loopStart - 1] < 0 && c.pcm[c.loopStart] >= 0 && c.pcm[c.loopEnd - 1] < 0 && seam(c) <= step + 1);
        TEST("snap: loop length becomes whole periods", (c.loopEnd - c.loopStart) % 100 == 0);
        TEST("crossfade longer than loopStart refused", !crossfadeLoop(b, 500, 9050, 1000).empty());
    }

    // ── batch ──
    {
        const fs::path in = root / "batch-in";
        fs::create_directories(in);
        Sample q = silence(30000);
        addSine(q, 5000, 15000, 261.63, 3000);   // middle C, quiet, with silence around it
        q.root = 0;
        put(in / "Piano_060_000_127.wav", sound::writeWav(q));
        put(in / "copy.wav", sound::writeWav(q));
        Sample n = silence(20000);
        addNoise(n, 5000, rng);
        put(in / "noise.wav", sound::writeWav(n));
        put(in / "broken.wav", {'R', 'I', 'F', 'F', 0, 0});
        Sample lp = silence(1000);
        addSine(lp, 0, 1000, 440, 10000);
        lp.looped = true; lp.loopStart = 100; lp.loopEnd = 900;
        auto lb = sound::writeWav(lp);
        const uint32_t far = 49999;   // smpl loop end (inclusive) far past the 1000 frames
        std::memcpy(&lb[lb.size() - 24 + 12], &far, 4);
        put(in / "zloop.wav", lb);
        BatchOptions o;
        o.trim = true;
        o.normalize = true;
        o.mode = NormMode::Peak;
        o.targetDb = -1;
        const BatchResult dry = runBatch(in, "", o);
        TEST("batch dry run: report only, nothing written", dry.error.empty() && !dry.report.empty() && !fs::exists(root / "batch-out"));
        const BatchResult r = runBatch(in, root / "batch-out", o);
        TEST("batch: no error", r.error.empty());
        TEST("batch: a loop outside the audio is refused, not trimmed", r.items.size() == 5 && r.items[4].error.find("loop") != std::string::npos &&
                                                                          !fs::exists(root / "batch-out" / "wav" / "zloop.wav"));
        TEST("batch: broken one reported and left out", true && r.items[1].file == "broken.wav" && !r.items[1].error.empty() &&
                                                                   r.report.find("NOT READ: broken.wav") != std::string::npos);
        const auto& pi = r.items[0];
        TEST("batch: trimmed and normalized to -1 dBFS", pi.trim.cutStart > 4990 && near(pi.after.peakDb, -1, 0.01) && pi.gain.gainDb > 0);
        TEST("batch: pitch measured (middle C)", near(pi.pitchHz, 261.63, 1));
        TEST("batch: exact duplicate reported", r.duplicates.size() == 1 && r.duplicates[0].exact &&
                                                 r.items[r.duplicates[0].a].file == "Piano_060_000_127.wav" && r.items[r.duplicates[0].b].file == "copy.wav");
        TEST("batch folder: manifest checks, 3 WAVs, broken one absent", manifestOk(root / "batch-out") && fs::exists(root / "batch-out" / "wav" / "noise.wav") &&
                                                                           !fs::exists(root / "batch-out" / "wav" / "broken.wav"));
        Sample back;
        TEST("batch WAV reads back with the smpl root it was written with",
             sound::readWav(slurp(root / "batch-out" / "wav" / "copy.wav"), &back).empty() && back.root == 0 && near(measureLevel(back).peakDb, -1, 0.01));
        TEST("batch: existing destination refused", !runBatch(in, root / "batch-out", o).error.empty());
        TEST("batch: empty folder refused", !runBatch(root / "slice" / "nothing", "", o).error.empty());
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
