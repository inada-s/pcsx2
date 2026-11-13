"""A replay child next to a parent in the lobby: boottime.py (local stack, p1 fast logs in up to the user_id
screen), then a child pcsx2 (-zdxsvreplay, p1's data dir, the parent's pid) plays FILE to its end.
  OUT=<dir> FILE=<replay .pb> python tests/zdxsv/lobbychild.py
Env as boottime.py (ROUTE: netend.py's, to 戦場選択), plus STATE (default RBKSTATES/rbk-p1.p2s), CLAMP, SKIP_MS (default 1), TURBO (default 1),
MAXS_CHILD (s, default 300), KILL_PARENT_S (kill the parent that many s after the child starts; pass = the child
exits within 5 s), INPROC=1 (control, must FAIL: a plain instance on p1's data dir plays FILE), NOCHILD=1 (control: the parent alone for NOCHILD_S s, default 60).
Exit 0 = the child plays to `replay end` with 0 state hashes differing and exits by itself; it opens no TCP
connection and no UDP port; the parent stays alive with the same connections to the local server; p1's files are unchanged
outside replay-child/ and logs/.
"""
import os
import re
import shutil
import subprocess
import sys
import time

import boottime
import launch
import rplay
import zrig


def udp(pid):
    return zrig.ps(f"Get-NetUDPEndpoint -OwningProcess {pid} -EA 0 | % {{ $_.LocalAddress + ':' + $_.LocalPort }}").split()


def play_child(out, p1, parent, t0):
    clog = os.path.join(p1, "replay-child", "logs", "emulog.txt")
    if os.path.exists(clog):
        os.remove(clog)
    e = launch.pcsx2_env(dict(os.environ))
    e.update({"ZDXSV_REPLAY_EXIT": "1", "ZDXSV_PW_HASH": "1",
              "ZDXSV_NET_TRACE": os.path.join(out, "trace-play.txt"),
              "ZDXSV_REPLAY_SKIP_MS": zrig.env("SKIP_MS", "1"),
              "ZDXSV_REPLAY_STATE": os.path.abspath(zrig.env("STATE") or
                                                    os.path.join(os.environ["RBKSTATES"], "rbk-p1.p2s"))})
    if zrig.env("TURBO", "1") == "1":
        e["ZDXSV_REPLAY_TURBO"] = "1"
    if zrig.env("CLAMP"):
        e["ZDXSV_EE_CLAMP"] = os.environ["CLAMP"]
    a = [launch.exe_path(), "-datapath", os.path.join(os.environ["RUN"], "p1"), "-batch", "-nogui"]
    if zrig.env("INPROC") == "1":
        e["ZDXSV_REPLAY"] = os.path.abspath(os.environ["FILE"])
    else:
        a += ["-zdxsvreplay", f"-1:{parent}:{os.path.abspath(os.environ['FILE'])}"]
    a += ["--", os.path.normpath(os.environ["ISO"])]
    child = subprocess.Popen(a, env=e, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL).pid
    print(f"child pid {child}", flush=True)
    seen_tcp, seen_udp = set(), set()
    maxs = int(zrig.env("MAXS_CHILD", "300"))
    kill_at = float(zrig.env("KILL_PARENT_S", "0"))
    killed = 0
    while time.time() - t0 < maxs and zrig.alive(child):
        if kill_at and not killed and time.time() - t0 >= kill_at:
            zrig.kill(parent)
            killed = time.time()
            print(f"{int(killed - t0)} s: parent killed", flush=True)
        if killed and time.time() - killed > 5:
            break
        seen_tcp.update(zrig.remote(child))
        seen_udp.update(udp(child))
        time.sleep(2)
    if killed:
        gone = not zrig.alive(child)
        print(f"{'PASS' if gone else 'FAIL'} child {'gone' if gone else 'still running'} {time.time() - killed:.1f} s after the parent was killed")
        zrig.kill(child)
        return -1 if gone else 1
    print(f"{int(time.time() - t0)} s: child {'still running' if zrig.alive(child) else 'gone'}", flush=True)
    ok = 0
    if zrig.alive(child):
        print("FAIL the child did not exit")
        zrig.kill(child)
        ok = 1
    if seen_tcp or seen_udp:
        print(f"FAIL child network: tcp {' '.join(sorted(seen_tcp))} udp {' '.join(sorted(seen_udp))}")
        ok = 1
    lines = rplay.grep(clog, r"ZdxsvGgpo: (replay end|replay state check|replay:)")
    for x in lines:
        print(x[:200])
    if not any("replay end" in x for x in lines):
        print("FAIL no replay end in the child's emulog")
        ok = 1
    checks = [x for x in lines if "replay state check" in x]
    if not checks or any(int(n) for x in checks for n in re.findall(r"(\d+) differ", x)):
        print("FAIL replay state check missing or hashes differ")
        ok = 1
    try:
        shutil.copyfile(clog, os.path.join(out, "emulog-child.txt"))
    except OSError:
        pass
    return ok


def after(out):
    p1 = os.path.join(os.environ["RUN"], "p1", "PCSX2")
    parents = zrig.pids_name("pcsx2")
    if len(parents) != 1:
        print(f"FAIL {len(parents)} pcsx2 before the child")
        return 1
    parent = parents[0]
    skip = lambda k: k.startswith("logs" + os.sep)
    before = {k: v for k, v in rplay.files_digest(p1).items() if not skip(k)}
    lobby = lambda: sorted(c for c in zrig.remote(parent) if c.startswith(os.environ["IP"] + ":"))
    conn = lobby()
    if not conn:
        print("FAIL the parent has no connection to the lobby")
        return 1
    print(f"parent pid {parent} tcp {' '.join(conn)}", flush=True)
    t0 = time.time()
    if zrig.env("NOCHILD") == "1":
        time.sleep(int(zrig.env("NOCHILD_S", "60")))
        ok = 0
    else:
        ok = play_child(out, p1, parent, t0)
        if ok == -1:
            return 0
    if not zrig.alive(parent):
        print("FAIL the parent is gone")
        return 1
    conn2 = lobby()
    if conn2 != conn:
        print(f"FAIL parent tcp changed: {' '.join(conn2)}")
        ok = 1
    aft = {k: v for k, v in rplay.files_digest(p1).items() if not skip(k)}
    changed = sorted(k for k in set(before) | set(aft) if before.get(k) != aft.get(k))
    if changed:
        print(f"FAIL p1 files changed: {' '.join(changed[:10])}")
        ok = 1
    print(f"{'PASS' if ok == 0 else 'FAIL'} parent {parent} in the lobby, {int(time.time() - t0)} s")
    return ok


def main():
    import netend
    os.environ.setdefault("ROUTE", netend.ROUTE)
    os.environ.setdefault("ROUTE_END", "1")
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "OUT", "CARDS", "RUN", "ISO")
    if zrig.env("NOCHILD") != "1":
        zrig.need("FILE")
    os.environ.setdefault("LIMIT", "60")
    return boottime.run(os.environ["OUT"], after)


if __name__ == "__main__":
    sys.exit(main())
