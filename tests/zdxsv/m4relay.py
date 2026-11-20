"""m4ggpo.py with a udprelay.py (delay, jitter, loss) on every GGPO path: the 4 peers get each other's
addresses from the lobby's battle info, which points at the relays (fake_lobby.py --ggpo-delay).
Env: OUT (required), DELAY (one-way ms, 30), JITTER (ms, 0), LOSS (0..1, 0), GDELAY (GGPO frame delay, 1);
  other env goes to m4ggpo.py. Checks: m4ggpo.py's, incl. every udprelay forwarded (LOSS=1: none does).
Usage: OUT=<dir> DELAY=30 LOSS=0.05 python tests/zdxsv/m4relay.py
"""
import os
import subprocess
import sys

import zrig


def main():
    zrig.need("OUT")
    e = dict(os.environ, GGPO_LAT=zrig.env("DELAY", "30"), GGPO_JITTER=zrig.env("JITTER", "0"),
             GGPO_LOSS=zrig.env("LOSS", "0"))
    return subprocess.call([zrig.PY, "-u", os.path.join(zrig.HERE, "m4ggpo.py")], env=e)


if __name__ == "__main__":
    sys.exit(main())
