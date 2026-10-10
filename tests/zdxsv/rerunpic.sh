#!/bin/bash
# Pictures after rollbacks: a GGPO synctest with check=2 (a 2-frame rollback after every 2nd frame) in
# the arcade battle, a GS snapshot of every shown frame (ZDXSV_SNAP), then tools/zdxsv/rerunpic.py
# judges them: PASS when the frames after a rollback move like the others. For changes to what rerun
# frames skip; ZDXSV_RERUN_TAIL=1 is a FAIL control.
#   OUT=<dir> bash tests/zdxsv/rerunpic.sh ["<env>"]   (e.g. "ZDXSV_RERUN_TAIL=1")
# Env: FRAMES (synctest frames, default 400). ~4 min, ~700 snapshots (~0.5 GB at 1x).
. "$(dirname "$0")/env.sh"
OUT=${OUT:?OUT=dir for the snapshots and the log}
FRAMES=${FRAMES:-400}
rm -rf "$OUT"; mkdir -p "$OUT"
export ZDXSV_SNAP="$(cygpath -w "$OUT"),1,9000"
[ -n "${1:-}" ] && export $1
powershell -NoProfile -File "$(dirname "$0")/deltatest.ps1" -Var ZDXSV_GGPO \
  -Spec "start=9000,frames=$FRAMES,check=2,mask=fcff" -Route arcade -End $((9100 + FRAMES)) -Tag t -Out "$OUT" |
  grep "synctest start\|MISMATCH\|rerun frame split"
"$PY" "$(dirname "$0")/../../tools/zdxsv/rerunpic.py" "$OUT" 9000 $((9000 + FRAMES))
