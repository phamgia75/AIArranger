#ifndef AI_ARRANGER_JOBS_EXPORT_SPECS_H
#define AI_ARRANGER_JOBS_EXPORT_SPECS_H

// Every existing export as an ExportSpec (export_job.h). Each one only wires the EXISTING code: the writer
// (writePackage via SoundConversionModel::generate, writeDrumPackage, writeKorgFolder, the Sample Lab writers,
// media::writeNewFile for a Style SMF or UASF, backupKst), the resource graph for dependencies and the memory
// estimate, and the shared verifiers for the level (verifyOutput, checkStyleSmf, verifyBackup). Inputs are captured
// by value or as immutable snapshots, so a job never touches state the UI keeps editing.

#include "jobs/export_job.h"
#include "library/library_apply.h"
#include "media/samplelab/drum_builder.h"
#include "media/sound/package_graph.h"
#include "ui/sound/sound_conversion_model.h"
#include "ui/studio/studio_tools.h"
#include "ui/studio/style_studio_model.h"

namespace ai_arranger::jobs {

namespace snd = media::sound;
namespace st = ui::studio;

namespace detail {
// Dependencies of a Sound or Drum Kit through the shared resource graph: missing = its issues; the memory budget note.
template <class Add>
inline std::vector<std::string> graphDeps(Add add, std::vector<std::string>& notes, uint64_t& sampleBytes) {
    media::graph::Graph g;
    std::string err;
    if (!add(g, &err)) return {"cannot build the resource graph: " + err};
    const auto b = media::graph::budget(g);
    sampleBytes = b.uncompressedBytes;
    notes.push_back(std::string("Pa5X sample memory (software estimate): ") + fmtBytes(b.uncompressedBytes) + ", " + media::graph::fitName(b.fit));
    std::vector<std::string> missing;
    for (const auto& i : g.issues()) missing.push_back(i);
    return missing;
}
inline std::pair<Verification, std::string> outputLevel(const fs::path& out) {
    const auto r = st::verifyOutput(out);
    return {r.level.value_or(Verification{}), r.text};
}
inline uint64_t fileBytes(const fs::path& p) { std::error_code ec; const auto n = fs::file_size(p, ec); return ec ? 0 : n; }
// The studio_tools texts start with "Written: <path>\n<level>\n\n"; inside a job that path is the job's work folder,
// and the job states the published path and level itself, so only the writer's report is kept.
inline std::string writerReport(const std::string& t) {
    const auto k = t.find("\n\n");
    return t.rfind("Written: ", 0) == 0 && k != std::string::npos ? t.substr(k + 2) : t;
}
// D17: a single file read back and compared with the bytes it was written from (SHA-256), before it is published.
inline std::string readBackMismatch(const fs::path& out, const std::vector<uint8_t>& want) {
    bool c = false;
    std::string err;
    const auto got = media::detail::sha256File(out, [](size_t) { return true; }, &c, &err);
    media::Sha256 h;
    h.update(want.data(), want.size());
    if (got.empty()) return "cannot read back " + out.filename().string() + ": " + err;
    return got == h.hex() ? "" : "read back of " + out.filename().string() + " does not match the bytes written (SHA-256)";
}
inline bool isFile(const fs::path& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
inline bool isDir(const fs::path& p) { std::error_code ec; return fs::is_directory(p, ec); }
inline fs::path absFolder(const fs::path& p) { std::error_code ec; return media::folderPath(fs::absolute(p, ec)); }
inline WriteResult labResult(const st::JobResult& r) { return r.level ? WriteResult{"", writerReport(r.text)} : WriteResult{r.text, ""}; }

using Conversion = std::vector<std::pair<std::string, std::string>>;
// What a Sound output carries (P1 conversion report). kmp: the KMP/KSF writer, which drops zone levels.
inline Conversion soundConversion(const snd::Instrument& in, const fs::path& source, bool kmp) {
    size_t zones = 0, looped = 0, levels = 0;
    for (const auto& L : in.layers) for (const auto& z : L.zones) { ++zones; levels += z.levelCentibels != 0; }
    for (const auto& x : in.samples) looped += x.looped;
    Conversion c = {{"EXACT", "16-bit audio of " + std::to_string(in.samples.size()) + " sample(s) as decoded, sample rates, mono/stereo"},
                    {"EXACT", std::to_string(in.layers.size()) + " velocity layer(s), " + std::to_string(zones) + " key zone(s): ranges, roots and tune as mapped"},
                    {"EXACT", std::to_string(looped) + " loop(s) at the frames shown in the preview (forward)"}};
    if (snd::isKsfFolder(source)) c.push_back({"MEASURED", "root pitch of each KSF sample (no KMP): see the writer report"});
    c.push_back({"MEASURED", "level per velocity layer (first 200 ms RMS, Sound Recipe)"});
    c.push_back({"ESTIMATED", "Pa5X sample memory (software estimate)"});
    if (kmp && levels) c.push_back({"MISSING", std::to_string(levels) + " zone level offset(s): not carried in a KMP (written 0)"});
    c.push_back({"MISSING", "envelopes, filters, LFOs, effects: not carried by the import (set by hand on the Pa5X; Sound Recipe in REPORT.txt)"});
    return c;
}
inline Conversion drumConversion(const snd::DrumKit& kit, const std::vector<std::string>& lost) {
    size_t keys = 0, chokes = 0;
    for (const auto& I : kit.instruments) { keys += I.keys.size(); chokes += I.choke != 0; }
    Conversion c = {{"EXACT", "16-bit audio of " + std::to_string(kit.samples.size()) + " sample(s), one-shot, as read (or converted, see the notes)"},
                    {"EXACT", std::to_string(keys) + " key(s), " + std::to_string(kit.instruments.size()) + " group(s), " + std::to_string(chokes) +
                                  " with an exclusive group; level and pan as mapped"}};
    if (lost.empty()) c.push_back({"EXACT", "group names, exclusive groups, pan and drum families (TXPERF matrices)"});
    for (const auto& l : lost) c.push_back({"MISSING", l});
    c.push_back({"MISSING", "Drum Kit filter/envelope/effects edits: set by hand on the Pa5X"});
    return c;
}
}  // namespace detail

// Sound package (SF2 + TXPROG + WAV), the Convert Sound Generate step.
inline ExportSpec soundPackage(std::shared_ptr<const snd::Instrument> in, fs::path sourceDir, std::string ksfNotes, fs::path dest) {
    ExportSpec s;
    s.type = "Sound package";
    s.source = sourceDir.string();
    if (!sourceDir.empty()) s.sources = {sourceDir};
    s.dest = std::move(dest);
    s.validate = [in] { const auto e = in ? snd::validate(*in) : std::vector<std::string>{"no Sound"}; return e.empty() ? "" : "invalid Sound: " + e.front(); };
    s.dependencies = [in](auto& notes, auto& bytes) {
        return detail::graphDeps([&](auto& g, auto* err) { return snd::addInstrument(g, *in, snd::packageId(in->name, {}), err); }, notes, bytes);
    };
    s.preview = [in] {
        const uint64_t pcm = snd::sampleBytes(*in), n = in->samples.size();
        return std::vector<OutFile>{{in->name + ".sf2 (SoundFont multisamples)", pcm, "", false},
                                    {"wav/ (" + std::to_string(n) + " looped WAVs)", pcm + 44 * n, "", false},
                                    {"tx/" + in->name + "/ (TXPROG Sound and its samples)", pcm + 44 * n, "", false},
                                    {"REPORT.txt, RESOURCES.tsv, PACKAGE.sha256", 65536, "", false}};
    };
    s.write = [in, src = std::move(sourceDir), notes = std::move(ksfNotes)](const fs::path& out, auto&) {
        std::string err;
        const auto rep = ui::SoundConversionModel::generate(*in, src, out, &err, notes);
        return rep.empty() ? WriteResult{"Sound package NOT written: " + err, ""} : WriteResult{"", rep};
    };
    s.verify = detail::outputLevel;
    s.conversion = [in, sourceDir] { return detail::soundConversion(*in, sourceDir, false); };
    return s;
}

// Drum Kit package (TX Drum Kit + samples).
inline ExportSpec drumKit(std::shared_ptr<const snd::DrumKit> kit, std::string notes, fs::path dest) {
    ExportSpec s;
    s.type = "Drum Kit package";
    s.source = kit ? kit->name : "";
    s.dest = std::move(dest);
    s.validate = [kit] { const auto e = kit ? snd::validate(*kit) : std::vector<std::string>{"no Drum Kit"}; return e.empty() ? "" : "invalid Drum Kit: " + e.front(); };
    s.dependencies = [kit](auto& n, auto& bytes) {
        return detail::graphDeps([&](auto& g, auto* err) { return snd::addDrumKit(g, *kit, snd::packageId(kit->name, {}), err); }, n, bytes);
    };
    s.preview = [kit] {
        uint64_t pcm = 0;
        for (const auto& x : kit->samples) pcm += x.pcm.size() * 2 + 44;
        return std::vector<OutFile>{{"tx/" + kit->name + "/ (TX Drum Kit and " + std::to_string(kit->samples.size()) + " samples)", pcm, "", false},
                                    {"REPORT.txt, RESOURCES.tsv, PACKAGE.sha256", 65536, "", false}};
    };
    s.write = [kit, notes = std::move(notes)](const fs::path& out, auto&) {
        std::string err;
        const auto rep = snd::writeDrumPackage(*kit, out, notes, &err);
        return rep.empty() ? WriteResult{"Drum Kit package NOT written: " + err, ""} : WriteResult{"", rep};
    };
    s.verify = detail::outputLevel;
    s.conversion = [kit] { return detail::drumConversion(*kit, {}); };
    return s;
}

// Drum Kit as a KMP/KSF folder (EXPERIMENTAL, L1): one zone per key (samplelab::drumKitAsInstrument).
inline ExportSpec drumKitKmp(std::shared_ptr<const snd::DrumKit> kit, std::string notes, fs::path dest) {
    auto s = drumKit(kit, {}, std::move(dest));
    s.type = "Drum Kit as KMP/KSF folder (EXPERIMENTAL)";
    auto in = std::make_shared<snd::Instrument>();
    auto lost = std::make_shared<std::vector<std::string>>();
    const std::string why = kit ? media::samplelab::drumKitAsInstrument(*kit, in.get(), lost.get()) : "no Drum Kit";
    s.validate = [kit, why] {
        if (!kit) return std::string("no Drum Kit");
        const auto e = snd::validate(*kit);
        return !e.empty() ? "invalid Drum Kit: " + e.front() : why.empty() ? std::string() : "Drum Kit cannot be a KMP: " + why;
    };
    s.preview = [in] {
        return std::vector<OutFile>{{std::to_string(in->samples.size()) + " .KSF samples", snd::sampleBytes(*in) + 64 * in->samples.size(), "", false},
                                    {".KMP multisample (one zone per key), REPORT.txt, PACKAGE.sha256", 65536, "", false}};
    };
    s.write = [in, lost, notes = std::move(notes)](const fs::path& out, auto&) {
        std::string err, n = notes + "Drum Kit as one multisample, one zone per key. Not in a KMP:\n";
        for (const auto& l : *lost) n += "  " + l + "\n";
        const auto rep = snd::writeKorgFolder(*in, out, n, &err);
        return rep.empty() ? WriteResult{"KMP/KSF folder NOT written: " + err, ""} : WriteResult{"", rep};
    };
    s.conversion = [kit, lost] { return detail::drumConversion(*kit, *lost); };
    return s;
}

// Drum Kit from a WAV folder and its drum map (samplelab/drum_builder.h), as a TXPERF package or (kmp) a KMP/KSF folder.
// The WAVs are read inside the job (its validate phase, a worker thread), never on the caller's thread; then every
// phase is the drumKit / drumKitKmp spec's own. The folder is a source (D18).
inline ExportSpec drumKitFromFolder(fs::path dir, std::string name, std::string map, bool kmp, fs::path dest) {
    ExportSpec s;
    s.type = kmp ? "Drum Kit as KMP/KSF folder (EXPERIMENTAL)" : "Drum Kit package";
    s.source = dir.string();
    s.sources = {dir};
    s.dest = dest;
    auto in = std::make_shared<ExportSpec>();
    s.validate = [=] {
        auto kit = std::make_shared<snd::DrumKit>();
        std::string notes;
        if (const auto e = media::samplelab::buildDrumKit(dir, name, map, kit.get(), &notes); !e.empty()) return "Drum Kit refused: " + e;
        notes = "Source: WAV folder " + dir.string() + " and its drum map\n" + (notes.empty() ? "" : "Converted to 16-bit / Pa5X rate:\n" + notes);
        *in = kmp ? drumKitKmp(kit, notes, dest) : drumKit(kit, notes, dest);
        return in->validate();
    };
    s.dependencies = [in](auto& n, auto& b) { return in->dependencies(n, b); };
    s.preview = [in] { return in->preview(); };
    s.write = [in](const fs::path& out, const auto& p) { return in->write(out, p); };
    s.verify = [in](const fs::path& out) { return in->verify(out); };
    s.conversion = [in] { return in->conversion(); };
    return s;
}

// KMP/KSF folder (EXPERIMENTAL, verification level L1 only).
inline ExportSpec kmpFolder(std::shared_ptr<const snd::Instrument> in, fs::path dest) {
    auto s = soundPackage(in, {}, {}, std::move(dest));
    s.type = "KMP/KSF folder (EXPERIMENTAL)";
    s.source = in ? in->name + " (the previewed Sound)" : "";
    s.preview = [in] {
        return std::vector<OutFile>{{std::to_string(in->samples.size()) + " .KSF samples", snd::sampleBytes(*in) + 64 * in->samples.size(), "", false},
                                    {".KMP multisamples, REPORT.txt, PACKAGE.sha256", 65536, "", false}};
    };
    s.write = [in](const fs::path& out, auto&) {
        std::string err;
        const auto rep = snd::writeKorgFolder(*in, out, snd::formatRecipe(snd::recipeFromSamples(*in)), &err);
        return rep.empty() ? WriteResult{"KMP/KSF folder NOT written: " + err, ""} : WriteResult{"", rep};
    };
    s.conversion = [in] { return detail::soundConversion(*in, {}, true); };
    return s;
}

// Sample Lab: Time Slice, Loop and Batch (the existing studio_tools jobs, which write through LabFolder).
inline ExportSpec labSpec(std::string type, fs::path input, fs::path dest) {
    ExportSpec s;
    s.type = std::move(type);
    s.source = input.string();
    s.sources = {input};
    s.dest = std::move(dest);
    s.dependencies = [](auto& notes, auto&) { notes.push_back("Input WAV(s) are only read; no other resource is referenced"); return std::vector<std::string>{}; };
    s.preview = [input] {
        uint64_t n = 0;
        for (const auto& f : listFiles(input)) n += f.bytes;
        return std::vector<OutFile>{{"wav/ (processed copies of the input), REPORT.txt, PACKAGE.sha256", n + 65536, "", false}};
    };
    s.verify = detail::outputLevel;
    return s;
}
inline ExportSpec labSlice(fs::path wav, double bars, double beats, fs::path dest) {
    auto s = labSpec("Sample Lab Time Slice", wav, std::move(dest));
    s.validate = [wav] { return detail::isFile(wav) ? "" : "Time Slice refused: " + wav.string() + " is not a file"; };
    s.write = [wav, bars, beats](const fs::path& out, auto&) { return detail::labResult(st::labSlice(wav, bars, beats, out)); };
    return s;
}
inline ExportSpec labLoop(fs::path wav, uint32_t start, uint32_t end, uint32_t snap, uint32_t xfade, fs::path dest) {
    auto s = labSpec("Sample Lab loop", wav, std::move(dest));
    s.validate = [wav] { return detail::isFile(wav) ? "" : "Loop refused: " + wav.string() + " is not a file"; };
    s.write = [=](const fs::path& out, auto&) { return detail::labResult(st::labLoop(wav, start, end, snap, xfade, out)); };
    s.conversion = [] {
        return detail::Conversion{{"EXACT", "16-bit audio, rate and root of the input; the loop at the frames in the writer report (after snap)"},
                                  {"MISSING", "nothing else: one WAV with its smpl loop"}};
    };
    return s;
}
inline ExportSpec labBatch(fs::path in, bool trim, bool normalize, fs::path dest, bool convert = false) {
    auto s = labSpec("Sample Lab batch", in, dest);
    // checked before the job folder is made, so nothing is ever created inside the input folder
    s.validate = [in, d = detail::absFolder(dest)] {
        if (!detail::isDir(in)) return "Batch refused: " + in.string() + " is not a folder";
        if (media::samplelab::sameOrBelow(d, in) || media::samplelab::sameOrBelow(in, d))
            return std::string("Batch refused: refusing: the output folder may not be inside the input folder, or contain it");
        return std::string();
    };
    s.write = [=](const fs::path& out, auto&) { return detail::labResult(st::labBatch(in, trim, normalize, out, convert)); };
    s.conversion = [=] {
        detail::Conversion c;
        if (!trim && !normalize && !convert) c.push_back({"EXACT", "16-bit audio, rate, root and loop of every file written, unchanged"});
        if (trim || normalize) c.push_back({"MEASURED", "levels before/after, trim points and gain per file (writer report)"});
        if (convert) c.push_back({"MEASURED", "format before/after per file: 24/32-bit and float requantised to 16-bit (TPDF dither), rates outside 11025-48000 Hz "
                                              "resampled (windowed sinc), loop points scaled and rounded"});
        c.push_back({"ESTIMATED", "root key from pitch where a file has no smpl chunk and no Pa5X-style name"});
        c.push_back({"MISSING", "files not read (listed in the writer report)"});
        return c;
    };
    return s;
}

// Style as Pa5X SMF (EXPERIMENTAL): `x` is exportPa5xSmf of the Style as it was when the job was made.
inline ExportSpec styleSmf(std::string styleName, style_edit::SmfExport x, fs::path dest, fs::path sourceFile = {}) {
    ExportSpec s;
    s.type = "Style as Pa5X SMF (EXPERIMENTAL)";
    s.source = std::move(styleName);
    if (!sourceFile.empty()) s.sources = {sourceFile};
    s.dest = std::move(dest);
    auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(x.bytes));
    s.validate = [ok = x.ok, e = x.error] { return ok ? "" : "export refused: " + e; };
    s.dependencies = [](auto& notes, auto&) {
        notes.push_back("Sounds are referenced by bank/program number and resolved on the Pa5X: UNPROVEN until an owner test");
        return std::vector<std::string>{};
    };
    s.preview = [bytes, d = s.dest] { return std::vector<OutFile>{{d.filename().string(), bytes->size()}}; };
    s.write = [bytes](const fs::path& out, auto&) { return WriteResult{media::writeNewFile(out, *bytes), ""}; };
    s.verify = [bytes](const fs::path& out) {
        if (const auto bad = detail::readBackMismatch(out, *bytes); !bad.empty()) {
            Verification v;
            v.l1 = {Check::Failed, bad};
            return std::pair<Verification, std::string>{v, bad};
        }
        const auto v = st::StyleStudioModel::checkStyleSmf(out);
        return std::pair<Verification, std::string>{v, "Style SMF: " + v.l1.detail + " (EXPERIMENTAL; Pa5X import not tested)"};
    };
    return s;
}

// Style as UASF (AIArranger's own format; no Pa5X level applies).
inline ExportSpec styleUasf(std::string styleName, std::vector<uint8_t> uasf, fs::path dest, fs::path sourceFile = {}) {
    ExportSpec s;
    s.type = "Style as UASF";
    s.source = std::move(styleName);
    if (!sourceFile.empty()) s.sources = {sourceFile};
    s.dest = std::move(dest);
    auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(uasf));
    s.validate = [bytes] { return bytes->empty() ? "the Style cannot be serialized as UASF" : ""; };
    s.preview = [bytes, d = s.dest] { return std::vector<OutFile>{{d.filename().string(), bytes->size()}}; };
    s.write = [bytes](const fs::path& out, auto&) { return WriteResult{media::writeNewFile(out, *bytes), ""}; };
    s.verify = [bytes](const fs::path& out) {
        Verification v;
        if (const auto bad = detail::readBackMismatch(out, *bytes); !bad.empty()) {
            v.l1 = {Check::Failed, bad};
            return std::pair<Verification, std::string>{v, bad};
        }
        v.l1.detail = "not applicable: UASF is AIArranger's own format, not a Pa5X file (read back matches the bytes written)";
        return std::pair<Verification, std::string>{v, "UASF written; it was checked by the UASF validator on every edit. No Pa5X level applies."};
    };
    return s;
}

// Verified KST backup: a byte copy with its SHA-256 manifest beside it (backupKst; cancellable inside the copy).
inline ExportSpec kstBackup(fs::path source, fs::path dest) {
    ExportSpec s;
    s.type = "Verified KST backup";
    s.source = source.string();
    s.sources = {source};
    s.dest = std::move(dest);
    // checked before the job folder is made, so nothing is ever created inside the source (backupKst refuses it too, but later)
    s.validate = [source, d = detail::absFolder(s.dest)] {
        if (!detail::isDir(source)) return "refusing: " + source.string() + " is not a folder";
        if (media::samplelab::sameOrBelow(d, source)) return "refusing: the backup may not be inside the source " + source.string();
        return std::string();
    };
    s.dependencies = [source](auto& notes, auto&) {
        const auto inv = media::scan(source);
        if (!inv.ok) return std::vector<std::string>{"cannot list the source: " + inv.error};
        notes.push_back("byte copy of " + std::to_string(inv.files) + " files; Pa5X file content is not decoded, so references between "
                        "Styles, Sounds and Samples are copied unchanged and not checked (container UNKNOWN, D14)");
        return std::vector<std::string>{};
    };
    s.preview = [source] { return listFiles(source); };
    s.write = [source](const fs::path& out, const std::function<bool(double)>& progress) {
        const auto r = media::backupKst(source, out, [&](const media::BackupProgress& p) {
            return progress(p.workTotal ? double(p.workDone) / double(p.workTotal) : 0);
        });
        return r.ok ? WriteResult{"", r.message} : WriteResult{r.cancelled ? "Backup cancelled" : r.message, ""};
    };
    s.verify = [](const fs::path& out) {
        size_t n = 0;
        const auto bad = media::verifyBackup(out, {}, &n);
        Verification v;
        v.l1 = bad.empty() ? ui::studio::Level{Check::Passed, "all " + std::to_string(n) + " files match the backup manifest"}
                           : ui::studio::Level{Check::Failed, "files do not match the backup manifest"};
        v.l2 = {Check::NotRun, "not applicable: no Pa5X KST reader (container UNKNOWN)"};
        return std::pair<Verification, std::string>{v, bad.empty() ? "Backup verified against " + media::backupManifestPath(out).string() : bad};
    };
    return s;
}

// A package already written and checked in a private staging folder (the plugin sampler's render), copied as a new
// output. The copy must equal the staged files before the job verifies it (L1 + L2 with the shared verifier).
inline ExportSpec stagedPackage(fs::path staged, fs::path dest) {
    ExportSpec s;
    s.type = "Sound package (rendered from a plugin)";
    s.source = staged.string();
    s.sources = {staged};
    s.dest = std::move(dest);
    s.validate = [staged] { return detail::isDir(staged) ? "" : "refusing: the rendered package " + staged.string() + " is gone (render again)"; };
    s.preview = [staged] { return listFiles(staged); };
    s.write = [staged](const fs::path& out, auto&) {
        std::error_code ec;
        fs::copy(staged, out, fs::copy_options::recursive, ec);
        if (ec) return WriteResult{"copy failed: " + ec.message(), ""};
        if (const auto d = media::diff(staged, out); !d.same())
            return WriteResult{"the copy does not match the rendered package (" + (d.ok ? std::string("files differ") : d.error) + ")", ""};
        return WriteResult{"", ui::SoundConversionModel::writtenSummary(out)};
    };
    s.verify = detail::outputLevel;
    return s;
}

// Library Manager plan EXECUTION (E8, file level): a NEW library = the target's verified copy with the plan's file-level
// steps applied (library_apply.h). Both libraries are sources (D18); verify re-opens the staged output, runs the
// validator and re-checks both sources before anything is published.
inline ExportSpec libraryApply(std::shared_ptr<const library::Library> A, std::shared_ptr<const library::Library> B, std::vector<library::Step> plan,
                               fs::path dest) {
    ExportSpec s;
    s.type = "Library plan execution (file level)";
    s.source = "A: " + (A ? A->root : std::string()) + ", B: " + (B ? B->root : std::string());
    if (A && B) s.sources = {A->root, B->root};
    s.dest = std::move(dest);
    s.validate = [A, B, plan] {
        if (!A || !B || !A->ok || !B->ok) return std::string("refusing: a plan needs a library open on both sides");
        const auto x = library::executable(*A, *B, plan, library::evaluate(*A, *B, plan));
        return x.refusal.empty() ? std::string() : "refusing: " + x.refusal;
    };
    s.dependencies = [A, B, plan](auto& notes, auto&) {
        const auto ev = library::evaluate(*A, *B, plan);
        notes.push_back(std::string(library::kFileLevelLabel) + ": files are copied, renamed or swapped as opaque bytes; no file content is written or decoded");
        for (size_t i = 0; i < ev.steps.size(); ++i)
            for (const auto& w : ev.steps[i].warnings) notes.push_back("step " + std::to_string(i + 1) + ": " + w);
        return std::vector<std::string>{};
    };
    s.preview = [A, B, plan] {
        const auto ev = library::evaluate(*A, *B, plan);
        const auto x = library::executable(*A, *B, plan, ev);
        const auto& T = x.target == 'A' ? *A : *B;
        const auto& S = x.target == 'A' ? *B : *A;
        std::vector<OutFile> r;
        for (const auto& [rel, f] : T.files) r.push_back({rel, f.size});
        for (const auto& sc : ev.steps) for (const auto& op : sc.ops)
            if (op.kind == library::FileOp::CopyIn) r.push_back({op.to, S.files.count(op.from) ? S.files.at(op.from).size : 0});
        return r;
    };
    s.write = [A, B, plan](const fs::path& out, const std::function<bool(double)>& progress) {
        const auto r = library::applyPlan(*A, *B, plan, out, progress);
        return WriteResult{r.error.empty() ? "" : r.error == "cancelled" ? "Execution cancelled" : "Plan NOT executed: " + r.error, r.text};
    };
    const auto pinned = std::make_shared<library::Pinned>();   // the verified staged output, re-checked right before publishing
    s.verify = [A, B, plan, pinned](const fs::path& out) {
        const auto [ok, text] = library::verifyApplied(*A, *B, plan, out, pinned.get());
        Verification v;
        v.l1 = ok ? ui::studio::Level{Check::Passed, "manifest, expected files, re-opened library, validator and unchanged sources all pass"}
                  : ui::studio::Level{Check::Failed, "the executed library failed a check (see the verification text)"};
        v.l2 = {Check::NotRun, "not applicable: file content is not decoded (" + std::string(library::kFileLevelLabel) + ")"};
        return std::pair<Verification, std::string>{v, text};
    };
    s.beforePublish = [pinned](const fs::path& out) { return library::recheckPinned(*pinned, out); };
    return s;
}

// Library Manager plan (E7, EXPORT-ONLY): plan.json + PLAN.txt as a new folder. Both libraries are sources, so the D18
// overlap check refuses a plan folder that is, contains or lies inside either one before anything is created.
inline ExportSpec libraryPlan(std::shared_ptr<const library::Library> A, std::shared_ptr<const library::Library> B, std::vector<library::Step> plan,
                              fs::path dest) {
    ExportSpec s;
    s.type = "Library plan (EXPORT-ONLY)";
    s.source = "A: " + (A ? A->root : std::string()) + ", B: " + (B ? B->root : std::string());
    if (A && B) s.sources = {A->root, B->root};
    s.dest = std::move(dest);
    s.validate = [A, B, n = plan.size()] {
        if (!A || !B || !A->ok || !B->ok) return std::string("refusing: a plan needs a library open on both sides");
        return n ? std::string() : std::string("refusing: the plan has no steps");
    };
    s.dependencies = [](auto& notes, auto&) {
        notes.push_back("EXPORT-ONLY: a plan document; both libraries are only read and no keyboard file is written");
        return std::vector<std::string>{};
    };
    s.preview = [] { return std::vector<OutFile>{{"plan.json, PLAN.txt, PACKAGE.sha256", 65536, "", false}}; };
    s.write = [A, B, plan](const fs::path& out, auto&) {
        const auto e = library::exportPlan(*A, *B, plan, out);
        return e.empty() ? WriteResult{"", library::planReport(*A, *B, plan, library::evaluate(*A, *B, plan))} : WriteResult{"Plan NOT written: " + e, ""};
    };
    s.verify = [](const fs::path& out) {
        const auto c = snd::verifyPackage(out);
        Verification v;
        v.l1 = st::fromPackageCheck(c).l1;
        v.l2 = {Check::NotRun, "not applicable: a plan document"};
        return std::pair<Verification, std::string>{v, v.l1.state == Check::Passed ? "L1: plan.json and PLAN.txt match PACKAGE.sha256. L2 not applicable "
                                                                                     "(a plan document). L3 never: nothing for the keyboard was written."
                                                                                   : c.text};
    };
    return s;
}

}  // namespace ai_arranger::jobs

#endif
