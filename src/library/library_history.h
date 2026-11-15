// Staged change set for the Library Manager: plan steps accumulate with multi-step undo/redo (style_edit::UndoHistory,
// whole-plan snapshots) before Execute Plan. Pure memory: nothing here reads or writes a file; only Execute Plan (E8
// library_apply.h through the E6/E9 job runner) writes, and only a NEW library.
#ifndef AI_ARRANGER_LIBRARY_LIBRARY_HISTORY_H
#define AI_ARRANGER_LIBRARY_LIBRARY_HISTORY_H

#include "library/library_plan.h"
#include "style_edit/undo_history.h"

namespace ai_arranger::library {

// A short label for one step, from the library it starts in (used for the undo/redo labels).
inline std::string stepLabel(const Library& L, const Step& s) {
    std::string t = std::string(opName(s.op)) + " in " + s.side + ": ";
    if (s.op == Op::BankSwap) return t + s.bank + " <-> " + s.text;
    t += s.item < L.items.size() ? "\"" + trimmed(L.items[s.item].name) + "\"" : "item " + std::to_string(s.item);
    if (s.wholeFile) t += " (whole bank file)";
    if (s.op == Op::Rename) t += " -> \"" + s.text + "\"";
    else t += " -> " + std::string(1, s.side == 'A' ? 'B' : 'A') + ":" + (s.bank.empty() ? "same bank" : s.bank);
    return t;
}

class PlanHistory {
public:
    const std::vector<Step>& steps() const { return steps_; }
    bool empty() const { return steps_.empty(); }

    void add(Step s, const std::string& label) { record("add " + label); steps_.push_back(std::move(s)); }
    bool removeLast() {
        if (steps_.empty()) return false;
        record("remove step " + std::to_string(steps_.size()));
        steps_.pop_back();
        return true;
    }
    bool clear() {
        if (steps_.empty()) return false;
        record("clear " + std::to_string(steps_.size()) + " step(s)");
        steps_.clear();
        return true;
    }
    bool undo() { return h_.undo(steps_); }
    bool redo() { return h_.redo(steps_); }
    // The libraries changed: steps name items by index, so the plan and its history no longer apply.
    void reset() { steps_.clear(); h_ = style_edit::UndoHistory<std::vector<Step>>(); }

    bool canUndo() const { return h_.canUndo(); }
    bool canRedo() const { return h_.canRedo(); }
    std::string undoLabel() const { return h_.undoLabel(); }
    std::string redoLabel() const { return h_.redoLabel(); }
    std::string text() const {
        return "History: " + std::to_string(h_.undoDepth()) + " edit(s) to undo" + (h_.canUndo() ? " (last: " + h_.undoLabel() + ")" : "") + ", " +
               std::to_string(h_.redoDepth()) + " to redo" + (h_.canRedo() ? " (next: " + h_.redoLabel() + ")" : "") +
               ". Staged only: nothing is written until Execute Plan.\n";
    }

private:
    void record(std::string label) { h_.record(std::move(label), steps_); }
    std::vector<Step> steps_;
    style_edit::UndoHistory<std::vector<Step>> h_;
};

}  // namespace ai_arranger::library

#endif
