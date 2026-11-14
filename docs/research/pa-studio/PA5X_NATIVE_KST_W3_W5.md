# Native KST writer W3-W5: observed structure, write rules, UNKNOWNs (2026-11-14)

Owner-authorised offline steps (task #41; plan `pa-studio-research/owner-libraries/pa5x-kst/NATIVE_KST_WRITER_UPDATED_PLAN.md`
§2-§3). **Nothing here is hardware-verified.** No Pa5X has loaded a file written by this code.

Source of every number: a read-only structure survey of the verified owner copy `Nov 2026.KST` (`SOURCE.sha256` 4 443
OK before and after). The survey reported structure only: node paths, XML element and attribute names, whether an
attribute held one value in every file, and the per-name role and track numbers. It reported no names, bytes or
musical content. The survey scripts are outside the repo. The repo holds only the derived structure names
(`tests/support/kst_observed_names.h`) and the format constants written by `src/media/panative/kst_builders.h`.

## Target versions (the ones the Pa5X itself wrote in its 2024 conversions)

| Object | Version | Files in the copy | Shape |
|---|---|---|---|
| Pad `.pdc` | `CLIP type="PadClip" version="0" release="4"` | 438 | Root → `Main.mid`, `ClipDescription.xml` |
| KBD Set `.ksc` | `KeyboardSetClip` v0 r4, session form | 1 053 | Root → `Main.ssn`{`RTTracks.scn`{`Lower/Upper1/Upper2/Upper3.plc`{`Main.mid`, `ClipDescription.xml`}, `SceneDescription.xml`}, `SessionDescription.xml`}, `MFX.mid`, `ClipDescription.xml` |
| Style `.stg` | `STAGE type="1" version="1" release="2"` | 443 (177 in the `SongStyle1` form) | Root → `Main.ssn`{`SongStyle1.scn`{`<name>.stc`, `SceneDescription.xml`}, `PadSet1.scn`, `KbdSet1-4.scn`, `SessionDescription.xml`}, `StageDescription.xml` |

Corrections to earlier notes, OBSERVED:
- **PadClip v0 r4 `Main.mid` is SMF format 1** (2-5 tracks) in 438/438 files, never format 0. Track 1 holds the header
  meta events, SysEx and the sound select; track 2 holds the notes, all on channel 1. Format 0 is used by the KBD Set
  parts and `MFX.mid`. The plan's "SMF0" applies to those, not to Pads.
- **STAGE v1 r2 has no `.ksa` branch.** The Style's 4 KBD Sets are embedded as `<name>.ksc` inside the fixed scenes
  `KbdSet1.scn`-`KbdSet4.scn` (scenenum 25-28, 177/177). The 4 Pads sit in `PadSet1.scn` (scenenum 15, tracks
  17-20). `.ksa`/`ks1-ks4` belongs to another version.
- **Every Pad, KBD Set and Style MIDI leaf carries Korg SysEx** (`F0 42 7F`/`F0 42 3F`). Examples: 438/438 Pad
  `Main.mid` (5 382 events), 1 053/1 053 of each KBD Set part, and 17 000 Style track clips. Chord clips (`.csc`) and
  meta-event clips carry none. The earlier "0" counts came from searching for the byte pair `F0 42`. In an SMF the
  length byte comes between them.

Constants observed in every file of their role and written as observed: the Style element `role`/`scenenum` pairs
(`Intro1` 4/1, `Intro2` 5/2, `CountIn` 11/3, `Var1-4` 0-3/4-7, `Fill1-2` 6-7/8-9, `Fill3-4` 13-14/10-11, `Break`
10/12, `End1-2` 8-9/13-14, `End3` 12/15; roles agree with the findings §2.1); the track clip number per clip name
(`Bass.pdc` 1, `Drum.pdc` 2, `Perc.pdc` 3, `Acc1-5.pdc` 4-8, `Chords.csc` 10); the TrackRole set (track n → role n-1,
n = 1..10); the KBD Set parts `Lower/Upper1-3.plc` → tracks 1-4; the clip types and releases (`PlainClip` r4,
`StyleClip` r7, `ChordClip` r4). The XML declaration is
`<?xml version="1.0" encoding="utf-8" standalone="yes"?>` in 71 273/71 273 leaves. Division is 384 PPQ throughout.

## Write rule (W3)

1. A node or element is written only if it was present in every observed file of that version and role.
2. An element is written only if every attribute it ever had held one constant value in every file. It is then
   written with exactly those values: format constants such as `EQSetting trim="99"` or `ReTrigger mode="3"`.
3. Link values are node names of the tree being built. Role, scene and track numbers are the per-name constants above.
4. A `TimeSignature` element present in every file of its role is written as 4/4 in the observed encoding
   (numerator 4, denominator 2 = log2, the same encoding as SMF meta 0x58; 4/2 is the most frequent observed pair).
5. Everything else is **omitted and listed as UNKNOWN** (`build::unknownOmitted()`, printed by
   `aiarranger-resources kst-native demo`):
   - Korg SysEx blocks.
   - Controller events after the Program Change.
   - Every XML parameter whose value varies between owner files: tempo, chord tables, loop lengths, sync modes,
     keyboard processing, track routes, the StyleClip parameters and LimitKeyRange (`acc5` was missing in 1 of 177).
6. The MIDI leaves hold only standard SMF events. They have time signature, optional tempo and key signature meta
   events in the observed order, then CC0, CC32 and Program Change on channel 1, the notes, and End of Track.

Schema check (`test_kst_native_writer`): every node path, element path and attribute name of the three builders is
in the observed name sets (81 node paths, 501 element paths, 973 attribute names over the three versions). An
invented attribute or node is caught.

## W4 closure, W5 assembler

- `kst_closure.h`: an original port of the research closure rule: MSB 121/120 with LSB 64-75 →
  `SOUND/USER(DK)nn.PCG` directory slot = Program Change. On the owner copy (private run, counts only):
  4 097 trees, 231 087 MIDI leaves, **32 156 resolved / 352 dangling** (320 slot empty, 32 file absent),
  179 126 factory/GM, 0 problems. This is the research result, reproduced. (Measured at `8f94627`, before the Codex r2 rules.)
- SMF rules of the closure reader (SMF 1.0 only; `selects()`): format 0 with exactly one track, or format 1; only
  `MTrk` chunks; End of Track at every track end; no trailing bytes. Channel-message data bytes must be < 0x80.
  Meta and SysEx events cancel running status. Bank state (CC0/CC32 per channel) is shared by all tracks. Events of
  all tracks are merged by absolute tick. **Tie rule:** at equal ticks the lower track number comes first, then file
  order within a track. A Program Change without both CC0 and CC32 set before it is **UNRESOLVED**. It is counted
  apart from factory/GM, and the assembler refuses it.
- Re-measured with these rules (Codex r2, counts only, 4 443 OK before and after): **32 152 resolved / 352
  dangling** (320 slot empty, 32 file absent), 179 126 factory/GM, **0 unresolved, 4 problems**.
  - The 4 problems are 4 Pad `Main.mid` leaves (3 objects, one of them also in a copied bank). Each has a CC11 in
    track 1 at tick 24 with a data byte ≥ 0x80, which is invalid under SMF 1.0. They are refused, not guessed at.
  - Each of these leaves holds exactly one user sound select. That accounts for resolved 32 156 → 32 152.
  - The cross-track merge and the unresolved category changed no other count. In the owner copy every bank select
    precedes its Program Change in the same track.
  - The owner-gated test pins `AIARR_KST_OWNER_EXPECT=32152/352/0/4` (resolved/dangling/unresolved/problems).
- `kst_assemble.h`: writes `<Name>.KST/` with only the type folders that hold data, one bank sub-folder and
  lower-case `.stg/.ksc/.pdc`. Optional KORF files are carried byte-for-byte after `panative::parse` accepts them.
  It refuses an existing path or manifest and any `/Volumes` target before the first write. Everything is staged in
  `NAME.KST.INCOMPLETE/`. There the files are read back (byte-compared and re-parsed), then checked: closure must
  report 0 dangling, 0 unresolved and 0 problems, the validator 0 errors. The SHA-256 manifest is written and verified there too.
  Only then are the manifest and then the folder published, by non-replacing renames. A failure never leaves a
  final-named KST.
- CLI: `aiarranger-resources kst-native closure KSTDIR` (read-only) and `kst-native demo OUTDIR` (the three W6 test
  KSTs: Pad, KBD Set, Style; GM2 sounds MSB 121/120 LSB 0, no samples).

## Open (blocks L3)

1. Does the Pa5X load objects without the SysEx blocks and without the varying parameters? This is W6 (owner, fresh
   USB stick).
2. The SysEx parameter block layout (track volume/pan/FX), needed to set anything beyond the sound.
3. Defaults the Pa5X assumes for omitted elements: chord table, sync, loop and tempo of a Style.
4. Whether a Style needs more than the Drum track, or tracks in every element, to be accepted.
