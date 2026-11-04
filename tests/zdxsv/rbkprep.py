"""Makes the rbk start states: each of p1..p4 from a fresh boot (freshboot.py: fast boot, fast login to
fake_lobby.py's login API), side entry (パートナー自動選抜) answered by fake_lobby.py, then saved (PINE slot 9)
while it waits for the battle start -> $OUT/rbk-pN.p2s. Then the battle is started (fake lobby --hold) and run for
HOLDS s so a probe (PCSX2_ENV_P1="ZDXSV_EE_PROBE=..") sees the battle start, and all is stopped.
  OUT=<dir> python tests/zdxsv/rbkprep.py
Env as m4.py: TRACE, CARDS, API, IP, ZBIN, PCSX2_ENV, PCSX2_ENV_Pn; HOLDS (default 120).
"""
import os
import shutil
import sys
import time

import freshboot as fb
import zrig

SS = "SLPS-25419 (435D8236).09.p2s"


def main():
    zrig.take()
    zrig.need("RUN", "OUT")
    run, out = os.path.abspath(os.environ["RUN"]), os.environ["OUT"]
    go = os.path.join(out, "go")
    if os.path.exists(go):
        os.remove(go)
    fb.servers(["--hold", go])
    for i in (1, 2, 3, 4):
        p = os.path.join(run, f"p{i}", "PCSX2", "sstates", SS)
        if os.path.exists(p):
            os.remove(p)
    if not fb.fresh_boot_all([1, 2, 3, 4]):
        return 1
    for i in (1, 2, 3, 4):
        if not fb.fresh_entry(i):
            return 1
        time.sleep(3)
        r = zrig.run([zrig.PY, os.path.join(fb.HERE, "pine.py"), "--slot", str(28010 + i), "save", "9"])
        fb.say(((r.stdout + r.stderr).strip().splitlines() or [""])[-1])
        ss = os.path.join(run, f"p{i}", "PCSX2", "sstates", SS)
        for _ in range(30):
            if os.path.isfile(ss) and os.path.getsize(ss):
                break
            time.sleep(1)
        time.sleep(2)
        dst = os.path.join(out, f"rbk-p{i}.p2s")
        try:
            shutil.copyfile(ss, dst)
        except OSError:
            print(f"FAIL client {i} save")
            return 1
        print(f"client {i}: saved {os.path.getsize(dst)} B", flush=True)
    open(go, "w").close()
    time.sleep(int(zrig.env("HOLDS", "120")))
    print(fb.count(os.path.join(out, "battle.log"), "join udp peer"), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
