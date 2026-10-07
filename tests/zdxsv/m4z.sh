#!/bin/bash
# M4 real-zproxy case: real stack (stack.sh: battle + lobby + login + dnas), 4 pcsx2 clients from a
# fresh boot (top + login + entry, no lobby states), all 4 act as real PS2s
# (ZDXSV_PLATFORM_INFO=0: no platform info). The lobby splits lobbies by platform
# (inada-s/zdxsv#47): a console never meets an emulator, so all 4 must be consoles (else e.g.
# p2 alone in the console ジャブロー2). Clients in $ZP run behind their own zproxy (port 8250+i):
# the lobby hands the PS2 zproxy's address in 0x6916, zproxy relays over UDP to the battle server
# and pings the other zproxies (P2P); the rest dial the battle server over TCP.
# Exit 0 = all checks passed.
#   OUT=<dir> bash tests/zdxsv/m4z.sh
# Env: ZP (clients behind zproxy, default "1 3": one per side), ZPROXY (exe), SHOW ("1 2":
# windowed with GS snapshots), MAXS (s after mash to wait for results).
# EMU=1: the 4 clients are emulators instead (platform info, TCP to the battle server, no zproxy by default);
#   GGPO=7101: with ZDXSV_GGPO net=1,lobby=1,port=GGPO+i-1 on GGPO_CLIENTS (default "1 2 3 4"; a lobby with
#   the GGPO battle info, ZBIN): the battle runs over GGPO (checks below).
#   BATTLES=N (CLIENTS="1 3", 1v1): N battles in the same processes (EXIT from 作戦後部屋, Lobby 02 again);
#   the result and GGPO checks then expect N battle codes and N GGPO battle ends per client.
#   GGPO_DEFAULT=K: client K gets no ZDXSV_GGPO (launch.ps1 ZDXSV_GGPO=default), so the ZdxsvGgpo setting
#   gives its options (port 7001); check: its log names the default options.
#   BAD_SESSION=K (GDELAY=auto): client K gets badsession=1: every ping test fails,
#   every client cuts the battle connection; no mash; waits for each cut to end + 2 lobby tcp conns per client;
#   checks: cut + cut ended per client, no GGPO session, no result with frames.
#   GGPO_CLIENTS without a client of CLIENTS (GGPO): that client has no GGPO port (ZDXSV_GGPO=0), so every
#   GGPO client cuts (`a peer has no GGPO address`); the same wait and checks as BAD_SESSION.
#   Both: CUTMAX=V fails a cut that took more than V vsyncs to end.
#   REPORT=1: the lobby logged one 0x9952 match report per
#   client for the results' battle code: result=ggpo + close=net battle end + 0 mismatches (cut: result=cut).
#   OSD=1: each GGPO client's last `osd frame` log line (-> osd-pN.txt)
#   has its delay and every opponent's user id, name (battle info name_) and ping; names = zdxsv.db (osdname.py).
#   NAT=open|cone|symmetric|unknown: each GGPO client ran the
#   connectivity test once (lobby reconnects too) with that result.
#   RELAY=1: lobby relay on :8203; each GGPO client registered it and every path
#   is RELAY_PATH (default direct: a LAN peer is never 16 ms slower than the relay).
#   GGPO_LAT=D (CLIENTS="1 3", GGPO, GDELAY=auto; pcsx2 ZDXSV_GGPO advertise=): a udprelay.py (D ms one way) on
#   127.0.0.1:7300/7301 between p1 and p3's GGPO ports; each announces only its relay socket, so the direct
#   path is ~2D ms rtt. Checks: lobby delay = max(GMIN or 2, ceil(2D/32)), or GMIN/2 when RELAY_PATH is a relay.
#   With RELAY=1 RELAY_PATH="relay 0" the GGPO traffic skips udprelay.py (its count = ping test only).
#   REPLAY_UP=1 (GGPO): lobby replay server on :8204 into $OUT/replays-up. Checks per battle code: one
#   GGPO client logged upload ok, the others "already there" (UPLOAD_OFF=K: client K has the setting
#   ZdxsvUploadReplay off and logs no upload); the stored file = that client's own .zdxr, byte for byte.
# db: copied from $RUN/zdxsv.db (the cards' accounts) into $OUT; stack logs land in $OUT.
# Cards: each client starts from $RUN/Mcd001-<CARDS[i]>.ps2 (CARDS, required: one registered card per client);
# the live card is put back after (m4.sh's lobby states use it). A post-battle prompt creates system
# data on the card, after which `top` lands in an offline battle.
here=$(cd "$(dirname "$0")" && pwd -W)
. "$here/riglock.sh"  # one rig at a time
. "$here/speedcheck.sh"  # measured speed vs SPEED
. "$here/entry2p.sh"  # 1v1 entry: Lobby 02 press checked + retried
zdxsv_need ZDXSV
OUT=${OUT:?OUT=dir for logs}
DRIVE="env MEMGATE_RESERVE_MB=${DRIVE_RESERVE_MB:-150} $PY $here/drive.py"
zdxsv_need IP
if [ -n "${EMU:-}" ]; then ZP=${ZP:-}; else ZP=${ZP:-1 3}; fi
ZPROXY=${ZPROXY:-$ZDXSV/bin/zproxy.exe}
zdxsv_need CARDS USERS
CARDS=($CARDS)
USERS=($USERS)  # the zdxsv.db user ids of the p1..p4 cards, in order
# CLIENTS="1 3": a 1v1 in Lobby 02 (route entry2p; p1 AEUG, p3 Titans), else the 2v2 in Lobby 06
CLIENTS=${CLIENTS:-1 2 3 4}; nc=$(echo $CLIENTS | wc -w)
ENTRY=entry; [ "$nc" -eq 2 ] && ENTRY=entry2p
nz=$(echo $ZP | wc -w)  # UDP joins: zproxy clients only (pcsx2 has no UDP bridge)
# client i's env: console (no platform info), or emulator (+ GGPO lobby battle on port GGPO+i-1)
cenv() {
  [ -z "${EMU:-}" ] && { echo ZDXSV_PLATFORM_INFO=0; return; }
  # PWTRACE=1: player-work hashes + net trace per client (OUT/trace-p<i>.txt; rplay.sh compares a replay to them)
  [ -n "${PWTRACE:-}" ] && echo "ZDXSV_PW_HASH=1 ZDXSV_NET_TRACE=$OUT/trace-p$1.txt"
  [ "$1" = "${UPLOAD_OFF:-}" ] && echo ZDXSV_UPLOAD_REPLAY=0
  # GGPO_DEFAULT=K (GGPO, GDELAY=auto): client K has no ZDXSV_GGPO, its GGPO options come from the setting
  [ -n "${GGPO:-}" ] && [ "$1" = "${GGPO_DEFAULT:-}" ] && { echo ZDXSV_GGPO=default; return; }
  case " ${GGPO_CLIENTS:-$CLIENTS} " in *" $1 "*) [ -n "${GGPO:-}" ] && echo "ZDXSV_GGPO=net=1,lobby=1,port=$((GGPO + $1 - 1))$([ "${GDELAY:-1}" = auto ] || echo ",delay=${GDELAY:-1}")${GMIN:+,mindelay=$GMIN}$([ "$1" = "${BAD_SESSION:-}" ] && echo ,badsession=1)$([ -n "${GGPO_LAT:-}" ] && echo ",advertise=$((7300 + ($1 == 1)))")";; esac  # GDELAY=auto: no delay= (rtt pick, floor GMIN)
}
mkdir -p "$OUT"
cp "$RUN/zdxsv.db" "$OUT/zdxsv.db" || exit 1
pids=()
# zdxsv*: a ZBIN like zdxsv-ggpo.exe outlives `kill` of its bash pid and holds the ports
# (the next run's clients would log in to the old lobby)
killall() { powershell -NoProfile -Command "Get-Process pcsx2*,zdxsv*,dnas*,zproxy* -EA 0 | Stop-Process -Force; Get-CimInstance Win32_Process | ? { \$_.CommandLine -match 'drive\.py' } | % { Stop-Process -Id \$_.ProcessId -Force -EA 0 }" 2>/dev/null; }
killall
cleanup() {
  kill "${pids[@]}" 2>/dev/null
  killall
  for i in 1 2 3 4; do
    cp "$RUN/p$i/PCSX2/logs/emulog.txt" "$OUT/emulog-p$i.txt" 2>/dev/null
    [ -f "$OUT/live-p$i.ps2" ] && cp -p "$OUT/live-p$i.ps2" "$RUN/p$i/PCSX2/memcards/Mcd001.ps2"
  done
}
trap 'cleanup; rig_release' EXIT
ok=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; ok=1; fi; }

# stack.sh runs in the background: its binary check fails here, before any client
ZDXSV=$ZDXSV bash "$here/zbincheck.sh" "${ZBIN:-$ZDXSV/bin/zdxsv.exe}" || exit 1
# -v=2: every lobby frame (entry check below, lobby_trace.py)
[ -n "${RELAY:-}" ] && export ZDXSV_LOBBY_RELAY_ADDR=:8203
[ -n "${REPLAY_UP:-}" ] && export ZDXSV_LOBBY_REPLAY_ADDR=:8204 ZDXSV_LOBBY_REPLAY_DIR="$OUT/replays-up"
RELAY_PATH=${RELAY_PATH:-direct}
LOBBY_ARGS=-v=2 RUN="$OUT" bash "$here/stack.sh" "$IP" > "$OUT/stack.out" 2>&1 & pids+=($!)
if [ -n "${GGPO_LAT:-}" ]; then
  [ "$CLIENTS" = "1 3" ] && [ -n "${GGPO:-}" ] && [ "${GDELAY:-}" = auto ] || { echo "FAIL GGPO_LAT needs CLIENTS=\"1 3\" GGPO GDELAY=auto"; exit 1; }
  # p1 sends to 7300 (p3's announced port) -> from 7301 to p3; p3 to 7301 (p1's) -> from 7300 to p1
  "$PY" -u "$TOOLS/udprelay.py" --a 7300 --b 7301 --p1 $GGPO --p2 $((GGPO + 2)) --delay "$GGPO_LAT" --seconds 3600 --idle 900 \
    --every 10 > "$OUT/udprelay.txt" 2>&1 & pids+=($!)
fi
sleep 6
# lobby RPC on 127.0.0.1 = LAN test: the lobby matches the proxy by -userid, not by address
for i in $ZP; do
  "$ZPROXY" -updatecheck=false -upnp=false -verbose -userid="${USERS[i-1]}" -tcpport=$((8250 + i)) -udpport=$((8250 + i)) \
    -lobbyrpcaddr=127.0.0.1:8201 > "$OUT/zproxy-p$i.log" 2>&1 & pids+=($!)
done
rm -f "$RUN"/p[1-4]/PCSX2/logs/emulog.txt
for i in $CLIENTS; do
  cp -p "$RUN/p$i/PCSX2/memcards/Mcd001.ps2" "$OUT/live-p$i.ps2"
  l=$(env $(cenv $i) powershell -NoProfile -Command "& '$here/launch.ps1' -N $i -Speed ${SPEED:-1} -Memcard $RUN/Mcd001-${CARDS[i-1]}.ps2 $(case " ${SHOW:-} " in *" $i "*) ;; *) echo -Headless;; esac)" 2>&1)
  echo "$l" | tail -1
  # a refused launch (probelint) ran 7 min of blind routes before the entry check
  echo "$l" | grep -q "pid .* slot" || { echo "FAIL launch p$i"; exit 1; }
  # top starts from a fresh boot (f1500): no other client's route in between
  $DRIVE top $i 2>&1 | grep -v memgate | tail -1
  $DRIVE login $i 2>&1 | grep -v memgate | tail -1
  echo "client $i: login results $(grep -c '== BattleResult ==' "$OUT/lobby.log")"
done
grep -a "Register zproxy" "$OUT/lobby.log" | tail -$nz | cut -c1-160
# entries close together: with one client per ~3 min (entry right after its login)
# each saw ジャブロー2 "1人". One at a time: `entry 1 2` in parallel dropped p2's Down
# (-> オペレーションリスト). Each must reach 0x640E (side).
k=0
for i in $CLIENTS; do
  k=$((k + 1))
  speed_check $i || exit 1  # routes hold keys in wall ms; a faster run than SPEED is no SPEED result
  if [ "$ENTRY" = entry2p ]; then entry2p_retry $i || { echo "FAIL client $i entry (no Lobby 02)"; exit 1; }
  else $DRIVE $ENTRY $i 2>&1 | grep -v memgate | tail -1; fi
  for t in $(seq 10); do [ "$(grep -c 'C->S \[Q\] ID:0x640E' "$OUT/lobby.log")" -ge "$k" ] && break; sleep 2; done
  [ "$(grep -c 'C->S \[Q\] ID:0x640E' "$OUT/lobby.log")" -ge "$k" ] || { echo "FAIL client $i entry (no 0x640E)"; exit 1; }
done
for t in $(seq 30); do [ "$(grep -c 'join udp peer\|join success' "$OUT/battle.log")" -ge "$nc" ] && break; sleep 4; done
grep "zproxy user" "$OUT/lobby.log" | cut -c1-160
# zproxy before inada-s/zdxsv#50 ignored -upnp=false and mapped its UDP port on the router (24 h lease)
check "zproxy no UPnP" "! cat '$OUT'/zproxy-p*.log 2>/dev/null | grep -q 'Router(s) found\|UPnP'"
for i in $ZP; do
  u=${USERS[i-1]}; zl="$OUT/zproxy-p$i.log"
  check "p$i zproxy registered + in battle" "grep -q 'Register zproxy $u' '$OUT/lobby.log' && grep -q 'zproxy user $u' '$OUT/lobby.log'"
  check "p$i PS2 -> zproxy -> UDP" "grep -q 'PS2との接続に成功しました' '$zl' && grep -q 'UDP通信を開始します' '$zl' && ! grep -q '対戦サーバとの接続に失敗' '$zl'"
done
check "join udp peer x$nz" "[ \$(grep -c 'join udp peer' '$OUT/battle.log') -eq $nz ]"
check "join tcp x$((nc - nz))" "[ \$(grep -c 'join success' '$OUT/battle.log') -eq $((nc - nz)) ]"
[ -n "${EMU:-}" ] && check "platform info sent x$nc" "[ \$(cat '$RUN'/p[1-4]/PCSX2/logs/emulog.txt | grep -a -c 'sent platform info') -ge $nc ]"
[ -z "${EMU:-}" ] && check "platform info off x4, no bridge" "[ \$(cat '$RUN'/p[1-4]/PCSX2/logs/emulog.txt | grep -a -c 'platform info off') -ge 4 ] && ! cat '$RUN'/p[1-4]/PCSX2/logs/emulog.txt | grep -a -q 'bridge'"
[ "$(grep -c 'join udp peer\|join success' "$OUT/battle.log")" -ge 1 ] || { echo "FAIL no battle"; exit 1; }
# a peer missing from the battle: 4 results never come
[ $ok -eq 0 ] || exit 1
grep -h "P2P Mode Enabled" "$OUT"/zproxy-p*.log 2>/dev/null | cut -c1-120
# per result: user, total_frame, kills, deaths, win, lose; the lobby also logs a zero
# BattleResult at every login (empty battle_code): only results with a battle_code count. A 1v1 that
# times out (mash, no kill) reports battle_count/total_frame 0 (both sides, same code)
# battle code of the first result (results.txt)
report_code() { grep -o -m1 'battle_code:[0-9]*' "$OUT/results.txt" | cut -d: -f2; }
results() {
  awk '/== BattleResult ==/{n++; b=1; next} /==================/{b=0} b&&/\] ID:/{id[n]=$NF} b&&/"battle_code": "[0-9]/{gsub(/[",]/,""); c[n]=$NF} b&&/"(total_frame|kill_count|death_count|win_count|lose_count)"/{gsub(/[",]/,""); v[n]=v[n]" "$(NF-1)$NF} END{for(i=1;i<=n;i++) if (c[i] != "") print "result", id[i], "battle_code:" c[i] v[i]}' "$OUT/lobby.log" > "$OUT/results.txt"
}
gc=${GGPO_CLIENTS:-$CLIENTS}; ngc=$(echo $gc | wc -w)
# a cut: BAD_SESSION, or a client without GGPO (not in GGPO_CLIENTS)
CUT=; [ -n "${BAD_SESSION:-}" ] && CUT=ping; [ -n "${GGPO:-}" ] && [ "$ngc" -lt "$nc" ] && CUT=noport
# vsyncs from the cut to its end (the game closed the battle sock), "?" if a line is missing
cutlen() {
  local a b
  a=$(grep -a -o -m1 'ZdxsvGgpo: lobby battle connection cut: .*vsync [0-9]*)' "$1" | grep -o 'vsync [0-9]*' | cut -d' ' -f2)
  b=$(grep -a -o -m1 'connection cut ended at vsync [0-9]*' "$1" | awk '{print $NF}')
  [ -n "$a" ] && [ -n "$b" ] && echo $((b - a)) || echo "?"
}
if [ -n "$CUT" ]; then
  t0=$SECONDS
  while [ $((SECONDS - t0)) -lt "${MAXS:-300}" ]; do
    sleep 15
    e=0; for i in $gc; do grep -a -q 'ZdxsvGgpo: lobby battle connection cut ended' "$RUN/p$i/PCSX2/logs/emulog.txt" && e=$((e + 1)); done
    c=$(grep -c 'A new tcp connection open' "$OUT/lobby.log")
    echo "$((SECONDS - t0)) s: cut ended $e/$ngc, lobby tcp conns $c/$((2 * nc))"
    [ "$e" -ge "$ngc" ] && [ "$c" -ge $((2 * nc)) ] && break
  done
  sleep 20; results; cat "$OUT/results.txt"
  grep -a -h "ZdxsvGgpo: \(lobby\|badsession\|net player\)\|zdxsv: ping test" "$RUN"/p[1-4]/PCSX2/logs/emulog.txt 2>/dev/null | cut -c1-200
  for i in $gc; do
    f="$RUN/p$i/PCSX2/logs/emulog.txt"
    if [ "$CUT" = ping ]; then
      check "p$i connection cut (unanswered ping test), no ggpo session" "grep -a -q 'ZdxsvGgpo: lobby battle connection cut: [0-9] of $((nc - 1)) peers answered the ping test' '$f' && ! grep -a -q 'ZdxsvGgpo: net player' '$f'"
    else
      check "p$i connection cut (a peer has no GGPO port), no ggpo session" "grep -a -q 'ZdxsvGgpo: lobby battle connection cut: a peer has no GGPO address' '$f' && ! grep -a -q 'ZdxsvGgpo: net player' '$f'"
    fi
    check "p$i game gave up the battle connection (cut ended)" "grep -a -q 'ZdxsvGgpo: lobby battle connection cut ended' '$f'"
    echo "p$i cut ended after $(cutlen "$f") vsyncs"
    [ -n "${CUTMAX:-}" ] && check "p$i cut ended within $CUTMAX vsyncs" "[ '$(cutlen "$f")' != '?' ] && [ $(cutlen "$f" | tr -d '?') -le $CUTMAX ]"
  done
  [ "$CUT" = ping ] && check "p$BAD_SESSION used the bad session" "grep -a -q 'badsession=1: ping test session' '$RUN/p$BAD_SESSION/PCSX2/logs/emulog.txt'"
  check "every client back in the lobby (tcp conns x$((2 * nc)))" "[ \$(grep -c 'A new tcp connection open' '$OUT/lobby.log') -ge $((2 * nc)) ]"
  check "no battle fought (no result with frames)" "! grep -q 'total_frame:[1-9]' '$OUT/results.txt'"
  if [ -n "${REPORT:-}" ]; then
    grep -a 'p2p matching report:' "$OUT/lobby.log" | cut -c1-300
    check "no match report without a battle code" "! grep -a -q 'p2p matching report: battle_code=\"\"' '$OUT/lobby.log'"
    check "match report x$ngc: battle code $(report_code), result cut, cut ended" "[ \$(grep -a 'p2p matching report: battle_code=\"$(report_code)\"' '$OUT/lobby.log' | grep 'result=\"cut\"' | grep -c 'cut_sends=') -eq $ngc ]"
  fi
  exit $ok
fi
B=${BATTLES:-1}
for b in $(seq "$B"); do
  if [ "$b" -gt 1 ]; then
    # the results come at the post-battle login, in 作戦後部屋 (待機 / EXIT): EXIT -> 戦場選択 (map up
    # < 12 s later, s728 shots), then Lobby 02 from the map
    for i in $CLIENTS; do
      $DRIVE seq "$i" "Down,w500,C,w12000" exit$b 2>&1 | grep -v memgate | tail -1
      entry2p_retry "$i" remap2p || { echo "FAIL client $i entry $b (no Lobby 02)"; exit 1; }
    done
    for t in $(seq 10); do [ "$(grep -c 'C->S \[Q\] ID:0x640E' "$OUT/lobby.log")" -ge $((nc * b)) ] && break; sleep 2; done
    [ "$(grep -c 'C->S \[Q\] ID:0x640E' "$OUT/lobby.log")" -ge $((nc * b)) ] || { echo "FAIL battle $b entry (no 0x640E)"; exit 1; }
  fi
  $DRIVE mash $CLIENTS 2>&1 | grep -v memgate | tail -1
  t0=$SECONDS
  while [ $((SECONDS - t0)) -lt "${MAXS:-2100}" ]; do
    sleep 30
    results; r=$(grep -c '^result' "$OUT/results.txt")
    echo "$((SECONDS - t0)) s: results $r/$((nc * b))"
    [ "$r" -ge $((nc * b)) ] && break
  done
done
cat "$OUT/results.txt"
grep -h "Error\|失敗" "$OUT"/zproxy-p*.log 2>/dev/null | tail -4 | cut -c1-160
check "BattleResult x$((nc * B))" "[ \$(grep -c '^result' '$OUT/results.txt') -ge $((nc * B)) ]"
# per battle code: $nc results, one total_frame; $B codes
check "results agree ($B battle codes)" "awk '{for(i=3;i<=NF;i++){split(\$i,a,\":\"); v[a[1]]=a[2]} c=v[\"battle_code\"]; n[c]++; f[c \" \" v[\"total_frame\"]]=1; k+=v[\"kill_count\"]; d+=v[\"death_count\"]} END{m=0; for(x in n){m++; if(n[x]<$nc)bad=1} u=0; for(x in f)u++; exit !(m==$B && u==$B && !bad && k==d)}' '$OUT/results.txt'"
if [ -n "${EMU:-}" ] && [ -n "${GGPO:-}" ]; then
  grep -a -h "ggpo peers\|ZdxsvGgpo: \(lobby\|net player\)" "$OUT"/emulog-p[1-4].txt "$RUN"/p[1-4]/PCSX2/logs/emulog.txt 2>/dev/null | cut -c1-160 | sort -u
  for i in ${GGPO_CLIENTS:-$CLIENTS}; do
    f="$RUN/p$i/PCSX2/logs/emulog.txt"
    check "p$i ggpo session, $((nc - 1)) lobby peers x$B" "grep -a -q 'ZdxsvGgpo: net player' '$f' && [ \$(grep -a -c 'ZdxsvGgpo: lobby peer position' '$f') -eq $(((nc - 1) * B)) ]"
  done
  [ -n "${GGPO_DEFAULT:-}" ] && check "p$GGPO_DEFAULT GGPO from the setting" "grep -a -q \"ZdxsvGgpo: options 'net=1,lobby=1' (serial SLPS-25419, setting 1)\" '$RUN/p$GGPO_DEFAULT/PCSX2/logs/emulog.txt'"
  # every GGPO client: $B GGPO battle ends; per battle the same end frame; 0 mismatches
  ends=$(for i in ${GGPO_CLIENTS:-$CLIENTS}; do grep -a -o 'net battle end frames [0-9]* rollback frames [0-9]* loads [0-9]* mismatches [0-9]*' "$RUN/p$i/PCSX2/logs/emulog.txt" | awk -v p=$i '{print "p" p, "battle", NR, "frames", $5, "mismatches", $NF}'; done)
  echo "$ends" | sed 's/^/ggpo end /'
  check "ggpo end frame agrees per battle ($B), 0 mismatches" "echo '$ends' | awk '{n++; if (\$7 != 0) bad=1; if ((\$3 in f) && f[\$3] != \$5) bad=1; f[\$3]=\$5} END{u=0; for(x in f)u++; exit !(n==$(echo ${GGPO_CLIENTS:-$CLIENTS} | wc -w) * $B && u==$B && !bad)}'"
  if [ -n "${REPORT:-}" ]; then
    ng=$(echo ${GGPO_CLIENTS:-$CLIENTS} | wc -w)
    grep -a 'p2p matching report:' "$OUT/lobby.log" | cut -c1-300
    check "no match report without a battle code" "! grep -a -q 'p2p matching report: battle_code=\"\"' '$OUT/lobby.log'"
    check "match report x$ng: battle code $(report_code), result ggpo, net battle end, 0 mismatches" "[ \$(grep -a 'p2p matching report: battle_code=\"$(report_code)\"' '$OUT/lobby.log' | grep 'result=\"ggpo\"' | grep 'close=\"net battle end\"' | grep -c 'mismatches=\"0\"') -eq $ng ]"
  fi
  if [ -n "${OSD:-}" ]; then
    for i in ${GGPO_CLIENTS:-$CLIENTS}; do
      f="$RUN/p$i/PCSX2/logs/emulog.txt"
      grep -a -o 'ZdxsvGgpo: osd frame .*' "$f" | tail -1 | tr -d '\r' > "$OUT/osd-p$i.txt"
      cut -c1-300 "$OUT/osd-p$i.txt"
      d=$(grep -a -o 'lobby delay [0-9]*' "$f" | tail -1 | awk '{print $3}'); d=${d:-${GDELAY:-1}}
      check "p$i osd: Delay ${d}fr, $((nc - 1)) opponents with user id + name + ping" "grep -q 'Delay ${d}fr |' '$OUT/osd-p$i.txt' && [ \$(grep -o '|[1-4]P [A-Z0-9]\{6\} | [^|]*[^ |] | Ping [0-9]*ms' '$OUT/osd-p$i.txt' | wc -l) -eq $((nc - 1)) ]"
    done
    check "osd names = zdxsv.db names (UTF-8, not garbled)" "'$PY' -I '$here/osdname.py' '$OUT/zdxsv.db' $(for i in ${GGPO_CLIENTS:-$CLIENTS}; do printf "'%s' " "$OUT/osd-p$i.txt"; done)"
  fi
fi
if [ -n "${NAT:-}" ]; then
  for i in ${GGPO_CLIENTS:-$CLIENTS}; do
    f="$RUN/p$i/PCSX2/logs/emulog.txt"
    grep -a 'udp test: nat=' "$f" | tr -d '\r' | cut -c1-200
    check "p$i udp test: exactly one, nat=$NAT" "[ \$(grep -a -c 'udp test: nat=' '$f') -eq 1 ] && grep -a -q 'udp test: nat=$NAT:' '$f'"
  done
fi
if [ -n "${RELAY:-}" ]; then
  check "lobby relay started" "grep -a -q 'Start relay' '$OUT/lobby.log'"
  for i in ${GGPO_CLIENTS:-$CLIENTS}; do
    f="$RUN/p$i/PCSX2/logs/emulog.txt"
    grep -a 'lobby path to\|ZdxsvGgpo: relay server' "$f" | tr -d '\r' | cut -c1-200
    check "p$i relay server registered" "grep -a -q 'ZdxsvGgpo: relay server $IP:8203' '$f'"
    check "p$i paths $RELAY_PATH" "grep -a -q 'ZdxsvGgpo: lobby path to position [0-9]: $RELAY_PATH' '$f' && ! grep -a 'ZdxsvGgpo: lobby path to' '$f' | grep -a -v -q ': $RELAY_PATH'"
  done
fi
if [ -n "${GGPO_LAT:-}" ]; then
  E=$(( (2 * GGPO_LAT + 31) / 32 )); M=${GMIN:-2}; [ -n "${RELAY:-}" ] && [ "$RELAY_PATH" != direct ] && E=0; [ "$E" -lt "$M" ] && E=$M
  for i in $CLIENTS; do
    f="$RUN/p$i/PCSX2/logs/emulog.txt"
    grep -a 'advertise=\|ping test\|lobby delay' "$f" | tr -d '\r' | cut -c1-200 | tail -4
    check "p$i announced 127.0.0.1:$((7300 + (i == 1))) only" "grep -a -q 'zdxsv advertise=$((7300 + (i == 1))):' '$f'"
    check "p$i lobby delay $E" "grep -a -q 'ZdxsvGgpo: lobby delay $E:' '$f'"
  done
  echo "udprelay: $(grep -a 'relay \(end\|stats\)' "$OUT/udprelay.txt" | tail -1 | tr -d '\r')"
  check "udprelay forwarded (the ping test reached it)" "grep -a 'relay \(end\|stats\)' '$OUT/udprelay.txt' | tail -1 | grep -a -q 'packets in [1-9]'"
fi
if [ -n "${REPLAY_UP:-}" ]; then
  check "lobby replay server started" "grep -a -q 'Start replay server' '$OUT/lobby.log'"
  up=""; for i in ${GGPO_CLIENTS:-$CLIENTS}; do [ "$i" != "${UPLOAD_OFF:-}" ] && up="$up $i"; done
  # the upload runs on a thread after the replay is written: up to 60 s
  for t in $(seq 60); do
    n=0; for i in $up; do n=$((n + $(grep -a -c 'ZdxsvGgpo: replay upload' "$RUN/p$i/PCSX2/logs/emulog.txt"))); done
    [ "$n" -ge $(($(echo $up | wc -w) * B)) ] && break; sleep 1
  done
  for code in $(grep -o 'battle_code:[0-9]*' "$OUT/results.txt" | cut -d: -f2 | sort -u); do
    oks=""; there=0
    for i in ${GGPO_CLIENTS:-$CLIENTS}; do
      f="$RUN/p$i/PCSX2/logs/emulog.txt"
      grep -a "ZdxsvGgpo: replay upload .*battle_code=$code&" "$f" | tr -d '\r' | cut -c1-200
      grep -a -q "replay upload .*battle_code=$code&.*: ok" "$f" && oks="$oks $i"
      there=$((there + $(grep -a -c "replay upload .*battle_code=$code&.*: already there" "$f")))
      [ "$i" = "${UPLOAD_OFF:-}" ] && check "p$i (setting off) no upload of $code" "! grep -a -q 'replay upload .*battle_code=$code&' '$f'"
    done
    check "$code: one upload ok ($oks), $(($(echo $up | wc -w) - 1)) already there" "[ $(echo $oks | wc -w) -eq 1 ] && [ $there -eq $(($(echo $up | wc -w) - 1)) ]"
    check "$code: stored file = p${oks# }'s own replay" "cmp -s '$OUT/replays-up/$code.zdxr' '$RUN/p${oks# }/PCSX2/replays/$code.zdxr'"
  done
fi
exit $ok
