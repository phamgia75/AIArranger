# Issue #26 evidence matrix: C_WHISPER "Fill In BA" tail

- **Status:** BLOCKED (owner / hardware decision). Prepared by REA on 2026-11-06; no reading is selected.
- The importer keeps the raw data unchanged, and no Break marker is exported (P1.3).
- The corpus is restricted (#27), so this file reports counts and positions only.

Bars are 0-based, as in the issue. The analysis was read-only and used stdlib Python (analysis scripts not committed). It reports counts only.

## Corpus facts

- **C_WHISPER:** Fill In BA starts at bar 17. End of track is exactly at bar 27.0.
- **BALLAD, POP and CLASSIC_6_8:** Fill In BA is the last marker, and end of track comes exactly 1 bar later.
- **Precedent:** BALLAD's Ending A is 2 bars, and its 2nd bar is empty (0 notes).
- **Tail = exact 2-bar loop.** Pitch, onset and velocity were compared. This contradicts the issue's "not exact repeats".
  - Bars 18 = 20 = 22 = 24: ch 8, 11, 12, 14; 52 notes.
  - Bars 19 = 21 = 23 = 25: add ch13; 58 notes.
  - Bar 26 = bar 24 minus one ch8 note.
- **Each (tail bar, channel) copies the same channel in a marked bar verbatim** (bar 26 ch8 excepted). ch8 and ch14 come from Main D bars 5–8. ch11, ch12 and ch13 come from the Mains and Fills.
- **Missing from the tail:** ch9 (Rhythm2), ch10 (Bass) and ch15.
- **No marked section in any of the 4 files lacks ch9.**
- **Bar 17:** 84 notes on all 7 BA channels.
- **CASM:** in every file, every Sdec name has a marker. In C_WHISPER, the Sdec names exactly equal the marker set. Some markers lack a CSEG (Intro A in 2 files, BA in CLASSIC). Unused names in C_WHISPER: Intro B/C, Ending B/C.
- **BA Ctb2:** source ch 8–14. The tail channels are a subset, but bar 17 already uses all of them, so this does not discriminate.

## Matrix

| Evidence | (a) truncate leftover | (b) intentional long break | (c) missing marker |
|---|---|---|---|
| Wierzba/Bedesem v2.1 §5.1.1.2 p.34: "Fill Ins and Break are limited to a single measure" | SUPPORTS: 1-bar cap | CONTRADICTS: over the cap | NEUTRAL: silent on unmarked data |
| Same §3 p.5: "Fill Ins are limited to one measure… other sections… up to 255 measures" | SUPPORTS | CONTRADICTS | NEUTRAL |
| Same §5.1.1 p.31–32: markers "delineate the midi patterns"; Section k drawn to the "last" measure; "Fill In BA represents the Break section" | NEUTRAL | WEAK SUPPORT: the last section runs to end of track | NEUTRAL |
| Same §4.6.3.1 p.14: Sdec names the parts a CSEG covers | NEUTRAL | NEUTRAL | CONTRADICTS: no CSEG for an unmarked part |
| Other 3 styles: 1-bar BA ending exactly at end of track | SUPPORTS | CONTRADICTS (weak, n=3) | NEUTRAL |
| Tail = looped channel copies minus Rhythm2/Bass; last bar clipped | SUPPORTS: looks like residue | NEUTRAL: a drum-less break is plausible, a 4.5× loop is not | CONTRADICTS (weak): fits no unused section type |
| Sdec = marker set; no orphan names in any file | NEUTRAL | NEUTRAL | CONTRADICTS |
| Tail channels are within BA Ctb2 | NEUTRAL | WEAK SUPPORT | NEUTRAL |
| Genos Owner's Manual: BREAK plays a "one-measure Break pattern", then Main (search excerpt; verbatim not verified, 403) | SUPPORTS | CONTRADICTS as playback | NEUTRAL |
| PSR Tutorial, BogdanH (via search, 403): a 2-bar Break made in MixMaster: "second bar will be silent even if contains notes" | SUPPORTS | CONTRADICTS: the tail never sounds | NEUTRAL |
| Genos playback of THIS file | HARDWARE/OWNER PENDING | HARDWARE/OWNER PENDING | HARDWARE/OWNER PENDING |

Sources: https://wierzba.hier-im-netz.de/StyleFileDescription_v21.pdf · https://forum.psrtutorial.com/index.php?msg=530559 · https://www.manualslib.com/manual/1309559/Yamaha-Genos.html?page=65

## Synthetic tests (future importer)

All cases are SFF2, 1920 PPQN, 4/4, with a 1-bar BA at bar 17 followed by a tail.

| # | Input | (a) | (b) | (c) |
|---|---|---|---|---|
| T1 | 9-bar tail on BA channels | BA 1 bar, tail dropped + warning | BA 10 bars | BA 1 bar + flagged orphan block |
| T2 | Tail on a channel absent from BA Ctb2 | dropped | unmapped channel in BA | orphan flagged "no CSEG" |
| T3 | Empty tail bars | BA 1 | BA 10 silent bars | nothing |
| T4 | CSEG "Ending B" with no marker | dropped | kept in BA | tail becomes Ending B |
| T5 | Note held across the bar-18 boundary | note-off at the boundary, no stuck note | unchanged | note-off at the boundary |
| T6 | 2-bar Fill In AA mid-file | capped? (consistency) | 2 bars | n/a |

T1 and T4 separate all three policies. T3 and T5 are safety checks.

## Confidence

**Medium-high** that a Genos sounds only 1 bar of Break. The format doc, the manual excerpt and the forum report agree. This favours (a) *for playback*. It does not show that the tail is "unrelated": it is verbatim material from the same style. (c) has no CASM support. (b) conflicts with every documented limit. No web evidence concerns this specific file.

## What would settle it

1. **Owner Genos test (PENDING):** trigger BREAK from each Main and record MIDI out. Does 1 bar sound, or 10?
2. **Genos Style Creator:** open the style, note the Break length, save it unchanged, then diff the BA marker and end of track.
3. **Provenance:** where "SC_GENOS" comes from, or an earlier copy of the file.
4. **Verbatim BREAK text** from the Yamaha Genos manual PDF.
