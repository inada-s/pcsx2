#!/bin/bash
# Delta-state / GGPO synctest run: VAR=SPEC (ZDXSV_DELTA_TEST or ZDXSV_GGPO), pine ROUTE first
# (-Route arcade: offline battle from ~f8000), snapshots at -Snaps "f1 f2..", log lines out.
# -Spec S -Var V -Snaps "F.." -End F -Tag T -Out DIR -Route R
here=$(cd "$(dirname "$0")" && pwd)
spec=start=1500,frames=3000,depth=8,every=20 var=ZDXSV_DELTA_TEST snaps= end=4600 tag=a out=delta route=
while [ $# -gt 0 ]; do
  case $1 in
    -Spec) spec=$2;; -Var) var=$2;; -Snaps) snaps=$2;; -End) end=$2;; -Tag) tag=$2;; -Out) out=$2;; -Route) route=$2;;
    *) echo "FAIL $0: unknown argument $1"; exit 2;;
  esac
  shift 2
done
# arcade: from boot, 1P side, name あああ, default suits, battle by ~f8000
[ "$route" = arcade ] && route=f1500,Return,w3000,C,w6000,C,w5000,C,w5000,C,w5000,C,w5000,Return,w2000,C,w2000,Up,w500,Left,w800,Down,w400,Down,w400,Down,w400,Down,w400,Left,w400,Left,w400,C,w5000,C,w5000,C,w5000,C,w5000,C,w6000,C,w6000,C,w6000,C,w6000,C,w6000,C,w6000
. "$here/env.sh"  # RUN, PY
mkdir -p "$out"
taskkill //F //IM pcsx2-qtx64.exe > /dev/null 2>&1
env "$var=$spec" bash "$here/launch.sh" -N 1 > /dev/null || exit $?
sleep 8
[ -n "$route" ] && "$PY" "$here/pine.py" seq "$route" 2> /dev/null
for f in $snaps; do
  "$PY" "$here/pine.py" seq "f$f" 2> /dev/null
  "$PY" "$here/pine.py" snap "$out/$tag-f$f.png" 0.5 > /dev/null 2>&1
done
"$PY" "$here/pine.py" seq "f$end" 2> /dev/null
sleep 2
log=$out/$tag-emulog.txt
cp -f "$RUN/p1/PCSX2/logs/emulog.txt" "$log"
taskkill //F //IM pcsx2-qtx64.exe > /dev/null 2>&1
grep -a 'ZdxsvDelta\|ZdxsvGgpo' "$log"
# ZDXSV_DELTA_TEST: exit 1 on `result FAIL` (pass 2+ differed), 3 when no result line was logged
if [ "$var" = ZDXSV_DELTA_TEST ]; then
  grep -aq 'ZdxsvDelta: result FAIL' "$log" && exit 1
  grep -aq 'ZdxsvDelta: result' "$log" || exit 3
fi
exit 0
