# Real-world data readiness: user-owned VST libraries and KST datasets

Date: 2026-11-08. Scope: what the tools accept today, and how to run them on real data without risk to it.
No tool here writes to the Pa5X, sends MIDI, or modifies source data.

## 1. Provenance and permission (before any tool runs)

- Only material the owner owns or has written permission to process. Third-party KST datasets: only within the
  existing authorization (`docs/research/KST_DATA_PROVENANCE_AND_RIGHTS.md`).
- Record for each dataset:
  - origin
  - licence/permission
  - who supplied it
  - date
  - SHA-256 manifest (step 3)
- Research KST files, renders of commercial libraries, and private screenshots are never committed to the
  public repository.

## 2. Supported inputs

| Input | Tool | Accepted | Refused with a message |
|---|---|---|---|
| AU instrument | `aiarranger-plugin-sampler` | Any installed AU instrument the owner may use; mono/stereo; up to 24 layers | Plugin not found or not an instrument |
| Folder of WAVs | `aiarranger-sound wav --in DIR --name N` | 16-bit PCM, 1–2 channels, one forward integer loop (play count 0), `smpl` root matching `<layer>_<OrgKey>_<LowKey>_<HighKey>.wav`, ≤ 16 MB per file | Other bit depths, more than 2 channels, extra or fractional loops, odd data length, missing `smpl`, root/name mismatch, oversize |
| KST folder | `kst-inventory`, `kst-diff`, `kst-merge-plan`, `kst_copy.py` | Any folder; treated as files only | Unreadable entries, symlinks (copy), existing destination |
| VST3 | — | Not supported (AU host only) | — |
| Kronos KSF/KMP | — | Not supported (no material, see `docs/research/KRONOS_KMP_KSF_IMPORT.md`) | — |

## 3. Safe workflow for a KST dataset

```
python3 -I tools/kst_copy.py ORIGINAL WORK        # 1 verified copy; manifest written next to WORK
python3 -I tools/kst_copy.py --verify WORK        # 2 re-check any time
kst-inventory WORK                                # 3 read-only report: types, sizes, duplicates, PCM folder
kst-diff WORK_A WORK_B                            # 4 byte-level comparison
kst-merge-plan WORK_SRC WORK_DST                  # 5 what a Pa5X Merge / Replace All would do
```

- **Copy.** Always work on a copy. `kst_copy.py` copies to a temporary folder first and re-hashes every file
  against its source. It then re-hashes the source to prove it did not change, and only then renames the
  copy into place.
  - It never overwrites.
  - It refuses an existing destination, symlinks, and a destination inside the source.
- **Read-only.** Every tool opens files read-only; none parses a KST file format.
- **Exit codes.**
  - `kst-diff`: 0 = same, 1 = different or something skipped, 2 = scan error.
  - `kst-merge-plan`: 0 = Replace All safe, 1 = not safe, 2 = scan error.
  - A scan error never reads as success.

## 4. Safe workflow for a VST/AU or WAV library

1. Render or collect into a new folder. The tools refuse an existing output folder.
2. `python3 -I tools/validate_sound_package.py OUT` (or `validate_drumkit_package.py`).
3. Optionally, a FluidSynth check through the SF2 export.
4. Read `REPORT.txt`. It lists what the Pa5X imports and what has to be set by hand. Sound Recipe values are
   labelled Extracted / Measured / Estimated / NotAvailable; nothing Estimated is presented as verified.
5. Pa5X import only through S6 with owner authorization.

## 5. KST management capability levels

| Level | Meaning | Status |
|---|---|---|
| 1. Byte-level file comparison | Same/different/added/removed per file; content match across renames (size → hash → bytes) | **Built**: `kst-diff` (26 tests, Codex-reviewed) |
| 2. File-level merge planning | Predicts the Pa5X Merge (same name replaced, MAN p.734) and Replace All (type deleted, p.734); flags a PCM folder that wipes User Samples (p.736) and name-case collisions (Pa5X case rule UNK) | **Built**: `kst-merge-plan`, labelled "FILE LEVEL ONLY" |
| 3. Resource dependency analysis | Which Sound uses which multisample/sample; what breaks if a file is removed | **Not available**: needs the KST internal format, which is not authorized for reverse engineering |
| 4. True Sound-aware merge | Merge Sounds and remap their sample references without loss | **Not available**: depends on level 3 and on Pa5X-side behaviour; do not claim it |

Level 2 is advice about file names, not about Sounds. Two files with different names can still share samples,
and a "safe" plan can still change which samples a Sound plays. Level 3 would be needed to know that.

## 6. Known limitations

- **Duplicate detection race.** `kst-inventory` treats a file that becomes unreadable between hashing and the
  byte comparison as "not a duplicate". This can under-report, but it never deletes anything. Files that are
  unreadable at hashing time are listed as skipped.
- **Name case.** How the Pa5X compares name case is UNK. Case-only differences are flagged, never resolved.
- **Exclusive groups.** Drum Kit exclusive groups are limited to TX16Wx's 1–8; the Pa5X allows 1–127
  (MAN p.1099).
