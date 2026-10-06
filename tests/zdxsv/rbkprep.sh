#!/bin/bash
# Makes the rbk start states: each of p1..p4 resumed from its 戦場選択 lobby state, side entry
# (パートナー自動選抜) answered by fake_lobby.py, then saved (PINE slot 9) while it waits for the
# battle start -> $OUT/rbk-pN.p2s. Then the battle is started (fake lobby --hold) and run for
# HOLDS s so a probe (PCSX2_ENV_P1="ZDXSV_EE_PROBE=..") sees the battle start, and all is stopped.
#   OUT=<dir> bash tests/zdxsv/rbkprep.sh
# Env as m4.sh: STATES, IP, ZBIN, PCSX2_ENV, PCSX2_ENV_Pn; HOLDS (default 120).
here=$(cd "$(dirname "$0")" && pwd -W)
. "$here/riglock.sh"  # one rig at a time
zdxsv_need ZDXSV
zdxsv_need STATES
OUT=${OUT:?OUT=dir for logs}
DRIVE="env MEMGATE_RESERVE_MB=${DRIVE_RESERVE_MB:-150} $PY $here/drive.py"
zdxsv_need IP
Z=${ZBIN:-$ZDXSV/bin/zdxsv.exe}
SS="SLPS-25419 (435D8236).09.p2s"
mkdir -p "$OUT"
export BATTLEREG=${BATTLEREG:-$RUN/battlereg.exe}
export ZDXSV_BATTLE_ADDR=:8210 ZDXSV_BATTLE_RPC_ADDR=127.0.0.1:3080 ZDXSV_BATTLE_PUBLIC_ADDR=$IP:8210
pids=()
cleanup() {
  kill "${pids[@]}" 2>/dev/null
  powershell -NoProfile -Command "Get-Process pcsx2* -EA 0 | Stop-Process -Force; Get-CimInstance Win32_Process | ? { \$_.CommandLine -match 'fake_lobby.py|drive.py' } | % { Stop-Process -Id \$_.ProcessId -Force -EA 0 }" 2>/dev/null
  for i in 1 2 3 4; do cp "$RUN/p$i/PCSX2/logs/emulog.txt" "$OUT/emulog-p$i.txt" 2>/dev/null; done
}
trap 'cleanup; rig_release' EXIT
. "$here/entry1.sh"
rm -f "$OUT/go"
(cd "$RUN" && exec "$Z" battle > "$OUT/battle.log" 2>&1) & pids+=($!)
sleep 2
"$PY" -u "$here/fake_lobby.py" --trace "$STATES/lobby-trace.log" --battle "$IP:8210" --hold "$OUT/go" > "$OUT/fake.log" 2>&1 & pids+=($!)
sleep 2
rm -f "$RUN"/p[1-4]/PCSX2/logs/emulog.txt
for i in 1 2 3 4; do
  eval "penv=\${PCSX2_ENV_P$i:-}"
  rm -f "$RUN/p$i/PCSX2/sstates/$SS"
  entry1_retry $i env $PCSX2_ENV $penv powershell -NoProfile -Command "& '$here/launch.ps1' -N $i -Headless -LobbyState -StateFile $STATES/lobby-p$i-senjo.p2s" \
    || { echo "FAIL client $i entry (no lobby send)"; exit 1; }
  $DRIVE entry2 $i 2>&1 | grep -v memgate | tail -1
  for t in $(seq 15); do [ "$(grep -c "entry .* side" "$OUT/fake.log")" -ge "$i" ] && break; sleep 4; done
  [ "$(grep -c "entry .* side" "$OUT/fake.log")" -ge "$i" ] || { echo "FAIL client $i entry"; exit 1; }
  sleep 3
  $PY $here/pine.py --slot $((28010 + i)) save 9 2>&1 | tail -1
  for t in $(seq 30); do [ -s "$RUN/p$i/PCSX2/sstates/$SS" ] && break; sleep 1; done
  sleep 2
  cp "$RUN/p$i/PCSX2/sstates/$SS" "$OUT/rbk-p$i.p2s" && echo "client $i: saved $(stat -c %s "$OUT/rbk-p$i.p2s") B" || { echo "FAIL client $i save"; exit 1; }
done
touch "$OUT/go"
sleep "${HOLDS:-120}"
grep -a -c "join udp peer" "$OUT/battle.log"
exit 0
