"""zd=1 check (pcsx2 ZDXSV_GGPO zd=1, NET_TRACE `Z` lines from OnStepCopy).

usage: zdcheck.py TRACE1 [TRACE2 ...]
- per trace: steps, own position (from its S lines), lag own record send -> first step that applies it
  (vsyncs; record = the game's lag-0 input)
- across traces: per (GGPO frame, position) the last written (slot, A, B); keys in all traces, mismatches
- with `counter K` fields: per (K, counter, position) the applied (A, B); timing may differ, A/B may not
Z line: `vsync Z[r] frame p slot A B oldA oldB [counter K]` (r = rollback rerun; the last write of a key wins).
"""
import sys
from collections import Counter


def load(path):
    z = {}
    zk = {}  # (K, counter, p) -> (A, B): the A/B a step applied, keyed by its agreed frame K
    me = None
    recs = []  # (vsync, A, B)
    fwd = []  # (vsync, p, A, B) forward steps
    al = []  # (vsync, frame, (A0..A3)) pad module A on change
    pads = []  # vsync of pad changes
    qs = []  # zdp: (vsync, frame, A, B) own pad-derived GGPO input per frame
    zf = []  # forward steps (frame, p, A, B, K)
    for line in open(path, errors="replace"):
        f = line.split()
        if len(f) < 3:
            continue
        if f[1] == "A" and len(f) == 7:
            al.append((int(f[0]), int(f[2]), tuple(int(x, 16) for x in f[3:7])))
        elif f[1] == "Q" and len(f) == 5:
            qs.append((int(f[0]), int(f[2]), int(f[3], 16), int(f[4], 16)))
        elif f[1] == "P":
            pads.append(int(f[0]))
        elif f[1] in ("Z", "Zr") and len(f) >= 9:
            v, fr, p, slot, a, b = int(f[0]), int(f[2]), int(f[3]), f[4], int(f[5], 16), int(f[6], 16)
            z[(fr, p)] = (slot, a, b & ~1)
            if len(f) >= 11:
                zk[(int(f[10]), f[9], p)] = (a, b & ~1)
            if f[1] == "Z" and len(f) >= 11:
                zf.append((fr, p, a, b & ~1, int(f[10])))
            if f[1] == "Z":
                fwd.append((v, p, a, b & ~1))
        elif f[1] == "S":
            d = bytes.fromhex(f[2]) if len(f[2]) % 2 == 0 else b""
            if len(d) > 1 and d[1] >> 4 == 2:
                me = d[1] & 0xF
            for s in f[3:]:
                t = s.split(":")
                if len(t) == 4:
                    a, b = t[3].split("/")
                    recs.append((int(f[0]), int(a, 16), int(b, 16) & ~1))
    return z, me, recs, fwd, al, pads, zk, qs, zf


def alags(me, recs, al):
    """Per own record whose A differs from the pad module's own A at its send: vsyncs until the pad
    module's own A equals it (the record is the game's lag-0 input: pad edge -> record 0-1 f)."""
    out = Counter()
    j = 0
    for v, a, b in recs:
        while j + 1 < len(al) and al[j + 1][0] <= v:
            j += 1
        if not al or al[j][0] > v or al[j][2][me] == a:
            continue
        hit = next((x for x, fr, aa in al[j:j + 40] if x > v and aa[me] == a), None)
        out[hit - v if hit is not None and hit - v < 30 else "none"] += 1
    return out


def aframes(al):
    """frame -> A vector, forward-filled between changes."""
    m = {}
    for i, (v, fr, aa) in enumerate(al):
        end = al[i + 1][1] if i + 1 < len(al) else fr + 1
        for x in range(fr, max(end, fr + 1)):
            m[x] = aa
    return m


def lags(me, recs, fwd):
    own = [(v, a, b) for v, p, a, b in fwd if p == me]
    out = Counter()
    j = 0
    prev = None
    for v, a, b in recs:
        if (a, b) == prev:
            continue
        prev = (a, b)
        while j < len(own) and own[j][0] < v:
            j += 1
        k = j
        while k < len(own) and (own[k][1], own[k][2]) != (a, b) and own[k][0] - v < 30:
            k += 1
        out[own[k][0] - v if k < len(own) and own[k][0] - v < 30 else "none"] += 1
    return out


def qlags(me, qs, zf):
    """zdp: per own pad-derived (A, B) change at GGPO frame F, frames until the first own forward step
    applying it (= GGPO delay + step frame - K)."""
    own = [(fr, a, b) for fr, p, a, b, k in zf if p == me]
    out = Counter()
    j = 0
    prev = None
    for v, fr, a, b in qs:
        if (a, b) == prev:
            continue
        prev = (a, b)
        while j < len(own) and own[j][0] < fr:
            j += 1
        k = j
        while k < len(own) and (own[k][1], own[k][2]) != (a, b) and own[k][0] - fr < 30:
            k += 1
        out[own[k][0] - fr if k < len(own) and own[k][0] - fr < 30 else "none"] += 1
    return out


def qrec(recs, qs):
    """zdp: own records whose (A, B & ~1) is a pad-derived value within 3 vsyncs of the send."""
    byv = {}
    for v, fr, a, b in qs:
        byv.setdefault(v, set()).add((a, b))
    hit = sum(1 for v, a, b in recs if any((a, b) in byv.get(x, ()) for x in range(v - 3, v + 4)))
    return hit, len(recs)


def main():
    runs = [load(p) for p in sys.argv[1:]]
    for path, (z, me, recs, fwd, al, pads, zk, qs, zf) in zip(sys.argv[1:], runs):
        print(f"{path}: pos {me} keys {len(z)} fwd steps {len(fwd)} records {len(recs)}"
              f" lag(record->applied) {sorted(lags(me, recs, fwd).items(), key=str)}")
        if al:
            print(f"  record->padmodule A {sorted(alags(me, recs, al).items(), key=str)}")
        if zf:
            sk = Counter(fr - k if k >= 0 else "nok" for fr, p, a, b, k in zf)
            print(f"  step frame - K {sorted(sk.items(), key=str)[:8]}")
        if qs:
            print(f"  pad->applied {sorted(qlags(me, qs, zf).items(), key=str)}; records = pad value {qrec(recs, qs)}")
    ams = [aframes(r[4]) for r in runs if r[4]]
    if len(ams) > 1:
        common = set(ams[0])
        for m in ams[1:]:
            common &= set(m)
        bad = sorted(k for k in common if len({m[k] for m in ams}) > 1)
        print(f"A frames common {len(common)} mismatches {len(bad)} first {bad[:8]}")
    if len(runs) > 1 and runs[0][0]:
        common = set(runs[0][0])
        for r in runs[1:]:
            common &= set(r[0])
        bad = [k for k in sorted(common) if len({r[0][k] for r in runs}) > 1]
        print(f"common keys {len(common)} mismatches {len(bad)} first {bad[:5]}")
        for k in bad[:3]:
            print(k, [r[0][k] for r in runs])
    if len(runs) > 1 and runs[0][6]:
        common = set(runs[0][6])
        for r in runs[1:]:
            common &= set(r[6])
        bad = [k for k in sorted(common) if len({r[6][k] for r in runs}) > 1]
        nok = sum(1 for r in runs for k in r[6] if k[0] < 0)
        print(f"(K, counter, p) common {len(common)} mismatches {len(bad)} first {bad[:5]}; K=-1 keys {nok}")


if __name__ == "__main__":
    main()
