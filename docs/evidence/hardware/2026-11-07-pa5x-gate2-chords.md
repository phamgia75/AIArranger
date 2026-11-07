# Pa5X Gate 2: keyboard reception and chord recognition (receive only), 2026-11-07

**Result: retry 12:07 = 4/4 PASS (see "Retry"); first attempt 11:36 = 4/4 BLOCKED. Nothing was transmitted in either.** Bench: Mac mini, `pa5x-probe` built from `fa780a2`, source `Pa5X KEYBOARD` (uid 534315493), `--channel any --split 60 --mode fingered`, parsed and `--raw` listeners in parallel. Pre-flight: `pa5x-doctor` KEYBOARD + SOUND online; no AIArranger / pa5x / korg-playback process; MusicAIApp, Music, Moises open (irrelevant to receiving, blocks Phase B).

## Captures

| Window | Parsed | Raw |
|---|---|---|
| 11:31:02-11:36:02 (300 s) | 20 note events: single notes C5 (72) and C6 (84), each sent twice, on **ch 1 and ch 4**, vel 86-101 on, 13-44 off; nothing below the split | 20 non-realtime packets (`93 54 56 90 54 56` = Note On ch4 + ch1 in one packet), FE x1180 |
| 11:36:08-11:46:08 (600 s), owner played the four chords below C4 | **0 note events** | **0 non-realtime packets**, FE x2381 (~4/s: link alive throughout) |

## Per test

| # | Played (owner) | Expected (AIArranger fingered) | Received | Verdict |
|---|---|---|---|---|
| 1 | C3-E3-G3 | C major, bass C | nothing | BLOCKED |
| 2 | E2-G2-C3 | C major, bass E (C/E) | nothing | BLOCKED |
| 3 | F1-A1-C2 | F major, bass F | nothing | BLOCKED |
| 4 | C3-E3-Bb3 (C7 no 5th; spec §4.4 partial, one tone missing) | C7, bass C | nothing | BLOCKED |

## Finding

The Pa5X transmits the upper-keyboard parts over USB (ch 1 and ch 4; two parts with MIDI OUT channels) but **not the lower part below its split point**. Hypothesis, not verified: the Lower part has no MIDI OUT channel in the current MIDI preset, so lower-hand chords reach only the Pa5X's own chord recognition. This is consistent with the 11-06 record (OUT ch1 = Upper 1, nothing for Lower) but the ch 4 sender is not in that record, so the preset photo is needed.

Consequence for AIArranger: with this preset the engine never sees the left hand. Live accompaniment needs either a Lower OUT channel (Pa5X setting, owner decision) or detection on the received upper notes.

## Unblock options (no setting changed by us)

1. Owner plays the same four chords **above C4**, listener with `--split 128 --channel 1` (all received notes count; per-channel held sets avoid the ch1/ch4 duplicate). Tests the detector, not the lower-keyboard path.
2. Owner assigns a MIDI OUT channel to Lower (Settings > MIDI > MIDI OUT), a Pa5X settings change needing the owner's decision; then rerun as specified.

## Tool fix

`pa5x-probe` output was block-buffered when piped, so events appeared only at exit and an early stop would lose them. Now line-buffered (`setvbuf(stdout, _IOLBF)`).

## Retry 12:07:53-12:12:53 (300 s), line-buffered probe `addd4bc`

Listeners: parsed `--channel 1 --split 128` (set up for upper-register playing) and `--raw --channel any`. The owner played the four chords in the **lower register** and the lower part **was transmitted this time, on ch 4** (status `0x93`/`0x83`); the cause of the empty 11:36 capture is not established (no Pa5X setting was changed by us; ask the owner whether anything changed on the panel). The parsed listener filtered ch 1 and so saw only right-hand single notes (G4, C5, B4 -> NoChord, correct for one note). The chords come from the raw capture; each was played twice (at ~0.2-36 s and ~60-81 s) with the same notes.

| # | Raw Note On (ch 4) | Notes | Fingered detector (engine code, replayed from these bytes) | Verdict |
|---|---|---|---|---|
| 1 | `93 24 15 93 28 1F` `93 2B 12` | C2 E2 G2 | Major, root C | **PASS** |
| 2 | `93 1C 0F 93 24 10` `93 1F 0F` | E1 G1 C2 | Major, root C. Fingered by design does not carry the bass (`test_chord_scan` "Fingered C/E 1st inversion"); Fingered-on-Bass gives C over E1 | **PASS** (inversion recognised as C; bass reported only in on-bass mode) |
| 3 | `93 1D 16 93 21 18` `93 24 12` | F1 A1 C2 | Major, root F | **PASS** |
| 4 | `93 24 15 93 2E 0D` `93 28 29` | C2 Bb2 E2 | Dom7, root C (spec §4.4 partial: fifth missing) | **PASS** |

Method, stated plainly: the notes and channels are hardware evidence. The chord names were computed **offline** by the same `FingeredDetector` / `FingeredOnBassDetector` code fed the captured bytes in arrival order through `HeldNotes` (as `listen` does), now a regression test in `tests/midi/test_pa5x_probe.cpp` (5 cases). A live parsed run with `--channel 4 --split 60` would show the same names on screen; not repeated.

Observations: lower part = ch 4 Note On with velocity, Note Off as `0x83` with release velocity; upper part = ch 1. Lower-hand velocities were low (13-51). The 11:31 window's ch 1 + ch 4 duplicates of C5/C6 suggest ch 4 also carries notes above the split in some keyboard modes; not resolved.

Still open: the low-register "F major" check passed for F1-A1-C2; NTT/bass-register behaviour on the Pa5X itself is Gate step 3 (transmit), not tested.
