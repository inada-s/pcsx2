"""Online limits at the lobby: p1 fast boots and fast logs in to boot_lobby.py (any token is its login_key, so a
dummy token works), which makes it online; then one case runs.
  OUT=<dir> python tests/zdxsv/onlinelimits.py pine|state|forced|hotkeys|breakpoints
pine:    ZDXSV_PINE_ONLINE=0; exit 0 = online, then a PINE request fails and the emulog has `PINE requests refused`.
state:   PINE save and load (slot 9) are refused (`refused while online` twice) and no state file is written.
forced:  p1's ini gets manual hardware fixes and texture dumping on (put back after); going online forces both off.
hotkeys: a shown window gets F1 (save state), F9 (software renderer), Shift+F8 (GS dump): each refused, no state
         file (p1's ini binds them so; the keys take the foreground for a moment).
breakpoints: ZDXSV_TEST_BREAKPOINT puts a breakpoint at an address that never runs at boot; going online clears it.
NOLOBBY=1 (control, must fail): no boot_lobby.py, the game never goes online, nothing is refused or forced.
Env: MAXS (s to wait for online, default 60). About 10 s each (control 20 s, MAXS=15).
"""
import os
import re
import shutil
import subprocess
import sys
import time

import launch
import zrig

SS = "SLPS-25419 (435D8236).%02d.p2s"  # PINE saves to slot 9, F1 to the current slot (1 after a launch)
REFUSED = "ZdxsvOnline: refused while online"


def set_gs(ini, values):
    with open(ini, encoding="utf-8", errors="surrogateescape") as f:
        lines = f.read().splitlines()
    out, section = [], ""
    for l in lines:
        if l.startswith("["):
            section = l
        m = re.match(r"(\w+) = ", l)
        if section == "[EmuCore/GS]" and m and m[1] in values:
            l = f"{m[1]} = {values[m[1]]}"
        out.append(l)
    with open(ini, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def main():
    case = sys.argv[1] if len(sys.argv) > 1 else ""
    if case not in ("pine", "state", "forced", "hotkeys", "breakpoints"):
        print(__doc__)
        return 2
    zrig.take()
    zrig.need("RUN", "OUT", "IP")
    out = os.environ["OUT"]
    os.makedirs(out, exist_ok=True)
    p1 = os.path.join(os.path.abspath(os.environ["RUN"]), "p1", "PCSX2")
    emulog = os.path.join(p1, "logs", "emulog.txt")
    states = [os.path.join(p1, "sstates", SS % n) for n in (1, 9)]
    token_file = os.path.join(p1, "inis", "zdxsv_auth_token.txt")
    ini = os.path.join(p1, "inis", "PCSX2.ini")
    ini_kept = os.path.join(out, "PCSX2.ini.before")
    for f in [emulog] + states:
        if os.path.exists(f):
            os.remove(f)
    if case == "forced":
        shutil.copyfile(ini, ini_kept)
        set_gs(ini, {"UserHacks": "true", "DumpReplaceableTextures": "true"})

    def log():
        try:
            with open(emulog, encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(s, maxs):
        end = time.time() + maxs
        while time.time() < end:
            if s in log():
                return True
            time.sleep(0.5)
        return False

    def pine(*a):
        r = zrig.run([zrig.PY, os.path.join(zrig.HERE, "pine.py"), "--slot", "28011"] + list(a))
        return r.returncode

    lobby = None
    pids = {}
    checks = []
    try:
        if not zrig.env("NOLOBBY"):
            lobby = subprocess.Popen([zrig.PY, "-u", os.path.join(zrig.HERE, "boot_lobby.py"), "--public",
                                      f"{os.environ['IP']}:8200", "--api", "127.0.0.1:9881"],
                                     stdout=open(os.path.join(out, "boot_lobby.log"), "wb"), stderr=subprocess.STDOUT)
            time.sleep(1)
        # ZDXSV_TLS=0: boot_lobby.py has no TLS port
        ex = {"ZDXSV_TLS": "0", "ZDXSV_FAST_BOOT": "1", "ZDXSV_PINE_ONLINE": "0" if case == "pine" else "1"}
        if case == "breakpoints":
            ex["ZDXSV_TEST_BREAKPOINT"] = "1fffff00"  # top of EE RAM: stack, never code
        # boot_lobby.py answers the token as the login_key: 16 characters at most
        pids = launch.launch([1], headless=case != "hotkeys", extra_env=ex, token="onlinelimitstest")
        online = wait_log("ZdxsvOnline: online", int(zrig.env("MAXS", "60")))
        checks.append(("online", online))
        time.sleep(2)
        if case == "pine":
            rc = pine("status")
            time.sleep(1)
            checks.append(("PINE refused", rc != 0 and "PINE requests refused while online" in log()))
        elif case == "forced":
            forced = [l for l in log().splitlines() if "ZdxsvSync: forced to defaults" in l]
            checks.append(("hardware fixes forced off", any("manual hardware renderer fixes" in l for l in forced)))
            checks.append(("texture dumping forced off", any("texture dumping" in l for l in forced)))
        elif case == "breakpoints":
            checks.append(("test breakpoint set", "ZdxsvOnline: test breakpoint at 1fffff00" in log()))
            checks.append(("breakpoints cleared", "ZdxsvOnline: debugger breakpoints cleared" in log()))
        elif case == "hotkeys":
            zrig.ctl(pids[1], "F1,w1000,F9,w1000,Shift+F8,w1000")
            checks.append(("3 hotkeys refused", log().count(REFUSED) >= 3))
            checks.append(("no state file", not any(os.path.exists(s) for s in states)))
        else:
            pine("save", "9")
            time.sleep(3)
            pine("load", "9")
            time.sleep(1)
            checks.append(("save and load refused", log().count(REFUSED) >= 2))
            checks.append(("no state file", not any(os.path.exists(s) for s in states)))
    finally:
        if lobby:
            lobby.kill()
        zrig.kill(*pids.values())
        time.sleep(1)
        for f in [token_file] + states:
            if os.path.exists(f):
                os.remove(f)
        if os.path.exists(ini_kept):
            os.replace(ini_kept, ini)
        with open(os.path.join(out, "emulog-p1.txt"), "w", encoding="utf-8") as f:
            f.write(log())
    for name, ok in checks:
        print(("ok   " if ok else "FAIL ") + name, flush=True)
    passed = bool(checks) and all(ok for _, ok in checks)
    print(f"onlinelimits {case}: {'PASS' if passed else 'FAIL'}", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
