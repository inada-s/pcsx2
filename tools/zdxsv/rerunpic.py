"""Judge the pictures shown after rollbacks (tests/zdxsv/rerunpic.sh).

usage: rerunpic.py <dir> <from> <to>
dir holds v<vsync>.png of every shown frame of a GGPO synctest with check=2: every 2nd shown frame
follows a rollback. Diffs: share of pixels off by more than 16 in a channel, to the frame before
(d1, split by vsync parity) and to the frame two before (d2). In a correct run both parities move
and d1 stays at or below d2 (frames further apart differ more). A rerun that skips too much shows
the previous picture again (one parity of d1 has no change) or every 2nd picture without its VU1
drawing (d1 far above d2: the broken and the good frames alternate).
Exit 0 PASS, 1 FAIL, 2 too few frames.
"""
import os
import sys

from PIL import Image, ImageChops


def diff(a, b):
    x = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 16 else 0)
    return x.histogram()[255] / (a.size[0] * a.size[1])


def median(s):
    return sorted(s)[len(s) // 2]


def main():
    d, lo, hi = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    d1, d2 = {0: [], 1: []}, []
    ims = {}
    for f in range(lo, hi):
        try:
            ims[f] = Image.open(os.path.join(d, f"v{f}.png")).convert("RGB")
        except OSError:  # missing, or truncated by the end of the run
            continue
        if f - 1 in ims:
            d1[f % 2].append(diff(ims[f - 1], ims[f]))
        if f - 2 in ims:
            d2.append(diff(ims[f - 2], ims[f]))
        ims.pop(f - 2, None)
    if min(len(d1[0]), len(d1[1]), len(d2)) < 50:
        print(f"rerunpic: too few frame pairs {len(d1[0])} {len(d1[1])} {len(d2)}")
        return 2
    m = {p: median(d1[p]) for p in (0, 1)}
    for p in (0, 1):
        print(f"rerunpic d1 parity {p}: pairs {len(d1[p])} median {m[p]:.3f} unchanged {sum(v == 0 for v in d1[p])}")
    m2 = median(d2)
    print(f"rerunpic d2: pairs {len(d2)} median {m2:.3f}")
    ok = min(m.values()) > 0 and max(m.values()) <= m2
    print(f"rerunpic: result {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
