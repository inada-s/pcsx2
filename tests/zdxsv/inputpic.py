"""Input to picture on the title main menu: ZDXSV_INPUT_LATENCY presses down/up 8 times (from vsync 2000,
every 30) and watches one pixel of the highlighted item (pixel=); prints the vsync of each press' picture
change. Exit 1 when a press has no change. Compare two envs by their offsets (the zero point is arbitrary).
  OUT=<dir> python tests/zdxsv/inputpic.py ["<env>"]
"""
import os
import re
import shutil
import sys

import deltatest
import zrig


def main(argv):
    zrig.need("OUT", "RUN")
    out = os.environ["OUT"]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    shutil.rmtree(os.path.join(os.environ["RUN"], "p1", "PCSX2", "patches"), ignore_errors=True)
    os.environ["ZDXSV_INPUT_LATENCY"] = "addr=0x00b45ac0,start=2000,count=8,gap=30,hold=4,pixel=433:104"
    zrig.export(argv[0] if argv else "")
    deltatest.run(["-Var", "ZDXSV_GGPO", "-Spec", "start=9000,frames=400,check=2,mask=fcff",
                   "-Route", "f1500,Return", "-End", "2300", "-Tag", "t", "-Out", out], show=False)
    lines = zrig.log_lines(os.path.join(out, "t-emulog.txt"),
                           r"ZdxsvLatency: press [0-9]* [a-z]* picture", r"ZdxsvLatency: picture changes")
    for l in lines:
        print(re.sub(r"^\[[ 0-9.]*\] ", "", l))
    ok = any(re.search(r"ZdxsvLatency: picture changes .* presses 8 .* with a change 8,", l) for l in lines)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
