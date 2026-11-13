# Pa5X KST writer roadmap: from samples to a KST, link by link

- Date: 2026-11-13. Run `run_f9bc7bdfd545`, worker W11 (E1). Branch `feat/e1-kmp-ksf-writer` from
  `origin/feat/pa5x-first-baseline` `dd22522`.
- Decisions: D12 (E1 = documented-format sound milestone) and **D13** (2026-11-13, owner): native KST writing is now a
  **strategic objective**. It is no longer a non-goal. It may be built only on documented or owner-observed semantics,
  and never on PA Studio Suite internals. Decisions file: `pa-studio-research/roadmap/DECISIONS.md` (outside the repo).
- Inputs: `pa-studio-research/static-and-gap/DEPENDENCY_CHAIN.md`, `FORMAT_MATRIX.md`, `UNSUPPORTED_ASSUMPTIONS.md`,
  `interop/KST_STRUCTURE_REQUIREMENTS.md` and `interop/PA5X_VALIDATION_PLAN.md` (captures C0-C6), plus
  `roadmap/SET_KST_GAP.md` and `REPO_CAPABILITY_BASELINE.md`. In this repo: `docs/research/KST_PARSER_FEASIBILITY.md`
  (gates G1-G5) and `PA5X_KST_RESOURCE_MODEL.md`.
- Page references:
  - MAN5 is the Pa5X User Manual v1.4, cited by printed page.
  - KPG is the KRONOS Parameter Guide En 3, cited by printed page. Its "Korg format sample files" section is printed
    p.1158-1160, which is PDF pp.1172-1174.

Tags used on every link:

| Tag | Meaning |
|---|---|
| **DOCUMENTED** | A Korg manual describes the on-disk layout or the behaviour (page given) |
| **OBSERVED** | Seen in an **owner-made** file. **None exists yet**: no owner Pa5X KST was found (W13 `owner-libraries/INVENTORY.md`, "Pa5X-made KST found: no"). The only owner-identified collection is a Pa700-family SET, which is not a Pa5X format |
| **UNKNOWN** | Neither a manual nor an owner file says it |

Verification levels for outputs: **L1** structural (written to a documented layout and checked by software).
**L2** read back by an independent reader or app. **L3** owner hardware test, recorded. Nothing below is L3.
No output is labelled Pa5X-compatible.

## 1. The chain

`KSF/KMP/WAV -> Multisample -> Sound -> Bank/slot -> Style refs -> KST`

| # | Link | On-disk form | Tag | Evidence | AIArranger (this branch) |
|---|---|---|---|---|---|
| 1a | WAV/AIFF sample | WAV `<name>_<OrgKey>_<LowKey>_<HighKey>.wav` | DOCUMENTED | MAN5 p.1155 (8/16-bit, 11 025-48 000 Hz, at most 1 048 576 frames), p.1212 (naming) | read (`wav_folder_source.h`) and written (`wav_writer.h`) |
| 1b | KSF sample (workstation) | chunks `SMP1 SMD1 [SMF1] [SNO1] [NAME]`, big endian | DOCUMENTED | KPG p.1158-1159 | read (`korg_kmp_source.h`). **Written (E1, new)**: `korg_kmp_writer.h` |
| 1c | KSF shared data (`SMF1`) | SMD1 holds only its 12 header bytes. A 12-byte file name points at the KSF whose SMD1 holds the audio | DOCUMENTED | KPG p.1159 | **read (E1, new)**, see §4 |
| 1d | KSF divided across media (`SPD1`/`SDD1`) | split audio | DOCUMENTED | KPG p.1159-1160 | refused, by name |
| 2a | KMP multisample (workstation) | `MSP1 [NAME] RLP1 [RLP2] [RLP3] [MNO1]`. A region = top key, original key, tune and KSF name. Low key = previous top + 1 | DOCUMENTED | KPG p.1158-1159 | read. **Written (E1, new)** |
| 2b | KMP stereo | KRONOS names the halves `<name> -L`/`-R`. The Pa5X KMP import pairs `_L`/`_R` *file* names | DOCUMENTED (both sides) | KPG p.836, p.1161; MAN5 p.1206, p.1209 | the writer does both: `-L`/`-R` inside the files, `_L`/`_R` in the file names |
| 2c | Pa5X accepts KRONOS-generation KMP/KSF | — | UNKNOWN | MAN5 p.722 names Trinity/Triton only | needs an owner Import test (H-test A6/A8) |
| 2d | Pa-native multisample in a KST (`MULTISMP/*.KMP`, `PCM/*.PCM`) | container | UNKNOWN | name listing only (one KST of unknown provenance, `PA5X_KST_RESOURCE_MODEL.md` §1). The Pa700 `RAM.KMP` is **not** the workstation `MSP1` format (W13) | none |
| 3a | Multisample -> Sound by import (TXPROG/TXPERF, SF2) | TX16Wx XML + WAV; SF2 | DOCUMENTED | MAN5 p.1206-1210; Korg TX16Wx guide | written (`txprog_writer.h`, `sf2_writer.h`). S6 never run |
| 3b | Sound oscillator -> multisample (High/Low velocity switch, bank FTY/USR + number) | inside `.PCG` | DOCUMENTED *what*, UNKNOWN *how* | MAN5 p.1076-1077 | none (gated) |
| 4 | Sound -> bank/slot (User grid, CC0/CC32/PC) | inside `.PCG`, grid position | DOCUMENTED grid rule (MAN5 p.722-723); the User address is reported by PA Manager only | MAN5 p.569; `PA5X_KST_RESOURCE_MODEL.md` §3 | provisional ids only (`package_graph.h`) |
| 5 | Style track/Element -> Sound; Style -> 4 Keyboard Sets, 4 Pads | inside `.STG` (`.KSC`, `.PDC`) | DOCUMENTED *what*, UNKNOWN *how* | MAN5 p.84, p.194, p.932-934, p.1936 | user-supplied CC0/CC32/PC in the marker SMF (`sound_map.h`) |
| 6a | KST container: a folder `NAME.KST` with reserved type folders, then banks, then files | folder tree | DOCUMENTED (folder, reserved sub-folders, load/merge rules) | MAN5 p.721, p.731-736 | file-level tools only (`kst_inventory.h`, `kst_diff.h`, `kst_backup.h`) |
| 6b | Type-folder spelling, bank folder names, files per bank | names | UNKNOWN (one listing of unknown provenance) | `KST_STRUCTURE_REQUIREMENTS.md` rows 3-5, 9-10 | none |
| 6c | Content of `.PCG .KMP .PCM .STG .KSC .PDC .SBD` in a KST | binary | UNKNOWN | no Korg layout published (`FORMAT_MATRIX.md`) | none |

**Where the documented part ends.** Every link up to 2b and 3a has a public layout, and AIArranger now writes all of
them (WAV, SF2, TXPROG/TXPERF and, new in E1, KMP/KSF). From link 2d upwards, every on-disk link is a Pa-native file
with no published layout. D13 does not change that. It changes only whether AIArranger *aims* to write them.

## 2. Plan under D13 (replaces "KST write: not planned")

`PA5X_KST_RESOURCE_MODEL.md` §6.4 ("Write / merge: not planned") and `KST_PARSER_FEASIBILITY.md` (non-goal "writing or
repairing KST data") predate D13. The staged plan is:

| Stage | What | Basis | Status |
|---|---|---|---|
| **(a) Interim route, kept** | AIArranger writes the documented imports (TXPROG/SF2/WAV packages, KMP/KSF folders, marker SMF Styles with a sound map). The owner imports them on the Pa5X, assigns them and saves a KST. AIArranger backs that KST up and verifies it (`kst-backup` + verify, `kst-inventory`, structural checks) | DOCUMENTED imports + file-level tools | Built in software (L1). Hardware steps pending (S6, P1.2) |
| **(b) Native KST writing, step by step** | Write a Pa-native file only where its meaning is DOCUMENTED, or OBSERVED in owner-made Pa5X KSTs together with controlled before/after keyboard saves (captures C1-C6, one change each). Order: the container layout (6a/6b), then grid files (Sound/PCG), then the referrers (STG/KSC/PDC) | owner captures, gates G1/G2/G5 answered | Not started: no owner Pa5X KST exists yet |
| **(c) Excluded** | No layout, rule or byte pattern taken from PA Studio Suite internals (EULA; DR-001). Its vendor claims are cross-checks only | — | Standing rule |

## 3. Milestones: buildable now vs gated

| Milestone | Links | Buildable now? | Gate | Owner captures / tests that unlock it |
|---|---|---|---|---|
| M1 KMP/KSF writer | 1b, 2a, 2b | **Done (E1), L1** | none | Route A Import KMP on the Pa5X (A6, A8) for L3; a KRONOS load of the folder would also give L3 for the KRONOS |
| M2 SMF1 shared-data reading | 1c | **Done (E1), L1** | none | a real KMP whose KSFs use SMF1, owner-made (A7) |
| M3 KSC 2.0 sample collection (KRONOS) | — | buildable (KPG p.1160-1163 DOCUMENTED) | none | not needed for the Pa5X: its `.KSC` is a Keyboard Set (MAN5 p.721). Not planned |
| M4 In-repo L1 KST structural validator | 6a, 6b | yes, for the DOCUMENTED rules (MAN5 p.721, p.731-736) | none for the code; rules 3-5 need confirmation | **C0** (manifest) and **C1** (empty-user KST) set the reserved folder and bank names |
| M5 Sound import proven | 3a | software done | hardware | **S6**, then **C2** (one TXPROG Sound) and **C3** (one TXPERF Drum Kit) |
| M6 Bank/slot addressing | 4 | no | G1 for any byte analysis | **C2** (one Sound in a known User slot) plus a CC0/CC32/PC select from the Mac (B3, B4) |
| M7 Sound/PCG + Pa-native KMP/PCM writing | 2d, 3b | no | **G1, G2, G5** | **C1 vs C2/C3/C4** diffs (what one import adds to SOUND/MULTISMP/PCM). **C6** (the same save twice: which bytes are timestamps) |
| M8 Style/Kbd Set/Pad references | 5 | no | **G1, G2, G5** | **C5** (`T_KST_V0` and the one-change variants V1-V4 of `KST_TEST_DATASET_SPEC.md`), B5-B7 |
| M9 Native KST container write | 6 | no | **G1, G2, G5** and M4, M7, M8 | **C0, C1, C6**, then a Pa5X load of an AIArranger-written KST (H3) for L3 |

## 4. SMF1 (E1 task 3): the Parameter Guide defines it

KPG p.1159 lists two KSF structures:

- `SMP1 + SMD1 + SNO1 + NAME`.
- `SMP1 + SMD1 + SMF1 + SNO1 + NAME`. Here the Sample chunk's data is empty, and the audio is the data of the KSF named
  in the 12-byte Sample Filename chunk ("shared sample data").

The reader now resolves it. Every case that p.1159 leaves open is refused:

- Resolved:
  - The named file must be a plain file name in the **same folder**. It is matched without case, as FAT does.
  - Refused: `/`, `\`, `:`, `.`, `..`, control characters and symbolic links, because they could point outside the
    folder.
  - Also refused: the file naming itself, `SKIPPEDSAMPL`, `INTERNALnnnn`, and a chain (the named file is itself
    shared).
- Refused because p.1159 does not say:
  - SMF1 while SMD1 also holds audio.
  - An empty SMD1 with no SMF1.
  - A named file whose rate or sample count differs from this file's SMD1 header. The guide does not say which one
    applies.
- The sharing file's own SMP1 (Start, Loop Start, End) and attributes apply to the shared audio.
- The KSF-only route (`ksf_folder_source.h`) reads each KSF on its own. It still refuses SMF1, and the message now names
  the chunk and its target file.
- **Open (OBSERVED in third-party files only, so not OBSERVED in the sense above):**
  - W3's structure walk saw `SMP1 SNO1 SMF1 SMD1` and `SMP1 SNO1 SMD1` in vendor KSFs.
  - The KMP route's 60-byte scan accepts only files that start `SMP1` then `SMD1`, the order p.1159 lists. That rule
    was added by an earlier review and is unchanged.
  - So such files are refused at the scan, with "does not start with SMP1 ... then SMD1".
  - Relaxing it needs an owner-made KRONOS save (no vendor bytes) and a review decision.
  - Recorded as gap **E1-G1**.

## 5. What the E1 writer does and does not claim

- **Claims (L1):**
  - The files follow KPG p.1158-1159 field by field.
  - Every name fits its documented field:
    - KSF and KMP file names are 8.3. The RLP1 file name field is "12 bytes incl. period and extension".
    - Multisample names fit 16 bytes in MSP1 and 24 in NAME.
    - Sample names fit 16 in SMP1 and 24 in NAME.
  - Before the folder is published, the existing reader reads it back. Zones, roots, tune, loops, rate, stereo pairs
    and audio must be equal (`test_kmp_writer`, `test_kmp_cli`).
- **Refuses:**
  - Zone tune outside -99..+99 (KPG p.1158).
  - A layer that mixes mono and stereo samples. KSF channels are documented as 1, so stereo is a `-L`/`-R` pair.
  - Names the reader would not give back unchanged: a comma, leading or trailing spaces, or a mono name ending `-L`/`-R`.
  - Two layers with the same name.
  - Everything the instrument model already refuses (Pa5X limits, MAN5 p.1155).
- **Does not carry (stated in REPORT.txt):**
  - Zone level. RLP1 Level is documented as "-99…+99 cents"; its unit for the Pa5X is unknown, so it is written as 0.
  - Velocity layers, which are not in a KMP.
  - Envelopes, filters and effects.
  - Audio after a loop end: it is kept in the KSF, but End = the loop end, so it does not play.
- **Inferred, not documented:** KSF End is inclusive (the last looped sample). The reader uses the same convention.
  A KRONOS WAV export of the same multisample settles it (A3).
- **Not claimed:**
  - Any acceptance by a KRONOS or a Pa5X. That is L3 and owner-only.
  - That Pa5X Import KMP reads KRONOS-generation files (2c).

## 6. Gaps recorded by E1

| Id | Gap | Unlocks it |
|---|---|---|
| E1-G1 | The KMP-route scan refuses KSFs whose chunks are not in p.1159's listed order. Vendor files use `SMP1 SNO1 ...` | an owner-made KRONOS save, then a review decision on chunk order |
| E1-G2 | The RLP1 Level unit on the Pa5X is unknown, so it is not carried | Pa5X import of a KMP with a known level (H-test) |
| E1-G3 | Whether the Pa5X accepts KRONOS KMP/KSF (and `-L`/`-R` names inside `_L`/`_R` files) is unknown | owner Import KMP test, mono and stereo |
| E1-G4 | KSF End inclusive (A3) is inferred | KRONOS "Export Samples as WAV" twin of one KSF |
| E1-G5 | No owner Pa5X KST exists, so M4-M9 have no OBSERVED basis | C0/C1 first |
