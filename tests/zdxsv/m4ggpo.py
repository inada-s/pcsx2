"""m4.py with GGPO started from the lobby's battle info (pcsx2 ZDXSV_GGPO net=1,lobby=1): no static
host=/relay=, the peers come from fake_lobby.py --ggpo (ggpo_<user>=PORT+k-1 for the k-th client).
Env: OUT (required), PORT (7101), GDELAY (GGPO frame delay, 1; auto = no delay=, picked from the ping test rtt),
  GMIN (mindelay=), GGPO_LAT (one-way ms on every GGPO path: fake_lobby.py --ggpo-delay),
  P2P (v4 default, v6, dual: fake_lobby.py --p2p; peers' addresses IPv4, this box's IPv6, both),
  GGPO_CLIENTS ("1 2 3 4": clients with lobby=1 and a ggpo_ line; fewer = control, expect every client
  cutting the battle connection: a peer has no GGPO address); other env goes to m4.py.
Checks (besides m4.py's): GGPO clients: `lobby peer position` x(N-1) and `net player` each;
  each client with a GGPO session: `peer report` with timesync=, input_blocks=, fps_history= and loss_ x(N-1);
  GDELAY=auto: `ping test` and `lobby delay E`, E = max(GMIN or 2, ceil(2*GGPO_LAT/32));
  P2P=v6: every peer at an IPv6 address; P2P=dual + GDELAY=auto: every peer's IPv6 candidate answered
  the ping test; P2P!=v4: every client found its IPv6 (`udp: ... IPv6 [`);
  control: `connection cut: a peer has no GGPO address` on each lobby=1 client, no `net player` anywhere.
  BAD_SESSION=K (GDELAY=auto; fake_lobby.py --bad-session): client K's ping test gets no answer and drops
  packets; every other client gets rtt -1 from K only, and drops packets; then every client cuts the battle
  connection (no GGPO, no TCP fallback) with the opponent-did-not-answer OSD, the game gives up on it and
  reconnects to the lobby (m4.py NORESULT: no BATTLE RESULT, conn from x8; MAXS default 360).
  RELAY=1 (GDELAY=auto, with GGPO_LAT): a `zdxsv relay` on 127.0.0.1:8203 (m4.py ZRELAY) and fake_lobby.py --relay;
  every path `relay 0`, peers at the relay, lobby delay max(GMIN or 2), relay.log forwarded > 0.
  RELAY=1 RELAY_ALT=1: the relay advertised at another address than it answers from; the same checks
  (control: a build that requires the advertised source: every path `direct`).
  RELAY=4: relay_0..3 offered, nothing at 0 and 1, `zdxsv relay`s at 2 and 3: every path `relay 2`, the other
  checks as RELAY=1.
  GDELAY=auto without RELAY: every path `direct`.
  NOADDR=K (GDELAY=auto; fake_lobby.py --no-addr): the others get no address of client K; the ping test
  finds it at the source of its pings, so the checks are the plain ones (control: a build without that: cut).
  BLOCK=K-L (GDELAY=auto, with GGPO_LAT; fake_lobby.py --ggpo-block): clients K and L get no path to each
  other, so each reaches the other through a third peer: K and L `path ... peer` x1 and `(relay)` x1, every
  other path `direct`, K and L take the lobby delay from the peer path rtt (control: the plain checks on those logs fail on K and L).
  UNREACH=K (GDELAY=auto, with GGPO_LAT; fake_lobby.py --ggpo-block K-x for every other client x): nobody has a
  path to client K, not even through a peer: K gets no answer, every other client gets rtt -1 from K only;
  then the BAD_SESSION checks (every client cuts, the game gives up and reconnects to the lobby).
"""
import os
import re
import socket
import subprocess
import sys

import zrig

RC = [0]


def ok(name, cond):
    print(f"{'PASS' if cond else 'FAIL'} {name}", flush=True)
    if not cond:
        RC[0] = 1


def has_addr(ip):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.bind((ip, 0))
            return True
        except OSError:
            return False


def main():
    zrig.need("RUN")
    run = os.environ["RUN"]
    port = int(zrig.env("PORT", "7101"))
    gc = zrig.env("GGPO_CLIENTS", "1 2 3 4").split()
    gdelay = zrig.env("GDELAY", "1")
    auto = gdelay == "auto"
    gmin, lat = zrig.env("GMIN"), zrig.env("GGPO_LAT")
    relay, alt = zrig.env("RELAY"), zrig.env("RELAY_ALT")
    # RELAY checks assume the ping test (GDELAY=auto); a fixed delay skips it
    if relay and not auto:
        print(f"m4ggpo: RELAY needs GDELAY=auto (got {gdelay})")
        return 2
    if relay and alt and not has_addr("127.0.0.2"):
        print("m4ggpo: RELAY_ALT needs 127.0.0.2 on the loopback (macOS: sudo ifconfig lo0 alias 127.0.0.2)")
        return 2
    for i in gc:
        prev = zrig.env(f"PCSX2_ENV_P{i}")  # keep a caller's per-client env (m4lat.py)
        g = f"ZDXSV_STUN_PORT=8201 ZDXSV_GGPO=net=1,lobby=1,port={port + int(i) - 1}" + ("" if auto else f",delay={gdelay}") + \
            (f",mindelay={gmin}" if gmin else "")
        os.environ[f"PCSX2_ENV_P{i}"] = f"{prev} {g}" if prev else g
    p2p, bad, noaddr = zrig.env("P2P", "v4"), zrig.env("BAD_SESSION"), zrig.env("NOADDR")
    if noaddr and not auto:
        print(f"m4ggpo: NOADDR needs GDELAY=auto (got {gdelay})")
        return 2
    block, unreach = zrig.env("BLOCK"), zrig.env("UNREACH")
    if block and (not auto or not lat):
        print("m4ggpo: BLOCK needs GDELAY=auto and GGPO_LAT")
        return 2
    if unreach and (not auto or not lat):
        print("m4ggpo: UNREACH needs GDELAY=auto and GGPO_LAT")
        return 2
    gb = ",".join(f"{unreach}-{i}" for i in gc if i != unreach) if unreach else block
    cut = bad or unreach
    # a bad session's packets arrive and are dropped; an unreachable peer's never arrive
    cutk, dl, dq = ("unreachable", "0 dropped", r" 0 dropped") if unreach else ("bad session", "drops", r" [1-9][0-9]* dropped")
    if cut:
        os.environ["NORESULT"] = "1"
        os.environ.setdefault("MAXS", "360")
    ra, rs = "127.0.0.1:8203", "1234:abcd"
    if relay:
        os.environ["ZRELAY"], os.environ["ZRELAY_SESSION"] = ra, rs
        # RELAY_ALT=1: the relay listens on every address and is advertised at 127.0.0.2, so it answers from 127.0.0.1
        if alt:
            ra, os.environ["ZRELAY"] = "127.0.0.2:8203", ":8203"
    # RELAY=4: relay_0..3 offered, nothing listens at 0 and 1, 2 (RA) and 3 run: every path takes relay 2
    rl, ri = ra, "0"
    if relay == "4":
        rl, ri = f"127.0.0.1:8261,127.0.0.1:8262,{ra},127.0.0.1:8263", "2"
        os.environ["ZRELAY"] = f"{ra},127.0.0.1:8263"
    fa = f"--ggpo {port} --ggpo-clients {''.join(gc)}" + (f" --ggpo-delay {lat}" if lat else "") + f" --p2p {p2p}" + \
        (f" --bad-session {bad}" if bad else "") + (f" --no-addr {noaddr}" if noaddr else "") + \
        (f" --ggpo-block {gb}" if gb else "") + (f" --relay {rl} --relay-session {rs}" if relay else "")
    RC[0] = subprocess.call([zrig.PY, "-u", os.path.join(zrig.HERE, "m4.py")], env=dict(os.environ, FAKE_ARGS=fa))
    n = len(gc)
    e = max((2 * int(lat or 0) + 31) // 32 if not relay else 0, int(gmin or 2))
    logs = {}
    for i in gc + ["1", "2", "3", "4"]:
        try:
            logs[i] = zrig.log_lines(os.path.join(run, f"p{i}", "PCSX2", "logs", "emulog.txt"), "")
        except OSError:
            logs[i] = []

    for i in gc:
        ls = logs[i]

        def g(rx):
            return [l for l in ls if re.search(rx, l)]

        def c(rx):
            return len(g(rx))

        ping = g(r"zdxsv: ping test")
        blk = bool(block) and f"-{i}-" in f"-{block}-"
        for l in g(r"ZdxsvGgpo: (lobby|net player|net armed)|zdxsv: ping test"):
            print(f"p{i} {l[:300]}")
        if n != 4:
            ok(f"p{i} connection cut (a peer has no GGPO port)", c(r"ZdxsvGgpo: lobby battle connection cut: a peer has no GGPO address"))
            continue
        if not cut:
            ok(f"p{i} lobby peers x3", c(r"ZdxsvGgpo: lobby peer position") == 3)
            ok(f"p{i} ggpo session", c(r"ZdxsvGgpo: net player"))
            if auto:
                ok(f"p{i} ping test 3 peers", any(re.search(r"rtt [0-9].*rtt [0-9].*rtt [0-9]", l) for l in ping))
        else:
            ok(f"p{i} connection cut (unanswered ping test), no ggpo session",
               c(r"ZdxsvGgpo: lobby battle connection cut: [0-9] of 3 peers answered the ping test") and not c(r"ZdxsvGgpo: net player"))
            ok(f"p{i} game gave up the battle connection (cut ended)", c(r"ZdxsvGgpo: lobby battle connection cut ended"))
            ok(f"p{i} cut OSD (opponent did not answer)",
               c(r"OSD \[ZdxsvBattleCut\]: No battle: an opponent did not answer|OSD \[ZdxsvBattleCut\]: 対戦できません: 対戦相手から応答がありません"))
            if i == cut:
                ok(f"p{i} ({cutk}) no peer answered, {dl}",
                   any(not re.search(r"rtt [0-9][0-9]* ms", l) and re.search(dq, l) for l in ping))
            else:
                ok(f"p{i} 2 peers answered, p{cut} rtt -1, {dl}",
                   any(re.search(r"rtt [0-9].*rtt [0-9]", l) and "rtt -1" in l and re.search(dq, l) for l in ping))
        if auto and not cut and not blk:
            ok(f"p{i} lobby delay {e}", c(rf"ZdxsvGgpo: lobby delay {e}:"))
        if p2p == "v6":
            ok(f"p{i} peers at IPv6 x3", c(r"ZdxsvGgpo: lobby peer position [0-9] at \[") == 3)
        if p2p == "dual" and auto:
            ok(f"p{i} IPv6 candidates answered x3", sum(len(re.findall(r"\]:[0-9]* rtt [0-9]", l)) for l in ping) == 3)
        if p2p != "v4":
            ok(f"p{i} own IPv6", c(r"zdxsv: udp: port.*IPv6 \["))
        if relay and not cut:
            for a in rl.split(","):
                ok(f"p{i} relay server {a} registered", c(r"ZdxsvGgpo: relay server " + re.escape(a)))
            if alt:
                ok(f"p{i} relay answers from 127.0.0.1", any("pongs) answers from 127.0.0.1:8203" in l for l in ping))
            ok(f"p{i} paths relay {ri} x3", c(rf"ZdxsvGgpo: lobby path to position [0-9]: relay {ri}") == 3)
            ok(f"p{i} peers via relay x3", c(r"ZdxsvGgpo: lobby peer position [0-9] at " + re.escape(ra) + r" \(relay\)") == 3)
        elif blk:
            ok(f"p{i} paths direct x2, peer x1", c(r"ZdxsvGgpo: lobby path to position [0-9]: direct") == 2
               and c(r"ZdxsvGgpo: lobby path to position [0-9]: peer [0-9]") == 1)
            ok(f"p{i} peer via relay x1", c(r"ZdxsvGgpo: lobby peer position [0-9] at .* \(relay\)") == 1)
            rr = [m.group(1) for l in ls for m in re.finditer(r"lobby path to position [0-9]: peer [0-9], rtt ([0-9]*)", l)]
            r = " ".join(rr)
            ok(f"p{i} lobby delay from the peer path (rtt {r})", c(rf"ZdxsvGgpo: lobby delay [0-9]*: slowest peer rtt {re.escape(r) or 'x'} ms"))
        elif not cut and auto:
            ok(f"p{i} paths direct x3", c(r"ZdxsvGgpo: lobby path to position [0-9]: direct") == 3)
    every = [logs[i] for i in "1234"]
    # no UDP bridge; STUN only on lobby=1 clients
    ok("no bridge", not any(re.search(r"zdxsv: bridge|UDP bridge", l) for ls in every for l in ls))
    ok(f"stun x{n}", sum(1 for ls in every if any("zdxsv: udp: port" in l for l in ls)) == n)
    if relay and not cut:
        try:
            rlog = zrig.log_lines(os.path.join(os.environ["OUT"], "relay.log"), "relay test session")
        except OSError:
            rlog = []
        ok("relay forwarded", any(re.search(r"forwarded [1-9]", l) for l in rlog))
    if n < 4:
        ok("no ggpo session", not any("ZdxsvGgpo: net player" in l for ls in every for l in ls))
    for i in gc:
        ls = logs[i]
        if not any("ZdxsvGgpo: net player" in l for l in ls):
            continue
        rep = [l for l in ls if "ZdxsvGgpo: peer report:" in l
               and re.search(r"timesync=[0-9]* input_blocks=[0-9]* fps_history=[0-9]", l)]
        ok(f"p{i} peer report: timesync, input blocks, fps history, loss x{n - 1}",
           sum(len(re.findall(r"loss_[0-9]=[0-9]*/[0-9]*", l)) for l in rep) == n - 1)
    return RC[0]


if __name__ == "__main__":
    sys.exit(main())
