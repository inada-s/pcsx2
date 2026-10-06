# sourced by m4.sh, rbkprep.sh: entry1_retry N LAUNCH-CMD..
# Launches client N from its lobby state, runs `$ENTRY1 N` (drive.py entry1) and waits for its
# first lobby send (the N-th 0x640f answer in $OUT/fake.log). No send = relaunch from the state, retry.
# Entry flake (230 client entries measured): first send 10.5-10.6 s after the
# driver connects (222), or never before the game's ~106 s RST (8 = 3.5 %; route timing, frames
# per hold and load did not explain it). ENTRY1_WAIT (25 s) past entry1, ENTRY_TRIES (3) launches.
entry1_retry() {
  local i=$1 k out pid
  shift
  for k in $(seq "${ENTRY_TRIES:-3}"); do
    out=$("$@" 2>&1 | tail -1)
    echo "$out"
    pid=$(echo "$out" | sed -n 's/.* pid \([0-9]*\) .*/\1/p')
    # refused launch (probelint, riglock): no client to drive, retries refuse too
    [ -n "$pid" ] || { echo "client $i: launch refused"; return 1; }
    sleep 8
    ${ENTRY1:-$DRIVE entry1} "$i" 2>&1 | grep -v memgate | tail -1
    for t in $(seq $(( ${ENTRY1_WAIT:-25} / 2 ))); do
      if [ "$(grep -c "C<-S cat 02 cmd 0x640f" "$OUT/fake.log")" -ge "$i" ]; then
        cp "$RUN/p$i/PCSX2/logs/emulog.txt" "$RUN/entry-ok-p$i.txt" 2>/dev/null  # reference for entrysig.py
        return 0
      fi
      sleep 2
    done
    echo "client $i: no lobby send ${ENTRY1_WAIT:-25} s after entry1 (try $k), relaunch"
    cp "$RUN/p$i/PCSX2/logs/emulog.txt" "$OUT/emulog-p$i-try$k.txt" 2>/dev/null
    # a flake logs nothing a passing entry did not; a client fault logs the same new line every try
    # (e.g. a client test that breaks the adopted connection): 2 tries sharing a new line = stop, not retry
    if ls "$RUN"/entry-ok-p*.txt >/dev/null 2>&1; then
      $PY "$here/entrysig.py" "$OUT/emulog-p$i-try$k.txt" "$RUN"/entry-ok-p*.txt | sed "s/^/client $i try $k new: /" | head -5
      if [ "$k" -gt 1 ] && $PY "$here/entrysig.py" --same "$OUT/emulog-p$i-try$((k - 1)).txt" "$OUT/emulog-p$i-try$k.txt" "$RUN"/entry-ok-p*.txt > /dev/null; then
        echo "client $i: FAIL same new emulog lines in tries $((k - 1)) and $k: a client fault, not the entry flake"
        powershell -NoProfile -Command "Stop-Process -Id $pid -Force -EA 0; Wait-Process -Id $pid -Timeout 20 -EA 0"
        return 1
      fi
    fi
    [ -n "$pid" ] && powershell -NoProfile -Command "Stop-Process -Id $pid -Force -EA 0; Wait-Process -Id $pid -Timeout 20 -EA 0"
  done
  return 1
}
