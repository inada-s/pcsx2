# sourced by every rig entry (rbk.sh, rbkprep.sh, m4*.sh, stack.sh): one rig at a time (riglock.ps1).
# A live rig -> FAIL rig busy, exit 75. A dead rig's leftovers are killed first. A nested rig (RIG_OWNER
# exported by the outer one) passes. A script that sets its own EXIT trap calls rig_release in it.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"  # RUN, PY, TOOLS, ...
RIG_PS1=$(dirname "${BASH_SOURCE[0]}")/riglock.ps1
RIG_ME=$(cat /proc/$$/winpid)
# bash ancestors' winpids (MSYS ppid: Windows parent pids end at dead exec stubs) -> lock line 2, for -Check
rig_anc= rig_p=$$
while rig_p=$(cat /proc/$rig_p/ppid 2>/dev/null) && [ "$rig_p" -gt 1 ]; do
  rig_anc="$rig_anc,$(cat /proc/$rig_p/winpid)"
done
rig_out=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$RIG_PS1" -Take "$RIG_ME" -Anc "${rig_anc#,}" -What "$0 $*")
rig_rc=$?
[ -n "$rig_out" ] && echo "${rig_out//$'\r'/}"
if [ "$rig_rc" != 0 ]; then echo "FAIL $0: rig busy (riglock rc $rig_rc)"; exit 75; fi
[ -n "${RIG_OWNER:-}" ] || export RIG_OWNER=$RIG_ME
rig_release() {
  [ "$RIG_OWNER" = "$RIG_ME" ] || return 0
  powershell -NoProfile -ExecutionPolicy Bypass -File "$RIG_PS1" -Release "$RIG_ME"
}
trap rig_release EXIT
