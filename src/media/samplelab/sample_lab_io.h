#ifndef AI_ARRANGER_MEDIA_SAMPLELAB_SAMPLE_LAB_IO_H
#define AI_ARRANGER_MEDIA_SAMPLELAB_SAMPLE_LAB_IO_H

// Sample Lab jobs that read and write folders (roadmap E4): Time Slice and batch processing. Input folders are only
// read. Output is a NEW folder written the way every package is (media/output_guard.h, sound/package_writer.h):
// under NAME.INCOMPLETE, every file hashed as written, PACKAGE.sha256 (sha256sum format) last, the folder checked
// against it, then one rename that never replaces anything. Mounted volumes are refused.
//
// The WAVs go to wav/ with the Pa5X's own naming <prefix>_<OrgKey>_<LowKey>_<HighKey>.wav when a key map exists, so
// `aiarranger-sound wav --in OUT/wav --name NAME --out PKG` turns them into an SF2 + TXPROG Sound package unchanged.
//
// Auto-sampling (MIDI note out, audio in) is NOT here: it needs MIDI hardware TX, which stays fail-closed
// (AIARR_MIDI_HW_TX). Design note in the E4 report; the offline counterpart is sound/rendered_source.h.

#include "media/kst_backup.h"
#include "media/samplelab/sample_lab.h"
#include "media/sound/ksf_folder_source.h"   // yinHz (pitch)
#include "media/sound/package_writer.h"      // pkgio::FileSet, outputFolderRefusal, publishNewFolder
#include "media/sound/wav_folder_source.h"   // readWav, scanWavFolder
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace ai_arranger::media::samplelab {

namespace fs = std::filesystem;
using sound::Instrument;

// Writes `files` (relative path -> bytes) as the new folder `dest`. "" on success or the reason; on a failure after the
// work folder was made it stays as DEST.INCOMPLETE, as with every other writer.
inline std::string writeLabFolder(const fs::path& destGiven, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    const fs::path dest = folderPath(destGiven), work = incompletePath(dest);
    if (const auto why = outputFolderRefusal(dest); !why.empty()) return why;
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (!fs::create_directory(work, ec)) return "cannot create " + work.string();
    sound::pkgio::FileSet out{work};
    for (const auto& [rel, b] : files) out.save(rel, b);
    out.saveManifest();
    const std::string kept = "; the unfinished folder stays at " + work.string() + " (delete it)";
    if (!out.ok) return "write failed" + kept;
    size_t n = 0;
    const std::string bad = verifyManifest(work, work / sound::pkgio::FileSet::kPackageManifest, {}, &n,
                                           [](const std::string& rel) { return rel == sound::pkgio::FileSet::kPackageManifest; }, true);
    if (!bad.empty() || n != files.size()) return "the written folder does not check against PACKAGE.sha256 (" + bad + ")" + kept;
    if (!publishNewFolder(work, dest)) return "refusing: " + dest.string() + " appeared while writing" + kept;
    return "";
}

inline std::vector<uint8_t> bytesOf(const std::string& s) { return {s.begin(), s.end()}; }

// ── Time Slice ────────────────────────────────────────────────────────────────────────────────────────────────

// Format 0 SMF, one note per slice on channel 1: key base + i at the slice's start, off at the next slice's start (the
// loop's end for the last), tempo meta from bpm, end of track at the loop's length. Ticks = frames / rate * bpm/60 * ppq.
inline std::vector<uint8_t> sliceMidi(const std::vector<uint32_t>& starts, size_t frames, uint32_t rate, double bpm, uint16_t ppq,
                                      uint8_t base, uint8_t velocity) {
    std::vector<uint8_t> t;
    auto vlq = [&](uint32_t v) {
        uint8_t b[5];
        int n = 0;
        do { b[n++] = uint8_t(v & 0x7F); v >>= 7; } while (v);
        while (n--) t.push_back(uint8_t(b[n] | (n ? 0x80 : 0)));
    };
    auto tick = [&](size_t f) { return uint32_t(std::llround(double(f) / rate * bpm / 60.0 * ppq)); };
    const uint32_t us = uint32_t(std::lround(60e6 / bpm));
    vlq(0);
    t.insert(t.end(), {0xFF, 0x51, 0x03, uint8_t(us >> 16), uint8_t(us >> 8), uint8_t(us)});
    uint32_t now = 0;
    for (size_t i = 0; i < starts.size(); ++i) {
        const uint8_t key = uint8_t(base + i);
        const uint32_t on = tick(starts[i]), off = tick(i + 1 < starts.size() ? starts[i + 1] : frames);
        vlq(on - now); t.insert(t.end(), {0x90, key, velocity});
        vlq(off - on); t.insert(t.end(), {0x80, key, 0});
        now = off;
    }
    vlq(tick(frames) - now);
    t.insert(t.end(), {0xFF, 0x2F, 0x00});
    std::vector<uint8_t> m = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, uint8_t(ppq >> 8), uint8_t(ppq), 'M', 'T', 'r', 'k'};
    const uint32_t len = uint32_t(t.size());
    m.insert(m.end(), {uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len)});
    m.insert(m.end(), t.begin(), t.end());
    return m;
}

struct SliceOptions {
    double bpm = 0;            // tempo of the loop (> 0)
    uint8_t baseNote = 36;     // slice i plays on key baseNote + i (C2 = 36, the usual drum-pad start)
    uint16_t ppq = 480;
    uint8_t velocity = 100;
    uint32_t fadeOut = 64;     // frames of linear fade at each slice's end, against clicks where a slice is cut
    OnsetOptions onsets;
};

struct SliceResult {
    std::string error;                // "" = ok
    std::vector<uint32_t> starts;     // slice start frames; the first is always 0
    Instrument instrument;            // one layer, one zone per key (root = key, so each slice plays untransposed)
    std::vector<uint8_t> midi;        // sliceMidi()
};

// Slices `loop` at its onsets (plus frame 0, so the whole loop replays) into one sample per key from baseNote.
inline SliceResult timeSlice(const Sample& loop, const std::string& name, const SliceOptions& o) {
    SliceResult r;
    if (!sound::nameOk(name, 16)) { r.error = "name must be 1-16 printable ASCII characters"; return r; }
    if (!(o.bpm >= 20 && o.bpm <= 400)) { r.error = "bpm must be 20-400"; return r; }
    if (loop.frames() < 64 || !loop.channels) { r.error = "the loop is shorter than 64 frames"; return r; }
    r.starts = {0};
    const size_t gap = std::max<size_t>(1, size_t(o.onsets.minGapMs * loop.rate / 1000));   // a hit at the very start belongs to slice 1
    for (uint32_t f : detectOnsets(loop, o.onsets)) if (f >= r.starts.back() + gap) r.starts.push_back(f);
    if (o.baseNote + r.starts.size() - 1 > 127)
        { r.error = std::to_string(r.starts.size()) + " slices do not fit on keys " + std::to_string(o.baseNote) + "-127"; return r; }
    Instrument& in = r.instrument;
    in.name = name;
    sound::Layer L;
    L.name = name;
    const size_t ch = loop.channels;
    for (size_t i = 0; i < r.starts.size(); ++i) {
        const size_t a = r.starts[i], b = i + 1 < r.starts.size() ? r.starts[i + 1] : loop.frames();
        const uint8_t key = uint8_t(o.baseNote + i);
        Sample s;
        s.name = name + " " + std::to_string(key);
        s.rate = loop.rate;
        s.channels = loop.channels;
        s.root = key;
        s.pcm.assign(loop.pcm.begin() + a * ch, loop.pcm.begin() + b * ch);
        const size_t fo = std::min<size_t>(o.fadeOut, b - a);
        for (size_t k = 0; k < fo; ++k)
            for (size_t c = 0; c < ch; ++c) {
                int16_t& v = s.pcm[(b - a - 1 - k) * ch + c];
                v = static_cast<int16_t>(std::lround(v * double(k) / fo));
            }
        L.zones.push_back({key, key, int(in.samples.size()), key, 0, 0});
        in.samples.push_back(std::move(s));
    }
    in.layers.push_back(std::move(L));
    if (const auto e = sound::validate(in); !e.empty()) { r.error = e.front(); return r; }
    r.midi = sliceMidi(r.starts, loop.frames(), loop.rate, o.bpm, o.ppq, o.baseNote, o.velocity);
    return r;
}

// The Time Slice folder: wav/<NAME>_<k>_<k>_<k>.wav per slice, <NAME>.mid, SLICES.tsv, REPORT.txt, PACKAGE.sha256.
// Returns the report, or "" with *error set.
inline std::string writeSliceFolder(const SliceResult& r, const SliceOptions& o, const std::string& source, const fs::path& dest,
                                    std::string* error) {
    const Instrument& in = r.instrument;
    const std::string stem = sound::wavPrefix(in.name);
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    std::string tsv = "slice\tkey\tstart_frame\tframes\tstart_ms\n", rep;
    char line[512];
    std::snprintf(line, sizeof line,
                  "AIArranger Sample Lab: Time Slice (SOFTWARE evidence only)\n\nSource: %s\nSlices: %zu on keys %u-%zu, %.2f BPM, %u PPQ\n"
                  "Onsets: energy rise >= %.1f dB over %u-frame windows (hop %u), gate %.1f dBFS, min gap %.0f ms\n\n"
                  "  slice  key  start frame   frames  start ms\n",
                  source.c_str(), in.samples.size(), o.baseNote, o.baseNote + in.samples.size() - 1, o.bpm, o.ppq, o.onsets.riseDb,
                  o.onsets.window, o.onsets.hop, o.onsets.gateDb, o.onsets.minGapMs);
    rep = line;
    for (size_t i = 0; i < in.samples.size(); ++i) {
        const Sample& s = in.samples[i];
        const double ms = 1000.0 * r.starts[i] / s.rate;
        std::snprintf(line, sizeof line, "%zu\t%u\t%u\t%zu\t%.2f\n", i + 1, s.root, r.starts[i], s.frames(), ms);
        tsv += line;
        std::snprintf(line, sizeof line, "  %5zu  %3u  %11u  %7zu  %8.2f\n", i + 1, s.root, r.starts[i], s.frames(), ms);
        rep += line;
        std::snprintf(line, sizeof line, "wav/%s_%03u_%03u_%03u.wav", stem.c_str(), s.root, s.root, s.root);
        files.push_back({line, sound::writeWav(s)});
    }
    files.push_back({stem + ".mid", r.midi});
    files.push_back({"SLICES.tsv", bytesOf(tsv)});
    rep += "\n" + stem + ".mid replays the loop at tempo from the slices (format 0, channel 1).\n"
           "Make a Sound package from the slices: aiarranger-sound wav --in <this folder>/wav --name NAME --out PKG\n"
           "Check a copy: shasum -a 256 -c PACKAGE.sha256 (inside the folder)\n";
    files.push_back({"REPORT.txt", bytesOf(rep)});
    if (const auto e = writeLabFolder(dest, files); !e.empty()) { if (error) *error = e; return ""; }
    return rep;
}

// ── Batch ─────────────────────────────────────────────────────────────────────────────────────────────────────

struct BatchOptions {
    bool trim = false;
    double trimDb = -60;
    uint32_t fadeIn = 32, fadeOut = 256;
    bool normalize = false;
    NormMode mode = NormMode::Peak;
    double targetDb = -1, headroomDb = 1;
    double nearThreshold = 0.98;   // findDuplicates
};

struct BatchItem {
    std::string file, error;   // error: not read (the file is skipped)
    Level before, after;
    GainReport gain;
    TrimReport trim;
    double pitchHz = 0;        // YIN on the mono mix from 100 ms in (sound/ksf_folder_source.h); 0 = none found
    int root = 60;
    std::string rootFrom;      // "smpl", "file name", "pitch", "default"
    std::vector<std::string> limits;   // Pa5X sample limits the result breaks (sound::validateSample)
};

struct BatchResult { std::string error; std::vector<BatchItem> items; std::vector<DuplicatePair> duplicates; std::string report; };

// Processes every .wav directly in `in` (sorted by name; `in` is only read): trim, then normalize, as `o` says.
// Duplicates are found on the input audio. `out` empty = analysis only, nothing is written; otherwise the new folder
// holds wav/<same file names>, REPORT.txt and PACKAGE.sha256. Files that cannot be read are reported and left out.
inline BatchResult runBatch(const fs::path& in, const fs::path& out, const BatchOptions& o) {
    BatchResult r;
    std::error_code ec;
    std::vector<fs::path> paths;
    for (const auto& e : fs::directory_iterator(in, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".wav") paths.push_back(e.path());
    }
    if (ec) { r.error = "cannot read " + in.string(); return r; }
    std::sort(paths.begin(), paths.end());
    std::map<std::string, uint8_t> named;   // Pa5X-style names state the root (only when every WAV is named so)
    if (std::map<std::string, std::vector<sound::WavFile>> by; sound::scanWavFolder(in, by).empty())
        for (const auto& [p, v] : by) for (const auto& f : v) named[f.p.filename().string()] = f.org;
    std::vector<Sample> inputs, outputs;
    std::vector<size_t> idx;   // items index of each input
    for (const fs::path& p : paths) {
        BatchItem it;
        it.file = p.filename().string();
        Sample s;
        bool noSmpl = false;
        const auto st = fs::symlink_status(p, ec);
        if (!sound::nameOk(it.file, 255)) it.error = "file name has control or non-ASCII characters";
        else if (!fs::is_regular_file(st) || fs::file_size(p, ec) > (16u << 20) || ec) it.error = "not a regular file of at most 16 MiB";
        else {
            std::ifstream f(p, std::ios::binary);
            const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            it.error = sound::readWav(b, &s, &noSmpl);
        }
        if (it.error.empty()) {
            std::vector<int16_t> mono(s.frames());
            for (size_t i = 0; i < mono.size(); ++i) mono[i] = int16_t(std::lround(monoAt(s, i)));
            const size_t need = 2048 + s.rate / 25, from = std::min<size_t>(s.rate / 10, mono.size() > need ? mono.size() - need : 0);
            if (mono.size() >= need) it.pitchHz = sound::yinHz(mono.data() + from, s.rate);
            if (!noSmpl) it.rootFrom = "smpl";
            else if (named.count(it.file)) { s.root = named[it.file]; it.rootFrom = "file name"; }
            else if (it.pitchHz > 0) { s.root = uint8_t(std::clamp<long>(std::lround(69 + 12 * std::log2(it.pitchHz / 440)), 0, 127)); it.rootFrom = "pitch"; }
            else { s.root = 60; it.rootFrom = "default"; }
            it.root = s.root;
            it.before = measureLevel(s);
            inputs.push_back(s);
            if (o.trim) it.trim = trimSilence(s, o.trimDb, o.fadeIn, o.fadeOut);
            if (o.normalize) it.gain = normalize(s, o.mode, o.targetDb, o.headroomDb);
            it.after = measureLevel(s);
            s.name = it.file.substr(0, 20);   // validateSample needs a name; the file keeps its own
            for (char& c : s.name) if (c < 0x20 || c > 0x7E) c = '_';
            sound::validateSample(s, 0, it.limits);
            outputs.push_back(std::move(s));
            idx.push_back(r.items.size());
        }
        r.items.push_back(std::move(it));
    }
    if (inputs.empty()) { r.error = "no readable 16-bit PCM .wav files in " + in.string(); return r; }
    r.duplicates = findDuplicates(inputs, o.nearThreshold);
    for (auto& d : r.duplicates) { d.a = idx[d.a]; d.b = idx[d.b]; }

    char line[768];
    std::string rep = "AIArranger Sample Lab: batch (SOFTWARE evidence only)\n\nSource: " + in.string() + "\n";
    std::snprintf(line, sizeof line, "Trim: %s  Normalize: %s\n\n", o.trim ? ("below " + std::to_string(int(o.trimDb)) + " dBFS").c_str() : "off",
                  !o.normalize ? "off" : (std::string(o.mode == NormMode::Peak ? "peak" : "RMS") + " to " + std::to_string(o.targetDb).substr(0, 5) +
                                          " dBFS, headroom " + std::to_string(o.headroomDb).substr(0, 4) + " dB").c_str());
    rep += line;
    rep += "  peak dB  rms dB -> peak dB  rms dB   gain dB  cut start/end    pitch Hz  root (from)        file\n";
    size_t written = 0;
    for (const auto& it : r.items) {
        if (!it.error.empty()) { rep += "  NOT READ: " + it.file + ": " + it.error + "\n"; continue; }
        std::snprintf(line, sizeof line, "  %7.2f %7.2f -> %7.2f %7.2f  %+7.2f%s %7u/%-7u  %8.2f  %3d (%s)  %s\n", it.before.peakDb, it.before.rmsDb,
                      it.after.peakDb, it.after.rmsDb, it.gain.gainDb, it.gain.limited ? "L" : " ", it.trim.cutStart, it.trim.cutEnd, it.pitchHz,
                      it.root, it.rootFrom.c_str(), it.file.c_str());
        rep += line;
        for (const auto& l : it.limits) rep += "      Pa5X limit: " + l + "\n";
        ++written;
    }
    rep += "  (L = RMS target limited by the headroom)\n\nDuplicates (on the input audio):\n";
    if (r.duplicates.empty()) rep += "  none\n";
    for (const auto& d : r.duplicates) {
        std::snprintf(line, sizeof line, "  %s  %s = %s%s\n", d.exact ? "EXACT" : "NEAR ", r.items[d.a].file.c_str(), r.items[d.b].file.c_str(),
                      d.exact ? " (same PCM SHA-256)" : (" (correlation " + std::to_string(d.correlation).substr(0, 6) + ")").c_str());
        rep += line;
    }
    rep += "  Nothing is removed: keep or delete duplicates by hand.\n";
    if (out.empty()) { r.report = rep; return r; }
    rep += "\n" + std::to_string(written) + " WAVs written to wav/ (16-bit, 'smpl' root as above). Check a copy: shasum -a 256 -c PACKAGE.sha256\n";
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    for (size_t k = 0; k < outputs.size(); ++k) files.push_back({"wav/" + r.items[idx[k]].file, sound::writeWav(outputs[k])});
    files.push_back({"REPORT.txt", bytesOf(rep)});
    if (const auto e = writeLabFolder(out, files); !e.empty()) { r.error = e; return r; }
    r.report = rep;
    return r;
}

}  // namespace ai_arranger::media::samplelab

#endif
