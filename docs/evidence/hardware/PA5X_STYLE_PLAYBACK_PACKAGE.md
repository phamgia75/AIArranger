# Pa5X Style playback validation package

Status 2026-11-07: **SOFTWARE PASS (offline)**. Nothing was sent to the Pa5X or any other MIDI device.
No Style event has been transmitted; physical playback is **UNCONFIRMED**.

Test: `tests/midi/test_pa5x_style_playback_offline.cpp` (ctest `test_pa5x_style_playback_offline`).
Output goes to a fake provider (`tests/midi/fake_midi_output_provider.h`); CoreMIDI is never opened.

| Requirement | How the package meets it |
|---|---|
| Known-good style | Built-in demo style, Main 1 (drums, bass, chord). The demo Intro is removed because it has no chord track |
| Controlled tempo | 120 BPM, 480 ppq, 48 kHz, 256-sample blocks |
| Explicit channel map | `pa5x::demoStyleTrackMap()`: bass → ch 9 (STY Bass), drums → ch 10 (STY Drum), chord → ch 12 (STY Acc1); everything else and all Bank Select / Program Change dropped |
| Bounded duration | 2 bars + 100 ticks (~4.1 s), then Stop |
| Predictable Note On/Off | Expected Note Ons (tick, channel, pitch) written by hand from the style data, not computed by the engine; 40 expected = 40 actual |
| Safe Stop / cleanup | Stop lands while the bar-3 downbeat notes sound; after Stop every Note On has its Note Off, no new Note On, output stays on the 3 mapped channels |
| Expected-vs-actual log | Printed row by row in the test output (kept in CI logs); mismatches marked |
| Independent timing/order check | Captured ticks are non-decreasing, none past the window; two runs are identical; G major keeps the event count and maps bass to G/D, Acc1 to G-B-D |

Found while building it (no engine defect):
- With the identity output map the demo bass (ch 2) and chord (ch 3) would be silent on the Pa5X: the
  "Ai Arranger" preset has ch 2-8 Off. See `PA5X_MIDI_ROUTING_MATRIX.md`.
- `Start` always plays section 0; for the demo that is the Intro (drums and bass only).

Not covered: the app/host path (the app does not set a Pa5X map), swing, section changes, tempo
changes during playback, the realtime CoreMIDI send timing.

## Hardware step: not proposed yet

No tool can play a Style to the Pa5X SOUND port today (`pa5x-probe` sends single notes and Control
PCs only), so no command exists to propose. A future proposal must name the command, the target
(Pa5X SOUND only, never FLOW 8), the 3 channels above, the 2-bar bound, the cleanup (CC123 + CC120
on ch 9, 10, 12 only) and the owner's actions, and wait for authorization. Before it: check R2-R4 in the routing matrix.
