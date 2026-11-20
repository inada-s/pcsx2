#!/usr/bin/env python3
"""Compare two ZDXSV_VU1_OPTEST files (pcsx2/Zdxsv/VuOpTest.cpp).

usage: vu1_optest_cmp.py <a.bin> <b.bin> <emulog of a> [shown cases per op]
Per op: differing cases per output, then the first cases that differ (inputs, both outputs).
"""
import re
import struct
import sys

IN, OUT = 35, 41
REC = (IN + OUT) * 4
IN_NAMES = [f"VF{1 + i // 4}{'xyzw'[i % 4]}" for i in range(28)] + [f"ACC{c}" for c in "xyzw"] + ["I", "Q", "P"]
OUT_NAMES = ([f"VF{1 + i // 4}{'xyzw'[i % 4]}" for i in range(32)] + [f"ACC{c}" for c in "xyzw"] +
             ["Q", "P", "status", "mac", "clip"])


def main():
    a = open(sys.argv[1], "rb").read()
    b = open(sys.argv[2], "rb").read()
    ops = re.findall(r"VU1 optest: (\S+) (\d+) cases", open(sys.argv[3], errors="replace").read())
    show = int(sys.argv[4]) if len(sys.argv) > 4 else 2
    size = sum(int(n) for _, n in ops) * REC
    if len(a) != len(b) or not ops or len(a) != size:
        sys.exit(f"sizes {len(a)} / {len(b)}, expected {size} for {len(ops)} ops")
    pos = 0
    total = 0
    for name, n in ops:
        n = int(n)
        diff = {}
        cases = 0
        shown = 0
        for i in range(n):
            ra = struct.unpack_from(f"<{IN + OUT}I", a, pos)
            rb = struct.unpack_from(f"<{IN + OUT}I", b, pos)
            pos += REC
            if ra[:IN] != rb[:IN]:
                sys.exit(f"{name} case {i}: inputs differ (not the same generator)")
            fields = [k for k in range(OUT) if ra[IN + k] != rb[IN + k]]
            cases += bool(fields)
            for k in fields:
                diff[OUT_NAMES[k]] = diff.get(OUT_NAMES[k], 0) + 1
            if fields and shown < show:
                shown += 1
                used = ", ".join(f"{IN_NAMES[k]}={ra[k]:08x}" for k in range(IN))
                outs = ", ".join(f"{OUT_NAMES[k]} {ra[IN + k]:08x}/{rb[IN + k]:08x}" for k in fields)
                print(f"  {name} case {i}: {outs}\n    in: {used}")
        print(f"{name}: {cases} of {n} differ" +
              (" (" + ", ".join(f"{k} {v}" for k, v in diff.items()) + ")" if diff else ""))
        total += cases
    if total:
        sys.exit(f"FAIL {total} cases differ")


if __name__ == "__main__":
    main()
