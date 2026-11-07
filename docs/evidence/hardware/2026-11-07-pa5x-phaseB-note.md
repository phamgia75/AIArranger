# Pa5X Phase B, step 1: one bounded Note On/Off, 2026-11-07 17:30:08

**Verdict (owner):** MIDI TRANSMISSION: SOFTWARE-SIDE SUCCESS. AUDIBLE HARDWARE OUTPUT: NOT CONFIRMED — NO SOUND HEARD. Not PASS; not a proven engine defect; cause not established. All transmission stopped by the owner.

## Owner's audible observation (verbatim)

> * Exactly one audible note: NO — nothing was heard.
> * Sound identity: UNKNOWN.
> * Note stopped after approximately 400 ms: UNCONFIRMED, because no sound was heard.

## Authorization and pre-send check

Owner: "volume safe … You are authorized to send exactly one bounded MIDI test note". Pre-send check at 17:30:07 in the same SSH command (send only if all pass): exactly one destination `Pa5X SOUND`, uid 1907663670, online; FLOW 8 (uid 2078839905) present and not selected; no MusicAIApp / Music / Moises / pa5x / korg / AIArranger process.

## Tool output (Mac mini, probe `c425c6f`)

```
check: sound_uid_match=1 sound_count=1 competing=0
== SEND 17:30:08.003
send: Note On ch 1 note 60 vel 64, 400 ms, Note Off, then All Sound Off
sent bytes so far: 2
done (bytes sent: see above; shutdown adds the silence). Observe the Pa5X and record the result.
exit=0 17:30:08
```

"sent bytes so far: 2" is a mislabel: the value is `CoreMidiOut::sentCount()`, the number of **messages** whose `MIDISend` returned `noErr` (Note On, Note Off). Shutdown's All Sound Off is sent after it and not counted. Fixed below.

## Confirmed facts

1. CoreMIDI accepted 2 messages for the endpoint selected by the exact name `Pa5X SOUND` (uid checked by the pre-send step): `90 3C 40` then, after 400 ms, `80 3C 00`; then, at shutdown, **CC 123 (All Notes Off) and CC 120 (All Sound Off) on all 16 channels** (32 messages, `B0..BF 7B 00` / `B0..BF 78 00`; corrected 2026-11-07, first written as "CC 120" only). Code path: `dispatchOne` counts `sent_count_` only on `MIDISend == noErr` (`src/engine/midi/coremidi_out.cpp`).
2. Nothing went to FLOW 8 (selection is by exact name; a second match would have refused).
3. Pa5X MIDI IN ch 1 = **Upper 3**, from the owner's photo of Settings > MIDI > MIDI IN Channel (~17:25). MIDI OUT ch 1 = Upper 1, ch 4 = Lower. Preset "Ai Arranger". Local Control on.
4. **No earlier bench run has ever confirmed audible output from the Mac to the Pa5X.** The 11-05 audit sent notes and a 25 s live loop with an independent send monitor, but states "Nothing in this log is an ear check". This test was the first ear check of the transmit path.
5. The receive path is verified (Gate 2: 34/34 chords, ch 4 lower / ch 1 upper).

## Unconfirmed assumptions

- That the Pa5X received the bytes (USB-MIDI delivery is not observable from the host; the Pa5X does not echo MIDI IN to MIDI OUT, 11-05 test 5).
- That MIDI IN to a keyboard part sounds when that part is switched off or muted in the current Keyboard Set (manual not available locally; not verified).
- That Upper 3 is on, unmuted, with non-zero volume, and has a sound with an audible attack within 400 ms.
- That nothing in MIDI IN Filters / MIDI IN Controls blocks Note messages on ch 1 (those pages were not photographed).
- That the monitoring chain makes a vel 64 note audible at the "about 1/4" master setting.

## Plausible causes (none established)

| # | Cause | Why plausible | Non-transmitting check |
|---|---|---|---|
| 1 | Upper 3 is **off / muted** in the current Keyboard Set | Many Keyboard Sets use Upper 1 (+2) only; MIDI IN ch 1 targets Upper 3 | Photo of the main screen showing the Keyboard Set and the Upper 1-3 / Lower part on/off states |
| 2 | Upper 3 volume or expression at 0 | Same | Photo of the Mixer / Volume page for the keyboard parts |
| 3 | MIDI IN **Filters** drop Note messages on ch 1 | The Filters page exists and was not photographed | Photo of Settings > MIDI > Filters and MIDI IN Controls |
| 4 | Slow-attack sound on Upper 3, then **All Sound Off** cuts the release | Note Off at 400 ms, then CC 120 within milliseconds silences the tail immediately; a pad/strings sound may barely start in 400 ms at vel 64 | Owner names Upper 3's sound (main screen) |
| 5 | Monitoring too low for one soft note | Master ~1/4, FLOW 8 faders for the Pa5X down; the Pa5X speakers or headphones are the only path | Owner: plays Upper 3 from the keyboard at the same levels (no MIDI) — but see #1, a local press only sounds Upper 3 if it is on |
| 6 | USB-MIDI transmit path to the Pa5X does not deliver to the sound engine at all | Never confirmed by ear before (fact 4) | Not checkable without transmitting; needs the test below |

On item 6 of the owner's list (All Sound Off): it was sent only **after** the Note Off at 400 ms, so it cannot explain a note that was never heard during its 400 ms, unless the sound's attack is slow (cause 4). It does remove any release tail, so "it stopped" cannot be judged from the tail.

## Fix: counter label (regression test)

`pa5x-probe` now prints `probe::sentSummary(n)`: "CoreMIDI accepted N MIDI messages (MIDISend noErr; host side only, not proof the device sounded); All Sound Off follows at shutdown", and ends with "exit 0 is not an audible result". Tests in `tests/midi/test_pa5x_probe.cpp` (messages not bytes; host-side wording).

## Smallest proposed next test (needs fresh, explicit owner authorization)

First, with **no transmission**: owner photos of (a) the main screen with the Keyboard Set and part on/off states, (b) the keyboard-part Mixer/Volume page, (c) MIDI > Filters and MIDI IN Controls; and the name of Upper 3's sound. If Upper 3 is off, the owner decides whether to switch it on (a panel/performance change; we do not change Pa5X settings).

Then, only if authorized: the same single note, longer and clearer, so a slow attack or a soft level cannot hide it:

```
$P/pa5x-probe note --dest "Pa5X SOUND" --channel 1 --note 60 --vel 100 --ms 1500 --confirm-send
```

Same pre-send check, one note, Note Off, All Sound Off. A positive result proves the USB transmit path end to end; a second silent result with Upper 3 confirmed on, unmuted and audible locally points at the transport or the MIDI IN routing, to be investigated further without transmitting.

## Read-only diagnostics (2026-11-07, after the owner's stop; nothing sent)

### Probe channel numbering and bytes (verified by test)

- `--channel 1` parses to wire channel 0, so status `0x90`/`0x80`; the command produced `90 3C 40`, then `80 3C 00`. New test `--channel 1 --note 60 --vel 64 is 90 3C 40, then 80 3C 00` and channel bounds (1-16 only) in `tests/midi/test_pa5x_probe.cpp`; the encoder's channel folding is already covered in `tests/engine/test_coremidi_out.cpp`.
- Shutdown cleanup is `CoreMidiOut::sendAllSoundOff`: CC 123 **and** CC 120 on every channel. Consequence: every `note` command also sends All Notes Off on all 16 channels. Any future authorization must cover that, or the cleanup must be narrowed first (owner decision; not changed).
- Timing: Note On, a 400 ms wait after it was dispatched, Note Off, then cleanup. The cleanup cannot have cut the note **before** 400 ms; it does remove any release tail.

### Official documentation on MIDI IN to a muted part

- Korg, *Pa-Series: Connecting an external controller* (official PDF, model-generic "Pa-Series"), p.11 and p.13: with MIDI IN Ch01 = Upper 3, an external keyboard on channel 1 plays the Upper 3 Sound; "go to the Global > MIDI > MIDI IN Controls page, and see how the **Track Mute Active** parameter has been deselected. This will let you play from an external keyboard any Sound that is set in mute in the Pa-Series instrument." The examples keep Upper 3 **muted** in the Keyboard Set on purpose.
- Pa4X, Pa2X and Pa300 user manuals (same parameter): Track Mute Active **On** = muted tracks do not play received MIDI; **Off** = they do; the Pa4X/Pa2X/Pa300 text applies it to data from MIDI IN and the **USB Device** port.
- **Pa5X-specific text was not found** (the Pa5X manual is not available locally; the 78 MB PDF was not retrieved). That the Pa5X has the same parameter with the same meaning is **UNVERIFIED**.

Reading: a muted or switched-off Upper 3 does **not necessarily** block incoming MIDI. It blocks it if Track Mute Active (or the Pa5X equivalent) is On. So cause #1 needs **two** facts: Upper 3's mute state and the Track Mute Active state on MIDI IN Controls.

### USB MIDI IN to the sound engine

- Pa5X spec sheet (korg.com): "USB to MIDI through the USB Device port"; Pa4X/Pa2X manuals treat USB Device data like the MIDI IN port for these settings. The bench uses the USB Device port (`Pa5X SOUND` destination). Consistent with the documentation; **not verified for the Pa5X**, and no audible confirmation exists yet (fact 4).

### Still needed from the owner (photos, no transmission)

1. Settings > MIDI > **MIDI IN Controls** (Track Mute Active, and anything about velocity or octave).
2. Settings > MIDI > **Filters** (MIDI IN filters for Note on ch 1).
3. **Main screen**: Keyboard Set name, Upper 1/2/3 and Lower on/off (mute) state, the **Upper 3 Sound name**.
4. **Mixer / Volume** page for the keyboard parts (Upper 3 level).

### Smallest safe next hardware test (needs fresh, explicit authorization)

Decide from the photos first; no transmission until then:
- If Upper 3 is muted **and** Track Mute Active is On: the note was blocked by design. The next step is the owner's decision on the panel (unmute Upper 3 or untick Track Mute Active; both are Pa5X state changes we do not make). Then repeat the **same** command (vel 64, 400 ms; no increase without separate approval).
- If Upper 3 can sound (unmuted, or Track Mute Active Off) and its level is > 0: repeat the **same** command once to test reproducibility, operator listening at the Pa5X.
- Either way the command also sends CC 123 + CC 120 on 16 channels at shutdown (above); the authorization must say so.
