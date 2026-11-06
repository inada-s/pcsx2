"""Local zdxsv stack without docker: battle, lobby, login, dnas.
Usage: python tests/zdxsv/stack.py [IP]   (IP = address pcsx2 peers dial; default first non-loopback)
Logs + db: $RUN. Runs in the foreground; Ctrl-C / kill stops all.
Env: ZDXSV_DIR (checkout), ZBIN (another build, e.g. a control), LOBBY_ARGS (-v=2: every message), BATTLE_ARGS (the same for the battle server),
DZ (exe for `dnas`, default ZBIN), DNAS (a standalone DNAS server instead), ZDXSV_LOBBY_API_ADDR.
"""
import os
import subprocess
import sys
import time

import zbincheck
import zrig


def main(argv):
    zrig.take()  # one rig at a time; nested under m4z.py
    zrig.need("ZDXSV_DIR")
    ip = argv[0] if argv else zrig.lanip()
    run = os.path.abspath(os.environ["RUN"])
    os.makedirs(run, exist_ok=True)
    os.chdir(run)
    e = dict(os.environ)
    e.update({
        "ZDXSV_DNAS_PUBLIC_ADDR": ip,
        "ZDXSV_LOGIN_ADDR": ":80", "ZDXSV_LOGIN_PUBLIC_ADDR": ip,
        "ZDXSV_LOBBY_ADDR": ":8200", "ZDXSV_LOBBY_RPC_ADDR": "127.0.0.1:8201", "ZDXSV_LOBBY_PUBLIC_ADDR": f"{ip}:8200",
        "ZDXSV_BATTLE_ADDR": ":8210", "ZDXSV_BATTLE_RPC_ADDR": "127.0.0.1:3080", "ZDXSV_BATTLE_PUBLIC_ADDR": f"{ip}:8210",
        "ZDXSV_STATUS_ADDR": "127.0.0.1:8080", "ZDXSV_DB_NAME": "zdxsv.db",
        "ZDXSV_LOBBY_API_ADDR": e.get("ZDXSV_LOBBY_API_ADDR") or "127.0.0.1:9881",
    })
    z = e.get("ZBIN") or os.path.join(e["ZDXSV_DIR"], "bin", "zdxsv" + zrig.EXE)
    if zbincheck.check(z):
        return 1
    if not os.path.isfile("zdxsv.db"):
        with open("initdb.log", "wb") as f:
            subprocess.call([z, "initdb"], env=e, stdout=f, stderr=subprocess.STDOUT)
    procs = []

    def start(args, log, env=e):
        with open(log, "wb") as f:
            procs.append(subprocess.Popen(args, env=env, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT))

    try:
        start([z, *e.get("BATTLE_ARGS", "").split(), "battle"], "battle.log")
        time.sleep(2)
        start([z, *e.get("LOBBY_ARGS", "").split(), "lobby"], "lobby.log")
        start([z, "login"], "login.log")
        if e.get("DNAS"):
            start([e["DNAS"], "-login", "127.0.0.1:80"], "dnas.log")
        else:
            start([e.get("DZ") or z, "dnas"], "dnas.log", dict(e, ZDXSV_LOGIN_ADDR="127.0.0.1:80"))
        print(f"stack up on {ip}: pids {' '.join(str(p.pid) for p in procs)}", flush=True)
        for p in procs:
            p.wait()
    except KeyboardInterrupt:
        pass
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
