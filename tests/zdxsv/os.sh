# OS layer of the rig scripts (sourced by env.sh): Windows (MSYS bash) or macOS/Linux (bash 3.2+).
case $(uname -s) in MINGW*|MSYS*|CYGWIN*) ZWIN=1;; *) ZWIN=;; esac
if [ -n "$ZWIN" ]; then ZEXE=.exe; else ZEXE=; fi
# zpath P: P as native programs (pcsx2, python, go) take it; zupath P: as bash takes it
zpath() { if [ -n "$ZWIN" ]; then cygpath -w "$1"; else printf '%s\n' "$1"; fi; }
zupath() { if [ -n "$ZWIN" ]; then cygpath -u "$1"; else printf '%s\n' "$1"; fi; }
zpwd() { if [ -n "$ZWIN" ]; then pwd -W; else pwd; fi; }
# zpid P: OS pid of bash child P (MSYS pids are not Windows pids)
zpid() { if [ -n "$ZWIN" ]; then cat "/proc/$1/winpid"; else echo "$1"; fi; }
# zppid P: parent of bash process P, empty at the top
zppid() {
  local p
  if [ -n "$ZWIN" ]; then p=$(cat "/proc/$1/ppid" 2>/dev/null); else p=$(ps -o ppid= -p "$1" 2>/dev/null | tr -d ' '); fi
  [ -n "$p" ] && [ "$p" -gt 1 ] && echo "$p"
}
# zanc P: P's bash ancestors as OS pids, comma separated
zanc() {
  local p=$1 a=
  while p=$(zppid "$p"); do a="$a,$(zpid "$p")"; done
  echo "${a#,}"
}
zalive() {
  if [ -n "$ZWIN" ]; then tasklist //FI "PID eq $1" 2>/dev/null | grep -q " $1 "; else kill -0 "$1" 2>/dev/null; fi
}
# zwaitgone PID SECS
zwaitgone() {
  local t=$SECONDS
  while [ $((SECONDS - t)) -lt "$2" ] && zalive "$1"; do sleep 0.5; done
}
zkill() {
  local p
  for p; do
    if [ -n "$ZWIN" ]; then taskkill //F //PID "$p" > /dev/null 2>&1; else kill -9 "$p" 2>/dev/null; fi
  done
}
# zpids_name NAME...: pids of processes whose executable name starts with NAME (any case)
zpids_name() {
  local n re=
  if [ -n "$ZWIN" ]; then
    for n; do re="$re,'$n*'"; done
    powershell -NoProfile -Command "(Get-Process ${re#,} -EA 0).Id" 2>/dev/null | tr -d '\r'
  else
    for n; do re="$re|$n"; done
    pgrep -i -- "^(${re#|})" 2>/dev/null
  fi
}
zkill_name() { zkill $(zpids_name "$@"); }
zcount_name() { zpids_name "$@" | grep -c .; }
# zpids_cmd RE [NOTRE]: pids whose command line matches extended regex RE (and not NOTRE), never this shell
zpids_cmd() {
  if [ -n "$ZWIN" ]; then
    powershell -NoProfile -Command "Get-CimInstance Win32_Process | ? { \$_.CommandLine -match '$1' -and ('${2:-}' -eq '' -or \$_.CommandLine -notmatch '${2:-}') -and \$_.CommandLine -notmatch 'Get-CimInstance' } | % { \$_.ProcessId }" 2>/dev/null | tr -d '\r'
  else
    { pgrep -f -- "$1"; [ -n "${2:-}" ] && pgrep -f -- "$2" | sed p; echo $$; echo $$; } | sort | uniq -u
  fi
}
zkill_cmd() { zkill $(zpids_cmd "$@"); }
# zstop PID|resume PID: pause / continue a process
zstop() { if [ -n "$ZWIN" ]; then powershell -NoProfile -Command "Add-Type 'using System; using System.Runtime.InteropServices; public static class Nt { [DllImport(\"ntdll.dll\")] public static extern int NtSuspendProcess(IntPtr h); }'; [void][Nt]::NtSuspendProcess((Get-Process -Id $1).Handle)"; else kill -STOP "$1"; fi; }
zcont() { if [ -n "$ZWIN" ]; then powershell -NoProfile -Command "Add-Type 'using System; using System.Runtime.InteropServices; public static class Nr { [DllImport(\"ntdll.dll\")] public static extern int NtResumeProcess(IntPtr h); }'; [void][Nr]::NtResumeProcess((Get-Process -Id $1).Handle)"; else kill -CONT "$1"; fi; }
# zlanip: first IPv4 address that is not loopback or link-local
zlanip() {
  if [ -n "$ZWIN" ]; then
    powershell -NoProfile -Command "(Get-NetIPAddress -AddressFamily IPv4 | ? { \$_.IPAddress -notlike '127.*' -and \$_.IPAddress -notlike '169.*' } | select -First 1).IPAddress" | tr -d '\r'
  elif command -v ifconfig > /dev/null; then
    ifconfig | awk '$1 == "inet" && $2 !~ /^(127|169\.254)\./ { print $2; exit }'
  else
    hostname -I | awk '{ print $1 }'
  fi
}
# zremote PID...: remote ip:port of the TCP connections of these processes
zremote() {
  local p
  if [ -n "$ZWIN" ]; then
    for p; do powershell -NoProfile -Command "Get-NetTCPConnection -OwningProcess $p -EA 0 | % { \$_.RemoteAddress + ':' + \$_.RemotePort }" | tr -d '\r'; done
  else
    for p; do lsof -nP -a -p "$p" -iTCP 2>/dev/null | awk 'NR > 1 && $9 ~ /->/ { sub(/.*->/, "", $9); print $9 }'; done
  fi
}
# zriglock: riglock.ps1's interface (-Take ME -Anc A -What W | -Check -Anc A | -Release ME); see riglock.ps1
zriglock() {
  if [ -n "$ZWIN" ]; then
    local out rc
    out=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$(dirname "${BASH_SOURCE[0]}")/riglock.ps1" "$@"); rc=$?
    [ -n "$out" ] && echo "${out//$'\r'/}"
    return $rc
  fi
  local take=0 check= release=0 anc= what= lock=$RUN/rig.lock owner=0 line= up a orph
  while [ $# -gt 0 ]; do
    case $1 in -Take) take=$2; shift;; -Check) check=1;; -Release) release=$2; shift;; -Anc) anc=$2; shift;; -What) what=$2; shift;; esac
    shift
  done
  [ -f "$lock" ] && { line=$(head -1 "$lock"); owner=${line%% *}; }
  if [ "$release" != 0 ]; then
    [ "$owner" = "$release" ] && rm -f "$lock"
    return 0
  fi
  zrig_bash() { [ "${1:-0}" != 0 ] && pgrep -x bash | grep -qx "$1"; }
  [ "$take" != 0 ] && [ -n "${RIG_OWNER:-}" ] && zrig_bash "$RIG_OWNER" && return 0
  local alive=; zrig_bash "$owner" && alive=1
  if [ -n "$check" ] && [ -n "$alive" ] && [ "$owner" != "${RIG_OWNER:-}" ]; then
    up=",$owner,$(sed -n '2s/^anc *//p' "$lock"),"
    for a in ${anc//,/ }; do case $up in *",$a,"*) return 0;; esac; done
  fi
  if [ -n "$alive" ] && [ "$owner" != "${RIG_OWNER:-}" ]; then
    echo "rig busy: $line"
    echo "  wait for it, or end it: kill $owner (then its children)"
    return 75
  fi
  [ -n "$check$alive" ] && return 0
  orph=$( (zpids_name pcsx2 zdxsv dnas zproxy; zpids_cmd 'python.*(udprelay|fake_lobby|drive)[.]py') | sort -u | tr '\n' ' ')
  if [ -n "${orph// /}" ]; then
    echo "riglock: killing orphans of a dead rig ($line): $orph"
    zkill $orph
    for a in $orph; do zwaitgone "$a" 20; done
  fi
  mkdir -p "$RUN"
  printf '%s %s %s\nanc %s\n' "$take" "$(date '+%m-%d %H:%M:%S')" "$what" "$anc" > "$lock"
}
# zstall PEER MS (macOS/Linux; stall.ps1 on Windows): pause the pcsx2 with -datapath .../PEER for MS ms
# every 5 s, 8 times, from 40 s after it appears
zstall() {
  local p= i k t
  for i in $(seq 120); do
    p=$(zpids_cmd "-datapath [^ ]*/$1( |\$)" | head -1)
    [ -n "$p" ] && break
    sleep 0.5
  done
  [ -n "$p" ] || { echo "stall: no pcsx2 for $1"; return 1; }
  echo "stall: $1 pid $p, first stall in 40 s"
  sleep 40
  for k in $(seq 0 7); do
    zalive "$p" || { echo "stall: $1 exited after $k stalls"; return 0; }
    t=$(perl -MTime::HiRes=time -e 'printf "%d", time * 1000')
    kill -STOP "$p"; sleep "$(awk -v m="$2" 'BEGIN { print m / 1000 }')"; kill -CONT "$p"
    echo "stall $k $(( $(perl -MTime::HiRes=time -e 'printf "%d", time * 1000') - t )) ms"
    sleep 5
  done
}
