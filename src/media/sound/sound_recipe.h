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

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_SOUND_RECIPE_H
