# Synthetic style fixtures (issue #27)

Original, generated test data. It contains no third-party musical content and no
material from the restricted Genos corpus, so public tests and CI can use it.

| File | Generator | What it is |
|---|---|---|
| `SYNTH_POP_SFF2.sty` | `tools/make_synthetic_sty.py` | SFF2-structured style: 1920 PPQN, 120 BPM, 4/4. Main A/B (2 bars each), Fill In AA/BB, Intro A, Ending A, Fill In BA (1 bar). Channels 9-11, 12 and 14 (Rhythm1, Rhythm2, Bass, Chord1, Pad). The CASM has one Ctb2 per track per section. Bass is NTR 0, NTT 1 + bass, High Key D#, Note Low 28. The unused low/high zones hold defaults, so reading the wrong zone fails the tests. |

Only the container structure follows the published format description
(Wierzba/Bedesem, "Style Files" v2.1). The patterns are generic pop figures
written for the generator.

**License:** CC0 1.0. You may use these files and the generator for any purpose.

**Regenerate:** run `python3 tools/make_synthetic_sty.py fixtures/synthetic/SYNTH_POP_SFF2.sty`.
The output is byte-identical on every run. If you change the generator, commit
the regenerated file with it.

**Tests:** `tests/exporters/test_synthetic_style_e2e.cpp` covers:

- reader, CASM and mapper;
- playback with High Key;
- no stuck notes and determinism;
- the Korg exporter validator, the withheld Break, and the write/parse round trip.

**Not a compatibility claim.** These files have never been loaded on a Genos or a
Pa5X. Hardware results stay "HARDWARE VALIDATION PENDING".
