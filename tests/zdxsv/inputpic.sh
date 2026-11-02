#!/bin/bash
# Input to picture on the title main menu: ZDXSV_INPUT_LATENCY presses down/up 8 times (from vsync 2000,
# every 30) and watches one pixel of the highlighted item (pixel=); prints the vsync of each press' picture
# change. Exit 1 when a press has no change. Compare two envs by their offsets (the zero point is arbitrary).
#   OUT=<dir> bash tests/zdxsv/inputpic.sh ["<env>"]
set -u
cd "$(dirname "$0")" || exit 2
OUT=${OUT:?OUT=<dir>}
rm -rf "$OUT"; mkdir -p "$OUT"
. ./env.sh
rm -rf "$RUN/p1/PCSX2/patches"
export ZDXSV_INPUT_LATENCY="addr=0x00b45ac0,start=2000,count=8,gap=30,hold=4,pixel=433:104"
[ -n "${1:-}" ] && export $1
powershell -NoProfile -File deltatest.ps1 -Var ZDXSV_GGPO -Spec "start=9000,frames=400,check=2,mask=fcff" \
  -Route 'f1500,Return' -End 2300 -Tag t -Out "$OUT" > /dev/null
grep -a "ZdxsvLatency: press [0-9]* [a-z]* picture\|ZdxsvLatency: picture changes" "$OUT/t-emulog.txt" | sed 's/^\[[ 0-9.]*\] //'
grep -aq "ZdxsvLatency: picture changes .* presses 8 .* with a change 8," "$OUT/t-emulog.txt"
