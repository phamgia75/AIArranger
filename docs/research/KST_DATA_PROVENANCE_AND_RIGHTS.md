# KST data: provenance, rights and legal-review checklist

Companion to `KST_PARSER_FEASIBILITY.md` (gates G1, G2, G4, G5). Status 2026-11-07: **no dataset is cleared for byte-level analysis.** Possession of a file is not treated as a right to analyse or redistribute it. This document records facts and open questions; it is not legal advice.

Datasets are referred to by ID. Names, paths and contents of the owner's files stay out of this repository.

## 1. Dataset provenance inventory

| ID | What it is (observed) | Where | Origin | Rights holder | Cleared for |
|---|---|---|---|---|---|
| D1 | Research **copy** of a Pa5X KST: 667 files / 719 manifest entries, 1.26 GB; manifest SHA-256 recorded on the PC | Windows research folder | **UNKNOWN.** PA Manager tags its Sounds with source models PA4X / PA3X / PA900, so the content was at least partly converted from older instruments' data; the set name suggests a distributed collection | **UNKNOWN** | PA Manager observation (owner authorization 2026-11-07). **Not** byte-level analysis |
| D2 | The original of D1's kind on the Windows desktop (3.40 GB KST) | Windows desktop | UNKNOWN | UNKNOWN | Nothing: never opened, never to be modified |
| D3 | A Pa4X `.SET` folder (1 file) next to D1 | Windows research folder | UNKNOWN | UNKNOWN | Nothing yet |
| D4 | Other KST / SET folders listed by PA Manager's explorer (3 KST + 2 SET entries) | Windows | UNKNOWN | UNKNOWN | Nothing |
| D5 | Data the owner would create on the Pa5X for research (e.g. a new KST with one user Sound) | Pa5X → copy | Owner-created (if made) | Owner, subject to the Pa5X licence for any factory content referenced | Candidate for byte-level analysis once made (needs hardware approval to save on the Pa5X) |
| D6 | Synthetic files AIArranger generates itself | Repository tests | AIArranger | Project | Everything, including public fixtures |

**Owner input needed for D1-D4:** where each came from (made by you, bought, downloaded, shared), any licence or terms that came with it, and whether it contains third-party samples.

## 2. Rights / limitations matrix

Columns are dataset classes; cells say what this project may do **after** legal review confirms the class. "No" means no, whatever the review says, unless the owner records an explicit exception.

| Action | Owner-created (D5) | Bought / licensed commercial set | Free / shared by a third party | Unknown provenance (D1-D4 today) | Synthetic (D6) |
|---|---|---|---|---|---|
| Observe in PA Manager, read-only | Yes | Yes, if the licence allows use on a PC | Per its terms | Yes (authorized for D1 only) | n/a |
| Hash / list files (no content) | Yes | Yes | Yes | Yes (done for D1) | Yes |
| Byte-level format analysis, kept private | Yes, after G2/G5 | Only if the licence and law allow it for interoperability | Per its terms | **No** | Yes |
| Record format *facts* publicly (field layout, no data) | After legal review | After legal review | After legal review | **No** | Yes |
| Commit file content or excerpts (fixtures, hex dumps) | Owner decides; prefer synthetic | **No** | **No** | **No** | Yes |
| Commit PA Manager reports | No (private by policy) | **No** | **No** | **No** | n/a |
| Commit pass/fail counts from oracle comparisons | Yes | Yes | Yes | Yes | Yes |
| Redistribute the data | Owner decides | **No** | Per its terms | **No** | Yes |

## 3. Legal-review checklist

For the reviewer, in order:

1. **Provenance:** for each dataset to be analysed, a written statement of origin and any licence text (§1).
2. **Instrument licence:** do the Pa5X owner's manual / software licence terms restrict analysing data files the instrument writes? (Read the actual text; not yet collected.)
3. **PA Manager licence:** the project neither decompiles nor debugs PA Manager. Confirm that using its *displayed output* as an oracle, and documenting its UI behaviour, is within its licence.
4. **Third-party content:** sample sets, Styles and Sounds inside a KST may be separately copyrighted. Analysing their *encoding* is a different question from using their *content*; confirm both.
5. **Legal basis for interoperability work** in the owner's jurisdiction. Correct the SFF1 note's citation (`importers/sff1-legal-interop-note.md` cites EU Directive 2019/790 Art. 6; the decompilation-for-interoperability rule known here is Directive 2009/24/EC Art. 6, which concerns computer programs, not data files).
6. **Publication:** what may be published (format notes, code, test data), under which licence, and whether issue #27 (corpus privacy) sets precedent.
7. **Trademarks:** wording for any compatibility claim ("works with Pa5X" vs "compatible"), consistent with the hardware-validation rule.
8. **Record:** the reviewer's conclusions, dated, stored privately; a public one-line summary only if the reviewer agrees.

## 4. Proposal: internally stored PA Manager reference reports

Purpose: the oracle of `KST_PARSER_FEASIBILITY.md` §3, without putting private data in Git.

- **Who saves them:** the owner (each report window's **Save Report** writes a file; that is a save action and is not done by the agent).
- **Which:** per dataset cleared for analysis: USER SOUND report, every DK Report, Sound Duplicate report, Unused Sound report, MultiSample Info and "Search Where this MultiSample is used" for a sample of multisamples, "Search Where this Sound is used" for a sample of Sounds.
- **Where:** outside every repository, e.g. `C:\PA-Research\reports\<dataset-id>\` on the PC, mirrored to a private folder on the Mac (not under any Git work tree, not in a synced public location).
- **Integrity:** a `SHA256SUMS` file per folder; the dataset's manifest hash recorded with it, so a report is always tied to the exact copy it describes.
- **What enters the repo:** only dataset IDs, report hashes, and comparison results (counts, pass/fail). Never report text.
- **Retention:** kept while the parser work is active; deleted on the owner's instruction.
- **Access:** owner and the agent session working on the copy; not attached to issues, PRs or chat logs.
