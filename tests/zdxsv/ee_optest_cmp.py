#!/usr/bin/env python3
"""Compare two ZDXSV_EE_OPTEST files (pcsx2/Zdxsv/EeOpTest.cpp).

usage: ee_optest_cmp.py <a.bin> <b.bin> <emulog of a> [shown cases per op] [-ops OP,..] [-fields OUT,..]
Per op: differing cases per output, then the first cases that differ (inputs, both outputs).
-ops: compare only these ops (each must be in the log). -fields: a case differs only in these outputs.
COP1 case: fs, ft, acc, f0 -> fd, fcr31. COP2 (op name V*): VF1, VF2, ACC, VF3, I, Q -> VF3, status, MAC, clip, Q.
"""
import re
import struct
import sys

COP1 = (["fs", "ft", "acc", "f0"], ["fd", "fcr31"])
COP2 = ([f"{r}{c}" for r in ("vf1", "vf2", "acc", "vf3") for c in "xyzw"] + ["i", "q"],
        ["vf3x", "vf3y", "vf3z", "vf3w", "status", "mac", "clip", "q"])


def main():
    args, only = [], {}
    argv = iter(sys.argv[1:])
    for x in argv:
        if x in ("-ops", "-fields"):
            only[x] = set(next(argv).split(","))
        else:
            args.append(x)
    a, b = (open(p, "rb").read() for p in args[0:2])
    log = open(args[2], errors="replace").read()
    ops = [(m.group(1), int(m.group(2))) for m in re.finditer(r"EE optest: (\S+) (\d+) cases", log)]
    show = int(args[3]) if len(args) > 3 else 0
    missing = only.get("-ops", set()) - {op for op, _ in ops}
    if missing:
        sys.exit(f"ops not in the log: {', '.join(sorted(missing))}")
    size = sum(n * 4 * sum(map(len, COP2 if op.startswith("V") else COP1)) for op, n in ops)
    if len(a) != len(b) or not ops or len(a) != size:
        sys.exit(f"sizes {len(a)} / {len(b)}, expected {size} for {len(ops)} ops")
    o = 0
    total = 0
    for op, n in ops:
        ins, outs = COP2 if op.startswith("V") else COP1
        ni, rec = len(ins), len(ins) + len(outs)
        if "-ops" in only and op not in only["-ops"]:
            o += n * rec * 4
            continue
        keep = [ni + j for j, name in enumerate(outs) if name in only.get("-fields", outs)]
        diff, counts = [], {}
        for i in range(n):
            ra, rb = struct.unpack_from(f"<{rec}I", a, o), struct.unpack_from(f"<{rec}I", b, o)
            o += rec * 4
            if ra[:ni] != rb[:ni] or any(ra[k] != rb[k] for k in keep):
                diff.append((ra, rb))
                for j, name in enumerate(outs):
                    if ra[ni + j] != rb[ni + j]:
                        counts[name] = counts.get(name, 0) + 1
        print(f"{op}: {len(diff)} of {n} differ" + (f" ({', '.join(f'{x} {c}' for x, c in counts.items())})" if diff else ""))
        for ra, rb in diff[:show]:
            print("  " + " ".join(f"{ins[j]}={ra[j]:08x}" for j in range(ni)))
            print("    a " + " ".join(f"{outs[j]}={ra[ni + j]:08x}" for j in range(len(outs))))
            print("    b " + " ".join(f"{outs[j]}={rb[ni + j]:08x}" for j in range(len(outs))))
        total += len(diff)
    if total:
        sys.exit(f"FAIL {total} cases differ")


main()
