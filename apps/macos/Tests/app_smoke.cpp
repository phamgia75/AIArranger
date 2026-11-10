// P2.5 UI smoke test: the real LiveHostComponent (the app's window content),
// driven only through what a user can reach: buttons found by their visible
// text, clicked with triggerClick(), the JUCE message loop run, and the labels
// the user sees read back. No hardware: no output is selected and
// AIARR_MIDI_HW_TX=0 blocks any send at the CoreMIDI boundary.

#include <juce_gui_extra/juce_gui_extra.h>
#include "../Source/LiveHostComponent.h"
#include "../Source/ResourcesComponent.h"
#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include "media/sound/package_writer.h"
#include "media/sound/wav_writer.h"
#include <chrono>
#include <csignal>
#include <mach/mach.h>
#include <fstream>
#include <cmath>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

template <typename T>
static T* find(juce::Component& root, const juce::String& textPrefix) {
    for (auto* c : root.getChildren()) {
        if (auto* b = dynamic_cast<juce::Button*>(c); b && std::is_base_of_v<juce::Button, T>)
            if (b->getButtonText() == textPrefix) return dynamic_cast<T*>(c);
        if (auto* l = dynamic_cast<juce::Label*>(c); l && std::is_same_v<T, juce::Label>)
            if (l->getText().startsWith(textPrefix)) return dynamic_cast<T*>(c);
        if (auto* s = dynamic_cast<juce::Slider*>(c); s && std::is_same_v<T, juce::Slider>)
            return dynamic_cast<T*>(c);
    }
    return nullptr;
}

static void click(juce::Component& root, const char* text) {
    if (auto* b = find<juce::Button>(root, text)) b->triggerClick();
    else { std::fprintf(stderr, "  FAIL: no button '%s'\n", text); ++failures; }
    pump(60);
}

static juce::String section(juce::Component& root) {
    auto* l = find<juce::Label>(root, "Section:");
    return l ? l->getText() : juce::String();
}

int main() {
    setenv("AIARR_MIDI_HW_TX", "0", 1);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf("Test: macOS app UI smoke (LiveHostComponent)\n");

    {
        auto host = std::make_unique<ai_arranger::app::LiveHostComponent>();
        pump(200);
        TEST("constructs at its design size", host->getWidth() == 720 && host->getHeight() == 460);
        for (const char* b : {"Start", "Stop", "Sync Start", "Intro", "A", "B", "C", "D", "Fill",
                              "Break", "Ending", "PANIC", "Refresh", "Load Style...", "Auto Fill"})
            TEST((juce::String("button present: ") + b).toRawUTF8(), find<juce::Button>(*host, b) != nullptr);
        TEST("starts stopped", section(*host).contains("(stopped)"));

        click(*host, "Start");
        pump(300);
        TEST("Start -> section label says playing", section(*host).contains("(playing)"));

        for (const char* b : {"B", "C", "D", "A", "Fill", "Break", "Intro"}) click(*host, b);
        pump(200);
        TEST("section controls while playing keep it playing", section(*host).contains("(playing)"));

        if (auto* s = find<juce::Slider>(*host, "")) s->setValue(150.0, juce::sendNotificationSync);
        pump(200);
        auto* tempo = find<juce::Label>(*host, "Tempo ");
        TEST("tempo slider -> engine -> label 'Tempo 150'", tempo && tempo->getText() == "Tempo 150");

        auto* af = find<juce::ToggleButton>(*host, "Auto Fill");
        const bool before = af && af->getToggleState();
        click(*host, "Auto Fill");
        TEST("Auto Fill toggles", af && af->getToggleState() != before);

        click(*host, "Stop");
        pump(300);
        TEST("Stop -> section label says stopped", section(*host).contains("(stopped)"));

        click(*host, "Ending");   // ending while stopped: must not start playback
        pump(200);
        TEST("Ending while stopped stays stopped", section(*host).contains("(stopped)"));

        click(*host, "Start");
        pump(200);
        click(*host, "PANIC");
        pump(200);
        TEST("PANIC while playing stops", section(*host).contains("(stopped)"));

        click(*host, "Start");
        pump(200);
    }   // destroyed while playing: the stop path must silence and join cleanly
    TEST("destroyed while playing without hang or crash", true);

    bool cycles = true;
    for (int i = 0; i < 5; ++i) {
        auto h = std::make_unique<ai_arranger::app::LiveHostComponent>();
        pump(50);
        click(*h, "Start");
        cycles &= h->getWidth() == 720;
    }
    TEST("5 create/start/destroy cycles", cycles);

    // ── Resources window (read-only): what a musician reaches from "Resources..." ──
    {
        namespace fs = std::filesystem;
        using namespace ai_arranger;
        auto host = std::make_unique<app::LiveHostComponent>();
        TEST("button present: Sound Tools...", find<juce::Button>(*host, "Sound Tools...") != nullptr);
        const fs::path root = fs::temp_directory_path() / ("aia-smoke-res-" + std::to_string(::getpid()));
        fs::remove_all(root);
        std::string err;
        media::sound::writePackage(media::sound::buildDemoOrgan(), root / "organ", "", &err);
        media::sound::writeDrumPackage(media::sound::buildDemoKit(), root / "drums", "", &err);
        {   // a load still running when the window goes away must not touch it (Codex P1/P2 round 1)
            auto gone = std::make_unique<app::ResourcesComponent>();
            gone->openPackage(juce::File(root.string() + "/organ"));
            gone.reset();
            pump(400);
            TEST("window destroyed during an async package load: no crash", true);
        }
        app::ResourcesComponent rc;
        auto open = [&](const char* sub) {
            rc.openPackage(juce::File(root.string() + sub));
            TEST("package load runs off the message thread (busy while loading)", rc.loading());
            for (int i = 0; i < 100 && rc.loading(); ++i) pump(20);
            return !rc.loading() && rc.detailsText().startsWith("Opened");
        };
        rc.setSize(980, 640);
        TEST("tabs in order", rc.tabs().getTabNames()[1] == "KST Library" && rc.tabs().getTabNames()[3] == "Convert Sound");
        TEST("empty: tells the user what to open", rc.detailsText().contains("Open Package"));
        open("/organ");
        rc.compareMemory();
        TEST("Memory Check: the untouched template compares without an error", rc.memoryText().contains("OBSERVATION ONLY") &&
             rc.memoryText().contains("Samples: not compared"));
        rc.model().clear();
        TEST("open two packages", open("/organ") && open("/drums"));
        TEST("same package twice refused with a reason", !open("/organ") && rc.detailsText().contains("already open"));
        {   // Close All while a load is pending: the load is dropped, not re-added (Codex round 2)
            app::ResourcesComponent c2;
            c2.openPackage(juce::File(root.string() + "/organ"));
            c2.model().clear();
            for (int i = 0; i < 100 && c2.loading(); ++i) pump(20);
            TEST("Close All during a load: the load is dropped", c2.model().packages().empty());
            std::ofstream(root / "ev.txt") << "aiarranger-evidence\t1\nsource\tR\towner ok\tpa-manager-report\nnode\tpa5x:USR/Style/1\tBallad\nend\t3\n";
            c2.openEvidence(juce::File(root.string() + "/ev.txt"));
            TEST("evidence loads off the message thread", c2.loading());
            for (int i = 0; i < 100 && c2.loading(); ++i) pump(20);
            TEST("evidence opened", c2.model().evidence().size() == 1 && c2.detailsText().contains("Opened evidence"));
        }
        auto* top = rc.tree().getRootItem();
        TEST("tree: Sound + Drum Kit at the top", top && top->getNumSubItems() == 2);
        juce::TreeViewItem* sound = nullptr;
        for (int i = 0; top && i < top->getNumSubItems(); ++i)
            if (top->getSubItem(i)->getUniqueName().contains("/Sound/")) sound = top->getSubItem(i);
        if (sound) sound->setOpen(true);
        TEST("expand Sound -> 2 oscillators", sound && sound->getNumSubItems() == 2);
        TEST("tree rows are announced by name and tag, not 'Level n row m' (GUI validation)",
             sound && sound->getAccessibilityName().startsWith("Sound '") && sound->getAccessibilityName().endsWith(", NO REFS"));
        auto* osc = sound && sound->getNumSubItems() ? sound->getSubItem(0) : nullptr;
        if (osc) osc->setOpen(true);
        auto* ms = osc && osc->getNumSubItems() ? osc->getSubItem(0) : nullptr;
        if (ms) ms->setOpen(true);
        TEST("expand oscillator -> multisample -> samples", ms && ms->getNumSubItems() > 0);
        if (ms && ms->getNumSubItems()) ms->getSubItem(0)->setSelected(true, true);
        pump(50);
        TEST("select a sample: exact bytes, where used, delete verdict, provisional identity",
             rc.detailsText().contains("uncompressed PCM:") && rc.detailsText().contains("Where used: 1") &&
             rc.detailsText().contains("Delete safety: IN USE") && rc.detailsText().contains("provisional: not a Pa5X slot"));
        TEST("summary: budget and unresolved dependencies", rc.summaryText().contains("Open: 2 package(s)") &&
             rc.summaryText().contains("Unresolved dependencies: 0"));
        fs::create_directories(root / "kst/STYLE");
        std::ofstream(root / "kst/STYLE/BANK01.STY") << "x";
        rc.runKst(ui::ResourceBrowserModel::KstAction::Inventory, juce::String(root.string() + "/kst"));
        for (int i = 0; i < 50 && rc.kstText().startsWith("Working"); ++i) pump(50);
        TEST("KST inventory runs in the background and says semantic merge is unavailable",
             rc.kstText().contains("UNAVAILABLE") && rc.kstText().contains("STY"));
        rc.memoryBefore().setText("aiarranger-memory-info\t1\nsamples\t3\n");
        rc.memoryAfter().setText("aiarranger-memory-info\t1\nsamples\t50\n");
        rc.compareMemory();
        TEST("Memory Check: compares with the open packages (47 samples)", rc.memoryText().contains("predicted +47  MATCH") &&
             rc.memoryText().contains("does not validate"));

        // ── Convert Sound: WAV folder -> preview -> package -> opens in Resource Graph; UI stays responsive ──
        {
            // Message-loop watchdog: the longest gap between 20 ms timer ticks while work runs.
            struct Watch : juce::Timer {
                std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
                double worstMs = 0;
                void timerCallback() override {
                    const auto now = std::chrono::steady_clock::now();
                    worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(now - last).count());
                    last = now;
                }
            } watch;
            // 2 layers x 32 keys, stereo, 2.5 s each: ~30 MB of PCM (original synthetic sines).
            fs::create_directories(root / "big");
            for (const char* layer : {"Soft", "Hard"})
                for (int k = 48; k < 80; ++k) {
                    media::sound::Sample sm;
                    sm.root = uint8_t(k); sm.rate = 48000; sm.channels = 2;
                    const double f = 440 * std::pow(2.0, (k - 69) / 12.0);
                    for (int i = 0; i < 120000; ++i) { const auto v = int16_t(8000 * std::sin(2 * M_PI * f * i / 48000)); sm.pcm.push_back(v); sm.pcm.push_back(v); }
                    sm.looped = true; sm.loopStart = 60000; sm.loopEnd = 120000;
                    const auto b = media::sound::writeWav(sm);
                    std::ofstream(root / "big" / (std::string(layer) + "_" + std::to_string(k) + "_" + std::to_string(k) + "_" + std::to_string(k) + ".wav"),
                                  std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
                }
            rc.model().clear();
            auto& cv = rc.converter();
            TEST("Convert Sound: a folder of wrongly named files gets a clear message", !cv.inspectFolder(juce::File(root.string() + "/kst")));
            TEST("Convert Sound: inspect lists layers and proposes order and splits",
                 cv.inspectFolder(juce::File(root.string() + "/big")) && cv.orderField().getText() == "Soft,Hard" && cv.splitsField().getText() == "64");
            cv.nameField().setText("Big Sine");
            cv.orderField().setText("Soft,Hard");
            pump(50);   // typed edits notify asynchronously
            watch.last = std::chrono::steady_clock::now(); watch.worstMs = 0; watch.startTimer(20);
            cv.buildPreview();
            TEST("preview decodes off the message thread", cv.busy());
            for (int i = 0; i < 1500 && cv.busy(); ++i) pump(20);
            const double previewGap = watch.worstMs;
            TEST("preview ready: map, checks, exact bytes, recipe", cv.model().built() && cv.previewText().contains("Layer 'Soft'  velocity 0-63") &&
                 cv.previewText().contains("30720000 bytes uncompressed PCM (exact") && cv.recipeText().contains("POST-IMPORT CONFIGURATION"));
            {   // key map: a one-key zone shows the zone colour, the root is only a tick (snapshot defect 2026-11-09)
                app::KeyMapView v;
                v.setBounds(0, 0, 128 * 8 + 128, 100);
                v.setInstrument(&cv.model().instrument());
                const auto img = v.createComponentSnapshot(v.getLocalBounds(), true, 1.0f);
                const auto px = img.getPixelAt(4 + 120 + 60 * 8 + 4, 4 + 10);   // key C4, upper part of the top row
                TEST("key map: one-key zone keeps its zone colour (root drawn as a tick)", px.getBlue() > px.getRed());
            }
            TEST("checks name the limited key range", cv.previewText().contains("plays only C3-G5"));
            {   // Codex review of bfe83b5: replaced texts are not kept as undo history
                app::TextView tv;
                tv.setText("first"); tv.setText("second");
                TEST("text view keeps no undo history of replaced texts", !tv.canUndo() && tv.shownText() == "second");
            }
            {   // recipe prose wraps to the pane instead of scrolling sideways (GUI validation 2026-11-09)
                cv.setSize(1000, 700);
                const auto shown = cv.recipeShownText();
                int longest = 0;
                for (const auto& l : juce::StringArray::fromLines(shown)) longest = std::max(longest, l.length());
                TEST("recipe wraps to the pane width, mapping table does not", shown.contains("POST-IMPORT") && longest < 60 &&
                     cv.recipeText().removeCharacters(" \n") == shown.removeCharacters(" \n"));
            }
            watch.last = std::chrono::steady_clock::now(); watch.worstMs = 0;
            cv.generate(juce::File(root.string() + "/out/Big Sine"));
            for (int i = 0; i < 3000 && cv.busy(); ++i) pump(20);
            const double generateGap = watch.worstMs;
            TEST("generate: package written, summary with budget", cv.statusText().startsWith("Package written") && cv.previewText().contains("verdict: fits") &&
                 fs::exists(root / "out/Big Sine/RESOURCES.tsv"));
            watch.last = std::chrono::steady_clock::now(); watch.worstMs = 0;
            for (int i = 0; i < 1500 && rc.loading(); ++i) pump(20);
            const double loadGap = watch.worstMs;
            watch.stopTimer();
            TEST("the new package opens in Resource Graph", rc.tabs().getCurrentTabIndex() == 0 && rc.model().packages().size() == 1 &&
                 rc.summaryText().contains("unique sample resources  64 (0 mono, 64 stereo)"));
            std::printf("  info: longest UI stall: preview %.0f ms, generate %.0f ms, load %.0f ms\n", previewGap, generateGap, loadGap);
            TEST("UI stays responsive (no stall over 500 ms) while a 30 MB Sound is decoded, written and loaded",
                 previewGap < 500 && generateGap < 500 && loadGap < 500);
            {   // Codex round 1 (conversion): edits invalidate the preview; a stale Generate result does not take over
                cv.splitsField().setText("100");
                pump(50);
                TEST("editing a setting after Preview disables writing the old mapping", !cv.model().built());
                cv.generate(juce::File(root.string() + "/out/Stale"));
                for (int i = 0; i < 50 && cv.busy(); ++i) pump(20);
                TEST("Generate with a stale preview writes nothing", !fs::exists(root / "out/Stale"));
                cv.splitsField().setText("64");
                pump(50);
                cv.buildPreview();
                for (int i = 0; i < 1500 && cv.busy(); ++i) pump(20);
                rc.tabs().setCurrentTabIndex(3);
                cv.generate(juce::File(root.string() + "/out/Older"));
                cv.inspectFolder(juce::File(root.string() + "/big"));   // the user moves on while it writes
                for (int i = 0; i < 3000 && cv.busy(); ++i) pump(20);
                TEST("an older Generate finishing does not replace newer state or switch tabs",
                     fs::exists(root / "out/Older/RESOURCES.tsv") && !cv.statusText().startsWith("Package written") && rc.tabs().getCurrentTabIndex() == 3);
                cv.buildPreview();
                for (int i = 0; i < 1500 && cv.busy(); ++i) pump(20);
            }
            TEST("generate refuses an existing folder", [&] { cv.generate(juce::File(root.string() + "/out/Big Sine"));
                 for (int i = 0; i < 500 && cv.busy(); ++i) pump(20); return cv.statusText().contains("already exists"); }());
        }

        // ── Sample Plugin: AU/VST -> offline render in a separate process -> preview -> package ──
        {
            auto& ps = rc.sampler();
            TEST("five tabs, the last is Sample Plugin", rc.tabs().getNumTabs() == 5 && rc.tabs().getTabNames()[4] == "Sample Plugin");
            TEST("not ready until an instrument is chosen", ps.estimateText().startsWith("Not ready: choose an instrument"));
            auto waitIdle = [&](int ms) { for (int i = 0; i < ms / 20 && ps.busy(); ++i) pump(20); };
            const std::string lister = "if [ \"$1\" = list ]; then printf 'AudioUnit\\tFake Synth\\tTest\\tfake-id\\n'; exit 0; fi\n"
                                       "if [ \"$1\" = programs ]; then printf 'program\\t0\\tSoft Pad\\nprogram\\t1\\tBright Lead\\n'; exit 0; fi\n"
                                       "while [ $# -gt 0 ]; do [ \"$1\" = --out ] && OUT=\"$2\"; shift; done\n";
            auto fake = [&](const char* name, const std::string& body) {
                const auto p = root / name;
                std::ofstream(p) << "#!/bin/sh\n" << lister << body;
                fs::permissions(p, fs::perms::owner_all);
                return juce::File(p.string());
            };
            auto form = [&](app::PluginSamplerComponent& c, const juce::File& exe, const juce::String& instrument, const juce::String& name) {
                c.setSamplerExecutable(exe);
                c.findInstruments();
                for (int i = 0; i < 1500 && c.instrumentNames().isEmpty(); ++i) pump(20);
                const bool found = c.selectInstrument(instrument);
                for (auto [k, v] : {std::pair<const char*, const char*>{"Lowest note", "48"}, {"Highest note", "72"}, {"Every (semitones)", "12"},
                                    {"Velocities, softest first", "60,110"}, {"Hold (s)", "1.0"}, {"Release (s)", "0.5"}})
                    c.field(k).setText(v);
                c.field("Sound name").setText(name);
                pump(50);
                return found;
            };
            auto run = [&](const juce::File& exe) {
                form(ps, exe, "Fake Synth", "Fake");
                ps.startRender();
                const auto staging = ps.stagingFolder();
                waitIdle(10000);
                return staging;
            };
            {   // program names from the plugin fill the Program field with the index the sampler renders
                form(ps, fake("progs.sh", "exit 1\n"), "Fake Synth", "Fake");
                auto& pc = ps.programChoice();
                for (int i = 0; i < 250 && pc.getNumItems() == 0; ++i) pump(20);
                const bool listed = pc.getNumItems() == 3 && pc.getItemText(2) == "1: Bright Lead" && pc.isEnabled() && ps.field("Program").getText().isEmpty();
                pc.setSelectedId(3, juce::sendNotificationSync);
                const bool picked = ps.field("Program").getText() == "1";
                pc.setSelectedId(1, juce::sendNotificationSync);
                TEST("program list: names shown, choosing one sets its index, 'current sound' clears it",
                     listed && picked && ps.field("Program").getText().isEmpty());
                pc.setSelectedId(3, juce::sendNotificationSync);
                ps.findInstruments();   // a new scan clears the instrument; the old program must not stay selected
                for (int i = 0; i < 250 && ps.instrumentNames().isEmpty(); ++i) pump(20);
                TEST("program choice is cleared when the instrument changes", ps.field("Program").getText().isEmpty());
            }
            juce::File st = run(fake("fail.sh", "echo 'JUCE v8.0.4'\necho 'cannot load Fake Synth: the plugin refused to open' >&2\nexit 1\n"));
            TEST("plugin load failure: the tool's message is shown, nothing kept",
                 ps.statusText().contains("Render failed: cannot load Fake Synth: the plugin refused to open") && !st.exists() && !ps.rendered());
            st = run(fake("nopkg.sh", "printf 'progress\\t1\\t1\\n' >&2\nexit 0\n"));
            TEST("tool exits 0 without a package: reported as a failure", ps.statusText().contains("ended without a package") && !st.exists());
            st = run(fake("badwav.sh", "mkdir -p \"$OUT/wav\"\necho junk > \"$OUT/wav/Fake_060_000_127.wav\"\n"
                                       "printf 'layers\\tFake\\nsplits\\t\\npackage\\t%s\\n' \"$OUT\"\nexit 0\n"));
            TEST("invalid plugin output (unreadable WAV) rejected, nothing kept",
                 ps.statusText().contains("not a valid package") && !ps.rendered() && !st.exists());

            const auto pidFile = root / "hang.pid";
            const auto hang = fake("hang.sh", "echo $$ > '" + pidFile.string() + "'\nprintf 'progress\\t1\\t10\\n' >&2\nexec sleep 60\n");
            auto pidAlive = [&] { std::ifstream f(pidFile); int pid = 0; f >> pid; return pid > 0 && ::kill(pid, 0) == 0; };
            form(ps, hang, "Fake Synth", "Fake");
            ps.startRender();
            st = ps.stagingFolder();
            for (int i = 0; i < 250 && ps.progress() <= 0; ++i) pump(20);
            TEST("progress arrives while the tool runs", ps.progress() > 0.05 && ps.statusText().contains("note 1 of 10") && pidAlive());
            ps.cancelRender();
            waitIdle(5000);
            TEST("cancel: the process is killed, staging removed, UI idle", ps.statusText().startsWith("Cancelled") && !pidAlive() && !st.exists() && !ps.busy());
            fs::remove(pidFile);
            {   // app shutdown during sampling
                auto c = std::make_unique<app::PluginSamplerComponent>(nullptr);
                form(*c, hang, "Fake Synth", "Fake");
                c->startRender();
                const auto cst = c->stagingFolder();
                for (int i = 0; i < 250 && c->progress() <= 0; ++i) pump(20);
                const bool wasRunning = pidAlive();
                const auto t0 = std::chrono::steady_clock::now();
                c.reset();
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                pump(100);
                std::printf("  info: close mid-render took %.0f ms (component teardown included)\n", ms);
                TEST("closing the window mid-render kills the tool quickly and removes staging", wasRunning && !pidAlive() && ms < 2000 && !cst.exists());
            }

            {   // a helper started by the plugin keeps the output pipe open after the sampler is killed
                const auto orphanPid = root / "orphan.pid";
                const auto orphan = fake("orphan.sh", "sleep 60 &\necho $! > '" + orphanPid.string() + "'\necho $$ > '" + pidFile.string() +
                                                          "'\nprintf 'progress\\t1\\t10\\n' >&2\nexec sleep 60\n");
                auto killOrphan = [&] { std::ifstream f(orphanPid); int pid = 0; f >> pid; if (pid > 0) ::kill(pid, SIGKILL); fs::remove(orphanPid); };
                form(ps, orphan, "Fake Synth", "Fake");
                ps.startRender();
                st = ps.stagingFolder();
                for (int i = 0; i < 250 && ps.progress() <= 0; ++i) pump(20);
                ps.cancelRender();
                const auto t0 = std::chrono::steady_clock::now();
                waitIdle(6000);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                TEST("cancel finishes even when a helper holds the output pipe open", ps.statusText().startsWith("Cancelled") && !ps.busy() &&
                                                                                        !pidAlive() && !st.exists() && ms < 4000);
                killOrphan();
                fs::remove(pidFile);
                auto c = std::make_unique<app::PluginSamplerComponent>(nullptr);
                form(*c, orphan, "Fake Synth", "Fake");
                c->startRender();
                for (int i = 0; i < 250 && c->progress() <= 0; ++i) pump(20);
                const auto t1 = std::chrono::steady_clock::now();
                c.reset();
                const double closeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
                const bool alive = pidAlive();
                std::printf("  info: close with a held pipe took %.0f ms, sampler alive %d\n", closeMs, int(alive));
                TEST("closing the window is bounded even when a helper holds the output pipe open", closeMs < 1500 && !alive);
                killOrphan();
                fs::remove(pidFile);
            }
            {   // a finished process is never signalled again: its PID may already belong to another program
                app::SamplerProcess p;
                bool done = false;
                p.start(juce::File("/bin/echo"), {"hi"}, [](const std::string&) {}, [&](int, bool) { done = true; });
                for (int i = 0; i < 250 && !done; ++i) pump(20);
                TEST("cancel after the tool exited sends no signal", done && !p.running() && !p.cancel());
            }

            // Real offline render with an instrument that ships with macOS (Apple DLSMusicDevice).
            const auto real = app::PluginSamplerComponent::locateSampler();
            if (const char* req = std::getenv("AIARR_REQUIRE_REAL_PLUGIN"); req && *req == '1') {   // CI: the exact build-tree tool, not a search
                const char* given = std::getenv("AIARR_PLUGIN_SAMPLER");
                TEST("CTest passes the sampler's exact path (AIARR_PLUGIN_SAMPLER)", given && *given && real == juce::File(given) && real.existsAsFile());
            }
            if (!real.existsAsFile() || !form(ps, real, "DLSMusicDevice", "DLS Piano")) {
                std::printf("  SKIP: real plugin render (%s)\n", real.existsAsFile() ? "DLSMusicDevice not installed" : "sampler tool not built");
                // CI sets AIARR_REQUIRE_REAL_PLUGIN=1: there a skipped real render is a failure, not silent partial coverage.
                if (const char* req = std::getenv("AIARR_REQUIRE_REAL_PLUGIN"); req && *req == '1')
                    TEST("real plugin render ran (required by AIARR_REQUIRE_REAL_PLUGIN)", false);
            } else {
                struct Watch : juce::Timer {
                    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
                    double worstMs = 0;
                    void timerCallback() override {
                        const auto now = std::chrono::steady_clock::now();
                        worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(now - last).count());
                        last = now;
                    }
                } watch;
                ps.loopToggle().setToggleState(true, juce::sendNotificationSync);
                TEST("estimate before rendering", ps.estimateText().startsWith("6 samples (3 notes x 2 layers), 9 s of audio"));
                watch.startTimer(20);
                ps.startRender();
                waitIdle(120000);
                watch.stopTimer();
                const auto* in = ps.instrument();
                TEST("DLSMusicDevice rendered: 2 layers x 3 notes, previewed with the WAV engine",
                     ps.rendered() && in && in->layers.size() == 2 && in->samples.size() == 6 && ps.progress() == 1.0 &&
                     ps.previewText().contains("Layer 'DLS Piano Lo'") && ps.reportText().contains("Source: DLSMusicDevice"));
                std::printf("  info: longest UI stall while sampling: %.0f ms\n", watch.worstMs);
                TEST("UI stays responsive while the plugin renders (no stall over 500 ms)", watch.worstMs < 500);
                ps.generate(juce::File("/Volumes/AIARR-NO-SUCH/DLS Piano"));
                TEST("generate refuses a mounted volume", ps.statusText().contains("mounted volume"));
                fs::create_directories(root / "plug-out");
                ps.generate(juce::File((root / "plug-out").string()));
                TEST("generate refuses an existing folder", ps.statusText().contains("already exists"));
                ps.generate(juce::File((root / "plug-out" / "DLS Piano").string()));
                waitIdle(10000);
                for (int i = 0; i < 200 && rc.loading(); ++i) pump(20);
                TEST("generate copies the package and opens it in Resource Graph",
                     fs::exists(root / "plug-out/DLS Piano/RESOURCES.tsv") && fs::exists(root / "plug-out/DLS Piano/REPORT.txt") &&
                     rc.tabs().getCurrentTabIndex() == 0 && ps.statusText().startsWith("Package written"));
                {   // closing the window while Generate copies: the copy completes before staging is deleted
                    auto c = std::make_unique<app::PluginSamplerComponent>(nullptr);
                    form(*c, real, "DLSMusicDevice", "DLS Piano");
                    c->startRender();
                    for (int i = 0; i < 6000 && c->busy(); ++i) pump(20);
                    c->generate(juce::File((root / "plug-out" / "DLS Piano 2").string()));
                    c.reset();
                    auto count = [](const fs::path& d) { size_t n = 0; for (auto& e : fs::recursive_directory_iterator(d)) n += e.is_regular_file(); return n; };
                    TEST("closing the window during Generate leaves a complete package",
                         fs::exists(root / "plug-out/DLS Piano 2/RESOURCES.tsv") && count(root / "plug-out/DLS Piano 2") == count(root / "plug-out/DLS Piano"));
                }
                {   // a program the plugin does not have is refused instead of being passed to it unchecked
                    juce::ChildProcess cp;
                    const auto out = root / "noprog";
                    cp.start(juce::StringArray{real.getFullPathName(), "render", "--plugin", "DLSMusicDevice", "--name", "X", "--out",
                                               juce::String(out.string()), "--low", "60", "--high", "60", "--program", "9999"});
                    const auto text = cp.readAllProcessOutput();
                    cp.waitForProcessToFinish(10000);   // JUCE reports exit code 0 for a child it has not reaped yet
                    TEST("sampler refuses a program number the plugin does not have",
                         cp.getExitCode() == 1 && text.contains("program 9999 does not exist") && !fs::exists(out));
                }
                ps.field("Hold (s)").setText("1.5");
                pump(50);
                TEST("editing the form after a render asks for a new render", ps.statusText().contains("Settings changed since the render"));
                // Memory soak (opt-in, not in CI): AIARR_SMOKE_SOAK=N repeats the real render N times.
                if (const char* n = std::getenv("AIARR_SMOKE_SOAK")) {
                    auto rss = [] { mach_task_basic_info i{}; mach_msg_type_number_t c = MACH_TASK_BASIC_INFO_COUNT;
                                    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&i, &c); return double(i.resident_size) / 1e6; };
                    std::vector<double> m;
                    for (int k = 0; k < std::atoi(n); ++k) {
                        ps.startRender();
                        waitIdle(120000);
                        pump(200);
                        m.push_back(rss());
                        std::printf("  info: soak render %d: rendered %d, app RSS %.1f MB\n", k + 1, int(ps.rendered()), m.back());
                    }
                    TEST("soak: app memory does not grow across renders (last vs second, < 20 MB)",
                         m.size() < 3 || m.back() - m[1] < 20);
                }
            }
        }
        fs::remove_all(root);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
