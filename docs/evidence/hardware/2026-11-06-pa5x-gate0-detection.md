# Pa5X Hardware Gate 0: detection (2026-11-06)

**Result: PASS (detection only).** No MIDI was sent, so nothing about playback, timing or sound is validated here.

- **Bench:** the Mac mini (Apple silicon, macOS 26.1) with the Pa5X attached over USB. Reached over SSH with key-only authentication. The owner verified the host key on the machine itself.
- **Tool:** `pa5x-doctor`, read-only (it creates no ports and sends nothing). It was built from `94580d8` sources with `git archive`, in a separate folder outside the existing repository, which was left untouched.

## pa5x-doctor output

```
Sources (2):
  [0] Pa5X KEYBOARD                uid=534315493   online  Pa5X KEYBOARD (Pa5X -> engine)
  [1] FLOW 8                       uid=1024724732  online
Destinations (2):
  [0] Pa5X SOUND                   uid=1907663670  online  Pa5X SOUND (engine -> Pa5X)
  [1] FLOW 8                       uid=2078839905  online

RESULT: Pa5X found (KEYBOARD + SOUND online). Select the SOUND port by uid.
exit=0
```

## USB (system_profiler, serial number omitted)

Pa5X, Manufacturer Korg Inc., Vendor ID 0x0944, Product ID 0x01cc, Product Version 0x0419, 480 Mb/s, Removable.

## Notes

- A second MIDI device, **FLOW 8**, is attached. Test traffic must go only to the exact destination `Pa5X SOUND` (uid 1907663670); `pa5x-probe` refuses anything else.
- **Not verified yet:** the Pa5X MIDI channel assignments (Settings > MIDI). The next step, receive-only: `pa5x-probe listen` while the operator plays, which shows the actual channel of each keyboard part.
- The existing repository on the bench is on `feat/gate3-5-live-arranger-loop` @ `713743a` (clean), without the `feat/pa5x-first-baseline` ref.
