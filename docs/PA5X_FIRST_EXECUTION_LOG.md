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
| 16 | P0.5 Pa5X Control-channel commands | Claude | `src/midi/pa5x_control.h`, `src/control/midi_output_bridge.h` (`sendUnmapped`), `src/session/live_engine_facade.*` (`sendDeviceCommand`), `tests/midi/test_pa5x_control.cpp` | (this commit) | new test 11/11; full 126/126; TSan + ASan 27/27 on facade/bridge/MIDI tests; mutation (commands through the mapped path) fails 3 | Done. PC table from manual p.613-614 (raw 0-127 values per the manual). Commands bypass the style channel map, go out before the tick's notes | HW pending: needs a Control channel assigned in the user's MIDI IN Channels; whether the Default preset has one is not stated in the manual |
| 17 | CI on `bad13d0` | ORCA | – | – | PR #38: all 4 jobs green | P0.5 stays **software-verified, hardware-unverified** | – |
| 18 | P0.6 MIDI Clock out (F8 24 ppq, FA, FC) | Claude | `src/engine/uasf/types.h` (TimingClock/Start/Stop, output only), `src/engine/midi/coremidi_out.*` (1-byte encode), `src/session/live_engine_facade.*` (`setMidiClockOut`, `emitMidiClock`), `tests/midi/test_midi_clock_out.cpp` | (this commit) | 20/20: grid after every tick, Start before pulse 0, Stop last, nothing while stopped, restart, facade stop(), 60/240 BPM rates, passes the Genos map, Control PC goes out ahead of the tick's pulses, style PCs still dropped | Done. Off by default; refused while playing. Pulses at tick granularity, same as notes | HW pending. Unknown: whether the Pa5X Players start/stop on FA/FC with Clock Source = External USB, and how that interacts with Control PC 104 |
| 19 | **Defect found by P0.6 tests:** a tempo change while playing jumped the song position (position = all samples × the *new* tempo). 60→240 BPM after 1 s skipped 1440 ticks (the scheduler flushed every note in between); slowing down jumped back | Claude | `src/engine/realtime/clock.{h,cpp}` (rebase on setTempo/setSampleRate), `tests/engine/test_clock_tempo_rebase.cpp` | (this commit) | 9/9; mutation (fix reverted) fails 5 + the clock-out 240 BPM check; full 128/128; ASan 125/125, TSan 125/125 (CoreMIDI-server tests excluded) | Fixed. No tempo change → byte-identical | Rebase is not atomic across two fields: setTempo must run on the advance() thread (it does: facade applies tempo inside tick()) |
| 20 | P1.4 (#26) | ORCA | – | – | – | **Blocked**: issue is an explicit owner decision | – |
| 21 | P1.5 prerequisite. **Defect:** Ctb2 NTR/NTT read from the unused LOW zone (bytes 22-23); the applied middle zone is 28-33 (REA: Wierzba/Bedesem *Style Files* v2.1, Tables 12-16; confirmed on all 4 corpus files: split bytes 0/127, middle ≠ low on Chord1 of 3 styles and CLASSIC_6_8 Bass/Phrase1) | REA + Claude | `src/importers/sff1/sff1_{types.h,reader.cpp,report.cpp}`, `tests/engine/test_sff1_reader.cpp`, `docs/importers/casm-structure-notes.md` | (this commit) | reader 24/24 (new: middle zone wins over a decoy low zone, short entry skipped, Guitar NTR counted chord-following); mutation (offset 22) fails 2; full 128/128; ASan importer 9/9 | Fixed. Playback unaffected (the engine does not read NTR/NTT yet); import report, exporter mapping report and serialized UASF metadata now carry the applied values | Low/high zones (styles with a real split) not modelled; source is community reverse engineering, not a Yamaha spec |
| 22 | P1.5 apply CASM High Key + Note Limit | Claude, Codex (Level 2) | `src/engine/music/ntt.*` (`RegisterLimits`), `src/engine/arranger/style_player.*`, `src/engine/uasf/{types.h,format.h,serializer.cpp,deserializer.cpp}` (optional 'TRNS' extension, version stays 1), `src/importers/sff1/sff1_mapper.cpp`, `tests/engine/test_register_limits.cpp`, 2 corpus tests adjusted | (this commit) | new 20/20 (spec example, exhaustive High Key/Note Limit/defaults, TRNS round-trip and rejects, mapper rules); mutation (High Key removed) fails 2; full 129/129; ASan 126/126, TSan 126/126 (CoreMIDI excluded) | Done. **Audible change**: corpus bass with High Key D# now plays an octave lower over E..B chords (F: C bass -7, was +5); chord/pad tracks with High Key G drop over G#..B. Defaults (no CASM data, demo style) unchanged | HW pending (ear check). Committed corpus `.uasf` fixtures not regenerated (#27): their test compares against an import with limits reset. Low/high CASM zones not modelled |
| 23 | CI regression from row 18 | ORCA | `tests/midi/test_midi_clock_out.cpp`, `tests/engine/test_coremidi_out.cpp` | `582f4e3` | CI on `554a99b`/`8e5d783`: the 3 Linux jobs failed (portable test included Apple-only `coremidi_out.h`); macOS passed. After fix: **all 4 jobs green** on `582f4e3` | Fixed: encoding check moved to the Apple-only test | Lesson: build new tests against the portable target before pushing |

Reviews 16-21: OCR Level 1 (delegation, system rules): PASS after self-found fixes (row 16; row 21: Guitar NTR scoring). No Level 2 trigger applies (realtime clock is not business-day/timezone logic), so Codex was not run.
Review 22 (UASF format = schema trigger): OCR Level 1 PASS; **Codex Level 2 (independent, read-only, tree unchanged): 1 × P2**, writer accepted register limits its reader rejects (unloadable file). Reproduced with a failing test, fixed with one shared validity rule. Reconciled: Codex-only 1 true positive, no false positives.

Credentials: during the Windows proof test (row 15) Notepad showed a plaintext Meta page access token on the PA Manager PC. **REQUIRES ROTATION by the owner.** Its value is not recorded anywhere.

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
10. Control channel: assign one in Settings > MIDI > MIDI IN Channels; send Variation 1-4, Fill 1, Break, Ending 1, Play/Stop Player 1 with `sendDeviceCommand(pa5x::controlEvent(...))`; the Pa5X arranger follows.
11. MIDI Clock: `setMidiClockOut(true)` while stopped; Pa5X Settings > MIDI > General Controls > Clock Source = External USB (resets to Internal at power-on, p.595); start/stop the transport and change tempo; the Pa5X tempo display follows; record whether its Players also start/stop on FA/FC.
12. Register limits: load BALLAD_FOLK; play C, F, F#, B chords; the bass must stay near its recorded register (over F about a fourth below C, not a fifth above) and never jump octaves between F2 and F4.

### Blocked

- Windows/PA Manager: Path A verified (row 15). PA Manager research sessions still need the owner present (foreground input) and a copy folder of test data (§7.4).
- Hardware validation of everything above (Pa5X not attached here; the 11-05 bench is the Mac mini).
- P0.8 CI (Actions disabled — owner), P1.6 factory sound table (owner), P1.10 native KST (formats undocumented), P3.1 Audio Intelligence (source not found on this host).

### Next executable task

**P1.7 chord types** (P1.4 blocked on owner) (see roadmap). When the Pa5X is attached: run `build/pa5x-doctor` first and save its output under `docs/evidence/hardware/`.
