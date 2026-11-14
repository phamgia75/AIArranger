#ifndef AI_ARRANGER_MEDIA_SOUND_SOUND_RECIPE_H
#define AI_ARRANGER_MEDIA_SOUND_SOUND_RECIPE_H

// Sound Recipe: the synthesis settings a TX16Wx/SF2 import does NOT carry into the Pa5X (envelopes,
// filters, pitch modulation, LFOs, effects: GUIDE pp.51, 85; docs/research/SOUND_PARAMETER_AUDIT.md).
// Each entry says where its value came from, so an estimate is never shown as a verified setting:
//   Extracted     read from the source (e.g. a plugin parameter), not mapped to a Pa5X value
//   Measured      computed from the rendered audio, with the method named
//   Estimated     derived by a stated rule; the basis must be given
//   NotAvailable  no evidence; no value is shown
// Output is a human-readable post-import checklist. AIArranger never sets these on the Pa5X.

#include "media/sound/rendered_source.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace ai_arranger::media::sound {

enum class Provenance { Extracted, Measured, Estimated, NotAvailable };

inline const char* provenanceName(Provenance p) {
    switch (p) {
        case Provenance::Extracted: return "EXTRACTED";
        case Provenance::Measured: return "MEASURED";
        case Provenance::Estimated: return "ESTIMATED";
        default: return "NOT AVAILABLE";
    }
}

struct RecipeParam {
    std::string block;     // "Amp EG", "Filter", "Pitch", "LFO", "Effects", "Velocity", "Source"
    std::string name;
    std::string value;     // empty when NotAvailable
    Provenance  prov = Provenance::NotAvailable;
    std::string basis;     // method (Measured), rule (Estimated), source (Extracted), reason (NotAvailable)
    std::string pa5x;      // where to set it on the Pa5X (manual page), "" if no Pa5X counterpart
};

struct SoundRecipe {
    std::string sound;
    std::vector<RecipeParam> params;
};

// Every entry needs a basis; NotAvailable must not carry a value; the others must.
inline std::vector<std::string> validate(const SoundRecipe& r) {
    std::vector<std::string> e;
    for (const auto& p : r.params) {
        const std::string n = p.block + " / " + p.name;
        if (p.basis.empty()) e.push_back(n + ": no basis given");
        if (p.prov == Provenance::NotAvailable && !p.value.empty()) e.push_back(n + ": NOT AVAILABLE entry carries a value");
        if (p.prov != Provenance::NotAvailable && p.value.empty()) e.push_back(n + ": value missing");
    }
    return e;
}

// From a plugin render: release and per-layer level are measured; attack is baked into the samples;
// filter, pitch modulation, LFO and effects have no evidence in the audio we keep. `extracted` holds
// raw source parameters (name, display value) as read from the plugin, listed unmapped.
inline SoundRecipe recipeFromRender(const RenderSpec& spec, const RenderReport& rep,
                                    const std::vector<std::pair<std::string, std::string>>& extracted = {}) {
    SoundRecipe r;
    r.sound = spec.name;
    auto add = [&](RecipeParam p) { r.params.push_back(std::move(p)); };
    char b[160];

    std::vector<double> rel;
    for (const auto& row : rep.rows) if (!std::isnan(row.releaseSec)) rel.push_back(row.releaseSec);
    std::sort(rel.begin(), rel.end());
    if (!rel.empty()) {
        std::snprintf(b, sizeof b, "%.2f s (median of %zu; range %.2f-%.2f s)", rel[rel.size() / 2], rel.size(), rel.front(), rel.back());
        add({"Amp EG", "Release time", b, Provenance::Measured,
             "note-off to -60 dB on the rendered note (10 ms RMS); the loop removes the recorded release",
             "Sound Edit > Amp > Envelope Generator (manual p.1130); the time-to-parameter mapping is unknown, set by ear"});
    } else {
        add({"Amp EG", "Release time", "", Provenance::NotAvailable,
             spec.loopHoldSec > 0 ? "render ended before -60 dB (raise --release)" : "one-shot samples keep their own decay; nothing to set",
             "Sound Edit > Amp > Envelope Generator (manual p.1130)"});
    }
    add({"Amp EG", "Attack", "fastest", Provenance::Estimated,
         "rule: the attack is recorded in each sample from its onset (2 ms pre-roll), so an added Pa5X attack would slow it",
         "Sound Edit > Amp > Envelope Generator (manual p.1130)"});

    // Velocity: level per layer, measured on the samples after the one shared gain.
    std::map<int, std::vector<double>> byVel;
    for (const auto& row : rep.rows) if (row.levelDb > -200) byVel[row.velocity].push_back(row.levelDb);
    if (byVel.size() >= 2) {
        std::string v;
        double top = -1e9;
        std::map<int, double> med;
        for (auto& [vel, xs] : byVel) { std::sort(xs.begin(), xs.end()); med[vel] = xs[xs.size() / 2]; top = std::max(top, med[vel]); }
        for (const auto& [vel, m] : med) { std::snprintf(b, sizeof b, "%svel %d: %+.1f dB", v.empty() ? "" : ", ", vel, m - top); v += b; }
        add({"Velocity", "Level per layer (relative to the loudest)", v, Provenance::Measured,
             "RMS of the first 200 ms of each sample, median over roots",
             "Sound Edit > Basic > Velocity/Range and Amp > Mod (manual pp.1082, 1126): within-layer velocity sensitivity is not measured"});
    } else {
        add({"Velocity", "Level per layer", "", Provenance::NotAvailable, "only one velocity rendered", ""});
    }
    add({"Velocity", "Response within a layer", "", Provenance::NotAvailable,
         "each layer is one render velocity; how the Pa5X scales level inside a layer is unknown until a hardware test",
         "Sound Edit > Amp > Mod (manual p.1126)"});
    add({"Filter", "Type / cutoff / resonance / EG", "", Provenance::NotAvailable,
         "the plugin's filter is baked into each sample at its render velocity and root; no Pa5X filter equivalent is derived",
         "Sound Edit > Filter (manual pp.1111-1118)"});
    add({"Pitch", "Pitch EG / modulation", "", Provenance::NotAvailable,
         "baked into the attack where present; not analysed", "Sound Edit > Pitch (manual pp.1102-1107)"});
    add({"LFO", "Vibrato / tremolo rate and depth", "", Provenance::NotAvailable,
         "not analysed; motion inside the loop repeats at the loop length", "Sound Edit > LFO (manual p.1134)"});
    add({"Effects", "Reverb / chorus / insert effects", "", Provenance::NotAvailable,
         "whatever the plugin applied is in the samples; render dry and choose Pa5X effects by ear",
         "Sound Edit > Effects (manual pp.1068, 1139)"});
    for (const auto& [name, value] : extracted)
        add({"Source", name, value.empty() ? "(no display value)" : value, Provenance::Extracted,
             "plugin parameter as reported by the host at render time; no Pa5X mapping", ""});
    return r;
}

// From recorded samples (WAV folder): only the per-layer level can be measured. Release, filter,
// modulation and effects are whatever the recording holds; nothing about them is in the files.
inline SoundRecipe recipeFromSamples(const Instrument& in) {
    SoundRecipe r;
    r.sound = in.name;
    auto add = [&](RecipeParam p) { r.params.push_back(std::move(p)); };
    std::vector<double> med;   // per layer: median over ALL its zones of the first-200 ms RMS (dBFS); NaN = the median zone is silent
    for (const Layer& L : in.layers) {
        std::vector<double> xs;
        for (const Zone& z : L.zones) {
            const Sample& s = in.samples[static_cast<size_t>(z.sample)];
            const size_t n = std::min(s.frames(), static_cast<size_t>(s.rate / 5)) * s.channels;
            double e = 0;
            for (size_t i = 0; i < n; ++i) e += double(s.pcm[i]) * s.pcm[i];
            xs.push_back(n && e > 0 ? 20 * std::log10(std::sqrt(e / n) / 32768.0) : -HUGE_VAL);   // silent zones count too
        }
        std::sort(xs.begin(), xs.end());
        med.push_back(xs.empty() || std::isinf(xs[xs.size() / 2]) ? std::nan("") : xs[xs.size() / 2]);
    }
    const size_t measured = static_cast<size_t>(std::count_if(med.begin(), med.end(), [](double x) { return !std::isnan(x); }));
    if (in.layers.size() >= 2 && measured < 2) {
        add({"Velocity", "Level per layer", "", Provenance::NotAvailable, "fewer than two layers have an audible median zone (first 200 ms)", ""});
    } else if (in.layers.size() >= 2) {
        double top = -1e300;
        for (double x : med) if (!std::isnan(x)) top = std::max(top, x);
        std::string v;
        char b[96];
        for (size_t l = 0; l < in.layers.size(); ++l) {
            if (std::isnan(med[l])) std::snprintf(b, sizeof b, "%s%s (vel %u-%u): silent (median zone)", v.empty() ? "" : ", ", in.layers[l].name.c_str(), in.layers[l].velLo, in.layers[l].velHi);
            else std::snprintf(b, sizeof b, "%s%s (vel %u-%u): %+.1f dB", v.empty() ? "" : ", ", in.layers[l].name.c_str(), in.layers[l].velLo, in.layers[l].velHi, med[l] - top);
            v += b;
        }
        add({"Velocity", "Level per layer (relative to the loudest)", v, Provenance::Measured,
             "RMS of the first 200 ms of each sample, median over the layer's zones",
             "Sound Edit > Basic > Velocity/Range and Amp > Mod (manual pp.1082, 1126)"});
    } else {
        add({"Velocity", "Level per layer", "", Provenance::NotAvailable, "only one layer", ""});
    }
    add({"Amp EG", "Attack", "fastest", Provenance::Estimated,
         "rule: the attack is recorded in each sample, so an added Pa5X attack would slow it",
         "Sound Edit > Amp > Envelope Generator (manual p.1130)"});
    add({"Amp EG", "Release time", "", Provenance::NotAvailable,
         "WAV files carry no release information; a looped sample stops at note-off unless a release is set by ear",
         "Sound Edit > Amp > Envelope Generator (manual p.1130)"});
    add({"Filter", "Type / cutoff / resonance / EG", "", Provenance::NotAvailable,
         "any filtering is baked into the recordings; no Pa5X filter setting is derived", "Sound Edit > Filter (manual pp.1111-1118)"});
    add({"Pitch", "Pitch EG / modulation", "", Provenance::NotAvailable, "not analysed", "Sound Edit > Pitch (manual pp.1102-1107)"});
    add({"LFO", "Vibrato / tremolo rate and depth", "", Provenance::NotAvailable,
         "not analysed; motion inside a loop repeats at the loop length", "Sound Edit > LFO (manual p.1134)"});
    add({"Effects", "Reverb / chorus / insert effects", "", Provenance::NotAvailable,
         "whatever was recorded is in the samples; choose Pa5X effects by ear", "Sound Edit > Effects (manual pp.1068, 1139)"});
    return r;
}

inline std::string formatRecipe(const SoundRecipe& r) {
    std::string o = "POST-IMPORT CONFIGURATION for \"" + r.sound + "\" (manual steps; AIArranger does not set these on the Pa5X,\n"
                    "and the TX16Wx/SF2 import does not carry them: TX16Wx guide pp.51, 85)\n";
    for (Provenance p : {Provenance::Measured, Provenance::Estimated, Provenance::Extracted, Provenance::NotAvailable}) {
        bool any = false;
        for (const auto& x : r.params) {
            if (x.prov != p) continue;
            if (!any) { o += std::string("\n  ") + provenanceName(p) + ":\n"; any = true; }
            o += "    " + x.block + " / " + x.name + (x.value.empty() ? "" : ": " + x.value) + "\n      basis: " + x.basis + "\n";
            if (!x.pa5x.empty()) o += "      Pa5X:  " + x.pa5x + "\n";
        }
    }
    return o;
}

// REPORT.txt notes for a WAV-folder package (CLI and macOS Convert Sound write the same text).
inline std::string wavFolderNotes(const std::string& sourceDir, const Instrument& in) {
    return "Source: WAV folder " + sourceDir + " (root and loop from each WAV's smpl chunk)\n\n" + formatRecipe(recipeFromSamples(in));
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_SOUND_RECIPE_H
