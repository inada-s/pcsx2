#!/bin/bash
# live auto-next skip test: a headless spectator in instance pN opens ZDXSV_REPLAY=udp://127.0.0.1:PORT where no
# lobby answers, with ZDXSV_LIVE_NEXT=1. Exit 0 = the failed open is skipped: the log names the skip and its cause,
# the VM pauses and waits for a new battle there (nothing boots to the title screen).
#   OUT=dir bash tests/zdxsv/livenext.sh   Env: N (instance, default 1), PORT (default 8209, nothing may listen),
# STATE (any state of the game to boot from; default rbk-p1), MAXS (default 60).
here=$(cd "$(dirname "$0")" && pwd -W)
. "$here/riglock.sh"  # one rig at a time
OUT=${OUT:?OUT=dir for logs}
N=${N:-1}
PORT=${PORT:-8209}
MAXS=${MAXS:-60}
STATE=${STATE:-${RBKSTATES:?set STATE or RBKSTATES}/rbk-p1.p2s}
mkdir -p "$OUT"
log="$RUN/p$N/PCSX2/logs/emulog.txt"
trap 'powershell -NoProfile -Command "Get-Process pcsx2* -EA 0 | Stop-Process -Force"; cp "$log" "$OUT/emulog-live.txt" 2>/dev/null; rig_release' EXIT
rm -f "$log"
env ZDXSV_REPLAY="udp://127.0.0.1:$PORT" ZDXSV_LIVE_NEXT=1 ZDXSV_REPLAY_EXIT=1 \
  powershell -NoProfile -Command "& '$here/launch.ps1' -N $N -Headless -StateFile $STATE" 2>&1 | tail -1
for _ in $(seq 1 "$MAXS"); do
  sleep 1
  grep -aq "ZdxsvGgpo: replay .*not a replay file" "$log" 2>/dev/null && break
done
sleep 10 # a wait that does not hold would boot on or stop by now
cp "$log" "$OUT/emulog-live.txt"
w=$(grep -a "ZdxsvGgpo: live: auto-next: waiting for a new battle at 127.0.0.1:$PORT (0 watched, 1 skipped): skipped, " "$log")
p=$(grep -a "(VMManager) Pausing" "$log" | tail -1)
r=$(grep -ac "(VMManager) Resuming\|ZdxsvGgpo: replay .*: end\|auto-next: stopped" "$log")
echo "${w:-no skip line}"
if [ -n "$w" ] && [ -n "$p" ] && [ "$r" = 0 ]; then
  echo "PASS skipped and waiting"
  exit 0
fi
echo "FAIL skip line ${w:+ok}${w:-missing}, pause ${p:+ok}${p:-missing}, resumed/stopped lines $r"
exit 1
