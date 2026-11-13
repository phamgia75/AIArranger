#ifndef AI_ARRANGER_UI_STUDIO_STYLE_STUDIO_MODEL_H
#define AI_ARRANGER_UI_STUDIO_STYLE_STUDIO_MODEL_H

// UI-toolkit-agnostic state behind the studio's Style Studio editor (integration of E3): open a Style (.sty through the
// SFF1 importer, or .uasf), browse its elements and tracks, apply the style_edit ops with undo/redo, import one SMF
// track, and write NEW files only (Pa5X Style SMF, EXPERIMENTAL; UASF) through the shared output guard (no overwrite,
// no mounted volume). Message-thread or worker use, never the audio thread. Needs the style-edit and importer-sff1
// libraries.

#include "engine/uasf/deserializer.h"
#include "engine/uasf/serializer.h"
#include "exporters/korg/genos_to_korg.h"   // korgMarkerFor
#include "exporters/korg/sff1_route.h"
#include "exporters/korg/sound_map.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"
#include "media/output_guard.h"
#include "style_edit/style_editor.h"
#include "ui/studio/studio_models.h"

#include <cstdlib>
#include <memory>
#include <sstream>

namespace ai_arranger::ui::studio {

class StyleStudioModel {
public:
    // The ops the Style Studio offers, in menu order, with what their value field holds.
    // Element ops act on the chosen element; Track ops on the chosen track's channel, style-wide (a Pa track). Part and
    // Sound are Pa5X export settings (the CASM route and the user's bank/program), not UASF edits: no undo, not in UASF.
    enum class Op { Quantize, Transpose, VelocitySet, VelocityOffset, VelocityScale, Tempo, Meter,
                    ElementCopy, ElementClear, ElementRename, ElementLength, TrackRole, TrackPart, TrackSound };
    static constexpr int kOps = 14;
    static const char* opName(Op o) {
        static const char* n[] = {"Quantize", "Transpose", "Velocity: set", "Velocity: offset", "Velocity: scale %", "Tempo", "Meter",
                                  "Element: copy to", "Element: clear", "Element: rename", "Element: length", "Track: role",
                                  "Track: Pa5X part", "Track: Pa5X sound"};
        return n[int(o)];
    }
    static const char* opHint(Op o) {
        static const char* h[] = {"grid and strength, e.g. 1/16 80", "semitones, e.g. -2", "velocity 1-127", "offset, e.g. -10",
                                  "percent, e.g. 90", "BPM 20-400 (whole style)", "num/den, e.g. 6/8 (element only)",
                                  "Intro 1-3, Variation 1-4, Fill 1-4, Break, Ending 1-3", "(no value)", "new name", "bars 1-256",
                                  "drum, percussion, bass, chord, pad, phrase1, phrase2, melody, accompaniment",
                                  "bass, drum, perc, acc1-acc5 or none", "MSB LSB PC (your Pa5X values) or unresolved"};
        return h[int(o)];
    }

    // Refused while the open Style has edits not written yet, unless `discard` (the user chose Discard): opening another
    // Style must never silently drop them. An invalid Style (UASF validator ERROR) never becomes an editable model.
    bool load(const std::filesystem::path& path, std::string* error, bool discard = false) {
        if (dirty() && !discard)
            return fail(error, "the open Style has edits not written yet: Save UASF... or Export Pa5X SMF..., or Discard them first");
        uasf::StyleDefinition s;
        exporters::korg::MapOptions opts;
        std::vector<std::string> notes;
        std::string ext = path.extension().string();
        for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".mid") {   // a Korg Style SMF separated by markers (ours or a Pa5X Export SMF)
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) > (8u << 20))
                return fail(error, path.string() + ": not a regular file of at most 8 MiB");
            std::ifstream f(path, std::ios::binary);
            auto in = style_edit::importKorgSmf({std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()}, path.stem().string());
            if (!in.ok) return fail(error, path.string() + ": " + in.error);
            s = std::move(in.style);
            opts.sounds = std::move(in.sounds);
            notes = std::move(in.notes);
            notes.insert(notes.begin(), "Source: Korg Style SMF separated by markers; bank/program per track taken as the Pa5X sounds");
        } else if (ext == ".uasf") {
            uasf::UasfDeserializer d;
            auto r = d.deserializeFromFile(path.string());
            if (!r.success) return fail(error, path.string() + ": " + r.error);
            s = std::move(r.style);
        } else {
            importers::sff1::Sff1Reader reader;
            const auto parsed = reader.parseFile(path.string());
            if (!parsed.success) return fail(error, path.string() + ": " + parsed.error);
            importers::sff1::Sff1ToUasfMapper m;
            auto mapped = m.map(parsed);
            if (!mapped.success) return fail(error, path.string() + ": " + mapped.error);
            s = std::move(mapped.style);
            opts.route = exporters::korg::routeFromCasm(parsed);
            std::string fmt = "unknown SFF version (no SFF1/SFF2 marker)";
            for (const auto& mk : parsed.markers)
                if (mk.name == "SFF1" || mk.name == "SFF2") fmt = mk.name;
            notes.push_back("Source: Yamaha " + fmt + ", " + std::to_string(parsed.casm_configs.size()) + " CASM track configs" +
                            (parsed.casm_configs.empty() ? " (none read: SFF1 Ctab blocks are not parsed), parts map by channel as written"
                                                         : ", CASM routing applied to the Pa5X parts"));
            notes.push_back("CASM/NTT: the Style SMF carries events only; Yamaha NTR/NTT, note limits and high key do not carry over "
                            "(listed per part below for reference). Yamaha bank/program numbers are never forwarded.");
        }
        if (s.sections.empty()) return fail(error, path.string() + ": no Style elements with events");
        uasf::UasfValidator v;
        for (const auto& i : v.validate(s).issues)
            if (i.severity == uasf::ValidationIssue::ERROR)
                return fail(error, path.string() + ": refused by the UASF validator: " + i.message + (i.location.empty() ? "" : " (" + i.location + ")"));
        ed_ = std::make_unique<style_edit::StyleEditor>(std::move(s));
        opts_ = std::move(opts);
        notes_ = std::move(notes);
        path_ = path;
        saved_ = bytes();
        return true;
    }
    bool loaded() const { return ed_ != nullptr; }
    const uasf::StyleDefinition& style() const { return ed_->style(); }
    const std::filesystem::path& path() const { return path_; }
    // The studio's staged changes: the model differs from what was opened or last written (Save UASF / Export SMF).
    // Compared by content, so save + undo is dirty and redo back to the written state is not.
    bool dirty() const {
        if (!ed_) return false;
        const auto now = bytes();
        return now.empty() || now != saved_;   // a Style that cannot be serialized counts as unwritten
    }

    // Element/track browser: one line per element, its tracks indented.
    std::string browser() const {
        if (!ed_) return "";
        const auto& s = style();
        std::string o = "Style: " + s.name + "  " + std::to_string(s.tempo_bpm) + " BPM  (from " + path_.filename().string() +
                        "; the source file is never changed)\n";
        for (const auto& n : notes_) o += "  " + n + "\n";
        for (size_t i = 0; i < s.sections.size(); ++i) {
            const auto& sec = s.sections[i];
            const char* marker = exporters::korg::korgMarkerFor(sec.type);
            o += "[" + std::to_string(i) + "] " + sec.name + "  (Korg " + (marker ? marker : "-") + ")  " + std::to_string(sec.bars) + " bars " +
                 std::to_string(sec.beats_per_bar) + "/" + std::to_string(sec.beat_note) + "\n";
            for (size_t t = 0; t < sec.tracks.size(); ++t) {
                const auto& tr = sec.tracks[t];
                size_t notes = 0;
                for (const auto& e : tr.events) notes += e.type == uasf::MidiEventType::NoteOn && e.data2 > 0;
                o += "    [" + std::to_string(t) + "] ch " + std::to_string(int(tr.midi_channel) + 1) + (tr.is_drum ? " drum" : "") + "  '" + tr.name +
                     "'  " + std::to_string(notes) + " notes  " + style_edit::trackRoleName(tr.role) + " -> Pa5X " + partOf(tr.midi_channel) + "\n";
            }
        }
        return o;
    }

    // Applies one op to the selection (section/track nullopt = all). Returns "" or why it was refused (nothing changed).
    std::string apply(Op op, std::optional<size_t> section, std::optional<size_t> track, const std::string& value) {
        if (!ed_) return "open a Style first";
        if (section && *section >= style().sections.size()) return "no element " + std::to_string(*section);
        style_edit::Selection sel;
        sel.section = section;
        sel.track = track;
        long a = 0, b = 0;
        style_edit::EditResult r;
        switch (op) {
            case Op::Quantize: {
                if (!section) return "quantize works on one element: choose it";
                const auto sp = value.find(' ');
                const uint32_t g = style_edit::gridTicks(style().sections[*section].resolution, value.substr(0, sp));
                if (!g) return "grid must be 1/4, 1/8, 1/16 or 1/32 (t = triplet)";
                if (sp != std::string::npos && (!num(value.substr(sp + 1), &a) || a < 0 || a > 100)) return "strength must be 0-100";
                r = ed_->quantize(sel, g, sp == std::string::npos ? 100 : uint32_t(a));
                break;
            }
            case Op::Transpose:
                if (!num(value, &a) || a < -48 || a > 48) return "semitones must be a whole number from -48 to 48";
                r = ed_->transpose(sel, int(a));
                break;
            case Op::VelocitySet: case Op::VelocityOffset: case Op::VelocityScale:
                if (!num(value, &a) || a < -127 || a > 1000) return "velocity value must be a whole number";
                r = ed_->velocity(sel, op == Op::VelocitySet ? style_edit::VelocityMode::Set
                                       : op == Op::VelocityOffset ? style_edit::VelocityMode::Offset : style_edit::VelocityMode::ScalePercent, int(a));
                break;
            case Op::Tempo:
                if (!num(value, &a) || a < 0) return "tempo must be a whole number of BPM";
                r = ed_->setTempo(uint32_t(a));
                break;
            case Op::Meter: {
                if (!section) return "meter works on one element: choose it";
                const auto sl = value.find('/');
                if (sl == std::string::npos || !num(value.substr(0, sl), &a) || !num(value.substr(sl + 1), &b) || a < 1 || b < 1) return "meter must be num/den, e.g. 6/8";
                r = ed_->setMeter(*section, uint32_t(a), uint32_t(b));
                break;
            }
            case Op::ElementCopy: case Op::ElementClear: case Op::ElementRename: case Op::ElementLength: {
                if (!section) return "choose an element";
                if (op == Op::ElementCopy) {
                    const auto to = style_edit::sectionTypeFromName(value);
                    if (!to) return "copy to: Intro 1-3, Variation 1-4, Fill 1-4, Break or Ending 1-3";
                    r = ed_->copySection(*section, *to);
                } else if (op == Op::ElementClear) r = ed_->clearSection(*section);
                else if (op == Op::ElementRename) r = ed_->renameSection(*section, value);
                else {
                    if (!num(value, &a) || a < 1 || a > 256) return "length must be 1-256 bars";
                    r = ed_->setSectionBars(*section, uint32_t(a));
                }
                break;
            }
            case Op::TrackRole: case Op::TrackPart: case Op::TrackSound: {
                if (!section || !track || *track >= style().sections[*section].tracks.size()) return "choose an element and a track";
                const uint8_t ch = style().sections[*section].tracks[*track].midi_channel;
                if (op == Op::TrackRole) {
                    const auto role = style_edit::trackRoleFromName(value);
                    if (!role) return std::string("role must be one of: ") + opHint(Op::TrackRole);
                    r = ed_->setChannelRole(ch, *role);
                    break;
                }
                namespace k = exporters::korg;
                if (op == Op::TrackPart) {
                    std::string v = value;
                    for (auto& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
                    for (uint8_t g = 0; g < 16; ++g) {   // the source channel the exporter maps onto that Pa5X track
                        const uint8_t kch = k::korgChannelFor(g);
                        if (v == "none" ? kch == 0xFF : kch != 0xFF && v == k::soundMapKey(kch)) { opts_.route[ch] = g; return ""; }
                    }
                    return std::string("part must be one of: ") + opHint(Op::TrackPart);
                }
                const uint8_t kch = korgCh(ch);
                if (kch == 0xFF) return "this track is not exported to a Pa5X part: set Track: Pa5X part first";
                std::istringstream line(std::string(k::soundMapKey(kch)) + " " + value);
                const auto m = k::parseSoundMap(line, "sound");
                if (!m.ok) return "sound: " + m.error.substr(m.error.find(": ") + 2) + " (MSB LSB PC 0-127, or unresolved)";
                if (m.sounds.count(kch)) opts_.sounds[kch] = m.sounds.at(kch);
                else opts_.sounds.erase(kch);
                return "";
            }
        }
        return done(r);
    }

    bool undo() { return ed_ && ed_->undo(); }
    bool redo() { return ed_ && ed_->redo(); }
    std::string undoLabel() const { return ed_ && ed_->history().canUndo() ? ed_->history().undoLabel() : ""; }
    std::string redoLabel() const { return ed_ && ed_->history().canRedo() ? ed_->history().redoLabel() : ""; }

    // Replaces one track with the first SMF track that has events on it (plain SMF 0/1, read-only source).
    std::string importSmfTrack(const std::filesystem::path& smf, size_t section, size_t track, size_t* dropped = nullptr) {
        if (!ed_) return "open a Style first";
        if (section >= style().sections.size() || track >= style().sections[section].tracks.size()) return "choose an element and a track";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(smf, ec) || std::filesystem::file_size(smf, ec) > (8u << 20)) return smf.string() + ": not a regular file of at most 8 MiB";
        std::ifstream f(smf, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const auto file = style_edit::readSmf(bytes);
        if (!file.ok) return smf.string() + ": " + file.error;
        const auto& sec = style().sections[section];
        for (size_t t = 0; t < file.tracks.size(); ++t)
            if (!file.tracks[t].empty())
                return done(ed_->replaceTrackEvents(section, track,
                                                    style_edit::smfTrackToUasf(file, t, std::nullopt, sec.resolution, style_edit::sectionLengthTicks(sec), dropped),
                                                    "Import SMF track"));
        return smf.string() + ": no channel events";
    }

    // Pa5X Style SMF (EXPERIMENTAL): mapper -> writer -> our reader -> Korg validate(). That validation is its L1.
    std::string exportSmf(const std::filesystem::path& out, Verification* level) {
        if (!ed_) return "open a Style first";
        const auto x = smfExport();
        if (!x.ok) return "export refused: " + x.error;
        if (auto e = media::writeNewFile(out, x.bytes); !e.empty()) return e;
        if (level) *level = checkStyleSmf(out);
        saved_ = bytes();
        return "";
    }
    // UASF (our own format) of the edited Style. Written only when the UASF validator has no ERROR.
    std::string saveUasf(const std::filesystem::path& out) {
        if (!ed_) return "open a Style first";
        uasf::UasfSerializer ser;
        const auto r = ser.serialize(style());
        if (!r.success) return "serialize failed: " + r.error;
        if (auto e = media::writeNewFile(out, r.data); !e.empty()) return e;
        saved_ = r.data;
        return "";
    }

    // The Pa5X Style SMF of the Style now, with the CASM route and the user's sounds, and its conversion report (what
    // mapped, what was dropped, what does not carry over). Built on the message thread; the job only writes the bytes.
    style_edit::SmfExport smfExport() const {
        auto x = style_edit::exportPa5xSmf(style(), 0, opts_);
        std::string head = "Conversion: " + path_.filename().string() + " -> Korg Pa5X Style SMF separated by markers\n"
                           "Status: structurally valid (our reader + Korg Style SMF validator), not verified on a Pa5X\n";
        for (const auto& n : notes_) head += n + "\n";
        x.report = head + "\n" + x.report;
        return x;
    }
    const exporters::korg::MapOptions& exportOptions() const { return opts_; }

    // For an export job (jobs/export_specs.h): the UASF bytes of the Style now, and, once the job has written them,
    // marking them as written (dirty() stays true when the Style was edited after the job was made).
    std::vector<uint8_t> uasfBytes() const { return bytes(); }
    void markWritten(std::vector<uint8_t> uasf) { saved_ = std::move(uasf); }

    // L1 of a Pa5X Style SMF file: it parses as "Style SMF separated by markers" and the Korg validate() finds nothing.
    // There is no L2 (no second reader), and L3 needs an owner hardware test.
    static Verification checkStyleSmf(const std::filesystem::path& mid) {
        Verification v;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(mid, ec) || std::filesystem::file_size(mid, ec) > (8u << 20)) {
            v.l1 = {Check::Failed, "not a regular file of at most 8 MiB"};
            return v;
        }
        std::ifstream f(mid, std::ios::binary);
        const auto p = exporters::korg::parse({std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()});
        if (!p.ok) { v.l1 = {Check::Failed, "does not parse as a Korg Style SMF: " + p.error}; return v; }
        const auto issues = exporters::korg::validate(p.style);
        v.l1 = issues.empty() ? Level{Check::Passed, "Korg Style SMF validator, " + std::to_string(p.style.elements.size()) + " elements"}
                              : Level{Check::Failed, issues.front()};
        v.l2 = {Check::NotRun, "not applicable: no second Style SMF reader"};
        return v;
    }

private:
    static bool fail(std::string* e, std::string m) { if (e) *e = std::move(m); return false; }
    static bool num(const std::string& s, long* v) {
        if (s.empty()) return false;
        char* end = nullptr;
        *v = std::strtol(s.c_str(), &end, 10);
        return end && !*end;
    }
    std::string done(const style_edit::EditResult& r) {
        return r.ok ? "" : r.error.empty() ? std::string("refused by the validator") : r.error;
    }

    uint8_t korgCh(uint8_t ch) const {
        const auto r = opts_.route.find(ch);
        return exporters::korg::korgChannelFor(r == opts_.route.end() ? ch : r->second);
    }
    std::string partOf(uint8_t ch) const {
        const uint8_t kch = korgCh(ch);
        if (kch == 0xFF) return "none (not exported)";
        const auto s = opts_.sounds.find(kch);
        return std::string(exporters::korg::korgTrackName(kch)) + (s == opts_.sounds.end() ? " sound unresolved" :
               " sound " + std::to_string(s->second.msb) + "/" + std::to_string(s->second.lsb) + "/" + std::to_string(s->second.pc));
    }

    std::unique_ptr<style_edit::StyleEditor> ed_;
    exporters::korg::MapOptions opts_;   // CASM route + user Pa5X sounds for the Style SMF export
    std::vector<std::string> notes_;     // source format and what the conversion cannot carry
    std::filesystem::path path_;
    std::vector<uint8_t> saved_;   // UASF bytes of the Style as opened or last written
    std::vector<uint8_t> bytes() const {
        uasf::UasfSerializer ser;
        auto r = ser.serialize(style());
        return r.success ? std::move(r.data) : std::vector<uint8_t>{};
    }
};

}  // namespace ai_arranger::ui::studio

#endif
