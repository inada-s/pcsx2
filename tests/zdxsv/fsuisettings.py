#!/usr/bin/env python3
"""Big Picture settings: opening Settings (Interface page) in the fullscreen UI must not crash.

usage: fsuisettings.py   (RUN, PCSX2_EXE as launch.py; Windows; no game, no network)
Starts with -bigpicture, then Right, Right, Return on the landing page (Game List, Start Game, Settings),
then Down*20 through the Interface page.
exit 0: alive 10 s after the keys.
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
"""


def main():
    zrig.need("RUN")
    if not zrig.WIN:
        print("SKIP: Windows only (pcsx2ctl.ps1)")
        return 0
    root = os.path.join(os.environ["RUN"], "fsuisettings")
    shutil.rmtree(root, ignore_errors=True)
    inis = os.path.join(root, "PCSX2", "inis")
    os.makedirs(inis)
    with open(os.path.join(inis, "PCSX2.ini"), "w") as f:
        f.write(INI)
    p = subprocess.Popen([launch.exe_path(), "-bigpicture", "-datapath", root], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(8)
    ctl = subprocess.call(["powershell", "-NoProfile", "-File", os.path.join(zrig.HERE, "pcsx2ctl.ps1"), "-AnyKey",
                           "-ProcId", str(p.pid), "-Seq", "Right,w500,Right,w500,Return,w2000,Down*20,w500"])
    end = time.time() + 10
    while time.time() < end and p.poll() is None:
        time.sleep(1)
    rc = p.poll()
    if rc is None:
        p.kill()
        p.wait()
    code = "none" if rc is None else hex(rc & 0xFFFFFFFF)
    ok = rc is None and ctl == 0
    print(f"{'PASS' if ok else 'FAIL'}: exit {code}, keys rc {ctl}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
