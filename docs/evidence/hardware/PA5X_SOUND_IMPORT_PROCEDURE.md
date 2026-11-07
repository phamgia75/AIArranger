# S6: import the AIArranger demo Sound on the Pa5X — preflight and hardware gate

Roadmap `docs/SOUND_CONVERSION_ROADMAP.md` S6. Proves the success criterion on a real Pa5X: an original sound, converted, packaged, loaded and played.

**Status:**
- Preflight is done on the Mac (§1).
- Owner checks O1–O5 (§2) are pending.
- **No write to the Pa5X is authorized.** Exactly **one** attempt follows the owner's explicit approval of the gate packet (§3): TX Sound (.txprog) import only. After a failure there is no SF2 attempt, no second destination and no retry.

Page numbers: **MAN** = Pa5X User Manual v1.4 (printed); **GUIDE** = Korg's *Making Pa5X Sounds and Drum Kits with TX16Wx* (printed).

## 1. Preflight (Mac only, done 2026-11-07)

| # | Check | Result |
|---|---|---|
| P1 | Generator | `build/aiarranger-sound demo --out DIR` at commit `9e13f86`. Two runs are byte-identical (`diff -r`) |
| P2 | Originality | The demo path reads no input files. The audio is a sum of sine harmonics (`src/media/sound/additive_source.h`). Each WAV holds only `fmt `, `data` and `smpl` chunks (no metadata from elsewhere) |
| P3 | Independent validation | `tools/validate_sound_package.py`: PASS (FluidSynth pitch worst 0.004 cent; Python XML/WAV re-read; each WAV's `smpl` root/loop equals the `.txprog` root/loop). A deliberately altered copy FAILs |
| P4 | Package structure | Follows GUIDE pp.9, 16, 31, 44-45 (table below) |
| P5 | Staged USB content | Mac folder `~/orca/workspaces/AIArranger/pa5x-import-stage/USB_ROOT/`, copied with `tools/kst_copy.py` and verified (`--verify`) |

Staged content (this is the **whole** stick content, nothing else):

```
USB_ROOT/
  AIA Demo Organ/                        39 files, 1 633 654 bytes
    AIA Demo Organ.txprog                sha256 3bda6474d936cddc1759408ae24cc4c2654183a8d6129b893600f6500cae615e
    AIA Demo Organ Samples/              38 WAV, mono 16-bit 48 kHz, looped
      AIA OrgS C1.wav, AIA OrgS E1.wav, ... AIA OrgS C7.wav   (Low layer, 19 files, roots every 4 semitones)
      AIA OrgB C1.wav, ... AIA OrgB C7.wav                    (High layer, 19 files)
      ('#' in note names is written '_' in file names, e.g. AIA OrgS G_5.wav = G#5)
  AIA Demo Organ.manifest.sha256         per-file SHA-256 of the folder above
                                         (file sha256 d86ff79b80c9201801b44c93fa8f44e5d81bf091bfb4905b1b1ef9bc660d15c5)
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

1. **Fields:** the Pa5X may need `.txprog` attributes our writer leaves out. Our file follows the structure of Korg's example but omits some attributes (region `mode`, `release`, most soundshape fields). The likely symptom is an import error.
2. **Path separator:** `/` in wave paths. Korg's drum examples use it; its piano example uses `\`.
3. **Loop source:** whether the loop and root are read from the `.txprog`, the WAV `smpl` chunk or both. Both say the same thing (P3), so either should give the same result.
4. **Imported names:** whether samples are named after the WAV file (then `G_5` for G#5) or the `.txprog`, and whether a 20-character Sound name is accepted as is (the dialog allows editing, MAN p.1210).
5. **Oscillators:** GUIDE p.14 says each Multisample goes to its own Oscillator (High Layer typically used). We therefore expect two Oscillators split by velocity, not one Oscillator's Low/High layers. Either plays the same.
6. **Name clashes:** what happens if a sample or multisample with the same name already exists. O3 rules this out.
7. **Envelope:** the release (0.1 s preview) and other envelopes are not imported (GUIDE p.85). The Pa5X default envelope applies.

## 5. Report back (text)

Route used (TX only), any message, the Sound location, OS version, Memory Info before and after, and the play table results. **PHYSICAL HARDWARE PASS** only when the owner reports the import succeeded and every play check passed.
