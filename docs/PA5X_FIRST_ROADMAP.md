# Pa5X First — Roadmap

- **Date:** 2026-11-06 · **Baseline:** `40fc2ff` (origin/main) + `feat/pa5x-first-baseline`
- **Inputs:** `PA5X_FIRST_BASELINE_AUDIT.md`, `KORG_PA_MANAGER_FEATURE_PARITY.md`
- **Rule:** nothing is "done" until Gates 1-4 pass; Gate 5 (real Pa5X) is reported separately as **HARDWARE VALIDATION PENDING** until it runs.

Complexity: S (≤1 day), M (≤1 week), L (multi-week), XL (research + build, or blocked on a format).

## P0 — Pa5X USB-MIDI and engine reliability

| # | Item | Baseline | Target | Cx | Depends on | Main risk | Acceptance tests | Hardware validation | Status |
|---|---|---|---|---|---|---|---|---|---|
| P0.1 | Chord inversions | E-G-C → NoChord | Spec §4.3 exact + inversions, bass preferred | S | – | 6th chords read as relative m7 | `test_chord_scan` exhaustive inversions | Play C/E, C/G, Am/C, G7/B on Pa5X lower zone; arranger chord follows | **Done `6319d30`** · HW partial 2026-11-07: C/E (E1-G1-C2) from real Pa5X bytes detected as C major (Gate 2, offline replay); arranger chord follow pending |
| P0.2 | Input hot-plug | Stub | Reconnect by unique ID, never another device | S | – | CoreMIDI behaviour of a real USB replug differs from virtual source | `test_coremidi_in_reconnect` | Unplug/replug Pa5X USB mid-performance; chords resume, no restart | **Done `08b12cd`** · HW pending |
| P0.3 | Pa5X port profile + read-only diagnostics CLI | Generic selection; bench tools off-repo | `pa5x-doctor`: enumerate, identify `Pa5X SOUND`/`Pa5X KEYBOARD` by name+uid, report channels, **send nothing** unless `--probe` with `AIARR_MIDI_HW_TX=1` | S | – | Name differs with class-compliant driver | Unit on name matcher; CLI test with no devices | Run on bench, attach output to `docs/evidence/hardware/` | **Done** (`tools/pa5x_doctor.cpp`) · **HW verified 2026-11-07**: identifies `Pa5X KEYBOARD` uid 534315493 / `Pa5X SOUND` uid 1907663670 on the bench, used in every pre-send check |
| P0.4 | Live Pa5X channel map | Live playback on style's own channels (Yamaha / demo 2,3,10) | Live output on Pa5X Style-track channels 9-16 under the **Default** MIDI preset (VERIFIED p.585-588), Yamaha bank/PC never sent | M | P0.3 | Default preset maps ch9-16 to Player 1 tracks *and* Style tracks; behaviour depends on the user's MIDI preset | Unit: no event leaves on ch1-8; no Yamaha bank/PC | Ear check each track on the Pa5X | **Done `f63dc85`** (opt-in `genosStyleTrackMap`) · HW pending |
| P0.5 | Pa5X Control-channel commands | None | Send Style Element PCs 80-94, Kbd Set 64-67, Play/Stop 104-107 on a configurable Control channel (VERIFIED p.613-614) — **drive the Pa5X's own arranger** as a second mode | M | P0.3 | Requires the user's MIDI preset to define a Control channel | Unit: byte-exact PC per command | Pa5X switches elements on command | **Done** (`midi/pa5x_control.h`, `LiveEngineFacade::sendDeviceCommand`; Control channel passed by the caller, no default assumed) · HW pending |
| P0.6 | MIDI clock / Start / Stop out | None | Optional 0xF8/0xFA/0xFC to the selected device | S | P0.3 | Pa5X Clock Source resets to Internal at power-on (VERIFIED p.594) | Unit: 24 ppq, start/stop ordering | Pa5X follows tempo with Clock Source = External USB | **Done** (`LiveEngineFacade::setMidiClockOut`, off by default; fixed tempo-change position jump in `RealtimeClock`) · HW pending |
| P0.7 | Re-run the 11-05 bench in-repo | Bench + spy tools on Mac mini only | Bench tool committed under `tools/`, results in `docs/evidence/hardware/` | S | Mac mini access | – | Builds in CI | Repeat audit tests 1-6, **with an operator** for ear checks | **Partial 2026-11-07**: bounded Note On/Off (audit test 2, one note) **PASS by ear** with an operator (`evidence/hardware/2026-11-07-pa5x-phaseB-retest.md`), pitch pending; tests 1, 3-6 not re-run |
| P0.8 | Regression CI | Actions disabled | CI running on PRs | S | **Owner decision** | – | Workflow green | – | **Done 2026-11-06**: Actions enabled (GitHub-owned actions only, read-only token); first run on PR #38 green on all 4 jobs |

## P1 — Style Studio and KST Manager foundations

| # | Item | Baseline | Target | Cx | Depends on | Main risk | Acceptance tests | Hardware validation | Status |
|---|---|---|---|---|---|---|---|---|---|
| P1.1 | Marker grammar per Pa5X manual | 2009 grammar (element 1-2) | v1.4 grammar; `i3 f3 f4 e3` exported | S | – | Intro 3 doubles as Count In on Pa5X | Exporter tests | Import SMF on Pa5X | **Done `f49e860`** · HW pending |
| P1.2 | Pa5X Import/Export SMF round-trip | Never run | Steps 2-8 of `pa5x-hardware-validation.md` | S | Hardware + USB stick | Pa5X rejects PPQN or tempo | `--compare` clean | **Is** the test | Blocked: hardware |
| P1.3 | Break export | Reported, not exported | Export `br` once confirmed by a Pa5X export (P1.2) and #26 fixed | S | P1.2, #26 | – | Exporter tests | Pa5X Break plays | Blocked |
| P1.4 | Fix #26 (unmarked tail imported as Break) | Open issue | Import tail handled explicitly | M | – | Behaviour change on corpus | Corpus test | – | **Blocked: owner decision** (#26 lists three readings the data cannot decide) |
| P1.5 | CASM High Key / note limit in NTT | Parsed, not applied | Applied per channel | M | – | Musical regressions; last hardware bug was in NTT | Exhaustive NTT property tests + corpus e2e | Bass/pad register on Pa5X across 12 roots | **Done** (`8e5d783` decode + this commit: High Key under Root Trans, Note Limit fold, UASF 'TRNS' block) · HW pending: ear check across 12 roots |
| P1.6 | Factory sound map from manual appendix | 0/32 verified, user-supplied | Pa5X factory CC0/CC32/PC table usable by `--sound-map` | M | **Owner decision** on reproducing the table | Copyright of the table as a compilation | Map entries well-formed | Spot-check 10 sounds on Pa5X | **Partial `9e770c9`**: 14-entry starter map from the manual, own format; full table not reproduced · HW pending |
| P1.7 | 6th / m6 / 9 / m7b5 chord types + partial match (spec §4.4) | Missing | ChordType + NTT tables extended | M | – | NTT table changes ripple through UASF | Chord + NTT tests | Ear check | **Done** (4 types; ambiguity rule; on-bass fix) · partial match (§4.4) **Done** (P1.7b) · HW partial: C7 without 5th from real Pa5X bytes detected as C7 (Gate 2, offline); ear check pending |
| P1.8 | Chord input MIDI channel filter | All channels feed detector (split by pitch only) | Honour Pa5X Global/Chord 1/Chord 2 channel roles | S | P0.3 | Depends on user's MIDI preset | Router unit tests | Pa5X Master Kbd preset | **Done `f63dc85`** (opt-in `defaultPresetKeyboardRoles`) · HW pending |
| P1.9 | KST inventory (report-only) | None | Scan a KST folder: file types, counts, byte-identical duplicates; never writes | S | – | Files are opaque; only names/hashes | Fixture trees | Compare with Pa5X Media listing | Done (SW); HW compare pending |
| P1.10 | Native `.STG`/`.KSC`/`.PCG` read/write | None | – | XL | Owner files + RE approval + legal review (gates G1-G5, `research/KST_PARSER_FEASIBILITY.md`) | Undocumented, OS-version-dependent | Oracle = PA Manager reports | – | **Blocked** (plan ready) |
| P1.11 | Read-only KST dependency graph API | Design only (`research/PA5X_KST_RESOURCE_MODEL.md` §6.3): Style/KS/Pad → Sound → OSC High/Low → Multisample → zone → Sample; reverse lookup, ref counts, closure; every edge tagged with its source | Typed nodes + edges, no writes | M | P1.10 (edges) | Without decoded and validated references the graph has no edges; never name-guessed | Unit on synthetic graphs | Compare with PA Manager "Search Where used" on the research copy | **Designed** · implementation blocked on P1.10 |

## P2 — Sound Factory and macOS / iPadOS integration

| # | Item | Target | Cx | Depends on | Main risk | Acceptance | HW | Status |
|---|---|---|---|---|---|---|---|---|
| P2.1 | Autosampler (macOS) | Host AU/VST3 instrument, render key/velocity grid with release tails to WAV | L | – | Plug-in licence terms; real-time vs offline render | Deterministic render of a test synth; trim/loop unit tests | – | Planned |
| P2.2 | Multisample writer | WAV set → **TX16Wx TXPROG** (Korg-supported since OS 1.3.0) and KMP+KSF | M | P2.1 | TXPROG field mapping (read Korg's "Making Pa5X Sounds using TX16Wx" guide first) | Independent reader round-trip | Pa5X Import; every zone plays at correct root | Planned |
| P2.3 | Memory estimator | `.PCM`/`.KMP` disk bytes vs budget (default PA Manager's 4 GiB PA5X profile) | S | P2.2 | Disk size ≠ sample RAM; compression and allocation unknown | Unit (`test_kst_inventory`) | Compare with Pa5X memory display | **Done** (disk estimate) · HW pending |
| P2.4 | C ABI completion | Style load, MIDI input injection, chord input, device selection | M | – | ABI stability | `test_bridge_c_consumer` extended | – | **Done** (contract 1.1.0; HW pending: iPad/macOS host over USB) |
| P2.5 | macOS app hardening | JUCE app exists, GUI never exercised | M | P2.4 | – | UI smoke test | Live set on Pa5X | **UI smoke test done** (`test_app_smoke`, in CI job `macos-app` since `f108183`) · Live set on Pa5X: HW pending |
| P2.6 | iPadOS app | SwiftUI over the C ABI, CoreMIDI USB to Pa5X; **no VST hosting on iPad** | L | P2.4 | Background audio/MIDI rules on iPadOS | Simulator tests | iPad + Pa5X over USB | **SW done** (simulator tests in CI, `apps/ipados`) · iPad + Pa5X: HW pending |

## P3 — AI expansion and other sources

| # | Item | Note | Status |
|---|---|---|---|
| P3.1 | Audio Intelligence (tempo/beat/harmony → style) | **No code exists in any repository on this host** (`ai-style-maker` is a 74-line Demucs/basic-pitch script without tempo logic). The "tempo confidence / half-time / double-time" issue cannot be investigated until the pipeline's source is located. Ask the owner where it lives. | Blocked: source missing |
| P3.2 | KMP/KSF import (Kronos/Nautilus sample source) | Documented chunk format; libgig reader exists | Planned |
| P3.3 | Kronos/Nautilus PCG | No public spec; Pa5X itself imports Triton PCG only | Blocked |
| P3.4 | Yamaha SFF2 | Detection only today | Planned |
| P3.5 | SF2 → Pa5X | Pa5X imports SF2 natively (no overlapping zones); a zone-overlap checker is cheap | **Done** (`sf2-check`, read-only: overlapping key×velocity zones per Instrument, linked stereo pairs exempt; manual p.1206) · HW pending: which overlaps the Pa5X actually rejects |

## Execution order (next sessions)

1. ~~P0.3~~ → 2. ~~P1.8~~ → 3. ~~P0.4~~ → 4. ~~P0.5~~ → 5. ~~P0.6~~ → 7. ~~P1.5~~ (all done; 6. P1.4 blocked on owner) → ~~8. P1.7~~ → ~~9. P1.9~~ → ~~10. P1.7b~~ → ~~11. P2.4~~ → ~~12. P2.5 smoke + #27 replacements~~ → 13. P2.6 iPadOS host over the C ABI.

Whenever the Pa5X is attached: run P1.2 and the hardware checks of every "HW pending" item **before** new P0 code, and record them under `docs/evidence/hardware/`.

## Owner decisions needed

1. ~~Enable GitHub Actions (P0.8)~~: done.
2. ~~Factory sound table~~: owner chose independent mapping data; starter map committed (P1.6).
3. Any work on native KST internals (P1.10) — needs owner-provided files and legal sign-off.
4. Audio Intelligence source (P3.1): searched 2026-11-06, **not found** anywhere reachable (see execution log). Possibly on a Windows `C:\dev` machine.
5. Issue #27 remains a legal blocker. Since `9e770c9` the corpus is optional (`AIARR_GENOS_CORPUS_DIR`; corpus tests Disabled when absent), so the files can be moved out once the owner decides; history purge is the owner's call.
