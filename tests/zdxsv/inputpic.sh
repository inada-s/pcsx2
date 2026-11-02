#!/bin/bash
# Input to picture on the title main menu: ZDXSV_INPUT_LATENCY presses down/up 8 times (from vsync 2000,
# every 30), ZDXSV_SNAP takes every shown frame, tools/zdxsv/inputpic.py prints the vsync of each picture
# change relative to its press. Compare two envs by their offsets (the offset's zero point is arbitrary).
#   OUT=<dir> bash tests/zdxsv/inputpic.sh ["<env>"] [pnach]
set -u
cd "$(dirname "$0")" || exit 2
OUT=${OUT:?OUT=<dir>}
rm -rf "$OUT"; mkdir -p "$OUT"
. ./env.sh
P="$RUN/p1/PCSX2/patches"
rm -rf "$P"
if [ -n "${2:-}" ]; then mkdir -p "$P"; cp "$2" "$P/SLPS-25419_435D8236.pnach"; fi
export ZDXSV_SNAP="$(cygpath -w "$OUT"),1,1990"
export ZDXSV_INPUT_LATENCY="addr=0x00b45ac0,start=2000,count=8,gap=30,hold=4,out=$(cygpath -w "$OUT")\\lat.csv"
[ -n "${1:-}" ] && export $1
powershell -NoProfile -File deltatest.ps1 -Var ZDXSV_GGPO -Spec "start=9000,frames=400,check=2,mask=fcff" \
  -Route 'f1500,Return' -End 2300 -Tag t -Out "$OUT" > /dev/null
rm -rf "$P"
"$PY" ../../tools/zdxsv/inputpic.py "$OUT" 2000 30 8
