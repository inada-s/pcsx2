# sourced by m4z.sh: entry2p_retry N -- client N from 戦場選択 into Lobby 02 (1v1) and its side dialog.
# Entry flake (3 of 15 entry2p entries): no 0x6305 EnterLobby after the map
# press, the client stays on the map polling 0x6303/04/08 and the 0x640E check fails ~4 min into the rig.
# Here: map2p, wait ENTRY2P_WAIT (12 s) for this client's 0x6305 `00 02`, else remap2p (walk again from
# the clamp), ENTRY_TRIES (3) presses; a 0x6305 into another lobby FAILs (no exit route).
# ENTRY2P_FAULT=N: client N's first press is left out (the flake's state: on the map, no 0x6305).
lobby_enters() { grep -a -A1 "C->S \[Q\] ID:0x6305" "$OUT/lobby.log" | grep -c "^00000000  $1 "; }
entry2p_retry() {
  local i=$1 k n0 a0 t route=map2p
  n0=$(lobby_enters "00 02"); a0=$(grep -ac "C->S \[Q\] ID:0x6305" "$OUT/lobby.log")
  for k in $(seq "${ENTRY_TRIES:-3}"); do
    if [ "$k" = 1 ] && [ "${ENTRY2P_FAULT:-}" = "$i" ]; then
      $DRIVE seq "$i" "${ROUTE_MAP2P_NOC:-C,w5000,Up:3000,Left:6000,w500}" map2pfault 2>&1 | grep -v memgate | tail -1
      echo "p$i map2p press left out (fault)"
    else
      $DRIVE $route "$i" 2>&1 | grep -v memgate | tail -1
    fi
    for t in $(seq "${ENTRY2P_WAIT:-12}"); do
      [ "$(lobby_enters "00 02")" -gt "$n0" ] && { $DRIVE lobby2p "$i" 2>&1 | grep -v memgate | tail -1; return 0; }
      [ "$(grep -ac "C->S \[Q\] ID:0x6305" "$OUT/lobby.log")" -gt "$a0" ] && { echo "client $i: entered another lobby than 02"; return 1; }
      sleep 1
    done
    echo "client $i: no 0x6305 Lobby 02 ${ENTRY2P_WAIT:-12} s after $route (try $k), walk again"
    route=remap2p
  done
  return 1
}
