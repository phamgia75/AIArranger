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

1. CoreMIDI accepted 2 messages for the endpoint selected by the exact name `Pa5X SOUND` (uid checked by the pre-send step): `90 3C 40` then, after 400 ms, `80 3C 00`; then CC 120 (All Sound Off) at shutdown. Code path: `dispatchOne` counts `sent_count_` only on `MIDISend == noErr` (`src/engine/midi/coremidi_out.cpp`).
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
