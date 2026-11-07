# Pa5X Gate 2: keyboard reception and chord recognition (receive only), 2026-11-07

**Result: BLOCKED (4/4). Nothing was transmitted.** Bench: Mac mini, `pa5x-probe` built from `fa780a2`, source `Pa5X KEYBOARD` (uid 534315493), `--channel any --split 60 --mode fingered`, parsed and `--raw` listeners in parallel. Pre-flight: `pa5x-doctor` KEYBOARD + SOUND online; no AIArranger / pa5x / korg-playback process; MusicAIApp, Music, Moises open (irrelevant to receiving, blocks Phase B).

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
