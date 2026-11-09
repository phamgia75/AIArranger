# macOS GUI validation, 2026-11-09 (SOFTWARE evidence; synthetic data only)

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
