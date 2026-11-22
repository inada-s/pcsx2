"""Fast boot: two boottime.py runs with fast boot on and no input. Run 1 must reach the user_id selection
within LIMIT s of the VM start, twice in a row from a full boot: no fast boot state file is written or loaded,
and the old-style state planted before run 1 is deleted.
Run 2 (an unformatted card it writes) must log that fast boot stopped within 15 s.
  OUT=<dir> python tests/zdxsv/fastboot.py
Env: LIMIT (s, default 30), CARD (as boottime.py).
Exit 0 = all runs pass. Logs: OUT/first, OUT/second, OUT/unformatted (boottime.py logs each).
"""
import glob
import os
import subprocess
import sys

import zrig

HERE = os.path.dirname(os.path.abspath(__file__))


def boottime(out, name, **env):
    e = dict(os.environ, ZDXSV_FAST_BOOT="1", OUT=os.path.join(out, name), **env)
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


def step(out, name, states):
    limit = zrig.env("LIMIT", "30")
    txt = boottime(out, name, LIMIT=limit)
    for l in txt.splitlines():
        if "PASS" in l or "FAIL" in l:
            print(l, flush=True)
    if "PASS user_id" not in txt:
        print(f"FAIL {name}: no user_id screen in {limit} s")
        return False
    if name == "first" and not emulog(out, name, "Zdxsv: fast boot: deleted the old boot state"):
        print(f"FAIL {name}: the planted old boot state was not deleted")
        return False
    if not emulog(out, name, "Zdxsv: fast boot: on"):
        print(f"FAIL {name}: no 'fast boot: on' in emulog")
        return False
    for line in ("saving", "loading"):
        if emulog(out, name, f"Zdxsv: fast boot: {line}"):
            print(f"FAIL {name}: fast boot is {line} a state")
            return False
    if glob.glob(states):
        print(f"FAIL {name}: a fast boot state in {os.path.dirname(states)}")
        return False
    return True


def main():
    zrig.need("OUT", "RUN")
    out, run = os.environ["OUT"], os.environ["RUN"]
    states = os.path.join(run, "p1", "PCSX2", "sstates", "* zdxsv-fastboot*.p2s")
    os.makedirs(os.path.dirname(states), exist_ok=True)
    with open(states.replace("* zdxsv-fastboot*", "SLPS-25419 zdxsv-fastboot 00000000 0000000000000000"), "wb") as f:
        f.write(b"not a state")
    os.makedirs(out, exist_ok=True)
    for name in ("first", "second"):
        if not step(out, name, states):
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
