# KORG PA Manager — Feature Parity Matrix (Pa5X First)

- **Date:** 2026-11-06
- **Prepared by:** REA (research), reconciled by ORCA against the code audit in `PA5X_FIRST_BASELINE_AUDIT.md`
- **Scope:** what the third-party Windows application *PA Manager* (korgpamanager.com, latest build 5.2.2616, 16 Oct 2026) publicly claims, against what AIArranger has. No PA Manager installation was accessed. No proprietary code, binaries or file layouts were examined, and none will be. Every AIArranger implementation proposed here is independent.

## Evidence tags

| Tag | Meaning |
|---|---|
| **VERIFIED** | Read in an official Korg document (URL given) |
| **DOCUMENTED** | Vendor or third-party page, changelog or forum (URL given) |
| **HYPOTHESIS** | Inference, needs confirmation |
| **UNSUPPORTED** | Searched, no source found |

PA Manager rows are all **DOCUMENTED** (the vendor's own site and changelog). The vendor's features page is marketing text; nothing here claims parity from it.

Primary sources:
- Pa5X User Manual v1.4 (2025-11-25): https://www.korg.com/us/support/download/manual/0/895/5309/ (page numbers are printed page numbers)
- PA Manager features: https://www.korgpamanager.com/features/ · v5.2: https://www.korgpamanager.com/5.2/ · changelog: https://korgpamanager.com/download/changes

## Facts that reshape the work (VERIFIED, Pa5X manual)

1. **`.KST` is a folder**, the Pa5X user-data container that replaces `.SET` (p.721). Inside: Styles are **`.STG`** (not `.STY`), Keyboard Sets **`.KSC`**, Pads `.PDC`, Chord Sequences `.CSC`, SongBook `.SBD/.SBL`, Global `.GBL`, MIDI Presets `.MPR`, Sounds **`.PCG`**, Multisamples **`.KMP`**, Samples **`.PCM`**. The binary layouts are **not published** (UNSUPPORTED).
2. The Pa5X converts **only Pa4X `.SET` → `.KST`, one way** (p.737-745), and writes `ConvertReport.txt`.
3. On-instrument import supports **Triton PCG**, **KMP/KSF**, **SF2** (no overlapping zones), **TX16Wx TXPROG/TXPERF** (OS ≥ 1.3.0), and **WAV/AIFF folders** (p.1155, 1206-1216). Kronos/Nautilus PCG and Yamaha styles are **not** listed.
4. Style Elements, Keyboard Sets and Players have **fixed Program Change numbers on the Control channel** (p.613-614). Remote Style selection goes through SongBook Entries by NRPN (p.333, 612). No public SysEx spec exists.
5. "Style as SMF separated by markers" grammar (p.1035-1036): `v1-4`, `i1-3`, `f1-4`, `e1-3` + `cvN`, and `br`.

## Matrix

Columns map to the directive's nine fields: (1) what PA Manager demonstrably does, (2) inputs, (3) outputs, (4) constraints, (5) AIArranger today, (6) gap, (7) suggested independent implementation, (8) test method, (9) hardware validation. Column (5) and (6) updated 2026-11-09 for work built after 2026-11-06; columns (1)-(4) unchanged.

| Feature | (1) PA Manager does | (2)→(3) | (4) Constraints | (5) AIArranger today | (6) Gap | (7) Independent implementation | (8) Test method | (9) HW validation |
|---|---|---|---|---|---|---|---|---|
| **SET/KST merging** | "Edit / Mix / Combine SETs"; "COPY from another KST" (C 5.2.1030) | SET/KST → SET/KST | Merge algorithm not described (marketing only) | `kst-merge-plan` (file-level Merge / Replace All prediction, read-only, 2026-11-08) | Partial: no Sound-aware merge, no KST write | Not feasible to write KST binaries (undocumented). **Feasible:** a KST *planner* that inventories two KST folders by file name/type and produces a Pa5X-side merge plan (the Pa5X's own Load > Merge "replaces only files with the same name", p.731-733) | Fixture KST folder trees (names only, empty files) | Load the planned files on the Pa5X with Merge; compare result listing |
| **Sound remapping** | Global Sound reMAP; remap to Factory; PCG remap (F; fixes in 5.2.1428/2226/2616) | SET/KST → SET/KST | Remap repeatedly buggy per changelog (HYPOTHESIS: hard) | Export-side sound map from a user file (`sound_map.h`); 0/32 verified | No factory table; no native write | Build `--sound-map` from manual appendix CC0/CC32/PC (from p.1572), after owner decision on reproducing the table | Unit: every map entry is a valid bank/PC triple; exporter writes it at tick 0 | Import SMF, confirm each track's sound name on the Pa5X |
| **Multisample processing** | Zone editor, AutoMS, Note→OrigKey, round robin, stereo → KMP, L/R split (5.0.1123, 5.1.2105, 5.2.1030) | WAV/KSF → set multisamples, KMP | – | Built SOFTWARE: instrument model, TXPROG/SF2/WAV writers, AU sampler, WAV-folder source (roadmap S1-S10) | Not hardware-validated (S6); no KMP | Sound Factory (P2): WAV set → **TX16Wx TXPROG** (Korg-supported import since OS 1.3.0) and/or KMP+KSF (documented in Korg Triton manual appendix; libgig reader exists) | Golden files; round-trip through an independent reader | Pa5X Import of TXPROG/KMP; play every zone |
| **Duplicate detection & cleanup** | Duplicate samples, unused Sounds/Samples auto-delete, unreferenced counts, empty-track Styles (5.0.1001, 5.1.2010, 5.1.2216) | Set → cleaned set, report | Whether duplicates are removed or only flagged: not stated | `kst-inventory` byte-identical duplicates (report only) | No name/usage level; no unused detection | Content hash of sample files inside a KST folder (`.PCM` byte-identical duplicates) — **report only**, never delete in place | Fixtures with duplicated files | None needed for report; any delete is done on the Pa5X by the user |
| **Style editing** | Event editor (Pa5X since 5.2.1030), copy/create Variations, MIDI import/export, Style→MIDI markers, Chord Sequencer | Style, SMF → Style, SMF | Features page and changelog contradict on Pa5X Style playback | UASF style model; Genos import; **marker-SMF export (Pa5X documented path)**, validator, independent parser, `--compare` round-trip | Native `.STG` write; Chord Table / Expression not encodable in SMF (p.1034) | Keep marker SMF as the Pa5X interchange; Style Studio edits UASF and exports marker SMF | Existing exporter tests + corpus | **Pending:** Pa5X Import SMF + Export SMF round-trip (`pa5x-hardware-validation.md`) |
| **Drum Kit editing** | Keys/samples, round robin, stereo→mono, WAV export (F, C) | Set → set, WAV | No full editor described | Drum channel mapping; TXPERF Drum Kit writer (roadmap S12) | Not hardware-validated (S6); no KST Drum Kit edit | Drum key map report (export side); kit building via TXPERF (Korg: performance → Drum Kit) in Sound Factory | Golden TXPERF | Pa5X import; play every key |
| **Keyboard Sets** | Edit sounds, copy banks, repair corrupt KBD SETs (5.1.2010, 5.1.2222) | KST → KST | – | None | Total | Out of scope until `.KSC` is documented. Control-channel PC 64-67 selects Kbd Set 1-4 (VERIFIED) — implement in live control | Unit: PC bytes on the configured Control channel | Pa5X switches Kbd Set |
| **Kronos/Nautilus PCG import** | Imports PCG programs from Trinity/Triton/M3/Kronos/Nautilus (F, C 5.1.2010) | PCG + samples → Sounds | No public Kronos/Nautilus PCG spec (UNSUPPORTED); libgig: PCG "differs quite a lot between models… and OS versions" | None | Total | P3. Only KMP/KSF (stable, documented) as a source; PCG blocked without a spec or owner-provided files + RE approval | – | – |
| **Legacy PA conversion** | Pa4X/3X/2X/800 conversions; SET→KST since 5.2.1030; KST↔5X MG | SET → SET/KST | One-way to KST | None | Total | Use the Pa5X's own Data Conversion for Pa4X (official). AIArranger adds value only for **non-Korg** sources (Genos → marker SMF, done) | – | Pa5X Data Conversion report |
| **Sample memory management** | RAM window, streaming large KSTs, compression, MB before copy (5.2.1030, 5.1.2105) | Set → info | 32-bit build ~3.7 GB limit (user review) | `kst-inventory` disk-byte budget; per-package % of 4 GB (S8 partial) | No unique-sample RAM model, no compression estimate | Memory estimator for Sound Factory output (sum of sample bytes vs 4 GB user PCM, "8 GB linear when compressed", VERIFIED specs) | Unit | Compare with Pa5X Media > Info |
| **Dependency resolution** | "Where is this Sound used", auto-import linked Sounds/Multisamples, delete with dependents (F) | Set → set | Not a named checker | None | Total | Requires reading `.STG`/`.KSC` references — **BLOCKED** on undocumented formats. Feasible: dependency graph for AIArranger's own outputs (style → sound map → user sounds) | Unit | – |
| **Output validation** | HTML/TXT reports, empty-track check, repair corrupt Kbd Sets (F, C) | Set → report | No validator against Korg rules claimed | **Marker-SMF validator** implementing every documented Korg import rule + independent parser + report | Native formats | Extend validator as rules are confirmed on hardware | Existing | Each rule confirmed by a Pa5X import |
| **Backup & restore** | (Implied by set copy tools) | – | – | None | – | The Pa5X does its own Backup (Media). AIArranger tools never modify a KST in place; every write goes to a new folder | Unit: writer refuses to overwrite | – |

## Observed-capability gap matrix (2026-11-07, PA Manager v5.2 on the research copy)

Unlike the matrix above (vendor claims), every PA Manager cell here was **seen** in a read-only session (`research/PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md` §3c-3e). AIArranger cells are the current code.

| Capability | PA Manager (OBSERVED) | AIArranger today | Gap | Feasible independent step |
|---|---|---|---|---|
| Open a Pa5X KST | Loads a KST folder, per-type progress, does not modify it (manifest check) | `kst-inventory`: folder/type/bank/extension counts, PCM warning, never writes | Content view (names, slots) | Names/slots need `.STG/.KSC/.PCG` decoding: blocked (P1.10) |
| Bank / slot model | Styles, KBD SETs in banks × 10-per-page × 6 pages; Sounds 12×128, DK 6×128 | `by_bank` from folders only | Slot positions | Document slot capacity as constants once confirmed on hardware |
| Memory budget | Per-model profiles (8 MB PA80 … 4 GB PA5X): used / total / available bytes, sample count limit, preload buffer | `kst-inventory` P2.3: `.PCM`/`.KMP` **disk** bytes vs `--budget-bytes` (default 4 GiB, PA Manager's figure) | RAM model | Measure Pa5X sample-RAM use on hardware (paused) |
| Per-Sound sample size | "Check Sample Size" | None | Total | Needs Sound→sample links: blocked |
| Reverse dependency | "Search Where this Sound is used": Style Element × track, KBD SET, PAD | None | Total | Blocked on reference encoding; a hardware-free path is to diff PA Manager reports (owner-run, saved outside the repo) — not planned without approval |
| Sound → Multisample | "Search Where this MultiSample is used": Sound, position, OSC High/Low | None | Total | Blocked on `.PCG` encoding |
| Multisample zones | Zone editor: key range, root, level, pitch per zone | `sf2-check` reads SF2 zones (import side only) | Native zones | Blocked on `.KMP` encoding |
| Duplicates / unused | Sound duplicates by name and content; unused Sounds | Byte-identical file duplicates only | Name/content/usage level | Name-similarity grouping is feasible from file names alone, usage needs references |
| Drum Kit → samples | DK Report: key → RAM/ROM/compressed, sample number, name; sharing visible | None | Total | Same blocker; the report columns define the target data model |
| Reports | Text reports per tab (Style, KBD SET, Sound, DK) | `kst-inventory` text report; marker-SMF validator report | Content-level reports | — |
| Sound remapping | Global Sound ReMAP: up to 4 A→B pairs across Style / KBD SET / PAD; RE-MAP MODE toggle (§3f) | Export-side `--sound-map` | Native remap | Research only (P3) |
| Merge | "Copy from another KST": second pane, "Automatically Copy Related USER Sounds" (§3f) | `kst-merge-plan` (file level only) | Sound-aware copy closure | KST merge *plan* by name (manual p.731-733), no writes |
| SF2 pre-check | (not observed) | `sf2-check` (overlapping zones, manual p.1206) | AIArranger ahead | Hardware validation of which overlaps the Pa5X rejects |
| Edit operations | Cut/Swap/Copy/Paste/Delete/Rename/Icon/Reset Sound/Export WAV | None (by design: AIArranger never writes KST) | — | Not planned |

## Conclusion

- **Parity is not a near-term goal.** PA Manager's core (reading and writing native SET/KST internals) depends on undocumented binary formats. AIArranger will not reverse-engineer them without owner-provided files and an explicit decision.
- **Where AIArranger can lead with documented paths:** (a) Genos/Yamaha → Pa5X via the marker SMF (implemented, hardware pending); (b) live arranger control of the Pa5X over USB-MIDI using the documented Control-channel PCs; (c) VST → TX16Wx TXPROG → Pa5X User Sound (Korg-supported import); (d) report-only KST inventory and duplicate detection.

## Open questions requiring the hardware

1. Port names with the class-compliant driver (11-05 bench saw `Pa5X SOUND` / `Pa5X KEYBOARD`, matching the manual p.582).
2. Whether CC0/CC32+PC on the Control channel still selects Styles.
3. Start/Stop acceptance while Clock Source = Internal; Song Position ignored.
4. SysEx Inquiry reply bytes.
5. User Sound / Drum Kit slot counts on the current OS (manual p.1935 says 1,536/768, and PA Manager agrees; an earlier web-spec figure of 768/384 is superseded).
6. Whether a Kronos/Nautilus PCG loads at all.
7. Which Sound parameters survive a TXPROG import.
8. Whether our marker SMF (now incl. `i3/f3/f4/e3`) imports cleanly; the exact Break marker.
