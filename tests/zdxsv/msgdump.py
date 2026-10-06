"""Decode ZDXSV_UDP_DUMP files (pcsx2 bridge): split TCP chunks into game battle messages
(McsMessage framing: byte 0 = length, byte 1 high nibble = kind, byte 2 = sub; 82 02 = 20-byte
connection id) and summarize per kind.
  python msgdump.py DIR/bridge-<user>.txt [--seq] [--kind K] [--from F --to F]
--seq: print every message (frame, dir, sender, hex); --kind: only that kind (hex nibble)."""
import argparse
import collections


def split(body):
    out, i = [], 0
    while i + 1 < len(body):
        if body[i] == 0x82 and body[i + 1] == 0x02:
            n = 22  # PS2: 12 bytes + 10-char session id (DC: 20)
        else:
            n = body[i]
        if n < 2 or i + n > len(body):
            out.append(body[i:])  # unframed rest
            break
        out.append(body[i:i + n])
        i += n
    return out


def kind(m):
    if len(m) >= 2 and m[0] == 0x82 and m[1] == 0x02:
        return 'conn'
    if len(m) < 3:
        return 'short'
    if m[1] >> 4 == 2:  # key msg: byte 2 = rolling counter (0x80..0xbf) or 0x00
        return "2/%s/%d" % ("ctr" if m[2] & 0x80 else "%02x" % m[2], m[0])
    return "%x/%02x/%d" % (m[1] >> 4, m[2], m[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--seq', action='store_true')
    ap.add_argument('--kind')
    ap.add_argument('--from', dest='f0', type=int, default=0)
    ap.add_argument('--to', dest='f1', type=int, default=1 << 40)
    a = ap.parse_args()
    stats = collections.OrderedDict()
    chunks = collections.Counter()
    for line in open(a.file):
        p = line.split()
        if len(p) < 7:
            continue
        frame, ms, d, user, seq = int(p[0]), int(p[1]), p[2], p[3], int(p[4])
        if not a.f0 <= frame <= a.f1:
            continue
        msgs = split(bytes.fromhex(p[6]))
        chunks[(d, len(msgs))] += 1
        for m in msgs:
            k = kind(m)
            if a.kind and not k.startswith(a.kind + '/'):
                continue
            key = (d, user if d == 'R' else 'self', k)
            s = stats.setdefault(key, [0, frame, frame, m.hex()])
            s[0] += 1
            s[2] = frame
            if a.seq:
                print(frame, ms, d, user, seq, m.hex())
    if a.seq:
        return
    print('dir sender kind(k/sub/len) count first_frame last_frame per_frame first_body')
    for (d, u, k), (n, f0, f1, h) in sorted(stats.items()):
        print(d, u, k, n, f0, f1, '%.2f' % (n / max(1, f1 - f0)), h[:48])
    print('msgs per TCP chunk:', dict(sorted(chunks.items())))


if __name__ == '__main__':
    main()
