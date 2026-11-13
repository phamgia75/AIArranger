// Style Studio editor model (ui/studio/style_studio_model.h): open the repo's synthetic SFF1 Style, browse it, edit with
// undo/redo, import an SMF track, and write NEW files only (Style SMF with its real level L1, UASF). Synthetic data only.

#include "ui/studio/style_studio_model.h"
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
    TEST("nothing staged after open", m.unsaved() == 0 && m.undoLabel().empty());

    const uint32_t tempo = m.style().tempo_bpm;
    TEST("tempo edit", m.apply(Op::Tempo, {}, {}, "97").empty() && m.style().tempo_bpm == 97 && m.unsaved() == 1);
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
    TEST("Style SMF export written", m.exportSmf(mid, &lvl).empty() && fs::is_regular_file(mid) && m.unsaved() == 0);
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

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
