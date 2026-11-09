#include "LiveHostComponent.h"
#include "ResourcesComponent.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "engine/arranger/chord_input.h"
#include "session/chord_label.h"

namespace ai_arranger::app {

namespace {
juce::String formatChord(const session::EngineSnapshot& s) {
    return juce::String(session::chordLabel(s.chordType, s.chordRoot, s.chordBass));
}

void styleLabel(juce::Label& l, const juce::String& text, float pt = 15.0f) {
    l.setText(text, juce::dontSendNotification);
    l.setFont(juce::Font(juce::FontOptions(pt)));
    l.setColour(juce::Label::textColourId, juce::Colours::white);
}
} // namespace

LiveHostComponent::LiveHostComponent() {
    styleLabel(titleLabel_, "AI Arranger - Live Reference Host", 20.0f);
    styleLabel(inLabel_,  "MIDI In");
    styleLabel(outLabel_, "MIDI Out");
    styleLabel(styleLabel_,   "Style: (demo)");
    styleLabel(chordLabel_,   "--", 34.0f);
    styleLabel(sectionLabel_, "Section: -");
    styleLabel(tempoLabel_,   "Tempo 120");
    styleLabel(statusLabel_,  "Status: starting...");

    addAndMakeVisible(titleLabel_);
    addAndMakeVisible(inLabel_);
    addAndMakeVisible(outLabel_);
    addAndMakeVisible(styleLabel_);
    addAndMakeVisible(chordLabel_);
    addAndMakeVisible(sectionLabel_);
    addAndMakeVisible(tempoLabel_);
    addAndMakeVisible(statusLabel_);
    addAndMakeVisible(inputBox_);
    addAndMakeVisible(outputBox_);

    // Device selection routes through the facade (lock-free).
    inputBox_.onChange = [this] {
        const int id = inputBox_.getSelectedId();
        driver_.facade().selectMidiInput(id == 1 ? -1 : id - 2);
    };
    outputBox_.onChange = [this] {
        const int id = outputBox_.getSelectedId();
        // Route through the driver: it silences the old device and stops the
        // transport before repointing, so a switch never leaves hanging notes.
        driver_.switchOutput(id == 1 ? -1 : id - 2);
    };

    // Engine range (20..400). The timer keeps the slider on the engine's tempo
    // (style load, Stop/Start) without notifying; only a user change is sent.
    tempoSlider_.setRange(ui::TempoControlModel::kMin, ui::TempoControlModel::kMax, 1.0);
    tempoModel_.setTempoSink([this](uint32_t bpm) { driver_.facade().setTempo(bpm); });
    if (tempoModel_.syncFromEngine(driver_.facade().snapshot().tempoBpm))
        tempoSlider_.setValue(tempoModel_.displayed(), juce::dontSendNotification);
    tempoSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    tempoSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    tempoSlider_.onValueChange = [this] { tempoModel_.userSet(tempoSlider_.getValue()); };
    addAndMakeVisible(tempoSlider_);

    // Transport + section controls: each posts a lock-free facade command.
    startBtn_.onClick  = [this] { driver_.facade().transportStart(); };
    stopBtn_.onClick   = [this] { driver_.facade().transportStop(); };
    syncBtn_.onClick   = [this] { driver_.facade().syncStart(); };
    introBtn_.onClick  = [this] { driver_.facade().intro(); };
    varA_.onClick      = [this] { driver_.facade().setVariation(0); };
    varB_.onClick      = [this] { driver_.facade().setVariation(1); };
    varC_.onClick      = [this] { driver_.facade().setVariation(2); };
    varD_.onClick      = [this] { driver_.facade().setVariation(3); };
    fillBtn_.onClick   = [this] { driver_.facade().fill(); };
    breakBtn_.onClick  = [this] { driver_.facade().breakSection(); };
    endingBtn_.onClick = [this] { driver_.facade().ending(); };
    panicBtn_.onClick  = [this] { driver_.facade().panic(); };
    autoFillBtn_.setToggleState(driver_.facade().autoFill(), juce::dontSendNotification);
    autoFillBtn_.onClick = [this] { driver_.facade().setAutoFill(autoFillBtn_.getToggleState()); };
    addAndMakeVisible(autoFillBtn_);
    refreshBtn_.onClick = [this] { driver_.retryMidi(); refreshDevices(); };   // #23
    loadBtn_.onClick    = [this] { loadStyleDialog(); };
    resourcesBtn_.onClick = [this] {
        if (!resourcesWindow_) {
            struct Window : juce::DocumentWindow {
                Window() : DocumentWindow("AI Arranger - Sounds & Resources", juce::Colours::black, DocumentWindow::allButtons) {
                    setUsingNativeTitleBar(true);
                    setContentOwned(new ResourcesComponent(), true);
                    setResizable(true, true);
                    centreWithSize(getWidth(), getHeight());
                }
                void closeButtonPressed() override { setVisible(false); }
            };
            resourcesWindow_ = std::make_unique<Window>();
        }
        resourcesWindow_->setVisible(true);
        resourcesWindow_->toFront(true);
    };

    panicBtn_.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);

    for (auto* b : {&refreshBtn_, &loadBtn_, &resourcesBtn_, &startBtn_, &stopBtn_, &syncBtn_,
                    &introBtn_, &varA_, &varB_, &varC_, &varD_, &fillBtn_,
                    &breakBtn_, &endingBtn_, &panicBtn_})
        addAndMakeVisible(*b);

    driver_.start();       // boot engine + open CoreMIDI (safe with 0 devices)
    refreshDevices();
    startTimerHz(30);
    setSize(720, 460);
}

LiveHostComponent::~LiveHostComponent() {
    stopTimer();
    driver_.stop();
}

void LiveHostComponent::refreshDevices() {
    inputBox_.clear(juce::dontSendNotification);
    inputBox_.addItem("(none)", 1);
    for (const auto& s : driver_.midiIn().enumerateSources())
        inputBox_.addItem(s.name.empty() ? "?" : s.name, s.index + 2);
    const int selIn = driver_.facade().snapshot().midiInLive
                          ? driver_.midiIn().selectedSource() : -1;
    inputBox_.setSelectedId(selIn < 0 ? 1 : selIn + 2, juce::dontSendNotification);

    outputBox_.clear(juce::dontSendNotification);
    outputBox_.addItem("(none)", 1);
    for (const auto& d : driver_.midiOut().enumerate())
        outputBox_.addItem(d.name.empty() ? "?" : d.name, d.index + 2);
    const int selOut = driver_.midiOut().selected();
    outputBox_.setSelectedId(selOut < 0 ? 1 : selOut + 2, juce::dontSendNotification);
}

void LiveHostComponent::loadStyleDialog() {
    chooser_ = std::make_unique<juce::FileChooser>(
        "Select an SFF1 .sty style", juce::File{}, "*.sty");
    const auto flags = juce::FileBrowserComponent::openMode |
                       juce::FileBrowserComponent::canSelectFiles;
    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc) {
        const juce::File f = fc.getResult();
        if (f == juce::File{}) return;   // cancelled

        importers::sff1::Sff1Reader reader;
        auto pr = reader.parseFile(f.getFullPathName().toStdString());
        if (!pr.success) {
            styleLabel(styleLabel_, "Load failed: " + juce::String(pr.error));
            return;
        }
        importers::sff1::Sff1ToUasfMapper mapper;
        auto mr = mapper.map(pr);
        if (!mr.success) {
            styleLabel(styleLabel_, "Map failed: " + juce::String(mr.error));
            return;
        }
        // Parse + map succeeded, so the old style is only replaced by a valid one.
        // reloadStyle() quiesces the engine tick thread (stopTimer blocks until the
        // in-flight callback returns) before swapping, so the load never races
        // tick(). Transport is left stopped — press Start to play the new style.
        driver_.reloadStyle(mr.style, midi::sffPartLayout());   // SFF1 part channels 9-16
        styleLabel(styleLabel_, "Style: " + juce::String(f.getFileNameWithoutExtension()));
    });
}

void LiveHostComponent::timerCallback() {
    driver_.midiIn().pollReconnect();   // re-attach the selected input after a USB replug
    const auto s = driver_.facade().snapshot();

    chordLabel_.setText(formatChord(s), juce::dontSendNotification);
    sectionLabel_.setText("Section: " + juce::String(s.section) +
                          (s.playing ? "  (playing)" : "  (stopped)"),
                          juce::dontSendNotification);
    tempoLabel_.setText("Tempo " + juce::String(s.tempoBpm), juce::dontSendNotification);
    if (tempoModel_.syncFromEngine(s.tempoBpm))
        tempoSlider_.setValue(tempoModel_.displayed(), juce::dontSendNotification);

    // Connection / error status. Transport must NOT stop on device loss; we only
    // surface the state (DOCTRINE 06).
    // A failed CoreMIDI init is not "no device": say so, and how to recover (#23).
    if (!driver_.midiReady()) {
        juce::String which = !driver_.midiInReady() && !driver_.midiOutReady() ? "in+out"
                           : !driver_.midiInReady() ? "in" : "out";
        statusLabel_.setText("MIDI " + which + " unavailable: CoreMIDI init failed. "
                             "Press Refresh to retry; if it persists, restart the app.",
                             juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, juce::Colours::red);
    } else {
        juce::String status;
        status << "In: "  << (s.midiInLive  ? "connected" : "none")
               << "   Out: " << (s.midiOutLive ? "connected" : "none")
               << "   rx: " << juce::String(s.receivedMessages)
               << "   tx: " << juce::String(s.dispatchedNotes);
        statusLabel_.setText(status, juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId,
                               s.midiOutLive ? juce::Colours::lightgreen
                                             : juce::Colours::orange);
    }

    startBtn_.setColour(juce::TextButton::buttonColourId,
                        s.playing ? juce::Colours::darkgreen
                                  : juce::Colours::darkgrey);
}

void LiveHostComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff1e1e1e));
}

void LiveHostComponent::resized() {
    auto area = getLocalBounds().reduced(12);
    titleLabel_.setBounds(area.removeFromTop(30));
    area.removeFromTop(6);

    // Device row.
    auto devRow = area.removeFromTop(26);
    inLabel_.setBounds(devRow.removeFromLeft(60));
    inputBox_.setBounds(devRow.removeFromLeft(230));
    devRow.removeFromLeft(12);
    outLabel_.setBounds(devRow.removeFromLeft(64));
    outputBox_.setBounds(devRow.removeFromLeft(230));
    area.removeFromTop(6);

    auto styleRow = area.removeFromTop(26);
    loadBtn_.setBounds(styleRow.removeFromLeft(120));
    styleRow.removeFromLeft(8);
    refreshBtn_.setBounds(styleRow.removeFromLeft(90));
    styleRow.removeFromLeft(8);
    resourcesBtn_.setBounds(styleRow.removeFromRight(110));
    styleRow.removeFromLeft(4);
    styleLabel_.setBounds(styleRow);
    area.removeFromTop(10);

    chordLabel_.setBounds(area.removeFromTop(48));
    sectionLabel_.setBounds(area.removeFromTop(24));
    area.removeFromTop(8);

    auto tempoRow = area.removeFromTop(28);
    tempoLabel_.setBounds(tempoRow.removeFromLeft(100));
    tempoSlider_.setBounds(tempoRow);
    area.removeFromTop(10);

    auto transportRow = area.removeFromTop(34);
    for (auto* b : {&startBtn_, &stopBtn_, &syncBtn_}) {
        b->setBounds(transportRow.removeFromLeft(110));
        transportRow.removeFromLeft(6);
    }
    area.removeFromTop(8);

    auto sectionRow = area.removeFromTop(34);
    introBtn_.setBounds(sectionRow.removeFromLeft(70));
    sectionRow.removeFromLeft(6);
    for (auto* b : {&varA_, &varB_, &varC_, &varD_}) {
        b->setBounds(sectionRow.removeFromLeft(44));
        sectionRow.removeFromLeft(4);
    }
    sectionRow.removeFromLeft(6);
    fillBtn_.setBounds(sectionRow.removeFromLeft(64));
    sectionRow.removeFromLeft(6);
    breakBtn_.setBounds(sectionRow.removeFromLeft(64));
    sectionRow.removeFromLeft(6);
    endingBtn_.setBounds(sectionRow.removeFromLeft(72));
    sectionRow.removeFromLeft(10);
    autoFillBtn_.setBounds(sectionRow.removeFromLeft(100));
    area.removeFromTop(10);

    auto bottom = area.removeFromTop(34);
    panicBtn_.setBounds(bottom.removeFromLeft(140));
    statusLabel_.setBounds(bottom);
}

} // namespace ai_arranger::app
