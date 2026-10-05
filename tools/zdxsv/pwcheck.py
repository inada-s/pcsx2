"""Player-work sync check across peers (pcsx2 ZDXSV_PW_HASH=1: NET_TRACE `0 H frame h0 h1 h2 h3`,
XXH3 of player work 0x8395d8 + 0x2200*p per GGPO frame, last save wins; since pcsx2 17499e81 the
1-frame machine-local scratch words are masked, so an in-sync battle shows 0 mismatches until the
result screens at battle end).

usage: pwcheck.py [--own] TRACE1 TRACE2 [...]
Per player p: frames common to all traces, frames where the hashes differ, first differing frame
and the runs of differing frames. In sync every machine simulates every player identically.
"""
import sys


def load(path):
    h = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.split()
        if len(f) == 7 and f[1] == "H":
            h[int(f[2])] = f[3:7]
    return h


# --own: keep the own player (pcsx2 masks the own-only words since s634; N=2 else sees each player once)
keep_own = sys.argv[1:2] == ["--own"]
paths = sys.argv[2:] if keep_own else sys.argv[1:]
ts = [load(p) for p in paths]
for p, t in zip(paths, ts):
    print(p, "frames", len(t), min(t) if t else None, max(t) if t else None)
common = sorted(set.intersection(*(set(t) for t in ts)))
print("common frames", len(common))
# A machine's own player work holds machine-local fields (s622 run3: player p differs only on the
# machine at position p). Own position = the player whose hash on this trace most often differs
# from every other trace's; that player is left out for that trace.
own = []
for i, t in enumerate(ts):
    n = [sum(1 for f in common if all(t[f][p] != o[f][p] for j, o in enumerate(ts) if j != i)) for p in range(4)]
    own.append(max(range(4), key=lambda p: n[p]) if len(ts) > 2 else -1)
print("own position per trace (guessed)", own, "used: trace index")
own = [-1] * len(ts) if keep_own else list(range(len(ts)))
for p in range(4):
    vals = [t for t, o in zip(ts, own) if o != p]
    bad = [f for f in common if len({t[f][p] for t in vals}) > 1]
    runs = []
    for f in bad:
        if runs and f == runs[-1][1] + 1:
            runs[-1][1] = f
        else:
            runs.append([f, f])
    long = [r for r in runs if r[1] > r[0]]
    print("player %d: mismatches %d first %s runs %d (1-frame %d) longer %s" % (
        p, len(bad), bad[0] if bad else None, len(runs), len(runs) - len(long), long[:8]))
