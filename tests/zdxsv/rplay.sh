#!/bin/bash
# replay play test (pcsx2 ZDXSV_REPLAY): plays a .zdxr headless in instance pN (turbo, exits at its end) with
# PW hashes, then pwcheck compares them with the live battle's traces (rbk.sh OUT/trace-p*.txt).
#   OUT=dir FILE=x.zdxr bash tests/zdxsv/rplay.sh <live trace>...   Exit 0 = replay end logged + player work equal.
# Env: N (instance, default 1), CLAMP (ZDXSV_EE_CLAMP of the recording: rbk.sh's is in its emulog `rbk env`;
# a lobby battle has none; unset = read from the `rbk env` lines of emulog-p*.txt next to the live traces, the
# replay or its parent dir, a CLAMP differing from them FAILs before launch, CLAMP=none plays without one), PLAYERS (players compared, default 2), OWN (default 1: pwcheck --own),
# STATE (any state of the game to boot from; default rbk-p1), MAXS (default 300).
# WINDOW=1: windowed, 1x (no turbo). KEYS="secs:seq;..." (needs WINDOW=1): pcsx2ctl.ps1 -Seq at secs after the
# replay started; binds the seek hotkeys (PageUp back / PageDown forward 10 s), TogglePause (Space), point of
# view (Home) and key display (End), round jump (Shift+PageUp / Shift+PageDown) in the instance ini. With PCSX2_ENV=ZDXSV_REPLAY_EXIT=0 the replay pauses at its end; Space then ends it (H lines written).
# FILE="a;b" (point of view) refuses unless a and b each PASSed alone on this exe ($RUN/rplay-ledger.txt); POV_UNTESTED=1 skips.
# SKIP_MS=1: skip MS selection (pcsx2's default; off here so KEYS seconds keep their frames).
# Control bar: KEYS="15:bar:show,w600,bar:timeline:0.7,shot:$OUT/a.png" (pcsx2ctl.ps1 mouse tokens).
here=$(cd "$(dirname "$0")" && pwd -W)
. "$here/riglock.sh"  # one rig at a time
OUT=${OUT:?OUT=dir for logs}
FILE=${FILE:?FILE=replay .zdxr}
N=${N:-1}
STATE=${STATE:-${RBKSTATES:?set STATE or RBKSTATES}/rbk-p1.p2s}
mkdir -p "$OUT"
trap 'powershell -NoProfile -Command "Get-Process pcsx2* -EA 0 | Stop-Process -Force"; cp "$RUN/p$N/PCSX2/logs/emulog.txt" "$OUT/emulog-play.txt" 2>/dev/null; rig_release' EXIT
# the recording's clamp (a play without it differs from the live battle at the first clamped frame)
rec=$(dirname "$FILE")
cl=$( { for f in "$@"; do ls "$(dirname "$f")"/emulog-p*.txt; done; ls "$rec"/emulog-p*.txt "$rec"/../emulog-p*.txt; } 2>/dev/null \
  | sort -u | while read -r e; do grep -a -h -o "ZdxsvGgpo: rbk env .* clamp=[^ ]*" "$e" | sed 's/.* clamp=//'; done | sort -u)
if [ "$(printf '%s' "$cl" | grep -c .)" -gt 1 ]; then
  echo "FAIL recordings' emulogs name different clamps: $(echo $cl)"; exit 1
elif [ -n "$cl" ]; then
  if [ -z "$CLAMP" ]; then CLAMP=$cl; echo "CLAMP=$cl (rbk env)"
  elif [ "$CLAMP" != "$cl" ] && [ "$CLAMP" != none ]; then echo "FAIL CLAMP=$CLAMP but the recording ran with clamp=$cl"; exit 1; fi
fi
[ "$CLAMP" = none ] && CLAMP=
# FILE="a;b" (point of view) needs each file's own single-file run to have PASSed on this exe (ledger below):
# a switch run on a file never played alone fails exactly like that file alone, and shows nothing new.
# POV_UNTESTED=1 runs anyway.
ledger=$RUN/rplay-ledger.txt
exe=${PCSX2_EXE:-$here/../../bin/pcsx2-qtx64.exe}
fid() { echo "$(sha1sum < "$1" | cut -c1-16).$(stat -c %s.%Y "$exe")"; }
if [[ $FILE == *\;* ]] && [ "${POV_UNTESTED:-0}" != 1 ]; then
  IFS=';' read -ra parts <<< "$FILE"
  for f in "${parts[@]}"; do
    r=$(grep -a " $(fid "$f") " "$ledger" 2>/dev/null | tail -1 | cut -d' ' -f1)
    [ "$r" = PASS ] || { echo "FAIL $f: last single-file rplay on this exe: ${r:-none}; run FILE=$f alone first (POV_UNTESTED=1 skips)"; exit 1; }
  done
fi
t0=$SECONDS
rm -f "$RUN/p$N/PCSX2/logs/emulog.txt" "$OUT/trace-play.txt"
ini=$RUN/p$N/PCSX2/inis/PCSX2.ini
if [ -n "$KEYS" ]; then
  [ "$WINDOW" = 1 ] || { echo "FAIL KEYS needs WINDOW=1"; exit 1; }
  for b in "ZdxsvReplaySeekBack = Keyboard/PageUp" "ZdxsvReplaySeekForward = Keyboard/PageDown" "TogglePause = Keyboard/Space" "ZdxsvReplayNextPov = Keyboard/Home" "ZdxsvReplayToggleKeys = Keyboard/End" "ZdxsvReplayPrevRound = Keyboard/Shift & Keyboard/PageUp" "ZdxsvReplayNextRound = Keyboard/Shift & Keyboard/PageDown"; do
    grep -q "^${b%% *} " "$ini" || sed -i "/^\[Hotkeys\]/a $b" "$ini"
  done
  grep -c "^ZdxsvReplaySeek\|^TogglePause \|^ZdxsvReplayNextPov \|^ZdxsvReplayToggleKeys " "$ini" | grep -q 5 || { echo "FAIL hotkeys not bound in $ini"; exit 1; }
  # every step's keys against the ini before launch (else an unbound key is refused only at its send)
  IFS=';' read -ra ks <<< "$KEYS"
  for k in "${ks[@]}"; do
    powershell -NoProfile -File "$here/pcsx2ctl.ps1" -Check -Ini "$(cygpath -w "$ini")" -Seq "${k#*:}" > "$OUT/keycheck.txt" 2>&1 \
      || { cat "$OUT/keycheck.txt"; echo "FAIL KEYS step '$k' before launch"; exit 1; }
  done
fi
env ZDXSV_REPLAY="$FILE" ZDXSV_REPLAY_EXIT=1 $([ "$WINDOW" = 1 ] || echo ZDXSV_REPLAY_TURBO=1) ${CLAMP:+ZDXSV_EE_CLAMP=$CLAMP} \
  ZDXSV_PW_HASH=1 ZDXSV_NET_TRACE="$OUT/trace-play.txt" ZDXSV_REPLAY_SKIP_MS=${SKIP_MS:-0} $PCSX2_ENV \
  powershell -NoProfile -Command "& '$here/launch.ps1' -N $N $([ "$WINDOW" = 1 ] || echo -Headless) -StateFile $STATE" 2>&1 | tail -1
l=$RUN/p$N/PCSX2/logs/emulog.txt
# start check: the replay header line within 30 s, else stop
for t in $(seq 30); do grep -a -q "ZdxsvGgpo: replay .*: \(position\|not a\|bad\)\|replay: state load failed" "$l" 2>/dev/null && break; sleep 1; done
grep -a "ZdxsvGgpo: replay" "$l" | cut -c1-200
grep -a -q "ZdxsvGgpo: replay .*: position" "$l" || { echo "FAIL replay did not start"; exit 1; }
t1=$SECONDS
IFS=';' read -ra keys <<< "$KEYS"
while [ $((SECONDS - t0)) -lt "${MAXS:-300}" ]; do
  sleep 1
  [ "$(tasklist | grep -ci pcsx2)" -eq 0 ] && break
  # `state load failed` can come 0.6 s after the start check's `position` line, the player ran to MAXS 300 s
  bad=$(grep -a -m1 "ZdxsvGgpo: replay\( [^:]*\)\?: \(state load failed\|not a replay\|bad header\|not another position\|cannot write\)\|ZdxsvGgpo: replay seek: key [0-9]* load failed" "$l")
  [ -n "$bad" ] && { echo "$bad" | cut -c1-200; echo "FAIL replay error at $((SECONDS - t0)) s"; exit 1; }
  if [ ${#keys[@]} -gt 0 ] && [ $((SECONDS - t1)) -ge "${keys[0]%%:*}" ]; then
    echo "$((SECONDS - t1)) s: keys ${keys[0]#*:}"
    powershell -NoProfile -File "$here/pcsx2ctl.ps1" -Seq "${keys[0]#*:}" > "$OUT/keys.txt" 2>&1
    rc=$?; tail -3 "$OUT/keys.txt"
    [ $rc = 0 ] || { echo "FAIL pcsx2ctl rc $rc (unbound key: nothing sent)"; exit 1; }
    keys=("${keys[@]:1}")
  fi
done
[ ${#keys[@]} -eq 0 ] || { echo "FAIL keys not sent: ${keys[*]}"; ok_keys=1; }
echo "$((SECONDS - t0)) s: $(tasklist | grep -ci pcsx2) pcsx2 left"
ok=${ok_keys:-0}
grep -a "ZdxsvGgpo: \(replay end\|replay:\|replay seek\|net sends\|zd steps\)" "$l" | cut -c1-200
grep -a -q "ZdxsvGgpo: replay end" "$l" || { echo "FAIL no replay end"; ok=1; }
$PY "$TOOLS/pwcheck.py" $([ "${OWN-1}" = 1 ] && echo --own) "$OUT/trace-play.txt" "$@" > "$OUT/pwcheck.txt"
grep '^common\|^player' "$OUT/pwcheck.txt" | cut -c1-160
awk -v n=${PLAYERS:-2} '$1=="player" && $2+0 < n {s += $4} $1=="common" {c = $3} END {exit !(c > 0 && s == 0)}' "$OUT/pwcheck.txt" \
  || { echo "FAIL player work differs (players < ${PLAYERS:-2}) or no frames"; ok=1; }
echo "rplay $(basename "$FILE"): $((SECONDS - t0)) s, $([ $ok = 0 ] && echo PASS || echo FAIL)"
[[ $FILE == *\;* ]] || echo "$([ $ok = 0 ] && echo PASS || echo FAIL) $(fid "$FILE") $FILE $(date +%F.%T)" >> "$ledger"
exit $ok
