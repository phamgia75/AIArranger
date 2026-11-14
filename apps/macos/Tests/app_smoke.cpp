// P2.5 UI smoke test: the real LiveHostComponent (the app's window content),
// driven only through what a user can reach: buttons found by their visible
// text, clicked with triggerClick(), the JUCE message loop run, and the labels
// the user sees read back. No hardware: no output is selected and
// AIARR_MIDI_HW_TX=0 blocks any send at the CoreMIDI boundary.

#include <juce_gui_extra/juce_gui_extra.h>
#include "../Source/LiveHostComponent.h"
#include "../Source/ResourcesComponent.h"
#include "../Source/StudioComponent.h"
#include "engine/uasf/deserializer.h"
#include "engine/uasf/serializer.h"
#include "jobs/export_specs.h"
#include "jobs/import_kit.h"
#include "media/kst_diff.h"
#include "media/sound/additive_source.h"
#include "media/sound/drum_source.h"
#include "media/sound/package_writer.h"
#include "media/sound/wav_writer.h"
#include "../../../tests/support/korg_fixture.h"
#include <chrono>
#include <csignal>
#include <mach/mach.h>
#include <fstream>
#include <cmath>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <chrono>
#include <ctime>

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
// The leftover banner is listed on a worker thread (Codex r2 #2): wait until the newest listing has landed.
static void settleLeftovers(ai_arranger::app::StudioComponent& s) { for (int i = 0; i < 500 && s.leftoversChecking(); ++i) pump(10); }
static std::string slurp(const std::filesystem::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }

// Per-phase timing (instrumentation only, no check): wall vs process CPU (all threads; children separately) and the
// 1-minute load average. CPU tracking wall under load = slow; wall growing with flat CPU = blocked or starved.
// "phase-start" lines carry the epoch second so a harness can sample a stalled phase.
static void phase(const char* next) {
    static std::string name;
    static double wall0 = 0, cpu0 = 0, child0 = 0;
    const auto secs = [](const timeval& t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; };
    rusage self{}, kids{};
    getrusage(RUSAGE_SELF, &self);
    getrusage(RUSAGE_CHILDREN, &kids);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double cpu = secs(self.ru_utime) + secs(self.ru_stime), child = secs(kids.ru_utime) + secs(kids.ru_stime);
    double load[1] = {0};
    getloadavg(load, 1);
    if (!name.empty())
        std::printf("  phase-end: %-38s wall %7.2fs  cpu %7.2fs  cpu/wall %4.2f  child-cpu %6.2fs  load1 %5.1f\n", name.c_str(), wall - wall0,
                    cpu - cpu0, (cpu - cpu0) / std::max(wall - wall0, 1e-3), child - child0, load[0]);
    name = next ? next : "";
    wall0 = wall, cpu0 = cpu, child0 = child;
    if (next) std::printf("  phase-start: %s at %lld\n", next, (long long)std::time(nullptr));
}

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
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // under ctest stdout is a pipe: a killed run (timeout) still shows its last check
    setenv("AIARR_MIDI_HW_TX", "0", 1);
    const auto stateDir = std::filesystem::temp_directory_path() / ("aia-smoke-state-" + std::to_string(::getpid()));
    setenv("AIARR_STATE_DIR", stateDir.c_str(), 1);   // the output record: never the user's own
    ai_arranger::jobs::setOutputRecord(ai_arranger::app::appOutputRecordFile());   // as Main.cpp does at launch
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf("Test: macOS app UI smoke (LiveHostComponent)\n");

    phase("live host");
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
    phase("resources: open + KST");
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
        phase("resources: backup KST");
        {   // Backup KST: verified copy into a new folder; refusals, cancel and window close leave nothing presented as a backup
            const fs::path kst = root / "kst", bdir = root / "kst-backups";
            fs::create_directories(bdir);
            auto waitBackup = [&](app::ResourcesComponent& c) { for (int i = 0; i < 600 && c.backingUp(); ++i) pump(20); };
            const auto b1 = app::ResourcesComponent::backupFolderFor(juce::File(kst.string()), juce::File(bdir.string()));
            TEST("backup folder name: '<source> backup <date time>' inside the chosen folder",
                 b1.getParentDirectory() == juce::File(bdir.string()) && b1.getFileName().startsWith("kst backup 20"));
            rc.startBackup(juce::File(kst.string()), b1);
            TEST("backup runs in the background", rc.backingUp());
            waitBackup(rc);
            const fs::path b1p = b1.getFullPathName().toStdString();
            TEST("Backup KST: complete and verified, manifest beside it, progress full",
                 rc.kstText().startsWith("Backup complete and verified") && media::diff(kst, b1p).same() && media::verifyBackup(b1p).empty() &&
                 rc.backupProgress() == 1.0);
            TEST("Backup KST: the copy ran on a worker thread, not the message thread", rc.lastBackupJob().writerThread != 0 &&
                 rc.lastBackupJob().writerThread != jobs::thisThread());
            TEST("Backup KST runs as an export job: its report sits beside the backup", fs::is_regular_file(b1p.string() + ".export.md") &&
                 rc.kstText().contains("Export job #"));
            rc.startBackup(juce::File(kst.string()), juce::File((kst / "STYLE/inside").string()));
            waitBackup(rc);
            TEST("Backup KST into a folder inside its source is refused, nothing created there (D18)", rc.kstText().contains("No backup was made") &&
                 (rc.kstText().contains("overlaps") || rc.kstText().contains("inside the source")) && !fs::exists(kst / "STYLE/inside") &&
                 !fs::exists(kst / "STYLE/inside.INCOMPLETE"));
            rc.startVerify(b1);
            TEST("Verify Backup (the check) runs off the message thread", rc.backingUp());
            waitBackup(rc);
            TEST("Verify Backup: a good backup verifies", rc.kstText().startsWith("Backup verified: all 1 files"));
            std::ofstream(b1p / "STYLE/BANK01.STY") << "tampered";
            rc.startVerify(b1);
            waitBackup(rc);
            TEST("Verify Backup: a changed file is reported, nothing is changed", rc.kstText().startsWith("Backup NOT verified") &&
                                                                                 rc.kstText().contains("changed: STYLE/BANK01.STY"));
            std::ofstream(b1p / "STYLE/BANK01.STY") << "x";
            rc.startVerify(juce::File(kst.string()));
            waitBackup(rc);
            TEST("Verify Backup: a folder without a manifest is not a backup", rc.kstText().contains("no manifest"));
            rc.startBackup(juce::File(kst.string()), b1);
            waitBackup(rc);
            TEST("Backup KST never reuses an existing folder", rc.kstText().contains("already exists") && rc.kstText().contains("No backup was made") &&
                                                                media::verifyBackup(b1p).empty());
            rc.startBackup(juce::File(kst.string()), juce::File("/Volumes/AIARR-NO-SUCH/kst backup"));
            waitBackup(rc);
            TEST("Backup KST refuses a USB stick (mounted volume)", rc.kstText().contains("mounted volume"));
            fs::create_directories(root / "bigkst/PCM");   // a large source so that cancel and close land mid-copy
            { std::ofstream big(root / "bigkst/PCM/RAM01.PCM", std::ios::binary); std::string mb(1 << 20, 'p'); for (int i = 0; i < 256; ++i) big << mb; }
            const fs::path b2 = bdir / "cancelled";
            rc.startBackup(juce::File((root / "bigkst").string()), juce::File(b2.string()));
            rc.cancelBackup();
            waitBackup(rc);
            // Cancelled before the copy started (during the source scan): nothing exists. Later: the partial copy stays, marked INCOMPLETE.
            // Either way nothing is published and nothing carries a manifest.
            auto notPublished = [&](const fs::path& d) {
                return !fs::exists(d) && !fs::exists(media::backupManifestPath(d)) &&
                       std::all_of(fs::directory_iterator(d.parent_path()), fs::directory_iterator(), [&](const fs::directory_entry& e) {
                           const auto n = e.path().filename().string();
                           return n.rfind(d.filename().string(), 0) != 0 || n == d.filename().string() + ".INCOMPLETE";
                       });
            };
            TEST("Backup KST cancel: reported, not published, any partial copy marked INCOMPLETE, buttons back",
                 rc.kstText().contains("cancelled") && notPublished(b2) && !rc.backingUp());
            {
                auto c = std::make_unique<app::ResourcesComponent>();
                const fs::path b3 = bdir / "closed";
                c->startBackup(juce::File((root / "bigkst").string()), juce::File(b3.string()));
                pump(20);
                const auto t0 = std::chrono::steady_clock::now();
                c.reset();
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::printf("  info: closing during a backup took %.0f ms\n", ms);
                TEST("closing the window during a backup stops it promptly; nothing published, any partial copy marked INCOMPLETE", ms < 1000 && notPublished(b3));
            }
            {   // closing the window during Verify Backup: the check stops, its result is dropped, nothing is changed
                auto c = std::make_unique<app::ResourcesComponent>();
                c->startVerify(b1);
                c.reset();
                pump(100);
                TEST("closing the window during Verify Backup is safe; the backup is unchanged", media::verifyBackup(b1p).empty());
            }
            fs::remove_all(root / "bigkst");
        }
        rc.memoryBefore().setText("aiarranger-memory-info\t1\nsamples\t3\n");
        rc.memoryAfter().setText("aiarranger-memory-info\t1\nsamples\t50\n");
        rc.compareMemory();
        TEST("Memory Check: compares with the open packages (47 samples)", rc.memoryText().contains("predicted +47  MATCH") &&
             rc.memoryText().contains("does not validate"));

        phase("resources: convert sound");
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
            {   // KRONOS Save Sampling Data folder (Korg-format KMP/KSF from the documented layout, synthetic audio)
                using namespace korgfix;
                const auto k = root / "kronos";
                put(k / "EPIAN000.KMP", kmp("E.Piano -L", {{48, 59, 0, 0, "EPL00000.KSF"}, {72, 127, 5, 0, "EPL00001.KSF"}}));
                put(k / "EPIAN001.KMP", kmp("E.Piano -R", {{48, 59, 0, 0, "EPR00000.KSF"}, {72, 127, 5, 0, "EPR00001.KSF"}}));
                for (const char* f : {"EPL00000.KSF", "EPR00000.KSF", "EPL00001.KSF", "EPR00001.KSF"})
                    put(k / f, ksf({tone(48000, 262, 7000, f[2]), 48000, 0, 24000, 47999}));
                TEST("Convert Sound: a KRONOS folder is recognised; the -L/-R pair is one layer",
                     cv.inspectFolder(juce::File(k.string())) && cv.model().korg() && cv.orderField().getText() == "E.Piano");
                cv.nameField().setText("Kronos EP");
                pump(50);
                cv.buildPreview();
                for (int i = 0; i < 1500 && cv.busy(); ++i) pump(20);
                TEST("Convert Sound: KRONOS preview shows the stereo zones, root and loop",
                     cv.model().built() && cv.previewText().contains("C-1 -B3    C3") && cv.previewText().contains("C4  -G9    C5") && cv.previewText().contains("24000-48000") &&
                     cv.model().instrument().samples.size() == 2 && cv.model().instrument().samples[0].channels == 2);
            }
            {   // KSF samples without a KMP: roots measured, zones proposed into the editable mapping field
                using namespace korgfix;
                const auto k = root / "ksfonly";
                put(k / "MS000000.KSF", ksf({tone(48000, 130.81, 7000, 1), 48000, 0, 24000, 47999, 0x20}, nullptr, "VLN48"));
                put(k / "MS000001.KSF", ksf({tone(48000, 261.63, 7000, 2), 48000, 0, 24000, 47999, 0x20}, nullptr, "VLN60"));
                TEST("Convert Sound: a KSF-only folder is accepted, the mapping editor appears empty",
                     cv.inspectFolder(juce::File(k.string())) && cv.model().ksf() && cv.mappingField().isVisible() && cv.mappingField().isEmpty() &&
                     cv.orderField().getText() == "ksfonly" && cv.splitsField().isEmpty());
                cv.nameField().setText("KSF Strings");
                pump(50);
                cv.buildPreview();
                for (int i = 0; i < 1500 && cv.busy(); ++i) pump(20);
                TEST("Convert Sound: Preview fills the proposed mapping and builds it",
                     cv.model().built() && cv.mappingField().getText().contains("MS000001.KSF     C4    F#3   G9") &&
                     cv.previewText().contains("C-1 -F3    C3") && cv.model().instrument().samples.size() == 2);
                TEST("Convert Sound: the KSF explanation (exact / measured / estimated / missing, quality) shows before Generate",
                     cv.recipeText().contains("EXACT (copied from the KSF files") && cv.recipeText().contains("MISSING (needs the original KMP") &&
                     cv.recipeText().contains("QUALITY (measured") && cv.recipeText().contains("root as measured"));
                cv.mappingField().setText("MS000000.KSF C3 C-1 B3\nMS000001.KSF C4 C4 G9 +7\n");   // the musician edits
                pump(50);
                TEST("Convert Sound: editing the mapping invalidates the preview", !cv.model().built());
                cv.buildPreview();
                for (int i = 0; i < 1500 && cv.busy(); ++i) pump(20);
                TEST("Convert Sound: the edited mapping is what is built", cv.model().built() && cv.previewText().contains("C-1 -B3    C3") &&
                     cv.model().instrument().layers[0].zones[1].tuneCents == 7);
                TEST("Convert Sound: the mapping editor hides for other folders",
                     cv.inspectFolder(juce::File(root.string() + "/kronos")) && !cv.mappingField().isVisible());
            }
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
            TEST("Generate: the package was written on a worker thread, not the message thread", cv.lastJob().writerThread != 0 &&
                 cv.lastJob().writerThread != jobs::thisThread());
            TEST("Generate runs as an export job: report beside the package, no work folder left", fs::is_regular_file(root / "out/Big Sine.export.md") &&
                 !fs::exists(root / "out/Big Sine.INCOMPLETE") && !cv.previewText().contains(".INCOMPLETE"));
            watch.last = std::chrono::steady_clock::now(); watch.worstMs = 0;
            for (int i = 0; i < 1500 && rc.loading(); ++i) pump(20);
            const double loadGap = watch.worstMs;
            watch.stopTimer();
            TEST("the new package opens in Resource Graph", rc.tabs().getCurrentTabIndex() == 0 && rc.model().packages().size() == 1 &&
                 rc.summaryText().contains("unique sample resources  64 (0 mono, 64 stereo)"));
            {   // Verify Package... (read-only): the generated package, then a damaged copy as a stick copy might be
                rc.verifyPackage(juce::File(root.string() + "/out/Big Sine"));
                for (int i = 0; i < 1500 && rc.loading(); ++i) pump(20);
                TEST("Verify Package: the generated package verifies", rc.detailsText().startsWith("Package verified: all ") && rc.detailsText().contains("loads back"));
                fs::copy(root / "out/Big Sine", root / "stick-copy", fs::copy_options::recursive);
                fs::remove(root / "stick-copy/RESOURCES.tsv");
                rc.verifyPackage(juce::File(root.string() + "/stick-copy"));
                for (int i = 0; i < 1500 && rc.loading(); ++i) pump(20);
                TEST("Verify Package: a copy missing a file is NOT verified and names it", rc.detailsText().startsWith("NOT verified") &&
                     rc.detailsText().contains("missing: RESOURCES.tsv") && rc.detailsText().contains("Do not import this copy"));
            }
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
                cv.generate(juce::File(root.string() + "/big/Inside"));
                for (int i = 0; i < 1500 && cv.busy(); ++i) pump(20);
                TEST("Generate into a folder inside the source WAV folder is refused, nothing created there (D18)",
                     cv.statusText().contains("overlaps the job's source") && !fs::exists(root / "big/Inside") && !fs::exists(root / "big/Inside.INCOMPLETE"));
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
            {   // Codex L2 P1: two overlapping Generates (the button is enabled again by a preview) each count once and land once
                cv.generate(juce::File(root.string() + "/out/Twice A"));
                cv.generate(juce::File(root.string() + "/out/Twice B"));
                for (int i = 0; i < 3000 && cv.busy(); ++i) pump(20);
                TEST("two overlapping Generates both land and leave the page idle (busy() false)", !cv.busy() &&
                     fs::exists(root / "out/Twice A/RESOURCES.tsv") && fs::exists(root / "out/Twice B/RESOURCES.tsv"));
            }
            {   // closing Convert Sound while Generate writes: its job is cancelled, its result dropped, nothing published
                const int workers = jobs::JobQueue::workers();
                auto c = std::make_unique<app::SoundConversionComponent>();
                c->inspectFolder(juce::File(root.string() + "/big"));
                c->buildPreview();
                for (int i = 0; i < 1500 && c->busy(); ++i) pump(20);
                c->generate(juce::File(root.string() + "/out/Closed"));
                c.reset();
                for (int i = 0; i < 500 && jobs::JobQueue::workers() > workers; ++i) pump(20);
                TEST("closing Convert Sound during Generate: no crash, nothing published, its worker exits", !fs::exists(root / "out/Closed") &&
                     jobs::JobQueue::workers() == workers);
            }
            TEST("generate refuses an existing folder", [&] { cv.generate(juce::File(root.string() + "/out/Big Sine"));
                 for (int i = 0; i < 500 && cv.busy(); ++i) pump(20); return cv.statusText().contains("already exists"); }());
            {   // P1: one editable zone map for every source; edits apply at the next Preview
                TEST("Convert Sound: Preview proposes the zone map", cv.zoneMapField().getText().contains("layer 0 63 Soft") &&
                     cv.zoneMapField().getText().contains("layer 64 127 Hard") && cv.zoneMapField().getText().contains("zone "));
                const auto z = cv.zoneMapField().getText().replace("layer 0 63 Soft", "layer 0 79 Soft").replace("layer 64 127 Hard", "layer 80 127 Hard");
                cv.zoneMapField().setText(z);
                pump(50);
                TEST("Convert Sound: editing the zone map invalidates the preview", !cv.model().built());
                cv.buildPreview();
                for (int i = 0; i < 1500 && cv.busy(); ++i) pump(20);
                TEST("Convert Sound: Preview applies the edited zone map (velocity split moved), the text is kept",
                     cv.model().built() && cv.previewText().contains("Layer 'Soft'  velocity 0-79") && cv.zoneMapField().getText() == z);
                cv.splitsField().setText("70");
                pump(50);
                TEST("Convert Sound: changing the splits clears the zone map (proposed again at Preview)", cv.zoneMapField().isEmpty());
            }
        }

        phase("resources: sample plugin");
        // ── Sample Plugin: AU/VST -> offline render in a separate process -> preview -> package ──
        {
            auto& ps = rc.sampler();
            TEST("five tabs, the last is Sample Plugin", rc.tabs().getNumTabs() == 5 && rc.tabs().getTabNames()[4] == "Sample Plugin");
            TEST("not ready until an instrument is chosen", ps.estimateText().startsWith("Not ready: choose an instrument"));
            TEST("the empty key map tells this tab's next step, not the WAV tab's", ps.keyMap().emptyText().startsWith("Render Samples to see"));
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
                for (auto [k, v] : {std::pair<const char*, const char*>{"Lowest note", "C3"}, {"Highest note", "c5"}, {"Every (semitones)", "12"},
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
            {   // Codex r5 P1: a render inside the Studio counts for its quit guard, and Cancel Jobs and Quit stops it
                using W = app::StudioComponent::WorkChoice;
                app::StudioComponent s([] {});
                auto& sp = s.resources().sampler();
                form(sp, hang, "Fake Synth", "Fake");
                sp.startRender();
                const auto sst = sp.stagingFolder();
                for (int i = 0; i < 250 && sp.progress() <= 0; ++i) pump(20);
                int q = 0;
                const bool deferred = !s.requestQuit([&q] { ++q; });
                const auto* still = find<juce::Label>(s.page(app::StudioComponent::Domain::ImportExport), "Still running:");
                TEST("Codex r5 P1: a Sample Plugin render in the Studio makes busy() true; quit is deferred and the prompt names it",
                     pidAlive() && s.busy() && deferred && q == 0 && s.workPromptShown() && still && still->getText().contains("Sample Plugin"));
                s.resolveRunningWork(W::CancelJobsAndQuit);
                for (int i = 0; i < 250 && q == 0; ++i) pump(20);
                pump(200);
                TEST("Codex r5 P1: Cancel Jobs and Quit stops the render (tool killed, staging removed), then quits once",
                     q == 1 && !pidAlive() && !sst.exists() && !sp.busy() && !s.busy());
                fs::remove(pidFile);
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
                {   // the same close, process wrapper only: separates the 500 ms reader wait from the rest of the teardown
                    auto p = std::make_unique<app::SamplerProcess>();
                    bool progressed = false;
                    p->start(orphan, {"render", "--out", (root / "orphan-out").string()}, [&](const std::string& l) { progressed |= l.rfind("progress", 0) == 0; },
                             [](int, bool) {});
                    for (int i = 0; i < 250 && !progressed; ++i) pump(20);
                    const auto t2 = std::chrono::steady_clock::now();
                    p.reset();
                    const double waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
                    std::printf("  info: held pipe, two separate closes: SamplerProcess alone %.0f ms (deadline %d ms), whole component %.0f ms\n", waitMs,
                                app::SamplerProcess::kCloseGraceMs, closeMs);
                    TEST("held pipe: the process wrapper waits the 500 ms bound (under 600 ms, not the 2 s cancel grace)", progressed && waitMs >= 500 && waitMs < 600 && !pidAlive());
                    killOrphan();
                    fs::remove(pidFile);
                }
            }
            {   // a finished process is never signalled again: its PID may already belong to another program
                app::SamplerProcess p;
                bool done = false;
                p.start(juce::File("/bin/echo"), {"hi"}, [](const std::string&) {}, [&](int, bool) { done = true; });
                for (int i = 0; i < 250 && !done; ++i) pump(20);
                TEST("cancel after the tool exited sends no signal", done && !p.running() && !p.cancel());
            }

            phase("resources: sample plugin real render");
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
                TEST("the form takes note names and shows the key and velocity map before rendering",
                     ps.estimateText().contains("Samples at C3, C4, C5; every key C-1 to G9 plays") && ps.estimateText().contains("velocity 0-85, 86-127."));
                {   // layout: the estimate and the map fit their label at the window's default size (no squashing, no "...")
                    rc.setSize(980, 640);
                    rc.tabs().setCurrentTabIndex(4);
                    pump(50);
                    const auto& L = ps.estimateLabel();
                    const auto area = L.getBorderSize().subtractedFrom(L.getLocalBounds());
                    juce::AttributedString as(L.getText());
                    as.setFont(L.getFont());
                    juce::TextLayout tl;
                    tl.createLayout(as, float(area.getWidth()));
                    const int maxLines = std::max(1, int(float(area.getHeight()) / L.getFont().getHeight()));
                    std::printf("  info: estimate label %dx%d, text needs %d lines, label shows %d\n", area.getWidth(), area.getHeight(), tl.getNumLines(), maxLines);
                    TEST("estimate and key/velocity map fit their label at 980x640 (nothing squashed or cut)", tl.getNumLines() <= maxLines);
                    {   // the longest status line (a loop shortfall in a big render) fits the one-line status label
                        const auto& St = ps.statusLabel();
                        const auto sa = St.getBorderSize().subtractedFrom(St.getLocalBounds());
                        const juce::String longest = "Rendered 9999 samples; 9999 could not be looped. See the preview.";
                        const float w = juce::GlyphArrangement::getStringWidth(St.getFont(), longest);
                        std::printf("  info: status label %dx%d, loop-shortfall line needs %.0f px\n", sa.getWidth(), sa.getHeight(), w);
                        TEST("the loop-shortfall status line fits its label at 980x640", w <= float(sa.getWidth()) && sa.getHeight() >= int(St.getFont().getHeight()));
                    }
                    {   // the longest map: 24 layers and a highest note off the sampling step
                        ps.field("Velocities, softest first").setText("5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100,105,110,115,120");
                        ps.field("Highest note").setText("C#5");
                        pump(50);
                        juce::AttributedString worst(L.getText());
                        worst.setFont(L.getFont());
                        juce::TextLayout wl;
                        wl.createLayout(worst, float(area.getWidth()));
                        std::printf("  info: longest map (24 layers, off-step top): %d lines, label shows %d\n", wl.getNumLines(), maxLines);
                        TEST("the longest map (24 layers, off-step top note) still fits its label", L.getText().contains("(24 layers) 0-7") && wl.getNumLines() <= maxLines);
                        ps.field("Velocities, softest first").setText("60,110");
                        ps.field("Highest note").setText("c5");
                        pump(50);
                    }
                    if (const char* dir = std::getenv("AIARR_SNAPSHOT_DIR"); dir && *dir) {   // offscreen render for a later look; not a hands-on check
                        const auto img = ps.createComponentSnapshot(ps.getLocalBounds(), true, 2.0f);
                        juce::File out = juce::File(dir).getChildFile("sample-plugin-tab.png");
                        out.deleteFile();
                        juce::FileOutputStream os(out);
                        juce::PNGImageFormat().writeImageToStream(img, os);
                        std::printf("  info: offscreen snapshot written to %s\n", out.getFullPathName().toRawUTF8());
                    }
                }
                watch.last = std::chrono::steady_clock::now();   // measure the render only, not the checks above
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
                const fs::path stagedPkg = ps.stagingFolder().getChildFile("pkg").getFullPathName().toStdString();
                fs::copy(stagedPkg, root / "staged-before", fs::copy_options::recursive);
                ps.generate(juce::File((root / "plug-out" / "DLS Piano").string()));
                waitIdle(10000);
                TEST("Generate leaves the rendered package (its source) byte-identical, and the copy equals it",
                     ai_arranger::media::diff(root / "staged-before", stagedPkg).same() &&
                     ai_arranger::media::diff(stagedPkg, root / "plug-out/DLS Piano").same());
                for (int i = 0; i < 200 && rc.loading(); ++i) pump(20);
                TEST("generate copies the package and opens it in Resource Graph",
                     fs::exists(root / "plug-out/DLS Piano/RESOURCES.tsv") && fs::exists(root / "plug-out/DLS Piano/REPORT.txt") &&
                     rc.tabs().getCurrentTabIndex() == 0 && ps.statusText().startsWith("Package written"));
                TEST("Sample Plugin Generate: the copy ran on a worker thread, not the message thread", ps.lastJob().writerThread != 0 &&
                     ps.lastJob().writerThread != jobs::thisThread());
                TEST("Sample Plugin Generate runs as an export job: report beside the package", fs::is_regular_file(root / "plug-out/DLS Piano.export.md"));
                ps.generate(ps.stagingFolder().getChildFile("pkg/copy"));
                waitIdle(10000);
                TEST("Sample Plugin Generate into the render's own staging folder is refused (D18)", ps.statusText().contains("overlaps the job's source") &&
                     !ps.stagingFolder().getChildFile("pkg/copy").exists() && !ps.stagingFolder().getChildFile("pkg/copy.INCOMPLETE").exists());
                {   // a copy that fails part way is never left under the package's name (only NAME.INCOMPLETE)
                    fs::path wav;
                    for (const auto& e : fs::directory_iterator(ps.stagingFolder().getChildFile("pkg/wav").getFullPathName().toStdString())) { wav = e.path(); break; }
                    fs::permissions(wav, fs::perms::none);
                    ps.generate(juce::File((root / "plug-out" / "DLS Piano bad").string()));
                    waitIdle(10000);
                    fs::permissions(wav, fs::perms::owner_read | fs::perms::owner_write);
                    TEST("a failed Generate copy stays NAME.INCOMPLETE, never a package under its name",
                         !wav.empty() && ps.statusText().contains("copy failed") && ps.statusText().contains("INCOMPLETE") &&
                         !fs::exists(root / "plug-out/DLS Piano bad") && fs::is_directory(root / "plug-out/DLS Piano bad.INCOMPLETE"));
                }
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
                {   // a loop that cannot be found: refused before any render, as the form does
                    juce::ChildProcess cp;
                    const auto out = root / "noloop";
                    cp.start(juce::StringArray{real.getFullPathName(), "render", "--plugin", "DLSMusicDevice", "--name", "X", "--out",
                                               juce::String(out.string()), "--low", "60", "--high", "60", "--loop", "on", "--hold", "0.5"});
                    const auto text = cp.readAllProcessOutput();
                    cp.waitForProcessToFinish(10000);
                    TEST("sampler refuses --loop on with a hold too short to loop", cp.getExitCode() == 1 && text.contains("--loop on needs --hold 0.61") && !fs::exists(out));
                    ps.field("Hold (s)").setText("0.5");
                    pump(50);
                    TEST("the form refuses Loop with a hold too short to loop", ps.estimateText().startsWith("Not ready: a loop needs a hold of at least 0.61 s"));
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

    // ── AI Music Studio window (E2): sidebar | content | inspector over the same tools; nothing written to a source ──
    phase("studio: setup + library workspace");
    {
        namespace fs = std::filesystem;
        using namespace ai_arranger;
        using D = app::StudioComponent::Domain;
        auto host = std::make_unique<app::LiveHostComponent>();
        TEST("studio: button present: Studio...", find<juce::Button>(*host, "Studio...") != nullptr);
        TEST("studio: the existing Sound Tools... window stays reachable", find<juce::Button>(*host, "Sound Tools...") != nullptr);
        host.reset();
        const fs::path root = fs::temp_directory_path() / ("aia-smoke-studio-" + std::to_string(::getpid()));
        fs::remove_all(root);
        bool hostShown = false;
        app::StudioComponent st([&] { hostShown = true; });
        pump(100);
        TEST("studio: spec default size 1440x900", st.getWidth() == 1440 && st.getHeight() == 900);
        auto waitIdle = [&](int ms) { for (int i = 0; i < ms / 20 && (st.busy() || st.resources().loading()); ++i) pump(20); };
        auto visibleIn = [](juce::Component* c, juce::Component* top) {
            for (; c && c != top; c = c->getParentComponent()) if (!c->isVisible()) return false;
            return c == top;
        };
        // Spec rule: every unavailable control on screen shows its reason inline (a visible label with the tooltip text).
        auto reasonsShown = [&]() {
            std::vector<juce::Component*> all, labels;
            std::function<void(juce::Component*)> walk = [&](juce::Component* c) {
                if (c == &st.resources()) return;   // the existing tabs keep their own conventions
                all.push_back(c);
                for (auto* ch : c->getChildren()) walk(ch);
            };
            walk(&st);
            for (auto* c : all) if (auto* l = dynamic_cast<juce::Label*>(c); l && visibleIn(l, &st) && l->getText().isNotEmpty()) labels.push_back(l);
            bool ok = true;
            for (auto* c : all)
                if (auto* b = dynamic_cast<juce::Button*>(c); b && !b->isEnabled() && visibleIn(b, &st)) {
                    const bool shown = b->getTooltip().isNotEmpty() && std::any_of(labels.begin(), labels.end(), [&](juce::Component* l) {
                        return static_cast<juce::Label*>(l)->getText() == b->getTooltip(); });
                    if (!shown) { std::printf("  info: no inline reason for '%s'\n", b->getButtonText().toRawUTF8()); ok = false; }
                }
            return ok;
        };
        for (const char* name : {"Library Workspace", "Sample Lab", "Style Studio", "Import / Export Center", "Validation & Reports", "Live Arranger"})
            TEST((juce::String("studio sidebar entry: ") + name).toRawUTF8(), find<juce::Button>(st.sidebar(), name) != nullptr);
        bool domainsOk = true, reasonsOk = true;
        for (const char* name : {"Library Workspace", "Sample Lab", "Style Studio", "Import / Export Center", "Validation & Reports"}) {
            click(st.sidebar(), name);
            pump(20);
            domainsOk &= juce::String(app::StudioComponent::domainName(st.domain())) == name && st.segment() == 0;
            for (int i = 0; i < st.segmentNames(st.domain()).size(); ++i) { st.showDomain(st.domain(), i); reasonsOk &= reasonsShown(); }
        }
        TEST("studio: each sidebar entry opens its domain", domainsOk);
        TEST("studio: every unavailable control in every domain view shows its reason inline", reasonsOk);
        click(st.sidebar(), "Live Arranger");
        TEST("studio: Live Arranger entry brings the live host forward", hostShown);
        st.keyPressed(juce::KeyPress('2', juce::ModifierKeys::commandModifier, 0));
        TEST("studio: Cmd-2 jumps to the second sidebar domain", st.domain() == D::SampleLab);

        // Library Workspace: the existing pages are segments of the shared Sounds & Resources view
        TEST("studio: Library segments wrap Resource Graph, KST Library, Memory Check",
             st.segmentNames(D::Library).joinIntoString("|") == "Sources|Resource Graph|KST Library|Memory Check|Library Manager");
        st.showDomain(D::Library, 2);
        TEST("studio: KST Library segment shows the existing tab", st.resources().isVisible() && st.resources().tabs().getCurrentTabIndex() == 1);
        st.showDomain(D::SampleLab, 1);
        TEST("studio: Sample Lab > Sample Plugin shows the existing tab", st.resources().isVisible() && st.resources().tabs().getCurrentTabIndex() == 4);
        st.showDomain(D::SampleLab, 3);
        juce::Button* writerSeg = nullptr;
        std::function<void(juce::Component&)> deep = [&](juce::Component& c) {
            for (auto* ch : c.getChildren()) {
                if (auto* b = dynamic_cast<juce::Button*>(ch); b && b->getButtonText() == "KMP / KSF Writer") writerSeg = b;
                deep(*ch);
            }
        };
        deep(st);
        TEST("studio: Sample Lab segments add KMP / KSF Writer and Tools",
             st.segmentNames(D::SampleLab).joinIntoString("|") == "Convert Sound|Sample Plugin|Quality Report|KMP / KSF Writer|Tools");
        auto* kmpBtn = st.currentPage() ? find<juce::Button>(*st.currentPage(), "Write KMP/KSF folder (EXPERIMENTAL, L1)...") : nullptr;
        TEST("studio: the KMP/KSF writer segment is enabled (E1 wired in)", st.segment() == 3 && writerSeg && writerSeg->isEnabled());
        TEST("studio: Write KMP/KSF folder waits for a previewed Sound and says so", kmpBtn && !kmpBtn->isEnabled() &&
             kmpBtn->getTooltip().startsWith("Needs: a previewed Sound"));
        st.resources().tabs().setCurrentTabIndex(0);   // what Convert Sound does when a package is written
        pump(60);                                      // the tab bar announces the change asynchronously
        TEST("studio: a Resource Graph hand-off moves the studio to Library > Resource Graph", st.domain() == D::Library && st.segment() == 1);
        st.showDomain(D::Library, 2);                  // our own tab switch, announced later, must not move the studio
        st.showDomain(D::StyleStudio);
        pump(60);
        TEST("studio: a stale tab announcement of its own switch is ignored", st.domain() == D::StyleStudio);
        st.showDomain(D::SampleLab, 2);
        TEST("studio: Quality Report says what to do when nothing is built", st.qualityText().startsWith("Nothing built yet"));

        // Sources: recognition + read-only library view; nothing in the source changes
        auto put = [](const fs::path& p, const std::string& b) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << b; };
        put(root / "Mine.KST/STYLE/USER01/Ballad.STG", "s");
        put(root / "Mine.KST/PAD/USER01/Hit.PDC", "pp");
        const auto stamp = fs::last_write_time(root / "Mine.KST/STYLE/USER01/Ballad.STG");
        st.showDomain(D::Library, 0);
        st.openSource(juce::File((root / "Mine.KST").string()));
        TEST("studio: a source is read off the message thread", st.busy());
        waitIdle(5000);
        TEST("studio: KST folder recognised by layout, content not claimed, zero rows visible",
             st.libraryText().contains("Pa5X user data (folder layout)") && st.libraryText().contains(juce::String::fromUTF8("content \xE2\x97\x87")) &&
             st.libraryText().contains("Keyboard Sets     none in this source") && st.libraryText().contains("STYLE/USER01: 1"));
        TEST("studio: the inspector shows the open source as read-only", st.inspectorText().contains("Mine.KST") && st.inspectorText().contains("Read-only source"));
        TEST("studio: the source is unchanged", fs::last_write_time(root / "Mine.KST/STYLE/USER01/Ballad.STG") == stamp);
        auto* toLab = find<juce::Button>(st.page(D::Library), "Send to Sample Lab");
        TEST("studio: Send to Sample Lab unavailable for a KST, with its reason", toLab && !toLab->isEnabled() && toLab->getTooltip().startsWith("Needs:"));
        auto* paInspect = find<juce::Button>(st.page(D::Library), "Inspect Pa-native (read-only)");
        TEST("studio: Pa-native inspect unavailable for a Pa5X KST (container UNKNOWN), says why", paInspect && !paInspect->isEnabled() &&
             paInspect->getTooltip().contains("UNKNOWN"));
        {   // E5: a SET without a .SET suffix is recognised by its Pa700 markers; its Pa-native items are listed read-only
            put(root / "Pa700copy/GLOBAL/LIMITER.AUD", "x");
            put(root / "Pa700copy/SOUND/USERDK01.PCG", std::string("\x01\x00\x01\x14\x00\x00", 6));   // magic only: invalid
            const auto setStamp = fs::last_write_time(root / "Pa700copy/SOUND/USERDK01.PCG");
            st.openSource(juce::File((root / "Pa700copy").string()));
            waitIdle(5000);
            TEST("studio: SET recognised by its Pa700 layout", st.libraryText().contains("Pa-family SET") && st.libraryText().contains("Pa700"));
            TEST("studio: Inspect Pa-native available for a SET", paInspect && paInspect->isEnabled());
            st.inspectPaNative();
            TEST("studio: Pa-native inspect runs off the message thread", st.busy());
            waitIdle(5000);
            TEST("studio: Pa-native inspect: READ-ONLY, every file listed, invalid container flagged", st.libraryText().contains("READ-ONLY, EXPERIMENTAL") &&
                 st.libraryText().contains("Files: 2") && st.libraryText().contains("INVALID Pa-native container") &&
                 st.libraryText().contains("no Pa-native wrapper"));
            TEST("studio: the SET source is unchanged", fs::last_write_time(root / "Pa700copy/SOUND/USERDK01.PCG") == setStamp);
        }
        phase("studio: library manager");
        {   // E7 Library Manager: two read-only libraries, search, compare, duplicates, an EXPORT-ONLY plan
            using A = app::LibraryManagerComponent::Action;
            using V = app::LibraryManagerComponent::View;
            auto& lm = st.manager();
            auto waitLm = [&](int ms) { for (int i = 0; i < ms / 20 && (lm.busy() || lm.computing()); ++i) pump(20); };
            st.showDomain(D::Library, 4);
            TEST("library manager: a Library segment, shown", st.currentPage() == &lm && lm.isVisible());
            TEST("library manager: Compare and Copy say what they need", lm.unavailable(A::Compare) == "Needs: a library open on both sides" &&
                 lm.unavailable(A::Copy) == "Needs: an item selected in a list" && lm.unavailable(A::Export).startsWith("Needs: a plan step") &&
                 lm.unavailable(A::Compat) == "Needs: an item selected in a list");
            TEST("library history: Undo and Where Used say what they need", lm.unavailable(A::Undo) == "Needs: a plan edit to undo" &&
                 lm.unavailable(A::WhereUsed) == "Needs: an item selected in a list");
            TEST("library manager: unavailable actions show their reason inline", reasonsShown());
            put(root / "Other.KST/STYLE/USER01/Ballad.STG", "s");       // same bytes as Mine.KST's
            put(root / "Other.KST/STYLE/USER02/Ballad.STG", "other");   // same name, other bank
            put(root / "Other.KST/PAD/USER09/Kick.PDC", "k");
            lm.openLibrary('A', juce::File((root / "Mine.KST").string()));
            TEST("library manager: a library is read off the message thread", lm.busy());
            waitLm(5000);
            lm.openLibrary('B', juce::File((root / "Other.KST").string()));
            waitLm(5000);
            TEST("library manager: both headers read-only with format and item count", lm.headerText('A').contains("Pa5X KST") &&
                 lm.headerText('A').contains("2 items") && lm.headerText('B').contains("3 items") && lm.headerText('B').contains("Read-only source"));
            TEST("library manager: details show sizes, sample disk size (not RAM) and banks", lm.reportText().contains("not keyboard sample RAM") &&
                 lm.reportText().contains("STYLE/USER02  Style"));
            lm.search().setText("ball", true);
            pump(60);   // TextEditor announces a change asynchronously
            TEST("library manager: the search box filters both panes", lm.paneText('A') == "Style        STYLE/USER01  Ballad" &&
                 lm.paneText('B').contains("STYLE/USER02  Ballad") && !lm.paneText('B').contains("Kick"));
            lm.search().setText("", true);
            pump(60);
            lm.showView(V::Compare);
            waitLm(5000);
            TEST("library manager: compare view (file level)", lm.reportText().contains("same 1, different 0, only in A 1, only in B 2") &&
                 lm.reportText().contains("only in A   PAD/USER01/Hit.PDC"));
            lm.showView(V::Duplicates);
            waitLm(5000);
            const auto requests = lm.reportRequests();
            lm.value().setText("unrelated", true);
            pump(60);   // the edit is announced asynchronously and reaches refresh()
            TEST("library manager: a value-field edit does not restart the Duplicates report (Codex re-check)",
                 lm.reportRequests() == requests && !lm.computing());
            lm.value().setText("", true);
            pump(60);
            TEST("library manager: duplicates view: names flagged as name-only", lm.reportText().contains("NAME ONLY") &&
                 lm.reportText().contains("Style \"Ballad\": STYLE/USER01 STYLE/USER02"));
            lm.selectRow('A', 1);   // Ballad (rows sorted by bank: PAD/USER01 Hit, STYLE/USER01 Ballad)
            waitLm(5000);
            TEST("library manager: selecting an item shows its details and evidenced dependencies only", lm.reportText().contains("Library A: Ballad") &&
                 lm.reportText().contains("UNKNOWN") && lm.unavailable(A::Copy).isEmpty());
            TEST("library manager: Rename needs a name and says so", lm.unavailable(A::Rename) == "Needs: the new name in the value field");
            lm.addStep(A::Copy);
            TEST("library manager: the plan is evaluated off the message thread (Codex C5)", lm.computing() && !lm.reportText().contains("1. copy"));
            waitLm(5000);
            TEST("library manager: Copy is staged in the plan view, nothing written", lm.plan().size() == 1 && lm.reportText().contains("READY TO EXPORT") &&
                 lm.reportText().contains("same bytes: nothing to do") && lm.reportText().contains("EXPORT-ONLY"));
            lm.selectRow('A', 0);   // Hit.PDC into B's selected bank: none selected there, so the same bank id
            lm.value().setText("STYLE/USER01", true);
            lm.addStep(A::SwapBanks);
            waitLm(5000);
            TEST("library manager: a conflicting step is shown as a conflict", lm.plan().size() == 2 && lm.reportText().contains("CONFLICT") &&
                 lm.reportText().contains("BLOCKED"));
            for (const auto& lib : {"Mine.KST", "Other.KST"}) {   // D18 through the job: both libraries are the plan job's sources
                lm.exportPlan(juce::File((root / lib / "plans/p").string()));
                waitLm(5000);
                TEST((std::string("library manager: plan export into a folder inside library ") + (lib[0] == 'M' ? "A" : "B") + " is refused").c_str(),
                     lm.reportText().contains("Plan NOT written") && lm.reportText().contains("overlaps the job's source") && !fs::exists(root / lib / "plans"));
            }
            lm.exportPlan(juce::File("/Volumes/AIA-NO-SUCH-STICK/plan"));
            waitLm(5000);
            TEST("library manager: export onto a mounted volume refused", lm.reportText().contains("Plan NOT written") && lm.reportText().contains("mounted volume"));
            lm.exportPlan(juce::File((root / "My Plan").string()));
            const bool busyAfterExport = lm.busy();   // read before the message loop runs again
            TEST("library manager: export runs off the message thread", busyAfterExport);
            waitLm(5000);
            {
                const auto all = st.jobList();
                const auto pj = all.empty() ? jobs::JobState{} : all.back();
                std::printf("  info: plan export job #%llu: writer thread %zx, message thread %zx, busy right after exportPlan %d, landed after the loop ran\n",
                            (unsigned long long)pj.id, pj.writerThread, jobs::thisThread(), int(busyAfterExport));
                TEST("library manager: the plan was written on a worker thread (thread id), never the message thread",
                     pj.type == "Library plan (EXPORT-ONLY)" && pj.writerThread != 0 && pj.writerThread != jobs::thisThread());
            }
            TEST("library manager: plan folder written at L1 only, EXPORT-ONLY", fs::is_regular_file(root / "My Plan/plan.json") &&
                 fs::is_regular_file(root / "My Plan/PACKAGE.sha256") && lm.reportText().contains(juce::String::fromUTF8("\xE2\x97\x86 L1  \xE2\x97\x87 L2  \xE2\x97\x87 L3")) &&
                 lm.reportText().contains("EXPORT-ONLY"));
            {
                const auto all = st.jobList();
                TEST("library manager: the plan export ran as an export job (E6 queue, report beside it)",
                     std::any_of(all.begin(), all.end(), [&](const jobs::JobState& j) {
                         return j.type == "Library plan (EXPORT-ONLY)" && j.phase == jobs::Phase::Done && j.dest == (root / "My Plan").string(); }) &&
                     fs::is_regular_file(root / "My Plan.export.md") && !fs::exists(root / "My Plan.INCOMPLETE"));
            }
            lm.exportPlan(juce::File((root / "My Plan").string()));
            waitLm(5000);
            TEST("library manager: an existing plan folder is never overwritten", lm.reportText().contains("already exists"));
            TEST("library manager: the libraries are unchanged", fs::last_write_time(root / "Mine.KST/STYLE/USER01/Ballad.STG") == stamp);
            TEST("library manager: reasons still inline with libraries open", reasonsShown());
            // E8: the validator view, and a plan executed at file level into a NEW library (a copy of the target)
            lm.showView(V::Plan);
            waitLm(5000);
            TEST("library manager E8: a blocked plan says why Execute is refused", lm.reportText().contains("Execute Plan: refused: the plan has 1 conflict(s)"));
            TEST("library manager E8: Reveal needs an executed plan", lm.unavailable(A::Reveal) == "Needs: an executed plan");
            lm.clearPlan();
            TEST("library history: Clear Plan is undoable", lm.plan().empty() && lm.unavailable(A::Undo).isEmpty() &&
                 lm.unavailable(A::Redo) == "Needs: an undone plan edit");
            lm.undo();
            waitLm(5000);
            TEST("library history: Undo restores the cleared steps; history and per-step effects shown", lm.plan().size() == 2 &&
                 lm.reportText().contains("(next: clear 2 step(s))") && lm.reportText().contains("effect    no file change") &&
                 lm.reportText().contains("effect    none: blocked by its conflicts"));
            lm.undo();
            waitLm(5000);
            TEST("library history: multi-step undo", lm.plan().size() == 1 && lm.plan()[0].op == library::Op::Copy && lm.reportText().contains("2 to redo"));
            lm.redo();
            lm.redo();
            waitLm(5000);
            TEST("library history: redo re-applies both edits, sources untouched", lm.plan().empty() && !lm.history().canRedo() &&
                 fs::last_write_time(root / "Mine.KST/STYLE/USER01/Ballad.STG") == stamp);
            lm.selectRow('A', 1);   // Ballad
            lm.showView(V::WhereUsed);
            waitLm(5000);
            TEST("where used: a Style's referrers are not modelled: UNKNOWN, none inferred", lm.reportText().contains("Where used: Style \"Ballad\"") &&
                 lm.reportText().contains("references UNKNOWN") && lm.reportText().contains("Known references: none") &&
                 lm.reportText().contains("what refers to a Style is not modelled"));
            lm.selectRow('A', 0);   // Hit.PDC
            waitLm(5000);
            TEST("where used follows the selection", lm.reportText().contains("Where used: Pad \"Hit\""));
            lm.showView(V::Validation);
            waitLm(5000);
            TEST("library manager E8: Validation view runs the structural validator on both sides", lm.reportText().contains("Library A: Structural validation") &&
                 lm.reportText().contains("Rule table: Pa5X KST") && lm.reportText().contains("not verified on a keyboard"));
            // P0 Keyboard Compatibility card: the selected item, per target keyboard, sourced rule tables only (C15, C16)
            lm.selectRow('A', 1);   // Ballad
            lm.showView(V::Compat);
            waitLm(5000);
            TEST("library manager: Compatibility card per keyboard, Pa5X first, every line sourced, never verified",
                 lm.reportText().contains("Keyboard Compatibility card") && lm.reportText().contains("STYLE/USER01/Ballad.STG") &&
                 lm.reportText().indexOf("hardware: not verified on a Pa5X") < lm.reportText().indexOf("hardware: not verified on a Pa4X family keyboard") &&
                 lm.reportText().contains("DOCUMENTED = a documented rule, applied by software") &&
                 lm.reportText().contains("DOCUMENTED  R-04  STYLE/.STG") && lm.reportText().contains("[MAN5 p.721") &&
                 lm.reportText().contains("L3  measured on hardware: not measured"));
            lm.selectRow('A', 0);   // Hit: the card follows the selection
            waitLm(5000);
            TEST("library manager: the Compatibility card follows the selection", lm.reportText().contains("PAD/USER01/Hit.PDC") && !lm.reportText().contains("Ballad"));
            {   // integ/p1-studio-plus merge #55 + #57: both selection-following views and every new button run their own handler
                click(lm, "Where Used");
                waitLm(5000);
                const bool wu = lm.reportText().contains("Where used: Pad \"Hit\"") && !lm.reportText().contains("Keyboard Compatibility card");
                lm.selectRow('A', 1);   // Ballad
                waitLm(5000);
                TEST("merge #55+#57: the Where Used button shows Where Used; it stays and follows a new selection",
                     wu && lm.reportText().contains("Where used: Style \"Ballad\"") && !lm.reportText().contains("Keyboard Compatibility card"));
                click(lm, "Compatibility");
                waitLm(5000);
                const bool kc = lm.reportText().contains("Keyboard Compatibility card") && lm.reportText().contains("STYLE/USER01/Ballad.STG") &&
                                !lm.reportText().contains("Where used:");
                lm.selectRow('A', 0);   // Hit
                waitLm(5000);
                TEST("merge #55+#57: the Compatibility button shows the card; it stays and follows a new selection",
                     kc && lm.reportText().contains("PAD/USER01/Hit.PDC") && !lm.reportText().contains("Ballad") && !lm.reportText().contains("Where used:"));
                const auto before = lm.plan().size();   // empty: the cleared plan, redone above
                click(lm, "Undo Step");
                waitLm(5000);
                const bool undone = lm.plan().size() == 2 && lm.history().canRedo();
                click(lm, "Redo Step");
                waitLm(5000);
                TEST("merge #55+#57: the Undo Step / Redo Step buttons undo and redo the plan edit (no other action)",
                     before == 0 && undone && lm.plan().empty() && !lm.history().canRedo());
            }
            lm.showView(V::Details);
            const auto bytesOf = [](const fs::path& d) {
                std::map<std::string, std::string> m;
                for (const auto& e : fs::recursive_directory_iterator(d))
                    if (e.is_regular_file()) { std::ifstream f(e.path(), std::ios::binary); m[e.path().string()] = std::string(std::istreambuf_iterator<char>(f), {}); }
                return m;
            };
            const auto mineBefore = bytesOf(root / "Mine.KST"), otherBefore = bytesOf(root / "Other.KST");
            lm.selectRow('A', 0);   // Hit.PDC: B has no PAD/USER01, so the copy makes that bank in B's copy
            lm.addStep(A::Copy);
            lm.selectRow('B', 0);   // Kick.PDC: a rename staged, then undone; Execute must not run it (#55)
            lm.value().setText("Kick2", true);
            lm.addStep(A::Rename);
            lm.value().setText("", true);
            TEST("library history #55: the rename is staged, then undone before Execute", lm.plan().size() == 2 && (lm.undo(), lm.plan().size() == 1));
            waitLm(5000);
            TEST("library manager E8: a conflict-free plan is ready to execute", lm.plan().size() == 1 && lm.reportText().contains("Execute Plan: ready") &&
                 lm.unavailable(A::Execute).isEmpty());
            fs::create_directories(root / "exec");
            lm.executePlan(juce::File((root / "exec").string()));
            TEST("library manager E8: execution queued as an export job, progress shown", lm.busy() && lm.progressText().contains("export job #"));
            waitLm(10000);
            const fs::path outLib = lm.lastOutput().getFullPathName().toStdString();
            TEST("library manager E8: a NEW library named after the target, with the copied file",
                 outLib.parent_path() == root / "exec" && outLib.filename().string().rfind("Other edited ", 0) == 0 && outLib.extension() == ".KST" &&
                 fs::is_regular_file(outLib / "PAD/USER01/Hit.PDC") && fs::is_regular_file(outLib / "PAD/USER09/Kick.PDC") &&
                 fs::is_regular_file(outLib.string() + ".manifest.sha256") && fs::is_regular_file(outLib.string() + ".export.md"));
            TEST("library manager E8: result shown with its label and checks; Reveal available",
                 lm.reportText().contains("Plan executed into a NEW library") && lm.reportText().contains("file-level operation; not verified on a keyboard") &&
                 lm.reportText().contains("PASS  plan re-check") && lm.progressText().startsWith("Executed: ") && lm.unavailable(A::Reveal).isEmpty());
            TEST("library history #55: the executed library is the plan shown after the undo (the undone rename is not in it)",
                 !fs::exists(outLib / "PAD/USER09/Kick2.PDC") && lm.reportText().contains("1. copy A:PAD/USER01") && !lm.reportText().contains("2. rename"));
            TEST("library manager E8: both sources byte-identical after the execution", bytesOf(root / "Mine.KST") == mineBefore &&
                 bytesOf(root / "Other.KST") == otherBefore);
            lm.clearPlan();
        }
        phase("studio: reports viewer");
        {   // Validation & Reports > Reports (P1, C17/C18): read-only viewer, reads off the message thread, HTML + CSV as an export job
            namespace rp = jobs::report;
            st.showDomain(D::Validation, 1);
            TEST("reports: Validation & Reports has a Reports segment", st.segmentNames(D::Validation).joinIntoString("|") == "Checks|Reports" && st.segment() == 1);
            TEST("reports: export needs an open report and says so", st.reportUnavailable().startsWith("Needs: a report open") && reasonsShown());
            st.openReport(app::StudioComponent::ReportSource::Inventory, juce::File((root / "Mine.KST").string()));
            TEST("reports: the inventory is read off the message thread (busy, not shown yet)", st.busy() && !st.reportViewText().contains("Library inventory"));
            waitIdle(5000);
            TEST("reports: inventory shown from the Library struct", st.reportViewText().contains("Library inventory: Mine.KST") &&
                 st.reportViewText().contains("Ballad") && st.reportViewText().contains("Not verified on a Pa5X") && st.reportStatus().startsWith("Showing (read-only)"));
            const auto out = root / "Inventory Report";
            const auto id = st.exportReport(juce::File(out.string()));
            const bool busyAfter = st.busy();
            waitIdle(10000);
            const auto all = st.jobList();
            const auto job = std::find_if(all.begin(), all.end(), [&](const jobs::JobState& j) { return j.id == id; });
            TEST("reports: export runs as an export job on a worker thread", id != 0 && busyAfter && job != all.end() && job->type == "Report (HTML + CSV)" &&
                 job->phase == jobs::Phase::Done && job->writerThread != 0 && job->writerThread != jobs::thisThread());
            std::vector<std::vector<std::string>> rows;
            TEST("reports: report.html + CSVs published with PACKAGE.sha256 and the job report, nothing staged left",
                 fs::is_regular_file(out / "report.html") && fs::is_regular_file(out / "banks.csv") && fs::is_regular_file(out / "PACKAGE.sha256") &&
                 fs::is_regular_file(root / "Inventory Report.export.json") && !fs::exists(root / "Inventory Report.INCOMPLETE"));
            TEST("reports: the written HTML is self-contained and items.csv parses", rp::htmlRefusal(slurp(out / "report.html")).empty() &&
                 rp::parseCsv(slurp(out / "items.csv"), &rows).empty() && rows.size() == 3 && rows[0][3] == "name");
            TEST("reports: status shows the published folder at L1 only", st.reportStatus().startsWith("Exported: ") &&
                 st.reportStatus().contains(juce::String::fromUTF8("\xE2\x97\x86 L1  \xE2\x97\x87 L2  \xE2\x97\x87 L3")));
            st.exportReport(juce::File((root / "Mine.KST/reports").string()));
            waitIdle(5000);
            TEST("reports: an export inside the library it reports on is refused (D18)", st.reportStatus().startsWith("Report NOT written") &&
                 st.reportStatus().contains("overlaps") && !fs::exists(root / "Mine.KST/reports") && !fs::exists(root / "Mine.KST/reports.INCOMPLETE"));
            st.openReport(app::StudioComponent::ReportSource::Validation, juce::File((root / "Mine.KST").string()));
            waitIdle(5000);
            TEST("reports: validation findings shown with their sources", st.reportViewText().contains("Structural validation: Mine.KST") &&
                 st.reportViewText().contains("== Findings ("));
            st.openReport(app::StudioComponent::ReportSource::JsonFile, juce::File((root / "My Plan/plan.json").string()));
            waitIdle(5000);
            TEST("reports: plan.json shown as the plan's steps", st.reportViewText().contains("Library Manager plan (EXPORT-ONLY)") &&
                 st.reportViewText().contains("== Steps (2) ==") && st.reportViewText().contains("1 | copy | A |") &&
                 st.reportViewText().contains("2 | bank-swap |"));
            st.openReport(app::StudioComponent::ReportSource::JsonFile, juce::File((root / "Inventory Report.export.json").string()));
            waitIdle(5000);
            TEST("reports: NAME.export.json shown as an export report", st.reportViewText().contains("Export report: Report (HTML + CSV)") &&
                 st.reportViewText().contains("Verification L1: passed"));
            {   // Codex r1 #1: a hostile many-keys report, superseded at once by a plan: the plan is shown, nothing stays busy
                std::string many = "{";
                for (int i = 0; i < 500000; ++i) many += (i ? ",\"" : "\"") + std::to_string(i) + "\":0";
                put(root / "many.export.json", many + "}");
                st.openReport(app::StudioComponent::ReportSource::JsonFile, juce::File((root / "many.export.json").string()));
                st.openReport(app::StudioComponent::ReportSource::JsonFile, juce::File((root / "My Plan/plan.json").string()));
                waitIdle(10000);
                TEST("reports r1#1: a superseded hostile read does not hold the viewer; the newest report is shown", !st.busy() &&
                     st.reportViewText().contains("Library Manager plan (EXPORT-ONLY)") && st.reportUnavailable().isEmpty());
            }
            {   // Codex r2 #1: 4 MB of scalar plan steps is refused with the reason, nothing built, the viewer free again
                std::string j = "{\"format\":\"aiarranger-library-plan\",\"steps\":[0";
                for (int i = 1; i < 1999997; ++i) j += ",0";
                put(root / "scalar.plan.json", j + "]}");
                st.openReport(app::StudioComponent::ReportSource::JsonFile, juce::File((root / "scalar.plan.json").string()));
                waitIdle(10000);
                TEST("reports r2#1: a plan of scalar steps is refused with the reason", !st.busy() &&
                     st.reportStatus().startsWith("Not shown: plan refused") && st.reportStatus().contains("not an object"));
            }
            put(root / "other.json", "{\"a\": 1}");
            st.openReport(app::StudioComponent::ReportSource::JsonFile, juce::File((root / "other.json").string()));
            waitIdle(5000);
            TEST("reports: another JSON is refused with the reason; export unavailable again", st.reportStatus().startsWith("Not shown: not an AIArranger") &&
                 st.reportUnavailable().startsWith("Needs: a report open"));
            TEST("reports: the library is unchanged", fs::last_write_time(root / "Mine.KST/STYLE/USER01/Ballad.STG") == stamp);
        }
        phase("studio: sample lab");
        {
            media::sound::Sample sm;
            sm.root = 60; sm.rate = 44100; sm.channels = 1;
            for (int i = 0; i < 44100; ++i) sm.pcm.push_back(int16_t(6000 * std::sin(2 * M_PI * 261.63 * i / 44100)));
            const auto b = media::sound::writeWav(sm);
            fs::create_directories(root / "wavs");
            std::ofstream(root / "wavs/Soft_60_60_60.wav", std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
        }
        st.openSource(juce::File((root / "wavs").string()));
        waitIdle(5000);
        TEST("studio: a WAV folder is offered to Sample Lab", toLab && toLab->isEnabled());
        if (toLab) toLab->triggerClick();
        pump(60);
        TEST("studio: Send to Sample Lab opens Convert Sound on that folder", st.domain() == D::SampleLab && st.segment() == 0 &&
             st.resources().converter().orderField().getText() == "Soft");
        const auto l1only = juce::String::fromUTF8("\xE2\x97\x86 L1  \xE2\x97\x87 L2  \xE2\x97\x87 L3");
        {   // E1: the previewed Sound written as an EXPERIMENTAL KMP/KSF folder through writeKorgFolder, at its real level L1
            auto& cv = st.resources().converter();
            cv.buildPreview();
            for (int i = 0; i < 1500 && cv.busy(); ++i) pump(20);
            TEST("studio: Convert Sound preview built for the KMP writer", cv.model().built() && st.kmpUnavailable().isEmpty());
            st.showDomain(D::SampleLab, 3);
            const fs::path kmp = root / "Soft KMP";
            st.writeKmp(juce::File(kmp.string()));
            TEST("studio: KMP/KSF folder written off the message thread", st.busy());
            waitIdle(20000);
            TEST("studio: KMP/KSF folder written via writeKorgFolder, EXPERIMENTAL, level L1", fs::is_regular_file(kmp / "PACKAGE.sha256") &&
                 st.kmpText().contains("EXPERIMENTAL") && st.kmpText().contains("level L1"));
            TEST("studio: the new KMP folder is the verification target at L1 only (no L2 claimed, no L3)", st.badgeText() == l1only &&
                 st.verification().l2.detail.find("L1 only") != std::string::npos && st.jobsText().contains("KMP/KSF folder (EXPERIMENTAL)  done"));
            st.writeKmp(juce::File(kmp.string()));
            waitIdle(20000);
            TEST("studio: an existing KMP folder is never overwritten", st.kmpText().contains("already exists"));
            st.writeKmp(juce::File("/Volumes/AIA-SMOKE-" + juce::String(::getpid()) + "/x"));
            waitIdle(20000);
            TEST("studio: KMP onto a mounted volume refused", st.kmpText().contains("mounted volume"));
            st.setValidationTarget(juce::File(kmp.string()));
            st.runValidator(app::StudioComponent::Validator::VerifyPackage);
            waitIdle(20000);
            TEST("studio: Verify Package on a KMP folder -> L1 only (not a package L2 failure)", st.badgeText() == l1only);
        }
        {   // E4: Sample Lab tools; previews write nothing, every write is a new folder at L1
            media::sound::Sample lp;
            lp.rate = 44100;
            lp.pcm.assign(11025 + 4 * 22050 + 11025, 0);
            for (size_t b = 0; b < 4; ++b)
                for (size_t i = 0; i < 8000; ++i)
                    lp.pcm[11025 + b * 22050 + i] = int16_t(12000 * std::exp(-double(i) / 2000) * std::sin(2 * M_PI * 440 * double(i) / 44100));
            const auto wb = media::sound::writeWav(lp);
            for (const char* n : {"labin/Loop.wav", "labin/Copy.wav"}) {
                fs::create_directories(root / "labin");
                std::ofstream(root / n, std::ios::binary).write(reinterpret_cast<const char*>(wb.data()), std::streamsize(wb.size()));
            }
            using T = app::StudioComponent::LabTool;
            st.showDomain(D::SampleLab, 4);
            TEST("studio: Sample Lab tools need an input and say so", st.labUnavailable(T::Analyze).startsWith("Needs: a WAV file") &&
                 st.labUnavailable(T::Batch).startsWith("Needs: a folder"));
            st.setLabInput(juce::File((root / "labin/Loop.wav").string()));
            st.runLabTool(T::Analyze);
            waitIdle(10000);
            TEST("studio: Analyze previews levels, onsets, normalize and trim; writes nothing", st.labText().contains("Onsets (4)") &&
                 st.labText().contains("Normalize preview") && st.labText().contains("Trim preview") && !fs::exists(root / "labin/Loop slices"));
            TEST("studio: Analyze draws the waveform (read on a worker, no playback)", st.labWave().data() && st.labWave().data()->frames == lp.frames() &&
                 st.labWave().data()->all.size() == 600 && !st.labWave().data()->looped);
            st.runLabTool(T::Slice, juce::File((root / "Loop slices").string()));
            waitIdle(10000);
            TEST("studio: Time Slice writes a new folder at L1", fs::is_regular_file(root / "Loop slices/SLICES.tsv") && st.badgeText() == l1only &&
                 st.labText().contains("Slices: 5"));
            st.labStart().setText("11025");
            st.labEnd().setText("99225");
            st.runLabTool(T::Loop, juce::File((root / "Loop loop").string()));
            waitIdle(10000);
            TEST("studio: Loop helper writes a new folder at L1", fs::exists(root / "Loop loop/wav/Loop.wav") && st.badgeText() == l1only);
            st.setLabInput(juce::File((root / "labin").string()));
            st.labTrim().setToggleState(true, juce::dontSendNotification);
            st.labNormalize().setToggleState(true, juce::dontSendNotification);
            st.runLabTool(T::BatchPreview);
            waitIdle(10000);
            TEST("studio: Batch Preview reports duplicates, writes nothing", st.labText().contains("Batch preview") && st.labText().contains("EXACT"));
            st.runLabTool(T::Batch, juce::File((root / "labin batch").string()));
            waitIdle(10000);
            TEST("studio: Batch writes a new folder at L1", fs::is_regular_file(root / "labin batch/PACKAGE.sha256") && st.badgeText() == l1only);
            st.runLabTool(T::Batch, juce::File((root / "labin/out").string()));
            waitIdle(10000);
            TEST("studio: Batch into its own input refused", st.labText().startsWith("Batch refused"));
            // P1: rate/bit-depth conversion and the Drum Kit builder
            st.labConvert().setToggleState(true, juce::dontSendNotification);
            st.runLabTool(T::BatchPreview);
            waitIdle(10000);
            TEST("studio: Batch Preview with Convert shows the before/after format", st.labText().contains("format: 44100 Hz 16-bit 1 ch"));
            st.labConvert().setToggleState(false, juce::dontSendNotification);
            st.labDrumMap().setText({});
            pump(50);
            TEST("studio: Drum Kit needs a drum map and says so", st.labUnavailable(T::DrumKitTx).startsWith("Needs: a drum map"));
            st.runLabTool(T::DrumMap);
            waitIdle(10000);
            TEST("studio: Propose Drum Map lists one key per WAV", st.labDrumMap().getText().contains("C2 Copy.wav group=Copy") &&
                 st.labDrumMap().getText().contains("C#2 Loop.wav group=Loop"));
            st.labDrumMap().setText("C2 Copy.wav group=Kick level=-3\nD2 Loop.wav group=Snare pan=20 family=S\n");
            pump(50);
            st.runLabTool(T::DrumKitTx, juce::File((root / "labin kit").string()));
            waitIdle(20000);
            TEST("studio: Drum Kit (TXPERF) job: new package, conversion report beside it", fs::is_regular_file(root / "labin kit/PACKAGE.sha256") &&
                 st.badgeText() == juce::String::fromUTF8("\xE2\x97\x86 L1  \xE2\x97\x86\xE2\x97\x86 L2  \xE2\x97\x87 L3") &&
                 slurp(root / "labin kit.export.md").find("structurally valid, not verified on a Pa5X") != std::string::npos);
            st.runLabTool(T::DrumKitKmp, juce::File((root / "labin kit KMP").string()));
            waitIdle(20000);
            TEST("studio: Drum Kit (KMP/KSF) job: L1 only, pan listed MISSING", fs::is_regular_file(root / "labin kit KMP/PACKAGE.sha256") &&
                 st.badgeText() == l1only && slurp(root / "labin kit KMP.export.md").find("**MISSING** pan (1 key(s) panned)") != std::string::npos);
        }
        std::string err;
        media::sound::writePackage(media::sound::buildDemoOrgan(), root / "organ", "", &err);
        st.openSource(juce::File((root / "organ").string()));
        waitIdle(10000);
        TEST("studio: a package source also opens in Resource Graph", st.resources().model().packages().size() == 1 &&
             st.libraryText().contains("AIArranger Sound / Drum Kit package"));

        phase("studio: import/export jobs");
        // Import / Export Center: routes into the existing flows; unbuilt exports say why
        st.showDomain(D::ImportExport);
        click(st.page(D::ImportExport), "AU / VST3 instrument");
        TEST("studio: import route AU / VST3 -> Sample Lab > Sample Plugin", st.domain() == D::SampleLab && st.segment() == 1);
        st.showDomain(D::ImportExport);
        click(st.page(D::ImportExport), "Verified KST backup");
        TEST("studio: export route KST backup -> Library > KST Library", st.domain() == D::Library && st.segment() == 2 &&
             st.resources().tabs().getCurrentTabIndex() == 1);
        auto* kstExport = find<juce::Button>(st.page(D::ImportExport), "Pa5X KST export");
        auto* kmpExport = find<juce::Button>(st.page(D::ImportExport), "KMP / KSF export");
        TEST("studio: Pa5X KST export unavailable: no writer, no L1 validator", kstExport && !kstExport->isEnabled() && kstExport->getTooltip().contains("KST writer"));
        TEST("studio: KMP/KSF export is a route to the EXPERIMENTAL writer", kmpExport == nullptr &&
             find<juce::Button>(st.page(D::ImportExport), "KMP / KSF folder (EXPERIMENTAL)") != nullptr);
        click(st.page(D::ImportExport), "KMP / KSF folder (EXPERIMENTAL)");
        TEST("studio: export route KMP/KSF -> Sample Lab > KMP / KSF Writer", st.domain() == D::SampleLab && st.segment() == 3);
        st.showDomain(D::ImportExport);
        click(st.page(D::ImportExport), "Style as Pa5X SMF (EXPERIMENTAL)");
        TEST("studio: export route Style SMF -> Style Studio", st.domain() == D::StyleStudio);
        st.showDomain(D::ImportExport);
        TEST("studio: staged change set and write rules stated", find<juce::Label>(st.page(D::ImportExport), "STAGED CHANGES: 0") != nullptr &&
             find<juce::Label>(st.page(D::ImportExport), "WRITE RULES: every write goes to NAME.INCOMPLETE") != nullptr);
        {   // E6: every export runs as a queued job (Import / Export Center job list); the page buttons enqueue jobs too
            using EJ = app::StudioComponent::ExportJob;
            using P = jobs::Phase;
            auto& io = st.page(D::ImportExport);
            auto job = [&](uint64_t id) { for (const auto& j : st.jobList()) if (j.id == id) return j; return jobs::JobState{}; };
            const auto all = st.jobList();
            TEST("jobs: the KMP writer and the Sample Lab writes ran as export jobs", std::any_of(all.begin(), all.end(), [](const jobs::JobState& j) {
                     return j.type == "KMP/KSF folder (EXPERIMENTAL)" && j.phase == P::Done; }) &&
                 std::any_of(all.begin(), all.end(), [](const jobs::JobState& j) { return j.type == "Sample Lab batch" && j.phase == P::Done; }) &&
                 std::any_of(all.begin(), all.end(), [](const jobs::JobState& j) {
                     return j.type == "Sample Lab batch" && j.phase == P::Failed && j.stoppedAt == P::Validate && j.reason.rfind("Batch refused", 0) == 0; }));
            auto* pkgJob = find<juce::Button>(io, "Package job...");
            auto* smfJob = find<juce::Button>(io, "SMF job...");
            TEST("jobs: export routes carry job buttons; unavailable ones say why", pkgJob && pkgJob->isEnabled() && smfJob && !smfJob->isEnabled() &&
                 smfJob->getTooltip().startsWith("Needs: a Style") && find<juce::Label>(io, "Needs: a Style opened for editing") != nullptr);
            const auto pkgDest = root / "pkg-job";
            const auto id = st.exportJob(EJ::SoundPackage, juce::File(pkgDest.string()));
            TEST("jobs: a Sound package job is queued and runs off the message thread", id > 0 && st.busy());
            waitIdle(30000);
            const auto pj = job(id);
            TEST("jobs: the studio's package job was written on a worker thread", pj.writerThread != 0 && pj.writerThread != jobs::thisThread());
            TEST("jobs: the package job is done: published, report beside, L1 + L2, never L3", pj.phase == P::Done &&
                 fs::is_regular_file(pkgDest / "PACKAGE.sha256") && fs::is_regular_file(root / "pkg-job.export.md") &&
                 fs::is_regular_file(root / "pkg-job.export.json") && !fs::exists(root / "pkg-job.INCOMPLETE") &&
                 pj.level.l1.state == ui::studio::Check::Passed && pj.level.l2.state == ui::studio::Check::Passed && pj.level.l3.state == ui::studio::Check::NotRun);
            {   // E9: the Pa5X Import Kit from a folder of outputs, as an export job
                fs::create_directories(root / "kit-src");
                fs::copy(pkgDest, root / "kit-src" / "pkg-job", fs::copy_options::recursive);
                TEST("kit: the Pa5X Import Kit route is offered", find<juce::Button>(io, "Pa5X Import Kit...") != nullptr &&
                     find<juce::Label>(io, "Sound/Drum Kit packages, KMP folders and Style SMFs in one USB folder") != nullptr);
                const auto kid = st.importKitJob(juce::File((root / "kit-src").string()), juce::File((root / "kit-job").string()));
                waitIdle(30000);
                const auto kj = job(kid);
                std::ifstream md(root / "kit-job/KIT.md");
                const std::string kitMd{std::istreambuf_iterator<char>(md), {}};
                TEST("kit: done off the message thread, member in SOUNDS, KIT.md labelled, L1 passed, never L3", kid > 0 && kj.phase == P::Done &&
                     kj.writerThread != jobs::thisThread() && fs::is_directory(root / "kit-job/SOUNDS/pkg-job") &&
                     jobs::kit::lastListingThread.load() == kj.writerThread &&   // the outputs folder was listed by the job's worker, not the UI
                     kitMd.find("STRUCTURALLY VALID — NOT VERIFIED ON A PA5X") != std::string::npos && kj.level.l1.state == ui::studio::Check::Passed &&
                     kj.level.l3.state == ui::studio::Check::NotRun && !fs::exists(root / "kit-job.INCOMPLETE"));
            }
            TEST("jobs: the job list shows the job, its level and its report path", st.jobListText().contains("#" + juce::String(int(id)) + "  Sound package  done") &&
                 st.jobListText().contains("pkg-job.export.md") && st.jobsText().contains("Sound package  done"));
            st.selectJob(id);
            TEST("jobs: Open Report is offered for a finished job", find<juce::Button>(io, "Open Report") && find<juce::Button>(io, "Open Report")->isVisible() &&
                 !find<juce::Button>(io, "Cancel Job")->isVisible());
            st.openJobReport();
            TEST("jobs: Open Report shows the report under the list", st.jobListText().contains("REPORT, job #" + juce::String(int(id))) &&
                 st.jobListText().contains("# Export report: Sound package") && st.jobListText().contains("PACKAGE.sha256"));

            // A Pa5X KST source: the backup job; a queued job cancelled before it starts writes nothing. The first job copies
            // and hashes 256 MB, so the second is still queued while this (message-thread) code runs.
            put(root / "Big.KST/STYLE/USER01/Big.STG", "s");
            put(root / "Big.KST/PCM/big.PCM", std::string(256u << 20, 'p'));
            st.openSource(juce::File((root / "Big.KST").string()));
            waitIdle(5000);
            const auto slow = st.exportJob(EJ::KstBackup, juce::File((root / "big-backup").string()));
            const auto queued = st.exportJob(EJ::KstBackup, juce::File((root / "kst-cancelled").string()));
            st.selectJob(queued);
            TEST("jobs: a queued job offers Cancel", job(queued).phase == P::Queued && find<juce::Button>(io, "Cancel Job")->isVisible());
            st.cancelJob();
            waitIdle(30000);
            TEST("jobs: cancelled while queued: cancelled, nothing written; the job before it finished", job(queued).phase == P::Cancelled &&
                 !fs::exists(root / "kst-cancelled") && !fs::exists(root / "kst-cancelled.INCOMPLETE") && job(slow).phase == P::Done &&
                 st.jobListText().contains("cancelled"));
            fs::remove_all(root / "Big.KST");
            fs::remove_all(root / "big-backup");
            st.openSource(juce::File((root / "Mine.KST").string()));
            waitIdle(5000);
            const auto bk = st.exportJob(EJ::KstBackup, juce::File((root / "kst-job").string()));
            waitIdle(30000);
            TEST("jobs: KST backup job: folder + manifest published, L1 from the manifest, no L2", job(bk).phase == P::Done &&
                 fs::is_regular_file(root / "kst-job/STYLE/USER01/Ballad.STG") && fs::is_regular_file(root / "kst-job.manifest.sha256") &&
                 st.badgeText() == l1only && !st.verification().l2.detail.empty());

            // Failure recovery: a leftover NAME.INCOMPLETE of an earlier job is refused; removed only with explicit consent
            put(root / "left.INCOMPLETE" / jobs::kJobMarker, "export job 7 Sound package -> " + (root / "left").string() + "\n");   // a crashed earlier run
            const auto lf = st.exportJob(EJ::SoundPackage, juce::File((root / "left").string()));
            waitIdle(30000);
            st.selectJob(lf);
            TEST("jobs: a leftover from an earlier job: failed with the reason, nothing published, re-run offered",
                 job(lf).phase == P::Failed && job(lf).reason.find("Re-run with consent") != std::string::npos && !fs::exists(root / "left") &&
                 find<juce::Button>(io, "Move Leftover Aside and Re-run")->isVisible() && st.jobListText().contains("FAILED at check destination"));
            const auto rr = st.rerunJobCleaning();
            waitIdle(30000);
            bool stale = false;
            for (const auto& e : fs::directory_iterator(root))
                stale |= e.path().filename().string().rfind("left.INCOMPLETE.stale-", 0) == 0 && fs::is_regular_file(e.path() / jobs::kJobMarker);
            TEST("jobs: re-run with consent moves the leftover aside (never deleted) and publishes", rr > lf && job(rr).phase == P::Done &&
                 fs::is_regular_file(root / "left/PACKAGE.sha256") && !fs::exists(root / "left.INCOMPLETE") && stale &&
                 job(rr).warnings.size() == 1 && job(rr).warnings[0].find("delete it by hand") != std::string::npos);
            TEST("jobs: nothing labelled compatible", !st.jobListText().containsIgnoreCase("compatible"));
        }

        phase("studio: style studio");
        // Style Studio: read-only report of an own synthetic style
        st.showDomain(D::StyleStudio);
        st.openStyle(juce::File(AIARR_REPO_TOOLS_DIR "/../fixtures/synthetic/SYNTH_WALTZ_68.sty"));
        TEST("studio: Style Studio reports the style's sections, read-only", st.styleText().contains("SYNTH_WALTZ_68.sty") &&
             st.styleText().contains("Parse: SUCCESS") && st.styleText().contains("Main A") && st.styleText().contains("nothing is written"));
        {   // E3: element/track browser, ops with undo/redo, SMF export (EXPERIMENTAL, L1) and import; the source is only read
            const fs::path sty = AIARR_REPO_TOOLS_DIR "/../fixtures/synthetic/SYNTH_WALTZ_68.sty";
            const auto styStamp = fs::last_write_time(sty);
            auto& pg = st.page(D::StyleStudio);
            TEST("studio: the Style opens for editing with an element/track browser", st.styleModel().loaded() &&
                 st.styleText().contains("    [0] ch ") && st.styleStatus().startsWith("Opened for editing"));
            TEST("studio: the Style inspector shows the open Style, not another page's output badges (Codex check)",
                 st.inspectorText().contains("STYLE\nSYNTH_WALTZ_68.sty") && st.inspectorText().contains("No output of this page checked yet") &&
                 !st.inspectorText().contains(l1only) && !st.inspectorText().contains("Level 1 structural"));
            const auto tempo = st.styleModel().style().tempo_bpm;
            st.styleOp().setSelectedId(6);   // Tempo
            st.styleValue().setText("97");
            click(pg, "Apply");
            TEST("studio: tempo edit applied and staged", st.styleModel().style().tempo_bpm == 97 && st.styleStatus().startsWith("Done") &&
                 st.inspectorText().contains("Style Studio edits not written yet") &&
                 find<juce::Label>(st.page(D::ImportExport), "STAGED CHANGES: Style Studio edits not written yet") != nullptr);
            TEST("studio: Undo names the op and restores", find<juce::Button>(pg, "Undo Tempo") != nullptr && st.styleUndo() &&
                 st.styleModel().style().tempo_bpm == tempo && find<juce::Button>(pg, "Redo Tempo") != nullptr);
            TEST("studio: Redo re-applies", st.styleRedo() && st.styleModel().style().tempo_bpm == 97);
            st.styleValue().setText("fast");
            click(pg, "Apply");
            TEST("studio: a bad value is refused, nothing changed", st.styleStatus().startsWith("Refused, nothing changed") && st.styleModel().style().tempo_bpm == 97);
            st.styleElement().setSelectedId(2, juce::sendNotificationSync);   // element [0]
            st.styleOp().setSelectedId(1);   // Quantize
            st.styleValue().setText("1/16 80");
            click(pg, "Apply");
            TEST("studio: quantize on one element", st.styleStatus().startsWith("Done: Quantize"));
            st.styleTrack().setSelectedId(2, juce::sendNotificationSync);     // track [0]
            st.styleOp().setSelectedId(2);   // Transpose
            st.styleValue().setText("2");
            click(pg, "Apply");
            TEST("studio: transpose on one track", st.styleStatus().startsWith("Done: Transpose"));
            st.styleOp().setSelectedId(5);   // Velocity scale
            st.styleValue().setText("90");
            click(pg, "Apply");
            TEST("studio: velocity scale", st.styleStatus().startsWith("Done: Velocity"));
            st.exportStyleSmf(juce::File((root / "waltz.mid").string()));
            waitIdle(10000);
            {
                const auto all = st.jobList();
                TEST("studio: the Style page's SMF export ran as an export job", !all.empty() && all.back().type == "Style as Pa5X SMF (EXPERIMENTAL)" &&
                     all.back().dest == (root / "waltz.mid").string());
                TEST("studio: the SMF was written on a worker thread, not the message thread", !all.empty() && all.back().writerThread != 0 &&
                     all.back().writerThread != jobs::thisThread());
            }
            TEST("studio: Pa5X SMF export writes a new file at L1 (validator), EXPERIMENTAL", fs::is_regular_file(root / "waltz.mid") &&
                 st.badgeText() == l1only && st.styleStatus().contains("EXPERIMENTAL") &&
                 st.inspectorText().contains("STAGED CHANGES\nStyle Studio edits not written yet. Save UASF... writes them all") &&   // r4 #1: SMF is lossy
                 st.inspectorText().contains("waltz.mid") && st.inspectorText().contains(l1only));
            st.exportStyleSmf(juce::File((root / "waltz.mid").string()));
            waitIdle(10000);
            TEST("studio: an existing SMF is never overwritten", st.styleStatus().contains("already exists"));
            st.exportStyleSmf(juce::File("/Volumes/AIA-SMOKE-" + juce::String(::getpid()) + "/w.mid"));
            waitIdle(10000);
            TEST("studio: SMF onto a mounted volume refused", st.styleStatus().contains("mounted volume"));
            st.importSmfTrack(juce::File((root / "waltz.mid").string()));
            TEST("studio: Import SMF Track replaces the chosen track (undoable)", st.styleStatus().startsWith("Imported") &&
                 find<juce::Button>(pg, "Undo Import SMF track") != nullptr);
            st.saveStyleUasf(juce::File((root / "waltz.uasf").string()));
            const bool dirtyWhileSaving = st.styleModel().dirty();   // D33 P2: only the job's result (message thread) clears it
            waitIdle(10000);
            pump(100);
            TEST("studio: Save UASF writes a new file", fs::is_regular_file(root / "waltz.uasf"));
            {
                const auto all = st.jobList();
                TEST("D33 P2 studio: Save UASF runs as an export job on a worker thread; dirty until the job is Done", dirtyWhileSaving &&
                     !st.styleModel().dirty() && !all.empty() && all.back().type == "Style as UASF" && all.back().phase == jobs::Phase::Done &&
                     all.back().dest == (root / "waltz.uasf").string() && all.back().writerThread != 0 && all.back().writerThread != jobs::thisThread());
                TEST("D33 P2 studio: the published UASF was read back, parsed and validated before it counted as saved", !all.empty() &&
                     all.back().verifyText.find("parsed by the UASF reader") != std::string::npos && !fs::exists(root / "waltz.uasf.INCOMPLETE"));
                st.styleOp().setSelectedId(6);   // Tempo
                st.styleValue().setText("101");
                click(pg, "Apply");
                st.saveStyleUasf(juce::File((root / "waltz-101.uasf").string()));
                st.styleValue().setText("102");
                click(pg, "Apply");   // an edit while the save job is pending
                waitIdle(10000);
                pump(100);
                uasf::UasfDeserializer des;
                const auto back = des.deserializeFromFile((root / "waltz-101.uasf").string());
                TEST("D33 P2 studio: an edit made during the save job keeps the Style dirty; the file holds the saved snapshot", back.success &&
                     back.style.tempo_bpm == 101 && st.styleModel().style().tempo_bpm == 102 && st.styleModel().dirty());
                st.styleUndo();
                TEST("D33 P2 studio: undo back to the saved snapshot: nothing unwritten", !st.styleModel().dirty());
            }
            {   // E6: Style exports as jobs from the Import / Export Center; once written the edits count as written
                using EJ = app::StudioComponent::ExportJob;
                st.styleOp().setSelectedId(6);   // Tempo
                st.styleValue().setText("93");
                click(pg, "Apply");
                const bool dirtyBefore = st.styleModel().dirty();
                const auto smf = st.exportJob(EJ::StyleSmf, juce::File((root / "waltz-job.mid").string()));
                const auto ua = st.exportJob(EJ::StyleUasf, juce::File((root / "waltz-job.uasf").string()));
                waitIdle(10000);
                jobs::JobState js, ju;
                for (const auto& j : st.jobList()) { if (j.id == smf) js = j; if (j.id == ua) ju = j; }
                TEST("jobs: Style SMF job: a new file at L1 with its report; UASF job: written, no Pa5X level", dirtyBefore &&
                     js.phase == jobs::Phase::Done && js.level.l1.state == ui::studio::Check::Passed && fs::is_regular_file(root / "waltz-job.mid") &&
                     fs::is_regular_file(root / "waltz-job.mid.export.md") && ju.phase == jobs::Phase::Done && fs::is_regular_file(root / "waltz-job.uasf") &&
                     ju.level.l1.state == ui::studio::Check::NotRun);
                TEST("jobs: after the Style jobs the edits count as written", !st.styleModel().dirty());
                TEST("P1 jobs: the Style SMF job report is the conversion report, not verified on a Pa5X", js.writerText.find("Section map") != std::string::npos &&
                     js.writerText.find("not verified on a Pa5X") != std::string::npos && js.verifyText.find("not verified on a Pa5X") != std::string::npos);
                st.styleValue().setText("94");
                click(pg, "Apply");
                const auto bad = st.exportJob(EJ::StyleUasf, juce::File((root / "waltz-job.uasf").string()));   // exists: the job fails
                waitIdle(10000);
                jobs::JobState jb;
                for (const auto& j : st.jobList()) if (j.id == bad) jb = j;
                TEST("D17 jobs: a failed Style job leaves the Style dirty (only Done marks edits written)", jb.phase == jobs::Phase::Failed &&
                     st.styleModel().dirty());
                const auto lossy = st.exportJob(EJ::StyleSmf, juce::File((root / "waltz-job2.mid").string()));
                waitIdle(10000);
                jobs::JobState jl;
                for (const auto& j : st.jobList()) if (j.id == lossy) jl = j;
                TEST("r4#1 jobs: a Done Style SMF job leaves the edits unwritten (SMF is lossy; only UASF clears them)",
                     jl.phase == jobs::Phase::Done && st.styleModel().dirty());
            }
            {   // P1: element op from the page, then the Korg Style SMF reopens for editing (no SFF1 report for a .mid)
                st.styleElement().setSelectedId(2, juce::sendNotificationSync);   // [0]
                st.styleOp().setSelectedId(8);   // Element: copy to
                st.styleValue().setText("Ending 3");
                click(pg, "Apply");
                TEST("P1 studio: Element copy applied from the page", st.styleStatus().startsWith("Done: Element: copy to") &&
                     find<juce::Button>(pg, "Undo Copy to Ending 3") != nullptr);
                const int n = int(st.styleModel().style().sections.size());
                TEST("r1#3 studio: every element, a new one included, is selectable with its current name",
                     st.styleElement().getNumItems() == n + 1 && st.styleElement().getItemText(n).contains(juce::String(st.styleModel().style().sections.back().name)) &&
                     st.styleElement().getSelectedId() == 2);
                st.styleOp().setSelectedId(10);   // Element: rename
                st.styleValue().setText("Groove X");
                click(pg, "Apply");
                TEST("r1#3 studio: a renamed element shows its new name", st.styleElement().getItemText(1).contains("Groove X"));
                st.styleUndo();
                st.styleUndo();
                TEST("r1#3 studio: undo removes the copied element from the choices", st.styleElement().getNumItems() == n &&
                     !st.styleElement().getItemText(1).contains("Groove X"));
                st.openStyle(juce::File((root / "waltz-job.mid").string()));
                if (st.unsavedPromptShown()) st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Discard);
                TEST("P1 studio: a Korg Style SMF opens for editing", st.styleStatus().startsWith("Opened for editing") &&
                     st.styleModel().loaded() && st.styleText().contains("Korg Style SMF") && !st.styleText().contains("Parse: SUCCESS"));
                st.openStyle(juce::File(sty.string()));
            }
            {   // Owner gate #53 (4-6): new Intro, Variation, Fill and Ending from the page, each selected and edited; save, reload, export
                const int base = int(st.styleModel().style().sections.size());   // SYNTH_WALTZ_68: 6 elements
                const char* kinds[] = {"Intro 2", "Variation 3", "Fill 3", "Ending 2"};
                for (int i = 0; i < 4; ++i) {
                    st.styleElement().setSelectedId(2, juce::sendNotificationSync);
                    st.styleOp().setSelectedId(8);   // Element: copy to
                    st.styleValue().setText(kinds[i]);
                    click(pg, "Apply");
                    const int id = base + i + 2;   // the new element's choice
                    const bool listed = st.styleElement().indexOfItemId(id) >= 0 && st.styleElement().getItemText(st.styleElement().indexOfItemId(id)).contains(kinds[i]);
                    st.styleElement().setSelectedId(id, juce::sendNotificationSync);
                    st.styleOp().setSelectedId(11);   // Element: length
                    st.styleValue().setText("6");
                    click(pg, "Apply");
                    const bool lengthened = st.styleStatus().startsWith("Done: Element: length") && st.styleModel().style().sections[size_t(base + i)].bars == 6;
                    st.styleTrack().setSelectedId(2, juce::sendNotificationSync);   // [0]
                    st.styleOp().setSelectedId(15);   // Element: Pa5X sound
                    st.styleValue().setText(juce::String(20 + i) + " 0 " + juce::String(i));
                    click(pg, "Apply");
                    TEST((juce::String("gate #53 studio: new ") + kinds[i] + " listed, selectable and editable (length, element sound)").toRawUTF8(),
                         listed && lengthened && st.styleStatus().startsWith("Done: Element: Pa5X sound") && st.styleElement().getSelectedId() == id);
                }
                TEST("gate #53 studio: Pa5X sound edits are unwritten edits with undo", st.styleModel().dirty() &&
                     find<juce::Button>(pg, "Undo Pa5X element sound") != nullptr);
                st.styleUndo();
                TEST("gate #53 studio: undo keeps a valid selection", st.styleElement().getSelectedId() == base + 5 && st.styleRedo());
                st.saveStyleUasf(juce::File((root / "gate53.uasf").string()));
                waitIdle(10000);
                pump(100);
                TEST("gate #53 studio: Save UASF writes every edit", fs::is_regular_file(root / "gate53.uasf") && !st.styleModel().dirty());
                st.openStyle(juce::File((root / "gate53.uasf").string()));
                TEST("gate #53 studio: reload lists the 4 new elements with their sounds", st.styleStatus().startsWith("Opened for editing") &&
                     st.styleElement().getNumItems() == base + 5 && st.styleElement().getItemText(base + 4).contains("Ending 2") &&
                     st.styleModel().style().korg_export.element_sounds.size() == 4 && st.styleText().contains("sound 23/0/3 (this element)"));
                using EJ = app::StudioComponent::ExportJob;
                const auto gid = st.exportJob(EJ::StyleSmf, juce::File((root / "gate53.mid").string()));
                waitIdle(10000);
                jobs::JobState gj;
                for (const auto& j : st.jobList()) if (j.id == gid) gj = j;
                TEST("gate #53 studio: export job after reload carries each element's sound", gj.phase == jobs::Phase::Done &&
                     gj.writerText.find("Element-specific Pa5X sounds") != std::string::npos && gj.writerText.find("e2cv1 ") != std::string::npos);
                // Gate 3: a Korg SMF whose bank/program cannot be kept exactly needs the explicit choice
                std::ifstream gin(root / "gate53.mid", std::ios::binary);
                auto k = exporters::korg::parse({std::istreambuf_iterator<char>(gin), std::istreambuf_iterator<char>()}).style;
                k.elements[0].events.insert(k.elements[0].events.begin(), {0, uint8_t(0xB0 | exporters::korg::kAcc1Ch), 0, 5});   // CC0 alone
                const auto w = exporters::korg::write(k);
                std::ofstream(root / "lossy.mid", std::ios::binary).write(reinterpret_cast<const char*>(w.bytes.data()), std::streamsize(w.bytes.size()));
                st.openStyle(juce::File((root / "lossy.mid").string()));
                TEST("gate #53 studio: lossy Korg SMF refused by default, naming the loss", w.ok && st.styleStatus().startsWith("Not editable") &&
                     st.styleStatus().contains("cannot be kept exactly") && fs::equivalent(st.styleModel().path(), root / "gate53.uasf"));
                st.styleKeepFirstSound().setToggleState(true, juce::dontSendNotification);
                st.openStyle(juce::File((root / "lossy.mid").string()));
                TEST("gate #53 studio: with keep-first chosen it opens and lists the loss", st.styleStatus().startsWith("Opened for editing") &&
                     st.styleText().contains("LOSS: "));
                st.styleKeepFirstSound().setToggleState(false, juce::dontSendNotification);
                st.openStyle(juce::File(sty.string()));
            }
            TEST("studio: the source Style is unchanged", fs::last_write_time(sty) == styStamp);
            st.setValidationTarget(juce::File((root / "waltz.mid").string()));
            st.runValidator(app::StudioComponent::Validator::StyleSmf);
            waitIdle(5000);
            TEST("studio: Style SMF Check -> L1 only", st.badgeText() == l1only && st.jobsText().contains("Style SMF Check"));
            st.showDomain(D::StyleStudio);
            TEST("studio: a Validation check is not shown as the Style page's badge", !st.inspectorText().contains(l1only));

            // Codex C2: opening another Style while edits are unwritten asks Save / Discard / Cancel; Cancel keeps it all
            st.styleOp().setSelectedId(6);   // Tempo
            st.styleValue().setText("99");
            click(pg, "Apply");
            st.openStyle(juce::File(sty.string()));
            TEST("C2 studio: open with unwritten edits -> asks first, nothing replaced", st.unsavedPromptShown() &&
                 st.styleModel().style().tempo_bpm == 99 && st.styleStatus().contains("not written yet") &&
                 find<juce::Button>(pg, "Discard Edits and Open") && find<juce::Button>(pg, "Discard Edits and Open")->isVisible());
            st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Cancel);
            TEST("C2 studio: Cancel keeps the Style, its edits and its undo", !st.unsavedPromptShown() && st.styleModel().style().tempo_bpm == 99 &&
                 st.styleModel().undoLabel() == "Tempo" && st.styleModel().dirty() && !find<juce::Button>(pg, "Discard Edits and Open")->isVisible());
            st.openStyle(juce::File(sty.string()));
            st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Save, juce::File((root / "kept.uasf").string()));
            const bool openWaits = st.styleModel().dirty() && st.styleModel().style().tempo_bpm == 99 && !st.jobList().empty() &&   // not reloaded yet
                                   st.jobList().back().type == "Style as UASF";   // the job may be finished, but not landed (message thread)
            waitIdle(10000);
            pump(100);
            TEST("D33 P2 studio: Save-before-open runs as an export job; the other Style opens only after it is Done", openWaits);
            TEST("C2 studio: Save writes the edits first, then opens the other Style", fs::is_regular_file(root / "kept.uasf") &&
                 !st.unsavedPromptShown() && fs::equivalent(st.styleModel().path(), sty) && !st.styleModel().dirty());
            st.styleValue().setText("98");
            click(pg, "Apply");
            st.openStyle(juce::File((root / "kept.uasf").string()));
            st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Discard);
            TEST("C2 studio: Discard opens the other Style", fs::equivalent(st.styleModel().path(), root / "kept.uasf") && st.styleModel().style().tempo_bpm == 99);
            st.styleValue().setText("97");
            click(pg, "Apply");
            st.openStyle(juce::File(sty.string()));
            st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Save, juce::File((root / "kept.uasf").string()));   // exists: refused
            waitIdle(10000);
            pump(100);
            TEST("D33 P2 studio: a failed Save-before-open opens nothing; the edits stay and the page asks again", st.unsavedPromptShown() &&
                 fs::equivalent(st.styleModel().path(), root / "kept.uasf") && st.styleModel().dirty() && st.styleModel().style().tempo_bpm == 97 &&
                 st.styleStatus().contains("not written yet"));
            st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Cancel);
            st.styleUndo();   // back to kept.uasf as opened

            // Codex C3: an invalid Style is refused for editing with the validator message; the open one stays
            {
                uasf::UasfDeserializer des;
                auto bad = des.deserializeFromFile((root / "kept.uasf").string());
                bad.style.sections[0].beats_per_bar = 0;
                uasf::UasfSerializer ser;
                const auto b = ser.serialize(bad.style);
                std::ofstream(root / "invalid.uasf", std::ios::binary).write(reinterpret_cast<const char*>(b.data.data()), std::streamsize(b.data.size()));
            }
            st.openStyle(juce::File((root / "invalid.uasf").string()));
            TEST("C3 studio: invalid Style not editable, validator named, the open Style unchanged",
                 st.styleStatus().startsWith("Not editable") && st.styleStatus().contains("UASF validator") && fs::equivalent(st.styleModel().path(), root / "kept.uasf"));

            // a job that runs until it is cancelled (its writer polls the job's cancel check)
            auto slowJob = [](fs::path dest) {
                jobs::ExportSpec slow;
                slow.type = "test: runs until cancelled";
                slow.dest = std::move(dest);
                slow.write = [](const fs::path& out, const std::function<bool(double)>& progress) {
                    fs::create_directories(out);
                    while (progress(0.5)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    return jobs::WriteResult{"stopped: cancelled", ""};
                };
                return slow;
            };
            phase("studio: quit while work runs");
            {   // Quit while work runs (export jobs, Library Manager scans/reports): Wait / Cancel Jobs and Quit / Keep Working
                using W = app::StudioComponent::WorkChoice;
                using EJ = app::StudioComponent::ExportJob;
                auto job = [&](uint64_t id) { for (const auto& j : st.jobList()) if (j.id == id) return j; return jobs::JobState{}; };
                int wq = 0;
                auto wfn = [&wq] { ++wq; };
                auto& io = st.page(D::ImportExport);
                st.manager().openLibrary('A', juce::File((root / "Mine.KST").string()));
                TEST("quit: a library scan running counts as busy", st.busy() && st.manager().busy());
                TEST("quit: work running -> deferred, the job page asks Wait / Cancel Jobs and Quit / Keep Working",
                     !st.requestQuit(wfn) && wq == 0 && st.workPromptShown() && st.domain() == D::ImportExport &&
                     find<juce::Button>(io, "Wait, then Quit")->isVisible() && find<juce::Button>(io, "Cancel Jobs and Quit")->isVisible() &&
                     find<juce::Button>(io, "Keep Working")->isVisible());
                st.resolveRunningWork(W::KeepWorking);
                waitIdle(5000);
                pump(200);
                TEST("quit: Keep Working aborts the quit; the work still finishes", wq == 0 && !st.workPromptShown() && !st.busy() &&
                     !find<juce::Button>(io, "Keep Working")->isVisible());
                st.manager().openLibrary('A', juce::File((root / "Mine.KST").string()));
                st.requestQuit(wfn);
                st.resolveRunningWork(W::Wait);
                TEST("quit: Wait does not quit while the work runs", wq == 0 && st.workPromptShown());
                waitIdle(5000);
                pump(300);
                TEST("quit: Wait quits once the work has finished", wq == 1 && !st.workPromptShown() && st.manager().headerText('A').contains("2 items"));
                // a job that runs until it is cancelled, and a job queued behind it
                const auto j1 = st.manager().enqueueJob(slowJob(root / "quit-slow"), {});
                const auto j2 = st.exportJob(EJ::SoundPackage, juce::File((root / "quit-pkg2").string()));
                pump(100);
                TEST("quit: export jobs running or queued -> deferred", j1 > 0 && j2 > 0 && job(j1).phase == jobs::Phase::Run &&
                     job(j2).phase == jobs::Phase::Queued && !st.requestQuit(wfn) && wq == 1);
                st.resolveRunningWork(W::CancelJobsAndQuit);
                TEST("quit: Cancel Jobs and Quit waits for the jobs to stop", wq == 1);
                waitIdle(30000);
                pump(300);
                TEST("quit: Cancel Jobs and Quit cancels every job, then quits", wq == 2 && !st.workPromptShown() &&
                     job(j1).phase == jobs::Phase::Cancelled && job(j2).phase == jobs::Phase::Cancelled);
                {   // leftover banner: read-only listing of the last output folder, on launch and on page open; nothing deleted
                    app::StudioComponent again([] {});
                    const bool idleAtOnce = !again.busy() && !again.manager().computing();   // before any pump: as the app's quit sees it
                    settleLeftovers(again);
                    TEST("quit guard: a new Studio with nothing started is idle at once (no Library Manager report counted, #43 gate D note 1)",
                         idleAtOnce);
                    TEST("leftovers: on launch a banner lists NAME.INCOMPLETE in the last output folder", again.leftoverText().contains("quit-slow.INCOMPLETE") &&
                         again.leftoverText().contains(juce::String(root.string())) && again.leftoverText().contains("Nothing is deleted"));
                    fs::create_directories(root / "old.INCOMPLETE.stale-20260101T000000Z");
                    again.showDomain(D::Validation);
                    TEST("leftovers r2#2: opening a page lists leftovers on a worker, not inside showDomain", again.leftoversChecking());
                    settleLeftovers(again);
                    TEST("leftovers: opening a page lists a moved-aside .stale- folder too", again.leftoverText().contains("old.INCOMPLETE.stale-20260101T000000Z"));
                    TEST("leftovers: the listing changes nothing", fs::is_directory(root / "quit-slow.INCOMPLETE") &&
                         fs::is_directory(root / "old.INCOMPLETE.stale-20260101T000000Z"));
                    {   // #43 gate D note 2: a job running in this process is being written, not left over
                        const auto rs = st.manager().enqueueJob(slowJob(root / "run-slow"), {});
                        for (int i = 0; i < 250 && !fs::exists(root / "run-slow.INCOMPLETE"); ++i) pump(20);
                        again.showDomain(D::ImportExport);
                        settleLeftovers(again);
                        TEST("leftovers: a running job's own NAME.INCOMPLETE is not listed; the others still are (#43 gate D note 2)",
                             fs::is_directory(root / "run-slow.INCOMPLETE") && !again.leftoverText().contains("run-slow.INCOMPLETE") &&
                             again.leftoverText().contains("quit-slow.INCOMPLETE"));
                        st.selectJob(rs);
                        st.cancelJob();
                        waitIdle(5000);
                        pump(100);
                        again.showDomain(D::Validation);
                        settleLeftovers(again);
                        TEST("leftovers: once that job is cancelled its NAME.INCOMPLETE is listed", again.leftoverText().contains("run-slow.INCOMPLETE"));
                        fs::remove_all(root / "run-slow.INCOMPLETE");   // the test's own cleanup (the app never deletes)
                    }
                    const auto rec = stateDir / "last-output-folder.txt";
                    const auto keep = juce::File(rec.string()).loadFileAsString();
                    fs::create_directories(root / "clean-out");
                    juce::File(rec.string()).replaceWithText(juce::String((root / "clean-out").string()));
                    again.showDomain(D::ImportExport);
                    settleLeftovers(again);
                    TEST("leftovers: no banner when the last output folder has none", again.leftoverText().isEmpty());
                    juce::File(rec.string()).replaceWithText(keep);
                    fs::remove_all(root / "old.INCOMPLETE.stale-20260101T000000Z");
                }
                {   // a studio closed while a job runs: the job is cancelled, its `done` is never called, nothing published
                    bool landed = false;
                    auto tmp = std::make_unique<app::StudioComponent>([] {});
                    tmp->manager().enqueueJob(slowJob(root / "destroyed-slow"), [&landed](const jobs::JobState&) { landed = true; });
                    pump(50);
                    tmp.reset();
                    pump(300);
                    TEST("closing the studio during a job: no crash, its done not called, nothing published", !landed && !fs::exists(root / "destroyed-slow"));
                }
                TEST("quit: a cancelled job leaves only NAME.INCOMPLETE, nothing published (D17)", !fs::exists(root / "quit-slow") &&
                     fs::is_directory(root / "quit-slow.INCOMPLETE") && !fs::exists(root / "quit-pkg2") && !fs::exists(root / "quit-pkg2.INCOMPLETE"));
                TEST("quit: idle -> quits at once, no work prompt", !st.busy() && st.requestQuit(wfn) && wq == 3 && !st.workPromptShown());
                const int listTop = st.jobListTop();   // the job list starts right under the job picker while the prompt is hidden
                // Codex P1-a: a KST backup (Sounds & Resources, its own thread) counts as running work
                st.resources().startBackup(juce::File((root / "Mine.KST").string()), juce::File((root / "quit-backup").string()));
                TEST("quit: a KST backup running counts as busy and defers the quit (Codex P1-a)", st.busy() && !st.requestQuit(wfn) && wq == 3 &&
                     st.workPromptShown() && find<juce::Label>(io, "Still running:") && find<juce::Label>(io, "Still running:")->getText().contains("KST backup"));
                TEST("quit: the prompt's two rows take space only while it shows", st.jobListTop() == listTop + 2 * (28 + 6));
                st.resolveRunningWork(W::Wait);
                TEST("quit: Wait does not quit while the backup runs", wq == 3);
                waitIdle(10000);
                pump(300);
                TEST("quit: Wait lets the backup finish and publish, then quits", wq == 4 && !st.resources().backingUp() && fs::is_directory(root / "quit-backup"));
                TEST("quit: the hidden prompt takes no rows again", st.jobListTop() == listTop);
                st.setValidationTarget(juce::File());   // the quit jobs' output is not a target the Validation checks below expect
            }

            phase("studio: app quit");
            // App quit (Cmd-Q / menu Quit / main window close): clean quits at once; unwritten edits ask first, Cancel aborts
            int quits = 0;
            auto quitFn = [&quits] { ++quits; };
            TEST("quit: no unwritten edits -> quits at once, no prompt", !st.styleModel().dirty() && st.requestQuit(quitFn) && quits == 1 &&
                 !st.unsavedPromptShown());
            st.styleOp().setSelectedId(6);   // Tempo
            st.styleValue().setText("96");
            click(pg, "Apply");
            st.showDomain(D::Validation);
            TEST("quit: unwritten edits -> deferred, the Style page asks", !st.requestQuit(quitFn) && quits == 1 && st.unsavedPromptShown() &&
                 st.domain() == D::StyleStudio && find<juce::Button>(pg, "Discard Edits and Quit") &&
                 find<juce::Button>(pg, "Discard Edits and Quit")->isVisible() && st.styleStatus().contains("Quit AI Arranger"));
            st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Cancel);
            TEST("quit: Cancel aborts the quit; edits and undo stay", quits == 1 && !st.unsavedPromptShown() && st.styleModel().dirty() &&
                 st.styleModel().style().tempo_bpm == 96 && st.styleModel().undoLabel() == "Tempo");
            st.requestQuit(quitFn);
            st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Save, juce::File((root / "quit-save.uasf").string()));
            const bool quitWaits = quits == 1 && st.styleModel().dirty() && st.busy();
            waitIdle(10000);
            pump(300);
            TEST("D33 P2 quit: Save-before-quit runs as an export job; the quit waits for it", quitWaits);
            TEST("quit: Save writes the edits, then quits", quits == 2 && fs::is_regular_file(root / "quit-save.uasf") && !st.styleModel().dirty());
            st.styleValue().setText("95");
            click(pg, "Apply");
            st.requestQuit(quitFn);
            TEST("quit: still deferred before a choice", quits == 2);
            st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Discard);
            TEST("quit: Discard quits", quits == 3 && !st.unsavedPromptShown());
            {   // Codex P1-b: an export starts while the Style prompt shows; Save or Discard then goes through the work guard
                using W = app::StudioComponent::WorkChoice;
                st.requestQuit(quitFn);   // the Style is still unwritten (Discard above quit without reloading it)
                const auto s1 = st.manager().enqueueJob(slowJob(root / "quit-slow2"), {});
                pump(100);
                st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Discard);
                TEST("quit: Discard while an export runs does not quit mid-job; the work prompt asks (Codex P1-b)", s1 > 0 && quits == 3 &&
                     st.workPromptShown() && !st.unsavedPromptShown() && st.domain() == D::ImportExport);
                st.resolveRunningWork(W::CancelJobsAndQuit);
                waitIdle(10000);
                pump(300);
                TEST("quit: after Discard, Cancel Jobs and Quit quits without asking about the Style again", quits == 4 && !st.unsavedPromptShown() &&
                     !st.workPromptShown() && !fs::exists(root / "quit-slow2"));
                st.requestQuit(quitFn);
                const auto s2 = st.manager().enqueueJob(slowJob(root / "quit-slow3"), {});
                pump(100);
                st.resolveUnsavedStyle(app::StudioComponent::UnsavedChoice::Save, juce::File((root / "quit-save2.uasf").string()));
                TEST("quit: Save while an export runs: the work prompt asks (the save job queues behind it)", quits == 4 &&
                     st.workPromptShown() && !st.unsavedPromptShown());
                st.resolveRunningWork(W::KeepWorking);
                st.selectJob(s2);
                st.cancelJob();
                waitIdle(10000);
                pump(100);
                TEST("quit: Save while an export runs writes the Style once the queue reaches it", quits == 4 &&
                     fs::is_regular_file(root / "quit-save2.uasf") && !st.styleModel().dirty() && !st.unsavedPromptShown());
                TEST("quit: Keep Working aborts; nothing running and the Style written -> the next quit is immediate", quits == 4 && !st.busy() &&
                     st.requestQuit(quitFn) && quits == 5);
            }
            {   // D33 P1: a Save or Discard chosen for a quit holds for the Style as it was then; an edit after it asks again
                using W = app::StudioComponent::WorkChoice;
                using U = app::StudioComponent::UnsavedChoice;
                auto edit = [&](const char* bpm) {
                    st.showDomain(D::StyleStudio);
                    st.styleOp().setSelectedId(6);   // Tempo
                    st.styleValue().setText(bpm);
                    click(pg, "Apply");
                };
                edit("94");
                st.requestQuit(quitFn);
                const auto d1 = st.manager().enqueueJob(slowJob(root / "d33-slow1"), {});
                pump(100);
                st.resolveUnsavedStyle(U::Discard);
                st.resolveRunningWork(W::Wait);
                edit("93");                 // after the Discard, while the quit waits for the job
                st.requestQuit(quitFn);     // a repeated Quit while waiting stays one pending quit
                st.selectJob(d1);
                st.cancelJob();
                waitIdle(10000);
                pump(300);
                TEST("D33 P1 quit: Discard, Wait, then an edit: no quit, the Style page asks again", quits == 5 && st.unsavedPromptShown() &&
                     st.styleModel().dirty() && st.styleModel().style().tempo_bpm == 93);
                st.resolveUnsavedStyle(U::Discard);
                TEST("D33 P1 quit: Discard of the Style as shown then quits once", quits == 6 && !st.unsavedPromptShown() && !st.workPromptShown());
                st.requestQuit(quitFn);   // still unwritten (tempo 93)
                const auto d2 = st.manager().enqueueJob(slowJob(root / "d33-slow2"), {});
                pump(100);
                st.resolveUnsavedStyle(U::Save, juce::File((root / "d33-save.uasf").string()));
                st.resolveRunningWork(W::Wait);
                edit("92");                 // after the Save was chosen, while waiting
                st.selectJob(d2);
                st.cancelJob();
                waitIdle(10000);
                pump(300);
                uasf::UasfDeserializer des;
                const auto saved = des.deserializeFromFile((root / "d33-save.uasf").string());
                TEST("D33 P1 quit: Save, Wait, then an edit: the file holds the Style as saved, no quit, the page asks again", quits == 6 &&
                     saved.success && saved.style.tempo_bpm == 93 && st.unsavedPromptShown() && st.styleModel().dirty() &&
                     st.styleModel().style().tempo_bpm == 92);
                st.resolveUnsavedStyle(U::Cancel);
                st.styleUndo();   // back to tempo 93, the state written
                TEST("D33 P1 quit: undo back to the saved state: nothing unwritten, the quit is immediate", !st.styleModel().dirty() &&
                     st.requestQuit(quitFn) && quits == 7);
                edit("91");
                st.requestQuit(quitFn);
                st.resolveUnsavedStyle(U::Save, juce::File((root / "d33-save.uasf").string()));   // exists: the save job fails
                waitIdle(10000);
                pump(300);
                TEST("D33 P1 quit: a failed Save never quits; the Style stays unwritten and the page asks again", quits == 7 &&
                     st.unsavedPromptShown() && st.styleModel().dirty() && !st.jobList().empty() &&
                     st.jobList().back().reason.find("already exists") != std::string::npos);
                st.resolveUnsavedStyle(U::Cancel);
                st.styleUndo();
                TEST("D33 P1 quit: Cancel then undo: clean again", !st.styleModel().dirty() && quits == 7);
                {   // Codex F2: a deferred Save-before-open never replaces a Style opened meanwhile
                    const auto a = st.styleModel().path();
                    edit("90");
                    const auto f2 = st.manager().enqueueJob(slowJob(root / "f2-slow"), {});
                    pump(100);
                    st.openStyle(juce::File(sty.string()));   // B: asks; Save queues behind the slow job
                    st.resolveUnsavedStyle(U::Save, juce::File((root / "f2-save.uasf").string()));
                    st.styleUndo();                             // back to clean
                    st.openStyle(juce::File((root / "gate53.uasf").string()));   // C opens at once (nothing unwritten)
                    const bool cOpen = fs::equivalent(st.styleModel().path(), root / "gate53.uasf");
                    st.selectJob(f2);
                    st.cancelJob();
                    waitIdle(10000);
                    pump(300);
                    TEST("Codex F2 studio: the earlier Save-before-open lands without replacing the Style opened meanwhile", !fs::equivalent(a, root / "gate53.uasf") &&
                         cOpen && fs::equivalent(st.styleModel().path(), root / "gate53.uasf") && !st.unsavedPromptShown() &&
                         fs::is_regular_file(root / "f2-save.uasf") && !st.styleModel().dirty());
                }
                for (const char* n : {"d33-slow1.INCOMPLETE", "d33-slow2.INCOMPLETE", "f2-slow.INCOMPLETE"}) fs::remove_all(root / n);   // the test's own cleanup (the app never deletes)
            }
            {
                app::LiveHostComponent h;
                int hq = 0;
                h.confirmQuit([&hq] { ++hq; });
                TEST("quit: the live host quits at once when no Studio window is open", hq == 1);
            }
            phase("studio: standalone window quit");
            {   // the standalone Sounds & Resources window's work goes through the same quit prompt (not cancelled silently)
                using W = app::StudioComponent::WorkChoice;
                // Owner gate A/B: the sources' SHA-256 manifest before and after each choice, and the output folder's listing
                auto manifest = [](const fs::path& dir) {
                    std::map<std::string, std::string> m;
                    for (const auto& e : fs::recursive_directory_iterator(dir)) {
                        if (!e.is_regular_file()) continue;
                        bool c = false;
                        std::string err;
                        m[fs::relative(e.path(), dir).string()] = media::detail::sha256File(e.path(), [](size_t) { return true; }, &c, &err);
                    }
                    return m;
                };
                auto outputs = [&](const std::string& name) {   // every entry NAME* in root
                    std::vector<std::string> r;
                    for (const auto& e : fs::directory_iterator(root)) if (e.path().filename().string().rfind(name, 0) == 0) r.push_back(e.path().filename().string());
                    std::sort(r.begin(), r.end());
                    return r;
                };
                auto evidence = [&](const char* choice, const std::string& name, const fs::path& src, const std::map<std::string, std::string>& before) {
                    const auto after = manifest(src);
                    std::printf("  evidence: %s: source %s: %zu files, SHA-256 manifest %s; outputs %s*:", choice, src.filename().c_str(), after.size(),
                                after == before ? "unchanged" : "CHANGED", name.c_str());
                    for (const auto& o : outputs(name)) std::printf(" %s", o.c_str());
                    std::printf("\n");
                    for (const auto& [f, h] : after) std::printf("  evidence:   %s  %s\n", h.substr(0, 16).c_str(), f.c_str());
                    std::fflush(stdout);
                    std::system(("ls -la '" + root.string() + "' | grep -E ' " + name + "' | sed 's/^/  evidence:   /'").c_str());
                    for (const auto& o : outputs(name))
                        std::system(("ls -la '" + (root / o).string() + "' | sed 's/^/  evidence:     " + o + ": /'").c_str());
                    return after == before;
                };
                app::LiveHostComponent h;
                find<juce::Button>(h, "Sound Tools...")->onClick();
                auto* rw = h.resources();
                int hq = 0;
                const auto mineBefore = manifest(root / "Mine.KST");
                rw->startBackup(juce::File((root / "Mine.KST").string()), juce::File((root / "host-backup").string()));
                h.confirmQuit([&hq] { ++hq; });
                auto* hs = h.studio();
                TEST("quit: a backup in the standalone Sounds & Resources window defers the app quit; the Studio asks", rw->busy() && hq == 0 && hs &&
                     hs->workPromptShown() && find<juce::Label>(hs->page(D::ImportExport), "Still running:") &&
                     find<juce::Label>(hs->page(D::ImportExport), "Still running:")->getText().contains("Sounds & Resources"));
                TEST("quit: the first prompt of a Studio made by the quit lists only real work, no Library Manager (#43 gate D note 1)",
                     hs && !find<juce::Label>(hs->page(D::ImportExport), "Still running:")->getText().contains("Library Manager"));
                hs->resolveRunningWork(W::Wait);
                for (int i = 0; i < 500 && hq == 0; ++i) pump(20);
                TEST("quit: Wait lets that backup finish and publish, then quits", hq == 1 && !rw->busy() && fs::is_directory(root / "host-backup"));
                TEST("quit: Wait: the final backup published with its report, no NAME.INCOMPLETE, the source unchanged (SHA-256 manifest)",
                     evidence("Wait", "host-backup", root / "Mine.KST", mineBefore) && !fs::exists(root / "host-backup.INCOMPLETE") &&
                     fs::is_regular_file(root / "host-backup.export.md") && media::verifyBackup(root / "host-backup").empty());
                fs::create_directories(root / "bigkst2/PCM");
                { std::ofstream big(root / "bigkst2/PCM/RAM01.PCM", std::ios::binary); std::string mb(1 << 20, 'p'); for (int i = 0; i < 128; ++i) big << mb; }
                put(root / "bigkst2/PCM/RAM02.PCM", "second source file");
                const auto bigBefore = manifest(root / "bigkst2");
                {   // closing (hiding) the Sounds & Resources window does not cancel its backup
                    auto* win = dynamic_cast<juce::DocumentWindow*>(rw->getTopLevelComponent());
                    rw->startBackup(juce::File((root / "bigkst2").string()), juce::File((root / "host-backup3").string()));
                    if (win) win->closeButtonPressed();
                    TEST("closing the Sounds & Resources window hides it; its backup keeps running", win && !win->isVisible() && rw->busy());
                    for (int i = 0; i < 1500 && rw->busy(); ++i) pump(20);
                    TEST("the hidden window's backup finishes and publishes (not cancelled)", !rw->busy() && fs::is_directory(root / "host-backup3") &&
                         rw->lastBackupJob().phase == jobs::Phase::Done && hq == 1 && !fs::exists(root / "host-backup3.INCOMPLETE") &&
                         manifest(root / "bigkst2") == bigBefore);
                    fs::remove_all(root / "host-backup3");
                }
                rw->startBackup(juce::File((root / "bigkst2").string()), juce::File((root / "host-backup2").string()));
                h.confirmQuit([&hq] { ++hq; });
                TEST("quit: again deferred while the standalone window backs up", hq == 1 && hs->workPromptShown());
                // the checks below run without pumping the message loop, so the backup cannot land between them
                h.confirmQuit([&hq] { ++hq; });
                TEST("quit: a second quit while the prompt shows changes nothing (one prompt, no quit)", hq == 1 && hs->workPromptShown() && rw->busy());
                hs->resolveRunningWork(W::KeepWorking);
                TEST("quit: Keep Working closes the prompt; the standalone backup keeps running", hq == 1 && !hs->workPromptShown() && rw->busy());
                h.confirmQuit([&hq] { ++hq; });
                TEST("quit: quitting again after Keep Working asks again", hq == 1 && hs->workPromptShown());
                hs->resolveRunningWork(W::KeepWorking);
                for (int i = 0; i < 1500 && rw->busy(); ++i) pump(20);
                pump(300);
                TEST("quit: Keep Working: the job continues and publishes normally; no quit; no NAME.INCOMPLETE; the source unchanged",
                     hq == 1 && !rw->busy() && rw->lastBackupJob().phase == jobs::Phase::Done && !hs->workPromptShown() &&
                     evidence("Keep Working", "host-backup2", root / "bigkst2", bigBefore) && fs::is_directory(root / "host-backup2") &&
                     !fs::exists(root / "host-backup2.INCOMPLETE") && media::verifyBackup(root / "host-backup2").empty());
                rw->startBackup(juce::File((root / "bigkst2").string()), juce::File((root / "host-backup5").string()));
                h.confirmQuit([&hq] { ++hq; });
                hs->resolveRunningWork(W::CancelJobsAndQuit);
                for (int i = 0; i < 500 && hq == 1; ++i) pump(20);
                TEST("quit: Cancel Jobs and Quit cancels that backup, then quits; nothing published", hq == 2 && !rw->busy() &&
                     rw->kstText().contains("cancelled") && !fs::exists(root / "host-backup5"));
                {
                    const auto left = outputs("host-backup5");   // the cancelled job's NAME.INCOMPLETE if it had started writing; never a NAME
                    TEST("quit: Cancel: only NAME.INCOMPLETE may remain (never deleted, never published), the source unchanged (SHA-256 manifest)",
                         evidence("Cancel Jobs and Quit", "host-backup5", root / "bigkst2", bigBefore) &&
                         std::all_of(left.begin(), left.end(), [](const std::string& n) { return n == "host-backup5.INCOMPLETE"; }) &&
                         rw->lastBackupJob().phase == jobs::Phase::Cancelled);
                }
                pump(300);
                TEST("quit: that quit ran exactly once", hq == 2);
                {   // #43 gate D e: a backup in the Sounds & Resources window (its own JobRunner), cancelled: the next launch's banner lists
                    // it, and a later Studio job elsewhere hides neither it nor the Studio's own leftovers
                    const auto b6 = root / "res-out" / "host-backup6";
                    rw->startBackup(juce::File((root / "bigkst2").string()), juce::File(b6.string()));
                    for (int i = 0; i < 500 && !fs::exists(b6.string() + ".INCOMPLETE"); ++i) pump(10);
                    {
                        app::StudioComponent s([] {});
                        settleLeftovers(s);
                        TEST("leftovers: the running Sounds & Resources backup's own NAME.INCOMPLETE is not listed (#43 gate D note 2)",
                             rw->busy() && fs::is_directory(b6.string() + ".INCOMPLETE") && !s.leftoverText().contains("host-backup6.INCOMPLETE"));
                    }
                    rw->cancelJobs();
                    for (int i = 0; i < 500 && rw->busy(); ++i) pump(20);
                    std::ifstream kept(root / "kept.uasf", std::ios::binary);   // a real UASF: the job parses what it publishes (D33 P2)
                    hs->manager().enqueueJob(jobs::styleUasf("later", {std::istreambuf_iterator<char>(kept), {}}, root / "later-out" / "later.uasf"), {});
                    for (int i = 0; i < 500 && !fs::exists(root / "later-out" / "later.uasf"); ++i) pump(20);
                    app::StudioComponent s([] {});   // as on the next launch
                    settleLeftovers(s);
                    TEST("leftovers: a cancelled Sounds & Resources window backup is listed on the next launch, after a later Studio job "
                         "elsewhere; the Studio's own leftovers too (#43 gate D e)",
                         fs::is_directory(b6.string() + ".INCOMPLETE") && !fs::exists(b6) && fs::is_regular_file(root / "later-out" / "later.uasf") &&
                         s.leftoverText().contains("host-backup6.INCOMPLETE") && s.leftoverText().contains(juce::String((root / "res-out").string())) &&
                         s.leftoverText().contains("quit-slow.INCOMPLETE"));
                }
                rw->startBackup(juce::File((root / "bigkst2").string()), juce::File((root / "host-backup4").string()));
                h.confirmQuit([&hq] { ++hq; });
                hs->resolveRunningWork(W::Wait);
                h.confirmQuit([&hq] { ++hq; });
                TEST("quit: quitting again during Wait keeps waiting (no second prompt, no early quit)", hq == 2 && rw->busy() &&
                     hs->workPromptShown());
                for (int i = 0; i < 1500 && hq == 2; ++i) pump(20);
                pump(300);
                TEST("quit: the Wait finishes the backup, publishes it and quits exactly once", hq == 3 && !rw->busy() &&
                     fs::is_directory(root / "host-backup4") && !fs::exists(root / "host-backup4.INCOMPLETE") && manifest(root / "bigkst2") == bigBefore);
                fs::remove_all(root / "bigkst2");
            }
            phase("studio: r5 scans");
            {   // Codex r5 P2: a KST Inventory / Compare Folders scan counts as the window's work (standalone and in the Studio)
                using W = app::StudioComponent::WorkChoice;
                using KA = ui::ResourceBrowserModel::KstAction;
                app::LiveHostComponent h;
                find<juce::Button>(h, "Sound Tools...")->onClick();
                auto* rw = h.resources();
                rw->runKst(KA::Inventory, (root / "Mine.KST").string());
                TEST("Codex r5 P2: a KST Inventory makes the standalone window busy()", rw->busy() && rw->kstScanning());
                int hq = 0;
                h.confirmQuit([&hq] { ++hq; });
                TEST("Codex r5 P2: quit during a KST Inventory is deferred to the prompt", hq == 0 && h.studio() && h.studio()->workPromptShown());
                if (auto* hs = h.studio()) hs->resolveRunningWork(W::CancelJobsAndQuit);   // a read-only scan cannot be stopped: quit waits
                for (int i = 0; i < 500 && hq == 0; ++i) pump(20);
                TEST("Codex r5 P2: Cancel Jobs and Quit waits for the read-only scan, shows its result, then quits once",
                     hq == 1 && !rw->busy() && rw->kstText().contains("Semantic merge"));
                rw->runKst(KA::Diff, (root / "Mine.KST").string(), (root / "Mine.KST").string());
                TEST("Codex r5 P2: a Compare Folders scan makes the standalone window busy()", rw->busy());
                for (int i = 0; i < 500 && rw->busy(); ++i) pump(20);
                app::StudioComponent s([] {});
                for (int i = 0; i < 500 && s.busy(); ++i) pump(20);   // its own start-up work first
                const bool idleBefore = !s.busy();
                s.resources().runKst(KA::MergePlan, (root / "Mine.KST").string(), (root / "Mine.KST").string());
                TEST("Codex r5 P2: a Merge Plan scan in the Studio's Sounds & Resources makes the (idle) Studio busy()", idleBefore && s.busy());
                for (int i = 0; i < 500 && s.busy(); ++i) pump(20);
                TEST("Codex r5 P2: the Studio is idle once the scan has landed", !s.busy());
            }
        }

        phase("studio: validation");
        // Validation & Reports: badges from the shared verifiers; L3 only from a matching owner record
        st.showDomain(D::Validation);
        TEST("studio: no target -> checks unavailable with a reason", st.validatorUnavailable(app::StudioComponent::Validator::VerifyPackage).startsWith("Needs:"));
        const auto pkg = root / "organ";
        st.setValidationTarget(juce::File(pkg.string()));
        st.runValidator(app::StudioComponent::Validator::VerifyPackage);
        waitIdle(20000);
        const auto filled = juce::String::fromUTF8("\xE2\x97\x86 L1  \xE2\x97\x86\xE2\x97\x86 L2  \xE2\x97\x87 L3");
        TEST("studio: Verify Package -> L1 and L2 reached, L3 hollow", st.badgeText() == filled && st.inspectorText().contains("Level 3 hardware test not recorded"));
        bool c = false;
        const auto manHash = media::detail::sha256File(pkg / "PACKAGE.sha256", [](size_t) { return true; }, &c, &err);
        auto record = [&](const std::string& hash) {
            put(root / "L3.txt", "aiarranger-l3-record 1\nmanifest-sha256 " + hash + "\ndevice Pa5X\ndate 2026-11-20\ntester owner\nresult pass\n");
            st.attachL3Record(juce::File((root / "L3.txt").string()));
            TEST("studio: an L3 record is applied in a background job", st.busy());
            waitIdle(20000);
            return st.verification().l3.state;
        };
        TEST("studio: an L3 record for another output gives no L3", record(std::string(64, '0')) == ui::studio::Check::NotRun && st.badgeText() == filled);
        TEST("studio: the owner's record for this output gives L3", record(manHash) == ui::studio::Check::Passed &&
             st.badgeText().endsWith(juce::String::fromUTF8("\xE2\x97\x86\xE2\x97\x86\xE2\x97\x86 L3")));
        {   // review finding: L1 from an earlier Verify Package must not carry L3 onto files changed since
            const auto keep = fs::path(pkg / "RESOURCES.tsv");
            std::ifstream in(keep, std::ios::binary);
            const std::string orig((std::istreambuf_iterator<char>(in)), {});
            st.runValidator(app::StudioComponent::Validator::VerifyPackage);
            waitIdle(20000);
            put(keep, "changed after Verify Package\n");
            TEST("studio: a record attached after the files changed gives no L3 (L1 re-checked)",
                 record(manHash) == ui::studio::Check::NotRun && st.verification().l1.state == ui::studio::Check::Failed);
            put(keep, orig);
        }
        put(pkg / "RESOURCES.tsv", "changed after the test\n");
        st.setValidationTarget(juce::File(pkg.string()));
        st.runValidator(app::StudioComponent::Validator::VerifyPackage);
        waitIdle(20000);
        TEST("studio: changed files -> L1 failed, and the old record no longer gives L3",
             st.verification().l1.state == ui::studio::Check::Failed && record(manHash) == ui::studio::Check::NotRun);
        st.setValidationTarget(juce::File((root / "Mine.KST").string()));
        st.runValidator(app::StudioComponent::Validator::KstInventory);
        waitIdle(5000);
        TEST("studio: KST Inventory runs in-process and sets no badge", st.jobsText().contains("File level only") &&
             st.badgeText() == juce::String::fromUTF8("\xE2\x97\x87 L1  \xE2\x97\x87 L2  \xE2\x97\x87 L3"));
        put(root / "Loose.KST/STYLE/USER01/x.STG", "x");
        put(root / "Loose.KST/NOTES.TXT", "n");   // a loose top-level file: R-06 WARN
        st.setValidationTarget(juce::File((root / "Loose.KST").string()));
        st.runValidator(app::StudioComponent::Validator::LibraryStructure);
        waitIdle(5000);
        TEST("studio E8: Library Structure validator: rule table, finding with its source, L1 from the validator, L2 not applicable",
             st.jobsText().contains("Rule table: Pa5X KST") && st.jobsText().contains("WARN  R-06  NOTES.TXT") && st.jobsText().contains("[MAN5 p.738") &&
             st.jobsText().contains("0 error(s), 1 warning(s)") && st.verification().l1.state == ui::studio::Check::Passed &&
             st.verification().l2.state == ui::studio::Check::NotRun);
        media::sound::writePackage(media::sound::buildDemoOrgan(), root / "organ2", "", &err);
        juce::File sf2;
        for (const auto& e : fs::directory_iterator(root / "organ2")) if (e.path().extension() == ".sf2") sf2 = juce::File(e.path().string());
        st.setValidationTarget(sf2);
        st.runValidator(app::StudioComponent::Validator::Sf2Check);
        waitIdle(5000);
        TEST("studio: SF2 Check -> L1 only", st.verification().l1.state == ui::studio::Check::Passed && st.verification().l2.state == ui::studio::Check::NotRun);
        st.setValidationTarget(juce::File((root / "organ2").string()));
        const auto pyWhy = st.validatorUnavailable(app::StudioComponent::Validator::SoundPy);
        if (pyWhy.isEmpty()) {
            st.runValidator(app::StudioComponent::Validator::SoundPy);
            waitIdle(120000);
            TEST("studio: the Python sound validator runs as a child process and leaves the badges alone",
                 st.jobsText().contains("exit code 0") && st.verification().l1.state == ui::studio::Check::NotRun);
        } else {
            std::printf("  info: Python sound validator not available here: %s\n", pyWhy.toRawUTF8());
            TEST("studio: an unavailable Python validator names what it needs", pyWhy.startsWith("Needs:"));
        }
        TEST("studio: nothing is labelled Pa5X-compatible", !st.inspectorText().containsIgnoreCase("compatible") &&
             !st.jobsText().containsIgnoreCase("Pa5X-compatible") && !st.libraryText().containsIgnoreCase("compatible"));

        if (const char* dir = std::getenv("AIARR_SNAPSHOT_DIR"); dir && *dir) {   // offscreen renders for a later look; not a hands-on check
            st.setValidationTarget(juce::File(pkg.string()));
            fs::remove_all(pkg);
            media::sound::writePackage(media::sound::buildDemoOrgan(), pkg, "", &err);
            st.runValidator(app::StudioComponent::Validator::VerifyPackage);
            waitIdle(20000);
            st.openSource(juce::File((root / "Mine.KST").string()));
            waitIdle(5000);
            int n = 0;
            for (D d : {D::Library, D::ImportExport, D::SampleLab, D::StyleStudio, D::Validation})
                for (int i = 0; i < st.segmentNames(d).size(); ++i) {
                    st.showDomain(d, i);
                    if (st.segment() != i) continue;   // an unavailable segment
                    pump(50);
                    const auto img = st.createComponentSnapshot(st.getLocalBounds(), true, 1.0f);
                    auto name = juce::String(app::StudioComponent::domainName(d)) + " - " + st.segmentNames(d)[i];
                    name = name.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -").replace(" ", "-").toLowerCase();
                    juce::File out = juce::File(dir).getChildFile(juce::String(++n).paddedLeft('0', 2) + "-" + name + ".png");
                    out.deleteFile();
                    juce::FileOutputStream os(out);
                    juce::PNGImageFormat().writeImageToStream(img, os);
                    std::printf("  info: offscreen snapshot written to %s\n", out.getFullPathName().toRawUTF8());
                }
        }
        fs::remove_all(root);
    }

    phase(nullptr);
    std::filesystem::remove_all(stateDir);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
