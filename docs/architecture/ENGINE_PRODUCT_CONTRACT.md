# Engine / Product Contract (Gate 4)

**Status:** FROZEN (Gate 4) — public contract for the portable engine, pinned so
the Gate 5 iPad product layer can be built against a stable surface.
**Date:** 2026-07-11
**Baseline:** hardware Gate 3 = `main@2e1f1fa` (unchanged by this contract work).

This document is the authority for how a product host (the Gate 5 iPad SwiftUI
app, the macOS reference host, or any other caller) drives the engine. Gate 4
**defines and tests** this contract; it does **not** change any Gate 3 realtime
timing / lifecycle implementation (CoreMIDI I/O, scheduler, clock, chord latch,
panic, trace harness are untouched).

---

## 1. Layers & platform boundary

```
┌──────────────────────────────────────────────────────────────┐
│ Product / UI layer   (Gate 5: iPad SwiftUI · macOS JUCE host)  │
│   drives commands, polls snapshots — no engine internals       │
├───────────────── C ABI bridge (opaque handle) ─────────────────┤
│   include/aiarranger/aiarranger_engine.h  ·  arranger-bridge    │
│   pure C · no STL/exceptions cross this line                    │
├──────────────────── LiveEngineFacade (C++) ────────────────────┤
│   session::LiveEngineFacade  — the single portable entry point  │
├───────────────────── Portable engine core ─────────────────────┤
│   arranger-engine  — links NO Apple framework                   │
├──────────────── Platform adapters (host-specific) ─────────────┤
│   platform-apple: CoreMIDI in/out, CoreAudio clock   (Gate 3)   │
│   filesystem / style-loader: importer-sff1                      │
└──────────────────────────────────────────────────────────────┘
```

**Boundary invariant (verified at link time):** the portable core
(`arranger-engine`) and everything in the contract layer link the core **only** —
no `platform-apple`, no Apple framework. The Gate 4 contract tests
(`tests/session/*`) and the bridge library + test (`arranger-bridge`,
`tests/bridge/*`) deliberately link `arranger-engine` **without** `platform-apple`;
if any contract code reached for CoreMIDI/CoreAudio it would fail to link. This is
the machine-checked form of "core is portable."

Named platform seams the core depends on only through interfaces:

| Seam | Interface (portable) | Apple impl (Gate 3, untouched) |
|------|----------------------|-------------------------------|
| MIDI input | `midi::IMidiInputSource` | `CoreMidiIn` |
| MIDI output | `midi::IMidiOutputProvider` | `CoreMidiOut` provider |
| Host clock/timer | tick(numSamples) driven by host | CoreAudio / JUCE timer |
| Style/filesystem | `uasf::StyleDefinition` in | `importer-sff1` |

---

## 2. Thread-ownership model

The engine is **not** globally synchronised. Ownership is explicit; follow it and
the lock-free queues keep their producer discipline (commands: multi-producer;
MIDI input: single read thread; engine control queue: the tick thread only).

| API group | Thread that may call it | Mechanism |
|-----------|------------------------|-----------|
| `start` / `stop` / `loadStyle` (lifecycle) | **owner thread** only | direct; not re-entrant |
| Transport / section / tempo / variation / panic commands | **any thread, concurrently** | enqueued on a bounded lock-free **MPSC** ring (exactly `maxCommandQueue` = 256 slots), applied in `tick()` |
| MIDI input events | the input source's **read thread** | routed lock-free onto a 2nd SPSC queue, drained in `tick()` |
| `tick(numSamples)` | the **engine thread** (one, consistent) | advances clock/sequencer, pumps output, publishes snapshot |
| `snapshot()` / `capabilities()` / `lastError()` | **any thread** | snapshot: lock-free seqlock read, never torn (published each tick; the tick side never takes a lock, so `tick()` may run on an audio thread) |

`lifecycleState()` reads the owner-thread tracker; for cross-thread polling read
`snapshot().lifecycleState`, which is published atomically inside `tick()`,
`start()`, and `stop()`.

Dropped commands (command queue full) are not silently lost: they set
`EngineError::QueueFull`, observable via `lastError()`. The same applies to MIDI
input events when the input queue overflows on the read thread.

The `tick()` hand-off from the facade queues into the engine's own control queue
is **lossless**: when a burst of commands + MIDI input exceeds what the engine
queue can take in one tick, the remainder stays queued (FIFO order kept) and is
delivered on the next tick(s) — it is never discarded (a discarded NoteOff would
leave a stuck held note). Pinned by `tests/session/test_facade_queue_integrity.cpp`
(capacity, 4-thread producer race, stuck-note reproducer; run under TSan in CI).

---

## 3. Lifecycle state machine

Defined and exhaustively tested headless in `session::EngineLifecycle`
(`src/session/engine_lifecycle.{h,cpp}`, `tests/session/test_engine_lifecycle_contract.cpp`).
The transition function is a **pure** static (`EngineLifecycle::step`) — an
identical event sequence from `Created` always replays to the identical state.

States: `Created → Configured → Ready → Running ⇄ Suspended → Stopped`, terminal
`ShutDown`.

```
Created    --Configure--> Configured
Created    --LoadStyle--> Ready          (construction already wired devices)
Created    --Start-----> Running         (boot() loads a default style)
Configured --LoadStyle--> Ready
Configured --Start-----> Running
Ready      --LoadStyle--> Ready          (reload)
Ready      --Start-----> Running
Running    --Suspend---> Suspended
Suspended  --Resume----> Running
Running    --Stop------> Stopped
Suspended  --Stop------> Stopped
Stopped    --Start-----> Running         (restart)
Stopped    --LoadStyle--> Ready
Stopped    --Configure--> Configured
<any but ShutDown> --Panic----> (unchanged)   emergency all-notes-off
<any but ShutDown> --Shutdown--> ShutDown
ShutDown   --<anything>--> InvalidState  (terminal)
```

Every other edge is illegal: `apply()` leaves the state unchanged and returns a
classifying `EngineError` (`NotStarted`, `AlreadyStarted`, `InvalidState`).

**Facade binding (Gate 4):** the `LiveEngineFacade` drives this tracker from its
existing methods — `start()→Start`, `stop()→Stop`, `loadStyle()→LoadStyle` (when
not running). `Suspend`/`Resume` are **defined and tested in the state machine**;
their facade/runtime binding is deferred to Gate 5 (see §7) because it would touch
device retention semantics best proven on hardware. The bookkeeping added to the
facade is pure state tracking and changes **no** runtime behaviour.

---

## 4. Public API surface

### 4.1 Value types — `src/session/engine_contract.h` (frozen POD)

- `EngineError` (`int32_t`): `Ok=0, NullArgument, InvalidState, NotConfigured,
  NoStyleLoaded, AlreadyStarted, NotStarted, DeviceUnavailable, QueueFull,
  Unsupported`. `toString()` for logs.
- `EngineCapabilities` (POD): contract version + `hasMidiInput/Output`,
  `hasChordDetect`, `hasSections`, `hasSuspendResume`, `latencyTrace`,
  `maxCommandQueue`.
- `LifecycleState` / `LifecycleEvent` (`uint8_t`) — §3.
- `EngineSnapshot` (`src/session/engine_snapshot.h`, POD) — UI poll value; gained
  one additive field `lifecycleState` in Gate 4.

### 4.2 `LiveEngineFacade` (`src/session/live_engine_facade.h`)

Commands (any thread, lock-free): `transportStart/Stop`, `syncStart`, `intro`,
`fill`, `breakSection`, `ending`, `panic`, `setVariation(0..3)`, `setTempo`.
Devices: `selectMidiInput/Output`. Lifecycle (owner thread): `start`, `stop`,
`loadStyle`. Engine step: `tick(numSamples)`. Reads (any thread): `snapshot`,
`capabilities`, `lifecycleState`, `lastError` / `clearError`, `contractVersion`.

**Gate 4 additions are additive** — no existing signature changed, no runtime
behaviour changed. See the API before/after in the PR description.

---

## 5. Product bridge boundary — C ABI

`include/aiarranger/aiarranger_engine.h` (+ `arranger-bridge`) is the one surface
the product host links. Rules:

- **Pure C**, opaque `AiArrEngine*` handle. Nothing C++ crosses the line: no STL,
  no exceptions. Every entry point is `noexcept`; failures are `AiArrError`
  return codes.
- Enum integers match the C++ contract **1:1**, locked by `static_assert` in
  `aiarranger_bridge.cpp` (ABI drift is a compile error).
- State is read into caller-owned POD (`AiArrSnapshot`, `AiArrCapabilities`).
  No callback retains C++ ownership.
- `aiarr_engine_create/destroy` are the explicit ownership pair; `destroy(NULL)`
  and every NULL-handle call are safe and deterministic.

Exercised by `tests/bridge/test_bridge_roundtrip.cpp` (create → capabilities →
lifecycle → transport → tick → snapshot → stop → destroy; null-handle safety;
200× create/destroy). This is the shape the Gate 5 Swift host imports via a module
map.

---

## 6. Error model & versioning

- **Deterministic:** identical inputs → identical `EngineError`. No error is
  silently swallowed; command-queue overflow surfaces as `QueueFull`, bad device
  index as `DeviceUnavailable`.
- **Contract version:** `kEngineContractVersion = MAJOR<<16 | MINOR<<8 | PATCH`
  (currently `1.0.1` — 1.0.1 made the implementation honour §2: MPSC
  command queue + lossless drain; no API change). Also reachable pre-handle via `aiarr_contract_version()`
  so a host can gate on ABI compatibility before creating an engine.
- **Backward-compatibility rules (additive-only):**
  - Existing `EngineError` / `LifecycleState` enumerators keep their integer
    values forever; new ones are **appended**.
  - POD structs grow by **appending** defaulted fields; never reorder/remove.
  - MAJOR bump = a break (never silent); MINOR = appended enumerators/fields;
    PATCH = docs/impl only.

---

## 7. Deferred to Gate 5

The following are intentionally **not** in Gate 4 (they need hardware or the Swift
toolchain, or would change Gate 3 runtime that stays frozen until hardware
validation):

- Runtime `suspend()`/`resume()` on the facade + bridge (device-retention
  semantics) — the state machine models them; the binding lands with hardware.
- MIDI device binding through the bridge (enumerate/select in/out) — Gate 4's
  bridge is headless.
- Style-file loading through the bridge (path → `importer-sff1` →
  `StyleDefinition`), and MIDI input-event injection through the bridge.
- Swift module map / SwiftPM packaging of `arranger-bridge`; iPad SwiftUI views
  that poll `AiArrSnapshot` and issue `AiArrError`-returning commands.
- Any change to CoreMIDI teardown / scheduler / timer / latch — remains frozen
  under `main@2e1f1fa` until Gate 3 hardware validation
  (`hardware_validated:false`).

---

## 8. Verification (Gate 4)

- Fresh headless configure/build/test: **81/81 pass, 0 warnings** from project
  code (78 baseline + 3 new contract suites).
- New suites: lifecycle contract (28), facade contract (21), bridge round-trip
  (29) — all headless, no hardware.
- Contract + bridge tests link the **core only** (platform-boundary assertion).
- macOS app (`-DBUILD_MACOS_APP=ON`) still compiles/links with the public-header
  additions.
- SFF1: 4/4 fixtures, 0 skip. No hardware-sensitive file touched (`git diff
  --stat`); `hardware_validated:false` unchanged.
