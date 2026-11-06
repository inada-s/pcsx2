"""Fast login with no token goes back to the network menu, twice (no server needed).
  python tests/zdxsv/netmenu.py
p1 boots through the `top` route with ZDXSV_FAST_LOGIN=1, no token file and no sign-in at DNAS.
Exit 0 = the game reached DNAS (net 0x21) and went back to the connection menu (net 3) twice, and the
emulog has two `back to the network menu` lines. Control: a build with the normal-login fallback stays
in DNAS.
"""
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.update(ZDXSV_FAST_LOGIN="1", ZDXSV_AUTH_AT_DNAS="0", ZDXSV_FAST_BOOT="0", ZDXSV_TLS="0",
                  ZDXSV_NODEV9="127.0.0.1", ZDXSV_LOGIN_API_URL="http://127.0.0.1:1/lbs/login")
import launch  # noqa: E402
import zrig  # noqa: E402
from drive import route_text  # noqa: E402
from pine import Pine  # noqa: E402

NET = 0xcd1360
PHASE = 0x6dd770 - 0x59ec
AGAIN = "C~lstate!/500,@net=5,Up~lstate!/500,C~lstate!/500,@net=0x21/30000"
LIMIT = float(os.environ.get("LIMIT", "180"))


def main():
    token = os.path.join(os.environ["RUN"], "p1", "PCSX2", "inis", "zdxsv_auth_token.txt")
    if os.path.exists(token):
        os.remove(token)
    pids = launch.launch(["1"], headless=True)
    try:
        time.sleep(8)
        p = Pine(28011)
        lock = threading.Lock()
        call = p.call
        p.call = lambda *a: (lock.acquire(), call(*a), lock.release())[1]

        def press(text):
            for tok in text.split(","):
                try:
                    p.seq(tok)
                except Exception as e:  # a missed key ends the route; the checks below fail
                    print("route:", tok, e, flush=True)
                    return

        threading.Thread(target=press, args=(route_text("top", 1),), daemon=True).start()
        backs, t0, last = 0, time.time(), None
        while time.time() - t0 < LIMIT and backs < 2:
            s = (p.r8(NET), p.r8(PHASE))
            if s != last:
                print("%.1f s net %x phase %d" % (time.time() - t0, s[0], s[1]), flush=True)
                if s[0] == 3 and s[1] == 2:
                    backs += 1
                    if backs < 2:
                        threading.Thread(target=press, args=(AGAIN,), daemon=True).start()
                last = s
            time.sleep(0.02)
    finally:
        zrig.kill(*pids.values())
    emulog = open(os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt"), encoding="utf-8",
                  errors="replace").read()
    logged = emulog.count("Zdxsv: fast login: not signed in, back to the network menu")
    ok = backs == 2 and logged == 2
    print(f"{'PASS' if ok else 'FAIL'}: back to the connection menu {backs} times, emulog {logged} lines")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
