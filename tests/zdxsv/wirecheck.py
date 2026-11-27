"""Check that tests/zdxsv/testdata/wire equals zdxsv's testdata/wire, the source of the wire fixtures.

  python tests/zdxsv/wirecheck.py [zdxsv tree, default a shallow clone of the zdxsv master branch]

Same file names, same contents (line endings ignored). A fixture change lands in zdxsv first,
then is copied here. Exit 0 = they agree; otherwise each difference is printed and the exit code is 1.
"""
import os
import subprocess
import sys
import tempfile

ZDXSV_URL = "https://github.com/inada-s/zdxsv"
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata", "wire")


def read(d):
    out = {}
    for name in sorted(os.listdir(d)):
        with open(os.path.join(d, name), "rb") as f:
            out[name] = f.read().replace(b"\r\n", b"\n")
    return out


def compare(theirs):
    ours = read(HERE)
    fails = []
    for name in sorted(set(ours) | set(theirs)):
        if name not in ours:
            fails.append(f"{name}: only in zdxsv")
        elif name not in theirs:
            fails.append(f"{name}: only here")
        elif ours[name] != theirs[name]:
            fails.append(f"{name}: differs")
    for f in fails:
        print("FAIL " + f)
    print(f"{len(ours)} here, {len(theirs)} in zdxsv, {len(fails)} differences")
    return 1 if fails else 0


def main():
    if len(sys.argv) > 1:
        return compare(read(os.path.join(sys.argv[1], "testdata", "wire")))
    with tempfile.TemporaryDirectory() as tmp:
        tree = os.path.join(tmp, "zdxsv")
        for cmd in (["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--sparse", ZDXSV_URL, tree],
                    ["git", "-C", tree, "sparse-checkout", "set", "testdata/wire"]):
            if subprocess.call(cmd):
                print("FAIL " + " ".join(cmd))
                return 1
        return compare(read(os.path.join(tree, "testdata", "wire")))


if __name__ == "__main__":
    sys.exit(main())
