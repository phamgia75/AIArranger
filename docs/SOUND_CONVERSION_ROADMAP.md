# Product priorities audit and Sound Conversion roadmap

Status 2026-11-07. Owner's priority order (supersedes the Pa5X-First roadmap's ordering; that work is kept):

| | Priority | Today |
|---|---|---|
| P0 | Reverse-engineering research and the Pa5X Sound engine model | Strong documentary model, PA Manager observations; no byte-level decoding (gated) |
| P1 | Sound conversion Kronos / VST → Pa5X | S1-S5, S7 built (SOFTWARE + INDEPENDENT TOOL); nothing imported on a Pa5X yet |
| P2 | KST management | Read-only inventory and budget; dependency graph designed, edges blocked |
| P3 | macOS / iOS / iPadOS apps | macOS JUCE performance app; iPadOS build shell; no Sound/KST UI |
| P4 | AI Style creator from MP3/WAV | Earlier prototype `~/dev/ai-style-maker` (Python, separate repo); not audited here |

Labels: **DOC** Pa5X User Manual v1.4 (printed pages) · **OBS** seen in PA Manager v5.2 (`docs/research/PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md`) · **DEMO** demonstrated on a real Pa5X · **UNK** unknown.

## 1. Audit against the priorities

### P0 — research and Sound model (reuse, do not redo)
- `docs/research/PA5X_KST_RESOURCE_MODEL.md`: KST container, resource types, the Style → Sound → oscillator (High/Low velocity layer) → Multisample → zone → Sample chain, Drum Kit key → layers → sample, shared resources, load/merge semantics. All DOC or OBS; **no edge is decoded from bytes**.
- `docs/research/KST_PARSER_FEASIBILITY.md` + `KST_DATA_PROVENANCE_AND_RIGHTS.md`: byte-level analysis is gated (G1 data provenance, G2 licence, G5 legal basis) and **no dataset is cleared**. Nothing in this roadmap depends on lifting those gates.
- `docs/research/PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md`: PA Manager's Sample Manager, zone editor, DK report, copy closure, memory budget (OBS).
- Gap: the Sound-level parameters that matter for a converter (oscillator count, envelopes, filter, which ones import from TXPROG). Answered for the TXPROG path in `docs/research/TXPROG_FORMAT_NOTES.md` (Korg's TX16Wx guide): what transfers, what does not.

### P1 — Sound conversion
- Exists: `media/sf2_check.h` + `sf2-check` (reads SF2 zones, reports overlapping key × velocity zones that the manual says may not import, p.1206).
- Missing, in order of the success criterion: a sample/multisample model; an original sound source; a writer for a format the Pa5X imports; loop handling; resource budgeting; offline validation; one hardware import.

### P2 — KST management
- Exists: `kst-inventory` (type/bank folders, counts, sizes, byte-identical duplicates, PCM-folder warning, disk-size budget). Read-only.
- Missing: dependency graph edges (need decoding: gated), any write/merge (not planned until references are decoded; a file-level merge could silently break grid references, DOC p.722-723).
- Doable without decoding: transactional **folder-level** copy of a KST to a new location with hash verification and a manifest; capacity checks against the documented limits; duplicate detection by content hash.

### P3 — apps
- macOS: JUCE performance UI (`apps/macos`), builds in CI. iPadOS: build shell (`apps/ipados`). No Sound or KST workflow UI.
- Shared core is already portable C++ (engine, media, importers, exporters); new Sound code goes there.

### P4 — AI Style creator
- Kept, not the focus. The engine, SFF1 importer, Korg exporter and MIR prototype stay as they are.

## 2. Pa5X Sound / KST creation steps: what is known

| Step | Status | Source |
|---|---|---|
| Import **SF2** on the Pa5X: Instruments become **multisamples** (mono or stereo); overlapping zones may fail | DOC | p.1206, p.1209 |
| Import **TXPROG** (TX16Wx Program): becomes a **Sound** with its linked samples; TXPERF becomes a Drum Kit (OS ≥ 1.3.0) | DOC | p.1206, p.1210, p.1216 |
| Import **KMP/KSF** (Triton multisample, mono; stereo as `_L`/`_R` pairs) | DOC | p.1206 |
| Import a **folder of WAV/AIFF** samples (optionally into drum families by name prefix `B_`, `S_`, …) | DOC | p.1211 |
| Triton **PCG** Programs import (no Drum Kits); **Kronos PCG is not listed** | DOC | p.1206, p.1208 |
| Sample limits: 8/16-bit, 11 025-48 000 Hz, mono/stereo, max 1 048 576 frames (longer is truncated) | DOC | p.1155 |
| User Sample memory 4 GB (8 GB compressed); import goes to that memory and survives power-off | DOC | p.1156, p.1207 |
| A Sound oscillator has High/Low layers split by one velocity switch; key zones live in the Multisample | DOC p.1076-1077 + OBS zone editor | resource model §3a |
| After an SF2 multisample import, the Sound is built in Sound Edit (assign multisample to an oscillator, save to a User location) | DOC (procedure for imported multisamples, p.1207) | |
| Export a multisample as TXPROG + WAV, or WAV with `_OrgKey_LowKey_HighKey` names: a lawful **round-trip oracle** | DOC | p.1212-1215 |
| Any of the above actually done on the owner's Pa5X | **UNK** (nothing imported yet) | — |
| Native `.PCG` / `.KMP` / `.PCM` (KST) encoding | **UNK**, gated | feasibility doc |
| Kronos sample-based instruments: user-made Kronos KMP/KSF would go through the KMP import; Kronos factory ROM cannot be exported | DOC (KMP) / UNK (Kronos side) | — |

## 3. Shortest technically credible path

Write **open formats the Pa5X documents as import formats**, and let the Pa5X build its own native data. No proprietary format is written, no restricted data is parsed, and compatibility is proven on the instrument.

1. **SF2 multisample** (public SoundFont 2.04 spec; AIArranger already reads it) → Pa5X Import SF2 → multisample → owner assigns it to a Sound oscillator and saves → play. First target: it needs no unknown format.
2. **TXPROG** → Pa5X Import TX Sound → a complete Sound in one step. Next target, once the format is known from Korg's/CWITEC's public material and from files the owner creates (TX16Wx itself, or the Pa5X's own TXPROG export of our imported multisample).
3. **WAV folder** with `_OrgKey_LowKey_HighKey` names: fallback that needs only WAV.

Sources, in legal order: (a) original samples synthesized by AIArranger; (b) VST/AU instruments the user is licensed for, rendered offline on the Mac (no physical MIDI); (c) the user's own Kronos samples (KMP/KSF) or recordings of the Kronos audio output made by the user; Kronos factory content only within its licence and never redistributed.

## 4. Roadmap and acceptance criteria

| # | Deliverable | Acceptance (measurable) | Evidence label |
|---|---|---|---|
| S1 | Sample/multisample model (portable C++): samples (16-bit, rate, root, loop), zones (key range, root, tune, level), up to 2 velocity layers | Validation refuses anything outside the DOC limits (rate, frames, bits, overlapping zones, > 2 layers); unit tests | SOFTWARE |
| S2 | Original synthesized instrument source, loop-ready | Each sample's loop is an exact number of periods; seam discontinuity below a stated bound; pitch of each zone root within ±1 cent (measured on the rendered buffer) | SOFTWARE |
| S3 | SF2 writer (SoundFont 2.04) | Round trip through `sf2-check` (0 overlapping zones); **independent** reader (FluidSynth or another third-party SF2 implementation) loads it and renders the expected pitch per zone; all bounds-checked; fuzzed prefixes refuse cleanly | SOFTWARE + INDEPENDENT TOOL |
| S4 | CLI `aiarranger-sound`: build the demo instrument → `.sf2` + WAV set + a human-readable report (zones, loops, memory use vs 4 GB) | One command produces the USB-ready folder; deterministic bytes (same input → same hash) | SOFTWARE |
| S5 | TXPROG research note + writer | Format facts with sources; writer output re-read by TX16Wx (owner) **or** byte-structure equal to a Pa5X TXPROG export of the same multisample | SOFTWARE → DOC |
| S6 | Hardware acceptance procedure (owner) | Copy to USB; Import SF2 → multisample; build Sound; save to an empty User location; play C2-C6, both velocity layers; photo of Memory Info. Pass = audible correct pitch per zone and loops without clicks | **PHYSICAL HARDWARE PASS** only when the owner reports it |
| S7 | VST/AU offline sampler (macOS): render notes from a user-chosen plugin into S1's model | Same validation as S2 on the rendered buffers; no MIDI device opened | SOFTWARE |
| S10 | Done (SOFTWARE) | WAV-folder source (route C): `aiarranger-sound wav`, Pa5X export naming `<layer>_<OrgKey>_<LowKey>_<HighKey>.wav`, `smpl` root/loop, velocity layers by `--layers`/`--splits`. Test `test_wav_folder_source` (exact round trip of the demo package; malformed and unsupported WAVs refused; every truncated prefix refused) |
| S11 | Done (SOFTWARE) | Parameter audit `docs/research/SOUND_PARAMETER_AUDIT.md` (preserved / approximated / lost, by evidence class). G1 release: sampler measures note-off → −60 dB and reports it for the Amp EG (envelopes are never imported, GUIDE p.85). Next gap: G2 Drum Kits (TXPERF) |
| K3 | Done (SOFTWARE) | `kst-diff A B` (`src/media/kst_diff.h`): read-only folder comparison (identical / changed / added / removed / moved, byte equality, symlinks skipped). For backup verification and before/after comparison of two KST saves. `test_kst_diff` 12/12 (trees untouched). Not yet run on owner data |
| S12 | Done (SOFTWARE) | Drum Kits (G2): `drumkit.h` model (keys, 1-16 velocity layers, exclusive groups 1-8, drum families, pan/transpose/tune, +6 dB cap), `txperf_writer.h` (subset of Korg's Real-Drums example), `aiarranger-sound drums` original demo kit (9 samples, hi-hat choke, stereo open hat). Independent `tools/validate_drumkit_package.py` passes ours (strict) and Korg's example (--reference). `test_drumkit` 30/30. Notes: `docs/research/TXPERF_DRUMKIT_NOTES.md` |
| S13 | Done (SOFTWARE) | Sound Recipe (`sound_recipe.h`): each non-transferred parameter tagged MEASURED / ESTIMATED / EXTRACTED / NOT AVAILABLE with its basis and Pa5X page; rules refuse a value on NOT AVAILABLE and an estimate without basis. Measured: release (note-off → −60 dB), level per velocity layer. Plugin parameters listed raw (EXTRACTED, unmapped). Printed in the sampler REPORT as a post-import checklist. No automatic Pa5X synthesis-parameter conversion is claimed. `test_sound_recipe` 10/10 |
| K4 | Done (SOFTWARE) | `kst-merge-plan SRC DST`: read-only prediction of a KST load onto saved User data under the documented rules (Merge replaces same-name files, Replace All deletes the type, a PCM folder deletes all User Samples: MAN pp.734-736): replace / add / unchanged / duplicate / case-only (UNK) / kept. Tests in `test_kst_diff` (18). Not yet run on owner data |
| K2 | BLOCKED (material) | Kronos KMP/KSF import architecture: `docs/research/KRONOS_KMP_KSF_IMPORT.md`. Pa5X reads KMP/KSF natively (MAN pp.722, 1206); routes C (WAV) → A (native, owner test) → B (reader, needs authorized material + Pa5X export oracle) |
| S8 | Resource budgeting and packaging | Report per instrument: frames, bytes, % of 4 GB; refuses > 1 048 576 frames per sample; folder layout per import format | SOFTWARE |
| K1 | KST transactional copy (folder level) | Copy to a new folder, SHA-256 manifest verified, source untouched (hash before/after), never overwrites | SOFTWARE |

### Status 2026-11-07

| # | State | Evidence |
|---|---|---|
| S1 | Done | `src/media/sound/instrument.h`; `test_sound_conversion` (validation refuses rate, frames, short loop, overlap, 3 layers) |
| S2 | Done | `additive_source.h`: loop = exact whole periods (seam frames identical), worst pitch 0.005 cent, fundamental strongest line |
| S3 | Done | `sf2_writer.h`: re-read by `sf2::check` (0 overlaps), shdr/RIFF checks, truncation refusal. **Independent**: FluidSynth 2.6.2 renders 15 probe notes within 0.004 cent and switches layer at velocity 89/90 (`tools/validate_sound_package.py`) |
| S4 | Done | `aiarranger-sound demo --out DIR`: `.sf2`, `tx/` TXPROG package, `wav/`, `REPORT.txt`; two runs byte-identical; refuses an existing DIR |
| S5 | Writer done; owner/Pa5X re-read open | `txprog_writer.h`, format facts with sources in `docs/research/TXPROG_FORMAT_NOTES.md`; package re-read by Python's XML/wave readers (keys and velocities tile, files exist, loops inside) |
| S6 | Procedure written; **not executed** | `docs/evidence/hardware/PA5X_SOUND_IMPORT_PROCEDURE.md`; needs owner approval (§5.1) |
| S7 | Done (one-shot samples, mono) | `media/sound/rendered_source.h` (zones, trim, one gain, per-sample pitch report; `test_rendered_source` 16/16) + macOS `aiarranger-plugin-sampler` (AU/VST3, offline, no audio device, no MIDI port). Local functional run on Apple's DLSMusicDevice (host test only, output not kept): 18 samples, sf2-check 0 overlaps, FluidSynth pitch worst 3.2 cents (piano stretch). `--loop on` loops the held part (best-matching seam, crossfaded; match reported per sample): GM organ 0.994-0.999, FluidSynth worst 1.9 cents through the loop. Limit: stereo summed to mono |
| S9 | Done (SOFTWARE) | Stereo samples (interleaved model; SF2 linked L/R pairs with hard-panned zones; stereo WAV in TXPROG and wav/) and 1-24 velocity layers with explicit velocity zones (TX16Wx guide p.14). Sampler: `--vel 40,80,120 [--splits ...] --stereo on`. Local run on Apple DLSMusicDevice (host test, output not kept): 3 layers x 5 roots stereo, sf2-check 0 overlaps, FluidSynth worst 0.49 cents. Demo organ output unchanged byte for byte except `created-by` |
| S8 | Partial | `media/sound/package_writer.h`: one folder per instrument (.sf2, tx/, wav/, REPORT with % of 4 GB); refuses existing folders. Budget across several instruments not built |
| K1 | Done (folder level) | `tools/kst_copy.py SRC DST` / `--verify DST`: copy to a partial folder, re-hash every file, re-hash the source after the copy, rename into place; SHA-256 manifest next to the copy (the KST folder stays byte-identical); refuses existing targets, DST inside SRC, symlinks. Self-test in ctest (`test_kst_copy`). No KST file is parsed. Not yet run on owner data |

## 5. Owner decisions and blockers

1. **S6 needs approval** to import data into the Pa5X User Sample memory and save a User Sound. Importing does not change MIDI or global settings, but it writes user data. The procedure keeps all existing data: individual multisample import, never whole-KST load (whole-KST load deletes User Samples, p.736).
2. **S5:** format facts now come from Korg's own guide and examples. Ground truth that would still help: TX16Wx (free) opening our `.txprog` on the Mac, or a Pa5X TXPROG export of any multisample. The decisive test is S6.
3. **Kronos:** which Kronos material exists (user KMP/KSF, recordings), and its rights.
4. KST byte-level gates G1/G2/G5 remain open and are not needed for S1-S8.
