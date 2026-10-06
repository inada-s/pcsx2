# One pcsx2/zdxsv rig at a time. Rigs share fixed ports (7001.., 8200.., 28010..) and
# each kills pcsx2*/zdxsv* at start, so a 2nd rig wrecks the 1st:
# - a 2nd batch started while the 1st ran: its rbk.sh killed the 1st's pcsx2.
# - a zdxsv build outlived the kill of its stack.sh, held :8201; m4z clients logged in to it.
# Lock $RUN\rig.lock = "<owner bash winpid> <time> <what>"; line 2 "anc <winpid>,..":
#   the owner's MSYS bash ancestors (-Anc, from riglock.sh).
# -Take <winpid> -What <text> (riglock.sh): owner alive -> exit 75 (rig busy) unless $env:RIG_OWNER is
#   the owner (nested rig). Else kill the dead rig's orphans, write the lock, exit 0.
# -Check (launch.ps1): owner alive, not $env:RIG_OWNER, caller not under an owner ancestor -> exit 75.
# -Release <winpid>: remove the lock if <winpid> owns it.
param([int]$Take = 0, [string]$What = "", [string]$Anc = "", [switch]$Check, [int]$Release = 0)
. "$PSScriptRoot\env.ps1"
$lock = Join-Path $env:RUN 'rig.lock'
$owner = 0; $line = ''
if (Test-Path $lock) { $line = (Get-Content $lock -TotalCount 1); $owner = [int](($line -split ' ')[0]) }
if ($Release) {
  if ($owner -eq $Release) { Remove-Item $lock -Force -EA 0 }
  exit 0
}
# alive = a bash.exe with that pid (a reused pid is not the owner)
$alive = $owner -and (Get-CimInstance Win32_Process -Filter "ProcessId = $owner AND Name = 'bash.exe'" -EA 0)
# -Check from the rig's own script (a script that ran `stack.sh &` then launch.ps1, RIG_OWNER unset, had
# its own launch was refused): passes when a live bash in the caller's Windows parent chain (forks: parents
# live) is one of the owner's MSYS ancestors (lock line 2, from riglock.sh). A 2nd tool call shares none.
if ($Check -and $alive -and "$owner" -ne "$env:RIG_OWNER") {
  $up = @{}; ((@(Get-Content $lock -TotalCount 2) + "")[1] -replace '^anc ?', '') -split ',' | ? { $_ } | % { $up[[int]$_] = 1 }
  $procs = @{}; Get-CimInstance Win32_Process -EA 0 | % { $procs[[int]$_.ProcessId] = $_ }
  $id = [int]$procs[$PID].ParentProcessId; $seen = @{}
  while ($id -and $procs.ContainsKey($id) -and $procs[$id].Name -eq 'bash.exe' -and -not $seen.ContainsKey($id)) {
    if ($up.ContainsKey($id)) { exit 0 }
    $seen[$id] = 1; $id = [int]$procs[$id].ParentProcessId
  }
}
if ($alive -and "$owner" -ne "$env:RIG_OWNER") {
  "rig busy: $line"
  "  wait for it, or end it: powershell -NoProfile -Command `"Stop-Process -Id $owner -Force`" (then its children)"
  exit 75
}
if ($Check -or $alive) { exit 0 }  # launch.ps1 check, or a nested rig under the live owner
# lock free or owner dead: whatever rig processes remain are orphans
$names = 'pcsx2*', 'zdxsv*', 'dnas*', 'zproxy*'
$orph = @(Get-Process $names -EA 0)
$orph += @(Get-CimInstance Win32_Process -Filter "Name like 'python%'" -EA 0 |
  ? { $_.CommandLine -match 'udprelay\.py|fake_lobby\.py|drive\.py' } | % { Get-Process -Id $_.ProcessId -EA 0 })
if ($orph.Count) {
  "riglock: killing orphans of a dead rig ($line): " + (($orph | % { "$($_.Name):$($_.Id)" }) -join ' ')
  $orph | Stop-Process -Force -EA 0
  $orph | Wait-Process -Timeout 20 -EA 0
}
New-Item -ItemType Directory -Force (Split-Path $lock) | Out-Null
"$Take $(Get-Date -Format 'MM-dd HH:mm:ss') $What", "anc $Anc" | Set-Content $lock -Encoding ASCII
exit 0
