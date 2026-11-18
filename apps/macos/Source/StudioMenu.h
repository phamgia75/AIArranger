#pragma once

// BL-A8: the macOS menu bar (Edit, View) for the AI Music Studio, so its commands and their shortcuts are findable from
// the menu bar (UX accessibility audit §3). The items are ApplicationCommandManager commands, so the menus show their key
// equivalents (Cmd-Z, Shift-Cmd-Z, Cmd-1...Cmd-5). A native key equivalent reaches the app before the focused view does,
// so Undo / Redo first goes to a focused text field's own undo (owner: native text-field Undo is preserved) and only then
// to the Studio page on screen.
// shortcut: no Settings (Cmd-,) or Window menu: there is no Settings page and one Studio window; add them with the page.

#include <juce_gui_extra/juce_gui_extra.h>
#include "LiveHostComponent.h"
#include "StudioComponent.h"

namespace ai_arranger::app {

class StudioMenu : public juce::MenuBarModel, public juce::ApplicationCommandTarget {
public:
    enum Command { kUndo = 0x7a01, kRedo = 0x7a02, kDomain = 0x7a10 };   // kDomain + sidebar index 0..4

    explicit StudioMenu(LiveHostComponent& host)
        : StudioMenu([&host] { return host.studio(); }, [&host] { return host.showStudio(); }) {}
    // `studio`: the Studio if it exists; `showStudio`: opens it. `focused` overrides the focused component (tests only).
    StudioMenu(std::function<StudioComponent*()> studio, std::function<StudioComponent*()> showStudio,
               std::function<juce::Component*()> focused = {})
        : studio_(std::move(studio)), showStudio_(std::move(showStudio)), focused_(std::move(focused)) {
        cm_.registerAllCommandsForTarget(this);
        cm_.setFirstCommandTarget(this);
    }

    juce::ApplicationCommandManager& commands() { return cm_; }

    // ── menu bar ──
    juce::StringArray getMenuBarNames() override { return {"Edit", "View"}; }
    juce::PopupMenu getMenuForIndex(int index, const juce::String&) override {
        juce::PopupMenu m;
        if (index == 0) {
            m.addCommandItem(&cm_, kUndo);
            m.addCommandItem(&cm_, kRedo);
        } else {
            for (int i = 0; i < StudioComponent::kDomains; ++i) m.addCommandItem(&cm_, kDomain + i);
        }
        return m;
    }
    void menuItemSelected(int, int) override {}   // commands run through perform()

    // ── commands ──
    ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& c) override {
        c.add(kUndo);
        c.add(kRedo);
        for (int i = 0; i < StudioComponent::kDomains; ++i) c.add(kDomain + i);
    }
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override {
        const auto cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;
        if (id == kUndo || id == kRedo) {
            const bool redo = id == kRedo;
            info.setInfo(redo ? "Redo" : "Undo", redo ? "Redo the last undone change on this page" : "Undo the last change on this page", "Edit", 0);
            info.addDefaultKeypress('z', redo ? cmd | shift : cmd);
            // Always enabled: Cocoa only fires a key equivalent of an item that is enabled when the menu was last built, and the menu is
            // not rebuilt when focus moves into a text field (Codex r3 P1). perform() decides what, if anything, to undo.
            (void) redo;
        } else if (id >= kDomain && id < kDomain + StudioComponent::kDomains) {
            const int i = id - kDomain;
            const juce::String name = StudioComponent::domainName(sidebarOrder(i));
            info.setInfo(name, "Open " + name, "View", 0);
            info.addDefaultKeypress('1' + i, cmd);
        }
    }
    bool perform(const InvocationInfo& info) override {
        if (info.commandID == kUndo || info.commandID == kRedo)
            return undoFrom(focused(), studio_(), info.commandID == kRedo);
        if (info.commandID >= kDomain && info.commandID < kDomain + StudioComponent::kDomains)
            if (auto* st = showStudio_()) { st->showDomain(sidebarOrder(info.commandID - kDomain)); return true; }
        return false;
    }

    // A focused editable text field (TextEditor or CodeEditor) keeps its own undo; otherwise the Studio page on screen.
    static bool undoFrom(juce::Component* focused, StudioComponent* st, bool redo) {
        if (auto* te = dynamic_cast<juce::TextEditor*>(focused); te && !te->isReadOnly()) { redo ? te->redo() : te->undo(); return true; }
        if (auto* ce = dynamic_cast<juce::CodeEditorComponent*>(focused); ce && !ce->isReadOnly()) { redo ? ce->redo() : ce->undo(); return true; }
        return st && st->undoOnPage(redo);
    }

    static StudioComponent::Domain sidebarOrder(int i) {   // the sidebar's order, the one Cmd-1...Cmd-5 follow
        using D = StudioComponent::Domain;
        static const D order[] = {D::Library, D::SampleLab, D::StyleStudio, D::ImportExport, D::Validation};
        return order[i];
    }

private:
    juce::Component* focused() const { return focused_ ? focused_() : juce::Component::getCurrentlyFocusedComponent(); }
    std::function<StudioComponent*()> studio_, showStudio_;
    std::function<juce::Component*()> focused_;
    juce::ApplicationCommandManager cm_;
};

} // namespace ai_arranger::app
