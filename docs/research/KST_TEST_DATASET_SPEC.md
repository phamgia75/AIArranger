# Original Pa5X KST test dataset (D5): specification

Status 2026-11-07: **specified, not created.** Creating it means saving data on the Pa5X, which needs the owner's separate approval (hardware workstream). It is the dataset class the rights matrix (`KST_DATA_PROVENANCE_AND_RIGHTS.md` §2) can clear for byte-level analysis, because every user resource in it has a known origin. Factory content is only *referenced* (factory Sounds / Multisamples by number), never copied into user slots.

## 1. Inputs (provenance: this repository)

`tools/make_kst_test_samples.py <dir>` writes five deterministic WAVs and a `MANIFEST.txt` with sizes and SHA-256 (self-test in CTest):

| File | Root | Length | Use |
|---|---|---|---|
| `T_SINE_C3.wav` | 48 | 0.50 s | MS1 zone 1 |
| `T_SINE_C4.wav` | 60 | 0.75 s | MS1 zone 2 |
| `T_SINE_C5.wav` | 72 | 1.00 s | MS2 (single zone) |
| `T_SINE_G4.wav` | 67 | 1.25 s | Sound C only (an unused-Sound case) |
| `T_CLICK.wav` | 60 | 0.05 s | Drum Kit, two keys |

All 48 kHz, 16-bit, mono, no loop. Import on the Pa5X uncompressed (if the Pa5X offers compression on import, record the choice: compression is a variable of its own, audit §3h).

## 2. Resources to build on the Pa5X (names chosen to be unmistakable)

| ID | Resource | Contents | Relationship it tests |
|---|---|---|---|
| MS1 | User Multisample "T MS SPLIT" | Zone 1: `T_SINE_C3`, keys C-1..B3, root C3; Zone 2: `T_SINE_C4`, keys C4..G9, root **D4** (deliberately ≠ the sample's C4) | Multisample → Sample, key zones, zone root override (audit §3h) |
| MS2 | User Multisample "T MS ONE" | One zone: `T_SINE_C5`, full range, root C5 | A second Multisample |
| SA | User Sound "T SND A" (User bank 1 slot 1) | OSC1 High = MS1, OSC1 Low = MS2 (velocity switch) | Sound → OSC High/Low → Multisample; Sound-level velocity |
| SB | User Sound "T SND B" (slot 2) | OSC1 High = MS1 only | MS1 **shared** by two Sounds |
| SC | User Sound "T SND C" (slot 3) | OSC1 = a Multisample of `T_SINE_G4` | Not used anywhere: the **unused** case |
| SD | User Sound "T SND A" copy (slot 4) | Byte-for-byte copy of SA (Pa5X copy function, if any) | The **duplicate** case (name and content) |
| DK | User Drum Kit "T DK" (UserDK bank 1 slot 1) | Keys C2 and D2 both = `T_CLICK` | Drum Kit → Sample; one sample **shared** by two keys |
| KS | Keyboard Set "T KS" | Upper 1 = SB; other parts factory | Keyboard Set → Sound |
| ST | User Style "T STY" | Recorded from scratch, 1 Variation, 1 bar: Bass track uses SA, Drum track uses DK; other tracks empty | Style Element × track → Sound / Drum Kit |
| PD | Pad "T PAD" | Uses SB | Pad → Sound |

Save all of it as one new KST, **T_KST_V0**.

## 3. Expected oracle (declared before the data exists)

| Check (PA Manager on a copy of V0) | Expected |
|---|---|
| Sample Manager counts | Samples ≥ 5 user (exact number recorded, since the Pa5X may split or add); Drum Samples ≥ 1; MultiSamples: MS1, MS2 and SC's |
| MultiSample Info, MS1 | `T_SINE_C3`, `T_SINE_C4` |
| Search Where MS1 is used | SA (OSC High), SB (OSC High) |
| Search Where MS2 is used | SA (OSC Low) |
| DK Report, T DK | C2 and D2: same RAM sample number |
| Search Where SA is used | ST, Variation 1, Bass track |
| Search Where SB is used | KS (Upper 1), PD; no Style |
| Check for Unused Sounds | SC |
| Check for Duplicates | SA / SD (Content, and Name if the copy keeps the name) |
| Copy Options closure of ST (audit §3i defaults) | SA and DK (tracks on), not SB (KBD SET / PAD off by default) |

A mismatch is itself a finding about PA Manager or the Pa5X, recorded before any parser exists.

## 4. Controlled variants (one change each, saved as separate KSTs)

| KST | Single change from V0 | What a diff isolates |
|---|---|---|
| T_KST_V1 | KS Upper 1: SB → SC | The Keyboard Set → Sound reference |
| T_KST_V2 | MS1 zone 2 root D4 → E4 | The zone root field |
| T_KST_V3 | SA OSC1 Low: MS2 → none | The OSC Low → Multisample reference |
| T_KST_V4 | ST Bass track: SA → SB | The Style track → Sound reference |

## 5. Handling

- Copies only; the originals stay on the Pa5X / USB stick. Every copy gets a manifest (path | size | mtime) and SHA-256, as for D1.
- Stored privately, outside every Git work tree, until legal review (G2, G5) says what may be published. Synthetic WAVs (§1) are public; the KSTs are not, by default.
- PA Manager reports for §3 saved by the owner per `KST_DATA_PROVENANCE_AND_RIGHTS.md` §4.
