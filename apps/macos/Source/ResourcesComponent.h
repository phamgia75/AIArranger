#pragma once

// Resources window: Resource Graph browser, KST Library (file-level) and Memory Check.
// A JUCE view over ui::ResourceBrowserModel. Inventory, Compare and Merge Plan only read. The one KST write
// is Backup KST: a verified copy into a NEW local folder (media/kst_backup.h); the source is only read.
// Never sends MIDI and never talks to the instrument.

#include <juce_gui_extra/juce_gui_extra.h>
#include "media/kst_backup.h"
#include <thread>
#include "ui/resources/resource_browser_model.h"
#include "SoundConversionComponent.h"
#include "PluginSamplerComponent.h"

namespace ai_arranger::app {

class ResourcesComponent : public juce::Component {
public:
    ResourcesComponent();
    ~ResourcesComponent() override;
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Used by the buttons and by the UI smoke test.
    void openPackage(const juce::File& dir);   // asynchronous; loading() is true until it lands
    void verifyPackage(const juce::File& dir); // asynchronous, read-only; loading() until it lands; result in detailsText()
    bool loading() const { return loads_ > 0; }
    void openEvidence(const juce::File& file);   // asynchronous, like openPackage
    void showDetails(const media::graph::ResourceId& id);
    void runKst(ui::ResourceBrowserModel::KstAction a, const juce::String& first, const juce::String& second = {});
    void compareMemory();
    void startBackup(const juce::File& source, const juce::File& newFolder);   // asynchronous; backingUp() until done
    void cancelBackup() { cancelBackup_ = true; }   // also stops a Verify Backup
    void startVerify(const juce::File& backupFolder);   // asynchronous, read-only; backingUp() until done
    bool backingUp() const { return backingUp_; }
    double backupProgress() const { return backupProgress_; }
    static juce::File backupFolderFor(const juce::File& source, const juce::File& parent);   // "<name> backup <date time>"

    ui::ResourceBrowserModel& model() { return model_; }
    juce::TreeView& tree() { return tree_; }
    juce::String detailsText() const { return details_.getText(); }
    juce::String summaryText() const { return summary_.getText(); }
    juce::String kstText() const { return kstOut_.getText(); }
    juce::String memoryText() const { return memOut_.getText(); }
    juce::TextEditor& memoryBefore() { return memBefore_; }
    juce::TextEditor& memoryAfter() { return memAfter_; }
    juce::TabbedComponent& tabs() { return tabs_; }
    SoundConversionComponent& converter() { return convert_; }
    PluginSamplerComponent& sampler() { return sampler_; }

private:
    void rebuildTree();
    void chooseFolder(const juce::String& title, std::function<void(juce::File)> then);
    void chooseFile(const juce::String& title, std::function<void(juce::File)> then);
    static void styleText(juce::TextEditor& t, bool readOnly);

    ui::ResourceBrowserModel model_;

    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
    juce::Component graphPage_, kstPage_, memPage_;

    // Resource Graph
    juce::TextButton openPkgBtn_{"Open Package..."}, verifyPkgBtn_{"Verify Package..."}, openEvBtn_{"Open Evidence..."}, clearBtn_{"Close All"};
    juce::Label legend_;
    juce::TreeView tree_;
    std::unique_ptr<juce::TreeViewItem> rootItem_;
    juce::TextEditor details_, summary_;

    // KST Library
    juce::TextButton invBtn_{"Inventory..."}, diffBtn_{"Compare Folders..."}, planBtn_{"Merge Plan..."}, semanticBtn_{"Semantic Merge"};
    juce::TextButton backupBtn_{"Backup KST..."}, verifyBtn_{"Verify Backup..."}, cancelBackupBtn_{"Cancel Backup"};
    double backupProgress_ = 0;   // message thread only
    juce::ProgressBar backupBar_{backupProgress_};
    bool backingUp_ = false;      // message thread only
    std::atomic<bool> cancelBackup_{false};
    std::thread backupThread_;    // joined in the destructor (cancelled first), never detached
    juce::Label kstNote_;
    juce::TextEditor kstOut_;
    std::atomic<int> kstJobs_{0};
    int loads_ = 0;   // message thread only

    // Memory Check
    juce::Label memNote_, beforeLbl_{{}, "Before import (Memory Info)"}, afterLbl_{{}, "After import (Memory Info)"};
    juce::TextEditor memBefore_, memAfter_, memOut_;
    juce::TextButton compareBtn_{"Compare"};

    // Convert Sound: a finished package opens in the Resource Graph tab.
    SoundConversionComponent convert_{[this](const juce::File& pkg) { tabs_.setCurrentTabIndex(0); openPackage(pkg); }};
    PluginSamplerComponent sampler_{[this](const juce::File& pkg) { tabs_.setCurrentTabIndex(0); openPackage(pkg); }};

    std::shared_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ResourcesComponent)
};

} // namespace ai_arranger::app
