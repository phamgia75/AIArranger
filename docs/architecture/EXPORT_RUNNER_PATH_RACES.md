# Export runner: path races, what is prevented and what is only detected

Scope: `src/jobs/export_job.h` (runner), `src/media/output_guard.h` (`media::pinned`), the writers it calls.
Threat: another process of the same user changes a folder name (rename, symlink swap) between a check and a use.
Not claimed: complete race freedom. Each row says PREVENTED (the OS operation cannot be redirected) or DETECTED (found
afterwards, job fails, but bytes may already have been written).

## Mechanism
1. The destination's nearest existing folder (the *anchor*) has its device+inode taken before the guards run.
2. The runner opens the anchor once, requires the same identity, makes missing folders below it with `mkdirat` +
   `openat(O_NOFOLLOW|O_DIRECTORY)`, and checks the opened folder by identity against every source (walking `..`) and at
   its real location (`F_GETPATH` / `/proc/self/fd`; unknown = refuse).
3. The job folder `NAME.INCOMPLETE`, the marker, the staged output, side-file moves, the publish rename, the report files
   and the leftover move-aside all use that handle.
4. Path-taking writers (`ExportSpec::write`) run with `media::pinned::Scope`: every creating helper
   (`makeFolder(s)`, `createNewFile`, `writeNewFile`, `publishNewFolder`, `FileSet` root) works relative to the pinned job
   folder and refuses any path not strictly below it. `writeAt` writers get the staged folder's fd directly.
5. The pinned folder is re-checked (identity vs sources, real path unchanged) before the write, after it, before publish
   and before the report.

## Table
| Operation | macOS | Linux | Class |
|---|---|---|---|
| open parent / ancestor swapped before the single open | identity + real-path check | same | PREVENTED (refused before any write) |
| create missing folders, staging, marker, side files, report | `*at` calls on the handle | same | PREVENTED |
| writer (`write`) creating below the job folder | `media::pinned` handle-relative | same | PREVENTED; paths outside the job folder refused |
| the pinned folder itself renamed (e.g. into a source) while the job runs | folder keeps being written (the handle follows it) | same | DETECTED after the write / before publish; bytes already written stay in the renamed folder (only `NAME.INCOMPLETE`), nothing published |
| final rename of the staged output | `renameatx_np(RENAME_EXCL)`: no rename-by-inode; a swap between the identity check and the rename is found by a second identity check after it and moved back | `renameat2(NOREPLACE)`, same limitation | DETECTED; move-back can fail (then the report says the entry stays published) |
| marker / temp-file cleanup | `unlinkat` after an identity check; no conditional unlink exists | same | DETECTED/best effort: a swap between check and unlink can delete the substitute |
| side files then output (multi-rename publish) | not atomic as a unit; rollback is a no-replace rename | same | DETECTED; a side file that cannot be moved back is reported as still published |
| `verify`, `beforePublish`, manifest reads by path | read-only; the staged output's real path is re-identified after | same | DETECTED (a substituted tree could be read; the identity check then fails the job). No filesystem mutation happens in these callbacks |
| hostile process that may write inside the authorized folder itself | not in scope (same class as D16 C1) | same | NOT PROTECTED |

## Export types (all go through the runner)
`writeAt` (handle given): Import Kit, SET to KST. Pinned path writers (`write`): Sound package, Drum Kit, Drum Kit KMP,
Drum Kit from folder, KMP/KSF folder, Sample Lab batch/slice/loop, Style SMF, Style UASF, KST backup, rendered package
copy, library plan, library apply, report. Tests: `test_export_path_race`, `test_export_writers_race`, and the race
blocks in `test_library_apply` and `test_report_export`.
