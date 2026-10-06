# Start pcsx2 instance(s) N with data dir $RUN\pN\PCSX2 (PINE slot 28010+N).
# New dirs are cloned from p1 (inis, bios, memcards); p1 is set up per GOALS.md.
# -Memcard <file> copies that card to memcards\Mcd001.ps2 first.
# -StateFile <p2s> boots from that save state. A lobby state resumes
# online: DEV9 adopts the PS2's open lobby connection, a stand-in (fake_lobby.py) answers.
# -LobbyState: ZDXSV_LOBBY_STATE=1 (off by default): needed to save AND load lobby states.
# -Speed X: normal emulation speed X (ini [Framerate] NominalScalar, 1 = 60 fps, max 10; written every launch).
#   Routes wait on frames (pine.py wait), so they hold at any speed.
param([int[]]$N = @(1), [string]$Memcard = "", [string]$StateFile = "", [switch]$NoGui, [switch]$Headless, [switch]$LobbyState, [double]$Speed = 1)
if ($LobbyState) { $env:ZDXSV_LOBBY_STATE = '1' }
. "$PSScriptRoot\env.ps1"  # RUN, PCSX2_EXE, ISO, ZDXSV_PY
if ($envFail) { $envFail; exit 2 }
# pcsx2 refuses a game path with / (local.env may use /)
$root = $env:RUN -replace '/', '\'
$exe = if ($env:PCSX2_EXE) { $env:PCSX2_EXE -replace '/', '\' } else { Join-Path $PSScriptRoot '..\..\bin\pcsx2-qtx64.exe' }
$iso = $env:ISO -replace '/', '\'
if (-not $iso) { 'FAIL: set ISO (the game image; environment or tests/zdxsv/local.env)'; exit 2 }
# probe writers of the exe's tree: no rig run on a bad or stale probe
$lint = & $py "$PSScriptRoot\probelint.py" --exe $exe (Split-Path (Split-Path $exe))
if ($LASTEXITCODE -ne 0) { $lint; exit 1 }
# another rig's lock (riglock.ps1): no pcsx2 into a running rig
& "$PSScriptRoot\riglock.ps1" -Check
if ($LASTEXITCODE -ne 0) { exit 75 }
foreach ($i in $N) {
  $d = "$root\p$i"
  if (!(Test-Path "$d\PCSX2\inis")) {
    New-Item -ItemType Directory -Force "$d\PCSX2" | Out-Null
    Copy-Item -Recurse "$root\p1\PCSX2\inis", "$root\p1\PCSX2\bios", "$root\p1\PCSX2\memcards" "$d\PCSX2\"
  }
  $ini = "$d\PCSX2\inis\PCSX2.ini"
  (Get-Content $ini -Encoding UTF8) -replace '^PINESlot =.*', "PINESlot = $(28010 + $i)" | Set-Content $ini -Encoding UTF8
  # -Headless: GS Null renderer (11) + -nogui; else Auto (-1). Persisted in the instance ini.
  $c = Get-Content $ini -Encoding UTF8
  if (!($c -match '^\[EmuCore/GS\]')) { $c += @('', '[EmuCore/GS]', 'Renderer = -1') }
  $r = if ($Headless) { 11 } else { -1 }
  $c = @($c -replace '^Renderer = .*', "Renderer = $r" | Where-Object { $_ -notmatch '^NominalScalar *=' })
  if (!($c -match '^\[Framerate\]')) { $c += @('', '[Framerate]') }
  $c = foreach ($l in $c) { $l; if ($l -match '^\[Framerate\]') { "NominalScalar = $Speed" } }
  $c | Set-Content $ini -Encoding UTF8
  if ($Memcard) { Copy-Item -Force $Memcard "$d\PCSX2\memcards\Mcd001.ps2" }
  $a = @('-datapath', $d, '-batch')
  if ($NoGui -or $Headless) { $a += '-nogui' }
  if ($StateFile) { $a += @('-statefile', $StateFile) }
  $p = Start-Process -FilePath $exe -ArgumentList ($a + @('--', $iso)) -PassThru
  if ($Headless) {  # hide the display window once it exists (keeps 60 fps)
    Add-Type -Namespace W -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c);' -ErrorAction SilentlyContinue
    for ($t = 0; $t -lt 60 -and (Get-Process -Id $p.Id).MainWindowHandle -eq 0; $t++) { Start-Sleep -Milliseconds 500 }
    [W.U]::ShowWindow((Get-Process -Id $p.Id).MainWindowHandle, 0) | Out-Null
  }
  "p$i pid $($p.Id) slot $(28010 + $i)"
}
