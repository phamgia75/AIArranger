#include "LibraryManagerComponent.h"
#include "jobs/export_specs.h"
#include <thread>

namespace ai_arranger::app {

namespace lib = library;

namespace {
// Tokens from UI spec rev 2 §3.2 (dark), as in StudioComponent.cpp.
const juce::Colour kSurface(0xff1a1a1f), kSurface2(0xff252530), kAccent(0xff00e5ff), kPending(0xffffb300), kText2(0xffa0a0b0);
juce::String u8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
}  // namespace

void LibraryManagerComponent::Pane::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selectedRow) {
    if (!lib || row < 0 || row >= int(rows.size())) return;
    g.fillAll(selectedRow ? kSurface2 : kSurface);
    g.setColour(selectedRow ? kAccent : juce::Colours::white);
    g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    g.drawText(u8(lib::itemLine(lib->items[rows[size_t(row)]])), 6, 0, w - 12, h, juce::Justification::centredLeft, true);
}

void LibraryManagerComponent::Pane::selectedRowsChanged(int row) {
    if (owner.quiet_) return;   // the list was rebuilt by refilter(), not clicked
    selected = row >= 0 && row < int(rows.size()) ? int(rows[size_t(row)]) : -1;
    owner.active_ = side;
    owner.view_ = View::Details;
    owner.refresh();
}

juce::TextButton& LibraryManagerComponent::button(const juce::String& text, std::function<void()> onClick) {
    auto* b = new juce::TextButton(text);
    owned_.add(b);
    b->onClick = std::move(onClick);
    auto* r = new juce::Label();
    owned_.add(r);
    r->setColour(juce::Label::textColourId, kPending);
    reasons_[b] = r;
    return *b;
}

// The inline reason next to an unavailable control (spec §3.4); also its tooltip.
void LibraryManagerComponent::setUnavailable(juce::Button& b, const juce::String& reason) {
    b.setEnabled(reason.isEmpty());
    b.setTooltip(reason);
    reasons_.at(&b)->setText(reason, juce::dontSendNotification);
}

LibraryManagerComponent::LibraryManagerComponent() {
    auto styleText = [](juce::TextEditor& t) {
        t.setMultiLine(true);
        t.setReadOnly(true);
        t.setScrollbarsShown(true);
        t.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        t.setColour(juce::TextEditor::backgroundColourId, kSurface);
    };
    styleText(report_);
    openA_ = &button("Open Library A...", [this] { chooseFolder("Library A: a KST or SET folder (only read)", [this](juce::File f) { openLibrary('A', f); }); });
    openB_ = &button("Open Library B...", [this] { chooseFolder("Library B: a KST or SET folder (only read)", [this](juce::File f) { openLibrary('B', f); }); });
    details_ = &button("Details", [this] { showView(View::Details); });
    auto act = [this](Action a, const juce::String& text, std::function<void()> f) -> juce::TextButton& { return *(actions_[a] = &button(text, std::move(f))); };
    auto& cmp = act(Action::Compare, "Compare", [this] { showView(View::Compare); });
    auto& dup = act(Action::Duplicates, "Duplicates", [this] { showView(View::Duplicates); });
    auto& planBtn = button("Plan", [this] { showView(View::Plan); });
    auto& cp = act(Action::Copy, "Copy to other side", [this] { addStep(Action::Copy); });
    auto& mv = act(Action::Move, "Move to other side", [this] { addStep(Action::Move); });
    auto& rn = act(Action::Rename, "Rename to value", [this] { addStep(Action::Rename); });
    auto& sw = act(Action::SwapBanks, "Swap bank with value", [this] { addStep(Action::SwapBanks); });
    auto& rm = act(Action::RemoveStep, "Remove Last Step", [this] { if (!plan_.empty()) plan_.pop_back(); ++planRev_; showView(View::Plan); });
    auto& cl = act(Action::ClearPlan, "Clear Plan", [this] { plan_.clear(); ++planRev_; showView(View::Plan); });
    auto& ex = act(Action::Export, "Export Plan (EXPORT-ONLY)...", [this] {
        chooseFolder("Parent folder for the new plan folder (local disk)", [this](juce::File parent) {
            exportPlan(parent.getChildFile("Library Plan " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S")));
        });
    });
    search_.setTextToShowWhenEmpty("Search names, banks, files (both sides)", kText2);
    search_.onTextChange = [this] { refilter(); };
    value_.setTextToShowWhenEmpty("value: new name, or the other bank (e.g. STYLE/USER05)", kText2);
    value_.onTextChange = [this] { refresh(); };
    hint_.setColour(juce::Label::textColourId, kText2);
    hint_.setText("Sources are only read. Copy/Move target: the bank of the item selected on the other side, else the same bank id; first free slot. "
                  "Plans are EXPORT-ONLY documents: no keyboard file is written.", juce::dontSendNotification);
    auto r = [this](juce::Component& c) -> juce::Component* { return reasons_.count(dynamic_cast<juce::Button*>(&c)) ? reasons_.at(dynamic_cast<juce::Button*>(&c)) : nullptr; };
    rows_ = {{{openA_, 150}, {r(*openA_), 140}, {&a_.header, 0}},
             {{openB_, 150}, {r(*openB_), 140}, {&b_.header, 0}},
             {{&search_, 300}, {details_, 80}, {&cmp, 90}, {&dup, 100}, {&planBtn, 70}, {r(cmp), 260}, {r(dup), 0}}};
    actionRows_ = {{{&cp, 160}, {r(cp), 300}, {&mv, 160}, {r(mv), 0}},
                   {{&value_, 300}, {&rn, 140}, {r(rn), 200}, {&sw, 170}, {r(sw), 0}},
                   {{&rm, 140}, {r(rm), 160}, {&cl, 100}, {r(cl), 160}, {&ex, 220}, {r(ex), 0}},
                   {{&hint_, 0}}};
    for (auto* rows : {&rows_, &actionRows_})
        for (auto& row : *rows) for (auto& [c, w] : row) addAndMakeVisible(c);
    for (auto* p : {&a_, &b_}) {
        p->header.setColour(juce::Label::textColourId, juce::Colours::white);
        p->header.setText(juce::String("Library ") + p->side + ": none open", juce::dontSendNotification);
        p->list.setColour(juce::ListBox::backgroundColourId, kSurface);
        p->list.setRowHeight(20);
        addAndMakeVisible(p->list);
    }
    addAndMakeVisible(report_);
    refresh();
}

LibraryManagerComponent::~LibraryManagerComponent() {
    *alive_ = false;
    a_.list.setModel(nullptr);
    b_.list.setModel(nullptr);
}

void LibraryManagerComponent::resized() {
    auto area = getLocalBounds();
    auto layRow = [](juce::Rectangle<int> line, const std::vector<std::pair<juce::Component*, int>>& row) {
        for (const auto& [c, w] : row) {
            c->setBounds(w > 0 ? line.removeFromLeft(w) : line);
            if (w > 0) line.removeFromLeft(8);
        }
    };
    for (const auto& row : rows_) { layRow(area.removeFromTop(28), row); area.removeFromTop(6); }
    auto bottom = area.removeFromBottom(area.getHeight() * 2 / 5);
    for (auto row = actionRows_.rbegin(); row != actionRows_.rend(); ++row) { area.removeFromBottom(6); layRow(area.removeFromBottom(28), *row); }
    area.removeFromBottom(6);
    a_.list.setBounds(area.removeFromLeft(area.getWidth() / 2 - 4));
    area.removeFromLeft(8);
    b_.list.setBounds(area);
    report_.setBounds(bottom.withTrimmedTop(6));
}

void LibraryManagerComponent::chooseFolder(const juce::String& title, std::function<void(juce::File)> then) {
    chooser_ = std::make_shared<juce::FileChooser>(title, juce::File());
    auto c = chooser_;
    c->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                   [c, then, alive = std::weak_ptr<bool>(alive_)](const juce::FileChooser& fc) {
                       if (!alive.expired() && fc.getResult() != juce::File()) then(fc.getResult());
                   });
}

void LibraryManagerComponent::openLibrary(char side, const juce::File& dir) {
    if (working()) return;
    ++jobs_;
    auto& p = pane(side);
    p.header.setText(juce::String("Library ") + side + ": reading " + dir.getFullPathName() + " (read-only)...", juce::dontSendNotification);
    refresh();
    std::weak_ptr<bool> alive = alive_;
    std::thread([this, alive, stop = stop_, side, d = dir.getFullPathName().toStdString()] {
        auto L = std::make_shared<const lib::Library>(lib::load(d, [alive, stop] { return !alive.expired() && !*stop; }));
        juce::MessageManager::callAsync([this, alive, side, L] {
            if (alive.expired()) return;
            --jobs_;
            auto& p = pane(side);
            if (L->ok) {
                p.lib = L;
                p.selected = -1;
                plan_.clear();
                ++planRev_;   // steps name items by index into the libraries they were made for
            }
            view_ = View::Details;
            p.header.setText(juce::String("Library ") + side + ": " + u8(L->ok ? L->name + "  ·  " + lib::formatName(L->format) + "  ·  " +
                                                                                     std::to_string(L->items.size()) + " items  ·  Read-only source"
                                                                                : "not opened: " + L->error),
                             juce::dontSendNotification);
            refilter();
        });
    }).detach();
}

void LibraryManagerComponent::refilter() {
    const juce::ScopedValueSetter<bool> q(quiet_, true);
    for (auto* p : {&a_, &b_}) {
        p->rows = p->lib ? lib::search(*p->lib, search_.getText().toStdString()) : std::vector<size_t>{};
        p->list.updateContent();
        const auto it = std::find(p->rows.begin(), p->rows.end(), size_t(p->selected));
        if (it == p->rows.end()) { p->selected = -1; p->list.deselectAllRows(); }
        else p->list.selectRow(int(it - p->rows.begin()), true, true);
        p->list.repaint();
    }
    refresh();
}

void LibraryManagerComponent::selectRow(char side, int row) { pane(side).list.selectRow(row); }

juce::String LibraryManagerComponent::paneText(char side) const {
    const auto& p = pane(side);
    juce::StringArray s;
    for (size_t i : p.rows) s.add(u8(lib::itemLine(p.lib->items[i])));
    return s.joinIntoString("\n");
}

juce::String LibraryManagerComponent::headerText(char side) const { return pane(side).header.getText(); }

juce::String LibraryManagerComponent::unavailable(Action a) const {
    if (working()) return "Needs: the running job to finish";
    const auto& act = pane(active_);
    const auto& other = pane(active_ == 'A' ? 'B' : 'A');
    switch (a) {
        case Action::Compare: return a_.lib && b_.lib ? "" : "Needs: a library open on both sides";
        case Action::Duplicates: return a_.lib || b_.lib ? "" : "Needs: an open library";
        case Action::Copy:
        case Action::Move:
        case Action::Rename:
        case Action::SwapBanks:
            if (!act.item()) return "Needs: an item selected in a list";
            if (!other.lib) return juce::String("Needs: library ") + other.side + " open (a plan is between two libraries)";
            if (a == Action::Copy || a == Action::Move) return "";
            return value_.getText().trim().isNotEmpty() ? ""
                   : a == Action::Rename ? "Needs: the new name in the value field" : "Needs: the other bank in the value field (e.g. STYLE/USER05)";
        case Action::RemoveStep:
        case Action::ClearPlan:
        case Action::Export: return plan_.empty() ? "Needs: a plan step (Copy, Move, Rename or Swap)" : "";
    }
    return "";
}

void LibraryManagerComponent::addStep(Action a) {
    if (unavailable(a).isNotEmpty()) return;
    const auto& p = pane(active_);
    const auto* target = pane(active_ == 'A' ? 'B' : 'A').item();
    lib::Step s;
    s.side = active_;
    s.item = size_t(p.selected);
    switch (a) {
        case Action::Copy: s.op = lib::Op::Copy; s.bank = target ? target->bank : ""; break;
        case Action::Move: s.op = lib::Op::Move; s.bank = target ? target->bank : ""; break;
        case Action::Rename: s.op = lib::Op::Rename; s.text = value_.getText().toStdString(); break;
        case Action::SwapBanks: s.op = lib::Op::BankSwap; s.bank = p.item()->bank; s.text = value_.getText().trim().toStdString(); break;
        default: return;
    }
    plan_.push_back(s);
    ++planRev_;
    showView(View::Plan);
}

void LibraryManagerComponent::showView(View v) {
    if (v == View::Compare) {
        if (unavailable(Action::Compare).isNotEmpty()) return;
        ++jobs_;
        view_ = v;
        report_.setText("Comparing (read-only)...", false);
        refresh();
        std::weak_ptr<bool> alive = alive_;
        std::thread([this, alive, A = a_.lib, B = b_.lib] {
            auto text = std::make_shared<std::string>(lib::compareView(*A, *B, lib::compare(*A, *B)));
            juce::MessageManager::callAsync([this, alive, text] {
                if (alive.expired()) return;
                --jobs_;
                report_.setText(u8(*text), false);
                refresh();
            });
        }).detach();
        return;
    }
    if (v == View::Duplicates && unavailable(Action::Duplicates).isNotEmpty()) return;
    view_ = v;
    refresh();
}

void LibraryManagerComponent::exportPlan(const juce::File& dest) {
    if (unavailable(Action::Export).isNotEmpty() || !a_.lib || !b_.lib) return;
    view_ = View::Plan;
    if (!enqueueJob) { report_.setText("Plan NOT written: no export job queue.\nNothing was written to either library.", false); return; }
    const juce::String d = dest.getFullPathName();
    std::weak_ptr<bool> alive = alive_;
    // E6 job: D18 refuses an output in or around either library, staged + verified before one publish, report beside it
    const auto token = planJobs_.start();   // (defensive: a port could land `done` inside enqueueJob)
    const auto id = enqueueJob(jobs::libraryPlan(a_.lib, b_.lib, plan_, d.toStdString()), [this, alive, token](const jobs::JobState& j) {
        if (alive.expired()) return;
        planJobs_.land(token);   // a re-run from the job list (same token) is only shown
        refresh(false);   // keep the export result: no view report replaces it
        const auto badge = u8(ui::studio::badgeLine(j.level));
        report_.setText(j.phase == jobs::Phase::Done
                            ? "Plan written: " + u8(j.dest) + "\n" + badge + "   " + u8(j.verifyText) + "\nExport job #" + juce::String(j.id) +
                                  ", report: " + u8(j.reportPath) + "\nEXPORT-ONLY: carry the steps out on the keyboard, then check with Compare.\n\n" + u8(j.writerText)
                            : "Plan NOT written: " + u8(j.reason) + "\n(export job #" + juce::String(j.id) + " " + jobs::phaseName(j.phase) + " at " +
                                  jobs::phaseName(j.stoppedAt) + ")\nNothing was written to either library.",
                        false);
    });
    if (!*token) return;   // already landed
    report_.setText("Plan export queued as export job #" + juce::String(id) + " (Import / Export Center lists every job): " + d, false);
    refresh(false);
}

void LibraryManagerComponent::stopWork() {
    stop_->store(true);   // the running scan stops at its next check and lands as "not opened"
    stop_ = std::make_shared<std::atomic<bool>>(false);
    ++reportGen_;         // the view report in flight is dropped; an empty job makes it stale so its loops stop
    deliveredGen_ = reportGen_;
    shownKey_.clear();
    reports_.submit([](const std::function<bool()>&) {});
}

// The report text of the Details, Duplicates and Plan views, from immutable snapshots: runs on a worker thread.
static std::string reportFor(LibraryManagerComponent::View view, std::shared_ptr<const lib::Library> A, std::shared_ptr<const lib::Library> B,
                             const std::vector<lib::Step>& plan, char active, int selected, const std::function<bool()>& stale) {
    const std::function<bool()> going = [&] { return !stale(); };
    using View = LibraryManagerComponent::View;
    std::string t;
    const auto& act = active == 'A' ? A : B;
    switch (view) {
        case View::Details:
            if (act && selected >= 0) return std::string("Library ") + active + ": " + lib::itemDetails(*act, act->items[size_t(selected)]);
            for (const auto& [side, L] : {std::pair{'A', A}, std::pair{'B', B}})
                if (L) t += std::string("Library ") + side + ": " + lib::summary(*L) + "Dependencies (only where evidenced):\n" + lib::dependencyView(L->deps) +
                            lib::bankView(*L) + "\n";
            return t.empty() ? "No library open. Open Library A... and Open Library B... read a Pa5X KST folder or a Pa-family SET folder each. "
                               "Nothing in them is changed."
                             : t;
        case View::Duplicates:
            for (const auto& [side, L] : {std::pair{'A', A}, std::pair{'B', B}})
                if (L) t += std::string("Library ") + side + ": " + lib::duplicatesView(*L, lib::duplicates(*L, going)) + "\n";
            return t;
        case View::Plan:
            return A && B ? lib::planReport(*A, *B, plan, lib::evaluate(*A, *B, plan, going)) : "A plan needs a library open on both sides.";
        case View::Compare: break;
    }
    return t;
}

void LibraryManagerComponent::refresh(bool report) {
    for (auto& [a, b] : actions_) setUnavailable(*b, unavailable(a));
    setUnavailable(*openA_, working() ? "Needs: the running job to finish" : "");
    setUnavailable(*openB_, working() ? "Needs: the running job to finish" : "");
    if (working() || !report || view_ == View::Compare) {   // a job (or Compare) writes the report when it lands
        ++reportGen_;               // drop any report still in flight
        deliveredGen_ = reportGen_;
        shownKey_.clear();          // the text no longer shows a view report: the next one is recomputed
        return;
    }
    // Only what the visible view depends on: a value-field edit does not restart Duplicates, a selection not Plan.
    const auto* sel = pane(active_).item();
    const auto ptr = [](const auto& p) { return std::to_string(reinterpret_cast<uintptr_t>(p.get())); };
    std::string key = std::to_string(int(view_)) + "|" + ptr(a_.lib) + "|" + ptr(b_.lib);
    if (view_ == View::Details) key += std::string("|") + active_ + "|" + std::to_string(sel ? pane(active_).selected : -1);
    if (view_ == View::Plan) key += "|" + std::to_string(planRev_);
    if (key == shownKey_) return;
    shownKey_ = key;
    if (!a_.lib && !b_.lib) {   // no library open: a fixed text, no worker (else computing() reads as running work, e.g. at quit)
        deliveredGen_ = ++reportGen_;
        report_.setText(u8(reportFor(view_, nullptr, nullptr, plan_, active_, -1, [] { return false; })), false);
        return;
    }
    // duplicates() and evaluate() can be slow on a large library: one background worker, latest request wins, a superseded
    // request stops inside their loops (library/latest_worker.h)
    const auto gen = ++reportGen_;
    std::weak_ptr<bool> alive = alive_;
    const bool queued = reports_.submit([this, alive, gen, v = view_, A = a_.lib, B = b_.lib, plan = plan_, side = active_,
                                         selIdx = sel ? pane(active_).selected : -1](const std::function<bool()>& stale) {
        auto text = std::make_shared<std::string>(reportFor(v, A, B, plan, side, selIdx, stale));
        if (stale()) return;   // superseded: its (possibly partial) text is never shown
        juce::MessageManager::callAsync([this, alive, gen, text] {
            if (alive.expired() || gen != reportGen_) return;
            deliveredGen_ = gen;
            report_.setText(u8(*text), false);
        });
    });
    if (!queued) { deliveredGen_ = gen; report_.setText(u8(reports_.error()), false); }
}

}  // namespace ai_arranger::app
