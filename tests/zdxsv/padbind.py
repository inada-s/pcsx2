"""Button settings in battle: the synced input record (A, B) uses each player's own in-game button settings.

  OUT=<dir> python tests/zdxsv/padbind.py [seed]   (seed default 1)

Copies $RBKSTATES to $OUT/states with every assignable button of peer p1 moved to another action (guest u16[8] at
0x7ade6c, the game's button settings of pad port 0); p3 keeps the default settings. Runs `rbk.py 2 <seed>` (random
input) on the copy. Pass: rbk.py passes (peers in sync), and on each peer, over its own position's `Z` trace lines
(at least MIN_PRESSES with an action bit): every synced A = the A that peer's settings give for the synced B, and = the
game's own record wherever the game's B pairs with the synced B.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

import zrig

HERE = os.path.dirname(os.path.abspath(__file__))
PAD_BIND_SLOTS = 0x7ade6c
DEFAULT_SLOTS = (0, 1, 7, 3, 2, 4, 6, 5)
MOVED_SLOTS = (3, 2, 1, 0, 7, 6, 5, 4)
ACTIONS = (0x0200, 0x0100, 0x0040, 0x0080, 0x0300, 0x0180, 0x0280, 0x0020)
BUTTON_B = (0x0200, 0x0100, 0x0080, 0x0040, 0x0020, 0x0010, 0x0008, 0x0004)  # □ △ L1 × ○ R1 L2 R2
ACTION_BITS = 0x03e0
STICK_A = 0x3c00
MIN_PRESSES = 100
POSITION = {1: 0, 3: 1}
Z_LINE = re.compile(r"^\d+ Z (-?\d+) (\d+) [0-9a-f]+ ([0-9a-f]{4}) ([0-9a-f]{4}) ([0-9a-f]{4}) ([0-9a-f]{4}) ")


def patch_state(src, dst):
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w") as zout:
        for info in zin.infolist():
            data = zin.read(info)
            if info.filename == "eeMemory.bin":
                m = bytearray(data)
                m[PAD_BIND_SLOTS:PAD_BIND_SLOTS + 16] = struct.pack("<8H", *MOVED_SLOTS)
                data = bytes(m)
            zout.writestr(info, data, compress_type=info.compress_type)


def expected_a(b, slots):
    a = b & 0x8000
    for i, bit in enumerate(BUTTON_B):
        if b & bit:
            a |= ACTIONS[slots[i]]
    return a


def check(trace, pos, slots):
    ours, game = {}, {}
    with open(trace, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = Z_LINE.match(line)
            if not m or int(m.group(2)) != pos:
                continue
            k = int(m.group(1))
            na, nb, a, b = (int(m.group(i), 16) for i in range(3, 7))
            ours[k] = (na, nb & ~1)
            game[k] = (a, b & ~1)
    unlike_settings = [k for k in ours if ours[k][0] & ~STICK_A != expected_a(ours[k][1], slots)]
    for k in unlike_settings[:3]:
        print(f"  pos {pos} frame {k}: synced A {ours[k][0]:04x} for B {ours[k][1]:04x}, settings give {expected_a(ours[k][1], slots):04x}")
    # The game's own record trails the synced one by a few frames; B does not depend on the settings, so it gives
    # the shift.
    def b_equal(s):
        return [k for k in game if k - s in ours and game[k][1] == ours[k - s][1]]
    shift = max(range(0, 9), key=lambda s: len(b_equal(s)))
    paired = b_equal(shift)
    unlike_game = [k for k in paired if game[k][0] != ours[k - shift][0]]
    for k in unlike_game[:3]:
        print(f"  pos {pos} frame {k}: synced A {ours[k - shift][0]:04x}, game's own {game[k][0]:04x}")
    presses = sum(1 for k in ours if ours[k][0] & ACTION_BITS)
    print(f"  pos {pos}: {len(ours)} frames, {presses} with an action; A unlike the settings {len(unlike_settings)}; "
          f"game's record at shift {shift}: B equal on {len(paired)}, A unlike {len(unlike_game)}")
    return presses >= MIN_PRESSES and not unlike_settings and paired and not unlike_game


def main(argv):
    seed = argv[0] if argv else "1"
    zrig.need("RBKSTATES", "OUT")
    out = os.environ["OUT"]
    states = os.path.join(out, "states")
    os.makedirs(states, exist_ok=True)
    for p in POSITION:
        src = os.path.join(os.environ["RBKSTATES"], f"rbk-p{p}.p2s")
        dst = os.path.join(states, f"rbk-p{p}.p2s")
        if p == 1:
            patch_state(src, dst)
        else:
            shutil.copyfile(src, dst)
    run = os.path.join(out, "rbk")
    env = dict(os.environ, RBKSTATES=states, OUT=run)
    rc = subprocess.call([sys.executable, os.path.join(HERE, "rbk.py"), "2", seed], env=env)
    ok = rc == 0
    print(f"rbk.py exit {rc}")
    for p, pos in POSITION.items():
        ok &= bool(check(os.path.join(run, f"trace-p{p}.txt"), pos, MOVED_SLOTS if p == 1 else DEFAULT_SLOTS))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
