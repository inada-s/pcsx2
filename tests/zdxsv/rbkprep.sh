#!/bin/bash
# Makes the rbk start states: each of p1..p4 from a fresh boot (freshboot.sh: fast boot, fast login to
# fake_lobby.py's login API), side entry (パートナー自動選抜) answered by fake_lobby.py, then saved (PINE slot 9)
# while it waits for the battle start -> $OUT/rbk-pN.p2s. Then the battle is started (fake lobby --hold) and run for
# HOLDS s so a probe (PCSX2_ENV_P1="ZDXSV_EE_PROBE=..") sees the battle start, and all is stopped.
#   OUT=<dir> bash tests/zdxsv/rbkprep.sh
# Env as m4.sh: TRACE, CARDS, API, IP, ZBIN, PCSX2_ENV, PCSX2_ENV_Pn; HOLDS (default 120).
. "$(dirname "$0")/os.sh"; here=$(cd "$(dirname "$0")" && zpwd)
. "$here/riglock.sh"  # one rig at a time
. "$here/freshboot.sh"
zdxsv_need ZDXSV
zdxsv_need TRACE
zdxsv_need CARDS
API=${API:-9881}
OUT=${OUT:?OUT=dir for logs}
DRIVE="env MEMGATE_RESERVE_MB=${DRIVE_RESERVE_MB:-150} $PY $here/drive.py"
zdxsv_need IP
Z=${ZBIN:-$ZDXSV/bin/zdxsv$ZEXE}
SS="SLPS-25419 (435D8236).09.p2s"
mkdir -p "$OUT"
export BATTLEREG=${BATTLEREG:-$RUN/battlereg$ZEXE}
export ZDXSV_BATTLE_ADDR=:8210 ZDXSV_BATTLE_RPC_ADDR=127.0.0.1:3080 ZDXSV_BATTLE_PUBLIC_ADDR=$IP:8210
pids=()
cleanup() {
  kill "${pids[@]}" 2>/dev/null
  { zkill_name pcsx2; zkill_cmd "fake_lobby[.]py|drive[.]py"; } 2>/dev/null
  for i in 1 2 3 4; do cp "$RUN/p$i/PCSX2/logs/emulog.txt" "$OUT/emulog-p$i.txt" 2>/dev/null; done
}
trap 'cleanup; rig_release' EXIT
rm -f "$OUT/go"
(cd "$RUN" && exec "$Z" battle > "$OUT/battle.log" 2>&1) & pids+=($!)
sleep 2
"$PY" -u "$here/fake_lobby.py" --trace "$TRACE/lobby.log" --api $API --db "$TRACE/zdxsv.db" --battle "$IP:8210" --hold "$OUT/go" > "$OUT/fake.log" 2>&1 & pids+=($!)
sleep 2
rm -f "$RUN"/p[1-4]/PCSX2/logs/emulog.txt
CARDS=($CARDS)
rm -f "$RUN"/p[1-4]/PCSX2/sstates/"$SS"
fresh_boot_all 1 2 3 4 || exit 1
for i in 1 2 3 4; do
  fresh_entry $i || exit 1
  sleep 3
  $PY $here/pine.py --slot $((28010 + i)) save 9 2>&1 | tail -1
  for t in $(seq 30); do [ -s "$RUN/p$i/PCSX2/sstates/$SS" ] && break; sleep 1; done
  sleep 2
  cp "$RUN/p$i/PCSX2/sstates/$SS" "$OUT/rbk-p$i.p2s" && echo "client $i: saved $(wc -c < "$OUT/rbk-p$i.p2s" | tr -d " ") B" || { echo "FAIL client $i save"; exit 1; }
done
touch "$OUT/go"
sleep "${HOLDS:-120}"
grep -a -c "join udp peer" "$OUT/battle.log"
exit 0
