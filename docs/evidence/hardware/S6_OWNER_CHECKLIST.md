# S6 owner checklist (one page)

Short form of `PA5X_SOUND_IMPORT_PROCEDURE.md` (the full gate packet: package details, page references,
recovery). **S6 stays BLOCKED** until every box in part A is filled and part B is signed. Nothing here is
sent over MIDI; the Mac never writes to the Pa5X.

**Ready on the Mac (re-checked 2026-11-11 at `611980a`):** the staged stick content
`~/orca/workspaces/AIArranger/pa5x-import-stage/USB_ROOT/` is byte-identical to a fresh
`aiarranger-sound demo` build (39 of 39 files) and verifies against its manifest
(`tools/kst_copy.py --verify`, `kst-backup --verify`). `.txprog` SHA-256 `21a2936c…83cb`.

## A. Read-only checks on the Pa5X (no changes)

| # | Check | Where (manual page) | Write down |
|---|---|---|---|
| 1 | OS version is 1.3.0 or later | FILE > page menu > System Info (MAN p.785) | OS version: ______ |
| 2 | One **empty** User Sound location | Sound Select > User banks (an empty slot) | bank ____ number ____ |
| 3 | At least **2 MB** free User Sample memory; no sample or multisample named `AIA…` | Sample Edit > Main > Memory Info (MAN p.1174); Sample and Multisample Select lists | free: ______ ; "no AIA names": yes / no |
| 4 | A backup of all User data **finished without error**, on a **separate** USB device, into a **new, blank** KST folder; the folder is not touched afterwards | FILE > Internal > All > page menu > Save (MAN pp.7, 789-791) | folder ______ device ______ date ______ |

Optional, only with separate permission: a second, verified copy of that backup on the Mac with
**Backup KST...** in Sounds & Resources (it reads the stick, writes a new folder, checks every file by SHA-256).

## B. Authorization (owner writes this, nothing proceeds without it)

> I authorize **one** TX Sound import of `AIA Demo Organ.txprog` from a USB stick holding only the staged
> content, into User Sound location ____ / ____, compression off. Checks 1-4 above are done. — date ______

## C. The import (owner, at the Pa5X)

1. Copy `USB_ROOT/` (only that) to an empty stick; on the Mac: `python3 -I tools/kst_copy.py --verify "/Volumes/<STICK>/AIA Demo Organ"` must say `verified 39 files`.
2. Sample Edit > Menu > Import/Export > Import (MAN p.1207) > **TX Sound (.txprog)** > select `AIA Demo Organ.txprog` > the location from A2 > compression **off** > Execute (MAN p.1210).
3. **Any error message: write it down exactly and stop.** No retry, no SF2, no other location. Then the recovery in the full procedure, §3.
4. Play C2, C4, C6 soft and hard: steady organ tone, right pitch, brighter above velocity 90, no click while a note is held.

## D. Report back (text only, no photos committed)

OS version · location · Memory Info before and after · the import message (or "none") · the play results.
**PHYSICAL HARDWARE PASS** only when the import succeeded and every play check passed.
