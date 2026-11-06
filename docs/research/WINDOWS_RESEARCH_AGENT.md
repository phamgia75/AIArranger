# Windows Research Agent — capability audit and plan

- **Date:** 2026-11-06
- **Purpose:** let ORCA/REA observe and operate the owner's licensed KORG PA Manager on a separate Windows Pro PC on the LAN, to build the feature-parity research (`docs/KORG_PA_MANAGER_FEATURE_PARITY.md`) from observed behaviour instead of vendor marketing.
- **Status:** **Path A VERIFIED (2026-11-06), foreground input only.** All five steps of §5 passed through the Windows App RDP window. Path B is not set up.
- Host addresses, account names and pairing codes are deliberately **not** recorded in this public repository.

## 1. Audit results (2026-11-06, from the Mac)

| Check | Result |
|---|---|
| Mac on the LAN | Yes, same /24 as the Windows PC |
| Windows PC reachable | TCP 3389 (RDP) **open**, TCP 135 (RPC endpoint mapper) open. ICMP ping blocked. TCP 22 (SSH), 445 (SMB), 5985/5986 (WinRM) closed or filtered. |
| Remote shell / remote execution into Windows | **None available.** No SSH, no WinRM, no SMB. Nothing was enabled. |
| RDP client on the Mac | Microsoft *Windows App* (`com.microsoft.rdc.macos`), running |
| Mac GUI automation | `orca computer` (Orca 1.4.250, provider `orca-computer-use-macos`): click, type, keys, scroll, drag, screenshots, accessibility tree, **background input**. No OCR. |
| Mac GUI automation permission | Initially `permission_denied`; **granted** by the owner (Accessibility + Screen Recording) later the same day. |
| Remote Orca runtime | Supported: `orca serve [--grant-desktop-control]` on the remote machine, paired from the Mac with `orca environment add --pairing-code …`. A paired client cannot drive the remote desktop unless the offer was created with `--grant-desktop-control`. |
| Other automation on the Mac | No FreeRDP, no PowerShell. SSH client only. |

**Conclusion (after §5):** ORCA can see and drive the Windows desktop through the RDP window (Path A). Path B remains optional, for an accessibility tree.

## 2. Recommended architecture

### Path A — drive the RDP window on the Mac (no Windows changes)

```
ORCA (Mac) --orca computer--> Windows App window (Mac) --RDP 3389--> Windows session --> PA Manager
```

- Needs only the two macOS permissions for *Orca Computer Use*.
- The remote desktop is a **bitmap**: no Windows accessibility tree, no UI Automation. ORCA works from screenshots and window-local coordinates, and reads text visually from screenshots (the provider has no OCR).
- Good for: observing screens, menus and dialogs; capturing screenshots for the parity matrix.
- Weak for: reliable clicks on small controls, reading lists/grids, anything long-running.

### Path B — Orca runtime inside the Windows interactive session (recommended for real research)

```
ORCA (Mac) --paired, LAN only, TCP 6768--> orca serve --grant-desktop-control (Windows, user session) --UI Automation--> PA Manager
```

- Gives the native Windows accessibility tree of PA Manager (whatever UIA exposes; a Delphi/VCL or custom-drawn UI may expose little — §5 measures this), real screenshots, and a shell for copying test data.
- Runs **in the foreground of the logged-in user's desktop**, not as a service, and stops with Ctrl+C.
- Authenticated by Orca's pairing code (one-time offer); scoped to desktop control only when `--grant-desktop-control` is given.

**Plan:** prove Path A first (zero install). Adopt Path B only with explicit owner approval, if PA Manager research needs reliable control.

## 3. Installation and configuration steps

### Path A (owner, on the Mac — about 2 minutes)

1. Run `orca computer permissions --id accessibility` and enable **Orca Computer Use** in System Settings > Privacy & Security > Accessibility.
2. Run `orca computer permissions --id screenshots` and enable it under **Screen & System Audio Recording**.
3. In Windows App, set the PC's display to a **fixed resolution (e.g. 1920×1080), "Optimize for Retina" off, Windows scaling 100%**, and use a normal (not full-screen) window. This keeps screenshot pixels equal to action coordinates.
4. Keep the RDP window **open and not minimised** while ORCA works.

### Path B (owner, on Windows — each step needs explicit approval first)

1. Install Orca for Windows from its official source, as the normal user, not as administrator. *(installation)*
2. Confirm the network is **Private**: `Get-NetConnectionProfile`. Do not change a Public network to Private on untrusted networks.
3. Add one inbound rule, Private profile only, limited to the Mac's address: *(firewall change)*
   ```powershell
   New-NetFirewallRule -DisplayName "Orca serve (LAN, Mac only)" -Direction Inbound -Protocol TCP `
     -LocalPort 6768 -RemoteAddress <MAC_LAN_IP> -Profile Private -Action Allow
   ```
   If Windows shows its own "allow on networks" prompt, tick **Private only**.
4. In a normal PowerShell window inside the logged-in session, start the runtime for a research session only:
   ```powershell
   orca serve --port 6768 --pairing-address <WINDOWS_LAN_IP> --grant-desktop-control
   ```
   *(remote execution setup)*
5. Give the printed `orca://pair?…` code to ORCA **in the chat only**. On the Mac: `orca environment add --name pa-manager-win --pairing-code '<code>'`. The code is never committed or logged.
6. Ending a session: Ctrl+C in that window. To remove: `orca environment rm --environment pa-manager-win` on the Mac, then `Remove-NetFirewallRule -DisplayName "Orca serve (LAN, Mac only)"` on Windows.

## 4. Security restrictions

1. **LAN only.** No router port forwarding, UPnP, public tunnel or relay for 3389 or 6768. If remote access from outside is ever needed, use a VPN such as Tailscale, never an exposed port.
2. **No passwords or licence data** are requested, typed, stored or transmitted by ORCA/REA. The owner logs in to Windows and to PA Manager by hand. If a login or licence dialog appears, ORCA stops and hands over.
3. **No unattended service.** `orca serve` runs only in the owner's visible session, only during research.
4. **Read-only first.** The first tests only open applications, read UI state and take screenshots.
5. **Copies only.** All file work uses copies in a dedicated folder, e.g. `C:\PA-Research\work\`, copied from originals that ORCA never opens for writing. PA Manager "save", "convert" or "clean up" runs only against copies.
6. **Destructive actions need per-action approval:** delete, overwrite, cleanup, merge into an existing set, registry, firewall and installs.
7. **Pairing code = credential:** chat only, never in Git, docs or logs. It is revoked by stopping `orca serve` and running `orca environment rm`.
8. **Licence terms:** observe PA Manager's behaviour only. No decompiling, no copying of its binaries or internal files, no bypassing its licence checks (see the parity document).

## 5. Minimal proof-of-control test

Pass means **every** step succeeds and its evidence is saved under `docs/evidence/windows-agent/` (screenshots with personal data cropped out).

| # | Step | Path A command (Mac) | Pass criterion |
|---|---|---|---|
| 1 | See the RDP window | `orca computer list-windows --app com.microsoft.rdc.macos --json` | At least one window with the PC's title |
| 2 | Read state | `orca computer get-app-state --app com.microsoft.rdc.macos --json` | A screenshot saved; ORCA describes the visible Windows desktop correctly and the owner confirms |
| 3 | Open an app | Click Start, type `notepad`, press Return | Notepad visible in the next screenshot |
| 4 | Read-only PA Manager | Owner opens PA Manager by hand (licence state untouched); ORCA takes a screenshot | Main window identified; ORCA lists its visible menus and panels |
| 5 | Close Notepad without saving | Alt+F4, choose "Don't save" | Notepad gone |

Path B adds: `orca computer list-apps --environment pa-manager-win --json` lists PA Manager, and `get-app-state` on it returns an accessibility tree. Record how many PA Manager controls UIA exposes; that decides between Path A and Path B for the research.

### 5.1 Result, 2026-11-06 (Path A)

| # | Result | Evidence |
|---|---|---|
| 1 | **PASS** | 2 windows; the session window is titled with the PC address, 1008×655 pt, not minimised |
| 2 | **PASS** | ScreenCaptureKit screenshot; Windows 11 desktop described (Edge, PA Manager, taskbar, clock) |
| 3 | **PASS (foreground only)** | Background clicks were delivered by macOS but **ignored by Windows App**. With `--restore-window`: the first click only focused the window, the second opened Start, a click on the pinned Notepad tile opened Notepad |
| 4 | **PASS** | PA Manager **v5.2** was already open; its home screen read from the screenshot: Language (English [British]), New KPM Window, KST Manager (PA5X), SET Manager (PA4X…PA1X), Sample Manager (.KST/.SET), File Viewer (.STY/.PRF/.PAD…). Nothing in PA Manager was clicked |
| 5 | **PASS** | Notepad's X closed it with no dialog; gone from the taskbar |

The RDP session window exposes only its frame to macOS Accessibility (6 elements); everything inside is pixels, as expected. The screenshot scale varies between captures (2.0 and 1.27), so coordinates must come from the latest capture.

Screenshots are **not** committed: they showed personal chat titles and, at step 3, **Notepad restored earlier tabs that contain plaintext credentials** (a `.env` file). The screenshot holding them was deleted. Lessons for every session:

- Opening any app may restore previous content. Before a session, the owner closes or moves away anything sensitive (Notepad tabs, browser tabs, `.env` files).
- Background input does not work on the RDP window; every action needs `--restore-window`, which takes over the Mac's screen and pointer. The owner keeps hands off during a run.
- After the last action the Mac pointer stays where it clicked inside the session (step 5 left it over Edge's close button); move it to a neutral spot before handing back.

### 5.2 Re-run attempt, 2026-11-06 (later)

**FAILED, not reproduced.** `get-app-state` returned `permission_denied` ("no accessibility window") for Windows App **and** for Finder, although permissions read as granted.

Cause: **the Mac's login session was locked** (`ioreg`: `CGSSessionScreenIsLocked = true`, `IOConsoleLocked = true`). Path A requires an unlocked, awake Mac as well as a connected, visible RDP window. This is the same reason it is supervised-only.

Next: one retry after the owner unlocks the Mac. Meanwhile, the owner can run the read-only UIA dump (`tools/windows/pa_manager_uia_dump.ps1`) inside the session.

### 5.3 Supervised retry after unlock, 2026-11-06: PASS

Owner unlocked the Mac and authorized foreground. Lock flags cleared; Windows App window listed (1008×655, not minimised). One `get-app-state`: accessibility tree + ScreenCaptureKit screenshot OK. Background coordinate clicks then opened KST Manager and Sample Manager (one click needed a second attempt). Observations: `PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md` §3a. Root cause of §5.2 confirmed: the Mac console lock.

### 5.4 Path C — owner-run UIA dump, local to Windows (no Mac unlock needed)

Path A needs an unlocked Mac; Path B needs an install, a firewall rule and a listening service. Path C needs none of those: the owner runs the read-only script `tools/windows/pa_manager_uia_dump.ps1` **at the Windows PC** (or inside RDP), and the Mac is not involved.

**Static review (2026-11-06, on macOS; no PowerShell here, so UNTESTED on Windows):**

- Reads only: `AutomationElement` properties plus `TreeWalker` navigation. No patterns (Invoke/Toggle/SetValue), no `SetFocus`, no input, no network. One file write (`-OutFile`).
- Uses the UIA assemblies shipped with Windows PowerShell 5.1; nothing to install.
- `-ExecutionPolicy Bypass` on the command line applies to that one `powershell.exe` process only; no system policy changes.
- Fixed before the first run:
  - off-screen elements have an empty rectangle (`X = Infinity`), and `[int]` on it would have aborted the whole dump: now written as `offscreen`;
  - an element that disappears mid-walk (a closing dialog) threw and aborted the run: now logged as `<unavailable: …>` and the walk continues;
  - KST Explorer / Sample Manager may be separate top-level windows whose titles lack "PA Manager": every top-level window of the matched process is now dumped.
- No window matching `-TitlePattern` → stops with an error that lists the open window titles (nothing written).
- Password fields are skipped. Names can include KST file names shown in the UI: review before sharing.

**Minimal proof test (owner, about 3 minutes; read-only):**

1. On the Windows PC, open PA Manager and leave it on its home screen. Open nothing else in it.
2. Copy `pa_manager_uia_dump.ps1` to `C:\PA-Research\work\` (the research folder; the original KST is not touched).
3. Start **Windows PowerShell** from the Start menu, normal user — **not** "Run as administrator".
4. Run exactly:
   ```
   cd C:\PA-Research\work
   powershell -ExecutionPolicy Bypass -File .\pa_manager_uia_dump.ps1 -OutFile C:\PA-Research\work\uia-home.txt
   ```
5. Report: the `Wrote N elements` line, the `windows=` value from the first line of `uia-home.txt`, and any error text verbatim. Do not send the file itself until it has been reviewed for personal file names.
6. If a dialog, UAC prompt or SmartScreen warning appears: cancel it and report it; do not approve anything.

**How to read the result:**

| Result | Meaning | Next |
|---|---|---|
| Hundreds of elements with button / tab / list names | PA Manager exposes UIA: local read-only research is viable | Repeat with KST Manager open on the **copy** (P3) |
| Only a few elements (a window and a pane) | Custom-drawn UI: UIA sees nothing inside | Screenshots only; Path B would not help either |
| Error before writing | Report the text; do not retry with other options | Fix the script on the Mac, re-test |

Path C reads state only: it never clicks. Navigation (opening KST Manager on the copy) stays a manual step by the owner, so no remote control is involved.

### 5.5 Path C transfer attempt, 2026-11-07: BLOCKED (Mac screen lock)

- Transfer file staged on the Mac from commit `b31e1dd`; SHA-256 `76d3017e80388ac4cdf77f4b3567a1573ece6e7859940015ce90c8759313035f`.
- Windows App: folder redirection enabled for **one** folder (the staging folder), **read-only**. Saved in the PC's Windows App configuration.
- No RDP session was live; reconnecting raised (1) a credentials prompt (the first one timed out, error 0x1f07; the owner supplied the credentials for the second), then (2) a self-signed RDP certificate warning. The owner checked the fingerprint and allowed "accept once" (not "always trust").
- Before Continue could be clicked, the Mac screen locked (`CGSSessionScreenIsLocked=Yes`) and every accessibility read returned `permission_denied`. Foreground operations stopped, per the project rule. Nothing was transferred or run on Windows.
- Still configured: the read-only folder redirection. Remove it after the transfer (or now if the research is abandoned).
- Lesson: Path A still needs the Mac unlocked for the whole sequence; the RDP prompts also time out, so connect + accept must happen in one supervised window.

### 5.6 Path C run, 2026-11-07: PASS (transfer, hash, UIA dump)

- Owner authenticated RDP manually. Transfer via read-only folder redirection; Windows hash matched the commit. Redirection disabled afterwards.
- **Input through Windows App:** synthetic keystrokes arrive as repeated "a" and Cmd+V loses its modifier. What works: put text on the Mac clipboard, switch focus away from and back to Windows App (it only syncs the clipboard on activation), then right-click in Windows Terminal to paste, and Return. Return/Backspace keys work.
- Switching focus moves the session window off the current Space; it is brought back with the device's **Open** menu item (no new login while connected).
- Saving the PC configuration in Windows App ended the live session; a reconnect needs the owner's login.
- UIA results: audit §3c.

### 5.7 Autonomous RDP reconnection, 2026-11-07: VERIFIED

- Method: Windows App saved credentials (owner-entered, macOS Keychain) + RDP certificate pinned ("Always trust") after its SHA-256 matched the owner-approved value. ORCA's connect procedure stops on any credential prompt and on any certificate whose fingerprint differs; it never types credentials.
- Two disconnect/reconnect cycles passed (32 s, 4 s). The session window leaves the current Space when Windows App loses focus; it is brought back with the device's Open item.
- Still required: an unlocked Mac (any lock blocks all GUI control); no background agent runs between owner messages.
- Option B (Orca runtime on Windows) was not needed and not installed.

### 5.8 Operating procedure (as used 2026-11-07)

1. Bring the session window back (device **Open**) after any focus change; confirm it is ≥ 1000 px wide.
2. Before a click into PA Manager or a paste into PowerShell: bring that window forward from the Windows taskbar and check a fresh screenshot.
3. Commands: Mac clipboard → Finder/Windows App focus toggle → right-click paste into Windows Terminal → screenshot check → Return.
4. Context menus are read and dismissed with Escape; report windows are closed with **Close**, never **Save Report**.
5. After each research block: manifest re-check of the copy (path | size | mtime) against the pre-load manifest.

## 6. RDP, locking and display limitations

- **Disconnected RDP session = no GUI.** When the RDP window is closed, the Windows session is disconnected: nothing is rendered, screenshots come back black, and synthetic input fails, for both paths. Keep RDP connected during a session. For Path B without RDP, an administrator can move the session to the console (`tscon <id> /dest:console`), but on a PC with no monitor that needs a display or a dummy HDMI plug. That is an optional later step, and it needs approval.
- **Mac lock screen:** a locked **Mac** blocks Path A entirely (all AX and capture), see §5.2.
- **Lock screen:** a locked session (Win+L, timeout) blocks automation. Disable the lock timeout **only for research sessions**, at the owner's discretion.
- **Minimised RDP window:** the client may stop sending frames. Keep it visible, which also lets the owner watch every action.
- **Display scaling:** Retina scaling in Windows App and Windows DPI scaling both shift coordinates. Use fixed resolution and 100% scaling, and always take coordinates from the latest screenshot (`scale` field).
- **One interactive session:** Windows Pro allows one user session at a time. Logging in at the physical console disconnects the RDP session, and the other way round.
- **UAC prompts** appear on the secure desktop and cannot be automated. That is intended: elevation stays with the owner.

## 7. Blockers that need the owner

1. ~~macOS permissions~~: granted 2026-11-06; Path A verified (§5.1).
2. Keep the Windows App session connected and visible during the test.
3. For Path B only: approve installing Orca on Windows, the firewall rule and running `orca serve --grant-desktop-control`, then send the pairing code.
4. Prepare a copy folder of test data on Windows (a Pa5X KST/SET copy, never the original).
