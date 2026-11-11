# macOS GUI validation, 2026-11-09 (SOFTWARE evidence; synthetic data only)

## 00. Sample Plugin tab (2026-11-10)

### Map and estimate layout: hands-on inspection PENDING (screen capture blocked)

**Offscreen evidence only (not a hands-on PASS).** The smoke test renders the tab offscreen at 980x640
(`createComponentSnapshot`; set `AIARR_SNAPSHOT_DIR` to save a PNG) and measures the estimate label with
JUCE's own text layout: the default form needs 3 of its 4 lines; the longest map (24 layers, highest note off
the sampling step) needs 4 of 4. That offscreen render showed one defect, now fixed: before rendering, the
empty key map said "Choose a WAV folder and press Preview", the Convert Sound tab's instruction; it now says
"Render Samples to see the key and velocity map of the rendered Sound." Offscreen rendering does not show
real window chrome, scaling, focus or VoiceOver.

**Checklist for the hands-on pass** (when screen capture works; verify the foreground window and take a
screenshot before each step; no MIDI, no hardware):
1. Open Sounds & Resources > Sample Plugin at the default window size and at the smallest size the window allows.
2. Find Instruments; choose DLSMusicDevice. Type `C3` / `c5` / `12` / `60,110`: the estimate shows 3 lines,
   the map line reads "Samples at C3, C4, C5; every key C-1 to G9 plays ...; velocity 0-85, 86-127." Nothing is cut or squashed.
3. Type 24 velocities `5,10,...,120` and Highest note `C#5`: the map shows "(24 layers) 0-7, 8-12, 13-17, ..., 113-117, 118-127."
   and "C#5 is not on the sampling step; the top sample is C5." in at most 4 lines.
4. Type `H2` as Lowest note: "Not ready: lowest and highest note: a name such as C2 or F#4 ...". Turn Loop on with Hold `0.5`:
   "Not ready: a loop needs a hold of at least 0.61 s ...".
5. Render with Loop on, Hold `1.0`: the key map shows 2 layers x 3 zones covering C-1 to G9; the status has no loop warning.
6. Generate to a new local folder: Resource Graph opens it; no `NAME.INCOMPLETE` is left beside it.
7. VoiceOver: each field reads its label; the estimate label is read in full.

**The workflow.**
1. Find Instruments runs `aiarranger-plugin-sampler list` in a child process.
2. Choose an instrument and set the notes, step, velocities, splits, hold, release, loop and stereo.
3. An estimate shows the samples, the audio seconds, the package size and the memory needed. The same
   rules the sampler uses mark the job ready or not.
4. Render Samples runs the sampler in a separate process, with a progress bar and Cancel.
5. The rendered WAVs are previewed with the Convert Sound engine: key map, checks, mapping table and
   the package REPORT.
6. Generate Package... copies the staged package to a new local folder.
7. The package opens in Resource Graph.

**Tested in `test_app_smoke`** (CI `macos-app`). Fake sampler scripts covered:
- a plugin load failure, where the tool's own message is shown
- exit 0 without a package
- an unreadable WAV in the output
- progress, then Cancel: the process is killed and staging removed
- closing the window during a render: the process is killed in under 2 s and staging removed

A real offline render with Apple **DLSMusicDevice**, an instrument that ships with macOS (no
third-party licence involved), covered:
- 2 layers x 3 notes, previewed
- a 30 ms longest UI stall while rendering
- refusal of a `/Volumes` destination and of an existing folder
- the package copied and opened in Resource Graph
- a form edit after the render disabling Generate

The `/Volumes`-through-symlink refusal is covered by `test_plugin_sampler_ui`.

**Measured.**

| Run | Result |
|---|---|
| Large job, sampler CLI | 183 stereo notes (C2-C7 every semitone x 3 velocities, 3 s): 67 s, 247 MB package, peak 1024 MB |
| Medium job, sampler CLI | 63 notes: peak 373 MB |
| Baseline, sampler CLI | 56 MB |
| Soak, in the app (`AIARR_SMOKE_SOAK=10`) | 10 real renders in a row: app memory 242 MB, then 133 MB; no growth |

**Defects found and fixed during this work, with regressions:**
- Progress lines arrived only at the end. JUCE `ChildProcess::readProcessOutput` uses `fread`,
  which blocks until a 4 KB buffer fills. Output is now read one byte at a time.
- The first memory estimate counted only the float renders, about 4.5x too low against the measured
  peak. It now uses the measured 18 bytes per frame and channel plus a baseline, checked against the
  large job within 15%.
- A re-scan kept the previous instrument list selectable until the new scan finished.

**Not done hands-on.** The live click-through of this tab. The display was asleep and Orca's
accessibility reads were blocked for every app, Finder included. It needs the owner to wake and
unlock the Mac. VoiceOver could not be checked for the same reason.

**KST Library hands-on, prepared.** `tools/make_synthetic_kst_library.py <dir>` writes two original
synthetic folders, SYNTH_A.KST and SYNTH_B.KST. They use the documented Pa5X extensions and contain
placeholder bytes, not Korg data. The planned differences are 4 identical, 2 changed, 2 only in A
and 1 only in B, plus a duplicate inside A. Inventory, Compare and Merge Plan report exactly these
on the command line. The self-test runs in CTest.

## 0. Update: hands-on GUI pass after the owner restored the permissions (same day)

Both permissions were restored. Finder's accessibility tree read back, and its screenshot was
captured.

**Setup.** The Debug app was launched at `0089cb3`, and later rebuilt at the commit that holds
these fixes, with `AIARR_MIDI_HW_TX=0`. MIDI Out stayed "(none)" and the status line read
"tx: 0" throughout. The app was driven with real accessibility clicks, field edits and pointer
clicks through `orca computer`.

**Test data.** A synthetic folder of 183 stereo WAVs (101 MB): 3 layers, pp, mf and ff, 61 keys
from C2 to C7, sine tones with a `smpl` root and loop. It lived in a temporary folder under the
home directory and was removed afterwards.

| Step | Real interaction | Result |
|---|---|---|
| Open the window | Clicked "Sound Tools..." | A "Sounds & Resources" window opened with 4 tabs |
| Choose the source | Clicked Choose WAV Folder... and used the native macOS panel | Folder accepted: "3 layer(s): ff (61 files, C2-C7) mf ... pp ..." |
| Edit the fields | Set the name and the layer order by accessibility value | Both values read back |
| Preview | Clicked Preview | Ready after 9.1 s (Debug, 101 MB). All 33 accessibility reads during decoding answered; the slowest took 565 ms, against about 300 ms for an idle read |
| Key map and texts | Screenshot | 3 rows, loudest on top; checks; mapping table; recipe |
| Generate | Clicked Generate Package..., chose a folder in the native panel | Written in about 32 s, then opened in Resource Graph automatically. All 101 reads during the write answered; the slowest took 557 ms |
| Files | Inspected on disk | SF2 (105 469 234 bytes), `tx/` with the TX16Wx program and 183 WAVs, `wav/` with 183 files, `REPORT.txt`, `RESOURCES.tsv`. Parsed independently: 366 sample headers (183 stereo pairs), 1 preset, 3 instruments; the first sample has 144 000 frames, loop 48 000-119 920 and root 36 |
| Resource Graph | Pointer clicks to expand Sound → oscillator → multisample → sample | Details: provisional identity, 576 000 bytes exact, Where Used 1 reference, IN USE |
| Close and reopen | Close button, then "Sound Tools..." | The window hides and comes back with its state kept |
| Memory Check | Synthetic readings typed into both fields, then Compare | Counts MATCH the open package; size growth CONSISTENT with raw PCM |
| Not exercised | KST Library tab | No authorized KST folder for this session |

**Defects found in the hands-on pass, fixed with regressions:**

| Defect | Fix | Regression |
|---|---|---|
| The proposed layer order was alphabetical (`ff,mf,pp`, and `Hard,Soft`), so the loudest layer came first | Dynamic names (ppp..fff, soft/medium/hard) and trailing numbers are ranked softest first; any other name falls back to alphabetical | `test_sound_conversion_ui`, `test_app_smoke` |
| WAVs without a `smpl` chunk (most DAW and library exports) were refused although the file name states the root | The WAV-folder import takes the root from the file name and treats the sample as a one-shot; package loading stays strict | `test_wav_folder_source` (2 checks) |
| Sound Recipe lines needed horizontal scrolling | The recipe pane wraps prose to its width; the mapping table does not wrap | `wrapLines` unit test; pane check in `test_app_smoke` |
| Resource Graph rows were announced as "Level 0 row 0" | Rows are announced with their label and tag, e.g. "Sound 'Sine Layers', NO REFS" (re-checked live) | `test_app_smoke` |

**Panel behaviour worth knowing.** Pressing an accessibility action on a row in the native
folder panel confirms the panel's current selection. In this pass that selected
`~/korg-pa5x-workspace`, a folder left selected from earlier use. The app only listed the file
names and refused it ("no <layer>_... files"). Nothing was read beyond the names, and nothing was
written.

**Not verified.** VoiceOver reading of the read-only text panes. JUCE exposes them as static text
with a text-range interface but no value.

## 1. Live-app interaction (earlier the same day): BLOCKED on a macOS permission

- The app was launched from the build (`AI Arranger.app`, Debug, pid 58385) with `AIARR_MIDI_HW_TX=0`,
  so CoreMIDI sends are blocked at the boundary. No output port was selected.
- **Driving it from outside failed:** `orca computer get-app-state` returned "visible windows but no
  accessibility window (AX reads stayed blocked)" for **every** app, Finder included, and
  `screencapture -l` could not capture the window.
- `orca computer permissions` reports both permissions as granted, so the helper's macOS grant has gone
  stale. That is an owner action: System Settings → Privacy & Security → Accessibility, switch
  **Orca Computer Use** off and on, and allow Screen Recording for the terminal.
- **Not yet done:** real pointer and keyboard interaction with the running app, native file dialogs,
  and window open/close by hand.

## 2. What was validated instead (in-process, the real components)

`apps/macos/Tests/app_smoke.cpp` (CTest `test_app_smoke`, runs in CI `macos-app`) constructs the real
`LiveHostComponent` and `ResourcesComponent`. It drives them through the same entry points the buttons
call, runs the JUCE message loop and reads back what the user sees. 57 checks, all passing.

| Area | Checked |
|---|---|
| Window entry | "Sound Tools..." button on the live host; window has 4 tabs (Resource Graph, KST Library, Memory Check, Convert Sound) |
| Resource Graph | Open 2 packages; the same package twice is refused with a reason; Sound → oscillators → multisample → samples; sample details (exact bytes, where used, IN USE, provisional identity); budget and unresolved summary |
| Background loading | Package and evidence loads run off the message thread; the window can be destroyed mid-load without a crash; Close All mid-load drops the load |
| KST Library | Inventory runs in the background; Semantic Merge is reported unavailable |
| Memory Check | The untouched template compares without error; typed readings match the open packages |
| Convert Sound | A folder of wrongly named files gives a clear message; inspect proposes order and splits; preview builds the map, checks, exact bytes and recipe; generate writes the package and opens it in Resource Graph; an existing folder is refused |
| Responsiveness | 30 MB Sound (64 stereo samples): longest message-loop gap 285–300 ms during preview, about 30 ms during generate and 30–140 ms during load (limit 500 ms) |

**Rendered screenshots.** The tabs were rendered from the live component tree with
`createComponentSnapshot` and inspected. The images stay in the session scratchpad: they hold only
synthetic content, but screenshots are not committed by rule.

## 3. Defects found and fixed

| Defect (how it was seen) | Fix | Regression |
|---|---|---|
| Key map: a one-key zone was painted entirely in the root colour, so zones were invisible (snapshot) | Root drawn as a tick under the zone | Pixel check in `test_app_smoke` |
| Key map: octave labels were overpainted by keys, then unreadable over black keys (snapshot, zoomed) | Labels drawn last, in their own row under the keyboard | Visual (snapshot re-checked) |
| Preview text built on the message thread (about 270 ms measured) | Checks, mapping table and recipe are built on the worker | Stall measurement in `test_app_smoke` |

**Former stall, fixed.** JUCE `TextEditor::setText` took about 200 ms for the 70-line mapping table
(632 ms on a CI runner, failing the check). The preview and recipe now use a read-only code view that
lays out only visible lines; the longest preview stall is about 30 ms.

## 4. Wording review (user-facing)

- Verdict tags: IN USE, UNPROVEN (the legend spells out "cannot be proven safe to delete"), NO REFS,
  MISSING.
- Identities: "package-local … (provisional: not a Pa5X slot)".
- Semantic Merge: "UNAVAILABLE until validated dependency data exists".
- Memory Check: "OBSERVATION ONLY".
- Convert Sound: "Layers, softest first" and "Generate writes a new package folder; nothing is sent to
  the instrument".

No hardware action is offered anywhere.
