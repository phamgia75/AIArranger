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

  Reviewed and hardened 2026-11-06 (still UNTESTED on Windows); exact owner-run proof steps: `WINDOWS_RESEARCH_AGENT.md` §5.4 (Path C).
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

## 3b. Session 2026-11-06 (evening): blocked

- Owner priority: the KST copy research. At session start the Mac console was locked (`IOConsoleLocked = Yes`, `CGSSessionScreenIsLocked = Yes`) and `get-app-state` returned `permission_denied` (no accessibility window). One attempt, no retry, no permission change.
- **Not yet verified:** that the authorized copy exists under `C:\PA-Research\work` and is a copy. Nothing on Windows was opened.
- **Done instead:** a documentation-based resource model from the Pa5X User Manual v1.4, in `PA5X_KST_RESOURCE_MODEL.md`. It covers the file types, the "reference grid" rule, Sound → Multisample → Sample and Drum Kit → Drum Sample references by bank + number, and the load/merge/replace and User Sample rules. Every statement there is labelled DOC, OBSERVED or UNKNOWN.

## 3c. Session 2026-11-07: UIA dump + research-copy structure (read-only)

**Route:** owner logged in to RDP; script delivered through Windows App folder redirection (one folder, read-only, removed afterwards). Commands were pasted into a non-elevated Windows PowerShell 5.1 (synthetic keystrokes do not survive Windows App; clipboard paste does).

**Transfer:** `C:\PA-Research\work\pa_manager_uia_dump.ps1`, 4157 bytes, SHA-256 `76D3017E…9313035F` = commit `b31e1dd`. The run was guarded on that hash.

**UIA result (OBSERVED), PA Manager v5.2 home screen:**

- PA Manager is a **Visual Basic 6** application: main window class `ThunderRT6FormDC`; every tile is a `ThunderRT6UserControlDC`.
- 14 elements in its window: 1 `Window` "KORG PA Manager" + 13 `Pane`. Panes expose a **Name** only (New SET / KST, SAMPLE Manager, New SET, SET Manager, New KST, FILE Viewer, New KPM Window, About, English [British], minimise, close; one pane unnamed). **No AutomationIds**, no child elements, no Button/List/Edit types.
- Interpretation (not yet tested): tiles are findable by name and have screen rectangles, so a UIA client can locate them; whether they expose Invoke is UNKNOWN (the dump does not list patterns). Lists and grids inside KST/Sample Manager are UNKNOWN until dumped with those views open.
- Script defect found: the unanchored title pattern also matched browser windows; fixed (anchored default), not yet re-run. The dump file on Windows contains browser UI and stays there.

**Research copy (OBSERVED, `C:\PA-Research\work`):** `Korg Pa5x Gold Set.KST`: 667 files, 1360 MB, plus a separate Pa4X `.set` (1 file).

| Type folder | Banks (sub-folders) | Files | MB |
|---|---|---|---|
| STYLE | 6 | 199 | 68.6 |
| KEYBOARDSET | 11 | 347 | 2.1 |
| PCM | 0 | 99 | 1285.4 |
| SOUND | 0 | 8 | 0.6 |
| SONGBOOK | 1 | 7 | 1.5 |
| SETTINGS | 0 | 2 | 0.0 |
| MULTISMP | 0 | 1 | 2.1 |
| MIDIPRESET, QTPRESET, MAXXAUDIOPRESET, MATRIXPRESET | 0 | 1 each | ~0 |
| PAD | 10 | 0 | 0 |
| CHORDSEQUENCE | 9 | 0 | 0 |
| GUITARPRESET, VOICEPRESET | 0 | 0 | 0 |

Extensions: `.KSC` 348, `.STG` 199, `.PCM` 99, `.PCG` 8, `.SBD` 6, and one each of `.SBL .MXP .GBL .KMP .MPR .QTP .MTP`.

Differences recorded without explanation:

- `.KSC` count (348) is one more than the KEYBOARDSET folder's file count (347): one `.KSC` sits in another folder (not identified yet).
- `.MTP` is not in the manual's file-type table as read so far (UNKNOWN).
- PAD and CHORDSEQUENCE have bank folders but no files.
- A **PCM** folder is present: per manual p.736, loading this whole KST deletes the User Samples in memory.

**Not done in that first run (done later, §3d):** opening the copy in PA Manager's KST Manager, and the `kst-inventory` comparison. The RDP session ended when the Windows App configuration was saved (folder redirection removal), and re-authentication is the owner's. A manifest of the copy was prepared but not yet captured.

## 3d. Session 2026-11-07 (later): copy opened in KST Manager, read-only

**Access (VERIFIED):** saved Windows App credentials (macOS Keychain, entered by the owner) + certificate pinned with "Always trust" only after its SHA-256 matched the owner-approved fingerprint. Two disconnect/reconnect cycles: 32 s (first, cert check) and 4 s (no prompt). No credential was typed by ORCA.

**Opening the copy:** KST MANAGER tile → ".KST EXPLORER" (defaults to the Desktop KST folder, where an **original** KST is pre-selected: not loaded) → ".KST Browse…" → Windows "Browse For Folder" tree → `C:\PA-Research\work\Korg Pa5x Gold Set.KST` → OK. PA Manager shows "Please Wait… Loading… <KST>\<TYPE>" per type folder, then the KST view (load took well under a minute).

**KST view (OBSERVED, screenshot):**

- Header: KST path; tabs STYLE / KBD SET / PAD / SOUND; summary tiles STY / PRF / PAD / PCG; Style bank list (6 banks; FAVORITE01 = 36 items; pages P1-P6); right column: Load KST, Save, Generate Report (STYLE), Copy from another KST, Advanced Settings. None of these were pressed.
- **Memory INFO** panel, model selector "[4 GB] | PA5X": Used 1 403 084 800 B (1.31 GB), Total 4 294 967 296 B (4.00 GB), Available 2 891 882 496 B (2.69 GB); Sample 14274 / 53130; preLoadBuffer 224 MB / 420 MB. So PA Manager budgets **4 GB** of Pa5X sample memory (PA Manager's figure; the manual value is still not found). The meaning of "Sample 14274 / 53130" (count of sample slots?) is UNKNOWN.

**UIA with the KST view open (OBSERVED):** 2 windows of the PA Manager process ("KORG PA Manager v5.2", "Memory INFO"); 90 elements = 88 Pane + 2 Window; classes `ThunderRT6UserControlDC` 80, `ThunderRT6PictureBoxDC` 6, `ThunderRT6FormDC` 2, `Edit` 2. 61 named. Exposed: every button label, tab and page control, the model selector text, and the **names of the 10 Style tiles on the visible page**. Not exposed: memory figures (drawn in picture boxes), the Edit values. Conclusion: UIA can locate controls by name and read the visible list page; counts and memory need screenshots; no AutomationIds.

**Integrity (VERIFIED):** manifest of the copy (relative path | size | mtime UTC, 719 entries incl. folders) taken before loading and after the UIA run: identical SHA-256, 0 differences. Original KST data not touched.

**Input method:** Windows App mangles synthetic keystrokes; commands go through the Mac clipboard (synced on app activation) and a right-click paste into Windows Terminal. A right-click once landed on a browser window because the terminal was not in front (context menu dismissed, no effect); the procedure now brings the terminal forward from the taskbar and checks a screenshot before every paste.

## 3e. Session 2026-11-07: resource tabs, reports and reverse dependencies (read-only)

All on the research copy. No Save / Save Report / Export / Copy / Remap / Delete / Rename was pressed; context menus were opened to read them and dismissed with Escape. Integrity re-checked afterwards: manifest identical (719 entries, 0 differences). Names of user content are not reproduced here.

| Area | OBSERVED |
|---|---|
| **STYLE** | 6 banks; bank 1 = 36 items; 10 tiles per page, pages P1-P6; tile shows name, source tag (e.g. "PA5X OS 1.2") and slot number |
| **KBD SET** | 11 banks (USER01-USER11); USER01 = 9 items; same 10-per-page grid; empty slot shown "- - -"; tile tag "PA5X" |
| **PAD** | No banks listed, all slots "- - -", header shows "NEW KST" (the copy has 10 empty PAD bank folders) |
| **SOUND** | "USER SOUND": 12 banks (User 1-12), "MEMORY User 1 128/128" → **128 slots per bank**; tiles tagged with source model (PA4X, PA3X, PA900); some tiles carry an orange corner mark (meaning UNKNOWN) |
| **DRUM KIT** | Same tab, "DK 1-6" switch → "USER DK": 6 banks (UserDK 1-6), UserDK 1 = **119/128** |
| Slot totals | 12×128 = 1 536 User Sounds, 6×128 = 768 User Drum Kits: matches manual p.1935 (PA Manager's model; not checked on hardware) |
| Tile context menu (KBD SET) | Cut, Swap, Copy, Delete, Rename, Icon |
| Tile context menu (SOUND/DK) | Cut, Swap, Copy, Paste, Delete, Reset Sound, Rename, Options ▸ {Check Sample Size, **Search Where this Sound is used…**, Export All Samples to .Wav, …} |
| Other actions | Load KST, Save, Generate Report (per tab), Copy from another KST, Advanced Settings, **RE-MAP MODE** (Sound tab), User DK Report / DK Report |

**Reverse dependency (OBSERVED): "Search Where this Sound is used…"** scans every Style bank ("STYLE Bank n / 6" progress), then reports for one Drum Kit: total uses in the KST (13); per Style: its bank/position and each **Style Element × track** that uses it (Intro 1-3, Var 1-4, Fill 1-4, Break, End 3 on the Perc track); then "KBD SET - Used 0 times" and "PAD - Used 0 times". So PA Manager resolves Style→Sound per Element and track, and Keyboard Set→Sound and Pad→Sound, from the KST data. *How* those references are encoded is still undecoded.

**Drum Kit report (OBSERVED, "DK Report"):** header (KST path, DK name, DK size 8.36 MB, position, version PA4X); then one row per key from C-1: Type (**RAM** = user sample, **ROM** = factory sample, **RAM [Comp]** = compressed user sample), Sample No (RAM: two numbers `n [m]`; ROM: one number), Sample Name. The same user sample number appears on more than one key → **samples are shared between keys**.

**Check Sample Size (OBSERVED):** a message box with the Sound's sample footprint (e.g. 8.36 MB, equal to the DK report's size).

**USER SOUND REPORT (OBSERVED, Generate Report on SOUND):** per bank: index, Sound name, Sound Version (source model), page; KST size. No multisample or sample references for ordinary Sounds.

**UI limitations:** reports are scrollable read-only text windows (each with "Save Report"); bank contents appear only after clicking a bank; the DK switch keeps the previous grid until a bank is clicked; counts are per bank (no KST-wide totals except in reports); UIA exposes tile names of the visible page and button labels, not report text (not yet tested: the report window may expose its text).

**Not yet inspected:** Sample Manager with the copy loaded (P1), Sound → Multisample for non-drum Sounds, RE-MAP MODE behaviour, duplicate detection, Copy from another KST (merge), Advanced Settings.

## 3f. Session 2026-11-07 (2): Sample Manager, duplicates, unused Sounds, remap and merge UI (read-only)

Same research copy. Nothing was saved, applied, remapped, copied, compressed, exported or deleted: dialogs were closed with **Close** / **Cancel** / the window X, menus dismissed with Escape. One app-level prompt (**Select TARGET Keyboard**, "Keyboard Model Environment", PA5X preselected) was answered **OK** with PA5X: it sets the view mode, not KST data. Integrity re-checked afterwards: manifest identical (719 entries, 0 differences). User content names are not reproduced.

**Settings menu (title-bar sliders icon):** File Viewer, New KPM Window, Memory Info Window, Sound Settings, Check for Duplicates [PCG], Check for Unused Sounds, Global Sound ReMap, Delete All Locked items [PCG] (disabled), Stream Drive (C, 2.50 GB), RAM / Memory.

| Feature | OBSERVED |
|---|---|
| **Check for Duplicates [PCG]** | Scans the KST, then a "SOUND Duplicate Report": count (104 here), groups of Sounds with Type **Name** (names equal apart from a suffix/number) or **Content**, and each one's Location (bank, position). Report only; Save Report not pressed |
| **Check for Unused Sounds** | Scans every Style bank, then an "Unused Sound Report": count (12 here), per Sound: Location, Name, and **Sound Pos as three numbers** |
| Sound Pos numbers | User bank *n*, position *p* → `121, 63+n, p-1`; User DK bank → `120, 64, p-1` (e.g. User 2 pos 38 → 121, 65, 37). Read as **Bank Select MSB, LSB, Program Change**; this reading is an interpretation, consistent on all 12 rows, not checked on the Pa5X |
| **Global Sound ReMAP** | "Change & Replace All Sound Instances of: SOUND A with SOUND B throughout the whole SET"; up to 4 pairs A1-A4 → B1-B4, each ON/Off; scope checkboxes STYLE, KBD SET / PERF, PAD. Cancelled |
| **RE-MAP MODE** (Sound tab) | Toggle: header becomes "USER RE-MAP", work area outlined, "Copy from another KST" disabled. What a tile click does in this mode was **not** tried (UNKNOWN). Toggled off |
| **Copy from another KST** | Opens a second KST pane (Load SET, Copy Options, "**Automatically Copy Related USER Sounds**" ON by default, a `<<` copy button) and a KST explorer. No source KST was loaded |
| KST explorer summary of the copy | ".KST Size 1.26 GB", STY 199, PRF 347, PAD 0, PCG 8, PCM 99. These counts equal the file counts per type folder (audit §3c), so the explorer counts files |

**Sample Manager** (launcher → SAMPLE Manager → Load .SET on the copy; reads `…KST\MULTISMP`, "Version: 7,0"):

| Item | OBSERVED |
|---|---|
| Counts | Sample **14 274**, Drum Sample **4 291**, MultiSample **483**; "No Ref (0)" (no unreferenced samples flagged) |
| Sample columns | ID, Name, File Size (bytes), Sample Rate (shown as "44100 kHz / 16bits" — the unit label is PA Manager's), Duration, Orig Note, Loop Start, Loop End, Type (Normal / Compressed / Locked), Stereo (mono/stereo), PreLoad (checkbox) |
| Loop and size | Loop points are in sample frames (898 ms × 44.1 kHz ≈ the loop end). A **Compressed** sample's file is about **1 byte per frame** where 16-bit PCM needs 2 (interpretation of a few rows) |
| MultiSample columns | ID, Name, Unlinked Samples, Zones, File Size |
| MultiSample menu | Properties, Rename, Get MultiSample Info, **Search Where this MultiSample is used…**, Export MultiSample, MultiSample Replace, Options ▸, Compress MS, De-Compress MS, Delete |
| Get MultiSample Info | Member samples: sample number, name, size; total |
| **Search Where this MultiSample is used** | Per Sound: name, User bank / position, and the oscillator slot as **OSC High: n / OSC Low: n**. One multisample was used 9 times by 3 Sounds → **Sound → Multisample is OBSERVED and multisamples are shared between Sounds** |
| Zone editor (Properties) | "MULTISAMPLE Manager": keyboard map of zones; per zone: sample, **From / To key**, **Original Note**, Level (dB), Pitch; Add Zone / Delete Zone / Save Changes / Close. **No velocity range per zone**: velocity layering appears only at Sound level (OSC High / Low). MIDI audition shown "Not Connected". Closed without saving |
| Drum Sample columns | ID, Name, Category, Stereo; a lower pane for the selected item's samples |
| Compression | Compress SET / De-Compress SET, a "34%" bar (meaning UNKNOWN: likely the compressed share), Options ▸ {Options ▸, Threads ▸} |
| Import formats | Label "aif / wav / ksf / kmp / rex" next to Import (not used) |
| Memory profiles | 8 MB / 16 MB PA80, 32 MB PA1X/PA300, 64 MB PA800, 96 MB PA600, 128 MB PA2X/3X/700, 192 MB PA900, 256 MB, 480 MB PA1000, 400 MB PA4X, 1.5 GB PA4X OS Next, **4 GB PA5X** (Total Size 4 294 967 296 B). Sample-count limit with PA5X: 53 130; preLoadBuffer 420 MB. PA Manager's figures, not hardware-verified |
| Memory vs disk | PA Manager "Used Size" 1 403 084 800 B; the copy's PCM folder ≈ 1285 MB and the explorer's ".KST Size 1.26 GB". How PA Manager computes Used Size is UNKNOWN; it is not simply the PCM file total |

**Procedure incident:** one paste into Windows Terminal showed a multi-line-paste warning whose preview was **unrelated text from the shared Mac clipboard** (another session had replaced it). Cancelled; nothing was pasted or run. Since then the clipboard is compared to the intended command immediately before each paste (`WINDOWS_RESEARCH_AGENT.md` §5.8).

## 3g. Session 2026-11-07 (3): stopped on an unexplained window

On reconnecting at 08:41 the session showed a **File Explorer window open on the research copy's `MULTISMP` folder** that this agent did not open (between 07:54 and 08:41 someone, or something else, used the PC). The integrity check run immediately afterwards (command recalled from PowerShell history, no clipboard) showed the copy unchanged: 719 entries, 0 differences. GUI research was **stopped** until the owner confirms who else is using the PC. Not yet observed, therefore: Copy Options dialog, Compress Options / Threads submenus, PreLoad semantics, RE-MAP MODE tile behaviour, and what "Automatically Copy Related USER Sounds" copies. The last two can only be seen by performing a remap or a copy, which needs separate authorization even on the copy.

## 3h. Session 2026-11-07 (4): compression options, threads, PreLoad, Copy Options

The owner confirmed opening the File Explorer window of §3g; the session was cleared. Integrity before (09:14) and after (09:18): 719 entries, 0 differences. Commands were recalled from PowerShell history; no clipboard was used. Menus were expanded with the keyboard (Down / Right) and closed with Escape; nothing was selected.

| Item | OBSERVED |
|---|---|
| Compress ▸ Options | **✓ Compress All Samples** (current), or "Duration Above > 1 / 2 / 3 / 4 / 5 / 10 Sec": compression can be limited to samples longer than a threshold |
| Compress ▸ Threads | 1, 2, **✓ 3** (current), 4 Thread(s): PC-side compression worker count |
| PreLoad | A per-sample checkbox; every visible row unchecked, yet "preLoadBuffer: 224 MB / 420 MB" (PA5X profile; 300 MB in the PA4X profile). What PreLoad marks and how the buffer is filled is **UNKNOWN**. Clicking the column header does not sort |
| Root note, two levels | Samples 43-47 show Orig Note C4 in the Sample tab, while the zone editor showed Original Note **G3** for the zone using sample 43: a Multisample zone can override a sample's own root note |
| Sample rates seen | 24 000, 44 100 and 48 000 Hz, all 16-bit, in one KST |
| Copy Options | Not reachable: the KST explorer is modal until a **source** KST is loaded into the right pane. Loading one was not done (it puts the `<<` copy button one click away); needs the owner's decision |

## 3i. Session 2026-11-07 (5): Copy Options (source loaded, nothing copied)

Owner-authorized: the research copy was loaded as the **source** of "Copy from another KST" (so source and target were the same copy). Copy Options was read tab by tab and closed with the window X (not OK); the source pane was collapsed with "<". The `<<` copy button was not touched. Integrity before (09:37) and after (09:41): 719 entries, 0 differences. No write prompt or unexpected dialog appeared.

Copy Options, "AUTO SOUND COPY" (current values):

| Tab | Options |
|---|---|
| STYLE | Automatically Copy Related USER Sounds **ON**; "Copy STYLE Tracks User Sounds" per track BASS, DK, PERC, ACC1-5 all **ON**, with a Style / Variation selector; "Copy KBD SET User Sounds" STS1-4 Off; "Copy PAD User Sounds" PAD1-4 Off; Do not Copy Muted Sounds **ON**; Do Not copy ZERO Volume Sounds Off; Do Not copy a Sound from an Empty Track Off; reMAP Missing USER Sound to Factory Off; reMAP to Factory Sound Off; Change Volume to ZERO Off; Convert MG > STD > MG **ON** |
| KBD SET | Automatically Copy Related USER Sounds **ON**; parts UPPER 1, UPPER 2, UPPER 3, LOWER **ON**; same muted / zero-volume / reMAP-to-Factory / volume-zero filters |
| PAD | Automatically Copy Related USER Sounds **ON** (no other options) |
| SOUND | Automatically Copy Related USER Sounds **ON**; "Check for Duplicates": reMap to an existing Sound if a Duplicate is found **ON** |

Reading (interpretation of labels, not tested by copying): copying a Style brings the User Sounds of its tracks, and optionally those of its linked Keyboard Sets (STS1-4) and Pads; muted and zero-volume parts can be skipped; a missing User Sound can be remapped to a factory Sound; a Sound that already exists in the target is reused instead of duplicated. "MG > STD > MG" is not explained in the UI (UNKNOWN).

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
