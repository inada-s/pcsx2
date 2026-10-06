"""Find EE RAM values that count down (or up) linearly across ZDXSV_RAM_DUMP dumps.

usage: ramcount.py [--per D] DUMP...   (dumps in frame order, equally spaced)
Prints address, width, values for every aligned u8/u16/u32 (LE) whose value changes by exactly
D per dump (default: tries -1, -60, +1, +60) on every step and is not 0 in the first dump.
"""
import sys

import numpy as np


def main():
    args = sys.argv[1:]
    deltas = [-1, -60, 1, 60]
    if args[:1] == ["--per"]:
        deltas = [int(args[1])]
        args = args[2:]
    # 5.9 GB box: raw dumps only, one signed difference at a time (wraps like the type)
    dumps = [np.fromfile(p, dtype=np.uint8) for p in args]
    for dt, st, w in ((np.uint8, np.int8, 1), (np.uint16, np.int16, 2), (np.uint32, np.int32, 4)):
        vs = [d.view(dt) for d in dumps]
        for delta in deltas:
            ok = vs[0] != 0
            for a, b in zip(vs, vs[1:]):
                ok &= (b - a).view(st) == delta
            for i in np.nonzero(ok)[0][:40]:
                print(f"0x{i * w:07x} u{w * 8} d{delta:+d} " + " ".join(str(v[i]) for v in vs))


if __name__ == "__main__":
    main()
