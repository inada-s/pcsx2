"""Stand-in lobby on :8200 for games resumed from lobby savestates (pcsx2 DEV9 adopts
the PS2's open lobby connection after a load, M3), and a lobby-less battle start (M4).

  fake_lobby.py [--port 8200] [--count N] [--trace LOBBY_LOG --players 4 --battle IP:PORT --rpc ADDR]

Without --trace: answers EnterLobby / GetLobbyEntryUserCount / GetPlazaJoinUser (--count
players in battle, shown as 対戦中 N人 on 戦場選択) only.
With --trace (a `zdxsv -v=2 lobby` log of a real battle, lobby_trace.py):
- Every question gets the answer the real lobby gave to the same question (same body, else
  the same command; echo if the lobby always echoed), from a recorded connection of the same
  kind ("template"); server frames that followed a client frame in the template follow it here.
- A connection that is silent for 1 s after accept is a fresh one (after a battle the game
  reconnects and logs in again): it gets the recorded key pair question first. FirstData
  picks the recorded user with the same answer.
- Battle start (the lobby's job, replaced): side entry 0x640E answered; once --players have
  entered, sessions are registered with the battle server (battlereg.exe, as startBattle),
  then BattleStart 0x6910 to all. Positions: side 1 in entry order, then side 2. The
  adopted connection at position k plays the k-th recorded user (0x6913 answers of the
  trace). 0x6915 gets a fresh battle code; 0x6916 the battle server, after the custom 0x9951
  battle info (session, users, p2p_<user>=addr from the STUN answers below, ggpo_<user> with --ggpo;
  ggpo_session + ggpo_ping_ms as zdxsv (--bad-session K: client K's differs); --no-addr K: client K has no p2p_ / ggpo_ line; --ggpo-delay D: ggpo_ = a udprelay.py per pair, D ms one way,
  so the GGPO ping test and battle see 2*D ms rtt); --ggpo-block K-L[,K-L...]: no relay for those pairs, so they reach each other only through another peer (K-1,K-2,... with every other client: K reaches nobody); --relay ADDR: relay_0 for a `zdxsv relay` there, ggpo_session from --relay-session.
- STUN on UDP --port+1 (JSON Ping -> Pong with public_addr). The emulator pings it right
  before its adopted connection sends, so the k-th unbound ping address belongs to the k-th
  adopted connection (launch / drive the games one at a time).
- Logs every frame; BattleResult answers (0x6138) are logged in full.
Frame (zdxsv pkg/lobby/message, big endian): dir u8, category u8, command u16,
body size u16, seq u16, status u32, body.
"""
import argparse
import http.server
import json
import os
import random
import socket
import sqlite3
import struct
import subprocess
import sys
import threading
import time
import urllib.parse

import lobby_trace

HDR = struct.Struct(">BBHHHI")
Q, A, N, C = 0x01, 0x02, 0x10, 0xFF


# question -> answer body, as zdxsv pkg/lobby/msg_lobby.go (empty lobby: no other users)
ANSWERS = {
    0x6305: lambda b: b"",  # EnterLobby
    0x640F: lambda b: b[:2] + struct.pack(">HH", 0, 0),  # GetLobbyEntryUserCount: id, aeug, titans
}
lock = threading.Lock()
seq_next = [1000]
logins = []  # sessions handed out by the login API, in order
key_sessions = {}  # login_key -> session handed out by the login API
log_lock = threading.Lock()
LBS_INFO = "0ad601082008,ZDX BETA SERVER"
UDPRELAY = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "zdxsv", "udprelay.py"))


def log(s):
    # one locked write per line: print() from connection threads glued lines under -u and lost a
    # 4th BATTLE RESULT (lograce.py)
    with log_lock:
        sys.stdout.write("%s %s\n" % (time.strftime("%T"), s))
        sys.stdout.flush()


def frame(cat, cmd, seq, body):
    return HDR.pack(0x18, cat, cmd, len(body), seq, 0x00FFFFFF) + body


def pstr(b, o):
    n = struct.unpack_from(">H", b, o)[0]
    return b[o + 2:o + 2 + n], o + 2 + n


def login_key(body, seq):
    """ResponseKeyPair (0x6101) body: key1, then the login key encrypted with the message seq ("" if it does not check)."""
    _, o = pstr(body, 0)
    enc, _ = pstr(body, o)
    if len(enc) <= 2:
        return ""
    chk, p, out = struct.unpack_from(">H", enc)[0], seq & 0xFF, bytearray()
    fix, masks = (21, 23, 10, 17, 23, 19, 6, 13), (0x33, 0x30, 0x3C, 0x34, 0x2D, 0x30, 0x3C, 0x34)
    for i, x in enumerate(enc[2:]):
        out.append(x ^ ((fix[i & 7] - (i & 0xF8) - p + ((p - 9 + i) & masks[i & 7]) * 2) & 0xFF))
    return out.rstrip(b"\0").decode("shift_jis", "replace") if sum(out) & 0xFFFF == chk else ""


class Trace:
    def __init__(self, path):
        conns = {}
        for f in lobby_trace.frames(path):
            conns.setdefault(f["conn"], []).append(f)
        self.conns = list(conns.values())
        has = lambda fs, cmd, cat: any(f["cmd"] == cmd and f["cat"] == cat and f["c2s"] for f in fs)
        self.lobby = [fs for fs in self.conns if has(fs, 0x640E, Q)]  # took part in the battle start
        login = [fs for fs in self.conns if has(fs, 0x6101, A)]
        self.fresh = [fs for fs in login if not has(fs, 0x640E, Q)] + [fs for fs in login if has(fs, 0x640E, Q)]  # after a battle first
        # battle users, by position: the 0x6913 answers of one battle connection
        self.users = {}
        for f in self.lobby[0]:
            if f["cmd"] == 0x6913 and f["cat"] == A:
                b = f["body"]
                uid, o = pstr(b, 2)
                name, o = pstr(b, o)
                self.users[b[0]] = dict(entry=b[1], id=uid.decode(), name=name, a6913=b)
            if f["cmd"] == 0x6917 and f["cat"] == A:
                self.users.setdefault(f["body"][0], {})["a6917"] = f["body"]
        self.rule = next(f["body"] for f in self.lobby[0] if f["cmd"] == 0x6914 and f["cat"] == A)
        # session per user: the PS2 got it at login (it is in the state), so reuse the recorded one
        for fs in self.lobby:
            for f in fs:
                if f["cmd"] == 0x9951:
                    kv = dict(l.split("=", 1) for l in f["body"].decode().splitlines() if "=" in l)
                    for u in self.users.values():
                        if u.get("id") == kv.get("user_id"):
                            u["session"] = kv["session_id"]

    @staticmethod
    def answer(fs, i, cmd, body):
        """Answer to question (cmd, body) at template index i: [(cat, cmd, body)] that followed it."""
        def after(j):
            out = []
            for f in fs[j + 1:]:
                if f["c2s"]:
                    break
                out.append(f)
            return out
        cands = [j for j, f in enumerate(fs) if f["c2s"] and f["cmd"] == cmd]
        exact = [j for j in cands if fs[j]["body"] == body]
        pick = next((j for j in exact if j >= i), exact[0] if exact else None)
        if pick is None:
            pick = next((j for j in cands if j >= i), cands[-1] if cands else None)
        if pick is None:
            return i, []
        out = after(pick)
        qa = [(fs[j]["body"], next((f["body"] for f in after(j) if f["cmd"] == cmd and f["cat"] == A), None)) for j in cands]
        echo = qa and all(q == a for q, a in qa)
        out = [dict(f, body=body) if echo and f["cmd"] == cmd and f["cat"] == A else f for f in out]
        return pick + 1, out


class Battle:
    def __init__(self, args, trace):
        self.args, self.trace = args, trace
        self.entries = []  # Conn, side
        self.stun = []  # ping source addrs in arrival order
        self.started = False
        self.code = ""

    def enter(self, conn, side):
        with lock:
            if conn in [c for c, _ in self.entries] or self.started:
                return
            self.entries.append((conn, side))
            log("entry %s side %d (%d/%d)" % (conn.name, side, len(self.entries), self.args.players))
            if len(self.entries) < self.args.players:
                return
            self.started = True
        if self.args.hold:
            threading.Thread(target=self.start_held, daemon=True).start()
        else:
            self.start()

    def start_held(self):
        # --hold FILE: all entered; the battle starts once FILE exists (rbkprep.py saves states first)
        log("all entered, holding until %s exists" % self.args.hold)
        while not os.path.exists(self.args.hold):
            time.sleep(0.5)
        self.start()

    def start(self):
        order =[c for c, s in self.entries if s == 1] + [c for c, s in self.entries if s != 1]
        sessions = []
        for pos, c in enumerate(order, 1):
            u = self.trace.users[pos]
            c.pos, c.user = pos, u["id"]
            c.session = c.session or u.get("session", "")  # the PS2 sends its login session in its battle handshake
            sessions.append("%s:%s:%s:%d" % (c.session, u["id"], "p%d" % pos, u["entry"]))
        self.order = order
        self.relays = self.start_relays(order) if self.args.ggpo_delay else {}
        self.code = "%013d" % (int(time.time() * 1000) % 10 ** 13)
        r = subprocess.run([self.args.battlereg, self.args.rpc] + sessions, capture_output=True, text=True)
        log("battlereg %s: %s%s" % (" ".join(sessions), r.stdout.strip(), r.stderr.strip()))
        for c in order:
            c.notice(0x6910, b"")

    def ggpo_port(self, c):
        # GGPO port of client c (--ggpo + k - 1 for the k-th adopted client), 0 = no ggpo_ line
        k = self.conns.index(c) + 1 if c in self.conns else 0
        return self.args.ggpo + k - 1 if self.args.ggpo and str(k) in self.args.ggpo_clients else 0

    def start_relays(self, order):
        # --ggpo-delay D: one udprelay.py per pair of GGPO clients; X is told ggpo_Y = the relay socket
        # that stands in for Y (p2p_Y IP 127.0.0.1): the ping test and the GGPO battle see 2*D ms rtt
        relays, port = {}, self.args.ggpo_relay_port
        gg = [c for c in order if c.udp and self.ggpo_port(c)]
        for i, x in enumerate(gg):
            for y in gg[i + 1:]:
                relays[(x, y)], relays[(y, x)] = port, port + 1
                if {self.conns.index(x) + 1, self.conns.index(y) + 1} in self.args.ggpo_block:
                    log("relay %s <-> %s: %d / %d, blocked (nothing listens)" % (x.user, y.user, port, port + 1))
                    port += 2
                    continue
                cmd = [sys.executable, "-u", UDPRELAY, "--a", str(port), "--b", str(port + 1),
                       "--p1", str(self.ggpo_port(x)), "--p2", str(self.ggpo_port(y)), "--delay", str(self.args.ggpo_delay),
                       "--seconds", "3600", "--idle", "600"]
                subprocess.Popen(cmd)
                log("relay %s :%d <-> %s :%d: %d / %d, %g ms one way" % (x.user, self.ggpo_port(x), y.user, self.ggpo_port(y),
                                                                         port, port + 1, self.args.ggpo_delay))
                port += 2
        return relays

    def info(self, conn):
        """The battle info JSON, as zdxsv's battleInfoNotice."""
        users, candidates = [], []
        ggpo = False
        for i, c in enumerate(self.order):
            user = dict(user_id=c.user)
            if c is not conn and c.udp and not (self.args.no_addr and self.conns.index(c) + 1 == self.args.no_addr):
                r = self.relays.get((conn, c))
                port = int(c.udp.split(":")[1])
                v4 = ("127.0.0.1" if r else c.udp.split(":")[0], port)
                v6 = (self.args.v6, port)
                for ip, p in {"v4": [v4], "v6": [v6], "dual": [v4, v6]}[self.args.p2p]:
                    candidates.append(dict(user_id=c.user, peer_id=i, ip=ip, port=p))
                if self.ggpo_port(c):
                    user["ggpo_port"] = r or self.ggpo_port(c)
                    ggpo = True
            users.append(user)
        m = dict(battle_code=self.code, peer_id=self.order.index(conn), users=users)
        if candidates:
            m["candidates"] = candidates
        if ggpo:
            # as zdxsv: ggpo_session = FNV-1 32 of the battle code, ping test 7500 ms
            h = 2166136261
            for ch in self.code.encode():
                h = ((h * 16777619) & 0xFFFFFFFF) ^ ch
            if self.args.relay:
                # the relay knows one session (zdxsv relay <id> <token>); zdxsv gives every battle its own
                sid, token = self.args.relay_session.split(":")
                h = int(sid)
                m["relays"] = [dict(ip=ip, port=int(port), token=int(token, 16))
                               for ip, port in (addr.rsplit(":", 1) for addr in self.args.relay.split(","))]
            if self.args.bad_session and self.conns.index(conn) + 1 == self.args.bad_session:
                h ^= 0x5A5A5A5A  # negative test: this client's ping packets carry a foreign session
            m.update(session_id=h, ping_test_duration=7500)
        m.update(lobby_session_id=conn.session, battle_server=self.args.battle)
        return json.dumps(m, ensure_ascii=False, separators=(",", ":")).encode()


class Conn:
    def __init__(self, sock, addr, args, trace, battle, fresh):
        self.sock, self.args, self.trace, self.battle = sock, args, trace, battle
        self.name = "%s:%d" % addr
        self.pos, self.user, self.udp = 0, "", ""
        # the PS2 joins the battle with its login session: its login key's (0x6101), else the newest login's
        self.session = logins[-1] if fresh and logins else ""
        self.tpl, self.ti = None, 0
        if trace:
            self.tpl = (trace.fresh if fresh else trace.lobby)[0]
        if battle and not fresh:
            with lock:
                bound = [c.udp for c in battle.conns]
                self.udp = next((a for a in battle.stun if a not in bound), "")
                battle.conns.append(self)
            log("%s adopted, udp %s" % (self.name, self.udp or "none"))

    def send(self, cat, cmd, seq, body):
        self.sock.sendall(frame(cat, cmd, seq, body))
        log("%s C<-S cat %02x cmd 0x%04x seq %d body %s" % (self.name, cat, cmd, seq, body.hex()))

    def notice(self, cmd, body, cat=N):
        with lock:
            seq_next[0] += 1
            s = seq_next[0]
        self.send(cat, cmd, s, body)

    def on_frame(self, cat, cmd, seq, body):
        log("%s C->S cat %02x cmd 0x%04x seq %d body %s" % (self.name, cat, cmd, seq, body.hex()))
        if cmd == 0x6138 and cat == A:
            log("BATTLE RESULT %s %s %s" % (self.name, battle_result(body), body.hex()))
        if cmd == 0x6101 and cat == A:
            k = login_key(body, seq)
            if k in key_sessions:
                self.session = key_sessions[k]
            log("%s login key %s, session %s" % (self.name, k or "?", self.session or "none"))
        if cmd == 0x6001:  # our line check answered
            return
        if not self.trace:
            if cat != Q:
                return
            if cmd == 0x6205 and len(body) >= 2:  # id, 0, users in battle
                return self.send(A, cmd, seq, body[:2] + struct.pack(">HH", 0, self.args.count))
            if cmd in ANSWERS:
                return self.send(A, cmd, seq, ANSWERS[cmd](body))
            return
        b = self.battle
        if cmd == 0x6103 and cat == A:  # FirstData: switch to the recorded login of the same user
            # before the battle start a fresh boot is headed for the entry: a recorded login that entered first
            tpls = self.trace.lobby + self.trace.fresh if b and not b.started else self.trace.fresh
            for fs in tpls:
                j = next((j for j, f in enumerate(fs) if f["c2s"] and f["cmd"] == cmd and f["body"] == body), None)
                if j is not None:
                    self.tpl, self.ti = fs, j
                    break
        if b and cat == Q and cmd == 0x640E:
            with lock:
                if self not in b.conns:
                    bound = [c.udp for c in b.conns]
                    self.udp = next((a for a in b.stun if a not in bound), "")
                    b.conns.append(self)
                    log("%s joins at entry, udp %s, session %s" % (self.name, self.udp or "none", self.session or "none"))
            self.send(A, cmd, seq, b"")
            return b.enter(self, body[0] if body else 1)
        if b and cat == Q and self.pos and cmd in (0x6911, 0x6912, 0x6913, 0x6917, 0x6914, 0x6915, 0x6916):
            u = self.trace.users
            p = body[0] if body else 0
            out = {0x6911: lambda: bytes([len(b.order)]), 0x6912: lambda: bytes([self.pos]),
                   0x6913: lambda: u[p]["a6913"], 0x6917: lambda: u[p]["a6917"], 0x6914: lambda: self.trace.rule,
                   0x6915: lambda: struct.pack(">H", len(b.code)) + b.code.encode()}
            if cmd == 0x6916:
                if self.udp or self.args.ggpo:  # like the lobby: battle info only to bridging or GGPO emulators
                    self.notice(0x9951, b.info(self), C)
                ip, port = b.args.battle.split(":")
                return self.send(A, cmd, seq, struct.pack(">H4BHH", 4, *map(int, ip.split(".")), 2, int(port)))
            return self.send(A, cmd, seq, out[cmd]())
        self.ti, out = Trace.answer(self.tpl, self.ti, cmd, body) if cat in (Q, A) else (self.ti, [])
        for f in out:
            if f["cat"] == A:
                self.send(A, f["cmd"], seq, f["body"])
            elif f["cmd"] not in (0x6910, 0x9951, 0x6001):  # battle start is ours; keepalives skipped
                self.notice(f["cmd"], f["body"], f["cat"])

    def serve(self, buf):
        while True:
            while len(buf) >= HDR.size:
                d, cat, cmd, size, seq, status = HDR.unpack_from(buf)
                if len(buf) < HDR.size + size:
                    break
                body, buf = buf[HDR.size:HDR.size + size], buf[HDR.size + size:]
                self.on_frame(cat, cmd, seq, body)
            data = self.sock.recv(4096)
            if not data:
                log("closed %s" % self.name)
                return
            buf += data


def accept(sock, addr, args, trace, battle):
    log("conn from %s:%d" % addr)
    sock.settimeout(1.0)
    try:
        buf = sock.recv(4096)
    except socket.timeout:
        buf = None
    sock.settimeout(None)
    c = Conn(sock, addr, args, trace, battle, fresh=buf is None)
    threading.Thread(target=line_check, args=(c,), daemon=True).start()
    if buf is None and trace:  # fresh connection: the lobby speaks first (key pair question)
        f = c.tpl[0]
        c.notice(f["cmd"], f["body"], f["cat"])
        buf = b""
    try:
        c.serve(buf or b"")
    except OSError as e:
        log("%s %s" % (c.name, e))


def set_rule(rule, gauge, count, time):
    """The 0x6914 rule body with 戦力ゲージ (both sides), 連続対戦数 and 作戦時間 replaced where nonzero."""
    r = bytearray(rule)
    if gauge:
        struct.pack_into(">HH", r, 72, gauge, gauge)
    if time:
        struct.pack_into(">H", r, 80, time)
    if count:
        r[87] = count
    return bytes(r)


def battle_result(b):
    """0x6138 answer -> the fields zdxsv stores (pkg/lobby parseBattleResult, model.BattleResult)."""
    try:
        o = 4 + struct.unpack_from(">H", b, 2)[0]  # unk01, encrypted battle code (size incl. chksum)
        n, w, l = b[o + 4:o + 7]
        kill, death, frames = struct.unpack_from(">III", b, o + 7)
        return "battles=%d win=%d lose=%d kill=%d death=%d frames=%d side=%d" % (n, w, l, kill, death, frames, b[o + 27])
    except (struct.error, IndexError, ValueError):
        return "unparsed"


def line_check(c):
    """LineCheck 0x6001 every 10 s, as the lobby (RequestLineCheck): without it the game
    resets its lobby connection ~116 s after the last one (all 4 resumed states)."""
    while True:
        time.sleep(10)
        try:
            c.notice(0x6001, b"", Q)
        except OSError:
            return


def stun(port, battle):
    """The lobby STUN: {"type":"ping",...} -> {"type":"pong","timestamp","user_id":"SERVER","public_addr"}."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("0.0.0.0", port))
    while True:
        data, src = u.recvfrom(2048)
        try:
            ping = json.loads(data)
        except ValueError:
            continue
        if not isinstance(ping, dict) or ping.get("type") != "ping":
            continue
        a = "%s:%d" % src
        pong = dict(type="pong", timestamp=ping.get("timestamp", 0), user_id="SERVER", public_addr=a)
        u.sendto(json.dumps(pong, separators=(",", ":")).encode(), src)
        with lock:
            if a not in battle.stun:
                battle.stun.append(a)
                log("stun %s" % a)


def login_api(port, db, lobby_addr):
    """POST /lbs/login (form token = the login_key, see tokenlogin.card_token) as zdxsv's: the account's session from zdxsv.db (a recorded login's), else a new one."""
    class H(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            form = urllib.parse.parse_qs(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode())
            key = form.get("token", [""])[0]
            sid = ""
            if db and key:
                with sqlite3.connect(db) as c:
                    r = c.execute("select session_id from account where login_key = ?", (key,)).fetchone()
                    sid = r[0] if r else ""
            sid = sid or "%08d" % random.randrange(10 ** 7, 10 ** 8)
            body = json.dumps(dict(login_key=key, session_id=sid, lobby_addr=lobby_addr, lbs_info=LBS_INFO)).encode()
            logins.append(sid)
            key_sessions[key] = sid
            log("login api %s -> session %s" % (key, sid))
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass
    http.server.ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8200)
    ap.add_argument("--count", type=int, default=7)
    ap.add_argument("--trace", help="zdxsv -v=2 lobby log of a real battle")
    ap.add_argument("--players", type=int, default=4)
    ap.add_argument("--battle", required=True, help="battle server as the games dial it (IP:8210)")
    ap.add_argument("--rpc", default="127.0.0.1:3080", help="battle server RPC")
    ap.add_argument("--hold", help="start the battle only once this file exists")
    ap.add_argument("--ggpo", type=int, default=0, help="battle info ggpo_<user>=GGPO+k-1 for the k-th adopted client "
                    "(adopted connections send no platform info; pcsx2 ZDXSV_GGPO lobby=1,port=GGPO+k-1)")
    ap.add_argument("--ggpo-clients", default="1234", help="adopted clients (1-based) that get a ggpo_ line")
    ap.add_argument("--ggpo-delay", type=float, default=0, help="one-way ms: ggpo_ ports point at a udprelay.py per pair")
    ap.add_argument("--ggpo-relay-port", type=int, default=7301, help="first relay port (2 per pair)")
    ap.add_argument("--ggpo-block", default="", help="K-L[,K-L...] (adopted clients, 1-based, with --ggpo-delay): those pairs get relay ports that nothing listens on")
    ap.add_argument("--bad-session", type=int, default=0, help="the k-th adopted client (1-based) gets a different ggpo_session")
    ap.add_argument("--no-addr", type=int, default=0, help="the k-th adopted client (1-based) gets no p2p_ / ggpo_ line in the others' battle infos")
    ap.add_argument("--p2p", choices=("v4", "v6", "dual"), default="v4",
                    help="p2p_ addresses: the STUN one (v4), [--v6]:port (v6), both (dual; as zdxsv with udp_addr6)")
    ap.add_argument("--v6", help="IPv6 address for --p2p v6/dual (default: this box's global one)")
    ap.add_argument("--relay", help="relay server ip:port[,ip:port...] (`zdxsv relay` runs): battle info relay_0, relay_1, ..., as zdxsv with ZDXSV_LOBBY_RELAY_ADDR")
    ap.add_argument("--relay-session", default="1234:abcd", help="--relay: ggpo_session:hex token, as given to `zdxsv relay`")
    ap.add_argument("--api", type=int, default=0, help="login API port (POST /lbs/login, pcsx2 fast login); 0: off")
    ap.add_argument("--db", help="--api: zdxsv.db whose account sessions the logins get (the trace's recording)")
    ap.add_argument("--battlereg",default=os.environ.get("BATTLEREG", os.path.join(here, "battlereg", "battlereg.exe")))
    ap.add_argument("--rule-gauge", type=int, default=0, help="battle rule 戦力ゲージ (1, 300, 600, ...); 0: the trace's")
    ap.add_argument("--rule-count", type=int, default=0, help="battle rule 連続対戦数 (1, 3, 5, 10); 0: the trace's")
    ap.add_argument("--rule-time", type=int, default=0, help="battle rule 作戦時間 s (90, 120, 180, 210, 270); 0: the trace's")
    a = ap.parse_args()
    a.ggpo_block = [{int(k) for k in p.split("-")} for p in a.ggpo_block.split(",")] if a.ggpo_block else []
    if a.ggpo_block and (any(len(p) != 2 for p in a.ggpo_block) or not a.ggpo_delay):
        sys.exit("--ggpo-block K-L[,K-L...]: pairs of two clients, with --ggpo-delay")
    if a.p2p != "v4" and a.ggpo_delay:
        sys.exit("--p2p %s: the --ggpo-delay relays are IPv4 only" % a.p2p)
    if a.ggpo_delay and not os.path.isfile(UDPRELAY):
        sys.exit("--ggpo-delay: %s not found" % UDPRELAY)
    if a.p2p != "v4" and not a.v6:
        # as pcsx2's udp_addr6: source address of a route to 2001:db8::1 (connect() sends nothing)
        p = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        p.connect(("2001:db8::1", 53))
        a.v6 = p.getsockname()[0]
        p.close()
        log("p2p %s: IPv6 %s" % (a.p2p, a.v6))
    trace = Trace(a.trace) if a.trace else None
    if trace:
        trace.rule = set_rule(trace.rule, a.rule_gauge, a.rule_count, a.rule_time)
        g, _, t = struct.unpack_from(">HHxxxxH", trace.rule, 72)
        log("rule gauge %d count %d time %d" % (g, trace.rule[87], t))
    battle = Battle(a, trace) if trace else None
    if battle:
        battle.conns = []
        log("trace %s: %d conns, %d battle, %d login; users %s" % (a.trace, len(trace.conns), len(trace.lobby), len(trace.fresh),
                                                                 [(p, u["id"]) for p, u in sorted(trace.users.items())]))
        threading.Thread(target=stun, args=(a.port + 1, battle), daemon=True).start()
    if a.api:
        lobby_addr = "%s:%d" % (a.battle.split(":")[0], a.port)
        threading.Thread(target=login_api, args=(a.api, a.db, lobby_addr), daemon=True).start()
        log("login api on :%d, lobby %s" % (a.api, lobby_addr))
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", a.port))
    s.listen()
    log("fake lobby on :%d" % a.port)
    while True:
        c, addr = s.accept()
        threading.Thread(target=accept, args=(c, addr, a, trace, battle), daemon=True).start()


if __name__ == "__main__":
    main()
