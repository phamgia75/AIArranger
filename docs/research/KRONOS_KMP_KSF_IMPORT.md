# Kronos KMP/KSF → Pa5X: import architecture (P1.6)

**Status (2026-11-11): route B BUILT (SOFTWARE), from Korg's own documentation.** The KRONOS Parameter Guide
(En 3, Korg's public PDF, SHA-256 `bfb0cba7…dd`, kept outside the repo) documents the format in full:
p.1158-1159 "Korg format sample files" (chunks, field sizes, big endian, the SMD1 attribute bits in a figure),
p.1157 AIFF/WAVE import/export limits, p.855-856 Save Sampling Data, p.858-859 Export Samples as AIF/WAV,
p.709-710 Start / Loop Start / End in single-sample units. Nothing is guessed: whatever is outside that
documentation is refused. **Not yet run on real KRONOS files** (none on this Mac): the owner's first saved
folder is the acceptance test.

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
| B. Convert | KMP/KSF → `Instrument` model → TXPROG package (whole Sound, velocity layers, loops) | **Built (SOFTWARE):** `src/media/sound/korg_kmp_source.h`; CLI `aiarranger-sound kronos --in SRC --name NAME [--layers a,b --splits 90] --out DIR`; app: Sounds & Resources > Convert Sound > Choose Folder... (a folder with .KMP files is read as KRONOS). Tests: `test_kronos_source` (files written from the documented layout), app smoke, 200 000-case ASan/UBSan fuzz of both readers |
| C. Via WAV | Owner exports WAV from the Kronos (or a DAW) → `Instrument` from WAV + key map → TXPROG | **Built (SOFTWARE):** `aiarranger-sound wav --in SRC --name NAME [--layers a,b --splits 90] --out DIR` (`src/media/sound/wav_folder_source.h`). Files named `<layer>_<OrgKey>_<LowKey>_<HighKey>.wav` (MAN p.1212); root and forward loop from `smpl`; 16-bit mono/stereo only; anything else refused. Round trip of the demo package is exact. Not yet run on Kronos-exported WAVs (BLOCKED, material) |

**Recommended order (revised 2026-11-11, KRONOS FIRST directive):** B. The KRONOS WAV export cannot carry the
map ("Zone data of the multisamples will not be exported", p.1157; mono 16-bit only; Loop Tune, Reverse, +12 dB and
the sample name are lost, p.858), and the Pa5X's own KMP import pairs stereo by `_L`/`_R` file names while the
KRONOS names stereo multisamples `<name> -L` / `<name> -R` (p.836, p.1161) in files like `NAME000.KMP`. Route B reads
the map from the KMP and joins the pair into stereo samples.

## What the reader does (route B)

| KRONOS (documented) | Package | |
|---|---|---|
| RLP1 region: top key; low key = previous top + 1, first = C-1 (p.1158) | zone keys | exact |
| original key (bits 6-0), tune -99..+99 cents | zone root, tune | exact |
| SMD1 rate, 16-bit mono, big endian; Start..End (p.710) | sample audio | exact (bit for bit) |
| Loop On (attribute bit 7 = 0), Loop Start..End | loop [Loop Start, End + 1) | **INF:** End taken as the last looped sample (the parameter is an "end point ... in single-sample units"); confirm with the oracle below |
| `<name> -L` + `<name> -R` multisamples | stereo samples | exact; halves that differ are refused |
| SKIPPEDSAMPL region | no zone (keys silent, shown in Checks) | |
| Level, Loop Tune, +12 dB mixed, RLP2/RLP3 | — | LOST, listed in REPORT.txt |
| Program (.PCG): velocity zones, filters, EGs, LFOs, FX | — | not in these files; the musician sets layer order and splits |

Refused, with the file and the reason: compressed data, Reverse, 8-bit, more than 1 channel in a KSF, shared data
(SMF1), divided files (SPD1/SDD1), INTERNALnnnn, Non Transpose over several keys, any undocumented chunk, any chunk
or value that does not fit the documented size.

## KSF samples without a KMP (route B2, BUILT 2026-11-11)

Loose `.KSF` files (for example a sample library shipped without its `.KMP`) are read with the same KSF reader:
audio, rate, Start..End and loop are exact. A KSF holds no root, key range or velocity (they are in the KMP and
Program), so `media/sound/ksf_folder_source.h`:

- MEASURES each root: YIN on five windows of the loop (or of the sustain past the first quarter); the five must agree
  within a semitone. Then the DTFT peak (`measurePitch`) gives the cents.
- Reads a note number at the end of the sample name as a HINT only. It is shown as "agrees" or "DISAGREES, check by
  ear". A name is used as the root only when no pitch is found, and then marked "NOT verified".
- PROPOSES zones (each key plays the nearest root, ties to the higher, C-1..G9) and a tune column that corrects the
  measured offset. This is plain text, editable in Convert Sound (or `--mapping FILE` in `aiarranger-sound ksf`).
  What is built is exactly that text.
- Makes one velocity layer. Two samples at one root are not guessed into layers; the second is listed NOT USED.
- Lists every file it cannot use, with the reason (compressed, no pitch and no number, duplicate root).
- REPORT.txt says what was RECOVERED, APPROXIMATED and NOT AVAILABLE. It re-measures each sample used, and records the
  mapping used.

Owner test (2026-11-11, an 18-sample solo violin KSF set from a third-party library; files not committed, source
hashed unchanged before/after):
- All 18 files are SMP1/SNO1/SMD1, uncompressed 16-bit mono 44.1 kHz, looped, no 2nd Start.
- The 18 measured roots (G#3..F#6 in whole tones) all agree with the names' note numbers.
- The measured offsets were -30..+26 cents.
- The package passes `shasum -c` (40 files), and all 18 samples match the source bit for bit (audio Start..End and
  loop points).
- `validate_sound_package.py --pitch-only`: every structure check passes.
- Its FluidSynth pitch check (5 cents) fails at 10.8 cents worst with the proposed tune, and at 23.4 cents without it.
  The validator measures 0.5-2.0 s into the note (vibrato sustain); the proposal measures the loop. Not a mapping
  error. The gate is unchanged.
- Pa5X playback: not done (owner's step).

### Pitch on vibrato recordings (2026-11-12)

The first owner run failed the validator's 5-cent check (10.8 cents worst). The cause was the measurement, not the
conversion:
- All 18 violin samples have vibrato: about 5 Hz, swinging 20-35 cents (10th-90th percentile).
- A single spectral peak, which both the first tune proposal and the validator used, lands anywhere inside that swing.
  On the same audio it disagreed with the centre of the pitch track by up to 20 cents.

Method now used (`ksf_folder_source.h`, `pitchPoints` and `summarise`):
- Track the pitch every 5 ms with YIN. The window is 3 periods, between 512 and 2048 frames.
- Read the track through the playback position, 0.5-2.0 s into the note as it plays with the loop repeated. No window
  straddles a loop seam.
- Vibrato (10th-90th percentile spread of at least 8 cents): the centre is the mean of the track. A steady tone keeps the
  spectral peak.
- Confidence: 2 standard errors of the per-cycle means, plus 1 cent of method bias.

Evidence:
- Synthetic vibrato with a known centre (+-20/40 cents, loudness tied to the swing):
  - the power-spectrum centroid is pulled +8.8/+17 cents towards the loud half;
  - a 2048-frame track +2.2/+4.5;
  - the 512-frame track +0.3/+1.1;
  - steady tones +-0.7.
- Owner package, all 37 keys G3-G6 rendered by FluidSynth and measured with the same estimator in the same window: the
  render is within 1.1 cents of source centre + tune, and within 1.3 cents of equal temperament.
- On that render the spectral peak still reads up to 20.4 cents.

`validate_sound_package.py --vibrato` (opt-in; the default is unchanged):
- Applies the same 5-cent limit to the vibrato centre, using its own estimator (a narrow-lag difference function every
  50 ms, pure Python, not AIArranger's code), and still prints the peak.
- Owner package: PASS, worst 1.75 cents (peak worst 12.4).
- Negative controls:
  - the same samples without tune: FAIL, 22.7 cents;
  - at C6 the peak reads -1.6 (a false pass) while the centre reads -10.8;
  - demo organ: PASS in both modes.

### Sound quality of the owner set (read-only, 2026-11-12)

- Loop seams are continuous. Pitch steps at the seam beyond the natural change are at most 4.6 cents, below the vibrato's
  own motion. Seam level steps match the natural level change within 1.2 dB.
- C6 (MS000014): the seam waveform matches only 0.74, so a long note may tick.
- Eight loops (A#3, G#4, A#4, C5, D5, F#5, A#5, F#6) hold about one vibrato cycle, so long notes repeat it.
- Level steps between neighbouring samples are up to 3.7 dB (E4 and D5 quieter).
- Timbre steps: C4 and C5 are about 5 dB darker than the sample below; D5 is 8.3 dB brighter.
- 11 samples peak at full scale with 1-2 samples at +-32767: the source's normalisation, not added clipping.
- These checks are now in REPORT.txt (QUALITY) and in the Convert Sound pane before Generate.

## Owner: what to save on the KRONOS (no Pa5X involved)

1. Disk > page menu > **Save Sampling Data** > **One Multisample** (or All Multisamples) to a USB stick (p.855-856).
   For velocity layers, save each multisample that the Program uses.
2. Optional oracle for the one inferred point: also **Export Samples as AIF/WAV** > WAVE > **Samples in One MS**
   for the same multisample (p.858-859). Its WAV loop end, compared with the KSF Loop End, settles whether End is
   inclusive. Keep both outside the repository.
3. On the Mac: Convert Sound > Choose Folder... > the saved folder; order the layers, Preview, Generate Package.

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
