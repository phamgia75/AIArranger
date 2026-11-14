#ifndef AI_ARRANGER_MEDIA_SOUND_MEMORY_CHECK_H
#define AI_ARRANGER_MEDIA_SOUND_MEMORY_CHECK_H

// Offline comparison of the budget's raw PCM prediction with Pa5X Memory Info readings (MAN p.1174:
// Samples, Samples Size, Preload Buffer Size, Drum Samples, Multisamples) that the owner transcribes as
// text before and after an import. Nothing here talks to the instrument. One before/after pair is an
// observation, not a validation: overhead, compression and the display's units are UNKNOWN (Q6), so a
// size is carried as an interval covering both the display's rounding and decimal/binary units.
//
//   aiarranger-memory-info <TAB> 1
//   samples              <TAB> 123
//   samples-size         <TAB> 12.3 MB          (B, KB, MB or GB, as shown on screen)
//   preload-buffer-size  <TAB> 1.5 MB
//   drum-samples         <TAB> 4
//   multisamples         <TAB> 7
// Every field is optional; a field missing or left blank in either reading is not compared. Spaces may
// separate a field from its value (typed by hand); a trailing CR is ignored.

#include "media/sound/resource_graph.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>

namespace ai_arranger::media::sound {

struct ByteRange { double lo = 0, hi = 0; };   // possible true byte values behind one displayed size

struct MemoryInfoReading {
    std::optional<uint64_t> samples, drumSamples, multisamples;
    std::optional<ByteRange> samplesSize, preloadSize;
};

// "12.3 MB" -> every byte count that displays as 12.3 under either unit reading (1000^n or 1024^n).
inline bool parseDisplayedSize(const std::string& s, ByteRange& out) {
    const auto sp = s.find(' ');
    if (sp == std::string::npos || sp == 0 || sp > 20) return false;
    const std::string n = s.substr(0, sp), u = s.substr(sp + 1);
    if (n.find_first_not_of("0123456789.") != std::string::npos || n.front() == '.' || n.back() == '.' ||
        std::count(n.begin(), n.end(), '.') > 1) return false;
    int p = u == "B" ? 0 : u == "KB" ? 1 : u == "MB" ? 2 : u == "GB" ? 3 : -1;
    if (p < 0) return false;
    const auto dot = n.find('.');
    const double v = std::stod(n), half = 0.5 * std::pow(10.0, -double(dot == std::string::npos ? 0 : n.size() - dot - 1));
    out = {std::max(0.0, v - half) * std::pow(1000.0, p), (v + half) * std::pow(1024.0, p)};
    return true;
}

inline bool parseMemoryInfo(const std::string& textIn, MemoryInfoReading& r, std::string* err) {
    auto fail = [&](size_t line, const std::string& m) { if (err) *err = "Memory Info line " + std::to_string(line) + ": " + m; return false; };
    std::stringstream ss(textIn);
    std::string l;
    auto fields = [](std::string x, std::string& k, std::string& v) {
        while (!x.empty() && (x.back() == '\r' || x.back() == ' ' || x.back() == '\t')) x.pop_back();
        const auto ws = x.find_first_of(" \t");
        k = x.substr(0, ws);
        const auto vs = ws == std::string::npos ? std::string::npos : x.find_first_not_of(" \t", ws);
        v = vs == std::string::npos ? "" : x.substr(vs);
    };
    std::string k, v;
    if (!std::getline(ss, l) || (fields(l, k, v), k != "aiarranger-memory-info" || v != "1")) return fail(1, "not an aiarranger-memory-info version 1 record");
    r = {};
    for (size_t i = 2; std::getline(ss, l); ++i) {
        fields(l, k, v);
        if (k.empty() || v.empty()) {
            if (k.empty() || k == "samples" || k == "drum-samples" || k == "multisamples" || k == "samples-size" || k == "preload-buffer-size") continue;
            return fail(i, "unknown field '" + k + "'");
        }
        auto count = [&](std::optional<uint64_t>& f) {
            if (f || v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos) return false;
            f = std::stoull(v);
            return true;
        };
        auto size = [&](std::optional<ByteRange>& f) { ByteRange b; if (f || !parseDisplayedSize(v, b)) return false; f = b; return true; };
        const bool ok = k == "samples" ? count(r.samples) : k == "drum-samples" ? count(r.drumSamples) : k == "multisamples" ? count(r.multisamples)
                      : k == "samples-size" ? size(r.samplesSize) : k == "preload-buffer-size" ? size(r.preloadSize) : false;
        if (!ok) return fail(i, "unknown, repeated or malformed field '" + k + "'");
    }
    return true;
}

struct PredictedImport { uint64_t samples = 0, multisamples = 0, drumSamples = 0, rawPcmBytes = 0; };

inline PredictedImport predict(const graph::Graph& g) {
    PredictedImport p;
    for (const auto& [id, n] : g.nodes()) {   // package resources only: evidence describes what is already on the Pa5X
        p.multisamples += id.provisional && id.kind == graph::Kind::Multisample;
        p.drumSamples += id.provisional && id.kind == graph::Kind::DrumSample;
    }
    const auto b = graph::budget(g);
    p.samples = b.samples;
    p.rawPcmBytes = b.uncompressedBytes;
    return p;
}

inline std::string compareMemoryInfo(const MemoryInfoReading& before, const MemoryInfoReading& after, const PredictedImport& p) {
    std::ostringstream o;
    o << "Memory Info comparison (OBSERVATION ONLY; HARDWARE VALIDATION PENDING)\n";
    auto cnt = [&](const char* what, const std::optional<uint64_t>& a, const std::optional<uint64_t>& b, uint64_t want) {
        if (!a || !b) { o << "  " << what << ": not compared (missing in a reading)\n"; return; }
        const long long d = (long long)*b - (long long)*a;
        o << "  " << what << ": " << *a << " -> " << *b << " (" << (d >= 0 ? "+" : "") << d << "), predicted +" << want
          << (d == (long long)want ? "  MATCH" : "  DIFFERENT: evidence for Q6 (how the Pa5X counts), not a fault") << "\n";
    };
    cnt("Samples", before.samples, after.samples, p.samples);
    cnt("Multisamples", before.multisamples, after.multisamples, p.multisamples);
    cnt("Drum Samples", before.drumSamples, after.drumSamples, p.drumSamples);
    o << "  predicted raw PCM: " << p.rawPcmBytes << " bytes (exact for the packages' 16-bit samples; overhead and compression not modelled)\n";
    if (!before.samplesSize || !after.samplesSize) {
        o << "  Samples Size: not compared (missing in a reading)\n";
    } else {
        // Each reading is a range; the true growth lies between these bounds.
        const double lo = std::max(0.0, after.samplesSize->lo - before.samplesSize->hi), hi = after.samplesSize->hi - before.samplesSize->lo;
        char l[256];
        std::snprintf(l, sizeof l, "  Samples Size growth: %.0f .. %.0f bytes (display rounding and both unit readings)\n", lo, hi);
        o << l;
        const double raw = double(p.rawPcmBytes);
        if (hi < 0) o << "  -> the size shrank: another change happened between the readings; not comparable\n";
        else if (raw >= lo && raw <= hi) o << "  -> CONSISTENT with raw PCM (the range is too wide to tell overhead or compression apart)\n";
        else if (hi < raw) { std::snprintf(l, sizeof l, "  -> SMALLER than raw PCM: stored at most %.1f%% of raw. Compression, or a different size formula (Q6)\n", raw > 0 ? 100.0 * hi / raw : 0.0); o << l; }
        else { std::snprintf(l, sizeof l, "  -> LARGER than raw PCM by at least %.0f bytes: per-sample overhead or other data (Q6)\n", lo - raw); o << l; }
    }
    if (before.preloadSize && after.preloadSize)
        o << "  Preload Buffer Size changed between the readings: " << (before.preloadSize->lo == after.preloadSize->lo && before.preloadSize->hi == after.preloadSize->hi ? "no" : "YES (not modelled)") << "\n";
    o << "  One reading pair does not validate the model; the budget claims raw PCM bytes only.\n";
    return o.str();
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_MEMORY_CHECK_H
