#!/bin/bash
# Local zdxsv stack without docker: battle, lobby, login, dnas.
# Usage: bash tests/zdxsv/stack.sh [IP]   (IP = address pcsx2 peers dial; default first non-loopback)
# Logs + db: $RUN. Runs in the foreground; Ctrl-C / kill stops all.
set -u
. "$(dirname "$0")/riglock.sh"  # one rig at a time; nested under m4z.sh
zdxsv_need ZDXSV
IP=${1:-$(powershell -NoProfile -Command "(Get-NetIPAddress -AddressFamily IPv4 | ? { \$_.IPAddress -notlike '127.*' -and \$_.IPAddress -notlike '169.*' } | select -First 1).IPAddress" | tr -d '\r')}
mkdir -p "$RUN"
cd "$RUN" || exit 1
export ZDXSV_DNAS_PUBLIC_ADDR=$IP
export ZDXSV_LOGIN_ADDR=:80 ZDXSV_LOGIN_PUBLIC_ADDR=$IP
export ZDXSV_LOBBY_ADDR=:8200 ZDXSV_LOBBY_RPC_ADDR=127.0.0.1:8201 ZDXSV_LOBBY_PUBLIC_ADDR=$IP:8200
export ZDXSV_BATTLE_ADDR=:8210 ZDXSV_BATTLE_RPC_ADDR=127.0.0.1:3080 ZDXSV_BATTLE_PUBLIC_ADDR=$IP:8210
export ZDXSV_STATUS_ADDR=127.0.0.1:8080 ZDXSV_DB_NAME=zdxsv.db
Z=${ZBIN:-$ZDXSV/bin/zdxsv.exe}  # ZBIN: another build (e.g. a control)
ZDXSV=$ZDXSV bash "$(dirname "$0")/zbincheck.sh" "$Z" || exit 1  # built from the checkout (ZREV= for an old one)
[ -f zdxsv.db ] || "$Z" initdb > initdb.log 2>&1
pids=()
trap 'kill ${pids[@]} 2>/dev/null; rig_release' EXIT
"$Z" battle > battle.log 2>&1 & pids+=($!)
sleep 2
"$Z" ${LOBBY_ARGS:-} lobby > lobby.log 2>&1 & pids+=($!)  # LOBBY_ARGS=-v=2: every message
"$Z" login > login.log 2>&1 & pids+=($!)
# DNAS: `zdxsv dnas` (:443) from $DZ (default ZBIN); DNAS=<exe>: a standalone DNAS server instead
DZ=${DZ:-$Z}
if [ -n "${DNAS:-}" ]; then "$DNAS" -login 127.0.0.1:80 > dnas.log 2>&1 & pids+=($!)
else ZDXSV_LOGIN_ADDR=127.0.0.1:80 "$DZ" dnas > dnas.log 2>&1 & pids+=($!); fi
echo "stack up on $IP: pids ${pids[*]}"
wait
