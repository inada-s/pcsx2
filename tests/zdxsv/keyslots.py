"""python keyslots.py k2.txt (msgdump.py --seq --kind 2 output): flatten each sender's key slots,
unwrap the 6-bit counter, align senders by it; k/X disagreements, record patterns, p0 record stats."""
import collections
import sys

slots = collections.defaultdict(dict)
last = {}
for line in open(sys.argv[1]):
    b = bytes.fromhex(line.split()[-1])
    w = b[1] & 0xf
    i = 2
    while i < len(b):
        if b[i] & 0x80:
            c, x, k, rec, i = b[i] & 0x3f, None, b[i + 1], None, i + 2
        else:
            r = b[i:i + 8]
            c, x, k, rec, i = r[1] & 0x3f, r[4], r[5], r[2:4].hex() + '/' + r[6:8].hex(), i + 8
        u = last.get(w, -1)
        u = u + ((c - u) % 64 if u >= 0 else c + 1)
        last[w] = u
        slots[w].setdefault(u, (x, k, rec))
ws = sorted(slots)
common = set.intersection(*(set(slots[w]) for w in ws))
print('slots', {w: len(slots[w]) for w in ws}, 'common', len(common))
bad = collections.Counter()
for u in sorted(common):
    col = [slots[w][u] for w in ws]
    if len({c[1] for c in col}) > 1:
        bad['k'] += 1
        if bad['k'] < 4:
            print('k', u, col)
    if len({c[0] for c in col if c[0] is not None}) > 1:
        bad['x'] += 1
        if bad['x'] < 4:
            print('x', u, col)
print('mismatch', dict(bad))
pat = collections.Counter(tuple(slots[w][u][2] is not None for w in ws) for u in common)
print('record patterns (per sender)', pat.most_common(8))
rs = [u for u in sorted(common) if slots[0][u][2]]
print('p0 record gaps', collections.Counter(b - a for a, b in zip(rs, rs[1:])).most_common(8))
print('p0 A/B', collections.Counter(slots[0][u][2] for u in rs).most_common(10))
