# Pa5X First — Execution Log

Branch `feat/pa5x-first-baseline` (worktree `~/orca/workspaces/AIArranger/pa5x-first`), based on `origin/main` `40fc2ff`.
Agents actually available on 2026-11-06: ORCA (this Claude Code session, orchestration), Claude subagents used as **REA** (web research, general-purpose agent) and as the code auditor (read-only Explore agent). No separate REA service was found; "REA" here means a delegated research subagent.

## Session 2026-11-06

**Hardware:** Pa5X **not connected** to this host (CoreMIDI: 0 sources, 0 destinations; no Korg USB device). Every hardware gate below is **HARDWARE VALIDATION PENDING**.

| # | Task | Agent | Files | Commit | Tests | Result | Remaining risk |
|---|---|---|---|---|---|---|---|
| 1 | Repo audit, build, baseline tests | ORCA + Explore auditor | – | – | `cmake --build build && ctest -j4` | 119/119 at `40fc2ff` | – |
| 2 | Pa5X / PA Manager research | REA | – | – | – | Manual v1.4 read; see parity doc | MIDI chart is OS 1.0; verify on OS 1.4.x |
| 3 | Chord inversions (spec §4.3) | Claude | `src/engine/chord/chord_scan_mode.{h,cpp}`, `tests/realtime/test_chord_scan.cpp` | `6319d30` | `test_chord_scan` 23/23; full 119/119; mutation: old code fails 6 | Done | 6th chords → relative m7; HW pending |
| 4 | Input hot-plug by unique ID | Claude | `src/engine/midi/coremidi_in.{h,cpp}`, `apps/macos/Source/LiveHostComponent.cpp`, `tests/midi/test_coremidi_in_reconnect.cpp`, `CMakeLists.txt` | `08b12cd` | new test 15/15; full 120/120 | Done; JUCE app builds (`-DBUILD_MACOS_APP=ON`, Debug, links `AI Arranger.app`) | Real USB replug untested; app GUI not exercised |
| 5 | Marker grammar per Pa5X manual v1.4 | Claude | `src/exporters/korg/*`, `tests/exporters/*`, `docs/exporters/pa5x-format-evidence.md` | `f49e860` | full 120/120; POP_ACOUSTIC_2 → 10 elements, validator PASS | Done | Pa5X Import pending; Break held back |
| 6 | Baseline audit, parity matrix, roadmap, this log | ORCA | `docs/PA5X_FIRST_*.md`, `docs/KORG_PA_MANAGER_FEATURE_PARITY.md` | `cc91d04` | – | Done | – |
| 7 | P0.3 Pa5X port profile + read-only `pa5x-doctor` | Claude | `src/midi/pa5x_profile.h`, `tools/pa5x_doctor.cpp`, `tests/midi/test_pa5x_profile.cpp`, `CMakeLists.txt` | (this commit) | `test_pa5x_profile` 9/9; full 121/121; doctor on this host: exit 3 "NOT ready" (correct, no Pa5X) | Done | Positive path (exit 0) only provable with the Pa5X attached |

Reviews: OCR Level 1 (delegation mode, system rules; repo has no `.opencodereview/rule.json`) on 3, 4, 5 and 7: PASS, no findings. None of these is a critical-category change, so no Codex Level 2 was run.

| 8 | Windows Research Agent capability audit (PA Manager on a LAN Windows PC) | ORCA | `docs/research/WINDOWS_RESEARCH_AGENT.md` | (this commit) | LAN port check from the Mac: RDP open; SSH/SMB/WinRM closed. `orca computer get-app-state` on Windows App: `permission_denied` | Audit done; **no control yet** | macOS Accessibility/Screen Recording for Orca Computer Use; Path B needs owner approval |


## Session 2026-11-06 (continued, after owner authorization)

| # | Task | Agent | Files | Commit | Tests | Result | Remaining risk |
|---|---|---|---|---|---|---|---|
| 9 | Push branch, draft PR | ORCA | – | PR #38 (draft) | – | Done | Do not merge before gates |
| 10 | Enable GitHub Actions | ORCA | repo settings | – | First CI run on #38: macOS build-and-test, Linux release, ASan+UBSan, TSan **all pass** | Done: Actions on, **GitHub-owned actions only**, default token **read-only**, Actions cannot approve PRs | – |
| 11 | Locate Audio Intelligence code | Explore agent (read-only) | – | – | Searched all 296 commits, 27 local + 52 remote branches, 7 worktrees, stash, both GitHub copies (phamgia75, hieucat75), ~/dev, ~/orca, Documents/Desktop/Downloads, Claude/Codex transcripts | **Not found.** Only docs saying it is missing, and one chat prompt asking to look for it. `ai-style-maker` (not under git) has no tempo logic | Possibly on a Windows `C:\dev` machine (a `C--dev-rea-lab` transcript folder exists) or never written. Nothing recreated |
| 12 | Chord-recognition channel roles + Pa5X style-track output map | Claude | `src/midi/midi_input_router.h`, `src/control/midi_output_bridge.h`, `src/session/live_engine_facade.*`, `src/engine/integration/performer_adapter.*`, `src/midi/pa5x_profile.h`, 3 tests | `f63dc85` | 124/124; ASan+UBSan 50/50, TSan 50/50; mutation check | Done (opt-in, defaults byte-identical, C ABI unchanged) | HW pending: does the Pa5X play ch9-16 from USB on Style tracks under the user's preset? |
| 13 | #27 prep: corpus optional; fixed a test that hung without it; starter sound map | Claude + REA (14 sounds from manual appendix) | `CMakeLists.txt`, `tests/engine/test_time_signature.cpp`, `docs/exporters/sound-maps/pa5x-starter.soundmap`, test | `9e770c9` | 125/125; empty corpus: 108 pass, 16 Disabled | Done | Restricted files still in the repo and history (owner decision) |
| 14 | Windows proof-of-control | ORCA | – | – | `orca computer get-app-state` on Windows App: still `permission_denied` | **Blocked** on macOS Accessibility + Screen Recording for Orca Computer Use | – |
| 15 | Windows proof-of-control, rerun after owner granted permissions and foreground input | ORCA | `docs/research/WINDOWS_RESEARCH_AGENT.md` §5.1 | (this commit) | 5/5 steps of §5 | **Path A VERIFIED, foreground only** (background input ignored by Windows App). PA Manager v5.2 home screen observed, untouched | Notepad restored tabs with plaintext credentials; screenshot deleted, owner told to rotate. Every action takes over the Mac screen |

Flaky, not caused by this work: `test_coremidi_out` failed once ("Initialize succeeds") and passed on 2 reruns, the known CoreMIDI-server condition (#21).

## Pa5X hardware runbook (for the next session with the instrument)

Record every result in `docs/evidence/hardware/<date>-pa5x-<topic>.md`, operator present for anything audible.

1. `build/pa5x-doctor` → expect exit 0, `Pa5X KEYBOARD` + `Pa5X SOUND` online; save the output.
2. Endpoints: select `Pa5X SOUND` by unique ID; confirm with `korg-playback --dest "Pa5X SOUND"`.
3. Chords (Global roles, split C4): play C, Am, G7, Bdim in root position **and** inversions (C/E, C/G, G7/B); the arranger chord must follow (`6319d30`).
4. NTT: play F2 and F4; bass/chord register identical (`40fc2ff`).
5. Channel roles: Pa5X Default MIDI preset, Pa5X SPLIT on, `defaultPresetKeyboardRoles()`; left-hand chords drive the arranger, right-hand melody never changes the chord.
6. Style playback: Genos style with `genosStyleTrackMap()`; each Style track audible on the right Pa5X track; no Yamaha bank/PC sent.
7. USB unplug/replug during playback: input recovers by itself (`08b12cd`), output re-resolves, no stuck notes.
8. Marker SMF: Pa5X Import SMF of POP_ACOUSTIC_2 (10 elements incl. f3/f4), re-export, `aiarranger-convert --compare`; check the Break marker form.
9. Starter sound map: spot-check 8 sounds and the PC counting (0- vs 1-based).

### Blocked

- Windows/PA Manager: Path A verified (row 15). PA Manager research sessions still need the owner present (foreground input) and a copy folder of test data (§7.4).
- Hardware validation of everything above (Pa5X not attached here; the 11-05 bench is the Mac mini).
- P0.8 CI (Actions disabled — owner), P1.6 factory sound table (owner), P1.10 native KST (formats undocumented), P3.1 Audio Intelligence (source not found on this host).

### Next executable task

**P0.5 Pa5X Control-channel commands** (Style Element PCs 80-94, manual p.613), then **P0.6 MIDI clock out**, **P1.4 #26**, **P1.5 CASM High Key** (see roadmap). When the Pa5X is attached: run `build/pa5x-doctor` first and save its output under `docs/evidence/hardware/`.
