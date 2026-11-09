# Resource evidence import and Memory Info validation (design + status, 2026-11-09)

Two optional, offline, read-only inputs to the resource graph (`PA5X_SOUND_RESOURCE_GRAPH.md`):
owner-authorized reference evidence (priority 3) and transcribed Pa5X Memory Info readings (priority 4).
Neither reads KST binary data, sends MIDI or touches the instrument.

## 1. Evidence import (`src/media/sound/evidence_import.h`)

**Status: implemented and tested with synthetic files only. No owner report has been accessed.** Using one
needs the owner's separate authorization (Q18 stays open).

### Why a neutral format
AIArranger does not parse PA Manager's own files or screens. The owner (or an authorized helper) transcribes
the references from an authorized report into `aiarranger-evidence 1`. That keeps three boundaries:
- No reverse engineering of PA Manager output and no copying of its code.
- No KST decoding: the file only holds identities, names and references.
- The owner sees and controls exactly what enters the graph.

### Format
Tab-separated, printable ASCII fields of at most 128 characters each, and at most 16 MiB in total.

```
aiarranger-evidence	1
source	<label>	<owner authorization reference>	pa-manager-report|owner-transcription
node	pa5x:<BANK>/<Kind>/<n>	<name>
ref	<from id>	<Via>	<to id>	confirmed|inferred	<detail>
scanned	<Via>	<what was covered>
end	<number of lines before this one>
```

### Rules (each one is enforced and tested in `tests/integration/test_resource_browser.cpp`)

| Rule | Why |
|---|---|
| One file = one evidence source; every reference keeps `source = PaManagerReport` and `provenance = <label>` | Provenance is preserved and shown on screen |
| The `source` record must carry an authorization reference | The import is optional, and only for authorized material |
| Only `pa5x:` identities are accepted; provisional (`prov:`) ones are refused | A report describes the instrument, not AIArranger packages |
| No `Sample` nodes | A report cannot give PCM facts. A referenced sample stays MISSING, of unknown size, and is counted as such in the budget |
| `confirmed` or `inferred` is required on every reference | An inferred-only user makes a resource **Unprovable**, never In Use or Unreferenced. Mixed users are In Use because of the confirmed one |
| A `pa-manager-report` source cannot declare `SongBookSound` or `ProgramChange` scanned | Q19: PA Manager's coverage of these is unverified, so they stay unresolved. An `owner-transcription` source (the owner checked the instrument) may declare them |
| Package-level vias (Zone, OscHigh, ...) cannot be declared scanned | Those come from packages, not reports |
| Edge types, slot rules and reference sources are checked by `Graph::addRef` | Same validation as packages |
| Two sources giving one identity different names are refused, and the first one is kept | Identity is never re-interpreted silently |
| The same file imported twice is refused | No double counting |
| Strict and all-or-nothing: any bad line, a wrong `end` count or a missing final newline leaves the graph unchanged | Truncation is detectable |

### Where it is used
- CLI: `aiarranger-resources <command> --evidence FILE ... PKG...`
- macOS Resources window: **Open Evidence...** asks for confirmation that the material is owner-authorized.

Report-sourced references are tagged `[evidence: label]` and `INFERRED, not proven` in the details pane.

## 2. Memory Info validation (`src/media/sound/memory_check.h`)

**Status: implemented. Waiting for hardware readings (S6, BLOCKED pending O1-O5 and owner approval).**

The Pa5X Memory Info page (MAN p.1174) shows Samples, Samples Size, Preload Buffer Size, Drum Samples and
Multisamples. The owner types these values as shown on screen, before and after one import:

```
aiarranger-memory-info	1
samples	123
samples-size	12.3 MB
preload-buffer-size	1.5 MB
drum-samples	4
multisamples	7
```

Fields may be blank or missing; spaces may replace the tab.

### What is compared
- **Counts:** for Samples, Multisamples and Drum Samples, the change between the readings is compared with
  the counts in the open packages. A mismatch is reported as evidence for Q6 (how the Pa5X counts), not as
  a fault.
- **Size:** each displayed size becomes a byte range covering both its display rounding (±half of the last
  digit) and both unit readings (1000ⁿ and 1024ⁿ). The growth range is compared with the predicted raw PCM:
  - **CONSISTENT:** raw PCM lies inside the range. The range may be too wide to tell overhead or
    compression apart, and the report says so.
  - **SMALLER:** compression or a different size formula.
  - **LARGER:** per-sample overhead or other data.
- **Preload:** a change in Preload Buffer Size is reported as not modelled.

The report always ends with: "One reading pair does not validate the model; the budget claims raw PCM bytes only."

### Where it is used
- CLI: `aiarranger-resources memory-compare BEFORE AFTER PKG...`
- macOS Resources window: the **Memory Check** tab.

### Not claimed
The budget is exact only for the raw PCM bytes of package samples. Overhead, compression, Preload and the
display's units remain UNKNOWN until several readings from the S6 session exist.

## 3. KST Library workflow (macOS Resources window)

The tab runs the existing read-only tools:
- **Inventory** (`kst-inventory`)
- **Compare Folders** (`kst-diff`)
- **Merge Plan** (`kst-merge-plan`, file-level prediction of Merge and Replace All)

They run off the UI thread. **Semantic Merge** is shown disabled, with the reason: dependency-aware merge
needs validated dependency data (Q17, Q18). Nothing is merged, copied or deleted, and nothing is decided
from filenames alone.
