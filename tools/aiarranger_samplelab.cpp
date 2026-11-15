// aiarranger-samplelab (roadmap E4): offline Sample Lab jobs on 16-bit PCM WAVs (media/samplelab/). Sources are only
// read; output goes to a NEW folder (DIR.INCOMPLETE + rename, PACKAGE.sha256) and is refused on a mounted volume.
//   analyze --in FILE.wav                       levels, onsets, pitch, loop (read-only)
//   slice --in LOOP.wav --name NAME (--bpm N | --bars N [--beats 4]) [--base 36] [--rise 10] --out DIR
//                                               Time Slice: wav/ per slice on keys from --base, NAME.mid, SLICES.tsv
//   batch --in SRC [--trim -60] [--normalize peak|rms --target -1 --headroom 1] [--near 0.98] [--out DIR]
//                                               every .wav in SRC; without --out only the report (duplicates included)
//   loop --in FILE.wav --start F --end F [--snap R] [--xfade N] --out DIR
//                                               zero-crossing snap (radius R frames) and/or crossfade loop
// Exit 0 = done, 1 = usage/IO/validation error.
#include "media/samplelab/sample_lab_io.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

using namespace ai_arranger::media::samplelab;
namespace sound = ai_arranger::media::sound;

static int usage() {
    std::fprintf(stderr, "usage: aiarranger-samplelab analyze --in FILE.wav\n"
                         "       aiarranger-samplelab slice --in LOOP.wav --name NAME (--bpm N | --bars N [--beats 4]) [--base 36] [--rise 10] --out DIR\n"
                         "       aiarranger-samplelab batch --in SRC [--trim -60] [--normalize peak|rms --target -1 --headroom 1] [--near 0.98] [--out DIR]\n"
                         "       aiarranger-samplelab loop --in FILE.wav --start F --end F [--snap R] [--xfade N] --out DIR\n"
                         "       (DIR must not exist)\n");
    return 1;
}

static bool num(const std::map<std::string, std::string>& o, const char* k, double* v) {
    const auto it = o.find(k);
    if (it == o.end()) return true;
    char* end = nullptr;
    *v = std::strtod(it->second.c_str(), &end);
    return end && !*end && std::isfinite(*v);
}

int main(int argc, char** argv) {
    if (argc < 2 || argc % 2) return usage();
    const std::string cmd = argv[1];
    std::map<std::string, std::string> opt;
    for (int i = 2; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
    if (!opt.count("--in")) return usage();
    std::string err;
    auto fail = [&](const std::string& m) { std::fprintf(stderr, "%s\n", m.c_str()); return 1; };

    if (cmd == "batch") {
        BatchOptions o;
        double trim = 1, near = o.nearThreshold;
        if (!num(opt, "--trim", &trim) || !num(opt, "--target", &o.targetDb) || !num(opt, "--headroom", &o.headroomDb) || !num(opt, "--near", &near))
            return usage();
        if (opt.count("--trim")) { o.trim = true; o.trimDb = trim; }
        o.nearThreshold = near;
        if (opt.count("--normalize")) {
            if (opt["--normalize"] != "peak" && opt["--normalize"] != "rms") return usage();
            o.normalize = true;
            o.mode = opt["--normalize"] == "peak" ? NormMode::Peak : NormMode::Rms;
        }
        const BatchResult r = runBatch(opt["--in"], opt.count("--out") ? opt["--out"] : "", o);
        if (!r.error.empty()) return fail(r.error);
        std::fputs(r.report.c_str(), stdout);
        return 0;
    }

    Sample s;
    if (!readWavFile(opt["--in"], &s, &err)) return fail(err);
    if (cmd == "analyze" && opt.size() == 1) {
        const Level l = measureLevel(s);
        std::printf("%s: %zu frames, %u Hz, %u ch, peak %.2f dBFS, RMS %.2f dBFS\n", opt["--in"].c_str(), s.frames(), s.rate, s.channels, l.peakDb, l.rmsDb);
        const auto on = detectOnsets(s);
        std::printf("onsets (%zu):", on.size());
        for (uint32_t f : on) std::printf(" %u (%.1f ms)", f, 1000.0 * f / s.rate);
        std::printf("\n");
        if (s.looped) std::printf("loop %u-%u\n", s.loopStart, s.loopEnd);
        return 0;
    }
    if (!opt.count("--out")) return usage();
    if (cmd == "slice" && opt.count("--name")) {
        SliceOptions o;
        double base = o.baseNote, bars = 0, beats = 4;
        if (!num(opt, "--bpm", &o.bpm) || !num(opt, "--bars", &bars) || !num(opt, "--beats", &beats) || !num(opt, "--base", &base) ||
            !num(opt, "--rise", &o.onsets.riseDb) || base < 0 || base > 127)
            return usage();
        if (opt.count("--bars") == opt.count("--bpm")) return fail("give exactly one of --bpm or --bars");
        if (bars > 0) o.bpm = bars * beats * 60.0 * s.rate / s.frames();   // the loop is exactly that many bars
        o.baseNote = uint8_t(base);
        const SliceResult r = timeSlice(s, opt["--name"], o);
        if (!r.error.empty()) return fail(r.error);
        const std::string rep = writeSliceFolder(r, o, opt["--in"], opt["--out"], &err);
        if (rep.empty()) return fail(err);
        std::fputs(rep.c_str(), stdout);
        return 0;
    }
    if (cmd == "loop" && opt.count("--start") && opt.count("--end")) {
        double a = 0, b = 0, snap = 0, xf = 0;
        if (!num(opt, "--start", &a) || !num(opt, "--end", &b) || !num(opt, "--snap", &snap) || !num(opt, "--xfade", &xf) || a < 0 || b < 0 || snap < 0 || xf < 0)
            return usage();
        if (!(a < b && b <= s.frames()) || b - a < 32) return fail("loop outside the sample or shorter than 32 frames");
        s.looped = true;
        s.loopStart = uint32_t(a);
        s.loopEnd = uint32_t(b);
        if (snap > 0) if (const auto e = snapLoop(s, uint32_t(snap)); !e.empty()) return fail(e);
        if (xf > 0) if (const auto e = crossfadeLoop(s, s.loopStart, s.loopEnd, uint32_t(xf)); !e.empty()) return fail(e);
        const std::string file = fs::path(opt["--in"]).filename().string();
        char rep[512];
        std::snprintf(rep, sizeof rep, "AIArranger Sample Lab: loop (SOFTWARE evidence only)\n\nSource: %s\nLoop: %u-%u (asked %.0f-%.0f), snap radius %.0f, crossfade %.0f frames\n",
                      opt["--in"].c_str(), s.loopStart, s.loopEnd, a, b, snap, xf);
        if (const auto e = writeLabFolder(opt["--out"], {{"wav/" + file, sound::writeWav(s)}, {"REPORT.txt", bytesOf(rep)}}); !e.empty()) return fail(e);
        std::fputs(rep, stdout);
        return 0;
    }
    return usage();
}
