"""Pictures after rollbacks by pixel color: a GGPO synctest with check=2 in the arcade battle and a
pixel watch (ZDXSV_INPUT_LATENCY count=0, 16 pixels, no presses, nothing written). PASS when every
vsync from the last watch line before the synctest showed a frame, frames after a rollback were shown, and none of
them repeated the frame before. "ZDXSV_SINGLE_DRAW_LIST=0 ZDXSV_RERUN_TAIL=1" is a FAIL control.
  OUT=<dir> python tests/zdxsv/rerunpix.py ["<env>"]
Env: FRAMES (synctest frames, default 1200).
"""
import os
import re
import shutil
import sys

import deltatest
import zrig

GRID = ";".join(f"{x}:{y}" for y in (56, 168, 280, 392) for x in (80, 240, 400, 560))


def num(s):
    m = re.match(r"-?[0-9]+", s or "")
    return int(m[0]) if m else 0


def counts(line):
    f = line.split()
    since = after = None
    au = other = 0
    for i, w in enumerate(f):
        if w.startswith("(") and i + 1 < len(f) and f[i + 1] == "since":
            since = w[1:]
        if w == "rollback" and i + 3 < len(f):
            after, au = f[i + 1], num(f[i + 3])
        if w == "other" and i + 1 < len(f):
            other = num(f[i + 1])
    return since, after, au, other


def verdict(line, base=""):
    since, after, au, other = counts(line)
    if since is None:
        return "FAIL: no pixel watch line"
    gap = num(since) - num(after) - other
    # menus and loads before the battle leave vsyncs without a new frame: count from the last line before it
    b_since, b_after, _, b_other = counts(base)
    gap -= num(b_since) - num(b_after) - b_other
    if abs(gap) > 1:
        return f"FAIL: {gap} vsyncs without a shown frame"
    if num(after) == 0:
        return "FAIL: no frame shown after a rollback"
    if au > 0:
        return f"FAIL: {au} frames after a rollback repeat the frame before"
    return "PASS"


def main(argv):
    zrig.need("OUT", "RUN")
    out = os.environ["OUT"]
    frames = int(zrig.env("FRAMES", "1200"))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    os.environ["ZDXSV_INPUT_LATENCY"] = f"count=0,pixel={GRID}"
    zrig.export(argv[0] if argv else "")
    _, lines = deltatest.run(["-Var", "ZDXSV_GGPO", "-Spec", f"start=9000,frames={frames},check=2,mask=fcff",
                              "-Route", "arcade", "-End", str(9100 + frames), "-Tag", "t", "-Out", out], show=False)
    for l in lines:
        if "synctest start" in l or "MISMATCH" in l:
            print(l)
    w = zrig.log_lines(os.path.join(out, "t-emulog.txt"), "ZdxsvLatency: pixel watch at vsync", "ZdxsvGgpo: synctest start")
    start = next((i for i, l in enumerate(w) if "synctest start" in l), len(w))
    base = next((l for l in reversed(w[:start]) if "pixel watch" in l), "")
    line = next((l for l in reversed(w[start:]) if "pixel watch" in l), "")
    print(base)
    print(line)
    v = verdict(line, base)
    print(v)
    return 0 if v == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
