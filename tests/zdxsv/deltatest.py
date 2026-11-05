"""Delta-state / GGPO synctest run: VAR=SPEC (ZDXSV_DELTA_TEST or ZDXSV_GGPO), pine ROUTE first
(-Route arcade: offline battle from ~f8000), snapshots at -Snaps "f1 f2..", log lines out.
  deltatest.py -Spec S -Var V -Snaps "F.." -End F -Tag T -Out DIR -Route R
ZDXSV_DELTA_TEST: exit 1 on `result FAIL` (pass 2+ differed), 3 when no result line was logged.
"""
import os
import shutil
import subprocess
import sys
import time

import zrig
from launch import launch

# from boot, 1P side, name あああ, default suits, battle by ~f8000
ARCADE = ("f1500,Return,w3000,C,w6000,C,w5000,C,w5000,C,w5000,C,w5000,Return,w2000,C,w2000,Up,w500,Left,w800,"
          "Down,w400,Down,w400,Down,w400,Down,w400,Left,w400,Left,w400,C,w5000,C,w5000,C,w5000,C,w5000,C,w6000,"
          "C,w6000,C,w6000,C,w6000,C,w6000,C,w6000")


def pine(*a, quiet_out=False):
    subprocess.call([zrig.PY, os.path.join(zrig.HERE, "pine.py"), *a], stderr=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL if quiet_out else None)


def wait_snaps(quiet_s=5, max_s=300):
    """ZDXSV_SNAP PNGs are compressed on threads that a kill cuts off: wait until their sizes stop changing."""
    d = os.environ.get("ZDXSV_SNAP", "").split(",")[0]
    if not d or not os.path.isdir(d):
        return
    last, still, t0 = None, 0.0, time.time()
    while time.time() - t0 < max_s and still < quiet_s:
        sizes = {e.name: e.stat().st_size for e in os.scandir(d) if e.name.endswith(".png")}
        still = still + 1 if sizes == last else 0
        last = sizes
        time.sleep(1)


def run(argv, show=True):
    """(rc, the ZdxsvDelta/ZdxsvGgpo log lines); the lines printed when SHOW."""
    o = {"-Spec": "start=1500,frames=3000,depth=8,every=20", "-Var": "ZDXSV_DELTA_TEST", "-Snaps": "",
         "-End": "4600", "-Tag": "a", "-Out": "delta", "-Route": ""}
    for k, v in zip(argv[::2], argv[1::2] + [""] * (len(argv) % 2)):
        if k not in o:
            print(f"FAIL deltatest.py: unknown argument {k}")
            return 2, []
        o[k] = v
    route = ARCADE if o["-Route"] == "arcade" else o["-Route"]
    zrig.need("RUN")
    out = o["-Out"]
    os.makedirs(out, exist_ok=True)
    zrig.kill_name("pcsx2")
    launch([1], extra_env={o["-Var"]: o["-Spec"]}, quiet=True)
    if route:
        pine("seq", route)
    for f in o["-Snaps"].split():
        pine("seq", f"f{f}")
        pine("snap", os.path.join(out, f"{o['-Tag']}-f{f}.png"), "0.5", quiet_out=True)
    pine("seq", f"f{o['-End']}")
    time.sleep(2)
    wait_snaps()
    log = os.path.join(out, f"{o['-Tag']}-emulog.txt")
    shutil.copyfile(os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt"), log)
    zrig.kill_name("pcsx2")
    with open(log, "rb") as f:
        lines = [l.decode("utf-8", "replace").rstrip("\r\n") for l in f if b"ZdxsvDelta" in l or b"ZdxsvGgpo" in l]
    for l in lines if show else ():
        print(l)
    if o["-Var"] == "ZDXSV_DELTA_TEST":
        if any("ZdxsvDelta: result FAIL" in l for l in lines):
            return 1, lines
        if not any("ZdxsvDelta: result" in l for l in lines):
            return 3, lines
    return 0, lines


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:])[0])
