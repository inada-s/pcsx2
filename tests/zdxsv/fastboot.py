"""Fast boot: four boottime.py runs with fast boot at its default (on) and no input. Run 1 starts without the boot
state and must save it at the NET file load (within SAVELIMIT s); run 2 must load it and reach the user_id
selection within LIMIT s of the VM start. Each run prints its time from the launch and the VM start.
Run 3 (another card) must not load the state (it holds run 1's system data) and must save its own in its place.
Run 4 (an unformatted card it writes) must log that fast boot stopped within 15 s.
  OUT=<dir> python tests/zdxsv/fastboot.py
Env: LIMIT (s, default 5), SAVELIMIT (s, default 30), CARD (as boottime.py), OTHER_CARD (default the second of CARDS).
Exit 0 = all runs pass. Logs: OUT/save, OUT/load, OUT/other, OUT/unformatted (boottime.py logs each).
"""
import glob
import os
import subprocess
import sys

import zrig

HERE = os.path.dirname(os.path.abspath(__file__))


def boottime(out, name, **env):
    e = dict(os.environ, ZDXSV_FAST_BOOT="default", OUT=os.path.join(out, name), **env)
    r = subprocess.run([zrig.PY, os.path.join(HERE, "boottime.py")], env=e, capture_output=True, text=True,
                       errors="replace")
    txt = r.stdout + r.stderr
    with open(os.path.join(out, f"{name}.txt"), "w", encoding="utf-8") as f:
        f.write(txt)
    return txt


def emulog(out, name, needle):
    try:
        return zrig.log_lines(os.path.join(out, name, "emulog-p1.txt"), needle)
    except OSError:
        return []


def step(out, name, limit, line, **env):
    txt = boottime(out, name, LIMIT=limit, **env)
    for l in txt.splitlines():
        if "PASS" in l or "FAIL" in l:
            print(l, flush=True)
    if "PASS user_id" not in txt:
        print(f"FAIL {name}: no user_id screen in {limit} s")
        return False
    hit = emulog(out, name, f"Zdxsv: fast boot: {line}")
    if not hit:
        print(f"FAIL {name}: no '{line}' in emulog")
        return False
    print(hit[0], flush=True)
    return True


def main():
    zrig.need("OUT", "RUN")
    out, run = os.environ["OUT"], os.environ["RUN"]
    states = os.path.join(run, "p1", "PCSX2", "sstates", "* zdxsv-fastboot*.p2s")
    for f in glob.glob(states):
        os.remove(f)
    os.makedirs(out, exist_ok=True)
    if not step(out, "save", zrig.env("SAVELIMIT", "30"), "saving"):
        return 1
    if not glob.glob(states):
        print(f"FAIL save: no boot state in {os.path.dirname(states)}")
        return 1
    if not step(out, "load", zrig.env("LIMIT", "5"), "loading"):
        return 1
    other = zrig.env("OTHER_CARD") or (os.environ["CARDS"].split() + [""])[1]
    if not other:
        print("FAIL: set OTHER_CARD (a card other than CARD)")
        return 1
    if not step(out, "other", zrig.env("SAVELIMIT", "30"), "saving", CARD=other):
        return 1
    if len(glob.glob(states)) != 1:
        print(f"FAIL other: {len(glob.glob(states))} boot states, want 1")
        return 1
    blank = os.path.join(run, "Mcd001-zdxsv-unformatted.ps2")
    with open(blank, "wb") as f:
        f.write(b"\xff" * 8650752)
    try:
        boottime(out, "unformatted", CARD="zdxsv-unformatted", MAXS="15", ZDXSV_FAST_LOGIN="0")
    finally:
        os.remove(blank)
    hit = emulog(out, "unformatted", "Zdxsv: fast boot: stopped, memory card error")
    if not hit:
        print("FAIL unformatted: fast boot did not stop")
        return 1
    print("\n".join(hit))
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
