#include "StudioComponent.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_report.h"
#include "jobs/export_specs.h"
#include "jobs/import_kit.h"
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
        if (!row.empty() && !row.front().first->isVisible() && std::count(collapse.begin(), collapse.end(), row.front().first)) continue;
        const auto h = row.empty() ? heights.end() : heights.find(row.front().first);
        auto line = r.removeFromTop(h == heights.end() ? 28 : h->second);
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
    paInspectBtn_ = &button("Inspect Pa-native (read-only)", [this] { inspectPaNative(); });
    sourcesPage_.rows = {{{&openSrc, 150}, {toSampleLab_, 150}, {&reasonFor(*toSampleLab_), 0}},
                         {{paInspectBtn_, 230}, {&reasonFor(*paInspectBtn_), 0}}};
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
    // An export route with its job button: the label beside it says where the export comes from, or why it cannot run now.
    auto exportRow = [this](const juce::String& text, D d, int seg, ExportJob j, const juce::String& job, const juce::String& where,
                            std::optional<ExportJob> j2 = {}, const juce::String& job2 = {}) -> std::vector<std::pair<juce::Component*, int>> {
        auto& b = button(text, [this, d, seg] { showDomain(d, seg); });
        auto jobButton = [this](ExportJob k, const juce::String& t, const juce::String& w) -> juce::TextButton& {
            auto& jb = button(t, [this, k] {
                auto pick = [this, k](juce::File f) { exportJob(k, f); };
                if (k == ExportJob::StyleSmf) chooseSave("New Pa5X Style SMF file (never overwrites)", "*.mid", [pick](juce::File f) { pick(f.withFileExtension("mid")); });
                else if (k == ExportJob::StyleUasf) chooseSave("New UASF file (never overwrites)", "*.uasf", [pick](juce::File f) { pick(f.withFileExtension("uasf")); });
                else chooseFolder("Parent folder for the new output (local disk)", [this, k, pick](juce::File parent) {
                    const auto in = res_.converter().model().snapshot();
                    pick(k == ExportJob::KstBackup && source_ ? ResourcesComponent::backupFolderFor(juce::File(source_->path), parent)
                                                              : parent.getChildFile(juce::File::createLegalFileName(
                                                                    juce::String(in ? in->name : "Sound") + (k == ExportJob::KmpFolder ? " KMP" : ""))));
                });
            });
            exportButtons_[k] = {&jb, w};
            return jb;
        };
        auto& jb = jobButton(j, job, where);
        std::vector<std::pair<juce::Component*, int>> row{{&b, 260}, {&jb, 120}};
        if (j2) row.push_back({&jobButton(*j2, job2, where), 120});
        row.push_back({&reasonFor(jb), 0});
        return row;
    };
    ioPage_.rows = {
        {{&label("IMPORT  (sources are only read)", kAccent), 0}},
        route("Keyboard backup (KST) folder", "Library Workspace > Sources: recognise it and show its library", D::Library, 0),
        route("AIArranger package", "Library Workspace > Resource Graph: Open Package...", D::Library, 1),
        route("KMP / KSF / WAV samples", "Sample Lab > Convert Sound: mapping editor, preview, quality checks", D::SampleLab, 0),
        route("AU / VST3 instrument", "Sample Lab > Sample Plugin: offline render into a package", D::SampleLab, 1),
        route("Yamaha style (.sty)", "Style Studio: report and EXPERIMENTAL editing (the source file is never changed)", D::StyleStudio, 0),
        route("Pa-family SET folder", "Library Workspace > Sources: recognise it, then Inspect Pa-native (read-only, EXPERIMENTAL)", D::Library, 0),
        {{&label("EXPORT  (always a NEW local folder)", kAccent), 0}},
        exportRow("Sound package (SF2, TXPROG, WAV)", D::SampleLab, 0, ExportJob::SoundPackage, "Package job...",
                  "The Sound previewed in Sample Lab > Convert Sound (also: Generate Package... there)"),
        exportRow("Verified KST backup", D::Library, 2, ExportJob::KstBackup, "Backup job...",
                  "The KST opened in Library Workspace > Sources: byte copy + SHA-256 manifest (also: Backup KST... in KST Library)"),
        route("Check a USB copy (read-only)", "Validation & Reports: Verify Package against its manifest", D::Validation, 0),
        off("Pa5X KST export", "Not available: needs a KST writer and a KST L1 validator; neither exists yet (docs/PA5X_KST_EXPORT_PLAN.md)."),
        {{&button("Pa5X Import Kit...", [this] {
              chooseFolder("Folder holding the AIArranger outputs for the kit (only read)", [this](juce::File in) {
                  chooseFolder("Parent folder for the new kit (local disk)", [this, in](juce::File parent) {
                      importKitJob(in, parent.getChildFile(juce::File::createLegalFileName(in.getFileName() + " Pa5X Kit")));
                  });
              });
          }), 260},
         {&label("Sound/Drum Kit packages, KMP folders and Style SMFs in one USB folder with KIT.md (import steps, manual pages): "
                 "STRUCTURALLY VALID, not verified on a Pa5X", kText2), 0}},
        exportRow("KMP / KSF folder (EXPERIMENTAL)", D::SampleLab, 3, ExportJob::KmpFolder, "KMP job...",
                  "The previewed Sound as .KMP + .KSF (Sample Lab > KMP / KSF Writer), verification level L1 only"),
        exportRow("Style as Pa5X SMF (EXPERIMENTAL)", D::StyleStudio, 0, ExportJob::StyleSmf, "SMF job...",
                  "The Style open in Style Studio (L1 = our Korg Style validator; structurally valid, not verified on a Pa5X). UASF: our own format",
                  ExportJob::StyleUasf, "UASF job..."),
        {{(staged_ = &label("STAGED CHANGES: 0", kPending)), 0}},
        {{&label("WRITE RULES: every write goes to NAME.INCOMPLETE and is published by one rename only after its checks pass; "
                 "a cancelled or failed write leaves only NAME.INCOMPLETE.", kText2), 0}},
        {{&label("Mounted volumes (USB sticks) are refused: write locally, copy by hand in Finder, then Check a USB copy.", kText2), 0}},
    };
    // ── the job list: queued, running (progress), done, failed with the reason, cancelled; Cancel and Open Report ──
    styleText(jobView_);
    jobView_.setText("No export jobs yet.", false);
    jobPick_.setTitle("Export job");
    jobPick_.onChange = [this] { refreshJobs(); };
    cancelJobBtn_ = &button("Cancel Job", [this] { cancelJob(); });
    reportBtn_ = &button("Open Report", [this] { openJobReport(); });
    rerunBtn_ = &button("Move Leftover Aside and Re-run", [this] { rerunJobCleaning(); });
    ioPage_.rows.push_back({{&label("JOBS  (run one at a time in the order added; this window never waits for them)", kAccent), 0}});
    ioPage_.rows.push_back({{&jobPick_, 300}, {cancelJobBtn_, 100}, {reportBtn_, 110}, {rerunBtn_, 200}});
    workStatus_ = &label("", kPending);
    waitQuitBtn_ = &button("Wait, then Quit", [this] { resolveRunningWork(WorkChoice::Wait); });
    cancelQuitBtn_ = &button("Cancel Jobs and Quit", [this] { resolveRunningWork(WorkChoice::CancelJobsAndQuit); });
    keepWorkingBtn_ = &button("Keep Working", [this] { resolveRunningWork(WorkChoice::KeepWorking); });
    ioPage_.rows.push_back({{workStatus_, 0}});
    ioPage_.rows.push_back({{waitQuitBtn_, 150}, {cancelQuitBtn_, 170}, {keepWorkingBtn_, 120}});
    ioPage_.fill = &jobView_;
    ioPage_.addAndMakeVisible(jobView_);
    queue_ = std::make_unique<jobs::JobQueue>([this, alive = std::weak_ptr<bool>(alive_), posted = jobsPosted_] {
        if (posted->exchange(true)) return;   // one refresh in flight is enough: it reads the latest state
        juce::MessageManager::callAsync([this, alive, posted] {
            posted->store(false);
            if (!alive.expired()) refreshJobs();
        });
    });
    for (auto& row : ioPage_.rows) for (auto& [c, w] : row) ioPage_.addAndMakeVisible(c);
    for (juce::Component* b : {(juce::Component*)workStatus_, (juce::Component*)waitQuitBtn_, (juce::Component*)cancelQuitBtn_, (juce::Component*)keepWorkingBtn_})
        b->setVisible(false);
    ioPage_.collapse = {workStatus_, waitQuitBtn_};   // the quit prompt's rows take no space while it is hidden
    manager_.enqueueJob = [this](jobs::ExportSpec spec, std::function<void(const jobs::JobState&)> done) { return enqueue(std::move(spec), nullptr, std::move(done)); };
    // Backup KST, Convert Sound Generate and Sample Plugin Generate run in this window's one queue too (job list, busy(), Cancel Job)
    manager_.jobState = [this](uint64_t id) -> std::optional<jobs::JobState> {
        for (const auto& j : queue_->snapshot()) if (j.id == id) return j;
        return std::nullopt;
    };
    res_.setJobPort({manager_.enqueueJob, [this](uint64_t id) { queue_->cancel(id); }, manager_.jobState});
    leftovers_.setColour(juce::Label::textColourId, kPending);
    leftovers_.setColour(juce::Label::backgroundColourId, kSurface2);
    leftovers_.setJustificationType(juce::Justification::centredLeft);
    leftovers_.setMinimumHorizontalScale(1.0f);
    addChildComponent(leftovers_);   // checked by showDomain below (launch) and on every page open

    // ── Sample Lab › Quality Report (the Convert Sound recipe with its measured QUALITY section) ──
    styleText(quality_);
    qualityPage_.fill = &quality_;
    qualityPage_.addAndMakeVisible(quality_);

    // ── Sample Lab › KMP / KSF Writer (E1): the Sound previewed in Convert Sound, as KRONOS-format files ──
    styleText(kmpReport_);
    kmpReport_.setText("Nothing written yet.", false);
    kmpBtn_ = &button("Write KMP/KSF folder (EXPERIMENTAL, L1)...", [this] {
        chooseFolder("Parent folder for the new KMP/KSF folder (local disk)", [this](juce::File parent) {
            const auto in = res_.converter().model().snapshot();
            writeKmp(parent.getChildFile(juce::File::createLegalFileName(juce::String(in ? in->name : "Sound") + " KMP")));
        });
    });
    kmpPage_.rows = {{{kmpBtn_, 300}, {&reasonFor(*kmpBtn_), 0}},
                     {{&label("Writes the Sound previewed in Convert Sound as .KMP + .KSF files (KRONOS Parameter Guide format) into a NEW "
                              "local folder, read back before it is published.", kText2), 0}},
                     {{&label("EXPERIMENTAL. Verification level L1 only; not tested on a KRONOS or a Pa5X (L3 needs an owner hardware test).",
                              kExperimental), 0}}};
    kmpPage_.fill = &kmpReport_;
    for (auto& row : kmpPage_.rows) for (auto& [c, w] : row) kmpPage_.addAndMakeVisible(c);
    kmpPage_.addAndMakeVisible(kmpReport_);

    // ── Sample Lab › Tools (E4): previews are read-only; every write is a new folder ──
    styleText(labReport_);
    labReport_.setText("Choose a WAV file (Analyze, Time Slice, Loop) or a folder of WAVs (Batch, duplicates).", false);
    labInput_.setColour(juce::Label::textColourId, juce::Colours::white);
    auto field = [this](juce::TextEditor& t, const char* text) { t.setText(text, false); t.setInputRestrictions(9, "0123456789."); };
    field(labBars_, "1"); field(labBeats_, "4"); field(labStart_, "0"); field(labEnd_, "0"); field(labSnap_, "64"); field(labXfade_, "0");
    auto& chooseWav = button("Choose WAV...", [this] {
        chooseFolder("WAV file to work on (only read)", [this](juce::File f) { setLabInput(f); }, true, "*.wav;*.WAV");
    });
    auto& chooseWavDir = button("Choose WAV Folder...", [this] {
        chooseFolder("Folder of WAVs for Batch (only read)", [this](juce::File f) { setLabInput(f); });
    });
    auto lab = [this](LabTool t, const juce::String& text, const char* suffix) -> juce::TextButton& {
        return *(labButtons_[t] = &button(text, [this, t, suffix] {
            if (t == LabTool::Analyze || t == LabTool::BatchPreview || t == LabTool::DrumMap) { runLabTool(t); return; }
            chooseFolder("Parent folder for the new output folder (local disk)", [this, t, suffix](juce::File parent) {
                runLabTool(t, parent.getChildFile(juce::File::createLegalFileName(labIn_.getFileNameWithoutExtension() + suffix)));
            });
        }));
    };
    auto& an = lab(LabTool::Analyze, "Analyze (preview)", "");
    auto& sl = lab(LabTool::Slice, "Time Slice...", " slices");
    auto& lp = lab(LabTool::Loop, "Loop...", " loop");
    auto& bp = lab(LabTool::BatchPreview, "Batch Preview (duplicates)", "");
    auto& bw = lab(LabTool::Batch, "Batch...", " batch");
    auto& dm = lab(LabTool::DrumMap, "Propose Drum Map", "");
    auto& dt = lab(LabTool::DrumKitTx, "Drum Kit (TXPERF)...", " kit");
    auto& dk = lab(LabTool::DrumKitKmp, "Drum Kit (KMP/KSF)...", " kit KMP");
    labDrumMap_.setMultiLine(true);
    labDrumMap_.setReturnKeyStartsNewLine(true);
    labDrumMap_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
    labDrumMap_.setTitle("Drum map (one sample per key; edit, then build)");
    labDrumMap_.setTextToShowWhenEmpty("Choose a WAV folder and press Propose Drum Map: <key> <file> group= choke= level= pan= family=", kText2);
    labDrumMap_.onTextChange = [this] { refreshAvailability(); };
    labPage_.rows = {
        {{&chooseWav, 140}, {&chooseWavDir, 160}, {&labInput_, 0}},
        {{&an, 200}, {&reasonFor(an), 0}},
        {{&label("Bars", kText2), 40}, {&labBars_, 50}, {&label("Beats", kText2), 44}, {&labBeats_, 40}, {&sl, 130}, {&reasonFor(sl), 0}},
        {{&label("Loop start", kText2), 70}, {&labStart_, 90}, {&label("end", kText2), 32}, {&labEnd_, 90}, {&label("snap", kText2), 36},
         {&labSnap_, 50}, {&label("xfade", kText2), 40}, {&labXfade_, 60}},
        {{&lp, 130}, {&reasonFor(lp), 0}},
        {{&labTrim_, 130}, {&labNorm_, 190}, {&labConvert_, 240}, {&bp, 200}, {&reasonFor(bp), 0}},
        {{&bw, 130}, {&reasonFor(bw), 0}},
        {{&labWave_, 0}},
        {{&dm, 160}, {&dt, 170}, {&dk, 170}, {&reasonFor(dt), 0}},
        {{&labDrumMap_, 0}},
    };
    labPage_.heights = {{&labWave_, 110}, {&labDrumMap_, 110}};
    labPage_.fill = &labReport_;
    for (auto& row : labPage_.rows) for (auto& [c, w] : row) labPage_.addAndMakeVisible(c);
    labPage_.addAndMakeVisible(labReport_);

    // ── Style Studio (E3 editing: EXPERIMENTAL; the source Style is only read) ──
    styleText(styleReport_);
    styleReport_.setText("No Style open. Open Style... reads a Yamaha SFF1 .sty (or an AIArranger .uasf) for the report and for "
                         "editing. The file is only read; edits are written only as NEW files.", false);
    auto& openSty = button("Open Style...", [this] {
        chooseFolder("Style file to read (.sty, .uasf or a Korg Style SMF .mid)", [this](juce::File f) { openStyle(f); }, true, "*.sty;*.STY;*.uasf;*.mid;*.MID");
    });
    liveFromStyle_ = &button("Open Live Arranger", [this] { if (showLiveHost_) showLiveHost_(); });
    setUnavailable(*liveFromStyle_, showLiveHost_ ? "" : "Needs: the Live Arranger window (open the Studio from it)");
    for (int i = 0; i < st::StyleStudioModel::kOps; ++i) styleOp_.addItem(st::StyleStudioModel::opName(st::StyleStudioModel::Op(i)), i + 1);
    styleOp_.setSelectedId(1, juce::dontSendNotification);
    styleOp_.onChange = [this] { styleValue_.setTextToShowWhenEmpty(st::StyleStudioModel::opHint(st::StyleStudioModel::Op(styleOp_.getSelectedId() - 1)), kText2); };
    styleOp_.onChange();
    styleElem_.onChange = [this] { refreshStyle(); };
    styleTrack_.onChange = [this] { refreshAvailability(); };
    styleStatus_.setColour(juce::Label::textColourId, kAccent);
    applyBtn_ = &button("Apply", [this] { applyStyleOp(); });
    undoBtn_ = &button("Undo", [this] { styleUndo(); });
    redoBtn_ = &button("Redo", [this] { styleRedo(); });
    importSmfBtn_ = &button("Import SMF Track...", [this] {
        chooseFolder("Plain SMF (format 0/1) to import into the chosen track (only read)", [this](juce::File f) { importSmfTrack(f); }, true, "*.mid;*.MID");
    });
    exportSmfBtn_ = &button("Export Pa5X SMF (EXPERIMENTAL)...", [this] {
        chooseSave("New Pa5X Style SMF file (never overwrites)", "*.mid", [this](juce::File f) { exportStyleSmf(f.withFileExtension("mid")); });
    });
    saveUasfBtn_ = &button("Save UASF...", [this] {
        chooseSave("New UASF file (never overwrites)", "*.uasf", [this](juce::File f) { saveStyleUasf(f.withFileExtension("uasf")); });
    });
    pendSave_ = &button("Save UASF, then Open...", [this] {
        chooseSave("New UASF file for the unwritten edits (never overwrites)", "*.uasf",
                   [this](juce::File f) { resolveUnsavedStyle(UnsavedChoice::Save, f.withFileExtension("uasf")); });
    });
    pendDiscard_ = &button("Discard Edits and Open", [this] { resolveUnsavedStyle(UnsavedChoice::Discard); });
    pendCancel_ = &button("Cancel", [this] { resolveUnsavedStyle(UnsavedChoice::Cancel); });
    stylePage_.rows = {{{&openSty, 150}, {liveFromStyle_, 160}, {&label("Load the Style there with Load Style... to audition it.", kText2), 0}},
                       {{&styleKeepFirst_, 0}},
                       {{&label("Element", kText2), 60}, {&styleElem_, 220}, {&label("Track", kText2), 44}, {&styleTrack_, 220}},
                       {{&styleOp_, 170}, {&styleValue_, 170}, {applyBtn_, 80}, {&reasonFor(*applyBtn_), 0}},
                       {{&styleStatus_, 0}},
                       {{pendSave_, 200}, {pendDiscard_, 190}, {pendCancel_, 90}},
                       {{undoBtn_, 170}, {&reasonFor(*undoBtn_), 150}, {redoBtn_, 170}, {&reasonFor(*redoBtn_), 0}},
                       {{importSmfBtn_, 200}, {&reasonFor(*importSmfBtn_), 0}},
                       {{exportSmfBtn_, 260}, {&reasonFor(*exportSmfBtn_), 0}},
                       {{saveUasfBtn_, 200}, {&reasonFor(*saveUasfBtn_), 0}}};
    stylePage_.fill = &styleReport_;
    for (auto& row : stylePage_.rows) for (auto& [c, w] : row) stylePage_.addAndMakeVisible(c);
    for (auto* b : {pendSave_, pendDiscard_, pendCancel_}) b->setVisible(false);
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
    auto& chooseMid = button("Choose MIDI...", [this] {
        chooseFolder("Pa5X Style SMF to check (read-only)", [this](juce::File f) { setValidationTarget(f); }, true, "*.mid;*.MID");
    });
    useSourceBtn_ = &button("Use Library Source", [this] { if (source_) setValidationTarget(juce::File(source_->path)); });
    auto validator = [this](Validator v, const juce::String& text) -> juce::TextButton& {
        return *(validatorButtons_[v] = &button(text, [this, v] { runValidator(v); }));
    };
    auto& vp = validator(Validator::VerifyPackage, "Verify Package (L1, L2)");
    auto& ki = validator(Validator::KstInventory, "KST Inventory");
    auto& sc = validator(Validator::Sf2Check, "SF2 Check (L1)");
    auto& sm = validator(Validator::StyleSmf, "Style SMF Check (L1)");
    auto& sp = validator(Validator::SoundPy, "Sound Validator (Python)");
    auto& dp = validator(Validator::DrumKitPy, "Drum Kit Validator (Python)");
    auto& ls = validator(Validator::LibraryStructure, "Library Structure (KST/SET)");
    l3Btn_ = &button("Attach L3 Record...", [this] {
        chooseFolder("Owner's L3 hardware test record (aiarranger-l3-record)", [this](juce::File f) { attachL3Record(f); }, true, "*.txt;*.md");
    });
    validationPage_.rows = {
        {{&chooseTarget, 150}, {&chooseSf2, 130}, {&chooseMid, 130}, {useSourceBtn_, 150}, {&reasonFor(*useSourceBtn_), 0}},
        {{&target_, 0}},
        {{&vp, 200}, {&reasonFor(vp), 0}},
        {{&ki, 200}, {&reasonFor(ki), 0}},
        {{&sc, 200}, {&reasonFor(sc), 0}},
        {{&sm, 200}, {&reasonFor(sm), 0}},
        {{&sp, 200}, {&reasonFor(sp), 0}},
        {{&dp, 200}, {&reasonFor(dp), 0}},
        {{&ls, 200}, {&reasonFor(ls), 0}},
        {{l3Btn_, 200}, {&reasonFor(*l3Btn_), 0}},
        {{&badges_, 0}},
        {{&label("L1 structural: our validator checked the files.  L2 app: our reader re-opened them.  "
                 "L3 hardware: only an owner's recorded test of this exact output.", kText2), 0}},
        {{&label("Real levels: Sound/Drum Kit package L1+L2.  KMP/KSF folder, Sample Lab folder, Style SMF: L1 only.  L3: owner record only.",
                 kText2), 0}},
    };
    validationPage_.fill = &jobsLog_;
    for (auto& row : validationPage_.rows) for (auto& [c, w] : row) validationPage_.addAndMakeVisible(c);
    validationPage_.addAndMakeVisible(jobsLog_);
    jobsLog_.setText("Jobs (this session)\n", false);

    // ── Validation & Reports › Reports (read-only viewer; Export HTML + CSV is an export job) ──
    styleText(reportView_);
    reportView_.setText("No report open. Open a plan.json or NAME.export.json, or a library folder's inventory or structural validation. "
                        "Everything is only read; Export HTML + CSV writes a NEW folder.", false);
    reportStatus_.setColour(juce::Label::textColourId, juce::Colours::white);
    auto& openJson = button("Open Report File...", [this] {
        chooseFolder("plan.json or NAME.export.json (read-only)", [this](juce::File f) { openReport(ReportSource::JsonFile, f); }, true, "*.json");
    });
    auto& openInv = button("Library Inventory...", [this] {
        chooseFolder("Library folder (Pa5X KST or Pa-family SET; read-only)", [this](juce::File f) { openReport(ReportSource::Inventory, f); });
    });
    auto& openVal = button("Library Validation...", [this] {
        chooseFolder("Library folder to validate (read-only)", [this](juce::File f) { openReport(ReportSource::Validation, f); });
    });
    reportExportBtn_ = &button("Export HTML + CSV...", [this] {
        chooseSave("New report folder (report.html + CSV files)", "*", [this](juce::File f) { exportReport(f); });
    });
    reportsPage_.rows = {
        {{&openJson, 170}, {&openInv, 170}, {&openVal, 170}},
        {{reportExportBtn_, 200}, {&reasonFor(*reportExportBtn_), 0}},
        {{&reportStatus_, 0}},
        {{&label("HTML: one self-contained file (no script, no external reference). CSV: RFC 4180, one file per table, formula cells "
                 "prefixed with '. Not verified on a Pa5X.", kText2), 0}},
    };
    reportsPage_.fill = &reportView_;
    for (auto& row : reportsPage_.rows) for (auto& [c, w] : row) reportsPage_.addAndMakeVisible(c);
    reportsPage_.addAndMakeVisible(reportView_);

    segments_[D::Library] = {{"Sources", -1, &sourcesPage_}, {"Resource Graph", 0}, {"KST Library", 1}, {"Memory Check", 2}, {"Library Manager", -1, &manager_}};
    segments_[D::ImportExport] = {{"Routes", -1, &ioPage_}};
    segments_[D::SampleLab] = {{"Convert Sound", 3}, {"Sample Plugin", 4}, {"Quality Report", -1, &qualityPage_},
                               {"KMP / KSF Writer", -1, &kmpPage_}, {"Tools", -1, &labPage_}};
    segments_[D::StyleStudio] = {{"Style Editor", -1, &stylePage_}};
    segments_[D::Validation] = {{"Checks", -1, &validationPage_}, {"Reports", -1, &reportsPage_}};
    for (auto* p : {(juce::Component*)&sourcesPage_, (juce::Component*)&ioPage_, (juce::Component*)&qualityPage_, (juce::Component*)&kmpPage_,
                    (juce::Component*)&labPage_, (juce::Component*)&stylePage_, (juce::Component*)&validationPage_, (juce::Component*)&reportsPage_,
                    (juce::Component*)&manager_})
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
    checkLeftovers();   // a page opens: the banner reflects the folder as it is now
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
    for (auto* p : {(juce::Component*)&sourcesPage_, (juce::Component*)&ioPage_, (juce::Component*)&qualityPage_, (juce::Component*)&kmpPage_,
                    (juce::Component*)&labPage_, (juce::Component*)&stylePage_, (juce::Component*)&validationPage_, (juce::Component*)&reportsPage_,
                    (juce::Component*)&manager_})
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
    if (style_.dirty()) {   // never drop unwritten edits silently: ask on the page
        pendingQuit_ = nullptr;
        pendingStyle_ = sty;
        askUnsaved("Open " + sty.getFileName(), "Open");
        return;
    }
    loadStyle(sty, false);
}

bool StudioComponent::requestQuit(std::function<void()> quit) {
    if (pendingWorkQuit_) { pendingWorkQuit_ = std::move(quit); return false; }   // already asking (or waiting)
    if (!busy()) return quitIfStyleWritten(std::move(quit));
    askRunningWork(std::move(quit));
    return false;
}

// The work guard's prompt (Import / Export page): Wait, then Quit / Cancel Jobs and Quit / Keep Working.
void StudioComponent::askRunningWork(std::function<void()> quit) {
    int n = 0;
    for (const auto& j : queue_->snapshot()) n += !jobs::finished(j.phase);
    juce::StringArray what;
    if (n) what.add(juce::String(n) + " export job(s) running or queued");
    if (manager_.busy() || manager_.computing()) what.add("a Library Manager scan, report or plan");
    if (res_.backingUp()) what.add("a KST backup or backup check");
    if (res_.loading()) what.add("a package or evidence load");
    if (res_.converter().busy()) what.add("Convert Sound work");
    if (res_.sampler().busy()) what.add("a Sample Plugin render or Generate");
    if (res_.kstScanning()) what.add("a KST Inventory, Compare Folders or Merge Plan scan (read-only; it cannot be stopped, quit waits for it)");
    if (otherBusy_ && otherBusy_()) what.add(otherWhat_);
    if (jobs_ > 0 || what.isEmpty()) what.add("a read or check in progress");
    pendingWorkQuit_ = std::move(quit);
    showDomain(Domain::ImportExport);
    workStatus_->setText("Still running: " + what.joinIntoString(", ") + ". Quit AI Arranger anyway? Wait for it, cancel the jobs, or keep working.",
                         juce::dontSendNotification);
    for (juce::Component* b : {(juce::Component*)workStatus_, (juce::Component*)waitQuitBtn_, (juce::Component*)cancelQuitBtn_, (juce::Component*)keepWorkingBtn_})
        b->setVisible(true);
    ioPage_.resized();
}

void StudioComponent::resolveRunningWork(WorkChoice c) {
    if (!pendingWorkQuit_) return;
    if (c == WorkChoice::KeepWorking) {
        pendingWorkQuit_ = nullptr;
        quitWhenIdle_ = false;
        styleResolved_ = false;   // a later quit asks about unwritten Style edits again
        for (auto* b : {waitQuitBtn_, cancelQuitBtn_, keepWorkingBtn_}) b->setVisible(false);
        workStatus_->setText("Quit cancelled: the work keeps running.", juce::dontSendNotification);
        ioPage_.resized();
        return;
    }
    if (c == WorkChoice::CancelJobsAndQuit) {
        const auto all = queue_->snapshot();   // newest first: a queued job is cancelled before the running one can end and free the worker
        for (auto j = all.rbegin(); j != all.rend(); ++j) if (!jobs::finished(j->phase)) queue_->cancel(j->id);
        manager_.stopWork();
        res_.cancelJobs();   // all Sounds & Resources work: a backup or check, a render, Convert Sound / Sample Plugin jobs (never published)
        if (otherCancel_) otherCancel_();
        workStatus_->setText("Cancelling: a cancelled job stops at its next check and leaves only NAME.INCOMPLETE (never deleted); nothing is "
                             "published half-way. The app quits once everything has stopped.", juce::dontSendNotification);
    } else {
        workStatus_->setText("Waiting for the running work to finish; the app quits then.", juce::dontSendNotification);
    }
    waitQuitBtn_->setVisible(false);
    ioPage_.resized();
    const bool polling = quitWhenIdle_;
    quitWhenIdle_ = true;
    if (!polling) quitWhenIdle();
}

void StudioComponent::quitWhenIdle() {
    if (!quitWhenIdle_ || !pendingWorkQuit_) return;
    if (busy()) {
        juce::Timer::callAfterDelay(50, [this, alive = std::weak_ptr<bool>(alive_)] { if (!alive.expired()) quitWhenIdle(); });
        return;
    }
    auto quit = std::move(pendingWorkQuit_);
    pendingWorkQuit_ = nullptr;
    quitWhenIdle_ = false;
    for (juce::Component* b : {(juce::Component*)workStatus_, (juce::Component*)waitQuitBtn_, (juce::Component*)cancelQuitBtn_, (juce::Component*)keepWorkingBtn_})
        b->setVisible(false);
    workStatus_->setText("", juce::dontSendNotification);
    ioPage_.resized();
    if (std::exchange(styleResolved_, false)) { quit(); return; }   // Saved or discarded on the Style page already: not asked again
    quitIfStyleWritten(std::move(quit));
}

bool StudioComponent::quitIfStyleWritten(std::function<void()> quit) {
    if (!style_.dirty()) { quit(); return true; }
    pendingStyle_ = juce::File();
    pendingQuit_ = std::move(quit);
    showDomain(Domain::StyleStudio);
    askUnsaved("Quit AI Arranger", "Quit");
    return false;
}

void StudioComponent::askUnsaved(const juce::String& what, const juce::String& then) {
    styleStatus_.setText("The open Style has edits not written yet. " + what + " anyway? Save them, discard them, or cancel.", juce::dontSendNotification);
    pendSave_->setButtonText("Save UASF, then " + then + "...");
    pendDiscard_->setButtonText("Discard Edits and " + then);
    for (auto* b : {pendSave_, pendDiscard_, pendCancel_}) b->setVisible(true);
}

void StudioComponent::resolveUnsavedStyle(UnsavedChoice c, const juce::File& saveAs) {
    const auto next = pendingStyle_;
    auto quit = pendingQuit_;
    if (next == juce::File() && !quit) return;
    if (c == UnsavedChoice::Save) {
        saveStyleUasf(saveAs);
        if (style_.dirty()) return;   // not written (the status says why): keep asking, nothing is lost
    }
    pendingStyle_ = juce::File();
    pendingQuit_ = nullptr;
    for (auto* b : {pendSave_, pendDiscard_, pendCancel_}) b->setVisible(false);
    if (c == UnsavedChoice::Cancel) {
        styleStatus_.setText((quit ? "Quit cancelled: " : "Cancelled: ") + juce::String(style_.path().filename().string()) +
                                 " stays open with its edits.", juce::dontSendNotification);
        refreshStyle();
        return;
    }
    if (quit) {   // Saved or discarded: quit, but only through the work guard (an export may have started meanwhile)
        if (!busy()) { quit(); return; }
        styleResolved_ = true;
        if (pendingWorkQuit_) pendingWorkQuit_ = std::move(quit);
        else askRunningWork(std::move(quit));
        return;
    }
    loadStyle(next, c == UnsavedChoice::Discard);
}

void StudioComponent::loadStyle(const juce::File& sty, bool discard) {
    std::string report;
    if (!sty.hasFileExtension("uasf;mid")) {
        importers::sff1::Sff1Reader reader;
        const auto r = reader.parseFile(sty.getFullPathName().toStdString());
        report = "Style: " + sty.getFileName().toStdString() + "  (read-only; nothing is written to it)\n" + sty.getFullPathName().toStdString() +
                 "\n\n" + importers::sff1::Sff1ReportGenerator::generateReport(r);
    }
    std::string err;
    const bool ok = style_.load(sty.getFullPathName().toStdString(), &err, discard,
                                styleKeepFirst_.getToggleState() ? style_edit::SoundLoss::KeepFirst : style_edit::SoundLoss::Refuse);
    styleStatus_.setText(ok ? "Opened for editing: " + sty.getFileName() : "Not editable: " + juce::String(err), juce::dontSendNotification);
    if (!ok) {   // the read-only report may still be shown, but only when no other Style is open for editing
        if (!style_.loaded()) sff1Report_ = report;
        refreshStyle();
        return;
    }
    sff1Report_ = report;
    styleElem_.clear(juce::dontSendNotification);   // refreshStyle() fills the element choices of the new Style
    refreshStyle();
    styleElem_.setSelectedId(1, juce::dontSendNotification);
}

void StudioComponent::setValidationTarget(const juce::File& f) {
    validationTarget_ = f;
    verification_ = {};
    verificationIsPackage_ = false;
    verificationDomain_ = Domain::Validation;
    refreshAvailability();
    refreshInspector();
}

juce::String StudioComponent::validatorUnavailable(Validator v) const {
    if (jobs_ > 0) return "Needs: the running job to finish";
    if (validationTarget_ == juce::File()) return "Needs: a folder or file to check (Choose Folder... or Choose SF2...)";
    const bool isDir = validationTarget_.isDirectory();
    switch (v) {
        case Validator::VerifyPackage:
        case Validator::KstInventory:
        case Validator::LibraryStructure: return isDir ? "" : "Needs: a folder (the target is a file)";
        case Validator::Sf2Check: return !isDir && validationTarget_.hasFileExtension("sf2") ? "" : "Needs: an .sf2 file (Choose SF2...)";
        case Validator::StyleSmf: return !isDir && validationTarget_.hasFileExtension("mid") ? "" : "Needs: a Style .mid file (Choose MIDI...)";
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
            runJob("Verify Package", [target, alive]() -> Out {   // the real level of any output folder (package, KMP/KSF, Sample Lab)
                const auto r = st::verifyOutput(target, [alive] { return !alive.expired(); });
                return {r.text, r.level};
            });
            break;
        case Validator::KstInventory:
            runJob("KST Inventory", [target]() -> Out {
                return {juce::String(ui::ResourceBrowserModel::kst(ui::ResourceBrowserModel::KstAction::Inventory, target)) +
                            "\nFile level only: an inventory sets no badge (the structural check is Library Structure (KST/SET)).", std::nullopt};
            });
            break;
        case Validator::LibraryStructure:
            runJob("Library Structure", [target, alive]() -> Out {   // E8 validator: the rule table of the detected layout
                const auto r = library::validate(target, [alive] { return !alive.expired(); });
                st::Verification ver;
                ver.l1 = {!r.ok ? st::Check::NotRun : r.count(library::Severity::Error) ? st::Check::Failed : st::Check::Passed,
                          "library structure (file level): " + std::to_string(r.count(library::Severity::Error)) + " error(s)"};
                ver.l2 = {st::Check::NotRun, "not applicable: file content is not decoded"};
                return {juce::String(library::validationText(r)), r.ok ? std::optional<st::Verification>(ver) : std::nullopt};
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
        case Validator::StyleSmf:
            runJob("Style SMF Check", [target]() -> Out {
                const auto v = st::StyleStudioModel::checkStyleSmf(target);
                return {"Style SMF: " + juce::String(v.l1.detail) + " (EXPERIMENTAL; structurally valid, not verified on a Pa5X)", v};
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
                verificationDomain_ = Domain::Validation;
            }
            addJob(juce::Time::getCurrentTime().formatted("%H:%M:%S") + "  " + name + "  done" +
                   (result->second ? "  " + juce::String::fromUTF8(st::badgeLine(*result->second).c_str()) : juce::String()) + "\n" +
                   result->first + "\n");
            refreshAvailability();
            refreshInspector();
        });
    }).detach();
}

void StudioComponent::openReport(ReportSource s, const juce::File& f) {
    namespace rp = jobs::report;
    ++jobs_;
    const auto gen = ++reportGen_;
    reportStatus_.setText("Reading " + f.getFullPathName() + " (read-only, on a worker thread)...", juce::dontSendNotification);
    refreshAvailability();
    std::weak_ptr<bool> alive = alive_;
    std::thread([this, alive, s, gen, path = f.getFullPathName().toStdString()] {
        auto r = std::make_shared<rp::Report>();
        std::string err;
        const auto going = [alive] { return !alive.expired(); };
        if (s == ReportSource::JsonFile) err = rp::loadJsonReport(path, r.get());
        else if (s == ReportSource::Inventory) {
            const auto L = library::load(path, going);
            *r = rp::fromInventory(L);
            if (!L.ok) err = "library not read: " + L.error;
        } else {
            const auto v = library::validate(path, going);
            *r = rp::fromValidation(v);
            if (!v.ok) err = "NOT validated: " + v.error;
        }
        const auto text = std::make_shared<const std::string>(err.empty() ? rp::text(*r) : std::string());
        juce::MessageManager::callAsync([this, alive, gen, r, err, text] {
            if (alive.expired()) return;
            --jobs_;
            if (gen == reportGen_) {   // a newer open wins
                if (err.empty()) report_ = r;
                else report_.reset();
                reportView_.setText(juce::String::fromUTF8(text->c_str()), false);
                reportStatus_.setText(err.empty() ? "Showing (read-only): " + juce::String::fromUTF8(r->title.c_str())
                                                  : "Not shown: " + juce::String::fromUTF8(err.c_str()), juce::dontSendNotification);
            }
            refreshAvailability();
        });
    }).detach();
}

juce::String StudioComponent::reportUnavailable() const {
    if (jobs_ > 0) return "Needs: the running read to finish";
    return report_ ? "" : "Needs: a report open (Open Report File..., Library Inventory... or Library Validation...)";
}

uint64_t StudioComponent::exportReport(const juce::File& dest) {
    if (reportUnavailable().isNotEmpty()) return 0;
    reportStatus_.setText("Exporting " + juce::String::fromUTF8(report_->title.c_str()) + " as an export job...", juce::dontSendNotification);
    return enqueue(jobs::reportExport(report_, dest.getFullPathName().toStdString()), nullptr, [this](const jobs::JobState& j) {
        reportStatus_.setText(j.phase == jobs::Phase::Done ? "Exported: " + juce::String::fromUTF8(j.dest.c_str()) + "  " +
                                                                 juce::String::fromUTF8(st::badgeLine(j.level).c_str())
                                                           : "Report NOT written: " + juce::String::fromUTF8(j.reason.c_str()),
                              juce::dontSendNotification);
    });
}

// The record is applied against a FRESH L1/L2 check: files changed since the last Verify Package never get L3.
void StudioComponent::attachL3Record(const juce::File& record) {
    if (!verificationIsPackage_ || jobs_ > 0) return;
    const auto out = validationTarget_.getFullPathName().toStdString(), rec = record.getFullPathName().toStdString();
    std::weak_ptr<bool> alive = alive_;
    runJob("L3 record", [out, rec, alive]() -> std::pair<juce::String, std::optional<st::Verification>> {
        const auto c = st::verifyOutput(out, [alive] { return !alive.expired(); });
        auto v = *c.level;
        v.l3 = st::checkL3Record(rec, out, v.l1);
        return {"L3 record " + juce::String(rec) + ": " + juce::String(v.l3.detail) + "\nL1/L2 re-checked first: " + juce::String(c.text), v};
    });
}

void StudioComponent::runOutputJob(const juce::String& name, juce::TextEditor& out, std::function<st::JobResult()> work) {
    ++jobs_;
    out.setText(name + ": working...", false);
    addJob(juce::Time::getCurrentTime().formatted("%H:%M:%S") + "  " + name + "  running...");
    refreshAvailability();
    std::weak_ptr<bool> alive = alive_;
    std::thread([this, alive, name, &out, from = domain_, work = std::move(work)] {
        auto r = std::make_shared<st::JobResult>(work());
        juce::MessageManager::callAsync([this, alive, name, &out, from, r] {
            if (alive.expired()) return;
            --jobs_;
            out.setText(juce::String::fromUTF8(r->text.c_str()), false);
            if (r->level && !r->output.empty()) {   // a new output: it becomes the verification target, at its real level
                validationTarget_ = juce::File(juce::String::fromUTF8(r->output.c_str()));
                verification_ = *r->level;
                verificationIsPackage_ = true;   // it has a PACKAGE.sha256, so an owner L3 record can name it
                verificationDomain_ = from;
            }
            addJob(juce::Time::getCurrentTime().formatted("%H:%M:%S") + "  " + name + "  done" +
                   (r->level ? "  " + juce::String::fromUTF8(st::badgeLine(*r->level).c_str()) + "  " + juce::String(r->output) : juce::String()) +
                   "\n" + juce::String::fromUTF8(r->text.substr(0, r->text.find('\n')).c_str()) + "\n");
            refreshInspector();
        });
    }).detach();
}

void StudioComponent::chooseSave(const juce::String& title, const juce::String& pattern, std::function<void(juce::File)> then) {
    chooser_ = std::make_shared<juce::FileChooser>(title, juce::File(), pattern);
    auto c = chooser_;
    c->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                   [c, then, alive = std::weak_ptr<bool>(alive_)](const juce::FileChooser& fc) {
                       if (!alive.expired() && fc.getResult() != juce::File()) then(fc.getResult());
                   });
}

// ── Library › Sources: Pa-native SET inspect (E5) ──
juce::String StudioComponent::paNativeUnavailable() const {
    using K = st::SourceProbe::Kind;
    if (jobs_ > 0) return "Needs: the running job to finish";
    if (source_ && (source_->kind == K::Pa5xUserData || source_->kind == K::KstBackupCopy))
        return "Not available for a Pa5X KST: its file container is UNKNOWN (no Pa5X-made file has been studied yet)";
    return source_ && st::canInspectPaNative(*source_) ? "" : "Needs: a SET folder as the open source";
}

void StudioComponent::inspectPaNative() {
    if (paNativeUnavailable().isNotEmpty()) return;
    const auto probe = *source_;
    runOutputJob("Pa-native inspect", libraryView_, [probe] {
        auto r = st::paNativeInspect(probe.path);
        r.text = st::libraryView(probe) + "\n" + r.text;
        return r;
    });
}

// ── Sample Lab › KMP / KSF Writer (E1) ──
juce::String StudioComponent::kmpUnavailable() {
    if (jobs_ > 0 || res_.converter().busy()) return "Needs: the running job to finish";
    return res_.converter().model().built() ? "" : "Needs: a previewed Sound (Sample Lab > Convert Sound: choose a folder, Preview)";
}

void StudioComponent::writeKmp(const juce::File& dest) {
    if (kmpUnavailable().isNotEmpty()) return;
    const auto in = res_.converter().model().snapshot();   // immutable; shared with the worker without copying audio
    enqueue(jobs::kmpFolder(in, dest.getFullPathName().toStdString()), &kmpReport_);
}

// ── Sample Lab › Tools (E4) ──
void StudioComponent::setLabInput(const juce::File& f) {
    labIn_ = f;
    labInput_.setText((f.isDirectory() ? "Folder: " : "File: ") + f.getFullPathName() + "  (only read)", juce::dontSendNotification);
    refreshAvailability();
}

juce::String StudioComponent::labUnavailable(LabTool t) const {
    if (jobs_ > 0) return "Needs: the running job to finish";
    const bool folder = t == LabTool::BatchPreview || t == LabTool::Batch || t == LabTool::DrumMap || t == LabTool::DrumKitTx || t == LabTool::DrumKitKmp;
    if (folder && !labIn_.isDirectory()) return "Needs: a folder of WAVs (Choose WAV Folder...)";
    if ((t == LabTool::DrumKitTx || t == LabTool::DrumKitKmp) && labDrumMap_.isEmpty()) return "Needs: a drum map (Propose Drum Map, then edit)";
    if (folder) return "";
    return labIn_.existsAsFile() && labIn_.hasFileExtension("wav") ? "" : "Needs: a WAV file (Choose WAV...)";
}

void StudioComponent::runLabTool(LabTool t, const juce::File& dest) {
    if (labUnavailable(t).isNotEmpty()) return;
    const auto in = labIn_.getFullPathName().toStdString(), out = dest.getFullPathName().toStdString();
    const auto n = [](juce::TextEditor& e) { return e.getText().getDoubleValue(); };
    switch (t) {
        case LabTool::Analyze: {
            runOutputJob("Sample Lab analyze", labReport_, [in] { return st::labAnalyze(in); });
            ++jobs_;   // the waveform is read on a worker too
            std::thread([this, alive = std::weak_ptr<bool>(alive_), in] {
                std::string err;
                auto d = WaveView::load(in, &err);
                juce::MessageManager::callAsync([this, alive, d] {
                    if (alive.expired()) return;
                    --jobs_;
                    labWave_.setData(d);
                    refreshAvailability();
                });
            }).detach();
            break;
        }
        case LabTool::DrumMap: {   // the folder listing is file work: a worker, then the text lands in the map editor
            ++jobs_;
            refreshAvailability();
            std::thread([this, alive = std::weak_ptr<bool>(alive_), in] {
                std::string text;
                const auto err = media::samplelab::proposeDrumMap(in, &text);
                juce::MessageManager::callAsync([this, alive, err, text] {
                    if (alive.expired()) return;
                    --jobs_;
                    if (err.empty()) labDrumMap_.setText(juce::String::fromUTF8(text.c_str()), false);
                    labReport_.setText(err.empty() ? juce::String("Drum map proposed (one key per WAV from C2). Edit groups, choke, level, pan, family; then build.")
                                                   : "Drum map not proposed: " + juce::String::fromUTF8(err.c_str()), false);
                    refreshAvailability();
                });
            }).detach();
            break;
        }
        case LabTool::DrumKitTx:
        case LabTool::DrumKitKmp: {
            const auto name = labIn_.getFileName().substring(0, 20).trim().toStdString();
            enqueue(jobs::drumKitFromFolder(in, name.empty() ? "Drum Kit" : name, labDrumMap_.getText().toStdString(), t == LabTool::DrumKitKmp, out), &labReport_);
            break;
        }
        case LabTool::Slice: enqueue(jobs::labSlice(in, n(labBars_), n(labBeats_), out), &labReport_); break;
        case LabTool::Loop:
            enqueue(jobs::labLoop(in, uint32_t(n(labStart_)), uint32_t(n(labEnd_)), uint32_t(n(labSnap_)), uint32_t(n(labXfade_)), out), &labReport_);
            break;
        case LabTool::BatchPreview:   // read-only: nothing is written, so no export job
            runOutputJob("Sample Lab batch preview", labReport_, [in, tr = labTrim_.getToggleState(), no = labNorm_.getToggleState(), cv = labConvert_.getToggleState()] {
                return st::labBatch(in, tr, no, {}, cv);
            });
            break;
        case LabTool::Batch:
            enqueue(jobs::labBatch(in, labTrim_.getToggleState(), labNorm_.getToggleState(), out, labConvert_.getToggleState()), &labReport_);
            break;
    }
}

// ── Style Studio editing (E3) ──
void StudioComponent::refreshStyle() {
    if (style_.loaded()) {
        // Element choices follow the Style after every edit, undo and redo (copy adds one, rename relabels it); the chosen
        // one stays chosen while it exists, else All elements.
        const int keepElem = styleElem_.getSelectedId();
        styleElem_.clear(juce::dontSendNotification);
        styleElem_.addItem("All elements", 1);
        for (size_t i = 0; i < style_.style().sections.size(); ++i)
            styleElem_.addItem("[" + juce::String(int(i)) + "] " + juce::String(style_.style().sections[i].name), int(i) + 2);
        styleElem_.setSelectedId(styleElem_.indexOfItemId(keepElem) >= 0 ? keepElem : 1, juce::dontSendNotification);
        const int keep = styleTrack_.getSelectedId(), sec = styleElem_.getSelectedId() - 2;
        styleTrack_.clear(juce::dontSendNotification);
        styleTrack_.addItem("All tracks", 1);
        if (sec >= 0 && size_t(sec) < style_.style().sections.size())
            for (size_t t = 0; t < style_.style().sections[size_t(sec)].tracks.size(); ++t)
                styleTrack_.addItem("[" + juce::String(int(t)) + "] " + juce::String(style_.style().sections[size_t(sec)].tracks[t].name), int(t) + 2);
        styleTrack_.setSelectedId(styleTrack_.indexOfItemId(keep) >= 0 ? keep : 1, juce::dontSendNotification);
        styleReport_.setText(juce::String::fromUTF8((style_.browser() + (sff1Report_.empty() ? "" : "\n" + sff1Report_)).c_str()), false);
    } else {
        styleTrack_.clear(juce::dontSendNotification);
        styleElem_.clear(juce::dontSendNotification);
        if (!sff1Report_.empty()) styleReport_.setText(juce::String::fromUTF8(sff1Report_.c_str()), false);
    }
    undoBtn_->setButtonText(style_.undoLabel().empty() ? juce::String("Undo") : "Undo " + juce::String(style_.undoLabel()));
    redoBtn_->setButtonText(style_.redoLabel().empty() ? juce::String("Redo") : "Redo " + juce::String(style_.redoLabel()));
    refreshInspector();
}

void StudioComponent::applyStyleOp() {
    if (!style_.loaded()) return;
    const int e = styleElem_.getSelectedId() - 2, t = styleTrack_.getSelectedId() - 2;
    const auto op = st::StyleStudioModel::Op(styleOp_.getSelectedId() - 1);
    const auto why = style_.apply(op, e >= 0 ? std::optional<size_t>(size_t(e)) : std::nullopt, t >= 0 ? std::optional<size_t>(size_t(t)) : std::nullopt,
                                  styleValue_.getText().trim().toStdString());
    styleStatus_.setText(why.empty() ? juce::String("Done: ") + st::StyleStudioModel::opName(op) + " (Undo to take it back; nothing written yet)"
                                     : "Refused, nothing changed: " + juce::String(why),
                         juce::dontSendNotification);
    refreshStyle();
}

bool StudioComponent::styleUndo() { const bool ok = style_.undo(); refreshStyle(); return ok; }
bool StudioComponent::styleRedo() { const bool ok = style_.redo(); refreshStyle(); return ok; }

void StudioComponent::importSmfTrack(const juce::File& mid) {
    const int e = styleElem_.getSelectedId() - 2, t = styleTrack_.getSelectedId() - 2;
    if (!style_.loaded() || e < 0 || t < 0) return;
    size_t dropped = 0;
    const auto why = style_.importSmfTrack(mid.getFullPathName().toStdString(), size_t(e), size_t(t), &dropped);
    styleStatus_.setText(why.empty() ? "Imported " + mid.getFileName() + " into the track" + (dropped ? " (" + juce::String(int(dropped)) + " events past the element end dropped)" : juce::String())
                                     : "Refused, nothing changed: " + juce::String(why),
                         juce::dontSendNotification);
    refreshStyle();
}

void StudioComponent::exportStyleSmf(const juce::File& mid) {
    // An export job like the Import / Export Center's (staged, read back, L1 check, then published); lands in styleStatus_
    styleStatus_.setText("Exporting " + mid.getFullPathName() + " (export job; Import / Export Center lists every job)...", juce::dontSendNotification);
    if (exportJob(ExportJob::StyleSmf, mid) == 0)
        styleStatus_.setText("Not written: " + exportUnavailable(ExportJob::StyleSmf), juce::dontSendNotification);
}

void StudioComponent::saveStyleUasf(const juce::File& f) {
    const auto why = style_.saveUasf(f.getFullPathName().toStdString());
    styleStatus_.setText(why.empty() ? "Wrote " + f.getFullPathName() : "Not written: " + juce::String(why), juce::dontSendNotification);
    refreshStyle();
}

// ── Export jobs (E6): one serial queue; each finished job lands on the page that started it and becomes the
// verification target at the level its verifier gave (L3 never: only an owner record, Validation & Reports) ──
juce::String StudioComponent::exportUnavailable(ExportJob j) {
    using K = st::SourceProbe::Kind;
    switch (j) {
        case ExportJob::SoundPackage:
        case ExportJob::KmpFolder:
            if (res_.converter().busy()) return "Needs: the Convert Sound work to finish";
            return res_.converter().model().built() ? "" : "Needs: a previewed Sound (Sample Lab > Convert Sound: choose a folder, Preview)";
        case ExportJob::KstBackup:
            return source_ && (source_->kind == K::Pa5xUserData || source_->kind == K::KstBackupCopy) ? ""
                                                                                                     : "Needs: a Pa5X KST folder opened in Library Workspace > Sources";
        case ExportJob::StyleSmf:
        case ExportJob::StyleUasf: return style_.loaded() ? "" : "Needs: a Style opened for editing in Style Studio";
    }
    return "";
}

uint64_t StudioComponent::exportJob(ExportJob j, const juce::File& dest) {
    if (exportUnavailable(j).isNotEmpty()) return 0;
    const std::string d = dest.getFullPathName().toStdString();
    auto& cv = res_.converter();
    switch (j) {
        case ExportJob::SoundPackage: return enqueue(jobs::soundPackage(cv.model().snapshot(), cv.model().folder(), cv.ksfNotes(), d), nullptr);
        case ExportJob::KmpFolder: return enqueue(jobs::kmpFolder(cv.model().snapshot(), d), &kmpReport_);
        case ExportJob::KstBackup: return enqueue(jobs::kstBackup(source_->path, d), nullptr);
        case ExportJob::StyleSmf:
        case ExportJob::StyleUasf: {
            // The Style as it is now (the editor keeps working); once a UASF is written, these bytes count as written (staged -> 0).
            // A Pa5X SMF does not keep element names or track roles, so after it the edits stay staged.
            const auto uasf = style_.uasfBytes();
            const auto path = style_.path();
            auto spec = j == ExportJob::StyleSmf ? jobs::styleSmf(path.filename().string(), style_.smfExport(), d, path)
                                                 : jobs::styleUasf(path.filename().string(), uasf, d, path);
            const bool smf = j == ExportJob::StyleSmf;
            return enqueue(std::move(spec), nullptr, [this, uasf, path, smf](const jobs::JobState& s) {
                const bool ok = s.phase == jobs::Phase::Done;
                styleStatus_.setText(ok ? "Wrote " + juce::String(s.dest) + "  " + juce::String::fromUTF8(st::badgeLine(s.level).c_str()) + (smf ? " (EXPERIMENTAL)" : "")
                                        : "Not written: " + juce::String(s.reason),
                                     juce::dontSendNotification);
                if (ok && !smf && style_.path() == path) style_.markWritten(uasf);
                refreshStyle();
            });
        }
    }
    return 0;
}

// E9: every visible entry of `outputs` (packages, KMP folders, Style .mid files) into the new kit `dest`, as a job. The
// folder is listed by the job on its worker thread, never here on the message thread.
uint64_t StudioComponent::importKitJob(const juce::File& outputs, const juce::File& dest) {
    return enqueue(jobs::kit::importKit({}, dest.getFullPathName().toStdString(), outputs.getFullPathName().toStdString()), nullptr);
}

// shortcut: lists the recorded output folders (at most jobs::kRecordedFolders) on the message thread (fast on a local
// disk); move it to a worker if an output folder can be a slow network volume.
void StudioComponent::checkLeftovers() {
    juce::StringArray parts;
    for (const auto& [dir, names] : jobs::leftoverReport()) {
        juce::StringArray shown;
        for (size_t i = 0; i < names.size() && i < 4; ++i) shown.add(juce::String::fromUTF8(names[i].c_str()));
        if (names.size() > 4) shown.add("and " + juce::String(int(names.size() - 4)) + " more");
        parts.add("in " + juce::String::fromUTF8(dir.c_str()) + ": " + shown.joinIntoString(", "));
    }
    const bool show = !parts.isEmpty();
    leftovers_.setText(show ? "Unfinished writes " + parts.joinIntoString("; ") +
                                  ". Nothing is deleted by the app: check them, then move them aside or delete them in Finder."
                            : juce::String(),
                       juce::dontSendNotification);
    leftovers_.setTooltip(leftovers_.getText());   // the whole list when the banner line is cut
    if (show != leftovers_.isVisible()) { leftovers_.setVisible(show); resized(); }
}

uint64_t StudioComponent::enqueue(jobs::ExportSpec spec, juce::TextEditor* out, std::function<void(const jobs::JobState&)> done) {
    const juce::String type(spec.type);
    JobHome home{spec, out, domain_, std::move(done)};
    const auto id = queue_->enqueue(std::move(spec));
    jobHomes_[id] = std::move(home);
    if (out) out->setText(type + ": queued as export job #" + juce::String(id) + " (Import / Export Center lists every job)...", false);
    addJob(juce::Time::getCurrentTime().formatted("%H:%M:%S") + "  " + type + "  queued as job #" + juce::String(id));
    jobPick_.addItem("#" + juce::String(id), int(id));
    jobPick_.setSelectedId(int(id), juce::dontSendNotification);
    refreshJobs(false);   // never lands here: a job's `done` always runs later, never inside the caller's enqueue
    return id;
}

void StudioComponent::refreshJobs(bool land) {
    using P = jobs::Phase;
    const auto all = queue_->snapshot();
    const auto badge = [](const jobs::JobState& j) { return juce::String::fromUTF8(st::badgeLine(j.level).c_str()); };
    const auto state = [&](const jobs::JobState& j) -> juce::String {
        switch (j.phase) {
            case P::Queued: return "queued";
            case P::Done: return "done  " + badge(j);
            case P::Failed: return "FAILED at " + juce::String(jobs::phaseName(j.stoppedAt)) + ": " + juce::String(j.reason);
            case P::Cancelled: return "cancelled: " + juce::String(j.reason);
            default: return "running " + juce::String(int(j.progress * 100)) + "%  (" + jobs::phaseName(j.phase) + ")";
        }
    };
    // Land each finished job once: its page shows the result, a checked output becomes the verification target.
    for (const auto& j : all) {
        auto h = jobHomes_.find(j.id);
        if (!land || !jobs::finished(j.phase) || h == jobHomes_.end() || h->second.landed) continue;
        h->second.landed = true;
        const bool ok = j.phase == P::Done;
        juce::String warn;
        for (const auto& w : j.warnings) warn << "\nWarning: " << juce::String::fromUTF8(w.c_str());
        if (h->second.out)
            h->second.out->setText(ok ? "Job #" + juce::String(j.id) + " done. Written: " + juce::String(j.dest) + "\n" + juce::String::fromUTF8(j.verifyText.c_str()) +
                                            "\n" + badge(j) + "\nReport: " + juce::String(j.reportPath) + warn + "\n\n" + juce::String::fromUTF8(j.writerText.c_str())
                                      : juce::String::fromUTF8(j.reason.c_str()) + "\n(job #" + juce::String(j.id) + " " + jobs::phaseName(j.phase) + " at " +
                                            jobs::phaseName(j.stoppedAt) + ")",
                                   false);
        if (ok && j.level.l1.state != st::Check::NotRun) {
            validationTarget_ = juce::File(juce::String::fromUTF8(j.dest.c_str()));
            verification_ = j.level;
            verificationIsPackage_ = std::filesystem::is_regular_file(std::filesystem::path(j.dest) / media::sound::pkgio::FileSet::kPackageManifest);
            verificationDomain_ = h->second.from;
        }
        addJob(juce::Time::getCurrentTime().formatted("%H:%M:%S") + "  " + juce::String(j.type) + "  " +
               (ok ? "done  " + badge(j) + "  " + juce::String(j.dest) : state(j)));
        if (h->second.done) h->second.done(j);
    }
    const int picked = jobPick_.getSelectedId();
    jobPick_.clear(juce::dontSendNotification);
    juce::String t;
    const jobs::JobState* sel = nullptr;
    for (auto it = all.rbegin(); it != all.rend(); ++it) {   // newest first
        jobPick_.addItem("#" + juce::String(it->id) + "  " + juce::String(it->type) + "  " + jobs::phaseName(it->phase), int(it->id));
        t << "#" << int(it->id) << "  " << juce::String(it->type) << "  " << state(*it) << "\n     -> " << juce::String(it->dest)
          << (it->reportPath.empty() ? juce::String() : "\n     report: " + juce::String(it->reportPath)) << "\n";
        if (int(it->id) == picked) sel = &*it;
    }
    jobPick_.setSelectedId(picked, juce::dontSendNotification);
    if (all.empty()) t = "No export jobs yet. Each export button queues a job; it runs here while you keep working.";
    for (const auto& j : all)
        if (j.id == reportJob_) t << "\n──── REPORT, job #" << int(j.id) << "  (" << juce::String(j.reportPath.empty() ? "in memory only: nothing was published" : j.reportPath)
                                 << ") ────\n" << juce::String::fromUTF8(j.reportMd.c_str());
    jobView_.setText(t, false);
    // Actions show only when they apply to the chosen job (no disabled button without a reason).
    cancelJobBtn_->setVisible(sel && !jobs::finished(sel->phase));
    reportBtn_->setVisible(sel && jobs::finished(sel->phase) && !sel->reportMd.empty());
    rerunBtn_->setVisible(sel && sel->phase == P::Failed && sel->reason.find("Re-run with consent") != std::string::npos);
    refreshInspector();
}

void StudioComponent::selectJob(uint64_t id) { jobPick_.setSelectedId(int(id), juce::dontSendNotification); refreshJobs(); }
void StudioComponent::cancelJob() { if (jobPick_.getSelectedId() > 0) queue_->cancel(uint64_t(jobPick_.getSelectedId())); }
void StudioComponent::openJobReport() { reportJob_ = uint64_t(std::max(0, jobPick_.getSelectedId())); refreshJobs(); }

uint64_t StudioComponent::rerunJobCleaning() {
    const auto h = jobHomes_.find(uint64_t(std::max(0, jobPick_.getSelectedId())));
    if (h == jobHomes_.end() || !rerunBtn_->isVisible()) return 0;
    auto spec = h->second.spec;
    spec.moveLeftoverAside = true;   // the user's explicit consent, for this one job (renamed aside, never deleted: D17)
    return enqueue(std::move(spec), h->second.out, h->second.done);
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
    setUnavailable(*reportExportBtn_, reportUnavailable());
    setUnavailable(*paInspectBtn_, paNativeUnavailable());
    setUnavailable(*kmpBtn_, kmpUnavailable());
    for (auto& [t, b] : labButtons_) setUnavailable(*b, labUnavailable(t));
    const juce::String noStyle = style_.loaded() ? "" : "Needs: a Style opened for editing (Open Style...)";
    setUnavailable(*applyBtn_, noStyle);
    setUnavailable(*undoBtn_, noStyle.isNotEmpty() ? noStyle : style_.undoLabel().empty() ? "Nothing to undo" : "");
    setUnavailable(*redoBtn_, noStyle.isNotEmpty() ? noStyle : style_.redoLabel().empty() ? "Nothing to redo" : "");
    setUnavailable(*importSmfBtn_, noStyle.isNotEmpty() ? noStyle
                                   : styleElem_.getSelectedId() < 2 || styleTrack_.getSelectedId() < 2 ? "Needs: one element and one track chosen" : "");
    setUnavailable(*exportSmfBtn_, noStyle);
    setUnavailable(*saveUasfBtn_, noStyle);
    for (auto& [v, b] : validatorButtons_) setUnavailable(*b, validatorUnavailable(v));
    for (auto& [j, b] : exportButtons_) {
        setUnavailable(*b.first, exportUnavailable(j));
        if (b.first->isEnabled()) reasonFor(*b.first).setText(b.second, juce::dontSendNotification);   // available: where it comes from
    }
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
                  : juce::String("No source open.\nLibrary Workspace > Sources > Open Source..."));
    if (domain_ == Domain::StyleStudio)
        t << "\n\nSTYLE\n"
          << (style_.loaded() ? juce::String(style_.path().filename().string()) + " (the file is only read)\nUASF validator: passed when opened; every "
                                "edit is re-validated. In memory only: L1 belongs to an exported Style SMF."
                              : juce::String("No Style open for editing."));
    // Badges only for an output of this page (or on Validation & Reports itself): never attached to another page's output.
    t << "\n\nVERIFICATION\n";
    if (domain_ == Domain::Validation || domain_ == verificationDomain_)
        t << (validationTarget_ == juce::File() ? juce::String("No target checked yet.") : validationTarget_.getFileName()) << "\n"
          << juce::String::fromUTF8(st::badgeLine(verification_).c_str()) << "\n" << juce::String::fromUTF8(st::spokenBadges(verification_).c_str());
    else
        t << "No output of this page checked yet. The last check is in Validation & Reports.";
    const bool dirty = style_.dirty();
    t << "\n\nSTAGED CHANGES\n"
      << (dirty ? juce::String("Style Studio edits not written yet. Save UASF... writes them all to a NEW file (Export Pa5X SMF... "
                               "does not keep element names or track roles); the source Style is never changed.")
                : juce::String("0. Nothing is written until an export job runs, always into a new local folder."))
      << "\n\nNOT VERIFIED\nLoading on a Pa5X. L3 comes only from an owner's recorded hardware test.";
    inspector_.setText(t, false);
    staged_->setText(juce::String("STAGED CHANGES: ") + (dirty ? "Style Studio edits not written yet" : "0") +
                         ". Sources are never changed in place; Style Studio edits are staged until Save UASF... "
                         "writes a new file (a Pa5X SMF does not keep them all).", juce::dontSendNotification);
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
    if (leftovers_.isVisible()) { leftovers_.setBounds(r.removeFromTop(40)); r.removeFromTop(8); }
    for (auto* p : {(juce::Component*)&res_, (juce::Component*)&sourcesPage_, (juce::Component*)&ioPage_, (juce::Component*)&qualityPage_,
                    (juce::Component*)&kmpPage_, (juce::Component*)&labPage_, (juce::Component*)&stylePage_, (juce::Component*)&validationPage_, (juce::Component*)&reportsPage_,
                    (juce::Component*)&manager_})
        p->setBounds(r);
}

}  // namespace ai_arranger::app
