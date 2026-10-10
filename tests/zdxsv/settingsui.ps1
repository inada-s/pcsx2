# settingsui.ps1 [-Exe pcsx2-qtx64.exe] [-Page zdxsv] [-Match regex] [-Shot out.png]
# Start a portable PCSX2 (portable.ini or portable.txt next to the exe, no game), open Settings -> page -Page,
# print each control whose AutomationId or name matches -Match (default: the page name) with its enabled
# state and value, save a PrintWindow capture of the settings window to -Shot, close PCSX2.
# Env overrides (ZDXSV_*) set by the caller apply, e.g. to see env-overridden settings greyed out.
# - inis\PCSX2.ini [UI] gets SetupWizardIncomplete = false, SettingsVersion = 1 (else a "Settings failed to
#   load" reset dialog) and Language = en-US (else menus in the system language) when missing.
# - Qt menus do not answer UIA Invoke: Settings opens by keyboard (Alt+N, Enter); the page item is clicked
#   with the mouse at its UIA rectangle.
# Exit 1 with the visible element names when a step finds nothing.
param([string]$Exe = (Join-Path $PSScriptRoot '..\..\bin\pcsx2-qtx64.exe'), [string]$Page = 'zdxsv',
      [string]$Match = '', [string]$Shot = '')
$ErrorActionPreference = 'Stop'
if (-not $Match) { $Match = $Page }
$dir = Split-Path -Parent (Resolve-Path $Exe)
if (-not ((Test-Path "$dir\portable.ini") -or (Test-Path "$dir\portable.txt"))) {
  "FAIL: $dir has no portable.ini/portable.txt: PCSX2 would use the user's Documents settings"; exit 1
}
$ini = "$dir\inis\PCSX2.ini"
$lines = if (Test-Path $ini) { @(Get-Content $ini) } else { New-Item -ItemType Directory -Force "$dir\inis" | Out-Null; @() }
$want = [ordered]@{ SetupWizardIncomplete = 'false'; SettingsVersion = '1'; Language = 'en-US'; StartFullscreen = 'false' }
$ui = [Array]::IndexOf($lines, '[UI]')
if ($ui -lt 0) { $lines = @('[UI]') + $lines; $ui = 0 }
$end = $ui + 1
while ($end -lt $lines.Count -and $lines[$end] -notmatch '^\[') { $end++ }
$have = $lines[($ui + 1)..($end - 1)] | Where-Object { $_ -match '=' } | ForEach-Object { ($_ -split '=')[0].Trim() }
$add = foreach ($k in $want.Keys) { if ($have -notcontains $k) { "$k = $($want[$k])" } }
if ($add) {
  $lines = $lines[0..$ui] + @($add) + $(if ($ui + 1 -lt $lines.Count) { $lines[($ui + 1)..($lines.Count - 1)] } else { @() })
  Set-Content -Path $ini -Value $lines -Encoding UTF8
  "ini: added $($add -join ', ')"
}

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class SUI {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
[void][SUI]::SetProcessDPIAware()
$A = [System.Windows.Automation.AutomationElement]
$all = [System.Windows.Automation.Condition]::TrueCondition
$p = Start-Process -FilePath $Exe -WorkingDirectory $dir -PassThru
try {
  $root = $A::RootElement
  $pidCond = New-Object System.Windows.Automation.PropertyCondition($A::ProcessIdProperty, $p.Id)
  function Names($scope) {
    (@(foreach ($e in $scope.FindAll('Descendants', $all)) { "$($e.Current.ControlType.ProgrammaticName -replace 'ControlType\.', ''):$($e.Current.Name)" }) |
      Select-Object -First 60) -join ' | '
  }
  function Wins { @($root.FindAll('Children', $pidCond)) }
  $main = $null
  for ($i = 0; $i -lt 60 -and -not $main; $i++) { Start-Sleep -Milliseconds 500; $main = (Wins) | Select-Object -First 1 }
  if (-not $main) { "FAIL: no PCSX2 window in 30 s"; exit 1 }
  Start-Sleep 2
  if ((Wins).Count -gt 1 -or $main.Current.Name -notmatch 'PCSX2') {
    "FAIL: a dialog instead of the main window: $(foreach ($w in Wins) { "[$($w.Current.Name)] $(Names $w)" })"; exit 1
  }
  [void][SUI]::SetForegroundWindow([IntPtr]$main.Current.NativeWindowHandle); $main.SetFocus()
  Start-Sleep -Milliseconds 500
  [System.Windows.Forms.SendKeys]::SendWait('%n'); Start-Sleep 1
  [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
  $win = $null
  for ($i = 0; $i -lt 30 -and -not $win; $i++) {
    Start-Sleep -Milliseconds 300
    foreach ($w in Wins) { if ($w.Current.Name -match 'Settings') { $win = $w } }
  }
  if (-not $win) { "FAIL: no settings window; windows: $(foreach ($w in Wins) { "[$($w.Current.Name)]" })"; exit 1 }
  "settings window: $($win.Current.Name)"
  $c = New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, $Page)
  $item = $null
  for ($i = 0; $i -lt 20 -and -not $item; $i++) { $item = $win.FindFirst('Descendants', $c); if (-not $item) { Start-Sleep -Milliseconds 300 } }
  if (-not $item) { "FAIL: no page item '$Page'; have: $(Names $win)"; exit 1 }
  [void][SUI]::SetForegroundWindow([IntPtr]$win.Current.NativeWindowHandle)
  $r = $item.Current.BoundingRectangle
  [void][SUI]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
  [SUI]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [SUI]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep 2
  $n = 0
  foreach ($e in $win.FindAll('Descendants', $all)) {
    $id = $e.Current.AutomationId; $nm = $e.Current.Name
    if ($e -eq $item -or ($id -notmatch $Match -and $nm -notmatch $Match)) { continue }
    $state = ''; $pat = $null
    if ($e.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pat)) { $state = "toggle=$($pat.Current.ToggleState)" }
    elseif ($e.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$pat)) { $state = "value=$($pat.Current.Value)" }
    "$($id -replace '.*\.', '') '$nm' enabled=$($e.Current.IsEnabled) $state"
    $n++
  }
  "controls matching '$Match': $n"
  if ($Shot) {
    $h = [IntPtr]$win.Current.NativeWindowHandle
    $wr = New-Object SUI+RECT; [void][SUI]::GetWindowRect($h, [ref]$wr)
    $bmp = New-Object System.Drawing.Bitmap ($wr.R - $wr.L), ($wr.B - $wr.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc(); [void][SUI]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png); "shot $Shot $($bmp.Width)x$($bmp.Height)"
  }
} finally {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
