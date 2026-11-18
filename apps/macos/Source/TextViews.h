#pragma once

// Text panes that do not re-lay out all their text on the message thread (#41, #44): juce::TextEditor::setText
// (JUCE 8.0.4) costs about 23-50 us per character, 10 s for a 440 KB report. Both keep the text in a CodeDocument
// and draw only the visible lines.

#include <juce_gui_extra/juce_gui_extra.h>
#include "ui/sound/sound_conversion_model.h"   // ui::wrapLines

namespace ai_arranger::app {

// Read-only text that lays out only the visible lines: juce::TextEditor::setText re-lays out all of
// it on the message thread (about 200 ms for the 5 KB mapping table, 600 ms on CI runners).
class TextView : public juce::Component {
public:
    TextView() { ed_.setReadOnly(true); ed_.setLineNumbersShown(false); addAndMakeVisible(ed_); }
    void setText(const juce::String& t) { raw_ = t; cols_ = -1; layout(); }
    void setText(const juce::String& t, bool /*notify*/) { setText(t); }   // TextEditor-style call sites; a view sends no notification
    juce::String getText() const { return raw_; }
    juce::String shownText() const { return doc_.getAllContent(); }
    bool canUndo() { return doc_.getUndoManager().canUndo(); }
    void clear() { setText({}); }
    void setFont(const juce::Font& f) { ed_.setFont(f); }
    void setWrap(bool w) { wrap_ = w; }   // prose: wrap to the width instead of scrolling sideways
    void setTitle(const juce::String& t) { Component::setTitle(t); ed_.setTitle(t); }   // the editor is what VoiceOver reads
    void setColours(juce::Colour background, juce::Colour text) {
        ed_.setColour(juce::CodeEditorComponent::backgroundColourId, background);
        ed_.setColour(juce::CodeEditorComponent::defaultTextColourId, text);
    }
    juce::CodeEditorComponent& editor() { return ed_; }   // tests: selection, copy command, scrolling, accessibility
    void resized() override { ed_.setBounds(getLocalBounds()); layout(); }
private:
    void layout() {
        const int cols = wrap_ ? std::max(20, int((getWidth() - 24) / std::max(1.0f, ed_.getCharWidth()))) : 0;
        if (cols == cols_) return;
        cols_ = cols;
        doc_.replaceAllContent(cols ? juce::String(ui::wrapLines(raw_.toStdString(), size_t(cols))) : raw_);
        doc_.clearUndoHistory();   // read-only view: no undo, and old texts must not pile up
        ed_.scrollToLine(0);
    }
    juce::String raw_;
    bool wrap_ = false;
    int cols_ = -1;
    juce::CodeDocument doc_;
    juce::CodeEditorComponent ed_{doc_, nullptr};
};

// Editable text on the same document model as TextView, for the key and zone maps. juce::TextEditor::setText
// (JUCE 8.0.4) costs about 50 us per character: 0.3 s for the 6 KB zone map of a 64-zone Sound, 2.6 s when the
// editor has no width yet (#41). Offers the TextEditor calls this tab and its smoke test use.
class EditView : public juce::Component, private juce::CodeDocument::Listener {
public:
    EditView() { ed_.setLineNumbersShown(false); doc_.addListener(this); addAndMakeVisible(ed_); }
    ~EditView() override { doc_.removeListener(this); }
    std::function<void()> onTextChange;   // user edits, and setText(t) with notify
    void setText(const juce::String& t, bool notify = true) {
        if (t == doc_.getAllContent()) return;   // as TextEditor: no change, no notification
        quiet_ = true; doc_.replaceAllContent(t); quiet_ = false;
        doc_.clearUndoHistory();
        ed_.scrollToLine(0);
        repaint();
        if (notify && onTextChange) onTextChange();
    }
    juce::String getText() const { return doc_.getAllContent(); }
    bool isEmpty() const { return doc_.getNumCharacters() == 0; }
    void setFont(const juce::Font& f) { ed_.setFont(f); }
    void setTitle(const juce::String& t) { Component::setTitle(t); ed_.setTitle(t); }   // the editor is what VoiceOver reads
    void setTextToShowWhenEmpty(const juce::String& t, juce::Colour c) { empty_ = t; emptyColour_ = c; repaint(); }
    void resized() override { ed_.setBounds(getLocalBounds()); }
    void paintOverChildren(juce::Graphics& g) override {
        if (!isEmpty() || empty_.isEmpty()) return;
        g.setColour(emptyColour_);
        g.setFont(ed_.getFont());
        g.drawFittedText(empty_, getLocalBounds().reduced(8, 4), juce::Justification::topLeft, 8);
    }
private:
    void codeDocumentTextInserted(const juce::String&, int) override { changed(); }
    void codeDocumentTextDeleted(int, int) override { changed(); }
    void changed() { repaint(); if (!quiet_ && onTextChange) onTextChange(); }
    juce::CodeDocument doc_;
    juce::CodeEditorComponent ed_{doc_, nullptr};
    juce::String empty_;
    juce::Colour emptyColour_;
    bool quiet_ = false;
};

} // namespace ai_arranger::app
