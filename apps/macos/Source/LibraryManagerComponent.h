#pragma once

// Library Workspace > Library Manager (E7): two read-only libraries side by side (a Pa5X KST folder or a Pa-family SET
// folder each), one search box over both, and four views: item Details (with evidenced dependencies only), Compare
// (file level), Duplicates (SHA-256 files; names flagged as name-only) and Plan (staged copy / move / rename / bank
// swap steps with their conflicts, warnings and file effects; multi-step undo/redo before Execute), Where Used (what may
// refer to the selected item: UNKNOWN wherever the payloads are not decoded), Validation (the structural validator, E8) and
// Compatibility (the selected item's Keyboard Compatibility card, library/library_compat.h: sourced rule tables only, read-only). A plan is exported as a
// document, or (E8) executed at file level into a NEW library, a copy of the target, by an export job (E6: the studio's
// queue, both libraries as its sources); the sources are never written. Every unavailable action shows its reason next to it. Loading, comparing and
// exporting run on worker threads; nothing here touches MIDI or audio.

#include <juce_gui_extra/juce_gui_extra.h>
#include "JobPort.h"
#include "library/latest_worker.h"
#include "library/library_apply.h"
#include "library/library_compat.h"
#include "library/library_history.h"
#include "library/library_where_used.h"

namespace ai_arranger::app {

class LibraryManagerComponent : public juce::Component, private juce::Timer {
public:
    enum class View { Details, Compare, Duplicates, Plan, Validation, Compat, WhereUsed };
    enum class Action { Compare, Duplicates, Copy, Move, Rename, SwapBanks, RemoveStep, ClearPlan, Undo, Redo, Export, Execute, Reveal, Validate, Compat, WhereUsed };

    LibraryManagerComponent();
    ~LibraryManagerComponent() override;
    void resized() override;

    // What the buttons do; also driven by the UI smoke test.
    void openLibrary(char side, const juce::File& dir);   // asynchronous, read-only; busy() until it lands
    bool busy() const { return working(); }
    void stopWork();                                       // quit: a running library scan and the view report stop early
    uint64_t reportRequests() const { return reportGen_; }   // report requests so far (each recompute or drop counts one)
    bool computing() const { return deliveredGen_ != reportGen_; }   // the visible view's report is still being computed off the message thread
    void selectRow(char side, int row);                    // a row of that pane's (filtered) list
    void showView(View v);                                 // Compare is asynchronous
    void addStep(Action a);                                // Copy, Move, Rename or SwapBanks from the current selection
    void exportPlan(const juce::File& dest);               // an export job: a NEW local folder (plan.json, PLAN.txt)
    void clearPlan() { planEdit([this] { return history_.clear(); }); }
    void undo() { planEdit([this] { return history_.undo(); }); }   // staged plan edits only: nothing on disk changes
    void redo() { planEdit([this] { return history_.redo(); }); }
    void executePlan(const juce::File& parent);            // E8 export job: a NEW library "<target> edited <time>" in `parent`
    juce::File lastOutput() const { return lastOutput_; }  // the last executed library (Reveal)
    juce::ToggleButton& wholeFile() { return wholeFile_; } // SET: Copy / Move / Rename act on the whole bank file
    juce::String progressText() const { return progress_.getText(); }
    // Set by the studio: queues an export job, `done` runs on the message thread once it has finished. Returns its id.
    std::function<uint64_t(jobs::ExportSpec, std::function<void(const jobs::JobState&)> done)> enqueueJob;
    std::function<std::optional<jobs::JobState>(uint64_t)> jobState;   // set by the studio: progress of a running job
    juce::String unavailable(Action a) const;              // "" when it can run now

    juce::TextEditor& search() { return search_; }
    juce::TextEditor& value() { return value_; }
    juce::String reportText() const { return report_.getText(); }
    juce::String paneText(char side) const;
    juce::String headerText(char side) const;
    const std::vector<library::Step>& plan() const { return history_.steps(); }
    const library::PlanHistory& history() const { return history_; }

private:
    struct Pane : juce::ListBoxModel {
        LibraryManagerComponent& owner;
        char side;
        std::shared_ptr<const library::Library> lib;
        std::vector<size_t> rows;   // item indices shown, after the search filter
        int selected = -1;          // item index, or -1
        juce::ListBox list;
        juce::Label header;
        Pane(LibraryManagerComponent& o, char s) : owner(o), side(s) { list.setModel(this); }
        int getNumRows() override { return int(rows.size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selectedRow) override;
        void selectedRowsChanged(int row) override;
        void listBoxItemClicked(int row, const juce::MouseEvent&) override;   // a click on the row already selected changes no selection
        const library::Item* item() const { return lib && selected >= 0 ? &lib->items[size_t(selected)] : nullptr; }
    };

    juce::TextButton& button(const juce::String& text, std::function<void()> onClick);
    void setUnavailable(juce::Button& b, const juce::String& reason);
    void refresh(bool report = true);   // action states now; the view report on a worker thread (newest wins)
    void refilter();
    void planEdit(const std::function<bool()>& edit);   // a staged plan edit, then the Plan view
    void chooseFolder(const juce::String& title, std::function<void(juce::File)> then);
    Pane& pane(char side) { return side == 'A' ? a_ : b_; }
    const Pane& pane(char side) const { return side == 'A' ? a_ : b_; }

    Pane a_{*this, 'A'}, b_{*this, 'B'};
    bool quiet_ = false;  // refilter() is rebuilding the lists: selection callbacks are not user clicks
    char active_ = 'A';   // the pane selected last: Copy / Move / Rename / Swap Banks act on its selection
    View view_ = View::Details;
    library::PlanHistory history_;   // the staged change set: steps plus undo/redo, in memory until Execute Plan
    juce::OwnedArray<juce::Component> owned_;
    std::map<juce::Button*, juce::Label*> reasons_;
    std::map<Action, juce::TextButton*> actions_;
    std::vector<std::vector<std::pair<juce::Component*, int>>> rows_;   // fixed rows above the lists
    std::vector<std::vector<std::pair<juce::Component*, int>>> actionRows_;   // rows between the lists and the report
    juce::TextButton *openA_ = nullptr, *openB_ = nullptr, *details_ = nullptr;
    juce::TextEditor search_, value_, report_;
    juce::Label hint_, progress_;
    juce::ToggleButton wholeFile_{"Whole bank file (SET)"};
    juce::File lastOutput_;
    uint64_t execJob_ = 0;   // the execution job shown in progress_ (message thread)
    void timerCallback() override;
    bool working() const { return jobs_ > 0 || planJobs_.waiting(); }
    int jobs_ = 0;   // message thread only
    PendingJobs planJobs_;   // plan export jobs in the studio's queue, until each has landed (message thread)
    std::shared_ptr<std::atomic<bool>> stop_ = std::make_shared<std::atomic<bool>>(false);   // stopWork(): the running scan
    uint64_t reportGen_ = 0, deliveredGen_ = 0;   // newest report request / the one shown; older results are dropped (message thread)
    uint64_t planRev_ = 0;       // bumped on every plan edit: the Plan view's input
    std::string shownKey_;       // inputs of the report on screen; the same inputs are not recomputed
    std::shared_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    library::LatestWorker reports_;   // last member: destroyed first, so its job is cancelled and joined before anything else goes

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LibraryManagerComponent)
};

}  // namespace ai_arranger::app
