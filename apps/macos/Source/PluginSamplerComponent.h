#pragma once

// "Sample Plugin" tab: an AU/VST instrument the user is licensed for -> offline multisample render
// -> key/velocity map, checks and report -> package folder -> Resource Graph.
// Rendering runs in the separate aiarranger-plugin-sampler process (docs/research/
// PLUGIN_SAMPLER_GUI_PLAN.md): a plugin that crashes or hangs takes down only that process, and it
// opens no audio device and no MIDI port. The render lands in a private staging folder; Generate
// copies it to a new local folder (never a mounted volume, never an existing folder).

#include "SoundConversionComponent.h"
#include "ui/sound/plugin_sampler_model.h"
#include <atomic>
#include <thread>

namespace ai_arranger::app {

// Runs the sampler once and streams its output lines. Owned by the message thread; the reader
// thread posts lines back with callAsync. Destroying it kills the process and joins the thread.
class SamplerProcess {
public:
    using LineFn = std::function<void(const std::string&)>;
    using DoneFn = std::function<void(int exitCode, bool cancelled)>;
    ~SamplerProcess() { cancel(); if (reader_.joinable()) reader_.join(); }
    bool start(const juce::File& exe, const std::vector<std::string>& args, LineFn onLine, DoneFn onDone);
    void cancel() { cancelled_ = true; proc_.kill(); }
    bool running() const { return running_; }
private:
    juce::ChildProcess proc_;
    std::thread reader_;
    std::atomic<bool> cancelled_{false}, running_{false};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

class PluginSamplerComponent : public juce::Component {
public:
    explicit PluginSamplerComponent(std::function<void(const juce::File&)> openInResources);
    ~PluginSamplerComponent() override;
    void resized() override;

    // Used by the buttons and by the UI smoke test.
    void setSamplerExecutable(const juce::File& exe) { exe_ = exe; }
    juce::File samplerExecutable() const { return exe_; }
    void findInstruments();                     // asynchronous: `sampler list`
    bool selectInstrument(const juce::String& name);
    void startRender();                         // asynchronous; busy() until it ends
    void cancelRender();
    void generate(const juce::File& outDir);    // asynchronous copy of the staged package

    bool busy() const { return jobs_ > 0; }
    bool rendered() const { return in_ != nullptr; }
    const media::sound::Instrument* instrument() const { return in_.get(); }
    juce::File stagingFolder() const { return staging_; }
    juce::StringArray instrumentNames() const;
    juce::TextEditor& field(const juce::String& label);   // by label text, for tests
    juce::ToggleButton& loopToggle() { return loop_; }
    juce::String estimateText() const { return estimate_.getText(); }
    juce::String statusText() const { return status_.getText(); }
    juce::String previewText() const { return preview_.getText(); }
    juce::String reportText() const { return report_.getText(); }
    double progress() const { return progress_; }

    static juce::File locateSampler();          // $AIARR_PLUGIN_SAMPLER, the app bundle, or the build tree

private:
    ui::SamplerSettings settings() const;
    void settingsChanged();
    void discardStaging();
    void finishRender(int exitCode, bool cancelled, uint64_t gen);

    std::function<void(const juce::File&)> openInResources_;
    juce::File exe_ = locateSampler();
    std::vector<ui::PluginEntry> plugins_;
    juce::TextButton findBtn_{"Find Instruments"}, renderBtn_{"Render Samples"}, cancelBtn_{"Cancel"}, generateBtn_{"Generate Package..."};
    juce::ComboBox plugin_;
    juce::ToggleButton loop_{"Loop the held part"}, stereo_{"Stereo"};
    juce::OwnedArray<juce::Label> labels_;
    juce::OwnedArray<juce::TextEditor> fields_;
    juce::Label estimate_, status_, licence_;
    double progress_ = 0;
    juce::ProgressBar bar_{progress_};
    KeyMapView map_;
    TextView preview_, report_;
    std::shared_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<SamplerProcess> proc_, listProc_;   // destroyed first in ~PluginSamplerComponent: their callbacks capture this
    ui::SamplerProgress got_;
    ui::SamplerSettings renderedWith_;
    juce::File staging_;
    std::shared_ptr<const media::sound::Instrument> in_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    int jobs_ = 0;
    uint64_t generation_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginSamplerComponent)
};

} // namespace ai_arranger::app
