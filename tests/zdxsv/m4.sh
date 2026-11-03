#!/bin/bash
# M4 P2P battle regression: battle server + fake_lobby.py --trace (no lobby server), 4 pcsx2
# clients from a fresh boot (fast boot + fast login to fake_lobby.py's login API), side entry, battle,
# post-battle login -> BattleResult. Exit 0 = all checks passed.
#   OUT=<dir> bash tests/zdxsv/m4.sh
# Env: TRACE (an `EMU=1 m4z.sh` OUT: lobby-trace.log + zdxsv.db), CARDS (as m4z.sh, the same cards), API (login API port,
#   default 9881), IP, ZBIN, FAKE_ARGS (fake_lobby.py), PCSX2_ENV (extra env
# for the clients), MAXS (s from battle start to give up on BattleResult x4, default 2100; a headless battle can run 1370 s).
# Every client joins the battle server by TCP (pcsx2 has no UDP bridge;
#   an older pcsx2 build joins over UDP and fails the join checks).
# MASH=<drive.py route> MASHP="1": battle input route and the clients it drives (default mash, all 4).
#   MASHP="1" = time-up with side 1 ahead (2 kills, 0 deaths).
# SHOW="1 3": these clients get a window (GS snapshots pN/<route>.png per drive call); others headless.
# PCSX2_ENV_P1="..": extra env for one client only. GO=file: touched GODELAY s (30) after mash starts
#   (ZDXSV_INPUT_LATENCY go= trigger, latency in battle: m4lat.sh).
# Checks: battlereg 4 users; `join success` (TCP) x4, no `join udp peer`, no `BattleInfo not found`;
# `BATTLE RESULT` x4 with the same total_frame and kills = deaths.
# NORESULT=1 (a battle that must fail): wait for 8 lobby conns (4 + each client's reconnect) instead;
#   checks every `BATTLE RESULT` has battles=0, conn from x8.
here=$(cd "$(dirname "$0")" && pwd -W)  # a Windows path, also inside the powershell command string
. "$here/riglock.sh"  # one rig at a time
. "$here/speedcheck.sh"  # measured speed vs SPEED
zdxsv_need ZDXSV
zdxsv_need TRACE
zdxsv_need CARDS
API=${API:-9881}
OUT=${OUT:?OUT=dir for logs}
# every python start waits for 800 MB free (memgate.pth); with 3-4 pcsx2 up that held drive.py
# ~100 s, past the game's 116 s lobby timeout. drive.py is ~15 MB.
DRIVE="env MEMGATE_RESERVE_MB=${DRIVE_RESERVE_MB:-150} $PY $here/drive.py"
zdxsv_need IP
Z=${ZBIN:-$ZDXSV/bin/zdxsv.exe}
mkdir -p "$OUT"
export BATTLEREG=${BATTLEREG:-$RUN/battlereg.exe}
[ -f "$BATTLEREG" ] || (cd "$here/battlereg" && \
  GO111MODULE=off "$GOEXE" build -o "$BATTLEREG" main.go) || { echo "FAIL battlereg build"; exit 1; }
export ZDXSV_BATTLE_ADDR=:8210 ZDXSV_BATTLE_RPC_ADDR=127.0.0.1:3080 ZDXSV_BATTLE_PUBLIC_ADDR=$IP:8210
pids=()
cleanup() {
  kill "${pids[@]}" 2>/dev/null
  # pids hold launchers only (pyenv python -> python)
  powershell -NoProfile -Command "Get-Process pcsx2* -EA 0 | Stop-Process -Force; Get-CimInstance Win32_Process | ? { \$_.CommandLine -match 'fake_lobby.py|drive.py|udprelay.py' } | % { Stop-Process -Id \$_.ProcessId -Force -EA 0 }" 2>/dev/null
  for i in 1 2 3 4; do cp "$RUN/p$i/PCSX2/logs/emulog.txt" "$OUT/emulog-p$i.txt" 2>/dev/null; done
}
trap 'cleanup; rig_release' EXIT
ok=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; ok=1; fi; }

(cd "$RUN" && exec "$Z" battle > "$OUT/battle.log" 2>&1) & pids+=($!)
# ZRELAY=ip:port[,ip:port...] ZRELAY_SESSION=id:hex token: a `zdxsv relay` (GGPO relay server) at each, logs relay.log, relay2.log, ...
if [ -n "${ZRELAY:-}" ]; then
  k=1
  for a in ${ZRELAY//,/ }; do
    log=relay.log; [ $k -gt 1 ] && log=relay$k.log
    (cd "$RUN" && ZDXSV_LOBBY_RELAY_ADDR=$a exec "$Z" relay ${ZRELAY_SESSION%%:*} ${ZRELAY_SESSION##*:} > "$OUT/$log" 2>&1) & pids+=($!)
    k=$((k + 1))
  done
fi
sleep 2
"$PY" -u "$here/fake_lobby.py" --trace "$TRACE/lobby-trace.log" --api $API --db "$TRACE/zdxsv.db" --battle "$IP:8210" ${FAKE_ARGS:-} > "$OUT/fake.log" 2>&1 & pids+=($!)
sleep 2
rm -f "$RUN"/p[1-4]/PCSX2/logs/emulog.txt
# fresh boots, one at a time: fast boot + fast login (this login API) to the fake lobby
CARDS=($CARDS)
for i in 1 2 3 4; do
  eval "penv=\${PCSX2_ENV_P$i:-}"
  l=$(env ZDXSV_TLS=0 ZDXSV_FAST_LOGIN=1 ZDXSV_LOGIN_API_URL=http://127.0.0.1:$API/lbs/login $PCSX2_ENV $penv powershell -NoProfile -Command "& '$here/launch.ps1' -N $i -Speed ${SPEED:-1} -Memcard $RUN/Mcd001-${CARDS[i-1]}.ps2 $(case " ${SHOW:-} " in *" $i "*) ;; *) echo -Headless;; esac)" 2>&1)
  echo "$l" | tail -1
  echo "$l" | grep -q "pid .* slot" || { echo "FAIL launch p$i"; exit 1; }
  $DRIVE top $i 2>&1 | grep -v memgate | tail -1
  $DRIVE login $i 2>&1 | grep -v memgate | tail -1
  echo "client $i: $(grep -c 'login api' "$OUT/fake.log") logins, $(grep -c 'conn from' "$OUT/fake.log") lobby conns"
done
map_pressed() {  # $1 = 0x6305 count before the map route; waits ENTRY_WAIT s for one more
  for t in $(seq "${ENTRY_WAIT:-20}"); do [ "$(grep -ac "C->S cat 01 cmd 0x6305" "$OUT/fake.log")" -gt "$1" ] && return 0; sleep 1; done
  return 1
}
for i in 1 2 3 4; do
  a0=$(grep -ac "C->S cat 01 cmd 0x6305" "$OUT/fake.log")
  $DRIVE map $i 2>&1 | grep -v memgate | tail -1
  map_pressed $a0 || { echo "p$i: no Lobby 06 press, C again"; $DRIVE mappress $i 2>&1 | grep -v memgate | tail -1; map_pressed $a0; } ||
    { echo "FAIL client $i entry (no 0x6305)"; exit 1; }
  $DRIVE lobby2p $i 2>&1 | grep -v memgate | tail -1
  for t in $(seq 15); do [ "$(grep -c "entry .* side" "$OUT/fake.log")" -ge "$i" ] && break; sleep 2; done
  n=$(grep -c "entry .* side" "$OUT/fake.log")
  echo "client $i: $n entered"
  [ "$n" -ge "$i" ] || { echo "FAIL client $i entry"; exit 1; }
done
# the settle time measures each client's speed (speedcheck.sh)
t1=$SECONDS
for i in 1 2 3 4; do speed_check $i || exit 1; done
sleep $(( SECONDS - t1 < 15 ? 15 - (SECONDS - t1) : 0 ))
grep -v "C->S\|C<-S\|S->C" "$OUT/fake.log" | grep "battlereg\|adopted" | cut -c1-160
check "battlereg 4" "grep -q 'registered 4 users' '$OUT/fake.log'"
check "join tcp x4" "[ \$(grep -c 'join success' '$OUT/battle.log') -eq 4 ]"
check "no join udp" "! grep -q 'join udp peer' '$OUT/battle.log'"
check "no BattleInfo not found" "! grep -q 'BattleInfo not found' '$OUT/battle.log'"
[ -n "${GO:-}" ] && (sleep "${GODELAY:-30}"; touch "$GO") &
$DRIVE ${MASH:-mash} ${MASHP:-1 2 3 4} 2>&1 | grep -v memgate | tail -1
# no input after mash: rounds are timed and every post-battle screen (memcard slot select /
# create system data, 戦友登録, ...) has a countdown; the route `after` looped on the
# resumed state's memcard prompts. POST=after: drive it anyway.
t0=$SECONDS
# A 4th result missing at results 3/4 rarely comes (67 past m4 outputs: 61 went to 4/4 within one
# poll, 1 got the 4th 30 s after 3/4, 5 never did in 212-1541 s): give up R34S (90) s after 3/4.
t34=
# one line per result, matched anywhere in a line: fake_lobby.py before the log lock glued a 4th result
# into another line (`grep -c` saw 3/4, all 4 were logged; lograce.py)
bres() { grep -a -o "BATTLE RESULT [0-9.:]* battles=[0-9]* win=[0-9]* lose=[0-9]* kill=[0-9]* death=[0-9]* frames=[0-9]* side=[0-9]*" "$OUT/fake.log"; }
while [ $((SECONDS - t0)) -lt "${MAXS:-2100}" ]; do
  [ -n "${POST:-}" ] && $DRIVE $POST 1 2 3 4 2>&1 | grep -v memgate | tail -1 || sleep 30
  r=$(bres | wc -l)
  if [ -n "${NORESULT:-}" ]; then
    c=$(grep -a -o 'conn from' "$OUT/fake.log" | wc -l)
    echo "$((SECONDS - t0)) s: results $r, lobby conns $c/8"
    [ "$c" -ge 8 ] && break
    continue
  fi
  echo "$((SECONDS - t0)) s: results $r/4"
  [ "$r" -ge 4 ] && break
  if [ "$r" -eq 3 ]; then
    t34=${t34:-$SECONDS}
    [ $((SECONDS - t34)) -ge "${R34S:-90}" ] && { echo "results 3/4 for $((SECONDS - t34)) s: 4th never comes, stop"; break; }
  fi
done
bres | cut -d' ' -f3-11
if [ -n "${NORESULT:-}" ]; then
  # the game still reports after the failed battle, with battles=0
  check "no battle fought (every BATTLE RESULT battles=0)" "! bres | grep -q -v ' battles=0 '"
  check "every client reconnected to the lobby (conn from x8)" "[ \$(grep -a -o 'conn from' '$OUT/fake.log' | wc -l) -ge 8 ]"
  exit $ok
fi
check "BATTLE RESULT x4" "[ \$(bres | wc -l) -ge 4 ]"
# one battle: same total_frame on all 4, kills = deaths
check "results agree" "bres | awk '{for(i=1;i<=NF;i++){split(\$i,a,\"=\"); v[a[1]]=a[2]} f[v[\"frames\"]]=1; k+=v[\"kill\"]; d+=v[\"death\"]} END{n=0; for(x in f)n++; exit !(NR>=4 && n==1 && k==d)}'"
exit $ok
