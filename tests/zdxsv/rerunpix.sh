#!/bin/bash
# Pictures after rollbacks by pixel color: a GGPO synctest with check=2 in the arcade battle and a
# pixel watch (ZDXSV_INPUT_LATENCY count=0, 16 pixels, no presses, nothing written). PASS when every
# vsync since the first shown frame showed a frame, frames after a rollback were shown, and none of
# them repeated the frame before. "ZDXSV_SINGLE_DRAW_LIST=0 ZDXSV_RERUN_TAIL=1" is a FAIL control.
#   OUT=<dir> bash tests/zdxsv/rerunpix.sh ["<env>"]
# Env: FRAMES (synctest frames, default 1200).
. "$(dirname "$0")/env.sh"
OUT=${OUT:?OUT=dir for the log}
FRAMES=${FRAMES:-1200}
rm -rf "$OUT"; mkdir -p "$OUT"
G="80:56;240:56;400:56;560:56;80:168;240:168;400:168;560:168;80:280;240:280;400:280;560:280;80:392;240:392;400:392;560:392"
export ZDXSV_INPUT_LATENCY="count=0,pixel=$G"
[ -n "${1:-}" ] && export $1
powershell -NoProfile -File "$(dirname "$0")/deltatest.ps1" -Var ZDXSV_GGPO \
  -Spec "start=9000,frames=$FRAMES,check=2,mask=fcff" -Route arcade -End $((9100 + FRAMES)) -Tag t -Out "$OUT" |
  grep "synctest start\|MISMATCH"
line=$(grep -a "ZdxsvLatency: pixel watch at vsync" "$OUT/t-emulog.txt" | tail -1)
echo "$line"
echo "$line" | awk '{
  for (i = 1; i <= NF; i++) {
    if ($(i) ~ /^\(/ && $(i+1) == "since") since = substr($i, 2)
    if ($i == "rollback") { after = $(i+1); au = $(i+3) + 0 }
    if ($i == "other") { other = $(i+1) + 0 }
  }
  if (since == "") { print "FAIL: no pixel watch line"; exit 1 }
  gap = since - after - other
  if (gap > 1 || gap < -1) { print "FAIL: " gap " vsyncs without a shown frame"; exit 1 }
  if (after == 0) { print "FAIL: no frame shown after a rollback"; exit 1 }
  if (au > 0) { print "FAIL: " au " frames after a rollback repeat the frame before"; exit 1 }
  print "PASS"
}'
