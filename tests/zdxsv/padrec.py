"""Check key-msg record A/B = f(pad) on a ZDXSV_NET_TRACE file (pcsx2 `P` pad lines + `S` sends).

usage: padrec.py TRACE [--lag N ...]
For each own record (c:k:X:A/B in an S line at frame f) take the pad (raw 2 button bytes, active
low) in effect at frame f+lag; print per lag: records, distinct pads, pads mapping to >1 (A,B),
and the pad -> (A,B) table of the best lag. Also: pad changes with no record within 30 frames.
"""
import bisect
import collections
import sys


def load(path):
    pads, recs = [], []  # (frame, b0b1), (frame, A, B)
    for line in open(path, encoding="latin-1"):
        p = line.split()
        if len(p) < 3 or not p[0].isdigit():
            continue
        f = int(p[0])
        if p[1] == "P":
            pads.append((f, p[2][:4]))
        elif p[1] == "S":
            for s in p[3:]:
                t = s.split(":")
                if len(t) == 4 and "/" in t[3]:
                    a, b = t[3].split("/")
                    recs.append((f, a, b))
    return pads, recs


def pad_at(pads, frames, f):
    i = bisect.bisect_right(frames, f) - 1
    return pads[i][1] if i >= 0 else None


def main():
    path = sys.argv[1]
    lags = [int(x) for x in sys.argv[3:]] if "--lag" in sys.argv else [0, 1, 2, 3]
    pads, recs = load(path)
    frames = [f for f, _ in pads]
    best = None
    for lag in lags:
        m = collections.defaultdict(collections.Counter)
        for f, a, b in recs:
            m[pad_at(pads, frames, f + lag)][(a, b)] += 1
        amb = sum(1 for v in m.values() if len(v) > 1)
        bad = sum(sum(v.values()) - max(v.values()) for v in m.values())
        print(f"lag {lag}: records {len(recs)} pads {len(m)} ambiguous {amb} off-majority {bad}")
        if best is None or bad < best[0]:
            best = (bad, lag, m)
    bad, lag, m = best
    print(f"table (lag {lag}): pad -> A/B x n")
    for pad, v in sorted(m.items(), key=lambda kv: -sum(kv[1].values())):
        print(" ", pad, " ".join(f"{a}/{b}x{n}" for (a, b), n in v.most_common(4)))
    rf = [f for f, _, _ in recs]
    miss, missed = 0, []
    for i, (f, pad) in enumerate(pads):
        if i and pads[i - 1][1] == pad or not rf[0] <= f <= rf[-1]:
            continue
        j = bisect.bisect_left(rf, f - lag - 30)
        if not any(abs(r - (f - lag)) <= 30 for r in rf[j:j + 8]):
            miss += 1
            missed.append(f)
    print(f"pad changes {len(pads)}, in record span with no record within 30 frames: {miss}", missed[:10])


main()
