# P1 Sound Studio + Sample Lab (offline)

Branch `feat/p1-sound-studio` from `integ/p0-library-kit` `886b378`. Local only (public repo: never pushed).
Every output is **structurally valid, not verified on a Pa5X**.

## Inventory before this work (2026-11-14)

| P1 item | Already there (code + tests) | Missing |
|---|---|---|
| KSF / KMP / WAV handling | Readers: `sound/wav_folder_source.h` (16-bit WAV + smpl, Pa5X file naming), `sound/korg_kmp_source.h` (KMP + KSF), `sound/ksf_folder_source.h` (KSF without KMP: measured roots, editable mapping text). Writers: `sound/package_writer.h` (SF2 + TXPROG + WAV), `sound/korg_kmp_writer.h` (KMP + KSF, EXPERIMENTAL L1). Tests: `test_ksf_folder`, `test_kmp_writer`, `test_sound_conversion*`, `test_sound_mapping` | WAV inputs other than 16-bit PCM (8/24/32-bit, float) are refused |
| Multisample key/velocity mapping | Zones from file names (WAV), from the KMP (KRONOS), proposed + editable text for KSF only (root/low/high/tune); layer order + velocity splits for WAV/KMP; `KeyMapView` draws it | One editable zone map for **every** source (split points, root, tune, level, velocity layers) |
| Loop editing | `samplelab/sample_lab.h`: `zeroCrossingNear`, `snapLoop`, `crossfadeLoop`; `studio_tools::labLoop` writes one WAV (E6 job `labLoop`). Tests: `test_samplelab`, `test_studio_tools` | Loops written into the Sound's SF2/TXPROG/KSF outputs (only a single WAV so far); waveform + numeric seam view |
| Sound construction | Convert Sound -> `jobs::soundPackage` / `jobs::kmpFolder` | (covered by the zone map) |
| Drum Kit construction | Model `sound/drumkit.h`, TXPERF writer `sound/txperf_writer.h`, `writeDrumPackage`, job `jobs::drumKit`; only the synthesized demo kit feeds it. Tests: `test_drumkit`, Python independent validator | A builder from the user's WAVs (one sample per key, groups, exclusive group, level, pan); a Drum Kit as KMP + KSF |
| Sample optimisation | `trimSilence`, `normalize`, batch with before/after levels (`runBatch`) | Bit-depth and sample-rate conversion into the Pa5X import limits (MAN p.1155: 11 025-48 000 Hz, 16-bit, 1 048 576 frames) with a before/after format report |
| Export + conversion reports | E6 runner (`jobs/export_job.h`): NAME.INCOMPLETE, SHA-256, verify, publish, NAME.export.md/.json. EXACT/MEASURED/ESTIMATED/MISSING exists only inside the KSF REPORT text | A structured EXACT/MEASURED/ESTIMATED/MISSING section in every Sound/Drum Kit/Sample Lab export report |

## Built here (reusing the modules above)

- `samplelab/zone_map.h`: one text zone map over any decoded Sound (WAV, KMP or KSF), with per-sample loop editing
  (start/end, zero-crossing snap, crossfade). Convert Sound proposes it at Preview; edits are applied on the next Preview,
  and the Sound package (SF2 + TXPROG + WAV) and KMP/KSF writers carry the loops.
- `samplelab/optimize.h`: any PCM WAV (8/16/24/32-bit, 32-bit float, WAVE_FORMAT_EXTENSIBLE) to 16-bit (TPDF dither,
  fixed seed), windowed-sinc rate conversion into 11 025-48 000 Hz, before/after format line. Used by Batch
  (`--convert`) and the Drum Kit builder.
- `samplelab/drum_builder.h`: Drum Kit map text (key, file, group, choke, level, pan, family) -> `DrumKit` -> TXPERF
  package (existing writer) or KMP + KSF (one zone per key, existing writer; pan/choke/groups listed as MISSING).
- Waveform overview and loop seam (`samplelab/sample_lab.h`): min/max columns for the view; numeric seam step.
- E6: `ExportSpec::conversion` -> `## Conversion` (EXACT / MEASURED / ESTIMATED / MISSING) in NAME.export.md and
  `"conversion"` in NAME.export.json, with the status line "structurally valid, not verified on a Pa5X".

Not done: audition (playback) of loops, round robins, ambience kits, auto-sampling over MIDI (fail-closed TX).
