# Pa5X MIDI hardware audit, 2026-11-05

- **Bench:** Mac mini (macOS 26.1, arm64), Korg Pa5X on direct USB, plus a FLOW 8.
- **Engine:** `main` at `9da66a9` (PR #35), Release build.
- **Endpoints, by CoreMIDI unique ID:**
  - destination `Pa5X SOUND` = 1907663670
  - source `Pa5X KEYBOARD` = 534315493
- **Operator:** none present at the instrument. Nothing in this log is an ear check.

## Independent monitor

A `DYLD_INSERT_LIBRARIES` interpose library logged every CoreMIDI send call in each process, with destination name, unique ID and bytes. It covered `MIDISend`, `MIDISendEventList`, `MIDISendSysex`, `MIDIReceived` and `MIDIReceivedEventList`. It is independent of the engine's own counters.

Positive control: one Note On and one Note Off sent to a process-owned virtual sink were logged as exactly those 2 calls.

## Results

| # | Test | Verified result |
|---|---|---|
| 1 | PR #35 gate: the 5 CoreMIDI tests via ctest, with the Pa5X attached and the guard set | 5/5 passed. `test_midi_output_safety` passed 25/25, no SKIP. The monitor loaded in all 5 processes and recorded **0 send calls**. |
| 2 | Bounded output: C4/E4/G4, ch 1, vel 64, explicit Note Off. Destination selected by unique ID. | `initialize()` selected nothing. Selection by uid resolved to `Pa5X SOUND`. Monitor: 6 note messages (3 Note On, 3 Note Off, in order), then CC123 + CC120 on all 16 channels at shutdown. 38 calls in total, all to uid 1907663670. No SysEx, Bank Select or Program Change. |
| 3 | Live loop, about 25 s: `Pa5X KEYBOARD` → `LiveEngineFacade` (demo style, fingered detector) → `Pa5X SOUND`. Start, then variation B, fill, ending, panic. | 466 engine events plus 32 cleanup CCs = 498 calls, all to uid 1907663670. Note On = Note Off = 201, nothing left sounding. Channels used: 2 (bass), 3 (chord), 10 (drums). |
| 4 | Send-time drum timing in test 3, from monitor timestamps | 102 onsets. Fitted period 250.006 ms against a nominal 250 ms at 120 BPM (+0.002%). Deviation from the grid: p50 0.64 ms, p95 1.76 ms, max 2.11 ms. Simultaneous hits were within 0.95 ms of each other. |
| 5 | Feedback / echo, during tests 2 and 3 | An independent raw listener on `Pa5X KEYBOARD` received only realtime bytes: 10 and 106. It received **0 note messages**, so no echo of sent notes came back. The loop breaker (more than 300 non-realtime messages per second) never tripped. `test_midi_input_no_thru` covers the no-thru invariant. |
| 6 | Genos → Pa5X SMF (`POP_ACOUSTIC_2`) | The export from `9da66a9` is byte-identical to the #33/#34 output: SHA-256 `f31d3058…`. Validator PASS, 8 elements. |

## Not performed (no claim)

- **Audibility, sound and groove by ear.** No one was at the instrument.
- **Chord recognition from the real keyboard.** Nobody played during the 25 s listen window, so the engine used its default chord (C).
- **Pa5X Import SMF / Export SMF of `POP_ACOUSTIC_2`.** This needs the front panel and a USB stick, and no stick was mounted. The round-trip `--compare` is therefore pending. See `docs/exporters/pa5x-hardware-validation.md`.
- **Gate 3.5 ear sign-off (H1–H5).** It needs a human.

## Notes

- The timing figures are the moments `MIDISend` was called on the host. USB transfer and Pa5X internal latency are not included.
- The demo style sends bass and chord on channels 2 and 3. Under the bench's MIDI preset those reach Scene-1 keyboard parts, not the accompaniment tracks. Whether they sounded was not checked.
- The test tools are outside the repository, at `~/pa5x-validation/` on the Mac mini: `pa5x_hw_bench.cpp`, `midi_send_spy.c`, `spy_positive_control.c`. Their logs are there too.
