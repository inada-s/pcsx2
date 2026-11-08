"""M4 P2P battle regression: battle server + fake_lobby.py --trace (no lobby server), 4 pcsx2
clients from a fresh boot (fast boot + fast login to fake_lobby.py's login API, seconds each), side entry, battle,
post-battle login -> BattleResult. Exit 0 = all checks passed.
  OUT=<dir> python tests/zdxsv/m4.py
Env: TRACE (an `EMU=1 m4z` OUT: lobby.log + zdxsv.db), CARDS (as m4z, the same cards), API (login API port,
  default 9881), BOOT_WAIT (s from launch to the fast boot end, default 60), IP, ZBIN, FAKE_ARGS (fake_lobby.py),
  PCSX2_ENV (extra env for the clients), MAXS (s from battle start to give up on BattleResult x4, default 2100;
  a headless battle can run 1370 s).
Every client joins the battle server by TCP (pcsx2 has no UDP bridge; an older pcsx2 build joins over UDP and
  fails the join checks).
MASH=<drive.py route> MASHP="1": battle input route and the clients it drives (default mash, all 4).
  MASHP="1" = time-up with side 1 ahead (2 kills, 0 deaths).
SHOW="1 3": these clients get a window (GS snapshots pN/<route>.png per drive call); others headless.
PCSX2_ENV_P1="..": extra env for one client only. GO=file: touched GODELAY s (30) after mash starts
  (ZDXSV_INPUT_LATENCY go= trigger, latency in battle: m4lat).
ZRELAY=ip:port[,ip:port...] ZRELAY_SESSION=id:hex token: a `zdxsv relay` (GGPO relay server) at each, logs
  relay.log, relay2.log, ...
GAUGE, COUNT, TIME: the battle rule (戦力ゲージ, 連続対戦数, 作戦時間 s; menu values) patched into the trace's
  (fake_lobby.py --rule-gauge/-count/-time). Default GAUGE=1 COUNT=1 TIME=90: one round, ended by the first
  shoot-down or 90 s (4 mashing clients rarely shoot anyone down); 0: the trace's value. MASHP="1" needs GAUGE=0.
POST=<route>: driven on all 4 after mash (default none: every post-battle screen has a countdown).
Checks: battlereg 4 users; `join success` (TCP) xJOINS (4; m4ggpo: 0, the EE answers for the battle server), no `join udp peer`, no `BattleInfo not found`;
  `BATTLE RESULT` x4 with the same total_frame and kills = deaths.
NORESULT=1 (a battle that must fail): wait for 8 lobby conns (4 + each client's reconnect) instead;
  checks every `BATTLE RESULT` has battles=0, conn from x8.
"""
import os
import re
import sys
import threading
import time

import freshboot as fb
import zrig

BRES = (r"BATTLE RESULT [0-9.:]* battles=[0-9]* win=[0-9]* lose=[0-9]* kill=[0-9]* death=[0-9]* "
        r"frames=[0-9]* side=[0-9]*")


def bres():
    """One per result, matched anywhere in a line (a result can be glued into another line)."""
    with open(fb.out("fake.log"), "rb") as f:
        return re.findall(BRES, f.read().decode("utf-8", "replace"))


def fields(r):
    return dict(x.split("=", 1) for x in r.split()[3:])


def grep_all(name, needle):
    with open(fb.out(name), "rb") as f:
        return f.read().decode("utf-8", "replace").count(needle)


def battlereg():
    p = os.environ["BATTLEREG"]
    if os.path.isfile(p):
        return True
    r = zrig.run([zrig.GOEXE, "build", "-o", p, "main.go"], cwd=os.path.join(fb.HERE, "battlereg"),
                 env=dict(os.environ, GO111MODULE="off"))
    return r.returncode == 0


def main():
    zrig.take()
    zrig.need("RUN", "OUT", "ZDXSV_DIR", "TRACE", "CARDS", "IP")
    os.makedirs(os.environ["OUT"], exist_ok=True)
    os.environ.setdefault("BATTLEREG", os.path.join(os.path.abspath(os.environ["RUN"]), "battlereg" + zrig.EXE))
    if not battlereg():
        print("FAIL battlereg build")
        return 1
    relays = []
    if zrig.env("ZRELAY"):
        sess, _, tok = zrig.env("ZRELAY_SESSION").partition(":")
        relays = [(a, sess, tok) for a in zrig.env("ZRELAY").split(",") if a]
    rule = [f"--rule-{k.lower()}={zrig.env(k, d)}" for k, d in (("GAUGE", "1"), ("COUNT", "1"), ("TIME", "90"))]
    fb.servers(rule + zrig.env("FAKE_ARGS").split(), relays)
    ok = [0]

    def check(name, cond):
        print(f"{'PASS' if cond else 'FAIL'} {name}", flush=True)
        if not cond:
            ok[0] = 1

    if not fb.fresh_boot_all([1, 2, 3, 4]):
        return 1
    for i in (1, 2, 3, 4):
        if not fb.fresh_entry(i):
            return 1
    t1 = time.time()
    sp = {}
    ts = [threading.Thread(target=lambda i=i: sp.__setitem__(i, fb.speed_check(i))) for i in (1, 2, 3, 4)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    if not all(sp.values()):
        return 1
    joins = int(zrig.env("JOINS", "4"))
    while time.time() - t1 < 15:
        try:
            if grep_all("fake.log", "registered 4 users") and fb.count(fb.out("battle.log"), "join success") >= joins:
                break
        except OSError:
            pass
        time.sleep(1)
    for l in zrig.log_lines(fb.out("fake.log"), "battlereg|adopted"):
        if not re.search(r"C->S|C<-S|S->C", l):
            print(l[:160], flush=True)
    check("battlereg 4", grep_all("fake.log", "registered 4 users") > 0)
    check(f"join tcp x{joins}", fb.count(fb.out("battle.log"), "join success") == joins)
    check("no join udp", grep_all("battle.log", "join udp peer") == 0)
    check("no BattleInfo not found", grep_all("battle.log", "BattleInfo not found") == 0)
    if zrig.env("GO"):
        threading.Timer(float(zrig.env("GODELAY", "30")), lambda: open(os.environ["GO"], "a").close()).start()
    fb.drive(zrig.env("MASH", "mash"), *zrig.env("MASHP", "1 2 3 4").split())
    t0, t34, last = time.time(), None, None
    maxs, r34s = float(zrig.env("MAXS", "2100")), float(zrig.env("R34S", "90"))
    while time.time() - t0 < maxs:
        if zrig.env("POST"):
            fb.drive(os.environ["POST"], 1, 2, 3, 4)
        else:
            time.sleep(5)
        r = len(bres())
        el = round(time.time() - t0)
        if zrig.env("NORESULT"):
            c = grep_all("fake.log", "conn from")
            if (r, c) != last or el % 30 < 5:
                print(f"{el} s: results {r}, lobby conns {c}/8", flush=True)
            last = (r, c)
            if c >= 8:
                break
            continue
        if r != last or el % 30 < 5:
            print(f"{el} s: results {r}/4", flush=True)
        last = r
        if r >= 4:
            break
        # a 4th result missing at 3/4 rarely comes (67 m4 outputs: 61 got 4/4 within 30 s, 1 the 4th 30 s
        # after 3/4, 5 never in 212-1541 s)
        if r == 3:
            t34 = t34 or time.time()
            if time.time() - t34 >= r34s:
                print(f"results 3/4 for {round(time.time() - t34)} s: 4th never comes, stop", flush=True)
                break
    res = bres()
    for x in res:
        print(" ".join(x.split()[2:11]), flush=True)
    if zrig.env("NORESULT"):
        check("no battle fought (every BATTLE RESULT battles=0)", all(fields(x)["battles"] == "0" for x in res))
        check("every client reconnected to the lobby (conn from x8)", grep_all("fake.log", "conn from") >= 8)
        return ok[0]
    check("BATTLE RESULT x4", len(res) >= 4)
    fs = [fields(x) for x in res]
    check("results agree", len(fs) >= 4 and len({f["frames"] for f in fs}) == 1
          and sum(int(f["kill"]) for f in fs) == sum(int(f["death"]) for f in fs))
    return ok[0]


if __name__ == "__main__":
    sys.exit(main())
