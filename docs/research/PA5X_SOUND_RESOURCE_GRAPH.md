# Pa5X Sound Resource Graph: specification

- **Date:** 2026-11-09.
- **Status:** specification. Nothing here is hardware-validated, and no KST file was decoded to write it.
- **Builds on:**
  - `PA5X_KST_RESOURCE_MODEL.md` §3a (observed chain) and §6.3 (graph API design);
  - `PA_MANAGER_PA5X_CONSOLIDATED_RESEARCH.md` (findings K*, M*, S*, O*, R*, C*, cited by id).
- **Labels:** **DOC** Korg manual (MAN p.N) or TX16Wx guide (GUIDE p.N); **OBS** seen in PA Manager v5.2 on the research copy (AUDIT §); **CODE** AIArranger code; **INF** inference with the basis given; **UNK** unknown.

## 1. Graph

```
Style ─(Element × track; also Program Change in its sequences)─▶ Sound ◀─(part)─ Keyboard Set
Pad ─(its single track)─▶ Sound      SongBook Entry ─(KBD Set / Style / Pad Sounds)─▶ Sound      MIDI Song ─(tracks, PC)─▶ Sound
                            │
                  oscillator n (1-24), slot High | Low, Vel SW
                            ▼
                       Multisample ─(zone i: From/To key, Original Note, Level, Pitch)─▶ Sample
Drum Kit ─(key k, layer l: velocity switch)─▶ Drum Sample ─?─▶ Sample (cardinality UNK)
```

| Node | Identity (how others refer to it) | Label |
|---|---|---|
| Sound / Drum Kit | Fixed position in a reference grid (User bank 1-12 / DK bank 1-6, slot 1-128); referrers expect it "where expected" | DOC MAN pp.722-723; OBS AUDIT §3e (K4, K5) |
| Sound position as seen by Styles | MSB/LSB/PC triple, e.g. User `121, 63+bank, slot-1` | OBS figures + INF reading (K10) |
| Multisample | Bank FTY (factory) or USR (user) plus number | DOC MAN p.1077 (O2) |
| Drum Sample | Bank FTY/USR plus number | DOC MAN p.1091 (S6) |
| Sample | Number (PA Manager column "ID") in User Sample memory | OBS AUDIT §3f (M2); DOC MAN p.1159 |
| Style, Keyboard Set, Pad | Name, in name-organized folders | DOC MAN p.722 (S3) |

**Identity is positional, not by content or name.** Two byte-identical samples are two nodes. A name is a label, and the same name can sit at several positions. Every tool must key nodes by (kind, bank, number), never by file name. This rule is already part of the RM §6.3 `NodeId`.

## 2. Edges and reference ownership

| Edge | Cardinality | Ownership | Evidence |
|---|---|---|---|
| Style track → Sound | many-to-one (13 uses of one Drum Kit in one Style) | **non-owning** | OBS AUDIT §3e |
| Keyboard Set part → Sound | many-to-one | non-owning | DOC MAN p.84; OBS (search exists) |
| Pad → Sound | exactly one Sound per Pad (single Pad Element and track) | non-owning | DOC MAN p.937, p.939 (Q11 answered 2026-11-09) |
| SongBook Entry → Sound (its Keyboard Set, Style Sounds, Pad Sounds) | many-to-one | non-owning | DOC MAN pp.390-391 |
| Style sequence / MIDI Song Program Change → Sound | many-to-one, inside MIDI data | non-owning | DOC MAN p.391 ("might be automatically changed by the Program Change messages") |
| Sound oscillator slot → Multisample | 0..1 per slot; one Multisample used 9× by 3 Sounds | **non-owning** | DOC MAN pp.1076-1077; OBS (O9) |
| Multisample zone → Sample | exactly 0..1 per zone; one Sample may sit in several Multisamples | non-owning | DOC MAN pp.1183-1188 (O6); MAN p.1190 "not used by other multisamples" (DOC wording; Q10 asks for a PA Manager confirmation) |
| Drum Kit key layer → Drum Sample | 0..1 per layer; same sample on several keys | non-owning | DOC MAN pp.1090-1091; OBS DK Report (K8) |
| Drum Sample → Sample | UNK (round-robin variations suggest 1..4) | UNK | INF from MAN p.1092 (S8) |

**Ownership rule (DOC):** a referrer never owns its target.
- Deleting Sounds or Drum Kits leaves their multisamples, samples and drum samples in memory. The Pa5X offers separate "Not assigned to any Sound" and "Not assigned to any Multisample/Drumkit" deletes to collect them (MAN p.1191, p.1179).
- Deleting a multisample deletes its samples only on request, and only those "not used by other multisamples" (MAN p.1190).

Garbage collection is therefore an explicit, user-run reference scan, not a cascade.

**Dangling references are legal states (DOC):** a Sound whose User samples are not loaded can be selected and named but plays silent (MAN p.1077, p.1091; S11).

## 3. Copy and remap semantics

| Operation | Behaviour | Label |
|---|---|---|
| Load whole KST with a PCM folder | Deletes **all** User Samples first | DOC MAN p.736 |
| Load individual Sounds | Brings the User Samples they use, keeps the samples already in memory | DOC MAN p.736 |
| Overflow during a load | Aborts; removes only the samples and multisamples just loaded | DOC MAN p.736 |
| Load to an occupied position | "Any item you are overwriting will be deleted" | DOC MAN p.735 |
| Merge vs Replace All | Same-name files replaced, others kept; Replace All deletes the whole type | DOC MAN p.734 |
| USR numbers on a single-Sound load | The incoming multisample/sample numbers must be renumbered when they collide | INF (positional identity + "keeps existing samples"); UNK how |
| Deduplication on load | Whether an incoming sample identical to one in memory is shared or duplicated | UNK |
| PA Manager copy closure | Style copy pulls the User Sounds of 8 tracks (optionally KBD SET, PAD); reMap to an existing duplicate ON | OBS options only, INF behaviour (C5); result UNK (Q5) |
| PA Manager Global Sound ReMAP | Rewrites Style/KBD SET/PAD → Sound references, 4 pairs | OBS dialog, result UNK (C7) |

Copy is a **closure** over forward edges, then a **rebind**: moved targets get new positions, and referrers are rewritten to the new positions. A file-level merge does neither, so moving a grid item breaks its referrers (C4).

## 4. Deletion safety

A delete of node N is safe only when all of these hold:

1. **Reverse lookup complete:** every referrer kind that can point at N's kind has been scanned. For a Sound that means:
   - Styles: track assignments **and** Program Change messages inside their sequences;
   - Keyboard Sets: in the library, in Styles and in SongBook Entries;
   - Pads;
   - SongBook Entries;
   - MIDI Songs.

   (DOC MAN pp.390-391, 937.) PA Manager's "Search Where this Sound is used" was seen covering Style / KBD SET / PAD only (OBS K7). Whether it also scans SongBook Entries and Program Change messages is UNK. A "not used" result from it is therefore not proof of safety.
2. **The source is stated:** each edge carries where it came from (`None` / `PaManagerReport` / `DecodedValidated`, RM §6.3). An empty reverse set from source `None` means "unknown", not "unused".
3. **Shared targets are kept:** deleting N releases its targets only when their other referrers are zero (MAN p.1190 rule).
4. **A backup exists and is verified** (`tools/kst_copy.py` manifest).

AIArranger performs no deletes. This rule constrains reports such as "unused" or "safe to remove": they must say which referrer kinds were checked and from which source.

## 5. Memory budgeting

- **Budget:** 4 GB of uncompressed User Sample memory, or up to 8 GB of samples if compressed. Compression is irreversible on the Pa5X, and Preload samples, loops under 16 samples and very quiet samples are not compressible. (DOC MAN pp.1174, 1176-1177, 1181, 1936; R1-R4.)
- **Unit:** memory is charged per **unique Sample node** reachable from the loaded Sounds and Drum Kits, not per use (INF from sharing, R14). A closure-based estimate is: sum over unique Samples of frames × channels × 2 bytes (uncompressed). This is a lower bound; per-sample overhead is UNK and PA Manager's Used Size formula is UNK (R11).
- **Count limit:** a maximum number of samples exists (MAN p.736; value UNK). PA Manager uses 53 130 (OBS, M9; Q6).
- **Preload buffer:** a separate RAM reservation (MAN p.1174), size UNK on the Pa5X; PA Manager says 420 MB (OBS).
- **Load-order rule:** an overflowing load aborts cleanly (MAN p.736). The plan must state the expected size *before* loading.

## 6. Unknown proprietary format boundaries

| Data | Where it lives | Status |
|---|---|---|
| Sound → oscillator → Multisample reference | inside `.PCG` | UNK, decoding gated (FEAS §2: G1/G2/G5) |
| Multisample zones → Sample | inside `.KMP` | UNK on the Pa5X. KMP is documented for the Triton generation only |
| Sample audio and loop data | `.PCM` | UNK container |
| Style / KBD Set / Pad → Sound | `.STG` / `.KSC` / `.PDC` | UNK |
| Same data from documented exports | Pa5X export of a multisample as TXPROG + WAV, or WAV with `_OrgKey_LowKey_HighKey` names, and sample WAV/AIFF export (MAN pp.1212-1216) | documented, readable without decoding |
| Same data from PA Manager | "Search Where used", DK Report, MultiSample Info, USER SOUND REPORT (OBS) | the owner can save reports (RIGHTS §4, Q18) |

## 7. Comparison with AIArranger's data models

| Spec element | AIArranger today | Match / gap |
|---|---|---|
| Sound with 1-24 oscillators | `Instrument` with 1-24 `Layer`s, softest first (CODE `instrument.h`, `kMaxLayers = 24`) | Match for the writer side, using the guide's convention: one multisample per oscillator, velocity by oscillator Velocity Zone (GUIDE p.14) |
| Oscillator High/Low + Vel SW | not modelled | Gap, intentional: the guide's import path uses one multisample per oscillator. Only needed for reading Pa5X Sounds |
| Multisample zones tiling the keyboard, Original Note separate from sample root | `Zone { lo, hi, sample, root, levelCentibels }`; validation enforces tiling and `lo <= root <= hi` | Match (CODE) |
| Sample shared by several zones | `Zone.sample` is an index into `Instrument::samples`, so sharing works **inside one Instrument** | Partial: there is no sharing across Instruments, and each package carries its own copies |
| Multisample shared by several Sounds | each `Layer` is owned by one `Instrument` | Gap (only matters for reading KSTs) |
| Drum Kit key → layer → sample, shared samples across keys | `DrumKey.layers[].sample` indexes `DrumKit::samples`; `validate` permits sharing, refuses orphans (CODE `drumkit.h`) | Match. A Drum Sample entity and round-robins are not modelled |
| Positional identity (bank, number) | none: names and vector indices | Gap; not needed for writing import packages (the Pa5X assigns numbers on import) |
| Edge provenance | `Graph::Source` designed (RM §6.3), not implemented | Gap; the design is ready |
| Reverse lookup, refcount, closure | designed (RM §6.3), not implemented | Gap; blocked on edges |
| Memory per unique sample | package REPORT: % of 4 GB per instrument; `kst-inventory`: disk bytes of `.PCM`/`.KMP` files | Partial: per package only. No cross-package unique-sample total, no 8 GB/compression line |
| Style → Sound | `--sound-map`: user-supplied bank/PC per Korg track, never inferred (CODE `sound_map.h`) | Export-side reference only (AIArranger writes Styles, so it owns these edges) |
| KST files | `kst-inventory`, `kst-diff`, `kst-merge-plan`: file nodes, no edges (CODE) | File level only (capability levels 1-2) |

## 8. Shortest safe path toward Sound-aware KST management

**Track A: documented exports only, no decoding.** Buildable within current authorization; owner actions are marked.

1. **A1 Graph core on AIArranger's own outputs (SOFTWARE, now):**
   - Implement RM §6.3 `Graph` with `Source`, `usersOf`, `usesOf`, `refCount` and `closure`.
   - Populate it from `Instrument`/`DrumKit` packages, where AIArranger knows every edge because it wrote them.
   - First consumer: a multi-package memory budget over unique samples (recommendations E3, E4), deduplicated by content hash *as a report*. The Pa5X itself may still duplicate them (dedup on load is UNK).
2. **A2 Ingest Pa5X exports (SOFTWARE plus owner exports):**
   - Read TXPROG + WAV exported by the Pa5X (a documented format, which our Python validator already reads).
   - This yields Multisample → Sample edges and content hashes for exported multisamples, with `Source = PaExport`.
   - It does not give Sound → Multisample edges, because the TXPROG export is per multisample.
3. **A3 Ingest PA Manager reports (owner action Q18, then SOFTWARE):**
   - Owner-saved "Search Where used", DK Report and MultiSample Info outputs give Sound ↔ Multisample and Style → Sound edges, with `Source = PaManagerReport`.
   - This parses PA Manager *output* the owner saved, not PA Manager code or KST bytes. Format and permission are to be confirmed.
4. **A4 Sound-level load plan (SOFTWARE):** using A1-A3, report for a chosen set of Sounds:
   - the closure;
   - the unique-sample memory delta against 4 GB and the count limit;
   - grid positions that would be overwritten (MAN p.735);
   - unknown edges, listed as unknown.

   It is executed by the owner on the Pa5X with the documented single-Sound load (MAN p.736 procedure). Advisory only; AIArranger writes nothing.

**Track B: needs authorized KST decoding.** Gates G1/G2/G5 must be answered in writing first (FEAS §2, Q17).

1. **B1** Decode reference fields in `.PCG`/`.KMP`/`.STG` on authorized data only. Validate every edge against the A3 reports (oracle, FEAS §3) before marking it `DecodedValidated`.
2. **B2** Automatic closure, where-used and unused reports on a raw KST copy.
3. **B3** True Sound-aware merge, meaning writing a KST with rebound references: **not planned**. It would need Pa5X-side validation of renumbering (UNK, §3) and a KST writer, which is out of scope by design.

Track A stops at a validated, advisory Sound-level load plan, which is capability level 3 for the data it has edges for. Level 4 (Sound-aware merge) is reachable only through B3, and B3 is not planned.

## 9. Implementation status (2026-11-09, A1 + E4)

| Spec element | Implemented in | Coverage |
|---|---|---|
| Identity separate from name and content | `resource_graph.h` `ResourceId` (`pa5x:<bank>/<Kind>/<n>`, `prov:<package>/<Kind>/<n>`); `SampleInfo.contentHash` is a fingerprint only | Full |
| Kinds | Sound, DrumKit, Oscillator, Multisample, Sample, DrumSample, KeyboardSet, Style, Pad, SongBookEntry, MidiSong | Package graphs populate Sound/DrumKit/Oscillator/Multisample/Sample/DrumSample only. Style, Keyboard Set, Pad, SongBook and MIDI Song nodes exist in the API, but nothing fills them (no authorized source, Q18) |
| Edge types (§2) | `edgeAllowed`: the 11 references of §2, every other combination refused | Full for the API |
| Non-owning references, dangling kept | `addRef` keeps references to absent targets; `dangling()`, `issues()` (silent oscillator, multisample or drum sample) | Full |
| Where used | `whereUsed`, `usedByRoots`, `closure` | Inside the loaded graph only; the CLI states that Pa5X-side referrers are invisible |
| Deletion safety (§4) | `deleteCheck`: InUse / Unprovable / Unreferenced. A Pa5X resource is Unreferenced only when every referrer kind in `referrerVias` was declared scanned. A Sound needs StyleTrack, KeyboardSetPart, PadTrack, SongBookSound **and** ProgramChange. A provisional resource can only be referenced from its own package. A provisional Sound/Drum Kit is Unreferenced (not imported, and no package kind can refer to it). Inner provisional resources are Unreferenced only where references into their kind are verified complete: graphs built from an Instrument/DrumKit in memory are complete. In a package *loaded from disk*, only sample referrers are verified (against the TX files), so its oscillators, multisamples and drum samples stay Unprovable | Full rule; nothing in the code base declares a Pa5X scan yet |
| Copy / remap impact (§3) | `remapImpact`, `Graph::copyImpact` (closure, overwritten positions, destination users affected, unresolved references, provisional resources needing a slot) | Advisory; the Pa5X renumbering itself is UNK |
| Memory (§5) | `budget`, `formatBudget`, `budgetJson`: per unique identity; byte-identical audio under distinct identities is counted and reported; both readings of "4 GB"; 8 GB compressed-equivalent; the compressed figure is an estimate; count against PA Manager's 53 130 | Exact for PCM bytes of package samples. Overhead, Preload and compressibility are not modelled |
| Package boundary | `package_graph.h`: every package writes `RESOURCES.tsv`. The strict parser accepts only provisional identities of that package, only the six package kinds, and self-contained references. The loader:
- re-checks every WAV (bit depth, format, frames, loop, fingerprint);
- cross-checks the manifest's samples against the WAVs named by the package's own `.txprog`/`.txmatrix`, in both directions;
- refuses symlinks, non-regular files and files over 16 MiB;
- enforces slot cardinality (one High and one Low multisample per oscillator, one Sound per oscillator).

 `--kst` summarises an undecoded KST outside the graph | Full |
| Writers | `writePackage` (Sound, also used by the WAV-folder source and the AU sampler), `writeDrumPackage` | Full. The Sound Recipe holds no resources; it stays in REPORT.txt |
| KST tools | `kst-inventory` via `aiarranger-resources budget --kst` (file-level, separate section). `kst-diff` / `kst-merge-plan` are unchanged | No edges from KSTs (track B gate) |

CLI: `aiarranger-resources budget|check|where-used|delete-check|remap`. Exit 0 = ok, 3 = not ok (no fit, in use or unprovable, conflicts), 1 = input error.
