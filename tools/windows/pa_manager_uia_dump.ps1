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

  STATUS: UNTESTED on the target PC (written on macOS, no PowerShell available).
  The first run is the test.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\pa_manager_uia_dump.ps1 -OutFile C:\PA-Research\work\uia-home.txt
#>
param(
    [string]$TitlePattern = 'PA ?Manager',   # regex on top-level window titles
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
$targets = @($tops | Where-Object { $_.Current.Name -match $TitlePattern })
if ($targets.Count -eq 0) {
    Write-Error "No top-level window title matches '$TitlePattern'. Open titles: $(( $tops | ForEach-Object { $_.Current.Name } ) -join ' | ')"
}

$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$lines = New-Object System.Collections.Generic.List[string]
$script:count = 0

function Dump($el, [int]$depth) {
    if ($script:count -ge $MaxElements -or $depth -gt $MaxDepth) { return }
    $c = $el.Current
    $script:count++
    $name = if ($c.IsPassword) { '<password field skipped>' } else { ($c.Name -replace '[\r\n\t]', ' ') }
    $r = $c.BoundingRectangle
    $lines.Add(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6},{7},{8},{9}" -f `
        $depth, $c.ControlType.ProgrammaticName, $name, $c.AutomationId, $c.ClassName, $c.IsEnabled,
        [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height))
    $child = $walker.GetFirstChild($el)
    while ($child -ne $null) {
        Dump $child ($depth + 1)
        $child = $walker.GetNextSibling($child)
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
