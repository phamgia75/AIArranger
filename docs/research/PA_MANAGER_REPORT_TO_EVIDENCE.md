# PA Manager reports → AIArranger evidence: compatibility study (2026-11-09)

Question (priority 2): can the reports PA Manager produces be turned into `aiarranger-evidence 1`
(`PA5X_RESOURCE_EVIDENCE_IMPORT.md`) **without decoding KST files**?

**Sources.** This uses only what the read-only sessions already recorded in
`PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md` §3e–3f. No owner report was opened for this study, no
report was saved, and no new session was run.

## 1. Report capabilities observed in the licensed PA Manager

| Report (where) | Content observed | Can become evidence? |
|---|---|---|
| **Search Where this Sound is used…** (Sound/DK tile → Options) | Total uses in the KST. Per Style: bank/position and each Element × track using the Sound. Then "KBD SET – Used n times" and "PAD – Used n times" | **Yes.** `ref` StyleTrack (detail = Element × track), KeyboardSetPart and PadTrack, `confirmed` (PA Manager resolved them from the KST) |
| **Search Where this MultiSample is used…** (Sample Manager) | The Sounds that use a multisample (one multisample used 9× by 3 Sounds) | **Yes.** OscHigh/OscLow `ref` from the Sound's oscillators. The oscillator number and the High/Low slot were not shown, so they are UNKNOWN: the `detail` must say so |
| **DK Report** (Drum Kit) | Per key: Type RAM / ROM / RAM [Comp], Sample No (`n [m]` for RAM), Sample Name; DK size | **Partly.** DrumKeyLayer/DrumSampleOf references. Sample numbers are PA Manager's; how they map to Pa5X sample identity is UNKNOWN (Q6/Q17). The layer index was not shown |
| **USER SOUND REPORT** (Generate Report, SOUND tab) | Per bank: index, Sound name, version, page; KST size | **Nodes only** (Sound identities and names). No references |
| **SOUND Duplicate Report** (Check for Duplicates) | Groups of Sounds with Type Name or Content, and their locations | **No.** Duplication is not a reference. Useful for review, not as graph evidence |
| **Unused Sound Report** (Check for Unused Sounds) | A count and per Sound its location, name and a three-number position | **Never as proof of "unused".** It scans Style banks; whether it covers SongBook and Program Change is unknown (Q19). Under the project rule, a Sound is never judged unused from Styles, Keyboard Sets and Pads alone. At most, nodes |
| **Check Sample Size** | A message box with a Sound's sample footprint (e.g. 8.36 MB) | **No** (not a reference). A Memory Check data point for Q6 at most |

## 2. Can it be done without KST decoding?

**Yes for the content:** every row above is human-readable text that PA Manager computed itself.
Transcribing it into `aiarranger-evidence 1` needs no KST parsing.

The source line records the report and the owner's authorization. For example, a Where-Used result
becomes:

```
source	PAM WhereUsed 2026-11-07 research copy	owner approval <ref>	pa-manager-report
node	pa5x:USR/Style/<n>	<style name>
ref	pa5x:USR/Style/<n>	StyleTrack	pa5x:USR/Sound/<m>	confirmed	Var 2 Perc track
```

The import rules already enforce the scope limits:
- A PA Manager report can never declare `SongBookSound` or `ProgramChange` scanned (Q19).
- "KBD SET – Used 0 times" may be recorded as a `scanned` KeyboardSetPart line only for the KST the
  report covered. It is not proof for what is loaded on the instrument.

## 3. Why no automatic adapter yet

The directive allows adapters "only for documented, authorized formats". Neither condition holds yet:
1. **The saved format is unknown.** Every report window has **Save Report**, but it was never pressed
   (read-only rule). The file type, encoding and layout are unobserved. UI Automation exposes button
   labels but not the report text (§3e, "UI limitations").
2. **There is no authorization for the owner's reports** (Q18 open).

An adapter written now would guess a format, so none is written. Owner-typed transcription into the
neutral format is the supported path today, and it works with the CLI and the macOS Resources window.

## 4. Smallest step that would allow an adapter

With the owner's authorization, on the **research copy** only:
1. Press **Save Report** once for each of: Where Used (Sound), Where Used (MultiSample), DK Report and
   USER SOUND REPORT.
2. Save the files into `C:\PA-Research\work\out\` and commit nothing from them.

From the saved files, record:
- the file type and encoding, and whether the format is stable across report kinds
- whether positions are bank/number or PA Manager-internal numbers

Then write a strict adapter per report kind, with synthetic fixtures modelled on the format and never
copied from owner content. Every reference it produces would be `confirmed` only where the report
names the referrer explicitly, and `inferred` otherwise (for example, an unknown oscillator or layer
index).

Status: **ANALYSIS DONE; ADAPTER BLOCKED** on Q18 (authorization) and the unobserved Save Report format.
