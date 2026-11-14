# Pa5X KST export plan: the documented route today, and the export blocker register

- Date: 2026-11-13 (E9, owner P0 correction: a validated pathway to Pa5X-readable libraries, with a documented-import
  alternative; "structurally valid" kept apart from "hardware-verified").
- Page numbers: **MAN** = Pa5X User Manual v1.4, printed pages. **KPG** = KRONOS Parameter Guide En 3, printed pages.
  No manual text is copied here; pages are cited.
- Detail per chain link and milestone: `docs/research/pa-studio/PA5X_KST_WRITER_ROADMAP.md` (E1). Owner captures C0-C6:
  `pa-studio-research/interop/PA5X_VALIDATION_PLAN.md` (outside the repo). Legal gates G1-G5:
  `docs/research/KST_PARSER_FEASIBILITY.md`.

## 1. Two labels, never mixed

| Label | Means | Who can give it |
|---|---|---|
| **STRUCTURALLY VALID** | Written to a documented layout and re-read by AIArranger's independent readers (L1, plus L2 where a second reader exists) | Software (this repo) |
| **HARDWARE-VERIFIED** | Imported, played (or loaded) and recorded on the owner's Pa5X (L3) | Only an owner record in Validation & Reports |

Nothing AIArranger writes is HARDWARE-VERIFIED today. Nothing is called "Pa5X-compatible".

## 2. The pathway that exists: Pa5X Import Kit (documented imports)

`aiarranger-kit build` (and **Pa5X Import Kit...** in Studio > Import / Export) gathers AIArranger outputs into one new
USB folder. The owner imports each member on the Pa5X through its own import pages, then the Pa5X saves the KST itself.

| Member | Kit folder | Pa5X import page | Readers that must pass (source and copy) |
|---|---|---|---|
| Sound package | `SOUNDS/<pkg>/` | TX Sound (TXPROG) MAN p.1206, 1207, 1210; alternative SoundFont (SF2) p.1209 | package manifest + package reader (RESOURCES.tsv, WAVs, TX references) + SF2 reader (no overlapping zones) |
| Drum Kit package | `DRUMKITS/<pkg>/` | TX DrumKit (TXPERF) MAN p.1206, 1210 | package manifest + package reader |
| KMP/KSF folder (EXPERIMENTAL) | `KMP/<folder>/` | KORG Multisample (KMP) MAN p.1206, 1209 | manifest + KMP/KSF reader (every KSF in full) |
| Style, marker SMF with the user's sound map | `STYLES/<name>.mid` | Style Edit > Import SMF MAN p.1037; Save Style p.1039-1040 | Korg Style SMF reader + validator |

The kit carries `KIT.md` (owner steps in import order, a page per step, what the Pa5X decides), `manifest.json`
(`aiarranger-pa5x-import-kit/1`, `"hardwareVerified": false`) and `PACKAGE.sha256`, and the top-level label
**STRUCTURALLY VALID — NOT VERIFIED ON A PA5X**. It is written as an E6 job (NAME.INCOMPLETE, staged verify, one
no-replace rename). Any member that fails a reader fails the whole kit. Names follow the FAT long-name rules for the
stick (MAN p.715: FAT32/FAT16 with long file names).

What the kit cannot do: choose a slot, name what the Pa5X creates, bind a Style track to a Sound beyond the Program
Change in the SMF, or write any Pa-native file. Those are the blockers below.

## 3. Export blocker register

Status: **DOCUMENTED** = a Korg manual gives it (page). **OBSERVED** = seen in an owner-made Pa5X file (none exists yet)
or, where said, in another tool. **UNKNOWN** = neither. **BLOCKED** = cannot proceed until the owner action happens
(evidence or authorization).

| # | Blocker | Status | Evidence needed | Owner action | Code that becomes possible |
|---|---|---|---|---|---|
| K1 | KST container: a `NAME.KST` folder of reserved type folders (MAN p.721), load/merge rules (MAN p.731-736) | **DOCUMENTED** (existence, load rules). Folder layout **OBSERVED** (2026-11-14, names only) in three owner Pa5X KST copies (one saved, two Pa5X SET conversions): `KEYBOARDSET`, bank sub-folders (user-named), one item per `.stg/.ksc/.pdc` file, `.MTP` in MATRIXPRESET, `My Setting.ksc` in SETTINGS, empty banks, bank `metadir.json`, `SONGBOOK/LISTDB.SBL` + `SONGDB.SBD/BANKnn.SBD`, `ConversionReport.txt` in conversions. Encoded in `library_validate.h` as WARN-level rules; file content and Pa5X loading of a written KST: **UNKNOWN** | **C0** manifest of a fresh "save all" KST; **C1** empty-user KST | FILE > Internal > All > Save to a new KST (MAN pp.789-791); copy read-only to the Mac outside every Git tree; `kst-backup` + verify | In-repo L1 KST structural validator (roadmap M4) with the observed names; `make_synthetic_kst_library.py` fixed to the observed layout |
| K2 | Native `.STG` (Style) writer | **UNKNOWN** content → **BLOCKED** (G1, G2, G5) | **C5** dataset `T_KST_V0` + one-change variants V1-V4 (`docs/research/KST_TEST_DATASET_SPEC.md`); **C6** same save twice (timestamps) | Build the dataset on the Pa5X and save each state | A read-only decoder first; a writer only after a byte-identical round trip of owner-made files (M8, M9) |
| K3 | Native `.KSC` (Keyboard Set) writer | **UNKNOWN** → **BLOCKED** (G1, G2, G5) | C5 variants that change one Keyboard Set; C6 | As K2 | As K2 |
| K4 | Native `.PDC` (Pad) writer | **UNKNOWN** → **BLOCKED** (G1, G2, G5) | C5 variants that change one Pad; C6 | As K2 | As K2 |
| K5 | Native `.PCG` (Sound grid) writer | **UNKNOWN** → **BLOCKED** (G1, G2, G5). What a Sound holds is DOCUMENTED (MAN p.1076-1077), not how | **C1 vs C2** (one TXPROG Sound added), **C1 vs C3** (one TXPERF Drum Kit) | After S6: save C2 and C3 as in the validation plan | Sound/PCG decoder, then writer (M7) |
| K6 | Pa-native `.KMP` (MULTISMP) writer | **UNKNOWN** → **BLOCKED**. The workstation KMP (KPG p.1158-1159) is DOCUMENTED and written by E1, but a Pa700 `RAM.KMP` is not that format, so the Pa5X's own is not assumed to be | **C2-C4** diffs (what one import adds under MULTISMP) | As K5, plus **C4** (a WAV-folder Samples Only import, MAN p.1211) | Pa-native multisample writer (M7) |
| K7 | `.PCM` (PCM folder) writer | **UNKNOWN** → **BLOCKED**. A whole-KST load replaces User samples (MAN p.736, DOCUMENTED) | C2-C4 diffs; C6 for non-content bytes | As K5 | Sample container writer (M7) |
| K8 | Slot / reference encoding: a User Sound's CC0/CC32/PC address | Grid rule **DOCUMENTED** (MAN p.722-723); the User address **OBSERVED in PA Manager only**, never on the owner's Pa5X → **UNKNOWN** for the Pa5X | The location S6 writes to, then a CC0/CC32/PC select of it from the Mac (B3, B4) | Record bank + number at S6 (checklist A2); authorize one Program Change test | Sound-map check that refuses addresses outside the User grid; the kit can pre-fill a sound map for imported Sounds |
| K9 | Style → Sound by Program Change survives Import SMF | **UNKNOWN** (P1.2 never run). Import SMF itself is DOCUMENTED (MAN p.1037; markers and chords p.974, p.980) | P1.2 with a sound map pointing at the S6 Sound | Import the kit's Style SMF, read each track's Sound, Save Style | If kept: the kit marks Style ↔ Sound pairs as linked. If not: KIT.md keeps the manual track-Sound step (today's text) |
| K10 | S6: TXPROG Sound import on the Pa5X | Import route **DOCUMENTED** (MAN p.1206, 1207, 1210); result **BLOCKED** (owner approval pending; `docs/evidence/hardware/S6_OWNER_CHECKLIST.md`) | Import message, location, Memory Info before/after, play results | Fill checklist part A, sign part B, run part C once | L3 record for the Sound route; the kit can show HARDWARE-VERIFIED per member only from that record |
| K11 | TXPERF Drum Kit import | **DOCUMENTED** route (MAN p.1206, 1210); result **UNKNOWN** (never run) | As K10 for a Drum Kit (C3) | After S6, one TXPERF import into an empty Drum Kit location | L3 for the Drum Kit route |
| K12 | SF2 route: which overlapping zones the Pa5X rejects | "may not import" **DOCUMENTED** (MAN p.1206); the exact rule **UNKNOWN** | One SF2 import with and one without an overlap | Only if the TXPROG route fails (KIT.md: never both) | Relax or tighten the kit's SF2 overlap refusal |
| K13 | Pa5X accepts KRONOS-generation KMP/KSF; which half of a `_L`/`_R` pair to select | **UNKNOWN** (MAN p.722 names Trinity/Triton; p.1206 the pair). E1 gaps E1-G2, E1-G3 | Import of a mono and a stereo kit KMP | Owner Import KMP test (A6, A8) | Drop EXPERIMENTAL from the kit's KMP member, or remove the route |
| K14 | Pa5X name and path limits (Sound/Style names, folder depth) | **UNKNOWN** (not in MAN; requirement 27). Stick rules are FAT long-name rules (general) | Imports with long names and deep folders | Note any truncation or refusal during S6/P1.2 | Tighten `fatName` with Pa5X limits |
| K15 | Legal gates G1 (data provenance), G2 (licence), G5 (legal basis) for any Pa-native decode | **BLOCKED** until owner-made captures exist (C0-C6) and the gates are answered | Owner-made KSTs only: no factory or purchased content, no PA Studio Suite internals (DR-001) | Provide C0/C1 and later captures; answer G1/G2/G5 | Lifts the gate on K2-K7 analysis; nothing is decoded before |

## 4. Order of work

1. **Now (software, done in E9):** the Import Kit. Owner can import the documented route end to end.
2. **Owner, first:** S6 (K10) with the kit's `SOUNDS/` member, recording the slot (K8). Then P1.2 (K9) with the kit's Style.
3. **Owner, then:** C0/C1 (K1), and C2/C3 after the imports (K5-K7 evidence).
4. **Software after C0/C1:** L1 KST structural validator from observed names (K1).
5. **Software after C2-C6 and G1/G2/G5:** decoders, then writers, each proven by a byte-identical round trip of an
   owner-made file before any write is offered. Native KST writing stays off until then.
