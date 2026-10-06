#!/bin/bash
# m4.sh with ZDXSV_GGPO net=1 and a udprelay.py between every pair of the 4 GGPO peers.
# Peer i (port PORT+i) reaches peer j at RELAY+8i+j (pcsx2 ZDXSV_GGPO relay=); one relay per pair.
# Env: OUT (required), DELAY (one-way ms, 30), JITTER (ms, 0), LOSS (0..1, 0), GDELAY (GGPO frame delay, 1),
#   PORT (7101), RELAY (7200), GGPO_EXTRA (appended to ZDXSV_GGPO); other env goes to m4.sh.
# Usage: OUT=<dir> DELAY=30 LOSS=0.05 bash tests/zdxsv/m4relay.sh
set -u
D=$(cd "$(dirname "$0")" && pwd)
. "$D/riglock.sh"  # one rig at a time: its relays start before m4.sh
PORT=${PORT:-7101} RELAY=${RELAY:-7200}
mkdir -p "$OUT"
pids=
for i in 0 1 2; do
  for j in $(seq $((i + 1)) 3); do
    "$PY" -u "$TOOLS/udprelay.py" --a $((RELAY + 8 * i + j)) --b $((RELAY + 8 * j + i)) --p1 $((PORT + i)) --p2 $((PORT + j)) \
      --delay "${DELAY:-30}" --jitter "${JITTER:-0}" --loss "${LOSS:-0}" --seed $((10 * i + j)) --seconds 3600 --idle 900 \
      > "$OUT/relay-$i$j.txt" 2>&1 &
    pids="$pids $!"
  done
done
PCSX2_ENV="ZDXSV_GGPO=net=1,players=4,port=$PORT,relay=$RELAY,delay=${GDELAY:-1}${GGPO_EXTRA:-} ${PCSX2_ENV:-}" bash "$D/m4.sh"
rc=$?
kill $pids 2>/dev/null
echo "relays:"
for f in "$OUT"/relay-*.txt; do echo "$(basename "$f" .txt) $(grep 'relay \(end\|stats\)' "$f" | tail -1 | tr -d '\r')"; done
# a relay that never forwarded = peers bypassed it (or never started): not a latency run
if grep -L 'relay \(end\|stats\): packets in [1-9]' "$OUT"/relay-*.txt | grep -q .; then echo "FAIL relay idle"; rc=1; fi
exit $rc
