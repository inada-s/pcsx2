#!/bin/bash
# Online without DEV9: p1 boots with DEV9 Eth off in its ini (restored after), ZDXSV_NODEV9=$IP and the fast
# login on, against the local stack (boottime.sh), route top to the lobby's user_id selection.
#   OUT=<dir> bash tests/zdxsv/nodev9.sh
# Env: ETH=true (Eth left on), MAXS (s to wait, default 200), NODEV9 (ZDXSV_NODEV9, default $IP).
# Exit 0 = the user_id screen is up and p1 logged no-DEV9 on, a fast login session with the lobby on $IP,
# DNAS got no POST, and pcsx2 had no TCP connection to a host other than $IP or 127.0.0.1.
here=$(cd "$(dirname "$0")" && pwd -W)
. "$here/env.sh"
zdxsv_need IP
OUT=${OUT:?OUT=dir for logs}
mkdir -p "$OUT"; rm -f "$OUT/remote.txt" "$OUT/samples.txt"; touch "$OUT/remote.txt"
ini="$RUN/p1/PCSX2/inis/PCSX2.ini"
cp "$ini" "$OUT/PCSX2.ini.bak" || exit 1
trap 'cp "$OUT/PCSX2.ini.bak" "$ini"; kill $sp 2>/dev/null' EXIT
sed -i "/^\[DEV9\/Eth\]/,/^\[/ s/^EthEnable = .*/EthEnable = ${ETH:-false}/" "$ini"
grep -q "^EthEnable = ${ETH:-false}" "$ini" || { echo "FAIL no EthEnable in $ini"; exit 1; }
(
  while :; do
    powershell -NoProfile -Command "\$p = (Get-Process pcsx2* -EA 0).Id; if (\$p) { Get-NetTCPConnection -OwningProcess \$p -EA 0 | ? { \$_.RemoteAddress -notin '0.0.0.0', '::', '127.0.0.1', '$IP' } | % { \$_.RemoteAddress + ':' + \$_.RemotePort } }" >> "$OUT/remote.txt"
    echo x >> "$OUT/samples.txt"; sleep 5
  done
) & sp=$!
ZDXSV_NODEV9=${NODEV9:-$IP} ZDXSV_FAST_LOGIN=1 ROUTE=top LIMIT=${MAXS:-200} MAXS=${MAXS:-200} OUT="$OUT" bash "$here/boottime.sh"
rc=$?
kill $sp 2>/dev/null
ok=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; ok=1; fi; }
log="$OUT/emulog-p1.txt"
check "user_id screen (boottime.sh rc $rc)" "[ $rc = 0 ]"
check "no DEV9 on, names resolve to ${NODEV9:-$IP}" "grep -a -q 'Zdxsv: no DEV9: .*names resolve to ${NODEV9:-$IP}' '$log'"
check "fast login session, lobby $IP" "grep -a -q 'Zdxsv: fast login: session [0-9]*, lobby $IP:' '$log'"
check "DNAS: no POST" "[ -f '$OUT/dnas.log' ] && ! grep -a -q POST '$OUT/dnas.log'"
check "pcsx2: no TCP to another host" "[ \$(wc -l < '$OUT/samples.txt') -ge 3 ] && ! grep -q . '$OUT/remote.txt'"
exit $ok
