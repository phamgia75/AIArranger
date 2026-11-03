> **Current status lives in `docs/PROJECT_REVIEW_AND_CONTINUATION_PLAN.md` (top section,
> audit of 2026-09-28).** This file's Gate 7 header is historical.

# H A N D O F F   P A C K A G E
## AI Arranger / Style Maker — Gate 7 (CASM Semantic Analysis)
## Transfer to: Agent [TBD by PTH]

---

## Hardware Review Status

**HARDWARE_REVIEW_STATUS: DEFERRED_BY_PTH** (as of 2026-06-14)

- Manual Yamaha/Korg validation explicitly deferred by PTH.
- Hardware checklist + NTT calibration docs PRESERVED for future milestone
  (`docs/gate-plans/GATE_10B_HARDWARE_CHECKLIST.md`,
  `docs/gate-plans/GATE_10B_NTT_CALIBRATION.md`).
- Yamaha/Korg full validation tracked as: **Future Milestone — Hardware Parity
  Validation** (no claim of compatibility yet).
- Engine slice (Gate 10A + Gate 10B Engine) MERGED to main and CI-green;
  hardware parity NOT claimed.

## Strategic Direction (2026-06-14)

> Full strategy: `docs/strategy/KORG_FIRST_VALIDATION_STRATEGY.md`.

- **Project direction:** "Generic Yamaha-compatible arranger engine" →
  **"Modern realtime arranger platform with a KORG-first validation strategy"**
  (vendor-agnostic runtime; Yamaha import is a feature, not the identity).
- **Primary hardware target:** **Korg PA700, Korg PA1000** (NOT Yamaha
  Genos/Tyros).
- **NOT recommended initially:** Yamaha Genos, Yamaha Tyros, Korg PA5X, and other
  flagship-tier devices (cost / scarcity / over-spec for first validation).
- **Next pre-hardware infrastructure:** `tools/korg_validation/` harness —
  MIDI capture, timing differential, section transitions, fill timing, chord
  latency, panic/stuck-note, jitter, and a parity harness. Build **before**
  acquiring hardware. (Future phase — not implemented yet.)
- **Long-term positioning:** a software-defined arranger instrument platform.
- Hardware validation remains **DEFERRED_BY_PTH**; no compatibility claimed.

## Release Notes Policy

> Full policy: `docs/policy/CLAIMS_AND_RELEASE_NOTES_POLICY.md`.

Every future release note, tag annotation, PR description, and README/docs
reference to Yamaha/Korg MUST include:

1. **Explicit statement: "Hardware validation pending"** in release notes for any
   tag that includes Yamaha/Korg-related code.
2. **No marketing/demo claim of "compatible with Yamaha/Korg"** unless
   real-device validation evidence (recorded scorecard + NTT A/B calibration
   data) is committed to the repo.
3. Allowed phrasing: "Engine-side Yamaha SFF/CASM importer (hardware parity
   unverified)" or "Supports Yamaha SFF1 parsing (no compatibility claim)".

---

## 1. PROJECT SNAPSHOT

### Gates Completed
| Gate | Status | SHA | Tests |
|------|--------|-----|-------|
| G1: Realtime Timing Spike | ✅ Merged | d6e9e8a | 46/46 |
| G2: Minimal Arranger Slice | ✅ Merged | 799ccd0 | 87/87 |
| G3: UASF Format v1 | ✅ Merged | e5fd139 | 125/125 |
| G4: SFF1 Research + Governance | ✅ Merged | fc8f659 | 125/125 |
| G5: SFF1 Parser Spike | ✅ Merged | ebaa59c | 140/140 |
| G6: SFF2/SMF Corpus Validation | ✅ Merged | 72131a5 | 140/140 |
| G7: CASM Semantic Analysis | ✅ BRANCH (ready) | b744774 | 173/173 |
| G8: NTR/NTT + Chord-Aware Transpose | ✅ Merged to main | d7441b7 | folded into G9 |
| G9: Multi-Playback Validation | ✅ DONE — CONDITIONAL PASS (manual Korg pending) | edb6a20 | 218/218 (14 binaries, 0 fail) |
| G9.1: .sty→.uasf resolution fix | ✅ Merged | 5341263 (PR #7) | 15 binaries, 0 fail |
| G10A: NTT + Articulation + CASM hardening (engine integration) | ✅ **Gate 10A Engine Integration PASS** (merged PR #8, tag `v0.10.0-gate10`) | 79e9f17 | 292/292 (19 binaries, 0 fail) |
| G10B (engine): swing + intro-fix + Guitar/MegaVoice + corpus split | ✅ **Gate 10B Engine PASS** (branch `gate-10b-...`, PR open) | (branch) | 345/345 (22 binaries, 0 fail) |
| G10B (hardware): Yamaha/Korg validation + NTT A/B calibration | ⏳ **PENDING PTH** (needs hardware; checklist + calibration docs ready) | — | — |
| G11: KORG validation harness (pre-hardware, synthetic) | ✅ **Gate 11 Harness PASS (synthetic)** (merged PR #14, tag `v0.11.0-korg-validation-harness`) | 41a2bf7 | 430/430 (29 binaries, 0 fail) |
| G12: Realtime performer layer (deterministic, synthetic) | ✅ **Gate 12 Engine PASS** (merged PR #16, tag `v0.12.0-realtime-performer`) | e1cc59c | 527/527 (38 binaries, 0 fail) |
| G13: StylePlayer E2E performer integration (deterministic, synthetic) | ✅ **Gate 13 Engine PASS** (merged PR #18, tag `v0.13.0-styleplayer-e2e`) | 1e32f20 | 576/576 (46 binaries, 0 fail) |
| G14: macOS app shell prototype (JUCE, headless-tested) | ✅ **Gate 14 Engine-side PASS** (merged PR #20, tag `v0.14.0-macos-shell`) | b9615aa | 661/661 (55 binaries, 0 fail) |
| G14B: macOS playable verification (build+launch+programmatic) | ✅ **Build+launch PASS** (merged PR #21, JUCE 8.0.4); manual PENDING PTH | 4ccdfc5 | see `MACOS_PLAYABLE_VERIFICATION.md` |
| G14C: CoreMIDI output wiring (engine→IAC/DAW) | ✅ **Engine-side PASS** (merged PR #23, tag `v0.14.2-coremidi-out`); DAW verify PENDING PTH | 6856d51 | 720/720 (61 binaries, 0 fail) |
| G15: Performer experience & instrument feel | ✅ **Engine-side PASS** (branch, PR open); feel review PENDING PTH | (branch) | 829/829 (71 binaries, 0 fail) |

### Current Branch / Release
`main` — Gate 9 merged via PR #6. Merge SHA `edb6a20`, tag **`v0.9.0-gate9`**.

> **Gate 9 → FULL PASS** once PTH completes the manual Korg review (Groove +
> Articulation) and records the scores into `.openclaw/review/GATE_9_MUSICAL_REVIEW.md`.
> See `docs/gate-plans/GATE_9_MANUAL_KORG_CHECKLIST.md` and `GATE_9_OPEN_ITEMS.md`.

> Note: the current committed test sources enumerate **143** baseline (G7+G8) assertions
> in this tree; Gate 9 adds 75 → **218 total** (the earlier "173" figure reflected an older
> enumeration and is unrelated to Gate 9). See `docs/gate-plans/GATE_9_HANDOFF.md`.

### Gate 10A — Engine Integration ✅ PASS (see `docs/gate-plans/GATE_10_HANDOFF.md`)
> **Scope is engine-side only.** This is NOT a full "Gate 10 complete" — it is
> the engine-integration slice (Gate 10A). Musical/hardware validation is
> Gate 10B. Do not claim "Gate 10 FULL PASS".
- ✅ Per-channel/per-role track splitting (fixes PR #7 musical-fidelity drop)
- ✅ Full NTR/NTT tables (`src/engine/music/ntt`) — Bass/Chord/Melody + scale modes
- ✅ Articulation render strategy (`IArticulationRenderer`: Naive + Keyswitch)
- ✅ CASM→UASF semantic hardening (defensive mapper + edge-case suite)

### Gate 10B — engine slice ✅ PASS (see `docs/gate-plans/GATE_10B_HANDOFF.md`)
> Engine-side complete; hardware-side pending PTH. Not "Gate 10B FULL PASS".
- ✅ Swing / shuffle groove model (`src/engine/music/groove`) — G9 P1 #3 closed.
- ✅ Intro one-bar delay fix (immediate first-section start) — G9 P1 #5 closed.
- ✅ Guitar NTT (`NttMode::Guitar`) + MegaVoice graceful-degrade renderer.
- ✅ Per-section split validation across the full corpus (4 Genos styles).

### Gate 11 — KORG validation harness ✅ PASS (synthetic) (see `docs/gate-plans/GATE_11_HANDOFF.md`)
> Pre-hardware measurement tooling under `tools/korg_validation/`. CI-safe,
> synthetic-only. **Software harness PASS allowed; hardware parity + KORG
> compatibility claims FORBIDDEN** until real PA capture evidence committed.
> Every report emits `hardware_validated: false`.
- ✅ MIDI capture (CSV+SMF), timing differential, transition validator, chord
  latency, panic/stuck-note, jitter, JSON+MD reporter, Korg fixture slots.
- ✅ 7 new test binaries / 85 assertions; sample report committed.
- ⏳ Reactivate with a real Korg PA700/PA1000: drop a capture in
  `fixtures/korg/<MODEL>/`, run `korg-validate`. No code changes needed.

### Gate 14 — macOS app shell prototype ✅ Engine-side PASS (see `docs/gate-plans/GATE_14_HANDOFF.md`)
> JUCE macOS shell that is a pure CLIENT of the engine (intent only; engine owns
> timing/scheduling). Headless modules `src/{session,control,ui,midi}` +
> snapshot are CI-tested; the JUCE GUI (`apps/macos`) builds locally
> (`BUILD_MACOS_APP=ON`, CI=OFF, no JUCE fetch in CI). Async lock-free UI↔engine
> bridge; no DSP/audio/web/hardware. **Reports `deterministic:true` +
> `hardware_validated:false`.**
- ✅ engine session lifecycle, async bridge, ViewModels, MIDI device manager
  (hot-plug), snapshot capture/restore; 9 new headless test binaries / 85
  assertions.
- ⏳ GUI build/screenshots = local (PTH); code signing/notarization deferred.

### Gate 13 — StylePlayer E2E performer integration ✅ Engine PASS (see `docs/gate-plans/GATE_13_HANDOFF.md`)
> Wires the Gate 12 performer modules into the real StylePlayer via a stateless
> `PerformerAdapter` (`src/engine/integration/`): control events + chord input
> drive section changes (bar-quantized), NTT transpose, panic/recovery. No
> engine-core behaviour change; sequencer stays the bar authority. Deterministic,
> lock-free, synthetic-only. **Software/E2E PASS; hardware/KORG claims FORBIDDEN.**
- ✅ Adapter + 8 E2E test binaries / 49 assertions; sample E2E latency report
  committed (`deterministic:true` + `hardware_validated:false`).
- ⏳ Live korg-playback CLI adapter swap + chord.bass-in-dispatch = future work.

### Gate 12 — Realtime performer layer ✅ Engine PASS (see `docs/gate-plans/GATE_12_HANDOFF.md`)
> Live-performance control layer (sync start/stop, fills, variations, chord scan
> modes, split/manual bass, vendor-neutral control surface, performer panic,
> latency budget) on top of the engine. Deterministic, lock-free RT path, no DSP,
> no UI, no hardware. **Software/realtime PASS allowed; hardware parity + KORG
> compatibility claims FORBIDDEN.** Reports emit `deterministic:true` +
> `hardware_validated:false`.
- ✅ 9 modules under `src/engine/{performance,control,chord}`; 9 new test
  binaries / 97 assertions; sample latency report committed.
- ⏳ Live MIDI front-end adapters + end-to-end StylePlayer wiring = future work.

### Gate 10B — hardware slice (PENDING PTH, needs a real device)
1. **Yamaha/Korg hardware playback validation** — `GATE_10B_HARDWARE_CHECKLIST.md`
   ready; scorecard feeds `GATE_9_MUSICAL_REVIEW.md` §6 (also G9 P1 #1/#2).
2. **NTT A/B calibration vs authentic Yamaha** — `GATE_10B_NTT_CALIBRATION.md`
   ready (engine reference table embedded); capture + log discrepancies.
3. Swing feel + per-style swing %, true MegaVoice rendering, marker-based
   per-section event splitting — future tuning/work.

### Repository
https://github.com/hieucat75/AIArranger

---

## 2. KEY ARCHITECTURE RULES

1. Runtime must NOT know Yamaha/Korg/Roland — src/engine/ is vendor-agnostic
2. Importer layer is ISOLATED — src/importers/sff1/ is where vendor code lives
3. UASF is the canonical format — runtime only understands UASF types
4. No DRM bypass — encrypted styles are REJECTED, not decrypted
5. No silent fallback — unsupported features are LOGGED + REJECTED
6. No malloc/mutex/fileIO in realtime path
7. No MegaVoice->GM silent mapping

---

## 3. CASM STRUCTURE (FROM 4 REAL GENOS .S718 FILES)

CASM chunk structure:
- "CSEG" (4-byte marker)
- Sub-chunks: "Sdec" (Section Definition), "Ctb2" (Track Configuration Block)
- Ctb2: 47 bytes per entry = track_name[20] + config[27]
- Config byte0=low_key, byte1=high_key, byte2=NTR, byte5=NTT

### NTR Values
0=Root, 1=Fifth, 2=Chord, 3=Melodic, 4=Scale, 5=Bypass, 6=Fixed

### NTT Values
0=Bypass, 1=Default, 2=Guitar, 3=Bass, 4=Scale, 5=Chord, 6=Percussion, 7=SFF2-ext

---

## 4. IMPLEMENTATION TASKS — ALL DONE (Gate 7 complete)

> See `docs/gate-plans/GATE_7_HANDOFF.md` for the full close-out report.

### Task A — CasmTrackConfig types ✅ DONE
- struct + ParseResult vector (extended in Task B with channels + ntt_bass)

### Task B — CASM parsing methods ✅ DONE
- parseCasm()/parseCtb2Block() had bodies but WRONG field offsets
  (20-byte name, NTT at byte 25 = garbage). Corrected to the validated
  47-byte Ctb2 layout: src-channel[0], name[1..8], NTR[22], NTT[23].

### Task C — Section detection (Sdec) ✅ DONE
- parseCasm groups Ctb2 configs under each Sdec section
- ParseResult::casm_sections (name + tracks); CASM section *structure*
  exposed. Event-to-section splitting deferred to Gate 8.

### Task D — NTR/NTT → UASF mapping ✅ DONE
- sff1_mapper: mapCasmTrackRole (name-primary + NTT bass flag),
  mapCasmSectionType; SffToUasfResult::casm_sections built from CASM.
- NTR/NTT logged in unmapped_features (no UASF v1 field).

### Task E — Tests ✅ DONE
- test_sff1_reader 15 → 48 assertions (synthetic CASM + real corpus +
  mapper). CMake passes CORPUS_DIR.

### Task F — Compatibility score ✅ DONE
- harmonic_fidelity + casm_sections/casm_tracks/bass_tracks in report.

### Build harness (G7-FIX) ✅
- CMake `run-tests` target never built (10 main()s) and omitted the uasf
  glob. Replaced with per-test executables + CTest; added importer-sff1
  lib and sff1-parser tool. Baseline restored to 140, now 173 assertions.

---

## 5. CORPUS FILES (4 GENOS .S718 STYLES)

1. CLASSIC_6_8_SC_GENOS — 39,850 B, 4,878 events
2. C_WHISPER_SC_GENOS — 35,366 B, 3,539 events
3. BALLAD_FOLK_SC_GENOS_ — 38,702 B, 4,325 events
4. POP_ACOUSTIC_2_SC_GENOS — 44,486 B, 5,563 events
