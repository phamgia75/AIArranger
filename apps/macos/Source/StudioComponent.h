#pragma once

// AI Music Studio window (UI spec rev 2, E2 first slice): sidebar | content with a segmented control at the top |
// inspector. An integrating shell over the existing views, not a rewrite: one ResourcesComponent (Resource Graph,
// KST Library, Memory Check, Convert Sound, Sample Plugin) is the shared project context; its tab bar is hidden and
// the sidebar plus segments choose its page. New here: source recognition with the read-only library view, the
// Import/Export routes, the Style report, Validation & Reports with L1/L2/L3 badges (ui/studio/studio_models.h).
// Sources are only read. Every write stays in the existing writers (new local folder, NAME.INCOMPLETE, output
// guard). Every unavailable control shows its reason next to it. Never sends MIDI.

#include <juce_gui_extra/juce_gui_extra.h>
#include "ResourcesComponent.h"
#include "ui/studio/studio_models.h"

namespace ai_arranger::app {

class StudioComponent : public juce::Component, private juce::ChangeListener {
public:
    enum class Domain { Library, ImportExport, SampleLab, StyleStudio, Validation };
    static constexpr int kDomains = 5;
    static const char* domainName(Domain d);

    explicit StudioComponent(std::function<void()> showLiveHost = {});
    ~StudioComponent() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& k) override;   // ⌘1…⌘5 jump to a domain

    void showDomain(Domain d, int segment = 0);
    Domain domain() const { return domain_; }
    int segment() const { return segment_; }
    juce::StringArray segmentNames(Domain d) const;

    // What the buttons do; also driven by the UI smoke test.
    void openSource(const juce::File& dir);       // asynchronous, read-only probe; busy() until it lands
    void openStyle(const juce::File& sty);        // read-only SFF1 report
    void setValidationTarget(const juce::File& f);
    enum class Validator { VerifyPackage, KstInventory, Sf2Check, SoundPy, DrumKitPy };
    void runValidator(Validator v);               // asynchronous; busy() until it lands
    void attachL3Record(const juce::File& record);
    juce::String validatorUnavailable(Validator v) const;   // "" when it can run now
    bool busy() const { return jobs_ > 0; }

    ResourcesComponent& resources() { return res_; }
    const ui::studio::Verification& verification() const { return verification_; }
    juce::String inspectorText() const { return inspector_.getText(); }
    juce::String libraryText() const { return libraryView_.getText(); }
    juce::String styleText() const { return styleReport_.getText(); }
    juce::String jobsText() const { return jobsLog_.getText(); }
    juce::String badgeText() const { return badges_.getText(); }
    juce::String qualityText() const { return quality_.getText(); }
    juce::Component& sidebar() { return sidebar_; }
    juce::Component& page(Domain d);

private:
    struct Segment { juce::String name; int resTab = -1; juce::Component* page = nullptr; juce::String unavailable; };
    // A page laid out as rows of fixed-width items; the last item of a row with width 0 takes the rest of the row;
    // `fill` takes the space left below the rows.
    struct FlowPage : juce::Component {
        std::vector<std::vector<std::pair<juce::Component*, int>>> rows;
        juce::Component* fill = nullptr;
        void resized() override;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;   // a ResourcesComponent hand-off switched its tab
    void refreshInspector();
    void refreshAvailability();
    void addJob(const juce::String& line);
    void runJob(const juce::String& name, std::function<std::pair<juce::String, std::optional<ui::studio::Verification>>()> work);
    void chooseFolder(const juce::String& title, std::function<void(juce::File)> then, bool files = false, const juce::String& pattern = {});
    juce::TextButton& button(const juce::String& text, std::function<void()> onClick);
    juce::Label& label(const juce::String& text, juce::Colour c);
    juce::Label& reasonFor(juce::Button& b);
    void setUnavailable(juce::Button& b, const juce::String& reason);   // "" = available
    static void styleText(juce::TextEditor& t);

    std::function<void()> showLiveHost_;
    ResourcesComponent res_;
    juce::OwnedArray<juce::Component> owned_;
    std::map<juce::Button*, juce::Label*> reasons_;
    juce::TooltipWindow tips_{this, 400};

    juce::Component sidebar_, segmentBar_;
    std::vector<juce::TextButton*> domainButtons_;
    juce::TextButton* liveBtn_ = nullptr;
    std::map<Domain, std::vector<juce::TextButton*>> segmentButtons_;
    juce::Label title_, experimental_;
    juce::TextEditor inspector_;

    Domain domain_ = Domain::Library;
    int segment_ = 0;
    int lastTab_ = -1;   // the ResourcesComponent tab the studio itself chose last
    std::map<Domain, std::vector<Segment>> segments_;

    // Library Workspace › Sources
    FlowPage sourcesPage_;
    juce::TextEditor libraryView_;
    juce::TextButton* toSampleLab_ = nullptr;
    std::optional<ui::studio::SourceProbe> source_;
    // Import / Export Center
    FlowPage ioPage_;
    // Sample Lab › Quality Report
    FlowPage qualityPage_;
    juce::TextEditor quality_;
    // Style Studio
    FlowPage stylePage_;
    juce::TextEditor styleReport_;
    juce::TextButton* liveFromStyle_ = nullptr;
    // Validation & Reports
    FlowPage validationPage_;
    juce::Label target_, badges_;
    juce::TextEditor jobsLog_;
    juce::File validationTarget_;
    std::map<Validator, juce::TextButton*> validatorButtons_;
    juce::TextButton *useSourceBtn_ = nullptr, *l3Btn_ = nullptr;
    ui::studio::Verification verification_;
    bool verificationIsPackage_ = false;

    int jobs_ = 0;   // message thread only
    std::shared_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StudioComponent)
};

}  // namespace ai_arranger::app
