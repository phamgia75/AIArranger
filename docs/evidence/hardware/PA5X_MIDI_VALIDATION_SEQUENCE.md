# Pa5X MIDI hardware validation sequence (prepared 2026-11-07; step 2 run and accepted, transmit steps NOT RUN)

**Status: nothing in this sequence has been sent.** Step 2 (receive only) was run on 2026-11-07 and accepted by the owner; see below. Every step that transmits needs the owner's explicit authorization *for that step*, given in the session, plus `--confirm-send` on the command line. Results go to a dated file next to this one; a step is PASS only with direct evidence (tool output) and, where marked, the operator's audible confirmation. Software tests passing says nothing about these results.

Bench: Mac mini over SSH (key-only, strict host key). Tools built from `fa780a2` in the separate worktree `~/developer/AIArranger-pa5x-doctor` (`build/pa5x-doctor`, `build/pa5x-probe`). Below, `P=~/developer/AIArranger-pa5x-doctor/build`.

## 0. Pre-flight (read-only, done 2026-11-07 ~10:00 unless marked)

| Check | Result | Evidence |
|---|---|---|
| Device availability | **OK**: `Pa5X KEYBOARD` uid 534315493 and `Pa5X SOUND` uid 1907663670 online; FLOW 8 also attached | `pa5x-doctor`, `2026-11-07-pa5x-redetection.md` |
| Only one destination named exactly `Pa5X SOUND` | **OK** (the probe refuses otherwise) | doctor listing |
| Send guard | **OK**: `pa5x-probe alloff --dest "Pa5X SOUND"` without `--confirm-send` refused, nothing sent | bench output |
| Competing MIDI processes | **ATTENTION**: no DAW or AIArranger tool running, but **MusicAIApp** (links CoreMIDI, running since 11-05), **Music** and **Moises** are open; Audio MIDI Setup is open | `ps`, `otool -L` |
| MIDI channel configuration | **Owner check.** Last recorded (11-06 photos, preset "Hieu"): MIDI OUT ch1 = Upper 1; MIDI IN ch1 = Upper 3, ch9-16 = Player 1 Style tracks; Chord 1/2 Off; Clock Send Off; Local On. Not re-verified today | `2026-11-06-pa5x-gate1-raw.md` |
| Safe volume and monitoring | **Owner check**: Pa5X master volume low (about 1/4), monitoring on the Pa5X speakers or headphones only, FLOW 8 faders for the Pa5X channel down | operator |
| Panic / recovery | **Ready**: `pa5x-probe alloff` sends All Sound Off (CC120) + All Notes Off (CC123) on all 16 channels; `note` ends with All Sound Off automatically. Manual fallback: the Pa5X STOP button, then reselect the Sound | tool header |

**Owner actions before step 1:** quit MusicAIApp (or confirm it has no MIDI output to the Pa5X), pause Music and Moises, set the volume and monitoring above, photograph Settings > MIDI (IN, OUT, General) so channels are verified rather than remembered.

## Tests (each needs its own authorization)

Abort rule for every step: any unexpected sound, stuck note or error → `$P/pa5x-probe alloff --dest "Pa5X SOUND" --confirm-send` (itself a send; pre-authorized as recovery once step 1 is authorized), record, stop.

### Before any transmit step (checklist, all required, re-done on the day)

1. **MIDI routing rechecked:** owner photographs Settings > MIDI (IN, OUT, General) and confirms which part MIDI IN ch 1 drives; nothing changed by us.
2. **No competing sender to Pa5X SOUND:** MusicAIApp quit, Music and Moises quit or paused, `ps` shows no other CoreMIDI app, no AIArranger tool running (checked by Claude on the bench).
3. **Safe volume:** Pa5X master volume low, monitoring on the Pa5X or headphones only, FLOW 8 Pa5X faders down (operator).
4. **Bounded command ready:** step 1 below; `note` sends one Note On, waits `--ms` (20-5000), sends Note Off, then All Sound Off. Since this change it ignores SIGHUP/SIGINT/SIGTERM/SIGPIPE during that window, so a dropped SSH session or Ctrl-C cannot leave the note hanging (verified by reading the code; not exercised, because exercising it means sending). Only SIGKILL or a power loss can interrupt it: then `alloff`, or the Pa5X STOP button.
5. **Explicit owner authorization** for step 1, in the session.

### Phase B gate status (2026-11-07): step 1 attempt 1 at 17:30:08 silent (`2026-11-07-pa5x-phaseB-note.md`, kept as is); **attempt 2 at 18:25:25 PASS** (`2026-11-07-pa5x-phaseB-retest.md`); no further transmission authorized

| Gate | Status |
|---|---|
| 1. Photos of Settings > MIDI: MIDI IN, MIDI OUT, General | **received 17:2x** (transcribed below; photos kept private, not in the repo) |
| 2. MusicAIApp, Music, Moises stopped | waiting for owner (checked 17:27:02: **all three still running**, pids 72689, 11698, 76868) |
| 3. Pa5X master low, FLOW 8 Pa5X faders down | waiting for owner |
| 4. Separate explicit authorization of the exact command below | waiting for owner |

**Pa5X Settings > MIDI as photographed by the owner, 2026-11-07 ~17:25 (transcription):**

| Page | Setting |
|---|---|
| General Controls | MIDI Preset **"Ai Arranger"** (the 11-06 record said "Hieu"); Clock Send **off**; Clock Source **Internal**; Local Control **on**; Note to RX Noise **on**; Send Style&KbdSet FXs SysEx **on** |
| MIDI IN Channel | ch 1 = **Upper 3**; ch 2-8 Off; ch 9 Ply.1 Tr 9 / STY Bass; ch 10 Tr 10 / STY Drum; ch 11 Tr 11 / STY Perc; ch 12-16 Tr 12-16 / STY Acc1-Acc5 |
| MIDI OUT Channel | ch 1 = **Upper 1**; ch 4 = **Lower**; all others Off |

Consequences:
- MIDI OUT confirms the receive evidence: lower = ch 4, upper = ch 1. No OUT channel for Upper 2/3, so the ch 1 + ch 4 duplicates of C5/C6 seen at 11:31 are **not explained** by this page (open).
- No MIDI IN channel is assigned to Chord 1/2 or Control, so the Pa5X's own arranger cannot be driven from the engine with this preset (steps 2-note and 5a).
- Step 1 (ch 1, C4) should sound **Upper 3**. Step 4 per-channel map: ch 9 Bass, 10 Drum, 11 Perc, 12-16 Acc1-5 (to compare with the engine's track map in P0.4).
- Not on these pages: the Filters page, MIDI IN Controls, keyboard mode (Split/Full) and split point.

**Immediately before sending** (Claude, read-only, same session, after gates 1-4):

```
$P/pa5x-doctor                                   # exactly one "Pa5X SOUND", uid 1907663670, online
ps -axo pid,comm | grep -Ei "MusicAI|Music.app/Contents/MacOS/Music$|Moises|pa5x|korg|aiarr"   # expect: nothing
```

Stop without sending if: the SOUND uid differs, a second destination matches, FLOW 8 is the only match, any listed process is running, or the photographed MIDI IN ch 1 assignment differs from what the owner expects to hear. The probe itself also refuses anything but one exact name match and requires `--confirm-send`.

**The only command in scope** (one C4, velocity 64, 400 ms, then Note Off and All Sound Off; survives SSH drop):

```
$P/pa5x-probe note --dest "Pa5X SOUND" --channel 1 --note 60 --vel 64 --ms 400 --confirm-send
```

Evidence to record: tool output (bytes sent) + operator: which sound played, that exactly one note sounded and that it stopped. Recovery if anything hangs: `$P/pa5x-probe alloff --dest "Pa5X SOUND" --confirm-send` (covered by the same authorization), or the Pa5X STOP button.

### 1. Basic Note On / Off (transmit)

**Result 2026-11-07 18:25:25: PASS** (owner): exactly one audible note, Gospel Organ on Upper 3 (volume 62), stopped after ~400 ms. **Pitch verification pending** (owner could not judge pitch; "b/#: -5" active). Evidence: `2026-11-07-pa5x-phaseB-retest.md`. The 17:30 attempt was silent (`2026-11-07-pa5x-phaseB-note.md`).

- Command: `$P/pa5x-probe note --dest "Pa5X SOUND" --channel 1 --note 60 --vel 64 --ms 400 --confirm-send`
- Expected: one C4 on the part that MIDI IN ch1 drives (Upper 3 in preset "Hieu"), 0.4 s, then silence.
- Evidence: tool output (`MIDISend` / bytes) + **operator: heard one note, it stopped**.
- Then: `alloff` once, operator confirms nothing changed (silence stays silence).

### 2. Keyboard reception and chord recognition (receive only)
- Command: `$P/pa5x-probe listen --seconds 30 --channel 1 --split 60 --mode fingered`
- Operator plays below the split: C, Am, F/C, G7/B, C/E, C/G (inversions are P0.1), one each, 2 s apart.
- Expected: each Note On/Off printed; detector names match what was played.
- Evidence: listen output. No transmission, so it can run before step 1 is authorized.
- Note: the Pa5X's own chord recognition from MIDI needs Chord 1/2 channels set; with them Off this step tests **AIArranger's** detector only.

**Result 2026-11-07 (accepted, `2026-11-07-pa5x-gate2-chords.md`):**

| Played | Received (Note On bytes) | Channel | Fingered | Fingered on Bass |
|---|---|---|---|---|
| C2 E2 G2 | `93 24` `93 28` `93 2B` | 4 | C major | C major, bass C2 |
| E1 G1 C2 | `93 1C` `93 24` `93 1F` | 4 | C major (bass not carried, by design) | C major over E1 |
| F1 A1 C2 | `93 1D` `93 21` `93 24` | 4 | F major | F major, bass F1 |
| C2 Bb2 E2 | `93 24` `93 2E` `93 28` | 4 | C7 (partial, fifth missing) | not run |

- **Verified channels:** lower part = **MIDI ch 4**; upper part = ch 1 (11:31 run: upper notes also appeared on ch 4 as duplicates; unresolved). The 11-06 record does not list ch 4; do not assume ch 2 is LOWER.
- Chord names were computed offline from the captured bytes by the engine detectors (regression test `tests/midi/test_pa5x_probe.cpp`), not displayed live.
- The listener for this step is therefore `--channel 4 --split 60`.
- **Unresolved intermittent:** an earlier 600 s run (11:36) received 0 notes while Active Sensing continued and no setting was changed (owner confirmed). Comparison and what it rules out: evidence file, "Intermittent receive".

### 2b. Receive repeatability (receive only; before any transmit step)

Purpose: show ch 4 reception is stable, or catch the dropout with timestamps.

1. Start both listeners and note the start time: `$P/pa5x-probe listen --seconds 180 --channel 4 --split 60 --mode fingered` and `$P/pa5x-probe listen --raw --seconds 180` (line-buffered since `addd4bc`, so events show live).
2. The operator plays **C2-E2-G2 ten times**, ~2 s held, ~2 s apart, then **one C5 alone** (upper-part control), and says the time they started and stopped.
3. PASS: 10 lines ending `-> CMajor` on ch 4 in the parsed log, the C5 on ch 1 in the raw log, and FE at ~4/s for the whole run. FAIL (intermittent reproduced): fewer than 10 with FE still arriving; record which repetitions are missing and their times. Run it **three times** (immediately, after 10 min idle, after the Pa5X screen has been changed by normal use). Nothing is transmitted and no setting is changed.
4. **Result 2026-11-07: PASS, 34/34 chords on ch 4 over the three runs** (evidence file, "Repeatability summary"). The earlier dropout was not reproduced.

### 3. NTT low-register regression (transmit; P1.5)
- Purpose: the bass / pad register stays in range across all 12 roots (the last hardware bug was in NTT).
- **Tool gap:** no bounded, non-interactive command plays a converted style through the engine with a 12-root chord cycle on the Pa5X style-track channels. `korg-playback` is interactive, sends on the style's own channels (not ch9-16) and cycles 6 chords. Needs engineering first (see "Gaps").
- Evidence when run: an event log of notes per channel per root + **operator: no octave jump or out-of-register bass**.

### 4. Style-track routing (transmit; P0.4)
- Command, per channel 9..16: `$P/pa5x-probe note --dest "Pa5X SOUND" --channel N --note 48 --ms 500 --confirm-send`
- Expected (Default-preset mapping, manual p.585-588; preset "Hieu" lists ch9-16 = Player 1 Style tracks): ch9-16 → the 8 Style tracks in order. Operator names the track that sounds for each.
- Evidence: 8 tool outputs + operator's track name per channel. This decides whether the engine's `genosStyleTrackMap` matches the owner's preset.

### 5. Style playback (transmit)
- 5a, Pa5X's own arranger via Control channel (P0.5): `$P/pa5x-probe control --dest "Pa5X SOUND" --channel C --cmd variation1 --confirm-send` and `playstop1`. **Blocked**: preset "Hieu" has no Control channel recorded; the owner must set one (a Pa5X settings change, separate approval) or confirm an existing one.
- 5b, AIArranger engine playback to the Pa5X tracks: same tool gap as step 3.

### 6. MIDI Clock, Start, Stop (transmit; P0.6)
- Owner sets Pa5X Clock Source = External (USB) (resets to Internal at power-on, manual p.594; a settings change, separate approval).
- Command: `$P/pa5x-probe clock --dest "Pa5X SOUND" --bpm 100 --beats 8 --confirm-send`, then `--bpm 140`.
- Expected: Pa5X tempo display follows (100, then 140); with a Style selected, Start starts and Stop stops it.
- Evidence: tool output + operator photo of the tempo display + **operator: started and stopped on cue**.

## Gaps before steps 3 and 5b

1. A bounded `pa5x-probe play` (or equivalent): load a UASF, map tracks onto ch9-16 with the same map as P0.4, cycle 12 roots at a fixed tempo for N bars, end with All Sound Off, log every sent event. Fail-closed like the other commands.
2. Confirmation of the owner's MIDI preset (step 0) so the map is chosen from evidence.
