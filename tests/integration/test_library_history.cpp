// Library Manager staged change set (undo/redo before Execute Plan), per-step effects, and where-used. Synthetic
// libraries only (placeholder bytes, Pa-native containers from tests/support/panative_fixture.h); no owner data.

#include "library/library_history.h"
#include "library/library_where_used.h"
#include "../support/panative_fixture.h"
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger::library;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static void put(const fs::path& p, const std::string& b) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << b; }
static size_t find(const Library& L, const std::string& name) {
    for (size_t i = 0; i < L.items.size(); ++i) if (trimmed(L.items[i].name) == name) return i;
    return size_t(-1);
}
static bool has(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }
static size_t count(const std::string& s, const std::string& x) {
    size_t n = 0;
    for (size_t p = s.find(x); p != std::string::npos; p = s.find(x, p + 1)) ++n;
    return n;
}

int main() {
    std::printf("Test: Library Manager history (staged undo/redo), step effects, where-used\n");
    const fs::path root = fs::temp_directory_path() / ("aia-libhist-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path ka = root / "A.KST", kb = root / "B.KST";
    put(ka / "STYLE/USER01/Ballad.STG", "AIARRANGER-SYNTHETIC ballad");
    put(ka / "STYLE/USER01/Swing.STG", "AIARRANGER-SYNTHETIC swing");
    put(ka / "SOUND/USER01/Piano.PCG", "AIARRANGER-SYNTHETIC piano");
    put(ka / "PAD/USER01/Hit.PDC", "AIARRANGER-SYNTHETIC pad");
    put(ka / "KEYBOARDSET/USER01/Live.KSC", "AIARRANGER-SYNTHETIC kbs");
    put(kb / "STYLE/USER02/Waltz.STG", "AIARRANGER-SYNTHETIC waltz");
    const auto A = load(ka), B = load(kb);
    TEST("synthetic KST libraries load", A.ok && B.ok && A.items.size() == 5 && B.items.size() == 1);
    const size_t ballad = find(A, "Ballad"), swing = find(A, "Swing"), piano = find(A, "Piano");

    // ── staged change set: undo / redo before Execute Plan ──
    PlanHistory h;
    const Step s1{Op::Copy, 'A', ballad, "STYLE/USER02", -1, ""}, s2{Op::Rename, 'A', swing, "", -1, "Swing2"}, s3{Op::Copy, 'A', piano, "", -1, ""};
    h.add(s1, stepLabel(A, s1));
    h.add(s2, stepLabel(A, s2));
    h.add(s3, stepLabel(A, s3));
    TEST("steps accumulate in order", h.steps().size() == 3 && h.steps()[1].op == Op::Rename && !h.canRedo());
    TEST("undo label names the step", h.undoLabel() == "add copy in A: \"Piano\" -> B:same bank");
    TEST("multi-step undo", h.undo() && h.undo() && h.steps().size() == 1 && h.steps()[0].item == ballad && h.redoLabel() == "add rename in A: \"Swing\" -> \"Swing2\"");
    TEST("multi-step redo restores the exact steps", h.redo() && h.redo() && h.steps().size() == 3 && h.steps()[2].item == piano && !h.redo());
    TEST("remove last and clear are undoable", h.removeLast() && h.steps().size() == 2 && h.clear() && h.empty() && h.undo() && h.steps().size() == 2 &&
         h.undo() && h.steps().size() == 3);
    h.undo();   // back to [s1, s2]
    h.add(s3, stepLabel(A, s3));
    TEST("a new step after undo drops the redo branch", !h.canRedo() && h.steps().size() == 3);
    TEST("history text says nothing is written until Execute Plan", has(h.text(), "Staged only: nothing is written until Execute Plan") &&
         has(h.text(), "to undo (last: add copy in A: \"Piano\""));
    h.reset();
    TEST("reopening a library resets plan and history", h.empty() && !h.canUndo() && !h.canRedo() && !h.undo());
    TEST("empty remove / clear record nothing", !h.removeLast() && !h.clear() && !h.canUndo());

    // ── each step shows its effect ──
    {
        const std::vector<Step> plan = {s1, s2, {Op::Copy, 'A', ballad, "STYLE/USER02", -1, ""}, {Op::BankSwap, 'B', 0, "STYLE/USER02", -1, "STYLE/USER03"}};
        const auto r = planReport(A, B, plan, evaluate(A, B, plan));
        TEST("copy effect: adds the file to the other side", has(r, "effect    adds B:STYLE/USER02/Ballad.STG (a copy of A:STYLE/USER01/Ballad.STG)"));
        TEST("rename effect: renames the file", has(r, "effect    renames A:STYLE/USER01/Swing.STG -> STYLE/USER01/Swing2.STG"));
        TEST("a step with nothing to do says no file change", has(r, "effect    no file change"));
        TEST("swap effect names both bank folders", has(r, "effect    swaps bank folders B:STYLE/USER02 <-> STYLE/USER03"));
        TEST("every effect is in the NEW library, not the sources", count(r, "in the NEW library Execute Plan writes") == 3);
        const std::vector<Step> bad = {{Op::Rename, 'A', swing, "", -1, "Bad/Name"}};
        TEST("a blocked step has no effect", has(planReport(A, B, bad, evaluate(A, B, bad)), "effect    none: blocked by its conflicts"));
    }

    // ── where-used: only references read from data; opaque payloads are UNKNOWN ──
    {
        const auto w = whereUsed(A, A.items[piano], &B);
        TEST("Sound: references UNKNOWN, none inferred", has(w.verdict, "references UNKNOWN"));
        auto line = [&](const WhereUsed& x, const std::string& start) {
            for (const auto& l : x.lines) if (l.rfind(start, 0) == 0) return l;
            return std::string();
        };
        TEST("Sound: Style referrers counted in both libraries, KST undecoded", has(line(w, "StyleTrack from Style: "), "2 Style item(s) in A.KST") &&
             has(line(w, "StyleTrack from Style: "), "1 Style item(s) in B.KST") && has(line(w, "StyleTrack from Style: "), "container UNKNOWN, D14"));
        TEST("Sound: Keyboard Set and Pad referrers listed", has(line(w, "KeyboardSetPart from KeyboardSet: "), "1 Keyboard Set item(s) in A.KST") &&
             has(line(w, "PadTrack from Pad: "), "1 Pad item(s) in A.KST"));
        TEST("Sound: a referrer kind with no open item is not proof of no reference",
             has(line(w, "SongBookSound from SongBookEntry: "), "no SongBook item in the open libraries") && has(line(w, "SongBookSound from SongBookEntry: "), "(UNKNOWN)"));
        TEST("Sound: every reference kind of the resource graph is covered", w.lines.size() == 8);   // StyleTrack, KeyboardSetPart x3, PadTrack, SongBookSound, ProgramChange x2
        const auto ws = whereUsed(A, A.items[ballad]);
        TEST("Style: referrers not modelled, said so", ws.lines.size() == 1 && has(ws.lines[0], "what refers to a Style is not modelled"));
        const auto self = whereUsed(A, A.items[find(A, "Live")], &B);
        TEST("Keyboard Set: no invented referrer", self.lines.size() == 1 && has(self.lines[0], "not modelled") && has(self.verdict, "UNKNOWN"));
        const auto text = whereUsedText(A, A.items[piano], &B);
        TEST("text: known references none, not proven unused", has(text, "Known references: none") && has(text, "Not proven unused"));

        const fs::path sa = root / "SetA";
        put(sa / "GLOBAL/LIMITER.AUD", "x");
        put(sa / "STYLE/USER01.STY", build({{0x06000030, 2, 1, 0, "Synth Pop", "OC31p"}}, 1));
        put(sa / "SOUND/USER01.PCG", build({{0x09020130, 1, 3, 0, "Synth Lead", "OC31s"}}, 1));
        const auto SA = load(sa);
        const size_t lead = find(SA, "Synth Lead");
        TEST("SET: the opaque OC31 payload is the reason", SA.ok && lead != size_t(-1) &&
             has(line(whereUsed(SA, SA.items[lead]), "StyleTrack from Style: "), "1 Style item(s) in SetA, whose Pa-native item payloads are opaque (most start \"OC31\", E5)"));
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
