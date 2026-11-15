# Compatibility validation matrix (Sound / Drum Kit conversion)

Date: 2026-11-08. Branch `feat/pa5x-first-baseline` at `8d01bdb`. Re-generated evidence lives in the session
scratchpad only (not committed); the commands below reproduce it.

**Rule:** a stage is PASS only with recorded evidence for that stage. A software test never stands in for a
Pa5X stage. Pa5X stages stay BLOCKED until S6 (O1–O5 plus explicit owner approval) runs.

| Source | Source processed | Package structurally validated | Third-party playback validated | Pa5X import validated | Pa5X sound quality validated |
|---|---|---|---|---|---|
| Original synthetic Sound (demo organ) | PASS (A) | PASS (B) | PASS (C) | BLOCKED (S6) | BLOCKED (S6) |
| Original synthetic Drum Kit (AIA Demo Kit) | PASS (D) | PASS (E) | **NOT AVAILABLE** (F) | BLOCKED (S6) | BLOCKED (S6) |
| Authorized VST/AU | PARTIAL (G) | PASS for G (B) | PASS for G (C) | BLOCKED (S6) | BLOCKED (S6) |
| Authorized Kronos material | BLOCKED (H) | — | — | BLOCKED | BLOCKED |

## Evidence

- **A** `aiarranger-sound demo --out ev/snd`: 38 WAVs, 2 velocity layers (0–89 / 90–127), looped.
- **B** `python3 -I tools/validate_sound_package.py ev/snd`. This is an independent Python reader, not the
  C++ writer. Result: PASS.
  - `smpl` root and loop equal the TXPROG.
  - Velocity zones are contiguous.
  - Every WAV decodes in macOS `afinfo` (CoreAudio).
- **C** FluidSynth (third-party engine), via the SF2 export of the same samples.
  - Demo organ: 15 notes, worst pitch error 0.004 cents; the layer switches at velocity 89/90.
  - DLSMusicDevice render (G): worst error 0.939 cents.
- **D** `aiarranger-sound drums --out ev/drm`: 7 keys, 9 layers, one stereo sample, hi-hat Exclusive Group 1.
- **E** `python3 -I tools/validate_drumkit_package.py ev/drm`: strict allowlist of the elements and attributes
  in Korg's own Real-Drums example. Result: PASS. Hardening:
  - 17 mutations are rejected (`tests/tools/test_validate_drumkit.py`).
  - All 9 WAVs decode in `afinfo`.
- **F** No third-party engine available here can play a TX16Wx Drum Kit. TX16Wx is not installed, and SF2 is
  not a Pa5X Drum Kit path. Structural and CoreAudio checks only. This needs TX16Wx (owner install) or the
  Pa5X itself.
- **G** The only AU instrument on this Mac is Apple `DLSMusicDevice`. No third-party VST/AU is installed or
  authorized, so "authorized VST/AU" is proven only for this one Apple plugin.
  - Command: `aiarranger-plugin-sampler --vel 50,110 --stereo on --loop on --hold 2 --release 6`.
  - Output: 18 stereo samples, 2 layers, release median 2.98 s (measured).
  - The Sound Recipe lists the first 64 plugin parameters as Extracted. Nothing is inferred as Pa5X synthesis.
- **H** No Kronos KSF/KMP material is on this machine (`docs/research/KRONOS_KMP_KSF_IMPORT.md`).

## What each stage does NOT prove

- **Structural validation**: proves the files match Korg's example format and the guide's limits. It does not
  prove the Pa5X accepts them. The Pa5X import step and the TX16Wx version handling are UNK.
- **FluidSynth playback**: proves the samples, roots, loops and velocity splits. It does not prove Pa5X
  envelopes, filters or effects, which are not imported at all (GUIDE pp.51, 85).
- **Known divergence**: Korg's own example ships 24-bit WAVs, but MAN p.1155 lists 8/16-bit. Our writer emits
  16-bit, the documented case.
- **Exclusive groups**: TX16Wx choke groups 1–8 are modelled (GUIDE p.50). The Pa5X itself allows Exclusive
  Groups 1–127 (MAN p.1099); groups above 8 must be set on the instrument after import.

## Next evidence needed (in order)

1. S6 single-Sound import on the Pa5X (owner-authorized, from USB storage per
   `docs/evidence/hardware/PA5X_SOUND_IMPORT_PROCEDURE.md`). Record the exact result; on failure, stop.
2. Pa5X listening comparison against the FluidSynth renders (pitch, loop, layer switch).
3. Drum Kit import (after the Sound), then a choke check on the instrument.
4. Owner-supplied third-party VST/AU and Kronos material with written permission
   (`docs/REAL_WORLD_DATA_READINESS.md`).
