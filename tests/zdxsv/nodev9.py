"""Online without DEV9: p1 boots with ZDXSV_SERVER_HOST=$IP, the fast login on and ZDXSV_HTTPS_LATENCY=0 (DEV9 adapter off, launch.py),
against the local stack (boottime.py), route top to the lobby's user_id selection.
  OUT=<dir> python tests/zdxsv/nodev9.py
Env: MAXS (s to wait, default 200), NODEV9 (ZDXSV_SERVER_HOST, default $IP).
Exit 0 = the user_id screen is up and p1 logged no-DEV9 on, a fast login session with the lobby on $IP,
DNAS got no POST, no connect refused, and pcsx2 had TCP to $IP only on the lobby (8200, TLS 8220) and login API (9881)
ports, and to no other host but 127.0.0.1.
"""
import os
import re
import subprocess
import sys
import threading

import zrig

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    zrig.need("IP", "OUT")
    ip, out = os.environ["IP"], os.environ["OUT"]
    nodev9 = zrig.env("NODEV9", ip)
    maxs = zrig.env("MAXS", "200")
    os.makedirs(out, exist_ok=True)
    local = re.compile(r"^(0\.0\.0\.0|::|127\.0\.0\.1):|^" + re.escape(ip) + r":(8200|8220|9881)$")
    remote, samples, done = [], [0], threading.Event()

    def sample():
        while not done.is_set():
            remote.extend(r for r in zrig.remote(*zrig.pids_name("pcsx2")) if not local.match(r))
            samples[0] += 1
            done.wait(5)

    t = threading.Thread(target=sample, daemon=True)
    t.start()
    e = dict(os.environ, ZDXSV_SERVER_HOST=nodev9, ZDXSV_FAST_LOGIN="1", ZDXSV_HTTPS_LATENCY="0", ROUTE="top", LIMIT=maxs, MAXS=maxs)
    rc = subprocess.call([zrig.PY, os.path.join(HERE, "boottime.py")], env=e)
    done.set()
    t.join()
    with open(os.path.join(out, "remote.txt"), "w") as f:
        f.write("".join(r + "\n" for r in remote))
    log = os.path.join(out, "emulog-p1.txt")
    lines = zrig.log_lines(log, "Zdxsv: ") if os.path.isfile(log) else []
    dnas = os.path.join(out, "dnas.log")
    checks = [
        (f"user_id screen (boottime.py rc {rc})", rc == 0),
        (f"no DEV9 on, names resolve to {nodev9}",
         any(re.search(r"Zdxsv: no DEV9: .*names resolve to " + re.escape(nodev9), l) for l in lines)),
        (f"fast login session, lobby {ip}",
         any(re.search(r"Zdxsv: fast login: session [0-9]*, lobby " + re.escape(ip) + ":", l) for l in lines)),
        ("DNAS: no POST", os.path.isfile(dnas) and b"POST" not in open(dnas, "rb").read()),
        ("no connect refused", not any("connect to" in l and "refused" in l for l in lines)),
        ("pcsx2: TCP only to the lobby and the login API", samples[0] >= 3 and not remote),
    ]
    for name, ok in checks:
        print(("PASS " if ok else "FAIL ") + name)
    return 0 if all(ok for _, ok in checks) else 1


if __name__ == "__main__":
    sys.exit(main())
