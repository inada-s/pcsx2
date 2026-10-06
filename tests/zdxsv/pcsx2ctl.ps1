# Drive a running pcsx2 window: key sequence, then optional screenshot.
# -Seq "Return,w2000,Down*3,X"  : key names (System.Windows.Forms.Keys), wN = sleep N ms, K*n = repeat, Shift+K / Control+K = with the modifier held.
# -Shot out.png : PrintWindow capture of the pcsx2 main window (works when occluded).
# -ProcId : pcsx2 pid (default: first pcsx2-qtx64). Keys go via SetForegroundWindow + keybd_event.
# Mouse tokens in -Seq (pixels of the render surface = the child window of the bar's layout size):
#   mv:X:Y move; ck:X:Y move + left click; bar:show moves over the bottom (shows the replay control bar);
#   bar:NAME[:F] clicks a bar element (pause back forward prevround nextround timeline pov) at fraction F of its width (default
#   0.5), from the last `replay bar: layout` line in the instance's emulog. shot:out.png = winshot.ps1 (overlays).
# -Check -Ini PCSX2.ini : only the binding check of -Seq against that ini, no process, nothing sent (rc 2 = unbound;
#   rplay.sh runs it on every KEYS step before launch).
param([string]$Seq = "", [string]$Shot = "", [int]$ProcId = 0, [int]$HoldMs = 120, [int]$GapMs = 350,
      [switch]$Check, [string]$Ini = "")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr extra);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
if (!$Check) {
  $p = if ($ProcId) { Get-Process -Id $ProcId } else { Get-Process pcsx2-qtx64 -ea Stop | Select-Object -First 1 }
  $h = $p.MainWindowHandle
}
# A key with no Keyboard/<name> binding in the instance ini does nothing (e.g. Space with TogglePause unbound:
# a run lost): refuse the whole call before any key is sent.
function Get-Unbound([string[]]$keys) {
  if ($Check) {
    $ini = $Ini
    if (!$ini -or !(Test-Path $ini)) { return @($keys | ForEach-Object { "$_ (-Check: no ini '$Ini')" }) }
  } else {
    $cl = (Get-CimInstance Win32_Process -Filter "ProcessId=$($p.Id)").CommandLine
    if ($cl -notmatch '-datapath\s+("([^"]+)"|(\S+))') { Write-Output "keycheck: no -datapath in pid $($p.Id), unchecked" | Out-Host; return @() }
    $d = if ($Matches[2]) { $Matches[2] } else { $Matches[3] }
    $ini = @("$d\PCSX2\inis\PCSX2.ini", "$d\inis\PCSX2.ini") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$ini) { return @($keys | ForEach-Object { "$_ (no PCSX2.ini under $d)" }) }
  }
  $bound = Get-Content $ini -Encoding UTF8 | Where-Object { $_ -match '^\w+ = ' }
  $map = @{ Back = 'Backspace'; Enter = 'Return'; Next = 'PageDown'; Prior = 'PageUp' }
  foreach ($kk in $keys | Select-Object -Unique) {
    # Shift+PageDown: needs the chord binding `Keyboard/Shift & Keyboard/PageDown`
    $parts = $kk.Split('+'); $k = $parts[-1]
    $name = if ($map[$k]) { $map[$k] } elseif ($k -match '^D(\d)$') { $Matches[1] } else { $k }
    $want = (@($parts | Select-Object -SkipLast 1 | ForEach-Object { "Keyboard/$_" }) + "Keyboard/$name") -join ' & '
    if (!($bound | Where-Object { $_ -match "(^| )$([regex]::Escape($want))( |$|&)" })) { "$kk ($want) in $ini" }
  }
}
function Get-DataDir {
  $cl = (Get-CimInstance Win32_Process -Filter "ProcessId=$($p.Id)").CommandLine
  if ($cl -match '-datapath\s+("([^"]+)"|(\S+))') { if ($Matches[2]) { $Matches[2] } else { $Matches[3] } }
}
# last `replay bar: layout WxH y Y0-Y1 name X0-X1 ...` line -> @{ w; h; y0; y1; name = @(x0, x1) }
function Get-BarLayout {
  $log = "$(Get-DataDir)\PCSX2\logs\emulog.txt"
  $line = Select-String -Path $log -Pattern 'replay bar: layout (.*)' -ea 0 | Select-Object -Last 1
  if (!$line) { return $null }
  $f = $line.Matches[0].Groups[1].Value.Trim() -split '\s+'
  $wh = $f[0] -split 'x'; $yy = $f[2] -split '-'
  $b = @{ w = [int]$wh[0]; h = [int]$wh[1]; y0 = [int]$yy[0]; y1 = [int]$yy[1] }
  for ($i = 3; $i + 1 -lt $f.Count; $i += 2) { $xx = $f[$i + 1] -split '-'; $b[$f[$i]] = @([int]$xx[0], [int]$xx[1]) }
  $b
}
# the render surface: the child window whose client size is WxH (else the largest child)
function Get-Surface([int]$w, [int]$hh) {
  $script:kids = New-Object System.Collections.ArrayList
  [void][W]::EnumChildWindows($h, { param($c, $l) [void]$script:kids.Add($c); $true }, [IntPtr]::Zero)
  $best = $null; $area = -1
  foreach ($c in $script:kids) {
    $r = New-Object W+RECT; [void][W]::GetClientRect($c, [ref]$r)
    if ($w -and $r.R -eq $w -and $r.B -eq $hh) { return $c }
    if ($r.R * $r.B -gt $area) { $area = $r.R * $r.B; $best = $c }
  }
  $best
}
function Move-To([double]$x, [double]$y, [int]$w = 0, [int]$hh = 0) {
  $s = Get-Surface $w $hh
  $pt = New-Object W+POINT; $pt.X = [int]$x; $pt.Y = [int]$y
  [void][W]::ClientToScreen($s, [ref]$pt); [void][W]::SetCursorPos($pt.X, $pt.Y)
}
function Invoke-Mouse([string]$t) {
  $a = $t.Split(':')
  if ($a[0] -in 'mv', 'ck') {
    Move-To $a[1] $a[2]; Start-Sleep -Milliseconds 100
  } elseif ($a[1] -eq 'show') {
    $s = Get-Surface 0 0; $r = New-Object W+RECT; [void][W]::GetClientRect($s, [ref]$r)
    foreach ($dx in 0, 8, 16) { Move-To ($r.R / 2 + $dx) ($r.B * 0.9); Start-Sleep -Milliseconds 150 }
    "bar:show surface $($r.R)x$($r.B)"; return
  } else {
    $b = Get-BarLayout
    if (!$b -or !$b[$a[1]]) { "FAIL no bar layout / element $($a[1])"; exit 3 }
    $frac = if ($a.Count -gt 2) { [double]$a[2] } else { 0.5 }
    $x = $b[$a[1]][0] + ($b[$a[1]][1] - $b[$a[1]][0]) * $frac; $y = ($b.y0 + $b.y1) / 2
    Move-To ($x - 6) $y $b.w $b.h; Start-Sleep -Milliseconds 120
    Move-To $x $y $b.w $b.h; Start-Sleep -Milliseconds 150
    "$t -> x $([int]$x) y $([int]$y) of $($b.w)x$($b.h)"
  }
  if ($a[0] -ne 'mv') {
    [W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $HoldMs
    [W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $GapMs
  }
}
if ($Seq) {
  $names = foreach ($tok in $Seq.Split(',')) { $t = $tok.Trim(); if ($t -and $t -notmatch '^w\d+$|^(mv|ck|bar|shot):') { $t -replace '\*\d+$', '' } }
  $un = @(Get-Unbound $names)
  if ($un.Count) { $un | ForEach-Object { "FAIL key not bound: $_" }; exit 2 }
}
if ($Check) { "keycheck ok: $Seq"; exit 0 }
if ($Seq) {
  [void][W]::ShowWindow($h, 9); [void][W]::SetForegroundWindow($h); Start-Sleep -Milliseconds 150
  foreach ($tok in $Seq.Split(',')) {
    $t = $tok.Trim(); if (!$t) { continue }
    if ($t -match '^w(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
    if ($t -match '^shot:(.+)$') { & "$PSScriptRoot\winshot.ps1" -ProcId $p.Id -Shot $Matches[1]; continue }
    if ($t -match '^(mv|ck|bar):') { [void][W]::SetForegroundWindow($h); Invoke-Mouse $t; continue }
    $n = 1; if ($t -match '^(.+)\*(\d+)$') { $t = $Matches[1]; $n = [int]$Matches[2] }
    $mods = @(); if ($t.Contains('+')) { $mods = @($t.Split('+') | Select-Object -SkipLast 1 | ForEach-Object { [byte][System.Windows.Forms.Keys]"${_}Key" }); $t = $t.Split('+')[-1] }
    $vk = [byte][System.Windows.Forms.Keys]$t
    $ext = if ($t -in 'Up','Down','Left','Right','Insert','Delete','Home','End','PageUp','PageDown') { 1 } else { 0 }
    $sc = [byte][W]::MapVirtualKey($vk, 0)
    for ($i = 0; $i -lt $n; $i++) {
      [void][W]::SetForegroundWindow($h)
      foreach ($m in $mods) { [W]::keybd_event($m, [byte][W]::MapVirtualKey($m, 0), 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 30 }
      [W]::keybd_event($vk, $sc, $ext, [UIntPtr]::Zero); Start-Sleep -Milliseconds $HoldMs
      [W]::keybd_event($vk, $sc, $ext -bor 2, [UIntPtr]::Zero)
      foreach ($m in $mods) { [W]::keybd_event($m, [byte][W]::MapVirtualKey($m, 0), 2, [UIntPtr]::Zero) }
      Start-Sleep -Milliseconds $GapMs
    }
  }
}
if ($Shot) {
  # PrintWindow on the D3D12 window is blank: use pcsx2's own screenshot hotkey (F8 -> <exe dir>/snaps).
  $snaps = Join-Path (Split-Path $p.Path) 'snaps'
  $t0 = Get-Date
  [void][W]::SetForegroundWindow($h)
  [W]::keybd_event(0x77, 0x42, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 100
  [W]::keybd_event(0x77, 0x42, 2, [UIntPtr]::Zero)
  for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 200
    $f = Get-ChildItem $snaps -Filter *.png | Where-Object { $_.LastWriteTime -ge $t0.AddSeconds(-1) } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($f) { break }
  }
  if (!$f) { "no snap"; exit 1 }
  $bmp = $null
  for ($i = 0; $i -lt 20 -and !$bmp; $i++) {
    Start-Sleep -Milliseconds 300
    try { $bmp = [System.Drawing.Image]::FromFile($f.FullName) } catch { }
  }
  if (!$bmp) { "snap unreadable $($f.FullName)"; exit 1 }
  $small = New-Object System.Drawing.Bitmap $bmp, ([int]($bmp.Width / 2)), ([int]($bmp.Height / 2))
  $bmp.Dispose(); Remove-Item $f.FullName
  $small.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png); "shot $Shot $($small.Width)x$($small.Height)"
}
