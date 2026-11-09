#pragma once

// "Convert Sound" tab: WAV folder -> layers and velocity splits -> key/velocity map preview, checks
// and Sound Recipe -> package folder (SF2, TX16Wx, WAVs, REPORT, RESOURCES.tsv). A view over
// ui::SoundConversionModel; conversion runs off the message thread. Reads the source, writes one new
// local folder, never talks to an instrument.

#include <juce_gui_extra/juce_gui_extra.h>
#include "ui/sound/sound_conversion_model.h"
#include <array>

namespace ai_arranger::app {

// Keys across, one row per velocity layer: every zone as a box with its root marked.
class KeyMapView : public juce::Component {
public:
    void setInstrument(const media::sound::Instrument* in) { in_ = in; repaint(); }
    void paint(juce::Graphics& g) override;
private:
    const media::sound::Instrument* in_ = nullptr;
};

class SoundConversionComponent : public juce::Component {
public:
    explicit SoundConversionComponent(std::function<void(const juce::File&)> openInResources = {});
    ~SoundConversionComponent() override;
    void resized() override;

    // Used by the buttons and by the UI smoke test.
    bool inspectFolder(const juce::File& dir);
    void buildPreview();                      // asynchronous (reads every WAV)
    void generate(const juce::File& outDir);   // asynchronous; busy() until it lands

    bool busy() const { return jobs_ > 0; }
    ui::SoundConversionModel& model() { return model_; }
    juce::TextEditor& nameField() { return name_; }
    juce::TextEditor& orderField() { return order_; }
    juce::TextEditor& splitsField() { return splits_; }
    juce::String previewText() const { return preview_.getText(); }
    juce::String recipeText() const { return recipe_.getText(); }
    juce::String statusText() const { return status_.getText(); }

private:
    ui::SoundConversionModel model_;
    std::function<void(const juce::File&)> openInResources_;
    juce::TextButton folderBtn_{"Choose WAV Folder..."}, previewBtn_{"Preview"}, generateBtn_{"Generate Package..."};
    juce::Label folder_, nameLbl_{{}, "Sound name"}, orderLbl_{{}, "Layers, softest first"}, splitsLbl_{{}, "Velocity splits"}, status_, pluginNote_;
    juce::TextEditor name_, order_, splits_, preview_, recipe_;
    KeyMapView map_;
    std::shared_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    int jobs_ = 0;
    uint64_t generation_ = 0;
    void showPreview(media::sound::Instrument* in, const std::array<std::string, 2>& texts, const std::string& err);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SoundConversionComponent)
};

} // namespace ai_arranger::app
