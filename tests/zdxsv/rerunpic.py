"""Pictures after rollbacks: a GGPO synctest with check=2 (a 2-frame rollback after every 2nd frame) in
the arcade battle, a GS snapshot of every shown frame (ZDXSV_SNAP), then tools/zdxsv/rerunpic.py
judges them: PASS when the frames after a rollback move like the others. For changes to what rerun
frames skip; ZDXSV_RERUN_TAIL=1 is a FAIL control.
  OUT=<dir> python tests/zdxsv/rerunpic.py ["<env>"]   (e.g. "ZDXSV_RERUN_TAIL=1")
Env: FRAMES (synctest frames, default 400). Runs at 2x speed. ~1.5 min, ~700 snapshots (~0.5 GB at 1x).
"""
import os
import shutil
import subprocess
import sys

import deltatest
import zrig


def main(argv):
    zrig.need("OUT", "RUN")
    out = os.path.abspath(os.environ["OUT"])
    frames = int(zrig.env("FRAMES", "400"))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    start = deltatest.ARCADE_START
    os.environ["ZDXSV_SNAP"] = f"{out},1,{start},{start + frames}"
    zrig.export(argv[0] if argv else "")
    _, lines = deltatest.run(["-Var", "ZDXSV_GGPO", "-Spec", f"start={start},frames={frames},check=2,mask=fcff",
                              "-Route", "arcade", "-End", str(start + 100 + frames), "-Tag", "t", "-Out", out,
                              "-Speed", "2"], show=False)
    for l in lines:
        if any(k in l for k in ("synctest start", "MISMATCH", "rerun frame split")):
            print(l, flush=True)
    return subprocess.call([zrig.PY, os.path.join(zrig.TOOLS, "rerunpic.py"), out, str(start), str(start + frames)])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
