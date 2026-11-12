"""M4 real-zproxy case: real stack (stack.py: battle + lobby + login + dnas), 4 pcsx2 clients from a
fresh boot (top + login + entry, no lobby states), all 4 act as real PS2s
(ZDXSV_PLATFORM_INFO=0: no platform info). The lobby splits lobbies by platform:
a console never meets an emulator, so all 4 must be consoles (else e.g.
p2 alone in the console ジャブロー2). Clients in ZP run behind their own zproxy (port 8250+i):
the lobby hands the PS2 zproxy's address in 0x6916, zproxy relays over UDP to the battle server
and pings the other zproxies (P2P); the rest dial the battle server over TCP.
Exit 0 = all checks passed.
  OUT=<dir> python tests/zdxsv/m4z.py
Env: ZP (clients behind zproxy, default "1 3": one per side), ZPROXY (exe), SHOW ("1 2":
windowed with GS snapshots), MAXS (s after mash to wait for results).
GAUGE, COUNT, TIME: the lobby battle rule (戦力ゲージ, 連続対戦数, 作戦時間 s; menu values) as the stack's
ZDXSV_LOBBY_BATTLE_*, or (server with m_rule) as an m_rule row for lobby 2 in OUT/zdxsv.db, printed. Default GAUGE=1 COUNT=1 (one round, the first shoot-down ends it); `GAUGE=0 COUNT=0`:
the lobby's default rule (rematches picked by the mash). Lobby 2's start follows CLIENTS: 4 clients 2v2,
2 1v1, 1 solo (one entry starts the battle). The game reports battle counts only from a 4-player battle (a 1v1 result: the code, all
counts 0): a 4-client run checks 4 results
with the battle_record's code per battle, a 2-client run checks each client's post-battle login instead.
EMU=1: the 4 clients are emulators instead (platform info, no zproxy by default; no DEV9 refuses the battle server: battles need GGPO;
  ZDXSV_TLS=0 unless ZDXSV_TLS_TEST_PIN=1);
  GGPO=7101: with ZDXSV_GGPO net=1,lobby=1,port=GGPO+i-1 on GGPO_CLIENTS (default "1 2 3 4"; a lobby with
  the GGPO battle info, ZBIN): the battle runs over GGPO (checks below).
  BATTLES=N (CLIENTS="1 3", 1v1): N battles in the same processes (EXIT from 作戦後部屋, Lobby 02 again);
  the result and GGPO checks then expect N battle codes and N GGPO battle ends per client.
  GOPT=opts (GGPO): appended to every ZDXSV_GGPO (e.g. GOPT=replay=0: saving off; live streams anyway).
  UPLOAD=1 (GGPO): replay upload. Clients post (ZDXSV_GGPO upload=) to the lobby's public api on
  127.0.0.1:9881 (/lbs/replay_upload, db migrated first), stored in OUT/upload (ZDXSV_LOBBY_REPLAY_DIR, served
  at /lbs/replays/). Checks per battle code: the stored .pb = one client's saved one,
  one client's upload ok + the rest 409, every db record's replay_url = the stored file's url.
  REPLAY_API=N (UPLOAD=1, a ZBIN with the lobby's /lbs/replay): after the upload checks, instance N (not a
  client) plays ZDXSV_REPLAY=http://127.0.0.1:9881/lbs/replay?battle_code=<first code> (the lobby's public API
  -> replay_url -> the stored .pb). Checks: the replay_url it took = the stored url, played to its end; with
  PWTRACE=1 its player work + rng = the players' (pwcheck, OUT/trace-api.txt).
  LIVE=N (GGPO): once the lobby logs `live upload`, instance N (not a client) plays
  ZDXSV_REPLAY=live:http://127.0.0.1:9881 (the lobby API, the newest live battle) with PW hashes
  (OUT/trace-live.txt). Checks: one client was the uplink, the spectator's stream closed at the uplink's saved
  .pb frame count, the lobby logged the close with that count; with PWTRACE=1 its player work + rng = the
  players' (pwcheck). It gets CENV and CENV<N>, as the clients (their ZDXSV_EE_CLAMP: else it diverges).
  LIVE_AT=S (LIVE): the spectator starts S s after the uplink, a late join. Checks (one battle): the round
  results the uplink sent = the ones the spectator got from the stream, none differs from its own playback;
  with LIVE_AT the spectator had some at its open.
  LIVE_NEXT=K (with BATTLES=K+1): the spectator moves on to the next battle K times (ZDXSV_LIVE_NEXT); the
  checks above run per watched battle, plus K+1 different battles watched (pwcheck too: --battle <code>).
  LIVE_SPEED=X: the spectator runs at normal speed X (launch.py -Speed), off the stream's rate: a pacing
  test (the pace lines' gap and waits, with and without ZDXSV_LIVE_PACING=0).
  LIVE_STALL=S (LIVE): S s after the spectator starts, the lobby process is suspended (the stream stalls at its
  newest frame), 3 s later the spectator's windows get a close (WM_CLOSE; macOS: SIGTERM), then the lobby
  resumes. Check, instead of the LIVE ones: the spectator exits within 5 s (a CPU thread stuck waiting for live
  data exits after ~30 s).
  GGPO_DEFAULT=K: client K gets no ZDXSV_GGPO (launch.py ZDXSV_GGPO=default), so GGPO is on by default
  with the default options (port: the ZdxsvGgpoPort setting); check: its log names the default options.
  BAD_SESSION=K (GDELAY=auto): client K gets badsession=1: every ping test fails,
  every client cuts the battle connection; no mash; waits for each cut to end + 2 lobby tcp conns per client;
  checks: cut + cut ended per client, no GGPO session, no result with frames.
  GGPO_CLIENTS without a client of CLIENTS (GGPO): that client has no GGPO port (ZDXSV_GGPO=0), so every
  GGPO client cuts (`a peer has no GGPO address`); the same wait and checks as BAD_SESSION.
  SYNC_SALT=K (GGPO): client K gets ZDXSV_SYNC_SALT=m4z (another sync fingerprint, as another build):
  the lobby gets two fingerprints, every client enters the battle, no battle starts (the lobby matches equal ones only).
  DROP_SYNC=K (GGPO): client K gets ZDXSV_DROP_SYNC=0 (position 0's sync missing from its battle info): only
  client K cuts, the others start GGPO; checks: K refuses their GGPO session, each other client closes on
  `another session` within 3 s of `net armed` (not the 10 s start timeout), every client back in the lobby;
  REPORT=1: K reports `cut`, the others `ggpo` closed on `session mismatch`.
  BREAK=K (GGPO): client K shuts down at GGPO frame BREAK_FRAME (default 1500) of the battle (BREAK_MODE=mute:
  its GGPO packets are dropped from then on, its battle TCP stays up); waits for the other clients to log in to
  the lobby again. Checks per other client: peer disconnected; prints the s from the disconnect to its next
  TCP connect.
  Both: CUTMAX=V fails a cut that took more than V vsyncs to end.
  REPORT=1: the lobby logged one 0x9952 match report per
  client for the results' battle code: result=ggpo + close_reason=net battle end + 0 mismatches (cut: result=cut).
  PAUSE=S (SHOW: the clients to pause): after the logins every SHOW client is paused (TogglePause = Space)
  for S s, then resumed. Checks: the lobby closed no connection for silence (Recv Timeout), each paused
  client answered >= S/10-2 line checks. PAUSE_ONLY=1: exit after these checks. Control:
  CENV=ZDXSV_PAUSE_LINE_CHECK=0 (S > 120: Recv Timeout).
  OSD=1: each GGPO client's last `osd frame` log line (-> osd-pN.txt)
  has its delay and every opponent's user id, name (battle info name_), pilot name (pilot_) and ping;
  names, pilot names = zdxsv.db (osdname.py).
  NAT=open|cone|symmetric|unknown: each GGPO client ran the
  connectivity test once (lobby reconnects too) with that result.
  HTTPS=1 (GGPO): each client measured the HTTPS latency once (online: the cloud regions) and a later lobby
  connection's platform info carried asia-northeast1=<ms> (lobby.log). HTTPS_OFF=K: client K runs with
  ZDXSV_HTTPS_LATENCY=0 (control): no region keys in its platform info.
  EXT_OFF=1: every client with ZDXSV_EXTENSIONS=0; checks per client: extensions off, no platform info, no GGPO
  options (replace the platform info checks); the battle runs through the battle server. PAR defaults to 0 (fast boot is an extension).
  RELAY=1: lobby relay on :8203; each GGPO client registered it and every path
  is RELAY_PATH (default direct: a LAN peer is never 16 ms slower than the relay).
  GGPO_LAT=D (CLIENTS="1 3", GGPO, GDELAY=auto; pcsx2 ZDXSV_GGPO advertise=): a udprelay.py (D ms one way) on
  127.0.0.1:7300/7301 between p1 and p3's GGPO ports; each announces only its relay socket, so the direct
  path is ~2D ms rtt. Checks: lobby delay = max(GMIN or 2, ceil(2D/32)), or GMIN/2 when RELAY_PATH is a relay.
  GGPO_JITTER=ms, GGPO_LOSS=0..1 (with GGPO_LAT): udprelay.py --jitter/--loss on that path.
  With RELAY=1 RELAY_PATH="relay 0" the GGPO traffic skips udprelay.py (its count = ping test only).
  PAR (default 1): all clients boot at once with fast boot, each waits for `fast boot: login
  browser` in its emulog (PAR_WAIT s, default 120), then `login` on all at once. PAR=0: `top` + `login` per client.
  EMU=1 (no DEV9: no DNAS): PAR=1 only; fast login with a token for the card's account (added to OUT/zdxsv.db),
  fast boot stops at `fast boot: user_id selection`, then `idlogin`.
db: copied from RUN/zdxsv.db (the cards' accounts) into OUT; stack logs land in OUT.
Cards: each client starts from RUN/Mcd001-<CARDS[i]>.ps2 (CARDS, required: one registered card per client);
the live card is put back after (m4.py's lobby states use it). A post-battle prompt creates system
data on the card, after which `top` lands in an offline battle.
"""
import atexit
import filecmp
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import threading
import time

import freshboot
import tokenlogin
import launch
import replay_check
import zbincheck
import zrig
from freshboot import drive, entry2p_retry, speed_check

RC = [0]
E = zrig.env


def ok(name, cond):
    print(f"{'PASS' if cond else 'FAIL'} {name}", flush=True)
    if not cond:
        RC[0] = 1


def say(s):
    print(s, flush=True)


def rd(p):
    try:
        with open(p, "rb") as f:
            return f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return []


def g(p, rx):
    return [l for l in rd(p) if re.search(rx, l)]


def c(p, rx):
    return len(g(p, rx))


def reports(p):
    """The match reports (0x9952 JSON) the lobby log p carries: its `p2p matching report` lines' "report" field."""
    out = []
    for l in g(p, r"p2p matching report\s+\{"):
        try:
            r = json.loads(l[l.index("{"):]).get("report")
        except ValueError:
            continue
        if isinstance(r, dict):
            out.append(r)
    return out


def ts(l):
    try:
        return float(l[1:l.index("]")])
    except ValueError:
        return 0.0


def pwcheck_ok(path, n):
    cm, s = 0.0, 0.0

    def num(x):
        m = re.match(r"[-+]?[0-9]*\.?[0-9]*", x)
        try:
            return float(m[0])
        except (ValueError, TypeError):
            return 0.0

    for l in rd(path):
        f = l.split()
        if not f:
            continue
        if f[0] == "common" and len(f) > 2:
            cm = num(f[2])
        if f[0] == "player" and len(f) > 3 and num(f[1]) < n:
            s += num(f[3])
        if f[0] == "rng:" and len(f) > 2:
            s += num(f[2])
    return cm > 0 and s == 0


def lobby_rule(path, start, gauge, count, time_s):
    """Writes lobby 2's rule (both platforms) and returns it as read back, None if the server has no m_rule
    (it then plays its built-in lobby 2 with ZDXSV_LOBBY_BATTLE_*). 0 = the menu default."""
    db = sqlite3.connect(path)
    try:
        if not db.execute("select 1 from sqlite_master where name='m_rule'").fetchone():
            return None
        db.execute("insert or replace into m_rule (id, battle_count, visible_team, battle_time, battle_gauge,"
                   " reload_type, damage_level, awake_type, awake_inc_ratio, awake_dec_ratio)"
                   " values ('m4z', ?, 0, ?, ?, 0, 3, 0, 100, 100)", (count, time_s or 180, gauge or 600))
        for platform in ("emu", "console"):
            db.execute("insert or replace into m_lobby_setting (platform, no, start, rule_id) values (?, 2, ?, 'm4z')",
                       (platform, start))
        db.commit()
        return db.execute("select s.platform, s.start, r.battle_gauge, r.battle_count, r.battle_time from m_lobby_setting s"
                          " join m_rule r on r.id = s.rule_id where s.no = 2 order by s.platform").fetchall()
    finally:
        db.close()


def battle_results(lobby_log):
    """(user_id, result dict) per battle result the lobby logged: `battle result` with the result as JSON, or the
    `== BattleResult ==` block of older servers."""
    out, blk = [], None
    for l in rd(lobby_log):
        m = re.search(r"\tbattle result\t(\{.*\})\s*$", l)
        if m:
            try:
                f = json.loads(m[1])
                out.append((f.get("user_id", ""), json.loads(f.get("result", "{}"))))
            except ValueError:
                pass
            continue
        if "== BattleResult ==" in l:
            blk = ["", {}]
            continue
        if blk is None:
            continue
        if "==================" in l:
            out.append(tuple(blk))
            blk = None
        elif " ID: " in l:
            blk[0] = l.split()[-1]
        else:
            blk[1].update(re.findall(r'"(\w+)": "?([^",]*)"?', l))
    return out


def ggpo_ends(logs, nb):
    """The end msg frame is each position's own (round-end steps run per point of view), so peers may
    differ; every recording must run past every peer's end msg for any upload to play every view."""
    ends = []
    for i, f in logs:
        ls = rd(f)
        em = [int(m[1]) for l in ls for m in re.finditer(r"net end msg of frame ([0-9]+) confirmed", l)]
        bf = [(int(m[1]), int(m[2])) for l in ls
              for m in re.finditer(r"net battle end frames ([0-9]+) rollback frames [0-9]+ loads [0-9]+ mismatches ([0-9]+)", l)]
        for k in range(max(len(em), len(bf))):
            ends.append((i, k + 1, em[k] if k < len(em) else None, *(bf[k] if k < len(bf) else (None, None))))
    good = len(ends) == len(logs) * nb and all(None not in e and e[4] == 0 for e in ends)
    for k in range(1, nb + 1):
        b = [e for e in ends if e[1] == k]
        good = good and bool(b) and max(e[2] for e in b) < min(e[3] for e in b)
    return ends, good


def main():
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "CARDS", "USERS", "RUN")
    par = E("PAR", "0" if E("EXT_OFF") else "1")
    out = os.path.abspath(os.environ.get("OUT") or sys.exit("OUT=dir for logs"))
    os.environ["OUT"] = out
    run = os.path.abspath(os.environ["RUN"])
    ip = os.environ["IP"]
    emu, ext_off, ggpo = E("EMU"), E("EXT_OFF"), E("GGPO")
    # no DEV9 connects only to the lobby: these clients log in by fast login with a token for the card's account
    fl = bool(emu) and not ext_off
    if fl and par != "1":
        print("FAIL EMU=1 logs in by fast login: PAR=1 only", flush=True)
        return 1
    zp = (E("ZP") if emu else E("ZP", "1 3")).split()
    zproxy = E("ZPROXY") or os.path.join(os.environ["ZDXSV_DIR"], "bin", "zproxy" + zrig.EXE)
    cards, users = os.environ["CARDS"].split(), os.environ["USERS"].split()
    clients = E("CLIENTS", "1 2 3 4").split()
    nc, nz = len(clients), len(zp)
    pair = nc <= 2
    gclients = (E("GGPO_CLIENTS") or " ".join(clients)).split()
    ngc = len(gclients)
    show = E("SHOW").split()
    gdelay = E("GDELAY", "1")
    zbin = E("ZBIN") or os.path.join(os.environ["ZDXSV_DIR"], "bin", "zdxsv" + zrig.EXE)
    lobby, battle = os.path.join(out, "lobby.log"), os.path.join(out, "battle.log")

    def el(i):
        return os.path.join(run, f"p{i}", "PCSX2", "logs", "emulog.txt")

    def eth():
        return bool(ext_off) or not emu

    def cenv(i):
        e = {}
        if par == "1":
            e.update(ZDXSV_FAST_BOOT="1", ZDXSV_FAST_LOGIN="1" if fl else "0")
            if fl:
                e["ZDXSV_FAST_BOOT_LOBBY"] = "0"
        if ext_off:
            e["ZDXSV_EXTENSIONS"] = "0"
            return e
        if not emu:
            e.update(ZDXSV_PLATFORM_INFO="0", DEV9="1", ZDXSV_TLS="0")
            e.update(freshboot.assigns(E("CENV")))
            e.update(freshboot.assigns(E(f"CENV{i}")))
            return e
        # PWTRACE=1: player-work hashes + net trace per client (OUT/trace-p<i>.txt; rplay.py compares a replay to them)
        if E("PWTRACE"):
            e.update(ZDXSV_PW_HASH="1", ZDXSV_NET_TRACE=os.path.join(out, f"trace-p{i}.txt"))
        if E("PWDUMP"):
            e["ZDXSV_PW_DUMP"] = os.path.join(out, f"pw-p{i}.bin")
        if i == E("HTTPS_OFF"):
            e["ZDXSV_HTTPS_LATENCY"] = "0"
        # the stack has TLS ports only for the TLS test (test pin, test certs)
        if E("ZDXSV_TLS_TEST_PIN") != "1":
            e["ZDXSV_TLS"] = "0"
        # CENV="K=V ...": extra env for every emulator client (e.g. a battle timer ZDXSV_EE_CLAMP for a time-up)
        e.update(freshboot.assigns(E("CENV")))
        e.update(freshboot.assigns(E(f"CENV{i}")))
        if i == E("SYNC_SALT"):
            e["ZDXSV_SYNC_SALT"] = "m4z"
        if i == E("DROP_SYNC"):
            e["ZDXSV_DROP_SYNC"] = "0"
        if i == E("BREAK"):
            e["ZDXSV_VM_TEST"] = f"{E('BREAK_FRAME', '1500')}:{E('BREAK_MODE', 'shutdown')}"
        if ggpo and i == E("GGPO_DEFAULT"):
            e["ZDXSV_GGPO"] = "default"
            return e
        if ggpo and i in gclients:
            # GDELAY=auto: no delay= (rtt pick, floor GMIN)
            v = f"net=1,lobby=1,port={int(ggpo) + int(i) - 1}" + ("" if gdelay == "auto" else f",delay={gdelay}")
            v += f",mindelay={E('GMIN')}" if E("GMIN") else ""
            v += ",upload=http://127.0.0.1:9881/lbs/replay_upload" if E("UPLOAD") else ""
            v += ",live=http://127.0.0.1:9881" if E("LIVE") else ""
            v += f",{E('GOPT')}" if E("GOPT") else ""
            v += ",badsession=1" if i == E("BAD_SESSION") else ""
            v += f",advertise={7300 + (i == '1')}" if E("GGPO_LAT") else ""
            e["ZDXSV_GGPO"] = v
        return e

    os.makedirs(out, exist_ok=True)
    shutil.copyfile(os.path.join(run, "zdxsv.db"), os.path.join(out, "zdxsv.db"))
    with sqlite3.connect(os.path.join(out, "zdxsv.db")) as db:
        old_codes = set(r[0] for r in db.execute("select battle_code from battle_record"))
    tokens = {}
    for i in clients if fl else ():
        tokens[i], err = tokenlogin.card_token(os.path.join(out, "zdxsv.db"),
                                               os.path.join(run, f"Mcd001-{cards[int(i) - 1]}.ps2"))
        if err:
            print(f"FAIL token p{i}: {err}", flush=True)
            return 1
    procs = []

    # zdxsv*: a ZBIN like zdxsv-ggpo.exe outlives a kill of its parent and holds the ports
    # (the next run's clients would log in to the old lobby)
    def killall():
        zrig.kill_name("pcsx2", "zdxsv", "dnas", "zproxy")
        zrig.kill_cmd(r"drive[.]py")

    killall()

    def cleanup():
        for p in procs:
            p.kill()
        killall()
        for i in "1234":
            try:
                shutil.copyfile(el(i), os.path.join(out, f"emulog-p{i}.txt"))
            except OSError:
                pass
            lc = os.path.join(out, f"live-p{i}.ps2")
            if os.path.isfile(lc):
                shutil.copy2(lc, os.path.join(run, f"p{i}", "PCSX2", "memcards", "Mcd001.ps2"))

    atexit.register(cleanup)

    def start(args, log, env=None, mode="wb"):
        with open(os.path.join(out, log), mode) as f:
            procs.append(subprocess.Popen(args, env=env, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT))

    def round_data_ok(code, n):
        rows = [r.get("round_data") for r in reports(lobby) if r.get("battle_code") == code]
        return (len(rows) == n and all(rows) and all(r == rows[0] for r in rows) and
                all(d.get("win_team") in (-1, 1, 2) and len(d.get("used_ms") or []) == n for d in rows[0]))

    def used_ms_ok(code, pb):
        try:
            rounds = replay_check.load(pb)[0]["used_ms"]
            db = sqlite3.connect(os.path.join(out, "zdxsv.db"))
            rows = db.execute("select pos, used_ms_list from battle_record where battle_code=?", (code,)).fetchall()
            db.close()
        except (OSError, sqlite3.Error):
            return False
        say(f"{code}: .pb used_ms {rounds}, db (pos, used_ms_list) {rows}")
        return bool(rounds) and bool(rows) and all(
            lst == ",".join(str(r[p - 1] if 0 < p <= len(r) else 0) for r in rounds) for p, lst in rows)

    def lobby_suspend(f):
        for p in zrig.pids_name("zdxsv"):
            f(p)

    def live_stall(pid, log):
        time.sleep(float(E("LIVE_STALL")))
        lobby_suspend(zrig.stop)
        time.sleep(3)
        t = time.time()
        # taskkill closes only the first top-level window in z-order: in a spectator that can be a hidden
        # Qt _q_titlebar window, which drops it; a WM_CLOSE to every window reaches the display surface
        if zrig.WIN:
            subprocess.call(["powershell", "-NoProfile", "-Command",
                             "Add-Type 'using System; using System.Runtime.InteropServices; public static class Wc { "
                             "public delegate bool EnumProc(IntPtr h, IntPtr l); [DllImport(\"user32.dll\")] public static "
                             "extern bool EnumWindows(EnumProc f, IntPtr l); [DllImport(\"user32.dll\")] public static extern "
                             "uint GetWindowThreadProcessId(IntPtr h, out uint p); [DllImport(\"user32.dll\")] public static "
                             "extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l); public static void Close(uint pid) "
                             "{ EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) "
                             "PostMessage(h, 0x10, IntPtr.Zero, IntPtr.Zero); return true; }, IntPtr.Zero); } }'; "
                             f"[Wc]::Close({pid})"])
        else:
            subprocess.call(["kill", "-TERM", str(pid)])
        zrig.waitgone(pid, 60)
        log(f"stall: exit after {int(time.time() - t)} s")
        lobby_suspend(zrig.cont)

    def live_thread():
        with open(os.path.join(out, "live-launch.txt"), "w", encoding="utf-8") as f:
            def log(s):
                f.write(s + "\n")
                f.flush()

            t1 = time.time()
            for _ in range(1800):
                if c(lobby, "live upload"):
                    break
                time.sleep(2)
            if not c(lobby, "live upload"):
                log(f"no live uplink in {int(time.time() - t1)} s")
                return
            n = E("LIVE")
            log(f"live uplink after {int(time.time() - t1)} s: spectator p{n}")
            if E("LIVE_AT"):
                time.sleep(float(E("LIVE_AT")))
            env = {**freshboot.assigns(E("CENV")), **freshboot.assigns(E(f"CENV{n}"))}
            env.update(ZDXSV_REPLAY="live:http://127.0.0.1:9881", ZDXSV_LIVE_NEXT=E("LIVE_NEXT", "0"), ZDXSV_REPLAY_EXIT="1",
                       ZDXSV_PW_HASH="1", ZDXSV_NET_TRACE=os.path.join(out, "trace-live.txt"))
            pid = launch.launch(ns=(int(n),), headless=True, speed=E("LIVE_SPEED", "1"), extra_env=env, quiet=True,
                                state=E("LIVE_STATE") or os.path.join(E("RBKSTATES"), "rbk-p1.p2s"))[int(n)]
            log(f"p{n} pid {pid} slot {28010 + int(n)}")
            if E("LIVE_STALL"):
                live_stall(pid, log)

    # stack.py runs in the background: its binary check fails here, before any client
    if zbincheck.check(zbin):
        return 1
    for k, d in (("GAUGE", "1"), ("COUNT", "1"), ("TIME", "0")):
        os.environ["ZDXSV_LOBBY_BATTLE_" + k] = E(k, d)
    with open(os.path.join(out, "migratedb.log"), "wb") as f:
        subprocess.call([zbin, "migratedb"], env=dict(os.environ, ZDXSV_DB_NAME=os.path.join(out, "zdxsv.db")),
                        stdout=f, stderr=subprocess.STDOUT)
    # the game reports battle counts only from a 4-player battle: 4 clients play 2v2, 2 play 1v1, 1 solo
    if nc == 3:
        say("FAIL CLIENTS: 4 (2v2), 2 (1v1) or 1 (solo)")
        return 1
    lobby_start = "2v2" if nc == 4 else "solo" if nc == 1 else "1v1"
    rule = lobby_rule(os.path.join(out, "zdxsv.db"), lobby_start, int(E("GAUGE", "1")), int(E("COUNT", "1")), int(E("TIME", "0")))
    if rule is None:
        say(f"lobby 2 rule: the server's built-in (no m_rule), ZDXSV_LOBBY_BATTLE_* GAUGE={E('GAUGE', '1')}"
            f" COUNT={E('COUNT', '1')} TIME={E('TIME', '0')}; {nc} clients need {lobby_start}")
    else:
        for p, s, gg, cn, tm in rule:
            say(f"lobby 2 rule ({p}): start {s}, gauge {gg}, count {cn}, time {tm} s")
        if any(r[1] != lobby_start for r in rule):
            say(f"FAIL lobby 2 rule: start is not {lobby_start}")
            return 1
    if E("RELAY"):
        os.environ["ZDXSV_LOBBY_RELAY_ADDR"] = ":8203"
    relay_path = E("RELAY_PATH", "direct")
    if E("LIVE"):
        os.environ.setdefault("ZDXSV_LOBBY_API_ADDR", "127.0.0.1:9881")
    if E("UPLOAD"):
        if not ggpo:
            say("FAIL UPLOAD needs GGPO")
            return 1
        os.environ.update(ZDXSV_LOBBY_API_ADDR="127.0.0.1:9881", ZDXSV_LOBBY_REPLAY_DIR=os.path.join(out, "upload"))
    # -v=2: every lobby frame (entry check below, lobby_trace.py)
    start([zrig.PY, "-u", os.path.join(zrig.HERE, "stack.py"), ip], "stack.out",
          env=dict(os.environ, LOBBY_ARGS="-v=2", RUN=out))
    lat = E("GGPO_LAT")
    if lat:
        if not (clients == ["1", "3"] and ggpo and gdelay == "auto"):
            say('FAIL GGPO_LAT needs CLIENTS="1 3" GGPO GDELAY=auto')
            return 1
        # p1 sends to 7300 (p3's announced port) -> from 7301 to p3; p3 to 7301 (p1's) -> from 7300 to p1
        start([zrig.PY, "-u", os.path.join(zrig.TOOLS, "udprelay.py"), "--a", "7300", "--b", "7301", "--p1", ggpo,
               "--p2", str(int(ggpo) + 2), "--delay", lat, "--jitter", E("GGPO_JITTER", "0"), "--loss", E("GGPO_LOSS", "0"),
               "--seconds", "3600", "--idle", "900", "--every", "10"],
              "udprelay.txt")
    time.sleep(6)
    # lobby API on 127.0.0.1 = LAN test: the lobby matches the proxy by -userid, not by address
    api = E("ZDXSV_LOBBY_API_ADDR") or "127.0.0.1:9881"
    for i in zp:
        start([zproxy, "-updatecheck=false", "-upnp=false", "-verbose", f"-userid={users[int(i) - 1]}",
               f"-tcpport={8250 + int(i)}", f"-udpport={8250 + int(i)}", f"-lobbyapi=http://{api}",
               "-stunaddr=127.0.0.1:8201"],
              f"zproxy-p{i}.log")
    for i in "1234":
        try:
            os.remove(el(i))
        except OSError:
            pass
    if E("LIVE"):
        # the spectator joins once a battle streams (no battle code = the newest live battle)
        threading.Thread(target=live_thread, daemon=True).start()

    def logins():
        return len(battle_results(lobby))

    cpid = {}
    for i in clients:
        mc = os.path.join(run, f"p{i}", "PCSX2", "memcards", "Mcd001.ps2")
        if os.path.isfile(mc):
            shutil.copy2(mc, os.path.join(out, f"live-p{i}.ps2"))
        cpid[i] = launch.launch(ns=(int(i),), eth=eth(), speed=E("SPEED", "1"), headless=i not in show,
                                memcard=os.path.join(run, f"Mcd001-{cards[int(i) - 1]}.ps2"),
                                extra_env=cenv(i), token=tokens.get(i))[int(i)]
        if par == "1":
            continue
        # top starts from a fresh boot (f1500): no other client's route in between
        drive("top", i)
        drive("login", i)
        say(f"client {i}: login results {logins()}")
    if par == "1":
        t1 = time.time()
        pw = int(E("PAR_WAIT", "120"))
        stop = "user_id selection at" if fl else "login browser"
        for i in clients:
            while not c(el(i), f"fast boot: {stop}"):
                if time.time() - t1 >= pw:
                    say(f"FAIL p{i}: no {stop} in {pw} s")
                    return 1
                time.sleep(0.5)
            say(f"client {i}: {stop} after {int(time.time() - t1)} s")
        # the page is still loading when fast boot stops: an early press is dropped
        time.sleep(3)
        th = [threading.Thread(target=drive, args=("idlogin" if fl else "login", i)) for i in clients]
        for t in th:
            t.start()
        for t in th:
            t.join()
        say(f"clients {' '.join(clients)}: login results {logins()}")
    if E("PAUSE"):
        # every client in SHOW is paused together, past the lobby's 2 min receive timeout (pause hotkey: window control)
        if not show:
            say("PAUSE needs SHOW")
            return 1
        ps = int(E("PAUSE"))
        for i in show:
            zrig.ctl(cpid[i], "Space")
        time.sleep(ps)
        for i in show:
            zrig.ctl(cpid[i], "Space")
        time.sleep(12)
        ok(f"paused {ps} s: no lobby Recv Timeout", not c(lobby, "Recv Timeout"))
        for i in show:
            f = el(i)
            ok(f"p{i} paused + resumed, line checks answered paused >= {ps // 10 - 2}",
               c(f, "Pausing") and c(f, "Resuming") and c(f, r"answered the lobby line check \(paused\)") >= ps // 10 - 2)
        if E("PAUSE_ONLY"):
            return RC[0]
    for l in g(lobby, "RegisterProxyRequest")[-nz:] if nz else []:
        say(l[:160])

    # entries close together: with one client per ~3 min (entry right after its login)
    # each saw ジャブロー2 "1人". One at a time: `entry 1 2` in parallel dropped p2's Down
    # (-> オペレーションリスト). Each must reach 0x640E (side).
    def map_pressed(a0):
        for _ in range(4 * int(E("ENTRY_WAIT", "20"))):
            if c(lobby, r"C->S \[Q\] ID:0x6305") > a0:
                return True
            time.sleep(0.25)
        return False

    def wait640e(n):
        for _ in range(80):
            if c(lobby, r"C->S \[Q\] ID:0x640E") >= n:
                return True
            time.sleep(0.25)
        return c(lobby, r"C->S \[Q\] ID:0x640E") >= n

    # routes hold keys in wall ms; a faster run than SPEED is no SPEED result
    sp = {}
    threads = [threading.Thread(target=lambda i=i: sp.__setitem__(i, speed_check(int(i)))) for i in clients]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if not all(sp.values()):
        return 1
    for k, i in enumerate(clients, 1):
        if pair:
            if not entry2p_retry(i):
                say(f"FAIL client {i} entry (no Lobby 02)")
                return 1
        else:
            a0 = c(lobby, r"C->S \[Q\] ID:0x6305")
            drive("map", i)
            # the map's last C is sometimes lost (the map stays up, cursor on Lobby 06): press it once more
            if not map_pressed(a0):
                say(f"p{i}: no Lobby 06 press, C again")
                drive("mappress", i)
                if not map_pressed(a0):
                    say(f"FAIL client {i} entry (no 0x6305)")
                    return 1
            drive("lobby2p", i)
        if not wait640e(k):
            say(f"FAIL client {i} entry (no 0x640E)")
            return 1
    for _ in range(30):
        if c(battle, "join udp peer|join success") >= nc:
            break
        time.sleep(4)
    for l in g(lobby, "zproxy user"):
        say(l[:160])
    zlogs = [os.path.join(out, f"zproxy-p{i}.log") for i in "1234"]
    # zproxy must honor -upnp=false: no UDP port mapped on the router
    ok("zproxy no UPnP", not any(c(z, "Router\\(s\\) found|UPnP") for z in zlogs))
    for i in zp:
        u, zl = users[int(i) - 1], os.path.join(out, f"zproxy-p{i}.log")
        ok(f"p{i} zproxy registered + in battle", c(lobby, f'RegisterProxyRequest.*"(UserID|user_id)":"{u}"')
           and c(lobby, f'zproxy user.*"user_id": "{u}"'))
        ok(f"p{i} PS2 -> zproxy -> UDP", c(zl, "PS2との接続に成功しました") and c(zl, "UDP通信を開始します")
           and not c(zl, "対戦サーバとの接続に失敗"))
    if E("SYNC_SALT"):
        # the lobby starts a battle only among equal sync fingerprints
        fps = {m[1] for l in g(lobby, "platform emu map") for m in [re.search(r" sync:([0-9a-f]+)", l)] if m}
        say(f"sync fingerprints at the lobby: {' '.join(sorted(fps))}")
        ok(f"lobby got {nc} different sync fingerprints", len(fps) == nc)
        ok(f"every client entered the lobby battle (x{nc})", c(lobby, r"C->S \[Q\] ID:0x640E") >= nc)
        ok("no battle between different sync fingerprints", c(battle, "join udp peer|join success") == 0)
        return RC[0]
    # a GGPO lobby battle: the emulator answers the battle server's part, the game never connects to it
    ee = sum(1 for i in clients if c(el(i), "ZdxsvGgpo: battle server answered on the EE"))
    if ee:
        ok(f"battle server answered on the EE x{ee}, battle server unused", not c(battle, r"\[ROOM\]"))
    # without DEV9 a client the EE does not answer (GGPO off) cannot reach the battle server: the game gives up
    # the battle and opens its reconnect, which must end in the lobby again
    lost = [i for i in clients if ee and c(el(i), "Zdxsv: no DEV9: the net RPCs")
            and not c(el(i), "ZdxsvGgpo: battle server answered on the EE")]
    if lost:
        t0 = time.time()
        while time.time() - t0 < 180 and not all(c(el(i), "ZdxsvOnline: online") >= 2 for i in lost):
            time.sleep(5)
        for i in lost:
            ok(f"p{i} battle unreachable without DEV9, back online after the game's reconnect",
               c(el(i), "ZdxsvOnline: online") >= 2)
        return RC[0]
    ok(f"join udp peer x{nz}", c(battle, "join udp peer") == nz)
    ok(f"join tcp x{nc - nz - ee}", c(battle, "join success") == nc - nz - ee)
    every = [el(i) for i in clients]
    if ext_off:
        for i in clients:
            f = el(i)
            ok(f"p{i} extensions off, no platform info, no GGPO",
               c(f, "ZDXSV_EXTENSIONS=0, all zdxsv extensions off")
               and not c(f, "sent platform info|ZdxsvGgpo: options"))
    if emu and not ext_off:
        ok(f"platform info sent x{nc}", sum(c(f, "sent platform info") for f in every) >= nc)
    if not emu and not ext_off:
        ok(f"DEV9 path x{nc} (adapter DHCP), no platform info, TLS, bridge or no-DEV9",
           sum(1 for f in every if c(f, "DEV9: DHCP")) >= nc
           and not any(c(f, "sent platform info|Zdxsv: TLS: on|bridge|Zdxsv: no DEV9: the net RPCs") for f in every))
    if emu and E("ZDXSV_TLS_TEST_PIN") == "1":
        for i in clients:
            f = el(i)
            for t in ["8220"]:
                ok(f"p{i} TLS port {t}", c(f, f"Zdxsv: TLS: .* through TLS port {t}"))
            ok(f"p{i} no TLS failure", not c(f, r"OSD \[ZdxsvTls\]"))
    if c(battle, "join udp peer|join success") < 1 and not ee:
        say("FAIL no battle")
        return 1
    # a peer missing from the battle: 4 results never come
    if RC[0]:
        return 1
    for z in zlogs:
        for l in g(z, "P2P Mode Enabled"):
            say(l[:120])

    # the lobby asks for the battle result at every login (empty battle code before any battle); after a 1v1
    # the result has the code but all counts 0: a pair run counts the post-battle logins and takes its codes
    # from the db's battle_record
    rtxt = os.path.join(out, "results.txt")

    def db_codes():
        try:
            db = sqlite3.connect(os.path.join(out, "zdxsv.db"))
            rows = db.execute("select battle_code from battle_record group by battle_code order by min(created)").fetchall()
            db.close()
        except sqlite3.Error:
            return []
        return [r[0] for r in rows if r[0] not in old_codes]

    def results():
        keys = ("battle_count", "win_count", "lose_count", "kill_count", "death_count", "total_frame")
        with open(rtxt, "w", encoding="utf-8") as f:
            for uid, r in battle_results(lobby):
                if str(r.get("battle_code", "")).strip():
                    f.write(f"result {uid} battle_code:{r['battle_code']}" + "".join(f" {k}:{r.get(k, '')}" for k in keys) + "\n")

    def report_code():
        m = re.search(r"battle_code:([0-9]*)", "\n".join(rd(rtxt)))
        return m[1] if m else next(iter(db_codes()), "")

    def nresults():
        return c(rtxt, "^result")

    # a cut: BAD_SESSION, SYNC_SALT, or a client without GGPO (not in GGPO_CLIENTS)
    cut = ""
    if E("BAD_SESSION"):
        cut = "ping"
    if ggpo and ngc < nc:
        cut = "noport"
    if E("SYNC_SALT"):
        cut = "sync"
    if E("DROP_SYNC"):
        cut = "drop"

    # vsyncs from the cut to its end (the game closed the battle sock), "?" if a line is missing
    def cutlen(f):
        ls = rd(f)
        a = next((m[1] for l in ls for m in [re.search(r"ZdxsvGgpo: lobby battle connection cut: .*vsync ([0-9]+)\)", l)] if m), None)
        b = next((m[1] for l in ls for m in [re.search(r"connection cut ended at vsync ([0-9]+)", l)] if m), None)
        return str(int(b) - int(a)) if a and b else "?"

    if cut:
        t0 = time.time()
        while time.time() - t0 < int(E("MAXS", "300")):
            time.sleep(15)
            e = sum(1 for i in gclients if c(el(i), "ZdxsvGgpo: lobby battle connection cut ended|ZdxsvGgpo: session mismatch frames"))
            n = c(lobby, "A new tcp connection open")
            say(f"{int(time.time() - t0)} s: cut ended {e}/{ngc}, lobby tcp conns {n}/{2 * nc}")
            if e >= ngc and n >= 2 * nc:
                break
        time.sleep(20)
        results()
        for l in rd(rtxt):
            say(l)
        for f in every:
            for l in g(f, r"ZdxsvGgpo: (lobby|badsession|net player)|zdxsv: ping test"):
                say(l[:200])
        for i in gclients:
            f = el(i)
            if cut == "drop" and i != E("DROP_SYNC"):
                a, dt = 0.0, None
                for l in rd(f):
                    if "ZdxsvGgpo: net armed" in l:
                        a = ts(l)
                    if a and "ZdxsvGgpo: a peer is in another session" in l:
                        dt = ts(l) - a
                        break
                if dt is not None:
                    say(f"p{i} another session {dt:.2f} s after net armed")
                ok(f"p{i} closed on another session within 3 s of net armed", dt is not None and dt <= 3)
                ok(f"p{i} no start timeout", not c(f, "net peers not synchronized"))
                continue
            if cut == "drop":
                ok(f"p{i} connection cut, refused the peers' GGPO session",
                   c(f, "ZdxsvGgpo: lobby battle connection cut: ") and c(f, "zdxsv: cut refusal: refused the GGPO session")
                   and not c(f, "ZdxsvGgpo: net player"))
            elif cut == "ping":
                ok(f"p{i} connection cut (unanswered ping test), no ggpo session",
                   c(f, f"ZdxsvGgpo: lobby battle connection cut: [0-9] of {nc - 1} peers answered the ping test")
                   and not c(f, "ZdxsvGgpo: net player"))
            elif cut == "sync":
                ok(f"p{i} connection cut (sync fingerprints differ), no ggpo session",
                   c(f, "ZdxsvGgpo: lobby battle connection cut: settings or build differ") and not c(f, "ZdxsvGgpo: net player"))
            else:
                ok(f"p{i} connection cut (a peer has no GGPO port), no ggpo session",
                   c(f, "ZdxsvGgpo: lobby battle connection cut: a peer has no GGPO address") and not c(f, "ZdxsvGgpo: net player"))
            ok(f"p{i} game gave up the battle connection (cut ended)", c(f, "ZdxsvGgpo: lobby battle connection cut ended"))
            cl = cutlen(f)
            say(f"p{i} cut ended after {cl} vsyncs")
            if E("CUTMAX"):
                ok(f"p{i} cut ended within {E('CUTMAX')} vsyncs", cl != "?" and int(cl) <= int(E("CUTMAX")))
        if cut == "ping":
            bs = E("BAD_SESSION")
            ok(f"p{bs} used the bad session", c(el(bs), "badsession=1: ping test session"))
        ok(f"every client back in the lobby (tcp conns x{2 * nc})", c(lobby, "A new tcp connection open") >= 2 * nc)
        # a 4-player cut reports total_frame 16200 (a 1v1 cut 0): frames do not tell a cut from a battle
        ok("no battle fought (no win, loss, kill or death)", not c(rtxt, "_count:[1-9]"))
        if E("REPORT"):
            for l in g(lobby, "p2p matching report"):
                say(l[:300])
            ok("no match report without a battle code", all(r.get("battle_code") for r in reports(lobby)))
            rc = report_code()
            rs = [r for r in reports(lobby) if r.get("battle_code") == rc]
            ncut = 1 if cut == "drop" else ngc
            ok(f"match report x{ncut}: battle code {rc}, result cut, cut ended",
               sum(1 for r in rs if r.get("result") == "cut" and "cut_sends" in r) == ncut)
            if cut == "drop":
                ok(f"match report x{ngc - 1}: battle code {rc}, result ggpo, closed on session mismatch",
                   sum(1 for r in rs if r.get("result") == "ggpo" and r.get("close_reason") == "session mismatch") == ngc - 1)
        return RC[0]

    def breakwait():
        t0 = time.time()
        while time.time() - t0 < int(E("MAXS", "300")):
            time.sleep(10)
            n = c(lobby, "A new tcp connection open")
            say(f"{int(time.time() - t0)} s: lobby tcp conns {n}/{2 * nc - 1}")
            if n >= 2 * nc - 1:
                break
        for i in clients:
            if i == E("BREAK"):
                continue
            f = el(i)
            ok(f"p{i} peer disconnected", c(f, "ZdxsvGgpo: peer disconnected"))
            d = 0.0
            for l in rd(f):
                if "ZdxsvGgpo: peer disconnected" in l and not d:
                    d = ts(l)
                if d and "Creating New TCP Connection to" in l:
                    say(f"p{i} TCP connect to {l.split()[-1]} {ts(l) - d:.1f} s after the disconnect")
                    break
        ok(f"the other clients back in the lobby (tcp conns x{2 * nc - 1})", c(lobby, "A new tcp connection open") >= 2 * nc - 1)

    nb = int(E("BATTLES", "1"))
    for b in range(1, nb + 1):
        if b > 1:
            # the results come at the post-battle login, in 作戦後部屋 (待機 / EXIT): EXIT -> 戦場選択 (step 2,1,2,5,1
            # as after the login, then dlg 29, 32, 0 ~8 s later with the map nearly up), then Lobby 02 from the map
            for i in clients:
                drive("seq", i, "Down~lstate!/500,C~lstate!/500,@step=5/10000,@step=1/3000,@dlg=0/20000", f"exit{b}")
                if not entry2p_retry(i, "remap2p"):
                    say(f"FAIL client {i} entry {b} (no Lobby 02)")
                    return 1
            if not wait640e(nc * b):
                say(f"FAIL battle {b} entry (no 0x640E)")
                return 1
        drive("mash", *clients)
        t0 = time.time()
        if E("BREAK"):
            breakwait()
            return RC[0]
        last = None
        while time.time() - t0 < int(E("MAXS", "2100")):
            time.sleep(5)
            results()
            r, want, what = (logins(), nc * (b + 1), "post-battle logins") if pair else (nresults(), nc * b, "results")
            secs = int(time.time() - t0)
            if r != last or secs % 30 < 5:
                say(f"{secs} s: {what} {r - (nc if pair else 0)}/{want - (nc if pair else 0)}")
            last = r
            if r >= want:
                time.sleep(2)
                results()
                break
    for l in rd(rtxt):
        say(l)
    for l in [l for z in zlogs for l in g(z, "Error|失敗")][-4:]:
        say(l[:160])
    if pair:
        say(f"1v1 results with a battle code: {nresults()} (counts not checked: 0 in a 1v1)")
        ok(f"every client logged in after each battle (battle result answers x{nc * (nb + 1)})", logins() >= nc * (nb + 1))
        ok(f"battle_record: {nb} battle codes", len(db_codes()) == nb)
    else:
        ok(f"battle results with a battle code x{nc * nb}", nresults() >= nc * nb)
        ok("result battle codes = battle_record's",
           sorted(set(re.findall(r"battle_code:([0-9]*)", "\n".join(rd(rtxt))))) == sorted(db_codes()))
        # per battle code: nc results, one total_frame; nb codes
        n, fr, kills, deaths = {}, set(), 0, 0
        for l in rd(rtxt):
            v = dict(x.split(":", 1) for x in l.split()[2:] if ":" in x)
            cd = v.get("battle_code", "")
            n[cd] = n.get(cd, 0) + 1
            fr.add((cd, v.get("total_frame", "")))
            kills += int(v.get("kill_count") or 0)
            deaths += int(v.get("death_count") or 0)
        ok(f"results agree ({nb} battle codes)",
           len(n) == nb and len(fr) == nb and all(x >= nc for x in n.values()) and kills == deaths)
    tools = zrig.TOOLS
    if emu and ggpo:
        seen = set()
        for f in [os.path.join(out, f"emulog-p{i}.txt") for i in "1234"] + every:
            for l in g(f, r"ggpo peers|ZdxsvGgpo: (lobby|net player)"):
                seen.add(l[:160])
        for l in sorted(seen):
            say(l)
        for i in gclients:
            f = el(i)
            ok(f"p{i} ggpo session, {nc - 1} lobby peers x{nb}",
               c(f, "ZdxsvGgpo: net player") and c(f, "ZdxsvGgpo: lobby peer position") == (nc - 1) * nb)
        gd = E("GGPO_DEFAULT")
        if gd:
            f = el(gd)
            ok(f"p{gd} GGPO by default", c(f, re.escape("ZdxsvGgpo: options 'net=1,lobby=1' (serial SLPS-25419)")))
            pm = [re.search(r"[0-9]*$", l)[0] for l in g(os.path.join(run, f"p{gd}", "PCSX2", "inis", "PCSX2.ini"),
                                                         r"^ZdxsvGgpoPort = [0-9]*")]
            p = pm[0] if pm else ""
            ok(f"p{gd} GGPO port from the setting", p.isdigit() and int(p) > 0 and c(f, f"ZdxsvGgpo: net player .* port {p} "))
        ends, good = ggpo_ends([(i, el(i)) for i in gclients], nb)
        for i, k, a, f, m in ends:
            say(f"ggpo end p{i} battle {k} end_msg {a} frames {f} mismatches {m}")
        ok(f"ggpo battle ends x{nb}: every recording past every peer's end msg, 0 mismatches", good)
        if E("REPORT"):
            for l in g(lobby, "p2p matching report"):
                say(l[:300])
            ok("no match report without a battle code", all(r.get("battle_code") for r in reports(lobby)))
            rc = report_code()
            ok(f"match report x{ngc}: battle code {rc}, result ggpo, net battle end, 0 mismatches",
               sum(1 for r in reports(lobby) if r.get("battle_code") == rc and r.get("result") == "ggpo" and
                   r.get("close_reason") == "net battle end" and r.get("mismatches") == 0) == ngc)
        if E("UPLOAD"):
            time.sleep(10)  # uploads run detached after the battle end
            for f in every:
                for l in g(f, "replay upload"):
                    say(l[:200])
            codes = sorted(db_codes())
            for code in codes:
                pb = os.path.join(out, "upload", "replays", f"{code}.pb")
                # each client's file is its own (position, frame 0 state): the first upload is kept
                same = sum(1 for i in gclients if os.path.isfile(pb) and
                           os.path.isfile(os.path.join(run, f"p{i}", "PCSX2", "replays", f"{code}.pb")) and
                           filecmp.cmp(pb, os.path.join(run, f"p{i}", "PCSX2", "replays", f"{code}.pb"), shallow=False))
                ok(f"{code}: stored .pb = exactly one client's saved .pb", same == 1)
                ok(f"{code}: {ngc} match reports, same round_data, {ngc} used_ms per round", round_data_ok(code, ngc))
                ok(f"{code}: every db record's used_ms_list = the stored .pb's round_data used_ms at its pos",
                   used_ms_ok(code, pb))
                try:
                    db = sqlite3.connect(os.path.join(out, "zdxsv.db"))
                    urls = sorted(set(r[0] for r in db.execute("select replay_url from battle_record where battle_code=?", (code,))))
                    db.close()
                except sqlite3.Error:
                    urls = []
                ok(f"{code}: every db record's replay_url = stored url", urls == [f"http://127.0.0.1:9881/lbs/replays/{code}.pb"])
            up_ok = sum(c(f, "replay upload.*: ok") for f in every)
            up_there = sum(c(f, "replay upload.*: already there") for f in every)
            ok(f"uploads x{ngc * nb}: {nb} ok + {(ngc - 1) * nb} already there, no failure",
               up_ok == nb and up_there == (ngc - 1) * nb and not any(c(f, "replay upload.*failed") for f in every))
            ra = E("REPLAY_API")
            if ra:
                code, f = report_code(), el(ra)
                try:
                    os.remove(f)
                except OSError:
                    pass
                env = dict(ZDXSV_REPLAY=f"http://127.0.0.1:9881/lbs/replay?battle_code={code}", ZDXSV_REPLAY_EXIT="1",
                           ZDXSV_REPLAY_TURBO="1", ZDXSV_PW_HASH="1", ZDXSV_NET_TRACE=os.path.join(out, "trace-api.txt"))
                pid = launch.launch(ns=(int(ra),), headless=True, extra_env=env, quiet=True,
                                    state=E("LIVE_STATE") or os.path.join(E("RBKSTATES"), "rbk-p1.p2s"))[int(ra)]
                line = f"p{ra} pid {pid} slot {28010 + int(ra)}"
                say(line)
                with open(os.path.join(out, "api-launch.txt"), "w", encoding="utf-8") as fo:
                    fo.write(line + "\n")
                for _ in range(120):
                    if c(f, r"ZdxsvGgpo: replay (end|.*: not a|.*: status|.*: no )"):
                        break
                    time.sleep(5)
                # the trace is complete once the instance has exited (ZDXSV_REPLAY_EXIT)
                zrig.waitgone(pid, 60)
                for l in g(f, "ZdxsvGgpo: replay")[:3] + g(f, "ZdxsvGgpo: replay end"):
                    say(l[:200])
                ok(f"{code}: API replay_url = stored url", c(f, re.escape(f"replay_url http://127.0.0.1:9881/lbs/replays/{code}.pb")))
                ends = [m for l in rd(f) for m in re.finditer(r"ZdxsvGgpo: replay end at frame ([0-9]*) of ([0-9]*)", l)]
                ok(f"{code}: API replay played to its end, instance exited",
                   bool(ends) and ends[0][1].isdigit() and ends[0][2].isdigit() and int(ends[0][1]) + 1 == int(ends[0][2])
                   and not zrig.alive(pid))
                if E("PWTRACE"):
                    pc = os.path.join(out, "pwcheck-api.txt")
                    with open(pc, "wb") as fo:
                        subprocess.call([zrig.PY, "-I", os.path.join(tools, "pwcheck.py"), "--own", "--battle", code,
                                         os.path.join(out, "trace-api.txt")] + [os.path.join(out, f"trace-p{i}.txt") for i in clients],
                                        stdout=fo)
                    for l in g(pc, "^common|^judged|^player|^rng"):
                        say(l[:160])
                    ok(f"{code}: API replay player work + rng = players' (pwcheck)", pwcheck_ok(pc, nc))
        if E("OSD"):
            osds = []
            for i in gclients:
                f = el(i)
                m = [re.search(r"ZdxsvGgpo: osd frame .*", l)[0].replace("\r", "") for l in g(f, "ZdxsvGgpo: osd frame ")]
                o = os.path.join(out, f"osd-p{i}.txt")
                osds.append(o)
                with open(o, "w", encoding="utf-8") as fo:
                    fo.write((m[-1] + "\n") if m else "")
                for l in rd(o):
                    say(l[:300])
                dl = [m[1] for l in rd(f) for m in re.finditer(r"lobby delay ([0-9]*)", l)]
                d = dl[-1] if dl else gdelay
                t = "\n".join(rd(o))
                ok(f"p{i} osd: Delay {d}fr, {nc - 1} opponents with user id + name + pilot name + ping",
                   f"Delay {d}fr |" in t and
                   len(re.findall(r"\|[1-4]P [A-Z0-9]{6} \| [^|]*[^ |] \| [^|]*[^ |] \| Ping [0-9]*ms", t)) == nc - 1)
            ok("osd names, pilot names = zdxsv.db (UTF-8, not garbled)",
               subprocess.call([zrig.PY, "-I", os.path.join(zrig.HERE, "osdname.py"), os.path.join(out, "zdxsv.db")] + osds) == 0)
    if E("NAT"):
        nat = E("NAT")
        for i in gclients:
            f = el(i)
            for l in g(f, "udp test: nat="):
                say(l[:200])
            ok(f"p{i} udp test: exactly one, nat={nat}", c(f, "udp test: nat=") == 1 and c(f, f"udp test: nat={nat}:"))
    if E("HTTPS"):
        for i in clients:
            f = el(i)
            for l in g(f, r"ZdxsvHttpsLatency: (asia-northeast1|done|off)"):
                say(l[:200])
            # the lobby logs each platform info as a Go map; a client's lines carry its GGPO port (ggpo:<port>)
            p = int(ggpo or 0) + int(i) - 1
            pl = g(lobby, rf"platform.*ggpo:{p}[\] ]")
            if i == E("HTTPS_OFF"):
                ok(f"p{i} (ZDXSV_HTTPS_LATENCY=0): off, no region in its platform info",
                   c(f, "ZdxsvHttpsLatency: off") and pl and not any("asia-northeast1:" in l for l in pl))
            else:
                ok(f"p{i} https latency: one run, its platform info has asia-northeast1",
                   c(f, "ZdxsvHttpsLatency: done") == 1 and any(re.search("asia-northeast1:[0-9]", l) for l in pl))
    if E("RELAY"):
        ok("lobby relay started", c(lobby, "[Ss]tart relay"))
        for i in gclients:
            f = el(i)
            for l in g(f, "lobby path to|ZdxsvGgpo: relay server"):
                say(l[:200])
            ok(f"p{i} relay server registered", c(f, re.escape(f"ZdxsvGgpo: relay server {ip}:8203")))
            ok(f"p{i} paths {relay_path}", c(f, "ZdxsvGgpo: lobby path to position [0-9]: " + re.escape(relay_path))
               and not any(f": {relay_path}" not in l for l in g(f, "ZdxsvGgpo: lobby path to")))
    if lat:
        e = (2 * int(lat) + 31) // 32
        m = int(E("GMIN", "2"))
        if E("RELAY") and relay_path != "direct":
            e = 0
        e = max(e, m)
        for i in clients:
            f = el(i)
            for l in g(f, "advertise=|ping test|lobby delay")[-4:]:
                say(l[:200])
            a = 7300 + (i == "1")
            ok(f"p{i} announced 127.0.0.1:{a} only", c(f, f"zdxsv advertise={a}:"))
            ok(f"p{i} lobby delay {e}", c(f, f"ZdxsvGgpo: lobby delay {e}:"))
        ul = g(os.path.join(out, "udprelay.txt"), r"relay (end|stats)")
        say(f"udprelay: {ul[-1] if ul else ''}")
        ok("udprelay forwarded (the ping test reached it)", bool(ul) and re.search("packets in [1-9]", ul[-1]))
    live = E("LIVE")
    llog = os.path.join(out, "live-launch.txt")
    if E("LIVE_STALL"):
        for _ in range(120):
            if c(llog, "^stall: "):
                break
            time.sleep(1)
        try:
            shutil.copyfile(el(live), os.path.join(out, "emulog-live.txt"))
        except OSError:
            pass
        for l in rd(llog):
            say(l)
        ok("live stall: the spectator exits within 5 s of its window's close", c(llog, "^stall: exit after [0-5] s"))
    elif live:
        f = el(live)
        # the spectator trails the players (buffer wait; the close goes out after the replay write)
        nw = int(E("LIVE_NEXT", "0")) + 1
        for _ in range(60 * nw):
            if c(f, "ZdxsvGgpo: live: stream closed") >= nw or c(f, "ZdxsvGgpo: live .*: |ZdxsvGgpo: replay end"):
                break
            time.sleep(5)
        time.sleep(5)
        for l in rd(llog):
            say(l)
        for l in [l for x in every for l in g(x, "live: |ZdxsvGgpo: live")][:12 * nw]:
            say(l[:200])
        for l in g(lobby, r"live (open|upload|end|drop)")[:8 * nw]:
            say(l[:200])
        # per watched battle, in order: its code and the frame its stream closed at
        ls = rd(f)
        codes = [m[1] for l in ls for m in re.finditer(r"ZdxsvGgpo: live: battle ([0-9]+)", l)]
        sfs = [m[1] for l in ls for m in re.finditer(r"ZdxsvGgpo: live: stream closed \([^)]*\) at frame ([0-9]+)", l)]
        ok(f"spectator watched {nw} different live battle(s)", len(codes) == nw and len(set(codes)) == nw)
        if nw == 1:
            ok("spectator watched the live battle", c(f, f"ZdxsvGgpo: live: battle {report_code()}"))
        for k, code in enumerate(codes):
            sf = sfs[k] if k < len(sfs) else ""
            up = [i for i in clients if c(el(i), f"live: uplink {code} to")]
            fr = ""
            if len(up) == 1:
                x = [m[1] for l in rd(el(up[0])) for m in re.finditer(rf"replays.{code}.pb frames=([0-9]*)", l)]
                fr = x[-1] if x else ""
                # saving off (GOPT=replay=0): the count the uplink says the lobby acked
                if not fr:
                    x = [m[1] for l in rd(el(up[0])) for m in re.finditer(rf"live: uplink {code} closed: ([0-9]*)", l)]
                    fr = x[-1] if x else ""
            say(f"{code}: uplink p{' '.join(up) or 'none'} saved frames {fr or 'none'}, spectator closed at {sf or 'none'}")
            ok(f"{code}: one client was the live uplink", len(up) == 1)
            ok(f"{code}: spectator stream closed at the uplink's saved frame count", bool(sf) and fr == sf)
            ok(f"{code}: lobby ended the live upload at its end", c(lobby, f"live end {code} eof "))
        sc = [m[0] for l in ls for m in [re.search(r"replay state check: hashes [0-9]* checked, [0-9]* differ", l)] if m]
        sc = sc[-1] if sc else ""
        say(f"spectator {sc or 'no state check'}")
        scf = sc.split()
        ok("spectator checked the stream's state hashes, none differ",
           len(scf) >= 7 and scf[4].isdigit() and int(scf[4]) > 0 and scf[6] == "0")
        if nw == 1:
            # the uplink's round results as sent, the spectator's as received (also ahead of its playback)
            rup = "".join(re.search(r"win_team (-*[0-9]*)", l)[1] + " " for i in clients
                          for l in g(el(i), "ZdxsvGgpo: live: round result win_team"))
            rsp = "".join(re.search(r"win_team (-*[0-9]*)", l)[1] + " " for l in g(f, "ZdxsvGgpo: live: round result [0-9]*: win_team"))
            say(f"round results: uplink [{rup}] spectator [{rsp}]")
            ok("spectator got the uplink's round results from the stream", bool(rup) and rup == rsp)
            ok("spectator's played rounds = the stream's", not c(f, "differs from the file"))
            if E("LIVE_AT"):
                ok("late spectator had round results at its open", c(f, "live: battle .*, [1-9][0-9]* bytes of round results"))
        # per watched battle: the traces' lines from its `B <code>` line (pwcheck-live-<code>.txt)
        if E("PWTRACE"):
            for code in codes:
                pc = os.path.join(out, f"pwcheck-live-{code}.txt")
                with open(pc, "wb") as fo:
                    subprocess.call([zrig.PY, "-I", os.path.join(tools, "pwcheck.py"), "--own", "--battle", code,
                                     os.path.join(out, "trace-live.txt")] + [os.path.join(out, f"trace-p{i}.txt") for i in clients],
                                    stdout=fo, stderr=subprocess.STDOUT)
                for l in g(pc, "^common|^judged|^player|^rng|no H lines"):
                    say(l[:160])
                ok(f"{code}: spectator player work + rng = players' (pwcheck)", pwcheck_ok(pc, nc))
    return RC[0]


if __name__ == "__main__":
    sys.exit(main())
