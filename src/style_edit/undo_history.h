#ifndef AI_ARRANGER_STYLE_EDIT_UNDO_HISTORY_H
#define AI_ARRANGER_STYLE_EDIT_UNDO_HISTORY_H

#include <cstddef>
#include <deque>
#include <string>
#include <utility>

// Bounded undo/redo for a non-real-time document. Each executed edit command is
// recorded as {label, document state before it}; undo swaps that state back in
// and keeps the current one for redo. Restoring a snapshot is an exact inverse
// for every op, so no op needs a hand-written (and possibly wrong) inverse.
// shortcut: whole-document snapshots (a style is ~24 bytes/event, so 64 steps of a
// 20k-event style is ~30 MB); switch to per-section patches if styles grow larger.

namespace ai_arranger::style_edit {

template <class Doc>
class UndoHistory {
public:
    explicit UndoHistory(size_t limit = 64) : limit_(limit ? limit : 1) {}

    // A new command was executed: `before` is the state it replaced. Clears redo.
    void record(std::string label, Doc before) {
        undo_.push_back({std::move(label), std::move(before)});
        if (undo_.size() > limit_) undo_.pop_front();   // oldest step falls off
        redo_.clear();
    }

    bool undo(Doc& current) { return step(undo_, redo_, current); }
    bool redo(Doc& current) { return step(redo_, undo_, current); }

    bool canUndo() const noexcept { return !undo_.empty(); }
    bool canRedo() const noexcept { return !redo_.empty(); }
    size_t undoDepth() const noexcept { return undo_.size(); }
    size_t redoDepth() const noexcept { return redo_.size(); }
    std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }

private:
    struct Command {
        std::string label;
        Doc state;
    };

    static bool step(std::deque<Command>& from, std::deque<Command>& to, Doc& current) {
        if (from.empty()) return false;
        Command c = std::move(from.back());
        from.pop_back();
        std::swap(current, c.state);
        to.push_back(std::move(c));
        return true;
    }

    size_t limit_;
    std::deque<Command> undo_, redo_;
};

} // namespace ai_arranger::style_edit

#endif // AI_ARRANGER_STYLE_EDIT_UNDO_HISTORY_H
