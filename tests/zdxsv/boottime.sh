#!/bin/bash
# Boot-to-lobby time: local stack (stack.sh) + p1 from a cold boot; measures from the launch to the
# lobby's user_id selection screen (ID選択, game RAM over PINE: boottime.py). No input by default.
#   OUT=<dir> bash tests/zdxsv/boottime.sh
# Env: LIMIT (s, default 5), MAXS (s to wait, default 200), ROUTE (drive.py keys sent from the start;
# ROUTE=top,C = today's menu path + ログイン at TOP), CARD (p1 card name, default the first of CARDS),
# SPEED (launch.ps1 -Speed, default 1), SHOW=1 (windowed).
# Exit 0 = the screen is up within LIMIT s. Logs: OUT/boottime.txt (net step changes), stack logs.
here=$(cd "$(dirname "$0")" && pwd -W)
. "$here/riglock.sh"  # one rig at a time
zdxsv_need ZDXSV
zdxsv_need IP
OUT=${OUT:?OUT=dir for logs}
zdxsv_need CARDS
CARD=${CARD:-${CARDS%% *}}
mkdir -p "$OUT"
cp "$RUN/zdxsv.db" "$OUT/zdxsv.db" || exit 1
pids=()
killall() { powershell -NoProfile -Command "Get-Process pcsx2*,zdxsv*,dnas* -EA 0 | Stop-Process -Force"; }
killall
cleanup() {
  kill "${pids[@]}" 2>/dev/null
  killall
  cp "$RUN/p1/PCSX2/logs/emulog.txt" "$OUT/emulog-p1.txt" 2>/dev/null
  [ -f "$OUT/live-p1.ps2" ] && cp -p "$OUT/live-p1.ps2" "$RUN/p1/PCSX2/memcards/Mcd001.ps2"
}
trap 'cleanup; rig_release' EXIT
ZDXSV=$ZDXSV bash "$here/zbincheck.sh" "${ZBIN:-$ZDXSV/bin/zdxsv.exe}" || exit 1
api=127.0.0.1:9881
ZDXSV_LOBBY_API_ADDR=$api LOBBY_ARGS=-v=2 RUN="$OUT" bash "$here/stack.sh" "$IP" > "$OUT/stack.out" 2>&1 & pids+=($!)
sleep 6
grep -q "stack up" "$OUT/stack.out" || { cat "$OUT/stack.out"; echo "FAIL stack"; exit 1; }
rm -f "$RUN/p1/PCSX2/logs/emulog.txt"
cp -p "$RUN/p1/PCSX2/memcards/Mcd001.ps2" "$OUT/live-p1.ps2"
t0=$(date +%s.%N)
# ZDXSV_TLS=0: the local stack has no TLS ports. Fast login API: the lobby API, /lbs/login
l=$(env ZDXSV_TLS=0 ZDXSV_LOGIN_API_URL=${ZDXSV_LOGIN_API_URL:-http://$api/lbs/login} powershell -NoProfile -Command "& '$here/launch.ps1' -N 1 -Speed ${SPEED:-1} -Memcard $RUN/Mcd001-$CARD.ps2 $([ -n "${SHOW:-}" ] || echo -Headless)" 2>&1)
echo "$l" | tail -1
echo "$l" | grep -q "pid .* slot" || { echo "FAIL launch p1"; exit 1; }
"$PY" "$here/boottime.py" --t0 "$t0" --out "$OUT" --limit "${LIMIT:-5}" --maxs "${MAXS:-200}" ${ROUTE:+--route "$ROUTE"}
