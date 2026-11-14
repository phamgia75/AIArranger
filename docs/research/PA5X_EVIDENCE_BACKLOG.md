# Pa5X research evidence backlog

- **Date:** 2026-11-09.
- **Source:** the 18 open questions in `PA_MANAGER_PA5X_CONSOLIDATED_RESEARCH.md` §5, numbered Q1-Q18 as there.
- **Priorities:** **P0** blocks Sound Conversion or safe KST management. **P1** limits quality, compatibility or functionality. **P2** informational or future enhancement.
- **How each is answered:** HW = Pa5X import or reading a Pa5X screen (needs owner authorization; S6 gates the first import); PAM = supervised read-only PA Manager session on a copy; MAN = manual re-read; OWN = owner decision or material.

## P0

| Q | Question | Why P0 | How | Status |
|---|---|---|---|---|
| Q1 | Does the Pa5X accept AIArranger's TXPROG Sound package? | Without it no conversion output is known to work | HW (S6) | BLOCKED: O1-O5 and owner approval |
| Q2 | Does it accept the TXPERF Drum Kit; drum sample naming; choke numbering | Same, for Drum Kits | HW, after Q1 | BLOCKED (S6) |
| Q17 | Provenance of the research copy D1, and legal answers G2/G5 | Gates every byte-level KST step (track B of the Resource Graph) | OWN plus legal | OPEN |
| Q18 | Will the owner save PA Manager reports privately as edge evidence? | The only no-decoding source of Sound ↔ Multisample and Style → Sound edges (track A3). Without it, Sound-aware management has no edges | OWN | OPEN |

## P1

| Q | Question | What it limits | How | Status |
|---|---|---|---|---|
| Q11 | Pad → Sound reference | Sound deletion safety (Resource Graph §4) | MAN re-read 2026-11-09: one Sound per Pad, Pad Edit > Main (MAN p.937). The same reading found SongBook Entries and Program Change messages as further Sound referrers (MAN pp.390-391) | ANSWERED (DOC). Follow-up **Q19** |
| Q19 (new) | Does PA Manager's "Search Where used" / "Check for Unused Sounds" scan SongBook Entries and Program Change messages in Styles and MIDI Songs? | A PA Manager "unused" verdict could be wrong, and track A3 edges incomplete | PAM | OPEN |
| Q14 | Is Merge name matching case-sensitive? | `kst-merge-plan` flags CASE? conservatively; the plan is safe, but stays ambiguous | HW or MAN | OPEN |
| Q6 | Used Size formula; is 53 130 the sample-count limit? | Accuracy of memory and overflow prediction | PAM, HW (Memory Info photo) | OPEN |
| Q10 | Can one Sample belong to two Multisamples? | Deletion and memory accounting | MAN p.1190 wording says yes (DOC wording, 2026-11-09); PAM to confirm | ANSWERED (DOC wording), confirmation pending |
| Q5 | What "Automatically Copy Related USER Sounds" and RE-MAP MODE do | Copy/rebind semantics in the Resource Graph §3 | OWN authorization to execute on a copy, then PAM | OPEN |
| Q16 | PA Manager export menu and formats | Which documented exports can feed track A2/A3 | PAM | OPEN |
| Q8 | Drum layers per key: 16 or 6 | Rich kits over 6 layers | Resolved on documents (16; TXPERF_DRUMKIT_NOTES); HW to confirm | DOC-RESOLVED, HW pending |
| Q9 | Drum Transpose/Tune ranges and import mapping | Drum tuning fidelity | DOC-resolved (MAN p.1093); mapping by HW | DOC-RESOLVED, HW pending |
| Q15 | Velocity response within an imported layer | Dynamic feel of converted Sounds | HW | OPEN |
| Q3 | Does a Kronos-generation KMP/KSF import? | Kronos route A | OWN material, then HW | OPEN (no material) |

## P2

| Q | Question | How | Status |
|---|---|---|---|
| Q4 | Does a Kronos/Nautilus PCG load at all? | HW | OPEN (route not planned) |
| Q7 | Does PA Manager De-Compress restore the original data? | PAM (options only), OWN | OPEN; not a contradiction (PC tool vs instrument) |
| Q12 | PA Manager orange tile mark; "MG > STD > MG" | PAM tooltips or vendor docs | OPEN |
| Q13 | PA Manager Locked/PreLoad = Pa5X Prot/Prel? | PAM; the Pa5X side is DOC (MAN p.1159) | INF, confirmation pending |

## Grouped next actions (fewest owner sessions)

1. **Owner decision batch (no hardware):** Q17 provenance and legal, Q18 report saving, Q5 authorization for one copy/remap on a copy, Q3 material.
2. **One read-only PA Manager session:** Q6, Q10, Q12, Q13, Q16 and Q19. None needs Save, Copy or Remap.
3. **S6 hardware session (after O1-O5 and approval):** Q1, then Q2. In the same visit, read the screens for Q8, Q9, Q14, Q15 and Memory Info (Q6). One import at a time; on failure, record the exact error and stop.
4. **Manual re-read (done 2026-11-09):** Q11 answered (MAN p.937). The manual gives no numeric sample-count limit: it mentions "the maximum number of samples allowed" (p.736) and the Memory Info "Samples" count (p.1174), but no value (Q6 stays open).
