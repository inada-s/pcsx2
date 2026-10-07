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
# Column rng = RNG A << 16 | RNG B. In sync = players + RNG B equal (`rng:`). RNG A (0x6d7940) also takes
# machine-local draws (s759 rbk 2 1 ZDXSV_EE_WATCH: sound pick 0x23abec on one peer only, 71 of 73 differing
# frames; RNG B callers equal on all 828 frames), so peers differ on it in sync: `rng_a:` is printed, not judged.
cols = [("player %d" % k, lambda h, k=k: h[k]) for k in range(4)]
cols += [("rng", lambda h: h[4][-4:]), ("rng_a", lambda h: h[4][:-4])]
for name, val in cols:
    bad = [f for f in common if len({val(t[f]) for t in ts}) > 1]
    runs = []
    for f in bad:
        if runs and f == runs[-1][1] + 1:
            runs[-1][1] = f
        else:
            runs.append([f, f])
    long = [r for r in runs if r[1] > r[0]]
    print("%s: mismatches %d first %s runs %d (1-frame %d) longer %s" % (
        name, len(bad), bad[0] if bad else None, len(runs), len(runs) - len(long), long[:8]))
