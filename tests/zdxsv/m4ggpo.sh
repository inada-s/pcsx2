#!/bin/bash
# m4.sh with GGPO started from the lobby's battle info (pcsx2 ZDXSV_GGPO net=1,lobby=1): no static
# host=/relay=, the peers come from fake_lobby.py --ggpo (ggpo_<user>=PORT+k-1 for the k-th client).
# Env: OUT (required), PORT (7101), GDELAY (GGPO frame delay, 1; auto = no delay=, picked from the ping test rtt),
#   GMIN (mindelay=), GGPO_LAT (one-way ms on every GGPO path: fake_lobby.py --ggpo-delay),
#   P2P (v4 default, v6, dual: fake_lobby.py --p2p; peers' addresses IPv4, this box's IPv6, both),
#   GGPO_CLIENTS ("1 2 3 4": clients with lobby=1 and a ggpo_ line; fewer = control, expect every client
#   cutting the battle connection: a peer has no GGPO address); other env goes to m4.sh.
# Checks (besides m4.sh's): GGPO clients: `lobby peer position` x(N-1) and `net player` each;
#   GDELAY=auto: `ping test` and `lobby delay E`, E = max(GMIN or 2, ceil(2*GGPO_LAT/32));
#   P2P=v6: every peer at an IPv6 address; P2P=dual + GDELAY=auto: every peer's IPv6 candidate answered
#   the ping test; P2P!=v4: every client found its IPv6 (`udp: ... IPv6 [`);
#   control: `connection cut: a peer has no GGPO address` on each lobby=1 client, no `net player` anywhere.
#   BAD_SESSION=K (GDELAY=auto; fake_lobby.py --bad-session): client K's ping test gets no answer and drops
#   packets; every other client gets rtt -1 from K only, and drops packets; then every client cuts the battle
#   connection (no GGPO, no TCP fallback) with the opponent-did-not-answer OSD, the game gives up on it and
#   reconnects to the lobby (m4.sh NORESULT: no BATTLE RESULT, conn from x8; MAXS default 360).
#   RELAY=1 (GDELAY=auto, with GGPO_LAT): a `zdxsv relay` on 127.0.0.1:8203 (m4.sh ZRELAY) and fake_lobby.py --relay;
#   every path `relay 0`, peers at the relay, lobby delay max(GMIN or 2), relay.log forwarded > 0.
#   RELAY=1 RELAY_ALT=1: the relay advertised at another address than it answers from; the same checks
#   (control: a build that requires the advertised source: every path `direct`).
#   RELAY=4: relay_0..3 offered, nothing at 0 and 1, `zdxsv relay`s at 2 and 3: every path `relay 2`, the other
#   checks as RELAY=1.
#   GDELAY=auto without RELAY: every path `direct`.
#   NOADDR=K (GDELAY=auto; fake_lobby.py --no-addr): the others get no address of client K; the ping test
#   finds it at the source of its pings, so the checks are the plain ones (control: a build without that: cut).
#   BLOCK=K-L (GDELAY=auto, with GGPO_LAT; fake_lobby.py --ggpo-block): clients K and L get no path to each
#   other, so each reaches the other through a third peer: K and L `path ... peer` x1 and `(relay)` x1, every
#   other path `direct`, K and L take the lobby delay from the peer path rtt (control: the plain checks on those logs fail on K and L).
#   UNREACH=K (GDELAY=auto, with GGPO_LAT; fake_lobby.py --ggpo-block K-x for every other client x): nobody has a
#   path to client K, not even through a peer: K gets no answer, every other client gets rtt -1 from K only;
#   then the BAD_SESSION checks (every client cuts, the game gives up and reconnects to the lobby).
set -u
D=$(cd "$(dirname "$0")" && pwd)
. "$D/env.sh"
PORT=${PORT:-7101}
GC=${GGPO_CLIENTS:-1 2 3 4}
GDELAY=${GDELAY:-1}
# RELAY checks assume the ping test (GDELAY=auto); a fixed delay skips it
[ -n "${RELAY:-}" ] && [ "$GDELAY" != auto ] && { echo "m4ggpo: RELAY needs GDELAY=auto (got $GDELAY)"; exit 2; }
for i in $GC; do
  eval "prev=\${PCSX2_ENV_P$i:-}"  # keep a caller's per-client env (m4lat.sh)
  export PCSX2_ENV_P$i="${prev:+$prev }ZDXSV_GGPO=net=1,lobby=1,port=$((PORT + i - 1))$([ "$GDELAY" = auto ] || echo ",delay=$GDELAY")${GMIN:+,mindelay=$GMIN}"
done
P2P=${P2P:-v4}
BAD=${BAD_SESSION:-}
NOADDR=${NOADDR:-}
[ -n "$NOADDR" ] && [ "$GDELAY" != auto ] && { echo "m4ggpo: NOADDR needs GDELAY=auto (got $GDELAY)"; exit 2; }
BLOCK=${BLOCK:-}
[ -n "$BLOCK" ] && { [ "$GDELAY" != auto ] || [ -z "${GGPO_LAT:-}" ]; } && { echo "m4ggpo: BLOCK needs GDELAY=auto and GGPO_LAT"; exit 2; }
UNREACH=${UNREACH:-}
[ -n "$UNREACH" ] && { [ "$GDELAY" != auto ] || [ -z "${GGPO_LAT:-}" ]; } && { echo "m4ggpo: UNREACH needs GDELAY=auto and GGPO_LAT"; exit 2; }
GB=$BLOCK
[ -n "$UNREACH" ] && GB=$(for i in $GC; do [ "$i" = "$UNREACH" ] || printf '%s-%s,' "$UNREACH" "$i"; done | sed 's/,$//')
CUT=${BAD:-$UNREACH}
# a bad session's packets arrive and are dropped; an unreachable peer's never arrive
CUTK="bad session" DL=drops DQ=' [1-9][0-9]* dropped'
[ -n "$UNREACH" ] && CUTK=unreachable DL="0 dropped" DQ=' 0 dropped'
[ -n "$CUT" ] && export NORESULT=1 MAXS=${MAXS:-360}
RELAY=${RELAY:-}
RA=127.0.0.1:8203 RS=1234:abcd
[ -n "$RELAY" ] && export ZRELAY=$RA ZRELAY_SESSION=$RS
# RELAY_ALT=1: the relay listens on every address and is advertised at 127.0.0.2, so it answers from 127.0.0.1
[ -n "$RELAY" ] && [ -n "${RELAY_ALT:-}" ] && RA=127.0.0.2:8203 ZRELAY=:8203
# RELAY=4: relay_0..3 offered, nothing listens at 0 and 1, 2 (RA) and 3 run: every path takes relay 2
RL=$RA RI=0
[ "$RELAY" = 4 ] && RL=127.0.0.1:8261,127.0.0.1:8262,$RA,127.0.0.1:8263 RI=2 ZRELAY=$RA,127.0.0.1:8263
FAKE_ARGS="--ggpo $PORT --ggpo-clients $(echo $GC | tr -d ' ')${GGPO_LAT:+ --ggpo-delay $GGPO_LAT} --p2p $P2P${BAD:+ --bad-session $BAD}${NOADDR:+ --no-addr $NOADDR}${GB:+ --ggpo-block $GB}${RELAY:+ --relay $RL --relay-session $RS}" bash "$D/m4.sh"
rc=$?
n=$(echo $GC | wc -w)
ok() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; rc=1; fi; }
E=$(( (2 * ${GGPO_LAT:-0} + 31) / 32 )); M=${GMIN:-2}; [ -n "$RELAY" ] && E=0; [ "$E" -lt "$M" ] && E=$M
for i in $GC; do
  f="$RUN/p$i/PCSX2/logs/emulog.txt"
  blk=; [ -n "$BLOCK" ] && case "-$BLOCK-" in *-$i-*) blk=1;; esac
  grep -a "ZdxsvGgpo: \(lobby\|net player\|net armed\)\|zdxsv: ping test" "$f" | cut -c1-300 | sed "s/^/p$i /"
  if [ "$n" -eq 4 ]; then
    [ -z "$CUT" ] && ok "p$i lobby peers x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby peer position' '$f') -eq 3 ]"
    if [ -n "$CUT" ]; then
      ok "p$i connection cut (unanswered ping test), no ggpo session" "grep -a -q 'ZdxsvGgpo: lobby battle connection cut: [0-9] of 3 peers answered the ping test' '$f' && ! grep -a -q 'ZdxsvGgpo: net player' '$f'"
      ok "p$i game gave up the battle connection (cut ended)" "grep -a -q 'ZdxsvGgpo: lobby battle connection cut ended' '$f'"
      ok "p$i cut OSD (opponent did not answer)" "grep -a -q -e 'OSD \[ZdxsvBattleCut\]: No battle: an opponent did not answer' -e 'OSD \[ZdxsvBattleCut\]: 対戦できません: 対戦相手から応答がありません' '$f'"
    else
      ok "p$i ggpo session" "grep -a -q 'ZdxsvGgpo: net player' '$f'"
    fi
    if [ -z "$CUT" ]; then
      [ "$GDELAY" = auto ] && ok "p$i ping test 3 peers" "grep -a 'zdxsv: ping test' '$f' | grep -a -q 'rtt [0-9].*rtt [0-9].*rtt [0-9]'"
    elif [ "$i" = "$CUT" ]; then
      ok "p$i ($CUTK) no peer answered, $DL" "grep -a 'zdxsv: ping test' '$f' | grep -a -v 'rtt [0-9][0-9]* ms' | grep -a -q '$DQ'"
    else
      ok "p$i 2 peers answered, p$CUT rtt -1, $DL" "grep -a 'zdxsv: ping test' '$f' | grep -a 'rtt [0-9].*rtt [0-9]' | grep -a 'rtt -1' | grep -a -q '$DQ'"
    fi
    [ "$GDELAY" = auto ] && [ -z "$CUT" ] && [ -z "$blk" ] && ok "p$i lobby delay $E" "grep -a -q 'ZdxsvGgpo: lobby delay $E:' '$f'"
    [ "$P2P" = v6 ] && ok "p$i peers at IPv6 x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby peer position [0-9] at \[' '$f') -eq 3 ]"
    [ "$P2P" = dual ] && [ "$GDELAY" = auto ] && ok "p$i IPv6 candidates answered x3" "[ \$(grep -a 'zdxsv: ping test' '$f' | grep -a -o '\]:[0-9]* rtt [0-9]' | wc -l) -eq 3 ]"
    [ "$P2P" != v4 ] && ok "p$i own IPv6" "grep -a -q 'zdxsv: udp: port.*IPv6 \[' '$f'"
    if [ -n "$RELAY" ] && [ -z "$CUT" ]; then
      for a in ${RL//,/ }; do ok "p$i relay server $a registered" "grep -a -q 'ZdxsvGgpo: relay server $a' '$f'"; done
      [ -n "${RELAY_ALT:-}" ] && ok "p$i relay answers from 127.0.0.1" "grep -a 'zdxsv: ping test' '$f' | grep -a -q 'pongs) answers from 127.0.0.1:8203'"
      ok "p$i paths relay $RI x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby path to position [0-9]: relay $RI' '$f') -eq 3 ]"
      ok "p$i peers via relay x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby peer position [0-9] at $RA (relay)' '$f') -eq 3 ]"
    elif [ -n "$blk" ]; then
      ok "p$i paths direct x2, peer x1" "[ \$(grep -a -c 'ZdxsvGgpo: lobby path to position [0-9]: direct' '$f') -eq 2 ] && [ \$(grep -a -c 'ZdxsvGgpo: lobby path to position [0-9]: peer [0-9]' '$f') -eq 1 ]"
      ok "p$i peer via relay x1" "[ \$(grep -a -c 'ZdxsvGgpo: lobby peer position [0-9] at .* (relay)' '$f') -eq 1 ]"
      R=$(grep -a -o 'lobby path to position [0-9]: peer [0-9], rtt [0-9]*' "$f" | grep -o '[0-9]*$')
      ok "p$i lobby delay from the peer path (rtt $R)" "grep -a -q 'ZdxsvGgpo: lobby delay [0-9]*: slowest peer rtt ${R:-x} ms' '$f'"
    elif [ "$n" -eq 4 ] && [ -z "$CUT" ] && [ "$GDELAY" = auto ]; then
      ok "p$i paths direct x3" "[ \$(grep -a -c 'ZdxsvGgpo: lobby path to position [0-9]: direct' '$f') -eq 3 ]"
    fi
  else
    ok "p$i connection cut (a peer has no GGPO port)" "grep -a -q 'ZdxsvGgpo: lobby battle connection cut: a peer has no GGPO address' '$f'"
  fi
done
# no UDP bridge; STUN only on lobby=1 clients
ok "no bridge" "! cat $RUN/p[1-4]/PCSX2/logs/emulog.txt | grep -a -q 'zdxsv: bridge\|UDP bridge'"
ok "stun x$n" "[ \$(cat $RUN/p[1-4]/PCSX2/logs/emulog.txt | grep -a -c 'zdxsv: udp: port') -eq $n ]"
[ -n "$RELAY" ] && [ -z "$CUT" ] && ok "relay forwarded" "grep -a 'relay test session' '$OUT/relay.log' | grep -a -q 'forwarded [1-9]'"
[ "$n" -lt 4 ] && ok "no ggpo session" "! cat $RUN/p[1-4]/PCSX2/logs/emulog.txt | grep -a -q 'ZdxsvGgpo: net player'"
exit $rc
