#!/usr/bin/env python3
"""A text font that does not load (a broken file in the user's resources/fonts) must not crash GS open.

usage: badfont.py   (RUN, PCSX2_EXE as launch.py; Windows; no game, no network)
Writes junk to <datapath>/resources/fonts/Roboto-Regular.ttf (the first text font), starts with -bigpicture.
exit 0: alive 12 s after the start.
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
        print("SKIP: Windows only")
        return 0
    root = os.path.join(os.environ["RUN"], "badfont")
    shutil.rmtree(root, ignore_errors=True)
    inis = os.path.join(root, "PCSX2", "inis")
    os.makedirs(inis)
    with open(os.path.join(inis, "PCSX2.ini"), "w") as f:
        f.write(INI)
    fonts = os.path.join(root, "PCSX2", "resources", "fonts")
    os.makedirs(fonts)
    with open(os.path.join(fonts, "Roboto-Regular.ttf"), "wb") as f:
        f.write(bytes(range(256)) * 16)
    p = subprocess.Popen([launch.exe_path(), "-bigpicture", "-datapath", root], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 12
    while time.time() < end and p.poll() is None:
        time.sleep(1)
    rc = p.poll()
    if rc is None:
        p.kill()
        p.wait()
    code = "none" if rc is None else hex(rc & 0xFFFFFFFF)
    ok = rc is None
    print(f"{'PASS' if ok else 'FAIL'}: exit {code}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
