# Pa5X First — Baseline Audit

- **Date:** 2026-11-06
- **Repository:** `AIArranger` (`/Users/mac/dev/AIArranger`, origin `main`)
- **Audited commit:** `40fc2ff` (origin/main, "NTT: transpose by the chord root's pitch class, not its octave (#37)")
- **Audit branch:** `feat/pa5x-first-baseline` (worktree `~/orca/workspaces/AIArranger/pa5x-first`)
- **Host:** MacBook Pro (`MacBook-Pro-cua-mac.local`), macOS 27.0.1, arm64
- **Auditors:** ORCA (orchestration), Claude (code audit and build), REA (external research, see `KORG_PA_MANAGER_FEATURE_PARITY.md`)

## Status legend

| Status | Meaning |
|---|---|
| VERIFIED | Implemented, automated tests pass, **and** observed on a real Pa5X (with evidence document) |
| IMPLEMENTED BUT UNVERIFIED | Code and tests exist; no Pa5X hardware evidence |
| PARTIAL | Some of the capability exists; named gaps |
| MISSING | Nothing in this repository |
| BLOCKED | Cannot progress without an external input (hardware, a format spec, a legal decision) |

A passing unit test is **not** hardware validation. "VERIFIED" below always cites a hardware evidence file.

## 1. Repository state

| Item | Evidence |
|---|---|
| Branch / HEAD | `main` @ `40fc2ff`, equal to `origin/main` |
| Working tree (main checkout) | Clean except untracked `settings.local.json` (left untouched) |
| Other worktrees | `phamgia75/razorbill`, `phamgia75/taimen-2` (@ `5e72ee6`), `phamgia75/connec6-mac-mini` (@ `04d81fd`), a detached `a9787fc`, and `integ/audit-merge-2026-11-03` |
| Build system | CMake ≥ 3.20, C++20. Portable core `arranger-engine` links no Apple framework; Apple layer (CoreMIDI/CoreAudio) is a separate target. Linux build supported for the core. |
| Platforms | macOS (engine, tools, opt-in JUCE app). iOS/iPadOS: plan only. Linux: core + tests (CI job). |
| CI | `.github/workflows/ctest.yml` (macOS + Linux Release/ASan+UBSan/TSan). **Never run on GitHub — Actions disabled** (`docs/RELEASE_READINESS_AUDIT.md` §1). |

### Build and test on this host

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8
cd build && ctest -j4 --output-on-failure
```

Result at `40fc2ff`: build clean, **119/119 tests passed** (13.4 s).
Result at `6319d30` (this branch, inversion fix): **119/119 passed**. `test_coremidi_reinit` reports a documented environment skip on some runs (#21 behaviour), not a failure.

## 2. Hardware status today

- CoreMIDI enumeration on this host (2026-11-06): **0 sources, 0 destinations**. `system_profiler SPUSBDataType` shows no Korg device.
- **The Pa5X is not connected to this machine.** All hardware gates in this audit are **HARDWARE VALIDATION PENDING**.
- The only Pa5X hardware evidence in the repository is `docs/evidence/hardware/2026-11-05-pa5x-midi-audit.md` (Mac mini bench, `main` @ `9da66a9`). It is an **unattended MIDI-traffic audit**: no operator at the instrument, no ear check, no chord played on the keyboard, no Import/Export SMF round-trip.
- The bench tools used that day (`pa5x_hw_bench.cpp`, `midi_send_spy.c`, `spy_positive_control.c`) live in `~/pa5x-validation/` **on the Mac mini, not in the repository**. They are not present on this host.

## 3. NTT low-register root transposition (the previously identified defect)

| Check | Result |
|---|---|
| Fix present on current branch | **Yes.** `40fc2ff`, `src/engine/music/ntt.cpp:124-131`: `rootDelta = (target.root - srcRootMidi) mod 12`, normalised to 0..11. |
| Regression tests | `tests/engine/test_ntt.cpp` — "Bass C2 over F2 chord -> F2 (41), same as over F4", "Chord E4 over F2 chord -> A4 (69), same as over F4", the exhaustive "all 12 roots x 7 chord types x NTT modes x NTR rules: root octave 0..10 gives identical output", "every output is a valid MIDI note (0..127)", "RootFixed never shifts by the root". `tests/engine/test_sff1_load_and_play_e2e.cpp` — "F2 and F4 chords give an identical note stream", "bass over F2 stays within C-chord bass range +0..11". |
| Tests pass here | Yes (part of 119/119). |
| Verified on Pa5X | **No.** The defect was observed on the Pa5X on 2026-11-05; the fix has not been re-run on the instrument. **HARDWARE VALIDATION PENDING.** |
| Residual gap | CASM per-channel High Key and note-limit folding are parsed (`src/importers/sff1/sff1_reader.cpp:531`) but not applied, so roots above the source can lift a pattern up to +11 semitones. |

## 4. Capability matrix

| # | Capability | Status | Evidence | Tests |
|---|---|---|---|---|
| 1 | CoreMIDI output to a selected device | **VERIFIED (traffic only)** | `src/engine/midi/coremidi_out.*`; hardware audit 11-05 tests 2-3: 498 calls, all to `Pa5X SOUND` uid 1907663670, Note On = Note Off = 201 | `test_coremidi_out`, `test_midi_output_safety`, `tests/midi/test_midi_output_*` |
| 2 | Fail-closed output selection (never send to an unselected device) | **VERIFIED** | `9da66a9`; `docs/policy/MIDI_OUTPUT_SAFETY.md`; audit test 1: "0 send calls" | `test_midi_output_safety` (25 checks), `test_midi_hw_guard_env` |
| 3 | CoreMIDI input | IMPLEMENTED BUT UNVERIFIED | `src/engine/midi/coremidi_in.*`, `src/midi/midi_input_parser.*`. 11-05 audit only confirmed no echo; no notes were played. | `test_midi_input_parser`, `test_midi_input_no_thru`, `test_midi_input_to_chord` |
| 4 | Output hot-plug recovery | IMPLEMENTED BUT UNVERIFIED | Re-resolve by unique ID, `coremidi_out.cpp:276-313` | synthetic only |
| 5 | Input hot-plug recovery | IMPLEMENTED BUT UNVERIFIED (since `08b12cd`) | At `40fc2ff`: `notifyProc` empty stub, selection by index/name. Now `pollReconnect()` by unique ID, called from the macOS UI timer | `test_coremidi_in_reconnect` |
| 6 | Pa5X device identification / port profile | **MISSING** | No Pa5X-aware code. Selection is generic (any name/uid). Known names from hardware: destination `Pa5X SOUND`, source `Pa5X KEYBOARD`. | – |
| 7 | Hardware diagnostics tooling in-repo | **PARTIAL** | `tools/korg_validation/*` (latency, jitter, stuck notes, capture). The Pa5X bench + send-spy tools are off-repo. | 7 tests in `tools/korg_validation/tests` |
| 8 | Chord recognition (fingered, on-bass, single-finger) | IMPLEMENTED BUT UNVERIFIED | `src/engine/chord/*`. **Inversions fixed on this branch (`6319d30`)**; before that E-G-C gave NoChord. No 6th / m6 / 9th / m7b5 templates; no partial match (spec §4.4). "AI" mode is a placeholder that falls back to Fingered. | `test_chord_scan` (23 checks), `test_chord_latch`, `test_e2e_chord`, `test_chord_input` |
| 9 | Split / chord zone | IMPLEMENTED BUT UNVERIFIED | Single split note (default C4), `src/engine/performance/split_router.h`; upper zone excluded from chord detection (`performer_adapter.cpp:156`). Input **MIDI channel is ignored** (`midi_input_router.h:31`). | `test_split_router`, `test_e2e_split`, `test_per_channel_split` |
| 10 | NTT / NTR | IMPLEMENTED BUT UNVERIFIED | See §3 | `test_ntt`, `test_sff1_load_and_play_e2e`, `test_sff1_mapper_casm_ntr` |
| 11 | Style model (UASF) and playback; Intro 1-3, Main 1-4, Fill 1-4, Break, Ending 1-3; auto-fill | IMPLEMENTED BUT UNVERIFIED | `src/engine/uasf/*`, `src/engine/arranger/*`, `src/engine/performance/*`. 11-05 audit drove demo style start/var B/fill/ending/panic unattended — sent traffic, nobody listened. | `test_section_*`, `test_auto_fill`, `test_e2e_{variation,fill}`, `test_uasf_*` |
| 12 | Drum timing on the wire | **VERIFIED (host send-time)** | 11-05 audit test 4: p95 1.76 ms, max 2.11 ms grid deviation at 120 BPM. USB + Pa5X internal latency not measured. | `test_latency_stability`, `test_transition_latency` |
| 13 | Live channel / sound mapping to Pa5X accompaniment tracks | **MISSING** | Live playback sends on the style's own channels (Yamaha for SFF1 imports; ch 2/3/10 for the demo). 11-05 audit: ch 2/3 "reach Scene-1 keyboard parts, not the accompaniment tracks". | – |
| 14 | Program Change / Bank Select / CC output | PARTIAL | Pass-through only. SFF1 SInt Yamaha bank/PC sent **unmapped**. No Pa5X sound map in the live path. | `test_panic`, `test_stop_noteoffs` |
| 15 | MIDI clock / Start / Stop output | **MISSING** | No 0xF8/0xFA/0xFC generation; input parser drops realtime bytes. | – |
| 16 | Panic / all-notes-off | **VERIFIED (traffic only)** | 11-05 audit: CC123 + CC120 on 16 channels at shutdown, no notes left sounding | `test_panic`, `test_performer_panic` |
| 17 | Yamaha SFF1 + CASM import | IMPLEMENTED BUT UNVERIFIED | `src/importers/sff1/*`; SFF2 detection only | `test_sff1_*`, `test_import_hardening`, `test_sty_to_uasf_roundtrip` |
| 18 | Korg "Style SMF separated by markers" export | IMPLEMENTED BUT UNVERIFIED | `src/exporters/korg/*`. At `40fc2ff`: V1-4, I1-2, F1-2, E1-2 only. Since `f49e860`: I3, F3-4, E3 added per Pa5X manual v1.4 p.1035; Break still reported, not exported. `docs/exporters/pa5x-hardware-validation.md`: import on Pa5X **NOT RUN**. | `test_korg_style_smf`, `test_genos_to_korg_mapper`, `test_korg_corpus_conversion`, `test_aiarranger_convert_cli` |
| 19 | Pa5X sound map (export) | PARTIAL | User-supplied map only. `docs/exporters/pa5x-sound-mapping.md`: "Verified Pa5X sound mappings: 0 of 32 Korg tracks". | `test_korg_sound_map` |
| 20 | Native Pa5X containers (SET folder, STY/KST bank, PCG, KMP/KSF) read or write | **MISSING / BLOCKED** | `korg_style_smf.h:14`: "The native Pa5X container (KST folder) is undocumented and is NOT written." Blocked on a verified format description (see parity doc). | – |
| 21 | Sound / sample processing (WAV, SF2, KMP, multisample) | **MISSING** | No code. `coreaudio_driver` only drives the clock. | – |
| 22 | VST/AU hosting, autosampling | **MISSING** | – | – |
| 23 | Audio Intelligence (tempo, beat, harmony, audio-to-style) | **MISSING in this repo** | `docs/PROJECT_REVIEW_AND_CONTINUATION_PLAN.md` §1 says it does not exist here. Sibling `~/dev/ai-style-maker` is a 74-line Python Demucs + basic-pitch script, not under git, with **no tempo/beat/confidence logic**. The "tempo confidence / half-time / double-time" issue in the directive therefore has **no code to fix** in any repository found on this host. | – |
| 24 | Stable C ABI | PARTIAL | `include/aiarranger/aiarranger_engine.h`: lifecycle, transport, tempo, variation, panic, tick, snapshot, capabilities. **No style load, MIDI input injection, chord input or device selection.** | `test_bridge_roundtrip`, `test_bridge_c_consumer` (C11) |
| 25 | macOS application | IMPLEMENTED BUT UNVERIFIED | JUCE app `apps/macos/Source/*`, opt-in `-DBUILD_MACOS_APP=ON`. Built and linked in this audit (Debug, `08b12cd` sources); GUI not exercised. | none automated |
| 26 | iPadOS application | **MISSING** | Plan only: `docs/gate-plans/IOS_THIN_VERTICAL_SLICE_PLAN.md`. | – |
| 27 | Regression CI | **BLOCKED** | Workflow exists; GitHub Actions disabled for the repo (owner decision). | – |

## 5. Open defects and risks

| ID | Risk | Source |
|---|---|---|
| R1 | Live playback is not mapped to Pa5X accompaniment channels or sounds; Genos imports send Yamaha bank/PC to the Pa5X | Audit 11-05 notes; `src/importers/sff1/sff1_mapper.cpp:173` |
| R2 | Pa5X Import SMF round-trip never run; unknown marker codes for Break/I3/F3-4/E3 cause silent section drop | `docs/exporters/pa5x-hardware-validation.md`; `src/exporters/korg/genos_to_korg.cpp:41-43` |
| R3 | CASM High Key / note limit not applied in NTT | `sff1_reader.cpp:531` vs `ntt.cpp:124` |
| R4 | Input hot-plug recovery missing | `coremidi_in.cpp` `notifyProc` |
| R5 | Hardware evidence is not reproducible from the repo (bench tools off-repo, CI disabled) | §2 |
| R6 | GitHub issue #27: Genos `.sty` files marked "no redistribution" committed to a public repo — legal | `gh issue list` |
| R7 | GitHub issue #26: unmarked 9-bar tail imported as Break | `gh issue list` |
| R8 | No LICENSE file | `docs/RELEASE_READINESS_AUDIT.md` |
| R9 | 6th chords read as their relative m7 (C-E-G-A -> Am7) since inversion support; no 6th type exists | `6319d30` commit notes |

## 6. Work done during this audit

| Commit | Change | Tests | Hardware |
|---|---|---|---|
| `6319d30` | Chord detection recognises inversions (chord-engine spec §4.3) | `test_chord_scan` 23/23; full 119/119; mutation check: old code fails 6 assertions | HARDWARE VALIDATION PENDING |
| `08b12cd` | MIDI input reconnects the selected source by unique ID | `test_coremidi_in_reconnect` 15/15; full 120/120; JUCE app builds | HARDWARE VALIDATION PENDING |
| `f49e860` | Korg export: Intro 3 / Fill 3-4 / Ending 3 markers per Pa5X manual v1.4 | exporter tests updated; full 120/120 | HARDWARE VALIDATION PENDING |
