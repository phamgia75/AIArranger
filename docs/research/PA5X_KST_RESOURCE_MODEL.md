# Pa5X KST resource model

**Status: REVIEWED (2026-11-06).** Every DOC claim was re-checked against the manual text, with page numbers. No KST has been opened in PA Manager or decoded, so every dependency below is documented, not verified on data.

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
| A real Pa5X KST copy (`Korg Pa5x Gold Set.KST`, 667 files) has 15 upper-case type folders (STYLE, KEYBOARDSET, PAD, SOUND, PCM, MULTISMP, SONGBOOK, CHORDSEQUENCE, SETTINGS, MIDIPRESET, VOICEPRESET, GUITARPRESET, QTPRESET, MAXXAUDIOPRESET, MATRIXPRESET); STYLE, KEYBOARDSET, PAD, CHORDSEQUENCE and SONGBOOK hold bank sub-folders, the others hold files directly (audit §3c) | OBSERVED (file listing only) |
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

How to read this: the manual says grid data must be found "where expected by other types of data" and that its positions are fixed. That implies other resources refer to Sounds by **position**, but the manual does not spell out a reference format. *How* a Style or Keyboard Set file encodes its Sound reference is **UNKNOWN** (undecoded; roadmap P1.10, blocked).

## 3. Dependency graph

Solid arrows are documented; `?` marks an edge whose existence or mechanism is not documented.

```
Style (.STG) ──> Sound per Style track (whole Style), optionally per Style Element (p.932)
      ├──> 4 Keyboard Sets recalled when the Style is selected (p.84)
      └──> 4 Pads recalled when the Style is selected (p.194)
Keyboard Set (.KSC) ──> the set of Sounds played on the keyboard (p.84)
Pad (.PDC) ──?──> Sound                 (chapter title only, p.932: UNKNOWN)
Sound (.PCG) ──> 1-2 stereo Multisamples per oscillator (High/Low, velocity switch),
                 chosen as bank FTY|USR + number (p.1076-1077) ──> Samples (?)
Drum Kit = a special Sound (p.934) ──> per key, layers ──> Drum Samples, bank FTY|USR + number (p.1091)
SongBook entry ──> Style / Keyboard Set / Sounds / Pads / MID / MP3 / TXT (p.737)
```

| Edge | Evidence | Label |
|---|---|---|
| Style → Sound for each Style track, for the whole Style. Per-Style-Element Sounds can override it (Style Edit). Saved with the Style (User Styles only) | p.932-934 | DOC |
| Style → 4 Keyboard Sets assigned to the KEYBOARD SET buttons each time the Style (or SongBook Entry) is selected | p.84 | DOC |
| Style → its 4 Pads, recalled when the Style is selected | p.194 | DOC |
| Keyboard Set → the Sounds playing together on the keyboard, with their settings | p.84 | DOC |
| Pad → Sound | Only the chapter title "Choosing the Style and Pad Sounds" (p.932); the mechanism was not found | **UNKNOWN** |
| Drum Kits (including Percussion Kits) are special Sounds: one percussive instrument per key, not transposed | p.934 | DOC |
| Sound oscillator → 1-2 stereo multisamples (High/Low, velocity switch), selected in the editor by bank (FTY factory / USR user) + number | p.1076-1077 | DOC (editor view; file encoding UNKNOWN) |
| USR multisamples are loaded "when loading some User Sounds" | p.1077 | DOC |
| Multisample → Samples | Not read in the manual | **UNKNOWN** |
| Drum Kit key → layers → drum samples by bank (FTY/USR) + number, velocity switching | p.1091 | DOC (editor view) |
| Which Style uses which Sound in a **real** KST | PA Manager "Search Where this Sound is used…" lists, per Style, the **Style Element × track** using a Sound (audit §3e) | **OBSERVED** (PA Manager view; encoding UNKNOWN) |
| Keyboard Set → Sound, Pad → Sound in a real KST | Same search reports per-type use counts for KBD SETs and PADs (0 for the Sound tested) | **OBSERVED** (search exists; no positive case seen yet) |
| Drum Kit key → sample | DK Report: per key, RAM/ROM/RAM [Comp] + sample number + name (audit §3e) | **OBSERVED** (PA Manager view) |
| One user sample shared by several keys of a Drum Kit | Same sample number on two keys in the DK Report | **OBSERVED** |
| Ordinary Sound → Multisample → Sample in a real KST | Not shown by the USER SOUND REPORT | **UNKNOWN** |
| User Sound / Drum Kit slot capacity | PA Manager: 12 banks × 128 and 6 × 128 (= 1 536 / 768, manual p.1935) | **OBSERVED** (PA Manager model) + DOC |
| Sample Manager tabs (Sample / Drum Sample / MultiSample) and columns (ID, Sample Name, File Size, Sample Rate, Duration, Orig Note, Loop Start, Loop End, Type, PreLoad) | PA Manager v5.2, nothing loaded | OBSERVED |

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
  - ~~whether a Pa5X memory budget is offered~~: yes, "[4 GB] | PA5X" (OBSERVED, audit §3d).

## 6. Proposed independent implementation in AIArranger (macOS / iPadOS)

Only documented behaviour is used, with no PA Manager code and no decoding of protected executables.

1. **Done in `2226694` (no decoding):** `kst-inventory` (P1.9) reports, for a KST **copy**:
   - the type folder → bank folder grouping (p.731-732), plus counts by documented extension;
   - whether a **PCM** folder exists. If it does, it warns that loading the whole KST on a Pa5X deletes the User Samples in memory (p.736);
   - per-type counts and sizes, for comparison with PA Manager's KST Explorer summary.

   Still to do: a run on the research copy, to compare with PA Manager's KST Explorer.

   Read-only and never writes a KST. It runs the same on macOS and on iPadOS (sandboxed folder access).
2. **Memory estimate (P2.3):** sum the PCM / KMP sizes against a configurable sample-memory budget. PA Manager's own budget comes per model, and the Pa5X figure is UNKNOWN until it is observed or found in the manual.
3. **Dependency graph:** only once the reference encoding inside `.STG` / `.KSC` / `.PDC` / `.PCG` is known. That needs owner-approved format research (P1.10 stays **blocked**). Until then the graph has typed nodes and no edges, never name-guessed edges.
4. **Write / merge:** not planned. If it ever is, it must reproduce the §4 semantics (grid positions, Replace All vs merge, the User Sample deletion rule).

## 7. Missing evidence

- A **research copy** loaded in PA Manager: its views of Styles, Sounds, Keyboard Sets, Pads and Drum Kits, and any "used by" view.
- The meaning of PA Manager's STY / PRF / PAD / PCG / PCM counts.
- KST Merge and Remapping dialogs (observe only).
- PA Manager's export menu and formats.
- The Pa5X sample-memory size **in the manual**. PA Manager v5.2 budgets 4 GB for its "PA5X" model (OBSERVED, audit §3d); the manual's specifications list a 1 GB internal drive, which is storage, not sample memory.

Windows access was blocked by the Mac console lock on 2026-11-06. On 2026-11-07 the research copy's folder structure was listed (OBSERVED rows above; audit §3c); it was then opened read-only in KST Manager with no file changes (audit §3d).
