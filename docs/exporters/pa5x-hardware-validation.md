# Pa5X hardware validation kit

**Status: NOT RUN. No Pa5X has been available.** Every result below is blank on purpose.
Do not mark a step PASS without doing it on a real Pa5X. When you fill one in, record the OS version, the date, the file name and the `aiarranger-convert` commit.

## 0. Prepare (computer)

For each corpus style:
```
aiarranger-convert --input <STYLE>.sty --target pa5x --output <STYLE>.mid \
    --sound-template <STYLE>.soundmap --report <STYLE>.report.txt
```
Keep `<STYLE>.report.txt`: it lists the expected elements, tracks and loss. Copy the `.mid` files to a FAT32 USB stick. Do not commit them (#27).

## 1. Import SMF

1. On Pa5X, choose a User style location you can overwrite and enter Style Edit.
2. Open Menu > Import/Export > **Import SMF**. Select `<STYLE>.mid`, check **Initialize**, then press **SHIFT + Execute** to import every marker.
3. Record the exact message if it fails (photo of the display). This is the most important result.

| Style | Accepted? | Exact message | Elements present |
|---|---|---|---|
| POP_ACOUSTIC_2 | | | expect V1-4, I1, F1-2, E1 |
| CLASSIC_6_8 | | | expect V1-4, I1, F1-2, E1 |
| BALLAD_FOLK | | | expect V1-4, I1-2, F1-2, E1 |
| C_WHISPER | | | expect V1-4, I1, F1-2, E1 |

## 2. Element setup (not carried in the SMF)

For every element:
- Set Key/Chord to **C Maj7**, the Genos source chord.
- Set the Chord Table so every chord type uses **CV1**.
- Set the style tempo from the report.
- Assign sounds following `pa5x-sound-mapping.md` §7.

| Item | Done? | Notes |
|---|---|---|
| Key/Chord C Maj7 on all elements | | |
| Chord Table → CV1 | | |
| Tempo | | |
| Sounds assigned | | |

## 3. Playback

| Check | POP | 6/8 | BALLAD | WHISPER |
|---|---|---|---|---|
| Variation 1-4 play, loop at the report's bar length | | | | |
| Fill 1, Fill 2 | | | | |
| Intro 1 (and Intro 2 on BALLAD) | | | | |
| Ending 1 | | | | |
| CLASSIC_6_8: meter 6/8, no extra measure appended | – | | – | – |
| Start/Stop 20×, no stuck notes | | | | |

## 4. Chord changes

Play C, Am, G7 and Bdim on each Variation:

| Check | POP | 6/8 | BALLAD | WHISPER |
|---|---|---|---|---|
| Bass follows the root | | | | |
| Chord parts follow the chord quality (major/minor 3rd, 7th) | | | | |
| Drum and Percussion do not transpose | | | | |

## 5. Drum kit

| Check | POP | 6/8 | BALLAD | WHISPER |
|---|---|---|---|---|
| Kick, snare, closed/open HH, toms, crash/ride sound | | | | |
| Non-GM keys (list in `pa5x-sound-mapping.md` §5) have a sound / are silent | | | | |
| Kit chosen (name, MSB/LSB/PC read on Pa5X) | | | | |

## 6. Export back from Pa5X and compare

1. With the imported style selected, open Style Edit > Menu > Import/Export > **Export SMF**. Press **SHIFT + Execute** to export every element as one SMF with markers. Save it as `<STYLE>_pa5x.mid`.
2. On the computer:
   ```
   aiarranger-convert --compare <STYLE>.mid --with <STYLE>_pa5x.mid
   ```
   - Exit 0 / `Round trip: EQUAL`: every element, length, meter and note came back. Ticks are compared at the coarser PPQN with a 1-tick tolerance. Bank/PC/CC/SysEx are ignored.
   - Exit 2 / `DIFF:` lines: record them.
   - `info: element only in the Pa5X export: '…'` lines show **marker codes Pa5X uses**. Record them: they are the evidence needed to export Break, Intro 3, Fill 3-4 and Ending 3.
3. Keep `<STYLE>_pa5x.mid` locally. It is the first real Pa5X sample (do not commit it until #27 is decided).

| Style | Compare result | DIFF lines | Extra Pa5X markers | PPQN of Pa5X export |
|---|---|---|---|---|
| POP_ACOUSTIC_2 | | | | |
| CLASSIC_6_8 | | | | |
| BALLAD_FOLK | | | | |
| C_WHISPER | | | | |

## 7. If something fails

Record the step, the exact message, and the `.mid` used. Then open an issue with the table row. The writer is patched only against such evidence: never to make an untested guess.
