# Pa5X First — Roadmap

- **Date:** 2026-11-06 · **Baseline:** `40fc2ff` (origin/main) + `feat/pa5x-first-baseline`
- **Inputs:** `PA5X_FIRST_BASELINE_AUDIT.md`, `KORG_PA_MANAGER_FEATURE_PARITY.md`
- **Rule:** nothing is "done" until Gates 1-4 pass; Gate 5 (real Pa5X) is reported separately as **HARDWARE VALIDATION PENDING** until it runs.

Complexity: S (≤1 day), M (≤1 week), L (multi-week), XL (research + build, or blocked on a format).

## P0 — Pa5X USB-MIDI and engine reliability

| # | Item | Baseline | Target | Cx | Depends on | Main risk | Acceptance tests | Hardware validation | Status |
|---|---|---|---|---|---|---|---|---|---|
| P0.1 | Chord inversions | E-G-C → NoChord | Spec §4.3 exact + inversions, bass preferred | S | – | 6th chords read as relative m7 | `test_chord_scan` exhaustive inversions | Play C/E, C/G, Am/C, G7/B on Pa5X lower zone; arranger chord follows | **Done `6319d30`** · HW pending |
| P0.2 | Input hot-plug | Stub | Reconnect by unique ID, never another device | S | – | CoreMIDI behaviour of a real USB replug differs from virtual source | `test_coremidi_in_reconnect` | Unplug/replug Pa5X USB mid-performance; chords resume, no restart | **Done `08b12cd`** · HW pending |
| P0.3 | Pa5X port profile + read-only diagnostics CLI | Generic selection; bench tools off-repo | `pa5x-doctor`: enumerate, identify `Pa5X SOUND`/`Pa5X KEYBOARD` by name+uid, report channels, **send nothing** unless `--probe` with `AIARR_MIDI_HW_TX=1` | S | – | Name differs with class-compliant driver | Unit on name matcher; CLI test with no devices | Run on bench, attach output to `docs/evidence/hardware/` | Next |
| P0.4 | Live Pa5X channel map | Live playback on style's own channels (Yamaha / demo 2,3,10) | Live output on Pa5X Style-track channels 9-16 under the **Default** MIDI preset (VERIFIED p.585-588), Yamaha bank/PC never sent | M | P0.3 | Default preset maps ch9-16 to Player 1 tracks *and* Style tracks; behaviour depends on the user's MIDI preset | Unit: no event leaves on ch1-8; no Yamaha bank/PC | Ear check each track on the Pa5X | Planned |
| P0.5 | Pa5X Control-channel commands | None | Send Style Element PCs 80-94, Kbd Set 64-67, Play/Stop 104-107 on a configurable Control channel (VERIFIED p.613-614) — **drive the Pa5X's own arranger** as a second mode | M | P0.3 | Requires the user's MIDI preset to define a Control channel | Unit: byte-exact PC per command | Pa5X switches elements on command | Planned |
| P0.6 | MIDI clock / Start / Stop out | None | Optional 0xF8/0xFA/0xFC to the selected device | S | P0.3 | Pa5X Clock Source resets to Internal at power-on (VERIFIED p.594) | Unit: 24 ppq, start/stop ordering | Pa5X follows tempo with Clock Source = External USB | Planned |
| P0.7 | Re-run the 11-05 bench in-repo | Bench + spy tools on Mac mini only | Bench tool committed under `tools/`, results in `docs/evidence/hardware/` | S | Mac mini access | – | Builds in CI | Repeat audit tests 1-6, **with an operator** for ear checks | Blocked: needs Mac mini + operator |
| P0.8 | Regression CI | Actions disabled | CI running on PRs | S | **Owner decision** | – | Workflow green | – | Blocked: owner |

## P1 — Style Studio and KST Manager foundations

| # | Item | Baseline | Target | Cx | Depends on | Main risk | Acceptance tests | Hardware validation | Status |
|---|---|---|---|---|---|---|---|---|---|
| P1.1 | Marker grammar per Pa5X manual | 2009 grammar (element 1-2) | v1.4 grammar; `i3 f3 f4 e3` exported | S | – | Intro 3 doubles as Count In on Pa5X | Exporter tests | Import SMF on Pa5X | **Done `f49e860`** · HW pending |
| P1.2 | Pa5X Import/Export SMF round-trip | Never run | Steps 2-8 of `pa5x-hardware-validation.md` | S | Hardware + USB stick | Pa5X rejects PPQN or tempo | `--compare` clean | **Is** the test | Blocked: hardware |
| P1.3 | Break export | Reported, not exported | Export `br` once confirmed by a Pa5X export (P1.2) and #26 fixed | S | P1.2, #26 | – | Exporter tests | Pa5X Break plays | Blocked |
| P1.4 | Fix #26 (unmarked tail imported as Break) | Open issue | Import tail handled explicitly | M | – | Behaviour change on corpus | Corpus test | – | Planned |
| P1.5 | CASM High Key / note limit in NTT | Parsed, not applied | Applied per channel | M | – | Musical regressions; last hardware bug was in NTT | Exhaustive NTT property tests + corpus e2e | Bass/pad register on Pa5X across 12 roots | Planned |
| P1.6 | Factory sound map from manual appendix | 0/32 verified, user-supplied | Pa5X factory CC0/CC32/PC table usable by `--sound-map` | M | **Owner decision** on reproducing the table | Copyright of the table as a compilation | Map entries well-formed | Spot-check 10 sounds on Pa5X | Blocked: owner |
| P1.7 | 6th / m6 / 9 / m7b5 chord types + partial match (spec §4.4) | Missing | ChordType + NTT tables extended | M | – | NTT table changes ripple through UASF | Chord + NTT tests | Ear check | Planned |
| P1.8 | Chord input MIDI channel filter | All channels feed detector (split by pitch only) | Honour Pa5X Global/Chord 1/Chord 2 channel roles | S | P0.3 | Depends on user's MIDI preset | Router unit tests | Pa5X Master Kbd preset | Planned |
| P1.9 | KST inventory (report-only) | None | Scan a KST folder: file types, counts, byte-identical duplicates; never writes | S | – | Files are opaque; only names/hashes | Fixture trees | Compare with Pa5X Media listing | Planned |
| P1.10 | Native `.STG`/`.KSC`/`.PCG` read/write | None | – | XL | Owner files + RE approval + legal review | Undocumented, OS-version-dependent | – | – | **Blocked** |

## P2 — Sound Factory and macOS / iPadOS integration

| # | Item | Target | Cx | Depends on | Main risk | Acceptance | HW | Status |
|---|---|---|---|---|---|---|---|---|
| P2.1 | Autosampler (macOS) | Host AU/VST3 instrument, render key/velocity grid with release tails to WAV | L | – | Plug-in licence terms; real-time vs offline render | Deterministic render of a test synth; trim/loop unit tests | – | Planned |
| P2.2 | Multisample writer | WAV set → **TX16Wx TXPROG** (Korg-supported since OS 1.3.0) and KMP+KSF | M | P2.1 | TXPROG field mapping (read Korg's "Making Pa5X Sounds using TX16Wx" guide first) | Independent reader round-trip | Pa5X Import; every zone plays at correct root | Planned |
| P2.3 | Memory estimator | Bytes vs 4 GB user PCM | S | P2.2 | Compression ratio unknown | Unit | Compare with Pa5X | Planned |
| P2.4 | C ABI completion | Style load, MIDI input injection, chord input, device selection | M | – | ABI stability | `test_bridge_c_consumer` extended | – | Planned |
| P2.5 | macOS app hardening | JUCE app exists, GUI never exercised | M | P2.4 | – | UI smoke test | Live set on Pa5X | Planned |
| P2.6 | iPadOS app | SwiftUI over the C ABI, CoreMIDI USB to Pa5X; **no VST hosting on iPad** | L | P2.4 | Background audio/MIDI rules on iPadOS | Simulator tests | iPad + Pa5X over USB | Planned |

## P3 — AI expansion and other sources

| # | Item | Note | Status |
|---|---|---|---|
| P3.1 | Audio Intelligence (tempo/beat/harmony → style) | **No code exists in any repository on this host** (`ai-style-maker` is a 74-line Demucs/basic-pitch script without tempo logic). The "tempo confidence / half-time / double-time" issue cannot be investigated until the pipeline's source is located. Ask the owner where it lives. | Blocked: source missing |
| P3.2 | KMP/KSF import (Kronos/Nautilus sample source) | Documented chunk format; libgig reader exists | Planned |
| P3.3 | Kronos/Nautilus PCG | No public spec; Pa5X itself imports Triton PCG only | Blocked |
| P3.4 | Yamaha SFF2 | Detection only today | Planned |
| P3.5 | SF2 → Pa5X | Pa5X imports SF2 natively (no overlapping zones); a zone-overlap checker is cheap | Planned |

## Execution order (next sessions)

1. P0.3 `pa5x-doctor` (read-only) → 2. P1.8 channel filter → 3. P0.4 live channel map → 4. P0.5 Control-channel commands → 5. P0.6 clock → 6. P1.4 #26 → 7. P1.5 CASM High Key → 8. P1.7 chord types → 9. P1.9 KST inventory → 10. P2.4 C ABI.

Whenever the Pa5X is attached: run P1.2 and the hardware checks of every "HW pending" item **before** new P0 code, and record them under `docs/evidence/hardware/`.

## Owner decisions needed

1. Enable GitHub Actions (P0.8).
2. Reproduce the Pa5X factory sound table in-repo (P1.6).
3. Any work on native KST internals (P1.10) — needs owner-provided files and legal sign-off.
4. Location of the Audio Intelligence pipeline source (P3.1).
5. Issue #27 (Genos `.sty` files with "no redistribution" in a public repo) remains a legal blocker for release.
