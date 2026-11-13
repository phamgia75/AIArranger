// P1 Sound Studio + Sample Lab (offline): zone map over any decoded Sound, loop editing carried into the SF2/KMP/KSF
// writers, waveform/seam view, bit-depth and rate conversion (optimize.h) with the batch's before/after report, the
// Drum Kit builder (TXPERF and KMP + KSF), and the EXACT/MEASURED/ESTIMATED/MISSING section of the E6 export report.
// Synthetic audio generated here only; no third-party or owner audio.

#include "jobs/export_specs.h"
#include "media/samplelab/drum_builder.h"
#include "media/samplelab/zone_map.h"
#include "media/sound/additive_source.h"
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;
namespace lab = media::samplelab;
namespace sound = media::sound;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } } while (0)

static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static const double kPi = 3.14159265358979323846;

// A WAV of `bits` (8/16/24/32 integer, or 32 float), interleaved samples in -1..1; optional smpl root + loop.
static std::vector<uint8_t> makeWav(int bits, bool isFloat, bool extensible, uint32_t rate, int ch, const std::vector<double>& x, int root = 60,
                                    long loopA = -1, long loopB = -1) {
    std::vector<uint8_t> b;
    auto u16 = [&](uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    auto id = [&](const char* f) { b.insert(b.end(), f, f + 4); };
    const int by = bits / 8;
    id("RIFF"); u32(0); id("WAVE");
    id("fmt "); u32(extensible ? 40 : 16); u16(extensible ? 0xFFFE : isFloat ? 3 : 1); u16(ch); u32(rate); u32(rate * by * ch); u16(by * ch); u16(bits);
    if (extensible) { u16(22); u16(bits); u32(ch == 2 ? 3 : 4); u16(isFloat ? 3 : 1); for (int i = 0; i < 14; ++i) b.push_back(0); }
    id("data"); u32(uint32_t(x.size() * by));
    for (double v : x) {
        if (isFloat) { float f = float(v); uint8_t t[4]; std::memcpy(t, &f, 4); b.insert(b.end(), t, t + 4); }
        else if (bits == 8) b.push_back(uint8_t(std::clamp(std::lround(v * 128 + 128), 0L, 255L)));
        else {
            const long long full = 1LL << (bits - 1);
            const long long q = std::clamp<long long>(std::llround(v * full), -full, full - 1);
            for (int i = 0; i < by; ++i) b.push_back(uint8_t(uint64_t(q) >> (8 * i)));
        }
    }
    if (x.size() * by % 2) b.push_back(0);
    id("smpl"); u32(loopA >= 0 ? 60 : 36);
    u32(0); u32(0); u32(1000000000u / rate); u32(uint32_t(root)); u32(0); u32(0); u32(0); u32(loopA >= 0 ? 1 : 0); u32(0);
    if (loopA >= 0) { u32(0); u32(0); u32(uint32_t(loopA)); u32(uint32_t(loopB - 1)); u32(0); u32(0); }
    const uint32_t riff = uint32_t(b.size() - 8);
    std::memcpy(&b[4], &riff, 4);
    return b;
}
static std::vector<double> sine(double hz, uint32_t rate, size_t frames, double amp, int ch = 1) {
    std::vector<double> x;
    for (size_t i = 0; i < frames; ++i) for (int c = 0; c < ch; ++c) x.push_back(amp * std::sin(2 * kPi * hz * double(i) / rate));
    return x;
}
static void putBytes(const fs::path& p, const std::vector<uint8_t>& b) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary).write((const char*)b.data(), long(b.size())); }
static JobState run(const ExportSpec& s) {
    JobState st;
    st.id = 1;
    runExport(s, st, [] { return false; }, [](const JobState&) {});
    return st;
}
static size_t risingCrossings(const lab::Sample& s) {
    size_t n = 0;
    for (size_t f = 1; f < s.frames(); ++f) n += lab::monoAt(s, f - 1) < 0 && lab::monoAt(s, f) >= 0;
    return n;
}

int main() {
    std::printf("Test: P1 Sound Studio + Sample Lab\n");
    const fs::path root = fs::temp_directory_path() / ("aia-sound-studio-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    // ── optimize.h: bit depth ──
    {
        const auto x = sine(440, 48000, 4800, 0.5);
        lab::WavFormat f;
        std::string err;
        const auto w16 = makeWav(16, false, false, 48000, 1, x, 64, 100, 4000);
        TEST("16-bit PCM is passed through byte for byte", lab::to16bit(w16, &f, &err) == w16 && f.bits == 16 && f.frames == 4800);
        lab::Sample ref, s;
        sound::readWav(w16, &ref);
        bool ok = true;
        for (const auto& [bits, isFloat, ext] : std::vector<std::tuple<int, bool, bool>>{{24, false, false}, {32, false, false}, {32, true, false}, {24, false, true}}) {
            s = {};
            const auto e = lab::readWavAny(makeWav(bits, isFloat, ext, 48000, 1, x, 64, 100, 4000), &s, &f);
            long worst = 0;
            for (size_t i = 0; e.empty() && i < s.pcm.size(); ++i) worst = std::max(worst, std::labs(long(s.pcm[i]) - ref.pcm[i]));
            ok &= e.empty() && f.bits == bits && f.isFloat == isFloat && s.frames() == 4800 && worst <= 1 && s.root == 64 && s.looped && s.loopStart == 100 &&
                  s.loopEnd == 4000;
        }
        TEST("24/32-bit, float and EXTENSIBLE -> 16-bit within 1 LSB (TPDF dither), smpl root and loop kept", ok);
        s = {};
        const auto w8 = makeWav(8, false, false, 22050, 1, {0.0, 0.5, -0.5, 127.0 / 128});
        TEST("8-bit is converted exactly (a shift)", lab::readWavAny(w8, &s, &f, nullptr).empty() && s.pcm == std::vector<int16_t>({0, 16384, -16384, 32512}));
        lab::Sample a, c;
        const auto w24 = makeWav(24, false, false, 48000, 1, x);
        bool n1 = false;
        TEST("dither is deterministic (same input, same samples)", lab::readWavAny(w24, &a, &f, &n1).empty() && lab::readWavAny(w24, &c, &f, &n1).empty() && a.pcm == c.pcm);
        auto w12 = makeWav(16, false, false, 48000, 1, x);
        w12[34] = 12;   // bits per sample
        TEST("not a WAV, or an unsupported depth: refused with a reason", lab::to16bit(std::vector<uint8_t>(20, 0), &f, &err).empty() && has(err, "RIFF") &&
                                                                            !lab::readWavAny(w12, &s, &f).empty());
    }

    // ── optimize.h: rate ──
    {
        TEST("Pa5X rates: 96k -> 48k, 88.2k -> 44.1k, 8k -> 11.025k, 44.1k unchanged",
             lab::pa5xRate(96000) == 48000 && lab::pa5xRate(88200) == 44100 && lab::pa5xRate(8000) == 11025 && lab::pa5xRate(44100) == 44100);
        lab::Sample s;
        lab::WavFormat f;
        lab::readWavAny(makeWav(24, false, false, 96000, 2, sine(1000, 96000, 96000, 0.5, 2), 60, 9600, 86400), &s, &f);
        const auto before = lab::measureLevel(s);
        lab::resample(s, 48000);
        const auto after = lab::measureLevel(s);
        TEST("96 kHz -> 48 kHz: half the frames, stereo kept", s.rate == 48000 && s.frames() == 48000 && s.channels == 2);
        TEST("1 kHz stays 1 kHz (rising crossings over 1 s)", risingCrossings(s) >= 999 && risingCrossings(s) <= 1001);
        TEST("level within 0.1 dB", std::abs(after.rmsDb - before.rmsDb) < 0.1);
        TEST("loop points scale with the rate", s.looped && s.loopStart == 4800 && s.loopEnd == 43200);
        lab::Sample hf;
        lab::readWavAny(makeWav(16, false, false, 96000, 1, sine(30000, 96000, 9600, 0.5)), &hf, &f);
        lab::resample(hf, 48000);
        TEST("30 kHz (above the new Nyquist) is filtered out, not aliased (< -40 dBFS)", lab::measureLevel(hf).rmsDb < -40);
    }

    // ── batch --convert: before/after format, Pa5X range ──
    {
        const fs::path in = root / "convin";
        putBytes(in / "hi.wav", makeWav(24, false, false, 96000, 1, sine(220, 96000, 48000, 0.4)));
        putBytes(in / "ok.wav", makeWav(16, false, false, 44100, 1, sine(330, 44100, 22050, 0.4)));
        const auto plain = lab::runBatch(in, {}, {});
        TEST("without convert a 24-bit 96 kHz file is not read", plain.error.empty() && !plain.items[0].error.empty());
        lab::BatchOptions o;
        o.convert = true;
        const auto r = lab::runBatch(in, root / "convout", o);
        TEST("convert: written, folder verifies", r.error.empty() && ui::studio::verifyOutput(root / "convout").level->l1.state == Check::Passed);
        TEST("convert: before/after format per file in the report",
             has(r.report, "format: 96000 Hz 24-bit 1 ch 48000 frames -> 48000 Hz 16-bit 1 ch 24000 frames") &&
             has(r.report, "format: 44100 Hz 16-bit 1 ch 22050 frames -> 44100 Hz 16-bit 1 ch 22050 frames"));
        lab::Sample s;
        std::string err;
        TEST("convert: the written WAV is 16-bit 48 kHz", lab::readWavFile(root / "convout/wav/hi.wav", &s, &err) && s.rate == 48000 && s.frames() == 24000);
        TEST("source folder unchanged", fs::file_size(in / "hi.wav") > 140000);
    }

    // ── waveform view and seam ──
    {
        lab::Sample s;
        s.rate = 48000;
        s.pcm.assign(1000, 0);
        s.pcm[10] = 1000; s.pcm[600] = -2000;
        const auto p = lab::overview(s, 0, 1000, 10);
        TEST("overview: min/max per column", p.size() == 10 && p[0].hi == 1000 && p[0].lo == 0 && p[6].lo == -2000 && p[3].lo == 0 && p[3].hi == 0);
        TEST("overview of an empty range is empty", lab::overview(s, 500, 500, 10).empty());
        TEST("no loop: no seam", !lab::loopSeam(s).has);
        for (size_t i = 0; i < 1000; ++i) s.pcm[i] = int16_t(std::lround(10000 * std::sin(2 * kPi * double(i) / 100)));
        s.looped = true; s.loopStart = 100; s.loopEnd = 925;   // ends a quarter period past a crossing, at the crest
        const auto bad = lab::loopSeam(s);
        lab::snapLoop(s, 60);
        const auto good = lab::loopSeam(s);
        TEST("seam: a loop half a period off jumps far more than the wave steps", bad.has && bad.jump > 5 * bad.slope);
        TEST("seam: after zero-crossing snap the jump is within one step", good.has && good.jump <= 2 * good.slope && good.jump < bad.jump / 10);
    }

    // ── zone map: any decoded Sound ──
    const auto organ = sound::buildDemoOrgan();
    {
        const std::string text = lab::formatZoneMap(organ);
        sound::Instrument same;
        TEST("zone map: proposed text applies back to the same Sound",
             lab::applyZoneMap(organ, text, &same).empty() && same.layers.size() == organ.layers.size() &&
                 lab::formatZoneMap(same) == text && same.samples.size() == organ.samples.size());
        // Edit: one layer, two zones split at C4, root/tune/level set, sample 0 looped with snap + crossfade, the rest unused
        const auto& s0 = organ.samples[0];
        const std::string edit = "layer 0 63 Soft  # comment\nzone 0 C-1 B3 A3 -12 -1.5 loop 4000 " + std::to_string(s0.frames() - 100) +
                                 " snap 64 xfade 512\nzone 1 C4 G9 C5 +5 0 oneshot\nlayer 64 127 Hard\nzone 1 0 127 C5 +0 0.0 oneshot\n";
        sound::Instrument m;
        std::string notes;
        TEST("zone map edit applies", lab::applyZoneMap(organ, edit, &m, &notes).empty());
        TEST("velocity layers and split points as written", m.layers.size() == 2 && m.layers[0].velHi == 63 && m.layers[1].velLo == 64 &&
                                                                  m.layers[0].zones.size() == 2 && m.layers[0].zones[0].hi == 59 && m.layers[0].zones[1].lo == 60);
        TEST("root, tune and level per zone", m.layers[0].zones[0].root == 57 && m.layers[0].zones[0].tuneCents == -12 &&
                                                  m.layers[0].zones[0].levelCentibels == 15 && m.layers[0].zones[1].tuneCents == 5);
        TEST("unused samples are left out, indices remapped", m.samples.size() == 2 && m.layers[1].zones[0].sample == 1);
        TEST("loop snapped to rising zero crossings and crossfaded; noted for the report",
             m.samples[0].looped && lab::zeroCrossingNear(m.samples[0], m.samples[0].loopStart, 0) == m.samples[0].loopStart &&
                 m.samples[0].pcm != s0.pcm && has(notes, "snapped") && has(notes, "crossfade 512"));
        TEST("oneshot removes a loop", !m.samples[1].looped);
        // errors, each with its line
        auto err = [&](const std::string& t) { sound::Instrument x; return lab::applyZoneMap(organ, t, &x); };
        TEST("refused: overlapping zones", has(err("layer 0 127 A\nzone 0 0 70 C4 +0 0 oneshot\nzone 1 60 127 C4 +0 0 oneshot\n"), "overlap"));
        TEST("refused: velocity gap", has(err("layer 0 60 A\nzone 0 0 127 C4 +0 0 oneshot\nlayer 70 127 B\nzone 1 0 127 C4 +0 0 oneshot\n"), "tile"));
        TEST("refused: tune without sign", has(err("layer 0 127 A\nzone 0 0 127 C4 5 0 oneshot\n"), "line 2"));
        TEST("refused: positive level", has(err("layer 0 127 A\nzone 0 0 127 C4 +0 3 oneshot\n"), "level"));
        TEST("refused: one sample, two loops", has(err("layer 0 63 A\nzone 0 0 127 C4 +0 0 loop 100 9000\nlayer 64 127 B\nzone 0 0 127 C4 +0 0 oneshot\n"), "another loop"));
        TEST("refused: one sample, two roots", has(err("layer 0 63 A\nzone 0 0 127 C4 +0 0 oneshot\nlayer 64 127 B\nzone 0 0 127 D4 +0 0 oneshot\n"), "one root"));
        TEST("refused: loop outside the sample", has(err("layer 0 127 A\nzone 0 0 127 C4 +0 0 loop 100 99999999\n"), "inside"));
        TEST("refused: zone before a layer", has(err("zone 0 0 127 C4 +0 0 oneshot\n"), "layer line above"));
        TEST("refused: sample number out of range", has(err("layer 0 127 A\nzone 99 0 127 C4 +0 0 oneshot\n"), "sample must be"));

        // the edited loop and zones reach the writers: KMP + KSF read back by the KRONOS reader, SF2 + WAV in the package
        std::string e;
        const auto kmpRep = sound::writeKorgFolder(m, root / "mapped-kmp", "", &e);
        sound::Instrument back;
        TEST("KMP/KSF folder written from the zone map", !kmpRep.empty());
        TEST("KRONOS reader gets the zones and the edited loop back",
             sound::buildFromKorgFolder(root / "mapped-kmp", "Back", {"Soft", "Hard"}, {64}, &back).empty() && back.layers.size() == 2 && back.layers[0].zones[0].hi == 59 &&
                 back.layers[0].zones[0].root == 57 && back.layers[0].zones[0].tuneCents == -12 &&
                 back.samples[size_t(back.layers[0].zones[0].sample)].loopStart == m.samples[0].loopStart &&
                 back.samples[size_t(back.layers[0].zones[0].sample)].loopEnd == m.samples[0].loopEnd);
        const auto pkg = sound::writePackage(m, root / "mapped-pkg", "zone map test\n", &e);
        lab::Sample w;
        std::string re;
        bool found = false;
        if (pkg.empty()) std::fprintf(stderr, "    writePackage: %s\n", e.c_str());
        else for (const auto& f : fs::directory_iterator(root / "mapped-pkg/wav"))
            if (lab::readWavFile(f.path(), &w, &re) && w.looped) found |= w.loopStart == m.samples[0].loopStart && w.loopEnd == m.samples[0].loopEnd;
        TEST("Sound package WAVs carry the edited loop (smpl)", !pkg.empty() && found);
    }

    // ── Convert Sound model: the zone map on a WAV folder source ──
    {
        const fs::path wavDir = root / "wavsrc";
        fs::create_directories(wavDir);
        for (int k : {48, 60, 72}) {
            const auto x = sine(440 * std::pow(2, (k - 69) / 12.0), 44100, 44100, 0.3);
            putBytes(wavDir / ("Pad_" + std::to_string(k) + "_" + std::to_string(k == 48 ? 0 : k - 5) + "_" + std::to_string(k == 72 ? 127 : k + 6) + ".wav"),
                     makeWav(16, false, false, 44100, 1, x, k, 2000, 40000));
        }
        sound::Instrument in;
        std::string err, map;
        TEST("decode proposes the zone map when it is empty",
             ui::SoundConversionModel::decode(wavDir, "Pad", "Pad", "", in, &err, nullptr, nullptr, &map) && has(map, "layer 0 127 Pad") && has(map, "loop 2000 40000"));
        std::string edited = map;
        edited.replace(edited.find("loop 2000 40000"), 15, "loop 3000 30000");
        sound::Instrument in2;
        std::string zn;
        TEST("decode applies an edited zone map (loop moved)",
             ui::SoundConversionModel::decode(wavDir, "Pad", "Pad", "", in2, &err, nullptr, nullptr, &edited, &zn) && in2.samples[0].loopStart == 3000 &&
                 in2.samples[0].loopEnd == 30000 && has(zn, "loop 3000-30000"));
        std::string broken = "layer 0 127 Pad\nzone 0 C4 B3 C4 +0 0 oneshot\n";
        TEST("decode refuses a bad zone map with its reason", !ui::SoundConversionModel::decode(wavDir, "Pad", "Pad", "", in2, &err, nullptr, nullptr, &broken) &&
                                                                  has(err, "low key above high key"));
        // through the E6 job: report has the conversion section
        const auto st = run(soundPackage(std::make_shared<const sound::Instrument>(in), wavDir, "", root / "pad-pkg"));
        TEST("Sound package job: done, report has EXACT/MEASURED/ESTIMATED/MISSING and the status line",
             st.phase == Phase::Done && has(st.reportMd, "## Conversion (EXACT / MEASURED / ESTIMATED / MISSING)") &&
                 has(st.reportMd, "Status: structurally valid, not verified on a Pa5X.") && has(st.reportMd, "**EXACT** 1 velocity layer(s), 3 key zone(s)") &&
                 has(st.reportMd, "**MISSING** envelopes") && has(slurp(root / "pad-pkg.export.json"), "\"status\": \"structurally valid, not verified on a Pa5X\"") &&
                 has(slurp(root / "pad-pkg.export.json"), "{\"class\": \"MEASURED\""));
        const auto k = run(kmpFolder(std::make_shared<const sound::Instrument>(in), root / "pad-kmp"));
        TEST("KMP job: conversion section, L1 only", k.phase == Phase::Done && has(k.reportMd, "**EXACT**") && k.level.l2.state != Check::Passed);
        const auto fail = run(soundPackage(std::make_shared<const sound::Instrument>(in), wavDir, "", wavDir / "inside"));
        TEST("a refused job's report never says structurally valid", fail.phase == Phase::Failed && has(fail.reportMd, "Status: not published.") &&
                                                                         !has(fail.reportMd, "structurally valid"));
    }

    // ── Drum Kit builder ──
    {
        const fs::path dk = root / "drumsrc";
        std::vector<double> kick, hat;
        for (size_t i = 0; i < 9600; ++i) kick.push_back(0.8 * std::sin(2 * kPi * 60 * i / 48000.0) * std::exp(-double(i) / 2000));
        uint32_t z = 7;
        for (size_t i = 0; i < 4800; ++i) { z ^= z << 13; z ^= z >> 17; z ^= z << 5; hat.push_back(0.3 * (z / 2147483648.0 - 1) * std::exp(-double(i) / 600)); }
        putBytes(dk / "Kick 1.wav", makeWav(24, false, false, 96000, 1, kick));
        putBytes(dk / "HH Closed.wav", makeWav(16, false, false, 48000, 1, hat));
        putBytes(dk / "HH Open.wav", makeWav(16, false, false, 48000, 2, std::vector<double>(9600, 0.1)));
        std::string map;
        TEST("proposed drum map: one key per WAV from C2 (36), sorted", lab::proposeDrumMap(dk, &map).empty() && has(map, "C2 HH Closed.wav group=HH_Closed") &&
                                                                      has(map, "C#2 HH Open.wav") && has(map, "D2 Kick 1.wav"));
        const std::string edit = "36 Kick 1.wav group=Kick level=-3 family=B name=Kick\n"
                                 "F#2 HH Closed.wav group=HiHat choke=1 pan=30 family=H\n"
                                 "A#2 HH Open.wav group=HiHat choke=1 pan=-20 level=2.5 family=H\n";
        sound::DrumKit kit;
        std::string notes;
        TEST("drum kit built from the edited map", lab::buildDrumKit(dk, "P1 Kit", edit, &kit, &notes).empty());
        TEST("groups, exclusive group, level and pan as mapped", kit.instruments.size() == 2 && kit.instruments[1].choke == 1 && kit.instruments[1].keys.size() == 2 &&
                                                                     kit.instruments[1].keys[0].panPct == 30 && kit.instruments[0].keys[0].layers[0].gainDb10 == -30 &&
                                                                     kit.instruments[1].keys[1].layers[0].gainDb10 == 25 && kit.instruments[0].keys[0].family == 'B');
        TEST("one sample per key, root = key, 24-bit 96 kHz converted (noted)", kit.samples.size() == 3 && kit.samples[0].rate == 48000 &&
                                                                                  kit.samples[0].root == 36 && kit.samples[1].root == 42 &&
                                                                                  has(notes, "Kick: 96000 Hz 24-bit 1 ch 9600 frames -> 48000 Hz 16-bit"));
        auto bad = [&](const std::string& t) { sound::DrumKit x; return lab::buildDrumKit(dk, "K", t, &x); };
        TEST("refused: two lines on one key", has(bad("36 Kick 1.wav\n36 HH Open.wav\n"), "key already assigned"));
        TEST("refused: one group, two chokes", has(bad("36 Kick 1.wav group=G choke=1\n38 HH Open.wav group=G choke=2\n"), "already has choke"));
        TEST("refused: level above +6 dB", has(bad("36 Kick 1.wav level=7\n"), "level"));
        TEST("refused: missing file", has(bad("36 Nope.wav\n"), "not a regular file"));
        TEST("refused: a path", has(bad("36 ../x.wav\n"), "not a path"));
        TEST("refused: unknown option", has(bad("36 Kick 1.wav velo=3\n"), "unknown option"));

        const auto kp = std::make_shared<const sound::DrumKit>(kit);
        const auto tx = run(drumKit(kp, notes, root / "kit-tx"));
        TEST("TXPERF Drum Kit job: done, L1 + L2, conversion section", tx.phase == Phase::Done && tx.level.l2.state == Check::Passed &&
                                                                          has(tx.reportMd, "**EXACT** group names, exclusive groups"));
        sound::Instrument asIn;
        std::vector<std::string> lost;
        TEST("as one multisample: refused when mono and stereo mix (KMP writer rule)",
             lab::drumKitAsInstrument(kit, &asIn, &lost).empty() && !run(drumKitKmp(kp, "", root / "kit-kmp-mixed")).reason.empty());
        sound::DrumKit mono;
        TEST("mono kit builds", lab::buildDrumKit(dk, "P1 Mono", "36 Kick 1.wav level=-3\nF#2 HH Closed.wav group=HiHat choke=1 pan=30\n", &mono).empty());
        const auto km = run(drumKitKmp(std::make_shared<const sound::DrumKit>(mono), "", root / "kit-kmp"));
        TEST("KMP/KSF Drum Kit job: done, L1, pan and exclusive groups listed MISSING", km.phase == Phase::Done && km.level.l1.state == Check::Passed &&
                                                                                          has(km.reportMd, "**MISSING** pan (1 key(s) panned)") &&
                                                                                          has(km.reportMd, "**MISSING** exclusive groups (1 choking"));
        sound::Instrument rb;
        TEST("KRONOS reader: one zone per key, root = key",
             sound::buildFromKorgFolder(root / "kit-kmp", "Back", {}, {}, &rb).empty() && rb.layers.size() == 1 && rb.layers[0].zones.size() == 2 &&
                 rb.layers[0].zones[0].lo == 36 && rb.layers[0].zones[0].hi == 36 && rb.layers[0].zones[0].root == 36 &&
                 rb.layers[0].zones[1].lo == 42 && rb.layers[0].zones[1].root == 42);
        TEST("drum source folder unchanged (3 files)", std::distance(fs::directory_iterator(dk), fs::directory_iterator{}) == 3);
    }

    // ── Drum Kit from a folder inside the job (the UI never reads the WAVs) ──
    {
        const auto j = run(drumKitFromFolder(root / "drumsrc", "Folder Kit", "36 Kick 1.wav level=-3\nF#2 HH Closed.wav choke=1\n", false, root / "kit-folder"));
        TEST("drum kit from folder: built in the job, TXPERF published at L1 + L2, source noted",
             j.phase == Phase::Done && j.level.l2.state == Check::Passed && has(j.writerText, "Source: WAV folder") && has(j.reportMd, "**EXACT** 2 key(s)"));
        const auto k = run(drumKitFromFolder(root / "drumsrc", "Folder Kit", "36 Kick 1.wav\n", true, root / "kit-folder-kmp"));
        TEST("drum kit from folder as KMP/KSF: published at L1", k.phase == Phase::Done && k.level.l1.state == Check::Passed && k.level.l2.state != Check::Passed);
        const auto bad = run(drumKitFromFolder(root / "drumsrc", "Folder Kit", "36 Nope.wav\n", false, root / "kit-bad"));
        TEST("drum kit from folder: a bad map fails at validate, nothing written", bad.phase == Phase::Failed && bad.stoppedAt == Phase::Validate &&
                                                                                     has(bad.reason, "Drum Kit refused") && !fs::exists(root / "kit-bad"));
        const auto in = run(drumKitFromFolder(root / "drumsrc", "Folder Kit", "36 Kick 1.wav\n", false, root / "drumsrc/out"));
        TEST("drum kit from folder: output inside the source refused", in.phase == Phase::Failed && !fs::exists(root / "drumsrc/out"));
    }

    // ── Analyze shows the loop seam numerically ──
    {
        lab::Sample s;
        s.rate = 48000;
        for (size_t i = 0; i < 4800; ++i) s.pcm.push_back(int16_t(std::lround(10000 * std::sin(2 * kPi * double(i) / 100))));
        s.looped = true; s.loopStart = 100; s.loopEnd = 4025;
        putBytes(root / "seam.wav", sound::writeWav(s));
        const auto a = ui::studio::labAnalyze(root / "seam.wav");
        TEST("Analyze: loop seam jump vs the wave's step, flagged", has(a.text, "Loop seam: jump") && has(a.text, "may click"));
    }

    // ── batch job with convert: conversion section ──
    {
        const auto b = run(labBatch(root / "convin", false, false, root / "convjob", true));
        TEST("batch convert job: done, MEASURED format line", b.phase == Phase::Done && has(b.reportMd, "**MEASURED** format before/after"));
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
