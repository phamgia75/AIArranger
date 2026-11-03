#pragma once

// LiveHostComponent (Gate 2) — the minimal Mac live-performance reference host
// UI. Selects MIDI in/out, loads an SFF1 style, shows the current chord/section,
// and drives transport + section controls. Every control posts a lock-free
// command through the LiveEngineFacade (never blocks the RT engine thread); a
// 30 Hz message-thread timer polls the facade's atomic snapshot for display.
//
// This is a reference/integration host, NOT the shippable product (iPad SwiftUI
// is Gate 5).

#include <juce_gui_extra/juce_gui_extra.h>
#include "LiveHostDriver.h"
#include "ui/tempo_control_model.h"

namespace ai_arranger::app {

class LiveHostComponent : public juce::Component, private juce::Timer {
public:
    LiveHostComponent();
    ~LiveHostComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;     // message thread: poll snapshot -> labels
    void refreshDevices();
    void loadStyleDialog();

    LiveHostDriver driver_;

    juce::Label   titleLabel_, inLabel_, outLabel_, styleLabel_, chordLabel_,
                  sectionLabel_, tempoLabel_, statusLabel_;
    juce::ComboBox inputBox_, outputBox_;
    juce::Slider  tempoSlider_;
    ui::TempoControlModel tempoModel_;   // slider <-> engine tempo (Issue #7)

    juce::TextButton refreshBtn_{"Refresh"}, loadBtn_{"Load Style..."},
                     startBtn_{"Start"}, stopBtn_{"Stop"}, syncBtn_{"Sync Start"},
                     introBtn_{"Intro"}, varA_{"A"}, varB_{"B"}, varC_{"C"},
                     varD_{"D"}, fillBtn_{"Fill"}, breakBtn_{"Break"},
                     endingBtn_{"Ending"}, panicBtn_{"PANIC"};

    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiveHostComponent)
};

} // namespace ai_arranger::app
