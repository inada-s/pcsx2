"""python inputlag.py TRACE: game-side input delay from a ZDXSV_NET_TRACE (`I` lines).

Per record (own S line or received R line: position, B), the frames until the game's input array
entry of that position (0x117f4d8 + 16*pos, u16 +0 = held B) first equals B. Own position = the
sender nibble of S key msgs. Prints a histogram per position."""
import collections
import sys


def records(hexmsg):
    """(pos, B) of every 8-byte record in a kind-2 key msg."""
    m = bytes.fromhex(hexmsg)
    out, i = [], 0
    while i + 1 < len(m):
        n = m[i]
        if n == 0 or i + n > len(m):
            break
        if m[i + 1] >> 4 == 2:
            pos, j = m[i + 1] & 0xf, i + 2
            while j < i + n:
                if m[j] & 0x80:
                    j += 2
                elif j + 8 <= i + n:
                    out.append((pos, (m[j + 6] << 8 | m[j + 7]) & ~1))
                    j += 8
                else:
                    break
        i += n
    return out


def main():
    pending = []  # (frame, pos, B, own)
    hist = collections.defaultdict(collections.Counter)
    miss = collections.Counter()
    held = [0, 0, 0, 0]
    last = {}  # pos -> last record B (records that keep B, e.g. A or bit-0 only, are skipped)
    for line in open(sys.argv[1], encoding='latin-1'):
        f = line.split()
        if len(f) < 3 or not f[0].isdigit():
            continue
        fr = int(f[0])
        if f[1] in ('S', 'R'):
            for pos, b in records(f[2]):
                if last.get(pos) == b or held[pos] == b:
                    continue
                last[pos] = b
                if any(p[1] == pos for p in pending):
                    miss['superseded'] += 1
                pending = [p for p in pending if p[1] != pos]
                pending.append((fr, pos, b, f[1] == 'S'))
        elif f[1] == 'I' and len(f) >= 6:
            held = [int(e[2:4] + e[0:2], 16) for e in f[2:6]]
            keep = []
            for p in pending:
                if held[p[1]] == p[2]:
                    hist[(p[1], 'own' if p[3] else 'remote')][fr - p[0]] += 1
                elif fr - p[0] > 30:
                    miss[p[1]] += 1
                else:
                    keep.append(p)
            pending = keep
    for k in sorted(hist):
        print(k[0], k[1], 'n=%d' % sum(hist[k].values()), 'miss=%d' % miss[k[0]],
              ' '.join('%d:%d' % x for x in sorted(hist[k].items())))
    print('superseded', miss['superseded'])


main()
