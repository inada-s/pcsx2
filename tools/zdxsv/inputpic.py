"""Picture change after each press (tests/zdxsv/inputpic.sh).

usage: inputpic.py <dir> <first> <gap> <count>
dir holds v<vsync>.png of every shown frame. A press' change is the first frame from its press on
whose diff to the frame before (share of pixels off by more than 16) is above 0.05: the menu cursor.
Prints each press' offset (vsyncs) and the median. Exit 1 when a frame is missing or a press has no change.
"""
import os
import statistics
import sys

from PIL import Image, ImageChops


def main():
    d, first, gap, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    shots = {}
    for f in os.listdir(d):
        if f.startswith("v") and f.endswith(".png"):
            try:
                shots[int(f[1:-4])] = Image.open(os.path.join(d, f)).convert("L")
            except OSError:
                pass  # the last ones may be cut off at shutdown
    if not shots:
        print("no snapshots")
        return 1
    lo, hi = first - 1, first + count * gap
    missing = [v for v in range(lo, hi) if v not in shots]
    offsets = []
    p = first
    while p < hi:
        found = None
        for v in range(p, p + gap):
            a, b = shots.get(v - 1), shots.get(v)
            if a is None or b is None:
                continue
            x = ImageChops.difference(a, b).point(lambda c: 255 if c > 16 else 0).histogram()[255] / (a.size[0] * a.size[1])
            if x > 0.05:
                found = v - p
                break
        print("press at %d: %s" % (p, "+%d" % found if found is not None else "no change"))
        offsets.append(found)
        p += gap
    ok = not missing and offsets and None not in offsets
    print("frames %d missing %d presses %d median %s" % (len(shots), len(missing), len(offsets),
        statistics.median(o for o in offsets if o is not None) if any(o is not None for o in offsets) else "-"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
