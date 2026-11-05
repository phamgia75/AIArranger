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

`LiveEngineFacade::selectMidiOutput` and the C ABI already worked on explicit selection, and are unchanged. `MidiOutputManager` already selected explicitly, but re-resolved by name. It now uses the device identity (see below). The C ABI only reports `hasMidiOutput`, which means "a provider is wired".

## Policy

1. **`initialize()` never selects a destination.** Nothing is sent until the caller calls `selectDestination(index)`. A connected device is never used as a default or as a fallback.
2. **No destination means a controlled no-op.** Events are counted in `droppedCount()` and none are sent. Selecting an out-of-range index fails and leaves the current selection unchanged.
3. **Hardware-transmission guard at the `MIDISend` boundary.**
   - `initialize()` reads the `AIARR_MIDI_HW_TX` environment variable. If it is `0`, every `MIDISend` is blocked: events in `dispatchOne()` and cleanup CCs in `sendAllSoundOff()`. Blocked messages are counted in `blockedCount()`.
   - Any other value, or no value, allows transmission, but only to a destination that was selected explicitly.
   - CTest sets the guard for the whole ctest process. CMake adds a `TEST_INCLUDE_FILES` script (`set(ENV{AIARR_MIDI_HW_TX} 0)`) to the root directory and, recursively, to every subdirectory. So every test inherits it: in any directory, added later, or run with ctest from a subdirectory's build tree.
   - `test_midi_hw_guard_env`, registered in `tools/korg_validation`, fails if the variable does not reach a subdirectory test. It caught the subdirectory case during development.
   - Each CoreMIDI test also calls `setenv("AIARR_MIDI_HW_TX", "0")` itself, so a direct binary run is guarded.
   - `dispatchOne()` holds the selection lock from reading the endpoint until `MIDISend` returns. A switch or deselect, which silences the old device under the same lock, therefore happens strictly before or after any send. An in-flight Note On cannot reach the old device after its cleanup.
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
- **`MidiOutputManager` re-resolves by identity.** It remembers the device's unique ID, carried in `MidiOutDeviceInfo::uid`, and on refresh or hot-plug never adopts a different device that shares the name. Covered by `test_midi_output_manager_identity`. Providers without identity, such as the test fakes, keep the by-name behaviour.
- **Selection carries identity end to end.**
  - `MidiOutputManager::selectDevice()` and its refresh/hot-plug re-resolution select through `IMidiOutputProvider::selectByUid()`, which maps to `CoreMidiOut::selectDestinationByUid()`.
  - The device the user saw is selected even if the list shifted after enumeration, or nothing is selected. A stale index cannot redirect output.
  - `selectByUid()` defaults to failing (fail closed) for providers without identity.
- **One picker still selects by index.** The live host (`LiveHostComponent` → `LiveEngineFacade::selectMidiOutput(index)`) passes a list index straight to the provider. A hot-plug between showing the list and the pick could shift it. The pick is still an explicit user action. Moving it to `selectByUid` is a follow-up.
- **Hot-plug re-resolution (fixed).**
  - The selection is remembered by CoreMIDI **unique ID**, not by name or index.
  - A device with the same name can never be adopted. `test_midi_output_safety` has a "hot-plug" case that failed on the name-based code and passes now.
  - Selecting an endpoint without a unique ID is refused.
  - Resolution and selection are serialised by a mutex, so an in-flight resolve cannot undo a deselect.
- **Not yet proven on the device.** Passing unit tests do not prove hardware safety. The fix still needs one CoreMIDI test run on the Mac mini with the Pa5X attached and a MIDI monitor watching, to show that nothing reaches `Pa5X SOUND`.
- **The `hieucat75/AIArranger` fork has the same auto-select** (`feat/gate3-5-live-arranger-loop`, which includes `tools/pa5x_live_cli.cpp`). This fix lands in `phamgia75/AIArranger` only.

## Intentional hardware tests

Run them outside CTest and select the destination by exact name: for example `korg-playback --dest "Pa5X SOUND"`. Keep a log, and silence with Panic, or with CC123/CC120 on exit.
