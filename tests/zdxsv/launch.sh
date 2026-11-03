#!/bin/bash
# Start pcsx2 instance(s) N with data dir $RUN/pN/PCSX2 (PINE slot 28010+N); prints "pN pid <OS pid> slot S".
# New dirs are cloned from p1 (inis, bios, memcards); p1 is set up per README.md.
#   -N "1 2"         instances (default 1)
#   -Memcard <file>  copies that card to memcards/Mcd001.ps2 first
#   -StateFile <p2s> boots from that save state. A lobby state resumes online: DEV9 adopts the PS2's open
#                    lobby connection, a stand-in (fake_lobby.py) answers.
#   -LobbyState      ZDXSV_LOBBY_STATE=1 (off by default): needed to save AND load lobby states
#   -Speed X         normal emulation speed X (ini [Framerate] NominalScalar, 1 = 60 fps, max 10; written
#                    every launch). Routes wait on frames (pine.py wait), so they hold at any speed.
#   -Headless        GS Null renderer (11) + -nogui, display window hidden; else -Renderer R (default -1, Auto).
#   -NoGui
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
N=1 memcard= state= nogui= headless= speed=1 renderer=-1
while [ $# -gt 0 ]; do
  case $1 in
    -N) N=$2; shift;;
    -Memcard) memcard=$2; shift;;
    -StateFile) state=$2; shift;;
    -LobbyState) export ZDXSV_LOBBY_STATE=1;;
    -Speed) speed=$2; shift;;
    -Renderer) renderer=$2; shift;;
    -NoGui) nogui=1;;
    -Headless) headless=1;;
    *) echo "FAIL $0: unknown argument $1"; exit 2;;
  esac
  shift
done
. "$here/env.sh"  # RUN, PY, PCSX2_EXE, ISO
# pcsx2 runs lobby battles over GGPO by default: a client without ZDXSV_GGPO gets
# ZDXSV_GGPO=0 (battle server); ZDXSV_GGPO=default leaves GGPO on.
case ${ZDXSV_GGPO:-} in '') export ZDXSV_GGPO=0;; default) unset ZDXSV_GGPO;; esac
# Fast boot (on by default) presses through the menus the routes drive: off unless
# ZDXSV_FAST_BOOT is set; ZDXSV_FAST_BOOT=default leaves the setting's default.
case ${ZDXSV_FAST_BOOT:-} in '') export ZDXSV_FAST_BOOT=0;; default) unset ZDXSV_FAST_BOOT;; esac
. "$here/localnet.sh"
zdxsv_need ISO
root=$(zupath "$RUN")
if [ -n "$ZWIN" ]; then exe=$(zupath "${PCSX2_EXE:-$here/../../bin/pcsx2-qtx64.exe}"); else zdxsv_need PCSX2_EXE; exe=$PCSX2_EXE; fi
case $exe in *.app) exe=$exe/Contents/MacOS/$(basename "$exe" .app);; esac
tree=$(dirname "$exe"); while [ "$tree" != / ] && [ ! -d "$tree/pcsx2/Zdxsv" ]; do tree=$(dirname "$tree"); done
[ "$tree" != / ] || tree=$here/../..
# pcsx2 refuses a game path with /
iso=$(zpath "$ISO")
# probe writers of the exe's tree: no rig run on a bad or stale probe
lint=$("$PY" "$here/probelint.py" --exe "$(zpath "$exe")" "$(zpath "$tree")") || { echo "$lint"; exit 1; }
# another rig's lock: no pcsx2 into a running rig
zriglock -Check -Anc "$(zpid $$),$(zanc $$)" || exit 75
for i in $N; do
  d=$root/p$i
  if [ ! -d "$d/PCSX2/inis" ]; then
    mkdir -p "$d/PCSX2"
    cp -R "$root/p1/PCSX2/inis" "$root/p1/PCSX2/bios" "$root/p1/PCSX2/memcards" "$d/PCSX2/"
  fi
  ini=$d/PCSX2/inis/PCSX2.ini
  r=$renderer; [ -n "$headless" ] && r=11
  awk -v slot=$((28010 + i)) -v r="$r" -v speed="$speed" '
    { sub(/\r$/, "") }
    /^PINESlot =/ { $0 = "PINESlot = " slot }
    /^Renderer = / { $0 = "Renderer = " r }
    /^NominalScalar *=/ { next }
    /^\[EmuCore\/GS\]$/ { gs = 1 }
    { print }
    /^\[Framerate\]$/ { fr = 1; print "NominalScalar = " speed }
    END {
      if (!gs) { print ""; print "[EmuCore/GS]"; print "Renderer = " r }
      if (!fr) { print ""; print "[Framerate]"; print "NominalScalar = " speed }
    }' "$ini" > "$ini.new" && mv -f "$ini.new" "$ini"
  [ -n "$memcard" ] && cp -f "$memcard" "$d/PCSX2/memcards/Mcd001.ps2"
  a=(-datapath "$(zpath "$d")" -batch)
  [ -n "$nogui$headless" ] && a+=(-nogui)
  [ -n "$state" ] && a+=(-statefile "$(zpath "$state")")
  # no stdio: a caller's $(launch.sh) returns while pcsx2 runs
  "$exe" "${a[@]}" -- "$iso" < /dev/null > /dev/null 2>&1 &
  wp=$(zpid $!)
  if [ -n "$headless" ] && [ -n "$ZWIN" ]; then  # hide the display window once it exists (keeps 60 fps)
    powershell -NoProfile -Command "Add-Type -Namespace W -Name U -MemberDefinition '[DllImport(\"user32.dll\")] public static extern bool ShowWindow(System.IntPtr h, int c);'
      for (\$t = 0; \$t -lt 60 -and (Get-Process -Id $wp).MainWindowHandle -eq 0; \$t++) { Start-Sleep -Milliseconds 500 }
      [void][W.U]::ShowWindow((Get-Process -Id $wp).MainWindowHandle, 0)" > /dev/null 2>&1
  fi
  echo "p$i pid $wp slot $((28010 + i))"
done
