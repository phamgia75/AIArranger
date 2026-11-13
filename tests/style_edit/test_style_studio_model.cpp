// Style Studio editor model (ui/studio/style_studio_model.h): open the repo's synthetic SFF1 Style, browse it, edit with
// undo/redo, import an SMF track, and write NEW files only (Style SMF with its real level L1, UASF). Synthetic data only.

#include "ui/studio/style_studio_model.h"
#include <algorithm>
#include <cstdio>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::ui::studio;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

int main() {
    std::printf("Test: Style Studio model\n");
    const fs::path root = fs::temp_directory_path() / ("aia-style-studio-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    using Op = StyleStudioModel::Op;

    StyleStudioModel m;
    std::string err;
    TEST("nothing open: ops refused", !m.apply(Op::Tempo, {}, {}, "100").empty());
    TEST("unreadable file refused", !m.load(root / "none.sty", &err) && !err.empty() && !m.loaded());
    const fs::path sty = fs::path(REPO_DIR) / "fixtures/synthetic/SYNTH_WALTZ_68.sty";
    TEST("SFF1 Style opens for editing", m.load(sty, &err) && m.loaded());
    const auto b = m.browser();
    TEST("browser lists elements and tracks", has(b, "[0] ") && has(b, "    [0] ch ") && has(b, "notes") && has(b, "never changed"));
    TEST("nothing staged after open", !m.dirty() && m.undoLabel().empty());

    const uint32_t tempo = m.style().tempo_bpm;
    TEST("tempo edit", m.apply(Op::Tempo, {}, {}, "97").empty() && m.style().tempo_bpm == 97 && m.dirty());
    TEST("undo label names the op", m.undoLabel() == "Tempo");
    TEST("undo restores", m.undo() && m.style().tempo_bpm == tempo && m.redoLabel() == "Tempo");
    TEST("redo re-applies", m.redo() && m.style().tempo_bpm == 97);
    TEST("bad tempo refused, nothing changed", !m.apply(Op::Tempo, {}, {}, "fast").empty() && !m.apply(Op::Tempo, {}, {}, "9999").empty() &&
         m.style().tempo_bpm == 97);
    TEST("quantize needs an element", !m.apply(Op::Quantize, {}, {}, "1/16").empty());
    TEST("quantize bad grid refused", !m.apply(Op::Quantize, 0, {}, "1/7").empty());
    TEST("quantize 1/16 at 80%", m.apply(Op::Quantize, 0, {}, "1/16 80").empty());
    TEST("transpose", m.apply(Op::Transpose, 0, {}, "2").empty());
    TEST("velocity scale", m.apply(Op::VelocityScale, {}, {}, "90").empty());
    TEST("velocity set out of range clamps or refuses, never crashes", (void(m.apply(Op::VelocitySet, {}, {}, "500")), true));
    TEST("meter needs num/den", !m.apply(Op::Meter, 0, {}, "68").empty());

    // ── SMF track import ──
    const fs::path mid = root / "ours.mid";
    Verification lvl;
    TEST("Style SMF export written", m.exportSmf(mid, &lvl).empty() && fs::is_regular_file(mid) && !m.dirty());
    TEST("Style SMF real level: L1 only (validator), L2 not applicable, no L3",
         lvl.l1.state == Check::Passed && lvl.l2.state == Check::NotRun && lvl.l3.state == Check::NotRun && has(lvl.l1.detail, "validator"));
    TEST("existing SMF never overwritten", has(m.exportSmf(mid, &lvl), "already exists"));
    TEST("element out of range refused", !m.apply(Op::Quantize, 99, {}, "1/16").empty() && !m.apply(Op::Meter, 99, {}, "3/4").empty());
    fs::create_symlink(root / "victim", root / "linked.mid.tmp");   // dangling link at the temp path
    TEST("a symlink at the temp path is never followed", has(m.exportSmf(root / "linked.mid", &lvl), "refusing") && !fs::exists(root / "victim") &&
         !fs::exists(root / "linked.mid"));
    TEST("SMF onto /Volumes refused", has(m.exportSmf("/Volumes/AIA-STUDIO-TEST/x.mid", &lvl), "mounted volume"));
    size_t dropped = 0;
    TEST("import SMF track: needs a track", !m.importSmfTrack(mid, 99, 0).empty());
    TEST("import SMF track from our own export", m.importSmfTrack(mid, 0, 0, &dropped).empty() && m.undoLabel() == "Import SMF track");
    TEST("garbage SMF refused", (std::ofstream(root / "bad.mid") << "nope", !m.importSmfTrack(root / "bad.mid", 0, 0).empty()));
    TEST("checkStyleSmf: garbage fails L1", StyleStudioModel::checkStyleSmf(root / "bad.mid").l1.state == Check::Failed);

    // ── UASF save and reopen ──
    TEST("UASF written", m.saveUasf(root / "edited.uasf").empty());
    StyleStudioModel back;
    TEST("UASF reopens with the edit", back.load(root / "edited.uasf", &err) && back.style().tempo_bpm == 97);
    // ── Codex C4: dirty = the model differs from what was last written (save, then undo twice) ──
    {
        StyleStudioModel d;
        d.load(sty, &err);
        d.apply(Op::Tempo, {}, {}, "101");
        d.apply(Op::Tempo, {}, {}, "102");
        TEST("C4: written -> nothing staged", d.saveUasf(root / "c4.uasf").empty() && !d.dirty());
        d.undo();
        d.undo();
        TEST("C4: save + undo twice -> still staged (model differs from the written file)", d.dirty());
        d.redo();
        d.redo();
        TEST("C4: redo back to the written state -> nothing staged", !d.dirty());
    }
    // ── Codex C3: an invalid style never becomes an editable model ──
    {
        uasf::UasfDeserializer des;
        auto good = des.deserializeFromFile(std::string(REPO_DIR) + "/fixtures/synthetic/SYNTH_FULL_SFF2.uasf");
        TEST("C3 fixture readable", good.success && !good.style.sections.empty());
        good.style.sections[0].beats_per_bar = 0;
        uasf::UasfSerializer ser;
        const auto bytes = ser.serialize(good.style);
        std::ofstream(root / "invalid.uasf", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data.data()), std::streamsize(bytes.data.size()));
        StyleStudioModel inv;
        std::string why;
        TEST("C3: invalid style refused with the validator message", bytes.success && !inv.load(root / "invalid.uasf", &why) && !inv.loaded() &&
             has(why, "validator"));
    }
    // ── Codex C2: opening another Style never silently drops unwritten edits ──
    {
        StyleStudioModel a;
        a.load(sty, &err);
        a.apply(Op::Tempo, {}, {}, "88");
        std::string why;
        TEST("C2: open B while A has unwritten edits -> refused, A intact with its undo",
             !a.load(root / "edited.uasf", &why) && a.style().tempo_bpm == 88 && a.undoLabel() == "Tempo" && has(why, "not written"));
        TEST("C2: open B after an explicit discard", a.load(root / "edited.uasf", &why, true) && a.style().tempo_bpm == 97);
    }
    // ── P1: element/track ops, Pa5X part + sound mapping, Yamaha -> Korg conversion report, Korg SMF reopen ──
    {
        StyleStudioModel f;
        std::string why;
        TEST("P1: SFF2 Style opens", f.load(fs::path(REPO_DIR) / "fixtures/synthetic/SYNTH_FULL_SFF2.sty", &why));
        TEST("P1: source format and CASM routing listed", has(f.browser(), "Source: Yamaha SFF2, ") && has(f.browser(), "CASM routing applied"));
        const size_t n = f.style().sections.size();
        TEST("P1: element op needs an element", !f.apply(Op::ElementClear, {}, {}, "").empty());
        TEST("P1: copy to an unknown element refused", has(f.apply(Op::ElementCopy, 0, {}, "Verse"), "Intro 1-3"));
        TEST("P1: copy element 0 to Intro 3", f.apply(Op::ElementCopy, 0, {}, "intro 3").empty() && f.style().sections.size() == n &&
             f.undoLabel() == "Copy to Intro 3");
        TEST("P1: rename + length + clear", f.apply(Op::ElementRename, 1, {}, "My Var").empty() && f.style().sections[1].name == "My Var" &&
             f.apply(Op::ElementLength, 1, {}, "8").empty() && f.style().sections[1].bars == 8 && !f.apply(Op::ElementLength, 1, {}, "0").empty() &&
             f.apply(Op::ElementClear, 1, {}, "").empty() && f.undoLabel() == "Clear element");
        TEST("P1: track op needs a track", !f.apply(Op::TrackRole, 0, {}, "pad").empty());
        TEST("P1: bad role refused", has(f.apply(Op::TrackRole, 0, 0, "lead"), "role must be"));
        const uint8_t ch = f.style().sections[0].tracks.back().midi_channel;
        const size_t last = f.style().sections[0].tracks.size() - 1;
        TEST("P1: role is one undo step", f.apply(Op::TrackRole, 0, last, "melody").empty() && f.undoLabel() == "Track role" &&
             f.style().sections[0].tracks[last].role == uasf::TrackRole::Melody);
        TEST("P1: bad part refused", has(f.apply(Op::TrackPart, 0, last, "acc9"), "part must be"));
        TEST("P1: part acc5", f.apply(Op::TrackPart, 0, last, "ACC5").empty() && f.style().korg_export.route.at(ch) == 15 && has(f.browser(), "-> Pa5X Acc5"));
        TEST("P1: bad sound refused", !f.apply(Op::TrackSound, 0, last, "200 0 0").empty() && !f.apply(Op::TrackSound, 0, last, "1 2").empty() &&
             f.style().korg_export.sounds.empty());
        TEST("P1: sound set", f.apply(Op::TrackSound, 0, last, "121 1 34").empty() && f.style().korg_export.sounds.at(15).pc == 34 &&
             has(f.browser(), "Acc5 sound 121/1/34"));
        const auto x = f.smfExport();
        TEST("P1: export carries the sound", x.ok && std::any_of(x.korg.elements[0].events.begin(), x.korg.elements[0].events.end(),
                                                                 [](const auto& e) { return e.status == 0xCF && e.d1 == 34; }));
        TEST("P1: conversion report", has(x.report, "not verified on a Pa5X") && has(x.report, "SFF2") && has(x.report, "CASM/NTT") &&
             has(x.report, "Section map") && has(x.report, "Dropped events") && has(x.report, "MSB 121 LSB 1 PC 34 (user map)"));
        TEST("P1: part none = not exported", f.apply(Op::TrackPart, 0, last, "none").empty() && has(f.browser(), "none (not exported)") &&
             has(f.apply(Op::TrackSound, 0, last, "1 2 3"), "not exported"));
        TEST("P1: part back + sound unresolved", f.apply(Op::TrackPart, 0, last, "acc5").empty() &&
             f.apply(Op::TrackSound, 0, last, "unresolved").empty() && f.style().korg_export.sounds.empty());
        f.apply(Op::TrackSound, 0, last, "121 1 34");
        // Codex r1 #1 / owner gate 1: part/sound are Style edits (UASF 'KEXP'): dirty, undo/redo, Save UASF + reload keep them
        {
            StyleStudioModel g;
            g.load(fs::path(REPO_DIR) / "fixtures/synthetic/SYNTH_FULL_SFF2.sty", &why);
            TEST("r1#1: a sound change alone is an unwritten edit", g.apply(Op::TrackSound, 0, 0, "1 2 3").empty() && g.dirty() &&
                 g.undoLabel() == "Pa5X sound");
            TEST("r1#1: undo / redo of the sound", g.undo() && !g.dirty() && g.redo() && g.dirty());
            TEST("r1#1: opening another Style is refused", !g.load(root / "edited.uasf", &why) && has(why, "not written"));
            TEST("r1#1: a part change is an undo step", g.apply(Op::TrackPart, 0, 0, "acc4").empty() && g.undoLabel() == "Pa5X part");
            TEST("r1#1: element sound needs a track, then is an undo step", !g.apply(Op::ElementSound, 2, {}, "4 5 6").empty() &&
                 g.apply(Op::ElementSound, 2, 0, "4 5 6").empty() && g.undoLabel() == "Pa5X element sound" &&
                 has(g.browser(), "sound 4/5/6 (this element)"));
            TEST("rename is an unwritten edit", g.apply(Op::ElementRename, 1, {}, "Renamed").empty() && g.dirty());
            TEST("r1#1: Save UASF writes them (nothing unwritten after)", g.saveUasf(root / "r1.uasf").empty() && !g.dirty());
            StyleStudioModel back2;
            TEST("r1#1: reload keeps part, sound and element sound", back2.load(root / "r1.uasf", &why) &&
                 back2.style().korg_export.route == g.style().korg_export.route && back2.style().korg_export.sounds == g.style().korg_export.sounds &&
                 back2.style().korg_export.element_sounds == g.style().korg_export.element_sounds &&
                 back2.style().sections[1].name == "Renamed" && !back2.dirty());
            TEST("r1#1: the reloaded Style exports the same SMF", back2.smfExport().bytes == g.smfExport().bytes);
            Verification l;
            TEST("r1#1: export after reload", back2.exportSmf(root / "r1.mid", &l).empty() && l.l1.state == Check::Passed);
            StyleStudioModel back3;
            TEST("r1#1: reopening the SMF restores the sounds, element sound included", back3.load(root / "r1.mid", &why) &&
                 back3.smfExport().bytes == back2.smfExport().bytes);
        }
        // Owner gate 5/6: create a new Intro, Variation, Fill and Ending, edit each, save, reload, export
        {
            StyleStudioModel w;
            w.load(sty, &why);   // SYNTH_WALTZ_68: Main A/B, Fill AA/BB, Intro A, Ending A
            const char* kinds[] = {"Intro 2", "Variation 3", "Fill 3", "Ending 2"};
            const uasf::SectionType types[] = {uasf::SectionType::Intro2, uasf::SectionType::Main3, uasf::SectionType::Fill3, uasf::SectionType::Ending2};
            bool all = true;
            for (int i = 0; i < 4; ++i) {
                const size_t n = w.style().sections.size();
                const bool made = w.apply(Op::ElementCopy, 0, {}, kinds[i]).empty() && w.style().sections.size() == n + 1 &&
                                  w.style().sections[n].type == types[i] && w.style().sections[n].name == kinds[i];
                const bool edited = w.apply(Op::Transpose, n, {}, "1").empty() && w.apply(Op::ElementLength, n, {}, "6").empty() &&
                                    w.apply(Op::ElementRename, n, {}, std::string("New ") + kinds[i]).empty() &&
                                    w.apply(Op::ElementSound, n, 0, std::to_string(10 + i) + " 0 " + std::to_string(i)).empty() &&
                                    w.style().sections[n].bars == 6;
                TEST((std::string("gate5: new ") + kinds[i] + " created, selected by index and edited").c_str(), made && edited);
                all = all && made && edited;
            }
            TEST("gate6: save after the new elements", all && w.saveUasf(root / "gate.uasf").empty() && !w.dirty());
            StyleStudioModel r;
            TEST("gate6: reload has the 4 new elements with their names, lengths and sounds", r.load(root / "gate.uasf", &why) &&
                 r.style().sections.size() == 10 && r.style().sections[9].name == "New Ending 2" && r.style().sections[6].bars == 6 &&
                 r.style().korg_export.element_sounds.size() == 4);
            const auto x = r.smfExport();
            auto marker = [&](const char* m) { return std::any_of(x.korg.elements.begin(), x.korg.elements.end(), [&](const auto& e) { return e.marker == m; }); };
            TEST("gate6: export has i2cv1, v3cv1, f3cv1, e2cv1 and each element's sound", x.ok && marker("i2cv1") && marker("v3cv1") && marker("f3cv1") &&
                 marker("e2cv1") && has(x.report, "e2cv1 ") && has(x.report, "MSB 13 LSB 0 PC 3"));
        }
        Verification lvl;
        TEST("P1: Korg SMF written", f.exportSmf(root / "conv.mid", &lvl).empty() && lvl.l1.state == Check::Passed);

        StyleStudioModel k;
        TEST("P1: our Korg Style SMF reopens for editing", k.load(root / "conv.mid", &why) && has(k.browser(), "Korg Style SMF") &&
             k.style().korg_export.sounds.at(15).msb == 121);
        TEST("P1: re-export of the reopened SMF is byte-identical", k.smfExport().bytes == f.smfExport().bytes);
        std::ofstream(root / "plain.mid", std::ios::binary) << std::string("MThd\0\0\0\6\0\0\0\1\0\x60MTrk\0\0\0\4\0\xFF\x2F\0", 26);
        TEST("P1: a plain SMF without markers is refused", !k.load(root / "plain.mid", &why, true) && has(why, "Korg"));

        // Codex r3 / own limit: a .sty that UASF cannot save (33 elements: the serializer would drop the 33rd) is refused at open
        {
            // Evidence that a .sty cannot open as an unsaveable Style: the SFF mapper ignores duplicate markers (at most
            // 15 elements) and replaces a tempo outside 20-400 BPM by the default; the open-time saveError() check stays
            // as the guard for every source.
            auto sty = [&](const fs::path& p, bool tempo500) {
                std::string trk = std::string("\0\xFF\x06\x04SFF2", 8);
                if (tempo500) trk += std::string("\0\xFF\x51\x03\x01\xD4\xC0", 7);   // 120000 us/q = 500 BPM
                for (int i = 0; i < 33; ++i) trk += std::string(i ? "\x83\x00" : "\x00", i ? 2 : 1) + std::string("\xFF\x06\x06Main A", 9) +
                                                    std::string("\0\x9B\x3C\x64\x30\x8B\x3C\0", 8);
                trk += std::string("\x83\x00\xFF\x2F\0", 5);
                std::string smf = std::string("MThd\0\0\0\6\0\0\0\1\0\x60MTrk", 18);
                const uint32_t n = uint32_t(trk.size());
                smf += char(n >> 24); smf += char(n >> 16); smf += char(n >> 8); smf += char(n);
                std::ofstream(p, std::ios::binary) << smf + trk;
            };
            sty(root / "many.sty", true);
            StyleStudioModel m33;
            TEST("r3: a .sty with 33 'Main A' markers and 500 BPM opens as 1 element at the default tempo, and saves",
                 m33.load(root / "many.sty", &why) && m33.style().sections.size() == 1 && m33.style().tempo_bpm == uasf::kDefaultTempoBpm &&
                 m33.saveUasf(root / "many.uasf").empty());
        }
        // An SFF1-tagged Style (no Ctb2 CASM): the report says nothing was routed.
        const std::string trk = std::string("\0\xFF\x06\x04SFF1", 8) + std::string("\0\xFF\x06\x06Main A", 10) +
                                std::string("\0\x9B\x3C\x64\x60\x8B\x3C\0\x82\x20\xFF\x2F\0", 13);   // ch 12 note, EOT at 384
        std::string sff1 = std::string("MThd\0\0\0\6\0\0\0\1\0\x60MTrk\0\0\0", 21) + char(trk.size()) + trk;
        std::ofstream(root / "old.sty", std::ios::binary) << sff1;
        StyleStudioModel o;
        TEST("P1: SFF1 Style opens", o.load(root / "old.sty", &why));
        TEST("P1: SFF1 without CASM reported", has(o.browser(), "Source: Yamaha SFF1, 0 CASM track configs (none read") &&
             has(o.smfExport().report, "SFF1 Ctab blocks are not parsed"));
    }
    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
