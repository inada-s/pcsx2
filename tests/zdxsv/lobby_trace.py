"""Parse a zdxsv lobby message trace (`zdxsv -v=2 lobby`: server.go logs every frame
with a hex dump of its body) into frames.

  lobby_trace.py LOBBY_LOG [CMD ...]   print conn, time, direction, category, cmd, seq, body
"""
import re
import sys

LINE = re.compile(r"^I\d+ ([\d:.]+) +\d+ server\.go:\d+\] \t(<-|->)(\S+) (C->S|C<-S) \[(.)\] ID:0x([0-9A-Fa-f]+) Seq:(\d+) Body\((\d+) bytes\)")
HEX = re.compile(r"^[0-9a-f]{8}  ([0-9a-f ]{48})")
CATEGORY = {"Q": 0x01, "A": 0x02, "N": 0x10, "C": 0xFF}


def frames(path):
    """Yields dicts: t, conn, c2s, cat, cmd, seq, body (bytes)."""
    cur = None
    for raw in open(path, encoding="utf-8", errors="replace"):
        m = LINE.match(raw)
        if m:
            if cur:
                yield cur
            t, _, conn, d, cat, cmd, seq, size = m.groups()
            cur = dict(t=t, conn=conn, c2s=d == "C->S", cat=CATEGORY.get(cat, 0), cmd=int(cmd, 16), seq=int(seq),
                       size=int(size), body=b"")
            continue
        h = HEX.match(raw)
        if cur and h:
            cur["body"] += bytes.fromhex(h.group(1).replace(" ", ""))  # 16 bytes, two 8-byte halves
        elif cur and cur["size"] == len(cur["body"]):
            yield cur
            cur = None
    if cur:
        yield cur


def main():
    cmds = {int(c, 16) for c in sys.argv[2:]}
    for f in frames(sys.argv[1]):
        if cmds and f["cmd"] not in cmds:
            continue
        print("%s %s %s %02x 0x%04x %d %s" % (f["t"], f["conn"], "C->S" if f["c2s"] else "C<-S", f["cat"], f["cmd"], f["seq"],
                                            f["body"].hex()))


if __name__ == "__main__":
    main()
