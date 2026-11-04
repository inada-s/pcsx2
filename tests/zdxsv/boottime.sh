#!/bin/bash
# Boot-to-lobby time: local stack (stack.sh) + p1 from a cold boot; measures from the launch to the
# lobby's user_id selection screen (ID選択, game RAM over PINE: boottime.py). No input by default.
#   OUT=<dir> bash tests/zdxsv/boottime.sh
# Env: LIMIT (s, default 5), MAXS (s to wait, default 200), ROUTE (drive.py keys sent from the start;
# ROUTE=top,C = today's menu path + ログイン at TOP), CARD (p1 card name, default the first of CARDS),
# SPEED (launch.sh -Speed, default 1), SHOW=1 (windowed), LOBBY=fake (boot_lobby.py instead of the stack:
# only the lobby API login and the lobby up to the user_id list).
# Exit 0 = the screen is up within LIMIT s. Logs: OUT/boottime.txt (net step changes), stack logs.
. "$(dirname "$0")/os.sh"; here=$(cd "$(dirname "$0")" && zpwd)
. "$here/riglock.sh"  # one rig at a time
zdxsv_need ZDXSV
zdxsv_need IP
OUT=${OUT:?OUT=dir for logs}
zdxsv_need CARDS
CARD=${CARD:-${CARDS%% *}}
mkdir -p "$OUT"
cp "$RUN/zdxsv.db" "$OUT/zdxsv.db" || exit 1
pids=()
killall() { zkill_name pcsx2 zdxsv dnas; }
killall
cleanup() {
  kill "${pids[@]}" 2>/dev/null
  [ -f "$OUT/boot_lobby.pid" ] && zkill "$(cat "$OUT/boot_lobby.pid")"
  killall
  cp "$RUN/p1/PCSX2/logs/emulog.txt" "$OUT/emulog-p1.txt" 2>/dev/null
  [ -f "$OUT/live-p1.ps2" ] && cp -p "$OUT/live-p1.ps2" "$RUN/p1/PCSX2/memcards/Mcd001.ps2"
}
trap 'cleanup; rig_release' EXIT
api=127.0.0.1:9881
if [ "${LOBBY:-}" = fake ]; then
  rm -f "$OUT/boot_lobby.pid"
  "$PY" -u "$here/boot_lobby.py" --public "$IP:8200" --api $api --pidfile "$OUT/boot_lobby.pid" > "$OUT/boot_lobby.log" 2>&1 & pids+=($!)
  sleep 2
  grep -q "boot lobby up" "$OUT/boot_lobby.log" || { cat "$OUT/boot_lobby.log"; echo "FAIL boot lobby"; exit 1; }
else
  ZDXSV=$ZDXSV bash "$here/zbincheck.sh" "${ZBIN:-$ZDXSV/bin/zdxsv$ZEXE}" || exit 1
  ZDXSV_LOBBY_API_ADDR=$api LOBBY_ARGS=-v=2 RUN="$OUT" bash "$here/stack.sh" "$IP" > "$OUT/stack.out" 2>&1 & pids+=($!)
  sleep 6
  grep -q "stack up" "$OUT/stack.out" || { cat "$OUT/stack.out"; echo "FAIL stack"; exit 1; }
fi
rm -f "$RUN/p1/PCSX2/logs/emulog.txt"
cp -p "$RUN/p1/PCSX2/memcards/Mcd001.ps2" "$OUT/live-p1.ps2"
t0=$(date +%s.%N)
# ZDXSV_TLS=0: the local stack has no TLS ports. Fast login API: the lobby API, /lbs/login
l=$(env ZDXSV_TLS=0 ZDXSV_LOGIN_API_URL=${ZDXSV_LOGIN_API_URL:-http://$api/lbs/login} bash "$here/launch.sh" -N 1 -Speed ${SPEED:-1} -Memcard $RUN/Mcd001-$CARD.ps2 $([ -n "${SHOW:-}" ] || echo -Headless) 2>&1)
echo "$l" | tail -1
echo "$l" | grep -q "pid .* slot" || { echo "FAIL launch p1"; exit 1; }
pid=$(echo "$l" | grep -o "pid [0-9]*" | tail -1 | cut -d" " -f2)
"$PY" "$here/boottime.py" --t0 "$t0" --pid "$pid" --emulog "$RUN/p1/PCSX2/logs/emulog.txt" --out "$OUT" --limit "${LIMIT:-5}" --maxs "${MAXS:-200}" ${ROUTE:+--route "$ROUTE"}
