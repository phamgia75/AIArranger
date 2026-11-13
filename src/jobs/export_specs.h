#ifndef AI_ARRANGER_JOBS_EXPORT_SPECS_H
#define AI_ARRANGER_JOBS_EXPORT_SPECS_H

// Every existing export as an ExportSpec (export_job.h). Each one only wires the EXISTING code: the writer
// (writePackage via SoundConversionModel::generate, writeDrumPackage, writeKorgFolder, the Sample Lab writers,
// media::writeNewFile for a Style SMF or UASF, backupKst), the resource graph for dependencies and the memory
// estimate, and the shared verifiers for the level (verifyOutput, checkStyleSmf, verifyBackup). Inputs are captured
// by value or as immutable snapshots, so a job never touches state the UI keeps editing.

#include "jobs/export_job.h"
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
}  // namespace detail

// Sound package (SF2 + TXPROG + WAV), the Convert Sound Generate step.
inline ExportSpec soundPackage(std::shared_ptr<const snd::Instrument> in, fs::path sourceDir, std::string ksfNotes, fs::path dest) {
    ExportSpec s;
    s.type = "Sound package";
    s.source = sourceDir.string();
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
    return s;
}

// Sample Lab: Time Slice, Loop and Batch (the existing studio_tools jobs, which write through LabFolder).
inline ExportSpec labSpec(std::string type, fs::path input, fs::path dest) {
    ExportSpec s;
    s.type = std::move(type);
    s.source = input.string();
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
    return s;
}
inline ExportSpec labBatch(fs::path in, bool trim, bool normalize, fs::path dest) {
    auto s = labSpec("Sample Lab batch", in, dest);
    // checked before the job folder is made, so nothing is ever created inside the input folder
    s.validate = [in, d = detail::absFolder(dest)] {
        if (!detail::isDir(in)) return "Batch refused: " + in.string() + " is not a folder";
        if (media::samplelab::sameOrBelow(d, in) || media::samplelab::sameOrBelow(in, d))
            return std::string("Batch refused: refusing: the output folder may not be inside the input folder, or contain it");
        return std::string();
    };
    s.write = [=](const fs::path& out, auto&) { return detail::labResult(st::labBatch(in, trim, normalize, out)); };
    return s;
}

// Style as Pa5X SMF (EXPERIMENTAL): `x` is exportPa5xSmf of the Style as it was when the job was made.
inline ExportSpec styleSmf(std::string styleName, style_edit::SmfExport x, fs::path dest) {
    ExportSpec s;
    s.type = "Style as Pa5X SMF (EXPERIMENTAL)";
    s.source = std::move(styleName);
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
inline ExportSpec styleUasf(std::string styleName, std::vector<uint8_t> uasf, fs::path dest) {
    ExportSpec s;
    s.type = "Style as UASF";
    s.source = std::move(styleName);
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

}  // namespace ai_arranger::jobs

#endif
