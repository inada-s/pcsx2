"""Pictures of the network menu and of the title menu: the game's menus must show with the single draw list on.
p1 walks from the title to the network menu by RAM keys, then its GS snapshot must not be blank.
  OUT=<dir> python tests/zdxsv/menupic.py ["ZDXSV_SINGLE_DRAW_LIST=0"]
Exit 0 = both pictures hold content. Pictures: OUT/menu.png, OUT/net.png.
"""
import os
import sys

import launch
import pine
import zrig

# A blank 588x441 snapshot is under 2 KB; these screens are over 100 KB.
MIN_PICTURE_BYTES = 20000
MENU_ROUTE = "w12000,Return~scene=2/500,w2000,Down~menu=7/400,w1500"
NET_ROUTE = "C~scene=7,w2000,C~net=2,w2000,C~net=3/500,w3000"


def picture(q, path, name):
    q.snap(path, 5)
    size = os.path.getsize(path)
    ok = size >= MIN_PICTURE_BYTES
    print(f"{'PASS' if ok else 'FAIL'} {name}: snapshot {size} bytes", flush=True)
    return ok


def main():
    zrig.need("OUT", "RUN", "ISO")
    out = os.environ["OUT"]
    os.makedirs(out, exist_ok=True)
    extra = dict(a.split("=", 1) for a in sys.argv[1:])
    pids = launch.launch([1], extra_env=extra)
    try:
        q = pine.Pine(28011)
        q.seq(MENU_ROUTE)
        ok = picture(q, os.path.join(out, "menu.png"), "title menu")
        q.seq(NET_ROUTE)
        return 0 if picture(q, os.path.join(out, "net.png"), "network menu") and ok else 1
    finally:
        zrig.kill(*pids.values())
        zrig.release()


if __name__ == "__main__":
    sys.exit(main())
