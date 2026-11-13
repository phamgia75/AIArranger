#include "StudioComponent.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_report.h"
#include "media/sf2_check.h"
#include <thread>

namespace ai_arranger::app {

namespace st = ui::studio;

namespace {
// Tokens from UI spec rev 2 §3.2 (dark): AIArranger's own studio identity.
const juce::Colour kBg(0xff0d0d0f), kSurface(0xff1a1a1f), kSurface2(0xff252530), kAccent(0xff00e5ff), kPending(0xffffb300),
    kExperimental(0xffbf8cff), kText2(0xffa0a0b0);

juce::File toolScript(const char* name) {
    juce::String dir = juce::SystemStats::getEnvironmentVariable("AIARR_TOOLS_DIR", {});
#ifdef AIARR_REPO_TOOLS_DIR
    if (dir.isEmpty()) dir = AIARR_REPO_TOOLS_DIR;
#endif
    return dir.isEmpty() ? juce::File() : juce::File(dir).getChildFile(name);
}

juce::File onPath(const juce::String& exe) {
    for (const auto& d : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("PATH", {}), ":", {}))
        if (const auto f = juce::File(d).getChildFile(exe); d.isNotEmpty() && f.existsAsFile()) return f;
    return {};
}
}  // namespace

const char* StudioComponent::domainName(Domain d) {
    switch (d) {
        case Domain::Library: return "Library Workspace";
        case Domain::ImportExport: return "Import / Export Center";
        case Domain::SampleLab: return "Sample Lab";
        case Domain::StyleStudio: return "Style Studio";
        case Domain::Validation: return "Validation & Reports";
    }
    return "";
}

void StudioComponent::styleText(juce::TextEditor& t) {
    t.setMultiLine(true);
    t.setReadOnly(true);
    t.setScrollbarsShown(true);
    t.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    t.setColour(juce::TextEditor::backgroundColourId, kSurface);
}

juce::TextButton& StudioComponent::button(const juce::String& text, std::function<void()> onClick) {
    auto* b = new juce::TextButton(text);
    owned_.add(b);
    b->onClick = std::move(onClick);
    return *b;
}

juce::Label& StudioComponent::label(const juce::String& text, juce::Colour c) {
    auto* l = new juce::Label({}, text);
    owned_.add(l);
    l->setColour(juce::Label::textColourId, c);
    l->setJustificationType(juce::Justification::centredLeft);
    return *l;
}

// The inline reason shown next to a control when it is unavailable (spec §3.4); also its tooltip.
juce::Label& StudioComponent::reasonFor(juce::Button& b) {
    auto& r = reasons_[&b];
    if (!r) { r = &label({}, kPending); r->setMinimumHorizontalScale(1.0f); }
    return *r;
}

void StudioComponent::setUnavailable(juce::Button& b, const juce::String& reason) {
    b.setEnabled(reason.isEmpty());
    b.setTooltip(reason);
    reasonFor(b).setText(reason, juce::dontSendNotification);
}

void StudioComponent::FlowPage::resized() {
    auto r = getLocalBounds();
    for (const auto& row : rows) {
        auto line = r.removeFromTop(28);
        for (const auto& [c, w] : row) {
            c->setBounds(w > 0 ? line.removeFromLeft(w) : line);
            if (w > 0) line.removeFromLeft(8);
        }
        r.removeFromTop(6);
    }
    if (fill) fill->setBounds(r);
}

StudioComponent::StudioComponent(std::function<void()> showLiveHost) : showLiveHost_(std::move(showLiveHost)) {
    using D = Domain;
    // ── shared project context: the existing Sounds & Resources pages, chosen by sidebar + segments ──
    res_.tabs().setTabBarDepth(0);
    res_.tabs().getTabbedButtonBar().addChangeListener(this);
    addAndMakeVisible(res_);

    // ── Library Workspace › Sources ──
    styleText(libraryView_);
    libraryView_.setText("No source open. Open Source... reads a folder (a Pa5X KST backup, a package, or KMP/KSF/WAV samples) "
                         "and shows what it is. Nothing in it is changed.", false);
    auto& openSrc = button("Open Source...", [this] { chooseFolder("Folder to open as a read-only source", [this](juce::File f) { openSource(f); }); });
    toSampleLab_ = &button("Send to Sample Lab", [this] {
        if (source_ && res_.converter().inspectFolder(juce::File(source_->path))) showDomain(D::SampleLab, 0);
    });
    sourcesPage_.rows = {{{&openSrc, 150}, {toSampleLab_, 150}, {&reasonFor(*toSampleLab_), 0}}};
    sourcesPage_.fill = &libraryView_;
    for (auto& row : sourcesPage_.rows) for (auto& [c, w] : row) sourcesPage_.addAndMakeVisible(c);
    sourcesPage_.addAndMakeVisible(libraryView_);

    // ── Import / Export Center: routes into the existing flows ──
    auto route = [this](const juce::String& text, const juce::String& where, D d, int seg) -> std::vector<std::pair<juce::Component*, int>> {
        auto& b = button(text, [this, d, seg] { showDomain(d, seg); });
        return {{&b, 260}, {&label(where, kText2), 0}};
    };
    auto off = [this](const juce::String& text, const juce::String& why) -> std::vector<std::pair<juce::Component*, int>> {
        auto& b = button(text, {});
        setUnavailable(b, why);
        return {{&b, 260}, {&reasonFor(b), 0}};
    };
    ioPage_.rows = {
        {{&label("IMPORT  (sources are only read)", kAccent), 0}},
        route("Keyboard backup (KST) folder", "Library Workspace > Sources: recognise it and show its library", D::Library, 0),
        route("AIArranger package", "Library Workspace > Resource Graph: Open Package...", D::Library, 1),
        route("KMP / KSF / WAV samples", "Sample Lab > Convert Sound: mapping editor, preview, quality checks", D::SampleLab, 0),
        route("AU / VST3 instrument", "Sample Lab > Sample Plugin: offline render into a package", D::SampleLab, 1),
        route("Yamaha style (.sty)", "Style Studio: read-only report", D::StyleStudio, 0),
        {{&label("EXPORT  (always a NEW local folder)", kAccent), 0}},
        route("Sound package (SF2, TXPROG, WAV)", "Sample Lab > Convert Sound: Generate Package...", D::SampleLab, 0),
        route("Verified KST backup", "Library Workspace > KST Library: Backup KST... (byte copy + SHA-256 manifest)", D::Library, 2),
        route("Check a USB copy (read-only)", "Validation & Reports: Verify Package against its manifest", D::Validation, 0),
        off("Pa5X KST export", "Not available: needs a KST writer and a KST L1 validator; neither exists yet."),
        off("KMP / KSF export", "Not in this milestone: the KMP/KSF writer is a separate work item (E1) and is not in the app yet."),
        off("Style as Pa5X SMF", "Command line only (aiarranger-convert); EXPERIMENTAL, Pa5X import not tested."),
        {{&label("STAGED CHANGES: 0. Sources are never changed in place. Nothing can be staged yet: SET/KST Manager and Style "
                 "editing are later phases.", kPending), 0}},
        {{&label("WRITE RULES: every write goes to NAME.INCOMPLETE and is published by one rename only after its checks pass; "
                 "a cancelled or failed write leaves only NAME.INCOMPLETE.", kText2), 0}},
        {{&label("Mounted volumes (USB sticks) are refused: write locally, copy by hand in Finder, then Check a USB copy.", kText2), 0}},
    };
    for (auto& row : ioPage_.rows) for (auto& [c, w] : row) ioPage_.addAndMakeVisible(c);

    // ── Sample Lab › Quality Report (the Convert Sound recipe with its measured QUALITY section) ──
    styleText(quality_);
    qualityPage_.fill = &quality_;
    qualityPage_.addAndMakeVisible(quality_);

    // ── Style Studio (read-only; editing is a later phase) ──
    styleText(styleReport_);
    styleReport_.setText("No Style open. Open Style... reads a Yamaha SFF1 .sty file and reports its sections, tracks and what "
                         "AIArranger understands of it. The file is only read.", false);
    auto& openSty = button("Open Style...", [this] {
        chooseFolder("Style file to read (.sty)", [this](juce::File f) { openStyle(f); }, true, "*.sty;*.STY");
    });
    liveFromStyle_ = &button("Open Live Arranger", [this] { if (showLiveHost_) showLiveHost_(); });
    setUnavailable(*liveFromStyle_, showLiveHost_ ? "" : "Needs: the Live Arranger window (open the Studio from it)");
    auto& edit = button("Edit Style", {});
    setUnavailable(edit, "Not available: Style editing is a later phase; this view only reads.");
    auto& smf = button("Export as Pa5X SMF", {});
    setUnavailable(smf, "Command line only (aiarranger-convert); EXPERIMENTAL, Pa5X import not tested.");
    stylePage_.rows = {{{&openSty, 150}, {liveFromStyle_, 160}, {&label("Load the Style there with Load Style... to audition it.", kText2), 0}},
                       {{&edit, 150}, {&reasonFor(edit), 0}},
                       {{&smf, 150}, {&reasonFor(smf), 0}}};
    stylePage_.fill = &styleReport_;
    for (auto& row : stylePage_.rows) for (auto& [c, w] : row) stylePage_.addAndMakeVisible(c);
    stylePage_.addAndMakeVisible(styleReport_);

    // ── Validation & Reports ──
    styleText(jobsLog_);
    target_.setColour(juce::Label::textColourId, juce::Colours::white);
    badges_.setColour(juce::Label::textColourId, kAccent);
    badges_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    auto& chooseTarget = button("Choose Folder...", [this] {
        chooseFolder("Output or package folder to check (read-only)", [this](juce::File f) { setValidationTarget(f); });
    });
    auto& chooseSf2 = button("Choose SF2...", [this] {
        chooseFolder("SoundFont to check (read-only)", [this](juce::File f) { setValidationTarget(f); }, true, "*.sf2");
    });
    useSourceBtn_ = &button("Use Library Source", [this] { if (source_) setValidationTarget(juce::File(source_->path)); });
    auto validator = [this](Validator v, const juce::String& text) -> juce::TextButton& {
        return *(validatorButtons_[v] = &button(text, [this, v] { runValidator(v); }));
    };
    auto& vp = validator(Validator::VerifyPackage, "Verify Package (L1, L2)");
    auto& ki = validator(Validator::KstInventory, "KST Inventory");
    auto& sc = validator(Validator::Sf2Check, "SF2 Check (L1)");
    auto& sp = validator(Validator::SoundPy, "Sound Validator (Python)");
    auto& dp = validator(Validator::DrumKitPy, "Drum Kit Validator (Python)");
    l3Btn_ = &button("Attach L3 Record...", [this] {
        chooseFolder("Owner's L3 hardware test record (aiarranger-l3-record)", [this](juce::File f) { attachL3Record(f); }, true, "*.txt;*.md");
    });
    validationPage_.rows = {
        {{&chooseTarget, 150}, {&chooseSf2, 130}, {useSourceBtn_, 150}, {&reasonFor(*useSourceBtn_), 0}},
        {{&target_, 0}},
        {{&vp, 200}, {&reasonFor(vp), 0}},
        {{&ki, 200}, {&reasonFor(ki), 0}},
        {{&sc, 200}, {&reasonFor(sc), 0}},
        {{&sp, 200}, {&reasonFor(sp), 0}},
        {{&dp, 200}, {&reasonFor(dp), 0}},
        {{l3Btn_, 200}, {&reasonFor(*l3Btn_), 0}},
        {{&badges_, 0}},
        {{&label("L1 structural: our validator checked the files.  L2 app: our reader re-opened them.  "
                 "L3 hardware: only an owner's recorded test of this exact output.", kText2), 0}},
    };
    validationPage_.fill = &jobsLog_;
    for (auto& row : validationPage_.rows) for (auto& [c, w] : row) validationPage_.addAndMakeVisible(c);
    validationPage_.addAndMakeVisible(jobsLog_);
    jobsLog_.setText("Jobs (this session)\n", false);

    segments_[D::Library] = {{"Sources", -1, &sourcesPage_}, {"Resource Graph", 0}, {"KST Library", 1}, {"Memory Check", 2}};
    segments_[D::ImportExport] = {{"Routes", -1, &ioPage_}};
    segments_[D::SampleLab] = {{"Convert Sound", 3}, {"Sample Plugin", 4}, {"Quality Report", -1, &qualityPage_},
                               {"KMP / KSF Writer", -1, nullptr, "Not in this milestone: the KMP/KSF writer (E1) is not in the app yet."}};
    segments_[D::StyleStudio] = {{"Style Report", -1, &stylePage_}};
    segments_[D::Validation] = {{"Checks", -1, &validationPage_}};
    for (auto* p : {(juce::Component*)&sourcesPage_, (juce::Component*)&ioPage_, (juce::Component*)&qualityPage_,
                    (juce::Component*)&stylePage_, (juce::Component*)&validationPage_})
        addChildComponent(p);
    for (auto& [d, segs] : segments_)
        for (size_t i = 0; i < segs.size(); ++i) {
            auto& b = button(segs[i].name, [this, d = d, i] { showDomain(d, int(i)); });
            b.setColour(juce::TextButton::buttonOnColourId, kSurface2);
            b.setColour(juce::TextButton::textColourOnId, kAccent);
            setUnavailable(b, segs[i].unavailable);
            segmentBar_.addChildComponent(b);
            segmentBar_.addChildComponent(reasonFor(b));
            segmentButtons_[d].push_back(&b);
        }

    // ── sidebar: our own grouping (spec §2), domains of this slice ──
    auto group = [this](const char* text) { sidebar_.addAndMakeVisible(label(text, kText2)); };
    auto entry = [this](D d) {
        auto& b = button(domainName(d), [this, d] { showDomain(d); });
        b.setRadioGroupId(1);
        b.setColour(juce::TextButton::buttonColourId, kSurface);
        b.setColour(juce::TextButton::buttonOnColourId, kSurface2);
        b.setColour(juce::TextButton::textColourOnId, kAccent);
        domainButtons_.push_back(&b);
        sidebar_.addAndMakeVisible(b);
    };
    group("LIBRARY");  entry(D::Library);
    group("CREATE");   entry(D::SampleLab); entry(D::StyleStudio);
    group("PERFORM");
    liveBtn_ = &button("Live Arranger", [this] { if (showLiveHost_) showLiveHost_(); });
    setUnavailable(*liveBtn_, showLiveHost_ ? "" : "Needs: the Live Arranger window");
    sidebar_.addAndMakeVisible(*liveBtn_);
    sidebar_.addAndMakeVisible(reasonFor(*liveBtn_));
    group("SHIP");     entry(D::ImportExport);
    group("VERIFY");   entry(D::Validation);
    addAndMakeVisible(sidebar_);

    title_.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, juce::Colours::white);
    experimental_.setText("EXPERIMENTAL", juce::dontSendNotification);
    experimental_.setColour(juce::Label::textColourId, kExperimental);
    addAndMakeVisible(title_);
    addChildComponent(experimental_);
    addAndMakeVisible(segmentBar_);
    styleText(inspector_);
    inspector_.setColour(juce::TextEditor::backgroundColourId, kSurface);
    addAndMakeVisible(inspector_);

    setWantsKeyboardFocus(true);
    refreshAvailability();
    setSize(1440, 900);
    showDomain(D::Library, 0);
}

StudioComponent::~StudioComponent() {
    res_.tabs().getTabbedButtonBar().removeChangeListener(this);
    *alive_ = false;
}

juce::StringArray StudioComponent::segmentNames(Domain d) const {
    juce::StringArray s;
    for (const auto& seg : segments_.at(d)) s.add(seg.name);
    return s;
}

juce::Component& StudioComponent::page(Domain d) {
    switch (d) {
        case Domain::Library: return sourcesPage_;
        case Domain::ImportExport: return ioPage_;
        case Domain::SampleLab: return qualityPage_;
        case Domain::StyleStudio: return stylePage_;
        case Domain::Validation: return validationPage_;
    }
    return sourcesPage_;
}

void StudioComponent::showDomain(Domain d, int segment) {
    const auto& segs = segments_.at(d);
    if (segment < 0 || segment >= int(segs.size()) || segs[size_t(segment)].unavailable.isNotEmpty()) segment = 0;
    domain_ = d;
    segment_ = segment;
    for (int i = 0; i < kDomains; ++i) domainButtons_[size_t(i)]->setToggleState(false, juce::dontSendNotification);
    for (auto* b : domainButtons_) if (b->getButtonText() == domainName(d)) b->setToggleState(true, juce::dontSendNotification);
    title_.setText(domainName(d), juce::dontSendNotification);
    experimental_.setVisible(d == Domain::StyleStudio);

    // segmented control, same place on every screen (spec §2); built once per domain
    for (auto& [dd, bs] : segmentButtons_)
        for (size_t i = 0; i < bs.size(); ++i) {
            bs[i]->setVisible(dd == d);
            reasonFor(*bs[i]).setVisible(dd == d && !bs[i]->isEnabled());
            bs[i]->setToggleState(dd == d && int(i) == segment, juce::dontSendNotification);
        }

    const auto& seg = segs[size_t(segment)];
    for (auto* p : {(juce::Component*)&sourcesPage_, (juce::Component*)&ioPage_, (juce::Component*)&qualityPage_,
                    (juce::Component*)&stylePage_, (juce::Component*)&validationPage_})
        p->setVisible(p == seg.page);
    res_.setVisible(seg.resTab >= 0);
    if (seg.resTab >= 0) {
        lastTab_ = seg.resTab;
        if (res_.tabs().getCurrentTabIndex() != seg.resTab) res_.tabs().setCurrentTabIndex(seg.resTab);
    }
    if (seg.page == &qualityPage_)
        quality_.setText(res_.converter().recipeText().isEmpty() ? "Nothing built yet. In Convert Sound, choose a KMP/KSF/WAV folder and "
                                                                    "press Preview: the measured quality checks appear here."
                                                                  : res_.converter().recipeText(), false);
    resized();
    refreshInspector();
}

// A hand-off inside the shared ResourcesComponent (a finished package opens in Resource Graph) follows its tab.
void StudioComponent::changeListenerCallback(juce::ChangeBroadcaster*) {
    const int t = res_.tabs().getCurrentTabIndex();
    if (t == lastTab_) return;   // our own switch, announced asynchronously (the user may have moved on since)
    if (t <= 2) showDomain(Domain::Library, t + 1);
    else showDomain(Domain::SampleLab, t - 3);
}

bool StudioComponent::keyPressed(const juce::KeyPress& k) {
    const int c = k.getKeyCode() - '1';
    if (k.getModifiers().isCommandDown() && c >= 0 && c < kDomains) {
        static const Domain order[] = {Domain::Library, Domain::SampleLab, Domain::StyleStudio, Domain::ImportExport, Domain::Validation};
        showDomain(order[c]);
        return true;
    }
    return false;
}

void StudioComponent::chooseFolder(const juce::String& title, std::function<void(juce::File)> then, bool files, const juce::String& pattern) {
    chooser_ = std::make_shared<juce::FileChooser>(title, juce::File(), pattern);
    auto c = chooser_;
    c->launchAsync(juce::FileBrowserComponent::openMode |
                       (files ? juce::FileBrowserComponent::canSelectFiles : juce::FileBrowserComponent::canSelectDirectories),
                   [c, then, alive = std::weak_ptr<bool>(alive_)](const juce::FileChooser& fc) {
                       if (!alive.expired() && fc.getResult() != juce::File()) then(fc.getResult());
                   });
}

void StudioComponent::openSource(const juce::File& dir) {
    ++jobs_;
    libraryView_.setText("Reading " + dir.getFullPathName() + " (read-only: names and sizes only)...", false);
    std::weak_ptr<bool> alive = alive_;
    std::thread([this, alive, d = dir.getFullPathName().toStdString()] {
        auto probe = std::make_shared<st::SourceProbe>(st::probeSource(d));
        juce::MessageManager::callAsync([this, alive, probe] {
            if (alive.expired()) return;
            --jobs_;
            libraryView_.setText(juce::String::fromUTF8(st::libraryView(*probe).c_str()), false);
            if (probe->layout) source_ = *probe;
            if (probe->kind == st::SourceProbe::Kind::AiaPackage) res_.openPackage(juce::File(probe->path));   // also in Resource Graph
            refreshAvailability();
            refreshInspector();
        });
    }).detach();
}

void StudioComponent::openStyle(const juce::File& sty) {
    importers::sff1::Sff1Reader reader;
    const auto r = reader.parseFile(sty.getFullPathName().toStdString());
    styleReport_.setText("Style: " + sty.getFileName() + "  (read-only; nothing is written)\n" + sty.getFullPathName() + "\n\n" +
                             juce::String::fromUTF8(importers::sff1::Sff1ReportGenerator::generateReport(r).c_str()),
                         false);
}

void StudioComponent::setValidationTarget(const juce::File& f) {
    validationTarget_ = f;
    verification_ = {};
    verificationIsPackage_ = false;
    refreshAvailability();
    refreshInspector();
}

juce::String StudioComponent::validatorUnavailable(Validator v) const {
    if (jobs_ > 0) return "Needs: the running job to finish";
    if (validationTarget_ == juce::File()) return "Needs: a folder or file to check (Choose Folder... or Choose SF2...)";
    const bool isDir = validationTarget_.isDirectory();
    switch (v) {
        case Validator::VerifyPackage:
        case Validator::KstInventory: return isDir ? "" : "Needs: a folder (the target is a file)";
        case Validator::Sf2Check: return !isDir && validationTarget_.hasFileExtension("sf2") ? "" : "Needs: an .sf2 file (Choose SF2...)";
        case Validator::SoundPy:
        case Validator::DrumKitPy: {
            if (!isDir) return "Needs: a package folder";
            const char* script = v == Validator::SoundPy ? "validate_sound_package.py" : "validate_drumkit_package.py";
            if (!toolScript(script).existsAsFile()) return juce::String("Needs: tools/") + script + " from the AIArranger repo (set AIARR_TOOLS_DIR)";
            if (onPath("python3") == juce::File()) return "Needs: python3 on PATH";
            if (v == Validator::SoundPy && onPath("fluidsynth") == juce::File()) return "Needs: fluidsynth on PATH (this validator renders the SF2)";
            return "";
        }
    }
    return "";
}

void StudioComponent::runValidator(Validator v) {
    if (validatorUnavailable(v).isNotEmpty()) return;
    const auto target = validationTarget_.getFullPathName().toStdString();
    std::weak_ptr<bool> alive = alive_;
    using Out = std::pair<juce::String, std::optional<st::Verification>>;
    switch (v) {
        case Validator::VerifyPackage:
            runJob("Verify Package", [target, alive]() -> Out {
                const auto c = media::sound::verifyPackage(target, [alive] { return !alive.expired(); });
                return {c.text, st::fromPackageCheck(c)};
            });
            break;
        case Validator::KstInventory:
            runJob("KST Inventory", [target]() -> Out {
                return {juce::String(ui::ResourceBrowserModel::kst(ui::ResourceBrowserModel::KstAction::Inventory, target)) +
                            "\nFile level only: no KST structural (L1) validator exists yet, so no badge is set.", std::nullopt};
            });
            break;
        case Validator::Sf2Check:
            runJob("SF2 Check", [target]() -> Out {
                const auto r = media::sf2::checkFile(target);
                st::Verification ver;
                ver.l1 = {r.ok && r.overlapping() == 0 ? st::Check::Passed : st::Check::Failed, "sf2-check"};
                return {juce::String(media::sf2::format(r)), ver};
            });
            break;
        case Validator::SoundPy:
        case Validator::DrumKitPy: {
            const auto script = toolScript(v == Validator::SoundPy ? "validate_sound_package.py" : "validate_drumkit_package.py");
            const auto py = onPath("python3");
            runJob(script.getFileName(), [script, py, target]() -> Out {
                juce::ChildProcess p;
                if (!p.start(juce::StringArray{py.getFullPathName(), "-I", script.getFullPathName(), juce::String(target)}))
                    return {"could not start " + py.getFullPathName(), std::nullopt};
                const auto out = p.readAllProcessOutput();
                return {out + "\nexit code " + juce::String(int(p.getExitCode())) +
                            " (independent tool evidence; it does not change the badges)", std::nullopt};
            });
            break;
        }
    }
}

void StudioComponent::runJob(const juce::String& name, std::function<std::pair<juce::String, std::optional<st::Verification>>()> work) {
    ++jobs_;
    const auto target = validationTarget_;
    addJob(juce::Time::getCurrentTime().formatted("%H:%M:%S") + "  " + name + "  " + target.getFullPathName() + "  running...");
    refreshAvailability();
    std::weak_ptr<bool> alive = alive_;
    std::thread([this, alive, name, target, work = std::move(work)] {
        auto result = std::make_shared<std::pair<juce::String, std::optional<st::Verification>>>(work());
        juce::MessageManager::callAsync([this, alive, name, target, result] {
            if (alive.expired()) return;
            --jobs_;
            if (target == validationTarget_ && result->second) {
                verification_ = *result->second;
                verificationIsPackage_ = name == "Verify Package" || name == "L3 record";
            }
            addJob(juce::Time::getCurrentTime().formatted("%H:%M:%S") + "  " + name + "  done" +
                   (result->second ? "  " + juce::String::fromUTF8(st::badgeLine(*result->second).c_str()) : juce::String()) + "\n" +
                   result->first + "\n");
            refreshAvailability();
            refreshInspector();
        });
    }).detach();
}

// The record is applied against a FRESH L1/L2 check: files changed since the last Verify Package never get L3.
void StudioComponent::attachL3Record(const juce::File& record) {
    if (!verificationIsPackage_ || jobs_ > 0) return;
    const auto out = validationTarget_.getFullPathName().toStdString(), rec = record.getFullPathName().toStdString();
    std::weak_ptr<bool> alive = alive_;
    runJob("L3 record", [out, rec, alive]() -> std::pair<juce::String, std::optional<st::Verification>> {
        const auto c = media::sound::verifyPackage(out, [alive] { return !alive.expired(); });
        auto v = st::fromPackageCheck(c);
        v.l3 = st::checkL3Record(rec, out, v.l1);
        return {"L3 record " + juce::String(rec) + ": " + juce::String(v.l3.detail) + "\nL1/L2 re-checked first: " + juce::String(c.text), v};
    });
}

void StudioComponent::addJob(const juce::String& line) {
    jobsLog_.moveCaretToEnd();
    jobsLog_.insertTextAtCaret(line + "\n");
}

void StudioComponent::refreshAvailability() {
    using K = st::SourceProbe::Kind;
    const bool samples = source_ && (source_->kind == K::KorgSamplingData || source_->kind == K::WavFolder);
    setUnavailable(*toSampleLab_, samples ? "" : "Needs: a KMP/KSF or WAV folder as the open source");
    setUnavailable(*useSourceBtn_, source_ ? "" : "Needs: a source opened in Library Workspace");
    for (auto& [v, b] : validatorButtons_) setUnavailable(*b, validatorUnavailable(v));
    setUnavailable(*l3Btn_, jobs_ > 0 ? "Needs: the running job to finish"
                            : !verificationIsPackage_ || verification_.l1.state != st::Check::Passed
                                ? "Needs: Verify Package with L1 passed on this target first" : "");
    target_.setText("Target: " + (validationTarget_ == juce::File() ? juce::String("none chosen") : validationTarget_.getFullPathName()),
                    juce::dontSendNotification);
    badges_.setText(juce::String::fromUTF8(st::badgeLine(verification_).c_str()), juce::dontSendNotification);
}

void StudioComponent::refreshInspector() {
    refreshAvailability();
    juce::String t;
    t << "SOURCE\n"
      << (source_ ? juce::String::fromUTF8(st::sourceHeader(*source_).c_str()) + "\n" + source_->path
                  : juce::String("No source open.\nLibrary Workspace > Sources > Open Source..."))
      << "\n\nVERIFICATION\n"
      << (validationTarget_ == juce::File() ? juce::String("No target checked yet.") : validationTarget_.getFileName()) << "\n"
      << juce::String::fromUTF8(st::badgeLine(verification_).c_str()) << "\n"
      << juce::String::fromUTF8(st::spokenBadges(verification_).c_str())
      << "\n\nSTAGED CHANGES\n0. Nothing is written until an export job runs, always into a new local folder."
      << "\n\nNOT VERIFIED\nLoading on a Pa5X. L3 comes only from an owner's recorded hardware test.";
    inspector_.setText(t, false);
    badges_.setTitle(st::spokenBadges(verification_));   // VoiceOver reads the meaning, not the glyphs
}

void StudioComponent::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    g.setColour(kSurface);
    g.fillRect(sidebar_.getBounds());
}

void StudioComponent::resized() {
    auto r = getLocalBounds();
    sidebar_.setBounds(r.removeFromLeft(220));
    {
        auto s = sidebar_.getLocalBounds().reduced(8, 12);
        for (auto* c : sidebar_.getChildren()) {
            const auto* l = dynamic_cast<juce::Label*>(c);
            const bool isLabel = l != nullptr;
            if (isLabel && l->getText().isEmpty()) continue;   // an empty reason (the control is available) takes no space
            if (isLabel) s.removeFromTop(8);
            c->setBounds(s.removeFromTop(isLabel ? 20 : 30));
            s.removeFromTop(4);
        }
    }
    inspector_.setBounds(r.removeFromRight(300).reduced(8, 12));
    r.reduce(16, 12);
    auto head = r.removeFromTop(32);
    experimental_.setBounds(head.removeFromRight(120));
    title_.setBounds(head);
    r.removeFromTop(6);
    segmentBar_.setBounds(r.removeFromTop(28));
    {
        auto s = segmentBar_.getLocalBounds();
        const auto& bs = segmentButtons_[domain_];
        for (auto* b : bs) { b->setBounds(s.removeFromLeft(140)); s.removeFromLeft(4); }
        for (auto* b : bs) if (!b->isEnabled()) { s.removeFromLeft(6); reasonFor(*b).setBounds(s); }
    }
    r.removeFromTop(10);
    for (auto* p : {(juce::Component*)&res_, (juce::Component*)&sourcesPage_, (juce::Component*)&ioPage_, (juce::Component*)&qualityPage_,
                    (juce::Component*)&stylePage_, (juce::Component*)&validationPage_})
        p->setBounds(r);
}

}  // namespace ai_arranger::app
