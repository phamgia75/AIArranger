# Pa5X KST resource model

**Status: DRAFT (2026-11-06).** Built from the Pa5X User Manual and two read-only PA Manager screens. No KST has been opened or decoded, so every dependency below is documented, not verified.

Labels:

- **OBSERVED**: seen on PA Manager v5.2 on Windows (`PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md` §3a).
- **DOC**: Pa5X User Manual v1.4 (2025-11-25), printed page numbers. Paraphrased; no tables reproduced.
- **UNKNOWN**: no evidence.

Dependencies are **never** inferred from file or folder names (project rule).

## 1. Container

| Statement | Label |
|---|---|
| A `.KST` is a reserved **folder** holding all the User data of one Pa5X, organized in reserved sub-folders (p.721, p.731) | DOC |
| Inside the KST, data is grouped by **type**, then by **bank** (folder), then by individual file (p.731-732) | DOC |
| PA Manager's KST Explorer treats a KST as a folder and summarizes it without loading. One original KST showed: 3.40 GB; STY 1893, PRF 1105, PAD 1029, PCG 16, PCM 231 | OBSERVED (summary only) |
| A KST that contains User Samples has a **PCM** folder (p.736) | DOC |
| What PA Manager's labels count: whether "STY" counts `.STG` files, "PRF" `.KSC` Keyboard Sets and "PAD" `.PDC` | **UNKNOWN**. The labels differ from the manual's extensions, so the mapping is not assumed |

## 2. Resource types (p.721-722)

The table lists the documented extensions. The meaning column is a short paraphrase, and the last column says how each type is organized and referenced (p.722-723).

| Extension | Resource | Organization |
|---|---|---|
| `KSC` | Keyboard Set (library) | free files/folders, by name |
| `STG` | Style | free files/folders, by name |
| `PDC` | Pad | free files/folders, by name |
| `CSC` | Chord Sequence | free files/folders, by name |
| `PCG` | Sound (Pa5X's own; Triton `.PCG` is a different, read-only import) | **reference grid** (fixed positions) |
| `KMP` | Multisample | **UNKNOWN**: grid or free |
| `PCM` | Sample | **UNKNOWN**: grid or free |
| `MPR`, `VOC`, `GTR` | MIDI / Mic / Guitar Presets | reference grid |
| `GBL`, `SBD`, `SBL` | Global, SongBook database and Set List (bundles only) | – |
| `MID`, `MP3`, `JBX`, `TXT`, `ARG`, `QTP`, `MXP`, `AIF`, `WAV` | songs, text, arrangements, presets, audio | – |
| Read only: `PKG`, `KAR`, Triton `PCG` / `KMP` / `KSF`, `SF2` | OS packages, karaoke, imports | – |

**Key documented rule (p.722-723):**

- **Reference-grid** data must always sit "where expected by other types of data": positions are fixed, so empty locations exist.
- Name-organized data (Styles, Keyboard Sets, Pads, Chord Sequences, Songs) is listed alphabetically.

Consequence: other resources point at a Sound by its **grid position**, not by its name. *How* a Style or Keyboard Set encodes that position inside the file is **UNKNOWN** (undecoded; roadmap P1.10, blocked).

## 3. Dependency graph

```
Style (.STG) ──┐                          ┌─> Multisample (bank FTY|USR, number) ──> Sample(s)
Keyboard Set ──┼─> Sound (.PCG, grid) ────┤      High/Low layer per oscillator, velocity switch
Pad (.PDC) ────┘   or Drum Kit            └─> Drum Kit key layers ──> Drum Sample (bank FTY|USR, number)
SongBook entry ──> Style / Keyboard Set / Sounds / MID / MP3 / TXT (relinked on conversion)
```

| Edge | Evidence | Label |
|---|---|---|
| Sound oscillator → 1-2 stereo **multisamples** (High/Low, velocity switch), chosen by **bank (FTY factory / USR user) + number** (p.1076-1077) | DOC |
| USR multisamples are loaded "when loading some User Sounds" (p.1077) | DOC |
| Drum Kit key → layers → **drum samples** by bank (FTY/USR) + number, velocity switching (p.1091) | DOC |
| Style / Keyboard Set / Pad → Sound or Drum Kit | DOC by implication only: "Wrong sounds" is fixed by reloading the right User Drum Kits, Styles or Keyboard Sets (p.1933). How the link is encoded: **UNKNOWN** |
| Which Style uses which Sound, in a real KST | **UNKNOWN**: PA Manager's own views not yet seen with a loaded copy |
| Sample Manager shows Sample / Drum Sample / MultiSample tabs with Orig Note, Loop Start/End and Sample Rate columns | OBSERVED (empty) |

## 4. Load / merge behaviour on the Pa5X (DOC, p.733-736)

These are the semantics an independent tool must respect:

- **Granularity:** a whole KST, one data type, one bank, or single files can be loaded.
- **Merge vs Replace:**
  - **Replace All** deletes every existing User file of that type;
  - otherwise files **merge**, and only same-name files are replaced.
  - Replaced data is deleted.
- **User Samples are special:**
  - loading a whole KST with User-Sample-based Sounds **deletes all User Samples in memory**;
  - loading individual Sounds keeps the samples already there.
  - If sample memory or the sample count overflows, the load is aborted and the newly loaded samples are removed; existing ones stay.
- **Merging samples from several KSTs:** load one KST, then load single Sounds from the others, then save the whole memory to a KST.
- **Pa4X conversion (p.737-738):**
  - Sounds, Drum Kits, Samples, Keyboard Sets, Styles, Pads and SongBook are converted;
  - Global, MIDI Presets and Voice Presets are not;
  - missing source data stays missing;
  - a `ConvertReport.txt` is written into the new KST.

## 5. PA Manager (Windows) features relevant here

- **OBSERVED:**
  - KST Explorer: per-KST size and type counts without loading;
  - Sample Manager: Sample / Drum Sample / MultiSample tabs, compression and decompression, import of aif / wav / kmp / …;
  - a sample memory budget per model (default "400 MB | PA4X", Total 419430400 bytes).
- **UNKNOWN:**
  - KST Merge and Remapping workflows (not opened);
  - "used by" or dependency views;
  - export formats;
  - whether a Pa5X memory budget is offered.

## 6. Proposed independent implementation in AIArranger (macOS / iPadOS)

Only documented behaviour is used, with no PA Manager code and no decoding of protected executables.

1. **Now (no decoding):** extend `kst-inventory` (P1.9) to report, for a KST **copy**:
   - the type → bank → file tree, by the documented extensions;
   - whether a **PCM** folder exists. If it does, warn that loading the whole KST on a Pa5X deletes the User Samples in memory (p.736);
   - per-type counts and sizes, for comparison with PA Manager's KST Explorer summary.

   Read-only and never writes a KST. It runs the same on macOS and on iPadOS (sandboxed folder access).
2. **Memory estimate (P2.3):** sum the PCM / KMP sizes against a configurable sample-memory budget. PA Manager's own budget comes per model, and the Pa5X figure is UNKNOWN until it is observed or found in the manual.
3. **Dependency graph:** only once the reference encoding inside `.STG` / `.KSC` / `.PDC` / `.PCG` is known. That needs owner-approved format research (P1.10 stays **blocked**). Until then the graph has typed nodes and no edges, never name-guessed edges.
4. **Write / merge:** not planned. If it ever is, it must reproduce the §4 semantics (grid positions, Replace All vs merge, the User Sample deletion rule).

## 7. Missing evidence

- A **research copy** loaded in PA Manager: its views of Styles, Sounds, Keyboard Sets, Pads and Drum Kits, and any "used by" view.
- The meaning of PA Manager's STY / PRF / PAD / PCG / PCM counts.
- KST Merge and Remapping dialogs (observe only).
- PA Manager's export menu and formats.
- The Pa5X sample-memory size. Not found in the manual's specifications (they list a 1 GB internal drive, which is storage, not sample memory).

Windows access was blocked by the Mac console lock at the start of this session.
