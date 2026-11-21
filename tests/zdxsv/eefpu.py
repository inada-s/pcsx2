#!/usr/bin/env python3
"""EE FPU interpreter as the recompiler: ZDXSV_EE_OPTEST with finite inputs on the EE recompiler and on the
EE interpreter of the same build, then ee_optest_cmp.py on Fd of the ops whose rounding the interpreter takes
from the recompiler on every build.

usage: eefpu.py   (RUN, ISO, PCSX2_EXE as launch.py; no network)
Env: OUT (default $RUN/eefpu), N (instance, default 9), CASES (default 512).
Instance N boots headless twice (its ini restored after); about 3 min.
exit 0: both runs log `EE optest: done ... intact`, only the second logs the EE recompiler off, and no
compared case differs.
"""
import os
import shutil
import subprocess
import sys
import time

import launch
import zrig

OPS = "ADD,SUB,MUL,DIV,ADDA,SUBA,MULA,MADD,MSUB,MADDA,MSUBA"
EE_OFF = "EE Recompiler is not enabled"


def run(out, n, mode, cases):
    pd = os.path.join(os.environ["RUN"], f"p{n}", "PCSX2")
    ini, log = os.path.join(pd, "inis", "PCSX2.ini"), os.path.join(pd, "logs", "emulog.txt")
    data = os.path.join(out, f"{mode}.bin")
    for p in (data, log):
        if os.path.exists(p):
            os.remove(p)
    if not os.path.exists(ini):
        launch.launch(ns=(n,), headless=True, quiet=True)
        zrig.kill_name(os.path.basename(launch.exe_path()))
        zrig.release()
    saved = os.path.join(out, "PCSX2.ini")
    shutil.copy(ini, saved)
    zrig.ini_set(ini, "EmuCore/CPU/Recompiler", "EnableEE", "false" if mode == "interp" else None)
    env = {"ZDXSV_EE_OPTEST": f"{data},{cases},finite"}
    if mode == "interp":
        env["ZDXSV_SYNC_FORCE"] = "0"
    try:
        pid = launch.launch(ns=(n,), headless=True, extra_env=env, quiet=True)[n]
        end = time.time() + 300
        while time.time() < end and not os.path.exists(data) and zrig.alive(pid):
            time.sleep(2)
        time.sleep(2)
        zrig.kill(pid)
    finally:
        zrig.release()
        time.sleep(2)
        shutil.copy(saved, ini)
    elog = os.path.join(out, f"{mode}.log")
    if os.path.exists(log):
        shutil.copy(log, elog)
    done = os.path.exists(elog) and zrig.log_lines(elog, r"EE optest: done .*intact")
    off = os.path.exists(elog) and bool(zrig.log_lines(elog, EE_OFF))
    ok = bool(done) and os.path.exists(data) and off == (mode == "interp")
    print(f"{'PASS' if ok else 'FAIL'} {mode}: optest done {bool(done)}, file {os.path.exists(data)}, "
          f"EE recompiler {'off' if off else 'on'}", flush=True)
    return ok, data, elog


def main():
    zrig.need("RUN", "ISO")
    out = os.path.abspath(os.environ.get("OUT") or os.path.join(os.environ["RUN"], "eefpu"))
    os.makedirs(out, exist_ok=True)
    n, cases = int(os.environ.get("N", "9")), os.environ.get("CASES", "512")
    ok_r, rec, rlog = run(out, n, "rec", cases)
    ok_i, itp, _ = run(out, n, "interp", cases)
    if not (ok_r and ok_i):
        return 1
    cmp = os.path.join(zrig.HERE, "ee_optest_cmp.py")
    r = subprocess.run([sys.executable, cmp, rec, itp, rlog, "2", "-ops", OPS, "-fields", "fd"])
    print(f"{'PASS' if r.returncode == 0 else 'FAIL'} interpreter Fd as the recompiler ({OPS})")
    return 0 if r.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
