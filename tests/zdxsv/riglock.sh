# sourced by every rig entry (rbk.sh, rbkprep.sh, m4*.sh, stack.sh): one rig at a time (riglock.ps1, zriglock).
# A live rig -> FAIL rig busy, exit 75. A dead rig's leftovers are killed first. A nested rig (RIG_OWNER
# exported by the outer one) passes. A script that sets its own EXIT trap calls rig_release in it.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"  # RUN, PY, TOOLS, zriglock, ...
RIG_ME=$(zpid $$)
# bash ancestors (on Windows MSYS ppids: Windows parent pids end at dead exec stubs) -> lock line 2, for -Check
zriglock -Take "$RIG_ME" -Anc "$(zanc $$)" -What "$0 $*"
rig_rc=$?
if [ "$rig_rc" != 0 ]; then echo "FAIL $0: rig busy (riglock rc $rig_rc)"; exit 75; fi
[ -n "${RIG_OWNER:-}" ] || export RIG_OWNER=$RIG_ME
rig_release() {
  [ "$RIG_OWNER" = "$RIG_ME" ] || return 0
  zriglock -Release "$RIG_ME"
}
trap rig_release EXIT
