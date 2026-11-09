#include "ResourcesComponent.h"
#include <thread>

namespace ai_arranger::app {

namespace rg = media::graph;
using Action = ui::ResourceBrowserModel::KstAction;

static const char* kMemoryTemplate = "aiarranger-memory-info\t1\nsamples\t\nsamples-size\t\nmultisamples\t\ndrum-samples\t\n";

namespace {
// One resource in the tree; children are created when the row is first opened.
class ResourceItem : public juce::TreeViewItem {
public:
    ResourceItem(ResourcesComponent& owner, rg::ResourceId id, std::optional<rg::Edge> via)
        : owner_(owner), id_(std::move(id)), via_(std::move(via)) {
        text_ = owner_.model().rowLabel(id_, via_ ? &*via_ : nullptr);
        missing_ = owner_.model().graph().find(id_) == nullptr;
        verdict_ = owner_.model().graph().deleteCheck(id_).verdict;
    }
    bool mightContainSubItems() override { return !owner_.model().children(id_).empty(); }
    juce::String getUniqueName() const override { return id_.str() + (via_ ? "|" + via_->detail : ""); }
    void itemOpennessChanged(bool open) override {
        if (open && getNumSubItems() == 0)
            for (const auto& e : owner_.model().children(id_)) addSubItem(new ResourceItem(owner_, e.to, e));
    }
    void itemSelectionChanged(bool selected) override { if (selected) owner_.showDetails(id_); }
    void paintItem(juce::Graphics& g, int w, int h) override {
        if (isSelected()) g.fillAll(juce::Colour(0xff2f4f6f));
        // Delete-safety tag on the right, coloured, so a verdict is visible without opening details.
        const char* tag = missing_ ? "MISSING" : verdict_ == rg::DeleteVerdict::InUse ? "IN USE"
                        : verdict_ == rg::DeleteVerdict::Unprovable ? "UNPROVEN" : "NO REFS";
        const auto colour = missing_ ? juce::Colour(0xffe05050) : verdict_ == rg::DeleteVerdict::InUse ? juce::Colour(0xff6fbf73)
                          : verdict_ == rg::DeleteVerdict::Unprovable ? juce::Colour(0xffe0b040) : juce::Colour(0xff9090a0);
        g.setFont(juce::FontOptions(13.0f));
        g.setColour(colour);
        g.drawText(tag, w - 84, 0, 80, h, juce::Justification::centredRight);
        g.setColour(missing_ ? colour : juce::Colours::white);
        g.drawText(text_, 4, 0, w - 92, h, juce::Justification::centredLeft, true);
    }
private:
    ResourcesComponent& owner_;
    rg::ResourceId id_;
    std::optional<rg::Edge> via_;
    juce::String text_;
    bool missing_ = false;
    rg::DeleteVerdict verdict_{};
};

class RootItem : public juce::TreeViewItem {
public:
    bool mightContainSubItems() override { return true; }
};
}  // namespace

void ResourcesComponent::styleText(juce::TextEditor& t, bool readOnly) {
    t.setMultiLine(true);
    t.setReadOnly(readOnly);
    t.setScrollbarsShown(true);
    t.setReturnKeyStartsNewLine(true);
    t.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
}

ResourcesComponent::ResourcesComponent() {
    // ── Resource Graph ──
    legend_.setText("Open AIArranger Sound / Drum Kit packages. Tags: IN USE, UNPROVEN (cannot be proven safe to delete), "
                    "NO REFS, MISSING. Package resources are provisional: the Pa5X assigns slots on import.", juce::dontSendNotification);
    legend_.setColour(juce::Label::textColourId, juce::Colour(0xffb8b8c0));
    openPkgBtn_.onClick = [this] { chooseFolder("Open an AIArranger package folder", [this](juce::File f) { openPackage(f); }); };
    openEvBtn_.onClick = [this] {
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Owner-authorized evidence only",
            "Open an aiarranger-evidence file only if the owner authorized the report it was transcribed from. "
            "It is kept as a separate evidence source; inferred references are never treated as confirmed.",
            "Choose File...", "Cancel", nullptr,
            juce::ModalCallbackFunction::create([this, alive = std::weak_ptr<bool>(alive_)](int r) {
                if (r == 1 && !alive.expired()) chooseFile("Open an aiarranger-evidence file", [this](juce::File f) { openEvidence(f); });
            }));
    };
    clearBtn_.onClick = [this] { model_.clear(); details_.clear(); rebuildTree(); };
    for (auto* b : {&openPkgBtn_, &openEvBtn_, &clearBtn_}) graphPage_.addAndMakeVisible(*b);
    graphPage_.addAndMakeVisible(legend_);
    tree_.setRootItemVisible(false);
    tree_.setDefaultOpenness(false);
    tree_.setColour(juce::TreeView::backgroundColourId, juce::Colour(0xff222228));
    graphPage_.addAndMakeVisible(tree_);
    styleText(details_, true);
    styleText(summary_, true);
    graphPage_.addAndMakeVisible(details_);
    graphPage_.addAndMakeVisible(summary_);

    // ── KST Library ──
    kstNote_.setText(ui::ResourceBrowserModel::kSemanticMergeNote, juce::dontSendNotification);
    kstNote_.setColour(juce::Label::textColourId, juce::Colour(0xffe0b040));
    invBtn_.onClick = [this] { chooseFolder("KST folder to inventory", [this](juce::File a) { runKst(Action::Inventory, a.getFullPathName()); }); };
    diffBtn_.onClick = [this] {
        chooseFolder("First KST folder", [this](juce::File a) {
            chooseFolder("Second KST folder", [this, a](juce::File b) { runKst(Action::Diff, a.getFullPathName(), b.getFullPathName()); });
        });
    };
    planBtn_.onClick = [this] {
        chooseFolder("KST folder you would LOAD (source)", [this](juce::File a) {
            chooseFolder("Saved copy of the CURRENT User data (destination)", [this, a](juce::File b) {
                runKst(Action::MergePlan, a.getFullPathName(), b.getFullPathName());
            });
        });
    };
    semanticBtn_.setEnabled(false);   // stays off until validated dependency data exists (Q17, Q18)
    semanticBtn_.setTooltip("Unavailable: needs validated Sound/sample dependency data");
    for (auto* b : {&invBtn_, &diffBtn_, &planBtn_, &semanticBtn_}) kstPage_.addAndMakeVisible(*b);
    kstPage_.addAndMakeVisible(kstNote_);
    styleText(kstOut_, true);
    kstPage_.addAndMakeVisible(kstOut_);

    // ── Memory Check ──
    memNote_.setText("Type the Pa5X Memory Info values (Sample Edit > Memory Info) before and after an import, exactly as shown, "
                     "e.g. 12.3 MB. Compares them with the open packages' raw PCM. Observation only: overhead and compression are unknown.",
                     juce::dontSendNotification);
    memNote_.setColour(juce::Label::textColourId, juce::Colour(0xffb8b8c0));
    for (auto* t : {&memBefore_, &memAfter_}) { styleText(*t, false); t->setText(kMemoryTemplate, false); t->setTabKeyUsedAsCharacter(true); memPage_.addAndMakeVisible(*t); }
    styleText(memOut_, true);
    compareBtn_.onClick = [this] { compareMemory(); };
    for (juce::Component* c : {(juce::Component*)&memNote_, (juce::Component*)&beforeLbl_, (juce::Component*)&afterLbl_,
                               (juce::Component*)&memOut_, (juce::Component*)&compareBtn_})
        memPage_.addAndMakeVisible(*c);

    const auto bg = juce::Colour(0xff1e1e1e);
    tabs_.addTab("Resource Graph", bg, &graphPage_, false);
    tabs_.addTab("KST Library", bg, &kstPage_, false);
    tabs_.addTab("Memory Check", bg, &memPage_, false);
    addAndMakeVisible(tabs_);

    rebuildTree();
    setSize(980, 640);
}

ResourcesComponent::~ResourcesComponent() {
    *alive_ = false;
    tree_.setRootItem(nullptr);
}

void ResourcesComponent::chooseFolder(const juce::String& title, std::function<void(juce::File)> then) {
    chooser_ = std::make_shared<juce::FileChooser>(title);
    auto c = chooser_;
    c->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                   [c, then, alive = std::weak_ptr<bool>(alive_)](const juce::FileChooser& fc) {
                       if (!alive.expired() && fc.getResult() != juce::File()) then(fc.getResult());   // the window may be gone
                   });
}
void ResourcesComponent::chooseFile(const juce::String& title, std::function<void(juce::File)> then) {
    chooser_ = std::make_shared<juce::FileChooser>(title, juce::File(), "*.txt;*.tsv");
    auto c = chooser_;
    c->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                   [c, then, alive = std::weak_ptr<bool>(alive_)](const juce::FileChooser& fc) {
                       if (!alive.expired() && fc.getResult() != juce::File()) then(fc.getResult());
                   });
}

void ResourcesComponent::openPackage(const juce::File& dir) {
    // Loading reads and fingerprints every WAV: do it off the message thread, adopt the result on it.
    ++loads_;
    details_.setText("Loading " + dir.getFullPathName() + " (read-only)...", false);
    std::weak_ptr<bool> alive = alive_;
    const uint64_t epoch = model_.epoch();
    std::thread([this, alive, epoch, d = dir.getFullPathName().toStdString()] {
        auto one = std::make_shared<media::graph::Graph>();
        std::string pkg, err;
        const bool loaded = media::sound::loadPackage(d, *one, pkg, &err);
        juce::MessageManager::callAsync([this, alive, epoch, one, pkg, err, loaded, d] {
            if (alive.expired()) return;
            --loads_;
            if (epoch != model_.epoch()) return;   // Close All was pressed meanwhile: drop the load
            std::string e = err;
            const bool ok = loaded && model_.adoptPackage(std::move(*one), pkg, d, &e);
            rebuildTree();
            details_.setText(ok ? juce::String("Opened ") + d : "Could not open package:\n" + juce::String(e), false);
        });
    }).detach();
}

void ResourcesComponent::openEvidence(const juce::File& file) {
    // Parse off the message thread against a snapshot; adopt only if nothing changed meanwhile.
    ++loads_;
    details_.setText("Reading evidence " + file.getFullPathName() + " (read-only)...", false);
    std::weak_ptr<bool> alive = alive_;
    auto work = std::make_shared<media::graph::Graph>(model_.graph());
    const uint64_t version = model_.version();
    std::thread([this, alive, work, version, f = file.getFullPathName().toStdString()] {
        std::string label, err;
        const bool parsed = ui::ResourceBrowserModel::prepareEvidence(f, *work, label, &err);
        juce::MessageManager::callAsync([this, alive, work, version, label, err, parsed] {
            if (alive.expired()) return;
            --loads_;
            std::string e = err;
            const bool ok = parsed && model_.adoptEvidence(std::move(*work), label, version, &e);
            rebuildTree();
            details_.setText(ok ? "Opened evidence source '" + juce::String(label) + "'" : "Evidence refused (nothing was added):\n" + juce::String(e), false);
        });
    }).detach();
}

void ResourcesComponent::rebuildTree() {
    tree_.setRootItem(nullptr);
    rootItem_ = std::make_unique<RootItem>();
    for (const auto& r : model_.roots()) rootItem_->addSubItem(new ResourceItem(*this, r, std::nullopt));
    tree_.setRootItem(rootItem_.get());
    summary_.setText(juce::String(model_.budgetText()) + model_.issuesText(), false);
    if (model_.roots().empty()) details_.setText("Nothing open. Use Open Package... on a folder written by aiarranger-sound.", false);
}

void ResourcesComponent::showDetails(const rg::ResourceId& id) { details_.setText(model_.details(id), false); }

void ResourcesComponent::runKst(Action a, const juce::String& first, const juce::String& second) {
    // Scans can take a while on a large KST: run off the message thread, post the text back.
    kstOut_.setText("Working (read-only)...", false);
    ++kstJobs_;
    std::weak_ptr<bool> alive = alive_;
    std::thread([this, alive, a, f = first.toStdString(), s = second.toStdString()] {
        auto text = ui::ResourceBrowserModel::kst(a, f, s);
        juce::MessageManager::callAsync([this, alive, text = std::move(text)] {
            if (alive.expired()) return;
            if (--kstJobs_ == 0) kstOut_.setText(text, false);
        });
    }).detach();
}

void ResourcesComponent::compareMemory() {
    memOut_.setText(model_.memoryCheck(memBefore_.getText().toStdString(), memAfter_.getText().toStdString()), false);
}

void ResourcesComponent::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff1e1e1e)); }

void ResourcesComponent::resized() {
    tabs_.setBounds(getLocalBounds());
    const auto page = tabs_.getLocalBounds().withTrimmedTop(tabs_.getTabBarDepth()).reduced(10);

    {   // Resource Graph
        auto r = page.withZeroOrigin();
        auto top = r.removeFromTop(28);
        for (auto* b : {&openPkgBtn_, &openEvBtn_, &clearBtn_}) { b->setBounds(top.removeFromLeft(140)); top.removeFromLeft(8); }
        r.removeFromTop(4);
        legend_.setBounds(r.removeFromTop(36));
        summary_.setBounds(r.removeFromBottom(150));
        r.removeFromBottom(6);
        tree_.setBounds(r.removeFromLeft(r.getWidth() * 11 / 20));
        r.removeFromLeft(6);
        details_.setBounds(r);
    }
    {   // KST Library
        auto r = page.withZeroOrigin();
        auto top = r.removeFromTop(28);
        for (auto* b : {&invBtn_, &diffBtn_, &planBtn_, &semanticBtn_}) { b->setBounds(top.removeFromLeft(150)); top.removeFromLeft(8); }
        r.removeFromTop(4);
        kstNote_.setBounds(r.removeFromTop(40));
        kstOut_.setBounds(r);
    }
    {   // Memory Check
        auto r = page.withZeroOrigin();
        memNote_.setBounds(r.removeFromTop(40));
        auto inputs = r.removeFromTop(160);
        auto left = inputs.removeFromLeft(inputs.getWidth() / 2 - 4);
        inputs.removeFromLeft(8);
        beforeLbl_.setBounds(left.removeFromTop(22));
        memBefore_.setBounds(left);
        afterLbl_.setBounds(inputs.removeFromTop(22));
        memAfter_.setBounds(inputs);
        r.removeFromTop(6);
        compareBtn_.setBounds(r.removeFromTop(28).removeFromLeft(140));
        r.removeFromTop(6);
        memOut_.setBounds(r);
    }
}

} // namespace ai_arranger::app
