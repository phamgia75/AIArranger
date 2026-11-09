# macOS GUI validation, 2026-11-09 (SOFTWARE evidence; synthetic data only)

## 1. Live-app interaction: BLOCKED on a macOS permission

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

**Remaining stall.** About 200 ms in JUCE `TextEditor::setText` for the roughly 70-line mapping table
in the Debug build. It is within the limit; not optimised.

## 4. Wording review (user-facing)

- Verdict tags: IN USE, UNPROVEN (the legend spells out "cannot be proven safe to delete"), NO REFS,
  MISSING.
- Identities: "package-local … (provisional: not a Pa5X slot)".
- Semantic Merge: "UNAVAILABLE until validated dependency data exists".
- Memory Check: "OBSERVATION ONLY".
- Convert Sound: "Layers, softest first" and "Generate writes a new package folder; nothing is sent to
  the instrument".

No hardware action is offered anywhere.
