#ifndef AI_ARRANGER_UI_SOUND_CONVERSION_MODEL_H
#define AI_ARRANGER_UI_SOUND_CONVERSION_MODEL_H

// UI-toolkit-agnostic state behind the macOS "Convert Sound" tab: WAV folder or KRONOS .KMP/.KSF folder -> inspect -> layer order
// and velocity splits -> mapping preview and checks -> Sound Recipe -> package folder (SF2, TX16Wx
// TXPROG, WAVs, REPORT.txt with memory, RESOURCES.tsv). All conversion logic is the existing engine
// (wav_folder_source.h, package_writer.h, sound_recipe.h, package_graph.h); this file only orders the
// steps and words the results. It reads the source folder and writes one new local folder; it never
// writes to an instrument or sends MIDI.

#include "media/sound/korg_kmp_source.h"
#include "media/sound/ksf_folder_source.h"
#include "media/sound/package_writer.h"
#include "media/sound/sound_recipe.h"
#include "media/sound/wav_folder_source.h"
#include <cctype>
#include <map>
#include <memory>
#include <sstream>

namespace ai_arranger::ui {

namespace snd = media::sound;

// Soft-wraps lines longer than cols at a space (hard break when there is none); continuation lines
// keep the line's indent plus two. For prose panes; tables must not be wrapped.
inline std::string wrapLines(const std::string& text, size_t cols) {
    std::string out, line;
    std::istringstream is(text);
    for (bool first = true; std::getline(is, line); first = false) {
        if (!first) out += '\n';
        const size_t ind = std::min(line.find_first_not_of(' '), line.size());
        const std::string pad(std::min(ind + 2, cols / 2), ' ');
        while (line.size() > cols) {
            const size_t body = std::min(line.find_first_not_of(' '), line.size());
            size_t cut = line.rfind(' ', cols);
            if (cut == std::string::npos || cut <= body) cut = cols;
            out += line.substr(0, cut) + '\n';
            line = pad + line.substr(cut + (line[cut] == ' '));
        }
        out += line;
    }
    return out;
}

class SoundConversionModel {
public:
    struct LayerInfo { std::string prefix; size_t files = 0; uint8_t lo = 127, hi = 0; };

    // Step 1: read the file names only (fast); proposes a layer order and even velocity splits.
    // A folder holding .KMP files is a KRONOS folder (one layer per multisample); .KSF files without a .KMP are one layer
    // whose key map is proposed at Preview (ksf()); otherwise a WAV folder.
    bool inspect(const std::filesystem::path& dir, std::string* err) {
        if (snd::isKsfFolder(dir)) {
            dir_ = dir;
            korg_ = false;
            ksf_ = true;
            built_ = false;
            layers_.clear();
            LayerInfo li{snd::korg::layerName(dir.filename().string()), 0, 0, 127};
            if (li.prefix.empty()) li.prefix = "KSF";
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) li.files += snd::korg::lower(e.path().extension().string()) == ".ksf";
            layers_.push_back(li);
            return true;
        }
        ksf_ = false;
        if (snd::isKorgFolder(dir)) {
            std::vector<snd::KorgMultisample> ms;
            if (const auto e = snd::scanKorgFolder(dir, ms); !e.empty()) return fail(err, e);
            dir_ = dir;
            korg_ = true;
            ksf_ = false;
            layers_.clear();
            built_ = false;
            for (const auto& m : ms) {
                LayerInfo li{m.name, m.left.regions.size()};
                li.lo = m.left.regions.front().lo;
                li.hi = m.left.regions.back().hi;
                layers_.push_back(li);
            }
            return true;
        }
        std::map<std::string, std::vector<snd::WavFile>> by;
        if (const auto e = snd::scanWavFolder(dir, by); !e.empty()) return fail(err, e);
        dir_ = dir;
        korg_ = false;
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
    bool korg() const { return korg_; }
    bool ksf() const { return ksf_; }   // KSF-only folder: the mapping text (proposed, editable) defines the zones
    // Softest first when every layer name is a dynamic (ppp..fff, soft/medium/hard) or ends in a
    // number (v1, vel2); otherwise alphabetical. The musician confirms either way.
    std::string proposedOrder() const {
        std::vector<std::pair<long, std::string>> r;
        bool ranked = true;
        for (const auto& l : layers_) { const long k = loudness(l.prefix); ranked &= k >= 0; r.push_back({k, l.prefix}); }
        if (ranked) std::stable_sort(r.begin(), r.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string o;
        for (const auto& p : r) o += (o.empty() ? "" : ",") + p.second;
        return o;
    }
    static long loudness(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static const std::map<std::string, long> dyn{{"ppp", 1}, {"pp", 2}, {"p", 3}, {"mp", 4}, {"mf", 5}, {"f", 6}, {"ff", 7}, {"fff", 8},
                                                    {"soft", 2}, {"medium", 5}, {"med", 5}, {"mid", 5}, {"hard", 7}, {"loud", 7}};
        if (const auto it = dyn.find(s); it != dyn.end()) return it->second;
        size_t d = s.size();
        while (d && std::isdigit(static_cast<unsigned char>(s[d - 1]))) --d;
        return d < s.size() && s.size() - d < 9 ? 100 + std::stol(s.substr(d)) : -1;
    }
    std::string proposedSplits() const {   // lowest velocity of layers 2..N, evenly spaced
        std::string o;
        for (size_t i = 1; i < layers_.size(); ++i) o += (o.empty() ? "" : ",") + std::to_string(128 * i / layers_.size());
        return o;
    }

    // Step 2: decode the WAVs with the given order and splits. On failure nothing is kept.
    bool build(const std::string& name, const std::string& orderCsv, const std::string& splitsCsv, std::string* err, std::string* mapping = nullptr,
               std::string* notes = nullptr) {
        built_ = false;
        if (dir_.empty()) return fail(err, "choose a WAV folder first");
        snd::Instrument in;
        if (!decode(dir_, name, orderCsv, splitsCsv, in, err, mapping, notes)) return false;
        adopt(std::move(in));
        return true;
    }
    // build() in two halves for the UI: decode() reads the WAVs (slow, any thread), adopt() keeps the result.
    // Returns the previous Sound, now owned only by the caller, so its audio can be freed off the UI thread.
    std::shared_ptr<const snd::Instrument> adopt(snd::Instrument in) {
        auto old = std::move(in_);
        in_ = std::make_shared<const snd::Instrument>(std::move(in));
        built_ = true;
        return old;
    }
    // Immutable decoded Sound, shared with background writers without copying its audio.
    std::shared_ptr<const snd::Instrument> snapshot() const { return in_; }
    void invalidate() { built_ = false; }   // a setting changed: the preview no longer describes what would be written
    const std::filesystem::path& folder() const { return dir_; }
    // KSF-only folder: `mapping` is the mapping text; when it is empty it is proposed here and returned in it. `notes`
    // receives the REPORT source section measured on the decoded audio; pass it to generate().
    static bool decode(const std::filesystem::path& dir, const std::string& name, const std::string& orderCsv, const std::string& splitsCsv,
                       snd::Instrument& out, std::string* err, std::string* mapping = nullptr, std::string* notes = nullptr) {
        if (snd::isKsfFolder(dir)) {
            if (!mapping) return fail(err, "a KSF folder needs a key mapping");
            const auto layer = csv(orderCsv);
            if (layer.size() != 1 || layer[0].empty())
                return fail(err, "a KSF folder without a KMP makes one layer: name it (velocity layers cannot be recovered from KSF files)");
            if (!csv(splitsCsv).empty()) return fail(err, "one layer has no velocity splits; leave the splits empty");
            if (mapping->find_first_not_of(" \t\r\n") == std::string::npos)
                if (const auto e = snd::proposeKsfMapping(dir, mapping); !e.empty()) return fail(err, e);
            if (const auto e = snd::buildFromKsfMapping(dir, name, layer[0], *mapping, &out, notes); !e.empty()) return fail(err, e);
            return true;
        }
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
        const auto e = snd::isKorgFolder(dir) ? snd::buildFromKorgFolder(dir, name, order, splits, &out)
                                              : snd::buildFromWavFolder(dir, name, order, splits, &out);
        if (!e.empty()) return fail(err, e);
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
    static std::string generate(const snd::Instrument& in, const std::filesystem::path& sourceDir, const std::filesystem::path& out, std::string* err,
                                const std::string& ksfNotes = {}) {   // KSF folder: decode()'s notes for this Sound
        if (const auto r = snd::outputFolderRefusal(out); !r.empty()) { if (err) *err = r; return ""; }
        const bool ksf = snd::isKsfFolder(sourceDir);
        if (ksf && ksfNotes.empty()) { if (err) *err = "no measurement notes for this KSF Sound: press Preview again"; return ""; }
        const std::string notes = snd::isKorgFolder(sourceDir) ? snd::korgFolderNotes(sourceDir, in)
                                : ksf                          ? ksfNotes + snd::formatRecipe(snd::recipeFromSamples(in))
                                                               : snd::wavFolderNotes(sourceDir.string(), in);
        const std::string rep = snd::writePackage(in, out, notes, err);
        if (rep.empty()) return "";
        return writtenSummary(out) + rep;
    }

    // Reload check, resources and memory budget of a package just written to `out` (shared by the
    // WAV conversion and the plugin sampler tab).
    static std::string writtenSummary(const std::filesystem::path& out) {
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
        return o;
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
    bool korg_ = false;
    bool ksf_ = false;
};

} // namespace ai_arranger::ui

#endif // AI_ARRANGER_UI_SOUND_CONVERSION_MODEL_H
