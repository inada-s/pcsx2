# sourced by m4.sh, rbkprep.sh: client N from a fresh boot into the 2v2 side entry, against fake_lobby.py --api.
#   fresh_boot N: launch, fast boot + fast login (login API on $API) to 戦場選択 (ID 1 picked by fast boot;
#     FB_LOBBY=0: fast boot stops at the user_id selection and drive.py idlogin picks ID 1 with fixed waits).
#   fresh_boot_all N..: fresh_boot for all of them at once (4 clients ~30 s, one at a time ~2 min).
#   fresh_entry N: map -> Lobby 06 (press checked, retried once), side entry; waits for N entries in fake.log.
# Env: API, CARDS (array), RUN, OUT, DRIVE, PCSX2_ENV, PCSX2_ENV_Pn, SPEED, SHOW, LAUNCH_ARGS (more launch.sh args),
#   BOOT_WAIT (s from launch to the fast boot end, default 60), ENTRY_WAIT (s for the Lobby 06 press, default 20).
fresh_boot() {
  local i=$1 penv l log t tb=$SECONDS end="lobby"
  [ "${FB_LOBBY:-1}" = 0 ] && end="user_id selection"
  eval "penv=\${PCSX2_ENV_P$i:-}"
  l=$(env ZDXSV_TLS=0 ZDXSV_FAST_BOOT=1 ZDXSV_FAST_BOOT_LOBBY=${FB_LOBBY:-1} ZDXSV_FAST_LOGIN=1 ZDXSV_LOGIN_API_URL=http://127.0.0.1:$API/lbs/login $PCSX2_ENV $penv bash "$here/launch.sh" -N $i -Speed ${SPEED:-1} -Memcard $RUN/Mcd001-${CARDS[i-1]}.ps2 ${LAUNCH_ARGS:-} $(case " ${SHOW:-} " in (*" $i "*) ;; (*) echo -Headless;; esac) 2>&1)
  echo "$l" | tail -1
  echo "$l" | grep -q "pid .* slot" || { echo "FAIL launch p$i"; return 1; }
  log="$RUN/p$i/PCSX2/logs/emulog.txt"
  for t in $(seq "${BOOT_WAIT:-60}"); do grep -aq "Zdxsv: fast boot: \($end at\|gave up\|stopped\)" "$log" 2>/dev/null && break; sleep 1; done
  grep -aq "Zdxsv: fast boot: $end at" "$log" ||
    { echo "FAIL client $i boot: $(grep -a 'Zdxsv: fast boot:' "$log" | tail -1)"; return 1; }
  [ "${FB_LOBBY:-1}" = 0 ] && $DRIVE idlogin $i 2>&1 | grep -v memgate | tail -1
  echo "client $i: in $((SECONDS - tb)) s, $(grep -c 'login api' "$OUT/fake.log") logins, $(grep -c 'conn from' "$OUT/fake.log") lobby conns"
}
fresh_boot_all() {  # fresh_boot for each client in $@ at once; each one's lines printed together, in order
  local i rc=0 pid=()
  for i in "$@"; do fresh_boot $i > "$OUT/boot-p$i.txt" 2>&1 & pid[i]=$!; done
  for i in "$@"; do wait ${pid[i]} || rc=1; cat "$OUT/boot-p$i.txt"; done
  return $rc
}
fresh_map_pressed() {  # $1 = 0x6305 count before the map route
  local t
  for t in $(seq "${ENTRY_WAIT:-20}"); do [ "$(grep -ac "C->S cat 01 cmd 0x6305" "$OUT/fake.log")" -gt "$1" ] && return 0; sleep 1; done
  return 1
}
fresh_entry() {
  local i=$1 a0 n t
  a0=$(grep -ac "C->S cat 01 cmd 0x6305" "$OUT/fake.log")
  $DRIVE map $i 2>&1 | grep -v memgate | tail -1
  fresh_map_pressed $a0 || { echo "p$i: no Lobby 06 press, C again"; $DRIVE mappress $i 2>&1 | grep -v memgate | tail -1; fresh_map_pressed $a0; } ||
    { echo "FAIL client $i entry (no 0x6305)"; return 1; }
  $DRIVE lobby2p $i 2>&1 | grep -v memgate | tail -1
  for t in $(seq 15); do [ "$(grep -c "entry .* side" "$OUT/fake.log")" -ge "$i" ] && break; sleep 2; done
  n=$(grep -c "entry .* side" "$OUT/fake.log")
  echo "client $i: $n entered"
  [ "$n" -ge "$i" ] || { echo "FAIL client $i entry"; return 1; }
}
