#!/usr/bin/env python3
"""Compare the ZDXSV_CYCLE_LOG lines of two emulogs of the same replay.

usage: cyclecmp.py <emulog a> <emulog b> [shown lines]
Exit 0 only if both logs have the same frames, at least 2, with equal EE and IOP cycles.
Otherwise prints the counts per CPU and the first differing frames, and exits 1.
"""
import re
import sys

LINE = re.compile(r"cycles at frame (\d+): ee (\d+) iop (\d+)")


def load(path):
    rows = {}
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if m:
                rows[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
    return rows


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    a, b = load(sys.argv[1]), load(sys.argv[2])
    show = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    common = sorted(a.keys() & b.keys())
    ok = True
    print(f"frames: a {len(a)}, b {len(b)}, common {len(common)}")
    if len(common) < 2:
        print("FAIL fewer than 2 common frames (ZDXSV_CYCLE_LOG not set?)")
        ok = False
    if a.keys() != b.keys():
        print(f"FAIL frame sets differ: only a {len(a.keys() - b.keys())}, only b {len(b.keys() - a.keys())}")
        ok = False
    diff = [f for f in common if a[f] != b[f]]
    for i, cpu in enumerate(("ee", "iop")):
        n = sum(1 for f in common if a[f][i] != b[f][i])
        print(f"{cpu}: {n} of {len(common)} differ")
    for f in diff[:show]:
        (ea, ia), (eb, ib) = a[f], b[f]
        print(f"  frame {f}: ee {ea} / {eb} ({eb - ea:+d}), iop {ia} / {ib} ({ib - ia:+d})")
    if diff:
        print(f"FAIL {len(diff)} frames differ, first {diff[0]}")
        ok = False
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
