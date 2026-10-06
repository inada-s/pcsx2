# winshot.ps1 -ProcId N -Shot out.png: screen capture of a pcsx2 window's rect (foreground first),
# so ImGui overlays are included (F8 GS snapshots are not).
param([int]$ProcId, [string]$Shot)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class WS {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][WS]::SetProcessDPIAware()
$p = Get-Process -Id $ProcId
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { "no window"; exit 1 }
[void][WS]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 700
$r = New-Object WS+RECT
[void][WS]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
"shot $Shot ${w}x$hh"
