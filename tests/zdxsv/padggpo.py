"""Every pad input reaches a GGPO battle: PINE presses on p1 go through the pad into the synced input of both peers.

  OUT=<dir> python tests/zdxsv/padggpo.py [seed]

Runs `rbk.py 2` (host pad input; a seed = random input instead, which ignores the pad: the control, must FAIL). From
p1's first `Z` trace line until its emulator exits, PINE (opcode 0x30, port 0) holds each bind of BINDS in turn.
Pass: rbk.py passes, and in both peers' traces, for each bind, some position-0 `Z` line has the synced (A, B) of that
bind alone, and some has the game's own record equal to it.
"""
import os
import re
import subprocess
import sys
import threading
import time

import pine
import zrig

HERE = os.path.dirname(os.path.abspath(__file__))
# PadDualshock2 bind -> (A, B): Start 0x8000 in both, left stick = d-pad bits of B, right stick = the same bits in A
BINDS = {
    18: (0, 0x2000), 19: (0, 0x0400), 20: (0, 0x1000), 21: (0, 0x0800),
    22: (0x2000, 0), 23: (0x0400, 0), 24: (0x1000, 0), 25: (0x0800, 0),
    9: (0x8000, 0x8000),
}
HOLD_MS, GAP_MS = 300, 300
Z_LINE = re.compile(r"^\d+ Z (-?\d+) (\d+) [0-9a-f]+ ([0-9a-f]{4}) ([0-9a-f]{4}) ([0-9a-f]{4}) ([0-9a-f]{4}) ")


def has_z(trace):
    try:
        with open(trace, encoding="utf-8", errors="replace") as f:
            return any(Z_LINE.match(l) for l in f)
    except OSError:
        return False


def presser(trace, proc, log):
    while proc.poll() is None and not has_z(trace):
        time.sleep(0.5)
    if proc.poll() is not None:
        log.append("rbk.py ended before a Z line")
        return
    rounds = 0
    try:
        p = pine.Pine(28011)
        while proc.poll() is None:
            for bind in BINDS:
                p.pad(bind, 255)
                p.wait(HOLD_MS)
                p.pad(bind, 0)
                p.wait(GAP_MS)
            rounds += 1
    except (OSError, TimeoutError) as e:
        log.append(f"PINE stopped after {rounds} rounds: {e!r}")
        return
    log.append(f"PINE {rounds} rounds")


def check(trace):
    synced, game = set(), set()
    with open(trace, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = Z_LINE.match(line)
            if not m or int(m.group(2)) != 0:
                continue
            na, nb, a, b = (int(m.group(i), 16) for i in range(3, 7))
            synced.add((na, nb & ~1))
            game.add((a, b & ~1))
    ok = True
    for bind, ab in BINDS.items():
        s, g = ab in synced, ab in game
        ok &= s and g
        print(f"  bind {bind:2d} A {ab[0]:04x} B {ab[1]:04x}: synced {'yes' if s else 'NO'}, game's record {'yes' if g else 'NO'}")
    return ok


def main(argv):
    seed = argv[0] if argv else ""
    zrig.need("RBKSTATES", "OUT")
    zrig.take()
    run = os.path.join(os.environ["OUT"], "rbk")
    env = dict(os.environ, OUT=run)
    trace = os.path.join(run, "trace-p1.txt")
    if os.path.exists(trace):
        os.remove(trace)
    proc = subprocess.Popen([sys.executable, os.path.join(HERE, "rbk.py"), "2"] + ([seed] if seed else []), env=env)
    log = []
    t = threading.Thread(target=presser, args=(trace, proc, log), daemon=True)
    t.start()
    rc = proc.wait()
    t.join(30)
    print("\n".join(log))
    print(f"rbk.py exit {rc}")
    ok = rc == 0
    for p in (1, 3):
        print(f"p{p} trace:")
        ok &= check(os.path.join(run, f"trace-p{p}.txt"))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
