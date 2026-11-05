# Korg Pa5X export: format evidence, mapping, open blockers

Status: **software converter complete and structurally validated; Pa5X hardware
acceptance not yet tested.** Do not describe the output as "Pa5X compatible"
until §5 passes on a real instrument (see `docs/policy/CLAIMS_AND_RELEASE_NOTES_POLICY.md`).

## 1. Which artifact Pa5X accepts

| Candidate | Evidence | Decision |
|---|---|---|
| Native Pa5X style data (in a **KST** folder) | Pa5X OS 1.2 New Features p.14-21: Pa5X stores user data in KST folders and converts Pa4X **SET** folders to KST itself (File > Data Conversion). No public byte-level spec. Community reports call it a new format with no published converter. | **Not written.** Undocumented. Reverse engineering it would need owner-provided Pa5X files. |
| Pa4X `.SET/STYLE/*.STY` | Undocumented binary (KVR thread: SMF skeleton plus unexplained blocks). | Not written. |
| **Style as SMF "separated by Markers"** | Korg tutorial *Importing/exporting Styles as SMF separated by Markers (Pa2X/Pa800)*, korg.com/download/global/paseries/tutorials/ImportExportSMF.pdf. Pa5X OS 1.2 New Features p.39-46 confirms the **Style Edit > Menu > Import/Export > Import SMF / Export SMF** pages on Pa5X. | **Written.** This is the only documented import path. |

### Format rules implemented (quoted from the Korg tutorial unless marked)

- SMF **format 0** only.
- Style track / MIDI channel table: Bass 9, Drum 10, Percussion 11, Acc1-Acc5 12-16. Other channels are ignored on import.
- Marker names `EnCVn`, **lowercase only**: E = `v` variation, `i` intro, `f` fill, `e` ending. Element number 1-4 for variations, 1-2 for the rest. `cv` is fixed. CV number 1-6 for variations, 1-2 for the rest. Valid: `i1cv2`, `v4cv3`. Rejected: `V1cv2`, `v1CV2`, `intro i1cv2`, `v1cv1 chorus`.
- Marker order inside the SMF does not matter.
- Moved to the element header on import: Time signature (**mandatory**), CC0/CC32, Program Change, CC11. CC0, CC32 and PC **must be at tick 0** of each CV.
- Import: Initialize checked for a new style; SHIFT + Execute imports every marker in one pass.
- Pa5X (New Features p.46): a Text Meta Event in the Chords/Global track, or a **note C-1..B-1 on a chord-driven track**, imports as a Chord event.
- Pa4X manual p.287-290 (manualsdir): "If a note extends beyond the last measure of the Chord Variation, an additional measure is appended."

### Not documented (resolved conservatively, verify on hardware)

| Question | Conservative choice |
|---|---|
| Marker code for **Break** | Break is not exported. It is reported. |
| Codes for Intro 3, Fill 3-4, Ending 3 (Pa5X has these elements, but the 2009 table stops at 2) | Not exported. They are reported. |
| Accepted PPQN | Source resolution is kept (1920). `--ppqn N` rescales exactly if Pa5X rejects it. |
| Whether a tempo meta event is used | One FF 51 is written at tick 0. Also set the tempo on Pa5X. |
| Default Key/Chord and Chord Table after Initialize | Not encodable in the SMF. Set them on Pa5X (§4). |

## 2. Pipeline

```
Genos .sty --Sff1Reader--> ParseResult --Sff1ToUasfMapper--> UASF StyleDefinition (canonical)
   --mapToKorg (exporters/korg/genos_to_korg)--> korg::Style (Pa5X semantic model)
   --write (korg_style_smf)--> SMF bytes --parse (independent reader)--> validate --> file
```

CLI: `aiarranger-convert --input X.sty --target pa5x --output X.mid [--ppqn N] [--sound-map FILE] [--report FILE]`.
Exit codes: 0 means written and validated. 1 means a usage or input error. 2 means conversion or validation failed, and no file is written.

## 3. Mapping tables

**Sections**: Main A-D → `v1cv1`..`v4cv1`; Intro A/B → `i1cv1`/`i2cv1`; Fill In AA/BB → `f1cv1`/`f2cv1`; Ending A/B → `e1cv1`/`e2cv1`.
Not exported (reported): Intro C, Fill In CC/DD, Ending C, and Break (Genos "Fill In BA").
That last one includes the C_WHISPER 10-bar block with its unmarked 9-bar tail (issue #26).
Zero-length and duplicate sections are reported.
Every element gets one Chord Variation (`cv1`), because Genos sections have no chord variations.

**Tracks** (Genos part channel, after CASM source→destination routing → Korg):
Rhythm1 (9) → Percussion (11); Rhythm2 (10, main kit) → Drum (10); Bass (11) → Bass (9); Chord1 / Chord2 / Pad / Phrase1 / Phrase2 (12-16) → Acc1-Acc5 (12-16).
Channels 1-8 with no CASM route are dropped and counted.
When two Genos channels land on one Korg track, a warning is emitted. Example: POP_ACOUSTIC_2 "E.P rt", ch 4, is routed to ch 15.

**Events**
- Notes are paired FIFO per pitch.
- A note that crosses the section end is ended on the last tick. Otherwise Pa would append a measure.
- A note that starts on the last tick is dropped.
- PPQN is rescaled per absolute tick (`round(t*dst/src)`), so there is no drift.
- CC, pitch bend and aftertouch are kept.
- Yamaha bank select and program change are **never forwarded**, because Genos voice numbers mean nothing on Pa5X. They are recorded in the report.
- With `--sound-map`, user-supplied Pa5X bank/PC values are written at tick 0 of every element.
- Notes 0-11 on chord-driven tracks are dropped and counted, because they would become Chord events. This is mostly MegaVoice noise.

**Sounds**: every track reports its Yamaha MSB/LSB/PC, a family hint (Drum Kit, Bass, Piano/EP, Guitar, Strings, Brass, Pad, Generic) and its Pa5X target.
The target is `UNRESOLVED` unless the sound map provides it. No Pa5X bank/program numbers are invented.

**Drums**: note numbers pass through unchanged. Drum and Percussion are not chord-driven on Korg, so they are never transposed.
Notes outside GM 35-81 are kept and counted per note, for example the Revo-kit notes 13-29. Whether a Pa5X kit has a sound there is unverified.
No Yamaha→Korg drum translation table exists in lawful sources, so none is invented.

**Chord rules (NTR/NTT)**: the SMF format has no field for them. The Pa5X equivalents (Key/Chord, Chord Table, per-track transposition settings) are element and track parameters set in Style Edit.
The report lists each track's Genos NTR/NTT so they can be programmed. CASM source chord in the corpus is CMaj7 (Ctb2 bytes 18-19).

## 4. Corpus results (local, `test_korg_corpus_conversion`)

| Metric | POP_ACOUSTIC_2 | CLASSIC_6_8 | BALLAD_FOLK | C_WHISPER |
|---|---|---|---|---|
| Elements exported / source sections | 8/11 | 8/11 | 9/12 | 8/11 |
| Source channels reported | 9 | 16 | 9 | 8 |
| Notes written | 2033 | 2051 | 1440 | 860 |
| Pa5X sounds unresolved (no sound map) | 9 | 8 | 9 | 8 |
| Non-GM drum notes kept | 570 | 255 | 342 | 252 |
| Dropped events (all reasons) | 1078 | 681 | 1015 | 1732 |
| Tempo | 100 | 90 | 105 | 76 |
| Meter | 4/4 | 6/8 | 4/4 | 4/4 |
| Validator (re-parsed bytes) | PASS | PASS | PASS | PASS |
| Byte-identical on rewrite | yes | yes | yes | yes |
| Kept notes identical after re-parse | yes | yes | yes | yes |

Most dropped events belong to the unexported sections (Fill CC/DD, Break) plus the Yamaha voice selects. `aiarranger-convert` prints the full breakdown for each style.
Generated `.mid` files are **not committed**: they derive from the Yamaha corpus, which is under legal review in issue #27.

## 5. Pa5X hardware acceptance checklist

1. Copy `X.mid` to a USB stick (or the Pa5X internal drive over USB).
2. On Pa5X, choose any User style and open Style Edit > Menu > Import/Export > Import SMF. Select the file, check **Initialize**, then press **SHIFT + Execute**. Record the exact message if it fails.
3. Confirm the elements appear: Variation 1-4, Intro 1-2, Fill 1-2, Ending 1, and Ending 2 where present.
4. Set Key/Chord to C Maj7 for each element (the Genos source chord). Set the Chord Table so every chord uses CV1. Set tempo. Assign Sounds from the report's family column.
5. Play Variation 1-4, Fill, Intro and Ending. Use C, Am, G7 and Bdim. Check that bass follows the root, chords follow the quality, and drums do not transpose.
6. Check CLASSIC_6_8: the meter is 6/8 and no extra measure is appended to any element.
7. Start/Stop many times and confirm there are no stuck notes.
8. Export the imported style again (Export SMF, SHIFT + Execute). Keep that file: it is the first real Pa5X sample. Run it through `korg::parse` to confirm the marker codes. Check for any Break code and for the CV SysEx the Pa5X writes.
9. If a step fails, record the step, the exact message, and the file. Patch the writer and regenerate.

## 6. Open blockers

1. **Hardware**: there is no Pa5X result yet for steps 2-8 of §5.
2. **Break / Intro 3 / Fill 3-4 / Ending 3 marker codes**: not in public docs. A Pa5X Export SMF from step 8 resolves this.
3. **Pa5X sound and drum-kit numbers**: there is no lawful table. The owner must provide a `--sound-map`, or the sounds are assigned on the instrument.
4. **Native KST style container**: undocumented. Out of scope unless the owner provides Pa5X files and approves a reverse-engineering effort.
