# PA Manager and Pa5X: consolidated research report

- **Date:** 2026-11-08
- **Author:** REA (Claude research subagent), for AIArranger (Pa5X First)
- **Scope:** one consolidated view of the completed research on KORG PA Manager v5.2 (Windows) and the Pa5X resource model, measured against what AIArranger has built. Gaps were filled **only** from official Korg documentation already on disk. No new PA Manager session, no Windows access, no Pa5X access, no KST file read, no MIDI, no network.
- **Hardware status:** **nothing in this report is hardware-validated.** No AIArranger output has been imported on a Pa5X (S6 not executed).
- **Sources:** the repo research docs listed in §7, the Pa5X User Manual v1.4 (text extract, printed pages, cited "MAN p.N") and KORG's *Making Pa5X Sounds and Drum Kits with TX16Wx* (cited "GUIDE p.N", printed page numbers).

**Evidence labels (on every claim):** **OBS** directly observed in PA Manager v5.2 on a research copy (cite audit §); **DOC** official Korg documentation (MAN / GUIDE page); **PUB** PA Manager public/vendor claim (parity doc); **CODE** verified in AIArranger code or tests (header re-read for this report, or recorded with a commit in the roadmap/log); **INF** inference (basis given); **UNK** unknown.

Abbreviations: AUDIT = `PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md`; RM = `PA5X_KST_RESOURCE_MODEL.md`; PARITY = `KORG_PA_MANAGER_FEATURE_PARITY.md`; SPA = `SOUND_PARAMETER_AUDIT.md`; ROAD = `SOUND_CONVERSION_ROADMAP.md`; LOG = `PA5X_FIRST_EXECUTION_LOG.md`; FEAS = `KST_PARSER_FEASIBILITY.md`; RIGHTS = `KST_DATA_PROVENANCE_AND_RIGHTS.md`. D1 = the research KST copy (RIGHTS §1); its name is not repeated here.

## 1. Executive summary

**Established.** The Pa5X resource chain is Style (Element × track) → Sound → oscillator (High/Low multisample, velocity switch) → Multisample (key zones) → Sample, and Drum Kit key → velocity layers → drum sample (DOC MAN pp.1076-1077, 1089-1091, 1183-1188; OBS AUDIT §3e-3f). Resources are shared and reference-counted in PA Manager's view: one multisample used 9× by 3 Sounds, one sample on several drum keys (OBS AUDIT §3e-3f). Pa5X load rules are documented: Merge replaces same-name files, Replace All deletes the type, loading a whole KST with a PCM folder deletes **all** User Samples, an overflow aborts the load and removes only the new samples (DOC MAN pp.734-736). User Sample memory is **4 GB uncompressed / 8 GB compressed**, and compression **cannot be undone on the Pa5X** (DOC MAN pp.1174, 1176, 1181, 1936). The only documented file routes into a Pa5X Sound carry multisample data plus names, ranges, roots, loops, level/pan/transpose/tune; envelopes, filters, LFOs and effects never transfer (DOC GUIDE pp.24-27, 51, 84-85; MAN pp.1206-1210).

**Not established.** How any reference is encoded inside `.STG/.KSC/.PDC/.PCG/.KMP/.PCM` (UNK, gated: FEAS §2); what PA Manager's "Copy from another KST" and RE-MAP MODE actually write (not executed, AUDIT §3g-3i); how PA Manager computes "Used Size" (AUDIT §3f); whether the Pa5X accepts any AIArranger package (S6 not run).

**Engineering implications.**
1. AIArranger KST tools are **file-level only** (inventory, byte diff, file-level merge *plan*); resource dependency analysis and Sound-aware merge are **not available** and cannot be without decoding (CODE `kst_diff.h` header; FEAS §4).
2. A whole-KST load is destructive for User Samples; every AIArranger workflow must target **single-Sound / single-multisample import** (DOC MAN p.736, p.1160).
3. Conversion output must be **multisample-shaped** (key zones in the multisample, velocity at oscillator level) and must ship a manual post-import checklist for synthesis parameters (DOC GUIDE pp.51, 85; CODE S13 Sound Recipe).
4. Memory planning must count **unique** samples, treat compression as one-way, and leave headroom for the documented overflow abort (DOC MAN pp.736, 1181; OBS sharing).
5. Two Korg documents disagree on Drum Kit ranges (layers, transpose, tune); AIArranger should keep to the intersection until S6 (DOC MAN pp.1090, 1093, 1152 vs GUIDE pp.18, 25; see §5 Q8-Q9).

## 2. Findings by area

### 2.1 KST Manager capabilities (PA Manager)

| # | Finding | Label | Source |
|---|---|---|---|
| K1 | KST Manager opens a KST **folder** via a ".KST EXPLORER"; it summarizes size and STY/PRF/PAD/PCG/PCM counts without loading | OBS | AUDIT §3a, §3d |
| K2 | Explorer counts equal file counts per type folder (D1: STY 199, PRF 347, PAD 0, PCG 8, PCM 99 = folder file counts) | OBS | AUDIT §3f vs §3c |
| K3 | Loading reads type folders with per-type progress; D1 manifest identical before/after every session (719 entries, 0 differences) | OBS | AUDIT §3d-3i |
| K4 | Tabs STYLE / KBD SET / PAD / SOUND (+ DK switch); 10 tiles per page, pages P1-P6; User Sounds 12 banks × 128, User DK 6 banks × 128 | OBS | AUDIT §3e |
| K5 | Slot totals 1 536 User Sounds / 768 User Drum Kits match the spec page | OBS + DOC | AUDIT §3e; MAN p.1935 |
| K6 | Tile menu (Sound/DK): Cut, Swap, Copy, Paste, Delete, Reset Sound, Rename, Options ▸ {Check Sample Size, Search Where this Sound is used…, Export All Samples to .Wav} | OBS | AUDIT §3e |
| K7 | "Search Where this Sound is used" scans all Style banks and reports per Style the Element × track using the Sound, plus KBD SET and PAD use counts | OBS | AUDIT §3e |
| K8 | DK Report: per key, Type RAM / ROM / RAM [Comp], sample number, name; shared samples visible | OBS | AUDIT §3e |
| K9 | Check for Duplicates [PCG] (by Name and by Content), Check for Unused Sounds (scans Styles), Global Sound ReMAP (4 A→B pairs, scope STYLE / KBD SET-PERF / PAD) | OBS (dialogs only; nothing applied) | AUDIT §3f |
| K10 | Unused Sound Report "Sound Pos" = `121, 63+bank, pos-1` (User) and `120, 64, pos-1` (User DK); read as MSB/LSB/PC | OBS (figures) + INF (reading) | AUDIT §3f |
| K11 | Copy from another KST: second pane, Copy Options with "Automatically Copy Related USER Sounds" ON, per-track/part filters, duplicate reMap ON | OBS (options only; `<<` never pressed) | AUDIT §3f, §3i |
| K12 | Vendor claims: Edit/Mix/Combine SETs, Sound reMAP, PCG remap (repeatedly fixed in changelog), streaming large KSTs | PUB | PARITY matrix |
| K13 | PA Manager is a VB6 app; UIA exposes button labels and visible tile names, not memory figures or report text | OBS | AUDIT §3c-3d |

### 2.2 Sample Manager (PA Manager)

| # | Finding | Label | Source |
|---|---|---|---|
| M1 | Tabs Sample / Drum Sample / MultiSample; D1 counts 14 274 / 4 291 / 483; "No Ref (0)" | OBS | AUDIT §3f |
| M2 | Sample columns: ID, Name, File Size, Rate, Duration, Orig Note, Loop Start/End (frames), Type (Normal / Compressed / Locked), Stereo, PreLoad | OBS | AUDIT §3f |
| M3 | Rates 24 000 / 44 100 / 48 000 Hz, all 16-bit, in one KST | OBS | AUDIT §3h |
| M4 | Pa5X itself: per sample flags Comp, Prot (copy-protected, needs protection key), Prel ("Play from Preload Buffer", whole sample loaded to RAM, cannot be compressed) | DOC | MAN p.1159, p.1177 |
| M5 | PA Manager "PreLoad" checkbox corresponds to Pa5X Prel; "Locked" corresponds to Prot | INF (same column semantics, MAN p.1159) | — |
| M6 | MultiSample columns: ID, Name, Unlinked Samples, Zones, File Size; menu incl. Search Where used, Export, Replace, Compress/De-Compress MS | OBS | AUDIT §3f |
| M7 | Compression: Compress SET / De-Compress SET; Options = all samples or only > 1/2/3/4/5/10 s; PC worker threads 1-4 | OBS (menus only) | AUDIT §3f, §3h |
| M8 | Import label "aif / wav / ksf / kmp / rex" | OBS (label only) | AUDIT §3f |
| M9 | Memory profiles per model, PA5X = 4 GB (4 294 967 296 B), sample-count limit 53 130, preLoadBuffer 420 MB | OBS (PA Manager's figures) | AUDIT §3d, §3f |
| M10 | Pa5X Memory Info page shows Samples, Samples Size, Preload Buffer Size, Drum Samples, Multisamples | DOC | MAN p.1174 |
| M11 | Pa5X documents a "maximum number of samples allowed in memory" but gives no number in the pages read | DOC (existence) / UNK (value) | MAN p.736 |

### 2.3 Sound and Drum Kit structures

| # | Finding | Label | Source |
|---|---|---|---|
| S1 | Sound = 1-24 oscillators (Oscillators Count); polyphony max 160 with 1 oscillator; Low Priority can drop high-numbered oscillators | DOC | MAN pp.1070-1071, p.1935 |
| S2 | Drum Kits are special Sounds, one instrument per key, not transposed | DOC | RM §3 (MAN p.934) |
| S3 | Sounds are a **reference grid** (fixed positions, empty slots exist), found "where expected by other types of data"; Styles/KBD Sets/Pads are name-organized | DOC | MAN pp.722-723 |
| S4 | Style → Sound per track (Element override), Style → 4 KBD Sets, 4 Pads; KBD Set → Sounds | DOC | RM §3 (MAN pp.84, 194, 932) |
| S5 | Pad → Sound: PA Manager searches it (0 hits seen); manual mechanism not found | OBS / UNK | AUDIT §3e; RM §3 |
| S6 | Drum Kit key: Layers count, Assign, per-layer drum sample (FTY/USR + number), Offset, Dry level, Attack/Decay offsets, Cutoff/Resonance, Transpose −64…+63 st, Tune −99…+99 ct, Amb level/time, 3-band EQ | DOC | MAN pp.1089-1095 |
| S7 | Drum key Voice/Mixer: Single Trigger, Exclusive Group None/1…127, Choke Fade Out Time, Subgroup Choke Off, Note On/Off receive, Level, Pan, MFX sends | DOC | MAN pp.1098-1099 |
| S8 | Round-robin: Play Mode 1…4 / Cycle / Random, only on samples that include variations | DOC | MAN p.1092 |
| S9 | Drum layers per key: layer selector has 1-8 and 9-16 groups (→ up to 16); GUIDE also says up to 16; MAN p.1152 says "up to six dynamic layers per note and up to four round-robins" | DOC (conflicting) | MAN p.1090, p.1152; GUIDE p.18 |
| S10 | USER SOUND REPORT lists name, source model tag (PA4X/PA3X/PA900), page; no sample refs for ordinary Sounds | OBS | AUDIT §3e |
| S11 | A Sound or Drum Kit whose User samples are not loaded can be selected and named but is silent | DOC | MAN p.1077, p.1091 |

### 2.4 Oscillators and multisamples

| # | Finding | Label | Source |
|---|---|---|---|
| O1 | Each oscillator plays one or two stereo multisamples (High / Low), split by Vel SW L→H; value 1 = High only | DOC | MAN pp.1076-1078 |
| O2 | Multisample bank FTY (factory) or USR (user, "loaded when loading some User Sounds"), chosen by number | DOC | MAN p.1077 |
| O3 | Per multisample: Offset (Off/1st-6th/No Attack/AMS/Pseudo Random, not on all), Reverse (some only), Stereo/Mono, Level; per oscillator Octave −2…+1, Transpose ±12, Tune ±1200 ct | DOC | MAN pp.1077-1078 |
| O4 | Trigger modes incl. Note Off, Natural Release, After Touch, Joystick, Legato variants, Sound Controllers, Cycle 1/2, Random; Delay 0-5000 ms | DOC | MAN pp.1079-1081 |
| O5 | GUIDE convention: one multisample per oscillator, velocity layers as separate oscillators with adjacent Velocity Zones; the High layer is "typically the one used" | DOC | GUIDE pp.14-16 |
| O6 | Multisample: each zone (Index) has From/To key, exactly one sample (or none = silent), Original Note, Level (negative offset only), Pitch (cents); zones always tile the keyboard (delete extends the neighbour; lowest zone starts at C-1) | DOC | MAN pp.1183-1188 |
| O7 | PA Manager zone editor shows the same fields and **no velocity per zone** | OBS | AUDIT §3f |
| O8 | Root note exists twice: sample Original Note (default) and zone Original Note (can differ: C4 vs G3 seen) | DOC + OBS | MAN p.1168, p.1187; AUDIT §3h |
| O9 | One multisample shared by 3 Sounds, 9 oscillator uses (OSC High/Low reported) | OBS | AUDIT §3f |
| O10 | Whether one sample can belong to two multisamples | UNK | RM §3a |
| O11 | MAN deletion text ("samples left free … not used by other multisamples") implies a sample *can* be shared between multisamples | INF (MAN p.1190 wording) | — |

### 2.5 Velocity and keyboard zones

| Level | Velocity | Key | Label / source |
|---|---|---|---|
| Oscillator | Velocity Zone top/bottom 0-127, Scaled Velocity; High/Low switch | Keyboard Range C-1…G9 | DOC MAN pp.1078, 1082-1083 |
| Multisample zone | none | From/To, Original Note | DOC MAN p.1186; OBS AUDIT §3f |
| Drum key | up to 16 layers (see S9), velocity switches, first/last fixed 001/127 | one key | DOC MAN p.1090 |
| TXPROG mapping | Region Lo/Hi V → OSC Velocity Zone / Scaled Velocity | Region Lo/Hi K → OSC Keyboard Range | DOC GUIDE p.27 |

- **V1** An SF2 instrument with overlapping or per-zone velocity ranges cannot map onto one Pa5X multisample; the Pa5X warns that such instruments may not import (DOC MAN p.1206; INF RM §3a). `sf2-check` reports overlaps (CODE, ROAD §1 P1).
- **V2** TX16Wx regions must cover the keyboard with no gaps and adjacent velocity zones must leave no gaps (DOC GUIDE p.16).
- **V3** "Exponential 2" TX velocity curve matches Pa5X Velocity Intensity 80 (DOC GUIDE p.42, via TXPROG_FORMAT_NOTES).

### 2.6 Loops and sample dependencies

| # | Finding | Label | Source |
|---|---|---|---|
| L1 | Sample limits: 8/16-bit, 11 025-48 000 Hz, mono/stereo, ≤ 1 048 576 frames (longer is truncated with a warning); recording max 21.8 s per sample | DOC | MAN p.1155, p.1164 |
| L2 | Loop: Loop On, Loop Start, End (= loop end), Use Zero, Loop Lock, Crossfade Loop (length bounded by start/end distances) | DOC | MAN pp.1167-1172 |
| L3 | Loops shorter than 16 samples, and very low-level samples, cannot be compressed | DOC | MAN p.1176 |
| L4 | PA Manager loop points are in frames; TXPROG loop end exclusive (Korg example) | OBS / INF | AUDIT §3f; TXPROG_FORMAT_NOTES |
| L5 | TX loop points are preserved on import; other wave-editor data dropped | DOC | GUIDE p.84 |
| L6 | Drum sample Rev on a looped sample plays one-shot reverse | DOC | MAN p.1091 |
| L7 | Reverse and full-range high transposition need the sample entirely in RAM (preload); transposition headroom: uncompressed-from-drive < compressed < RAM | DOC | MAN pp.1176-1177 |
| L8 | Dependency deletes: delete unassigned samples; delete multisamples not assigned to any Sound (+ samples/drum samples) | DOC | MAN p.1179, p.1191 |
| L9 | AIArranger: one forward loop, correlation seam + crossfade, match reported, < 0.9 flagged | CODE | SPA §3; LOG row 80 |

### 2.7 Sound copying, remapping and merging

| # | Finding | Label | Source |
|---|---|---|---|
| C1 | Pa5X load granularity: whole KST, one type, one bank, single file | DOC | MAN pp.733-734 |
| C2 | Merge (default) replaces same-name files only; Replace All replaces all User files of that type; replaced data is deleted | DOC | MAN p.734-735 |
| C3 | To merge samples from several KSTs: load one KST, then single Sounds from others, then import, then save memory to a KST | DOC | MAN p.736 |
| C4 | Grid data must stay where referrers expect it; a file-level merge that moves a Sound breaks references | DOC (rule) + INF (consequence) | MAN pp.722-723; ROAD §1 P2 |
| C5 | PA Manager copy closure (labels): Style copy pulls User Sounds of tracks BASS, DK, PERC, ACC1-5 (ON), optionally KBD SET STS1-4 and PAD1-4 (Off); skip muted ON; reMAP missing User Sound to Factory Off; "Convert MG > STD > MG" ON (meaning UNK); SOUND tab: reMap to existing duplicate ON | OBS (options), INF (behaviour) | AUDIT §3i |
| C6 | What "Automatically Copy Related USER Sounds" actually copies, and RE-MAP MODE tile behaviour | UNK (needs an executed copy/remap on a copy) | AUDIT §3g |
| C7 | PA Manager Global Sound ReMAP rewrites Sound references across Style / KBD SET / PAD | OBS (dialog) / UNK (result) | AUDIT §3f |
| C8 | Vendor changelog: remap repeatedly fixed (5.2.1428/2226/2616) | PUB | PARITY "Sound remapping" |
| C9 | AIArranger `kst-merge-plan SRC DST`: read-only file-level prediction (replace / add / unchanged / duplicate / case-only CASE? / kept), PCM-folder warning, strict scan errors → exit 2 | CODE | ROAD K4; LOG rows 89-90; `kst_diff.h` header |
| C10 | AIArranger `kst-diff A B`: byte-equal folder comparison (identical / changed / added / removed / moved), symlinks skipped | CODE | ROAD K3; `kst_diff.h` header |
| C11 | AIArranger never writes a KST; `tools/kst_copy.py` makes a verified folder copy with SHA-256 manifest | CODE | ROAD K1 |

### 2.8 Resource allocation and memory management

| # | Finding | Label | Source |
|---|---|---|---|
| R1 | User Sample memory: **4 GB of non-compressed** samples; with compression up to **8 GB** ("equivalent to 8 GB linear when compressed") | DOC | MAN p.1174, p.1176, p.1936 |
| R2 | Compression reduces a sample "up to a half"; offered at save, in batch ("Compress all samples", long, do not power off), and at KMP/SF2/TXPROG/TXPERF/WAV-folder import | DOC | MAN pp.1181-1182, 1209-1211; GUIDE p.88 |
| R3 | **Compressed samples can no longer be decompressed** on the Pa5X; save a copy first | DOC | MAN p.1181 |
| R4 | Compression excludes Preload samples, loops < 16 samples, very low-level samples | DOC | MAN pp.1176-1177 |
| R5 | Preload Buffer reserves RAM for samples streamed from the internal drive's virtual memory | DOC | MAN p.1174 |
| R6 | **Load interruption:** reaching max sample memory or max sample count aborts the load; samples/multisamples just loaded are removed; previous content untouched | DOC | MAN p.736 |
| R7 | Whole-KST load with a PCM folder deletes **all** User Samples; single-Sound loads keep them | DOC | MAN p.736 |
| R8 | Not-enough-memory: Delete to clean, or compress; "importing single multisamples is the safest way to avoid filling the memory" | DOC | MAN p.1160 |
| R9 | Imported data goes to User Sample memory and survives power-off | DOC | MAN p.1207 |
| R10 | PA Manager PA5X profile: Used 1 403 084 800 B of 4 294 967 296 B on D1; sample count 14 274 / 53 130; preload 224 / 420 MB | OBS | AUDIT §3d |
| R11 | PA Manager Used Size ≠ D1 PCM folder bytes (~1285 MB); its formula is unknown | OBS / UNK | AUDIT §3f |
| R12 | Compressed sample ≈ 1 byte per frame vs 2 for 16-bit (a few rows) | OBS + INF | AUDIT §3f |
| R13 | PA Manager offers De-Compress on the PC; whether that restores original data, given R3, is unknown | OBS / UNK | AUDIT §3a, §3f |
| R14 | Memory is per unique resource (a sample used 9× is stored once) | INF (from O9 sharing) | RM §3a |
| R15 | AIArranger: `kst-inventory` sums `.PCM`/`.KMP` **disk** bytes vs `--budget-bytes` (default 4 GiB); package REPORT gives % of 4 GB per instrument; multi-instrument budget not built | CODE | ROAD S8 (partial); RM §6.2; `kst_inventory.h` |

### 2.9 File conversion capabilities

| Route | PA Manager | Pa5X native | AIArranger |
|---|---|---|---|
| Triton PCG → Sound | PUB (Trinity…Nautilus PCG import, PARITY) | DOC import single Program or bank; similar or `<empty>` multisample substituted; IFX, EQ, Arp, Combi, Global, Drum Kits not imported (MAN p.1206, 1208) | not planned |
| Kronos/Nautilus PCG | PUB | not listed (UNK whether it loads) (MAN p.1206) | blocked (no spec; PARITY) |
| KMP/KSF | OBS label "ksf / kmp" import (AUDIT §3f) | DOC import KMP (mono; stereo as `_L`/`_R`), KSF samples compressed or not; export KMP+KSF (MAN pp.1155, 1206, 1209, 1212-1214) | reader blocked on authorized material (KRONOS_KMP_KSF_IMPORT, ROAD K2) |
| SF2 | not observed | DOC import multisamples (single or bank, mono/stereo), overlapping-zone instruments may fail; no Drum Kits (MAN pp.1206, 1209) | CODE SF2 2.04 writer (S3) + `sf2-check` |
| TXPROG → Sound | not observed | DOC import with linked samples, OS ≥ 1.3.0; export multisample to TXPROG + WAV (MAN pp.1206, 1210, 1212, 1215-1216; GUIDE pp.87-90) | CODE writer (S5), structure mirrors Korg example; not imported (S6) |
| TXPERF → Drum Kit | not observed | DOC import (MAN p.1206, p.1210) | CODE writer (S12), subset of Korg example; not imported |
| WAV/AIFF folder | OBS label "aif / wav" | DOC import folder, drum families by prefix B_ S_ H_ T_ C_ L_ E_ X_ (MAN p.1211); export WAV with `_OrgKey_LowKey_HighKey` names (MAN p.1212) | CODE route C reader `aiarranger-sound wav` (S10) + WAV writer |
| REX | OBS label only | not documented in pages read | not planned |
| Pa4X SET → KST | PUB (SET→KST since 5.2.1030) | DOC one-way Data Conversion with ConvertReport.txt (MAN pp.737-738) | not planned |
| WAV export of Sound samples | OBS menu "Export All Samples to .Wav" | DOC export sample WAV/AIFF, all samples as WAV (MAN p.1178, p.1215) | n/a |
| Native KST write | PUB / OBS (Save, edit ops) | native | **not planned** (never writes KST) |

## 3. Feature gap matrix

AIArranger KST tools are **file-level only**: byte comparison (`kst-diff`), file inventory (`kst-inventory`), file-level merge planning (`kst-merge-plan`), verified copy (`kst_copy.py`). **Resource dependency analysis and Sound-aware merge are NOT available** (CODE `kst_diff.h` header: "What this is NOT: a resource dependency analysis or a Sound-aware merge"); the graph API in RM §6.3 is design only, with an empty edge list until P1.10.

| Capability | PA Manager | Pa5X native | AIArranger status | Gap / notes |
|---|---|---|---|---|
| Open/inventory a KST | OBS (AUDIT §3d) | DOC (MAN p.731) | built SOFTWARE: `kst-inventory` (`src/media/kst_inventory.h`) | AIArranger: types/banks/sizes only; no names or slots (decoding blocked) |
| Bank / slot view, names | OBS (AUDIT §3e) | DOC | blocked (P1.10) | needs `.PCG/.STG/.KSC` decoding |
| Folder diff / backup verify | not observed | n/a | built SOFTWARE: `kst-diff` (`kst_diff.h`) | not run on owner data |
| Merge planning | OBS dialog (AUDIT §3i); PUB merge | DOC Merge/Replace (MAN p.734) | built SOFTWARE, **file level**: `kst-merge-plan` (`kst_diff.h`, ROAD K4) | no reference awareness; case rule UNK |
| Sound-aware merge / copy closure | OBS options (AUDIT §3i) | partial (load single Sounds, MAN p.736) | blocked (edges need decoding) | — |
| Reverse dependency ("where used") | OBS (AUDIT §3e-3f) | DOC delete-unassigned only (MAN pp.1179, 1191) | blocked | — |
| Unused / unreferenced | OBS (AUDIT §3f) | DOC delete unassigned (MAN p.1191) | blocked | — |
| Duplicates | OBS name + content (AUDIT §3f) | none documented | built SOFTWARE: byte-identical files only (`kst-inventory`) | no name/usage level |
| Global Sound remap | OBS dialog; PUB | none documented | export-side `--sound-map` only (PARITY) | native remap not planned |
| Multisample zone editing | OBS (AUDIT §3f) | DOC (MAN pp.1183-1188) | built SOFTWARE on the source side: instrument model (`src/media/sound/instrument.h`) | native KMP edit not planned |
| Drum Kit report / editing | OBS (AUDIT §3e) | DOC (MAN pp.1089-1100) | built SOFTWARE: `drumkit.h` + `txperf_writer.h` (S12) | not imported |
| Sample compression | OBS (AUDIT §3f, §3h) | DOC one-way (MAN p.1181) | not planned (left to Pa5X import dialog) | — |
| Memory budget | OBS 4 GB profile (AUDIT §3d) | DOC 4/8 GB (MAN p.1174) | built SOFTWARE, partial: disk-bytes budget (`kst-inventory`), per-package % (S8) | no unique-sample RAM model, no compressed-size estimate |
| SF2 pre-check | not observed | DOC caution (MAN p.1206) | built SOFTWARE: `sf2-check` | AIArranger ahead |
| Sound creation from audio | PUB AutoMS etc. | DOC sampler (MAN p.1152ff) | built SOFTWARE: S1-S5, S7, S9, S10, S12, S13 | not hardware-validated (S6) |
| Kronos KMP/KSF import | PUB | DOC KMP import (Triton-generation) | blocked (material; ROAD K2) | route C (WAV) built |
| Kronos/Nautilus PCG | PUB | not supported | not planned | — |
| Native KST write / edit ops | OBS (AUDIT §3e) | native | not planned | by design |

## 4. Technical implications for Kronos/VST → Pa5X conversion

1. **Target the documented import formats only.** TXPROG (Sound) and TXPERF (Drum Kit) are the only routes that create a complete Sound/Drum Kit in one import (DOC MAN p.1206; GUIDE p.87). SF2/KMP/WAV give multisamples or samples, and the Sound is then built by hand (DOC MAN p.1192, p.1207).
2. **Synthesis does not transfer.** Envelopes, filters, LFOs, effects, mod routing arrive at Pa5X defaults (DOC GUIDE pp.51, 85; MAN p.1208 for PCG). Bake what can be baked (attack, filter at render velocity), render dry, and report the rest as a checklist (CODE SPA §3, S13).
3. **Velocity structure.** Use one multisample per oscillator with adjacent Velocity Zones (GUIDE p.14) — what the AIArranger model does (CODE `instrument.h`: 1-24 layers tiling 0-127). Each extra layer costs an oscillator per note; 160-voice polyphony is shared (DOC MAN p.1070). No per-zone velocity in multisamples (DOC/OBS).
4. **Keyboard coverage.** Zones must tile the keyboard with no gaps (DOC MAN p.1186; GUIDE p.16); the writer enforces `lo <= root <= hi` and full tiling (CODE SPA §3).
5. **Sample format.** 16-bit, 11 025-48 000 Hz, ≤ 1 048 576 frames; AIArranger refuses rather than letting the Pa5X truncate (DOC MAN p.1155; CODE `instrument.h`).
6. **Loops.** Loops ≥ 16 samples if compression may be chosen (DOC MAN p.1176). Evolving sounds loop poorly (CODE loop-match flag). Release must be set by hand (CODE measured release).
7. **Compression choice belongs to the user at import**, and is irreversible on the Pa5X (DOC MAN pp.1181, 1210; GUIDE p.88). AIArranger should present the trade-off: half the memory vs no Preload (Reverse, high transposition limits, MAN pp.1176-1177).
8. **Memory.** Budget against 4 GB uncompressed; 8 GB only if everything is compressed (DOC MAN p.1174). An import that overflows is aborted cleanly (DOC MAN p.736) — not data-destroying, but the package must state its size up front (CODE S8 partial).
9. **Never whole-KST.** Deliver single imports; a whole-KST load deletes all User Samples (DOC MAN p.736). The S6 procedure already follows this (ROAD §5.1).
10. **Drum Kits.** Keep to the intersection of the two Korg documents: ≤ 16 layers (MAN p.1090, GUIDE p.18; but MAN p.1152 says 6), Exclusive Group 1-8 (TX limit; Pa5X accepts 1-127, MAN p.1099), transpose within ±12 and tune within ±50 ct (GUIDE p.25; MAN p.1093 states −64…+63 st / ±99 ct). The current writer does this (CODE `drumkit.h`).
11. **Kronos.** Only multisample data can move: native KMP import of owner-made Kronos KMP/KSF (UNK whether accepted), or WAV export → route C (CODE). Kronos factory samples are Korg copyright (SPA §4; KRONOS_KMP_KSF_IMPORT).
12. **Round trip oracle.** The Pa5X exports a multisample as TXPROG + WAV or `_OrgKey_LowKey_HighKey` WAVs (DOC MAN pp.1212-1215): the lawful ground truth for S5 and the KMP reader.

## 5. Open questions

Tags: **[MAN]** manual re-read; **[PAM]** future supervised read-only PA Manager session (on a copy); **[OWN]** owner action/decision; **[HW]** Pa5X hardware import (blocked until owner approval).

1. Does the Pa5X accept AIArranger's TXPROG package (root, loop, layers, stereo, `/` paths, `created-by`)? **[HW]** (S6)
2. Does it accept the TXPERF Drum Kit, and how are drum samples named and choke groups numbered? **[HW]**
3. Does a Kronos-generation KMP/KSF import via Import KMP? **[OWN]** material + **[HW]**
4. Does a Kronos/Nautilus PCG load at all? **[HW]** (PARITY Q6)
5. What does "Automatically Copy Related USER Sounds" copy, and what does a tile click do in RE-MAP MODE? **[OWN]** authorization to execute on a copy + **[PAM]**
6. How does PA Manager compute Used Size, and is "Sample n / 53 130" the Pa5X sample-count limit? **[PAM]** + **[HW]** (Pa5X Memory Info photo) — the manual gives no number (**[MAN]** re-read of the Specifications and Sample Edit chapters for a count limit)
7. Does PA Manager's De-Compress restore original sample data, given that the Pa5X cannot decompress? **[PAM]** (read its options only) + **[OWN]**
8. Maximum Drum Kit velocity layers per key: 16 (MAN p.1090, GUIDE p.18) or 6 (MAN p.1152)? **[MAN]** (other chapters, e.g. Drum Kit quick edit) then **[HW]**
9. Drum Kit Transpose/Tune ranges: ±12 st / ±1200 ct (GUIDE p.25) or −64…+63 st / ±99 ct (MAN p.1093)? **[HW]** (read the Pa5X Drum Kit page)
10. Can one sample belong to two multisamples? **[PAM]** (MultiSample Info on several multisamples) — MAN p.1190 wording suggests yes
11. Pad → Sound reference mechanism. **[MAN]** (Pad Edit chapter) + **[PAM]** (positive "where used" hit)
12. What do PA Manager's orange tile corner mark and "MG > STD > MG" mean? **[PAM]** (help/tooltips) or vendor docs
13. Is PA Manager "Locked" exactly the Pa5X "Prot" flag, and "PreLoad" exactly "Prel"? **[PAM]** + **[MAN]** p.1159 already defines the Pa5X side
14. Is Pa5X name comparison during Merge case-sensitive (CASE? in `kst-merge-plan`)? **[HW]** or **[MAN]**
15. Velocity response within a layer (how strongly the Pa5X velocity→level curve applies on imported layers). **[HW]**
16. PA Manager export menu and formats (beyond "Export All Samples to .Wav", Export MultiSample). **[PAM]**
17. D1 provenance and rights; legal review G2/G5 before any byte-level work. **[OWN]** (FEAS §7, RIGHTS §1)
18. Will the owner save PA Manager reports privately as a parser oracle? **[OWN]** (RIGHTS §4)

## 6. Engineering recommendations (prioritised)

| # | Recommendation | Traces to | Effort / gate |
|---|---|---|---|
| E1 | Run S6 (single TXPROG import, then TXPERF) under the existing procedure; record photos of Memory Info before/after | Q1-Q2; §4.1 | owner approval |
| E2 | Fix the stale memory statement in RM §7 and the wrong page cites in ROAD §2 (4 GB/8 GB is DOC MAN p.1174/1176/1936) | R1; §8 contradictions | docs only |
| E3 | Add to the package REPORT: total unique sample bytes, % of 4 GB and of 8 GB, "compression is irreversible on the Pa5X", and samples that cannot be compressed (loop < 16 frames) | R1-R4, L3 | SOFTWARE, small |
| E4 | Extend S8 to a multi-package budget that warns before 4 GB (uncompressed) and states the overflow-abort rule | R6, R15 | SOFTWARE |
| E5 | Add a validator warning for loops shorter than 16 frames (blocks compression) | L3 | SOFTWARE, trivial |
| E6 | Keep drum ranges at the conservative intersection (16 layers, choke 1-8, ±12 st, ±50 ct) and record the MAN/GUIDE conflict in TXPERF_DRUMKIT_NOTES; resolve on hardware | S6-S9, Q8-Q9 | docs + HW |
| E7 | Add a polyphony note to the REPORT: "N oscillators per note; 160-voice pool" | S1, SPA G4 | SOFTWARE, trivial |
| E8 | State Preload trade-offs in the post-import checklist (Reverse and high transposition need Preload; Preload excludes compression) | L7, R4 | SOFTWARE, small |
| E9 | Keep `kst-merge-plan` labelled file-level; add its output line "references not checked: Sounds are grid data (MAN p.722-723)" | C4, C9 | SOFTWARE, trivial |
| E10 | Run `kst-inventory` / `kst-diff` on an authorized copy and compare with PA Manager explorer counts (K2) | RM §6.1 | owner data |
| E11 | Next PA Manager session (read-only, copy): Q6, Q10, Q12, Q13, Q16 — all answerable without Save/Copy/Remap | §5 | owner supervision |
| E12 | Kronos: proceed route C → A → B as designed; request one owner-made KMP/KSF set | Q3; §4.11 | owner material |
| E13 | Do not start byte-level KST work until G1/G2/G5 are answered in writing | Q17; FEAS §2 | owner + legal |

## 7. Evidence index

Repo docs (`/Users/mac/orca/workspaces/AIArranger/pa5x-first/docs/`):
- `KORG_PA_MANAGER_FEATURE_PARITY.md` (2026-11-06): vendor-claim matrix (PUB), observed-capability matrix, conclusions, open hardware questions.
- `research/PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md`: §3a home/explorer/Sample Manager; §3c folder structure + UIA; §3d KST view + Memory INFO; §3e tabs, reports, "where used"; §3f Sample Manager, duplicates, unused, ReMAP, Copy UI; §3g stop; §3h compression options, PreLoad, root override; §3i Copy Options.
- `research/PA5X_KST_RESOURCE_MODEL.md`: §1-§4 container, types, dependency graph, load/merge; §3a resource chain and sharing; §6 independent design; §7 missing evidence.
- `research/KST_PARSER_FEASIBILITY.md` (§2 gates, §3 oracle, §4 feasibility); `research/KST_DATA_PROVENANCE_AND_RIGHTS.md` (§1-§4).
- `research/SOUND_PARAMETER_AUDIT.md`; `research/TXPROG_FORMAT_NOTES.md`; `research/TXPERF_DRUMKIT_NOTES.md`; `research/KRONOS_KMP_KSF_IMPORT.md`.
- `SOUND_CONVERSION_ROADMAP.md` (S1-S13, K1-K4, status 2026-11-07); `PA5X_FIRST_EXECUTION_LOG.md` rows 50-90.
- Code headers re-read: `src/media/kst_inventory.h`, `src/media/kst_diff.h`, `src/media/sound/instrument.h`, `src/media/sound/drumkit.h`.

Pa5X User Manual v1.4 (printed pages): 721-723 (file types, reference grid); 731-736 (load, Merge/Replace, User Samples, interrupted load, merging samples); 737-738 via RM (Pa4X conversion); 1069-1071 (oscillator count, polyphony); 1076-1083 (oscillator multisamples, triggers, velocity/range); 1089-1100 (Drum Kit keys, layers, round-robin, EQ, exclusive groups); 1152 (sampling overview); 1155-1160 (sample formats, limits, flags, freeing memory); 1164 (21.8 s); 1167-1172 (loops, crossfade); 1174-1182 (memory info, save, compression, preload, export, delete); 1183-1191 (multisamples, zones, delete); 1192-1194 (building Sounds/Drum Kits); 1206-1216 (import/export, TX16Wx); 1935-1936 (specifications).

TX16Wx guide (printed pages): 14-18 (Sound/Drum Kit structure, up to 24 multisamples, up to 16 drum layers); 24-30 (parameter mapping, drum ranges); 50-51 (choke 1-8, Sounds not imported); 84-85 (waves, Sounds not imported); 87-90 (import/export).

## 8. Contradictions between sources

| # | Contradiction | Locations | Resolution |
|---|---|---|---|
| X1 | RM says the manual's sample-memory size was "not found"; the manual states 4 GB / 8 GB compressed | RM §7 last bullet (and §6.2 "PA Manager's 4 GiB") vs MAN p.1174, p.1176, p.1936 | **fixed** in `9f8cd19` |
| X2 | ROAD cites the 4 GB/8 GB fact at MAN "p.1156, p.1207"; those pages do not contain it | ROAD §2 row "User Sample memory" vs MAN p.1174/1176/1936 | **fixed** in `9f8cd19` |
| X3 | Drum layers per key: 16 vs 6 | MAN p.1090 and GUIDE p.18 vs MAN p.1152 | **resolved 2026-11-09 (DOC):** 16 — the Drum Kit editor page (1-8 / 9-16 buttons) and the guide are specific; p.1152 is an overview outlier. HW confirmation still Q8 (TXPERF_DRUMKIT_NOTES) |
| X4 | Drum Kit Transpose/Tune range on the Pa5X: ±12 st / ±1200 ct vs −64…+63 st / ±99 ct | GUIDE p.25 (followed by TXPERF_DRUMKIT_NOTES and `drumkit.h`) vs MAN p.1093 | **resolved 2026-11-09 (DOC + INF):** MAN p.1093 is the Drum Kit range; the guide's figures equal the oscillator ranges (MAN p.1078). Writer ±12 st / ±50 ct is inside both. Import mapping still Q9 |
| X5 | ROAD S1 says the model allows "up to 2 velocity layers" and refuses 3; S9 and code allow 1-24 | ROAD §4 S1 row and S1 status vs ROAD S9, `instrument.h` (`kMaxLayers = 24`) | **fixed** in `9f8cd19` (ROAD S1 row annotated) |
| X6 | AUDIT header says "No file has been loaded … not a completed audit", and the §3 feature inventory lists Sound/Multisample dependencies, Keyboard Sets, Drum Kits as UNKNOWN; §3d-3i observed them | AUDIT status line and §3 inventory vs AUDIT §3d-3i | **fixed** 2026-11-09: status line and §3 inventory rows updated, session records unchanged |
| X7 | RM §5 lists KST merge/remap and "used by" views as UNKNOWN; RM §3/§3a and §7 record them as observed | RM §5 vs RM §3a, §7 | **fixed** 2026-11-09 (RM §5 split into observed later / still unknown) |
| X8 | PARITY vendor matrix says AIArranger has "None" for multisample processing, drum kits, sample memory; later work built them | PARITY matrix rows vs ROAD S1-S12, P2.3 (and PARITY's own observed matrix row "Memory budget") | **fixed** 2026-11-09 (columns 5-6 updated, dated) |
| X9 | PARITY open question 5 cites "web specs 768/384" User Sounds/DKs; manual and PA Manager agree on 1 536/768 | PARITY §Open questions 5 vs MAN p.1935, AUDIT §3e | **fixed** in `9f8cd19` |
| X10 | AUDIT "PreLoad semantics UNKNOWN"; the manual defines Play from Preload Buffer | AUDIT §3h vs MAN p.1159, pp.1176-1177 | **fixed** 2026-11-09: AUDIT §3h annotated (Pa5X side DOC; PA Manager mapping INF) |
| X11 | SPA cites multisample key zones at "MAN p.1190"; zones are documented at pp.1183-1188 (p.1190 is deletion) | SPA §1 "Key zones" row | **fixed** in `9f8cd19` |
| X12 | PA Manager offers De-Compress; the Pa5X says compressed samples cannot be decompressed | AUDIT §3a/§3f vs MAN p.1181 | **resolved:** not a contradiction (PC tool vs instrument); AUDIT §3f annotated; data fidelity still Q7 |
| X13 | MAN p.1210 heading "Importing TXPERF files" repeats "When importing a TXPROG file" | MAN p.1210 | **resolved:** manual typo; recorded in TXPERF_DRUMKIT_NOTES |
