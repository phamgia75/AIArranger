# Gate 1 preparation: an "AIArranger" MIDI preset (operator procedure)

The operator performs these steps by hand on the Pa5X; ORCA changes nothing remotely. The steps are paraphrased from the Pa5X User Manual v1.4 (2025-11-25); page numbers are the printed ones. "Hieu" stays untouched.

## Steps

1. **Duplicate "Hieu" first (p.592-593).**
   - With "Hieu" active, open any Settings > MIDI page and choose **Save MIDI Preset** from the page menu.
   - Rename it **AIArranger** with the Text Edit button, then OK.
   - **Touch Browse.** Pick an **empty location shown as `---`**, then press EXIT.
   - Only then touch Save.
   - **Warning (p.592):** touching Save *without* Browse overwrites the current preset, which would be "Hieu".
2. **Make "AIArranger" the active preset (p.583-584).** Go to Settings > MIDI > General Controls, touch the MIDI Preset button and select **AIArranger**. The MIDI Preset field must read "AIArranger" before you continue. Whether saving to a new location also makes it active is NOT FOUND in the manual, so check the field.
3. **MIDI OUT channels (p.605).** Go to Settings > MIDI > MIDI OUT Channels and set **CH4 = Lower**. Leave CH1 = Upper 1 and every other channel as it is.
4. **Leave MIDI IN untouched** (p.603-604, CH9-16 Style tracks), and leave Clock Source = Internal.
5. **Save the change into "AIArranger" itself.** Use Save MIDI Preset, check that the name shown is "AIArranger", then touch Save. This overwrites the *current* preset, which must now be AIArranger.
6. **SPLIT on.** Press SPLIT so its indicator is lit. The split point is shown at the right of the Keys area on the Main page (p.33). **Read it only; do not change it** (p.102-103).

## Optional, not in the plan above

MIDI OUT can also send a **"Chord"** track: the notes recognized by the Pa5X's own chord engine (p.605). Assigning it to a free channel would let the next test compare the Pa5X's own recognition with AIArranger's on the same input. That needs the operator's decision.

## Not established by the manual (NOT FOUND)

Whether MIDI OUT notes include the Upper/Lower Octave Transpose. The manual describes octave transpose only for **received** notes (p.598-600). The C4 → C5 offset seen on 2026-11-06 stays open until physical keys are compared with received note numbers.
