# KORG PA Manager v5.2 on Windows: functional audit

**Status: NOT STARTED beyond the home screen.** GUI control is currently blocked (see §2). Nothing below the home screen has been observed. This document must not be read as a completed audit.

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

## 2. Current blocker (2026-11-06)

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

## 3. Feature inventory

| Area | Status | What is known |
|---|---|---|
| Home screen | **OBSERVED** 2026-11-06 (screenshot, not committed; it contained personal data) | Version **5.2**. Entries: Language (English [British]), New KPM Window, **KST Manager (PA5X)**, **SET Manager (PA4X…PA1X)**, **Sample Manager (.KST/.SET)**, **File Viewer (.STY/.PRF/.PAD…)**. Nothing was clicked |
| KST Manager (Pa5X) | UNKNOWN | Not opened |
| SET Manager (older Pa) | UNKNOWN | Not opened. Out of the Pa5X-first scope except for import paths |
| Sample Manager | UNKNOWN | The label says it works on .KST/.SET. Behaviour not seen |
| Sound / Multisample / PCM dependencies | UNKNOWN | Not seen |
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

1. Clear the §2 blocker; take a fresh home-screen screenshot with personal data cropped.
2. Open KST Manager (Pa5X) with no file loaded: record menus, panels and supported file types from its Open dialog (cancel the dialog).
3. Load the KST **copy**: record the tree, item types, any dependency or "used by" views, and the export options shown (do not run them).
4. Sample Manager on the same copy: record what it lists for Multisamples and PCM samples, and whether it shows which Sounds use them.
5. File Viewer: record the full list of supported extensions.
6. Import/export: list the commands and target formats as shown in the menus. Run one only on a copy, with per-action approval, into `C:\PA-Research\work\out\`.
7. Run `kst-inventory` on the same copy (copied to the Mac or run on Windows), then fill in the §5 table.
8. Stop at once on any licence, login, update or overwrite dialog, and hand over to the owner.
