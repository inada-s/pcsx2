"""Sync check across peers (@inada-s, inada-s/ai-automation#62: in sync = during the game, each player's
coordinates x, y, z and the game RNG equal frame by frame). pcsx2 ZDXSV_PW_HASH=1 writes NET_TRACE
`0 H frame h0 h1 h2 h3 rng` per GGPO frame (last save wins): h<p> = XXH3 of player p's x, y, z,
rng = RNG A (u16 0x6d7940) << 16 | RNG B (u16 0x6d793c).

usage: pwcheck.py [--own] TRACE1 TRACE2 [...]   (--own: accepted, no effect; the check has no machine-local fields)
Per player p and for the RNG: frames where the values differ, first differing frame and the runs of differing
frames, counted from the play start; `pre-play` = differing frames before it. Judged: players + `rng` (RNG B).
Play start = the first `PS` trace line (ZDXSV_ZDS_PS barrier frame, equal on all peers); in traces without
one (lobby battles, replays) the first frame the tick state 0xc627b4 (`L` lines, last column) leaves 8 =
battle load end; the latest over the traces. All frames if a trace has neither.
- RNG A also takes machine-local draws (s759 rbk 2 1 ZDXSV_EE_WATCH: sound pick 0x23abec on one peer only,
  71 of 73 differing frames; RNG B callers equal on all 828 frames): `rng_a` is printed, not judged.
- Before play start the scene steps run on local load timing (s759 rbk 4 1: RNG B draw 0x2f1770 one frame
  earlier on position 0, frame 204, same value; s759 lobby p1 replay: tick state 8 at 203 vs live 204, RNG B
  the same way), so those frames are not judged.
Traces of builds before the coordinate check (no rng column) are refused.
"""
import sys


def load(path):
    h, ps, load_end, tick = {}, None, None, None
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.split()
        if len(f) >= 3 and f[1] == "H":
            if len(f) != 8:
                sys.exit("%s: H line without the rng column (pcsx2 before the coordinate + RNG check): %s" % (path, line.strip()))
            h[int(f[2])] = f[3:8]
        elif len(f) >= 3 and f[1] == "PS" and ps is None:
            ps = int(f[2])
        elif len(f) == 7 and f[1] == "L":
            if tick == "8" and f[6] != "8" and load_end is None:
                load_end = int(f[2])
            tick = f[6]
    return h, ps if ps is not None else load_end, "play start" if ps is not None else "load end"


paths = [a for a in sys.argv[1:] if a != "--own"]
loaded = [load(p) for p in paths]
ts = [t for t, _, _ in loaded]
for p, (t, ps, how) in zip(paths, loaded):
    print(p, "frames", len(t), min(t) if t else None, max(t) if t else None, how, ps)
common = sorted(set.intersection(*(set(t) for t in ts)))
print("common frames", len(common))
pss = [ps for _, ps, _ in loaded]
start = max(pss) if None not in pss else 0
print("judged from frame %d (%s), %d frames" % (start, "/".join(sorted({how for _, _, how in loaded})) if start
                                                  else "no PS or load end: all frames",
                                                  sum(1 for f in common if f >= start)))
cols = [("player %d" % k, lambda h, k=k: h[k]) for k in range(4)]
cols += [("rng", lambda h: h[4][-4:]), ("rng_a", lambda h: h[4][:-4])]
for name, val in cols:
    diff = [f for f in common if len({val(t[f]) for t in ts}) > 1]
    bad = [f for f in diff if f >= start]
    runs = []
    for f in bad:
        if runs and f == runs[-1][1] + 1:
            runs[-1][1] = f
        else:
            runs.append([f, f])
    long = [r for r in runs if r[1] > r[0]]
    print("%s: mismatches %d first %s runs %d (1-frame %d) pre-play %d longer %s" % (
        name, len(bad), bad[0] if bad else None, len(runs), len(runs) - len(long), len(diff) - len(bad), long[:8]))
