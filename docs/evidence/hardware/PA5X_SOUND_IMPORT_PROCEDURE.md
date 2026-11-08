# S6: import the AIArranger demo Sound on the Pa5X — preflight and hardware gate

Roadmap `docs/SOUND_CONVERSION_ROADMAP.md` S6. Proves the success criterion on a real Pa5X: an original sound, converted, packaged, loaded and played.

**Status:**
- Preflight is done on the Mac (§1).
- Owner checks O1–O5 (§2) are pending.
- **No write to the Pa5X is authorized.** Exactly **one** attempt follows the owner's explicit approval of the gate packet (§3): TX Sound (.txprog) import only. After a failure there is no SF2 attempt, no second destination and no retry.

**Transport boundary.** Files reach the Pa5X **only** on a USB storage device, through the Pa5X's own Sample Edit > Import page. The Mac ↔ Pa5X USB MIDI link carries performance MIDI; it is not a file path and is never used for Sound data. The Pa5X's internal-drive access from a computer (MAN p.797) is not used either. Copying to a physical USB device also waits for the owner's authorization.

Page numbers: **MAN** = Pa5X User Manual v1.4 (printed); **GUIDE** = Korg's *Making Pa5X Sounds and Drum Kits with TX16Wx* (printed).

## 1. Preflight (Mac only, done 2026-11-07)

| # | Check | Result |
|---|---|---|
| P1 | Generator | `build/aiarranger-sound demo --out DIR` at the commit that adds this preflight (the `.txprog` writer change after `9e13f86`). Two runs are byte-identical (`diff -r`) |
| P2 | Originality | The demo path reads no input files. The audio is a sum of sine harmonics (`src/media/sound/additive_source.h`). Each WAV holds only `fmt `, `data` and `smpl` chunks (no metadata from elsewhere) |
| P3 | Independent validation | `tools/validate_sound_package.py`: PASS (FluidSynth pitch worst 0.004 cent; Python XML/WAV re-read; each WAV's `smpl` root/loop equals the `.txprog` root/loop). A deliberately altered copy FAILs |
| P4 | Package structure | Follows GUIDE pp.9, 16, 31, 44-45 (table below) |
| P6 | Structure vs Korg's example | Our `.txprog` has the same element paths, attribute names and top-level order (wave, bounds, soundshape, group) as Korg's example Sound `Acoustic Piano.txprog`. Including `tx:created-by="30601"`, the format stamp carried by every Korg example and also written by the open-source ConvertWithMoss TX16Wx writer. No structural difference remains. Compared locally; Korg's file is not in the repo |
| P7 | Stage matches the current generator | 2026-11-08: `build/kst-diff` of the staged folder against a fresh `aiarranger-sound demo` build at `434e207`: 39 identical, 0 changed/added/removed |
| P5 | Staged USB content | Mac folder `~/orca/workspaces/AIArranger/pa5x-import-stage/USB_ROOT/`, copied with `tools/kst_copy.py` and verified (`--verify`) |

Staged content (this is the **whole** stick content, nothing else):

```
USB_ROOT/
  AIA Demo Organ/                        39 files, 1 641 662 bytes
    AIA Demo Organ.txprog                sha256 21a2936cb53ce2b9cd8f1f22c77817641e3bafecfe381bd75d6b48d8bbf083cb
    AIA Demo Organ Samples/              38 WAV, mono 16-bit 48 kHz, looped
      AIA OrgS C1.wav, AIA OrgS E1.wav, ... AIA OrgS C7.wav   (Low layer, 19 files, roots every 4 semitones)
      AIA OrgB C1.wav, ... AIA OrgB C7.wav                    (High layer, 19 files)
      ('#' in note names is written '_' in file names, e.g. AIA OrgS G_5.wav = G#5)
  AIA Demo Organ.manifest.sha256         per-file SHA-256 of the folder above
                                         (file sha256 81c1be0b97b9c2a15ca4ee6daf96d282aa76f0a3b0112d08d5cc564c140a88d0)
```

The `.txprog` refers to samples as `AIA Demo Organ Samples/<name>.wav`, relative to its own folder, so the samples folder sits inside the project folder as GUIDE p.31 requires. On the USB stick, verify with `python3 -I tools/kst_copy.py --verify "/Volumes/<STICK>/AIA Demo Organ"`.

Package vs the guide's rules:

| Rule | Package |
|---|---|
| One Program → one Sound; Program name → Sound name (GUIDE p.24) | Program `AIA Demo Organ` |
| Group = Multisample on its own Oscillator, limited to one velocity zone; zones adjacent, no gap (GUIDE pp.14-16, 44) | Group `AIA Organ Soft` vel 0-89, Group `AIA Organ Bright` vel 90-127 |
| Regions cover every key, no gap or overlap (GUIDE p.45) | 19 Regions per Group, keys 0-127 tiled |
| Region Lo/Hi V → Velocity Zone, Root → Original Note, loops kept (GUIDE pp.24-27, 84) | velocity on each Region; root and forward loop on each wave |
| Samples within the Pa5X limits (MAN p.1155) | 16-bit, 48 kHz, at most 24 803 frames each |

## 2. Owner checks before the gate (on the Pa5X, read-only)

| # | Check | How | Record |
|---|---|---|---|
| O1 | OS ≥ 1.3.0 (TX16Wx import, MAN p.1216) | FILE > page menu > System Info (MAN p.785) | OS version |
| O2 | Target User Sound location is **empty** | Sound Select window, User banks: pick a location shown as empty | bank + number |
| O3 | Enough User Sample memory, and no existing `AIA…` samples or multisamples | Sample Edit > Main > Memory Info (MAN p.1174); the Sample and Multisample Select lists | free memory; "no AIA names" |
| O4 | **Backup** of all User data, completed | FILE > Internal > All > page menu > Save, into a **new, blank KST folder** on a **separate** USB device (MAN pp.7, 789-791). Do not touch the saved folder afterwards | backup folder name, device, date; that the save finished without error |
| O5 | Photos for the record | Memory Info screen and the target location, before import (transcribed as text, never committed) | text |

Restoring the backup (MAN p.7): FILE > Drives > the KST folder > Load, **selectively**. Loading a whole KST replaces User data, including the User Samples (MAN p.736).

## 3. Hardware gate packet (for the owner's explicit approval)

| Item | Value |
|---|---|
| Import source | `AIA Demo Organ/AIA Demo Organ.txprog` on a USB stick that holds only the staged content above (hashes in §1) |
| Method | Sample Edit > Menu > Import/Export > Import (MAN p.1207), pop-up **TX Sound (.txprog)**, Path > Select the `.txprog`, Execute (MAN p.1210; GUIDE pp.87-88) |
| Destination | The empty User Sound location from O2. Name kept as `AIA Demo Organ`. **Compression off** |
| Required free User Sample memory | About 1.64 MB (the 38 WAVs; 16-bit, uncompressed). O3 records the free memory before import |
| Resources created (DOC MAN p.1206 "Importing it will also import all the linked samples", GUIDE p.88 "together with the Multisample(s) … and the linked Samples") | 1 User Sound; 2 Multisamples (`AIA Organ Soft`, `AIA Organ Bright`, GUIDE p.24: Group name → Multisample name); 38 Samples; about 1.6 MB of User Sample memory (stored permanently, MAN p.1207) |
| Not touched | Other Sounds, Styles, Keyboard Sets, Global/MIDI settings, existing samples and multisamples. No KST load, no MIDI from the Mac, no compression, no Pa5X internal-drive access from a computer (MAN p.797). The USB stick is the only path |
| Expected result | One import dialog, then the Sound in the chosen location. Play C2-C6 soft and hard: steady organ tone, correct pitch, brighter above velocity 90, no click while held |
| On failure | Record the exact message and **stop**. No SF2, no other location, no retry. Then run the recovery for whatever was created |
| Backup | O4 confirmed by the owner, with the folder name |

### Recovery (all documented)

Run these in order. Each step removes only the `AIA…` items:

1. **Sound:** Sound Select window (or FILE pages) > select the imported User Sound > page menu > Delete > OK (MAN p.778: User data can be deleted in the Select and File pages).
2. **Multisamples:** Sample Edit > Multisample section > page menu > Delete > **Selected [n]**, once for `AIA Organ Soft` and once for `AIA Organ Bright` (MAN p.1190). Leave **Delete Unassigned Samples** unchecked, and do **not** use "Not assigned to any Sound": both can remove unrelated samples.
3. **Samples:** Sample Edit > Main > Sample Select window, SHIFT-select only the `AIA Org…` samples, Delete in the title bar, confirm (MAN p.1180).
4. Compare Memory Info with the O5 record.
5. Last resort only: selective load from the O4 backup (MAN p.7).

## 4. Uncertain assumptions (UNK until the import)

1. **Fields:** the structure equals Korg's example (P6); values differ where the content differs (names, keys, loops, 2 Groups instead of 1). Whether the Pa5X checks anything beyond the structure is unknown. The likely symptom of a mismatch is an import error.
2. **Path separator:** `/` in wave paths. Korg's drum examples use it; its piano example uses `\`.
3. **Loop source:** whether the loop and root are read from the `.txprog`, the WAV `smpl` chunk or both. Both say the same thing (P3), so either should give the same result.
4. **Imported names:** whether samples are named after the WAV file (then `G_5` for G#5) or the `.txprog`, and whether a 20-character Sound name is accepted as is (the dialog allows editing, MAN p.1210).
5. **Oscillators:** GUIDE p.14 says each Multisample goes to its own Oscillator (High Layer typically used). We therefore expect two Oscillators split by velocity, not one Oscillator's Low/High layers. Either plays the same.
6. **Name clashes:** what happens if a sample or multisample with the same name already exists. O3 rules this out.
7. **Envelope:** the release (0.1 s preview) and other envelopes are not imported (GUIDE p.85). The Pa5X default envelope applies.

## 5. Report back (text)

Route used (TX only), any message, the Sound location, OS version, Memory Info before and after, and the play table results. **PHYSICAL HARDWARE PASS** only when the owner reports the import succeeded and every play check passed.
