"""Sync check across peers (@inada-s, inada-s/ai-automation#62: in sync = each player's coordinates x, y, z
and the game RNG equal frame by frame). pcsx2 ZDXSV_PW_HASH=1 writes NET_TRACE `0 H frame h0 h1 h2 h3 rng` per
GGPO frame (last save wins): h<p> = XXH3 of player p's x, y, z, rng = the 2 game RNG states.

usage: pwcheck.py [--own] TRACE1 TRACE2 [...]   (--own: accepted, no effect; the check has no machine-local fields)
Per player p and for the RNG: frames common to all traces, frames where the values differ, first differing frame
and the runs of differing frames. Traces of builds before the coordinate check (no rng column) are refused.
"""
import sys


def load(path):
    h = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.split()
        if len(f) >= 3 and f[1] == "H":
            if len(f) != 8:
                sys.exit("%s: H line without the rng column (pcsx2 before the coordinate + RNG check): %s" % (path, line.strip()))
            h[int(f[2])] = f[3:8]
    return h


paths = [a for a in sys.argv[1:] if a != "--own"]
ts = [load(p) for p in paths]
for p, t in zip(paths, ts):
    print(p, "frames", len(t), min(t) if t else None, max(t) if t else None)
common = sorted(set.intersection(*(set(t) for t in ts)))
print("common frames", len(common))
for k in range(5):
    bad = [f for f in common if len({t[f][k] for t in ts}) > 1]
    runs = []
    for f in bad:
        if runs and f == runs[-1][1] + 1:
            runs[-1][1] = f
        else:
            runs.append([f, f])
    long = [r for r in runs if r[1] > r[0]]
    print("%s: mismatches %d first %s runs %d (1-frame %d) longer %s" % (
        "player %d" % k if k < 4 else "rng", len(bad), bad[0] if bad else None, len(runs), len(runs) - len(long), long[:8]))
