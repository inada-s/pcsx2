#!/bin/bash
# Sync-settings matrix (pcsx2/Zdxsv/SyncSettings.cpp): per case, rbk N=2 with the case's ini entries on
# peer 0 only (rbk.sh INI0), twice: control = ZDXSV_SYNC_FORCE=0 on both peers (nothing forced: shows
# whether the setting desyncs), forced = the build as shipped (must PASS: 0 mismatches).
#   OUT=<dir> bash tests/zdxsv/syncset.sh [case...]   (no case: all; `list` prints them)
# A case "SPEED0=X" runs peer 0 at speed X instead (rbk.sh SPEED0).
# Env: SEED (rbk seed, default 1), TIME (default 30), MODES (default "control forced").
# Exit 0 = every forced run passed. Result lines: "<case> <mode> PASS|DESYNC|FAIL <secs> s".
here=$(cd "$(dirname "$0")" && pwd)
cases=(
  "ee_round|EmuCore/CPU:FPU.Roundmode=0"
  "fpudiv_round|EmuCore/CPU:FPUDiv.Roundmode=3"
  "vu_round|EmuCore/CPU:VU0.Roundmode=0;EmuCore/CPU:VU1.Roundmode=0"
  "ee_clamp|EmuCore/CPU/Recompiler:fpuOverflow=false"
  "ee_clamp_full|EmuCore/CPU/Recompiler:fpuExtraOverflow=true;EmuCore/CPU/Recompiler:fpuFullMode=true"
  "vu_clamp|EmuCore/CPU/Recompiler:vu0Overflow=false;EmuCore/CPU/Recompiler:vu1Overflow=false"
  "vu_clamp_full|EmuCore/CPU/Recompiler:vu0ExtraOverflow=true;EmuCore/CPU/Recompiler:vu0SignOverflow=true;EmuCore/CPU/Recompiler:vu1ExtraOverflow=true;EmuCore/CPU/Recompiler:vu1SignOverflow=true"
  "ee_rec|EmuCore/CPU/Recompiler:EnableEE=false"
  "iop_rec|EmuCore/CPU/Recompiler:EnableIOP=false"
  "vu0_rec|EmuCore/CPU/Recompiler:EnableVU0=false"
  "vu1_rec|EmuCore/CPU/Recompiler:EnableVU1=false"
  "ee_cache|EmuCore/CPU/Recompiler:EnableEECache=true"
  "cycle_rate|EmuCore/Speedhacks:EECycleRate=-1"
  "cycle_rate_up|EmuCore/Speedhacks:EECycleRate=2"
  "cycle_skip|EmuCore/Speedhacks:EECycleSkip=2"
  "waitloop|EmuCore/Speedhacks:WaitLoop=false"
  "intc|EmuCore/Speedhacks:IntcStat=false"
  "vuflag|EmuCore/Speedhacks:vuFlagHack=false"
  "vu1instant|EmuCore/Speedhacks:vu1Instant=false"
  "mtvu|EmuCore/Speedhacks:vuThread=true"
  "fastcdvd|EmuCore/Speedhacks:fastCDVD=true"
  "extramem|EmuCore/CPU:ExtraMemory=true"
  "gamefix|EmuCore:EnableGameFixes=true;EmuCore/Gamefixes:EETimingHack=true"
  "hwdownload|EmuCore/GS:HWDownloadMode=2"
  "nominal|SPEED0=0.5"
  "turbo|Framerate:TurboScalar=1"
  "framerate|EmuCore/GS:FramerateNTSC=50"
)
[ "${1:-}" = list ] && { printf '%s\n' "${cases[@]}" | tr '|' '\t'; exit 0; }
OUT=${OUT:?OUT=dir for logs}
mkdir -p "$OUT"
rc=0
for c in "${cases[@]}"; do
  name=${c%%|*}; ini=${c#*|}
  if [ $# -gt 0 ]; then
    want=; for a in "$@"; do [ "$a" = "$name" ] && want=1; done
    [ -n "$want" ] || continue
  fi
  for mode in ${MODES:-control forced}; do
    o=$OUT/$name-$mode
    env=; [ "$mode" = control ] && env="ZDXSV_SYNC_FORCE=0"
    t0=$(date +%s)
    sp=; i0=$ini; case $ini in SPEED0=*) sp=${ini#SPEED0=}; i0=;; esac
    SPEED0=$sp PCSX2_ENV="$env${PCSX2_ENV:+ $PCSX2_ENV}" INI0="$i0" TIME=${TIME:-30} OUT=$o bash "$here/rbk.sh" 2 "${SEED:-1}" > "$o.out" 2>&1
    r=$?
    # a rig failure (e.g. no session end) is FAIL; a run whose only failure is the sync check is DESYNC
    res=PASS
    if [ $r -ne 0 ]; then
      if grep -aq '^FAIL coordinates' "$o.out" && ! grep -aq '^FAIL p' "$o.out"; then res=DESYNC; else res=FAIL; fi
    fi
    [ "$mode" = forced ] && [ "$res" != PASS ] && rc=1
    # control: the setting must have reached the VM unforced
    seen=$(grep -a -m1 'ZdxsvSync: ZDXSV_SYNC_FORCE=0, not forced' "$o/emulog-p1.txt" 2>/dev/null | sed 's/.*not forced: //' | tr -d '\r')
    [ "$mode" = control ] && seen=" (unforced: ${seen:-none})" || seen=
    echo "$name $mode $res $(( $(date +%s) - t0 )) s$seen"
  done
done
. "$here/env.sh"
[ -f "$RUN/p1/PCSX2/inis/PCSX2.ini.rbk0" ] && mv -f "$RUN/p1/PCSX2/inis/PCSX2.ini.rbk0" "$RUN/p1/PCSX2/inis/PCSX2.ini"
exit $rc
