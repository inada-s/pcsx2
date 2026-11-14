#!/usr/bin/env python3
"""Pad macro on a controller with unnamed binding slots: PCSX2 must start and keep the macro.

NeGcon has an unnamed slot before Start, so looking up Start passes it.
usage: padmacro.py   (RUN, PCSX2_EXE as launch.py; no game, no network)
exit 0: alive after 30 s and no "Invalid bind" in the log.
"""
import os
import shutil
import subprocess
import sys
import time

import launch
import zrig

INI = """[UI]
SettingsVersion = 1
SetupWizardIncomplete = false
Language = en-US
[AutoUpdater]
CheckAtStartup = false
[DEV9/Eth]
ZdxsvCrashReports = false
[Pad1]
Type = NeGcon
Macro1Binds = Start
"""


def main():
    zrig.need("RUN")
    root = os.path.join(os.environ["RUN"], "padmacro")
    shutil.rmtree(root, ignore_errors=True)
    inis = os.path.join(root, "PCSX2", "inis")
    os.makedirs(inis)
    with open(os.path.join(inis, "PCSX2.ini"), "w") as f:
        f.write(INI)
    p = subprocess.Popen([launch.exe_path(), "-datapath", root], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 30
    while time.time() < end and p.poll() is None:
        time.sleep(1)
    rc = p.poll()
    if rc is None:
        p.kill()
        p.wait()
    log = os.path.join(root, "PCSX2", "logs", "emulog.txt")
    has_log = os.path.exists(log)
    bad = zrig.log_lines(log, "Invalid bind") if has_log else []
    ok = rc is None and has_log and not bad
    code = "none" if rc is None else hex(rc & 0xFFFFFFFF)
    print(f"{'PASS' if ok else 'FAIL'}: exit {code}, invalid binds {len(bad)}, log {'yes' if has_log else 'no'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
