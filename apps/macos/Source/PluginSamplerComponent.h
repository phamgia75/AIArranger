#pragma once

// "Sample Plugin" tab: an AU/VST instrument the user is licensed for -> offline multisample render
// -> key/velocity map, checks and report -> package folder -> Resource Graph.
// Rendering runs in the separate aiarranger-plugin-sampler process (docs/research/
// PLUGIN_SAMPLER_GUI_PLAN.md): a plugin that crashes or hangs takes down only that process, and it
// opens no audio device and no MIDI port. The render lands in a private staging folder; Generate
// copies it to a new local folder as an export job (never a mounted volume, never an existing folder).

#include "SoundConversionComponent.h"
#include "ui/sound/plugin_sampler_model.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace ai_arranger::app {

// Runs the sampler once and streams its output lines. Owned by the message thread; the reader
// thread posts lines back with callAsync and holds only the shared State, never `this`.
// Cancel and destruction are bounded: a helper the plugin started can keep the output pipe open after
// the sampler is killed, so if the reader has not finished kCancelGraceMs after cancel(), onDone(-1, true)
// is delivered anyway; the destructor waits at most kCloseGraceMs, then detaches the reader instead of joining it.
class SamplerProcess {
public:
    using LineFn = std::function<void(const std::string&)>;
    using DoneFn = std::function<void(int exitCode, bool cancelled)>;
    static constexpr int kCancelGraceMs = 2000, kCloseGraceMs = 500;
    ~SamplerProcess();
    bool start(const juce::File& exe, const std::vector<std::string>& args, LineFn onLine, DoneFn onDone);
    bool cancel();   // true when a kill signal was sent; never signals a child that was already reaped (its PID may be reused)
    bool running() const { return s_->started && !s_->finished && !s_->doneSent; }
private:
    struct State {
        juce::ChildProcess proc;
        std::mutex m;          // orders kill() against the reaping waitpid()
        bool reaped = false;   // guarded by m
        std::atomic<bool> started{false}, cancelled{false}, finished{false}, doneSent{false};
    };
    static void reap(State& st);
    std::shared_ptr<State> s_ = std::make_shared<State>();
    std::thread reader_;
    DoneFn onDone_;
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
    void generate(const juce::File& outDir);    // an export job copying the staged package
    void setJobPort(JobPort p) { port_ = std::move(p); }   // the studio's queue; else this tab's own

    bool busy() const { return jobs_ > 0; }
    bool rendered() const { return in_ != nullptr; }
    const media::sound::Instrument* instrument() const { return in_.get(); }
    juce::File stagingFolder() const { return staging_; }
    juce::StringArray instrumentNames() const;
    juce::TextEditor& field(const juce::String& label);   // by label text, for tests
    juce::ToggleButton& loopToggle() { return loop_; }
    juce::ComboBox& programChoice() { return program_; }   // fills the (hidden) Program field: "" or the index
    juce::String estimateText() const { return estimate_.getText(); }
    const juce::Label& estimateLabel() const { return estimate_; }
    const juce::Label& statusLabel() const { return status_; }
    const KeyMapView& keyMap() const { return map_; }
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
    juce::ComboBox plugin_, program_;
    juce::ToggleButton loop_{"Loop the held part"}, stereo_{"Stereo"};
    juce::OwnedArray<juce::Label> labels_;
    juce::OwnedArray<juce::TextEditor> fields_;
    juce::Label estimate_, status_, licence_;
    double progress_ = 0;
    juce::ProgressBar bar_{progress_};
    KeyMapView map_;
    TextView preview_, report_;
    std::shared_ptr<juce::FileChooser> chooser_;
    void findPrograms();
    std::unique_ptr<SamplerProcess> proc_, listProc_, progProc_;   // destroyed first in ~PluginSamplerComponent: their callbacks capture this
    ui::SamplerProgress got_;
    ui::SamplerSettings renderedWith_;
    juce::File staging_;
    std::shared_ptr<const media::sound::Instrument> in_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    int jobs_ = 0;
    uint64_t generation_ = 0;
    PendingJob genJob_;
    JobRunner ownJobs_;   // the destructor waits for its Generate copy before staging is deleted (a package is never half-copied)
    JobPort port_ = ownJobs_.port();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginSamplerComponent)
};

} // namespace ai_arranger::app
