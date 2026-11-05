"""ネット終了 from the lobby: boottime.py with ID 1 chosen (戦場選択), then ネット終了 → はい by pad keys.
  OUT=<dir> python tests/zdxsv/netend.py
Env as boottime.py (ROUTE and ROUTE_END preset); LIMIT_END (s, default 5): the logout answer (0x6006)
to the closed lobby connection, WATCH (s, default 30) to wait for it.
Exit 0 = the lobby sees the game's connection close within LIMIT_END s of the logout.
"""
import os
import re
import subprocess
import sys
import time
from datetime import datetime

import boottime
import zrig

ROUTE = "f3300,C,w3000,C,w4000"
KEYS = "w2000,Start,w1500" + ",Down,w600" * 7 + ",w400,C,w2500,Up,w800,C"
STAMP = re.compile(r"^I\d{4} (\d\d:\d\d:\d\d\.\d+)")


def stamp(line):
    m = STAMP.match(line)
    return datetime.strptime(m.group(1), "%H:%M:%S.%f") if m else None


def find(lines, text):
    return next((stamp(s) for s in lines if text in s), None)


def after(out):
    rc = subprocess.call([zrig.PY, os.path.join(zrig.HERE, "drive.py"), "seq", "1", KEYS, "netend"])
    print("netend keys rc", rc, flush=True)
    log = os.path.join(out, "lobby.log")
    deadline = time.time() + float(zrig.env("WATCH", "30"))
    logout = closed = None
    while time.time() < deadline:
        with open(log, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        logout = find(lines, "C<-S [A] ID:0x6006")
        closed = logout and find([s for s in lines if stamp(s) and stamp(s) >= logout], "eventPeerLeave")
        if closed:
            break
        time.sleep(1)
    if not logout:
        print("FAIL: no logout (0x6006) in lobby.log")
        return 1
    if not closed:
        print("FAIL: the lobby connection stays open after the logout")
        return 1
    secs = (closed - logout).total_seconds()
    ok = secs <= float(zrig.env("LIMIT_END", "5"))
    print(f"{'PASS' if ok else 'FAIL'}: lobby connection closed {secs:.1f} s after the logout")
    return 0 if ok else 1


def main():
    os.environ.setdefault("ROUTE", ROUTE)
    os.environ.setdefault("ROUTE_END", "1")
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "OUT", "CARDS")
    return boottime.run(os.environ["OUT"], after)


if __name__ == "__main__":
    sys.exit(main())
