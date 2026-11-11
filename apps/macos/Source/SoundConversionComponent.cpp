#include "SoundConversionComponent.h"
#include <thread>

namespace ai_arranger::app {

using ui::SoundConversionModel;

void KeyMapView::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff222228));
    const auto r = getLocalBounds().toFloat().reduced(4);
    const float labelW = 120, kbH = 20, octH = 13, keyW = (r.getWidth() - labelW) / 128.0f, x0 = r.getX() + labelW;
    // Keyboard strip at the bottom, octave labels in their own row under it (readable on any key).
    const float kbY = r.getBottom() - kbH - octH;
    for (int k = 0; k < 128; ++k) {
        const bool black = juce::String("010100101010")[k % 12] == '1';
        g.setColour(black ? juce::Colour(0xff30303a) : juce::Colour(0xffd8d8e0));
        g.fillRect(x0 + k * keyW, kbY, keyW - (black ? 0.0f : 0.5f), kbH);
    }
    g.setColour(juce::Colour(0xffb8b8c0));
    g.setFont(juce::FontOptions(11.0f));
    for (int k = 0; k < 128; k += 12)
        g.drawText(SoundConversionModel::key(uint8_t(k)), juce::Rectangle<float>(x0 + k * keyW, kbY + kbH + 1, 40, octH - 1), juce::Justification::left);
    if (!in_ || in_->layers.empty()) {
        g.setColour(juce::Colour(0xff9090a0));
        g.drawText(empty_, r.withBottom(kbY), juce::Justification::centred);
        return;
    }
    const size_t n = in_->layers.size();
    const float rowH = std::min(34.0f, (kbY - r.getY() - 4) / float(n));
    for (size_t i = 0; i < n; ++i) {   // loudest layer on top
        const auto& L = in_->layers[n - 1 - i];
        const float y = r.getY() + i * rowH;
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(juce::String(L.name) + "  vel " + juce::String(L.velLo) + "-" + juce::String(L.velHi),
                   juce::Rectangle<float>(r.getX(), y, labelW - 6, rowH), juce::Justification::centredLeft, true);
        for (size_t z = 0; z < L.zones.size(); ++z) {
            const auto& Z = L.zones[z];
            const auto box = juce::Rectangle<float>(x0 + Z.lo * keyW, y + 3, (Z.hi - Z.lo + 1) * keyW, rowH - 6);
            g.setColour((z % 2 ? juce::Colour(0xff3f7fbf) : juce::Colour(0xff5f9fdf)).withAlpha(0.85f));
            g.fillRoundedRectangle(box, 3);
            g.setColour(juce::Colour(0xfff0c040));   // root: a tick under the zone, so a one-key zone still shows its colour
            g.fillRect(x0 + Z.root * keyW + keyW * 0.25f, y + rowH - 9, std::max(2.0f, keyW * 0.5f), 4.0f);
        }
    }
}

SoundConversionComponent::SoundConversionComponent(std::function<void(const juce::File&)> openInResources)
    : openInResources_(std::move(openInResources)) {
    for (auto* l : {&folder_, &status_, &pluginNote_, &nameLbl_, &orderLbl_, &splitsLbl_})
        { l->setColour(juce::Label::textColourId, juce::Colour(0xffb8b8c0)); addAndMakeVisible(*l); }
    folder_.setText("No folder chosen. A KRONOS folder (.KMP + .KSF from Save Sampling Data), .KSF samples alone, or WAVs named <layer>_<RootKey>_<LowKey>_<HighKey>.wav.",
                    juce::dontSendNotification);
    pluginNote_.setText("AU/VST instruments: use the Sample Plugin tab.",
                        juce::dontSendNotification);
    mapping_.setMultiLine(true);
    mapping_.setReturnKeyStartsNewLine(true);
    mapping_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
    mapping_.setTitle("Key mapping (proposed; edit and press Preview)");
    mapping_.setTextToShowWhenEmpty("Press Preview: AIArranger measures each sample's pitch and proposes a key map here.", juce::Colour(0xff9090a0));
    addChildComponent(mapping_);
    for (auto* t : {&name_, &order_, &splits_, &mapping_}) {
        addAndMakeVisible(*t);
        t->onTextChange = [this] {   // the preview no longer matches the settings: Generate must not write it
            ++generation_;
            model_.invalidate();
            generateBtn_.setEnabled(false);
            if (model_.instrument().layers.size()) status_.setText("Settings changed: press Preview again.", juce::dontSendNotification);
        };
    }
    for (auto* t : {&preview_, &recipe_}) {
        t->setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
        t->setWrap(t == &recipe_);
        addAndMakeVisible(*t);
    }
    addAndMakeVisible(map_);
    folderBtn_.onClick = [this] {
        chooser_ = std::make_shared<juce::FileChooser>("Choose a KRONOS .KMP/.KSF folder or a folder of WAV samples");
        auto c = chooser_;
        c->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                       [c, this, alive = std::weak_ptr<bool>(alive_)](const juce::FileChooser& fc) {
                           if (!alive.expired() && fc.getResult() != juce::File()) inspectFolder(fc.getResult());
                       });
    };
    previewBtn_.onClick = [this] { buildPreview(); };
    generateBtn_.onClick = [this] {
        if (!model_.built()) { status_.setText("Press Preview first.", juce::dontSendNotification); return; }
        chooser_ = std::make_shared<juce::FileChooser>("Choose where to create the package folder (a local folder)");
        auto c = chooser_;
        c->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                       [c, this, alive = std::weak_ptr<bool>(alive_)](const juce::FileChooser& fc) {
                           if (alive.expired() || fc.getResult() == juce::File()) return;
                           generate(fc.getResult().getChildFile(juce::File::createLegalFileName(name_.getText().trim())));
                       });
    };
    for (auto* b : {&folderBtn_, &previewBtn_, &generateBtn_}) addAndMakeVisible(*b);
    generateBtn_.setEnabled(false);
}

SoundConversionComponent::~SoundConversionComponent() { *alive_ = false; }

bool SoundConversionComponent::inspectFolder(const juce::File& dir) {
    std::string err;
    ++generation_;   // a preview still reading the previous folder is dropped
    map_.setInstrument(nullptr);
    generateBtn_.setEnabled(false);
    if (!model_.inspect(dir.getFullPathName().toStdString(), &err)) {
        status_.setText("Cannot use this folder: " + juce::String(err), juce::dontSendNotification);
        return false;
    }
    mapping_.setText({}, false);
    mapping_.setVisible(model_.ksf());
    resized();
    juce::String s = dir.getFullPathName() + "  -  " + (model_.korg() ? "KRONOS multisamples, " : "") + juce::String(model_.layers().size()) + " layer(s): ";
    if (model_.ksf())
        s = dir.getFullPathName() + "  -  " + juce::String(model_.layers().front().files) +
            " Korg .KSF samples without a .KMP: Preview measures each root and proposes key zones (not the original map).";
    else for (const auto& l : model_.layers())
        s += juce::String(l.prefix) + " (" + juce::String(l.files) + (model_.korg() ? " zones, " : " files, ") + SoundConversionModel::key(l.lo) + "-" + SoundConversionModel::key(l.hi) + ")  ";
    folder_.setText(s, juce::dontSendNotification);
    // Programmatic: no change notification (JUCE delivers it later and it would invalidate the next preview).
    if (name_.isEmpty()) name_.setText(dir.getFileName().substring(0, 20), false);
    order_.setText(model_.proposedOrder(), false);
    splits_.setText(model_.proposedSplits(), false);
    status_.setText(model_.ksf() ? "Name the layer, then press Preview to propose the key map."
                                 : "Put the layers in order from softest to loudest, check the splits, then press Preview.", juce::dontSendNotification);
    return true;
}

void SoundConversionComponent::buildPreview() {
    if (model_.folder().empty()) { status_.setText("Choose a folder first.", juce::dontSendNotification); return; }
    ++jobs_;
    previewBtn_.setEnabled(false);
    generateBtn_.setEnabled(false);
    map_.setInstrument(nullptr);
    status_.setText("Reading samples...", juce::dontSendNotification);
    std::weak_ptr<bool> alive = alive_;
    const auto gen = ++generation_;
    std::thread([this, alive, gen, dir = model_.folder(), name = name_.getText().trim().toStdString(),
                 order = order_.getText().toStdString(), splits = splits_.getText().toStdString(),
                 mapping = std::make_shared<std::string>(mapping_.getText().toStdString()), notes = std::make_shared<std::string>()] {
        auto in = std::make_shared<media::sound::Instrument>();
        auto texts = std::make_shared<std::array<std::string, 2>>();   // checks + preview, recipe
        std::string err;
        const bool ok = SoundConversionModel::decode(dir, name, order, splits, *in, &err, mapping.get(), notes.get());
        if (ok) {   // text building is slow on big Sounds too: keep it off the message thread
            const auto checks = SoundConversionModel::checksOf(*in);
            std::string c = checks.empty() ? "Checks: nothing to report.\n\n" : "Checks (" + std::to_string(checks.size()) + "):\n";
            for (const auto& w : checks) c += "  - " + w + "\n";
            (*texts)[0] = c + "\n" + SoundConversionModel::previewOf(*in);
            (*texts)[1] = media::sound::formatRecipe(media::sound::recipeFromSamples(*in));
        }
        juce::MessageManager::callAsync([this, alive, gen, in, texts, ok, err, mapping, notes] {
            if (alive.expired()) return;
            --jobs_;
            previewBtn_.setEnabled(true);
            if (gen != generation_) return;   // another folder was chosen meanwhile
            showPreview(ok ? in.get() : nullptr, *texts, err, *mapping, *notes);
        });
    }).detach();
}

void SoundConversionComponent::showPreview(media::sound::Instrument* in, const std::array<std::string, 2>& texts, const std::string& err,
                                           const std::string& mapping, const std::string& ksfNotes) {
    ksfNotes_ = ksfNotes;
    if (model_.ksf() && mapping_.getText().toStdString() != mapping)
        mapping_.setText(mapping, false);   // the proposal (no change notification: it is what the preview was built from)
    if (!in) {
        preview_.clear(); recipe_.clear();
        status_.setText("Cannot build: " + juce::String(err), juce::dontSendNotification);
        return;
    }
    if (auto old = model_.adopt(std::move(*in)))   // the model no longer holds it: free its audio off the message thread
        std::thread([o = std::move(old)] {}).detach();
    map_.setInstrument(&model_.instrument());
    preview_.setText(texts[0]);
    recipe_.setText(texts[1]);
    generateBtn_.setEnabled(true);
    status_.setText("Preview ready. Generate writes a new package folder; nothing is sent to the instrument.", juce::dontSendNotification);
}

void SoundConversionComponent::generate(const juce::File& outDir) {
    if (!model_.built()) return;
    ++jobs_;
    generateBtn_.setEnabled(false);
    status_.setText("Writing " + outDir.getFullPathName() + "...", juce::dontSendNotification);
    auto in = model_.snapshot();   // shared, immutable: no audio copy on the message thread
    std::weak_ptr<bool> alive = alive_;
    const auto gen = generation_;
    std::thread([this, alive, gen, in, out = outDir.getFullPathName().toStdString(), src = model_.folder(), notes = ksfNotes_] {
        std::string err;
        const std::string sum = SoundConversionModel::generate(*in, src, out, &err, notes);
        juce::MessageManager::callAsync([this, alive, gen, sum, err, out] {
            if (alive.expired()) return;
            --jobs_;
            if (gen != generation_) return;   // the user moved on (new folder, settings or preview): keep their state
            generateBtn_.setEnabled(true);
            if (sum.empty()) { status_.setText("Not written: " + juce::String(err), juce::dontSendNotification); return; }
            preview_.setText(sum);
            status_.setText("Package written to " + juce::String(out) + ". Opened in Resource Graph.", juce::dontSendNotification);
            if (openInResources_) openInResources_(juce::File(out));
        });
    }).detach();
}

void SoundConversionComponent::resized() {
    auto r = getLocalBounds().reduced(10);
    auto top = r.removeFromTop(28);
    folderBtn_.setBounds(top.removeFromLeft(170));
    top.removeFromLeft(8);
    folder_.setBounds(top);
    r.removeFromTop(6);
    auto row = r.removeFromTop(26);
    nameLbl_.setBounds(row.removeFromLeft(80)); name_.setBounds(row.removeFromLeft(170)); row.removeFromLeft(10);
    orderLbl_.setBounds(row.removeFromLeft(140)); order_.setBounds(row.removeFromLeft(200)); row.removeFromLeft(10);
    splitsLbl_.setBounds(row.removeFromLeft(100)); splits_.setBounds(row.removeFromLeft(90)); row.removeFromLeft(10);
    previewBtn_.setBounds(row.removeFromLeft(90));
    r.removeFromTop(6);
    map_.setBounds(r.removeFromTop(150));
    r.removeFromTop(6);
    auto bottom = r.removeFromBottom(28);
    generateBtn_.setBounds(bottom.removeFromLeft(170));
    bottom.removeFromLeft(8);
    status_.setBounds(bottom);
    pluginNote_.setBounds(r.removeFromBottom(22));
    auto left = r.removeFromLeft(r.getWidth() * 3 / 5);
    if (mapping_.isVisible()) { mapping_.setBounds(left.removeFromTop(left.getHeight() / 2)); left.removeFromTop(6); }
    preview_.setBounds(left);
    r.removeFromLeft(6);
    recipe_.setBounds(r);
}

} // namespace ai_arranger::app
