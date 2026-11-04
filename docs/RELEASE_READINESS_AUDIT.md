# Release Readiness Audit

> **Date:** 2026-11-04 · **Audited main:** `7694d59`
> **Verdict:** **READY WITH EXTERNAL BLOCKERS.** No repo-side engineering work blocks a release.
> Every remaining blocker needs physical hardware, an owner decision (legal or licensing), or an
> account setting (GitHub Actions). The product must **not** ship, or be called live-safe, until
> they clear.

## 1. What is ready (verified on `7694d59`, local macOS arm64)

| Area | Evidence |
|---|---|
| Test matrix | Release, Debug, ASan+UBSan, TSan, Trace: all tests pass. 0 sanitizer reports. |
| Playback | 4 Genos SFF1 styles (4/4, 6/8, PPQN 1920): tempo exact, NoteOn = NoteOff, 0 outstanding notes, lifecycle Intro → Main → Fill → Ending. |
| C ABI | Contract 1.0.3: command results mean "accepted" (#18); capability flags mean "callable now" (#19); covered by a pure-C11 consumer and a C++ host. |
| CoreMIDI | Init failure surfaced explicitly, with manual retry and no loop (#23). The `-304` root cause is characterised as environmental and mitigated (#21). |
| Arranger behaviour | Auto-fill on variation change (spec §5.3), bar-quantized, tested on a demo style and a Genos style. |
| macOS host | Builds, launches, no crash reports in a smoke run. |
| CI code | Workflow hardened (#31): read-only token, timeouts, PR-only cancellation, C sanitizer flags. Linux job replayed in Docker ubuntu:24.04 (aarch64): release, ASan+UBSan and TSan all 106/106. actionlint clean. Never run on GitHub (Actions disabled). |

## 2. Blockers (all external)

| # | Blocker | Kind | Who clears it | Tracking |
|---|---|---|---|---|
| 1 | No real-hardware validation. `hardware_validated:false`; the host is not live-safe. | Hardware | Owner, with gear | `docs/gate-plans/GATE3_HARDWARE_VALIDATION_CHECKLIST.md` (A–D) |
| 2 | 4 Genos `.sty` files marked "no redistribution" are committed to a **public** repo. | Legal / owner | Owner (+ counsel) | #27 |
| 3 | No `LICENSE` file, so the code has no granted license. | Owner decision | Owner | this audit |
| 4 | GitHub Actions disabled for the repo (`actions/permissions: enabled=false`). | Account setting | Owner | this audit |
| 5 | C_WHISPER: an unmarked 9-bar tail is imported as a 10-bar Break. | Owner decision (semantics) | Owner | #26 |

The audit did **not** change any account, billing or ruleset setting, and did not move or delete any corpus file.

## 3. Release-hygiene gaps (repo-side; not blocking for an internal build)
- Version drift: CMake `project(... VERSION 0.2.0)`, while the latest tag is `v0.15.0-performer-feel`.
- No `CHANGELOG`.
- No `install()`/export rules. The bridge is a static library consumed in-tree.
- Signing and notarization of the macOS app: not set up. It needs an Apple Developer account (external).

## 4. Known residual risk
- A CoreMIDI test can fail with `-304` when the machine is CPU-saturated by other work. It is environmental (#21); a rerun passes.
- The auto-fill "beat-start" nuance (spec §5.3) is not implemented. Only the bar-quantized behaviour is.
- Pa5X / Korg: only the generic CoreMIDI path is in scope. No Pa5X-specific protocol behaviour is implemented or claimed.

## 5. Next action when blockers clear
Run `GATE3_HARDWARE_VALIDATION_CHECKLIST.md` sections A–D on real gear against the then-current main.
Record PASS/FAIL per item with the SHA, then set `hardware_validated:true` only if every item passes.
