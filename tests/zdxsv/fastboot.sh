#!/bin/bash
# Fast boot: three boottime.sh runs with fast boot at its default (on) and no input. Run 1 starts without the boot
# state and must save it at the NET file load; run 2 must load it and reach the user_id selection
# within LIMIT s of the launch. Each run prints its time from the launch. Run 3 (an unformatted card it
# writes) must log that fast boot stopped within 15 s.
#   OUT=<dir> bash tests/zdxsv/fastboot.sh
# Env: LIMIT (s from the launch, each run, default 30), CARD (as boottime.sh).
# Exit 0 = all runs pass. Logs: OUT/save, OUT/load, OUT/unformatted (boottime.sh logs each).
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
OUT=${OUT:?OUT=dir for logs}
st="$RUN/p1/PCSX2/sstates"
rm -f "$st"/*" zdxsv-fastboot "*.p2s
run() {  # name limit log-line
  ZDXSV_FAST_BOOT=default OUT="$OUT/$1" LIMIT=$2 bash "$here/boottime.sh" | tee "$OUT/$1.txt" | grep -a 'PASS\|FAIL'
  grep -a 'PASS user_id' "$OUT/$1.txt" > /dev/null || { echo "FAIL $1: no user_id screen in $2 s"; exit 1; }
  grep -a "Zdxsv: fast boot: $3" "$OUT/$1/emulog-p1.txt" | head -1
  grep -aq "Zdxsv: fast boot: $3" "$OUT/$1/emulog-p1.txt" || { echo "FAIL $1: no '$3' in emulog"; exit 1; }
}
mkdir -p "$OUT"
run save "${LIMIT:-30}" saving
ls "$st"/*" zdxsv-fastboot "*.p2s > /dev/null 2>&1 || { echo "FAIL save: no boot state in $st"; exit 1; }
run load "${LIMIT:-30}" loading
blank="$RUN/Mcd001-zdxsv-unformatted.ps2"
head -c 8650752 /dev/zero | tr '\0' '\377' > "$blank"
ZDXSV_FAST_BOOT=default CARD=zdxsv-unformatted OUT="$OUT/unformatted" MAXS=15 bash "$here/boottime.sh" > "$OUT/unformatted.txt"
rm -f "$blank"
grep -a 'Zdxsv: fast boot: stopped, memory card error' "$OUT/unformatted/emulog-p1.txt" || { echo "FAIL unformatted: fast boot did not stop"; exit 1; }
echo PASS
