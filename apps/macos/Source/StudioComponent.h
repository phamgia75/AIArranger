#pragma once

// AI Music Studio window (UI spec rev 2, E2 first slice): sidebar | content with a segmented control at the top |
// inspector. An integrating shell over the existing views, not a rewrite: one ResourcesComponent (Resource Graph,
// KST Library, Memory Check, Convert Sound, Sample Plugin) is the shared project context; its tab bar is hidden and
// the sidebar plus segments choose its page. New here: source recognition with the read-only library view, the
// Import/Export routes, the Style report, Validation & Reports with L1/L2/L3 badges (ui/studio/studio_models.h).
// Phase 2 (ui/studio/studio_tools.h, style_studio_model.h): Sample Lab > KMP / KSF Writer (EXPERIMENTAL, L1) and Tools
// (E4), Style Studio editing with undo/redo and SMF import/export (E3), Pa-native SET inspect (E5, read-only), and the
// real level of every new output in Validation & Reports. Jobs run on worker threads, never the audio thread.
// Sources are only read. Every write stays in the existing writers (new local folder, NAME.INCOMPLETE, output
// guard). Every unavailable control shows its reason next to it. Never sends MIDI.

#include <juce_gui_extra/juce_gui_extra.h>
#include "LibraryManagerComponent.h"
#include "ResourcesComponent.h"
#include "jobs/job_queue.h"
#include "ui/studio/studio_models.h"
#include "ui/studio/studio_tools.h"
#include "ui/studio/style_studio_model.h"

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
    enum class Validator { VerifyPackage, KstInventory, Sf2Check, StyleSmf, SoundPy, DrumKitPy };
    void runValidator(Validator v);               // asynchronous; busy() until it lands
    void attachL3Record(const juce::File& record);
    juce::String validatorUnavailable(Validator v) const;   // "" when it can run now
    bool busy() const {   // a finished export job counts until it has landed on its page (message thread); also the Library Manager and
                          // all Sounds & Resources work (its own busy() is the one list)
        return jobs_ > 0 || !queue_->idle() || std::any_of(jobHomes_.begin(), jobHomes_.end(), [](const auto& h) { return !h.second.landed; }) ||
               manager_.busy() || manager_.computing() || res_.busy() || (otherBusy_ && otherBusy_());
    }

    // Phase 2, also driven by the UI smoke test. Every write is a NEW local folder or file; jobs are asynchronous.
    void inspectPaNative();                       // Library > Sources: read-only item list of an open SET
    juce::String paNativeUnavailable() const;
    void writeKmp(const juce::File& dest);        // Sample Lab > KMP / KSF Writer: the previewed Sound (EXPERIMENTAL, L1)
    juce::String kmpUnavailable();
    enum class LabTool { Analyze, Slice, Loop, BatchPreview, Batch };
    void setLabInput(const juce::File& wavOrFolder);
    void runLabTool(LabTool t, const juce::File& dest = {});
    juce::String labUnavailable(LabTool t) const;
    juce::TextEditor& labBars() { return labBars_; }
    juce::TextEditor& labStart() { return labStart_; }
    juce::TextEditor& labEnd() { return labEnd_; }
    juce::ToggleButton& labTrim() { return labTrim_; }
    juce::ToggleButton& labNormalize() { return labNorm_; }
    // Style Studio editing (E3): element/track browser, ops, undo/redo, SMF import/export
    juce::ComboBox& styleElement() { return styleElem_; }
    juce::ComboBox& styleTrack() { return styleTrack_; }
    juce::ComboBox& styleOp() { return styleOp_; }
    juce::TextEditor& styleValue() { return styleValue_; }
    void applyStyleOp();
    // Opening another Style while the open one has unwritten edits asks first (inline, on the Style page): Save UASF...
    // (then open), Discard (then open) or Cancel (the open Style stays as it is, undo included).
    enum class UnsavedChoice { Save, Discard, Cancel };
    void resolveUnsavedStyle(UnsavedChoice c, const juce::File& saveAs = {});
    bool unsavedPromptShown() const { return pendingStyle_ != juce::File() || pendingQuit_ != nullptr; }
    // The app's quit request: with unwritten Style edits it shows the same choice on the Style page and returns false
    // (quit runs later, after Save or Discard; Cancel aborts it). Clean: quit runs now and it returns true.
    bool requestQuit(std::function<void()> quit);
    // Quit while work runs (export jobs running or queued, a read or check, a Library Manager scan or report): the Import /
    // Export page asks. Wait quits once everything has finished; Cancel Jobs and Quit cancels every job (a cancelled job
    // leaves only NAME.INCOMPLETE, D17) and stops the library work, then quits once all has stopped; Keep Working aborts.
    enum class WorkChoice { Wait, CancelJobsAndQuit, KeepWorking };
    void resolveRunningWork(WorkChoice c);
    bool workPromptShown() const { return pendingWorkQuit_ != nullptr; }
    // Work outside this window that the quit guard also waits for or cancels (the standalone Sounds & Resources window).
    void setOtherWork(juce::String what, std::function<bool()> busy, std::function<void()> cancel) {
        otherWhat_ = std::move(what); otherBusy_ = std::move(busy); otherCancel_ = std::move(cancel);
    }
    bool styleUndo();
    bool styleRedo();
    void importSmfTrack(const juce::File& mid);
    void exportStyleSmf(const juce::File& mid);   // an export job (EXPERIMENTAL, L1); the status says when it lands
    juce::String leftoverText() const { return leftovers_.isVisible() ? leftovers_.getText() : juce::String(); }
    void saveStyleUasf(const juce::File& uasf);   // direct and synchronous (writeNewFile): the Save choice of the unwritten-edits prompt needs it
    const ui::studio::StyleStudioModel& styleModel() const { return style_; }
    juce::String styleStatus() const { return styleStatus_.getText(); }
    juce::String kmpText() const { return kmpReport_.getText(); }
    juce::String labText() const { return labReport_.getText(); }
    juce::Component* currentPage() const { return segments_.at(domain_)[size_t(segment_)].page; }

    // Import / Export Center job list (E6, jobs/): every export runs as a queued job off the message thread, one at a
    // time: validate, dependencies, destination guard, preview, estimate, write into NAME.INCOMPLETE, publish, SHA-256,
    // verify, report beside the output. The KMP/KSF writer and the Sample Lab writes enqueue jobs too.
    enum class ExportJob { SoundPackage, KmpFolder, KstBackup, StyleSmf, StyleUasf };
    uint64_t exportJob(ExportJob j, const juce::File& dest);   // 0 when unavailable
    juce::String exportUnavailable(ExportJob j);
    uint64_t importKitJob(const juce::File& outputs, const juce::File& dest);   // E9 Pa5X Import Kit from a folder of outputs
    void selectJob(uint64_t id);
    void cancelJob();                 // the selected job
    void openJobReport();             // the selected job's report, shown under the job list
    uint64_t rerunJobCleaning();      // the selected job, failed on a leftover NAME.INCOMPLETE: consent to move it aside (never deleted)
    std::vector<jobs::JobState> jobList() const { return queue_->snapshot(); }
    juce::String jobListText() const { return jobView_.getText(); }
    int jobListTop() const { return jobView_.getY(); }

    ResourcesComponent& resources() { return res_; }
    LibraryManagerComponent& manager() { return manager_; }   // Library Workspace > Library Manager (E7)
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
        std::vector<juce::Component*> collapse;   // rows led by one of these take no space while it is hidden
        void resized() override;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;   // a ResourcesComponent hand-off switched its tab
    void refreshInspector();
    void refreshAvailability();
    void addJob(const juce::String& line);
    void runJob(const juce::String& name, std::function<std::pair<juce::String, std::optional<ui::studio::Verification>>()> work);
    // A write/inspect job: its text goes to `out`; a written output becomes the verification target with its level.
    void runOutputJob(const juce::String& name, juce::TextEditor& out, std::function<ui::studio::JobResult()> work);
    void chooseSave(const juce::String& title, const juce::String& pattern, std::function<void(juce::File)> then);
    void refreshStyle();
    void loadStyle(const juce::File& sty, bool discard);
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
    juce::Label leftovers_;   // non-blocking banner: unfinished writes (NAME.INCOMPLETE, .stale-*) in the recorded output folders (jobs::leftoverReport)
    void checkLeftovers();    // read-only listing; on launch and whenever a page opens
    juce::TextEditor inspector_;

    Domain domain_ = Domain::Library;
    int segment_ = 0;
    int lastTab_ = -1;   // the ResourcesComponent tab the studio itself chose last
    std::map<Domain, std::vector<Segment>> segments_;

    // Library Workspace › Sources
    FlowPage sourcesPage_;
    juce::TextEditor libraryView_;
    juce::TextButton* toSampleLab_ = nullptr;
    juce::TextButton* paInspectBtn_ = nullptr;
    std::optional<ui::studio::SourceProbe> source_;
    LibraryManagerComponent manager_;   // Library Workspace › Library Manager (E7): two read-only libraries, compare, plan
    // Import / Export Center
    FlowPage ioPage_;
    juce::Label* staged_ = nullptr;
    // Sample Lab › KMP / KSF Writer and Tools
    FlowPage kmpPage_, labPage_;
    juce::TextEditor kmpReport_, labReport_;
    juce::TextButton* kmpBtn_ = nullptr;
    std::map<LabTool, juce::TextButton*> labButtons_;
    juce::TextEditor labBars_, labBeats_, labStart_, labEnd_, labSnap_, labXfade_;
    juce::ToggleButton labTrim_{"Trim -60 dBFS"}, labNorm_{"Normalize peak -1 dBFS"};
    juce::Label labInput_;
    juce::File labIn_;
    // Sample Lab › Quality Report
    FlowPage qualityPage_;
    juce::TextEditor quality_;
    // Style Studio
    FlowPage stylePage_;
    juce::TextEditor styleReport_;
    juce::TextButton* liveFromStyle_ = nullptr;
    ui::studio::StyleStudioModel style_;
    std::string sff1Report_;
    juce::File pendingStyle_;   // asked to open while the open Style has unwritten edits
    juce::String otherWhat_;
    std::function<bool()> otherBusy_;
    std::function<void()> otherCancel_;
    std::function<void()> pendingWorkQuit_;   // the app asked to quit while work runs (workPromptShown)
    bool quitWhenIdle_ = false;               // Wait or Cancel Jobs and Quit chosen: polled until busy() is false
    juce::Label* workStatus_ = nullptr;
    juce::TextButton *waitQuitBtn_ = nullptr, *cancelQuitBtn_ = nullptr, *keepWorkingBtn_ = nullptr;
    bool styleResolved_ = false;              // Save or Discard was chosen for this quit: the Style is not asked about again
    void askRunningWork(std::function<void()> quit);
    bool quitIfStyleWritten(std::function<void()> quit);   // the unwritten-Style check (b80f49a)
    void quitWhenIdle();
    std::function<void()> pendingQuit_;   // the app asked to quit while the open Style has unwritten edits
    void askUnsaved(const juce::String& what, const juce::String& then);
    juce::TextButton *pendSave_ = nullptr, *pendDiscard_ = nullptr, *pendCancel_ = nullptr;
    juce::ComboBox styleElem_, styleTrack_, styleOp_;
    juce::TextEditor styleValue_;
    juce::Label styleStatus_;
    juce::TextButton *applyBtn_ = nullptr, *undoBtn_ = nullptr, *redoBtn_ = nullptr, *importSmfBtn_ = nullptr, *exportSmfBtn_ = nullptr,
                     *saveUasfBtn_ = nullptr;
    // Validation & Reports
    FlowPage validationPage_;
    juce::Label target_, badges_;
    juce::TextEditor jobsLog_;
    juce::File validationTarget_;
    std::map<Validator, juce::TextButton*> validatorButtons_;
    juce::TextButton *useSourceBtn_ = nullptr, *l3Btn_ = nullptr;
    ui::studio::Verification verification_;
    bool verificationIsPackage_ = false;
    Domain verificationDomain_ = Domain::Validation;   // the page whose output the badges describe

    // Export jobs (E6)
    uint64_t enqueue(jobs::ExportSpec spec, juce::TextEditor* out, std::function<void(const jobs::JobState&)> done = {});
    void refreshJobs(bool land = true);   // message thread: job list; finished jobs land on their page and in the verification target
    juce::String exportReason(ExportJob j, juce::String where);
    struct JobHome { jobs::ExportSpec spec; juce::TextEditor* out = nullptr; Domain from; std::function<void(const jobs::JobState&)> done; bool landed = false; };
    std::map<uint64_t, JobHome> jobHomes_;
    std::map<ExportJob, std::pair<juce::TextButton*, juce::String>> exportButtons_;   // button, where it comes from
    juce::TextEditor jobView_;
    juce::ComboBox jobPick_;
    juce::TextButton *cancelJobBtn_ = nullptr, *reportBtn_ = nullptr, *rerunBtn_ = nullptr;
    uint64_t reportJob_ = 0;
    std::shared_ptr<std::atomic<bool>> jobsPosted_ = std::make_shared<std::atomic<bool>>(false);

    int jobs_ = 0;   // message thread only
    std::shared_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::unique_ptr<jobs::JobQueue> queue_;   // after alive_: destroyed first, so no job reports into a half-destroyed window

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StudioComponent)
};

}  // namespace ai_arranger::app
