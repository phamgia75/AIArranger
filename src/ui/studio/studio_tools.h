#ifndef AI_ARRANGER_UI_STUDIO_TOOLS_H
#define AI_ARRANGER_UI_STUDIO_TOOLS_H

// UI-toolkit-agnostic jobs behind the AI Music Studio's phase-2 pages (integration of E1, E4, E5): the EXPERIMENTAL
// KMP/KSF writer, the Sample Lab tools, the read-only Pa-native inspect view, and the level each new output really
// reaches. Every function is synchronous file/CPU work for a worker thread (never the message or audio thread).
// Sources are only read; every write is a NEW local folder (NAME.INCOMPLETE + one rename, mounted volumes refused).

#include "media/panative/panative_reader.h"
#include "media/samplelab/sample_lab_io.h"
#include "media/sound/korg_kmp_writer.h"
#include "media/sound/sound_recipe.h"
#include "ui/studio/studio_models.h"

namespace ai_arranger::ui::studio {

namespace lab = media::samplelab;

struct JobResult { std::string text; std::optional<Verification> level; std::string output; };   // output = folder written

// ── Output levels ──
// What an output folder is, from the first line of the REPORT.txt its writer put there (read-only, first 256 bytes).
enum class OutputKind { Package, KorgFolder, SampleLabFolder };
inline OutputKind outputKind(const fs::path& dir) {
    std::error_code ec;
    const auto rep = dir / "REPORT.txt";
    if (!fs::is_regular_file(fs::symlink_status(rep, ec))) return OutputKind::Package;
    std::ifstream f(rep, std::ios::binary);
    std::string head(256, '\0');
    f.read(head.data(), std::streamsize(head.size()));
    head.resize(size_t(f.gcount()));
    if (head.rfind("AIArranger Korg-format multisample folder", 0) == 0) return OutputKind::KorgFolder;
    if (head.rfind("AIArranger Sample Lab", 0) == 0) return OutputKind::SampleLabFolder;
    return OutputKind::Package;
}

// The real level of an output folder: L1 = every file matches PACKAGE.sha256 (the shared package verifier). L2 only
// for a Sound/Drum Kit package (our package reader loads it). A KMP/KSF folder stays at L1 as E1 states (its read back
// uses a reader written from the same Guide pages), and a Sample Lab folder holds working files with no reader. L3 is
// never set here (only an owner record, checkL3Record).
inline JobResult verifyOutput(const fs::path& dir, const std::function<bool()>& keepGoing = {}) {
    const auto c = media::sound::verifyPackage(dir, keepGoing);
    JobResult r{c.text, fromPackageCheck(c), {}};
    const auto kind = outputKind(dir);
    if (kind == OutputKind::Package || r.level->l1.state != Check::Passed) return r;
    r.level->l2 = {Check::NotRun, kind == OutputKind::KorgFolder ? "not claimed: EXPERIMENTAL KMP/KSF folder, verification level L1 only"
                                                                 : "not applicable: Sample Lab working files"};
    r.text = std::string(kind == OutputKind::KorgFolder ? "EXPERIMENTAL KMP/KSF folder" : "Sample Lab folder") +
             ": L1 passed, every file matches PACKAGE.sha256; L2 " + r.level->l2.detail + ". " + dir.string();
    return r;
}

// ── KMP/KSF writer (E1, EXPERIMENTAL, L1) ──
// The previewed Sound of Convert Sound, written as KRONOS-format .KMP + .KSF files into the new folder dest.
inline JobResult writeKmpFolder(const media::sound::Instrument& in, const fs::path& dest) {
    std::string err;
    const auto rep = media::sound::writeKorgFolder(in, dest, media::sound::formatRecipe(media::sound::recipeFromSamples(in)), &err);
    if (rep.empty()) return {"KMP/KSF folder NOT written: " + err, std::nullopt, {}};
    auto v = verifyOutput(media::folderPath(dest));
    return {"Written: " + media::folderPath(dest).string() + "\n" + v.text + "\n\n" + rep, v.level, media::folderPath(dest).string()};
}

// ── Sample Lab tools (E4) ──
inline std::string ms(const lab::Sample& s, size_t frames) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f ms", 1000.0 * double(frames) / s.rate);
    return b;
}

// Read-only preview: levels, onsets, the loop, and what normalize and trim WOULD do (on a copy).
inline JobResult labAnalyze(const fs::path& wav) {
    lab::Sample s;
    std::string err;
    if (!lab::readWavFile(wav, &s, &err)) return {err, std::nullopt, {}};
    char b[256];
    const auto l = lab::measureLevel(s);
    std::snprintf(b, sizeof b, "%zu frames, %u Hz, %u ch, peak %.2f dBFS, RMS %.2f dBFS\n", s.frames(), s.rate, unsigned(s.channels), l.peakDb, l.rmsDb);
    std::string o = "Sample Lab: analyze (read-only preview; nothing is written)\n" + wav.string() + "\n" + b;
    o += s.looped ? "Loop: " + std::to_string(s.loopStart) + "-" + std::to_string(s.loopEnd) + "\n" : std::string("Loop: none\n");
    const auto on = lab::detectOnsets(s);
    o += "Onsets (" + std::to_string(on.size()) + "):";
    for (size_t i = 0; i < on.size() && i < 64; ++i) o += " " + std::to_string(on[i]) + " (" + ms(s, on[i]) + ")";
    o += on.size() > 64 ? " ...\n" : "\n";
    auto n = s;
    const auto g = lab::normalize(n, lab::NormMode::Peak, -1, 1);
    std::snprintf(b, sizeof b, "Normalize preview (peak -1 dBFS, 1 dB headroom): gain %+.2f dB%s%s\n", g.gainDb, g.limited ? ", limited by headroom" : "",
                  g.silent ? ", silent: unchanged" : "");
    o += b;
    auto t = s;
    const auto tr = lab::trimSilence(t, -60);
    o += "Trim preview (-60 dBFS): cut " + std::to_string(tr.cutStart) + " frames at the start (" + ms(s, tr.cutStart) + "), " +
         std::to_string(tr.cutEnd) + " at the end (" + ms(s, tr.cutEnd) + ")" + (tr.silent ? ", silent: unchanged" : "") +
         (s.looped ? "; the loop is never cut" : "") + "\n";
    return {o, std::nullopt, {}};
}

// Time Slice: the loop is `bars` bars of `beats` beats; one slice per onset on keys from 36, written to dest.
inline JobResult labSlice(const fs::path& wav, double bars, double beats, const fs::path& dest) {
    lab::Sample s;
    std::string err;
    if (!lab::readWavFile(wav, &s, &err)) return {err, std::nullopt, {}};
    if (!(bars > 0 && bars <= 64 && beats > 0 && beats <= 16)) return {"bars must be 1-64 and beats 1-16", std::nullopt, {}};
    lab::SliceOptions o;
    o.bpm = bars * beats * 60.0 * s.rate / double(s.frames());
    const auto r = lab::timeSlice(s, wav.stem().string(), o);
    if (!r.error.empty()) return {"Time Slice refused: " + r.error, std::nullopt, {}};
    const auto rep = lab::writeSliceFolder(r, o, wav.string(), dest, &err);
    if (rep.empty()) return {"Time Slice NOT written: " + err, std::nullopt, {}};
    auto v = verifyOutput(media::folderPath(dest));
    return {"Written: " + media::folderPath(dest).string() + "\n" + v.text + "\n\n" + rep, v.level, media::folderPath(dest).string()};
}

// Loop helpers: zero-crossing snap (radius frames) and/or crossfade (frames) of [start, end), written to dest.
inline JobResult labLoop(const fs::path& wav, uint32_t start, uint32_t end, uint32_t snap, uint32_t xfade, const fs::path& dest) {
    lab::Sample s;
    std::string err;
    if (!lab::readWavFile(wav, &s, &err)) return {err, std::nullopt, {}};
    if (!(start < end && end <= s.frames()) || end - start < 32) return {"loop outside the sample or shorter than 32 frames", std::nullopt, {}};
    s.looped = true;
    s.loopStart = start;
    s.loopEnd = end;
    if (snap > 0) if (const auto e = lab::snapLoop(s, snap); !e.empty()) return {"Loop refused: " + e, std::nullopt, {}};
    if (xfade > 0) if (const auto e = lab::crossfadeLoop(s, s.loopStart, s.loopEnd, xfade); !e.empty()) return {"Loop refused: " + e, std::nullopt, {}};
    char rep[640];
    std::snprintf(rep, sizeof rep,
                  "AIArranger Sample Lab: loop (SOFTWARE evidence only)\n\nSource: %s\nLoop: %u-%u (asked %u-%u), snap radius %u, crossfade %u frames\n",
                  wav.string().c_str(), s.loopStart, s.loopEnd, start, end, snap, xfade);
    if (const auto e = lab::writeLabFolder(dest, {{"wav/" + wav.filename().string(), media::sound::writeWav(s)}, {"REPORT.txt", lab::bytesOf(rep)}});
        !e.empty())
        return {"Loop NOT written: " + e, std::nullopt, {}};
    auto v = verifyOutput(media::folderPath(dest));
    return {"Written: " + media::folderPath(dest).string() + "\n" + v.text + "\n\n" + rep, v.level, media::folderPath(dest).string()};
}

// Batch over every .wav of a folder (optional trim, peak normalize, 16-bit/rate conversion) with the duplicates report. An empty dest is a
// preview: the same table, nothing written.
inline JobResult labBatch(const fs::path& in, bool trim, bool normalize, const fs::path& dest, bool convert = false) {
    lab::BatchOptions o;
    o.trim = trim;
    o.normalize = normalize;
    o.convert = convert;   // P1: any PCM depth/rate into the Pa5X limits, with the before/after format per file
    const auto r = lab::runBatch(in, dest, o);
    if (!r.error.empty()) return {"Batch refused: " + r.error, std::nullopt, {}};
    if (dest.empty()) return {"Batch preview (nothing written)\n\n" + r.report, std::nullopt, {}};
    auto v = verifyOutput(media::folderPath(dest));
    return {"Written: " + media::folderPath(dest).string() + "\n" + v.text + "\n\n" + r.report, v.level, media::folderPath(dest).string()};
}

// ── Pa-native inspect (E5, read-only, EXPERIMENTAL) ──
inline bool canInspectPaNative(const SourceProbe& p) { return p.kind == SourceProbe::Kind::Pa4xSet; }
inline JobResult paNativeInspect(const fs::path& dir) {
    int invalid = 0, unreadable = 0;
    std::string err;
    const auto rep = media::panative::inspect(dir, &invalid, &unreadable, &err);
    if (rep.empty()) return {"Inspect failed: " + err, std::nullopt, {}};
    return {rep + "Read-only: nothing was changed. Item names, kind, bank and slot only; Sound, multisample and sample references "
                  "inside the items are not decoded.\n", std::nullopt, {}};
}

}  // namespace ai_arranger::ui::studio

#endif
