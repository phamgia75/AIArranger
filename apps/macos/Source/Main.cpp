// Gate 2 — AI Arranger macOS live reference host entry point (JUCE).
// The window hosts the Gate 2 LiveHostComponent (facade-driven live loop). The
// Gate 14 MainComponent remains in the tree for reference but is no longer the
// window content.
#include <juce_gui_extra/juce_gui_extra.h>
#include "JobPort.h"
#include "LiveHostComponent.h"

namespace ai_arranger::app {

class AIArrangerApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "AI Arranger"; }
    const juce::String getApplicationVersion() override { return "0.14.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override {
        jobs::setOutputRecord(appOutputRecordFile());   // every job queue records where it writes, for the leftover banner
        mainWindow_ = std::make_unique<MainWindow>("AI Arranger");
    }
    void shutdown() override { mainWindow_ = nullptr; }
    // Cmd-Q, the app menu Quit and the main window's close button all land here: the live host asks the Studio first
    // when a Style has unwritten edits (Cancel keeps the app running).
    void systemRequestedQuit() override {
        if (auto* host = mainWindow_ ? dynamic_cast<LiveHostComponent*>(mainWindow_->getContentComponent()) : nullptr)
            host->confirmQuit([] { juce::JUCEApplicationBase::quit(); });
        else
            quit();
    }

private:
    class MainWindow : public juce::DocumentWindow {
    public:
        explicit MainWindow(const juce::String& name)
            : DocumentWindow(name,
                             juce::Colours::black,
                             DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new LiveHostComponent(), true);
            setResizable(true, true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow_;
};

} // namespace ai_arranger::app

START_JUCE_APPLICATION(ai_arranger::app::AIArrangerApplication)
