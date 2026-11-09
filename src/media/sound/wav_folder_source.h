#ifndef AI_ARRANGER_MEDIA_SOUND_WAV_FOLDER_SOURCE_H
#define AI_ARRANGER_MEDIA_SOUND_WAV_FOLDER_SOURCE_H

// Route C of docs/research/KRONOS_KMP_KSF_IMPORT.md: an Instrument from a folder of WAVs named
// <layer>_<OrgKey>_<LowKey>_<HighKey>.wav, the naming the Pa5X itself exports (manual v1.4 p.1212)
// and package_writer.h writes to wav/. Root and forward loop come from the 'smpl' chunk.
// No proprietary format is parsed. Reads only; the source folder is never modified.
// Untrusted input: every chunk read is bounds-checked; anything unsupported is refused, never guessed.

#include "media/sound/instrument.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace ai_arranger::media::sound {

// 16-bit PCM, 1-2 channels. Returns "" on success or the reason it was refused.
inline std::string readWav(const std::vector<uint8_t>& b, Sample* s) {
    auto u16 = [&](size_t o) { return uint32_t(b[o] | b[o + 1] << 8); };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(&b[8], "WAVE", 4)) return "not a RIFF/WAVE file";
    bool fmt = false, data = false;
    size_t dataAt = 0, dataLen = 0;
    s->looped = false;
    bool haveRoot = false;
    for (size_t o = 12; o + 8 <= b.size();) {
        const size_t len = u32(o + 4), body = o + 8;
        if (len > b.size() - body) return "chunk overruns the file";
        if (!std::memcmp(&b[o], "fmt ", 4)) {
            if (len < 16) return "fmt chunk too short";
            if (u16(body) != 1) return "only uncompressed PCM is supported";
            if (u16(body + 2) != 1 && u16(body + 2) != 2) return "only mono or stereo is supported";   // check before narrowing
            s->channels = static_cast<uint8_t>(u16(body + 2));
            s->rate = u32(body + 4);
            if (u16(body + 14) != 16) return "only 16-bit samples are supported";
            fmt = true;
        } else if (!std::memcmp(&b[o], "data", 4)) {
            dataAt = body; dataLen = len; data = true;
        } else if (!std::memcmp(&b[o], "smpl", 4)) {
            if (len < 36) return "smpl chunk too short";
            if (u32(body + 12) > 127) return "smpl unity note outside 0-127";
            s->root = static_cast<uint8_t>(u32(body + 12));
            haveRoot = true;
            const uint32_t loops = u32(body + 28);
            if (loops > 1) return "only one loop per sample is supported";
            if (loops) {
                if (len < 36 + 24) return "smpl loop record missing";
                if (u32(body + 36 + 4) != 0) return "only forward loops are supported";
                if (u32(body + 36 + 16) != 0) return "fractional loop points are not supported";
                if (u32(body + 36 + 20) != 0) return "only endless loops (play count 0) are supported";
                s->loopStart = u32(body + 36 + 8);
                s->loopEnd = u32(body + 36 + 12) + 1;   // inclusive in 'smpl'
                s->looped = true;
            }
        }
        o = body + len + (len & 1);
    }
    if (!fmt || !data) return "missing fmt or data chunk";
    if (!haveRoot) return "no smpl chunk (root note unknown)";
    if (dataLen % (2u * s->channels)) return "data chunk does not hold whole 16-bit frames";
    s->pcm.resize(dataLen / 2);
    for (size_t i = 0; i < s->pcm.size(); ++i) s->pcm[i] = static_cast<int16_t>(u16(dataAt + 2 * i));
    return "";
}

struct WavFile { std::filesystem::path p; uint8_t org, lo, hi; };

// Groups a folder's WAVs by layer prefix from their names alone (no audio is read). "" on success.
inline std::string scanWavFolder(const std::filesystem::path& dir, std::map<std::string, std::vector<WavFile>>& byPrefix) {
    namespace fs = std::filesystem;
    byPrefix.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".wav") continue;
        const std::string stem = e.path().stem().string();
        int k[3];
        size_t end = stem.size(), cut = stem.size();
        bool ok = true;
        for (int i = 2; i >= 0 && ok; --i) {
            cut = end ? stem.rfind('_', end - 1) : std::string::npos;
            const std::string t = cut == std::string::npos ? "" : stem.substr(cut + 1, end - cut - 1);
            ok = !t.empty() && t.size() <= 3 && t.find_first_not_of("0123456789") == std::string::npos && (k[i] = std::stoi(t)) <= 127;
            end = cut;
        }
        if (!ok || cut == 0) return "file name is not <layer>_<OrgKey>_<LowKey>_<HighKey>.wav: " + e.path().filename().string();
        if (k[1] > k[2] || k[0] < k[1] || k[0] > k[2]) return "root outside its key range: " + e.path().filename().string();
        byPrefix[stem.substr(0, cut)].push_back({e.path(), uint8_t(k[0]), uint8_t(k[1]), uint8_t(k[2])});
    }
    if (ec) return "cannot read " + dir.string();
    if (byPrefix.empty()) return "no <layer>_<OrgKey>_<LowKey>_<HighKey>.wav files in " + dir.string();
    return "";
}

// layers: file prefixes in velocity order, softest first (empty = the folder must hold exactly one);
// splits: lowest velocity of layers 2..N (as RenderSpec::splits).
inline std::string buildFromWavFolder(const std::filesystem::path& dir, const std::string& name,
                                      std::vector<std::string> layers, const std::vector<uint8_t>& splits, Instrument* out) {
    namespace fs = std::filesystem;
    using File = WavFile;
    std::map<std::string, std::vector<File>> byPrefix;
    std::error_code ec;
    if (const auto e = scanWavFolder(dir, byPrefix); !e.empty()) return e;
    if (layers.empty()) {
        if (byPrefix.size() != 1) return "folder holds " + std::to_string(byPrefix.size()) + " layer prefixes; name them in velocity order";
        layers.push_back(byPrefix.begin()->first);
    }
    if (layers.size() != byPrefix.size()) return "the folder's layer prefixes do not match the layers given";
    if (splits.size() + 1 != layers.size()) return "need one split per layer after the first";
    Instrument in;
    in.name = name;
    for (size_t l = 0; l < layers.size(); ++l) {
        const auto it = byPrefix.find(layers[l]);
        if (it == byPrefix.end()) return "no files for layer " + layers[l];
        auto files = it->second;
        std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.lo < b.lo; });
        Layer L;
        L.name = layers[l];
        std::replace(L.name.begin(), L.name.end(), '_', ' ');
        L.velLo = l == 0 ? 0 : splits[l - 1];
        L.velHi = l + 1 == layers.size() ? 127 : static_cast<uint8_t>(splits[l] - 1);
        for (const File& f : files) {
            if (fs::file_size(f.p, ec) > (16u << 20) || ec) return "too large or unreadable: " + f.p.filename().string();
            std::ifstream is(f.p, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
            Sample s;
            if (const auto e = readWav(bytes, &s); !e.empty()) return f.p.filename().string() + ": " + e;
            if (s.root != f.org) return f.p.filename().string() + ": smpl root " + std::to_string(s.root) + " differs from the file name";
            s.name = L.name.substr(0, 16) + " " + std::to_string(f.org);
            L.zones.push_back({f.lo, f.hi, static_cast<int>(in.samples.size()), f.org, 0, 0});
            in.samples.push_back(std::move(s));
        }
        in.layers.push_back(std::move(L));
    }
    if (const auto e = validate(in); !e.empty()) return e.front();
    *out = std::move(in);
    return "";
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_WAV_FOLDER_SOURCE_H
