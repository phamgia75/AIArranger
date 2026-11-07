// P2.5 UI smoke test: the real LiveHostComponent (the app's window content),
// driven only through what a user can reach: buttons found by their visible
// text, clicked with triggerClick(), the JUCE message loop run, and the labels
// the user sees read back. No hardware: no output is selected and
// AIARR_MIDI_HW_TX=0 blocks any send at the CoreMIDI boundary.

#include <juce_gui_extra/juce_gui_extra.h>
#include "../Source/LiveHostComponent.h"
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

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
