#pragma once

// "Convert Sound" tab: WAV folder -> layers and velocity splits -> key/velocity map preview, checks
// and Sound Recipe -> package folder (SF2, TX16Wx, WAVs, REPORT, RESOURCES.tsv). A view over
// ui::SoundConversionModel; conversion runs off the message thread. Reads the source, writes one new
// local folder as an export job (JobPort), never talks to an instrument.

#include <juce_gui_extra/juce_gui_extra.h>
#include "JobPort.h"
#include "TextViews.h"
#include "ui/sound/sound_conversion_model.h"
#include <array>

namespace ai_arranger::app {

// Keys across, one row per velocity layer: every zone as a box with its root marked.
class KeyMapView : public juce::Component {
public:
    void setInstrument(const media::sound::Instrument* in) { in_ = in; repaint(); }
    void setEmptyText(const juce::String& t) { empty_ = t; repaint(); }   // shown before there is a map
    juce::String emptyText() const { return empty_; }
    void paint(juce::Graphics& g) override;
private:
    const media::sound::Instrument* in_ = nullptr;
    juce::String empty_ = "Choose a KRONOS or WAV folder and press Preview to see the key and velocity map.";
};

class SoundConversionComponent : public juce::Component {
public:
    explicit SoundConversionComponent(std::function<void(const juce::File&)> openInResources = {});
    ~SoundConversionComponent() override;
    void resized() override;

    // Used by the buttons and by the UI smoke test.
    bool inspectFolder(const juce::File& dir);
    void buildPreview();                      // asynchronous (reads every WAV)
    void generate(const juce::File& outDir);   // an export job; busy() until it lands
    void setJobPort(JobPort p) { port_ = std::move(p); }   // the studio's queue; else this tab's own
    void cancelJobs() { ownJobs_.cancelAll(); }            // app quit: this tab's own jobs (a cancelled job publishes nothing)
    const jobs::JobState& lastJob() const { return lastJob_; }   // the last Generate job that landed (tests: thread, result)

    bool busy() const { return jobs_ > 0; }
    ui::SoundConversionModel& model() { return model_; }
    const std::string& ksfNotes() const { return ksfNotes_; }   // for a Sound package export job (Import / Export Center)
    juce::TextEditor& nameField() { return name_; }
    juce::TextEditor& orderField() { return order_; }
    juce::TextEditor& splitsField() { return splits_; }
    EditView& mappingField() { return mapping_; }   // KSF-only folders: proposed key map, editable
    EditView& zoneMapField() { return zoneMap_; }   // P1, every source: zone map + loops (samplelab/zone_map.h), editable
    juce::String previewText() const { return preview_.getText(); }
    juce::String recipeText() const { return recipe_.getText(); }
    juce::String recipeShownText() const { return recipe_.shownText(); }
    juce::String statusText() const { return status_.getText(); }

private:
    ui::SoundConversionModel model_;
    std::function<void(const juce::File&)> openInResources_;
    juce::TextButton folderBtn_{"Choose Folder..."}, previewBtn_{"Preview"}, generateBtn_{"Generate Package..."};
    juce::Label folder_, nameLbl_{{}, "Sound name"}, orderLbl_{{}, "Layers, softest first"}, splitsLbl_{{}, "Velocity splits"}, status_, pluginNote_;
    juce::TextEditor name_, order_, splits_;
    EditView mapping_, zoneMap_;
    TextView preview_, recipe_;
    KeyMapView map_;
    std::shared_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::string ksfNotes_;   // KSF folder: REPORT source section measured on the previewed audio
    int jobs_ = 0;
    uint64_t generation_ = 0;
    PendingJobs genJobs_;
    jobs::JobState lastJob_;
    void showPreview(media::sound::Instrument* in, const std::array<std::string, 2>& texts, const std::string& err, const std::string& mapping,
                     const std::string& ksfNotes, const std::string& zoneMap);

    JobRunner ownJobs_;   // destroyed first: its jobs are cancelled (never published half-way), its done callbacks dropped
    JobPort port_ = ownJobs_.port();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SoundConversionComponent)
};

} // namespace ai_arranger::app
