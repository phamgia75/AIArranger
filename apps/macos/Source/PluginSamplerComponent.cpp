#include "media/kst_diff.h"
#include "PluginSamplerComponent.h"
#include "jobs/export_specs.h"
#include <cstdlib>
#include <fstream>

namespace ai_arranger::app {

using ui::SoundConversionModel;
namespace fs = std::filesystem;

bool SamplerProcess::start(const juce::File& exe, const std::vector<std::string>& args, LineFn onLine, DoneFn onDone) {
    juce::StringArray a;
    a.add(exe.getFullPathName());
    for (const auto& s : args) a.add(juce::String(s));
    if (s_->started || !exe.existsAsFile() || !s_->proc.start(a, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return false;
    s_->started = true;
    onDone_ = onDone;
    std::weak_ptr<bool> alive = alive_;
    reader_ = std::thread([st = s_, alive, onLine = std::move(onLine), onDone = std::move(onDone)] {
        std::string buf;
        auto post = [&](std::string line) {
            juce::MessageManager::callAsync([alive, onLine, line = std::move(line)] { if (!alive.expired()) onLine(line); });
        };
        // One byte per call: JUCE reads with fread(), which blocks until the whole request is filled, so a
        // bigger buffer would hold progress lines back until the end (stdio still buffers underneath).
        for (char c; st->proc.readProcessOutput(&c, 1) == 1;) {
            if (c != '\n') { buf += c; continue; }
            post(buf);
            buf.clear();
        }
        if (!buf.empty()) post(buf);
        for (;;) {   // output closed: wait for the exit and reap it under the lock, so cancel() cannot signal a reused PID
            {
                std::lock_guard<std::mutex> g(st->m);
                if (!st->proc.isRunning()) { st->reaped = true; break; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const int code = static_cast<int>(st->proc.getExitCode());
        st->finished = true;
        juce::MessageManager::callAsync([st, alive, onDone, code] {
            if (!alive.expired() && !st->doneSent.exchange(true)) onDone(code, st->cancelled);
        });
    });
    return true;
}

void SamplerProcess::reap(State& st) {   // collect a killed child whose reader is stuck, so no zombie is left
    std::lock_guard<std::mutex> g(st.m);
    if (!st.reaped && !st.proc.isRunning()) st.reaped = true;
}

bool SamplerProcess::cancel() {
    if (!s_->started || s_->finished) return false;
    s_->cancelled = true;
    bool signalled = false;
    {
        std::lock_guard<std::mutex> g(s_->m);
        if (!s_->reaped) signalled = s_->proc.kill();
    }
    std::weak_ptr<bool> alive = alive_;
    juce::Timer::callAfterDelay(kCancelGraceMs, [st = s_, alive, onDone = onDone_] {
        reap(*st);
        if (!alive.expired() && !st->doneSent.exchange(true)) onDone(-1, true);   // the reader is stuck on a pipe a helper still holds
    });
    return signalled;
}

SamplerProcess::~SamplerProcess() {
    cancel();
    alive_.reset();   // no callback runs after this
    if (!reader_.joinable()) return;
    // A killed sampler closes its pipe within milliseconds; only a helper holding it open makes the reader outlast this.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCloseGraceMs);
    while (!s_->finished && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (s_->finished) reader_.join();
    else { reap(*s_); reader_.detach(); }   // it owns State and ends when the pipe closes
}

juce::File PluginSamplerComponent::locateSampler() {
    if (const char* e = std::getenv("AIARR_PLUGIN_SAMPLER"); e && *e) return juce::File(e);
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    if (const auto f = exe.getSiblingFile("aiarranger-plugin-sampler"); f.existsAsFile()) return f;
    // shortcut: development builds find the tool in the CMake build tree; bundle it into the app for distribution.
    int depth = 0;
    for (auto d = exe.getParentDirectory(); depth++ < 8 && d.getParentDirectory() != d; d = d.getParentDirectory())
        for (const char* cfg : {"", "Debug", "Release", "RelWithDebInfo", "MinSizeRel"})   // "": single-config build without CMAKE_BUILD_TYPE
            if (const auto f = d.getChildFile("aiarranger-plugin-sampler_artefacts").getChildFile(cfg).getChildFile("aiarranger-plugin-sampler");
                f.existsAsFile())
                return f;
    return {};
}

PluginSamplerComponent::PluginSamplerComponent(std::function<void(const juce::File&)> openInResources)
    : openInResources_(std::move(openInResources)) {
    const ui::SamplerSettings d;
    const std::pair<const char*, std::string> form[] = {
        {"Sound name", ""}, {"Program", d.program}, {"Lowest note", d.low}, {"Highest note", d.high}, {"Every (semitones)", d.step},
        {"Velocities, softest first", d.velocities}, {"Splits", d.splits}, {"Hold (s)", d.hold}, {"Release (s)", d.release}};
    for (const auto& [label, value] : form) {
        auto* l = labels_.add(new juce::Label({}, label));
        l->setColour(juce::Label::textColourId, juce::Colour(0xffb8b8c0));
        addAndMakeVisible(l);
        auto* t = fields_.add(new juce::TextEditor(label));
        t->setText(value, false);
        t->onTextChange = [this] { settingsChanged(); };
        addAndMakeVisible(t);
    }
    map_.setEmptyText("Render Samples to see the key and velocity map of the rendered Sound.");
    for (auto* l : {&estimate_, &status_, &licence_}) { l->setColour(juce::Label::textColourId, juce::Colour(0xffb8b8c0)); addAndMakeVisible(*l); }
    licence_.setText("Sample only instruments whose licence permits it. Rendering runs offline in a separate process: "
                     "no audio device, no MIDI port, nothing is sent to the instrument.", juce::dontSendNotification);
    status_.setText(exe_.existsAsFile() ? "Press Find Instruments." : "The sampler tool (aiarranger-plugin-sampler) was not found.",
                    juce::dontSendNotification);
    plugin_.setTextWhenNothingSelected("(no instrument chosen)");
    plugin_.onChange = [this] { findPrograms(); settingsChanged(); };
    // The Program field stays the single source of truth (checked, passed to the sampler); the list only fills it.
    fields_[1]->setVisible(false);
    program_.setTextWhenNothingSelected("(the plugin's current sound)");
    program_.setTitle("Program");
    program_.onChange = [this] {
        const int k = program_.getSelectedId();   // 1 = current sound, 2.. = program k-2
        fields_[1]->setText(k >= 2 ? juce::String(k - 2) : juce::String(), juce::sendNotificationSync);
    };
    addAndMakeVisible(program_);
    for (auto* t : {&loop_, &stereo_}) { t->onClick = [this] { settingsChanged(); }; addAndMakeVisible(*t); }
    for (auto* t : {&preview_, &report_}) {
        t->setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
        t->setWrap(t == &report_);
        addAndMakeVisible(*t);
    }
    findBtn_.onClick = [this] { findInstruments(); };
    renderBtn_.onClick = [this] { startRender(); };
    cancelBtn_.onClick = [this] { cancelRender(); };
    generateBtn_.onClick = [this] {
        if (!in_) return;
        chooser_ = std::make_shared<juce::FileChooser>("Choose where to create the package folder (a local folder)");
        auto c = chooser_;
        c->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                       [c, this, alive = std::weak_ptr<bool>(alive_)](const juce::FileChooser& fc) {
                           if (alive.expired() || fc.getResult() == juce::File()) return;
                           generate(fc.getResult().getChildFile(juce::File::createLegalFileName(juce::String(renderedWith_.name))));
                       });
    };
    for (auto* c : std::initializer_list<juce::Component*>{&findBtn_, &plugin_, &renderBtn_, &cancelBtn_, &generateBtn_, &bar_, &map_})
        addAndMakeVisible(c);
    settingsChanged();
}

PluginSamplerComponent::~PluginSamplerComponent() {
    *alive_ = false;
    proc_.reset();   // kills a running render and stops its reader (bounded); queued callbacks are then dropped
    listProc_.reset();
    progProc_.reset();
    ownJobs_.waitIdle();   // let Generate finish copying out of staging before it is deleted
    discardStaging();
}

juce::TextEditor& PluginSamplerComponent::field(const juce::String& label) {
    for (auto* t : fields_) if (t->getName() == label) return *t;
    jassertfalse;
    return *fields_.getFirst();
}

juce::StringArray PluginSamplerComponent::instrumentNames() const {
    juce::StringArray a;
    for (const auto& p : plugins_) a.add(juce::String(p.name));
    return a;
}

void PluginSamplerComponent::findPrograms() {
    progProc_.reset();   // a previous instrument's scan: killed, its callbacks dropped
    program_.clear(juce::dontSendNotification);
    fields_[1]->setText({}, juce::dontSendNotification);
    program_.setEnabled(false);
    const auto id = settings().pluginId;
    if (id.empty()) return;
    auto lines = std::make_shared<std::string>();
    progProc_ = std::make_unique<SamplerProcess>();
    progProc_->start(exe_, {"programs", "--plugin", id}, [lines](const std::string& l) { *lines += l + "\n"; }, [this, lines](int, bool) {
        const auto names = ui::parseProgramList(*lines);
        program_.addItem("(the plugin's current sound)", 1);
        for (size_t i = 0; i < names.size(); ++i) program_.addItem(juce::String(i) + ": " + juce::String(names[i]), int(i) + 2);
        program_.setSelectedId(1, juce::sendNotificationSync);
        program_.setEnabled(names.size() > 1);
    });
}

bool PluginSamplerComponent::selectInstrument(const juce::String& name) {
    for (size_t i = 0; i < plugins_.size(); ++i)
        if (juce::String(plugins_[i].name) == name) { plugin_.setSelectedItemIndex(int(i), juce::sendNotificationSync); return true; }
    return false;
}

ui::SamplerSettings PluginSamplerComponent::settings() const {
    ui::SamplerSettings s;
    const int i = plugin_.getSelectedItemIndex();
    if (i >= 0 && size_t(i) < plugins_.size()) s.pluginId = plugins_[size_t(i)].id;
    auto f = [this](int k) { return fields_[k]->getText().trim().toStdString(); };
    s.name = f(0); s.program = f(1); s.low = f(2); s.high = f(3); s.step = f(4);
    s.velocities = f(5); s.splits = f(6); s.hold = f(7); s.release = f(8);
    s.loop = loop_.getToggleState();
    s.stereo = stereo_.getToggleState();
    return s;
}

void PluginSamplerComponent::settingsChanged() {
    ui::SamplerPlan plan;
    const auto s = settings();
    const auto problem = ui::planRender(s, &plan);
    estimate_.setText(problem.empty() ? juce::String(ui::estimateText(plan)) : "Not ready: " + juce::String(problem), juce::dontSendNotification);
    const bool rendering = proc_ && proc_->running();
    renderBtn_.setEnabled(problem.empty() && !busy() && exe_.existsAsFile());
    cancelBtn_.setEnabled(rendering);
    // Generate writes what was rendered and previewed; after an edit the form no longer describes it.
    const bool same = ui::samplerArgs(s, "") == ui::samplerArgs(renderedWith_, "");
    generateBtn_.setEnabled(in_ != nullptr && !busy() && same);
    if (in_ && !same && !busy())
        status_.setText("Settings changed since the render: Render Samples again to apply them.", juce::dontSendNotification);
}

void PluginSamplerComponent::findInstruments() {
    if (!exe_.existsAsFile()) { status_.setText("The sampler tool was not found: " + exe_.getFullPathName(), juce::dontSendNotification); return; }
    findBtn_.setEnabled(false);
    plugins_.clear();   // a new scan replaces the list; nothing stale stays selectable
    plugin_.clear(juce::dontSendNotification);
    findPrograms();     // no instrument: clears the program list and the Program field
    settingsChanged();
    status_.setText("Looking for AU/VST3 instruments...", juce::dontSendNotification);
    auto lines = std::make_shared<std::string>();
    listProc_ = std::make_unique<SamplerProcess>();
    const bool ok = listProc_->start(exe_, {"list"}, [lines](const std::string& l) { *lines += l + "\n"; },
        [this, lines](int code, bool) {
            findBtn_.setEnabled(true);
            plugins_ = ui::parsePluginList(*lines);
            plugin_.clear(juce::dontSendNotification);
            for (size_t i = 0; i < plugins_.size(); ++i)
                plugin_.addItem(juce::String(plugins_[i].name) + "  (" + juce::String(plugins_[i].manufacturer) + ", " + juce::String(plugins_[i].format) + ")",
                                int(i) + 1);
            status_.setText(plugins_.empty() ? "No instruments found" + (code ? " (the scan failed)" : juce::String()) + "."
                                             : juce::String(plugins_.size()) + " instrument(s) found. Choose one and set the range.",
                            juce::dontSendNotification);
            settingsChanged();
        });
    if (!ok) { findBtn_.setEnabled(true); status_.setText("Cannot start " + exe_.getFullPathName(), juce::dontSendNotification); }
}

void PluginSamplerComponent::discardStaging() {
    // Only ever the folder this component created: <temp>/aiarranger-render-<uuid>.
    if (staging_ != juce::File() && staging_.getFileName().startsWith("aiarranger-render-") &&
        staging_.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory))
        staging_.deleteRecursively();
    staging_ = juce::File();
}

void PluginSamplerComponent::startRender() {
    const auto s = settings();
    if (const auto problem = ui::planRender(s, nullptr); !problem.empty() || busy()) return;
    proc_.reset();
    discardStaging();
    staging_ = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("aiarranger-render-" + juce::Uuid().toString());
    if (!staging_.createDirectory()) { status_.setText("Cannot create a staging folder.", juce::dontSendNotification); return; }
    in_ = nullptr;
    map_.setInstrument(nullptr);
    preview_.clear();
    report_.clear();
    got_ = {};
    renderedWith_ = s;
    progress_ = 0;
    ++jobs_;
    const auto gen = ++generation_;
    proc_ = std::make_unique<SamplerProcess>();
    const bool ok = proc_->start(exe_, ui::samplerArgs(s, staging_.getChildFile("pkg").getFullPathName().toStdString()),
        [this](const std::string& line) {
            ui::readSamplerLine(line, got_);
            if (got_.total) {
                progress_ = double(got_.done) / double(got_.total);
                status_.setText("Rendering note " + juce::String(got_.done) + " of " + juce::String(got_.total) + "...", juce::dontSendNotification);
            }
        },
        [this, gen](int code, bool cancelled) { finishRender(code, cancelled, gen); });
    if (!ok) {
        --jobs_;
        status_.setText("Cannot start the sampler: " + exe_.getFullPathName(), juce::dontSendNotification);
        discardStaging();
    } else {
        status_.setText("Starting the instrument...", juce::dontSendNotification);
    }
    settingsChanged();
}

void PluginSamplerComponent::cancelRender() {
    if (proc_ && proc_->running()) { proc_->cancel(); status_.setText("Cancelling...", juce::dontSendNotification); }
}

void PluginSamplerComponent::finishRender(int code, bool cancelled, uint64_t gen) {
    if (gen != generation_) return;
    const fs::path pkg = got_.package;
    std::error_code ec;
    if (cancelled || code != 0 || pkg.empty() || !fs::is_directory(pkg / "wav", ec)) {
        --jobs_;
        progress_ = 0;
        status_.setText(cancelled ? juce::String("Cancelled. Nothing was kept.")
                        : "Render failed: " + juce::String(!got_.lastLine.empty() && code != 0 ? got_.lastLine
                                                           : "the sampler ended without a package (exit " + std::to_string(code) + ")"),
                        juce::dontSendNotification);
        discardStaging();
        settingsChanged();
        return;
    }
    status_.setText("Reading the rendered samples...", juce::dontSendNotification);
    std::weak_ptr<bool> alive = alive_;
    std::thread([this, alive, gen, pkg, got = got_, name = renderedWith_.name] {
        auto in = std::make_shared<media::sound::Instrument>();
        auto texts = std::make_shared<std::array<std::string, 2>>();
        std::string err;
        const bool ok = SoundConversionModel::decode(pkg / "wav", name, got.layers, got.splits, *in, &err);
        if (ok) {
            const auto checks = SoundConversionModel::checksOf(*in);
            std::string c = checks.empty() ? "Checks: nothing to report.\n\n" : "Checks (" + std::to_string(checks.size()) + "):\n";
            for (const auto& w : checks) c += "  - " + w + "\n";
            (*texts)[0] = c + "\n" + SoundConversionModel::previewOf(*in);
            std::ifstream f(pkg / "REPORT.txt");
            (*texts)[1].assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        juce::MessageManager::callAsync([this, alive, gen, in, texts, ok, err] {
            if (alive.expired()) return;
            --jobs_;
            if (gen != generation_) return;
            progress_ = ok ? 1.0 : 0.0;
            if (!ok) {
                status_.setText("The sampler's output is not a valid package: " + juce::String(err), juce::dontSendNotification);
                discardStaging();
            } else {
                in_ = in;
                map_.setInstrument(in_.get());
                preview_.setText(ui::loopWarning(*in_, renderedWith_.loop) + (*texts)[0]);
                report_.setText((*texts)[1]);
                status_.setText(ui::renderedStatus(*in_, renderedWith_.loop), juce::dontSendNotification);
            }
            settingsChanged();
        });
    }).detach();
}

void PluginSamplerComponent::generate(const juce::File& outDir) {
    if (!in_ || busy()) return;
    const fs::path out = outDir.getFullPathName().toStdString(), from = staging_.getChildFile("pkg").getFullPathName().toStdString();
    if (const auto r = media::sound::outputFolderRefusal(out); !r.empty()) { status_.setText("Not written: " + juce::String(r), juce::dontSendNotification); return; }
    ++jobs_;
    settingsChanged();
    status_.setText("Writing " + outDir.getFullPathName() + " (export job)...", juce::dontSendNotification);
    std::weak_ptr<bool> alive = alive_;
    const auto gen = generation_;
    // An export job (D17/D18): the copy is staged in NAME.INCOMPLETE, must equal the render and load back, then is published
    port_.enqueue(jobs::stagedPackage(from, out), [this, alive, gen, token = genJobs_.start()](const jobs::JobState& j) {
            if (alive.expired()) return;
            if (genJobs_.land(token)) --jobs_;
            lastJob_ = j;
            settingsChanged();
            if (j.phase != jobs::Phase::Done) { status_.setText("Not written: " + juce::String(j.reason), juce::dontSendNotification); return; }
            if (gen != generation_) return;
            preview_.setText(j.writerText + preview_.getText().toStdString());
            status_.setText("Package written to " + juce::String(j.dest) + ". Opened in Resource Graph.", juce::dontSendNotification);
            if (openInResources_) openInResources_(juce::File(juce::String(j.dest)));
    });
}

void PluginSamplerComponent::resized() {
    auto r = getLocalBounds().reduced(10);
    auto row = r.removeFromTop(28);
    findBtn_.setBounds(row.removeFromLeft(150)); row.removeFromLeft(8);
    plugin_.setBounds(row.removeFromLeft(420)); row.removeFromLeft(12);
    labels_[1]->setBounds(row.removeFromLeft(64)); program_.setBounds(row.removeFromLeft(260));
    r.removeFromTop(6);
    auto put = [&](juce::Rectangle<int>& line, int k, int lw, int fw) {
        labels_[k]->setBounds(line.removeFromLeft(lw)); fields_[k]->setBounds(line.removeFromLeft(fw)); line.removeFromLeft(10);
    };
    row = r.removeFromTop(26);
    put(row, 0, 84, 170); put(row, 2, 90, 50); put(row, 3, 96, 50); put(row, 4, 124, 50);
    r.removeFromTop(6);
    row = r.removeFromTop(26);
    put(row, 5, 170, 150); put(row, 6, 50, 90); put(row, 7, 64, 50); put(row, 8, 84, 50);
    loop_.setBounds(row.removeFromLeft(150)); stereo_.setBounds(row.removeFromLeft(80));
    r.removeFromTop(4);
    estimate_.setBounds(r.removeFromTop(68));   // estimate + key/velocity map: up to 4 lines (smoke test checks the longest map fits)
    row = r.removeFromTop(28);
    renderBtn_.setBounds(row.removeFromLeft(150)); row.removeFromLeft(8);
    cancelBtn_.setBounds(row.removeFromLeft(90)); row.removeFromLeft(8);
    bar_.setBounds(row.removeFromLeft(220)); row.removeFromLeft(8);
    status_.setBounds(row);
    r.removeFromTop(6);
    map_.setBounds(r.removeFromTop(130));
    r.removeFromTop(6);
    auto bottom = r.removeFromBottom(28);
    generateBtn_.setBounds(bottom.removeFromLeft(170)); bottom.removeFromLeft(8);
    licence_.setBounds(bottom);
    r.removeFromBottom(6);
    preview_.setBounds(r.removeFromLeft(r.getWidth() / 2));
    r.removeFromLeft(6);
    report_.setBounds(r);
}

} // namespace ai_arranger::app
