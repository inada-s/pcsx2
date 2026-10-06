# Pad input latency on the title main menu. Needs a pcsx2 build with
# ZdxsvInputLatency. One instance, p1 data dir, windowed.
#   latency.ps1                      -> measure on the menu cursor byte, 40 presses
#   latency.ps1 -Spec 'count=10'     -> search mode (no addr=): candidate bytes
#   latency.ps1 -LowLatency 0|1      -> sets EmuCore/GS ZdxsvLowLatencyVsync in p1's ini first
# p1's card autoloads system data; f2600 + Start = attract -> main menu.
# Prints the ZdxsvLatency log lines; CSV per press in -Out.
param([string]$Spec = 'addr=0x00b45ac0,count=40', [string]$Out = 'latency.csv', [int]$Seed = 1, [int]$LowLatency = -1)
$z = $PSScriptRoot
. "$PSScriptRoot\env.ps1"  # $py, RUN
if ($LowLatency -ge 0) {
  $ini = "$env:RUN\p1\PCSX2\inis\PCSX2.ini"
  $v = if ($LowLatency) { 'true' } else { 'false' }
  $c = (Get-Content $ini -Encoding UTF8) | Where-Object { $_ -notmatch '^ZdxsvLowLatencyVsync =' }
  $c -replace '^\[EmuCore/GS\]$', "[EmuCore/GS]`nZdxsvLowLatencyVsync = $v" | Set-Content $ini -Encoding UTF8
}
$count = if ($Spec -match 'count=(\d+)') { [int]$Matches[1] } else { 20 }
$env:ZDXSV_INPUT_LATENCY = "btn=down,back=up,start=2900,gap=60,seed=$Seed,out=$Out,$Spec"
& "$z\launch.ps1" -N 1 | Out-Null
Remove-Item Env:ZDXSV_INPUT_LATENCY
Start-Sleep 8
& $py "$z\pine.py" seq "f2600,Return" 2>$null
& $py "$z\pine.py" seq "f$(2900 + 60 * $count + 60)" 2>$null
Select-String -Path "$env:RUN\p1\PCSX2\logs\emulog.txt" -Pattern 'ZdxsvLatency' | ForEach-Object { $_.Line }
Get-Process pcsx2-qtx64 -ErrorAction SilentlyContinue | Stop-Process -Force
