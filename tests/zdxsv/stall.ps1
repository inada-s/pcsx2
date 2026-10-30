# stall.ps1: suspend the pcsx2 started with -datapath <...>\<Peer> for Ms every EverySec, Count times,
# from StartSec after it appears (rbk.sh STALL: makes the other peers run ahead, so GGPO asks them to wait).
param([string]$Peer = 'p3', [int]$StartSec = 40, [double]$EverySec = 5, [int]$Ms = 400, [int]$Count = 8)
Add-Type -Namespace ZdxsvStall -Name Nt -MemberDefinition @'
[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(System.IntPtr h);
[DllImport("ntdll.dll")] public static extern int NtResumeProcess(System.IntPtr h);
'@
$re = "-datapath\s+`"?[^`"]*[\\/]$([regex]::Escape($Peer))(`"|\s|$)"
$proc = $null
for ($i = 0; $i -lt 120 -and -not $proc; $i++) {
  $c = Get-CimInstance Win32_Process -Filter "Name like 'pcsx2-qt%.exe'" | Where-Object { $_.CommandLine -match $re } | Select-Object -First 1
  if ($c) { $proc = Get-Process -Id $c.ProcessId } else { Start-Sleep -Milliseconds 500 }
}
if (-not $proc) { "stall: no pcsx2 for $Peer"; exit 1 }
"stall: $Peer pid $($proc.Id), first stall in $StartSec s"
Start-Sleep -Seconds $StartSec
for ($k = 0; $k -lt $Count; $k++) {
  if ($proc.HasExited) { "stall: $Peer exited after $k stalls"; exit 0 }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  [void][ZdxsvStall.Nt]::NtSuspendProcess($proc.Handle)
  Start-Sleep -Milliseconds $Ms
  [void][ZdxsvStall.Nt]::NtResumeProcess($proc.Handle)
  "stall $k $([int]$sw.ElapsedMilliseconds) ms"
  Start-Sleep -Milliseconds ([int]($EverySec * 1000 - $Ms))
}
