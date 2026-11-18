"""Checks the files the rigs need before a run and names the setup step that makes each missing one.

  python tests/zdxsv/rigcheck.py [base] [lobby] [trace] [rbk]   (default: all four)

base: PCSX2 build, ISO, the p1 data dir (inis, BIOS). lobby: zdxsv binaries, IP, RUN/zdxsv.db with
the USERS, one RUN/Mcd001-<card>.ps2 per CARDS. trace: TRACE. rbk: RBKSTATES.
Steps are those of "Setup" in README.md. Exit 0 = all present, 1 = something missing.
"""
import glob
import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zrig  # noqa: E402

missing = 0


def report(ok, what, step):
    global missing
    print(("ok      " if ok else "MISSING ") + what + ("" if ok else f" -> step {step}"))
    missing += not ok


def setting(name, step):
    v = zrig.env(name)
    report(bool(v), f"setting {name}", step)
    return v


def base():
    import launch
    if zrig.WIN or zrig.env("PCSX2_EXE"):
        exe = launch.exe_path()
        report(os.path.isfile(exe), f"PCSX2 build {exe}", 1)
    else:
        report(False, "setting PCSX2_EXE", 1)
    iso = setting("ISO", 2)
    if iso:
        report(os.path.isfile(iso), f"game image {iso}", 2)
    run = setting("RUN", 3)
    if run:
        p1 = os.path.join(run, "p1", "PCSX2")
        report(os.path.isfile(os.path.join(p1, "inis", "PCSX2.ini")), f"{p1}/inis/PCSX2.ini", 3)
        report(bool(glob.glob(os.path.join(p1, "bios", "*"))), f"{p1}/bios (a BIOS file)", 3)
    return run


def lobby(run):
    zdir = setting("ZDXSV_DIR", 4)
    for name, key in (("zdxsv", "ZBIN"), ("zproxy", "ZPROXY")):
        b = zrig.env(key) or (os.path.join(zdir, "bin", name + zrig.EXE) if zdir else "")
        if b:
            report(os.path.isfile(b), f"{name} binary {b}", 4)
    setting("IP", 4)
    cards, users = setting("CARDS", 5).split(), setting("USERS", 5).split()
    if not run:
        return
    for c in cards:
        p = os.path.join(run, f"Mcd001-{c}.ps2")
        report(os.path.isfile(p), f"card {p}", 5)
    db = os.path.join(run, "zdxsv.db")
    report(os.path.isfile(db), f"lobby database {db}", 4)
    if os.path.isfile(db) and users:
        try:
            with sqlite3.connect(f"file:{db}?mode=ro", uri=True) as c:
                have = {r[0] for r in c.execute("select user_id from user")}
        except sqlite3.Error as e:
            have = set()
            print(f"        {db}: {e}")
        for u in users:
            report(u in have, f"user {u} in {db}", 5)
    if len(cards) != len(users):
        report(False, f"CARDS ({len(cards)}) and USERS ({len(users)}) of equal length", 5)


def trace():
    t = setting("TRACE", 6)
    if t:
        for f in ("lobby-trace.log", "zdxsv.db"):
            report(os.path.isfile(os.path.join(t, f)), f"{t}/{f}", 6)


def rbk():
    d = setting("RBKSTATES", 7)
    if d:
        for i in range(1, 5):
            report(os.path.isfile(os.path.join(d, f"rbk-p{i}.p2s")), f"{d}/rbk-p{i}.p2s", 7)


def main():
    want = sys.argv[1:] or ["base", "lobby", "trace", "rbk"]
    bad = [w for w in want if w not in ("base", "lobby", "trace", "rbk")]
    if bad:
        sys.exit(f"unknown group {bad[0]}")
    run = base() if "base" in want else zrig.env("RUN")
    if "lobby" in want:
        lobby(run)
    if "trace" in want:
        trace()
    if "rbk" in want:
        rbk()
    print("rigcheck: " + ("PASS" if not missing else f"FAIL, {missing} missing"))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
