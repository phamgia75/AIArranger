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
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"
#include "media/output_guard.h"
#include "style_edit/style_editor.h"
#include "ui/studio/studio_models.h"

#include <cstdlib>
#include <memory>

namespace ai_arranger::ui::studio {

class StyleStudioModel {
public:
    // The ops the Style Studio offers, in menu order, with what their value field holds.
    enum class Op { Quantize, Transpose, VelocitySet, VelocityOffset, VelocityScale, Tempo, Meter };
    static constexpr int kOps = 7;
    static const char* opName(Op o) {
        static const char* n[] = {"Quantize", "Transpose", "Velocity: set", "Velocity: offset", "Velocity: scale %", "Tempo", "Meter"};
        return n[int(o)];
    }
    static const char* opHint(Op o) {
        static const char* h[] = {"grid and strength, e.g. 1/16 80", "semitones, e.g. -2", "velocity 1-127", "offset, e.g. -10",
                                  "percent, e.g. 90", "BPM 20-400 (whole style)", "num/den, e.g. 6/8 (element only)"};
        return h[int(o)];
    }

    bool load(const std::filesystem::path& path, std::string* error) {
        uasf::StyleDefinition s;
        std::string ext = path.extension().string();
        for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".uasf") {
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
        }
        if (s.sections.empty()) return fail(error, path.string() + ": no Style elements with events");
        ed_ = std::make_unique<style_edit::StyleEditor>(std::move(s));
        path_ = path;
        unsaved_ = 0;
        return true;
    }
    bool loaded() const { return ed_ != nullptr; }
    const uasf::StyleDefinition& style() const { return ed_->style(); }
    const std::filesystem::path& path() const { return path_; }
    // Edits since the Style was opened or last written: the studio's staged changes (sources are never changed in place).
    int unsaved() const { return unsaved_; }

    // Element/track browser: one line per element, its tracks indented.
    std::string browser() const {
        if (!ed_) return "";
        const auto& s = style();
        std::string o = "Style: " + s.name + "  " + std::to_string(s.tempo_bpm) + " BPM  (from " + path_.filename().string() +
                        "; the source file is never changed)\n";
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
                     "'  " + std::to_string(notes) + " notes\n";
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
        }
        return done(r);
    }

    // shortcut: an undo/redo after a write counts as a change (it may overcount, never shows 0 while edited); a saved
    // revision marker would be exact if the staged count must ever drive a "discard changes?" prompt.
    bool undo() { return ed_ && ed_->undo() && (unsaved_ = unsaved_ > 0 ? unsaved_ - 1 : 1, true); }
    bool redo() { return ed_ && ed_->redo() && (++unsaved_, true); }
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
        const auto x = style_edit::exportPa5xSmf(style());
        if (!x.ok) return "export refused: " + x.error;
        if (auto e = media::writeNewFile(out, x.bytes); !e.empty()) return e;
        if (level) *level = checkStyleSmf(out);
        unsaved_ = 0;
        return "";
    }
    // UASF (our own format) of the edited Style. Written only when the UASF validator has no ERROR.
    std::string saveUasf(const std::filesystem::path& out) {
        if (!ed_) return "open a Style first";
        uasf::UasfSerializer ser;
        const auto r = ser.serialize(style());
        if (!r.success) return "serialize failed: " + r.error;
        if (auto e = media::writeNewFile(out, r.data); !e.empty()) return e;
        unsaved_ = 0;
        return "";
    }

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
        if (!r.ok) return r.error.empty() ? std::string("refused by the validator") : r.error;
        ++unsaved_;
        return "";
    }

    std::unique_ptr<style_edit::StyleEditor> ed_;
    std::filesystem::path path_;
    int unsaved_ = 0;
};

}  // namespace ai_arranger::ui::studio

#endif
