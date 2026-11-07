# Read-only KST parser: feasibility plan (P1.10 / P1.11)

Status (2026-11-07): **plan only.** No byte of any `.STG` / `.KSC` / `.PDC` / `.PCG` / `.KMP` / `.PCM` file has been inspected for structure. Nothing here claims a decoded format. The design reference is the resource model (`PA5X_KST_RESOURCE_MODEL.md` §3a, §6.3).

## 1. Goal and non-goals

Goal: a **read-only** parser that, for an authorized KST copy, yields the graph of §6.3 with every edge tagged `DecodedValidated`.

Non-goals: writing or repairing KST data; reading PA Manager or Pa5X executables; shipping any proprietary data, even as a test fixture.

## 2. Gates: all must be confirmed before byte-level work starts

Provenance inventory, rights matrix, legal checklist and the private-report proposal: `KST_DATA_PROVENANCE_AND_RIGHTS.md`.

| Gate | Question | Owner of the answer | Status |
|---|---|---|---|
| G1 Data | Which KSTs may be analysed (copies only, kept outside the repo)? Owner rule 2026-11-07: byte-level analysis only for original, user-created or demonstrably authorized data. D1's provenance is UNKNOWN, so D1 is **not** cleared | Owner | **Open** (needs provenance, §1 of the rights doc) |
| G2 Licence | Do the Pa5X and PA Manager licence terms restrict analysing *data files* the owner created or bought? Are any of those KST contents third-party commercial sets with their own terms? | Owner + legal review | **Open** |
| G3 Method | Clean-room only: evidence comes from the data files and from PA Manager's *displayed output*, never from disassembling or debugging PA Manager or the Pa5X OS | Owner sign-off | Proposed |
| G4 Publication | Format notes and code go in the public repo only after legal review; fixtures are synthetic | Owner + legal review | Proposed (consistent with issue #27) |
| G5 Legal basis | The SFF1 note (`importers/sff1-legal-interop-note.md`) cites "EU Directive 2019/790 Art. 6"; the decompilation-for-interoperability rule is, as far as known here, Directive 2009/24/EC Art. 6 and concerns computer programs, not data files. That note needs correcting by legal review, and KST work must not rely on it | Legal review | **Open** |

Until G1, G2 and G5 are answered, work stays at the level of this plan.

## 3. Ground truth (oracle)

A parser result counts only when it reproduces what PA Manager displays for the same copy:

| Objective | Oracle (PA Manager v5.2, audit §3e-3f) |
|---|---|
| Resource identification | Bank / position grids, Sample / Drum Sample / MultiSample lists with counts (14 274 / 4 291 / 483) |
| Sound → Multisample | MultiSample "Search Where this MultiSample is used" (Sound, position, OSC High/Low) |
| Multisample → Sample | MultiSample Info (member sample numbers) and the zone editor (key range, root note) |
| Drum Kit → Sample | DK Report (key, RAM/ROM/compressed, sample number) |
| Shared resources | The same reports: a multisample with several users; a sample on several keys |
| Reverse lookup | Sound "Search Where this Sound is used" (Style Element × track, KBD SET, PAD) |
| Referential integrity | "No Ref (n)", Check for Unused Sounds, Check for Duplicates |

The owner would save these reports **outside the repo** (each has a Save Report button; not pressed so far). Comparisons are automated against those private files; only pass/fail counts are recorded publicly.

## 4. Feasibility per objective

| Objective | Without decoding (today) | With decoding | Main risk |
|---|---|---|---|
| Resource identification | Type, bank folder, extension, size, byte-identical duplicates (`kst-inventory`) | Names, slot positions, sample properties | Encoding may differ by OS version (Pa5X OS 1.x vs Pa4X-origin data tagged PA4X / PA3X / PA900 in PA Manager) |
| Sound → Multisample | Not possible | Oscillator slot → multisample index, FTY vs USR (manual p.1076) | Index spaces: user multisamples are numbered inside the KST's `MULTISMP` data; factory ones refer to ROM |
| Multisample → Sample | Not possible | Zone table: key range, root, level, pitch, sample index | The copy has **one** `RAM.KMP` holding 483 multisamples, so the container layout matters before any record |
| Shared resources | Byte-identical files only | Reference counts from decoded edges | None beyond decoding |
| Reverse lookup | Not possible | Invert the graph; Style refs need `.STG` decoding (Element × track) | `.STG` is the largest and most version-dependent format |
| Referential integrity | Not possible | Dangling references, unused resources, duplicate names | Defining "unused" exactly as PA Manager does (it scans Styles, KBD SETs, Pads) |

Order of attack, cheapest validation first: (1) Multisample → Sample (one container, oracle = MultiSample Info), (2) Sound → Multisample (8 `.PCG` files, oracle = Search Where used), (3) Drum Kit → Sample, (4) Style → Sound (`.STG`, last).

## 5. Method (only after the gates)

1. Hash and size every file of the copy (already done: the 719-entry manifest).
2. Look for **self-describing structure** first (chunk headers, length fields, tables whose counts equal oracle counts such as 483). Record each hypothesis with the oracle value it must reproduce.
3. Accept a field only when it reproduces the oracle on **every** row, and on a second authorized KST where available.
4. Implement it in a bounds-checked reader (the `sf2-check` pattern: every offset validated, every prefix of a file must fail cleanly; ASan/UBSan job).
5. Tag the resulting edges `DecodedValidated`; anything else stays out of the graph.

Controlled-change analysis would be faster: the original dataset and its one-change variants are specified in `KST_TEST_DATASET_SPEC.md`. Creating them needs the hardware workstream and the owner's explicit approval to save data on the instrument.

## 6. Exit criteria

| Phase | Done when |
|---|---|
| F0 Gates | G1, G2 and G5 answered in writing by the owner |
| F1 Multisample → Sample | All 483 multisamples' member sample lists equal PA Manager's MultiSample Info |
| F2 Sound → Multisample | Every "Search Where this MultiSample is used" report reproduced |
| F3 Integrity | "No Ref" and unused-Sound counts reproduced (0 and 12 on the research copy) |
| F4 Style → Sound | Sound "Search Where used" reports reproduced |

## 7. Decisions needed from the owner

1. Is the research copy authorized for **byte-level** analysis (G1), and is any of its content third-party commercial material?
2. Who performs the legal review (G2, G5), and may the result be summarised in the public repo?
3. Will you save PA Manager reports for the oracle (outside the repo)?
