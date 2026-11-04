"""Sync-settings matrix (pcsx2/Zdxsv/SyncSettings.cpp): per case, rbk N=2 with the case's ini entries on
peer 0 only (rbk.py INI0), twice: control = ZDXSV_SYNC_FORCE=0 on both peers (nothing forced: shows
whether the setting desyncs), forced = the build as shipped (must PASS: 0 mismatches).
  OUT=<dir> python tests/zdxsv/syncset.py [case...]   (no case: all; `list` prints them)
A case "SPEED0=X" runs peer 0 at speed X instead (rbk.py SPEED0).
Env: SEED (rbk seed, default 1), TIME (default 30), MODES (default "control forced").
Exit 0 = every forced run passed. Result lines: "<case> <mode> PASS|DESYNC|FAIL <secs> s".
"""
import os
import re
import subprocess
import sys
import time

import zrig

CASES = (
    ("ee_round", "EmuCore/CPU:FPU.Roundmode=0"),
    ("fpudiv_round", "EmuCore/CPU:FPUDiv.Roundmode=3"),
    ("vu_round", "EmuCore/CPU:VU0.Roundmode=0;EmuCore/CPU:VU1.Roundmode=0"),
    ("ee_clamp", "EmuCore/CPU/Recompiler:fpuOverflow=false"),
    ("ee_clamp_full", "EmuCore/CPU/Recompiler:fpuExtraOverflow=true;EmuCore/CPU/Recompiler:fpuFullMode=true"),
    ("vu_clamp", "EmuCore/CPU/Recompiler:vu0Overflow=false;EmuCore/CPU/Recompiler:vu1Overflow=false"),
    ("vu_clamp_full", "EmuCore/CPU/Recompiler:vu0ExtraOverflow=true;EmuCore/CPU/Recompiler:vu0SignOverflow=true;EmuCore/CPU/Recompiler:vu1ExtraOverflow=true;EmuCore/CPU/Recompiler:vu1SignOverflow=true"),
    ("ee_rec", "EmuCore/CPU/Recompiler:EnableEE=false"),
    ("iop_rec", "EmuCore/CPU/Recompiler:EnableIOP=false"),
    ("vu0_rec", "EmuCore/CPU/Recompiler:EnableVU0=false"),
    ("vu1_rec", "EmuCore/CPU/Recompiler:EnableVU1=false"),
    ("ee_cache", "EmuCore/CPU/Recompiler:EnableEECache=true"),
    ("cycle_rate", "EmuCore/Speedhacks:EECycleRate=-1"),
    ("cycle_rate_up", "EmuCore/Speedhacks:EECycleRate=2"),
    ("cycle_skip", "EmuCore/Speedhacks:EECycleSkip=2"),
    ("waitloop", "EmuCore/Speedhacks:WaitLoop=false"),
    ("intc", "EmuCore/Speedhacks:IntcStat=false"),
    ("vuflag", "EmuCore/Speedhacks:vuFlagHack=false"),
    ("vu1instant", "EmuCore/Speedhacks:vu1Instant=false"),
    ("mtvu", "EmuCore/Speedhacks:vuThread=true"),
    ("fastcdvd", "EmuCore/Speedhacks:fastCDVD=true"),
    ("extramem", "EmuCore/CPU:ExtraMemory=true"),
    ("gamefix", "EmuCore:EnableGameFixes=true;EmuCore/Gamefixes:EETimingHack=true"),
    ("hwdownload", "EmuCore/GS:HWDownloadMode=2"),
    ("nominal", "SPEED0=0.5"),
    ("turbo", "Framerate:TurboScalar=1"),
    ("framerate", "EmuCore/GS:FramerateNTSC=50"),
    ("hostsync", "EmuCore/GS:SyncToHostRefreshRate=true"),
)


def grep(path, rx):
    try:
        return zrig.log_lines(path, rx)
    except OSError:
        return []


def main(argv):
    if argv[:1] == ["list"]:
        for c in CASES:
            print("\t".join(c))
        return 0
    zrig.need("OUT", "RUN")
    out = os.environ["OUT"]
    os.makedirs(out, exist_ok=True)
    rc = 0
    for name, ini in CASES:
        if argv and name not in argv:
            continue
        for mode in zrig.env("MODES", "control forced").split():
            o = os.path.join(out, f"{name}-{mode}")
            env = "ZDXSV_SYNC_FORCE=0" if mode == "control" else ""
            pe = zrig.env("PCSX2_ENV")
            sp, i0 = (ini[len("SPEED0="):], "") if ini.startswith("SPEED0=") else ("", ini)
            t0 = time.time()
            with open(o + ".out", "wb") as f:
                r = subprocess.call([zrig.PY, "-u", os.path.join(zrig.HERE, "rbk.py"), "2", zrig.env("SEED", "1")],
                                    env=dict(os.environ, SPEED0=sp, INI0=i0, TIME=zrig.env("TIME", "30"), OUT=o,
                                             PCSX2_ENV=f"{env} {pe}".strip() if env else pe),
                                    stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
            # a rig failure (e.g. no session end) is FAIL; a run whose only failure is the sync check is DESYNC
            res = "PASS"
            if r:
                res = "DESYNC" if grep(o + ".out", "^FAIL coordinates") and not grep(o + ".out", "^FAIL p") else "FAIL"
            if mode == "forced" and res != "PASS":
                rc = 1
            seen = ""
            if mode == "control":
                s = grep(os.path.join(o, "emulog-p1.txt"), "ZdxsvSync: ZDXSV_SYNC_FORCE=0, not forced")
                seen = f" (unforced: {re.sub(r'.*not forced: ', '', s[0]) if s else 'none'})"
            print(f"{name} {mode} {res} {int(time.time() - t0)} s{seen}", flush=True)
    ini = os.path.join(os.environ["RUN"], "p1", "PCSX2", "inis", "PCSX2.ini")
    if os.path.isfile(ini + ".rbk0"):
        os.replace(ini + ".rbk0", ini)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
