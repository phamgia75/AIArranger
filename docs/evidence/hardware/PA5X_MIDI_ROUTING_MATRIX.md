# Pa5X MIDI routing matrix (Phase C preparation)

Status 2026-11-07. Offline analysis only: **nothing in this document was sent to any MIDI device**.
Sources: Pa5X User Manual v1.4 (pages cited), the owner's Settings > MIDI photos of 2026-11-07 17:2x
(transcribed in `PA5X_MIDI_VALIDATION_SEQUENCE.md`, photos kept private), and the receive captures
of 2026-11-07 (`2026-11-07-pa5x-gate2-chords.md`). MIDI channels are 1-16; "index" is the 0-15 wire value.

Labels: **HW-CONFIRMED** = observed on the device; **PHOTO** = read from the owner's settings photos,
not exercised; **MANUAL** = documented, not observed; **ASSUMED** = engine/tool assumption.

## Pa5X → computer ("Pa5X KEYBOARD", MIDI OUT page)

| Part | Manual default (p.585-588, 605) | Preset "Ai Arranger" | Evidence | Engine use |
|---|---|---|---|---|
| Upper 1 | ch 1 | ch 1 | HW-CONFIRMED (11-06, 11-07 captures) | ignored for chords |
| Upper 2 | ch 2 | Off | PHOTO | — |
| Upper 3 | ch 3 | Off | PHOTO | — |
| **Lower** | ch 4 | **ch 4** | **HW-CONFIRMED**: 34/34 chords on ch 4 (11-07). Receive is intermittent: 11:36 run 0 notes, open | chord detection (`defaultPresetKeyboardRoles`, ch 4 only) |
| Chord 1/2 track | Off | Off | PHOTO | not used |
| Clock / Start / Stop out | Clock Send | **Off** | HW-CONFIRMED: 0 F8 bytes (11-06) | not used |

Open: the ch 1 + ch 4 duplicates of C5/C6 seen at 11:31 are not explained by this page (no OUT channel for Upper 2/3).
**Do not assume ch 2 is Lower.**

## Computer → Pa5X ("Pa5X SOUND", MIDI IN page)

| Role | Pa5X receive channel ("Ai Arranger") | Evidence | What the engine emits today (identity output map) | Gap |
|---|---|---|---|---|
| Upper 1 | not assigned | PHOTO | — | — |
| Upper 2 | not assigned | PHOTO | — | — |
| **Upper 3** | **ch 1** | **HW-CONFIRMED** by ear 18:25:25 (one note, Gospel Organ). Pitch UNCONFIRMED | `pa5x-probe note --channel 1` only | — |
| Lower | not assigned | PHOTO | — | — |
| Bass | ch 9 (Ply.1 Tr 9 / STY Bass) | PHOTO; manual p.1035 | demo style: **index 1 = ch 2** (Off on the Pa5X); demo map → ch 9; Genos map: ch 11 → ch 9 | identity map: demo bass **silent** |
| Drum | ch 10 (Tr 10 / STY Drum) | PHOTO | demo style: index 9 = ch 10 | matches |
| Perc | ch 11 (Tr 11 / STY Perc) | PHOTO | Genos map: ch 9 → ch 11 | — |
| Acc 1-5 | ch 12-16 (Tr 12-16 / STY Acc1-5) | PHOTO | demo chord: **index 2 = ch 3** (Off); demo map → ch 12; Genos map: ch 12-16 → 12-16 | identity map: demo chord **silent** |
| Control (Pa5X arranger commands, Program Change) | **none assigned** (ch 2-8 Off) | PHOTO | `pa5x-probe` control commands | the Pa5X's own arranger cannot be driven with this preset |
| Chord 1/2 (Pa5X chord recognition input) | none assigned | PHOTO | — | — |
| Clock / Start / Stop in | Clock Source **Internal** | PHOTO | engine clock out exists, off by default (P0.6) | expected: the Pa5X follows its own clock with Source Internal (setting name; manual page not checked); not observed |

Notes
- "Ply.1 Tr 9-16 / STY …": the page shows both the Player 1 track and the Style track names for ch 9-16.
  Which one sounds while a Pa5X Style or Song is (not) running is **UNCONFIRMED**.
- Pa5X output maps in code (`src/midi/pa5x_profile.h`): `genosStyleTrackMap()` (Genos SFF1 styles) and
  `demoStyleTrackMap()` (demo style: bass ch 9, drums ch 10, chord ch 12 Acc1). Both are tested offline
  and **neither is wired into the app**; the app output map is identity unless the host sets one.
- Track Mute Active OFF, velocity Normal, all filters Off (PHOTO, 17:2x).

## Hardware checks still needed (each needs separate owner authorization)

| # | Check | Send | Target | Max | Cleanup |
|---|---|---|---|---|---|
| R1 | Pitch of the Upper 3 test note | 1 Note On/Off, ch 1, C4 | Pa5X SOUND | 1 s | CC123+CC120 ch 1 |
| R2 | Bass receive on ch 9 | 1 note, ch 9 | Pa5X SOUND | 1 s | ch 9 only |
| R3 | Drum receive on ch 10 | 1 note, ch 10 | Pa5X SOUND | 1 s | ch 10 only |
| R4 | Acc1 receive on ch 12 | 1 note, ch 12 | Pa5X SOUND | 1 s | ch 12 only |
| R5 | Repeat Lower receive (intermittent 11:36) | nothing (receive only) | Pa5X KEYBOARD | 180 s | — |

None of these has been run. FLOW 8 is never a target.
