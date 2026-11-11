#ifndef AI_ARRANGER_MEDIA_SOUND_KSF_FOLDER_SOURCE_H
#define AI_ARRANGER_MEDIA_SOUND_KSF_FOLDER_SOURCE_H

// A folder of Korg .KSF samples WITHOUT the .KMP that mapped them (e.g. a sample library shipped as loose KSFs).
// The KSF itself is read exactly as in korg_kmp_source.h (KRONOS Parameter Guide En 3 p.1159): audio, rate, Start..End,
// loop. A KSF holds no root key, key range or velocity: those were in the KMP / Program. So the root is MEASURED from
// the audio (YIN, then the DTFT peak of measurePitch) and compared with the note number a sample name may end in
// (a hint, never trusted alone); key zones are PROPOSED (each key plays the nearest root). The proposal is plain text
// the musician reviews and edits; the build uses exactly that text. One velocity layer: there is no evidence of more.
// Reads only; the source folder is never modified.

#include "media/sound/korg_kmp_source.h"
#include "media/sound/rendered_source.h"   // measurePitch
#include "media/sound/txprog_writer.h"     // txw::noteName
#include <cmath>
#include <sstream>

namespace ai_arranger::media::sound {

// .KSF files and no .KMP (a folder with a KMP is a multisample folder: korg_kmp_source.h).
inline bool isKsfFolder(const std::filesystem::path& dir) {
    std::error_code ec;
    bool ksf = false;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const auto x = korg::lower(e.path().extension().string());
        if (x == ".kmp") return false;
        ksf |= x == ".ksf";
    }
    return ksf;
}

// YIN fundamental (de Cheveigne & Kawahara 2002, threshold 0.15) of one W-frame window at x, from rate/tMax (default
// 25 Hz) up to rate/2 (lag 2: the normalised difference keeps low notes from dipping there). 0 = no periodicity found.
// x must hold W + tMax frames.
inline double yinHz(const int16_t* x, uint32_t rate, size_t W = 2048, size_t tMax = 0) {
    if (!tMax) tMax = rate / 25;
    const size_t tMin = 2;
    std::vector<double> d(tMax + 1, 0.0);
    for (size_t t = 1; t <= tMax; ++t)
        for (size_t i = 0; i < W; ++i) { const double v = double(x[i]) - x[i + t]; d[t] += v * v; }
    double run = 0;
    for (size_t t = 1; t <= tMax; ++t) { run += d[t]; d[t] = run > 0 ? d[t] * double(t) / run : 1; }
    for (size_t t = std::max<size_t>(tMin, 2); t < tMax; ++t) {
        if (d[t] >= 0.15) continue;
        while (t + 1 < tMax && d[t + 1] < d[t]) ++t;
        const double a = d[t - 1], b = d[t], c = d[t + 1], den = a - 2 * b + c;
        return rate / (double(t) + (den > 0 ? 0.5 * (a - c) / den : 0));
    }
    return 0;
}

// Pitch over time, for vibrato. YIN every 5 ms (point i at frame i * rate/200) with a window of 3 periods (at least
// 512 frames, at most 2048): a long window averages the louder half of each vibrato cycle (a synthetic +-40 cent vibrato
// with tied loudness read +4.5 cents with 2048 frames, +0.4..1.1 with 512). Cents relative to refHz; NAN where no
// pitch was found within 150 cents or the window would run past the audio.
inline std::vector<double> pitchPoints(const std::vector<int16_t>& x, uint32_t rate, double refHz) {
    const double period = rate / refHz;
    const size_t W = std::clamp<size_t>(size_t(3 * period), 512, 2048), tMax = std::min<size_t>(rate / 25, size_t(2 * period) + 2);
    std::vector<double> c;
    for (size_t p = 0; p + W + tMax <= x.size(); p += std::max<uint32_t>(1, rate / 200)) {
        const double f = yinHz(&x[p], rate, W, tMax);
        const double v = f > 0 ? 1200 * std::log2(f / refHz) : NAN;
        c.push_back(std::fabs(v) <= 150 ? v : NAN);
    }
    return c;
}
// Fills runs of at most maxRun NAN points by linear interpolation between their neighbours (circular: the run at the end
// joins the start, as a loop does). Used for the few points next to a loop seam, whose windows would straddle it.
inline void fillGaps(std::vector<double>& v, size_t maxRun, bool circular) {
    const size_t n = v.size();
    std::vector<size_t> ok;
    for (size_t i = 0; i < n; ++i) if (!std::isnan(v[i])) ok.push_back(i);
    if (ok.size() < 2) return;
    for (size_t j = 0; j + (circular ? 0 : 1) < ok.size(); ++j) {
        const size_t a = ok[j], b = j + 1 < ok.size() ? ok[j + 1] : ok[0] + n, run = b - a - 1;
        if (run == 0 || run > maxRun) continue;
        for (size_t i = a + 1; i < b; ++i) v[i % n] = v[a] + (v[b % n] - v[a]) * double(i - a) / double(b - a);
    }
}

struct PitchTrack { double mean = 0, sd = 0, p10 = 0, p90 = 0, vibHz = 0, cycleSe = 0; size_t n = 0; };
inline PitchTrack summarise(std::vector<double> c) {   // c: points 5 ms apart
    PitchTrack t;
    c.erase(std::remove_if(c.begin(), c.end(), [](double v) { return std::isnan(v); }), c.end());
    t.n = c.size();
    if (c.empty()) return t;
    for (double v : c) t.mean += v;
    t.mean /= c.size();
    for (double v : c) t.sd += (v - t.mean) * (v - t.mean);
    t.sd = std::sqrt(t.sd / c.size());
    size_t zc = 0;
    for (size_t i = 1; i < c.size(); ++i) zc += (c[i] - t.mean) * (c[i - 1] - t.mean) < 0;
    t.vibHz = zc / 2.0 / (c.size() / 200.0);
    // standard error of the mean from whole vibrato cycles (their means vary far less than the points do)
    const size_t per = t.vibHz > 0.5 ? size_t(std::lround(200 / t.vibHz)) : c.size();
    std::vector<double> cm;
    for (size_t i = 0; per && i + per <= c.size(); i += per) { double m = 0; for (size_t j = i; j < i + per; ++j) m += c[j]; cm.push_back(m / per); }
    if (cm.size() >= 2) {
        double m = 0, q = 0;
        for (double v : cm) m += v;
        m /= cm.size();
        for (double v : cm) q += (v - m) * (v - m);
        t.cycleSe = std::sqrt(q / (cm.size() - 1) / cm.size());
    } else {
        t.cycleSe = t.sd;
    }
    std::sort(c.begin(), c.end());
    t.p10 = c[c.size() / 10];
    t.p90 = c[c.size() * 9 / 10];
    return t;
}

struct KsfInfo {
    std::string file, name;   // file name; SMP1 sample name
    std::string refused;      // non-empty: cannot be converted, and why
    KsfHead head;
    int hint = -1;            // note number the sample name ends in (1-3 digits, <= 127); a hint only
    int detected = -1;        // nearest note to the measured fundamental; -1 = none (see why)
    double cents = 0;         // measured pitch relative to `detected`: the vibrato centre, or the spectral peak of a steady tone
    std::string why;
    // How `cents` was measured (detected >= 0): over the note as it plays, 0.5-2.0 s after note-on with the loop repeated
    // (a one-shot: from a quarter in to its end).
    bool vibrato = false;     // the pitch moves (10th..90th percentile >= 8 cents apart): cents = mean of the pitch track
    double moveSd = 0, moveLo = 0, moveHi = 0, vibHz = 0;   // spread; 10th..90th percentile (cents re detected); cycles per s
    double confidence = 0;    // +- cents (about 95 %): 2 standard errors of the per-cycle means, plus 1 cent method bias
    double loopCents = NAN;   // the loop alone, repeated (what a long-held note settles to); NAN = no loop
};

using txw::noteName;   // C-1 = 0, C4 = 60
// Untrusted names go into the mapping text, whose lines are parsed: control characters become '?'.
inline std::string printable(std::string t) { for (auto& c : t) if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?'; return t; }
inline std::string padTo(std::string t, size_t w) { return t.size() < w ? t + std::string(w - t.size(), ' ') : t; }
// "G#3", "C-1" (C4 = 60) or a number 0-127; -1 if neither.
inline int parseNote(const std::string& t) {
    if (!t.empty() && t.size() <= 3 && t.find_first_not_of("0123456789") == std::string::npos) { const int v = std::stoi(t); return v <= 127 ? v : -1; }
    static const int base[] = {9, 11, 0, 2, 4, 5, 7};   // A..G
    if (t.size() < 2 || t[0] < 'A' || t[0] > 'G') return -1;
    int k = base[t[0] - 'A'];
    size_t i = 1;
    if (t[i] == '#') { ++k; ++i; }
    const std::string o = t.substr(i);
    if (o != "-1" && (o.size() != 1 || !std::isdigit(static_cast<unsigned char>(o[0])))) return -1;
    const int v = (std::stoi(o) + 1) * 12 + k;
    return v >= 0 && v <= 127 ? v : -1;
}

// Decodes one KSF's bytes and measures its pitch. s (optional) receives the decoded audio.
inline KsfInfo analyseKsf(const std::string& file, const std::vector<uint8_t>& b, Sample* s = nullptr) {
    KsfInfo k;
    k.file = file;
    Sample tmp;
    Sample& a = s ? *s : tmp;
    if (const auto e = readKsf(b, &k.head, &a); !e.empty()) { k.refused = e; return k; }
    // the Pa5X range, and what the analysis below needs (its 5 ms step is rate / 200 frames)
    if (a.rate < 11025 || a.rate > 48000) { k.refused = "rate outside 11025-48000 Hz"; return k; }
    korg::walk(b, [&](const std::string& id, size_t o, size_t) { if (id == "SMP1") k.name = printable(korg::text(b, o, 16)); return std::string(); });
    size_t d = k.name.size();
    while (d && std::isdigit(static_cast<unsigned char>(k.name[d - 1]))) --d;
    if (d < k.name.size() && k.name.size() - d <= 3 && std::stoi(k.name.substr(d)) <= 127) k.hint = std::stoi(k.name.substr(d));
    // Steady part: the loop when it is long enough, else from a quarter in (past the attack); at most 32768 frames.
    const size_t n = a.frames(), need = 2048 + a.rate / 25;
    size_t from = n / 4, len = n - n / 4;
    if (a.looped && a.loopEnd - a.loopStart >= need) { from = a.loopStart; len = a.loopEnd - a.loopStart; }
    len = std::min<size_t>(len, 32768);
    if (len < need) { k.why = "too short to measure (" + std::to_string(len) + " frames)"; return k; }
    std::vector<double> hz;   // five windows across the region: the note must not change between them
    for (int w = 0; w < 5; ++w) if (const double f = yinHz(&a.pcm[from + (len - need) * w / 4], a.rate); f > 0) hz.push_back(f);
    if (hz.size() < 3) { k.why = "no steady pitch (unpitched or noisy)"; return k; }
    std::sort(hz.begin(), hz.end());
    const double med = hz[hz.size() / 2];
    if (1200 * std::log2(hz.back() / hz.front()) > 100) { k.why = "pitch moves by more than a semitone"; return k; }
    int note = int(std::lround(69 + 12 * std::log2(med / 440)));
    if (note < 0 || note > 127) { k.why = "pitch outside the MIDI range"; return k; }
    const double f = measurePitch(&a.pcm[from], len, a.rate, 440 * std::pow(2.0, (note - 69) / 12.0));
    if (f <= 0) { k.why = "too short to measure"; return k; }
    if (std::fabs(1200 * std::log2(f / med)) > 50) { k.why = "spectrum and period disagree (unsure pitch)"; return k; }
    // The note as a held key plays it: the sample, then its loop repeated, 0.5-2.0 s after note-on (a one-shot: from a
    // quarter in to its end). The pitch is tracked on the source audio and looked up through the playback position, so
    // no window straddles the loop seam; the few points next to it (a window's length) are interpolated across it.
    const uint32_t R = a.rate;
    const size_t hop = R / 200, LS = a.loopStart, LE = a.loopEnd;
    const double ref = 440 * std::pow(2.0, (note - 69) / 12.0), per = R / ref;
    const size_t seamRun = (std::clamp<size_t>(size_t(3 * per), 512, 2048) + std::min<size_t>(R / 25, size_t(2 * per) + 2)) / hop + 2;
    const std::vector<int16_t> src(a.pcm.begin(), a.pcm.begin() + std::ptrdiff_t(a.looped ? LE : n));
    const auto pts = pitchPoints(src, R, ref);
    const auto at = [&](size_t q) { return q / hop < pts.size() ? pts[q / hop] : NAN; };
    std::vector<double> held, loopOnly;
    size_t w0 = R / 2, w1 = 2 * size_t(R), seg0 = w0;   // seg0: where the window's audio starts in the source
    if (a.looped) {
        for (size_t p = w0; p < w1; p += hop) held.push_back(at(p < LE ? p : LS + (p - LE) % (LE - LS)));
        for (size_t p = LS; p < LE; p += hop) loopOnly.push_back(at(p));
    } else {
        w0 = seg0 = n / 4; w1 = n;
        for (size_t p = w0; p < w1; p += hop) held.push_back(at(p));
    }
    if (a.looped && LE <= w0) seg0 = LS;
    fillGaps(held, seamRun, false);
    fillGaps(loopOnly, seamRun, true);
    const PitchTrack t = summarise(held);
    double centre = 1200 * std::log2(f / ref);   // spectral peak of the steady part (above)
    if (t.n >= 20 && t.p90 - t.p10 >= 8) {   // percentiles: a few points at a rough loop seam do not make a tone "vibrato"
        k.vibrato = true;
        centre = t.mean;
        k.confidence = 2 * t.cycleSe + 1;
    } else {   // steady: the spectral peak of the window's source audio (no seam), at most 2 s
        const size_t e = a.looped ? LE : n;
        if (e > seg0 + need)
            if (const double g = measurePitch(&a.pcm[seg0], std::min<size_t>(e - seg0, 2 * size_t(R)), R, ref); g > 0) centre = 1200 * std::log2(g / ref);
        k.confidence = 1;
    }
    k.moveSd = t.sd;
    if (a.looped) {
        const PitchTrack tl = summarise(loopOnly);
        if (tl.n >= 10) k.loopCents = tl.mean;
    }
    const double exact = note + centre / 100;
    k.detected = int(std::lround(exact));
    k.cents = 100 * (exact - k.detected);
    const double shift = 100.0 * (note - k.detected);   // track figures were relative to the coarse note
    k.moveLo = t.p10 + shift;
    k.moveHi = t.p90 + shift;
    k.vibHz = t.vibHz;
    if (!std::isnan(k.loopCents)) k.loopCents += shift;
    return k;
}
inline KsfInfo analyseKsf(const std::filesystem::path& p, Sample* s = nullptr) {
    std::string err;
    const auto b = korg::slurp(p, &err);
    if (!err.empty()) { KsfInfo k; k.file = p.filename().string(); k.refused = err; return k; }
    return analyseKsf(p.filename().string(), b, s);
}

// Proposed mapping for every KSF in dir, as editable text: "<file> <root> <low> <high>  # evidence"; '#' at the start
// of a line or after a space starts a comment. Unusable files appear as comment lines with the reason. "" on success.
inline std::string proposeKsfMapping(const std::filesystem::path& dir, std::string* text) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (korg::lower(e.path().extension().string()) == ".ksf" && e.is_regular_file(ec)) files.push_back(e.path());
    if (ec) return "cannot read " + dir.string();
    if (files.empty()) return "no .KSF files in " + dir.string();
    std::sort(files.begin(), files.end());
    struct Use { KsfInfo k; int root, tune; std::string ev; };
    std::vector<Use> use;
    std::string skipped;
    std::map<int, std::string> byRoot;
    for (const auto& p : files) {
        Sample a;
        KsfInfo k = analyseKsf(p, &a);
        const std::string fn = k.file;
        if (fn.empty() || fn.front() == ' ' || fn.front() == '#' || fn.back() == ' ' || fn.find(" #") != std::string::npos || printable(fn) != fn)
            k.refused = "this file name cannot be written in a mapping line (leading/trailing space or '#', control character, or \" #\"); use a renamed copy";
        if (k.refused.empty()) {   // the Pa5X sample limits: a sample the build must refuse is not proposed
            std::vector<std::string> lim;
            a.name = "x";
            validateSample(a, 0, lim);
            if (!lim.empty()) k.refused = lim.front().substr(lim.front().find(": ") + 2);
        }
        k.file = printable(k.file);
        if (!k.refused.empty()) { skipped += "# " + k.file + "  NOT USED: " + k.refused + "\n"; continue; }
        std::string ev, hint = k.hint >= 0 ? "name " + k.name + " ends in " + std::to_string(k.hint) + " (" + noteName(k.hint) + ")" : "";
        int root = k.detected, tune = 0;
        if (root >= 0) {
            tune = -int(std::lround(k.cents));
            char m[96];
            std::snprintf(m, sizeof m, "measured %s %+.0f cents +-%.0f%s", noteName(root).c_str(), k.cents, std::ceil(k.confidence), k.vibrato ? " (vibrato centre)" : "");
            ev = m;
            if (k.hint >= 0) ev += k.hint == root ? "; " + hint + ": agrees" : "; " + hint + ": DISAGREES, check by ear";
        } else if (k.hint >= 0) {
            root = k.hint;
            ev = "root from the sample name only, NOT verified (" + k.why + "); " + hint;
        } else {
            skipped += "# " + k.file + "  NOT USED: no root (" + k.why + ", and the sample name has no note number)\n";
            continue;
        }
        if (byRoot.count(root)) {   // a second sample at one root: a velocity layer or alternate, which a KSF cannot tell
            skipped += "# " + k.file + "  NOT USED: same root " + noteName(root) + " as " + byRoot[root] +
                       " (a velocity layer or alternate? not recorded in a KSF)\n";
            continue;
        }
        byRoot[root] = k.file;
        use.push_back({std::move(k), root, tune, ev});
    }
    if (use.empty()) return "no KSF in " + dir.string() + " can be used:\n" + skipped;
    std::sort(use.begin(), use.end(), [](const Use& a, const Use& b) { return a.root < b.root; });
    std::string o =
        "# KSF samples without a .KMP: the original key map is not in these files.\n"
        "# root = MEASURED pitch of each sample (checked against a note number in its name); low/high = PROPOSED zones,\n"
        "# each key plays the nearest root (ties to the higher). tune = cents that correct the measured pitch to the root\n"
        "# (optional column; 0 plays the recording as it is). Not the original mapping. Root, zones and tune are\n"
        "# estimates: change any of them here (e.g. a root by ear, or tune +0), then press Preview.\n"
        "# One velocity layer: velocity layers cannot be recovered without the KMP and Program.\n"
        "# file           root  low   high  tune  # evidence\n";
    for (size_t i = 0; i < use.size(); ++i) {
        const int lo = i == 0 ? 0 : (use[i - 1].root + use[i].root - 1) / 2 + 1;
        const int hi = i + 1 == use.size() ? 127 : (use[i].root + use[i + 1].root - 1) / 2;
        const auto pad = padTo;   // no fixed buffer: long names stay whole
        o += pad(use[i].k.file, 16) + " " + pad(noteName(use[i].root), 5) + " " + pad(noteName(lo), 5) + " " + pad(noteName(hi), 5) + " " +
             pad((use[i].tune < 0 ? "-" : "+") + std::to_string(std::abs(use[i].tune)), 4) + "  # " + use[i].ev + "\n";
    }
    *text = o + skipped;
    return "";
}

struct KsfLine { std::string file; int root, lo, hi, tune = 0; };
// A line is <file> <root> <low> <high> [tune]; the tune carries its sign (+16, -5, +0), so the columns are read from the
// right and everything before them is the file name, spaces included.
inline std::string parseKsfMapping(const std::string& text, std::vector<KsfLine>& out) {
    out.clear();
    std::istringstream is(text);
    int n = 0;
    for (std::string line; std::getline(is, line);) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (size_t c = 0; c < line.size(); ++c)   // a comment is '#' at the start or after a space ("G#3" is a key)
            if (line[c] == '#' && (c == 0 || line[c - 1] == ' ' || line[c - 1] == '\t')) { line.resize(c); break; }
        std::vector<std::pair<size_t, size_t>> t;   // token start, end
        for (size_t c = 0; c < line.size();) {
            if (line[c] == ' ' || line[c] == '\t') { ++c; continue; }
            const size_t b = c;
            while (c < line.size() && line[c] != ' ' && line[c] != '\t') ++c;
            t.push_back({b, c});
        }
        if (t.empty()) continue;
        const auto tok = [&](size_t i) { return line.substr(t[i].first, t[i].second - t[i].first); };
        const std::string at = "mapping line " + std::to_string(n) + ": ";
        const bool tuned = line[t.back().first] == '+' || line[t.back().first] == '-';
        const size_t cols = tuned ? 4 : 3;
        if (t.size() < cols + 1) return at + "needs <file> <root> <low> <high> [tune with sign] (e.g. MS000000.KSF G#3 C-1 G#3 +16)";
        const size_t r = t.size() - cols;
        KsfLine k{line.substr(t[0].first, t[r - 1].second - t[0].first), parseNote(tok(r)), parseNote(tok(r + 1)), parseNote(tok(r + 2))};
        if (k.file.find_first_of("/\\") != std::string::npos) return at + "a file name, not a path";
        if (k.root < 0 || k.lo < 0 || k.hi < 0) return at + "keys are note names (C-1..G9, C4 = 60) or numbers 0-127";
        if (k.lo > k.hi) return at + "low key above high key (a tune needs its sign: +5, -5)";
        if (tuned) {
            const std::string v = tok(r + 3);
            if (v.size() < 2 || v.size() > 3 || v.find_first_not_of("0123456789", 1) != std::string::npos)
                return at + "tune is whole cents from -99 to +99, with its sign";
            k.tune = std::stoi(v.substr(1)) * (v[0] == '-' ? -1 : 1);
        }
        out.push_back(k);
    }
    if (out.empty()) return "the mapping has no sample lines";
    return "";
}

// One layer `layer` (velocity 0-127) from the mapping text. "" on success. notes (optional) receives the REPORT.txt
// source section, measured on the same bytes that were decoded (not re-read later, when the files may have changed).
inline std::string buildFromKsfMapping(const std::filesystem::path& dir, const std::string& name, const std::string& layer,
                                       const std::string& mapping, Instrument* out, std::string* notes = nullptr) {
    std::vector<KsfLine> lines;
    if (const auto e = parseKsfMapping(mapping, lines); !e.empty()) return e;
    Instrument in;
    in.name = name;
    Layer L;
    L.name = layer;
    std::error_code ec;
    std::string rows;
    struct Q { int lo, hi; std::string file; double level, bright, seam, cycles; bool looped; };
    std::vector<Q> q;
    size_t lt = 0, p12 = 0, s2 = 0, vib = 0;
    for (const auto& m : lines) {
        std::vector<std::filesystem::path> hits;   // FAT is case-insensitive; two files differing only in case are refused
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (korg::lower(e.path().filename().string()) == korg::lower(m.file) && e.is_regular_file(ec)) hits.push_back(e.path());
        if (hits.size() != 1) return m.file + (hits.empty() ? ": not in the folder" : ": several files differ only in case");
        std::string err;
        const auto b = korg::slurp(hits.front(), &err);
        if (!err.empty()) return err;
        Sample s;
        const KsfInfo k = analyseKsf(m.file, b, &s);
        if (!k.refused.empty()) return m.file + ": " + k.refused;
        char l[200];
        const std::string tune = std::string(m.tune < 0 ? "-" : "+") + std::to_string(std::abs(m.tune));
        rows += "  " + padTo(printable(m.file), 16) + " " + padTo(k.name, 16) + " keys " + padTo(noteName(m.lo) + "-" + noteName(m.hi), 9) + " root " +
                padTo(noteName(m.root), 4) + " tune " + padTo(tune, 3) + "\n      ";
        if (k.detected >= 0) {
            std::snprintf(l, sizeof l, "measured %s %+.1f cents +-%.1f", noteName(k.detected).c_str(), k.cents, k.confidence);
            rows += l;
            if (k.vibrato) {
                std::snprintf(l, sizeof l, " (vibrato centre; swings %+.0f..%+.0f at %.1f Hz", k.moveLo, k.moveHi, k.vibHz);
                rows += l;
                if (!std::isnan(k.loopCents)) { std::snprintf(l, sizeof l, "; a long-held note settles to %+.1f", k.loopCents); rows += l; }
                rows += ")";
                ++vib;
            } else {
                rows += " (steady)";
            }
            const int correction = -int(std::lround(k.cents));
            rows += k.detected != m.root ? "; ROOT SET BY YOU" : "; root as measured";
            rows += m.tune == correction ? ", tune = the measured correction" : ", TUNE SET BY YOU (measured correction " +
                                                                     std::string(correction < 0 ? "-" : "+") + std::to_string(std::abs(correction)) + ")";
            std::snprintf(l, sizeof l, "; plays %+.1f cents from %s", 100.0 * (k.detected - m.root) + k.cents + m.tune, noteName(m.root).c_str());
            rows += l;
        } else {
            rows += "pitch not measured (" + k.why + "); root as given";
        }
        if (k.hint >= 0) rows += k.hint == m.root ? "; name agrees" : "; name says " + noteName(k.hint);
        rows += k.head.loopOn ? "; looped\n" : "; one-shot\n";
        lt += k.head.loopOn && k.head.loopTune != 0;
        p12 += k.head.plus12dB;
        s2 += k.head.use2ndStart && k.head.start2 != k.head.start;
        // what the quality checks compare: the sustained part (the loop, or a one-shot from a quarter in)
        const size_t a0 = s.looped ? s.loopStart : s.frames() / 4, a1 = s.looped ? s.loopEnd : s.frames();
        double e2 = 0, d2 = 0, xy = 0, xx = 0, yy = 0;
        for (size_t f = a0 + 1; f < a1; ++f) { const double v = s.pcm[f], d = v - s.pcm[f - 1]; e2 += v * v; d2 += d * d; }
        if (s.looped && s.loopStart >= 256)
            for (size_t f = 0; f < 256; ++f) { const double u = s.pcm[s.loopEnd - 256 + f], v = s.pcm[s.loopStart - 256 + f]; xy += u * v; xx += u * u; yy += v * v; }
        q.push_back({m.lo, m.hi, printable(m.file), 10 * std::log10(e2 / std::max<size_t>(1, a1 - a0) / 1073741824.0 + 1e-20),
                     10 * std::log10(d2 / (e2 + 1e-9) + 1e-20), xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 1.0,
                     k.vibrato ? k.vibHz * double(a1 - a0) / s.rate : 99.0, s.looped});
        s.root = uint8_t(m.root);
        s.name = layer.substr(0, 16) + " " + std::to_string(in.samples.size() + 1);
        L.zones.push_back({uint8_t(m.lo), uint8_t(m.hi), int(in.samples.size()), uint8_t(m.root), int8_t(m.tune), 0});
        in.samples.push_back(std::move(s));
    }
    std::sort(L.zones.begin(), L.zones.end(), [](const Zone& a, const Zone& b) { return a.lo < b.lo; });
    in.layers.push_back(std::move(L));
    if (const auto e = validate(in); !e.empty()) return e.front();
    std::string o = "Source: Korg .KSF samples WITHOUT a .KMP in " + dir.string() +
                    "\nFormat: KSF per KRONOS Parameter Guide En 3 p.1159 (Korg format sample files)\n"
                    "A KSF is one recording. Which keys play it, how loud it is beside its neighbours and which velocities "
                    "play it were in the KMP and Program, which this folder does not have.\n\n"
                    "EXACT (copied from the KSF files unchanged): the 16-bit audio from Start to End, the sample rate, the loop "
                    "(Loop Start..End), mono.\n"
                    "MEASURED (from the audio; +- is a 95 % range): the pitch of each sample. With vibrato it is the centre of "
                    "the pitch, averaged 0.5-2.0 s into the note as it plays (loop repeated).\n"
                    "ESTIMATED (proposed by AIArranger; change them in the mapping): the root key of each sample, the key "
                    "zones (each key plays the nearest root), the tune correction, one velocity layer 0-127.\n"
                    "MISSING (needs the original KMP and Program): the original key zones, zone tune and level, velocity "
                    "layers, filters, envelopes, LFOs, effects.\n";
    if (lt) o += "  " + std::to_string(lt) + " sample(s) have Loop Tune: LOST\n";
    if (p12) o += "  " + std::to_string(p12) + " sample(s) have +12 dB: not carried (set the level on the Pa5X)\n";
    if (s2) o += "  " + std::to_string(s2) + " sample(s) have a 2nd Start in use: not carried\n";
    o += "\nPer sample:\n" + rows;
    // QUALITY: neighbours in key order, loop seams, short vibrato loops. Measured, so the musician knows where to listen.
    std::sort(q.begin(), q.end(), [](const Q& a, const Q& b) { return a.lo < b.lo; });
    std::string qo;
    char l[300];
    for (size_t i = 0; i < q.size(); ++i) {
        const std::string keys = noteName(q[i].lo) + "-" + noteName(q[i].hi) + " (" + q[i].file + ")";
        if (i) {
            const double dl = q[i].level - q[i - 1].level, db = q[i].bright - q[i - 1].bright;
            if (std::fabs(dl) > 3) { std::snprintf(l, sizeof l, "  keys %s are %.1f dB %s than the keys below: zone levels were in the KMP; adjust by ear\n", keys.c_str(), std::fabs(dl), dl > 0 ? "louder" : "quieter"); qo += l; }
            if (std::fabs(db) > 5) { std::snprintf(l, sizeof l, "  keys %s sound %.1f dB %s than the keys below: the timbre changes there\n", keys.c_str(), std::fabs(db), db > 0 ? "brighter" : "darker"); qo += l; }
        }
        if (q[i].looped && q[i].seam < 0.8) { std::snprintf(l, sizeof l, "  keys %s: the loop seam matches %.2f (1 = seamless): a long-held note may tick at each loop pass\n", keys.c_str(), q[i].seam); qo += l; }
        if (q[i].looped && q[i].cycles < 1.5) { std::snprintf(l, sizeof l, "  keys %s: the loop holds about %.1f vibrato cycle(s): a long-held note repeats it and may sound mechanical\n", keys.c_str(), q[i].cycles); qo += l; }
    }
    o += "\nQUALITY (measured; listen to these on the Pa5X):\n" + (qo.empty() ? "  nothing stood out (level and timbre steps under 3 and 5 dB, seams match, loops longer than 1.5 vibrato cycles)\n" : qo);
    o += "\nMapping used:\n" + mapping + (mapping.empty() || mapping.back() == '\n' ? "" : "\n") + "\n";
    if (notes) *notes = o;
    *out = std::move(in);
    return "";
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_KSF_FOLDER_SOURCE_H
