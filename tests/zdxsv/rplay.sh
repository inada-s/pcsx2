#!/bin/bash
# replay play test (pcsx2 ZDXSV_REPLAY): plays a replay .pb headless in instance pN (turbo, exits at its end) with
# PW hashes, then pwcheck compares them with the live battle's traces (rbk.sh OUT/trace-p*.txt).
#   OUT=dir FILE=x.pb bash tests/zdxsv/rplay.sh <live trace>...   Exit 0 = replay end logged + player work equal.
# Env: N (instance, default 1), CLAMP (ZDXSV_EE_CLAMP of the recording: rbk.sh's is in its emulog `rbk env`;
# a lobby battle has none; unset = read from the `rbk env` lines of emulog-p*.txt next to the live traces, the
# replay or its parent dir, a CLAMP differing from them FAILs before launch, CLAMP=none plays without one), PLAYERS (players compared, default 2), OWN (default 1: pwcheck --own),
# STATE (any state of the game to boot from; default rbk-p1), MAXS (default 300).
# WINDOW=1: windowed, 1x (no turbo). KEYS="secs:seq;..." (needs WINDOW=1): pcsx2ctl.ps1 -Seq at secs after the
# replay started; binds the seek hotkeys (PageUp back / PageDown forward 10 s), TogglePause (Space), point of
# view (Home) and key display (End), round jump (Shift+PageUp / Shift+PageDown) in the instance ini. With PCSX2_ENV=ZDXSV_REPLAY_EXIT=0 the replay pauses at its end; Space then ends it (H lines written).
# POV=p: plays position p's point of view (ZDXSV_REPLAY_POV; default the recorder's): the lobby answers name it.
# SKIP_MS=1: skip MS selection (pcsx2's default; off here so KEYS seconds keep their frames).
# FOUR=1: four-screen, the point of view in pN, one spawned guest per other position (PLAYERS); each
# guest's trace (trace-play-povP.txt) is pwchecked too, and the host's `spread` lines with all members live must stay
# <= SPREAD (default 4) frames. With PCSX2_ENV=ZDXSV_REPLAY_SYNC=0 (control: no waiting) the spread check FAILs.
# Control bar: KEYS="15:bar:show,w600,bar:timeline:0.7,shot:$OUT/a.png" (pcsx2ctl.ps1 mouse tokens).
# API=1: plays ZDXSV_REPLAY=http://127.0.0.1:API_PORT/lbs/replay?battle_code=<FILE's name> from apiserve.py (API_PORT,
# default 9891), whose replay_url serves FILE; checks the replay_url line too.
here=$(cd "$(dirname "$0")" && pwd -W)
. "$here/riglock.sh"  # one rig at a time
OUT=${OUT:?OUT=dir for logs}
FILE=${FILE:?FILE=replay .pb}
N=${N:-1}
STATE=${STATE:-${RBKSTATES:?set STATE or RBKSTATES}/rbk-p1.p2s}
mkdir -p "$OUT"
trap 'powershell -NoProfile -Command "Get-Process pcsx2* -EA 0 | Stop-Process -Force"; cp "$RUN/p$N/PCSX2/logs/emulog.txt" "$OUT/emulog-play.txt" 2>/dev/null; cp "$RUN/p$N"/PCSX2/logs/emulog-pov*.txt "$OUT/" 2>/dev/null; [ -n "$api" ] && kill $api; rig_release' EXIT
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
t0=$SECONDS
rm -f "$RUN/p$N/PCSX2/logs/emulog.txt" "$OUT/trace-play.txt" "$RUN/p$N"/PCSX2/logs/emulog-pov*.txt "$OUT"/trace-play-pov*.txt
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
play=$FILE api=
if [ "${API:-}" = 1 ]; then
  "$PY" -I "$here/apiserve.py" ${API_PORT:-9891} "$FILE" > "$OUT/apiserve.log" 2>&1 & api=$!
  play="http://127.0.0.1:${API_PORT:-9891}/lbs/replay?battle_code=$(basename "$FILE" .pb)"
fi
env ZDXSV_REPLAY="$play" ${POV:+ZDXSV_REPLAY_POV=$POV} ZDXSV_REPLAY_EXIT=1 $([ "$WINDOW" = 1 ] || echo ZDXSV_REPLAY_TURBO=1) ${CLAMP:+ZDXSV_EE_CLAMP=$CLAMP} \
  $([ "${FOUR:-0}" = 1 ] && echo ZDXSV_REPLAY_FOUR=1)   ZDXSV_PW_HASH=1 ZDXSV_NET_TRACE="$OUT/trace-play.txt" ZDXSV_REPLAY_SKIP_MS=${SKIP_MS:-0} $PCSX2_ENV \
  powershell -NoProfile -Command "& '$here/launch.ps1' -N $N $([ "$WINDOW" = 1 ] || echo -Headless) -StateFile $STATE" 2>&1 | tail -1
l=$RUN/p$N/PCSX2/logs/emulog.txt
# start check: the replay header line within 30 s, else stop
for t in $(seq 30); do grep -a -q "ZdxsvGgpo: replay .*: \(recorded at position\|not a\|bad\|no lobby answers\)\|replay: state load failed" "$l" 2>/dev/null && break; sleep 1; done
grep -a "ZdxsvGgpo: replay" "$l" | cut -c1-200
grep -a -q "ZdxsvGgpo: replay .*: recorded at position" "$l" || { echo "FAIL replay did not start"; exit 1; }
[ -z "$api" ] || grep -a -q "replay_url http://127.0.0.1:${API_PORT:-9891}/pb" "$l" || { echo "FAIL no replay_url line"; exit 1; }
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
# the game's own position at its arm (its first key msg) is the asked point of view
if [ -n "$POV" ] && ! grep -a -q "net armed at vsync [0-9]*, position $POV\b" "$l"; then echo "FAIL the game did not arm as position $POV"; ok=1; fi
$PY "$TOOLS/pwcheck.py" $([ "${OWN-1}" = 1 ] && echo --own) "$OUT/trace-play.txt" "$@" > "$OUT/pwcheck.txt"
grep '^common\|^player\|^rng' "$OUT/pwcheck.txt" | cut -c1-160
awk -v n=${PLAYERS:-2} '$1=="player" && $2+0 < n {s += $4} $1=="rng:" {s += $3} $1=="common" {c = $3} END {exit !(c > 0 && s == 0)}' "$OUT/pwcheck.txt" \
  || { echo "FAIL coordinates (players < ${PLAYERS:-2}) or RNG differ, or no frames"; ok=1; }
if [ "${FOUR:-0}" = 1 ]; then
  cp "$RUN/p$N"/PCSX2/logs/emulog-pov*.txt "$OUT/" 2>/dev/null
  nt=0
  for t in "$OUT"/trace-play-pov*.txt; do
    [ -f "$t" ] || continue
    nt=$((nt + 1)); p=${t##*-pov}; p=${p%.txt}
    gl="$OUT/emulog-pov$p.txt"
    grep -a "ZdxsvGgpo: replay \(end\|four-screen: the host\|four-screen: frame [0-9]*, [0-9]* behind\)" "$gl" | head -5 | cut -c1-160
    grep -a -q "ZdxsvGgpo: replay end" "$gl" || { echo "FAIL pov $p: no replay end in $gl"; ok=1; }
    $PY "$TOOLS/pwcheck.py" $([ "${OWN-1}" = 1 ] && echo --own) "$t" "$@" > "$OUT/pwcheck-pov$p.txt"
    grep '^common\|^player\|^rng' "$OUT/pwcheck-pov$p.txt" | sed "s/^/pov $p: /" | cut -c1-160
    awk -v n=${PLAYERS:-2} '$1=="player" && $2+0 < n {s += $4} $1=="rng:" {s += $3} $1=="common" {c = $3} END {exit !(c > 0 && s == 0)}' "$OUT/pwcheck-pov$p.txt" \
      || { echo "FAIL pov $p: coordinates or RNG differ, or no frames"; ok=1; }
  done
  [ $nt = $((${PLAYERS:-2} - 1)) ] || { echo "FAIL $nt guest traces for ${PLAYERS:-2} players"; ok=1; }
  # host's spread lines while all members are live
  grep -a "four-screen: frame [0-9]*, ${PLAYERS:-2} members" "$l" | awk -v max=${SPREAD:-4} '{n++; match($0, /spread -?[0-9]+/); s = substr($0, RSTART + 7, RLENGTH - 7) + 0; if (s > m) m = s} END {
    printf "spread: %d lines with all members, max %d frames\n", n, m; exit !(n >= 10 && m <= max)}' \
    || { echo "FAIL spread over ${SPREAD:-4} frames or under 10 samples"; ok=1; }
fi
echo "rplay $(basename "$FILE"): $((SECONDS - t0)) s, $([ $ok = 0 ] && echo PASS || echo FAIL)"
exit $ok
