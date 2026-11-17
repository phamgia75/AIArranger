#pragma once

// BL-A8: the macOS menu bar (Edit, View) for the AI Music Studio, so its commands are findable from the menu bar
// (UX accessibility audit §3). Plain items: JUCE gives native key equivalents only to ApplicationCommandManager items,
// and those would take Cmd-Z from a focused text field; the keys stay with StudioComponent::keyPressed.
// shortcut: no key equivalents shown in the menus; add an ApplicationCommandManager that hands Cmd-Z to a focused text field first.

#include <juce_gui_extra/juce_gui_extra.h>
#include "LiveHostComponent.h"
#include "StudioComponent.h"

namespace ai_arranger::app {

class StudioMenu : public juce::MenuBarModel {
public:
    explicit StudioMenu(LiveHostComponent& host) : host_(host) {}

    juce::StringArray getMenuBarNames() override { return {"Edit", "View"}; }

    juce::PopupMenu getMenuForIndex(int index, const juce::String&) override {
        juce::PopupMenu m;
        auto* st = host_.studio();
        if (index == 0) {
            m.addItem(kUndo, "Undo", st && st->canUndoOnPage(false));
            m.addItem(kRedo, "Redo", st && st->canUndoOnPage(true));
        } else {
            for (int i = 0; i < StudioComponent::kDomains; ++i) m.addItem(kDomain + i, StudioComponent::domainName(sidebarOrder(i)));
        }
        return m;
    }

    void menuItemSelected(int id, int) override {
        if (id == kUndo || id == kRedo) { if (auto* st = host_.studio()) st->undoOnPage(id == kRedo); return; }
        if (id >= kDomain && id < kDomain + StudioComponent::kDomains)
            if (auto* st = host_.showStudio()) st->showDomain(sidebarOrder(id - kDomain));
    }

    static StudioComponent::Domain sidebarOrder(int i) {   // the sidebar's order, the one Cmd-1...Cmd-5 follow
        using D = StudioComponent::Domain;
        static const D order[] = {D::Library, D::SampleLab, D::StyleStudio, D::ImportExport, D::Validation};
        return order[i];
    }

private:
    enum { kUndo = 1, kRedo = 2, kDomain = 10 };
    LiveHostComponent& host_;
};

} // namespace ai_arranger::app
