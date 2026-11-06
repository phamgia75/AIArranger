# KORG PA Manager v5.2 on Windows: functional audit

**Status: STARTED (2026-11-06).** The KST Manager file explorer and the Sample Manager main screen have been observed (§3a). **No file has been loaded**, so nothing inside a KST has been seen. This document must not be read as a completed audit.

Each statement carries one of these labels:

- **OBSERVED**: seen directly on the Windows PC, with the date and the evidence.
- **DOC**: taken from KORG documentation, with the source cited. Not yet confirmed on the PC.
- **UNKNOWN**: no evidence yet.

## 1. Environment and route

- Windows 11 PC on the LAN, reached through Microsoft Windows App (RDP) from the Mac. The address and credentials are kept out of the repo.
- Control route: Path A, macOS Computer Use on the RDP window (`docs/research/WINDOWS_RESEARCH_AGENT.md` §5.1). It works in the **foreground only** (`--restore-window`). The owner supervises and keeps hands off during a run.
- Rules (`WINDOWS_RESEARCH_AGENT.md` §4):
  - read-only first;
  - copies only (`C:\PA-Research\work\`);
  - no licence or login handling;
  - no changes to original SET/KST files or to licensing.

## 2. GUI control status

**Cleared 2026-11-06 (supervised session, owner present and authorized foreground).** The Mac was unlocked (`IOConsoleLocked = No`, `CGSSessionScreenIsLocked` absent). The single authorized retry succeeded: `get-app-state` on Windows App returned the accessibility tree and a ScreenCaptureKit screenshot. Input was delivered in **background** mode (no `--restore-window`).

- The RDP session is **one opaque scroll area** in the macOS accessibility tree. PA Manager's own controls are not visible to it, so reading PA Manager is screenshot-only and clicks are by coordinate.
- Background clicks are **unreliable**. The KST Manager tile opened on the first click (after about 3 s). The Sample Manager tile showed only a hover on the first click and opened on the second. Every action needs a fresh screenshot to confirm it.

The earlier blocker is kept below for the record.

### 2.1 Earlier blocker (2026-11-06, resolved)

- **Proof-of-control test: COMPLETE.** 5/5 PASS (Path A, foreground only), recorded 2026-11-06 in `WINDOWS_RESEARCH_AGENT.md` §5.1.
- **GUI capture now fails.** `orca computer get-app-state --app com.microsoft.rdc.macos` returns `permission_denied`: "app 'Windows App' has visible windows but no accessibility window (AX reads stayed blocked for 1500ms after retries)".
  - `orca computer permissions` reports Accessibility and Screen Recording as **granted**.
  - `list-windows` still sees the session window (titled with the PC address, 1008×655, not minimised).
- **Diagnosis (2026-11-06, read-only):**
  - AX reads fail for **every** app, not just Windows App. Finder returns the same `permission_denied`.
  - The helper's code signature and binary are unchanged since 2026-11-05.
  - `ioreg` shows **`CGSSessionScreenIsLocked = true`** and **`IOConsoleLocked = true`**: the Mac's login session is **locked**. While locked, macOS withholds Accessibility windows and screen capture from all clients.
  - **Root cause:** the Mac console lock. It is not a permission or RDP problem, so toggling Accessibility would not fix it.
- **Owner action:** unlock the Mac and keep it unlocked (no screen saver or lock) for the supervised session, keep the RDP window visible, then say "foreground". ORCA then retries discovery and capture **once**. If that fails, it stops and reports. ORCA will not use `--restore-window`, which takes over the screen, unless the owner is watching.
- **Windows-native alternative (no install, no service):** `tools/windows/pa_manager_uia_dump.ps1` is a **read-only** UI Automation dump that the owner runs in PowerShell inside the RDP session, with PA Manager open.
  - It uses the UIA client built into Windows.
  - It sends no input, makes no network access and skips password fields.
  - It writes one text file of the element tree.
  - **UNTESTED** until its first run on the PC.

  It shows how much of PA Manager UIA exposes, which decides whether Path B (Orca on Windows: install + firewall + `orca serve`, all needing owner authorization) is worth it.
- **Also needed for anything past the home screen:** a **copy** of Pa5X KST/SET data in `C:\PA-Research\work\`. No authorized copy exists yet.

## 3a. Observations, 2026-11-06 (read-only, no file loaded)

Screenshots are kept off the repo: the desktop showed personal browser content. Actions taken: two clicks to open the KST Manager and Sample Manager tiles, and one click on the KST Explorer close button. **Not pressed:** Load .KST, Load .SET, Browse, New KST/SET, Rec, Save, Compress, Import, or the memory selector.

**Home screen (OBSERVED).** Tiles for KST Manager (label "PA5X"), SET Manager, Sample Manager (label ".KST .SET") and File Viewer (labels ".STY .PRF .PAD"). Each manager tile has a "New KST", "New SET" or "New SET / KST" button. SET Manager lists the models it covers: PA4X; PA700-PA1000; PA3X - 3x Le; PA900-600-300; PA2X-800-500; PA1X-PA80.

**KST Manager -> ".KST EXPLORER" (OBSERVED).** Opening KST Manager first shows a folder browser, not an editor:

- Prompt: "Select the '.KST' Folder to Load."
- A count of the KSTs found ("3 .KSTs"), a Search box, and quick places: Recent SET List, Desktop, and "Drag & Drop Folder for Quick Access".
- Buttons: "Load .KST" and ".KST Browse…".
- **Without loading**, it shows a summary for the highlighted KST folder. For the KST on the Windows desktop (an **original, not a research copy**): **.KST Size 3.40 GB; STY 1893, PRF 1105, PAD 1029, PCG 16, PCM 231.**

So PA Manager models a KST as a **folder** and classifies its contents into STY / PRF / PAD / PCG / PCM. The meaning of those categories (for example whether PCM means sample files) is **UNKNOWN** until a copy is loaded.

**Sample Manager (OBSERVED, empty).** Title "KORG PA SAMPLE Manager". It opens with a ".SET EXPLORER" overlay that has ".SET" and ".KST" filter toggles (both ON) and lists 5 items: the 3 KST folders and 2 SET folders.

- **Main screen:**
  - Tabs: **Sample [0], Drum Sample [0], MultiSample [0]**; "Auto Play" toggle (ON); << >> paging.
  - Table columns: **ID, Sample Name, File Size, Sample Rate, Duration, Orig Note, Loop Start, Loop End, Type, PreLoad.**
- **Toolbar:** Rec, Search, New SET, a target-model label "PA4X", a settings icon.
- **Right-hand panel** (partly cut off at the window edge):
  - Load SET, Save, New MultiSample;
  - a "COMPRESSION" section with Options, Compress, a 0% progress bar and De-Compress;
  - a text "aif / wav / kmp / …" next to an Import button;
  - "Save Rep…" (probably Save Report; cut off, so UNKNOWN).
- **Footer:** "Available Memory" selector set to **[400 MB] | PA4X**; Used Size 0 [0 bytes]; Total Size 419430400 [400 MB].

Whether the selector offers a Pa5X memory size is UNKNOWN (not opened).

**Import formats seen (OBSERVED as labels only):** aif, wav and kmp for Sample Manager import, and "…" means more.

**State left behind:** Sample Manager is open with the SET Explorer overlay and nothing loaded. The PA Manager home window is underneath.

## 3. Feature inventory

| Area | Status | What is known |
|---|---|---|
| Home screen | **OBSERVED** 2026-11-06 (screenshot, not committed; it contained personal data) | Version **5.2**. Entries: Language (English [British]), New KPM Window, **KST Manager (PA5X)**, **SET Manager (PA4X…PA1X)**, **Sample Manager (.KST/.SET)**, **File Viewer (.STY/.PRF/.PAD…)**. Nothing was clicked |
| KST Manager (Pa5X) | **OBSERVED** (explorer only) | Folder browser with per-KST summary: size, STY/PRF/PAD/PCG/PCM counts (§3a). Editor not seen: nothing loaded |
| SET Manager (older Pa) | UNKNOWN | Not opened. Out of the Pa5X-first scope except for import paths |
| Sample Manager | **OBSERVED** (empty) | Sample / Drum Sample / MultiSample tabs, per-sample loop and rate columns, compression, aif/wav/kmp import, memory budget per model (§3a). Nothing loaded |
| Sound / Multisample / PCM dependencies | UNKNOWN | Sample Manager has a MultiSample tab; no "used by" view seen yet. Needs a loaded copy |
| Style management | UNKNOWN | The File Viewer label lists .STY. Editing ability unknown |
| Keyboard Sets, Drum Kits | UNKNOWN | Not seen |
| Import / export formats | UNKNOWN beyond the home-screen labels (.KST, .SET, .STY, .PRF, .PAD) | |

## 4. Input/output formats

| Format | Source | Status |
|---|---|---|
| `.KST` (Pa5X) | Home-screen labels | OBSERVED as a label only |
| `.SET` (Pa4X and older) | Home-screen labels | OBSERVED as a label only |
| `.STY`, `.PRF`, `.PAD` | File Viewer label ("…" means the list goes on) | OBSERVED as a label only. The full list is UNKNOWN |

## 5. Dependency handling (KST Dependency Inspector feasibility)

What AIArranger can verify **today** without PA Manager:

- File inventory of a KST folder copy: `kst-inventory <folder>` (P1.9, `d2ae12d`). It reports:
  - names, extensions and sizes;
  - top-level folders;
  - byte-identical duplicates.

  It never writes and never follows symlinks. This is **verified file inventory**.

What **cannot** be claimed without more evidence:

- Which Style/STG references which Sound, Multisample, PCM sample, Keyboard Set or Drum Kit. These relationships live inside undocumented binary files (roadmap P1.10, **Blocked**: owner files plus RE approval plus legal review).
- Matching by filename is **not** evidence of a dependency, and the inspector must not infer one from names.

Feasible next step: run `kst-inventory` on an authorized **copy** of a real KST. Then open the same copy in PA Manager's KST Manager and Sample Manager under supervision, and record the dependencies **PA Manager itself displays**: OBSERVED, with screenshots. Comparing the two separates:

- **verified inventory**: files present;
- **PA Manager-reported relationships**: observed in its UI;
- **internal decoding**: unverified, not attempted.

| Compare | Inventory (AIArranger) | PA Manager UI | Status |
|---|---|---|---|
| KST directory structure | `kst-inventory` | KST Manager tree | Needs a KST copy |
| STG references | – (opaque) | ? | UNKNOWN |
| Sounds | count/size only | ? | UNKNOWN |
| Multisamples / PCM samples | count/size only | Sample Manager | UNKNOWN |
| Keyboard Sets | count/size only | ? | UNKNOWN |
| Drum Kits | count/size only | ? | UNKNOWN |

## 6. Implications for AIArranger (provisional)

- No implication is drawn from unobserved behaviour.
- The only established point: PA Manager v5.2 has a dedicated Pa5X KST Manager and a Sample Manager covering .KST/.SET. That makes it the reference tool to compare AIArranger's read-only inventory against.
- AIArranger keeps P1.10 (native KST read/write) **Blocked**.

## 7. Missing evidence and next tests (supervised, read-only, on copies)

1. ~~Clear the §2 blocker~~ Done 2026-11-06. Still to do: a home-screen screenshot with personal data cropped, for the repo.
2. Open KST Manager (Pa5X) with no file loaded: record menus, panels and supported file types from its Open dialog (cancel the dialog).
3. Load the KST **copy**: record the tree, item types, any dependency or "used by" views, and the export options shown (do not run them).
4. Sample Manager on the same copy: record what it lists for Multisamples and PCM samples, and whether it shows which Sounds use them.
5. File Viewer: record the full list of supported extensions.
6. Import/export: list the commands and target formats as shown in the menus. Run one only on a copy, with per-action approval, into `C:\PA-Research\work\out\`.
7. Run `kst-inventory` on the same copy (copied to the Mac or run on Windows), then fill in the §5 table.
8. Stop at once on any licence, login, update or overwrite dialog, and hand over to the owner.
