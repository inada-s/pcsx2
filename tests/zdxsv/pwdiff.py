"""Field-level player-work diff across peers (pcsx2 ZDXSV_PW_DUMP=file: records of s32 frame +
4 * 0x2200 bytes of player work 0x8395d8 + 0x2200*p, one per GGPO save, last record of a frame wins).

usage: pwdiff.py [--from F] [--to F] [--own] DUMP1 DUMP2 [...]   (dump i = position i, own player left out)
Per player: words (u32, offset in the player work) that differ across the other machines, how many
frames, first frames, and sample values per machine at the first differing frame.
"""
import sys
from collections import defaultdict

PW = 0x2200
REC = 4 + 4 * PW

args = sys.argv[1:]
lo, hi = -1, 1 << 30
while args and args[0].startswith("--"):
    if args[0] == "--from":
        lo = int(args[1])
    elif args[0] == "--to":
        hi = int(args[1])
    elif args[0] == "--word":  # P:OFF -> per frame the u32 (and as float) on every machine
        word = [int(x, 0) for x in args[1].split(":")]
    elif args[0] == "--own":  # keep the own player (N=2: else each player is seen by 1 machine only)
        own = True
        args = args[1:]
        continue
    args = args[2:]
own = globals().get("own", False)
word = globals().get("word")


def index(path):
    off = {}
    with open(path, "rb") as f:
        pos = 0
        while True:
            h = f.read(4)
            if len(h) < 4:
                break
            fr = int.from_bytes(h, "little", signed=True)
            if lo <= fr <= hi:
                off[fr] = pos
            pos += REC
            f.seek(pos)
    return off


idx = [index(p) for p in args]
fs = [open(p, "rb") for p in args]
common = sorted(set.intersection(*(set(i) for i in idx)))
print("common frames", len(common), common[0] if common else None, common[-1] if common else None)
if word:
    import struct
    p, o = word
    for fr in common:
        ws = []
        for f, i in zip(fs, idx):
            f.seek(i[fr] + 4 + p * PW + o)
            ws.append(f.read(4))
        print(fr, " ".join("%08x(%.1f)" % (int.from_bytes(w, "little"), struct.unpack("<f", w)[0]) for w in ws))
    sys.exit(0)

# per player: offset -> [frames]; first sample (frame, values per machine)
bad = [defaultdict(list) for _ in range(4)]
sample = [dict() for _ in range(4)]
badframes = [set() for _ in range(4)]
for fr in common:
    data = []
    for f, i in zip(fs, idx):
        f.seek(i[fr] + 4)
        data.append(f.read(4 * PW))
    for p in range(4):
        vals = [d[p * PW:(p + 1) * PW] for m, d in enumerate(data) if own or m != p]
        if all(v == vals[0] for v in vals[1:]):
            continue
        badframes[p].add(fr)
        for o in range(0, PW, 4):
            ws = [v[o:o + 4] for v in vals]
            if any(w != ws[0] for w in ws[1:]):
                bad[p][o].append(fr)
                if o not in sample[p]:
                    sample[p][o] = (fr, [int.from_bytes(w, "little") for w in ws])

for p in range(4):
    print("player %d: frames differing %d" % (p, len(badframes[p])))
    for o, frs in sorted(bad[p].items(), key=lambda kv: -len(kv[1]))[:24]:
        fr, ws = sample[p][o]
        print("  +0x%04x (0x%06x) frames %d first %s  @%d %s" % (
            o, 0x8395d8 + PW * p + o, len(frs), frs[:6], fr, " ".join("%08x" % w for w in ws)))
