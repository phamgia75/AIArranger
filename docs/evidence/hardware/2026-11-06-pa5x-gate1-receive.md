# Pa5X Hardware Gate 1: receive-only chord input (2026-11-06)

**Result: NOT OBSERVED.** No note event arrived from the Pa5X during the authorized 480 s session, so no chord recognition result exists. This is **not** a PASS and **not** a chord-detection FAIL: the detector received no input.

- **Bench:** the Mac mini, Pa5X over USB. Nothing was sent.
- **Tool:** `pa5x-probe` built from `94580d8` sources in the isolated hardware folder; source SHA-256 matched the commit.
- **Command:** `pa5x-probe listen --seconds 480 --channel any --split 128 --mode fingered`, connected to `[0] Pa5X KEYBOARD` only. FLOW 8 was not connected.

## Raw log (timestamps from the Mac mini)

```
22:01:14.707 listening 480 s on [0] Pa5X KEYBOARD (receive only: nothing is sent), split 128, mode fingered, channel any
22:09:14.727 done: 0 note events
```

## Per step

| Steps | Expected | Observed | Result |
|---|---|---|---|
| 1-3 Channels (C5, C3, C2) | One Note On/Off each, with its channel | No note events | NOT OBSERVED |
| 4-7 Root position (C, Am, G7, Bdim) | C maj, A m, G 7, B dim | No note events | NOT OBSERVED |
| 8-11 Inversions (C/E, C/G, G7/B, Am/C) | C maj bass E, C maj bass G, G 7 bass B, A m bass C | No note events | NOT OBSERVED |
| 12-15 Partial (C E Bb; C E B; C D E G; C E) | C 7, C maj7, C maj, NoChord (P1.7b rules) | No note events | NOT OBSERVED |
| 16-18 Low register (C1 maj, F1 maj, C2 maj) | C maj, F maj, C maj | No note events | NOT OBSERVED |
| 19 End marker (C6 x3) | 3 Note On/Off | No note events | NOT OBSERVED |

## Classification of the failure

| Cause | Status |
|---|---|
| Missing MIDI input | **Yes**: 0 note events on the only Pa5X source |
| Channel-routing mismatch | Not applicable: `--channel any` accepts every channel |
| Chord-detection failure | Not tested: no input reached the detector |

Why no input arrived is **UNKNOWN**. It is one of:

1. Nobody played during the window. The operator has not yet confirmed playing.
2. The Pa5X does not send its keyboard parts to USB under the current MIDI settings (the MIDI OUT channel assignments).

The earlier 11-05 audit also saw 0 note messages on this port, but nobody played then either.

## Tool changes after this session (`pa5x-probe`)

- **New:** `listen --raw`, receive-only below the parser. It shows non-realtime messages as hex and counts realtime bytes, so the next run shows whether the port delivers anything at all.
- **Found by a software positive control:** the raw listen and `control` paths listed endpoints before creating a CoreMIDI client and saw none. Fixed.
- **Positive controls,** with software-only virtual endpoints on the MacBook and no device:
  - a virtual source playing C3 E3 G3 on channel 3: parsed listen showed 6 events and C maj; raw showed 2 packets and 10 clock bytes;
  - a virtual sink: `control` delivered exactly `CF 54` (Variation 2, channel 16) and nothing else.

## Next (receive-only, needs the operator)

1. The operator confirms whether they played during 22:01:14-22:09:14.
2. The operator **reads, without changing**, the Pa5X MIDI OUT channel page (Settings > MIDI): which parts (Upper 1, Lower, ...) are assigned a channel.
3. A 60 s `listen --raw` while the operator plays C3 a few times, then the 19 steps again.

HARDWARE VALIDATION PENDING.
