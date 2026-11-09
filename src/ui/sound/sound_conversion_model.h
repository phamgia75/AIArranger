#ifndef AI_ARRANGER_UI_SOUND_CONVERSION_MODEL_H
#define AI_ARRANGER_UI_SOUND_CONVERSION_MODEL_H

// UI-toolkit-agnostic state behind the macOS "Convert Sound" tab: WAV folder -> inspect -> layer order
// and velocity splits -> mapping preview and checks -> Sound Recipe -> package folder (SF2, TX16Wx
// TXPROG, WAVs, REPORT.txt with memory, RESOURCES.tsv). All conversion logic is the existing engine
// (wav_folder_source.h, package_writer.h, sound_recipe.h, package_graph.h); this file only orders the
// steps and words the results. It reads the source folder and writes one new local folder; it never
// writes to an instrument or sends MIDI.

#include "media/sound/package_writer.h"
#include "media/sound/sound_recipe.h"
#include "media/sound/wav_folder_source.h"
#include <map>
#include <memory>
#include <sstream>

namespace ai_arranger::ui {

namespace snd = media::sound;

class SoundConversionModel {
public:
    struct LayerInfo { std::string prefix; size_t files = 0; uint8_t lo = 127, hi = 0; };

    // Step 1: read the file names only (fast); proposes a layer order and even velocity splits.
    bool inspect(const std::filesystem::path& dir, std::string* err) {
        std::map<std::string, std::vector<snd::WavFile>> by;
        if (const auto e = snd::scanWavFolder(dir, by); !e.empty()) return fail(err, e);
        dir_ = dir;
        layers_.clear();
        built_ = false;
        for (const auto& [p, files] : by) {
            LayerInfo li{p, files.size()};
            for (const auto& f : files) { li.lo = std::min(li.lo, f.lo); li.hi = std::max(li.hi, f.hi); }
            layers_.push_back(li);
        }
        return true;
    }
    const std::vector<LayerInfo>& layers() const { return layers_; }
    std::string proposedOrder() const {   // alphabetical: the musician must confirm softest first
        std::string o;
        for (const auto& l : layers_) o += (o.empty() ? "" : ",") + l.prefix;
        return o;
    }
    std::string proposedSplits() const {   // lowest velocity of layers 2..N, evenly spaced
        std::string o;
        for (size_t i = 1; i < layers_.size(); ++i) o += (o.empty() ? "" : ",") + std::to_string(128 * i / layers_.size());
        return o;
    }

    // Step 2: decode the WAVs with the given order and splits. On failure nothing is kept.
    bool build(const std::string& name, const std::string& orderCsv, const std::string& splitsCsv, std::string* err) {
        built_ = false;
        if (dir_.empty()) return fail(err, "choose a WAV folder first");
        snd::Instrument in;
        if (!decode(dir_, name, orderCsv, splitsCsv, in, err)) return false;
        adopt(std::move(in));
        return true;
    }
    // build() in two halves for the UI: decode() reads the WAVs (slow, any thread), adopt() keeps the result.
    void adopt(snd::Instrument in) { in_ = std::make_shared<const snd::Instrument>(std::move(in)); built_ = true; }
    // Immutable decoded Sound, shared with background writers without copying its audio.
    std::shared_ptr<const snd::Instrument> snapshot() const { return in_; }
    void invalidate() { built_ = false; }   // a setting changed: the preview no longer describes what would be written
    const std::filesystem::path& folder() const { return dir_; }
    static bool decode(const std::filesystem::path& dir, const std::string& name, const std::string& orderCsv, const std::string& splitsCsv,
                       snd::Instrument& out, std::string* err) {
        std::vector<std::string> order;
        std::vector<uint8_t> splits;
        for (const auto& t : csv(orderCsv)) order.push_back(t);
        for (const auto& t : csv(splitsCsv)) {
            if (t.empty() || t.size() > 3 || t.find_first_not_of("0123456789") != std::string::npos || std::stoi(t) < 1 || std::stoi(t) > 127)
                return fail(err, "velocity split '" + t + "' is not a number from 1 to 127");
            splits.push_back(static_cast<uint8_t>(std::stoi(t)));
        }
        for (size_t i = 1; i < splits.size(); ++i)
            if (splits[i] <= splits[i - 1]) return fail(err, "velocity splits must rise from the softest layer to the loudest");
        if (const auto e = snd::buildFromWavFolder(dir, name, order, splits, &out); !e.empty()) return fail(err, e);
        return true;
    }
    bool built() const { return built_; }
    const snd::Instrument& instrument() const { static const snd::Instrument none; return in_ ? *in_ : none; }

    // Step 3: what the musician should look at before writing. Blocking problems were refused by build().
    std::vector<std::string> checks() const { return built_ ? checksOf(*in_) : std::vector<std::string>{}; }
    std::string preview() const { return built_ ? previewOf(*in_) : ""; }
    std::string recipeText() const { return built_ ? snd::formatRecipe(snd::recipeFromSamples(*in_)) : ""; }

    // Static forms, so the UI can compute them off the message thread before adopt().
    static std::vector<std::string> checksOf(const snd::Instrument& in_) {
        std::vector<std::string> w;
        for (const auto& L : in_.layers) {
            std::vector<bool> on(128, false);
            uint8_t lo = 127, hi = 0;
            for (const auto& z : L.zones) {
                for (int k = z.lo; k <= z.hi; ++k) on[size_t(k)] = true;
                lo = std::min(lo, z.lo); hi = std::max(hi, z.hi);
                const int span = std::max(int(z.root) - int(z.lo), int(z.hi) - int(z.root));
                const auto& s = in_.samples[size_t(z.sample)];
                if (span > 12) w.push_back(L.name + " " + key(z.lo) + "-" + key(z.hi) + ": " + s.name + " is transposed up to " + std::to_string(span) + " semitones (may sound unnatural)");
                if (s.looped && (s.loopEnd - s.loopStart) * 1000ull < 10ull * s.rate)
                    w.push_back(L.name + ": " + s.name + " loop is shorter than 10 ms (may buzz)");
                if (s.looped && s.loopStart * 1000ull < 2ull * s.rate)
                    w.push_back(L.name + ": " + s.name + " loop starts within 2 ms of the start (attack is looped)");
            }
            for (int k = lo, a = -1; k <= hi + 1; ++k) {
                const bool gap = k <= hi && !on[size_t(k)];
                if (gap && a < 0) a = k;
                if (!gap && a >= 0) { w.push_back(L.name + ": keys " + key(uint8_t(a)) + "-" + key(uint8_t(k - 1)) + " are silent (no zone)"); a = -1; }
            }
            if (lo > 0 || hi < 127) w.push_back(L.name + ": plays only " + key(lo) + "-" + key(hi) + " (outside it the layer is silent)");
        }
        std::map<uint32_t, int> rates;
        size_t stereo = 0, oneShot = 0;
        for (const auto& s : in_.samples) { ++rates[s.rate]; stereo += s.channels == 2; oneShot += !s.looped; }
        if (rates.size() > 1) w.push_back("samples use " + std::to_string(rates.size()) + " different sample rates");
        if (stereo && stereo < in_.samples.size()) w.push_back("mixed mono and stereo samples");
        if (oneShot) w.push_back(std::to_string(oneShot) + " sample(s) have no loop: they stop when the recording ends");
        return w;
    }

    static std::string previewOf(const snd::Instrument& in_) {
        std::string o;
        char l[200];
        for (const auto& L : in_.layers) {
            std::snprintf(l, sizeof l, "Layer '%s'  velocity %u-%u  %zu zone(s)\n  keys        root  sample                 length    rate   ch  loop\n",
                          L.name.c_str(), L.velLo, L.velHi, L.zones.size());
            o += l;
            for (const auto& z : L.zones) {
                const auto& s = in_.samples[size_t(z.sample)];
                std::snprintf(l, sizeof l, "  %-4s-%-4s  %-4s  %-20s  %6.2f s  %5u  %u   %s\n", key(z.lo).c_str(), key(z.hi).c_str(), key(z.root).c_str(),
                              s.name.c_str(), double(s.frames()) / s.rate, s.rate, s.channels,
                              s.looped ? (std::to_string(s.loopStart) + "-" + std::to_string(s.loopEnd)).c_str() : "none");
                o += l;
            }
        }
        const uint64_t bytes = snd::sampleBytes(in_);
        std::snprintf(l, sizeof l, "\n%zu samples, %llu bytes uncompressed PCM (exact; %.4f%% of 4 000 000 000)\n", in_.samples.size(),
                      (unsigned long long)bytes, 100.0 * double(bytes) / 4e9);
        return o + l;
    }

    // Step 4: write the package to parent/<name>. Returns the summary, or "" with *err set.
    // shortcut: writing straight onto a mounted volume is refused while hardware writes are not
    // authorized for this project phase; write locally and copy the folder to the USB stick by hand.
    static std::string generate(const snd::Instrument& in, const std::filesystem::path& sourceDir, const std::filesystem::path& out, std::string* err) {
        std::error_code ec;
        const auto real = std::filesystem::weakly_canonical(out, ec);   // follows symlinks in the existing part of the path
        if (ec || real.string().rfind("/Volumes/", 0) == 0) {
            if (err) *err = "refusing to write onto a mounted volume (" + out.string() + "): write the package to a local folder, then copy it to the USB stick";
            return "";
        }
        const std::string notes = snd::wavFolderNotes(sourceDir.string(), in);
        const std::string rep = snd::writePackage(in, out, notes, err);
        if (rep.empty()) return "";
        media::graph::Graph g;
        std::string pkg, e;
        std::string o = "Package written: " + out.string() + "\n";
        if (!snd::loadPackage(out, g, pkg, &e)) {
            o += "WARNING: the written package does not reload: " + e + "\n";
        } else {
            const auto issues = g.issues();
            o += "Resources: package " + pkg + ", " + std::to_string(g.nodes().size()) + " resources, " + std::to_string(issues.size()) + " unresolved\n";
            for (const auto& i : issues) o += "  " + i + "\n";
            o += media::graph::formatBudget(media::graph::budget(g));
        }
        o += "\nUSB: copy the whole folder to a USB stick unchanged; import is the owner's step (see REPORT.txt).\n\n";
        return o + rep;
    }

    static std::string key(uint8_t k) {
        static const char* n[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        return n[k % 12] + std::to_string(int(k / 12) - 1);
    }

private:
    static bool fail(std::string* err, const std::string& m) { if (err) *err = m; return false; }
    static std::vector<std::string> csv(const std::string& s) {
        std::vector<std::string> v;
        std::stringstream ss(s);
        for (std::string t; std::getline(ss, t, ',');) {
            while (!t.empty() && t.front() == ' ') t.erase(t.begin());
            while (!t.empty() && t.back() == ' ') t.pop_back();
            v.push_back(t);
        }
        return v;
    }

    std::filesystem::path dir_;
    std::vector<LayerInfo> layers_;
    std::shared_ptr<const snd::Instrument> in_;
    bool built_ = false;
};

} // namespace ai_arranger::ui

#endif // AI_ARRANGER_UI_SOUND_CONVERSION_MODEL_H
