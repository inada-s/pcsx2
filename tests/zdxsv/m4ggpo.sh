#!/bin/bash
# m4.sh with GGPO started from the lobby's battle info (pcsx2 ZDXSV_GGPO net=1,lobby=1): no static
# host=/relay=, the peers come from fake_lobby.py --ggpo (ggpo_<user>=PORT+k-1 for the k-th client).
# Env: OUT (required), PORT (7101), GDELAY (GGPO frame delay, 1; auto = no delay=, picked from the ping test rtt),
#   GMIN (mindelay=), GGPO_LAT (one-way ms on every GGPO path: fake_lobby.py --ggpo-delay),
#   P2P (v4 default, v6, dual: fake_lobby.py --p2p; peers' addresses IPv4, this box's IPv6, both),
#   GGPO_CLIENTS ("1 2 3 4": clients with lobby=1 and a ggpo_ line; fewer = control, expect every client
#   on the battle server); other env goes to m4.sh.
# Checks (besides m4.sh's): GGPO clients: `lobby peer position` x(N-1) and `net player` each;
#   GDELAY=auto: `ping test` and `lobby delay E`, E = max(GMIN or 2, ceil(2*GGPO_LAT/32));
#   P2P=v6: every peer at an IPv6 address; P2P=dual + GDELAY=auto: every peer's IPv6 candidate answered
#   the ping test; P2P!=v4: every client found its IPv6 (`udp: ... IPv6 [`);
#   control: `stays on the battle server` on each lobby=1 client, no `net player` anywhere.
#   BAD_SESSION=K (GDELAY=auto; fake_lobby.py --bad-session): client K's ping test gets no answer and drops
#   packets; every other client gets rtt -1 from K only, and drops packets; then every client cuts the battle
#   connection (no GGPO, no TCP fallback), the game gives up on it and
#   reconnects to the lobby (m4.sh NORESULT: no BATTLE RESULT, conn from x8; MAXS default 360).
#   RELAY=1 (GDELAY=auto, with GGPO_LAT): a `zdxsv relay` on 127.0.0.1:8203 (m4.sh ZRELAY) and fake_lobby.py --relay;
#   every path `relay 0`, peers at the relay, lobby delay max(GMIN or 2), relay.log forwarded > 0.
#   GDELAY=auto without RELAY: every path `direct`.
set -u
D=$(cd "$(dirname "$0")" && pwd)
. "$D/env.sh"
PORT=${PORT:-7101}
GC=${GGPO_CLIENTS:-1 2 3 4}
GDELAY=${GDELAY:-1}
for i in $GC; do
  export PCSX2_ENV_P$i="ZDXSV_GGPO=net=1,lobby=1,port=$((PORT + i - 1))$([ "$GDELAY" = auto ] || echo ",delay=$GDELAY")${GMIN:+,mindelay=$GMIN}"
done
P2P=${P2P:-v4}
BAD=${BAD_SESSION:-}
[ -n "$BAD" ] && export NORESULT=1 MAXS=${MAXS:-360}
RELAY=${RELAY:-}
RA=127.0.0.1:8203 RS=1234:abcd
[ -n "$RELAY" ] && export ZRELAY=$RA ZRELAY_SESSION=$RS
FAKE_ARGS="--ggpo $PORT --ggpo-clients $(echo $GC | tr -d ' ')${GGPO_LAT:+ --ggpo-delay $GGPO_LAT} --p2p $P2P${BAD:+ --bad-session $BAD}${RELAY:+ --relay $RA --relay-session $RS}" bash "$D/m4.sh"
rc=$?
n=$(echo $GC | wc -w)
ok() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; rc=1; fi; }
E=$(( (2 * ${GGPO_LAT:-0} + 31) / 32 )); M=${GMIN:-2}; [ -n "$RELAY" ] && E=0; [ "$E" -lt "$M" ] && E=$M
for i in $GC; do
  f="$RUN/p$i/PCSX2/logs/emulog.txt"
  grep -a "ZdxsvGgpo: \(lobby\|net player\|net armed\)\|zdxsv: ping test" "$f" | cut -c1-300 | sed "s/^/p$i /"
  if [ "$n" -eq 4 ]; then
    [ -z "$BAD" ] && ok "p$i lobby peers x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby peer position' '$f') -eq 3 ]"
    if [ -n "$BAD" ]; then
      ok "p$i connection cut (unanswered ping test), no ggpo session" "grep -a -q 'ZdxsvGgpo: lobby battle connection cut: [0-9] of 3 peers answered the ping test' '$f' && ! grep -a -q 'ZdxsvGgpo: net player' '$f'"
      ok "p$i game gave up the battle connection (cut ended)" "grep -a -q 'ZdxsvGgpo: lobby battle connection cut ended' '$f'"
    else
      ok "p$i ggpo session" "grep -a -q 'ZdxsvGgpo: net player' '$f'"
    fi
    if [ -z "$BAD" ]; then
      [ "$GDELAY" = auto ] && ok "p$i ping test 3 peers" "grep -a 'zdxsv: ping test' '$f' | grep -a -q 'rtt [0-9].*rtt [0-9].*rtt [0-9]'"
    elif [ "$i" = "$BAD" ]; then
      ok "p$i (bad session) no peer answered, drops" "grep -a 'zdxsv: ping test' '$f' | grep -a -v 'rtt [0-9]' | grep -a -q ' [1-9][0-9]* dropped'"
    else
      ok "p$i 2 peers answered, p$BAD rtt -1, drops" "grep -a 'zdxsv: ping test' '$f' | grep -a 'rtt [0-9].*rtt [0-9]' | grep -a 'rtt -1' | grep -a -q ' [1-9][0-9]* dropped'"
    fi
    [ "$GDELAY" = auto ] && [ -z "$BAD" ] && ok "p$i lobby delay $E" "grep -a -q 'ZdxsvGgpo: lobby delay $E:' '$f'"
    [ "$P2P" = v6 ] && ok "p$i peers at IPv6 x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby peer position [0-9] at \[' '$f') -eq 3 ]"
    [ "$P2P" = dual ] && [ "$GDELAY" = auto ] && ok "p$i IPv6 candidates answered x3" "[ \$(grep -a 'zdxsv: ping test' '$f' | grep -a -o '\]:[0-9]* rtt [0-9]' | wc -l) -eq 3 ]"
    [ "$P2P" != v4 ] && ok "p$i own IPv6" "grep -a -q 'zdxsv: udp: port.*IPv6 \[' '$f'"
    if [ -n "$RELAY" ] && [ -z "$BAD" ]; then
      ok "p$i relay server registered" "grep -a -q 'ZdxsvGgpo: relay server $RA' '$f'"
      ok "p$i paths relay 0 x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby path to position [0-9]: relay 0' '$f') -eq 3 ]"
      ok "p$i peers via relay x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby peer position [0-9] at $RA (relay)' '$f') -eq 3 ]"
    elif [ "$n" -eq 4 ] && [ -z "$BAD" ] && [ "$GDELAY" = auto ]; then
      ok "p$i paths direct x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby path to position [0-9]: direct' '$f') -eq 3 ]"
    fi
  else
    ok "p$i stays on the battle server" "grep -a -q 'ZdxsvGgpo: lobby battle stays on the battle server' '$f'"
  fi
done
# no UDP bridge; STUN only on lobby=1 clients
ok "no bridge" "! cat $RUN/p[1-4]/PCSX2/logs/emulog.txt | grep -a -q 'zdxsv: bridge\|UDP bridge'"
ok "stun x$n" "[ \$(cat $RUN/p[1-4]/PCSX2/logs/emulog.txt | grep -a -c 'zdxsv: udp: port') -eq $n ]"
[ -n "$RELAY" ] && [ -z "$BAD" ] && ok "relay forwarded" "grep -a 'relay test session' '$OUT/relay.log' | tail -1 | grep -a -q 'forwarded [1-9]'"
[ "$n" -lt 4 ] && ok "no ggpo session" "! cat $RUN/p[1-4]/PCSX2/logs/emulog.txt | grep -a -q 'ZdxsvGgpo: net player'"
exit $rc
