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
// Read-only text that lays out only the visible lines: juce::TextEditor::setText re-lays out all of
// it on the message thread (about 200 ms for the 5 KB mapping table, 600 ms on CI runners).
class TextView : public juce::Component {
public:
    TextView() { ed_.setReadOnly(true); ed_.setLineNumbersShown(false); addAndMakeVisible(ed_); }
    void setText(const juce::String& t) { raw_ = t; cols_ = -1; layout(); }
    juce::String getText() const { return raw_; }
    juce::String shownText() const { return doc_.getAllContent(); }
    bool canUndo() { return doc_.getUndoManager().canUndo(); }
    void clear() { setText({}); }
    void setFont(const juce::Font& f) { ed_.setFont(f); }
    void setWrap(bool w) { wrap_ = w; }   // prose: wrap to the width instead of scrolling sideways
    void resized() override { ed_.setBounds(getLocalBounds()); layout(); }
private:
    void layout() {
        const int cols = wrap_ ? std::max(20, int((getWidth() - 24) / std::max(1.0f, ed_.getCharWidth()))) : 0;
        if (cols == cols_) return;
        cols_ = cols;
        doc_.replaceAllContent(cols ? juce::String(ui::wrapLines(raw_.toStdString(), size_t(cols))) : raw_);
        doc_.clearUndoHistory();   // read-only view: no undo, and old texts must not pile up
        ed_.scrollToLine(0);
    }
    juce::String raw_;
    bool wrap_ = false;
    int cols_ = -1;
    juce::CodeDocument doc_;
    juce::CodeEditorComponent ed_{doc_, nullptr};
};

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
    void generate(const juce::File& outDir);   // asynchronous; busy() until it lands

    bool busy() const { return jobs_ > 0; }
    ui::SoundConversionModel& model() { return model_; }
    juce::TextEditor& nameField() { return name_; }
    juce::TextEditor& orderField() { return order_; }
    juce::TextEditor& splitsField() { return splits_; }
    juce::TextEditor& mappingField() { return mapping_; }   // KSF-only folders: proposed key map, editable
    juce::String previewText() const { return preview_.getText(); }
    juce::String recipeText() const { return recipe_.getText(); }
    juce::String recipeShownText() const { return recipe_.shownText(); }
    juce::String statusText() const { return status_.getText(); }

private:
    ui::SoundConversionModel model_;
    std::function<void(const juce::File&)> openInResources_;
    juce::TextButton folderBtn_{"Choose Folder..."}, previewBtn_{"Preview"}, generateBtn_{"Generate Package..."};
    juce::Label folder_, nameLbl_{{}, "Sound name"}, orderLbl_{{}, "Layers, softest first"}, splitsLbl_{{}, "Velocity splits"}, status_, pluginNote_;
    juce::TextEditor name_, order_, splits_, mapping_;
    TextView preview_, recipe_;
    KeyMapView map_;
    std::shared_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::string ksfNotes_;   // KSF folder: REPORT source section measured on the previewed audio
    int jobs_ = 0;
    uint64_t generation_ = 0;
    void showPreview(media::sound::Instrument* in, const std::array<std::string, 2>& texts, const std::string& err, const std::string& mapping,
                     const std::string& ksfNotes);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SoundConversionComponent)
};

} // namespace ai_arranger::app
