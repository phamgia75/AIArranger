# S6: import the AIArranger demo Sound on the Pa5X (owner procedure, not yet executed)

Roadmap `docs/SOUND_CONVERSION_ROADMAP.md` S6. This is the step that turns SOFTWARE evidence into **PHYSICAL HARDWARE PASS** for the success criterion: a generated, legally usable sound converted, packaged, loaded and played on a real Pa5X.

**Status: NOT EXECUTED. Needs the owner's explicit approval**, because it writes user data into the Pa5X User Sample memory and a User Sound location.

## What this does and does not touch

- Writes: about 1.6 MB of new samples (38 mono 16-bit 48 kHz samples, named `AIA Org…`), two multisamples and one Sound, into an **empty** User Sound location the owner chooses.
- Does **not**: send MIDI from the Mac (no cable or app needed), change Global/MIDI settings, load a KST (a whole-KST load deletes User Samples, manual p.736), overwrite any existing Sound or multisample, use compression.
- The sound is AIArranger's own additive synthesis (no third-party audio).

## Prepare (Mac, no device connected)

```
build/aiarranger-sound demo --out ~/Desktop/AIA_SOUND_TEST       # refuses if the folder exists
python3 -I tools/validate_sound_package.py ~/Desktop/AIA_SOUND_TEST   # expect PASS (needs fluidsynth)
```

Copy to the root of a USB stick: the folder `tx/AIA Demo Organ/` (with its `AIA Demo Organ Samples` subfolder, structure unchanged) and `AIA_Demo_Organ.sf2`. Keep `REPORT.txt` for the zone table.

## On the Pa5X

Before you start, photograph **Sample Edit > Main > Memory Info** (p.1174) and note which User Sound location is empty.

### Route A: TXPROG, one step (try this first)

1. REC/EDIT > Sample Edit > Menu > Import/Export > **Import** (p.1207).
2. Import pop-up: **TX Sound (.txprog)**. Path > Select: `AIA Demo Organ/AIA Demo Organ.txprog`.
3. Execute. In the dialog keep the name, choose the **empty** User Sound location, leave Compression **off** (p.1210).
4. Record the exact message shown if it fails, and stop there (go to Route B).

### Route B: SF2, if A fails

1. Same Import page, pop-up **SoundFont (SF2)**, file `AIA_Demo_Organ.sf2`, **Complete Bank**, Override Sample Names **off** (p.1209).
2. Expect two multisamples: `AIA Organ Soft` and `AIA Organ Bright`.
3. Sound Edit on the empty User location: one oscillator, Low layer = `AIA Organ Soft`, High layer = `AIA Organ Bright`, velocity switch 90, and save (p.1207).

## Play and observe (local keyboard only)

| Check | Pass |
|---|---|
| C2, C3, C4, C5, C6 held 3 s each, soft touch | correct pitch for each key (tuner or by ear against a factory piano), steady tone, no click or pulse while held (loops) |
| The same keys, hard touch | brighter tone (High layer) |
| Keys between sample roots (e.g. C#4, D4, D#4) | correct pitch, no jump in timbre larger than between neighbouring roots |
| Lowest and highest keys | sound, correct octave |
| Memory Info after import | grows by about 1.6 MB, nothing else removed |

Record, as text (photos are transcribed, not committed): the route used, any message, the Sound location, Memory Info before and after, and the observations above. **PHYSICAL HARDWARE PASS** only when the owner reports every row passing.

## Rollback

- Multisamples: Sample Edit > Multisample section > page menu > **Delete** > Selected [n], for `AIA Organ Soft`, then `AIA Organ Bright` (p.1190). Leave **Delete Unassigned Samples** unchecked: it would also remove unrelated unlinked samples.
- Samples: Sample Select window, SHIFT-select only the `AIA Org…` samples, Delete (p.1180).
- The User Sound location: it was empty, so it can be overwritten later. A documented Sound delete command was not found in the manual (UNK).

## Known unknowns this test closes

TXPROG fields the Pa5X needs, `/` path separators, root and loop taken from our files, and the 48 kHz mono WAV with a `smpl` chunk (docs/research/TXPROG_FORMAT_NOTES.md, Unknowns 1-3).
