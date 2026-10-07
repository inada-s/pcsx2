# Delta-state / GGPO synctest run: $Var=$Spec (ZDXSV_DELTA_TEST or ZDXSV_GGPO), pine $Route first (-Route arcade: offline battle from ~f8000), snapshots at $Snaps frames, log lines out.
param([string]$Spec = 'start=1500,frames=3000,depth=8,every=20', [string]$Var = 'ZDXSV_DELTA_TEST', [int[]]$Snaps = @(), [int]$End = 4600, [string]$Tag = 'a', [string]$Out = 'delta', [string]$Route = '')
. "$PSScriptRoot/routes.ps1"  # $routes
if ($routes.ContainsKey($Route)) { $Route = $routes[$Route] }
$z = $PSScriptRoot
. "$PSScriptRoot\env.ps1"  # $py, RUN
if ($envFail) { $envFail; exit 2 }
$o = $Out -replace '/', '\';New-Item -ItemType Directory -Force $o | Out-Null
Get-Process pcsx2-qtx64 -ErrorAction SilentlyContinue | Stop-Process -Force
Set-Item "Env:$Var" $Spec
& "$z\launch.ps1" -N 1 | Out-Null
Remove-Item "Env:$Var"
Start-Sleep 8
if ($Route) { & $py "$z\pine.py" seq $Route 2>$null }
foreach ($f in $Snaps) { & $py "$z\pine.py" seq "f$f" 2>$null; & $py "$z\pine.py" snap "$o\$Tag-f$f.png" 0.5 2>$null | Out-Null }
& $py "$z\pine.py" seq "f$End" 2>$null
Start-Sleep 2
Copy-Item "$env:RUN\p1\PCSX2\logs\emulog.txt" "$o\$Tag-emulog.txt"
Get-Process pcsx2-qtx64 -ErrorAction SilentlyContinue | Stop-Process -Force
Select-String -Path "$o\$Tag-emulog.txt" -Pattern 'ZdxsvDelta|ZdxsvGgpo' | ForEach-Object { $_.Line }
# ZDXSV_DELTA_TEST: exit 1 on `result FAIL` (pass 2+ differed), 3 when no result line was logged
if ($Var -eq 'ZDXSV_DELTA_TEST') {
  if (Select-String -Path "$o\$Tag-emulog.txt" -Pattern 'ZdxsvDelta: result FAIL' -Quiet) { exit 1 }
  if (-not (Select-String -Path "$o\$Tag-emulog.txt" -Pattern 'ZdxsvDelta: result' -Quiet)) { exit 3 }
}
