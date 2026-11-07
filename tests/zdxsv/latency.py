"""Pad input latency on the title main menu. One instance, p1 data dir, windowed.
  latency.py                      -> measure on the menu cursor byte, 40 presses
  latency.py -Spec 'count=10'     -> search mode (no addr=): candidate bytes
  latency.py -LowLatency 0|1      -> sets EmuCore/GS ZdxsvLowLatencyPresent in p1's ini first
  -Out latency.csv (CSV per press), -Seed 1
p1's card autoloads system data; f2600 + Start = attract -> main menu.
Prints the ZdxsvLatency log lines.
"""
import os
import re
import subprocess
import sys

import launch
import zrig


def main(argv):
    o = {"-Spec": "addr=0x00b45ac0,count=40", "-Out": "latency.csv", "-Seed": "1", "-LowLatency": ""}
    for k, v in zip(argv[::2], argv[1::2] + [""] * (len(argv) % 2)):
        if k not in o:
            print(f"FAIL latency.py: unknown argument {k}")
            return 2
        o[k] = v
    zrig.need("RUN")
    pd = os.path.join(os.environ["RUN"], "p1", "PCSX2")
    if o["-LowLatency"]:
        zrig.ini_set(os.path.join(pd, "inis", "PCSX2.ini"), "EmuCore/GS", "ZdxsvLowLatencyPresent",
                     "true" if o["-LowLatency"] == "1" else "false")
    m = re.search(r"count=([0-9]+)", o["-Spec"])
    count = int(m.group(1)) if m else 20
    lat = f"btn=down,back=up,start=2900,gap=60,seed={o['-Seed']},out={o['-Out']},{o['-Spec']}"
    launch.launch(["1"], extra_env={"ZDXSV_INPUT_LATENCY": lat}, quiet=True)
    pine = [zrig.PY, os.path.join(zrig.HERE, "pine.py"), "seq"]
    subprocess.call(pine + ["f2600,Return"], stderr=subprocess.DEVNULL)
    subprocess.call(pine + [f"f{2900 + 60 * count + 60}"], stderr=subprocess.DEVNULL)
    for line in zrig.log_lines(os.path.join(pd, "logs", "emulog.txt"), "ZdxsvLatency"):
        print(line)
    zrig.kill_name("pcsx2")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
