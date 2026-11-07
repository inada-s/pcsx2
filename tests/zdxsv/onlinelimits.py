"""Online limits at the lobby: p1 fast boots and fast logs in to boot_lobby.py (any token is its login_key, so a
dummy token works), which makes it online; then one case runs.
  OUT=<dir> python tests/zdxsv/onlinelimits.py pine|state
pine:  ZDXSV_PINE_ONLINE=0; exit 0 = online, then a PINE request fails and the emulog has `PINE requests refused`.
state: PINE save and load (slot 9) are refused (`refused while online` twice) and no state file is written.
NOLOBBY=1 (control, must fail): no boot_lobby.py, the game never goes online, PINE and the save work.
Env: MAXS (s to wait for online, default 60). About 10 s each (control 20 s).
"""
import os
import subprocess
import sys
import time

import launch
import zrig

SS = "SLPS-25419 (435D8236).09.p2s"
REFUSED = "ZdxsvOnline: refused while online"


def main():
    case = sys.argv[1] if len(sys.argv) > 1 else ""
    if case not in ("pine", "state"):
        print(__doc__)
        return 2
    zrig.take()
    zrig.need("RUN", "OUT", "IP")
    out = os.environ["OUT"]
    os.makedirs(out, exist_ok=True)
    p1 = os.path.join(os.path.abspath(os.environ["RUN"]), "p1", "PCSX2")
    emulog = os.path.join(p1, "logs", "emulog.txt")
    ss = os.path.join(p1, "sstates", SS)
    token_file = os.path.join(p1, "inis", "zdxsv_auth_token.txt")
    for f in (emulog, ss):
        if os.path.exists(f):
            os.remove(f)

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
        # boot_lobby.py answers the token as the login_key: 16 characters at most
        pids = launch.launch([1], headless=True, extra_env=ex, token="onlinelimitstest")
        online = wait_log("ZdxsvOnline: online", int(zrig.env("MAXS", "60")))
        checks.append(("online", online))
        time.sleep(2)
        if case == "pine":
            rc = pine("status")
            time.sleep(1)
            checks.append(("PINE refused", rc != 0 and "PINE requests refused while online" in log()))
        else:
            pine("save", "9")
            time.sleep(3)
            pine("load", "9")
            time.sleep(1)
            checks.append(("save and load refused", log().count(REFUSED) >= 2))
            checks.append(("no state file", not os.path.exists(ss)))
    finally:
        if lobby:
            lobby.kill()
        zrig.kill(*pids.values())
        time.sleep(1)
        for f in (token_file, ss):
            if os.path.exists(f):
                os.remove(f)
        with open(os.path.join(out, "emulog-p1.txt"), "w", encoding="utf-8") as f:
            f.write(log())
    for name, ok in checks:
        print(("ok   " if ok else "FAIL ") + name, flush=True)
    passed = bool(checks) and all(ok for _, ok in checks)
    print(f"onlinelimits {case}: {'PASS' if passed else 'FAIL'}", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
