"""Check the pcsx2 replay key display (ZDXSV_REPLAY_KEY_DISPLAY=1) against the replay file.

  python keycheck.py X.zdxr emulog.txt [--min N]

pcsx2 logs `ZdxsvGgpo: replay keys frame F pos P: bbbb*n ...` every 600 played frames: the runs of
position P's B word (input bytes 2-3, bit 0 dropped) up to frame F, newest first, at most 14.
This recomputes them from the file's inputs (frames 0..F) and compares. --min N: at least N lines (default 1).
Exit 1 on any mismatch or too few lines.
"""
import re
import struct
import sys

from replay_check import load

KEY_RUNS = 14
LINE = re.compile(r"ZdxsvGgpo: replay keys frame (\d+) pos (\d+):(.*)")


def runs(inputs, players, isz, f, p):
    out = []
    for g in range(f, -1, -1):
        off = (g * players + p) * isz + 2
        b = struct.unpack_from("<H", inputs, off)[0] & ~1
        if out and out[-1][0] == b:
            out[-1][1] += 1
        elif len(out) == KEY_RUNS:
            break
        else:
            out.append([b, 1])
    return " ".join(f"{b:04x}*{n}" for b, n in out)


def main():
    args = sys.argv[1:]
    need = 1
    if "--min" in args:
        i = args.index("--min")
        need = int(args[i + 1])
        del args[i:i + 2]
    h, state, inputs, players, frames, isz = load(args[0])
    seen = bad = 0
    for line in open(args[1], encoding="utf-8", errors="replace"):
        m = LINE.search(line)
        if not m:
            continue
        f, p, got = int(m[1]), int(m[2]), m[3].strip()
        want = runs(inputs, players, isz, f, p)
        seen += 1
        if got != want:
            bad += 1
            print(f"FAIL frame {f} pos {p}\n  got  {got}\n  want {want}")
    print(f"{'PASS' if not bad and seen >= need else 'FAIL'} key lines {seen}, mismatched {bad} (min {need})")
    sys.exit(0 if not bad and seen >= need else 1)


if __name__ == "__main__":
    main()
