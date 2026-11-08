"""Reset, then the lobby again: p1 fast boots and fast logs in to boot_lobby.py (online at the user_id list), the
lobby stops (p1 goes offline, so a reset is allowed), a new boot_lobby.py starts, F11 resets the VM (bound in p1's
ini for the run). Fast boot runs again after the reset and connects.
  OUT=<dir> python tests/zdxsv/lobbyreset.py
Exit 0 = online, offline, reset, then online again with the second lobby's user_id list sent.
Env: MAXS (s to wait for each online, default 90). About 3 min (the game drops a silent lobby after about 2 min).
"""
import os
import shutil
import subprocess
import sys
import time

import launch
import zrig

ONLINE = "ZdxsvOnline: online"
OFFLINE = "ZdxsvOnline: offline"


def main():
    zrig.take()
    zrig.need("RUN", "OUT", "IP")
    out = os.environ["OUT"]
    os.makedirs(out, exist_ok=True)
    maxs = int(zrig.env("MAXS", "90"))
    p1 = os.path.join(os.path.abspath(os.environ["RUN"]), "p1", "PCSX2")
    emulog = os.path.join(p1, "logs", "emulog.txt")
    token_file = os.path.join(p1, "inis", "zdxsv_auth_token.txt")
    ini = os.path.join(p1, "inis", "PCSX2.ini")
    ini_kept = os.path.join(out, "PCSX2.ini.before")
    if os.path.exists(emulog):
        os.remove(emulog)
    shutil.copyfile(ini, ini_kept)
    with open(ini, encoding="utf-8", errors="surrogateescape") as f:
        lines = [l for l in f.read().splitlines() if not l.startswith("ResetVM = ")]
    if "[Hotkeys]" not in lines:
        lines.append("[Hotkeys]")
    lines.insert(lines.index("[Hotkeys]") + 1, "ResetVM = Keyboard/F11")
    with open(ini, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    def log():
        try:
            with open(emulog, encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait(cond, s):
        end = time.time() + s
        while time.time() < end:
            if cond():
                return True
            time.sleep(0.5)
        return False

    def lobby_path(n):
        return os.path.join(out, f"boot_lobby{n}.log")

    def start_lobby(n):
        return subprocess.Popen([zrig.PY, "-u", os.path.join(zrig.HERE, "boot_lobby.py"), "--public",
                                 f"{os.environ['IP']}:8200", "--api", "127.0.0.1:9881"],
                                stdout=open(lobby_path(n), "wb"), stderr=subprocess.STDOUT)

    def lobby_log(n):
        try:
            with open(lobby_path(n), encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    lobby = None
    pids = {}
    checks = []
    try:
        lobby = start_lobby(1)
        time.sleep(1)
        # ZDXSV_TLS=0: boot_lobby.py has no TLS port; it answers the token as the login_key (16 chars max)
        ex = {"ZDXSV_TLS": "0", "ZDXSV_FAST_BOOT": "1", "ZDXSV_FAST_LOGIN": "1"}
        pids = launch.launch([1], extra_env=ex, token="lobbyresettest")
        checks.append(("online", wait(lambda: ONLINE in log(), maxs)))
        lobby.kill()
        lobby.wait()
        checks.append(("offline after the lobby stops", wait(lambda: OFFLINE in log(), 180)))
        lobby = start_lobby(2)
        time.sleep(1)
        zrig.ctl(pids[1], "w1000,F11,w1000")
        checks.append(("reset", wait(lambda: log().count("Zdxsv: fast boot: on") >= 2, 20)))
        checks.append(("online again", wait(lambda: log().count(ONLINE) >= 2, maxs)))
        checks.append(("second lobby reached", "user_id list sent" in lobby_log(2)))
    finally:
        if lobby:
            lobby.kill()
        zrig.kill(*pids.values())
        time.sleep(1)
        if os.path.exists(token_file):
            os.remove(token_file)
        os.replace(ini_kept, ini)
        with open(os.path.join(out, "emulog-p1.txt"), "w", encoding="utf-8") as f:
            f.write(log())
    for name, ok in checks:
        print(("ok   " if ok else "FAIL ") + name, flush=True)
    passed = len(checks) == 5 and all(ok for _, ok in checks)
    print(f"lobbyreset: {'PASS' if passed else 'FAIL'}", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
