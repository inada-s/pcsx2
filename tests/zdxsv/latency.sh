#!/bin/bash
# Pad input latency on the title main menu. One instance, p1 data dir, windowed.
#   latency.sh                      -> measure on the menu cursor byte, 40 presses
#   latency.sh -Spec 'count=10'     -> search mode (no addr=): candidate bytes
#   latency.sh -LowLatency 0|1      -> sets EmuCore/GS ZdxsvLowLatencyVsync in p1's ini first
#   -Out latency.csv (CSV per press), -Seed 1
# p1's card autoloads system data; f2600 + Start = attract -> main menu.
# Prints the ZdxsvLatency log lines.
here=$(cd "$(dirname "$0")" && pwd)
spec=addr=0x00b45ac0,count=40 out=latency.csv seed=1 low=
while [ $# -gt 0 ]; do
  case $1 in
    -Spec) spec=$2;; -Out) out=$2;; -Seed) seed=$2;; -LowLatency) low=$2;;
    *) echo "FAIL $0: unknown argument $1"; exit 2;;
  esac
  shift 2
done
. "$here/env.sh"  # RUN, PY
if [ -n "$low" ]; then
  ini=$RUN/p1/PCSX2/inis/PCSX2.ini
  v=false; [ "$low" = 1 ] && v=true
  awk -v v=$v '{ sub(/\r$/, "") } /^ZdxsvLowLatencyVsync =/ { next } { print } /^\[EmuCore\/GS\]$/ { print "ZdxsvLowLatencyVsync = " v }' \
    "$ini" > "$ini.new" && mv -f "$ini.new" "$ini"
fi
count=20
[[ $spec =~ count=([0-9]+) ]] && count=${BASH_REMATCH[1]}
ZDXSV_INPUT_LATENCY="btn=down,back=up,start=2900,gap=60,seed=$seed,out=$out,$spec" bash "$here/launch.sh" -N 1 > /dev/null || exit $?
sleep 8
"$PY" "$here/pine.py" seq "f2600,Return" 2> /dev/null
"$PY" "$here/pine.py" seq "f$((2900 + 60 * count + 60))" 2> /dev/null
grep -a 'ZdxsvLatency' "$RUN/p1/PCSX2/logs/emulog.txt"
taskkill //F //IM pcsx2-qtx64.exe > /dev/null 2>&1
exit 0
