"""Fast login again after ネット終了: netend.py, then 通信対戦 → card prompt → broadband notice → connect
menu → DNAS start by pad keys.
  OUT=<dir> python tests/zdxsv/relogin.py
Env as netend.py; WATCH_AGAIN (s, default 60): time for the second login after the keys.
Exit 0 = emulog has two `fast login: session` lines and the lobby sent the user_id list (0x6131) twice.
"""
import os
import subprocess
import sys
import time

import boottime
import netend
import zrig

KEYS = "w16000,C" + ",w4000,Up,w400,C" * 18


def counts(out):
    emulog = os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt")
    with open(emulog, encoding="utf-8", errors="replace") as f:
        logins = f.read().count("Zdxsv: fast login: session")
    with open(os.path.join(out, "lobby.log"), encoding="utf-8", errors="replace") as f:
        lists = sum(1 for s in f if "ID:0x6131" in s)
    return logins, lists


def after(out):
    if netend.after(out):
        return 1
    rc = subprocess.call([zrig.PY, os.path.join(zrig.HERE, "drive.py"), "seq", "1", KEYS, "relogin"])
    print("relogin keys rc", rc, flush=True)
    deadline = time.time() + float(zrig.env("WATCH_AGAIN", "60"))
    while True:
        logins, lists = counts(out)
        if (logins >= 2 and lists >= 2) or time.time() > deadline:
            break
        time.sleep(1)
    ok = logins >= 2 and lists >= 2
    print(f"{'PASS' if ok else 'FAIL'}: fast logins {logins}, user_id lists {lists}")
    return 0 if ok else 1


def main():
    os.environ.setdefault("ROUTE", netend.ROUTE)
    os.environ.setdefault("ROUTE_END", "1")
    os.environ.setdefault("LIMIT", "60")
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "OUT", "CARDS")
    return boottime.run(os.environ["OUT"], after)


if __name__ == "__main__":
    sys.exit(main())
