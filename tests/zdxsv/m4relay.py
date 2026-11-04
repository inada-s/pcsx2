"""m4.py with ZDXSV_GGPO net=1 and a udprelay.py between every pair of the 4 GGPO peers.
Peer i (port PORT+i) reaches peer j at RELAY+8i+j (pcsx2 ZDXSV_GGPO relay=); one relay per pair.
Env: OUT (required), DELAY (one-way ms, 30), JITTER (ms, 0), LOSS (0..1, 0), GDELAY (GGPO frame delay, 1),
  PORT (7101), RELAY (7200), GGPO_EXTRA (appended to ZDXSV_GGPO); other env goes to m4.py.
Usage: OUT=<dir> DELAY=30 LOSS=0.05 python tests/zdxsv/m4relay.py
"""
import glob
import os
import re
import subprocess
import sys

import zrig


def main():
    zrig.take()  # one rig at a time: its relays start before m4.py
    zrig.need("OUT")
    out = os.environ["OUT"]
    port, relay = int(zrig.env("PORT", "7101")), int(zrig.env("RELAY", "7200"))
    os.makedirs(out, exist_ok=True)
    procs = []
    for i in range(3):
        for j in range(i + 1, 4):
            f = open(os.path.join(out, f"relay-{i}{j}.txt"), "wb")
            procs.append(subprocess.Popen(
                [zrig.PY, "-u", os.path.join(zrig.TOOLS, "udprelay.py"), "--a", str(relay + 8 * i + j),
                 "--b", str(relay + 8 * j + i), "--p1", str(port + i), "--p2", str(port + j),
                 "--delay", zrig.env("DELAY", "30"), "--jitter", zrig.env("JITTER", "0"),
                 "--loss", zrig.env("LOSS", "0"), "--seed", str(10 * i + j), "--seconds", "3600", "--idle", "900"],
                stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT))
    ggpo = f"ZDXSV_GGPO=net=1,players=4,port={port},relay={relay},delay={zrig.env('GDELAY', '1')}{zrig.env('GGPO_EXTRA')}"
    e = dict(os.environ, PCSX2_ENV=f"{ggpo} {zrig.env('PCSX2_ENV')}")
    rc = subprocess.call([zrig.PY, "-u", os.path.join(zrig.HERE, "m4.py")], env=e)
    for p in procs:
        p.kill()
    print("relays:")
    idle = False
    for f in sorted(glob.glob(os.path.join(out, "relay-*.txt"))):
        ls = zrig.log_lines(f, r"relay (end|stats)")
        print(f"{os.path.basename(f)[:-4]} {ls[-1] if ls else ''}")
        # a relay that never forwarded = peers bypassed it (or never started): not a latency run
        if not any(re.search(r"relay (end|stats): packets in [1-9]", l) for l in ls):
            idle = True
    if idle:
        print("FAIL relay idle")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
