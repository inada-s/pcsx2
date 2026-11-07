"""Pad input latency + frame pacing in a 4-client battle: m4.py with p1 windowed and
measured (ZDXSV_INPUT_LATENCY on p1 only, presses start GODELAY s after mash begins),
p2-p4 headless and mashing. Stops everything once p1 reported. Exit 0 = p1 reported, no FAIL from m4.
  OUT=dir LOWLAT=0|1 SPEC='held=1,count=12' python tests/zdxsv/m4lat.py   -> search (pad buffer byte)
  OUT=dir LOWLAT=0|1 SPEC='addr=0x...,count=40' python tests/zdxsv/m4lat.py
  M4=m4ggpo.py: the battle over GGPO (as the product default), its env (GDELAY, ..) passes through.
  LOG=p1-emulog.txt python tests/zdxsv/m4lat.py: print a past run's result, no rig.
Needs a pcsx2 build with ZdxsvInputLatency go=/held=.
Prints the last TAILN (40) per-frame lines, then every summary line: many late presents
used to push the summaries out of a plain tail.
"""
import os
import re
import shutil
import subprocess
import sys
import time

import zrig

EACH = re.compile(r"ZdxsvLatency: ( |press [0-9]|late present [0-9])")


def summary(log):
    ls = zrig.log_lines(log, "ZdxsvLatency:")
    for l in [l for l in ls if EACH.search(l)][-int(zrig.env("TAILN", "40")):]:
        print(l[:200])
    for l in ls:
        if not EACH.search(l):
            print(l[:200])


def main():
    if zrig.env("LOG"):
        summary(os.environ["LOG"])
        return 0
    zrig.take()
    zrig.need("OUT", "SPEC", "RUN")
    out = os.environ["OUT"]
    os.makedirs(out, exist_ok=True)
    pd = os.path.join(os.environ["RUN"], "p1", "PCSX2")
    ini = os.path.join(pd, "inis", "PCSX2.ini")
    zrig.ini_set(ini, "EmuCore/GS", "ZdxsvLowLatencyPresent", "false" if zrig.env("LOWLAT", "1") == "0" else "true")
    for l in zrig.log_lines(ini, "ZdxsvLowLatencyPresent"):
        print(l)
    go = os.path.join(out, "go")
    if os.path.exists(go):
        os.remove(go)
    lat = (f"ZDXSV_INPUT_LATENCY=btn={zrig.env('BTN', 'down')},back={zrig.env('BACK', 'up')},go={go},"
           f"seed={zrig.env('SEED', '1')},out={os.path.join(out, 'lat.csv')},{os.environ['SPEC']}")
    e = dict(os.environ, SHOW="1", MASHP="2 3 4", GO=go, GODELAY=zrig.env("GODELAY", "30"), PCSX2_ENV_P1=lat)
    m4out = os.path.join(out, "m4.out")
    with open(m4out, "wb") as f:
        m4 = subprocess.Popen([zrig.PY, "-u", os.path.join(zrig.HERE, zrig.env("M4", "m4.py"))], env=e,
                              stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
    log = os.path.join(pd, "logs", "emulog.txt")
    t0 = time.time()
    while time.time() - t0 < int(zrig.env("MAXS", "540")):
        time.sleep(10)
        try:
            if zrig.log_lines(log, r"ZdxsvLatency: (search done|present->present)"):
                break
        except OSError:
            pass
        if zrig.log_lines(m4out, "^FAIL"):
            break
    print(f"{int(time.time() - t0)} s")
    for l in zrig.log_lines(m4out, "^FAIL|^PASS|client")[:12]:
        print(l)
    try:
        reported = bool(zrig.log_lines(log, r"ZdxsvLatency: (search done|present->present)"))
    except OSError:
        reported = False
    failed = bool(zrig.log_lines(m4out, "^FAIL"))
    summary(log)
    m4.terminate()
    zrig.kill_name("pcsx2", "zdxsv")
    zrig.kill_cmd(r"fake_lobby|drive[.]py|m4[a-z]*[.]py", r"powershell|m4lat")
    shutil.copyfile(log, os.path.join(out, "p1-emulog.txt"))
    if failed or not reported:
        print("FAIL " + ("m4 failed" if failed else "p1 reported no latency"))
        return 1
    print("PASS p1 reported")
    return 0


if __name__ == "__main__":
    sys.exit(main())
