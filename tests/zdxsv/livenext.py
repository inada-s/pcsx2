"""live auto-next skip test: a headless spectator in instance pN opens ZDXSV_REPLAY=live:http://127.0.0.1:PORT where no
lobby answers, with ZDXSV_LIVE_NEXT=1. Exit 0 = the failed open is skipped: the log names the skip and its cause,
the VM pauses and waits for a new battle there (nothing boots to the title screen).
  OUT=dir python tests/zdxsv/livenext.py   Env: N (instance, default 1), PORT (default 8209, nothing may listen),
STATE (any state of the game to boot from; default $RBKSTATES/rbk-p1.p2s), MAXS (default 60).
"""
import atexit
import os
import re
import shutil
import sys
import time

import launch
import zrig


def main():
    zrig.take()
    zrig.need("OUT", "RUN")
    out = os.environ["OUT"]
    n, port, maxs = zrig.env("N", "1"), zrig.env("PORT", "8209"), int(zrig.env("MAXS", "60"))
    state = zrig.env("STATE")
    if not state:
        zrig.need("RBKSTATES")
        state = os.path.join(os.environ["RBKSTATES"], "rbk-p1.p2s")
    os.makedirs(out, exist_ok=True)
    log = os.path.join(os.environ["RUN"], f"p{n}", "PCSX2", "logs", "emulog.txt")
    keep = os.path.join(out, "emulog-live.txt")

    def cleanup():
        zrig.kill_name("pcsx2")
        try:
            shutil.copyfile(log, keep)
        except OSError:
            pass

    atexit.register(cleanup)
    try:
        os.remove(log)
    except OSError:
        pass
    pid = launch.launch([n], state=state, headless=True, quiet=True, extra_env={
        "ZDXSV_REPLAY": f"live:http://127.0.0.1:{port}", "ZDXSV_LIVE_NEXT": "1", "ZDXSV_REPLAY_EXIT": "1"})[int(n)]
    print(f"p{n} pid {pid} slot {28010 + int(n)}", flush=True)

    def lines(rx):
        try:
            return zrig.log_lines(log, rx)
        except OSError:
            return []

    for _ in range(maxs):
        time.sleep(1)
        if lines(r"ZdxsvGgpo: replay .*not a replay file"):
            break
    time.sleep(10)  # a wait that does not hold would boot on or stop by now
    shutil.copyfile(log, keep)
    w = lines(re.escape(f"ZdxsvGgpo: live: auto-next: waiting for a new battle at 127.0.0.1:{port} "
                        "(0 watched, 1 skipped): skipped, "))
    p = lines(r"\(VMManager\) Pausing")
    r = len(lines(r"\(VMManager\) Resuming|ZdxsvGgpo: replay .*: end|auto-next: stopped"))
    print(w[0] if w else "no skip line")
    if w and p and r == 0:
        print("PASS skipped and waiting")
        return 0
    print(f"FAIL skip line {'ok' if w else 'missing'}, pause {'ok' if p else 'missing'}, resumed/stopped lines {r}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
