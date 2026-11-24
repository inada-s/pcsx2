"""Rerun sends: `rbk.py 2` over COUNT battles (default 3) with LAT ms (default 50) and JITTER ms (default 20), so
rollbacks rerun the round ends and loads. Exit 0 = rbk.py PASS and every peer logged `late 0` (no own msg of an
earlier timeline went into an input unlike the rerun's) and no `went into an input` line.
  OUT=<dir> python tests/zdxsv/rerunsend.py [seed]   (default 1)
Other rbk.py env passes through.
"""
import os
import re
import subprocess
import sys

import rbk
import zrig

HERE = os.path.dirname(os.path.abspath(__file__))


def main(argv):
    zrig.need("OUT", "RUN")
    env = dict(os.environ)
    for k, v in (("COUNT", "3"), ("LAT", "50"), ("JITTER", "20")):
        env.setdefault(k, v)
    rc = subprocess.call([sys.executable, os.path.join(HERE, "rbk.py"), "2", (argv + ["1"])[0]], env=env)
    ok = rc == 0
    for p in (1, 3):
        net = rbk.lines(rbk.emulog(p), r"ZdxsvGgpo: net sends ")
        late = [int(m.group(1)) for m in (re.search(r" late (\d+)", l) for l in net) if m]
        went = rbk.lines(rbk.emulog(p), r"went into an input")
        good = bool(late) and max(late) == 0 and not went
        print(f"{'PASS' if good else 'FAIL'} p{p}: late {late} {len(went)} 'went into an input' lines", flush=True)
        ok = ok and good
    print("PASS" if ok else f"FAIL (rbk.py exit {rc})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
