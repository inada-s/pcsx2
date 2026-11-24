"""Game left the battle: `rbk.py 2 1` (COUNT 2) where position 0 drops its 3rd own kind-3 msg
(ZDXSV_NET_DROP_K3=3), so the next load never ends; the load barrier limit is raised (ZDXSV_NET_BARRIER_MS)
so the game gives up first. Exit 0 = each peer logged `the game connects elsewhere` and ended its session
`game left` (rbk.py itself reports FAIL: no net battle end).
  OUT=<dir> python tests/zdxsv/gameleft.py
"""
import os
import subprocess
import sys

import rbk
import zrig

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    zrig.need("OUT", "RUN")
    env = dict(os.environ, COUNT="2")
    env["ENV0"] = (env.get("ENV0", "") + " ZDXSV_NET_DROP_K3=3").strip()
    env["PCSX2_ENV"] = (env.get("PCSX2_ENV", "") + " ZDXSV_NET_BARRIER_MS=600000").strip()
    subprocess.call([sys.executable, os.path.join(HERE, "rbk.py"), "2", "1"], env=env)
    ok = True
    for p in (1, 3):
        left = rbk.lines(rbk.emulog(p), r"the game connects elsewhere")
        end = rbk.lines(rbk.emulog(p), r"ZdxsvGgpo: game left frames")
        good = bool(left) and bool(end)
        print(f"{'PASS' if good else 'FAIL'} p{p}: {(left or ['no connect line'])[0]}", flush=True)
        ok = ok and good
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
