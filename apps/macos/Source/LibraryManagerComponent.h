#pragma once

// Library Workspace > Library Manager (E7): two read-only libraries side by side (a Pa5X KST folder or a Pa-family SET
// folder each), one search box over both, and four views: item Details (with evidenced dependencies only), Compare
// (file level), Duplicates (SHA-256 files; names flagged as name-only) and Plan (staged copy / move / rename / bank
// swap steps with their conflicts and warnings). Plans are EXPORT-ONLY documents written as a new local folder; no
// keyboard file is ever written. Every unavailable action shows its reason next to it. Loading, comparing and
// exporting run on worker threads; nothing here touches MIDI or audio.

#include <juce_gui_extra/juce_gui_extra.h>
#include "library/library_plan.h"

namespace ai_arranger::app {

class LibraryManagerComponent : public juce::Component {
public:
    enum class View { Details, Compare, Duplicates, Plan };
    enum class Action { Compare, Duplicates, Copy, Move, Rename, SwapBanks, RemoveStep, ClearPlan, Export };

    LibraryManagerComponent();
    ~LibraryManagerComponent() override;
    void resized() override;

    // What the buttons do; also driven by the UI smoke test.
    void openLibrary(char side, const juce::File& dir);   // asynchronous, read-only; busy() until it lands
    bool busy() const { return jobs_ > 0; }
    bool computing() const { return computing_ > 0; }      // a view report is being computed off the message thread
    void selectRow(char side, int row);                    // a row of that pane's (filtered) list
    void showView(View v);                                 // Compare is asynchronous
    void addStep(Action a);                                // Copy, Move, Rename or SwapBanks from the current selection
    void exportPlan(const juce::File& dest);               // asynchronous: a NEW local folder (plan.json, PLAN.txt)
    juce::String unavailable(Action a) const;              // "" when it can run now

    juce::TextEditor& search() { return search_; }
    juce::TextEditor& value() { return value_; }
    juce::String reportText() const { return report_.getText(); }
    juce::String paneText(char side) const;
    juce::String headerText(char side) const;
    const std::vector<library::Step>& plan() const { return plan_; }

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
        const library::Item* item() const { return lib && selected >= 0 ? &lib->items[size_t(selected)] : nullptr; }
    };

    juce::TextButton& button(const juce::String& text, std::function<void()> onClick);
    void setUnavailable(juce::Button& b, const juce::String& reason);
    void refresh(bool report = true);   // action states now; the view report on a worker thread (newest wins)
    void refilter();
    void chooseFolder(const juce::String& title, std::function<void(juce::File)> then);
    Pane& pane(char side) { return side == 'A' ? a_ : b_; }
    const Pane& pane(char side) const { return side == 'A' ? a_ : b_; }

    Pane a_{*this, 'A'}, b_{*this, 'B'};
    bool quiet_ = false;  // refilter() is rebuilding the lists: selection callbacks are not user clicks
    char active_ = 'A';   // the pane selected last: Copy / Move / Rename / Swap Banks act on its selection
    View view_ = View::Details;
    std::vector<library::Step> plan_;
    juce::OwnedArray<juce::Component> owned_;
    std::map<juce::Button*, juce::Label*> reasons_;
    std::map<Action, juce::TextButton*> actions_;
    std::vector<std::vector<std::pair<juce::Component*, int>>> rows_;   // fixed rows above the lists
    std::vector<std::vector<std::pair<juce::Component*, int>>> actionRows_;   // rows between the lists and the report
    juce::TextButton *openA_ = nullptr, *openB_ = nullptr, *details_ = nullptr;
    juce::TextEditor search_, value_, report_;
    juce::Label hint_;
    int jobs_ = 0;   // message thread only
    int computing_ = 0;          // view reports in flight (message thread only)
    uint64_t reportGen_ = 0;     // the newest report request; older results are dropped
    std::shared_ptr<juce::FileChooser> chooser_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LibraryManagerComponent)
};

}  // namespace ai_arranger::app
