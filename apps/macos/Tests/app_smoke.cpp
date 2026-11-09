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
        TEST("button present: Resources...", find<juce::Button>(*host, "Resources...") != nullptr);
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
        TEST("three tabs", rc.tabs().getNumTabs() == 3 && rc.tabs().getTabNames()[1] == "KST Library");
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
        fs::remove_all(root);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
