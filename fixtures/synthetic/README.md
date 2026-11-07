# Synthetic style fixtures (issue #27)

Original, generated test data. It contains no third-party musical content and no
material from the restricted Genos corpus, so public tests and CI can use it.

| File | What it is |
|---|---|
| `SYNTH_POP_SFF2.sty` | 4/4, 120 BPM. Main A/B (2 bars each), Fill In AA/BB, Intro A, Ending A, Fill In BA (1 bar). Rhythm1, Rhythm2, Bass, Chord1, Pad. Bass is NTR 0, NTT 1 + bass, High Key D#, Note Low 28. Unused low/high Ctb2 zones hold defaults, so reading the wrong zone fails the tests |
| `SYNTH_FULL_SFF2.sty` | 4/4, **92 BPM**, **all 15 sections**: Intro A-C, Main A-D, Fill In AA-DD, Ending A-C, Fill In BA. Adds a guitar track `Chord2` (**NTR 2**, NTT 1) strummed in Mains C/D |
| `SYNTH_FULL_SFF2.uasf` | UASF golden: `build/sff1-parser SYNTH_FULL_SFF2.sty --uasf-output …`. The test requires byte equality with the importer + serializer output |
| `SYNTH_WALTZ_68.sty` | **6/8**, 132 BPM. Main A/B (4 bars each), Fill In AA/BB, Intro A, Ending A |
| `SYNTH_BREAK_TAIL.sty` | Main A, a 1-bar Fill In BA, then a **9-bar unmarked tail** (Rhythm1/Chord1/Pad only), the #26 shape. Used to show the tail is preserved and Break is not exported. It does not decide #26 |

Only the container structure follows the published format description
(Wierzba/Bedesem, "Style Files" v2.1). The patterns are generic pop figures
written for the generator.

**License:** CC0 1.0. You may use these files and the generator for any purpose.

**Regenerate:** run `python3 tools/make_synthetic_sty.py fixtures/synthetic`. That writes all `.sty` files; rebuild the `.uasf` golden with `sff1-parser` as above.
The output is byte-identical on every run. If you change the generator, commit
the regenerated file with it.

**Tests:** `tests/exporters/test_synthetic_corpus.cpp` covers 6/8, tempo, all sections, guitar NTR, Korg export, the UASF golden and the #26 tail. `tests/exporters/test_synthetic_style_e2e.cpp` covers:

- reader, CASM and mapper;
- playback with High Key;
- no stuck notes and determinism;
- the Korg exporter validator, the withheld Break, and the write/parse round trip.

**Not a compatibility claim.** These files have never been loaded on a Genos or a
Pa5X. Hardware results stay "HARDWARE VALIDATION PENDING".
