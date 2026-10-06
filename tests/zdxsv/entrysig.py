"""Emulog lines of a failed lobby entry that a passing entry never logged.

  entrysig.py FAILED_EMULOG REF_EMULOG [MORE_REFS..]   prints the failed log's new lines (normalized), exit 0
  entrysig.py --same A B REF..                         exit 0 when A and B share >= 1 new line (same fault), else 1

Lines are compared without timestamps, numbers and hex (pids, ports, addresses, sizes differ per run).
References: $RUN/entry-ok-p*.txt, the emulog at a passing entry (entry1.sh writes them).
An entry flake logs nothing new: 12 flake tries vs passing references: 0 new lines
(the game's RST teardown lines are NOISE).
A fault in the client (e.g. a test that breaks the adopted lobby connection) logs the same new line every try.
"""
import re
import sys

NUM = re.compile(r"0x[0-9a-fA-F]+|\d+")
# lines of every boot that vary in text, not in meaning
NOISE = re.compile(r"^(Loading|Saving|Closing|Opening|Load|Save)|memcard|Mcd00|\.p2s|\.ps2|snaps|Elapsed|fps|FPS|TCP: Reset closed connection|Closed Dead TCP Connection|^PCSX# ")


def norm(line):
    line = re.sub(r"^\[\s*[\d.]+\]\s*", "", line.rstrip("\r\n"))
    return NUM.sub("#", line).strip()


def lines(path):
    with open(path, "rb") as f:
        return [norm(l.decode("utf-8", "replace")) for l in f]


def new_lines(failed, refs):
    seen = set()
    for r in refs:
        seen.update(lines(r))
    out = []
    for l in lines(failed):
        if l and l not in seen and not NOISE.search(l) and l not in out:
            out.append(l)
    return out


def main(argv):
    if argv and argv[0] == "--same":
        a, b, refs = argv[1], argv[2], argv[3:]
        common = [l for l in new_lines(a, refs) if l in set(new_lines(b, refs))]
        for l in common:
            print("same new line:", l[:200])
        return 0 if common else 1
    for l in new_lines(argv[0], argv[1:]):
        print(l[:200])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
