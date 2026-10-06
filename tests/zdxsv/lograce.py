"""fake_lobby.log() under concurrent connection threads (a 4th client's 0x6138
answer line was glued to another and its BATTLE RESULT line lost -> m4.sh saw results 3/4).
  python -u lograce.py > OUT 2>&1; python lograce.py --check OUT   -> lost/glued counts, rc 1 if any
Each of T threads logs N lines of fake_lobby's 0x6138 shape, then a BATTLE RESULT line per line."""
import os
import re
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_lobby  # noqa: E402

T, N = 4, 3000
BODY = "6789001102a51e1c1f1c07141c1113151e1c083f32" * 3
LINE = re.compile(r"^\d\d:\d\d:\d\d (c\d+ C->S cat 02 cmd 0x6138 seq \d+ body %s|BATTLE RESULT c\d+ seq \d+)$" % BODY)


def writer(t):
    for s in range(N):
        fake_lobby.log("c%d C->S cat 02 cmd 0x6138 seq %d body %s" % (t, s, BODY))
        fake_lobby.log("BATTLE RESULT c%d seq %d" % (t, s))


def check(path):
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    good = sum(1 for x in lines if LINE.match(x))
    bad = [x for x in lines if x and not LINE.match(x)]
    results = sum(1 for x in lines if LINE.match(x) and "BATTLE RESULT" in x)
    print("lines ok %d of %d, bad %d, BATTLE RESULT %d of %d" % (good, 2 * T * N, len(bad), results, T * N))
    for x in bad[:3]:
        print("bad:", x[:120])
    return 0 if good == 2 * T * N and not bad else 1


if __name__ == "__main__":
    if sys.argv[1:2] == ["--check"]:
        sys.exit(check(sys.argv[2]))
    ts = [threading.Thread(target=writer, args=(t,)) for t in range(T)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
