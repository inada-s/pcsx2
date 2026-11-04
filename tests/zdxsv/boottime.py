"""Boot-to-lobby time: local stack (stack.py) + p1 from a cold boot; measures from the launch to the
lobby's user_id selection screen (ID選択, game RAM over PINE: bootwatch.py). No input by default.
  OUT=<dir> python tests/zdxsv/boottime.py
Env: LIMIT (s, default 5), MAXS (s to wait, default 200), ROUTE (drive.py keys sent from the start;
ROUTE=top,C = today's menu path + ログイン at TOP), CARD (p1 card name, default the first of CARDS),
SPEED (launch -Speed, default 1), SHOW=1 (windowed), LOBBY=fake (boot_lobby.py instead of the stack:
only the lobby API login and the lobby up to the user_id list), SAVE_CARD (path: p1's card after the run is copied there).
Exit 0 = the screen is up within LIMIT s. Logs: OUT/boottime.txt (net step changes), stack logs.
"""
import os
import shutil
import subprocess
import sys
import time

import launch
import zbincheck
import zrig

API = "127.0.0.1:9881"


def run(out):
    run_ = os.environ["RUN"]
    p1 = os.path.join(run_, "p1", "PCSX2")
    card = zrig.env("CARD") or os.environ["CARDS"].split()[0]
    os.makedirs(out, exist_ok=True)
    shutil.copyfile(os.path.join(run_, "zdxsv.db"), os.path.join(out, "zdxsv.db"))
    procs = []

    def killall():
        zrig.kill_name("pcsx2", "zdxsv", "dnas")

    def cleanup():
        for p in procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
        pf = os.path.join(out, "boot_lobby.pid")
        if os.path.isfile(pf):
            with open(pf) as f:
                zrig.kill(f.read().strip() or 0)
        killall()
        try:
            shutil.copyfile(os.path.join(p1, "logs", "emulog.txt"), os.path.join(out, "emulog-p1.txt"))
        except OSError:
            pass
        mc = os.path.join(p1, "memcards", "Mcd001.ps2")
        if zrig.env("SAVE_CARD") and os.path.isfile(mc):
            shutil.copyfile(mc, os.environ["SAVE_CARD"])
        live = os.path.join(out, "live-p1.ps2")
        if os.path.isfile(live):
            shutil.copy2(live, os.path.join(p1, "memcards", "Mcd001.ps2"))

    killall()
    try:
        if zrig.env("LOBBY") == "fake":
            pf = os.path.join(out, "boot_lobby.pid")
            if os.path.exists(pf):
                os.remove(pf)
            log = os.path.join(out, "boot_lobby.log")
            with open(log, "wb") as f:
                procs.append(subprocess.Popen([zrig.PY, "-u", os.path.join(zrig.HERE, "boot_lobby.py"), "--public",
                                               f"{os.environ['IP']}:8200", "--api", API, "--pidfile", pf],
                                              stdout=f, stderr=subprocess.STDOUT))
            time.sleep(2)
            txt = open(log, encoding="utf-8", errors="replace").read()
            if "boot lobby up" not in txt:
                print(txt + "FAIL boot lobby", flush=True)
                return 1
        else:
            if zbincheck.check(zrig.env("ZBIN") or os.path.join(os.environ["ZDXSV_DIR"], "bin", "zdxsv" + zrig.EXE)):
                return 1
            so = os.path.join(out, "stack.out")
            e = dict(os.environ, ZDXSV_LOBBY_API_ADDR=API, LOBBY_ARGS="-v=2", RUN=out)
            with open(so, "wb") as f:
                procs.append(subprocess.Popen([zrig.PY, os.path.join(zrig.HERE, "stack.py"), os.environ["IP"]],
                                              env=e, stdout=f, stderr=subprocess.STDOUT))
            time.sleep(6)
            txt = open(so, encoding="utf-8", errors="replace").read()
            if "stack up" not in txt:
                print(txt + "FAIL stack", flush=True)
                return 1
        emulog = os.path.join(p1, "logs", "emulog.txt")
        if os.path.exists(emulog):
            os.remove(emulog)
        shutil.copy2(os.path.join(p1, "memcards", "Mcd001.ps2"), os.path.join(out, "live-p1.ps2"))
        t0 = time.time()
        # ZDXSV_TLS=0: the local stack has no TLS ports. Fast login API: the lobby API, /lbs/login
        ex = {"ZDXSV_TLS": "0", "ZDXSV_LOGIN_API_URL": zrig.env("ZDXSV_LOGIN_API_URL", f"http://{API}/lbs/login")}
        pids = launch.launch([1], speed=zrig.env("SPEED", "1"), memcard=os.path.join(run_, f"Mcd001-{card}.ps2"),
                             headless=not zrig.env("SHOW"), extra_env=ex)
        a = [zrig.PY, os.path.join(zrig.HERE, "bootwatch.py"), "--t0", str(t0), "--pid", str(pids[1]),
             "--emulog", emulog, "--out", out, "--limit", zrig.env("LIMIT", "5"), "--maxs", zrig.env("MAXS", "200")]
        if zrig.env("ROUTE"):
            a += ["--route", os.environ["ROUTE"]]
        return subprocess.call(a)
    finally:
        cleanup()


def main():
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "OUT", "CARDS")
    return run(os.environ["OUT"])


if __name__ == "__main__":
    sys.exit(main())
