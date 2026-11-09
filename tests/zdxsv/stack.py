"""Local zdxsv stack without docker: battle, lobby, login, dnas.
Usage: python tests/zdxsv/stack.py [IP]   (IP = address pcsx2 peers dial; default first non-loopback)
Logs + db: $RUN. Runs in the foreground; Ctrl-C / kill stops all.
Env: ZDXSV_DIR (checkout), ZBIN (another build, e.g. a control), LOBBY_ARGS (-v=2: every message), BATTLE_ARGS (the same for the battle server),
DZ (exe for `dnas`, default ZBIN), DNAS (a standalone DNAS server instead), ZDXSV_LOBBY_API_ADDR.
A ZBIN with no `lobby` command runs as one `zdxsv all` (no DNS on :53, no battle RPC on 3080;
DZ and DNAS ignored) into all.log, hard-linked as lobby.log, battle.log, login.log and dnas.log.
"""
import os
import subprocess
import sys
import time

import zbincheck
import zrig


def has_lobby_cmd(z, env):
    """The usage line of an unknown command lists the commands."""
    r = zrig.run([z, "stack-py-usage"], env=env)
    return " lobby," in r.stdout + r.stderr


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
    split = has_lobby_cmd(z, e)
    if not split and (e.get("DNAS") or e.get("DZ")):
        print(f"stack: DZ/DNAS ignored, {z} runs dnas inside `zdxsv all`", flush=True)
    if not os.path.isfile("zdxsv.db"):
        with open("initdb.log", "wb") as f:
            subprocess.call([z, "initdb"], env=e, stdout=f, stderr=subprocess.STDOUT)
    procs = []

    def start(args, log, env=e):
        with open(log, "wb") as f:
            procs.append(subprocess.Popen(args, env=env, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT))

    try:
        if split:
            start([z, *e.get("BATTLE_ARGS", "").split(), "battle"], "battle.log")
            time.sleep(2)
            start([z, *e.get("LOBBY_ARGS", "").split(), "lobby"], "lobby.log")
            start([z, "login"], "login.log")
            if e.get("DNAS"):
                start([e["DNAS"], "-login", "127.0.0.1:80"], "dnas.log")
            else:
                start([e.get("DZ") or z, "dnas"], "dnas.log", dict(e, ZDXSV_LOGIN_ADDR="127.0.0.1:80"))
        else:
            args = dict.fromkeys(e.get("BATTLE_ARGS", "").split() + e.get("LOBBY_ARGS", "").split())
            start([z, *args, "all"], "all.log", dict(e, ZDXSV_DNS_ADDR="127.0.0.1:0"))
            for name in ("lobby.log", "battle.log", "login.log", "dnas.log"):
                if os.path.lexists(name):
                    os.remove(name)
                os.link("all.log", name)
        time.sleep(1)
        dead = [p.args for p in procs if p.poll() is not None]
        if dead:
            print(f"FAIL stack: exited at start: {dead}", flush=True)
            return 1
        print(f"stack up on {ip}{'' if split else ' (zdxsv all)'}: pids {' '.join(str(p.pid) for p in procs)}", flush=True)
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
