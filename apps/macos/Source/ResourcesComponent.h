#pragma once

// Resources window: Resource Graph browser, KST Library (file-level) and Memory Check.
// A JUCE view over ui::ResourceBrowserModel. Inventory, Compare and Merge Plan only read. The one KST write
// is Backup KST: a verified copy into a NEW local folder (media/kst_backup.h), run as an export job (JobPort:
// D17 never delete, D18 never inside the source); the source is only read.
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
    void cancelBackup();   // the backup job, or a Verify Backup
    void setJobPort(JobPort p);   // the studio's queue for Backup KST, Convert Sound and Sample Plugin; else their own
    // The one list of this window's work (the standalone window's quit and the Studio's busy() both ask it): a backup or check,
    // a load, a KST Inventory / Compare Folders / Merge Plan scan, Convert Sound, Sample Plugin (render or Generate).
    bool busy() const { return backingUp_ || loads_ > 0 || kstScanning() || convert_.busy() || sampler_.busy(); }
    // Cancels all of it except a KST scan: it is read-only and has no stop point, so quit waits for it (it changes nothing).
    void cancelJobs() { cancelBackup(); ownJobs_.cancelAll(); convert_.cancelJobs(); sampler_.cancelJobs(); }
    bool kstScanning() const { return kstJobs_ > 0; }
    const jobs::JobState& lastBackupJob() const { return lastBackup_; }   // the last Backup KST job that landed (tests)
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
    const SoundConversionComponent& converter() const { return convert_; }
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
    std::thread backupThread_;    // Verify Backup: joined in the destructor (cancelled first), never detached
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
    void pollBackup();      // the progress bar follows the backup job's state
    PendingJobs backupJobs_;   // at most one (startBackup refuses while backingUp_), counted the same way as Generate
    jobs::JobState lastBackup_;
    uint64_t backupId_ = 0;    // the running backup job, for Cancel Backup and the progress bar; 0 once it has landed
    JobRunner ownJobs_;     // destroyed first: closing the window cancels a backup (only NAME.INCOMPLETE stays, D17)
    JobPort port_ = ownJobs_.port();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ResourcesComponent)
};

} // namespace ai_arranger::app
