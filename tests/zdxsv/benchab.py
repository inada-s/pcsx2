"""Speed A/B on one build: env knobs are the switch (each speed change keeps a control env, e.g.
ZDXSV_RERUN_VU1=1). Runs rbk.py N=2 seed 1 alternating A, B, A, B, .. (R runs per side) and prints,
per part of the battle report, each side's mean and min-max over every client log (R x 2 samples);
`differs` only when the two ranges do not overlap. Parts: rerun frame mean (wall), spu2 / vu1 ms per
rerun frame, save / load ms mean, emulate mean.
  OUT=<dir> python tests/zdxsv/benchab.py "<A env>" "<B env>" [R=2]   (e.g. "" "ZDXSV_RERUN_REVERB=1")
Env: as rbk.py; defaults TRACE=0 LAT=40 JITTER=8. PCSX2_ENV is kept, the side's env is appended.
Logs: $OUT/a1, b1, a2, .. (rbk.py OUT), summary $OUT/benchab.txt. A failed rbk.py run stops it (exit 1).
"""
import glob
import os
import re
import subprocess
import sys

import zrig

NUM = re.compile(r"[-+]?([0-9]+[.]?[0-9]*|[.][0-9]+)([eE][-+]?[0-9]+)?")
PARTS = (("rerun frame mean", "rerun_frame_mean"), ("emulate mean", "emulate_mean"), ("save ms mean", "save_ms_mean"),
         ("load ms mean", "load_ms_mean"), ("spu2 ms", "spu2_ms"), ("vu1 ms", "vu1_ms"))


def values(line):
    """Battle report line -> [(part, value)]: the word after each part's words."""
    w = line.split()
    r = []
    for i in range(len(w)):
        for words, part in PARTS:
            k = words.split()
            if w[i:i + len(k)] == k and i + len(k) < len(w):
                m = NUM.match(w[i + len(k)])
                r.append((part, float(m.group(0)) if m else 0.0))
    return r


def main(argv):
    zrig.need("OUT")
    out = os.environ["OUT"]
    a, b, reps = (argv + ["", "", ""])[:3]
    reps = reps or "2"
    os.makedirs(out, exist_ok=True)
    for k, v in (("TRACE", "0"), ("LAT", "40"), ("JITTER", "8")):
        os.environ.setdefault(k, v)
    base = zrig.env("PCSX2_ENV")
    for r in range(1, int(reps) + 1):
        for s, e in (("a", a), ("b", b)):
            print(f"== {s}{r}: {e or '(no env)'}", flush=True)
            txt = os.path.join(out, f"{s}{r}.txt")
            with open(txt, "wb") as f:
                rc = subprocess.call([zrig.PY, "-u", os.path.join(zrig.HERE, "rbk.py"), "2", "1"],
                                     env=dict(os.environ, PCSX2_ENV=f"{base} {e}", OUT=os.path.join(out, f"{s}{r}")),
                                     stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
            if rc:
                with open(txt, encoding="utf-8", errors="replace") as f:
                    print("".join(f.readlines()[-5:]), end="")
                print(f"FAIL {s}{r}: rbk.py rc {rc} (log {txt})")
                return 1
    vals, order = {}, []
    for f in sorted(glob.glob(os.path.join(out, "[ab][0-9]*", "emulog-p*.txt"))):
        s = os.path.basename(os.path.dirname(f))[0]
        for line in zrig.log_lines(f, "ZdxsvGgpo: net battle end"):
            for part, v in values(line):
                vals.setdefault((s, part), []).append(v)
                if part not in order:
                    order.append(part)
    rows = [f"A: {a or '(no env)'}", f"B: {b or '(no env)'}",
            f"{'part':<17} {'A mean [min-max] n':<26} {'B mean [min-max] n':<26} verdict"]
    for part in order:
        va, vb = vals.get(("a", part)), vals.get(("b", part))
        if not va or not vb:
            rows.append(f"{part:<17} missing on a side")
            continue
        d = "differs" if max(va) < min(vb) or max(vb) < min(va) else "overlap"
        fa = f"{sum(va) / len(va):.3f} [{min(va):.3f}-{max(va):.3f}] {len(va)}"
        fb_ = f"{sum(vb) / len(vb):.3f} [{min(vb):.3f}-{max(vb):.3f}] {len(vb)}"
        rows.append(f"{part:<17} {fa:<26} {fb_:<26} {d}")
    with open(os.path.join(out, "benchab.txt"), "w") as f:
        f.write("\n".join(rows) + "\n")
    print("\n".join(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
