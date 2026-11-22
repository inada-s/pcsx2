# SPDX-FileCopyrightText: 2026 zdxsv contributors
# SPDX-License-Identifier: GPL-3.0+
"""Round jump from a recorded round state: rplay.py with a jump to round ROUND at frame AT (default 400).

Pass: rplay.py passes, the jump loaded a round state and the replay state check has 0 differing hashes and rngs. Same env and arguments as rplay.py (FILE, OUT, RUN, traces ...).
Control: PCSX2_ENV=ZDXSV_REPLAY_ROUND_BAD=1 fails (the replay state check counts the changed state).
"""
import os
import re
import subprocess
import sys

import zrig


def main(argv):
    zrig.need("OUT", "ROUND")
    out = os.environ["OUT"]
    jump = f"ZDXSV_REPLAY_ROUND_AT={zrig.env('AT', '400')}:{os.environ['ROUND']}"
    env = {**os.environ, "PCSX2_ENV": f"{zrig.env('PCSX2_ENV')} {jump}".strip()}
    rc = subprocess.call([zrig.PY, os.path.join(os.path.dirname(os.path.abspath(__file__)), "rplay.py")] + argv, env=env)
    try:
        with open(os.path.join(out, "emulog-play.txt"), encoding="utf-8", errors="replace") as f:
            log = f.read()
    except OSError:
        log = ""
    fails = []
    if rc != 0:
        fails.append(f"rplay.py rc {rc}")
    if not re.search(r"replay round jump: key \d+ \+ round state \d+", log):
        fails.append("no round jump from a round state")
    m = re.findall(r"replay state check: hashes (\d+) checked, (\d+) differ.*?rngs (\d+) checked, (\d+) differ", log)
    if not m or m[-1][1] != "0" or m[-1][3] != "0" or m[-1][0] == "0":
        fails.append(f"replay state check {m[-1] if m else 'missing'}")
    for f in fails:
        print(f"FAIL {f}")
    print(f"roundjump round {os.environ['ROUND']}: {'FAIL' if fails else 'PASS'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
