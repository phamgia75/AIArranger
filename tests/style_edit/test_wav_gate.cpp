// Owner gate #52 (Codex r1 on the P1 Sound Studio): WAV decoding and conversion limits, written against the API that
// already existed at da2e6db so the same file builds on the original code and shows which checks failed there.
//   - WAVE_FORMAT_EXTENSIBLE PCM decoded by container size, wValidBitsPerSample honoured (16-in-16, 16-in-24, 20-in-24,
//     24-in-32), subformat GUID checked;
//   - malformed headers and truncated data refused with a reason; untrusted rates refused before anything is resampled;
//   - the KMP Drum Kit report never calls a dropped or clamped setting EXACT.
// AIARR_DUMP_DIR=<dir>: also writes the outputs of VALID inputs there, for a byte comparison between two builds.
// Synthetic audio only.

#include "jobs/export_specs.h"
#include "media/samplelab/drum_builder.h"
#include "media/sound/additive_source.h"
#include <cfenv>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
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
static const double kPi = 3.14159265358979323846;

struct Fmt {
    uint16_t tag = 1, channels = 1, bits = 16, blockAlign = 0, valid = 0, cbSize = 22, subTag = 1;
    uint32_t rate = 48000, fmtLen = 0;
    bool ext = false, badGuid = false;
};
// A WAV whose header fields can be set one by one (blockAlign 0 = the consistent value). `data` is the raw data chunk.
static std::vector<uint8_t> wav(const Fmt& f, const std::vector<uint8_t>& data, long dataLenOverride = -1, bool smpl = true) {
    std::vector<uint8_t> b;
    auto u16 = [&](uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    auto id = [&](const char* s) { b.insert(b.end(), s, s + 4); };
    const uint16_t ba = f.blockAlign ? f.blockAlign : uint16_t(f.channels * f.bits / 8);
    id("RIFF"); u32(0); id("WAVE");
    id("fmt ");
    const size_t lenAt = b.size();
    u32(0);
    u16(f.ext ? 0xFFFE : f.tag); u16(f.channels); u32(f.rate); u32(f.rate * ba); u16(ba); u16(f.bits);
    if (f.ext) {
        u16(f.cbSize); u16(f.valid ? f.valid : f.bits); u32(f.channels == 2 ? 3 : 4);
        u16(f.subTag);
        const uint8_t tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        b.insert(b.end(), tail, tail + 14);
        if (f.badGuid) b.back() ^= 0xFF;
    }
    if (f.fmtLen) b.resize(lenAt + 4 + f.fmtLen);   // a short fmt chunk: its body really is that short
    const uint32_t fl = uint32_t(b.size() - lenAt - 4);
    std::memcpy(&b[lenAt], &fl, 4);
    if (smpl) { id("smpl"); u32(36); u32(0); u32(0); u32(1000000000u / std::max<uint32_t>(f.rate, 1)); u32(60); for (int i = 0; i < 5; ++i) u32(0); }
    id("data"); u32(dataLenOverride >= 0 ? uint32_t(dataLenOverride) : uint32_t(data.size()));
    b.insert(b.end(), data.begin(), data.end());   // the data chunk ends the file
    const uint32_t riff = uint32_t(b.size() - 8);
    std::memcpy(&b[4], &riff, 4);
    return b;
}
// Little-endian words of `bytes` each: v is the left-justified container value.
static std::vector<uint8_t> words(const std::vector<int64_t>& v, int bytes) {
    std::vector<uint8_t> o;
    for (int64_t x : v) for (int i = 0; i < bytes; ++i) o.push_back(uint8_t(uint64_t(x) >> (8 * i)));
    return o;
}
static std::vector<int64_t> tone(size_t n, double amp) {   // amp in 16-bit units
    std::vector<int64_t> v;
    for (size_t i = 0; i < n; ++i) v.push_back(std::llround(amp * std::sin(2 * kPi * 440 * double(i) / 48000)));
    return v;
}
static void putBytes(const fs::path& p, const std::vector<uint8_t>& b) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary).write((const char*)b.data(), long(b.size())); }
static JobState run(const ExportSpec& s) { JobState st; st.id = 1; runExport(s, st, [] { return false; }, [](const JobState&) {}); return st; }
static std::string decodeErr(const std::vector<uint8_t>& b) { lab::Sample s; lab::WavFormat f; bool n = false; return lab::readWavAny(b, &s, &f, &n); }

int main() {
    std::printf("Test: WAV gate (#52)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-wav-gate-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const char* dumpEnv = std::getenv("AIARR_DUMP_DIR");
    const fs::path dump = dumpEnv ? fs::path(dumpEnv) : fs::path();

    const auto t16 = tone(4800, 12000);
    Fmt plain;
    lab::Sample ref;
    sound::readWav(wav(plain, words(t16, 2)), &ref);
    auto decode = [](const std::vector<uint8_t>& b, lab::Sample* s) { lab::WavFormat f; bool n = false; return lab::readWavAny(b, s, &f, &n); };

    // ── EXTENSIBLE PCM by container, valid bits honoured ──
    {
        Fmt e; e.ext = true;
        lab::Sample s;
        TEST("EXTENSIBLE 16-in-16 with the data chunk ending the file decodes exactly", decode(wav(e, words(t16, 2)), &s).empty() && s.pcm == ref.pcm);
        std::vector<int64_t> v24;
        for (auto x : t16) v24.push_back(x * 256);
        Fmt e24 = e; e24.bits = 24; e24.valid = 16;
        TEST("EXTENSIBLE 16-in-24 (valid 16): exact, no dither", decode(wav(e24, words(v24, 3)), &s).empty() && s.pcm == ref.pcm);
        std::vector<int64_t> v32;
        for (auto x : t16) v32.push_back(x * 65536);
        Fmt e32 = e; e32.bits = 32; e32.valid = 16;
        TEST("EXTENSIBLE 16-in-32 (valid 16): exact", decode(wav(e32, words(v32, 4)), &s).empty() && s.pcm == ref.pcm);
        // 20-in-24: the 4 padding bits must be ignored (here they hold garbage)
        std::vector<int64_t> clean, dirty;
        for (size_t i = 0; i < t16.size(); ++i) { const int64_t x = t16[i] * 256 + (int64_t(i * 37) % 16) * 16; clean.push_back(x); dirty.push_back(x | (int64_t(i) % 16)); }
        Fmt e20 = e; e20.bits = 24; e20.valid = 20;
        lab::Sample a, c;
        TEST("EXTENSIBLE 20-in-24: padding bits ignored (garbage there decodes like zeros)",
             decode(wav(e20, words(clean, 3)), &a).empty() && decode(wav(e20, words(dirty, 3)), &c).empty() && a.pcm == c.pcm);
        Fmt e2432 = e; e2432.bits = 32; e2432.valid = 24;
        std::vector<int64_t> d2432;
        for (size_t i = 0; i < clean.size(); ++i) d2432.push_back(clean[i] * 256 + int64_t(i % 256));   // garbage below bit 8
        lab::Sample p;
        Fmt p24; p24.bits = 24;
        TEST("EXTENSIBLE 24-in-32 decodes like the same 24-bit PCM", decode(wav(e2432, words(d2432, 4)), &a).empty() &&
                                                                       decode(wav(p24, words(clean, 3)), &p).empty() && a.pcm == p.pcm);
        Fmt ef = e; ef.bits = 32; ef.subTag = 3;
        std::vector<uint8_t> fl;
        for (auto x : t16) { float y = float(double(x) / 32768.0); uint8_t q[4]; std::memcpy(q, &y, 4); fl.insert(fl.end(), q, q + 4); }
        TEST("EXTENSIBLE float 32 decodes within 1 LSB", decode(wav(ef, fl), &a).empty() && [&] {
            for (size_t i = 0; i < a.pcm.size(); ++i) if (std::labs(long(a.pcm[i]) - ref.pcm[i]) > 1) return false;
            return a.pcm.size() == ref.pcm.size(); }());
    }

    // ── malformed headers and data: refused with a reason ──
    {
        Fmt e; e.ext = true;
        const auto d = words(t16, 2);
        auto bad = [&](const char* name, const std::vector<uint8_t>& b, const char* why) {
            const auto err = decodeErr(b);
            TEST(std::string("refused: ") + name + " (" + err + ")", has(err, why));
        };
        Fmt x = plain; x.blockAlign = 4;
        bad("blockAlign != channels * container bytes", wav(x, d), "block align");
        x = e; x.blockAlign = 4;
        bad("EXTENSIBLE blockAlign mismatch", wav(x, d), "block align");
        x = e; x.bits = 24; x.valid = 28;
        bad("valid bits above the container", wav(x, words(t16, 3)), "valid bits");
        x = e; x.cbSize = 10;
        bad("cbSize too small", wav(x, d), "cbSize");
        x = e; x.fmtLen = 24; x.cbSize = 22;
        bad("EXTENSIBLE fmt chunk shorter than 40", wav(x, std::vector<uint8_t>(d.begin(), d.end())), "fmt");
        x = e; x.badGuid = true;
        bad("subformat GUID not PCM/float", wav(x, d), "subformat");
        x = e; x.subTag = 2;
        bad("ADPCM subformat", wav(x, d), "subformat");
        x = plain; x.channels = 0;
        bad("zero channels", wav(x, d), "mono or stereo");
        x = plain; x.rate = 0;
        bad("rate 0", wav(x, d), "sample rate");
        bad("data chunk longer than the file", wav(plain, d, long(d.size()) + 100), "overruns");
        bad("data chunk not whole frames", wav(plain, std::vector<uint8_t>(d.begin(), d.end() - 1)), "whole");
        x = e; x.bits = 24;
        bad("EXTENSIBLE 24-bit data not whole frames", wav(x, std::vector<uint8_t>(10, 0)), "whole");
        bad("not RIFF", std::vector<uint8_t>(64, 0), "RIFF");
        bad("fmt chunk too short", [&] { Fmt y = plain; y.fmtLen = 12; return wav(y, d); }(), "fmt");
    }

    // ── untrusted rates: refused before anything is allocated or resampled ──
    {
        Fmt one; one.rate = 1;
        const auto oneHz = wav(one, words(std::vector<int64_t>(1000, 100), 2));
        TEST("1 Hz WAV refused at read (rate outside the accepted input range)", has(decodeErr(oneHz), "sample rate 1 Hz"));
        Fmt hi; hi.rate = 4000000;
        TEST("4 MHz WAV refused at read", has(decodeErr(wav(hi, words(std::vector<int64_t>(100, 0), 2))), "sample rate 4000000 Hz"));
        putBytes(root / "big/one-hz.wav", oneHz);
        lab::BatchOptions o;
        o.convert = true;
        const auto b = lab::runBatch(root / "big", {}, o);
        TEST("batch convert: the 1 Hz file is not read, with the rate reason", b.items.size() == 1 && has(b.items[0].error, "sample rate 1 Hz"));
        sound::DrumKit k;
        TEST("drum kit: the 1 Hz file is refused with the rate reason", has(lab::buildDrumKit(root / "big", "K", "36 one-hz.wav\n", &k), "sample rate 1 Hz"));
        // a long file at an accepted rate whose conversion would pass the frame limit: refused, not truncated
        Fmt slow; slow.rate = 4000;
        putBytes(root / "long/slow.wav", wav(slow, words(std::vector<int64_t>(400000, 100), 2)));   // 400 000 frames -> 1 102 500 at 11025 Hz
        const auto l = lab::runBatch(root / "long", {}, o);
        TEST("batch convert: output over 1 048 576 frames refused before resampling", l.items.size() == 1 && has(l.items[0].error, "1102500 frames"));
    }

    // ── Codex r2 P2: the drum map is bounded BEFORE audio is read ──
    {
        Fmt st; st.channels = 2;
        putBytes(root / "hits/hit.wav", wav(st, words(std::vector<int64_t>(2 * 1048576, 50), 2)));   // 1 048 576 stereo frames, 4 MiB
        std::string dup;
        for (int i = 0; i < 3; ++i) dup += "36 hit.wav\n";
        sound::DrumKit k;
        const auto e = lab::buildDrumKit(root / "hits", "K", dup, &k);
        TEST("drum map: a repeated key is refused on its own line, before its audio is read (" + e + ")",
             has(e, "drum map line 2: key 36 (C2) is already on line 1"));
        std::string many;   // 4 MiB of audio + its header per file: line 32 passes the 128 MiB kit budget
        for (int i = 0; i < 33; ++i) many += std::to_string(36 + i) + " hit.wav name=hit" + std::to_string(i) + "\n";
        const auto b = lab::buildDrumKit(root / "hits", "K", many, &k);
        TEST("drum map: the cumulative audio budget is refused at the line that passes it, before reading it (" + b + ")",
             has(b, "drum map line 32: ") && has(b, "budget"));
    }

    // ── Codex r2 P2: an invalid source loop is refused, never repaired by the resampler ──
    {
        Fmt hi96; hi96.rate = 96000;
        auto w = wav(hi96, words(std::vector<int64_t>(10000, 100), 2));
        // add a smpl loop [9000, 15000): its end lies past the 10 000 frames
        const size_t at = 12 + 8 + 16;   // the smpl chunk follows a 16-byte fmt chunk
        const uint32_t loops = 1, a0 = 9000, a1 = 15000 - 1;
        std::vector<uint8_t> rec(24, 0);
        std::memcpy(&rec[8], &a0, 4); std::memcpy(&rec[12], &a1, 4);
        std::memcpy(&w[at + 8 + 28], &loops, 4);
        const uint32_t smplLen = 36 + 24;
        std::memcpy(&w[at + 4], &smplLen, 4);
        w.insert(w.begin() + long(at + 8 + 36), rec.begin(), rec.end());
        const uint32_t riff = uint32_t(w.size() - 8);
        std::memcpy(&w[4], &riff, 4);
        putBytes(root / "badloop/loop.wav", w);
        lab::BatchOptions o;
        o.convert = true;
        const auto r = lab::runBatch(root / "badloop", {}, o);
        TEST("batch convert: a source loop past the audio is refused (not clamped to the resampled end)",
             r.items.size() == 1 && has(r.items[0].error, "loop"));
    }

    // ── Codex r3 P2: float bounded before rounding; non-finite floats refused, not zeroed ──
    {
        Fmt ff; ff.tag = 3; ff.bits = 32;
        auto floats = [](std::vector<float> v) { std::vector<uint8_t> o(v.size() * 4); std::memcpy(o.data(), v.data(), o.size()); return o; };
        lab::Sample s;
        std::feclearexcept(FE_ALL_EXCEPT);
        const auto e = decode(wav(ff, floats({FLT_MAX, -FLT_MAX, 1e30f, -1e30f, 2.0f, -2.0f})), &s);
        const bool invalid = std::fetestexcept(FE_INVALID);
        TEST("float: huge finite values saturate to full scale with the right polarity, no FE_INVALID from rounding",
             e.empty() && s.pcm == std::vector<int16_t>({32767, -32768, 32767, -32768, 32767, -32768}) && !invalid);
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        TEST("float: NaN refused with a reason (not silently zeroed)", has(decodeErr(wav(ff, floats({0.1f, nan}))), "not finite"));
        TEST("float: infinity refused with a reason", has(decodeErr(wav(ff, floats({-inf, 0.1f}))), "not finite"));
    }

    // ── Codex r3 P2: an invalid source loop is refused at an unchanged rate too ──
    {
        auto loopWav = [&](uint32_t rate) {   // 10 000 frames, smpl loop [9000, 15000)
            Fmt f; f.rate = rate;
            auto w = wav(f, words(std::vector<int64_t>(10000, 100), 2));
            const size_t at = 12 + 8 + 16;
            const uint32_t loops = 1, a0 = 9000, a1 = 15000 - 1, smplLen = 36 + 24;
            std::vector<uint8_t> rec(24, 0);
            std::memcpy(&rec[8], &a0, 4); std::memcpy(&rec[12], &a1, 4);
            std::memcpy(&w[at + 8 + 28], &loops, 4);
            std::memcpy(&w[at + 4], &smplLen, 4);
            w.insert(w.begin() + long(at + 8 + 36), rec.begin(), rec.end());
            const uint32_t riff = uint32_t(w.size() - 8);
            std::memcpy(&w[4], &riff, 4);
            return w;
        };
        putBytes(root / "loop48/hit.wav", loopWav(48000));
        sound::DrumKit k;
        TEST("drum kit: a 48 kHz WAV with a loop past its audio is refused (not cleared)",
             has(lab::buildDrumKit(root / "loop48", "K", "36 hit.wav\n", &k), "loop"));
        lab::BatchOptions o;
        o.convert = true;
        const auto r = lab::runBatch(root / "loop48", {}, o);
        TEST("batch convert at an unchanged rate: the same loop is refused", r.items.size() == 1 && has(r.items[0].error, "loop"));
    }

    // ── Codex r3 P2: velocity layers are MISSING in a KMP report, EXACT where SF2/TXPROG carry them ──
    {
        const auto organ = std::make_shared<const sound::Instrument>(sound::buildDemoOrgan());   // two velocity layers
        const auto k = run(kmpFolder(organ, root / "organ-kmp"));
        bool exactVel = false, missingVel = false;
        for (const auto& [cls, item] : k.conversion) { exactVel |= cls == "EXACT" && has(item, "velocity"); missingVel |= cls == "MISSING" && has(item, "velocity layer"); }
        TEST("KMP report: velocity layers and ranges MISSING, never EXACT", k.phase == Phase::Done && !exactVel && missingVel);
        const auto p = run(soundPackage(organ, {}, "", root / "organ-pkg"));
        bool pkgVel = false;
        for (const auto& [cls, item] : p.conversion) pkgVel |= cls == "EXACT" && has(item, "velocity layer");
        TEST("Sound package (SF2 + TXPROG) report: velocity layers stay EXACT", p.phase == Phase::Done && pkgVel);
        // converted drum audio (96 kHz 24-bit) is not EXACT
        Fmt h; h.rate = 96000; h.bits = 24;
        std::vector<int64_t> v;
        for (int i = 0; i < 9600; ++i) v.push_back(int64_t(std::lround(1e6 * std::sin(i * 0.05))));
        putBytes(root / "conv-kit/c.wav", wav(h, words(v, 3)));
        const auto d = run(drumKitFromFolder(root / "conv-kit", "Conv Kit", "36 c.wav\n", false, root / "conv-kit-out"));
        bool audioExact = false;
        for (const auto& [cls, item] : d.conversion) audioExact |= cls == "EXACT" && has(item, "audio");
        TEST("Drum Kit report: converted (resampled/dithered) audio is not called EXACT", d.phase == Phase::Done && !audioExact);
    }

    // ── KMP Drum Kit report: nothing dropped or clamped is EXACT ──
    {
        Fmt m;
        putBytes(root / "kit/a.wav", wav(m, words(t16, 2)));
        putBytes(root / "kit/b.wav", wav(m, words(t16, 2)));
        sound::DrumKit kit;
        TEST("kit builds", lab::buildDrumKit(root / "kit", "Gate Kit", "36 a.wav group=G choke=1 pan=40 level=-6\n38 b.wav group=G choke=1 level=3\n", &kit).empty());
        const auto j = run(drumKitKmp(std::make_shared<const sound::DrumKit>(kit), "", root / "kit-kmp"));
        TEST("KMP job done", j.phase == Phase::Done);
        bool exactOk = true;
        for (const auto& [cls, item] : j.conversion)
            if (cls == "EXACT" && (has(item, "pan") || has(item, "exclusive") || has(item, "level") || has(item, "group"))) exactOk = false;
        TEST("KMP report: pan, exclusive groups, groups and level are never EXACT", exactOk);
        bool cut = false, boost = false;
        for (const auto& [cls, item] : j.conversion) { cut |= cls == "MISSING" && has(item, "level cut"); boost |= cls == "MISSING" && has(item, "above 0 dB"); }
        TEST("KMP report: the level cut (not written) and the clamped +3 dB are MISSING", cut && boost);
        const auto t = run(drumKit(std::make_shared<const sound::DrumKit>(kit), "", root / "kit-tx"));
        bool txExact = false;
        for (const auto& [cls, item] : t.conversion) txExact |= cls == "EXACT" && has(item, "pan");
        TEST("TXPERF report keeps pan/level EXACT (written there)", t.phase == Phase::Done && txExact);
    }

    // ── valid inputs: outputs for the before/after byte comparison ──
    if (!dump.empty()) {
        fs::remove_all(dump);
        fs::create_directories(dump);
        auto keep = [&](const std::string& name, const std::vector<uint8_t>& b) { putBytes(dump / name, b); };
        std::vector<int64_t> v8, v24, v32;
        for (auto x : t16) { v8.push_back(int64_t(std::lround(double(x) / 256)) + 128); v24.push_back(x * 256 + 77); v32.push_back(x * 65536 + 12345); }
        Fmt f8; f8.bits = 8; Fmt f24; f24.bits = 24; Fmt f32; f32.bits = 32; Fmt ff; ff.tag = 3; ff.bits = 32; Fmt e24; e24.ext = true; e24.bits = 24;
        std::vector<uint8_t> fl;
        for (auto x : t16) { float y = float(double(x) / 32768.0 * 0.9); uint8_t q[4]; std::memcpy(q, &y, 4); fl.insert(fl.end(), q, q + 4); }
        Fmt st2; st2.channels = 2; st2.bits = 24; st2.rate = 96000;
        std::vector<int64_t> stereo;
        for (size_t i = 0; i < 9600; ++i) { stereo.push_back(std::llround(2e6 * std::sin(2 * kPi * 1000 * i / 96000.0))); stereo.push_back(std::llround(1e6 * std::sin(2 * kPi * 300 * i / 96000.0))); }
        const std::vector<std::pair<std::string, std::vector<uint8_t>>> inputs = {
            {"in16", wav(plain, words(t16, 2))}, {"in8", wav(f8, words(v8, 1))}, {"in24", wav(f24, words(v24, 3))}, {"in32", wav(f32, words(v32, 4))},
            {"infloat", wav(ff, fl)}, {"inext24", wav(e24, words(v24, 3))}, {"in24-96k-stereo", wav(st2, words(stereo, 3))}};
        for (const auto& [n, b] : inputs) {
            lab::WavFormat f;
            std::string err;
            keep("to16bit-" + n + ".wav", lab::to16bit(b, &f, &err));
            putBytes(root / "valid" / (n + ".wav"), b);
        }
        lab::BatchOptions o;
        o.convert = true;
        const auto r = lab::runBatch(root / "valid", root / "valid-out", o);
        for (const auto& e : fs::recursive_directory_iterator(root / "valid-out"))
            if (e.is_regular_file() && e.path().extension() == ".wav") { std::ifstream f(e.path(), std::ios::binary); keep("batch-" + e.path().filename().string(), {std::istreambuf_iterator<char>(f), {}}); }
        sound::DrumKit kit;
        lab::buildDrumKit(root / "valid", "Dump Kit", "36 in16.wav\n38 in24.wav level=-2 pan=10\n40 in24-96k-stereo.wav group=X choke=2\n", &kit);
        for (const auto& s : kit.samples) keep("drum-" + s.name + ".wav", sound::writeWav(s));
        std::printf("  info: dumped %zu files to %s (batch error: '%s')\n", size_t(std::distance(fs::directory_iterator(dump), fs::directory_iterator{})),
                    dump.string().c_str(), r.error.c_str());
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
