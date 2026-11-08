# Sound parameter audit: what AIArranger preserves, approximates or loses

**Scope:** the conversion paths in `src/media/sound/` as of this commit, measured against the Pa5X Sound engine.

**Evidence classes:**
- **DOC**: Pa5X User Manual v1.4 (MAN, printed pages), or Korg's *Making Pa5X Sounds and Drum Kits with TX16Wx* (GUIDE).
- **OBS**: seen in PA Manager v5.2 (`PA5X_KST_RESOURCE_MODEL.md` §3a).
- **CODE**: verified in our writer or tests.
- **INF**: inferred.
- **UNK**: unknown until a hardware import.

Nothing here is Pa5X hardware-verified. S6 is still BLOCKED.

## 1. The Pa5X Sound engine (DOC)

| Block | Pa5X | Source |
|---|---|---|
| Oscillators | 1-24 per Sound (Oscillators Count). Polyphony falls as the count rises (160 with 1 oscillator) | MAN p.1070 |
| Multisamples | Each oscillator plays a **High** and optionally a **Low** stereo multisample, switched at Vel SW L→H. Offset, Reverse, Stereo/Mono mode and Level per multisample | MAN pp.1076-1077 |
| Oscillator zones | Velocity Zone top/bottom, Keyboard Range top/bottom, Scaled Velocity top/bottom | MAN p.1082 |
| Key zones | Inside the multisample: From/To key, Original Note, Level, Pitch, one Sample per zone; no velocity per zone | OBS, MAN p.1190 |
| Pitch | Pitch Slope, Pitch Mod, Pitch EG, LFO modulation | MAN pp.1102-1107 |
| Filter | 2 filter types per oscillator (LP Resonance; LP+HP), filter mod, filter LFO mod | MAN pp.1111-1118 |
| Amp | Level/Pan, Amp mod, Amp LFO, **Amp EG** (levels and times) | MAN pp.1124-1130 |
| LFO | LFO 1 and 2 | MAN p.1134 |
| Voice | Poly/Mono, Legato modes; Damper Mode/Trigger; Basic EQ | MAN pp.1068, 1084-1088 |
| Effects | Sound IFX 1-3 and MFX 1-3; Sound Edit > Effects sends to the FX B group | MAN pp.1068, 1139 |
| Drum Kits | Per key: variable number of velocity layers, samples, Dry/Ambience level, pan, exclusive group | MAN pp.1089-1100 |

## 2. What the Pa5X imports from a TXPROG (DOC, GUIDE pp.24-27, 51, 84-85)

| TX16Wx | Pa5X | Imported |
|---|---|---|
| Program name | Sound name | yes |
| Group name / Volume / Pan / Shift / Tune | Multisample name / OSC Level / Amp Pan / OSC Transpose / OSC Tune | yes |
| Region Lo/Hi Key | OSC Keyboard Range | yes |
| Region Lo/Hi Vel | OSC Velocity Zone and Scaled Velocity | yes |
| Region Root / Attenuation / Tune | Sample Original Note / zone Level / zone Pitch | yes |
| Wave loop points | Sample loop | yes (GUIDE p.84) |
| Sounds panel: envelopes, modulation, high-level parameters | none | **no**: "The Sounds parameters are not imported into Pa5X" (GUIDE pp.51, 85) |
| Wave editor extras (attack positions…) | none | **no** (GUIDE p.84) |

So **filters, envelopes, LFOs, pitch modulation and effects cannot enter the Pa5X through any documented file route**. They arrive at Pa5X defaults and must be set in Sound Edit by hand. The same holds for SF2 and KMP, which carry multisamples only (MAN pp.1206, 1209).

## 3. AIArranger status per parameter

**P** = preserved; **A** = approximated; **L** = lost; **N/A** = no source.

| Parameter | Status | How / evidence |
|---|---|---|
| Velocity layers | **P** (1-24) | One Group = one oscillator per layer, Lo/Hi V tiling 0-127 (CODE `validate`, TXPROG writer, tests) |
| Velocity response *within* a layer | **A** | Each layer is one render velocity. Steps between layers; the Pa5X's own velocity→level response applies on top (UNK how strongly) |
| Key zones and roots | **P** | Exact `lo <= root <= hi`; WAV `smpl` root equals TXPROG root (validator) |
| Sample rate / bit depth | **P** | 16-bit, 11.025-48 kHz enforced |
| Stereo | **P** | Stereo WAV in TXPROG; SF2 linked L/R pairs (CODE). Pa5X oscillators play stereo multisamples (MAN p.1076) |
| Sustain loop | **P/A** | One forward loop, seam found by correlation and crossfaded (≥ 0.99 match on test tones). Evolving sounds (chorus, unison, LFO) loop with audible repetition |
| Zone tune / level | **P** | `Zone.tuneCents` → region `tx:tune`, `levelCentibels` → `tx:attenuation`, same formats as Korg's examples (CODE). No current source sets them |
| Group level / pan / transpose / tune | **L (N/A today)** | The writer emits 0 dB / 0% / 0 / 0. The model has no per-layer field. Needed only for sources that carry it (SF2 reader, KMP); none exists yet |
| Region pan | **L (N/A today)** | Korg's examples use region pan (±25-75%); our model has none |
| Attack | **P (baked)** | Rendered into the sample start |
| Release | **A** (manual step) | Looping cuts the rendered release, and the Pa5X never imports envelopes. **New:** the sampler measures note-off → −60 dB per sample and reports the median for setting the Amp EG by hand (MAN p.1130). DLSMusicDevice local run: median 2.69 s |
| Filter, filter EG, key tracking | **A (baked)** | Frozen into each sample at its render velocity and root. Brightness steps every `--step` semitones; no filter sweep while holding a note beyond what the loop contains |
| Pitch EG / LFO vibrato | **A (baked)** or **L** | Baked into the attack; vibrato inside a loop repeats at the loop length (or breaks the seam, flagged by loop match < 0.9) |
| Mod wheel, aftertouch, pitch-bend response | **L** | Pa5X defaults |
| Effects (reverb, chorus) | **A (baked)** | In the render if the plugin applies them. Render dry and use Pa5X IFX/MFX instead, because a reverb tail is cut by the loop |
| Round robin / alternates | **L** | One sample per zone. Pa5X Sounds have no documented round robin (Korg's round-robin example is a Drum Kit) |
| Drum Kits | **L (not built)** | No TXPERF writer. Pa5X imports TXPERF as a Drum Kit (MAN p.1206; GUIDE pp.24-25: choke → exclusive group, matrix → velocity layers, drum family prefixes) |
| Multisample sharing between Sounds | **N/A** | Each package creates its own samples. The KST dependency model handles sharing (P2) |
| Polyphony cost | **INF** | One oscillator per velocity layer. 24 layers can cut Pa5X polyphony sharply (MAN p.1070). The report states the layer count; no polyphony estimate yet |

## 4. Concrete limitations by source

**VST/AU (plugin sampler):** everything time-varying beyond the loop is frozen.
- Plugins with **evolving pads, unison/chorus or tempo-synced motion** loop poorly. The report flags loop match < 0.9.
- **Release** must be set by hand (now measured).
- **Filter sweeps and mod-wheel timbre** are lost.
- **Velocity timbre** is stepwise: more layers cost polyphony, fewer give audible steps. Recommended 2-4 layers (INF).
- Render **dry**; add Pa5X effects.

**Kronos** (see `KRONOS_KMP_KSF_IMPORT.md`):
- A Kronos Program's synthesis (filters, EGs, AMS, effects) has no file route into the Pa5X (§2). Only multisamples transfer.
- Kronos material can come in only as (a) its KMP/KSF multisamples (native Pa5X import, UNK for Kronos-generation files), or (b) audio recorded from the Kronos output, then the route-C WAV folder.
- Route (b) bakes the sound at fixed velocities, like the VST path.
- Factory Kronos samples remain Korg copyright: owner-made material only.

## 5. Prioritised technical gaps

| # | Gap | Evidence | Action |
|---|---|---|---|
| G1 | Release lost when looping | GUIDE p.85; CODE | **Done:** measured and reported (`measureRelease`, test 0.69 s ± 20 ms) |
| G2 | No Drum Kit (TXPERF) output | MAN p.1206; GUIDE pp.24-25; Korg's drum examples (local) | Next: model drum keys + velocity layers + exclusive groups, then a TXPERF writer mirroring Korg's example |
| G3 | Per-layer level/pan/transpose/tune not modelled | GUIDE p.24 | Add once a source carries them (needed by an SF2 or KMP reader) |
| G4 | Polyphony cost not reported | MAN p.1070 | Report "N oscillators per note" with the MAN note; a numeric estimate is UNK |
| G5 | Velocity response within a layer | UNK | Hardware check after S6 (soft/hard play test) |
| G6 | Loop of evolving sounds | CODE (loop match) | Possible: longer loop search window or crossfade length option; measure on real plugins first |

**Sound Recipe (S13):** `src/media/sound/sound_recipe.h` carries the non-transferred parameters with their provenance (measured / estimated / extracted / not available) into the package REPORT as a post-import checklist.

Not pursued: guessing Pa5X Sound Edit parameters into TXPROG fields. They are documented as ignored.
