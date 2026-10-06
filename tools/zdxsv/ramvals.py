"""python ramvals.py DUMPDIR [A|B] [MIN]: EE RAM u16 slots whose values over all dumps (ZDXSV_RAM_DUMP
<frame>.bin) stay in the record B (or A) value set, bit 0 masked, and take >= MIN (default 5) of them.
Prints address + value per dump for each hit (= candidate per-player input stores)."""
import glob
import os
import sys

import numpy as np

B_SET = [0, 0x20, 0x40, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x80, 0x10, 0x8, 0x4]
A_SET = [0, 0x20, 0x40, 0x80, 0x100, 0x200, 0x180, 0x280, 0x300]

files = sorted(glob.glob(os.path.join(sys.argv[1], '*.bin')), key=lambda p: int(os.path.basename(p)[:-4]))
vals = A_SET if len(sys.argv) > 2 and sys.argv[2] == 'A' else B_SET
need = int(sys.argv[3]) if len(sys.argv) > 3 else 5
lut = np.full(0x10000, 255, np.uint8)
for i, v in enumerate(vals):
    lut[v] = i
seen = None
for p in files:
    m = np.fromfile(p, np.uint16) & 0xfffe
    c = lut[m]
    bits = np.where(c == 255, np.uint32(1 << 31), np.left_shift(np.uint32(1), c.astype(np.uint32)))
    seen = bits if seen is None else seen | bits
ok = (seen & (1 << 31)) == 0
cnt = np.zeros_like(seen)
for i in range(len(vals)):
    cnt += (seen >> i) & 1
hits = np.nonzero(ok & (cnt >= need))[0]
print(len(files), 'dumps', len(hits), 'hits')
lo = int(sys.argv[4], 16) // 2 if len(sys.argv) > 4 else 0
rows = {int(h): [] for h in hits[hits >= lo][:40]}
for p in files:
    m = np.fromfile(p, np.uint16)
    for h in rows:
        rows[h].append(int(m[h]))
for h, r in rows.items():
    print('%08x' % (2 * h), ' '.join('%x' % v for v in r))
