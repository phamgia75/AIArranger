# TX16Wx Performance (.txperf) for Pa5X Drum Kits: format notes

**Sources:**
- **GUIDE**: Korg's *Making Pa5X Sounds and Drum Kits with TX16Wx* (printed pages).
- **MAN**: Pa5X User Manual v1.4.
- **EX**: Korg's example projects from the guide download, compared locally and **not** in the repo:
  - `Real-Drums DrumKit`: Standard kit, 19 keys, 27 velocity layers.
  - `Round Robin DrumKit with 16 Layers`: round-robin + Ambience.

**Evidence classes:** DOC = documented; EX = present in Korg's example; INF = inferred; UNK = unknown until a Pa5X import.

Writer: `src/media/sound/txperf_writer.h`. Model: `src/media/sound/drumkit.h`. Independent check: `tools/validate_drumkit_package.py`.

## Package

```
<Kit>/<Kit>.txperf          performance: one slot -> <Kit>.txprog              (DOC GUIDE p.18; EX)
<Kit>/<Kit>.txprog          program: wavematrix list, bounds, soundshape, groups (EX)
<Kit>/<Family>_<Name>.txmatrix   one per key: waves, X axis "--", Y axis "Vel", rows (EX; GUIDE p.78)
<Kit>/<Kit> Samples/*.wav   mono or stereo (GUIDE p.82)
```

The Pa5X imports the `.txperf` as a Drum Kit (DOC MAN p.1206, "TX DrumKit (TXPERF)"; GUIDE p.18). SF2 cannot produce a Drum Kit: an SF2 import yields multisamples only (MAN p.1206).

## Mapping (DOC GUIDE pp.24-25, 28)

| TX16Wx | Pa5X Drum Kit | AIArranger model |
|---|---|---|
| Performance name | Drum Kit name | `DrumKit::name` |
| Group Choke 1-8 | Voice Mixer > Exclusive Group | `DrumInstrument::choke` |
| Region (single key) | the key | `DrumKey::key` |
| Region Pan | Voice Mixer > Pan | `panPct` |
| Region Shift | Drum Kit > Transpose (±12 st on Pa5X, ±127 in TX) | `transpose`, refused beyond ±12 |
| Region Tune | Drum Kit > Tune (±50 ct in TX, ±1200 on Pa5X) | `tuneCents`, refused beyond ±50 |
| Matrix name | Assigned Sample | `DrumKey::name` |
| Matrix name prefix `B_ S_ H_ T_ C_ L_ E_ X_` | Drum Family (GUIDE p.80) | `family` |
| Matrix row (Y = Vel), up to 16 | Velocity Layer (GUIDE pp.76, 78) | `DrumCell`, `velLo` = row value |
| Cell attenuation | Dry level, at most +6 dB (GUIDE pp.25, 81) | `gainDb10`, refused above +60 |
| Wave root / loop | sample Original Note / loop | from `Sample` |

**Not imported** (GUIDE p.85): the TX16Wx Sounds panel (envelopes, modulation). Envelopes, filters, kit EQ and effects stay at Pa5X defaults; the package `REPORT.txt` lists them as manual settings.

## Structure check against Korg's example

A local element-path/attribute comparison (`structdiff`) found our three file types to be a **subset** of Korg's:
- No element or attribute appears only in ours.
- Korg additionally writes optional `tx:fine` / `tx:root` on some regions and cells.
- Korg writes `<tx:loop>` on waves; our writer emits the same element only for looped samples.

`tools/validate_drumkit_package.py --reference` passes on Korg's `Real-Drums DrumKit` (positive control: 19 keys, 27 layers, exclusive group 1 = Hi-hat). Strict mode, used for our packages, rejects Korg's example for its 24-bit WAVs and its WAVs without `smpl`. That is expected: our writer keeps to MAN p.1155's 8/16-bit.

## Uncertainties (UNK until a Pa5X import)

1. Whether the Pa5X needs anything absent from our files that Korg's example has (`tx:fine`, `tx:root=""` on regions or cells).
2. Korg's matrix waves carry zero-length "Forward" loops (start = end) on one-shot samples; ours carry no loop element. Effect unknown.
3. How the Pa5X names imported drum samples: from the matrix name, the cell, or the WAV file name.
4. Exclusive Group numbering: whether TX choke 1 becomes Pa5X Exclusive Group 1.
5. Velocity curve in the `.txperf` slot (`Exponential 2`, copied from the example): a preview setting in TX16Wx, presumably ignored.
6. Out of scope: round-robins (up to 4 matrix columns, GUIDE p.79) and Ambience kits (second `_A` Program, GUIDE p.23).
