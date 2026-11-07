# Pa5X Gate 2: keyboard reception and chord recognition (receive only), 2026-11-07

**Result: accepted by the owner as receive-path evidence. Retry 12:07 = 4/4 PASS (see "Retry"); first attempt 11:36 = 4/4 BLOCKED. Nothing was transmitted in either.** Bench: Mac mini, `pa5x-probe` built from `fa780a2`, source `Pa5X KEYBOARD` (uid 534315493), `--channel any --split 60 --mode fingered`, parsed and `--raw` listeners in parallel. Pre-flight: `pa5x-doctor` KEYBOARD + SOUND online; no AIArranger / pa5x / korg-playback process; MusicAIApp, Music, Moises open (irrelevant to receiving, blocks Phase B).

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

## Finding (first attempt; hypothesis withdrawn, see "Intermittent receive")

The Pa5X transmits the upper-keyboard parts over USB (ch 1 and ch 4; two parts with MIDI OUT channels) but **not the lower part below its split point**. Hypothesis, not verified: the Lower part has no MIDI OUT channel in the current MIDI preset, so lower-hand chords reach only the Pa5X's own chord recognition. This is consistent with the 11-06 record (OUT ch1 = Upper 1, nothing for Lower) but the ch 4 sender is not in that record, so the preset photo is needed.

Consequence for AIArranger: with this preset the engine never sees the left hand. Live accompaniment needs either a Lower OUT channel (Pa5X setting, owner decision) or detection on the received upper notes.

## Unblock options (no setting changed by us)

1. Owner plays the same four chords **above C4**, listener with `--split 128 --channel 1` (all received notes count; per-channel held sets avoid the ch1/ch4 duplicate). Tests the detector, not the lower-keyboard path.
2. Owner assigns a MIDI OUT channel to Lower (Settings > MIDI > MIDI OUT), a Pa5X settings change needing the owner's decision; then rerun as specified.

## Tool fix

`pa5x-probe` output was block-buffered when piped, so events appeared only at exit and an early stop would lose them. Now line-buffered (`setvbuf(stdout, _IOLBF)`).

## Retry 12:07:53-12:12:53 (300 s), line-buffered probe `addd4bc`

Listeners: parsed `--channel 1 --split 128` (set up for upper-register playing) and `--raw --channel any`. The owner played the four chords in the **lower register** and the lower part **was transmitted this time, on ch 4** (status `0x93`/`0x83`); the cause of the empty 11:36 capture is not established; the owner confirmed no change to Split, LOWER, MIDI OUT channels or MIDI preset between the runs. The parsed listener filtered ch 1 and so saw only right-hand single notes (G4, C5, B4 -> NoChord, correct for one note). The chords come from the raw capture; each was played twice (at ~0.2-36 s and ~60-81 s) with the same notes.

| # | Raw Note On (ch 4) | Notes | Fingered detector (engine code, replayed from these bytes) | Verdict |
|---|---|---|---|---|
| 1 | `93 24 15 93 28 1F` `93 2B 12` | C2 E2 G2 | Major, root C | **PASS** |
| 2 | `93 1C 0F 93 24 10` `93 1F 0F` | E1 G1 C2 | Major, root C. Fingered by design does not carry the bass (`test_chord_scan` "Fingered C/E 1st inversion"); Fingered-on-Bass gives C over E1 | **PASS** (inversion recognised as C; bass reported only in on-bass mode) |
| 3 | `93 1D 16 93 21 18` `93 24 12` | F1 A1 C2 | Major, root F | **PASS** |
| 4 | `93 24 15 93 2E 0D` `93 28 29` | C2 Bb2 E2 | Dom7, root C (spec §4.4 partial: fifth missing) | **PASS** |

Method, stated plainly: the notes and channels are hardware evidence. The chord names were computed **offline** by the same `FingeredDetector` / `FingeredOnBassDetector` code fed the captured bytes in arrival order through `HeldNotes` (as `listen` does), now a regression test in `tests/midi/test_pa5x_probe.cpp` (5 cases). A live parsed run with `--channel 4 --split 60` would show the same names on screen; not repeated.

Observations: lower part = ch 4 Note On with velocity, Note Off as `0x83` with release velocity; upper part = ch 1. Lower-hand velocities were low (13-51). The 11:31 window's ch 1 + ch 4 duplicates of C5/C6 suggest ch 4 also carries notes above the split in some keyboard modes; not resolved.

Still open: the low-register "F major" check passed for F1-A1-C2; NTT/bass-register behaviour on the Pa5X itself is Gate step 3 (transmit), not tested.

## Intermittent receive (unresolved)

The owner confirmed (2026-11-07) that **no Pa5X setting changed** between the 11:36 run (0 notes) and the 12:07 run (93 packets). The "Lower has no MIDI OUT channel" hypothesis above is therefore **withdrawn**: the lower part is sent on **ch 4** with the unchanged configuration. Do not assume ch 2 (or any other) is the LOWER channel; ch 4 is the verified one.

Comparison of the three listener runs (all on the same bench, same CoreMIDI source):

| | 11:31:02 (300 s) | 11:36:08 (600 s) | 12:07:53 (300 s) |
|---|---|---|---|
| Probe build | `fa780a2` | `fa780a2` | `addd4bc` (stdout line-buffered only) |
| Parsed filter | any / split 60 | any / split 60 | ch 1 / split 128 |
| Raw filter | none (all channels) | none | none |
| Source | `[0] Pa5X KEYBOARD` | same | same |
| Raw non-realtime packets | 20 | **0** | 93 |
| FE Active Sensing | 1180 (3.93/s) | 2381 (3.97/s) | 1163 (3.88/s) |
| CoreMIDI client connection | opened 11:31:02.30, closed 11:36:02.31 | opened 11:36:08.72, closed 11:46:08.73 | opened 12:07:54.13, closed 12:12:54 |

What this rules out, from evidence:
- **Channel filtering:** the raw listener has no channel filter and also saw 0 packets.
- **Output buffering:** the 0 is the raw listener's end-of-run counter, not missing output.
- **Wrong or lost source:** the same named source in all runs; Active Sensing from that source arrived at the normal ~4/s rate for the whole 600 s, so the subscription was live throughout.
- **CoreMIDI or USB reset:** `MIDIServer` is the same process (pid 69718, since 11-05); the system log 11:36-12:13 has no kernel USB, device or setup-change events, and no CoreMIDI activity other than our own tool connections.

What remains open (not tested, no fix applied):
- Whether keys were actually played inside 11:36:08-11:46:08: the owner reported "Done" ~76 s after start; no timestamped confirmation of what was played when.
- Whether the Pa5X was in a state that suppresses note transmission while Active Sensing continues (a screen, page or mode), with no setting change.
- Whether this is a transient USB-MIDI fault on the Pa5X side.

The next step is the repeatability test in `PA5X_MIDI_VALIDATION_SEQUENCE.md` §2b, not a code or setting change.

## Repeatability 2b, run 1 of 3: 12:24:20-12:27:20 (180 s), probe `c425c6f` — PASS

Listeners: parsed `--channel 4 --split 60 --mode fingered`, raw all channels. Operator started ~12:25 (owner's time).

| Check | Result |
|---|---|
| C2-E2-G2 presses (Note On C2 on ch 4) | 14 (operator played more than the 10 asked) |
| Complete chords received (raw `93 24`, `93 28`, `93 2B`) | 14 / 14 / 14: no note lost |
| Detected on the full chord | `Cmaj (bass C)` 14/14 |
| Transitional names while keys go down | `C2+E2` -> NoChord (11), `C2+G2` -> `C5` = C **power chord** (3, when G landed before E); each followed by Cmaj once the third key arrived |
| Upper control note | `90 54` = **C6** on ch 1 (operator played C6, not C5): upper part on ch 1 confirmed |
| FE Active Sensing | 694 in 180 s (3.86/s), continuous |
| Raw packets / parsed events | 64 / 84 |

No dropout.

## Repeatability 2b, run 2 of 3: 12:39:38-12:42:38 (180 s), after ~12 min idle — PASS (chords); upper control not received

| Check | Result |
|---|---|
| C2-E2-G2 presses | 10 |
| Complete chords received (raw `93 24` / `93 28` / `93 2B`) | 10 / 10 / 10 |
| Detected on the full chord | `Cmaj (bass C)` 10/10 (transitions: C2+E2 NoChord 9, C2+G2 power chord 1) |
| Upper control note (C5 on ch 1) | **none received** (no ch 1 event in raw). Not determined whether it was played; does not affect the ch 4 result |
| FE Active Sensing | 704 in 180 s (3.91/s), continuous |
| Raw packets / parsed events | 43 / 60; first chord at 15.6 s, last release at 66.6 s |


## Repeatability 2b, run 3 of 3: 12:46:35-12:49:35 (180 s), after normal use of the Pa5X screen — PASS

| Check | Result |
|---|---|
| Upper part first | ch 1 notes from 23.7 s (A4, C5, ... 72 Note Ons, B3-C6, a played line): upper = ch 1 confirmed |
| C2-E2-G2 presses | 10; complete chords received (raw `93 24` / `93 28` / `93 2B`) 10 / 10 / 10 |
| Detected | `Cmaj (bass C)` 9; once B1 (`93 23`) was also held -> `Cmaj7 (bass C)`, correct for the four notes held |
| FE Active Sensing | 649 in 180 s (3.6/s); lower rate is expected with 195 packets of other traffic (Active Sensing fills idle gaps), no gap observed |

## Repeatability summary

| Run | Condition | Chords received complete on ch 4 | Correctly named for the notes held |
|---|---|---|---|
| 1 | immediately | 14/14 | 14/14 |
| 2 | after ~12 min idle | 10/10 | 10/10 |
| 3 | after normal screen use | 10/10 | 10/10 (9 Cmaj, 1 Cmaj7 with an extra B1) |

**34/34 chords, no lost note, ch 4 stable.** The 11:36 zero-note run was **not reproduced**; it stays recorded as an unresolved intermittent. The one possibly missed upper note (run 2 C5 control) is unconfirmed: the owner has not said whether it was played.
