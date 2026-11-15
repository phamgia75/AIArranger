# Pa5X SET -> KST: the rules the phase-1 converter uses (evidence, counts, limits)

Code: `src/media/panative/set_to_kst.h`, `jobs::setToKst` (`src/jobs/export_specs.h`). Tests:
`tests/style_edit/test_set_to_kst.cpp` (generated fixtures; `--oracle` = the private run below).

This page is sanitized: structure and counts only. It holds no owner file, item, bank or Style names, no bytes and no
hashes. The measurements come from owner-held research copies outside this repository (read-only; not distributed).
Status of every rule: **STRUCTURALLY VALID - NOT VERIFIED ON A PA5X** (no KST written by this code has been loaded on a
keyboard).

Evidence classes, as in the research notes: **A** = holds in all measured cases, with the mechanism visible in the
bytes; **B** = observed, mechanism partly visible; **C** = documented or inferred only; **U** = unknown.

## Measured pair
One owner Pa700-family SET folder (142 files) and the KST that a Pa5X itself made from it with its own SET conversion
(885 files, holding a `ConversionReport.txt`). This is the only measured pair: no second model's SET has been checked.

| SET type folder | files | what the Pa5X wrote |
|---|---|---|
| `PCM/*.PCM` | 99 | `PCM/*.pcm`, 99 files, each byte-identical to its SET file |
| `SOUND/*.PCG` | 4 | `SOUND/*.pcg`, 4 files byte-identical (one still has the older format-0 directory) |
| `MULTISMP/*.KMP` | 1 | `MULTISMP/*.kmp`, byte-identical (its item payload is OC31-encoded) |
| `SONGBOOK/*.SBL` | 1 | `SONGBOOK/*.sbl`, byte-identical |
| `STYLE/*.STY` | 12 (486 Styles) | 486 Root-tree `.stg` files in 12 named bank folders |
| `KEYBOARDSET/*.PRF` | 9 (280 Sets) | 280 `.ksc` files in 9 named bank folders |
| `PAD/*.PAD` | 1 (4 Pads) | 4 `.pdc` files |
| `SONGBOOK/SONGDB.SBD/*.SBD` | 9 | the same names with new bytes (KORF, re-written) |
| `GLOBAL/*` | 5 | nothing |
| `VOICEPRESET/*` | 1 | nothing |
| `GUITARPRESET/` | 0 (empty folder) | nothing |

## Rules
| # | Rule in the code | Evidence | Class | Limits |
|---|---|---|---|---|
| R1 | CARRY: `PCM/*.PCM`, `SOUND/*.PCG`, `MULTISMP/*.KMP`, `SONGBOOK/*.SBL` are copied byte for byte | 99/99, 4/4, 1/1, 1/1 identical in the measured pair | A (this pair) | one pair; a Pa5X-era SET is not measured |
| R2 | CARRY destination: the same upper-case type folder, the file name lower-cased (`PCM/RAM01.PCM` -> `PCM/ram01.pcm`) | 105/105 carried files in the pair | A (this pair) | names are matched exactly in the observed upper case; other spellings are UNKNOWN |
| R3 | BLOCKED: `STYLE/*.STY`, `KEYBOARDSET/*.PRF`, `PAD/*.PAD`, `SONGBOOK/SONGDB.SBD/*.SBD` are not written at all | the Pa5X re-writes them (table above); their item payloads are OC31-encoded, which is not decoded | A (re-written), U (encoding) | where the Pa5X's bank folder names come from is U |
| R4 | DROPPED: `GLOBAL/`, `VOICEPRESET/`, `GUITARPRESET/` | not in the Pa5X's KST; consistent with the Pa5X manual (MAN5 p.737) | A (GLOBAL, VOICEPRESET), C (GUITARPRESET: an empty folder in the measured SET, so the manual only) | |
| R5 | UNKNOWN: anything else is listed and not carried | no other member occurs in the measured SET | - | a rule for any other file would be a guess |
| R6 | PCM sample = one directory record of a PCM file; name = directory field 2; sample ID = directory field 4 (8 bytes) | Pa5X KST backup: field 4 in 34 339/34 339 PCM records, equal to the 8 bytes at item payload +8 | A | Pa700 PCM records have no field 4 (0 of the 535 samples in the measured SET): their links are U |
| R7 | KMP -> PCM: the KMP item's sample table, read only in this layout: item chunk id `11080018`, raw (not OC31), u16 `0x0103` at +0, u32 sample count at +2, table of fixed 122-byte records from item offset 85, sample ID at record +40; the table must fit in the item | Pa5X KST backup: one KMP file, one item; count 34 339 = the PCM sample count; each of the 34 339 PCM sample IDs occurs once in the table | A (table and IDs), U (version meaning, multisample/zone section) | one KMP file observed; any other version, an OC31 item or a table past the item -> UNKNOWN, and then no sample is reported unreferenced or dangling |
| R8 | A CARRY file must be a valid KORF container (wrapper, header, directory, index as in `src/media/panative/panative_reader.h`) | 142/142 files of the SET; 281/281 KORF files of a Pa5X KST backup | A | the reader's limits apply (64 MiB per file) |

## Data flow (no check-then-use window, Codex r1 + r2)
1. **Pin.** The SET folder is opened once without following a link. Every member is then reached below that descriptor,
   folder by folder, without following a link, and the file must be a regular file. A SET that holds any link or
   special file is refused. The name-based listing must have listed the pinned folder (same device and inode).
2. **Snapshot.** Each CARRY member is read exactly once into memory, with its SHA-256 and its identity (device, inode,
   size, modification time; the read fails if the identity changes while the file is read). Total bound:
   2 GiB (`kMaxSnapshotBytes`); a larger SET is refused with that reason. The destination is checked before anything
   is read. When the job ends in any state (done, refused, failed, cancelled), the bytes are released: the job queue keeps
   the job (and its hashes and identities) for its list, not the bytes. The KORF check, the dependency map, the
   CONVERSION report and the staged write all use these bytes. No source is read again for content (a test counts the
   reads: one per CARRY member).
3. **Write.** The staged files are created exclusively below the job's pinned output folder. Each file's device and
   inode are recorded from the descriptor that wrote it.
4. **Verify.** The staged folder is pinned (`library::Pinned`): each file hashed through the descriptors. The hashes
   must equal the snapshot's, and the identities must equal the recorded ones. The W5 closure and validator checks run on it.
5. **Publish gate**, in this order, right before the no-replace rename:
   1. source identity: the SET pathname still names the pinned folder (device, inode), and every CARRY member still has
      its snapshot identity (no content re-read); then the SET pathname is compared with the pinned folder again;
   2. last: `library::recheckPinned` re-hashes every staged file through the pinned folder and compares hashes and
      device/inode with the verified ones. (This step was extended once in the shared code; Library Manager Execute Plan uses it too.)

**Residual boundary.** After the publish rename, a concurrent process of the same user can still change the published
files or the SET. The report states what was verified at publish time: the SET was still the folder that was read,
every carried member was unchanged, and every staged file was re-hashed. It cannot vouch for later changes.

The same boundary covers the last window inside the gate: a staged file that `recheckPinned` has already hashed
could be replaced by a same-length file before the rename (Codex r3 E1). The verdict on that window (no change made):
- (a) Who writes into the staging folder `NAME.KST.INCOMPLETE/`? Only the job, which creates it fresh beside the
  output. No other AIArranger code writes there. Ordinary macOS services read but do not rewrite files there
  (Spotlight indexing, Time Machine). Finder writes a `.DS_Store` only if someone opens the folder; a sync client in a synced
  parent may add a conflict copy. Both are new entries, and the re-walk refuses them as unexpected. Replacing a hashed
  file with other same-length bytes needs a deliberate concurrent process of the same user. That process could just
  as well change the output one instant after the publish, which is the boundary above. A further
  check-then-use layer would only move the window.
- (b) Read-only staging files (0444) plus a folder-identity check: not added. Replacing a file is an unlink/rename in
  the folder, which the folder's permission governs, not the file's. A same-user process can also restore any mode it
  likes. A read-only staging folder would also block the job's own publishing rename. The folder identity (device and
  inode, reached from the pinned parent) is already checked by `recheckPinned` and by the runner right before the rename.
  So the hardening is not trivial and does not stop the process it would be meant for.

## Private oracle (counts only)
`test_set_to_kst --oracle` (CTest `test_set_to_kst_oracle`, label private-corpus; Skipped without
`AIARR_SET_ORACLE_SET` and `AIARR_SET_ORACLE_KST`) converts a local copy of the SET and compares the result with the
Pa5X's KST: every output file must be byte-identical to the Pa5X's file at the same path, and every Pa5X file that
equals a SET member must be produced. Result on the measured pair: PCM 99, SOUND 4, MULTISMP 1, SONGBOOK 1 (105)
identical; 0 missed.

## Not emulated
The Pa5X writes `ConversionReport.txt` inside its converted KST; its format is not documented, so the converter writes
its own report beside the KST (`NAME.KST.export.md` / `.json`), never inside it.
