# AI Arranger — Project Review & Continuation Plan

---

## ▶ macOS integration validation — 2026-11-03 (supersedes conflicting claims below)

> Branch `integ/audit-merge-2026-11-03` = the 20 audit commits below + 5 follow-ups.
> Environment: **macOS 27 arm64, Apple clang 17, CMake 4.3, Command Line Tools only (no
> Xcode), no MIDI hardware.** `main@5e72ee6` is unchanged and is the baseline.

**Corrections to the 2026-09-28 audit (it ran on Linux only):**
- "93/93 ASan+UBSan" did not hold on macOS: 4 tests aborted with UBSan
  `serializer.cpp:49: load of value 165 ... type 'bool'`. Root cause: the uasf model structs
  had no member initializers, so `uasf::TrackDefinition track;` left
  `articulation.recommended.*`, `has_keyswitch` etc. indeterminate; serialized
  `playback_flags` depended on heap contents, and the committed corpus `.uasf` fixtures
  carried `0x0F` on every track. Fixed in `318913b` (defaults at the type definitions, fixtures
  regenerated: only the playback_flags bytes changed), test `test_uasf_deterministic_init`.
- "1920-PPQN imports ran 4× too slow through the facade — fixed" held only for
  start-then-load. `loadStyle()` before `start()` (the documented order) was undone by
  `EngineSession::boot()` forcing 480 PPQN. Fixed in `de660a1`; `57d20d2` makes Start
  re-adopt the loaded style's grid after a running style swap.
- Hostile SMF (division 1, meter 1/16) divided by a zero-tick bar in the mapper: `d85831e`.
- `test_latency_stability` had a test-harness data race (read the tap's vector while the
  dispatch thread could still write it): `555ad3a`. Not a product race.

**Build/test matrix (HEAD of the integration branch, macOS arm64):**

| Configuration | Result |
|---|---|
| Release, Debug, `-DAIARR_LATENCY_TRACE=ON` | **98/98** each (95 C++ incl. 3 Apple-only + pure-C ABI + Python) |
| ASan+UBSan (`-fno-sanitize-recover=undefined`) | **98/98**, 0 sanitizer reports |
| TSan | **98/98** ×3 full runs, 0 TSan warnings |
| `-DBUILD_MACOS_APP=ON` (JUCE 8.0.4) | builds (only JUCE-internal + 2 deprecated-`Font` warnings); launches, alive 20 s, ~107 MB RSS, no crash report. **GUI not exercised.** |

**Real Genos playback through `LiveEngineFacade`** (`test_genos_facade_runtime`, all 4
corpus styles, 1920 PPQN, one 6/8): Intro A → Main A (auto) → Main A loops → Variation B →
Fill In BB → Main B → Break (Fill In BA) → Main B → Ending A → transport stops; every switch
on a bar boundary; NoteOn == NoteOff and 0 outstanding notes at the end of every run.
Playback tempo: imported from SMF Set Tempo and persisted in UASF v1.1 (Issue #2; corpus 100 / 90 / 105 / 76 BPM).

**Still not verified:** any MIDI hardware (CoreMIDI in/out exist in
`src/engine/midi/coremidi_{in,out}.cpp`; there is no Pa5X-specific code), GUI interaction,
iOS/iPad (no target exists), GitHub Actions (0 runs recorded on `phamgia75/AIArranger`).

---

## ▶ CURRENT STATUS — audit of 2026-09-28 (supersedes the status claims below)

> Branch `claude/aiarranger-audit-handoff-fyv4vy` (19 commits on top of `main@5e72ee6`, the
> Gate 4 merge, PR #32). Method: fresh clone, git history and branches, full doc read, clean
> builds, full CTest in 4 configurations, sanitizers (never used on this repo before), a
> parser mini-fuzz, an independent SMF reference decoder, and per-bar output measurement through
> `LiveEngineFacade`. Environment: **Linux x86_64, GCC 13 / Clang 18, no macOS, no Xcode,
> no MIDI hardware**. The sections after this one (dated 2026-07-11) are the historical baseline.

### 1. Executive status

The repo is a **C++20 engine-first arranger**: a vendor-agnostic realtime core, a Yamaha SFF1
importer, a UASF format, a headless app shell, a JUCE macOS reference host, and a frozen Gate 4
engine/product contract with a C ABI. There is **still no iPad/iOS app and no Xcode project**
(Gate 5 has not started), and **nothing has been validated on hardware** (`hardware_validated:false`).

This audit found that the "play a Yamaha style live" path was broken in several places that the
existing tests did not cover. **All of them are now fixed, each with a reproducer test:**
- imported styles were time-stretched about 15× (SysEx decode);
- a style played its Intro and then **fell silent**, because no section ever looped or handed over (spec §5 was never implemented);
- an imported `.sty` was a single section, so Intro, Variation, Fill, Break and Ending were unaddressable;
- 1920-PPQN imports ran 4× too slow through the facade;
- Variation A–D pointed at raw section indexes (A reached the Intro, D the Ending);
- 6/8 styles ran on a 4/4 grid;
- the Gate 4 "commands from any thread" was a data race, and queue overflow dropped NoteOffs (stuck chord);
- plus UASF, importer and session hardening.

**GitHub Actions is currently not running any job** (see §5), so CI status for this branch is
**unverified**, and macOS was not built this session.

**Prompt items that do not exist in this repository** (not on any branch; only strategy docs
mention the PA5X): the Audio Intelligence / MIR pipeline (MP3/WAV → analysis → Performance Model →
Style), the `.air` format, beat/tempo detection, Fill1 generation from audio, `aiarr_engine_load_style`,
CMake install/export, a shared library with symbol visibility, a Pa5X USB-MIDI path, and any
iPad/iOS/Xcode target. If that work exists, it is in another repository or on a machine that was
never pushed. **Ask the owner for it; do not assume it.**

### 2. Verified completed (repo + test evidence, this session)

| Subsystem | Evidence | Validation |
|---|---|---|
| Portable core builds off-Apple | `5f1ce4f` | Linux GCC 13: configure + build, 0 errors |
| Full hardware-independent suite | 93 CTest entries (91 C++ + 1 Python + 1 pure-C) | **93/93** in release, ASan+UBSan, TSan and trace-ON |
| Live style playback per spec §5 (Intro→Main, Main loop, Fill/Break return, Ending stop) | `99e6a05`, `test_section_lifecycle` (29 checks, including through the facade) | pass; loop repeat is exact; notes balanced |
| SFF1 import: timing, sections, meter, CASM roles | `0a65b27`, `f58b3ac`, `0997a87`, `a6e6955`; `test_sff1_smf_decode` (identical to an independent decoder on 4 Genos styles), `test_time_signature`, `test_sff1_mapper_casm_ntr` | pass |
| SFF1 hostile input | `ae9af06`, `test_sff1_reader_malformed` + mini-fuzz | no sanitizer finding |
| UASF read/write/validate + golden gate | `3c12f80`, `5f2e8b9`, `f94839c`; `test_uasf_*` | pass; golden files are now actually tested (ADR-015) |
| Gate 4 contract: lifecycle, facade, C ABI | existing tests + `test_bridge_c_consumer` (strict C11), `test_facade_queue_integrity`, `test_snapshot_seqlock` | pass; TSan clean with 0 warnings |
| Variation A–D → Main A–D; Fill of the playing Main | `f58b3ac` | tested via the facade on the demo style and POP_ACOUSTIC_2 |
| Tempo ownership (Start keeps the performer's tempo) | `fd18ad4`, `test_style_player_tempo` | pass |
| Session JSON range safety | `bbfdcba` | pass |

### 3. Fixed this session (one commit per bug, each with a test that failed first)

| Sev | Bug | Commit |
|---|---|---|
| **P0** | No section lifecycle: after Start the demo style played its 4-bar intro, then silence forever (facade-measured). Main never looped, Fill/Intro never returned, Ending never stopped. Sections were also anchored one engine tick late. | `99e6a05` |
| **P0** | SMF decode: SysEx payload read as delta-times (styles about 15× too long); EoT and truncated messages emitted as events | `0a65b27` |
| **P0** | Imported `.sty` = one section, so Intro/Main A–D/Fill/Break/Ending were unaddressable. Variation A–D mapped to indexes 0–3 (A = Intro). | `f58b3ac` (+ `7e5a04c`) |
| **P0** | Clock stayed at 480 PPQN for 1920-PPQN imports (4× slow via the facade) | `99e6a05` |
| **P0** | Facade SPSC command queue under "any thread" contract → data race; tick drain dropped overflow (stuck chord) | `af540ac` |
| **P0** | Hardware-checklist `.uasf` fixtures stale (res 7, 1 track, stretched) | `e68b9e4`, regenerated again with sections/meter |
| **P1** | 6/8 styles on a 4/4 grid (`ticksPerBar` ignored `beat_note`) | `0997a87` |
| **P1** | UASF serializer: unsorted tracks lost timing after save/load (demo drums went silent); the event cap corrupted following tracks | `5f2e8b9` |
| **P1** | SFF1 hostile input: CSEG recursion stack overflow, 219× memory amplification leading to terminate, `memcpy(nullptr)` | `ae9af06` |
| **P1** | Start reset tempo to the style tempo | `fd18ad4` |
| **P1** | Mutex-backed snapshot publish on the tick path (the contract allows an audio-thread tick) | `618a0a6` |
| **P1** | Mapper gave CASM tracks another section's NTR/NTT (C_WHISPER: 6/62) | `a6e6955` |
| **P1** | Validator rejected every real Genos import (res ≤ 960); golden files were never tested | `f94839c` |
| **P2** | UASF deserializer ignored `kMax*` limits | `3c12f80` |
| **P2** | Session JSON wrapped out-of-range numbers into valid ones | `bbfdcba` |
| **P2** | Performer FSM stayed Playing after an Ending stopped the transport | `48c4d8b` |
| infra | Linux CI job (release/ASan+UBSan/TSan), CI on `claude/**`, pure-C ABI test, Python test in CTest | `5f1ce4f`, `be456b3` |

### 4. In progress (code exists, not "done")

- **macOS JUCE reference host** (`apps/macos`): builds only with `-DBUILD_MACOS_APP=ON` on a Mac.
  **Not built this session**, and not built by CI.
- **C ABI bridge**: a skeleton only. There is no style loading, MIDI binding, input injection or
  suspend/resume through the ABI (deferred to Gate 5 by the contract §7 *and* by the owner's
  2026-07-11 direction: no bridge/Swift work until Gate 3 passes on hardware).
- **SFF2**: detection only. The corpus is 4 styles; no compatibility percentage is proven.
- **Imported styles**: the Intro/Ending **B/C variants** are imported when the file marks them; one
  corpus file (C_WHISPER) has about 9 unmarked bars after "Fill In BA" that end up in the Break.
  Auto-fill on variation change (spec §5.3 `autoFill`) is not implemented.

### 5. Known issues

| Issue | Sev | Evidence | Impact | Proposed fix |
|---|---|---|---|---|
| GitHub Actions jobs never start | **Blocker (external)** | Runs 50–52+ on this branch: all jobs (including the unchanged macOS job) "failure" after 3–8 s, job logs HTTP 404. `main` last passed 2026-07-11 (run 49). | No CI signal; the macOS build is unverified | Owner: repo/account Settings → Actions (enabled? billing/spending limit/payment), then re-run the latest run |
| macOS build of this branch unverified | P1 | No Mac here | Apple-side files touched: `clock.cpp`/`clock.h` (mach include moved out of the header), `coreaudio_driver.cpp` (explicit include), `coreaudio_clock.cpp`, `LiveHostDriver.h` (comment) | `bash scripts/run_tests.sh` on macOS (expect **96**: 93 + 3 Apple-only), then `-DBUILD_MACOS_APP=ON` |
| Musical behaviour changed materially | P1 (validation) | sections loop/hand over; imports split, timed and metered correctly | Every earlier listening impression of imported styles is void | Re-run the Gate 3 hardware checklist (A.1, item 44) |
| Spec vs implementation: PPQ | Decision | uasf-spec-v0.9 says "normalise to 480"; importer keeps the source 1920 and the validator now accepts ≤ 1920 | None at runtime | Owner: pick one and update the spec or the importer |
| `Sff1Reader::parseSection` fabricates 12 empty tracks | P3 | stub `parseTrack` | Report noise (bounded by the chunk cap) | Remove the stub loop |
| Duplicate clock/groove subsystems; JUCE `EngineDriver` still on a 1 ms timer | P3 | historical §H | Drift | Deliberate consolidation gate |

### 6. Pending validation (cannot be done here)

- **Gate 3 hardware checklist** (`docs/gate-plans/GATE3_HARDWARE_VALIDATION_CHECKLIST.md`), on real
  keyboard + module. Start with A.1 (load a `.sty`, Start) and item 44 (Intro, Variation A–D, Fill,
  Break, Ending): all of these behave differently, and now correctly per spec, after this session.
- Gate 9/10B Korg checklists: use the regenerated `.uasf` files (sectioned, 1920 PPQN).
- iPad device testing: blocked, since no iOS target exists.
- SFF1/SFF2 at-scale compatibility: needs a larger, legally-owned corpus.

### 7. Build/test matrix (this session, Linux x86_64)

| Command | Result |
|---|---|
| `cmake -S . -B b -G Ninja && cmake --build b && ctest --test-dir b` (GCC 13) | **93/93 pass** |
| same with `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined"` | **93/93 pass** |
| same with `-fsanitize=thread` | **93/93 pass**, 0 TSan reports, 0 `-Wtsan` warnings |
| same with `-DAIARR_LATENCY_TRACE=ON` | **93/93 pass** |
| `test_snapshot_seqlock` ×60 ASan + ×60 release + ×10 TSan | 0 failures |
| mini-fuzz (SFF1 corpus seeds, UASF, MIDI-in, session JSON) | no sanitizer finding after the fixes |
| macOS `scripts/run_tests.sh`, `-DBUILD_MACOS_APP=ON`, iOS/Xcode | **NOT RUN** (no macOS; no Xcode project exists; CI not starting) |

Apple-only tests not built on Linux: `test_coremidi_out`, `test_latency_stability`,
`test_midi_output_provider` (plus the `korg-playback` tool).

### 8. Recommended next work

**P0 — blockers**
1. *(Owner)* Restore GitHub Actions (§5), re-run the latest run on this branch, and require all 4
   jobs green. Then open a PR from this branch to `main`.

**P1 — next milestone (Gate 3 hardware)**
2. On a Mac: `bash scripts/run_tests.sh` (expect 96/96), then
   `cmake -B build -DBUILD_MACOS_APP=ON && cmake --build build --target aiarranger-macos`.
   Fix anything in the Apple-touching files listed in §5.
3. Gate 3 hardware checklist with a real keyboard and module (A.1, 44 first).
4. Auto-fill on variation change (spec §5.3): when enabled, Main X→Y inserts the Fill of X, then
   commits Y. The hooks are `SectionSequencer::queueFill` (now picks the playing Main's fill) and
   `StylePlayer::handleSectionEnd` (fill → return target). Add the target to the fill return, test
   headless.
5. Gate 5 ABI surface, only once the owner lifts the Gate 3 hold: `aiarr_engine_load_style_uasf`,
   `aiarr_engine_post_midi`, a Swift module map. Test from `test_bridge_c_consumer.c`.

**P2 — hardening**
6. Time-boxed parser fuzzing in CI (a corpus-mutation test).
7. Decide the PPQ spec question (§5) and align the spec, importer and validator.

**P3 — cleanup / future**
8. Consolidate the duplicate clock/groove trees; move `EngineDriver` to `CoreAudioClock`.
9. Remove the `parseSection` stub; move the Apple adapters to `src/platform/apple/`.

---

## (Historical) Review of 2026-07-11


> Author: audit session (autonomous). Date: **2026-07-11**.
> Method: full documentation read + source architecture map + fresh build/test
> run + BRD↔implementation traceability. Every claim below is backed by concrete
> evidence (file:line, command output, or commit). Nothing is inferred from
> README/old reports alone.

---

## DIRECTION UPDATE (PTH, 2026-07-11) — supersedes the "iOS-first absolute" framing below

**Mac-first for the ENGINE / integration / verification environment; iPad-first
for the FINAL PRODUCT.** The macOS JUCE app becomes the **live-performance
reference host + integration harness** used to get the engine's live loop correct
and measured before any iPad UI is built. The final shippable product remains the
**iPad SwiftUI app** — deferred to Gate 5. No SwiftUI / Swift-C++ bridge / iOS
lifecycle / signing work happens until the engine live loop is proven on the Mac
host. The already-landed **Phase-1 platform split** and **LiveEngineFacade** are
kept (they satisfy the Gate-4 portability contract).

**Gate roadmap (this supersedes §K's ordering):**
1. **Gate 1 — MIDI-input PR** ✅ ([PR #26](https://github.com/hieucat75/AIArranger/pull/26)).
2. **Gate 2 — Mac live-performance reference host (JUCE)**: real keyboard → MIDI
   in → parser/router → chord → EngineSession → StylePlayer → CoreMIDI out →
   external device. Minimal macOS UI (input/output pick, SFF1 load, chord,
   transport, Intro/Variation/Fill/Break/Ending, Sync Start, track mute, panic,
   device/error status). Reference implementation, NOT the product. See
   `docs/gate-plans/GATE_2_MAC_REFERENCE_HOST_PLAN.md`.
3. **Gate 3 — Live correctness**: input→engine wiring, chord latch ~80 ms,
   sustain/hold, CC123/panic, disconnect/reconnect, no stuck notes, bar-boundary
   transitions, real-keyboard tests, 30–60 min soak, real MIDI-timing measurement.
   No wide 2-clock/2-groove refactor unless evidence shows it blocks the live loop.
4. **Gate 4 — Freeze the engine contract for iPad** (public API, command/event
   model, snapshot, thread ownership, MIDI in/out abstraction, callback rules,
   lifecycle, error reporting, bridge + platform-adapter boundary). Portable core
   stays Apple-framework-free; CoreMIDI in the platform adapter (started Phase 1).
5. **Gate 5 — iPad SwiftUI** (final product): only after Gates 2–4 pass acceptance.
   `docs/gate-plans/IOS_THIN_VERTICAL_SLICE_PLAN.md` is **deferred to Gate 5**.

---

## A. EXECUTIVE SUMMARY

AI Arranger is a **live-performance arranger engine**. After 15 disciplined
"gates" it has a **mature, vendor-agnostic C++20 realtime arranger + MIDI-out
engine** and a **Yamaha SFF1 importer**, plus a **local-only JUCE macOS prototype
UI**. The engine builds clean (**0 errors / 0 warnings**) and **71/71 test
binaries pass** on a fresh build (verified this session).

The project is **strong on its #1–#2 priorities** (timing + MIDI-out
correctness, per the Master Delivery Plan's "Tower of Time" doctrine) and
**honest about what it has not proven** (every report emits
`hardware_validated:false`; no Yamaha/Korg compatibility is claimed).

The single most important **reality gap vs the BRD**: the BRD/roadmap describe an
**iPad-first SwiftUI product on TestFlight by Q2 2026 / App Store Q3 2026**. Today
(2026-07-11) there is **no iOS/iPadOS app, no SwiftUI, no Xcode project, and no
internal sound** — execution went **engine-first on macOS/JUCE**. This is a
defensible engineering choice (the hard part — the realtime engine — is portable
C++), but the **business timeline in the BRD is not met** and the **live MIDI
INPUT path is missing**, so the app is **not yet a playable end-to-end product**.

**Selected first task (implemented this session):** complete the **missing MIDI
INPUT half** of MIDI I/O — an RT-safe asynchronous note/sustain input path + pure
MIDI parser + input-provider abstraction + router, wiring the already-tested chord
detection so a real keyboard can drive the arranger. This is the symmetric
counterpart to Gate 14C (MIDI output) and the most valuable in-scope,
headless-verifiable vertical slice. See §L–§N.

---

## B. PRODUCT UNDERSTANDING (source of truth: BRD + Master Delivery Plan + Korg strategy)

- **Who:** gigging keyboard players (wedding/restaurant/church), 28–50, who own
  large Yamaha `.STY` libraries and MIDI keyboards/sound modules (`brd-v0.9.md`
  §2–3).
- **Value / moat:** import your existing Yamaha style library + live-first UX +
  open (vendor-neutral) architecture. "The Loopy Pro of the arranger world"
  (`live-arranger-strategy.md` §1).
- **Explicitly NOT:** a DAW, a composer/AI generator, a style editor, or a sound
  module (Master Plan DOCTRINE 02/11).
- **Sound strategy (ADR-013, governing):** Phase 1 value is **musical workflow via
  MIDI-out to external devices**, NOT sound generation. The bundled GM bank is a
  degraded *preview* only and "MUST NOT be marketed as a sound engine."
- **Identity evolution (Korg-first strategy, 2026-06-14, governing):** a
  **vendor-agnostic realtime arranger platform**; runtime speaks only UASF + MIDI;
  Yamaha lives in the importer. First **hardware-validation** target = Korg
  PA700/PA1000 (import stays Yamaha-first; Korg *import* is Phase 2).
- **Non-negotiable ship gate:** section-switch timing error ≤ 2 ticks @480 PPQ,
  120 BPM (DOCTRINE 01/10).

---

## C. REPOSITORY MAP

```
brd / prd / roadmap / *-spec-v0.9.md   ← v0.9 spec set (2026-06-13, Vietnamese)
docs/implementation/AI_ARRANGER_MASTER_DELIVERY_PLAN.md  ← v2.0 governing plan (ADR-013)
docs/strategy/KORG_FIRST_VALIDATION_STRATEGY.md          ← 2026-06-14 governing pivot
docs/gate-plans/GATE_*_{PLAN,HANDOFF}.md                 ← per-gate plans + close-outs (G1–G15)
docs/architecture/{adr,UASF_*}                           ← ADR-013/015 + UASF policies
docs/{governance,policy,importers}/                      ← claims policy, review policy, SFF1 notes
.openclaw/{handoff,review}/                              ← HANDOFF_MASTER + codex/musical reviews
src/engine/{uasf,realtime,midi,arranger,music,articulation,chord,control,performance,integration}
                                                         ← vendor-agnostic C++ engine (~8.3k LOC)
src/importers/sff1/                                      ← Yamaha SFF1 importer (isolated, ~1.3k LOC)
src/{midi,control,session,ui,performance}/               ← app-shell modules (Gate 14/14C/15), headless
apps/macos/Source/                                       ← JUCE GUI prototype (CI-optional, local only)
tools/{sff1_parser_cli,korg_playback_cli}.cpp, tools/korg_validation/  ← CLI tools + synthetic harness
tests/{engine,realtime,integration,ui,midi,performance}/ ← 65 test files, one binary each
corpus/yamaha/sff1/                                      ← 4 real Genos .S718 styles (+ .uasf/.meta)
fixtures/korg/{PA700,PA1000}/                            ← EMPTY (.gitkeep) — awaiting hardware capture
```

Runnable artifacts: `sff1-parser`, `korg-playback`, `korg-validate`,
`timing-benchmark`, `aiarranger-macos` (JUCE, `-DBUILD_MACOS_APP=ON`), and ~64
CTest binaries. `arranger-engine` + `importer-sff1` are static libs.

---

## D. ACTUAL ARCHITECTURE (verified from code)

**Layering (enforced by CMake targets):** `importer-sff1 → arranger-engine`
(one-way). Verified: **no file under `src/engine` includes anything from
`src/importers`**; the only crossing is `sff1_mapper.h → engine/uasf/types.h`.
Vendor isolation (DOCTRINE 05 / tech-arch P2) is **real and enforced**.

**JUCE isolation:** `grep juce:: src/` = **none**. JUCE appears only in
`apps/macos`. The entire engine + headless shell is JUCE-free.

**Realtime path (RT-safe, lock-free):** `RealtimeClock` (sample→tick, `mach_time`)
→ `StylePlayer::tick()` → `SectionSequencer` (bar authority) → NTR/NTT transform +
groove + articulation → `MidiScheduler` (SPSC `LockFreeQueue`) → `CoreMidiOut`
(producer RT-safe; dedicated dispatch thread does `MIDISend`). `PanicHandler`,
`PerformerAdapter`, performer FSM all lock-free.

**Data model:** `uasf::StyleDefinition` (POD) is the universal leaf everything
depends on. UASF serialize/deserialize/validate is complete with version checks.

**Deviation from tech-arch doc:** tech-arch §5 requires "Core Engine modules
import **no** Apple framework" (for future Windows portability). In reality the
engine binds directly to `<mach/mach_time.h>`, `<AudioToolbox>`, `<CoreMIDI>`
inside `src/engine/*` (e.g. `engine/realtime/clock.h`,
`engine/midi/coremidi_out.h`). The engine is therefore **macOS/iOS-locked** (still
vendor-agnostic). Acceptable for Phase 1 (iOS/macOS only); a portability debt for
Phase 3. See §H.

---

## E. BUILD & TEST STATUS (verified this session)

| Item | Result |
|---|---|
| Toolchain | CMake 4.3.3, Apple clang 17.0.0, C++20, macOS 27.0 |
| Configure | OK (1.4s), `BUILD_MACOS_APP=OFF` (default) |
| Build (fresh scratch dir) | **124 objects, 0 errors, 0 warnings** |
| Tests | `ctest` → **71/71 passed, 0 failed** (6.1s) |
| Pre-existing `build/` dir | **STALE** (Jun 14; Gate 15 sources are Jun 22). `file(GLOB)` means it would miss new files — must reconfigure. |

Could **not** verify (documented, not a defect): JUCE GUI app (needs
`BUILD_MACOS_APP=ON` + network JUCE fetch); real MIDI/audio hardware I/O
(synthetic-only by design; `hardware_validated:false`).

---

## F. BRD ↔ IMPLEMENTATION TRACEABILITY MATRIX

Legend: ✅ done+tested · 🟡 partial · 🟥 spec-only (no code) · ⚙️ engine-only
(no product/UI/hardware).

| # | Requirement (source) | Code status | Test | Completion | Gap |
|---|---|---|---|---|---|
| 1 | Sample-accurate audio clock, no OS timers (tech-arch ADR-005) | ✅ `engine/realtime/clock` | ✅ timing tests | ✅ | JUCE app driver still uses 1ms timer (G15 residual) |
| 2 | Section state machine, quantized switch ≤2 tick (DOCTRINE 01) | ✅ `section_sequencer`, `style_player` | ✅ | ✅ (synthetic) | Not measured on hardware |
| 3 | NTR/NTT chord transform (arranger-spec §6–7) | ✅ `engine/music/ntt` (Bass/Chord/Melody + minors + Dorian) | ✅ | 🟡 | Guitar NTR + SFF2 ext → RootTransposition fallback (`ntt.h:34`) |
| 4 | Retrigger policies (6) | 🟡 in `style_player` | ✅ partial | 🟡 | Not all 6 policies fully surfaced |
| 5 | Swing / groove humanization | ✅ `engine/music/groove` + `performance/groove` | ✅ | 🟡 | **Two parallel impls** (see §H); per-style swing % un-tuned |
| 6 | Chord recognition: fingered / on-bass / single-finger (chord-spec) | ✅ `engine/chord/*` detectors | ✅ | 🟡 | AI-fingered is a labeled placeholder; latch(80ms)/hold-mode not implemented |
| 7 | **Live chord input from external MIDI keyboard** (midi-spec §22) | 🟥 **glue exists but no MIDI-in source; not RT-safe async** | — | 🟥 | **Addressed by this session's task — see §L** |
| 8 | MIDI OUTPUT (USB/virtual), hot-plug, panic (midi-spec, G14C) | ✅ `engine/midi/coremidi_out` (real `MIDIClientCreate/MIDISend`) + provider/manager | ✅ | ⚙️ | DAW/hardware verify pending PTH |
| 9 | MIDI clock out (24 PPQN) | 🟡 scheduler supports clock | 🟡 | 🟡 | Not fully wired/tested end-to-end |
| 10 | UASF format (.uas zip, manifest, validation V01–V07) | ✅ `engine/uasf/*` | ✅ | 🟡 | UASF spec doc behind ADR-013 (articulation fields) |
| 11 | SFF1 import (magic→sections→CASM→Ctab, all fields) | ✅ `importers/sff1` (489-LOC reader, offsets validated vs 4 real Genos) | ✅ | 🟡 | Corpus is 4 styles, not 500; ≥95% claim unproven at scale |
| 12 | SFF2 import (Ctb2 multi-range, ext NTT, MegaVoice) | 🟡 **detection only** in sff1_reader; no full SFF2 parser | 🟡 | 🟥 | No SFF2 corpus fixtures; ≥75% claim unproven |
| 13 | Articulation metadata (ADR-013) | 🟡 `ArticulationMetadata` type + `articulation/renderer` (Naive/Keyswitch) | ✅ | 🟡 | MegaVoice detection matrix not built |
| 14 | Live UX: section pads, chord display, panic, perf mode (WS-06) | 🟡 **JUCE macOS prototype only** (`apps/macos`) + headless VMs | ✅ (VMs) | 🟥 (as iPad product) | No SwiftUI, no iPad, local-build only |
| 15 | Internal sound (sfizz + GM preview) — Phase 1 shallow | 🟥 **none** | — | 🟥 | Deliberately deferred (external MIDI is Phase 1 path) |
| 16 | Style browser / library (SQLite/GRDB) | 🟥 none (Style browser is a JUCE panel reading UASF from disk) | — | 🟥 | No library DB |
| 17 | Setlist / performance persistence | 🟡 `session/session_persistence` (JSON) | ✅ | 🟡 | Not the full setlist/next-song-preload UX |
| 18 | Korg validation harness (pre-hardware) | ✅ `tools/korg_validation` (synthetic) | ✅ | ⚙️ | Awaiting real PA capture (`fixtures/korg/*` empty) |
| 19 | Hardware/musical validation (Yamaha/Korg) | 🟥 DEFERRED_BY_PTH | — | 🟥 | Needs hardware — out of this environment |

---

## G. WHAT IS DONE (evidence-backed)

- **Vendor-agnostic realtime engine** (timing, sections, NTR/NTT, groove,
  articulation, panic, performer FSM/sync/fills/variations/split) — mature,
  lock-free, deterministic, headless-tested.
- **Real CoreMIDI output** with dedicated dispatch thread + hot-plug + provider
  abstraction (Gate 14C).
- **UASF** read/write/validate + **SFF1 importer** validated against 4 real Genos
  `.S718` files.
- **Synthetic Korg validation harness** (timing differential, jitter, transitions,
  chord latency, panic) — reactivatable the day hardware arrives.
- **Headless app-shell** (session lifecycle, async UI↔engine bridge, ViewModels,
  MIDI output manager/bridge, snapshot + session persistence) + **JUCE macOS
  prototype** that builds locally.
- **Zero TODO/FIXME markers** in the entire tree; one clearly-labeled placeholder
  (`ai_fingered_placeholder.h`, deterministically falls back to Fingered).

---

## H. WHAT IS NOT DONE / TECHNICAL DEBT

**P0 (blocks a playable MVP):**
- **No live MIDI INPUT source** — the chord-aggregation glue exists in
  `PerformerAdapter` but nothing reads a real keyboard, and note input is **not
  RT-safe for an async input thread** (bypasses the SPSC queue). *(Addressed this
  session — §L.)*
- **No iOS/iPadOS app, no SwiftUI, no Xcode project** — the BRD product does not
  exist yet as a shippable target. Only a local macOS JUCE prototype.
- **No internal sound** (intentionally deferred; external MIDI is the Phase-1 sound
  path — acceptable, but means the app is unusable without external gear today).

**P1:**
- **SFF2**: detection only, no real parser; **no SFF2 corpus** → the ≥75% claim is
  unproven. SFF1 ≥95% also unproven (corpus is 4 styles, not 500).
- **Chord latch (80ms) / hold-mode (CC64)** from chord-spec §6 not implemented.
- **MIDI clock-out** not wired end-to-end.
- **No library DB / style browser / setlist** product surface (SQLite/GRDB per
  tech-arch never built).

**P2 (architecture drift / debt, do NOT rewrite reactively):**
- **Duplicate/parallel subsystems** from Gate 15: two clock abstractions
  (`engine/realtime/clock` vs `performance/clock/{realtime,coreaudio,synthetic}`)
  and two groove impls (`engine/music/groove::applySwing` vs
  `performance/groove/groove_engine`). Functional but conceptually overlapping;
  consolidate deliberately later, not ad hoc.
- **Engine imports Apple frameworks directly** (`src/engine/*`), violating
  tech-arch §5's portability rule. Fine for iOS/macOS; debt for Phase-3 Windows.
- **Gate 15 residual:** `EngineDriver` (JUCE app) still uses a 1ms
  `HighResolutionTimer` instead of the new `CoreAudioClock`; stage-mode/theme
  toggle in-app wiring incomplete.
- Stale `build/` dir (must reconfigure due to `file(GLOB)`).

---

## I. DOCUMENT ↔ CODE CONTRADICTIONS

| Topic | Doc says | Code/reality | Source of truth | Action |
|---|---|---|---|---|
| Platform | iPad-first, **SwiftUI = all UI**, JUCE = audio only (tech-arch ADR-001, DOCTRINE 03) | macOS **JUCE UI**, no SwiftUI, no iPad | BRD/Master Plan (iPad-first) is the product truth; code is an engine-first prototype | Update docs to state the current phase is "engine + macOS prototype; iOS UI not yet started" |
| Engine portability | Engine imports no Apple framework (tech-arch §5) | Engine imports CoreMIDI/CoreAudio/mach directly | Reality (macOS-locked) | Note as accepted Phase-1 debt |
| Internal sound | Real (shallow) P1 MVP feature (sound-engine-spec, PRD) | Not built; ADR-013 downgrades to "degraded preview only" | **ADR-013 / Master Plan** (newer) | Reconcile old specs to ADR-013 |
| MegaVoice | "skip/warning" (file-import-strategy, uasf-spec) | ADR-013 = "detect & preserve, never silent GM" | **ADR-013** | Update uasf-spec §11 |
| Korg | Older docs Yamaha-only | Korg-first *validation* strategy adopted 2026-06-14 | **Korg strategy** (import stays Yamaha-first) | Already governing; older docs lag |
| Korg harness | Strategy doc: "not implemented yet" | **Implemented** in Gate 11 (`tools/korg_validation`) | Code | Update strategy doc §"Required Infrastructure" |
| Gate numbering | Master Plan Gate 0→10 (Gate 5 = iPad UX) | Actual G1→G15 diverged (G14/15 = macOS prototype) | Actual git history | Add a gate-mapping note |
| `HANDOFF_MASTER.md` header | "Gate 7 … main = Gate 9" | main = **Gate 15** (PR #25 merged) | git log | Refresh the top-of-file snapshot |

---

## J. MVP READINESS ASSESSMENT (candid)

- **Real phase:** **engine + importer + macOS prototype**, NOT a shippable
  iOS/iPadOS MVP. Roughly the Master Plan's Gates 1–4 (engine/importer) done
  well, Gate 5 (iPad UX) **not started**, Gate 6 (hardware) **deferred**.
- **What genuinely runs:** the realtime engine + MIDI-out + SFF1 import, headless
  and (locally) in a JUCE window. Timing/RT-safety/determinism are real and tested.
- **Skeleton / prototype / rewrite-risk:** the macOS JUCE UI is a validation
  prototype, not the product UI (the product is iPad/SwiftUI); the two
  clock/groove trees are drift; SFF2 is detection-only.
- **Biggest risks to MVP / live use:**
  1. **No MIDI input** → cannot actually play live (top gap; started this session).
  2. **No iOS product** → BRD business timeline (TestFlight Q2 / launch Q3 2026)
     already missed; the largest scope item (SwiftUI iPad app) is untouched.
  3. **Compatibility unproven** — no hardware, tiny corpus; claims correctly
     withheld, but "import your library" moat is unvalidated at scale.
  4. **Architecture drift** (dup clock/groove) will compound if not consolidated.

---

## K. CONTINUATION PLAN (prioritized workstreams)

Priority order follows the Master Plan doctrine: correctness → timing/live-safety
→ import correctness → live UI → product surface. **Do not** start the internal
sound engine (Phase 2) or claim hardware compatibility (needs PTH + a device).

1. **MIDI INPUT path (this session's task)** — RT-safe async note/sustain input +
   parser + provider + router, then a real CoreMIDI input endpoint. *In progress.*
   Acceptance: bytes→chord end-to-end headless; panic/sustain handled; existing
   tests still green. Risk: low. MVP: **yes (core)**.
2. **Chord latch + hold-mode (CC64)** — implement chord-spec §6 (80ms release
   latch, sustain-hold). Fully headless-testable. MVP: yes. Complexity: low.
3. **Wire EngineDriver → CoreAudioClock** (close G15 residual) + consolidate the
   two clock abstractions behind one interface. MVP: timing polish. Complexity:
   low-med (touches JUCE app; test locally).
4. **SFF2 real parser + corpus** — build a real SFF2/Ctb2 parser and gather a
   legally-owned SFF2 corpus **before** claiming ≥75%. MVP: import moat.
   Complexity: high. Blocker: corpus.
5. **iOS/iPadOS SwiftUI shell** — the actual BRD product: Xcode project, Swift/C++
   bridge over `arranger-engine`, Live Screen (section pads, chord display, panic),
   CoreMIDI on iOS. This is the largest missing MVP piece. Complexity: high.
   **Decision needed** (see §O) — start iOS now vs finish engine live-loop first.
6. **Library/setlist product surface** (SQLite/GRDB) — Phase-1 UX. Complexity: med.
7. **Hardware/Korg validation** — DEFERRED_BY_PTH; needs a PA700/PA1000.
8. **Consolidate duplicate groove/clock trees** — deliberate refactor, not ad hoc.
9. **Internal sound (Phase 2)** — only after MIDI-out MVP is solid (DOCTRINE 10).

---

## L. SELECTED TASK — MIDI INPUT (rationale)

**Why this one:** MIDI I/O is the doctrine's priority #2, and **output is done
(G14C) while input is entirely missing** — yet the arranger is a *live* instrument
that is useless without a keyboard driving chords. The chord detectors +
`PerformerAdapter` aggregation already exist and are tested, but (a) **no code
reads a real MIDI source**, and (b) `PerformerAdapter::noteOn/noteOff` mutate
state **synchronously on the caller thread**, so they are **not safe to call from
an async CoreMIDI input thread** (control events already go through an SPSC queue;
note input bypasses it). This task closes both, as the **symmetric counterpart to
Gate 14C**, using existing seams, additively (no breaking change), and is
**fully headless-verifiable** except the CoreMIDI hardware endpoint (a thin edge,
consistent with how `coremidi_out` is treated).

**Which gap it closes:** matrix row #7 (live chord input) → from 🟥 to ⚙️
(engine-side complete; hardware endpoint pending PTH like output).

**Definition of done:** raw MIDI bytes → typed messages → router → RT-safe SPSC
queue → existing chord aggregation → `StylePlayer::setChord`, with NoteOn(vel0)=
NoteOff, CC64 sustain-hold, CC123 panic; all covered by new headless tests; the
full 71-binary suite still green; no engine-core behaviour change for existing
callers.

---

## M. CHANGES IMPLEMENTED (this session)

Vertical slice: **raw MIDI input bytes → chord → StylePlayer**, RT-safe and
headless-verified. All additive; no existing behaviour changed.

| File | Change |
|---|---|
| `src/engine/control/control_events.h` | Appended `NoteOn`, `NoteOff`, `Sustain` to `ControlAction` (stable values) so raw note/sustain input flows through the existing lock-free SPSC queue. |
| `src/engine/integration/performer_adapter.{h,cpp}` | `handleControl` now handles NoteOn/NoteOff/Sustain (drained in `tick()`); added **CC64 sustain-hold** (deferred note releases via `sustained_[]`), helpers `removeHeld/rememberSustained/forgetSustained`, `setSustain()`, and `heldCount()`/`sustain()` accessors. Public `noteOn/noteOff` unchanged for existing callers. |
| `src/midi/midi_input_parser.{h,cpp}` | **New.** Pure `parseMidiInput(bytes→MidiInputMessage[])` — running status, NoteOn-vel0→NoteOff, CC, SysEx/realtime skip, incomplete-trailing safe. No alloc/locks (symmetric to `midiEventToBytes`). |
| `src/midi/midi_input_router.h` | **New.** Header-only `routeMidiInput/routeMidiBytes<Sink>`: NoteOn/Off→held-note events, CC64→Sustain, CC123→Panic. Templated (RT-safe, testable). |
| `src/engine/midi/coremidi_in.{h,cpp}` | **New.** Real `CoreMidiIn` source (MIDIClientCreate + MIDIInputPortCreate + connect + readProc→parse→sink), hot-plug-aware, headless-safe no-op with zero sources — the symmetric counterpart to `CoreMidiOut`. |
| `tests/midi/test_midi_input_parser.cpp`, `tests/integration/test_midi_input_to_chord.cpp` | **New** headless tests. |

Scope discipline: dropped a speculative `IMidiInputProvider` interface (no
consumer yet — YAGNI); the CoreMIDI source is used directly and a
manager/provider wrapper is left as a follow-up. The **CoreMIDI hardware
endpoint** compiles + links but is only fully exercisable with a real keyboard
(local, pending PTH) — exactly how `CoreMidiOut` is treated. Everything else is
CI-verified.

---

## N. TEST EVIDENCE (this session)

- Full suite (fresh reconfigure + build): **73/73 test binaries pass, 0 failed,
  0 compiler warnings** (was 71/71 before this task; +2 new).
- `test_midi_input_parser` — 24 assertions: NoteOn/Off decode, vel0→NoteOff,
  running status, CC64/CC123, realtime/SysEx skip, incomplete-trailing, `maxOut`
  cap, and router → ControlEvent mapping (including CC64→Sustain, CC123→Panic).
- `test_midi_input_to_chord` — 12 assertions, end-to-end via `EngineSession`:
  raw bytes C3-E3-G3 → `routeMidiBytes` → adapter `tick()` → **detected C major
  (root 48) reached the player**; CC64 sustain **holds notes after key release**,
  pedal-up releases them; normal release clears; upper-zone note ignored;
  NoteOn-vel0 counts as release.
- No regression: existing `test_performer_adapter`, `test_chord_control_event`,
  and all realtime/engine suites remain green.
- Not verified here (documented, needs hardware): real CoreMIDI keyboard capture
  through `CoreMidiIn` — `hardware_validated:false` remains the project stance.

---

## O. OPEN DECISIONS (need PTH — product/architecture level)

1. **iOS now, or engine live-loop first?** The BRD product is an **iPad SwiftUI
   app**, entirely unbuilt. Do we (a) start the Xcode/SwiftUI shell now over the
   existing engine, or (b) first finish the engine's live loop (MIDI in→arrange→
   MIDI out end-to-end on macOS) then port UI to iPad? Docs assume iPad-first; the
   execution has been engine-first. **This changes the next several gates.**
2. **Reconcile the v0.9 specs to ADR-013 / Korg strategy**, or leave them as
   historical? (internal-sound positioning, MegaVoice, articulation schema,
   Yamaha vs vendor-agnostic framing.)
3. **SFF2 corpus** — need legally-owned SFF2 style files before any SFF2
   compatibility work/claim. Who sources them?
4. **Consolidate the duplicate clock/groove subsystems** (Gate 15 drift) — worth a
   deliberate refactor gate?
5. **Hardware validation** — remains DEFERRED_BY_PTH; unblock requires a Korg
   PA700/PA1000 (out of this environment).

---

## P. RECOMMENDATIONS FOR NEXT SESSION

1. Finish the MIDI-input vertical loop: add a real CoreMIDI input endpoint test on
   hardware (PTH), then wire the input router into `EngineSession` + the JUCE app so
   a keyboard drives the arranger end-to-end.
2. Implement chord latch + CC64 hold-mode (small, high live-value).
3. Refresh stale docs (`HANDOFF_MASTER.md` header, Korg strategy "not implemented"
   note, tech-arch platform/portability claims) to match reality.
4. Get a **go/no-go from PTH on the iOS-vs-engine-first question (§O.1)** — it
   gates the largest remaining scope.
5. Keep the discipline: `hardware_validated:false`, no compatibility claims, tests
   before claims, no internal sound engine until MIDI-out MVP is solid.
