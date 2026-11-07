# Kronos KMP/KSF → Pa5X: import architecture (P1.6)

**Status: DESIGN only. BLOCKED on authorized test material** (a Kronos KMP/KSF set the owner owns and authorizes, or a Pa5X KMP export of our own demo Sound).

No parser exists yet. None will be written from guessed byte layouts.

Evidence classes:
- **DOC**: Pa5X User Manual v1.4, printed page numbers.
- **INF**: inferred.
- **UNK**: unknown until tested.

## What the Pa5X itself already does (DOC)

| Fact | Source |
|---|---|
| Reads (does not write as a file type) PCG Triton programs, KMP "Trinity/Triton Keymaps", KSF "Trinity/Triton Sample" | MAN p.722 |
| KSF is "KORG's native sample format, used by the Pa-Series", compressed or uncompressed; samples 8/16-bit, 11 025-48 000 Hz, ≤ 1 048 576 frames | MAN p.1155 |
| Import KMP: mono; stereo = two KMP files of the same name ending `_L` / `_R`; name editable; optional compression | MAN pp.1206, 1209 |
| Import PCG: single Program or bank; the Pa5X substitutes "similar" multisamples or `<empty>`; Insert FX, EQ, Arp, Combi, Global and Drum Kits are not imported | MAN p.1208 |
| Export the current multisample to KMP + KSF, WAV (with `_OrgKey_LowKey_HighKey` names) or TXPROG + WAV | MAN pp.1212-1215 |

**Consequence (INF):** a KMP/KSF set that the Pa5X accepts needs **no conversion by AIArranger**. The workflow is copy to USB, then Import > KMP. AIArranger adds value only where native import fails or loses data.

## Routes

| Route | Path | Status |
|---|---|---|
| A. Native | Kronos KMP/KSF → USB → Pa5X Import KMP (one multisample, then build the Sound by hand) | UNK: whether Kronos-generation KMP/KSF (newer than Triton) is accepted. Needs one owner test, under the same gate rules as S6 |
| B. Convert | KMP/KSF → `Instrument` model → TXPROG package (whole Sound, velocity layers, loops) | Needs a KMP/KSF reader, BLOCKED on material |
| C. Via WAV | Owner exports WAV from the Kronos (or a DAW) → `Instrument` from WAV + key map → TXPROG | **Built (SOFTWARE):** `aiarranger-sound wav --in SRC --name NAME [--layers a,b --splits 90] --out DIR` (`src/media/sound/wav_folder_source.h`). Files named `<layer>_<OrgKey>_<LowKey>_<HighKey>.wav` (MAN p.1212); root and forward loop from `smpl`; 16-bit mono/stereo only; anything else refused. Round trip of the demo package is exact. Not yet run on Kronos-exported WAVs (BLOCKED, material) |

**Recommended order:** C (no proprietary parsing), then A (one owner test), then B only if A fails and authorized material exists.

## Reader design for B (when unblocked)

- **Input:** a folder; `.kmp` plus the `.ksf` files it references. Read-only: hash the inputs, never modify the originals (P2 rule).
- **Output:** the existing `Instrument` (`src/media/sound/instrument.h`): one Layer per KMP; `_L`/`_R` pairs become stereo Samples. Then the existing `validate()`, writers and `tools/validate_sound_package.py`.
- **Ground truth:** before the reader trusts any field, build a round-trip oracle. The Pa5X exports our known demo organ as KMP/KSF (owner action, read-only for Pa5X data, writes only to USB). Each field is then checked against values we generated.
- **Refusal rules:** unknown chunk → refuse with the file and offset; compressed KSF → refuse until the compression is documented; frames > 1 048 576 → refuse (do not truncate silently as the Pa5X does).
- **Tests:** synthetic files from the oracle only. No Korg factory content and no third-party datasets in the repo.

## What a converter cannot carry (DOC/INF)

- Programs/Combis, effects, EQ, arpeggios (MAN p.1208 for PCG): the Sound gets Pa5X defaults.
- Kronos engine-specific parameters (filters, envelopes, modulation) have no TXPROG equivalent in our writer. Envelopes are not imported from TXPROG either (GUIDE p.85).
- Copyrighted Kronos factory samples: AIArranger converts only material the owner holds rights to.

## Owner decisions needed (BLOCKED)

1. Authorize a source set: which Kronos KMP/KSF (owner-made) may be used as test material, kept outside the public repo.
2. Authorize the route-A test (one Import KMP on the Pa5X), after S6.
3. Authorize a Pa5X KMP export of the imported demo Sound to USB (oracle).
