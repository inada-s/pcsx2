"""Check that tests/zdxsv/README.md and docs/zdxsv/features.md name the same features.

  python tests/zdxsv/featurecheck.py [pcsx2 tree, default this repository]

Every `- **Name**` line of features.md needs a row in the README's "Tests per feature"
tables whose Feature cell is `Name` or `Name: scenario`; every such row must start with
a features.md name. Rows under "Upstream fixes" are fixes, not features, and are skipped.
Exit 0 = they agree; otherwise each mismatch is printed and the exit code is 1.
"""
import os
import re
import sys

UNCHECKED_SECTIONS = {"Upstream fixes"}


def features(text):
    return [m.group(1) for m in re.finditer(r"^- \*\*(.+?)\*\*", text, re.M)]


def rows(text):
    out = []
    section = None
    started = False
    for n, line in enumerate(text.splitlines(), 1):
        if line.startswith("## "):
            started = line == "## Tests per feature"
            continue
        if not started:
            continue
        if line.startswith("### "):
            section = line[4:].strip()
            continue
        if not line.startswith("| ") or section in UNCHECKED_SECTIONS:
            continue
        cell = re.split(r"(?<!\\)\|", line)[1].strip()
        if cell == "Feature":
            continue
        out.append((n, cell))
    return out


def feature_of(cell, names):
    if cell in names:
        return cell
    for name in sorted(names, key=len, reverse=True):
        if cell.startswith(name + ": "):
            return name
    return None


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    with open(os.path.join(root, "docs", "zdxsv", "features.md"), encoding="utf-8") as f:
        names = features(f.read())
    with open(os.path.join(root, "tests", "zdxsv", "README.md"), encoding="utf-8") as f:
        table = rows(f.read())
    bad = 0
    covered = set()
    for n, cell in table:
        name = feature_of(cell, set(names))
        if name is None:
            print(f"FAIL README.md:{n}: '{cell}' is not a features.md name (use 'Name' or 'Name: scenario')")
            bad += 1
        else:
            covered.add(name)
    for name in names:
        if name not in covered:
            print(f"FAIL features.md: '{name}' has no row in tests/zdxsv/README.md (a 'no test' row counts)")
            bad += 1
    if not names or not table:
        print("FAIL no features or no rows found: the document layout changed")
        bad += 1
    print(f"{len(names)} features, {len(table)} rows, {bad} mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
