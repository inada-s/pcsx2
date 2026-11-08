# frameosd.py [--rollback] <emulog>...: exit 1 unless every log has `frame osd` lines (ZDXSV_FRAME_OSD=1) whose stages
# add up to the frame total; --rollback: the rollback stage is above 0 on some line of every log, else it is 0 on all.
import re
import sys

LINE = re.compile(r"frame osd frame (\d+): frames \d+ total ([\d.]+) emulate ([\d.]+) rollback ([\d.]+) state ([\d.]+) "
                  r"wait ([\d.]+) sleep ([\d.]+) other ([\d.]+)")


def main():
    args = sys.argv[1:]
    want_rollback = "--rollback" in args
    logs = [a for a in args if a != "--rollback"]
    if not logs:
        print(__doc__ or "usage: frameosd.py [--rollback] <emulog>...")
        return 2
    ok = True
    for path in logs:
        with open(path, encoding="utf-8", errors="replace") as f:
            rows = [tuple(float(x) for x in m.groups()) for m in map(LINE.search, f) if m]
        if not rows:
            print(f"FAIL {path}: no frame osd lines")
            ok = False
            continue
        for r in rows:
            total, stages = r[1], r[2:]
            if abs(sum(stages) - total) > 0.01:
                print(f"FAIL {path}: frame {int(r[0])} stages {sum(stages):.3f} != total {total:.3f}")
                ok = False
        rollback = max(r[3] for r in rows)
        if (rollback > 0) != want_rollback:
            print(f"FAIL {path}: max rollback {rollback:.3f} ms, expected {'> 0' if want_rollback else '0'}")
            ok = False
        else:
            print(f"ok {path}: {len(rows)} lines, max rollback {rollback:.3f} ms")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
