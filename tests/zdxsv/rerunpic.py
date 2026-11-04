"""Pictures after rollbacks: a GGPO synctest with check=2 (a 2-frame rollback after every 2nd frame) in
the arcade battle, a GS snapshot of every shown frame (ZDXSV_SNAP), then tools/zdxsv/rerunpic.py
judges them: PASS when the frames after a rollback move like the others. For changes to what rerun
frames skip; ZDXSV_RERUN_TAIL=1 is a FAIL control.
  OUT=<dir> python tests/zdxsv/rerunpic.py ["<env>"]   (e.g. "ZDXSV_RERUN_TAIL=1")
Env: FRAMES (synctest frames, default 400). ~4 min, ~700 snapshots (~0.5 GB at 1x).
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
    os.environ["ZDXSV_SNAP"] = f"{out},1,9000"
    zrig.export(argv[0] if argv else "")
    _, lines = deltatest.run(["-Var", "ZDXSV_GGPO", "-Spec", f"start=9000,frames={frames},check=2,mask=fcff",
                              "-Route", "arcade", "-End", str(9100 + frames), "-Tag", "t", "-Out", out], show=False)
    for l in lines:
        if any(k in l for k in ("synctest start", "MISMATCH", "rerun frame split")):
            print(l, flush=True)
    return subprocess.call([zrig.PY, os.path.join(zrig.TOOLS, "rerunpic.py"), out, "9000", str(9000 + frames)])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
