# TXPROG format notes (roadmap S5)

Status 2026-11-07. Purpose: generate a `.txprog` package that the Pa5X imports as a complete Sound (Sample Edit > Menu > Import/Export > Import > TX Sound, manual v1.4 p.1207, p.1210). Writer: `src/media/sound/txprog_writer.h`.

Sources:
- **GUIDE**: KORG Italy, *Making Pa5X Sounds and Drum Kits with TX16Wx* (Korg Pa5X download page, 2024-07-03), 100 pages (page numbers below are the printed ones), with the example projects that come with it. The download licence allows one personal, non-commercial copy, so it was read locally and is **not** in this repository. Nothing from it is copied here except format facts.
- **MAN**: Pa5X User Manual v1.4.
- **EX**: an observation from Korg's example files in that download (element/attribute names and values only).
- Labels: **DOC** (stated in GUIDE/MAN) · **INF** (inferred from EX) · **UNK** (unknown).

## What the Pa5X takes from a TX Program

| TX16Wx | Pa5X | Label |
|---|---|---|
| Program (`.txprog`) | Sound; Performance (`.txperf`) → Drum Kit | DOC GUIDE p.9, p.87; MAN p.1206 |
| Program name | Sound name (editable in the import dialog) | DOC GUIDE p.24, p.87 |
| Group | Multisample, on one oscillator (up to 24); velocity layers are separate Groups in adjacent velocity zones | DOC GUIDE pp.14-16 |
| Group volume / pan / shift / tune | Oscillator Level / Pan / Transpose / Tune | DOC GUIDE pp.24-27 |
| Region low/high key, low/high velocity | Keyboard Zone, Velocity Zone | DOC GUIDE pp.24-27 |
| Wave root | Original Note | DOC GUIDE pp.24-27 |
| Region attenuation, tune | sample Level, Pitch | DOC GUIDE pp.24-27 |
| Loop points | kept; other sample-editor data (e.g. attack positions) dropped | DOC GUIDE p.84 |
| Envelopes, modulation, "Sounds" parameters | **not** imported (preview aids only); filters not mentioned | DOC GUIDE pp.51, 85 |
| Velocity curve "Exponential 2" | matches the Pa5X Velocity Intensity 80 | DOC GUIDE p.42 |

## Package rules

- Regions must cover the whole keyboard with no gaps and no overlaps (DOC GUIDE pp.45, 52, 61).
- The samples folder sits inside the project folder, and paths must be preserved ("mandatory", DOC GUIDE p.31). Copy the whole folder to USB with its structure unchanged (DOC GUIDE p.87).
- For a project saved from TX16Wx, the Bank must contain only what is needed (DOC GUIDE p.10). The writer emits no `.txbank`; the Pa5X loads the `.txprog` (DOC GUIDE p.9).
- The import dialog chooses the target Sound location and offers compression (DOC MAN p.1210, GUIDE p.88).
- Sample limits are the Pa5X's own: 8/16-bit, 11 025-48 000 Hz, ≤ 1 048 576 frames (DOC MAN p.1155).

## File structure

- XML, UTF-8 with a BOM, root `tx:program`, namespace `http://www.tx16wx.com/3.0/program` (INF EX).
- `tx:wave` elements: `tx:id`, a relative `tx:path`, `tx:root` (key name), plus `tx:loop` children (`tx:start`, `tx:end`, `tx:mode` Forward/None) (INF EX).
- A `tx:soundshape` (envelope, velocity curve) is referenced by id from the `tx:group` (INF EX).
- `tx:group` elements: `tx:name`, `tx:volume`, `tx:pan`, `tx:coarse`, `tx:fine`, `tx:playmode`, `tx:soundshape`. Its `tx:region` children carry `tx:wave`, `tx:attenuation` ("-1.1 dB"), `tx:pan`, `tx:tune` (cents), `tx:loop` (loop index) and a `tx:bounds` child (`tx:low-key`, `tx:high-key`, `tx:low-vel`, `tx:high-vel`) (INF EX).
- Key names: C-1 = MIDI 0 … G9 = 127 (INF EX: full-range bounds are C-1..G9).
- **Loop end is exclusive**: in Korg's piano example the first sample has 89 567 frames and its loop ends at `89567` (INF EX, measured with ffprobe). The writer emits `loopEnd` with `[loopStart, loopEnd)` semantics.
- The path separator is `\` in the piano example and `/` in the drum examples. The writer uses `/` (INF; **UNK** whether the Pa5X accepts both).
- Group velocity: the examples use full-range velocity bounds. The writer puts the two layers' ranges on the regions (Low 0..switch-1, High switch..127), which is what GUIDE pp.14-16 describes (INF).

## Unknowns (closed only by an import on a real Pa5X, roadmap S6)

1. Whether the Pa5X needs `tx:created-by`. Since the S6 preflight the writer emits the same element paths, attribute names and order as Korg's example Sound, including the program-level full-range `tx:bounds` and the empty second loop on each wave, and omits only that TX16Wx build stamp.
2. Whether a 48 kHz 16-bit mono WAV with a `smpl` chunk imports with root and loop exactly as written.
3. Whether `/` paths are accepted (the drum examples use them).
4. Licence: TX16Wx's free licence covers the software; it says nothing about files created with it (DOC tx16wx.com licence). The generated files are AIArranger's own output from original samples.
