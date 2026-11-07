# Pa5X Phase B controlled retest: one bounded Note On/Off, 2026-11-07 18:25:25

**Software side:** SUCCESS. **Audible output:** HEARD (owner). Exactly one note and that it stopped: **not yet stated** by the owner.

## Authorization

Owner directive "PA5X PHASE B CONTROLLED RETEST": one note with the exact command below, after the owner raises Upper 3 to about 70/127 without saving the Keyboard Set; no additional notes, Clock, Style playback or 16-channel PANIC. Owner readiness: "Ready".

## Pre-send check and tool output (Mac mini, probe `4e305fa`)

```
18:25:25
check: sound_uid_match=1 sound_count=1 competing=0 scoped_cleanup=1 probe_rev=4e305fa
== SEND 18:25:25
send: Note On ch 1 note 60 vel 64, 400 ms, Note Off, then All Notes Off + All Sound Off on ch 1 only
CoreMIDI accepted 2 MIDI messages (MIDISend noErr; host side only, not proof the device sounded); cleanup follows at shutdown
done. Observe the Pa5X and record what was heard; exit 0 is not an audible result.
exit=0 18:25:26
```

Sent: `90 3C 40`, 400 ms, `80 3C 00`, then `B0 7B 00`, `B0 78 00` (channel 1 only). FLOW 8 not selected.

## Owner's audible observation (verbatim, Vietnamese, with translation)

> "Tôi đã change voice, nghe được tiếng organ"
> (I changed the voice, I heard the organ sound.)

Owner photo after the test (Main > Mixer): **Upper 3 = "Gospel Organ", not muted, volume 62**; Upper 2 Global Concert 105 (L-30); Upper 1 NylonSlide 4x V5 90; Lower Tine EP Distorted muted 97; Pad 4 Steel Guitar RX1 84. Header "b/#: -5", key E, 93 -> 57 bpm on Player 2 "Pop '21". The owner changed the Upper 3 Sound (and the other parts differ from the 18:05 photo), so this is not the same Keyboard Set state as the first test.

## Verdict

- **USB MIDI transmit path Mac -> Pa5X SOUND -> MIDI IN ch 1 -> Upper 3 -> audio: confirmed by ear for the first time.** The sound heard (organ) matches the part routed from ch 1 (Upper 3 = Gospel Organ).
- With "b/#: -5" the C4 would sound as G3 (pitch not reported by the owner).
- Exactly one note, and stop after ~400 ms: **to be confirmed** by the owner.

## The first (17:30) silence, revisited

Between the two tests the Upper 3 Sound changed (Stra. Vel. Pro -> Gospel Organ) and its level rose (22 -> 62); the transport and routing were the same. That fits the level/sound explanation, but **two variables changed at once**, so the 17:30 cause is still not isolated. It does show the transport was not the cause, given the same tool path worked at 18:25.

## Not done

No other message sent. The Keyboard Set changes are the owner's; saving or restoring them is the owner's decision.
