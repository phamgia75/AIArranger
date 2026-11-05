# MIDI output safety: fail-closed policy

## Incident (2026-11-05)

On the Mac mini with a Korg Pa5X attached over USB, a software-only `ctest` run sent MIDI to the instrument. It was not asked to.

The cause was `CoreMidiOut::initialize()`, which auto-selected destination 0 whenever one existed, and destination 0 was `Pa5X SOUND`. The following tests then drove the instrument:

| Test | What reached `Pa5X SOUND` |
|---|---|
| `test_midi_output_provider` | 1 Note On (ch 1, note 60, vel 100), **no Note Off**; nothing silenced it at shutdown |
| `test_latency_stability` | ~130 balanced Note On/Off pairs (ch 1, notes 36-59), plus CC123/CC120 × 16 channels on "unplug" |
| `test_coremidi_out` | CC123/CC120 × 16 channels from `selectDestination(-1)` |
| `test_coremidi_reinit` | nothing sent, but 25 init cycles each selected the device |

No SysEx, Bank Select or Program Change was sent, so no instrument setting was changed.

## Affected call paths (before the fix)

- `CoreMidiOut::initialize()` auto-selected the first destination.
  - Every user of the class inherited this: `CoreMidiOutputProvider`, `tools/korg_playback_cli.cpp`, and both app drivers.
- `apps/macos/Source/EngineDriver.h`: `initialize()` followed by `MidiOutputManager::refresh()`. With no name stored, `refresh()` calls `select(-1)`, which sent 32 CCs to the auto-selected device at every app start.
- `apps/macos/Source/LiveHostDriver.h`: the live host played to whichever device was first, before the user picked one.
- `CoreMidiOut::shutdown()` did not silence the device, so a Note On without its Note Off kept sounding after the process exited.
- `CoreMidiOut::shutdown()` kept the remembered selection. After a re-`initialize()`, the hot-plug probe silently reconnected the old device by name.
- Hot-plug re-resolution matched by display name. A different device with the same name could take over the output.

`MidiOutputManager`, `LiveEngineFacade::selectMidiOutput` and the C ABI already worked on explicit selection. They are unchanged. The C ABI only reports `hasMidiOutput`, which means "a provider is wired".

## Policy

1. **`initialize()` never selects a destination.** Nothing is sent until the caller calls `selectDestination(index)`. A connected device is never used as a default or as a fallback.
2. **No destination means a controlled no-op.** Events are counted in `droppedCount()` and none are sent. Selecting an out-of-range index fails and leaves the current selection unchanged.
3. **Hardware-transmission guard at the `MIDISend` boundary.**
   - `initialize()` reads the `AIARR_MIDI_HW_TX` environment variable. If it is `0`, every `MIDISend` is blocked: events in `dispatchOne()` and cleanup CCs in `sendAllSoundOff()`. Blocked messages are counted in `blockedCount()`.
   - Any other value, or no value, allows transmission, but only to a destination that was selected explicitly.
   - CTest sets `AIARR_MIDI_HW_TX=0` on **every** registered test, in the top-level directory and in `tools/korg_validation`. Tests added later get it too, because the block runs over the directory's `TESTS` property rather than over a list of names.
4. **Deterministic cleanup when authorized.**
   - Deselecting or switching sends CC123 and CC120 on all 16 channels to the old device. This was already the behaviour.
   - `shutdown()` now sends the same after the final queue drain.
   - Both go through the guard.
5. **No MIDI thru.**
   - Input is routed only to chord detection and transport control (`routeMidiInput`). No input message is forwarded to an output.
   - `test_midi_input_no_thru` locks this in. On the Pa5X the keyboard and the sound engine are the same device, so an echo would double notes and could loop.
6. **Tools select explicitly.**
   - `korg-playback` takes `--dest "NAME"` or interactive `[` / `]`.
   - `--dest` goes through `CoreMidiOut::selectDestinationByName()`. The name must match exactly one live destination, and the endpoint that matched is the one selected, in a single pass. A hot-plug between the lookup and the selection cannot redirect output to another device.
   - A missing or ambiguous name fails and selects nothing.
   - With neither, it plays silently.

## Regression coverage

`tests/engine/test_midi_output_safety.cpp` uses only virtual CoreMIDI destinations that the test creates itself:

- a decoy named exactly `Pa5X SOUND`
- a uniquely named sink

It selects endpoints by `MIDIEndpointRef` identity, never by name, so a real device with the same name is never touched. It proves the following:

- Initialization with a "Pa5X SOUND" present selects nothing.
- With no destination, nothing is sent and the decoy receives nothing, including from shutdown cleanup.
- Selection must be explicit, and an out-of-range select changes nothing.
- The guard blocks events and the deselect cleanup at the send boundary, and the selected sink receives nothing.
- `shutdown()` attempts the cleanup for the selected device: 32 CCs, blocked here by the guard.
- `shutdown()` forgets the selection. A re-initialized object has no destination, and the hot-plug probe does not reconnect the old one.
- No automated test transmits, even to its own virtual sink. Selection is by index, and a device plugged in mid-test could shift the indices. Delivery of Note Off and cleanup to a real device is verified only in a manual hardware run.

To check that the test catches the bug, the auto-select was put back temporarily. Seven checks failed.

`test_latency_stability` now "replugs" to a virtual sink it owns instead of real destination 0.

## Remaining risks

- **The guard is opt-out, not opt-in.**
  - Production code transmits to an explicitly selected destination unless `AIARR_MIDI_HW_TX=0`.
  - Every CoreMIDI test also sets `AIARR_MIDI_HW_TX=0` itself at the top of `main`, so a binary run directly, outside CTest, is guarded too.
  - A new CoreMIDI test that forgets this line is covered only under CTest.
- **App UI pickers still select by index.** The JUCE output menus and `MidiOutputManager` pick a destination by its index in a list enumerated a moment earlier. A device plugged or unplugged in between could shift the index. Every such pick is still an explicit user action, but moving these pickers to name or identity selection is a follow-up.
- **Hot-plug re-resolution (fixed).**
  - The selection is remembered by CoreMIDI **unique ID**, not by name or index.
  - A device with the same name can never be adopted. `test_midi_output_safety` has a "hot-plug" case that failed on the name-based code and passes now.
  - Selecting an endpoint without a unique ID is refused.
  - Resolution and selection are serialised by a mutex, so an in-flight resolve cannot undo a deselect.
- **Not yet proven on the device.** Passing unit tests do not prove hardware safety. The fix still needs one CoreMIDI test run on the Mac mini with the Pa5X attached and a MIDI monitor watching, to show that nothing reaches `Pa5X SOUND`.
- **The `hieucat75/AIArranger` fork has the same auto-select** (`feat/gate3-5-live-arranger-loop`, which includes `tools/pa5x_live_cli.cpp`). This fix lands in `phamgia75/AIArranger` only.

## Intentional hardware tests

Run them outside CTest and select the destination by exact name: for example `korg-playback --dest "Pa5X SOUND"`. Keep a log, and silence with Panic, or with CC123/CC120 on exit.
