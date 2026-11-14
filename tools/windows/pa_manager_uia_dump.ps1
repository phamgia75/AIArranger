<#
.SYNOPSIS
  READ-ONLY dump of the KORG PA Manager UI Automation tree (Workstream B).

.DESCRIPTION
  The owner runs this in a normal (non-elevated) PowerShell window inside the
  logged-in Windows session, with PA Manager already open on the screen to inspect.
  It uses the UI Automation client that ships with Windows (.NET), so nothing is
  installed. There is no network access, no service, and no input: no clicks,
  keys, Invoke/Toggle/SetValue patterns or focus changes. It only reads element
  properties and writes one text file.

  Output: one line per element: depth, control type, name, AutomationId, class,
  enabled, and on-screen rectangle. Element names can include file names shown
  in the UI, so review the file before sharing it. It contains no passwords:
  password fields are skipped.

  Every other top-level window of the same process is dumped too (KST Explorer,
  Sample Manager...), since those titles do not contain "PA Manager".

  Run it with Windows PowerShell 5.1 (powershell.exe), which ships the UI
  Automation assemblies. -ExecutionPolicy Bypass applies to that one process
  only and changes no system setting.

  STATUS: first run 2026-11-07 on the target PC (Windows PowerShell 5.1): worked,
  PA Manager v5.2 home screen = 14 elements. That run used the old unanchored title
  pattern, which also dumped browser windows. The KST-view run used the pattern that
  is now the default (it also matches the dash-less KST-view title), via -TitlePattern.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\pa_manager_uia_dump.ps1 -OutFile C:\PA-Research\work\uia-home.txt
#>
param(
    # Anchored: an unanchored 'PA ?Manager' also matched browser tabs titled "KORG PA Manager | ..."
    # (seen 2026-11-07), and the same-process rule below then pulled in every window of that browser.
    # Titles seen: "KORG PA Manager  -  v5.2" (home) and "KORG PA Manager v5.2" (KST view, no dash).
    [string]$TitlePattern = '^KORG PA ?Manager( +-)?( +v[0-9.]+)?$',
    [string]$OutFile = "$env:USERPROFILE\Desktop\pa_manager_uia.txt",
    [int]$MaxDepth = 12,
    [int]$MaxElements = 5000
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$AE = [System.Windows.Automation.AutomationElement]

$root = $AE::RootElement
$cond = [System.Windows.Automation.Condition]::TrueCondition
$tops = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
$matched = @($tops | Where-Object { $_.Current.Name -match $TitlePattern })
if ($matched.Count -eq 0) {
    Write-Error "No top-level window title matches '$TitlePattern'. Open titles: $(( $tops | ForEach-Object { $_.Current.Name } ) -join ' | ')"
}
# Same process, any title: PA Manager opens its managers as separate windows.
$pids = @($matched | ForEach-Object { $_.Current.ProcessId } | Sort-Object -Unique)
$targets = @($tops | Where-Object { $pids -contains $_.Current.ProcessId })

$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$lines = New-Object System.Collections.Generic.List[string]
$script:count = 0

function Dump($el, [int]$depth) {
    if ($script:count -ge $MaxElements -or $depth -gt $MaxDepth) { return }
    try {
        $c = $el.Current
        $script:count++
        $name = if ($c.IsPassword) { '<password field skipped>' } else { ($c.Name -replace '[\r\n\t]', ' ') }
        $r = $c.BoundingRectangle
        # Off-screen elements report an empty rect (X = Infinity); [int] would throw.
        $rect = if ($r.IsEmpty) { 'offscreen' } else { '{0},{1},{2},{3}' -f [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height }
        $lines.Add(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6}" -f `
            $depth, $c.ControlType.ProgrammaticName, $name, $c.AutomationId, $c.ClassName, $c.IsEnabled, $rect))
        $child = $walker.GetFirstChild($el)
    } catch {
        # The element vanished (a dialog closed) or refused a property: note it, keep walking.
        $lines.Add(("{0}`t<unavailable: {1}>" -f $depth, $_.Exception.GetType().Name))
        return
    }
    while ($child -ne $null) {
        Dump $child ($depth + 1)
        try { $child = $walker.GetNextSibling($child) } catch { $child = $null }
    }
}

$header = "# PA Manager UIA dump (read-only) $(Get-Date -Format s) windows=$($targets.Count) maxDepth=$MaxDepth"
$lines.Add($header)
$lines.Add("# depth`tcontrolType`tname`tautomationId`tclass`tenabled`trect")
foreach ($t in $targets) {
    $lines.Add("# window: $($t.Current.Name) pid=$($t.Current.ProcessId)")
    Dump $t 0
}
$lines.Add("# elements=$script:count")
[System.IO.File]::WriteAllLines($OutFile, $lines)
Write-Host "Wrote $script:count elements to $OutFile"
