"""zbincheck.py <exe>: FAIL (exit 1) unless the go binary was built from the zdxsv checkout as it is now
(vcs.revision == HEAD, not vcs.modified, clean src/). Called by stack.py and m4z.py before any client starts.
ZREV=<rev prefix>: expect that build instead (a control on an old build); ZREV=any: no check.
Env: ZDXSV_DIR (zdxsv checkout), GOEXE (go binary, default go).
"""
import os
import re
import sys

import zrig


def check(exe):
    """0 or 1, with the verdict line printed."""
    zrig.need("ZDXSV_DIR")
    zrev = zrig.env("ZREV")
    if zrev == "any":
        return 0
    if not os.path.isfile(exe):
        print(f"FAIL zbincheck: no {exe}", flush=True)
        return 1
    r = zrig.run([zrig.GOEXE, "version", "-m", exe])
    info = (r.stdout + r.stderr).replace("\r", "")
    rev = (re.search(r"vcs\.revision=(\S+)", info) or [None, ""])[1]
    mod = (re.search(r"vcs\.modified=(\S+)", info) or [None, ""])[1] == "true"
    if not rev:
        print(f"FAIL zbincheck: {exe} has no vcs.revision (built without git info?)", flush=True)
        return 1
    if zrev:
        if rev.startswith(zrev):
            print(f"zbincheck: {exe} = {rev} (ZREV)", flush=True)
            return 0
        print(f"FAIL zbincheck: {exe} is {rev}, ZREV={zrev}", flush=True)
        return 1
    z = os.environ["ZDXSV_DIR"]
    head = zrig.run(["git", "-C", z, "rev-parse", "HEAD"]).stdout.strip()
    dirty = zrig.run(["git", "-C", z, "status", "--porcelain", "--", "src", "go.mod", "go.sum"]).stdout.strip()
    if rev != head or mod or dirty:
        br = zrig.run(["git", "-C", z, "branch", "--show-current"]).stdout.strip()
        print(f"FAIL zbincheck: {exe} built from {rev[:9]}{'+modified' if mod else ''}, checkout {z} is "
              f"{head[:9]}{'+modified' if dirty else ''} ({br})")
        print(f"  rebuild: (cd {z} && {zrig.GOEXE} build -o {exe} ./src/zdxsv)  [commit edits first], "
              f"or ZREV={rev[:9]} for this old build", flush=True)
        return 1
    print(f"zbincheck: {exe} = {head[:9]} (checkout HEAD)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(check(sys.argv[1]))
