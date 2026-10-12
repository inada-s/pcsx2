#!/usr/bin/env python3
"""Compare two ZDXSV_EE_OPTEST files (pcsx2/Zdxsv/EeOpTest.cpp).

usage: ee_optest_cmp.py <a.bin> <b.bin> <emulog of a> [shown cases per op]
Per op: differing cases per output, then the first cases that differ (inputs, both outputs).
"""
import re
import struct
import sys

IN_NAMES = ["fs", "ft", "acc", "f0"]
OUT_NAMES = ["fd", "fcr31"]
REC = 24


def main():
    a, b = (open(p, "rb").read() for p in sys.argv[1:3])
    ops = [m.group(1) for m in re.finditer(r"EE optest: (\S+) (\d+) cases", open(sys.argv[3], errors="replace").read())]
    show = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    if len(a) != len(b) or not ops or len(a) % (REC * len(ops)):
        sys.exit(f"sizes {len(a)} / {len(b)}, {len(ops)} ops")
    n = len(a) // REC // len(ops)
    for k, op in enumerate(ops):
        diff, counts = [], {}
        for i in range(n):
            o = (k * n + i) * REC
            ra, rb = struct.unpack_from("<6I", a, o), struct.unpack_from("<6I", b, o)
            if ra != rb:
                diff.append((ra, rb))
                for j in range(2):
                    if ra[4 + j] != rb[4 + j]:
                        counts[OUT_NAMES[j]] = counts.get(OUT_NAMES[j], 0) + 1
        print(f"{op}: {len(diff)} of {n} differ" + (f" ({', '.join(f'{x} {c}' for x, c in counts.items())})" if diff else ""))
        for ra, rb in diff[:show]:
            ins = ", ".join(f"{IN_NAMES[j]}={ra[j]:08x}" for j in range(4))
            print(f"  {ins}: a fd={ra[4]:08x} fcr31={ra[5]:08x}, b fd={rb[4]:08x} fcr31={rb[5]:08x}")


main()
