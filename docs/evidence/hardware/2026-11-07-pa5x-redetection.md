# Pa5X hardware: re-detection on the bench (2026-11-07)

**Result: PASS (detection only).** No MIDI was sent; no driver, FLOW 8 setting or system setting was changed. Playback, timing and sound remain **HARDWARE VALIDATION PENDING**.

- **Bench access:** SSH key-only (`BatchMode`, password and keyboard-interactive disabled) with `StrictHostKeyChecking=yes` against the owner-verified host key.
- **Bench repo:** the existing checkout and its three worktrees were clean and left untouched. The PR branch was fetched into a scratch ref (`refs/orca/pa5x-first`) and `pa5x-doctor` was built in a **new** detached worktree at `5858e1a` (target `pa5x-doctor` only).

| Time (local) | USB bus | CoreMIDI | `pa5x-doctor` |
|---|---|---|---|
| ~09:12 | FLOW 8 only, no KORG device (vendor 0x0944) | FLOW 8 source + destination only | exit 3, "Pa5X NOT ready" |
| ~09:42 | **Pa5X** (vendor 2372 = 0x0944) + FLOW 8 | see below | exit 0, "Pa5X found" |

```
Sources (2):
  [0] Pa5X KEYBOARD                uid=534315493   online  Pa5X KEYBOARD (Pa5X -> engine)
  [1] FLOW 8                       uid=1024724732  online
Destinations (2):
  [0] Pa5X SOUND                   uid=1907663670  online  Pa5X SOUND (engine -> Pa5X)
  [1] FLOW 8                       uid=2078839905  online
```

The uids equal those of Gate 0 (`2026-11-06-pa5x-gate0-detection.md`): the same device and CoreMIDI identities. No KORG USB-MIDI driver is installed (only two non-KORG MIDI plugins); the Pa5X enumerates without it.

**Endpoints for any future playback test:** destination `Pa5X SOUND` uid 1907663670 only; source `Pa5X KEYBOARD` uid 534315493 for receive-only checks. FLOW 8 is never a target. Sending anything needs the owner's separate authorization.
