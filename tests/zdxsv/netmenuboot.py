"""Fast boot stops when fast login goes back to the network menu (no server needed).
  python tests/zdxsv/netmenuboot.py
p1 boots with fast boot on (ZDXSV_FAST_BOOT=1), fast login on, no token file and no sign-in at DNAS,
so the first DNAS start goes back to the connection menu. LIMIT (s, default 180) bounds the wait for it,
SETTLE (s, default 30) is how long nothing may follow.
Exit 0 = emulog has one `back to the network menu` line and `fast boot: stopped, fast login went back`.
Control: a build without the stop connects again and again (several lines).
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.update(ZDXSV_FAST_BOOT="1", ZDXSV_FAST_LOGIN="1", ZDXSV_AUTH_AT_DNAS="0", ZDXSV_TLS="0",
                  ZDXSV_NODEV9="127.0.0.1", ZDXSV_LOGIN_API_URL="http://127.0.0.1:1/lbs/login")
import launch  # noqa: E402
import zrig  # noqa: E402

LIMIT = float(os.environ.get("LIMIT", "180"))
SETTLE = float(os.environ.get("SETTLE", "30"))
BACK = re.compile(r"Zdxsv: fast login: .*, back to the network menu")


def main():
    p1 = os.path.join(os.environ["RUN"], "p1", "PCSX2")
    token = os.path.join(p1, "inis", "zdxsv_auth_token.txt")
    if os.path.exists(token):
        os.remove(token)
    emulog = os.path.join(p1, "logs", "emulog.txt")
    pids = launch.launch(["1"], headless=True)

    def read():
        return open(emulog, encoding="utf-8", errors="replace").read() if os.path.exists(emulog) else ""

    try:
        t0 = time.time()
        while time.time() - t0 < LIMIT and not BACK.search(read()):
            time.sleep(0.5)
        time.sleep(SETTLE)
    finally:
        zrig.kill(*pids.values())
    text = read()
    backs = len(BACK.findall(text))
    stopped = "Zdxsv: fast boot: stopped, fast login went back to the network menu" in text
    ok = backs == 1 and stopped
    print(f"{'PASS' if ok else 'FAIL'}: back to the network menu {backs} times, fast boot stopped on it {stopped}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
