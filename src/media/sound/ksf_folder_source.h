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

// YIN fundamental (de Cheveigne & Kawahara 2002, threshold 0.15) of one 2048-frame window at x, 25 Hz up to rate/2
// (lag 2: the normalised difference keeps low notes from dipping there). 0 = no periodicity found.
// x must hold 2048 + rate/25 frames.
inline double yinHz(const int16_t* x, uint32_t rate) {
    const size_t W = 2048, tMin = 2, tMax = rate / 25;
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

struct KsfInfo {
    std::string file, name;   // file name; SMP1 sample name
    std::string refused;      // non-empty: cannot be converted, and why
    KsfHead head;
    int hint = -1;            // note number the sample name ends in (1-3 digits, <= 127); a hint only
    int detected = -1;        // nearest note to the measured fundamental; -1 = none (see why)
    double cents = 0;         // measured pitch relative to `detected`
    std::string why;
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
    const double exact = 69 + 12 * std::log2(f / 440);
    k.detected = int(std::lround(exact));
    k.cents = 100 * (exact - k.detected);
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
            char m[64];
            std::snprintf(m, sizeof m, "measured %s %+.0f cents", noteName(root).c_str(), k.cents);
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
        "# (optional column; 0 plays the recording as it is). Not the original mapping. Edit, then press Preview.\n"
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
    std::string o = "Source: Korg .KSF samples WITHOUT a .KMP in " + dir.string() +
                    "\nFormat: KSF per KRONOS Parameter Guide En 3 p.1159 (Korg format sample files)\n"
                    "RECOVERED exactly from the files: 16-bit audio (Start..End), sample rate, loop (Loop Start..End), mono.\n"
                    "APPROXIMATED (not in a KSF): root key of each sample (measured pitch, below), key zones (proposed by "
                    "AIArranger and confirmed or edited by the musician), one velocity layer 0-127.\n"
                    "NOT AVAILABLE without the KMP and Program: the original key zones, zone tune and level, velocity layers, "
                    "filters, envelopes, LFOs, effects.\n"
                    "Zone tune: the mapping's tune column (proposed as the correction of the measured pitch; 0 = as recorded).\n"
                    "Per sample (measured on the audio packaged):\n";
    size_t lt = 0, p12 = 0, s2 = 0;
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
        char l[96];
        o += "  " + padTo(printable(m.file), 16) + " " + padTo(k.name, 16) + " root " + padTo(noteName(m.root), 4) + " keys " + noteName(m.lo) + "-" +
             noteName(m.hi) + " tune " + (m.tune < 0 ? "-" : "+") + std::to_string(std::abs(m.tune)) + "  ";
        if (k.detected >= 0) {
            std::snprintf(l, sizeof l, "measured %s %+.1f cents%s", noteName(k.detected).c_str(), k.cents, k.detected == m.root ? "" : " (ROOT DIFFERS)");
            o += l;
        } else {
            o += "not measured: " + k.why;
        }
        if (k.hint >= 0) o += k.hint == m.root ? "; name agrees" : "; name says " + noteName(k.hint);
        o += k.head.loopOn ? "; looped\n" : "; one-shot\n";
        lt += k.head.loopOn && k.head.loopTune != 0;
        p12 += k.head.plus12dB;
        s2 += k.head.use2ndStart && k.head.start2 != k.head.start;
        s.root = uint8_t(m.root);
        s.name = layer.substr(0, 16) + " " + std::to_string(in.samples.size() + 1);
        L.zones.push_back({uint8_t(m.lo), uint8_t(m.hi), int(in.samples.size()), uint8_t(m.root), int8_t(m.tune), 0});
        in.samples.push_back(std::move(s));
    }
    std::sort(L.zones.begin(), L.zones.end(), [](const Zone& a, const Zone& b) { return a.lo < b.lo; });
    in.layers.push_back(std::move(L));
    if (const auto e = validate(in); !e.empty()) return e.front();
    if (lt) o += "  " + std::to_string(lt) + " sample(s) have Loop Tune: LOST\n";
    if (p12) o += "  " + std::to_string(p12) + " sample(s) have +12 dB: not carried (set the level on the Pa5X)\n";
    if (s2) o += "  " + std::to_string(s2) + " sample(s) have a 2nd Start in use: not carried\n";
    o += "Mapping used:\n" + mapping + (mapping.empty() || mapping.back() == '\n' ? "" : "\n") + "\n";
    if (notes) *notes = o;
    *out = std::move(in);
    return "";
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_KSF_FOLDER_SOURCE_H
