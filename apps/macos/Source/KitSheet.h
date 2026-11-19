#pragma once

// BL-E1 S4: the guided sheet over the Pa5X Import Kit (E9). One page, the design's steps in order:
//   1 Contents  2 Dependencies  3 Memory  4 Missing  (the read-only review of the chosen outputs folder: jobs::kit::plan)
//   5 Destination (an explicit parent folder + kit name: jobs::kit::checkDestination, free space included)
//   6 Review   7 Create kit (the existing Import Kit job, given the review's fingerprint)   8 Result
// The review and the destination check run on workers and write nothing; a stale result (a newer review, a closed
// window) is dropped. The only write is the E6 job, which re-validates everything at the execution boundary: it refuses a
// source that changed since the review, an existing or overlapping destination, a leftover, and too little free space.
// Titled "Pa5X Import Kit", never "KST export". Label: STRUCTURALLY VALID — NOT VERIFIED ON A PA5X; L3 never recorded here.

#include <juce_gui_extra/juce_gui_extra.h>
#include "TextViews.h"
#include "jobs/import_kit.h"
#include "ui/studio/studio_models.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace ai_arranger::app {

class KitSheet : public juce::Component {
public:
    KitSheet() {
        view_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        view_.setTitle("Pa5X Import Kit review");
        view_.setWrap(true);
        chooseOutputs_.setTitle("Step 1: choose the folder of outputs");
        chooseDest_.setTitle("Step 5: choose the destination folder");
        create_.setTitle("Step 7: create the kit");
        name_.setTitle("Kit name");
        name_.setTextToShowWhenEmpty("kit folder name", juce::Colours::grey);
        reason_.setTitle("Why Create kit is unavailable");
        reason_.setJustificationType(juce::Justification::centredLeft);
        chooseOutputs_.onClick = [this] {
            if (pickFolder) pickFolder("Folder holding the AIArranger outputs for the kit (only read)", [this](juce::File f) { review(f); });
        };
        chooseDest_.onClick = [this] {
            if (pickFolder) pickFolder("Parent folder for the new kit (local disk; a new folder is made, nothing is overwritten)",
                                       [this](juce::File f) { chooseDestination(f, name_.getText()); });
        };
        create_.onClick = [this] { createKit(); };
        name_.onReturnKey = name_.onFocusLost = [this] { if (parentSet_ && name_.getText() != checkedName_) chooseDestination(parent_, name_.getText()); };
        for (juce::Component* c : {(juce::Component*)&chooseOutputs_, (juce::Component*)&chooseDest_, (juce::Component*)&name_, (juce::Component*)&create_,
                                   (juce::Component*)&reason_, (juce::Component*)&view_})
            addAndMakeVisible(c);
        render();
    }
    ~KitSheet() override { *alive_ = false; stopWork(); }

    // Set by the Studio: the folder chooser, and the one job queue (`done` runs once on the message thread, any outcome).
    std::function<void(const juce::String&, std::function<void(juce::File)>)> pickFolder;
    std::function<uint64_t(jobs::ExportSpec, std::function<void(const jobs::JobState&)>)> enqueueJob;

    // Step 1: review a folder of outputs on a worker (read-only). A newer review replaces an older one.
    void review(const juce::File& outputs) {
        stopWork();
        const auto gen = ++reviewGen_;
        stop_ = std::make_shared<std::atomic<bool>>(false);
        outputs_ = outputs;
        plan_.reset();
        destCheck_.reset();
        stale_ = false;
        result_.clear();
        reviewing_ = true;
        render();
        std::weak_ptr<bool> alive = alive_;
        std::thread([this, alive, gen, stop = stop_, dir = outputs.getFullPathName().toStdString()] {
            lastWorker = jobs::thisThread();
            auto p = std::make_shared<const jobs::kit::ReviewPlan>(jobs::kit::plan(dir, [stop] { return !stop->load(); }));
            juce::MessageManager::callAsync([this, alive, gen, p] {
                if (alive.expired() || gen != reviewGen_) return;   // a newer review (or a closed window) lands instead
                reviewing_ = false;
                plan_ = p;
                render();
                if (parentSet_) chooseDestination(parent_, checkedName_);   // the free space depends on the plan's bytes
            });
        }).detach();
    }

    // Step 5: an explicit parent folder and a kit name (default: the outputs folder's name + " Pa5X Kit"). Read-only check.
    void chooseDestination(const juce::File& parent, const juce::String& kitName = {}) {
        parent_ = parent;
        parentSet_ = true;
        checkedName_ = kitName.isNotEmpty() ? kitName : juce::File::createLegalFileName(outputs_.getFileName() + " Pa5X Kit");
        if (name_.getText() != checkedName_) name_.setText(checkedName_, juce::dontSendNotification);
        destCheck_.reset();
        if (!plan_) { render(); return; }   // checked as soon as the review lands
        const auto gen = ++destGen_;
        checking_ = true;
        render();
        std::weak_ptr<bool> alive = alive_;
        std::thread([this, alive, gen, par = parent.getFullPathName().toStdString(), name = checkedName_.toStdString(),
                     bytes = plan_->bytes, src = outputs_.getFullPathName().toStdString()] {
            lastWorker = jobs::thisThread();
            auto d = std::make_shared<const jobs::kit::DestinationCheck>(jobs::kit::checkDestination(par, name, bytes, {src}));
            juce::MessageManager::callAsync([this, alive, gen, d] {
                if (alive.expired() || gen != destGen_) return;
                checking_ = false;
                destCheck_ = d;
                render();
            });
        }).detach();
    }

    // Step 7. false: not started (createReason() says why). The job re-validates at the execution boundary.
    bool createKit() {
        if (createReason().isNotEmpty() || !enqueueJob) return false;
        creating_ = true;
        result_ = "Creating the kit as an export job (progress and Cancel Job are in the job list)...";
        render();
        std::weak_ptr<bool> alive = alive_;
        const auto dest = (parent_.getChildFile(checkedName_)).getFullPathName().toStdString();
        enqueueJob(jobs::kit::importKit({}, dest, outputs_.getFullPathName().toStdString(), plan_->fingerprint),
                   [this, alive, dest](const jobs::JobState& st) {
                       if (alive.expired()) return;
                       landed(st, dest);
                   });
        return true;
    }

    void stopWork() { if (stop_) stop_->store(true); }   // quit: a running review stops early
    bool busy() const { return reviewing_ || checking_ || creating_; }

    // "" = Create kit is available; otherwise the reason, shown inline and as the button's tooltip.
    juce::String createReason() const {
        if (creating_) return "A kit is being created.";
        if (reviewing_) return "The review is still running.";
        if (!plan_) return "Step 1: choose the folder of outputs.";
        if (stale_) return "The sources changed since the review: review the folder again.";
        if (!plan_->blocking.empty()) return "Step 4: " + juce::String(int(plan_->blocking.size())) + " blocking problem(s): fix the sources, then review again.";
        if (!parentSet_) return "Step 5: choose the destination folder.";
        if (checking_ || !destCheck_) return "Step 5: the destination is being checked.";
        if (!destCheck_->ok) return "Step 5: " + juce::String::fromUTF8(destCheck_->refusals.front().c_str());
        return {};
    }
    bool createEnabled() const { return create_.isEnabled(); }
    juce::String text() const { return view_.getText(); }
    std::shared_ptr<const jobs::kit::ReviewPlan> plan() const { return plan_; }
    std::shared_ptr<const jobs::kit::DestinationCheck> destination() const { return destCheck_; }
    bool stale() const { return stale_; }
    juce::Button& createButton() { return create_; }
    TextView& view() { return view_; }
    static inline std::atomic<size_t> lastWorker{0};   // test hook: the thread that last ran a review or a destination check

    void resized() override {
        auto r = getLocalBounds().reduced(8);
        auto top = r.removeFromTop(30);
        chooseOutputs_.setBounds(top.removeFromLeft(240));
        top.removeFromLeft(8);
        chooseDest_.setBounds(top.removeFromLeft(260));
        r.removeFromTop(6);
        auto mid = r.removeFromTop(30);
        name_.setBounds(mid.removeFromLeft(300));
        mid.removeFromLeft(8);
        create_.setBounds(mid.removeFromLeft(130));
        mid.removeFromLeft(8);
        reason_.setBounds(mid);
        r.removeFromTop(6);
        view_.setBounds(r);
    }

private:
    void landed(const jobs::JobState& st, const std::string& dest) {
        creating_ = false;
        using P = jobs::Phase;
        if (st.phase == P::Done)
            result_ = "DONE: " + juce::String::fromUTF8(dest.c_str()) + "\n" + juce::String::fromUTF8(ui::studio::badgeLine(st.level).c_str()) +
                      "\nL1 detail: " + juce::String::fromUTF8(st.level.l1.detail.c_str()) +
                      "\nL3 (the Pa5X itself): not recorded. " + jobs::kit::kLabel + ". Copy the folder to a USB stick by hand (Finder), then follow KIT.md.";
        else {
            if (st.phase == P::Failed && st.reason.find("changed since") != std::string::npos) stale_ = true;
            result_ = juce::String(st.phase == P::Cancelled ? "CANCELLED: " : "FAILED at " + juce::String(jobs::phaseName(st.stoppedAt)) + ": ") +
                      juce::String::fromUTF8(st.reason.c_str()) + "\nNothing was published" +
                      (st.phase == P::Cancelled ? "; a NAME.INCOMPLETE folder may remain (the job list says where)." : ".");
        }
        render();
    }

    void render() {
        using jobs::fmtBytes;
        juce::String t;
        const auto u = [](const std::string& s) { return juce::String::fromUTF8(s.c_str()); };
        t << "PA5X IMPORT KIT: review (nothing is written until Step 7)\n"
          << jobs::kit::kLabel << ". The kit copies files byte for byte: nothing is converted, no Korg-native (KST) file is written, nothing is sent to the instrument.\n\n";
        t << "1 Contents      " << (outputs_ == juce::File() ? juce::String("(no folder chosen)") : outputs_.getFullPathName()) << (reviewing_ ? "   [reviewing...]" : "") << "\n";
        if (plan_) {
            for (const auto& it : plan_->items) t << "   " << u(it.kitPath()) << "  (" << u(jobs::kit::kindName(it.kind)) << ")\n";
            for (const auto& w : plan_->warnings) t << "   " << u(w) << "\n";
            t << "2 Dependencies  (the independent readers, on the source)\n";
            for (const auto& it : plan_->items) t << "   " << u(it.status) << ": " << u(it.kitPath()) << " (" << u(it.detail) << ")\n";
            t << "3 Memory        not estimated for a kit (it has no sample accounting; no number is invented)\n";
            t << "4 Missing       " << (plan_->blocking.empty() ? juce::String("none") : juce::String(int(plan_->blocking.size())) + " blocking problem(s), never resolved automatically") << "\n";
            for (const auto& b : plan_->blocking) t << "   BLOCKING: " << u(b) << "\n";
        } else {
            t << "2 Dependencies  after the review\n3 Memory        after the review\n4 Missing       after the review\n";
        }
        t << "5 Destination   ";
        if (!parentSet_) t << "(no folder chosen: a NEW local folder is made inside the one you choose; nothing is overwritten)\n";
        else {
            t << parent_.getChildFile(checkedName_).getFullPathName() << (checking_ ? "   [checking...]" : "") << "\n";
            if (destCheck_) {
                t << "   free space " << u(fmtBytes(destCheck_->freeBytes)) << ", the kit needs " << u(fmtBytes(destCheck_->neededBytes)) << " (members + 10%)\n";
                for (const auto& r : destCheck_->refusals) t << "   REFUSED: " << u(r) << "\n";
                if (destCheck_->ok) t << "   ok: a new folder, outside every source, on a local disk\n";
            }
        }
        if (plan_ && plan_->blocking.empty()) {
            t << "6 Review        " << int(plan_->items.size()) << " member(s), " << u(fmtBytes(plan_->bytes)) << ", plan " << u(plan_->fingerprint.substr(0, 12)) << "...\n"
              << "   Will NOT happen: no conversion, no Korg-native file, nothing sent to the instrument, no source changed, nothing overwritten.\n"
              << "   Step 7 re-validates at the moment of writing: a source that changed since this review, an existing or overlapping destination, a leftover and free space are refused.\n";
        } else {
            t << "6 Review        after a review with no blocking problem and an accepted destination\n";
        }
        t << "7 Create kit    " << (createReason().isEmpty() ? juce::String("available") : createReason()) << "\n";
        if (result_.isNotEmpty()) t << "8 Result        " << result_ << "\n";
        view_.setText(t);
        const auto why = createReason();
        create_.setEnabled(why.isEmpty());
        create_.setTooltip(why);
        reason_.setText(why, juce::dontSendNotification);
    }

    juce::TextButton chooseOutputs_{"1  Choose outputs folder..."}, chooseDest_{"5  Choose destination folder..."}, create_{"7  Create kit"};
    juce::TextEditor name_;
    juce::Label reason_;
    TextView view_;
    juce::File outputs_, parent_;
    juce::String checkedName_;
    bool parentSet_ = false, reviewing_ = false, checking_ = false, creating_ = false, stale_ = false;
    std::shared_ptr<const jobs::kit::ReviewPlan> plan_;
    std::shared_ptr<const jobs::kit::DestinationCheck> destCheck_;
    juce::String result_;
    uint64_t reviewGen_ = 0, destGen_ = 0;
    std::shared_ptr<std::atomic<bool>> stop_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace ai_arranger::app
