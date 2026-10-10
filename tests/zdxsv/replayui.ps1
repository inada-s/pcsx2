# replayui.ps1 -DataPath <dir> [-Exe pcsx2-qtx64.exe] [-Tab Local|Remote|Live] [-Row regex] [-Pov 2P] [-Wait 90] [-Shot out.png]
# Replay dialog test: starts PCSX2 with -datapath <dir> (its game list must hold the Z game: ini [GameList] Paths),
# opens Tools -> zdxsv Replays, shows tab -Tab (Remote: Search, Live: Refresh), prints the rows, clicks the first
# row matching -Row, picks point of view -Pov (combo text) and invokes Play. After -Wait s: -Shot (PrintWindow of
# the display window), PCSX2 closed. The replay's own log lines are in <dir>\PCSX2\logs\emulog.txt (or the
# data dir's logs\). No -Row: lists only.
# - The lobby API URL (Remote, Live) comes from the ini ([DEV9/Eth] ZdxsvLobbyApiUrl).
# - Qt menus do not answer UIA Invoke: menu and rows are clicked with the mouse at their UIA rectangles.
# Exit 1 with the visible element names when a step finds nothing.
param([Parameter(Mandatory)][string]$DataPath, [string]$Exe = '',
      [string]$Tab = 'Local', [string]$Row = '', [string]$Pov = '', [int]$Wait = 90, [string]$Shot = '')
$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot '..\..\bin\pcsx2-qtx64.exe' }
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class RUI {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
[void][RUI]::SetProcessDPIAware()
$A = [System.Windows.Automation.AutomationElement]
$all = [System.Windows.Automation.Condition]::TrueCondition
$root = $A::RootElement
function Names($scope) {
  (@(foreach ($e in $scope.FindAll('Descendants', $all)) { if (-not $e.Current.IsOffscreen) { "$($e.Current.ControlType.ProgrammaticName -replace 'ControlType\.', ''):$($e.Current.Name)" } }) |
    Select-Object -First 80) -join ' | '
}
function Click($e) {
  $r = $e.Current.BoundingRectangle
  [void][RUI]::SetCursorPos([int]($r.X + [Math]::Min($r.Width / 2, 40)), [int]($r.Y + $r.Height / 2))
  Start-Sleep -Milliseconds 150
  [RUI]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [RUI]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 400
}
# first visible descendant of $scope with that control type whose name matches $re, polled up to $secs
function Find($scope, $type, $re, $secs = 10) {
  for ($t = 0; $t -lt $secs * 4; $t++) {
    foreach ($e in $scope.FindAll('Descendants', (New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, $type)))) {
      if (-not $e.Current.IsOffscreen -and $e.Current.Name -match $re) { return $e }
    }
    Start-Sleep -Milliseconds 250
  }
  $null
}
$CT = [System.Windows.Automation.ControlType]
$p = Start-Process -FilePath $Exe -ArgumentList @('-datapath', $DataPath) -PassThru
try {
  $pidCond = New-Object System.Windows.Automation.PropertyCondition($A::ProcessIdProperty, $p.Id)
  function Wins { @($root.FindAll('Children', $pidCond)) }
  $main = $null
  for ($i = 0; $i -lt 60 -and -not $main; $i++) { Start-Sleep -Milliseconds 500; $main = (Wins) | Where-Object { $_.Current.Name -match 'PCSX2' } | Select-Object -First 1 }
  if (-not $main) { "FAIL: no PCSX2 window in 30 s: $(foreach ($w in Wins) { "[$($w.Current.Name)]" })"; exit 1 }
  Start-Sleep 2
  [void][RUI]::SetForegroundWindow([IntPtr]$main.Current.NativeWindowHandle)
  $tools = Find $main $CT::MenuItem '^Tools$'
  if (-not $tools) { "FAIL: no Tools menu: $(Names $main)"; exit 1 }
  Click $tools
  $item = $null
  for ($i = 0; $i -lt 20 -and -not $item; $i++) { foreach ($w in @($main) + (Wins)) { if (-not $item) { $item = Find $w $CT::MenuItem 'zdxsv Replays' 0.25 } } }
  if (-not $item) { "FAIL: no 'zdxsv Replays' menu item: $(foreach ($w in Wins) { Names $w })"; exit 1 }
  Click $item
  $dlg = $null
  for ($i = 0; $i -lt 40 -and -not $dlg; $i++) {
    Start-Sleep -Milliseconds 250
    foreach ($w in @($main.FindAll('Descendants', (New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Window)))) + (Wins)) { if ($w.Current.Name -eq 'zdxsv Replays') { $dlg = $w } }
  }
  if (-not $dlg) { "FAIL: no zdxsv Replays dialog: $(foreach ($w in Wins) { "[$($w.Current.Name)]" })"; exit 1 }
  "dialog open"
  $tabItem = Find $dlg $CT::TabItem "^$Tab$"
  if (-not $tabItem) { "FAIL: no tab $Tab : $(Names $dlg)"; exit 1 }
  Click $tabItem
  $btn = switch ($Tab) { 'Remote' { 'Search' } 'Live' { 'Refresh' } default { '' } }
  if ($btn) {
    $b = Find $dlg $CT::Button "^$btn$"
    if (-not $b) { "FAIL: no $btn button: $(Names $dlg)"; exit 1 }
    $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  }
  Start-Sleep 3
  $status = foreach ($e in $dlg.FindAll('Descendants', (New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Text)))) {
    if (-not $e.Current.IsOffscreen) { $e.Current.Name } }
  "labels: $($status -join ' | ')"
  # QTreeWidget rows: one element per cell; a row's cells share the Y of their rectangle
  $cells = @(foreach ($e in $dlg.FindAll('Descendants', $all)) {
    $t = $e.Current.ControlType
    if (-not $e.Current.IsOffscreen -and ($t -eq $CT::DataItem -or $t -eq $CT::TreeItem -or $t -eq $CT::ListItem) -and $e.Current.Name) { $e } })
  $rows = $cells | Group-Object { [int]$_.Current.BoundingRectangle.Y } | Sort-Object { [int]$_.Name }
  foreach ($r in $rows) { "row: $(($r.Group | Sort-Object { $_.Current.BoundingRectangle.X } | ForEach-Object { $_.Current.Name }) -join ' | ')" }
  if (-not $Row) { exit 0 }
  $hit = $rows | Where-Object { (($_.Group | ForEach-Object { $_.Current.Name }) -join ' | ') -match $Row } | Select-Object -First 1
  if (-not $hit) { "FAIL: no row matches $Row"; exit 1 }
  Click ($hit.Group | Sort-Object { $_.Current.BoundingRectangle.X } | Select-Object -First 1)
  if ($Pov) {
    $combo = Find $dlg $CT::ComboBox '.*'
    Click $combo
    $opt = $null
    foreach ($w in @($dlg) + (Wins)) { if (-not $opt) { $opt = Find $w $CT::ListItem "^$Pov$" 3 } }
    if (-not $opt) { "FAIL: no point of view $Pov"; exit 1 }
    Click $opt
  }
  $play = Find $dlg $CT::Button '^Play$'
  if (-not $play -or -not $play.Current.IsEnabled) { "FAIL: Play missing or disabled"; exit 1 }
  $play.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  "play invoked"
  Start-Sleep $Wait
  if ($Shot) {
    $h = (Get-Process -Id $p.Id).MainWindowHandle
    foreach ($w in Wins) { if ($w.Current.Name -notmatch '^PCSX2' -and $w.Current.Name) { $h = [IntPtr]$w.Current.NativeWindowHandle } }
    $r = New-Object RUI+RECT; [void][RUI]::GetWindowRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R - $r.L)), ([Math]::Max(1, $r.B - $r.T))
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][RUI]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $bmp.Save($Shot); $g.Dispose(); $bmp.Dispose()
    "shot $Shot"
  }
  "windows: $(foreach ($w in Wins) { "[$($w.Current.Name)]" })"
} finally {
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
